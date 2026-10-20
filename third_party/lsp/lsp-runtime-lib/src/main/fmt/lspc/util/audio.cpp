/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 24 окт. 2022 г.
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

#include <lsp-plug.in/common/alloc.h>
#include <lsp-plug.in/fmt/lspc/util/audio.h>
#include <lsp-plug.in/fmt/lspc/AudioReader.h>
#include <lsp-plug.in/fmt/lspc/AudioWriter.h>
#include <lsp-plug.in/fmt/lspc/IAudioFormatSelector.h>
#include <lsp-plug.in/fmt/lspc/InAudioStream.h>
#include <lsp-plug.in/mm/InAudioFileStream.h>
#include <lsp-plug.in/mm/OutAudioFileStream.h>
#include <lsp-plug.in/stdlib/stdlib.h>

namespace lsp
{
    namespace lspc
    {
        static IAudioFormatSelector default_selector;

        LSP_RUNTIME_LIB_PUBLIC
        status_t write_audio(chunk_id_t *chunk_id, File *file, const char *path, IAudioFormatSelector *selector, size_t buf_size)
        {
            mm::InAudioFileStream is;
            status_t res = is.open(path);
            if (res != STATUS_OK)
                return res;
            res = write_audio(chunk_id, file, &is, selector, buf_size);
            status_t res2 = is.close();
            return (res != STATUS_OK) ? res2 : res;
        }

        LSP_RUNTIME_LIB_PUBLIC
        status_t write_audio(chunk_id_t *chunk_id, File *file, const io::Path *path, IAudioFormatSelector *selector, size_t buf_size)
        {
            mm::InAudioFileStream is;
            status_t res = is.open(path);
            if (res != STATUS_OK)
                return res;
            res = write_audio(chunk_id, file, &is, selector, buf_size);
            status_t res2 = is.close();
            return (res != STATUS_OK) ? res2 : res;
        }

        LSP_RUNTIME_LIB_PUBLIC
        status_t write_audio(chunk_id_t *chunk_id, File *file, const LSPString *path, IAudioFormatSelector *selector, size_t buf_size)
        {
            mm::InAudioFileStream is;
            status_t res = is.open(path);
            if (res != STATUS_OK)
                return res;
            res = write_audio(chunk_id, file, &is, selector, buf_size);
            status_t res2 = is.close();
            return (res != STATUS_OK) ? res2 : res;
        }

        LSP_RUNTIME_LIB_PUBLIC
        status_t write_audio(chunk_id_t *chunk_id, File *file, mm::IInAudioStream *is, IAudioFormatSelector *selector, size_t buf_size)
        {
            status_t res;
            if (is == NULL)
                return STATUS_BAD_ARGUMENTS;

            // Select audio format
            audio_format_t ofmt;
            mm::audio_stream_t ifmt;
            if ((res = is->info(&ifmt)) != STATUS_OK)
                return res;
            if (selector == NULL)
                selector            = &default_selector;
            if ((res = selector->decide(&ofmt, &ifmt)) != STATUS_OK)
                return res;

            // Create audio writer
            audio_parameters_t params;
            AudioWriter wr;
            params.channels     = ifmt.channels;
            params.frames       = ifmt.frames;
            params.sample_rate  = ofmt.sample_rate;
            params.codec        = ofmt.codec;
            params.sample_format= ofmt.sample_format;
            if ((res = wr.open(file, &params, false)) != STATUS_OK)
                return res;
            lsp_finally { wr.close(); };
            chunk_id_t res_chunk_id = wr.unique_id();

            // Allocate buffer for I/O
            size_t min_buf_size = ifmt.channels * sizeof(float);
            buf_size            = lsp_max(min_buf_size, buf_size - (buf_size % min_buf_size));
            float *data         = static_cast<float *>(malloc(buf_size));
            if (data == NULL)
                return STATUS_NO_MEM;
            lsp_finally { free(data); };
            ssize_t max_frames  = buf_size / (ifmt.channels * sizeof(float));

            // Perform data copy
            for (wssize_t frame = 0; frame < ifmt.frames; )
            {
                size_t to_do        = lsp_min(max_frames, ifmt.frames - frame);
                ssize_t nread       = is->read(data, to_do);
                if (nread < 0)
                    return status_t(-nread);
                if ((res = wr.write_frames(data, nread)) != STATUS_OK)
                    return res;
                frame              += nread;
            }

            // Close the audio chunk writer
            if ((res = wr.close()) != STATUS_OK)
                return res;

            // Return success result
            if (chunk_id != NULL)
                *chunk_id           = res_chunk_id;
            return STATUS_OK;
        }

