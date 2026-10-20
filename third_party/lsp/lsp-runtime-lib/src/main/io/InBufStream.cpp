/*
 * Copyright (C) 2026 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2026 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 17 янв. 2026 г.
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

#include <lsp-plug.in/io/InBufStream.h>
#include <lsp-plug.in/io/InFileStream.h>

namespace lsp
{
    namespace io
    {
        InBufStream::InBufStream(size_t buf_size)
        {
            pIS         = NULL;
            vBuffer     = NULL;
            nBufCap     = uint32_t(buf_size);
            nBufHead    = 0;
            nBufOff     = 0;
            nBufSize    = 0;
            nPosition   = 0;
            nWrapFlags  = EXT_BUFFERING;
        }

        InBufStream::~InBufStream()
        {
            do_close();
        }

        inline void InBufStream::clear_buffer()
        {
            nBufHead    = 0;
            nBufOff     = 0;
            nBufSize    = 0;
        }

        status_t InBufStream::do_close()
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
            nWrapFlags &= ~WRAP_FULL;

            // Free the buffer
            if (vBuffer != NULL)
            {
                free(vBuffer);
                vBuffer     = NULL;
            }

            clear_buffer();

            return res;
        }

        status_t InBufStream::close()
        {
            if (pIS == NULL)
                return STATUS_OK;

            status_t res = do_close();
            return set_error(res);
        }

        bool InBufStream::init_buffer()
        {
            if (nBufCap <= 0)
                return false;

            if (vBuffer == NULL)
            {
                vBuffer     = static_cast<uint8_t *>(malloc(nBufCap));
                if (vBuffer == NULL)
                    return false;
            }

            clear_buffer();

            return true;
        }

        status_t InBufStream::do_wrap(IInStream *is, size_t flags)
        {
            // Obtain the position of the input stream
            wssize_t pos = is->position();
            if (pos < 0)
            {
                if (pos != -STATUS_NOT_IMPLEMENTED)
                    return pos;
                else
                    pos = 0;
            }

            // Check buffer capacity
            if (nBufCap <= 0)
                return STATUS_BAD_STATE;

            // Store pointers
            pIS         = is;
            nWrapFlags  = (nWrapFlags & (~uint32_t(WRAP_FULL))) | (uint32_t(flags) & WRAP_FULL);
            nPosition   = pos;

            return STATUS_OK;
        }

        status_t InBufStream::open(const char *path)
        {
            if (pIS != NULL)
                return set_error(STATUS_BAD_STATE);
            if (!init_buffer())
                return set_error(STATUS_NO_MEM);

            status_t res;

            InFileStream *ofs = new InFileStream();
            if (ofs == NULL)
                return set_error(STATUS_NO_MEM);

            if ((res = ofs->open(path)) == STATUS_OK)
                res     = do_wrap(ofs, WRAP_CLOSE | WRAP_DELETE);

            if (res != STATUS_OK)
            {
                ofs->close();
                delete ofs;
            }

            return set_error(res);
        }

        status_t InBufStream::open(const LSPString *path)
        {
            if (pIS != NULL)
                return set_error(STATUS_BAD_STATE);
            if (!init_buffer())
                return set_error(STATUS_NO_MEM);

            status_t res;

            InFileStream *ofs = new InFileStream();
            if (ofs == NULL)
                return set_error(STATUS_NO_MEM);

            if ((res = ofs->open(path)) == STATUS_OK)
                res     = do_wrap(ofs, WRAP_CLOSE | WRAP_DELETE);

            if (res != STATUS_OK)
            {
                ofs->close();
                delete ofs;
            }

            return set_error(res);
        }

        status_t InBufStream::open(const Path *path)
        {
            if (pIS != NULL)
                return set_error(STATUS_BAD_STATE);
            if (!init_buffer())
                return set_error(STATUS_NO_MEM);

            status_t res;

            InFileStream *ofs = new InFileStream();
            if (ofs == NULL)
                return set_error(STATUS_NO_MEM);

            if ((res = ofs->open(path)) == STATUS_OK)
                res     = do_wrap(ofs, WRAP_CLOSE | WRAP_DELETE);

            if (res != STATUS_OK)
            {
                ofs->close();
                delete ofs;
            }

            return set_error(res);
        }

        status_t InBufStream::wrap(FILE *fd, bool close)
        {
            if (pIS != NULL)
                return set_error(STATUS_BAD_STATE);
            if (!init_buffer())
                return set_error(STATUS_NO_MEM);

            status_t res;

            InFileStream *ofs = new InFileStream();
            if (ofs == NULL)
                return set_error(STATUS_NO_MEM);

            if ((res = ofs->wrap(fd, close)) == STATUS_OK)
                res     = do_wrap(ofs, WRAP_CLOSE | WRAP_DELETE);

            if (res != STATUS_OK)
            {
                ofs->close();
                delete ofs;
            }

            return set_error(res);
        }

        status_t InBufStream::wrap_native(fhandle_t fd, bool close)
        {
            if (pIS != NULL)
                return set_error(STATUS_BAD_STATE);
            if (!init_buffer())
                return set_error(STATUS_NO_MEM);

            status_t res;

            InFileStream *ofs = new InFileStream();
            if (ofs == NULL)
                return set_error(STATUS_NO_MEM);

            if ((res = ofs->wrap_native(fd, close)) == STATUS_OK)
                res     = do_wrap(ofs, WRAP_CLOSE | WRAP_DELETE);

            if (res != STATUS_OK)
            {
                ofs->close();
                delete ofs;
            }

            return set_error(res);
        }

        status_t InBufStream::wrap(File *fd, size_t flags)
        {
            if (pIS != NULL)
                return set_error(STATUS_BAD_STATE);
            if (!init_buffer())
                return set_error(STATUS_NO_MEM);

            status_t res;

            InFileStream *ofs = new InFileStream();
            if (ofs == NULL)
                return set_error(STATUS_NO_MEM);

            if ((res = ofs->wrap(fd, flags)) == STATUS_OK)
                res     = do_wrap(ofs, WRAP_CLOSE | WRAP_DELETE);

            if (res != STATUS_OK)
            {
                ofs->close();
                delete ofs;
            }

            return set_error(res);
        }

        status_t InBufStream::wrap(IInStream *is, size_t flags)
        {
            if (pIS != NULL)
                return set_error(STATUS_BAD_STATE);
            else if (is == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

            if (!init_buffer())
                return set_error(STATUS_NO_MEM);

            return set_error(do_wrap(is, flags));
        }

        void InBufStream::set_buffering(bool enable)
        {
            nWrapFlags      = lsp_setflag(nWrapFlags, EXT_BUFFERING, enable);
        }

        void InBufStream::set_seek_buffering(bool enable)
        {
            nWrapFlags      = lsp_setflag(nWrapFlags, EXT_SEEK_BUFFERING, enable);
        }

        wssize_t InBufStream::avail()
        {
            const wssize_t res = pIS->avail();
            if (res < 0)
                return -set_error(status_t(-res));

            return res + nBufSize - nBufOff;
        }

    // Fixing GCC 16.1 false positive warnings
    #ifdef COMPILER_GCC
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Warray-bounds"
    #endif /* COMPILER_GCC */
        wssize_t InBufStream::position()
        {
            return nPosition + nBufOff;
        }
    #ifdef COMPILER_GCC
        #pragma GCC diagnostic pop
    #endif /* COMPILER_GCC */

        ssize_t InBufStream::read_byte()
        {
            if (pIS == NULL)
                return set_error(STATUS_BAD_STATE);

            // Check that byte is cached in buffer
            if (nBufOff < nBufSize)
            {
                const uint8_t byte  = vBuffer[(nBufHead + nBufOff++) % nBufCap];
                set_error(STATUS_OK);
                return byte;
            }

            // Fetch new byte
            ssize_t res = pIS->read_byte();
            if (res < 0)
                return -set_error(status_t(-res));

            // Push byte to buffer
            if (nWrapFlags & EXT_BUFFERING)
            {
                vBuffer[(nBufHead + nBufOff) % nBufCap] = uint8_t(res);
                if (nBufOff >= nBufHead)
                {
                    nBufHead        = (nBufHead + 1) % nBufCap;
                    ++nPosition;
                }
                else
                    nBufSize        = ++nBufOff;
            }
            else
            {
                nPosition      += nBufSize + 1;
                nBufSize        = 0;
            }

            return res;
        }

        wssize_t InBufStream::seek(wsize_t position)
        {
            if (pIS == NULL)
                return set_error(STATUS_BAD_STATE);

            // Do quick positioning if possible
            if ((position >= nPosition) && (position <= (nPosition + nBufSize)))
            {
                nBufOff             = uint32_t(position - nPosition);
                return position;
            }

            // Check that buffering is enabled
            wssize_t res;
            if ((nWrapFlags & (EXT_BUFFERING | EXT_SEEK_BUFFERING)) != (EXT_BUFFERING | EXT_SEEK_BUFFERING))
            {
                if ((res = pIS->seek(position)) < 0)
                    return -set_error(-status_t(res));

                clear_buffer();
                nPosition       = res;
                return res;
            }

            // Forward seek?
            if (position >= nPosition)
            {
                res                 = skip(position - nBufSize + nBufOff);
                if (res < 0)
                    return res;

                return this->position();
            }

            // Backward seek
            const wssize_t start    = lsp_max(wssize_t(position) - wssize_t(nBufCap), wssize_t(0));
            if ((res = pIS->seek(start)) < 0)
                return -set_error(-status_t(res));

            // Perform read
            ssize_t nread       = pIS->read(vBuffer, nBufCap);
            if (nread < 0)
                return -set_error(-status_t(nread));
            else if (wsize_t(start + nread) < position)
                return -set_error(STATUS_CORRUPTED);

            // Update buffer position
            nBufHead            = 0;
            nBufOff             = uint32_t(position - start);
            nBufSize            = uint32_t(nread);
            nPosition           = start;

            set_error(STATUS_OK);

            return position;
        }

        ssize_t InBufStream::read(void *dst, size_t count)
        {
            if (pIS == NULL)
                return set_error(STATUS_BAD_STATE);

            size_t avail, head, split;
            size_t nread    = 0;
            uint8_t *dptr   = static_cast<uint8_t *>(dst);

            // Check that bytes are cached in buffer
            avail       = uint32_t(lsp_min(count, size_t(nBufSize - nBufOff)));
            if (avail > 0)
            {
                head        = (nBufHead + nBufOff) % nBufCap;
                if ((head + avail) > nBufCap)
                {
                    split    = nBufCap - head;
                    memcpy(dptr, &vBuffer[head], split);
                    memcpy(&dptr[split], vBuffer, avail - split);
                }
                else
                    memcpy(dptr, &vBuffer[head], avail);

                nBufOff            += uint32_t(avail);
                nread              += avail;
                dptr               += avail;
            }
            if (nread >= count)
            {
                set_error(STATUS_OK);
                return nread;
            }

            // Perform read
            ssize_t res     = pIS->read(dptr, count - nread);
            if (res < 0)
                return (nread > 0) ? ssize_t(nread) : -set_error(status_t(-res));
            nread          += ssize_t(res);

            // Fill buffer with the data
            if (nWrapFlags & EXT_BUFFERING)
            {
                avail               = nBufCap - nBufOff;

                if (size_t(res) >= nBufCap)
                {
                    // Need to replace full buffer
                    dptr               += res - nBufCap;

                    memcpy(vBuffer, dptr, nBufCap);
                    nPosition           = nPosition + nBufSize + res - nBufCap;
                    nBufSize            = nBufCap;
                    nBufHead            = 0;
                    nBufOff             = nBufCap;
                }
                else
                {
                    // Append to the end of buffer
                    avail               = lsp_min(size_t(res), size_t(nBufCap));
                    dptr               += res - avail;

                    // Copy data
                    head                = (nBufHead + nBufOff) % nBufCap;
                    if ((head + avail) > nBufCap)
                    {
                        split               = nBufCap - head;
                        memcpy(&vBuffer[head], dptr, split);
                        memcpy(vBuffer, &dptr[split], avail - split);
                    }
                    else
                        memcpy(&vBuffer[head], dptr, avail);

                    // Update position
                    nBufOff            += res;
                    if (nBufOff > nBufCap)
                    {
                        split               = nBufOff % nBufCap;
                        nBufHead            = (nBufHead + split) % nBufCap;
                        nBufSize            = nBufCap;
                        nBufOff             = nBufCap;
                        nPosition          += split;
                    }
                    else
                        nBufSize            = nBufOff;
                }
            }
            else
            {
                nPosition      += nBufSize + nread;
                nBufSize        = 0;
            }

            return nread;
        }

        wssize_t InBufStream::skip(wsize_t amount)
        {
            if (pIS == NULL)
                return set_error(STATUS_BAD_STATE);

            // Do quick skip if possible
            uint32_t head, split;
            wsize_t skipped = 0;

            if (nBufOff < nBufSize)
            {
                split               = uint32_t(lsp_min(amount, wsize_t(nBufSize - nBufOff)));
                nBufOff            += uint32_t(split);
                skipped            += split;
                if (skipped >= amount)
                    return skipped;
            }

            // Check that buffering is enabled
            if ((nWrapFlags & (EXT_BUFFERING | EXT_SEEK_BUFFERING)) != (EXT_BUFFERING | EXT_SEEK_BUFFERING))
            {
                wssize_t res        = pIS->skip(amount - skipped);
                if (res < 0)
                    return -set_error(-status_t(res));

                nPosition      = nPosition + nBufOff + res;
                clear_buffer();

                return res;
            }

            // Do buffered skip using underlying read() operations
            while (skipped < amount)
            {
                head                    = (nBufHead + nBufOff) % nBufCap;
                split                   = uint32_t(lsp_min(amount - skipped, wsize_t(nBufCap - head)));
                ssize_t nread           = pIS->read(&vBuffer[head], split);
                if (nread <= 0)
                    return skipped;

                nBufOff                += uint32_t(nread);
                if (nBufOff > nBufCap)
                {
                    split                   = nBufOff - nBufCap;
                    nBufHead               += split;
                    nBufSize                = nBufCap;
                    nBufOff                 = nBufCap;
                    nPosition              += split;
                }
                else
                    nBufSize            = nBufOff;

                skipped            += nread;
            }

            return skipped;
        }

    } /* namespaec io */
} /* namespace lsp */


