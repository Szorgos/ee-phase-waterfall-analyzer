/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 14 янв. 2018 г.
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

#include <lsp-plug.in/common/endian.h>
#include <lsp-plug.in/stdlib/string.h>
#include <lsp-plug.in/fmt/lspc/ChunkWriter.h>

namespace lsp
{
    namespace lspc
    {
        ChunkWriter::ChunkWriter(Resource *fd, uint32_t magic):
            ChunkAccessor(fd, magic),
            sStream(this)
        {
            nChunksOut      = 0;
            nPosition       = 0;
            if (last_error() != STATUS_OK)
                return;
            if (pFile != NULL)
                set_error(pFile->allocate(&nUID));
        }

        ChunkWriter::~ChunkWriter()
        {
        }
    
        status_t ChunkWriter::do_flush(size_t flags)
        {
            if (pFile == NULL)
                return set_error(STATUS_CLOSED);

            if ((nBufPos > 0) || ((flags & F_FORCE) && (nChunksOut <= 0)) || (flags & F_LAST))
            {
                chunk_header_t hdr;
                hdr.magic       = nMagic;
                hdr.size        = uint32_t(nBufPos);
                hdr.flags       = (flags & F_LAST) ? LSPC_CHUNK_FLAG_LAST : 0;
                hdr.uid         = nUID;

                // Convert CPU -> BE
                hdr.magic       = CPU_TO_BE(hdr.magic);
                hdr.size        = CPU_TO_BE(hdr.size);
                hdr.flags       = CPU_TO_BE(hdr.flags);
                hdr.uid         = CPU_TO_BE(hdr.uid);

                // Write buffer header and data to file
                status_t res    = pFile->write(&hdr, sizeof(chunk_header_t));
                if (res == STATUS_OK)
                    pFile->write(pBuffer, nBufPos);
                if (set_error(res) != STATUS_OK)
                    return res;

                // Flush the buffer
                nBufPos         = 0;
                nChunksOut      ++;
            }

            return STATUS_OK;
        }

        status_t ChunkWriter::write(const void *buf, size_t count)
        {
            if (pFile == NULL)
                return set_error(STATUS_CLOSED);

            chunk_header_t hdr;
            const uint8_t *src = static_cast<const uint8_t *>(buf);

            while (count > 0)
            {
                size_t can_write    = nBufSize - nBufPos;

                if ((nBufPos > 0) || (count < can_write))
                {
                    if (can_write > count)
                        can_write       = count;

                    // Copy data to buffer
                    ::memcpy(&pBuffer[nBufPos], src, can_write);
                    nBufPos        += can_write;
                    count          -= can_write;
                    src            += can_write;

                    // Check buffer size
                    if (nBufPos >= nBufSize)
                    {
                        hdr.magic       = nMagic;
                        hdr.size        = uint32_t(nBufSize);
                        hdr.flags       = 0;
                        hdr.uid         = nUID;

                        // Convert CPU -> BE
                        hdr.magic       = CPU_TO_BE(hdr.magic);
                        hdr.size        = CPU_TO_BE(hdr.size);
                        hdr.flags       = CPU_TO_BE(hdr.flags);
                        hdr.uid         = CPU_TO_BE(hdr.uid);

                        // Write buffer header and data to file
                        status_t res    = pFile->write(&hdr, sizeof(chunk_header_t));
                        if (res == STATUS_OK)
                            res             = pFile->write(pBuffer, nBufSize);
                        if (set_error(res) != STATUS_OK)
                            return res;

                        // Update position and counter
                        nBufPos         = 0;
                        nPosition      += nBufSize;
                        nChunksOut     ++;
                    }
                }
                else // Write directly avoiding buffer
                {
                    hdr.magic       = nMagic;
                    hdr.size        = uint32_t(can_write);
                    hdr.flags       = 0;
                    hdr.uid         = nUID;

                    // Convert CPU -> BE
                    hdr.magic       = CPU_TO_BE(hdr.magic);
                    hdr.size        = CPU_TO_BE(hdr.size);
                    hdr.flags       = CPU_TO_BE(hdr.flags);
                    hdr.uid         = CPU_TO_BE(hdr.uid);

                    // Write buffer header and data to file
                    status_t res    = pFile->write(&hdr, sizeof(chunk_header_t));
                    if (res == STATUS_OK)
                        pFile->write(src, can_write);
                    if (set_error(res) != STATUS_OK)
                        return res;

                    // Update position and counter
                    count          -= can_write;
                    src            += can_write;
                    nPosition      += can_write;
                    nChunksOut     ++;
                }
            }

            return set_error(STATUS_OK);
        }

        status_t ChunkWriter::write_header(const void *buf)
        {
            if (pFile == NULL)
                return set_error(STATUS_CLOSED);

            const header_t *phdr = static_cast<const header_t *>(buf);
            if (phdr->size < sizeof(header_t))
                return set_error(STATUS_BAD_ARGUMENTS);

            // Write encoded header
            header_t shdr;
            shdr.size           = CPU_TO_BE(phdr->size);
            shdr.version        = CPU_TO_BE(phdr->version);
            status_t res        = write(&shdr, sizeof(shdr));
            if (res != STATUS_OK)
                return res;

            // Write header data
            return write(&phdr[1], phdr->size - sizeof(header_t));
        }

        status_t ChunkWriter::flush()
        {
            return do_flush(0);
        }

        status_t ChunkWriter::close()
        {
            status_t result = do_flush(F_FORCE | F_LAST);
            status_t result2 = ChunkAccessor::close();
            return set_error((result == STATUS_OK) ? result2 : result);
        }

    } /* namespace lspc */
} /* namespace lsp */
