/*
 * Copyright (C) 2024 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2024 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 14 июн. 2018 г.
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

#include <lsp-plug.in/io/IInSequence.h>
#include <lsp-plug.in/io/IOutSequence.h>
#include <lsp-plug.in/common/debug.h>

namespace lsp
{
    namespace io
    {
        static lsp_wchar_t skip_buf[0x1000];

        IInSequence::IInSequence()
        {
            nErrorCode      = STATUS_OK;
        }

        IInSequence::~IInSequence()
        {
        }

        ssize_t IInSequence::read(lsp_wchar_t *dst, size_t count)
        {
            return -set_error(STATUS_EOF);
        }

        lsp_swchar_t IInSequence::read()
        {
            return -set_error(STATUS_EOF);
        }

        status_t IInSequence::read_line(LSPString *s, bool force)
        {
            return set_error(STATUS_EOF);
        }

        ssize_t IInSequence::skip(size_t count)
        {
            ssize_t skipped = 0;

            while (count > 0)
            {
                size_t to_read  = (count > ((sizeof(skip_buf))/sizeof(lsp_wchar_t))) ?
                        ((sizeof(skip_buf))/sizeof(lsp_wchar_t)) : count;
                ssize_t nread   = read(skip_buf, to_read);
                if (nread <= 0)
                    break;

                count      -= nread;
                skipped    += nread;
            }
            return skipped;
        }

        status_t IInSequence::close()
        {
            return set_error(STATUS_OK);
        }

        status_t IInSequence::mark(ssize_t limit)
        {
            return set_error(STATUS_NOT_SUPPORTED);
        }

        status_t IInSequence::reset()
        {
            return set_error(STATUS_NOT_SUPPORTED);
        }

        wssize_t IInSequence::sink(IOutSequence *os, size_t buf_size)
        {
            if ((os == NULL) || (buf_size < 1))
                return -set_error(STATUS_BAD_ARGUMENTS);

            lsp_wchar_t *buf = reinterpret_cast<lsp_wchar_t *>(::malloc(buf_size * sizeof(lsp_wchar_t)));
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
                status_t res = os->write(buf, nread);
                if (res != STATUS_OK)
                {
                    set_error(-res);
                    return -res;
                }
            }
        }

    } /* namespace io */
} /* namespace lsp */
