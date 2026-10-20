/*
 * Copyright (C) 2024 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2024 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-dsp-units
 * Created on: 11 мая 2024 г.
 *
 * lsp-dsp-units is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * lsp-dsp-units is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with lsp-dsp-units. If not, see <https://www.gnu.org/licenses/>.
 */

#include <lsp-plug.in/runtime/uuid.h>
#include <lsp-plug.in/stdlib/stdlib.h>

namespace lsp
{
    static const char *hex_lower = "0123456789abcdef";
    static const char *hex_upper = "0123456789ABCDEF";
    static const char *base64 =
        "0123456789abcdef"
        "ghijklmnopqrstuv"
        "wxyzABCDEFGHIJKL"
        "MNOPQRSTUVWXYZ_-";

    static char *fmt_bytes(char *dst, const uint8_t *ptr, const char *table, size_t count)
    {
        for (size_t i=0; i<count; ++i, dst += 2)
        {
            const uint8_t v = ptr[i];
            dst[0]      = table[v >> 4];
            dst[1]      = table[v & 0x0f];
        }

        return dst;
    }

    void generate_uuid(uuid_t *uuid)
    {
        uint16_t *u16 = uuid->u16;
        for (size_t i=0; i < 8; ++i)
            u16[i] = uint16_t(rand());

        uint8_t *u8 = uuid->u8;
        u8[6]       = (u8[6] & 0x0f) | 0x40; // Set version to 0100
        u8[8]       = (u8[8] & 0x3f) | 0x80; // Set bits 6-7 to 10
    }

    char *format_uuid_hex(char *dst, const uuid_t *uuid, bool upper)
    {
        const char *table = (upper) ? hex_upper : hex_lower;
        dst = fmt_bytes(dst, uuid->u8, table, 16);
        *dst = '\0';

        return dst;
    }

    char *format_uuid_dashed(char *dst, const uuid_t *uuid, bool upper)
    {
        const char *table = (upper) ? hex_upper : hex_lower;
        const uint8_t *u = uuid->u8;

        dst = fmt_bytes(dst, u, table, 4);
        *dst++ = '-';
        u += 4;

        dst = fmt_bytes(dst, u, table, 2);
        *dst++ = '-';
        u += 2;

        dst = fmt_bytes(dst, u, table, 2);
        *dst++ = '-';
        u += 2;

        dst = fmt_bytes(dst, u, table, 2);
        *dst++ = '-';
        u += 2;

        dst = fmt_bytes(dst, u, table, 6);
        *dst = '\0';

        return dst;
    }

    static void fmt_triple(char *dst, const uint8_t *ptr)
    {
        uint32_t value =
            uint32_t(ptr[0]) |
            (uint32_t(ptr[1]) << 8) |
            (uint32_t(ptr[2]) << 16);
        for (size_t i=0; i<4; ++i)
        {
            dst[i]      = base64[value & 0x3f];
            value     >>= 6;
        }
    }

    char *format_uuid_base64(char *dst, const uuid_t *uuid)
    {
        const uint8_t *ptr = uuid->u8;
        for (size_t i=0; i<5; ++i)
        {
            fmt_triple(dst, ptr);
            ptr        += 3;
            dst        += 4;
        }
        
        uint32_t value = uint32_t(ptr[0]);
        for (size_t i=0; i<2; ++i)
        {
            dst[i]      = base64[value & 0x3f];
            value     >>= 6;
        }
        
        dst += 2;
        *dst = '\0';
        return dst;
    }

} /* namespace lsp */


