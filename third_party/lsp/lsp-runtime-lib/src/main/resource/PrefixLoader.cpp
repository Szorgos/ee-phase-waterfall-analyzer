/*
 * Copyright (C) 2023 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2023 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 8 июн. 2021 г.
 *
 * lsp-runtime-lib is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * lsp-runtime-lib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with lsp-runtime-lib. If not, see <https://www.gnu.org/licenses/>.
 */

#include <lsp-plug.in/common/debug.h>
#include <lsp-plug.in/resource/PrefixLoader.h>

namespace lsp
{
    namespace resource
    {
        PrefixLoader::PrefixLoader(ILoader *dfl)
        {
            pDefault    = dfl;
        }

        PrefixLoader::~PrefixLoader()
        {
            pDefault    = NULL;

            for (size_t i=0, n=vLoaders.size(); i<n; ++i)
            {
                prefix_t *p = vLoaders.uget(i);
                if (p == NULL)
                    continue;

                if ((p->bFree) && (p->pLoader != NULL))
                {
                    delete p->pLoader;
                    p->pLoader = NULL;
                }
                delete p;
            }

            vLoaders.flush();
        }

        bool PrefixLoader::match_prefix(const LSPString *str, const LSPString *prefix)
        {
            if (str->length() < prefix->length())
                return false;

            // We need to match prefix respective to the possible mix of '/' and '\' characters
            for (size_t i=0, n=prefix->length(); i<n; ++i)
            {
                lsp_wchar_t ch = prefix->char_at(i);
                lsp_wchar_t sc = str->char_at(i);
                if (sc != ch)
                {
                    if ((ch != '/') && (ch != '\\'))
                        return false;
                    if ((sc != '/') && (sc != '\\'))
                        return false;
                }
            }

            return true;
        }

        ILoader *PrefixLoader::lookup_prefix(LSPString *dst, const LSPString *path)
        {
            // Check path
            if (path == NULL)
            {
                set_error(STATUS_BAD_ARGUMENTS);
                return NULL;
            }

            // Clear the error first
            set_error(STATUS_OK);

            // Find the matching prefix
            for (size_t i=0, n=vLoaders.size(); i<n; ++i)
            {
                prefix_t *p = vLoaders.uget(i);
                if (p == NULL)
                    continue;

                // Match found?
                if (match_prefix(path, &p->sPrefix))
                {
                    if (!dst->set(path, p->sPrefix.length()))
                    {
                        set_error(STATUS_NO_MEM);
                        return NULL;
                    }

//                    lsp_trace("Matched prefix '%s', delegating path '%s' to loader %p",
//                        p->sPrefix.get_utf8(), dst->get_utf8(), p->pLoader);

                    return p->pLoader;
                }
            }

            // Prefix has not been found
            return pDefault;
        }

        ILoader *PrefixLoader::lookup_prefix(LSPString *dst, const char *path)
        {
            if (path == NULL)
            {
                set_error(STATUS_BAD_ARGUMENTS);
                return NULL;
            }

            LSPString tmp;
            if (!tmp.set_utf8(path))
            {
                set_error(STATUS_NO_MEM);
                return NULL;
            }

            return lookup_prefix(dst, &tmp);
        }

        ILoader *PrefixLoader::lookup_prefix(LSPString *dst, const io::Path *path)
        {
            if (path == NULL)
            {
                set_error(STATUS_BAD_ARGUMENTS);
                return NULL;
            }

            return lookup_prefix(dst, path->as_string());
        }

