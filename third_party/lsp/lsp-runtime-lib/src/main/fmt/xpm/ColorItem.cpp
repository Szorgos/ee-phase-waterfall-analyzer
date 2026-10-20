/*
 * Copyright (C) 2026 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2026 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 19 янв. 2026 г.
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

#include <lsp-plug.in/runtime/version.h>

#include <lsp-plug.in/fmt/xpm/ColorItem.h>
#include <lsp-plug.in/common/alloc.h>
#include <lsp-plug.in/stdlib/string.h>

namespace lsp
{
    namespace xpm
    {
        static constexpr float COLOR32_TO_FLOAT         = 1.0f / 255.0f;
        static constexpr float COLOR64_TO_FLOAT         = 1.0f / 65535.0f;

        ColorItem::ColorItem() noexcept
        {
            nColor64        = 0;
            enState         = STATE_UNSET;
        }

        ColorItem::ColorItem(const ColorItem & src)
        {
            if (src.enState == STATE_NAME)
            {
                nColor64        = 0;
                sName           = (src.sName != NULL) ? strdup(src.sName) : NULL;
            }
            else
                nColor64        = 0;
            enState         = src.enState;
        }

        ColorItem::ColorItem(ColorItem && src) noexcept
        {
            nColor64        = src.nColor64;
            sName           = src.sName;
            enState         = src.enState;

            src.nColor64    = 0;
            src.enState     = STATE_UNSET;
        }

        ColorItem::ColorItem(const char *id)
        {
            nColor64        = 0;
            sName           = (id != NULL) ? strdup(id) : NULL;
            enState         = STATE_NAME;
        }

        ColorItem::ColorItem(uint32_t value) noexcept
        {
            nColor64        = 0;
            nColor32        = value;
            enState         = STATE_COLOR32;
        }

        ColorItem::ColorItem(uint64_t value) noexcept
        {
            nColor64        = value;
            enState         = STATE_COLOR64;
        }

        ColorItem::~ColorItem()
        {
            if ((enState == STATE_NAME) && (sName != NULL))
            {
                free(sName);
                sName           = NULL;
            }
            nColor64        = 0;
            enState         = STATE_UNSET;
        }

        void ColorItem::clear_name()
        {
            if ((enState == STATE_NAME) && (sName != NULL))
            {
                free(sName);
                sName           = NULL;
            }
            nColor64        = 0;
            enState         = STATE_NAME;
        }

        bool ColorItem::set_name(const char *name)
        {
            if (name == NULL)
            {
                clear_name();
                return true;
            }

            char *str   = strdup(name);
            if (str == NULL)
                return false;

            if (enState == STATE_NAME)
            {
                if (sName != NULL)
                    free(sName);
                sName       = str;
            }
            else
            {
                nColor64        = 0;
                sName           = str;
                enState         = STATE_NAME;
            }

            return true;
        }

        bool ColorItem::set_name(const char *name, size_t len)
        {
            if (name == NULL)
            {
                clear_name();
                return true;
            }

            char *str   = strmemdup(name, len);
            if (str == NULL)
                return false;

            if (enState == STATE_NAME)
            {
                if (sName != NULL)
                    free(sName);
                sName       = str;
            }
            else
            {
                nColor64        = 0;
                sName           = str;
                enState         = STATE_NAME;
            }

            return true;
        }

        bool ColorItem::has_name(const char *name) const noexcept
        {
            if (enState != STATE_NAME)
                return false;
            if (sName == name) // for NULL argument
                return true;
            if (sName == NULL)
                return false;
            return strcmp(sName, name) == 0;
        }

        uint32_t ColorItem::rgb24() const noexcept
        {
            switch (enState)
            {
                case STATE_COLOR32:
                    return uint32_t(nColor32);

                case STATE_COLOR64:
                {
                    return
                        (uint32_t(nColor64 >> 24) & 0xff0000) |
                        ((uint32_t(nColor64) >> 16) & 0x00ff00) |
                        ((uint32_t(nColor64) >> 8) & 0x0000ff);
                }

                default:
                    break;
            }
            return 0;
        }

        uint64_t ColorItem::rgb48() const noexcept
        {
            switch (enState)
            {
                case STATE_COLOR64:
                    return nColor64;

                case STATE_COLOR32:
                {
                    const uint64_t c =
                        (uint64_t(nColor32) & 0xff0000) << 16 |
                        ((nColor32 & 0x00ff00) << 8) |
                        (nColor32 & 0x0000ff);
                    return c | (c << 8);
                }

                default:
                    break;
            }
            return 0;
        }

        bool ColorItem::get(lsp::Color & color) const noexcept
        {
            switch (enState)
            {
                case STATE_COLOR64:
                {
                    const float r = ((nColor64 >> 32) & 0x00ffff) * COLOR64_TO_FLOAT;
                    const float g = ((nColor64 >> 16) & 0x00ffff) * COLOR64_TO_FLOAT;
                    const float b = (nColor64 & 0x00ffff) * COLOR64_TO_FLOAT;
                    color.set_rgba(r, g, b, 0.0f);
                    return true;
                }

                case STATE_COLOR32:
                {
                    const float r = ((nColor32 >> 16) & 0x0000ff) * COLOR32_TO_FLOAT;
                    const float g = ((nColor32 >> 8) & 0x0000ff) * COLOR32_TO_FLOAT;
                    const float b = (nColor32 & 0x0000ff) * COLOR32_TO_FLOAT;
                    color.set_rgba(r, g, b, 0.0f);
                    return true;
                }

                default:
                    break;
            }
            return false;
        }

        bool ColorItem::get(lsp::Color * color) const noexcept
        {
            return (color != NULL) ? get(*color) : false;
        }

        void ColorItem::set_rgb24(uint32_t value) noexcept
        {
            if ((enState == STATE_NAME) && (sName != NULL))
            {
                free(sName);
                sName           = NULL;
            }
            nColor32    = value;
            enState     = STATE_COLOR32;
        }

        void ColorItem::set_rgb48(uint64_t value) noexcept
        {
            if ((enState == STATE_NAME) && (sName != NULL))
            {
                free(sName);
                sName           = NULL;
            }
            nColor64    = value;
            enState     = STATE_COLOR64;
        }

        void ColorItem::swap(ColorItem & src) noexcept
        {
            char * const tmp_n  = sName;
            uint64_t tmp_c      = nColor64;
            nColor64            = src.nColor64;
            sName               = src.sName;
            src.nColor64        = tmp_c;
            src.sName           = tmp_n;

            lsp::swap(enState, src.enState);
        }

        void ColorItem::swap(ColorItem * src) noexcept
        {
            char * const tmp_n  = sName;
            uint64_t tmp_c      = nColor64;
            nColor64            = src->nColor64;
            sName               = src->sName;
            src->nColor64       = tmp_c;
            src->sName          = tmp_n;

            lsp::swap(enState, src->enState);
        }

        bool ColorItem::set(const ColorItem & src)
        {
            if (&src == this)
                return true;

            // Source color item does not contain name
            if (src.enState != STATE_NAME)
            {
                if (enState == STATE_NAME)
                    free(sName);

                sName       = NULL;
                nColor64    = src.nColor64;
                enState     = src.enState;
                return true;
            }

            // Source color item contains name
            char *s     = NULL;
            if (src.sName != NULL)
            {
                if ((s = strdup(src.sName)) == NULL)
                    return false;
            }

            if (enState == STATE_NAME)
            {
                if (sName != NULL)
                    free(sName);
                sName       = s;
            }
            else
            {
                nColor64    = 0;
                sName       = s;
                enState     = src.enState;
            }

            return true;
        }

        ColorItem & ColorItem::operator = (const ColorItem & src)
        {
            set(src);
            return *this;
        }

        ColorItem & ColorItem::operator = (ColorItem && src) noexcept
        {
            if (&src == this)
                return *this;

            if ((enState == STATE_NAME) && (sName != NULL))
                free(sName);

            nColor64        = src.nColor64;
            sName           = src.sName;
            enState         = src.enState;

            src.nColor64    = 0;
            src.sName       = NULL;
            src.enState     = STATE_UNSET;

            return *this;
        }

        void ColorItem::reset()
        {
            if ((enState == STATE_NAME) && (sName != NULL))
            {
                free(sName);
                sName           = NULL;
            }
            nColor64        = 0;
            enState         = STATE_UNSET;
        }

    } /* namespace xpm */
} /* namespace lsp */


