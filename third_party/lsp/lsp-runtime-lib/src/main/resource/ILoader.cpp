/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 23 окт. 2020 г.
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
#include <lsp-plug.in/io/InSequence.h>
#include <lsp-plug.in/io/InFileStream.h>
#include <lsp-plug.in/io/Dir.h>
#include <lsp-plug.in/io/Path.h>
#include <lsp-plug.in/lltl/darray.h>
#include <lsp-plug.in/resource/ILoader.h>
#include <lsp-plug.in/stdlib/string.h>

namespace lsp
{
    namespace resource
    {
        ILoader::ILoader()
        {
            nError          = STATUS_OK;
        }

        ILoader::~ILoader()
        {
            nError          = STATUS_OK;
        }

        io::IInStream *ILoader::read_stream(const char *name)
        {
            io::Path tmp;
            if ((nError = tmp.set(name)) != STATUS_OK)
                return NULL;
            return read_stream(&tmp);
        }

        io::IInStream *ILoader::read_stream(const LSPString *name)
        {
            io::Path tmp;
            if ((nError = tmp.set(name)) != STATUS_OK)
                return NULL;
            return read_stream(&tmp);
        }

        io::IInStream *ILoader::read_stream(const io::Path *name)
        {
            io::InFileStream *is = new io::InFileStream();
            if (is != NULL)
            {
                nError      = is->open(name);
                if (nError == STATUS_OK)
                    return is;
            }
            else
                nError      = STATUS_NO_MEM;

            if (is != NULL)
            {
                is->close();
                delete is;
            }

            return NULL;
        }

        io::IInSequence *ILoader::read_sequence(const char *name, const char *charset)
        {
            io::Path tmp;
            if ((nError = tmp.set(name)) != STATUS_OK)
                return NULL;
            return read_sequence(&tmp, charset);
        }

        io::IInSequence *ILoader::read_sequence(const LSPString *name, const char *charset)
        {
            io::Path tmp;
            if ((nError = tmp.set(name)) != STATUS_OK)
                return NULL;
            return read_sequence(&tmp, charset);
        }

        io::IInSequence *ILoader::read_sequence(const io::Path *name, const char *charset)
        {
            io::IInStream *is = read_stream(name);
            if (is == NULL)
                return NULL;

            io::InSequence *isq = new io::InSequence();
            if (isq != NULL)
            {
                // Wrap the file and return result
                nError = isq->wrap(is, WRAP_CLOSE | WRAP_DELETE, charset);
                if (nError == STATUS_OK)
                    return isq;

                isq->close();
                delete isq;
            }
            else
                nError = STATUS_NO_MEM;

            is->close();
            delete is;

            return NULL;
        }

        ssize_t ILoader::enumerate(const char *path, resource_t **list)
        {
            io::Path tmp;
            if ((nError = tmp.set(path)) != STATUS_OK)
                return -nError;
            return enumerate(&tmp, list);
        }

        ssize_t ILoader::enumerate(const LSPString *path, resource_t **list)
        {
            io::Path tmp;
            if ((nError = tmp.set(path)) != STATUS_OK)
                return -nError;
            return enumerate(&tmp, list);
        }

        ssize_t ILoader::enumerate(const io::Path *path, resource_t **list)
        {
            lsp_trace("path = %s", path->as_utf8());

            lltl::darray<resource_t> xlist;
            io::Dir dir;
            LSPString item;
            io::fattr_t attr;

            status_t res = dir.open(path);
            if (res != STATUS_OK)
                return -set_error(res);

            while ((res = dir.reads(&item, &attr)) == STATUS_OK)
            {
                // Skip dot and dotdot
                if ((item.equals_ascii(".")) ||
                    (item.equals_ascii("..")))
                    continue;

                // Add resource to list
                resource_t *r = xlist.add();
                if (r == NULL)
                {
                    dir.close();
                    return -set_error(STATUS_NO_MEM);
                }
                const char *name = item.get_utf8();
                if (name == NULL)
                {
                    dir.close();
                    return -set_error(STATUS_NO_MEM);
                }

                r->type = (attr.type == io::fattr_t::FT_DIRECTORY) ? RES_DIR : RES_FILE;
                strncpy(r->name, name, RESOURCE_NAME_MAX);
                r->name[RESOURCE_NAME_MAX-1] = '\0';
            }

            if (res != STATUS_EOF)
            {
                dir.close();
                return -set_error(res);
            }
            else if ((res = dir.close()) != STATUS_OK)
                return -set_error(res);

            // Detach data pointer from the collection and return as result
            const ssize_t return_size = xlist.size();
            *list       = xlist.release();

            lsp_trace("return %d resources %p", int(return_size), *list);

            set_error(STATUS_OK);
            return return_size;
        }

    } /* namespace resource */
} /* namespace lsp */
