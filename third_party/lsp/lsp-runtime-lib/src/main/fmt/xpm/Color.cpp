/*
 * Copyright (C) 2026 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2026 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 20 янв. 2026 г.
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

#include <lsp-plug.in/fmt/xpm/Color.h>
#include <lsp-plug.in/stdlib/string.h>

namespace lsp
{
    namespace xpm
    {
        Color::~Color()
        {
            if (sCode != NULL)
            {
                free(sCode);
                sCode = NULL;
            }
        }

        Color::Color() noexcept
        {
            sCode       = NULL;
        }

        Color::Color(const char * code)
        {
            sCode       = (code != NULL) ? strdup(code) : NULL;
        }

        Color::Color(const char * code, size_t len)
        {
            sCode       = (code != NULL) ? strndup(code, len) : NULL;;
        }

        Color::Color(const Color & src):
            sMono(src.sMono),
            sSymbolic(src.sSymbolic),
            sGray4(src.sGray4),
            sGray(src.sGray),
            sColor(src.sColor)
        {
            sCode       = (src.sCode != NULL) ? strdup(src.sCode) : NULL;
        }

        Color::Color(Color && src) noexcept:
            sMono(lsp::move(src.sMono)),
            sSymbolic(lsp::move(src.sSymbolic)),
            sGray4(lsp::move(src.sGray4)),
            sGray(lsp::move(src.sGray)),
            sColor(lsp::move(src.sColor))
        {
            sCode       = lsp::exchange(src.sCode, static_cast<char *>(NULL));
        }

        void Color::clear_code()
        {
            if (sCode != NULL)
            {
                free(sCode);
                sCode = NULL;
            }
        }

        bool Color::set(const Color & src)
        {
            if (&src == this)
                return true;

            Color tmp;
            if (!tmp.set_code(src.sCode))
                return false;
            if (!tmp.sMono.set(src.sMono))
                return false;
            if (!tmp.sSymbolic.set(src.sSymbolic))
                return false;
            if (!tmp.sGray4.set(src.sGray4))
                return false;
            if (!tmp.sGray.set(src.sGray))
                return false;
            if (!tmp.sColor.set(src.sColor))
                return false;

            swap(tmp);
            return true;
        }

        bool Color::set_code(const char *code)
        {
            if (code == NULL)
            {
                clear_code();
                return true;
            }

            char *c     = strdup(code);
            if (c == NULL)
                return false;

            if (sCode != NULL)
                free(sCode);
            sCode       = c;
            return true;
        }

        bool Color::set_code(const char *code, size_t len)
        {
            if (code == NULL)
            {
                clear_code();
                return true;
            }

            char *c     = strmemdup(code, len);
            if (c == NULL)
                return false;

            if (sCode != NULL)
                free(sCode);
            sCode       = c;

            return true;
        }

        bool Color::has_code(const char *code) const noexcept
        {
            if (code == sCode) // Handle if NULL has been passed
                return true;
            if (sCode == NULL)
                return false;

            return strcmp(sCode, code) == 0;
        }

        Color & Color::operator = (const Color & src)
        {
            set(src);
            return *this;
        }

        Color & Color::operator = (Color && src) noexcept
        {
            if (&src == this)
                return *this;

            if (sCode != NULL)
                free(sCode);

            sCode       = lsp::exchange(src.sCode, static_cast<char *>(NULL));
            sMono       = lsp::move(src.sMono);
            sSymbolic   = lsp::move(src.sSymbolic);
            sGray4      = lsp::move(src.sGray4);
            sGray       = lsp::move(src.sGray);
            sColor      = lsp::move(src.sColor);

            return *this;
        }

        void Color::swap(Color & src) noexcept
        {
            lsp::swap(sCode, src.sCode);
            sMono.swap(src.sMono);
            sSymbolic.swap(src.sSymbolic);
            sGray4.swap(src.sGray4);
            sGray.swap(src.sGray);
            sColor.swap(src.sColor);
        }

        void Color::swap(Color * src) noexcept
        {
            lsp::swap(sCode, src->sCode);
            sMono.swap(src->sMono);
            sSymbolic.swap(src->sSymbolic);
            sGray4.swap(src->sGray4);
            sGray.swap(src->sGray);
            sColor.swap(src->sColor);
        }

    } /* namespace xpm */
} /* namespace lsp */


