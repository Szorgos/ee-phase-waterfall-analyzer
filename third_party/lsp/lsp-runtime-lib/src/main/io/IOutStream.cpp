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

#include <lsp-plug.in/io/IOutStream.h>
#include <lsp-plug.in/common/status.h>

namespace lsp
{
    namespace io
    {
        
        IOutStream::IOutStream()
        {
            nErrorCode      = STATUS_OK;
        }
        
        IOutStream::~IOutStream()
        {
            nErrorCode      = STATUS_OK;
        }

        wssize_t IOutStream::position()
        {
            return - set_error(STATUS_NOT_IMPLEMENTED);
        }

        ssize_t IOutStream::write(const void *buf, size_t count)
        {
            return - set_error(STATUS_NOT_IMPLEMENTED);
        }

        ssize_t IOutStream::writeb(int v)
        {
            uint8_t b = v;
            return write(&b, 1);
        }

        status_t IOutStream::write_byte(int v)
        {
            uint8_t b = v;
            ssize_t n = write(&b, 1);
            if (n == 1)
                return STATUS_OK;
            return (n < 0) ? status_t(n) : STATUS_IO_ERROR;
        }

        wssize_t IOutStream::seek(wsize_t position)
        {
            return - set_error(STATUS_NOT_IMPLEMENTED);
        }

        status_t IOutStream::flush()
        {
            return - set_error(STATUS_NOT_IMPLEMENTED);
        }

        status_t IOutStream::close()
        {
            return set_error(STATUS_OK);
        }

        status_t finalize(io::IOutStream *os, size_t wrap_flags)
        {
            if (os == NULL)
                return STATUS_OK;
            status_t res = (wrap_flags & WRAP_CLOSE) ? os->close() : STATUS_OK;
            if (wrap_flags & WRAP_DELETE)
                delete os;
            return res;
        }
    
    } /* namespace ws */
} /* namespace lsp */
