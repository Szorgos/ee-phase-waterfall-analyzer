/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 13 мар. 2021 г.
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

#include <lsp-plug.in/common/alloc.h>
#include <lsp-plug.in/stdlib/string.h>
#include <lsp-plug.in/resource/buffer.h>

namespace lsp
{
    namespace resource
    {
        cbuffer_t::cbuffer_t()
        {
            data        = NULL;
            index       = NULL;
            root        = NULL;
            head        = 0;
            length      = 0;
            cap         = 0;
        }

        cbuffer_t::~cbuffer_t()
        {
            destroy();
        }

        status_t cbuffer_t::init(size_t log_capacity)
        {
            const size_t capacity   = 1 << log_capacity;
            const size_t dbuf       = capacity * sizeof(uint8_t);
            const size_t ibuf       = capacity * sizeof(uint32_t);
            const size_t rbuf       = 0x100 * sizeof(uint32_t);

            uint8_t *ptr    = static_cast<uint8_t *>(realloc(data, dbuf + ibuf + rbuf));
            if (ptr == NULL)
                return STATUS_NO_MEM;

            data            = advance_ptr_bytes<uint8_t>(ptr, dbuf);
            index           = advance_ptr_bytes<uint32_t>(ptr, ibuf);
            root            = advance_ptr_bytes<uint32_t>(ptr, rbuf);
            head            = 0;
            length          = 0;
            cap             = uint32_t(capacity);

            for (size_t i=0; i < 0x100; ++i)
                root[i]     = -1;

            return STATUS_OK;
        }

        void cbuffer_t::destroy()
        {
            if (data != NULL)
                free(data);
            data            = NULL;
            index           = NULL;
            root            = NULL;
            head            = 0;
            length          = 0;
            cap             = 0;
        }

        void cbuffer_t::append(const void *src, size_t count)
        {
            const uint8_t *v        = static_cast<const uint8_t *>(src);
            if (count >= cap)
            {
                // Cleanup root index and reset head
                for (size_t i=0; i<0x100; ++i)
                    root[i]         = -1;
                length              = 0;
                head                = 0;

                // Append tail only
                v                  += count - cap;
                count               = cap;
            }

            // Fill the buffer
            const size_t mask   = cap - 1;
            const size_t ohead  = head;
            head                = (head + count) & mask;
            if (head < ohead)
            {
                const size_t split  = cap - ohead;
                memcpy(&data[ohead], v, split * sizeof(uint8_t));
                memcpy(&data[0], &v[split], (count - split) * sizeof(uint8_t));
            }
            else
                memcpy(&data[ohead], v, count * sizeof(uint8_t));

            // Update index
            uint32_t new_length = length;
            for (size_t i=0; i<count; ++i)
            {
                const size_t off    = (ohead + i) & mask;       // Compute offset
                const uint8_t b     = v[i];                     // Fetch byte
                index[off]          = root[b];                  // Store offset of previous element
                root[b]             = new_length++;             // Update absolute root offset
            }

            // Update buffer length and sequence counter
            length              = new_length;
        }

        void cbuffer_t::append(uint8_t b)
        {
            // Append byte
            data[head]      = b;                        // Data byte
            index[head]     = root[b];                  // Store offset of previous element
            root[b]         = length++;                 // Update length
            head            = (head + 1) & (cap - 1);   // Update head
        }

