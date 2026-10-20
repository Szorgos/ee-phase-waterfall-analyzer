/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 12 февр. 2021 г.
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

#include <lsp-plug.in/io/InMarkSequence.h>

#define MARKSEQ_SIZE        0x400

namespace lsp
{
    namespace io
    {
        InMarkSequence::InMarkSequence()
        {
            pSequence       = NULL;
            nMarkPos        = 0;
            nMarkLen        = -1;
            nMarkMax        = 0;
            pBuf            = NULL;
            nBufCap         = 0;
            bClose          = false;
        }

        InMarkSequence::~InMarkSequence()
        {
            do_close();
        }

        status_t InMarkSequence::wrap(IInSequence *in, bool close)
        {
            if (in == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);
            if (pSequence != NULL)
                return set_error(STATUS_BAD_STATE);

            pSequence       = in;
            bClose          = close;

            clear_mark();

            return set_error(STATUS_OK);
        }

        status_t InMarkSequence::do_close()
        {
            status_t res = STATUS_OK;

            if ((pSequence != NULL) && (bClose))
                res         = pSequence->close();

            pSequence       = NULL;
            bClose          = false;

            clear_mark();

            return res;
        }

        status_t InMarkSequence::close()
        {
            status_t res = do_close();
            return set_error(res);
        }

        ssize_t InMarkSequence::grow_buffer(size_t amount)
        {
            size_t last = lsp_min(nMarkMax, ssize_t(nMarkPos + amount));
            if (last <= nBufCap)
                return nBufCap - nMarkPos;
            last        = lsp_min(last, size_t(MARKSEQ_SIZE));
            size_t cap  = lsp_max(last, (nBufCap + (nBufCap >> 1)));

            lsp_wchar_t *buf    = reinterpret_cast<lsp_wchar_t *>(realloc(pBuf, sizeof(lsp_wchar_t) * cap));
            if (buf == NULL)
                return -STATUS_NO_MEM;

            pBuf        = buf;
            nBufCap     = cap;

            return cap - nMarkPos;
        }

        void InMarkSequence::clear_mark()
        {
            nMarkPos    = 0;
            nMarkLen    = 0;
            nMarkMax    = -1;

            if (pBuf != NULL)
                free(pBuf);
            pBuf        = NULL;
            nBufCap     = 0;
        }

        ssize_t InMarkSequence::read(lsp_wchar_t *dst, size_t count)
        {
            ssize_t nread;
            if (pSequence == NULL)
                return -set_error(STATUS_CLOSED);

            // Simple read if there is no mark set or mark buffer is full
            if (nMarkMax < 0)
            {
                nread = pSequence->read(dst, count);
                if (nread < 0)
                    set_error(status_t(-nread));
                return nread;
            }

            // Fetch all pending characters from the buffer
            size_t total    = 0;
            nread           = lsp_min(count, size_t(nMarkLen - nMarkPos));
            if (nread > 0)
            {
                memcpy(dst, &pBuf[nMarkPos], sizeof(lsp_wchar_t) * nread);
                nMarkPos       += nread;
                total          += nread;
                dst            += nread;

                if ((count -= nread) <= 0)
                {
                    set_error(STATUS_OK);
                    return total;
                }
            }

            // Read data to buffer and emit to output
            while (nMarkPos < nMarkMax)
            {
                // Grow buffer
                ssize_t avail   = grow_buffer(MARKSEQ_SIZE);
                if (avail < 0)
                    return -set_error(STATUS_NO_MEM);
                if (avail > ssize_t(count))
                    avail       = count;

                // Read data
                nread       = pSequence->read(&pBuf[nMarkPos], avail);
                if (nread < 0)
                {
                    if (total > 0)
                    {
                        set_error(STATUS_OK);
                        return total;
                    }

                    set_error(status_t(-nread));
                    return nread;
                }

                // Copy data and pdate positions
                memcpy(dst, &pBuf[nMarkPos], sizeof(lsp_wchar_t) * nread);

                nMarkPos   += nread;
                nMarkLen   += nread;
                total      += nread;
                dst        += nread;

                if ((count -= nread) <= 0)
                {
                    set_error(STATUS_OK);
                    return total;
                }
            }

            // Reset mark if we are out of buffer and return
            nread      = pSequence->read(dst, count);
            if (nread > 0)
            {
                set_error(STATUS_OK);
                clear_mark();
                return total + nread;
            }
            else if (total > 0)
            {
                set_error(STATUS_OK);
                return total;
            }
            set_error(status_t(-nread));
            return nread;
        }

