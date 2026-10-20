/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 14 нояб. 2017 г.
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

#include <lsp-plug.in/io/IInStream.h>
#include <lsp-plug.in/io/IOutStream.h>
#include <lsp-plug.in/common/status.h>

#include <stdlib.h>

namespace lsp
{
    namespace io
    {
        
        IInStream::IInStream()
        {
            nErrorCode      = STATUS_OK;
        }
        
        IInStream::~IInStream()
        {
            nErrorCode      = STATUS_OK;
        }
    
        wssize_t IInStream::avail()
        {
            return - set_error(STATUS_NOT_IMPLEMENTED);
        }

        wssize_t IInStream::position()
        {
            return - set_error(STATUS_NOT_IMPLEMENTED);
        }

        ssize_t IInStream::read(void *dst, size_t count)
        {
            return - set_error(STATUS_NOT_IMPLEMENTED);
        }

        ssize_t IInStream::read_byte()
        {
            uint8_t byte;
            ssize_t nread = read(&byte, sizeof(byte));
            if (nread > 0)
                return byte;
            return (nread != 0) ? nread : -STATUS_EOF;
        }

        ssize_t IInStream::read_fully(void *dst, size_t count)
        {
            uint8_t *ptr    = reinterpret_cast<uint8_t *>(dst);
            size_t left     = count;
            while (left > 0)
            {
                ssize_t act_read = read(ptr, left);
                if (act_read < 0)
                {
                    if (left > count)
                        break;
                    else
                        return act_read;
                }

                left   -= act_read;
                ptr    += act_read;
            }

            return count - left;
        }

        status_t IInStream::read_block(void *dst, size_t count)
        {
            if (dst == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);
            else if (count == 0)
                return set_error(STATUS_OK);

            ssize_t read = read_fully(dst, count);
            if (read < 0)
                return set_error(status_t(-read));

            return set_error((size_t(read) == count) ? STATUS_OK : STATUS_EOF);
        }

        wssize_t IInStream::seek(wsize_t position)
        {
            return - set_error(STATUS_NOT_IMPLEMENTED);
        }

        wssize_t IInStream::skip(wsize_t amount)
        {
            uint8_t skip_buf[0x1000];

            wsize_t initial = amount;
            while (amount > 0)
            {
                ssize_t n = read(skip_buf, (amount > sizeof(skip_buf)) ? sizeof(skip_buf) : amount);
                if (n < 0)
                {
                    if (initial > amount)
                        break;
                    return n;
                }
                amount -= n;
            }

            return initial - amount;
        }

        wssize_t IInStream::sink(IOutStream *os, size_t buf_size)
        {
            if ((os == NULL) || (buf_size < 1))
                return -set_error(STATUS_BAD_ARGUMENTS);

            uint8_t *buf = reinterpret_cast<uint8_t *>(::malloc(buf_size));
            if (buf == NULL)
                return STATUS_NO_MEM;
            lsp_finally {
                ::free(buf);
            };

            wssize_t count = 0;
            while (true)
            {
                // Read data
                ssize_t nread = read(buf, buf_size);
                if (nread < 0)
                {
                    if (nread == -STATUS_EOF)
                    {
                        set_error(STATUS_OK);
                        return count;
                    }

                    set_error(status_t(-nread));
                    return nread;
                }
                count += nread;

                // Write data
                ssize_t off = 0;
                while (off < nread)
                {
                    ssize_t nwritten = os->write(&buf[off], nread-off);
                    if (nwritten < 0)
                    {
                        set_error(status_t(-nwritten));
                        return nwritten;
                    }
                    off    += nwritten;
                }
            }
        }

        status_t IInStream::close()
        {
            return set_error(nErrorCode);
        }

        status_t finalize(io::IInStream *is, size_t wrap_flags)
        {
            if (is == NULL)
                return STATUS_OK;
            status_t res = (wrap_flags & WRAP_CLOSE) ? is->close() : STATUS_OK;
            if (wrap_flags & WRAP_DELETE)
                delete is;
            return res;
        }

    } /* namespace io */
} /* namespace lsp */
