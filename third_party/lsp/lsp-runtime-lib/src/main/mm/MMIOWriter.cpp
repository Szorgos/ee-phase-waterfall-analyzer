/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 25 апр. 2020 г.
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

#include <lsp-plug.in/common/types.h>

#ifdef PLATFORM_WINDOWS

#include <lsp-plug.in/common/endian.h>
#include <private/mm/MMIOWriter.h>

namespace lsp
{
    namespace mm
    {
        
        MMIOWriter::MMIOWriter()
        {
            hMMIO           = NULL;
            nWritePos       = 0;
            nDataSize       = 0;
            nFrames         = -1;
            bSeekable       = false;
            
            ::ZeroMemory(&ckRiff, sizeof(ckRiff));
            ::ZeroMemory(&ckData, sizeof(ckData));
            ::ZeroMemory(&ckFact, sizeof(ckFact));
        }
        
        MMIOWriter::~MMIOWriter()
        {
            close();
        }

        status_t MMIOWriter::open(const LSPString *path, WAVEFORMATEX *fmt, wssize_t frames)
        {
            MMRESULT error;
            status_t res;
            MMCKINFO ckOut;

            nFrames = frames;

            // Open MMIO file
            hMMIO   = ::mmioOpenW(const_cast<lsp_utf16_t *>(path->get_utf16()), NULL, MMIO_ALLOCBUF | MMIO_READWRITE | MMIO_CREATE);
            if (hMMIO == NULL)
                return close(STATUS_IO_ERROR);

            // Create the output file RIFF chunk of form type 'WAVE'
            ckRiff.ckid         = FOURCC_RIFF;
            ckRiff.fccType      = mmioFOURCC('W', 'A', 'V', 'E');
            ckRiff.cksize       = 0;
            if ((error = ::mmioCreateChunk(hMMIO, &ckRiff, MMIO_CREATERIFF)) != 0)
                return close(STATUS_IO_ERROR);

            // Now create the 'fmt ' chunk. Since we know the size of this chunk,
            ckOut.ckid          = mmioFOURCC('f', 'm', 't', ' ');
            ckOut.cksize        =
                ((fmt->wFormatTag == WAVE_FORMAT_PCM) || (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)) ?
                    sizeof(PCMWAVEFORMAT) : sizeof(WAVEFORMATEX) + fmt->cbSize;
            if ((error = ::mmioCreateChunk(hMMIO, &ckOut, 0)) != 0)
                return close(STATUS_IO_ERROR);

            // Write the format structure to the 'fmt ' chunk
            if ((res = write_padded(fmt, ckOut.cksize)) != STATUS_OK)
                return close(res);

            // Ascend out of the 'fmt ' chunk, back into the 'RIFF' chunk.
            if ((error = ::mmioAscend(hMMIO, &ckOut, 0)) != 0)
                return close(STATUS_IO_ERROR);

            // Now create the fact chunk, not required by PCM but nice to have.
            ckFact.ckid         = mmioFOURCC('f', 'a', 'c', 't');
            ckFact.cksize       = 0;
            if ((error = ::mmioCreateChunk(hMMIO, &ckFact, 0)) != 0)
                return close(STATUS_IO_ERROR);

            DWORD factSize      = 0;
            if ((res = write_padded(&factSize, sizeof(DWORD))) != STATUS_OK)
                return close(res);

            // Ascend out of the 'fact' chunk, back into the 'RIFF' chunk.
            if ((error = ::mmioAscend(hMMIO, &ckFact, 0)) != 0)
                return close(STATUS_IO_ERROR);

            // Create the 'data' chunk that holds the waveform samples.
            ckData.ckid         = mmioFOURCC('d', 'a', 't', 'a');
            ckData.cksize       = 0;
            if ((error = ::mmioCreateChunk(hMMIO, &ckData, 0)) != 0)
                return close(STATUS_IO_ERROR);

            nWritePos           = 0;
            nDataSize           = 0;
            bSeekable           = ((fmt->wFormatTag == WAVE_FORMAT_PCM) || (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT));
            return STATUS_OK;
        }

