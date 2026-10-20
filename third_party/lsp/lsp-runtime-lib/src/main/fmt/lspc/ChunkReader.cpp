/*
 * Copyright (C) 2020 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2020 Vladimir Sadovnikov <sadko4u@gmail.com>
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
#include <lsp-plug.in/fmt/lspc/lspc.h>
#include <lsp-plug.in/fmt/lspc/ChunkReader.h>

namespace lsp
{
    namespace lspc
    {
        ChunkReader::ChunkReader(Resource *fd, uint32_t magic, uint32_t uid):
            ChunkAccessor(fd, magic),
            sStream(this)
        {
            nUnread     = 0;
            nBufTail    = 0;
            nFileOff    = 0;
            nPosition   = 0;
            nUID        = uid;
            bLast       = false;
        }

        ChunkReader::~ChunkReader()
        {
        }
    
        ssize_t ChunkReader::read(void *buf, size_t count)
        {
            if (pFile == NULL)
                return -set_error(STATUS_CLOSED);

            chunk_header_t hdr;

            uint8_t *dst        = static_cast<uint8_t *>(buf);
            ssize_t total       = 0;

            while (count > 0)
            {
                size_t to_read = nBufTail - nBufPos;

                if (to_read > 0) // There is data in the buffer?
                {
                    if (to_read > count)
                        to_read     = count;

                    // Copy memory from buffer
                    memcpy(dst, &pBuffer[nBufPos], to_read);

                    // Update pointer
                    dst        += to_read;
                    nBufPos    += to_read;
                    nPosition  += to_read;
                    count      -= to_read;
                    total      += to_read;
                }
                else if (nUnread > 0)
                {
                    if (nUnread <= count)
                    {
                        // Read data
                        ssize_t n   = pFile->read(nFileOff, dst, nUnread);
                        if (n <= 0)
                            return total;

                        // Update pointer
                        dst        += n;
                        count      -= n;
                        total      += n;
                        nUnread    -= n;
                        nFileOff   += n;
                        nPosition  += n;
                    }
                    else // Fill buffer
                    {
                        to_read     = (nUnread < nBufSize) ? nUnread : nBufSize;

                        // Read data
                        ssize_t n   = pFile->read(nFileOff, pBuffer, to_read);
                        if (n <= 0)
                            return total;

                        // Update pointer
                        nBufPos     = 0;
                        nBufTail    = n;
                        nFileOff   += n;
                        nUnread    -= n;
                    }
                }
                else // Seek for the next valid chunk
                {
                    // There is no chunk after current
                    if (bLast)
                    {
                        set_error(STATUS_EOF);
                        return total;
                    }

                    // Read chunk header
                    ssize_t n       = pFile->read(nFileOff, &hdr, sizeof(chunk_header_t));
                    if (n < ssize_t(sizeof(chunk_header_t)))
                    {
                        set_error(STATUS_EOF);
                        return total;
                    }
                    nFileOff       += sizeof(chunk_header_t);

                    hdr.magic       = BE_TO_CPU(hdr.magic);
                    hdr.flags       = BE_TO_CPU(hdr.flags);
                    hdr.size        = BE_TO_CPU(hdr.size);
                    hdr.uid         = BE_TO_CPU(hdr.uid);

                    // Validate chunk header
                    if ((hdr.magic == nMagic) && (hdr.uid == nUID)) // We've found our chunk, remember unread bytes count
                    {
                        bLast           = hdr.flags & LSPC_CHUNK_FLAG_LAST;
                        nUnread         = hdr.size;
                    }
                    else // Skip this chunk
                        nFileOff       += hdr.size;
                }
            }

            return total;
        }

        ssize_t ChunkReader::read_header(void *hdr, size_t size)
        {
            if (size < sizeof(header_t))
                return -set_error(STATUS_BAD_ARGUMENTS);

            // Read header data first
            header_t shdr;
            ssize_t count   = read(&shdr, sizeof(header_t));
            if (count < 0)
                return count;
            else if (count < ssize_t(sizeof(header_t)))
                return -set_error(STATUS_EOF); // Unexpected end of file

            // Now read header
            chunk_raw_header_t *dhdr    = reinterpret_cast<chunk_raw_header_t *>(hdr);
            size_t hdr_size             = BE_TO_CPU(shdr.size);
            if (hdr_size < sizeof(header_t)) // header size should be at least of sizeof(lspc_header_t)
                return -set_error(STATUS_CORRUPTED_FILE);
            dhdr->common.size           = uint32_t(hdr_size);
            dhdr->common.version        = BE_TO_CPU(shdr.version);
            hdr_size                   -= sizeof(header_t);
            size                       -= sizeof(header_t);

            // Read header contents
            ssize_t to_read             = lsp_min(size, hdr_size);
            count                       = read(&dhdr->data, to_read);
            if (count < 0)
                return count;
            else if (count < to_read)
                return -set_error(STATUS_EOF); // Unexpected end of file

            // Analyze size of header
            if (size < hdr_size) // Requested size less than actual header size?
            {
                // We need to skip extra bytes that do not fit into header
                to_read     = hdr_size - size;
                count       = skip(to_read);
                if (count < 0)
                    return count;
                else if (count < to_read)
                    return -set_error(STATUS_EOF); // Unexpected end of file

                // Patch the header size to be at most of size bytes
                dhdr->common.size           = uint32_t(size + sizeof(header_t));
            }
            else if (size > hdr_size)
                bzero(&dhdr->data[count], size - hdr_size);

            return dhdr->common.size;
        }

        ssize_t ChunkReader::skip(size_t count)
        {
            if (pFile == NULL)
                return -set_error(STATUS_CLOSED);

            chunk_header_t hdr;

            ssize_t total       = 0;

            while (count > 0)
            {
                size_t to_read = nBufTail - nBufPos;

                if (to_read > 0) // There is data in the buffer?
                {
                    if (to_read > count)
                        to_read     = count;

                    // Update pointer
                    nBufPos    += to_read;
                    count      -= to_read;
                    total      += to_read;
                }
                else if (nUnread > 0)
                {
                    if (nUnread <= count)
                    {
                        // Update counters
                        count      -= nUnread;
                        total      += nUnread;
                        nFileOff   += nUnread;
                        nUnread     = 0;
                    }
                    else // Fill buffer
                    {
                        nUnread    -= count;
                        nFileOff   += count;
                        total      += count;
                        count       = 0;
                    }
                }
                else // Seek for the next valid chunk
                {
                    // There is no chunk after current
                    if (bLast)
                    {
                        set_error(STATUS_EOF);
                        return total;
                    }

                    // Read chunk header
                    ssize_t n   = pFile->read(nFileOff, &hdr, sizeof(chunk_header_t));
                    if (n < ssize_t(sizeof(chunk_header_t)))
                    {
                        set_error(STATUS_EOF);
                        return 0;
                    }
                    nFileOff   += sizeof(chunk_header_t);

                    hdr.magic       = BE_TO_CPU(hdr.magic);
                    hdr.flags       = BE_TO_CPU(hdr.flags);
                    hdr.size        = BE_TO_CPU(hdr.size);
                    hdr.uid         = BE_TO_CPU(hdr.uid);

                    // Validate chunk header
                    if ((hdr.magic == nMagic) && (hdr.uid == nUID)) // We've found our chunk, remember unread bytes count
                    {
                        bLast           = hdr.flags & LSPC_CHUNK_FLAG_LAST;
                        nUnread         = hdr.size;
                    }
                    else // Skip this chunk
                        nFileOff       += hdr.size;
                }
            }

            return total;
        }
    }

} /* namespace lsp */
