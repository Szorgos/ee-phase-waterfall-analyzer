/*
 * Copyright (C) 2020 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2020 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 29 авг. 2019 г.
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

#include <lsp-plug.in/common/types.h>
#include <lsp-plug.in/fmt/java/defs.h>
#include <lsp-plug.in/fmt/java/Object.h>

namespace lsp
{
    namespace java
    {
        ftype_t decode_primitive_type(size_t tcode)
        {
            switch (tcode)
            {
                #define XDEC(a, b) case a: return b;
                XDEC(PTC_BYTE, JFT_BYTE)
                XDEC(PTC_CHAR, JFT_CHAR)
                XDEC(PTC_DOUBLE, JFT_DOUBLE)
                XDEC(PTC_FLOAT, JFT_FLOAT)
                XDEC(PTC_INTEGER, JFT_INTEGER)
                XDEC(PTC_LONG, JFT_LONG)
                XDEC(PTC_SHORT, JFT_SHORT)
                XDEC(PTC_BOOL, JFT_BOOL)
                XDEC(PTC_ARRAY, JFT_ARRAY)
                XDEC(PTC_OBJECT, JFT_OBJECT)
                #undef XDEC
                default: break;
            }

            return JFT_UNKNOWN;
        }

        const char *primitive_type_name(ftype_t tcode)
        {
            switch (tcode)
            {
                #define XDEC(a, b) case a: return #b;
                XDEC(JFT_BYTE, byte)
                XDEC(JFT_CHAR, char)
                XDEC(JFT_DOUBLE, double)
                XDEC(JFT_FLOAT, float)
                XDEC(JFT_INTEGER, int)
                XDEC(JFT_LONG, long)
                XDEC(JFT_SHORT, short)
                XDEC(JFT_BOOL, bool)
                #undef XDEC
                default: break;
            }

            return NULL;
        }
    }
}

