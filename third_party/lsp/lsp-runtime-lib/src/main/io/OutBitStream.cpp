/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 3 мар. 2021 г.
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

#include <lsp-plug.in/io/OutBitStream.h>
#include <lsp-plug.in/io/OutFileStream.h>
#include <lsp-plug.in/common/endian.h>

namespace lsp
{
    namespace io
    {
        static constexpr size_t BITSTREAM_BUFSZ     = sizeof(umword_t) * 8;
        static constexpr size_t BITSTREAM_BUFSZ32   = sizeof(uint32_t) * 8;

        OutBitStream::OutBitStream()
        {
            pOS         = NULL;
            nWrapFlags  = 0;
            nBuffer     = 0;
            nBits       = 0;
        }

        OutBitStream::~OutBitStream()
        {
            if (pOS != NULL)
            {
                do_flush_buffer();

                if (nWrapFlags & WRAP_CLOSE)
                    pOS->close();
                if (nWrapFlags & WRAP_DELETE)
                    delete pOS;
                pOS         = NULL;
            }

            nBuffer = 0;
            nBits   = 0;
        }

        status_t OutBitStream::close()
        {
            status_t res = STATUS_OK, tres;

            // Close file descriptor
            if (pOS != NULL)
            {
                // Flush buffers
                res = flush();

                // Perform close
                if (nWrapFlags & WRAP_CLOSE)
                {
                    tres = pOS->close();
                    if (res == STATUS_OK)
                        res = tres;
                }
                if (nWrapFlags & WRAP_DELETE)
                    delete pOS;
                pOS         = NULL;
            }
            nWrapFlags  = 0;

            // Return result
            return set_error(res);
        }

