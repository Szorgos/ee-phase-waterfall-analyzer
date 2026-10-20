/*
 * Copyright (C) 2026 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2026 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-lltl-lib
 * Created on: 18 янв. 2026 г.
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

#include <lsp-plug.in/common/alloc.h>
#include <lsp-plug.in/lltl/shbuffer.h>
#include <lsp-plug.in/stdlib/string.h>

namespace lsp
{
    namespace lltl
    {
        void *raw_shbuffer::get(size_t offset)
        {
            if ((ptr == NULL) || (offset >= ptr->bytes))
                return NULL;
            return &ptr->data[offset];
        }

        void raw_shbuffer::map(void *data, size_t length, deleter_t deleter)
        {
            const size_t szof_hdr       = sizeof(buffer_t);
            buffer_t * const hdr        = static_cast<buffer_t *>(malloc(szof_hdr));
            if (hdr != NULL)
            {
                atomic_store(&hdr->references, 1);
                hdr->deleter                = deleter;
                hdr->bytes                  = length;
                hdr->data                   = static_cast<uint8_t *>(data);
            }

            reference_down(hdr);
        }

        void raw_shbuffer::make(const void *data, size_t length)
        {
            // Create new data structure
            const size_t szof_hdr       = sizeof(buffer_t);
            buffer_t * const hdr        = static_cast<buffer_t *>(malloc(szof_hdr + length + DEFAULT_ALIGN));
            if (hdr != NULL)
            {
                atomic_store(&hdr->references, 1);
                hdr->deleter                = NULL;
                hdr->bytes                  = length;
                hdr->data                   = reinterpret_cast<uint8_t *>(align_ptr(&hdr[1], DEFAULT_ALIGN));
                memcpy(hdr->data, data, length);
            }

            reference_down(hdr);
        }

        raw_shbuffer::buffer_t *raw_shbuffer::reference_up()
        {
            if (ptr != NULL)
                atomic_add(&ptr->references, 1);
            return ptr;
        }

        void raw_shbuffer::reference_down(buffer_t *replace)
        {
            if (ptr == NULL)
            {
                ptr         = replace;
                return;
            }

            const uatomic_t refs = atomic_add(&ptr->references, -1) - 1;
            if (refs > 0)
            {
                ptr         = replace;
                return;
            }

            // Destroy the memory
            if (ptr->deleter != NULL)
                ptr->deleter(ptr->data);
            free(ptr);
            ptr     = replace;
        }

        ptrdiff_t raw_shbuffer::compare(const raw_shbuffer & src)
        {
            if (ptr == src.ptr)
                return 0;
            const uint8_t *a = (ptr != NULL) ? ptr->data : NULL;
            const uint8_t *b = (src.ptr != NULL) ? src.ptr->data : NULL;
            return a - b;
        }

        ptrdiff_t raw_shbuffer::compare(const void *p)
        {
            const uint8_t *a = (ptr != NULL) ? ptr->data : NULL;
            const uint8_t *b = reinterpret_cast<const uint8_t *>(p);
            return a - b;
        }


    } /* namespace lltl */
} /* namespace lsp */


