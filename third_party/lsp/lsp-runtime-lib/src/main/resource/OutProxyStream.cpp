/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 20 мар. 2021 г.
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

#include <lsp-plug.in/resource/OutProxyStream.h>

namespace lsp
{
    namespace resource
    {
        OutProxyStream::OutProxyStream()
        {
            pOS         = NULL;
            nPosition   = 0;
            nWrapFlags  = 0;
        }

        OutProxyStream::~OutProxyStream()
        {
            // Close file descriptor
            if (pOS != NULL)
            {
                pOS->flush();

                if (nWrapFlags & WRAP_CLOSE)
                    pOS->close();
                if (nWrapFlags & WRAP_DELETE)
                    delete pOS;
                pOS         = NULL;
            }

            nPosition   = 0;
            nWrapFlags  = 0;
        }


        status_t OutProxyStream::close()
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

        status_t OutProxyStream::wrap(io::IOutStream *os, size_t flags)
        {
            if (pOS != NULL)
                return set_error(STATUS_BAD_STATE);

            pOS         = os;
            nWrapFlags  = flags;
            nPosition   = 0;

            return set_error(STATUS_OK);
        }

        wssize_t OutProxyStream::position()
        {
            return (pOS != NULL) ? nPosition : -set_error(STATUS_CLOSED);
        }

        ssize_t OutProxyStream::write(const void *buf, size_t count)
        {
            if (pOS == NULL)
                return -set_error(STATUS_CLOSED);

            ssize_t res = pOS->write(buf, count);
            if (res >= 0)
            {
                nPosition      += res;
                set_error(STATUS_OK);
                return res;
            }

            set_error(status_t(-res));
            return res;
        }

        wssize_t OutProxyStream::seek(wsize_t position)
        {
            if (pOS == NULL)
                return -set_error(STATUS_CLOSED);

            wssize_t pos = pOS->seek(position);
            if (pos >= 0)
            {
                nPosition = pos;
                set_error(STATUS_OK);
            }
            else
                set_error(status_t(-pos));

            return pos;
        }

        status_t OutProxyStream::flush()
        {
            return (pOS != NULL) ? pOS->flush() : set_error(STATUS_CLOSED);
        }

    } /* namespace resource */
} /* namespace lsp */