        LSP_RUNTIME_LIB_PUBLIC
        status_t write_audio(
            chunk_id_t *chunk_id, File *file,
            const float *frames, const audio_parameters_t *params)
        {
            status_t res;
            if ((frames == NULL) || (params == NULL))
                return STATUS_BAD_ARGUMENTS;

            // Create audio writer
            AudioWriter wr;
            if ((res = wr.open(file, params, false)) != STATUS_OK)
                return res;
            lsp_finally { wr.close(); };
            chunk_id_t res_chunk_id = wr.unique_id();

            // Perform data write
            if ((res = wr.write_frames(frames, params->frames)) != STATUS_OK)
                return res;

            // Close the audio chunk writer
            if ((res = wr.close()) != STATUS_OK)
                return res;

            // Return success result
            if (chunk_id != NULL)
                *chunk_id           = res_chunk_id;
            return STATUS_OK;
        }

        status_t copy_frames(mm::IOutAudioStream *os, lspc::AudioReader *is, const audio_parameters_t *params, size_t buf_size)
        {
            // Allocate buffer for I/O
            size_t min_buf_size = params->channels * sizeof(float);
            buf_size            = lsp_max(min_buf_size, buf_size - (buf_size % min_buf_size));
            float *data         = static_cast<float *>(malloc(buf_size));
            if (data == NULL)
                return STATUS_NO_MEM;
            lsp_finally { free(data); };
            size_t max_frames   = buf_size / (params->channels * sizeof(float));

            // Perform data copy
            for (wsize_t frame = 0; frame < params->frames; )
            {
                size_t to_do        = lsp_min(max_frames, params->frames - frame);
                ssize_t nread       = is->read_frames(data, to_do);
                if (nread < 0)
                    return status_t(-nread);
                ssize_t nwritten    = os->write(data, nread);
                if (nwritten < 0)
                    return status_t(-nwritten);
                else if (nwritten < nread)
                    return STATUS_IO_ERROR;
                frame              += nread;
            }

            // Return success result
            return STATUS_OK;
        }

        LSP_RUNTIME_LIB_PUBLIC
        status_t read_audio(
            chunk_id_t chunk_id, File *file,
            const char *path, size_t format, size_t codec, size_t buf_size)
        {
            io::Path tmp;
            status_t res = tmp.set(path);
            if (res != STATUS_OK)
                return res;
            return read_audio(chunk_id, file, &tmp, format, codec, buf_size);
        }

        LSP_RUNTIME_LIB_PUBLIC
        status_t read_audio(
            chunk_id_t chunk_id, File *file,
            const LSPString *path, size_t format, size_t codec, size_t buf_size)
        {
            io::Path tmp;
            status_t res = tmp.set(path);
            if (res != STATUS_OK)
                return res;
            return read_audio(chunk_id, file, &tmp, format, codec, buf_size);
        }

        LSP_RUNTIME_LIB_PUBLIC
        status_t read_audio(
            chunk_id_t chunk_id, File *file,
            const io::Path *path, size_t format, size_t codec, size_t buf_size)
        {
            if ((file == NULL) || (path == NULL))
                return STATUS_BAD_ARGUMENTS;

            // Read the chunk
            lspc::AudioReader rd;
            status_t res = rd.open(file, chunk_id);
            if (res != STATUS_OK)
                return res;
            lsp_finally { rd.close(); };

            // Get audio parameters and open output audio stream
            audio_parameters_t aparams;
            mm::audio_stream_t sparams;
            mm::OutAudioFileStream os;
            if ((res = rd.get_parameters(&aparams)) != STATUS_OK)
                return res;

            sparams.channels    = aparams.channels;
            sparams.srate       = aparams.sample_rate;
            sparams.frames      = aparams.frames;
            sparams.format      = format;

            if ((res = os.open(path, &sparams, codec)) != STATUS_OK)
                return res;
            lsp_finally { os.close(); };

            // Copy data and close streams
            if ((res = copy_frames(&os, &rd, &aparams, buf_size)) != STATUS_OK)
                return res;
            if ((res = os.close()) != STATUS_OK)
                return res;
            if ((res = rd.close()) != STATUS_OK)
                return res;

            // Return result
            return STATUS_OK;
        }

