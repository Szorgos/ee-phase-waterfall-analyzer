/*
 * Copyright (C) 2026 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2026 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 14 мар. 2021 г.
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

#include <lsp-plug.in/common/endian.h>
#include <lsp-plug.in/io/InBitStream.h>
#include <lsp-plug.in/io/InFileStream.h>

namespace lsp
{
    namespace io
    {
        static constexpr size_t BITSTREAM_BUFSZ     = sizeof(umword_t) * 8;
        static constexpr size_t BITSTREAM_BUFSZ32   = sizeof(uint32_t) * 8;

        InBitStream::InBitStream()
        {
            pIS         = NULL;
            nWrapFlags  = 0;
            nBuffer     = 0;
            nBits       = 0;
        }

        InBitStream::~InBitStream()
        {
            do_close();
        }

        status_t InBitStream::wrap(FILE *fd, bool close)
        {
            if (pIS != NULL)
                return set_error(STATUS_BAD_STATE);

            status_t res;

            InFileStream *ofs = new InFileStream();
            if (ofs == NULL)
                return set_error(STATUS_NO_MEM);

            if ((res = ofs->wrap(fd, close)) == STATUS_OK)
                res     = wrap(ofs, WRAP_CLOSE | WRAP_DELETE);

            if (res != STATUS_OK)
            {
                ofs->close();
                delete ofs;
            }

            return res;
        }

        status_t InBitStream::wrap_native(fhandle_t fd, bool close)
        {
            if (pIS != NULL)
                return set_error(STATUS_BAD_STATE);

            status_t res;

            InFileStream *ofs = new InFileStream();
            if (ofs == NULL)
                return set_error(STATUS_NO_MEM);

            if ((res = ofs->wrap_native(fd, close)) == STATUS_OK)
                res     = wrap(ofs, WRAP_CLOSE | WRAP_DELETE);

            if (res != STATUS_OK)
            {
                ofs->close();
                delete ofs;
            }

            return res;
        }

        status_t InBitStream::wrap(File *fd, size_t flags)
        {
            if (pIS != NULL)
                return set_error(STATUS_BAD_STATE);

            status_t res;

            InFileStream *ofs = new InFileStream();
            if (ofs == NULL)
                return set_error(STATUS_NO_MEM);

            if ((res = ofs->wrap(fd, flags)) == STATUS_OK)
                res     = wrap(ofs, WRAP_CLOSE | WRAP_DELETE);

            if (res != STATUS_OK)
            {
                ofs->close();
                delete ofs;
            }

            return res;
        }

        status_t InBitStream::open(const char *path)
        {
            if (pIS != NULL)
                return set_error(STATUS_BAD_STATE);

            status_t res;

            InFileStream *ofs = new InFileStream();
            if (ofs == NULL)
                return set_error(STATUS_NO_MEM);

            if ((res = ofs->open(path)) == STATUS_OK)
                res     = wrap(ofs, WRAP_CLOSE | WRAP_DELETE);

            if (res != STATUS_OK)
            {
                ofs->close();
                delete ofs;
            }

            return res;
        }

        status_t InBitStream::open(const LSPString *path)
        {
            if (pIS != NULL)
                return set_error(STATUS_BAD_STATE);

            status_t res;

            InFileStream *ofs = new InFileStream();
            if (ofs == NULL)
                return set_error(STATUS_NO_MEM);

            if ((res = ofs->open(path)) == STATUS_OK)
                res     = wrap(ofs, WRAP_CLOSE | WRAP_DELETE);

            if (res != STATUS_OK)
            {
                ofs->close();
                delete ofs;
            }

            return res;
        }


        status_t InBitStream::open(const Path *path)
        {
            if (pIS != NULL)
                return set_error(STATUS_BAD_STATE);

            status_t res;

            InFileStream *ofs = new InFileStream();
            if (ofs == NULL)
                return set_error(STATUS_NO_MEM);

            if ((res = ofs->open(path)) == STATUS_OK)
                res     = wrap(ofs, WRAP_CLOSE | WRAP_DELETE);

            if (res != STATUS_OK)
            {
                ofs->close();
                delete ofs;
            }

            return res;
        }

        status_t InBitStream::wrap(IInStream *is, size_t flags)
        {
            if (pIS != NULL)
                return set_error(STATUS_BAD_STATE);
            else if (is == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

            // Store pointers
            pIS         = is;
            nWrapFlags  = flags;
            nBuffer     = 0;
            nBits       = 0;

            return set_error(STATUS_OK);
        }

        status_t InBitStream::do_close()
        {
            status_t res = STATUS_OK;

            // Close file descriptor
            if (pIS != NULL)
            {
                // Perform close
                if (nWrapFlags & WRAP_CLOSE)
                    res = pIS->close();
                if (nWrapFlags & WRAP_DELETE)
                    delete pIS;
                pIS         = NULL;
            }

            nWrapFlags  = 0;
            nBuffer     = 0;
            nBits       = 0;

            return res;
        }

        status_t InBitStream::close()
        {
            return set_error(do_close());
        }

        ssize_t InBitStream::read(void *dst, size_t count)
        {
            ssize_t nread   = bread(dst, count * 8);
            if (nread < 0)
                return nread;

            // Need to unget something?
            if (nread & 7)
            {
                uint8_t *p      = reinterpret_cast<uint8_t *>(dst);
                unread(p[nread >> 3], nread & 7);
            }

            return nread >> 3;
        }

        ssize_t InBitStream::bread(void *buf, size_t bits)
        {
            if (pIS == NULL)
                return -set_error(STATUS_CLOSED);

            uint8_t *dst        = reinterpret_cast<uint8_t *>(buf);
            size_t nread        = 0;

            while (nread < bits)
            {
                size_t to_read      = lsp_min(bits - nread, 8u);
                ssize_t n           = readv(dst++, to_read);
                if (n < 0)
                {
                    if (nread > 0)
                        break;
                    set_error(status_t(-n));
                    return n;
                }

                nread              += n;
                if (n < 8)
                    break;
            }

            set_error(STATUS_OK);
            return nread;
        }

        wssize_t InBitStream::bskip(wsize_t amount)
        {
            if (pIS == NULL)
                return -set_error(STATUS_CLOSED);

            wsize_t skipped = 0;

            // Simple skip?
            if (amount <= nBits)
            {
                nBuffer   <<= amount;
                nBits      -= amount;
                return amount;
            }
            else
            {
                skipped     = nBits;
                nBits       = 0;
                amount     -= skipped;
            }

            // Can skip bytes?
            wssize_t bytes  = amount >> 3;
            while (bytes > 0)
            {
                wssize_t n  = pIS->skip(bytes);
                if (n < 0)
                {
                    if (skipped > 0)
                        break;
                    set_error(status_t(-n));
                    return n;
                }
                bytes      -= n;
                skipped    += n << 3;
                amount     -= n << 3;
            }

            // Tail left?
            if (amount > 0)
            {
                umword_t v;
                ssize_t n   = readv(&v, amount);
                if ((n < 0) && (skipped <= 0))
                {
                    set_error(status_t(-n));
                    return n;
                }
            }

            set_error(STATUS_OK);
            return skipped;
        }

        ssize_t InBitStream::readb(bool *value)
        {
            status_t res;
            if (pIS == NULL)
                return -set_error(STATUS_CLOSED);

            // Fill buffer with new data
            if ((res = fill()) != STATUS_OK)
                return -set_error(res);

            *value      = nBuffer >> (BITSTREAM_BUFSZ - 1);
            nBuffer   <<= 1;
            --nBits;

            set_error(STATUS_OK);
            return 1;
        }

        ssize_t InBitStream::readv(uint8_t *value, size_t bits)
        {
            umword_t v;
            ssize_t n = readv(&v, bits);
            if (n > 0)
                *value      = uint8_t(v);
            return n;
        }

        ssize_t InBitStream::readv(uint16_t *value, size_t bits)
        {
            umword_t v;
            ssize_t n = readv(&v, bits);
            if (n > 0)
                *value      = uint16_t(v);
            return n;
        }

        ssize_t InBitStream::readv(uint32_t *value, size_t bits)
        {
            if (pIS == NULL)
                return -set_error(STATUS_CLOSED);

            size_t nread    = 0;
            status_t res;
            uint32_t v      = 0;

            while (nread < bits)
            {
                // Fill buffer with new data
                if ((res = fill()) != STATUS_OK)
                {
                    if (nread > 0)
                        break;
                    return -set_error(res);
                }

                // Estimate number of bits to read
                const size_t to_read= lsp_min(nBits, bits - nread);
                uint32_t x          = uint32_t(nBuffer >> (BITSTREAM_BUFSZ - to_read));
                v                   = (to_read < BITSTREAM_BUFSZ32) ? uint32_t((v << to_read) | x) : x;
                nBuffer           <<= to_read;
                nBits              -= to_read;
                nread              += to_read;
            }

            *value          = v;
            set_error(STATUS_OK);
            return nread;
        }

        ssize_t InBitStream::readv(uint64_t *value, size_t bits)
        {
            if (pIS == NULL)
                return -set_error(STATUS_CLOSED);

            size_t nread    = 0;
            status_t res;
            uint64_t v      = 0;

            while (nread < bits)
            {
                // Fill buffer with new data
                if ((res = fill()) != STATUS_OK)
                {
                    if (nread > 0)
                        break;
                    return -set_error(res);
                }

                // Estimate number of bits to read
                size_t to_read      = lsp_min(nBits, bits - nread);
                v                   = (to_read < BITSTREAM_BUFSZ) ? (v << to_read) | (nBuffer >> (BITSTREAM_BUFSZ - to_read)) : nBuffer;
                nBuffer           <<= to_read;
                nBits              -= to_read;
                nread              += to_read;
            }

            *value          = v;
            set_error(STATUS_OK);
            return nread;
        }

        void InBitStream::unread(umword_t v, size_t bits)
        {
            if (bits < BITSTREAM_BUFSZ)
                v         <<= BITSTREAM_BUFSZ - bits;
            nBuffer     = (nBuffer >> bits) | v;
            nBits      += bits;
        }

        status_t InBitStream::fill()
        {
            if (nBits > 0)
                return STATUS_OK;

            nBuffer     = 0;
            ssize_t n   = pIS->read(&nBuffer, sizeof(umword_t));
            if (n <= 0)
                return status_t(-n);

            nBits       = n << 3;
            nBuffer     = BE_TO_CPU(nBuffer);

            return STATUS_OK;
        }

    } /* namespace io */
} /* namespace lsp */