        size_t cbuffer_t::lookup(size_t *out, const void *src, size_t avail)
        {
            const uint8_t *v    = static_cast<const uint8_t *>(src);
            const size_t dmax   = size();

            size_t len          = 0;
            ssize_t offset      = 0;

            // We can not find sequence larger than 'length' bytes
            avail               = lsp_min(avail, dmax);

            // Lookup among all matches
            uint32_t delta      = length - root[*v];
            const size_t mask   = cap - 1;
            while (delta <= dmax)
            {
                // Byte matched, do some heuristics
                const size_t soff       = (head + cap - delta) & mask;
                if ((len > 1) && (data[(soff + len - 1) & mask] != v[len-1]))
                {
                    delta                   = length - index[soff];
                    continue;
                }

                // Compute the length of the sub-sequence
                const size_t lookup     = lsp_min(avail, delta);
                size_t slen             = 1;
                for (size_t i=1; i<lookup; ++i)
                {
                    if (v[i] != data[(soff + i) & mask])
                        break;
                    ++slen;
                }

                // Update lookup result
                if (len < slen)
                {
                    offset              = delta;
                    len                 = slen;
                    if (len >= avail)
                        break;
                }

                // Update search position
                delta                   = length - index[soff];
            }

            *out        = offset - 1;
            return len;
        }

        uint8_t cbuffer_t::byte_at(size_t offset)
        {
            return (offset < cap) ? data[(head + cap - offset - 1) & (cap - 1)] : 0;
        }

        void cbuffer_t::clear()
        {
            head            = 0;
            length          = 0;

            if (root != NULL)
            {
                for (size_t i=0; i < 0x100; ++i)
                    root[i]     = -1;
            }
        }

        dbuffer_t::dbuffer_t()
        {
            data        = NULL;
            length      = 0;
            head        = 0;
            cap         = 0;
        }

        dbuffer_t::~dbuffer_t()
        {
            destroy();
        }

        status_t dbuffer_t::init(size_t log_capacity)
        {
            size_t dbuf     = sizeof(uint8_t) << log_capacity;
            uint8_t *ptr    = static_cast<uint8_t *>(realloc(data, dbuf));
            if (ptr == NULL)
                return STATUS_NO_MEM;

            data            = ptr;
            length          = 0;
            head            = 0;
            cap             = uint32_t(1 << log_capacity);

            return STATUS_OK;
        }

        void dbuffer_t::destroy()
        {
            if (data != NULL)
                free(data);
            data            = NULL;
            length          = 0;
            head            = 0;
            cap             = 0;
        }

        void dbuffer_t::append(const void *src, size_t count)
        {
            const uint8_t *v    = static_cast<const uint8_t *>(src);
            if (count >= cap)
            {
                // Replace data in the buffer
                memcpy(data, &v[count - cap], cap * sizeof(uint8_t));
                length      = cap;
                head        = 0;
                return;
            }

            // Copy data to the buffer
            size_t ohead        = head;
            head                = (head + count) & (cap-1);
            if (head < ohead)
            {
                const size_t split  = cap - ohead;
                memcpy(&data[ohead], v, split * sizeof(uint8_t));
                memcpy(&data[0], &v[split], head * sizeof(uint8_t));
            }
            else
                memcpy(&data[ohead], v, count * sizeof(uint8_t));

            length              = uint32_t(lsp_min(length + count, cap));
        }

        status_t dbuffer_t::extract(void *dst, size_t offset, size_t count)
        {
            if (offset >= length)
                return STATUS_UNDERFLOW;

            const size_t shift  = offset + 1;
            if (count > shift)
                return STATUS_UNDERFLOW;

            uint8_t *dptr       = static_cast<uint8_t *>(dst);
            uint32_t soff       = (head + cap - shift) & (cap-1);

            if ((soff + count) > cap)
            {
                const size_t split = cap - soff;
                memcpy(dptr, &data[soff], split * sizeof(uint8_t));
                memcpy(&dptr[split], data, (count - split) * sizeof(uint8_t));
            }
            else
                memcpy(dptr, &data[soff], count * sizeof(uint8_t));

            return STATUS_OK;
        }

        void dbuffer_t::append(uint8_t v)
        {
            data[head]      = v;
            head            = (head + 1) & (cap - 1);
            length          = lsp_min(length + 1, cap);
        }

        void dbuffer_t::clear()
        {
            length          = 0;
            head            = 0;
        }

    } /* namespace resource */
} /* namespace lsp */


