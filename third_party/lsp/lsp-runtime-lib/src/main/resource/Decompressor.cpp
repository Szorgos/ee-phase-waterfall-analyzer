/*
 * Copyright (C) 2026 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2026 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 16 мар. 2021 г.
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
#include <lsp-plug.in/common/debug.h>
#include <lsp-plug.in/io/InMemoryStream.h>
#include <lsp-plug.in/resource/Decompressor.h>

#define BUFFER_QUANTITY         0x1000

namespace lsp
{
    namespace resource
    {
        Decompressor::Decompressor()
        {
            sReplay.data    = NULL;
            sReplay.off     = 0;
            sReplay.size    = 0;
            sReplay.cap     = 0;
            sReplay.rep     = 0;

            nOffset         = 0;
            nSize           = 0;
        }

        Decompressor::~Decompressor()
        {
            do_close();
        }

        status_t Decompressor::do_close()
        {
            // Destroy compression buffer
            sBuffer.destroy();

            // Destroy replay buffer
            if (sReplay.data != NULL)
                free(sReplay.data);

            // Clear values
            sReplay.data    = NULL;
            sReplay.off     = 0;
            sReplay.size    = 0;
            sReplay.cap     = 0;
            sReplay.rep     = 0;

            nOffset         = 0;
            nSize           = 0;

            return sIn.close();
        }

        status_t Decompressor::init(const void *data, size_t data_size, size_t decompressed_size, size_t log_buf_sz)
        {
            // Create buffer
            status_t res = sBuffer.init(log_buf_sz);
            if (res != STATUS_OK)
                return res;

            // Create input memory stream
            io::InMemoryStream *ims = new io::InMemoryStream(data, data_size);
            if (ims == NULL)
                return STATUS_NO_MEM;

            // Wrap input memory stream
            res = sIn.wrap(ims, WRAP_CLOSE | WRAP_DELETE);
            if (res != STATUS_OK)
            {
                ims->close();
                delete ims;
            }

            // Update positions
            nOffset         = 0;
            nSize           = decompressed_size;

            // Clear replay buffer
            sReplay.off     = 0;
            sReplay.size    = 0;
            sReplay.cap     = 0;

            return res;
        }

        status_t Decompressor::read_uint(size_t *out, size_t initial, size_t stepping)
        {
            ssize_t res;
            ssize_t bits    = initial;
            size_t value    = 0;
            bool flag;

            while (true)
            {
                if ((res = sIn.readb(&flag)) != 1)
                    return (res < 0) ? status_t(-res) : STATUS_IO_ERROR;
                if (!flag)
                    break;

                value      += 1 << bits;
                bits       += stepping;
            }

            fixed_size_t v  = 0;
            if ((res = sIn.readv(&v, bits)) != bits)
                return (res < 0) ? status_t(-res) : STATUS_IO_ERROR;

            *out            = value + size_t(v);
            return STATUS_OK;
        }

        status_t Decompressor::close()
        {
            return do_close();
        }

        status_t Decompressor::set_buf(size_t off, size_t count, size_t rep)
        {
            // Need to allocate data?
            if ((sReplay.cap < count) || (sReplay.data == NULL))
            {
                size_t cap      = align_size(lsp_max(sReplay.cap + (sReplay.cap >> 1), count), BUFFER_QUANTITY);
                uint8_t *ptr    = reinterpret_cast<uint8_t *>(realloc(sReplay.data, cap));
                if (ptr == NULL)
                    return STATUS_NO_MEM;

                sReplay.data    = ptr;
                sReplay.cap     = cap;
            }

            // Copy data to replay buffer
            status_t res = sBuffer.extract(sReplay.data, off, count);
            if (res != STATUS_OK)
                return res;

            sReplay.off     = 0;
            sReplay.size    = count;
            sReplay.rep     = rep;

            return STATUS_OK;
        }

        status_t Decompressor::set_bufc(uint8_t c, size_t rep)
        {
            // Need to allocate data?
            if ((sReplay.cap < 1) || (sReplay.data == NULL))
            {
                uint8_t *ptr    = reinterpret_cast<uint8_t *>(realloc(sReplay.data, BUFFER_QUANTITY));
                if (ptr == NULL)
                    return STATUS_NO_MEM;

                sReplay.data    = ptr;
                sReplay.cap     = BUFFER_QUANTITY;
            }

            // Copy data to replay buffer
            sReplay.data[0] = c;
            sReplay.off     = 0;
            sReplay.size    = 1;
            sReplay.rep     = rep;

            return STATUS_OK;
        }

        size_t Decompressor::get_buf(uint8_t *dst, size_t count)
        {
            size_t nread = 0;

            // Check for data available in buffer
            size_t avail = sReplay.size - sReplay.off;
            if (avail > 0)
            {
                avail           = lsp_min(avail, count);
                memcpy(dst, &sReplay.data[sReplay.off], avail);
                sReplay.off    += avail;
                dst            += avail;
                nread          += avail;

                if (nread >= count)
                    return nread;
            }

            // Now check if there are any repeats available
            if (sReplay.rep > 0)
            {
                avail           = lsp_min(sReplay.rep, count - nread);
                memset(dst, sReplay.data[sReplay.off-1], avail);
                sReplay.rep    -= avail;
                nread          += avail;
            }

            return nread;
        }

        ssize_t Decompressor::get_bufc()
        {
            if (sReplay.off < sReplay.size)
                return sReplay.data[sReplay.off++];

            if (sReplay.rep <= 0)
                return -STATUS_EOF;

            --sReplay.rep;
            return sReplay.data[sReplay.off-1];
        }

        status_t Decompressor::fill_buf()
        {
            // Check that data is present in the buffer
            if ((sReplay.off < sReplay.size) || (sReplay.rep > 0))
                return STATUS_OK;

            status_t res;
            size_t offset = 0, length = 0, rep = 0, append = 0;
            uint8_t b;

            // Read offset
            if ((res = read_uint(&offset, 5, 5)) != STATUS_OK)
                return res;

            if (offset < sBuffer.size())
            {
                // REPLAY BUFFER
                // Length
                if ((res = read_uint(&length, 5, 5)) != STATUS_OK)
                    return res;
                // Repeat
                if ((res = read_uint(&rep, 0, 4)) != STATUS_OK)
                    return res;

                // Fill replay buffer with data
                length += 1;
                if ((res = set_buf(offset, length, rep)) != STATUS_OK)
                    return res;

                // Append decompression buffer
                b           = sReplay.data[length - 1];
                append      = lsp_min(rep, REPEAT_BUF_MAX);
                sBuffer.append(sReplay.data, length);
            }
            else
            {
                // EMIT OCTET
                // Repeat
                if ((res = read_uint(&rep, 0, 4)) != STATUS_OK)
                    return res;

                b           = offset - sBuffer.size();
                append      = lsp_min(rep, REPEAT_BUF_MAX) + 1;

                // Fill replay buffer with data
                if ((res = set_bufc(b, rep)) != STATUS_OK)
                    return res;
            }

            // Append decompression buffer
            while (append--)
                sBuffer.append(b);

            return STATUS_OK;
        }

        ssize_t Decompressor::read(void *dst, size_t count)
        {
            status_t res;
            uint8_t *d      = reinterpret_cast<uint8_t *>(dst);
            size_t nread    = 0;

            while (nread < count)
            {
                // Check offset
                if (nOffset >= nSize)
                {
                    if (nread <= 0)
                        return -set_error(STATUS_EOF);
                    break;
                }

                // Check if data has been read
                size_t bufrd    = get_buf(&d[nread], count - nread);
                if (bufrd > 0)
                {
                    nOffset        += bufrd;
                    nread          += bufrd;
                    continue;
                }

                // There is no data in the buffer, try to get new data
                if ((res = fill_buf()) != STATUS_OK)
                {
                    if (nread > 0)
                        break;
                    set_error(res);
                    return -res;
                }
            }

            set_error(STATUS_OK);
            return nread;
        }

        ssize_t Decompressor::read_byte()
        {
            ssize_t res;

            if (nOffset >= nSize)
                return -set_error(STATUS_EOF);

            do
            {
                // Check if data has been read
                res         = get_bufc();
                if (res >= 0)
                {
                    ++nOffset;
                    return res;
                }
            } while ((res = fill_buf()) == STATUS_OK);

            set_error(status_t(res));
            return res;
        }

        wssize_t Decompressor::position()
        {
            return nOffset;
        }

    } /* namespace resource */
} /* namespace lsp */