        status_t OutBitStream::open(const char *path, size_t mode)
        {
            if (pOS != NULL)
                return set_error(STATUS_BAD_STATE);
            else if (path == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

            LSPString tmp;
            if (!tmp.set_utf8(path))
                return set_error(STATUS_NO_MEM);
            return open(&tmp, mode);
        }

        status_t OutBitStream::open(const LSPString *path, size_t mode)
        {
            if (pOS != NULL)
                return set_error(STATUS_BAD_STATE);
            else if (path == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

            OutFileStream *f = new OutFileStream();
            if (f == NULL)
                return set_error(STATUS_NO_MEM);
            status_t res = f->open(path, mode);
            if (res != STATUS_OK)
            {
                f->close();
                delete f;
                return set_error(res);
            }

            res = wrap(f, WRAP_CLOSE | WRAP_DELETE);
            if (res != STATUS_OK)
            {
                f->close();
                delete f;
                return set_error(res);
            }

            return set_error(STATUS_OK);
        }

        status_t OutBitStream::open(const io::Path *path, size_t mode)
        {
            return open(path->as_string(), mode);
        }

        status_t OutBitStream::wrap(FILE *fd, bool close)
        {
            if (pOS != NULL)
                return set_error(STATUS_BAD_STATE);
            else if (fd == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

            OutFileStream *f = new OutFileStream();
            if (f == NULL)
                return set_error(STATUS_NO_MEM);
            status_t res = f->wrap(fd, close);
            if (res != STATUS_OK)
            {
                f->close();
                delete f;
                return set_error(res);
            }

            res = wrap(f, WRAP_CLOSE | WRAP_DELETE);
            if (res != STATUS_OK)
            {
                f->close();
                delete f;
            }

            return set_error(STATUS_OK);
        }

        status_t OutBitStream::wrap_native(fhandle_t fd, bool close)
        {
            if (pOS != NULL)
                return set_error(STATUS_BAD_STATE);

            OutFileStream *f = new OutFileStream();
            if (f == NULL)
                return set_error(STATUS_NO_MEM);
            status_t res = f->wrap_native(fd, close);
            if (res != STATUS_OK)
            {
                f->close();
                delete f;
                return set_error(res);
            }

            res = wrap(f, WRAP_CLOSE | WRAP_DELETE);
            if (res != STATUS_OK)
            {
                f->close();
                delete f;
            }

            return set_error(STATUS_OK);
        }

        status_t OutBitStream::wrap(File *fd, size_t flags)
        {
            if (pOS != NULL)
                return set_error(STATUS_BAD_STATE);
            else if (fd == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

            OutFileStream *f = new OutFileStream();
            if (f == NULL)
                return set_error(STATUS_NO_MEM);
            status_t res = f->wrap(fd, flags);
            if (res != STATUS_OK)
            {
                f->close();
                delete f;
                return set_error(res);
            }

            res = wrap(f, WRAP_CLOSE | WRAP_DELETE);
            if (res != STATUS_OK)
            {
                f->close();
                delete f;
            }

            return set_error(STATUS_OK);
        }

        status_t OutBitStream::wrap(IOutStream *os, size_t flags)
        {
            if (pOS != NULL)
                return set_error(STATUS_BAD_STATE);
            else if (os == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

            // Store pointers
            pOS         = os;
            nWrapFlags  = flags;
            nBuffer     = 0;
            nBits       = 0;

            return set_error(STATUS_OK);
        }

        ssize_t OutBitStream::write(const void *buf, size_t count)
        {
            if (pOS == NULL)
                return -set_error(STATUS_CLOSED);

            size_t written          = 0;

        #ifdef LSP_UNALIGNED_MEMORY_SAFE
            // x86 allows unaligned access, write with machine words first
            const umword_t *wptr    = reinterpret_cast<const umword_t *>(buf);
            size_t blocks           = count & (~(sizeof(umword_t) - 1));

            for ( ; written < blocks; written += sizeof(umword_t))
            {
                status_t res            = writev(BE_TO_CPU(*(wptr++)), sizeof(umword_t)*8);
                if (res != STATUS_OK)
                {
                    set_error(res);
                    return (written <= 0) ? -res : written;
                }
            }

            buf = wptr;
        #endif /* LSP_UNALIGNED_MEMORY_SAFE */

            // Write the rest data with bytes
            const uint8_t *bptr     = reinterpret_cast<const uint8_t *>(buf);
            for ( ; written < count; ++written)
            {
                status_t res = writev(*(bptr++), sizeof(uint8_t) * 8);
                if (res != STATUS_OK)
                {
                    set_error(res);
                    return (written <= 0) ? -res : written;
                }
            }

            return written;
        }

        ssize_t OutBitStream::bwrite(const void *buf, size_t bits)
        {
            if (pOS == NULL)
                return -set_error(STATUS_CLOSED);

            size_t written          = 0;

        #ifdef LSP_UNALIGNED_MEMORY_SAFE
            // x86 allows unaligned memory access, write with machine words first
            const umword_t *wptr    = reinterpret_cast<const umword_t *>(buf);
            size_t blocks           = bits & (~((sizeof(umword_t) << 3) - 1));
            for ( ; written < blocks; written += sizeof(umword_t)*8)
            {
                status_t res            = writev(BE_TO_CPU(*(wptr++)), sizeof(umword_t)*8);
                if (res != STATUS_OK)
                {
                    set_error(res);
                    return (written <= 0) ? -res : written;
                }
            }

            buf                     = wptr;
        #endif /* LSP_UNALIGNED_MEMORY_SAFE */

            // Write the rest data with bytes
            const uint8_t *bptr     = reinterpret_cast<const uint8_t *>(buf);
            while (written < bits)
            {
                size_t to_write         = lsp_min(sizeof(uint8_t) * 8, bits - written);
                status_t res            = writev(*(bptr++), to_write);
                if (res != STATUS_OK)
                {
                    set_error(res);
                    return (written <= 0) ? -res : written;
                }
                written                += to_write;
            }

            return written;
        }

        status_t OutBitStream::do_flush_buffer()
        {
            if (nBits == 0)
                return set_error(STATUS_OK);

            size_t written, bytes;
            umword_t buf;

            if (nBits == sizeof(umword_t) * 8)
            {
                buf         = CPU_TO_BE(nBuffer);
                written     = pOS->write(&buf, sizeof(umword_t));

                if (written != sizeof(umword_t))
                    return -set_error(status_t(-written));
            }
            else
            {
                uint8_t data[sizeof(umword_t)];

                bytes       = (nBits + 7) >> 3; // Overall number of bytes to write
                buf         = nBuffer << (BITSTREAM_BUFSZ - nBits);
                size_t s    = BITSTREAM_BUFSZ - 8;

                for (size_t i=0; i<bytes; ++i, s -= 8)
                    data[i]     = buf >> s;

                written     = pOS->write(data, bytes);
                if (written != bytes)
                    return -set_error(status_t(-written));
            }

            nBits       = 0;

            return set_error(STATUS_OK);
        }

        status_t OutBitStream::flush()
        {
            if (pOS == NULL)
                return set_error(STATUS_CLOSED);

            return do_flush_buffer();
        }

        status_t OutBitStream::bwrite(bool value)
        {
            status_t res;
            if (pOS == NULL)
                return set_error(STATUS_CLOSED);

            // Need to flush?
            if (nBits >= BITSTREAM_BUFSZ)
            {
                if ((res = flush()) != STATUS_OK)
                    return res;
            }

            nBuffer     = (nBuffer << 1) | umword_t(value);
            ++nBits;

            return set_error(STATUS_OK);
        }

        status_t OutBitStream::writev(uint32_t value, size_t bits)
        {
            status_t res;
            if (pOS == NULL)
                return set_error(STATUS_CLOSED);

            value  <<= BITSTREAM_BUFSZ32 - bits;
            while (bits > 0)
            {
                // Need to flush?
                if (nBits >= BITSTREAM_BUFSZ)
                {
                    if ((res = flush()) != STATUS_OK)
                        return res;
                }

                size_t avail    = lsp_min(bits, BITSTREAM_BUFSZ - nBits);
                nBuffer         = (avail < BITSTREAM_BUFSZ) ? (nBuffer << avail) | (value >> (BITSTREAM_BUFSZ32 - avail)) : value;
                nBits          += avail;
                bits           -= avail;
                value         <<= avail;
            }

            return set_error(STATUS_OK);
        }

        status_t OutBitStream::writev(uint64_t value, size_t bits)
        {
            status_t res;

            #if defined(ARCH_32BIT)
                // Need to write high part?
                if (bits > BITSTREAM_BUFSZ32)
                {
                    if ((res = writev(uint32_t(value >> BITSTREAM_BUFSZ32), bits - BITSTREAM_BUFSZ32)) != STATUS_OK)
                        return res;
                    bits    = BITSTREAM_BUFSZ32;
                }

                // Write low part
                return writev(uint32_t(value), bits);
            #else
                if (pOS == NULL)
                    return set_error(STATUS_CLOSED);

                value  <<= (BITSTREAM_BUFSZ - bits);
                while (bits > 0)
                {
                    // Need to flush?
                    if (nBits >= BITSTREAM_BUFSZ)
                    {
                        if ((res = flush()) != STATUS_OK)
                            return res;
                    }

                    size_t avail    = lsp_min(bits, BITSTREAM_BUFSZ - nBits);
                    nBuffer         = (avail < BITSTREAM_BUFSZ) ? (nBuffer << avail) | (value >> (BITSTREAM_BUFSZ - avail)) : value;
                    nBits          += avail;
                    bits           -= avail;
                    value         <<= avail;
                }

                return set_error(STATUS_OK);
            #endif /* ARCH_32BIT */
        }

    } /* namespace io */
} /* namespace lsp */