        status_t MMIOWriter::finalize_riff_file()
        {
            MMRESULT error;

            // Seek to the end of chunk if position is wrong
            if (::mmioSeek(hMMIO, ckData.dwDataOffset + nDataSize, SEEK_SET) < 0)
                return STATUS_IO_ERROR;

            // Pad the ckData chunk
            if (nDataSize & 1)
            {
                // Write padding data
                BYTE data           = 0;
                ssize_t written     = ::mmioWrite(hMMIO, reinterpret_cast<HPSTR>(&data), sizeof(BYTE));
                if (written != 1)
                    return STATUS_IO_ERROR;
                nWritePos           = ++nDataSize;
            }

            // Ascend the output file out of the 'data' chunk
            // this will cause the chunk size of the 'data' chunk to be written.
            if ((error = ::mmioAscend(hMMIO, &ckData, 0)) != 0)
                return STATUS_IO_ERROR;

            // Ascend the RIFF chunk
            if ((error = ::mmioAscend(hMMIO, &ckRiff, 0)) != 0)
                return STATUS_IO_ERROR;

            // Seek to the begin of 'fact' chunk
            if (::mmioSeek(hMMIO, ckFact.dwDataOffset, SEEK_SET) < 0)
                return STATUS_IO_ERROR;

            // Write actual size of the 'data' chunk in samples
            DWORD factSize      = nFrames;
            factSize            = CPU_TO_LE(factSize);
            if ((mmioWrite(hMMIO, reinterpret_cast<HPSTR>(&factSize), sizeof(DWORD))) != sizeof(DWORD))
                return STATUS_IO_ERROR;

            return STATUS_OK;
        }

        wssize_t MMIOWriter::seek(wsize_t offset)
        {
            if (!bSeekable)
                return -STATUS_NOT_SUPPORTED;

            if (offset > nDataSize)
                offset = nDataSize;

            if (::mmioSeek(hMMIO, ckData.dwDataOffset + offset, SEEK_SET) < 0)
                return -STATUS_IO_ERROR;

            return nWritePos = offset;
        }

        ssize_t MMIOWriter::write(const void *buf, size_t count)
        {
            ssize_t written     = ::mmioWrite(hMMIO, reinterpret_cast<HPSTR>(const_cast<void *>(buf)), count);
            if (written < 0)
                return -STATUS_IO_ERROR;
            nWritePos          += written;
            if (nDataSize < nWritePos)
                nDataSize       = nWritePos;
            return written;
        }

        status_t MMIOWriter::write_padded(const void *buf, size_t count)
        {
            // Write data
            ssize_t written     = ::mmioWrite(hMMIO, reinterpret_cast<HPSTR>(const_cast<void *>(buf)), count);
            if ((written < 0) || (size_t(written) != count))
                return STATUS_IO_ERROR;

            // Need to pad?
            if (count & 1)
            {
                BYTE data           = 0;
                ssize_t written     = ::mmioWrite(hMMIO, reinterpret_cast<HPSTR>(&data), sizeof(BYTE));
                if (written != 1)
                    return STATUS_IO_ERROR;
            }

            return STATUS_OK;
        }

        status_t MMIOWriter::flush()
        {
            return (::mmioFlush(hMMIO, MMIO_EMPTYBUF) == 0) ? STATUS_OK : STATUS_IO_ERROR;
        }

        status_t MMIOWriter::close(status_t code)
        {
            if (hMMIO != NULL)
            {
                // If all is OK, then finalize the RIFF file
                if (code == STATUS_OK)
                    code    = finalize_riff_file();

                ::mmioClose(hMMIO, 0);
                hMMIO       = NULL;
            }

            nWritePos       = 0;
            nDataSize       = 0;
            nFrames         = -1;
            bSeekable       = false;

            ::ZeroMemory(&ckRiff, sizeof(ckRiff));
            ::ZeroMemory(&ckData, sizeof(ckData));
            ::ZeroMemory(&ckFact, sizeof(ckFact));

            return code;
        }
    
    } /* namespace mm */
} /* namespace lsp */

#endif /* PLATFORM_WINDOWS */
