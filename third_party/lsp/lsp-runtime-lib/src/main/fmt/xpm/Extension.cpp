/*
 * Copyright (C) 2026 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2026 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 22 янв. 2026 г.
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

#include <lsp-plug.in/fmt/xpm/Extension.h>
#include <lsp-plug.in/common/alloc.h>
#include <lsp-plug.in/stdlib/string.h>

namespace lsp
{
    namespace xpm
    {
        Extension::Extension() noexcept
        {
            sName       = NULL;
        }

        Extension::Extension(const char * name)
        {
            sName       = (name != NULL) ? strdup(name) : NULL;
        }

        Extension::Extension(const char * name, size_t len)
        {
            sName       = (name != NULL) ? strmemdup(name, len) : NULL;
        }

        bool Extension::copy(lltl::parray<char> & dst, const lltl::parray<char> & src)
        {
            if (!dst.reserve(src.size()))
                return false;

            for (size_t i=0; i<src.size(); ++i)
            {
                const char *sc  = src.uget(i);
                if (sc == NULL)
                    return false;
                char *cc        = strdup(sc);
                if (cc == NULL)
                    return false;
                if (!dst.add(cc))
                {
                    free(cc);
                    return false;
                }
            }

            return true;
        }

        void Extension::destroy(lltl::parray<char> & src)
        {
            for (size_t i=0; i<src.size(); ++i)
            {
                char *sc  = src.uget(i);
                if (sc != NULL)
                    free(sc);
            }
            src.flush();
        }

        Extension::Extension(const Extension & src)
        {
            sName       = (src.sName != NULL) ? strdup(src.sName) : NULL;
            copy(vData, src.vData);
        }

        Extension::Extension(Extension && src) noexcept
        {
            sName       = lsp::exchange(src.sName, static_cast<char *>(NULL));

            vData.swap(src.vData);
            destroy(src.vData);
        }

        Extension::~Extension()
        {
            if (sName != NULL)
            {
                free(sName);
                sName = NULL;
            }
            destroy(vData);
        }

        bool Extension::set_name(const char *name)
        {
            if (name == NULL)
            {
                clear_name();
                return true;
            }

            char *str   = strdup(name);
            if (str == NULL)
                return false;

            if (sName != NULL)
                free(sName);
            sName       = str;

            return true;
        }

        bool Extension::set_name(const char *name, size_t len)
        {
            if (name == NULL)
            {
                clear_name();
                return true;
            }

            char *str   = strmemdup(name, len);
            if (str == NULL)
                return false;

            if (sName != NULL)
                free(sName);
            sName       = str;

            return true;
        }

        void Extension::clear_name()
        {
            if (sName != NULL)
            {
                sName       = NULL;
                free(sName);
            }
        }

        bool Extension::add_row(const char *row)
        {
            if (row == NULL)
                return false;

            char *str   = strdup(row);
            if (str == NULL)
                return false;

            if (vData.add(str))
                return true;

            free(str);
            return false;
        }

        bool Extension::add_row(const char *row, size_t length)
        {
            if (row == NULL)
                return false;

            char *str   = strmemdup(row, length);
            if (str == NULL)
                return false;

            if (vData.add(str))
                return true;

            free(str);
            return false;
        }

        bool Extension::set(const Extension & src)
        {
            if (&src == this)
                return true;

            Extension tmp;
            if (!tmp.set_name(src.sName))
                return false;
            if (!copy(tmp.vData, src.vData))
                return false;

            swap(tmp);
            return true;
        }

        bool Extension::has_name(const char *name) const noexcept
        {
            if (name == sName) // Handle if NULL has been passed
                return true;
            if (sName == NULL)
                return false;

            return strcmp(sName, name) == 0;
        }

        Extension & Extension::operator = (const Extension & src)
        {
            set(src);
            return *this;
        }

        Extension & Extension::operator = (Extension && src) noexcept
        {
            Extension tmp(lsp::move(src));
            swap(tmp);
            return *this;
        }

        void Extension::swap(Extension & src) noexcept
        {
            lsp::swap(sName, src.sName);
            vData.swap(src.vData);
        }

        void Extension::swap(Extension * src) noexcept
        {
            lsp::swap(sName, src->sName);
            vData.swap(src->vData);
        }

    } /* namespace xpm */
} /* namespace lsp */