        status_t PrefixLoader::add_prefix(const char *prefix, ILoader *loader, bool free)
        {
            if (prefix == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

            prefix_t *pref = new prefix_t;
            if (pref == NULL)
                return set_error(STATUS_NO_MEM);
            if (!pref->sPrefix.set_utf8(prefix))
            {
                delete pref;
                return set_error(STATUS_NO_MEM);
            }
            pref->pLoader   = loader;
            pref->bFree     = free;

            if (!vLoaders.add(pref))
            {
                delete pref;
                return set_error(STATUS_NO_MEM);
            }

            return set_error(STATUS_OK);
        }

        status_t PrefixLoader::add_prefix(const LSPString *prefix, ILoader *loader, bool free)
        {
            if (prefix == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

            prefix_t *pref = new prefix_t;
            if (pref == NULL)
                return set_error(STATUS_NO_MEM);
            if (!pref->sPrefix.set(prefix))
            {
                delete pref;
                return set_error(STATUS_NO_MEM);
            }
            pref->pLoader   = loader;
            pref->bFree     = free;

            if (!vLoaders.add(pref))
            {
                delete pref;
                return set_error(STATUS_NO_MEM);
            }

            return set_error(STATUS_OK);
        }

        status_t PrefixLoader::add_prefix(const io::Path *prefix, ILoader *loader, bool free)
        {
            if (prefix == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

            prefix_t *pref = new prefix_t;
            if (pref == NULL)
                return set_error(STATUS_NO_MEM);
            if (!pref->sPrefix.set(prefix->as_string()))
            {
                delete pref;
                return set_error(STATUS_NO_MEM);
            }
            pref->pLoader   = loader;
            pref->bFree     = free;

            if (!vLoaders.add(pref))
            {
                delete pref;
                return set_error(STATUS_NO_MEM);
            }

            return set_error(STATUS_OK);
        }

        io::IInStream *PrefixLoader::read_stream(const char *name)
        {
            LSPString tmp;
            ILoader *ldr = lookup_prefix(&tmp, name);
            if (ldr != NULL)
            {
                io::IInStream *ret = ldr->read_stream(&tmp);
                set_error(ldr->last_error());
                return ret;
            }

            return (last_error() == STATUS_OK) ? ILoader::read_stream(name) : NULL;
        }

        io::IInStream *PrefixLoader::read_stream(const LSPString *name)
        {
            LSPString tmp;
            ILoader *ldr = lookup_prefix(&tmp, name);
            if (ldr != NULL)
            {
                io::IInStream *ret = ldr->read_stream(&tmp);
                set_error(ldr->last_error());
                return ret;
            }

            return (last_error() == STATUS_OK) ? ILoader::read_stream(name) : NULL;
        }

        io::IInStream *PrefixLoader::read_stream(const io::Path *name)
        {
            LSPString tmp;
            ILoader *ldr = lookup_prefix(&tmp, name);
            if (ldr != NULL)
            {
                io::IInStream *ret = ldr->read_stream(&tmp);
                set_error(ldr->last_error());
                return ret;
            }

            return (last_error() == STATUS_OK) ? ILoader::read_stream(name) : NULL;
        }

        io::IInSequence *PrefixLoader::read_sequence(const char *name, const char *charset)
        {
            LSPString tmp;
            ILoader *ldr = lookup_prefix(&tmp, name);
            if (ldr != NULL)
            {
                io::IInSequence *ret = ldr->read_sequence(&tmp, charset);
                set_error(ldr->last_error());
                return ret;
            }

            return (last_error() == STATUS_OK) ? ILoader::read_sequence(name, charset) : NULL;
        }

        io::IInSequence *PrefixLoader::read_sequence(const LSPString *name, const char *charset)
        {
            LSPString tmp;
            ILoader *ldr = lookup_prefix(&tmp, name);
            if (ldr != NULL)
            {
                io::IInSequence *ret = ldr->read_sequence(&tmp, charset);
                set_error(ldr->last_error());
                return ret;
            }

            return (last_error() == STATUS_OK) ? ILoader::read_sequence(name, charset) : NULL;
        }

        io::IInSequence *PrefixLoader::read_sequence(const io::Path *name, const char *charset)
        {
            LSPString tmp;
            ILoader *ldr = lookup_prefix(&tmp, name);
            if (ldr != NULL)
            {
                io::IInSequence *ret = ldr->read_sequence(&tmp, charset);
                set_error(ldr->last_error());
                return ret;
            }

            return (last_error() == STATUS_OK) ? ILoader::read_sequence(name, charset) : NULL;
        }

        ssize_t PrefixLoader::enumerate(const char *path, resource::resource_t **list)
        {
            lsp_trace("path = %s", path);
            LSPString tmp;
            ILoader *ldr = lookup_prefix(&tmp, path);
            if (ldr != NULL)
            {
                ssize_t ret = ldr->enumerate(&tmp, list);
                set_error(ldr->last_error());
                return ret;
            }

            return (last_error() == STATUS_OK) ? ILoader::enumerate(path, list) : -last_error();
        }

        ssize_t PrefixLoader::enumerate(const LSPString *path, resource::resource_t **list)
        {
            lsp_trace("path = %s", path->get_utf8());
            LSPString tmp;
            ILoader *ldr = lookup_prefix(&tmp, path);
            if (ldr != NULL)
            {
                ssize_t ret = ldr->enumerate(&tmp, list);
                set_error(ldr->last_error());
                return ret;
            }

            return (last_error() == STATUS_OK) ? ILoader::enumerate(path, list) : -last_error();
        }

        ssize_t PrefixLoader::enumerate(const io::Path *path, resource::resource_t **list)
        {
            LSPString tmp;
            ILoader *ldr = lookup_prefix(&tmp, path);
            if (ldr != NULL)
            {
                ssize_t ret = ldr->enumerate(&tmp, list);
                set_error(ldr->last_error());
                return ret;
            }

            return (last_error() == STATUS_OK) ? ILoader::enumerate(path, list) : -last_error();
        }

    } /* namespace resource */
} /* namespace lsp */