        lsp_swchar_t InMarkSequence::read()
        {
            lsp_swchar_t ch;
            if (pSequence == NULL)
                return -set_error(STATUS_CLOSED);

            // Simple read if there is no mark set or mark buffer is full
            if (nMarkMax < 0)
            {
                ch = pSequence->read();
                set_error((ch < 0) ? -ch : STATUS_OK);
                return ch;
            }

            // Check that we should read from buffer
            if (nMarkPos < nMarkLen)
            {
                set_error(STATUS_OK);
                return pBuf[nMarkPos++];
            }

            // Read the character
            ch = pSequence->read();
            if (ch < 0)
            {
                set_error(-ch);
                return ch;
            }

            // Reset mark if we are out of buffer and return
            if (nMarkPos >= nMarkMax)
            {
                set_error(STATUS_OK);
                clear_mark();
                return ch;
            }

            // Push character to buffer, update counters and return
            if (grow_buffer(1) < 0)
                return -set_error(STATUS_NO_MEM);

            set_error(STATUS_OK);
            nMarkLen++;
            pBuf[nMarkPos++]        = ch;

            return ch;
        }

        ssize_t InMarkSequence::skip(size_t count)
        {
            if (pSequence == NULL)
                return -set_error(STATUS_CLOSED);

            // No mark set?
            if (nMarkMax < 0)
            {
                ssize_t res = pSequence->skip(count);
                set_error((res < 0) ? status_t(-res) : STATUS_OK);
                return res;
            }

            // Skip all pending characters in the buffer
            size_t skipped  = 0;
            ssize_t step    = lsp_min(count, size_t(nMarkLen - nMarkPos));
            nMarkPos       += step;
            skipped        += step;

            if ((count -= skipped) <= 0)
            {
                set_error(STATUS_OK);
                return skipped;
            }

            // Read data to buffer
            while (nMarkPos < nMarkMax)
            {
                // Grow buffer
                ssize_t avail   = grow_buffer(MARKSEQ_SIZE);
                if (avail < 0)
                    return -set_error(STATUS_NO_MEM);
                if (avail > ssize_t(count))
                    avail       = count;

                // Read data
                ssize_t nread   = pSequence->read(&pBuf[nMarkPos], avail);
                if (nread < 0)
                {
                    if (skipped > 0)
                    {
                        set_error(STATUS_OK);
                        return skipped;
                    }

                    set_error(status_t(-nread));
                    return nread;
                }

                // Update positions
                nMarkPos   += nread;
                nMarkLen   += nread;
                skipped    += nread;

                if ((count -= skipped) <= 0)
                {
                    set_error(STATUS_OK);
                    return skipped;
                }
            }

            // Reset mark if we are out of buffer and return
            step        = pSequence->skip(count);
            if (step > 0)
            {
                set_error(STATUS_OK);
                clear_mark();
                return skipped + step;
            }
            else if (skipped > 0)
            {
                set_error(STATUS_OK);
                return skipped;
            }
            set_error(status_t(-step));
            return step;
        }

        status_t InMarkSequence::mark(ssize_t limit)
        {
            if (pSequence == NULL)
                return set_error(STATUS_CLOSED);
            else if (limit < 0)
                return set_error(STATUS_OK);

            // Clear previous mark
            nMarkPos        = 0;
            nMarkLen        = 0;
            nMarkMax        = limit;

            return set_error(STATUS_OK);
        }

        status_t InMarkSequence::reset()
        {
            if (pSequence == NULL)
                return set_error(STATUS_CLOSED);
            else if (nMarkMax < 0)
                return set_error(STATUS_NOT_FOUND);

            nMarkPos        = 0;
            return set_error(STATUS_OK);
        }


    } /* namespace io */
} /* namespace lsp */
