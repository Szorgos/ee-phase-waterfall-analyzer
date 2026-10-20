/*
 * Copyright (C) 2022 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2022 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 23 окт. 2022 г.
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

#include <lsp-plug.in/fmt/lspc/IAudioFormatSelector.h>

namespace lsp
{
    namespace lspc
    {
        IAudioFormatSelector::IAudioFormatSelector()
        {
        }

        IAudioFormatSelector::~IAudioFormatSelector()
        {
        }

        status_t IAudioFormatSelector::decide(lspc::audio_format_t *out, const mm::audio_stream_t *in)
        {
            if ((out == NULL) || (in == NULL))
                return STATUS_BAD_ARGUMENTS;

            switch (mm::sformat_format(in->format))
            {
                case mm::SFMT_U8:   out->sample_format = lspc::SAMPLE_FMT_U8LE;     break;
                case mm::SFMT_S8:   out->sample_format = lspc::SAMPLE_FMT_S8LE;     break;
                case mm::SFMT_U16:  out->sample_format = lspc::SAMPLE_FMT_U16LE;    break;
                case mm::SFMT_S16:  out->sample_format = lspc::SAMPLE_FMT_S16LE;    break;
                case mm::SFMT_U24:  out->sample_format = lspc::SAMPLE_FMT_U24LE;    break;
                case mm::SFMT_S24:  out->sample_format = lspc::SAMPLE_FMT_S24LE;    break;
                case mm::SFMT_U32:  out->sample_format = lspc::SAMPLE_FMT_U32LE;    break;
                case mm::SFMT_S32:  out->sample_format = lspc::SAMPLE_FMT_S32LE;    break;
                case mm::SFMT_F32:  out->sample_format = lspc::SAMPLE_FMT_F32LE;    break;
                case mm::SFMT_F64:  out->sample_format = lspc::SAMPLE_FMT_F64LE;    break;
                default:
                    return STATUS_INCOMPATIBLE;
            }

            out->codec          = lspc::CODEC_PCM;
            out->sample_rate    = in->srate;

            return STATUS_OK;
        }
    } /* namespace lspc */
} /* namespace lsp */


