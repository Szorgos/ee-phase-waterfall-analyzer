/*
 * Copyright (C) 2023 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2023 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 16 апр. 2020 г.
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

#include <lsp-plug.in/mm/IInAudioStream.h>
#include <lsp-plug.in/mm/sample.h>
#include <lsp-plug.in/common/alloc.h>
#include <stdlib.h>

namespace lsp
{
    namespace mm
    {
        IInAudioStream::IInAudioStream()
        {
            nOffset             = -1;
            pBuffer             = NULL;
            nBufSize            = 0;
            nErrorCode          = STATUS_CLOSED;
            pUserData           = NULL;
            pDeleter            = NULL;
        }
        
        IInAudioStream::~IInAudioStream()
        {
            do_close();
            set_user_data(NULL);
        }

        void IInAudioStream::set_user_data(void *data, user_data_deleter_t deleter)
        {
            if (pDeleter != NULL)
                pDeleter(pUserData);
            pUserData   = data;
            pDeleter    = deleter;
        }

        void IInAudioStream::do_close()
        {
            // Free allocated buffer if present
            if (pBuffer != NULL)
            {
                ::free(pBuffer);
                pBuffer     = NULL;
            }

            nOffset     = -1;   // Mark as closed
        }

        status_t IInAudioStream::close()
        {
            do_close();
            return set_error(STATUS_OK);
        }
    
        status_t IInAudioStream::info(audio_stream_t *dst) const
        {
            if (dst == NULL)
                return STATUS_BAD_ARGUMENTS;

            dst->srate          = sample_rate();
            dst->channels       = channels();
            dst->frames         = length();
            dst->format         = format();

            return STATUS_OK;
        }

        size_t IInAudioStream::sample_rate() const
        {
            return 0;
        }

        size_t IInAudioStream::channels() const
        {
            return 0;
        }

        wssize_t IInAudioStream::length() const
        {
            return -1;
        }

        size_t IInAudioStream::format() const
        {
            return SFMT_NONE;
        }

        wssize_t IInAudioStream::skip(wsize_t nframes)
        {
            if (nframes == 0)
            {
                set_error(STATUS_OK);
                return 0;
            }

            // Select format and check frame size
            size_t afmt     = select_format(0);
            size_t asize    = sformat_size_of(afmt) * channels();
            if (asize <= 0)
                return -set_error(STATUS_UNSUPPORTED_FORMAT);

            // Perform direct read
            wssize_t nread = 0;
            do
            {
                // Ensure capacity
                size_t to_read      = (nframes > IO_BUF_SIZE) ? IO_BUF_SIZE : nframes;
                if (!ensure_capacity(to_read * asize))
                    return -set_error(STATUS_NO_MEM);

                // Perform read
                ssize_t read        = direct_read(pBuffer, to_read, 0);
                if (read < 0)
                {
                    if (nread > 0)
                        break;
                    set_error(-status_t(nread));
                    return nread;
                }

                // Update position
                nframes    -= read;
                nread      += read;
            } while (nframes > 0);

            // Update statistics
            set_error(STATUS_OK);
            nOffset    += nread;
            return nread;
        }

        wssize_t IInAudioStream::position()
        {
            return (nOffset >= 0) ? nOffset : -set_error(STATUS_CLOSED);
        }

        wssize_t IInAudioStream::seek(wsize_t offset)
        {
            if (nOffset < 0)
                return -set_error(STATUS_CLOSED);
            if (nOffset > wssize_t(offset))
                return -set_error(STATUS_NOT_SUPPORTED);

            return skip(offset - nOffset);
        }

        ssize_t IInAudioStream::direct_read(void *dst, size_t nframes, size_t fmt)
        {
            return -STATUS_NOT_IMPLEMENTED;
        }

        size_t IInAudioStream::select_format(size_t fmt)
        {
            return 0;
        }

        bool IInAudioStream::ensure_capacity(size_t bytes)
        {
            // Not enough space in temporary buffer?
            if (nBufSize >= bytes)
                return true;

            // Perform buffer re-allocation
            bytes           = align_size(bytes, 0x200);
            uint8_t *buf    = static_cast<uint8_t *>(::realloc(pBuffer, bytes));
            if (buf == NULL)
                return false;
            pBuffer         = buf;
            nBufSize        = bytes;

            return true;
        }

        ssize_t IInAudioStream::conv_read(void *dst, size_t nframes, size_t fmt)
        {
            if (nOffset < 0)
                return -set_error(STATUS_CLOSED);

            // Prepare pointers and remember frame size
            size_t nchan    = channels();
            size_t fsize    = sformat_size_of(fmt) * nchan;
            if (fsize <= 0)
                return -set_error(STATUS_BAD_FORMAT);

            size_t afmt     = select_format(fmt);
            size_t asize    = sformat_size_of(afmt) * nchan;
            if (asize <= 0)
                return -set_error(STATUS_UNSUPPORTED_FORMAT);

            uint8_t *dptr   = static_cast<uint8_t *>(dst);
            size_t nread    = 0;

            // Perform direct read
            if (fmt != afmt)
            {
                while (nframes > 0)
                {
                    // Ensure capacity
                    size_t to_read      = lsp_min(nframes, IO_BUF_SIZE);
                    if (!ensure_capacity(to_read * asize))
                        return -set_error(STATUS_NO_MEM);

                    // Perform direct read
                    ssize_t read = direct_read(pBuffer, to_read, afmt);

                    // Analyze read status
                    if (read < 0)
                    {
                        if (nread > 0)
                            break;
                        set_error(-status_t(read));
                        return read;
                    }

                    // Data is stored in pBuffer which can be updated, perform sample conversion
                    if (!convert_samples(dptr, pBuffer, read * nchan, fmt, afmt))
                        return -set_error(STATUS_UNSUPPORTED_FORMAT);

                    // Update position and pointers
                    nframes    -= read;
                    nread      += read;
                    dptr       += fsize * read;
                }
            }
            else
            {
                while (nframes > 0)
                {
                    // Select number of frames and perform read
                    size_t to_read      = lsp_min(nframes, IO_BUF_SIZE);
                    ssize_t read        = direct_read(dptr, to_read, afmt);

                    // Analyze read status
                    if (read < 0)
                    {
                        if (nread > 0)
                            break;
                        set_error(-status_t(read));
                        return read;
                    }

                    // Update position and pointers
                    nframes    -= read;
                    nread      += read;
                    dptr       += fsize * read;
                }
            }

            // Update statistics
            set_error(STATUS_OK);
            nOffset    += nread;
            return nread;
        }

        ssize_t IInAudioStream::read(uint8_t *dst, size_t nframes)
        {
            return conv_read(dst, nframes, SFMT_U8 | SFMT_CPU);
        }

        ssize_t IInAudioStream::read(int8_t *dst, size_t nframes)
        {
            return conv_read(dst, nframes, SFMT_S8 | SFMT_CPU);
        }

        ssize_t IInAudioStream::read(uint16_t *dst, size_t nframes)
        {
            return conv_read(dst, nframes, SFMT_U16 | SFMT_CPU);
        }

        ssize_t IInAudioStream::read(int16_t *dst, size_t nframes)
        {
            return conv_read(dst, nframes, SFMT_S16 | SFMT_CPU);
        }

        ssize_t IInAudioStream::read(uint32_t *dst, size_t nframes)
        {
            return conv_read(dst, nframes, SFMT_U32 | SFMT_CPU);
        }

        ssize_t IInAudioStream::read(int32_t *dst, size_t nframes)
        {
            return conv_read(dst, nframes, SFMT_S32 | SFMT_CPU);
        }

        ssize_t IInAudioStream::read(f32_t *dst, size_t nframes)
        {
            return conv_read(dst, nframes, SFMT_F32 | SFMT_CPU);
        }

        ssize_t IInAudioStream::read(f64_t *dst, size_t nframes)
        {
            return conv_read(dst, nframes, SFMT_F64 | SFMT_CPU);
        }

    } /* namespace mm */
} /* namespace lsp */
