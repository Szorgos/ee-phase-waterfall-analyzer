/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
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

#include <lsp-plug.in/mm/IOutAudioStream.h>
#include <lsp-plug.in/mm/sample.h>
#include <lsp-plug.in/common/alloc.h>
#include <lsp-plug.in/stdlib/string.h>
#include <stdlib.h>

namespace lsp
{
    namespace mm
    {
        
        IOutAudioStream::IOutAudioStream()
        {
            nOffset             = -1;
            pBuffer             = NULL;
            nBufSize            = 0;
            nErrorCode          = STATUS_CLOSED;
            sFormat.srate       = 0;
            sFormat.channels    = 0;
            sFormat.frames      = -1;
            sFormat.format      = SFMT_NONE;
            pUserData           = NULL;
            pDeleter            = NULL;
        }
        
        IOutAudioStream::~IOutAudioStream()
        {
            do_close();
            set_user_data(NULL);
        }

        void IOutAudioStream::set_user_data(void *data, user_data_deleter_t deleter)
        {
            if (pDeleter != NULL)
                pDeleter(pUserData);
            pUserData   = data;
            pDeleter    = deleter;
        }

        void IOutAudioStream::do_close()
        {
            if (pBuffer != NULL)
            {
                ::free(pBuffer);
                pBuffer = NULL;
            }
            nOffset = -1;
        }
    
        status_t IOutAudioStream::ensure_capacity(size_t bytes)
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

        ssize_t IOutAudioStream::direct_write(const void *src, size_t nframes, size_t fmt)
        {
            return -STATUS_NOT_IMPLEMENTED;
        }

        ssize_t IOutAudioStream::conv_write(const void *src, size_t nframes, size_t fmt)
        {
            if (nOffset < 0)
                return -set_error(STATUS_CLOSED);

            size_t rsize    = sformat_size_of(fmt) * sFormat.channels;
            if (rsize <= 0)
                return -set_error(STATUS_BAD_FORMAT);

            size_t afmt     = select_format(fmt);
            size_t fsize    = sformat_size_of(afmt) * sFormat.channels;
            if (fsize <= 0)
                return -set_error(STATUS_UNSUPPORTED_FORMAT);

            // Perform conversion loop
            const uint8_t *sptr = static_cast<const uint8_t *>(src);
            size_t nwritten = 0;

            while (nframes > 0)
            {
                size_t to_write = (nframes > IO_BUF_SIZE) ? IO_BUF_SIZE : nframes;

                // Need to perform encoding?
                if (afmt != fmt)
                {
                    // Check that we have enough place
                    size_t atotal = to_write * fsize;
                    size_t rtotal = to_write * rsize;
                    if (!ensure_capacity(to_write * (fsize + rsize)))
                        return -set_error(STATUS_NO_MEM);

                    // Perform conversion
                    ::memcpy(&pBuffer[atotal], src, rtotal);
                    if (!convert_samples(pBuffer, &pBuffer[atotal], to_write * sFormat.channels, afmt, fmt))
                        return -set_error(STATUS_UNSUPPORTED_FORMAT);
                    src = pBuffer;
                }
                else
                    src = sptr;

                // Call direct write
                ssize_t written = direct_write(src, to_write, afmt);
                if (written < 0)
                {
                    if (nwritten > 0)
                        break;
                    set_error(-status_t(written));
                    return written;
                }

                // Update pointers
                nwritten   += written;
                nframes    -= written;
                sptr       += fsize * written;
            }

            set_error(STATUS_OK);
            nOffset    += nwritten;
            return nwritten;
        }

        size_t IOutAudioStream::select_format(size_t rfmt)
        {
            return 0;
        }

        status_t IOutAudioStream::info(audio_stream_t *dst) const
        {
            if (dst == NULL)
                return STATUS_BAD_ARGUMENTS;
            *dst    = sFormat;
            return STATUS_OK;
        }

        status_t IOutAudioStream::flush()
        {
            return set_error(STATUS_OK);
        }

        status_t IOutAudioStream::close()
        {
            do_close();
            return set_error(STATUS_OK);
        }

        wssize_t IOutAudioStream::position()
        {
            return (nOffset >= 0) ? nOffset : -set_error(STATUS_CLOSED);
        }

        wssize_t IOutAudioStream::seek(wsize_t nframes)
        {
            return -set_error(STATUS_NOT_SUPPORTED);
        }

        ssize_t IOutAudioStream::write(const uint8_t *dst, size_t nframes)
        {
            return conv_write(dst, nframes, SFMT_U8 | SFMT_CPU);
        }

        ssize_t IOutAudioStream::write(const int8_t *dst, size_t nframes)
        {
            return conv_write(dst, nframes, SFMT_S8 | SFMT_CPU);
        }

        ssize_t IOutAudioStream::write(const uint16_t *dst, size_t nframes)
        {
            return conv_write(dst, nframes, SFMT_U16 | SFMT_CPU);
        }

        ssize_t IOutAudioStream::write(const int16_t *dst, size_t nframes)
        {
            return conv_write(dst, nframes, SFMT_S16 | SFMT_CPU);
        }

        ssize_t IOutAudioStream::write(const uint32_t *dst, size_t nframes)
        {
            return conv_write(dst, nframes, SFMT_U32 | SFMT_CPU);
        }

        ssize_t IOutAudioStream::write(const int32_t *dst, size_t nframes)
        {
            return conv_write(dst, nframes, SFMT_S32 | SFMT_CPU);
        }

        ssize_t IOutAudioStream::write(const f32_t *dst, size_t nframes)
        {
            return conv_write(dst, nframes, SFMT_F32 | SFMT_CPU);
        }

        ssize_t IOutAudioStream::write(const f64_t *dst, size_t nframes)
        {
            return conv_write(dst, nframes, SFMT_F64 | SFMT_CPU);
        }
    } /* namespace mm */
} /* namespace lsp */
