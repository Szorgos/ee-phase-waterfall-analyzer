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

#include <lsp-plug.in/fmt/lspc/ChunkAccessor.h>
#include <lsp-plug.in/common/debug.h>

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#if defined(PLATFORM_WINDOWS)
    #include <windows.h>

    #define FD_INVALID(fd)      ((fd) == INVALID_HANDLE_VALUE)
#else
    #define FD_INVALID(fd)      ((fd) < 0)
#endif /* PLATFORM_WINDOWS */

namespace lsp
{
    namespace lspc
    {
        status_t Resource::acquire()
        {
            if (FD_INVALID(fd))
                return STATUS_CLOSED;
            ++refs;
            return STATUS_OK;
        }

        status_t Resource::release()
        {
            if (FD_INVALID(fd))
                return STATUS_CLOSED;
            if ((--refs) <= 0)
            {
    #if defined(PLATFORM_WINDOWS)
                CloseHandle(fd);
                fd      = INVALID_HANDLE_VALUE;
    #else
                close(fd);
                fd      = -1;
    #endif /* PLATFORM_WINDOWS */
            }

            return STATUS_OK;
        }
    
        status_t Resource::allocate(uint32_t *id)
        {
            uint32_t cid = chunk_id + 1;
            if (cid == 0)
                return STATUS_OVERFLOW;
            *id = chunk_id = cid;
            return STATUS_OK;
        }

        status_t Resource::write(const void *buf, size_t count)
        {
            if (FD_INVALID(fd))
                return STATUS_CLOSED;

            // Write data at the end of file
            const uint8_t *bptr = static_cast<const uint8_t *>(buf);
            while (count > 0)
            {
    #if defined(PLATFORM_WINDOWS)
                DWORD written = 0;
                if (!WriteFile(fd, bptr, count, &written, NULL))
                {
                    DWORD error = GetLastError();
                    if (error != ERROR_IO_PENDING)
                    {
                        lsp_trace("Error write: GetLastError()=%d", int(error));
                        return STATUS_IO_ERROR;
                    }
                    written = 0;
                }
    #else
                errno       = 0;

                ssize_t written  = pwrite(fd, bptr, count, length);
                if (written < ssize_t(count))
                {
                    int error = errno;
                    if (error != 0)
                    {
                        lsp_trace("Error write: errno=%d", error);
                        return STATUS_IO_ERROR;
                    }
                }
    #endif /* PLATFORM_WINDOWS */

                bptr       += written;
                length     += written;
                count      -= written;
            }

            return STATUS_OK;
        }

        ssize_t Resource::read(wsize_t pos, void *buf, size_t count)
        {
            if (FD_INVALID(fd))
                return -STATUS_CLOSED;

            // Write data at the end of file
            uint8_t *bptr   = static_cast<uint8_t *>(buf);
            ssize_t total   = 0;

    #if defined(PLATFORM_WINDOWS)
            LARGE_INTEGER set_pos;
            LARGE_INTEGER seek_pos;

            set_pos.QuadPart    = pos;
            if (!SetFilePointerEx(fd, set_pos, &seek_pos, FILE_BEGIN))
                return -STATUS_IO_ERROR;
            else if (seek_pos.QuadPart != set_pos.QuadPart)
                return -STATUS_IO_ERROR;
    #endif /* PLATFORM_WINDOWS */

            while (count > 0)
            {
    #if defined(PLATFORM_WINDOWS)
                DWORD read = 0;
                if (!ReadFile(fd, bptr, count, &read, NULL))
                {
                    DWORD error = GetLastError();
                    if (error != ERROR_IO_PENDING)
                        return -STATUS_IO_ERROR;
                    read = 0;
                }
    #else
                ssize_t read    = pread(fd, bptr, count, pos);
    #endif /* PLATFORM_WINDOWS */
                if (read < 0)
                {
                    int error = errno;
                    switch (error)
                    {
                        case EISDIR:    return -STATUS_IS_DIRECTORY;
                        case EPERM:     return -STATUS_PERMISSION_DENIED;
                        case EINTR:     continue;
                        case EIO:       return -STATUS_IO_ERROR;
                        case EBADF:     return -STATUS_BAD_STATE;
                        case EINVAL:    return -STATUS_BAD_STATE;
                        case EAGAIN:    break;
                        default:        return -STATUS_IO_ERROR;
                    }

                    return total;
                }
                else if (ssize_t(read) < ssize_t(count))
                    return total;

                bptr       += read;
                count      -= read;
                total      += read;
            }

            return total;
        }

        ChunkAccessor::ChunkAccessor(Resource *fd, uint32_t magic)
        {
            pBuffer         = NULL;
            pFile           = fd;
            set_error((pFile != NULL) ? pFile->acquire() : STATUS_BAD_STATE);
            nMagic          = magic;
            nBufSize        = (pFile != NULL) ? pFile->bufsize : 0;
            nBufPos         = 0;
            nUID            = 0;

            if (nBufSize > 0)
            {
                // Adjust buffer size
                if (nBufSize < MIN_BUF_SIZE)
                    nBufSize        = MIN_BUF_SIZE;

                // Allocate buffer
                pBuffer         = static_cast<uint8_t *>(malloc(nBufSize));
                if (pBuffer == NULL)
                {
                    set_error(STATUS_NO_MEM);
                    return;
                }
            }

            set_error(STATUS_OK);
        }

        ChunkAccessor::~ChunkAccessor()
        {
            do_close();
        }

        status_t ChunkAccessor::do_close()
        {
            if (pBuffer != NULL)
            {
                free(pBuffer);
                pBuffer = NULL;
            }
            if (pFile == NULL)
                return set_error(STATUS_CLOSED);
            set_error(pFile->release());
            if (pFile->refs <= 0)
                delete pFile;
            pFile = NULL;
            return last_error();
        }
    
        status_t ChunkAccessor::close()
        {
            return do_close();
        }
    }

} /* namespace lsp */