        LSP_RUNTIME_LIB_PUBLIC
        status_t read_audio(
            chunk_id_t chunk_id, File *file,
            float **frames, audio_parameters_t *params)
        {
            if ((file == NULL) || (frames == NULL))
                return STATUS_BAD_ARGUMENTS;

            // Read the chunk
            lspc::AudioReader rd;
            status_t res = rd.open(file, chunk_id);
            if (res != STATUS_OK)
                return res;
            lsp_finally { rd.close(); };

            // Get audio parameters
            audio_parameters_t aparams;
            if ((res = rd.get_parameters(&aparams)) != STATUS_OK)
                return res;

            // Allocate the buffer of necessary size
            size_t buf_size     = align_size(sizeof(float) * aparams.channels * aparams.frames, DEFAULT_ALIGN);
            float *data         = static_cast<float *>(malloc(buf_size));
            if (data == NULL)
                return STATUS_NO_MEM;
            lsp_finally {
                if (data != NULL)
                    free(data);
            };

            // Read the contents and close stream
            wssize_t read       = rd.read_frames(data, aparams.frames);
            if (read < 0)
                return status_t(-read);
            else if (read < wssize_t(aparams.frames))
                return STATUS_CORRUPTED;
            if ((res = rd.close()) != STATUS_OK)
                return res;

            // Return the result
            *frames             = data;
            data                = NULL;
            if (params != NULL)
                *params             = aparams;
            return STATUS_OK;
        }

        LSP_RUNTIME_LIB_PUBLIC
        status_t read_audio(
            chunk_id_t chunk_id, File *file,
            mm::IOutAudioStream *os, audio_parameters_t *params, size_t buf_size)
        {
            if ((file == NULL) || (os == NULL))
                return STATUS_BAD_ARGUMENTS;

            // Read the chunk
            lspc::AudioReader rd;
            status_t res = rd.open(file, chunk_id);
            if (res != STATUS_OK)
                return res;
            lsp_finally { rd.close(); };

            // Get audio parameters
            audio_parameters_t aparams;
            if ((res = rd.get_parameters(&aparams)) != STATUS_OK)
                return res;
            if (os->sample_rate() != aparams.sample_rate)
                return STATUS_INCOMPATIBLE;

            // Copy data and close streams
            if ((res = copy_frames(os, &rd, &aparams, buf_size)) != STATUS_OK)
                return res;
            if ((res = rd.close()) != STATUS_OK)
                return res;

            // Return result
            if (params != NULL)
                *params             = aparams;
            return STATUS_OK;
        }

        LSP_RUNTIME_LIB_PUBLIC
        status_t read_audio(chunk_id_t chunk_id, File *file, mm::IInAudioStream **is)
        {
            if ((file == NULL) || (is == NULL))
                return STATUS_BAD_ARGUMENTS;

            // Read the chunk
            lspc::AudioReader *rd = new lspc::AudioReader();
            if (rd == NULL)
                return STATUS_NO_MEM;
            lsp_finally {
                if (rd != NULL)
                    delete rd;
            };

            status_t res = rd->open(file, chunk_id);
            if (res != STATUS_OK)
                return res;

            // Get audio parameters
            mm::audio_stream_t sparams;
            audio_parameters_t aparams;
            if ((res = rd->get_parameters(&aparams)) != STATUS_OK)
                return res;

            sparams.srate       = aparams.sample_rate;
            sparams.channels    = aparams.channels;
            sparams.frames      = aparams.frames;
            sparams.format      = mm::SFMT_F32_CPU;

            // Create audio stream
            InAudioStream *ias = new InAudioStream(rd, &sparams, true);
            if (ias == NULL)
                return STATUS_NO_MEM;

            // Return result
            *is                 = ias;
            ias                 = NULL;
            rd                  = NULL;
            return STATUS_OK;
        }
    } /* namespace lspc */
} /* namespace lsp */



