/*
 * Copyright (C) 2024 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2024 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 27 окт. 2022 г.
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

#include <lsp-plug.in/fmt/lspc/InAudioStream.h>

namespace lsp
{
    namespace lspc
    {
        InAudioStream::InAudioStream(AudioReader *in, const mm::audio_stream_t *fmt, bool free)
        {
            pReader     = in;
            bDelete     = free;
            sFormat     = *fmt;
            nOffset     = 0;
        }

        InAudioStream::~InAudioStream()
        {
            do_close();
        }

        status_t InAudioStream::do_close()
        {
            nOffset         = -1;
            if (pReader == NULL)
                return STATUS_OK;

            status_t res    = pReader->close();
            if (bDelete)
                delete pReader;
            pReader         = NULL;
            return res;
        }

        status_t InAudioStream::close()
        {
            return set_error(do_close());
        }

        size_t InAudioStream::select_format(size_t fmt)
        {
            return mm::SFMT_F32_CPU;
        }

        wssize_t InAudioStream::skip(wsize_t nframes)
        {
            if (pReader == NULL)
                return -set_error(STATUS_CLOSED);
            ssize_t nskipped = pReader->skip_frames(nframes);
            set_error((nskipped < 0) ? status_t(-nskipped) : STATUS_OK);

            return nskipped;
        }

        ssize_t InAudioStream::direct_read(void *dst, size_t nframes, size_t fmt)
        {
            if (dst == NULL)
                return -set_error(STATUS_BAD_ARGUMENTS);
            if (pReader == NULL)
                return -set_error(STATUS_CLOSED);
            if (fmt != sFormat.format)
                return -set_error(STATUS_BAD_STATE);

            ssize_t nread = pReader->read_frames(reinterpret_cast<float *>(dst), nframes);
            if (nread < 0)
                set_error(status_t(-nread));
            else if (nread == 0)
                return -set_error(STATUS_EOF);

            set_error(STATUS_OK);
            return nread;
        }

        status_t InAudioStream::info(mm::audio_stream_t *dst) const
        {
            if (dst == NULL)
                return STATUS_BAD_ARGUMENTS;
            *dst                = sFormat;
            return STATUS_OK;
        }

        size_t InAudioStream::sample_rate() const
        {
            return sFormat.srate;
        }

        size_t InAudioStream::channels() const
        {
            return sFormat.channels;
        }

        wssize_t InAudioStream::length() const
        {
            return sFormat.frames;
        }

        size_t InAudioStream::format() const
        {
            return sFormat.format;
        }

    } /* namespace lspc */
} /* namespace lsp */


