/*
 * Copyright (C) 2020 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2020 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-lltl-lib
 * Created on: 11 мая 2020 г.
 *
 * lsp-lltl-lib is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * lsp-lltl-lib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with lsp-lltl-lib. If not, see <https://www.gnu.org/licenses/>.
 */

#include <lsp-plug.in/lltl/types.h>

namespace lsp
{
    namespace lltl
    {
        LSP_LLTL_LIB_PUBLIC
        ssize_t  default_compare_func(const void *a, const void *b, size_t size)
        {
            return ::memcmp(a, b, size);
        }

        LSP_LLTL_LIB_PUBLIC
        size_t default_hash_func(const void *ptr, size_t size)
        {
            size_t v, hash = 0;
            const size_t *p = static_cast<const size_t *>(ptr);

            // Main part
            for ( ; size >= sizeof(size_t); size -= sizeof(size_t))
            {
                v       = *(p++);

                hash    = (hash << (sizeof(size_t)*4 + 1)) | (hash >> (sizeof(size_t)*4 - 1));  // Rotate hash
                v       = (v << 7) + (v << 4) + v;                          // v *= 137
                hash   ^= v;
            }

            // optional part
            if (size > 0)
            {
                v = 0;
                for (const uint8_t *z = reinterpret_cast<const uint8_t *>(p); size > 0; --size)
                    v       = (v << 8) | *(z++);

                hash    = (hash << (sizeof(size_t)*4 + 1)) | (hash >> (sizeof(size_t)*4 - 1));  // Rotate hash
                v       = (v << 7) + (v << 4) + v;                          // v *= 137
                hash   ^= v;
            }

            return hash;
        }

        LSP_LLTL_LIB_PUBLIC
        size_t char_hash_func(const void *ptr, size_t size)
        {
            const uint8_t *s = static_cast<const uint8_t *>(ptr);
            size_t hash = 0;

            while (true)
            {
                size_t  v = *(s++);
                if (v == 0)
                    break;

                hash    = ((hash << 7) + (hash << 4) + hash) ^ v;
            }

            return hash;
        }

        LSP_LLTL_LIB_PUBLIC
        size_t ptr_hash_func(const void *ptr, size_t size)
        {
            uintptr_t a     = uintptr_t(ptr);
            uintptr_t v     = (a >> 5) | (a << (sizeof(uintptr_t)*8 - 5));

            // Generate two pseudo-random values
            uintptr_t h1    = v * 0x4ef1d1e9 + 0x46777db9;
            uintptr_t h2    = v * 0x4b0faf0d + 0x412318bb;

            // Shuffle data
            #ifdef ARCH_64BIT
                h1              = ((h1 & 0xffff0000ffff0000ULL) >> 16) | ((h1 & 0x0000ffff0000ffffULL) << 16);
                h2              = (h2 >> 13) | (h2 << 51);
            #else
                h1              = ((h1 & 0xffff0000UL) >> 16) | ((h1 & 0x0000ffffUL) << 16);
                h2              = (h2 >> 13) | (h2 << 19);
            #endif

            // Return the final result as xor'ed expression
            return h1 ^ h2 ^ a;
        }

        LSP_LLTL_LIB_PUBLIC
        ssize_t char_cmp_func(const void *a, const void *b, size_t size)
        {
            return ::strcmp(static_cast<const char *>(a), static_cast<const char *>(b));
        }

        LSP_LLTL_LIB_PUBLIC
        ssize_t ptr_cmp_func(const void *a, const void *b, size_t size)
        {
            return (a > b) ? 1 : (a < b) ? -1 : 0;
        }

        LSP_LLTL_LIB_PUBLIC
        void *char_clone_func(const void *ptr, size_t size)
        {
            return ::strdup(static_cast<const char *>(ptr));
        }
    } /* namespace lltl */
} /* namespace lsp */


