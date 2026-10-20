/*
 * Copyright (C) 2026 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2026 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 7 мар. 2019 г.
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

#include <lsp-plug.in/io/NativeFile.h>
#include <lsp-plug.in/runtime/system.h>

#if defined(PLATFORM_WINDOWS)
    #include <windows.h>
    #include <fileapi.h>
#endif /* PLATFORM_WINDOWS */

#if defined(PLATFORM_UNIX_COMPATIBLE)
    #include <unistd.h>
    #include <sys/file.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <errno.h>
#endif /* PLATFORM_UNIX_COMPATIBLE */

#define BAD_FD      fhandle_t(-1)

namespace lsp
{
    namespace io
    {
        
        NativeFile::NativeFile()
        {
            hFD     = BAD_FD;
            nFlags  = 0;
        }
        
        NativeFile::~NativeFile()
        {
            if (hFD != BAD_FD)
            {
                if (nFlags & SF_CLOSE)
                {
                    #if defined(PLATFORM_WINDOWS)
                        CloseHandle(hFD);
                    #else
                        ::close(hFD);
                    #endif
                }
                hFD     = BAD_FD;
            }
            nFlags  = 0;
        }

        status_t NativeFile::open(const char *path, size_t mode)
        {
            if (path == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

            LSPString tmp;
            if (!tmp.set_utf8(path))
                return set_error(STATUS_NO_MEM);
            return open(&tmp, mode);
        }

    #if defined(PLATFORM_WINDOWS)
        status_t NativeFile::open(const LSPString *path, size_t mode)
        {
            if (path == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

            fattr_t stat;
            status_t res = File::stat(path, &stat);
            if (res == STATUS_OK)
            {
                if (stat.type == fattr_t::FT_DIRECTORY)
                    return (mode & FM_CREATE) ? STATUS_ALREADY_EXISTS : STATUS_IS_DIRECTORY;
            }
            else if (!(mode & FM_CREATE))
                return set_error(res);

            int oflags;
            size_t fflags;
            size_t cmode;
            size_t atts = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS;
            if (mode & FM_READ)
            {
                oflags      = (mode & FM_WRITE) ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
                fflags      = (mode & FM_WRITE) ? SF_READ | SF_WRITE : SF_READ;
            }
            else if (mode & FM_WRITE)
            {
                oflags      = GENERIC_WRITE;
                fflags      = SF_WRITE;
            }
            else
                return set_error(STATUS_INVALID_VALUE);

            if (mode & FM_CREATE)
                cmode       = (mode & FM_EXCL) ? CREATE_NEW : CREATE_ALWAYS;
            else if (mode & FM_TRUNC)
                cmode       = TRUNCATE_EXISTING;
            else
                cmode       = OPEN_EXISTING;

            if (mode & FM_DIRECT)
                atts           |= FILE_FLAG_NO_BUFFERING;

            fhandle_t fd = CreateFileW(path->get_utf16(),
                oflags,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                NULL, cmode, atts, NULL);
            if (fd == INVALID_HANDLE_VALUE)
            {
                DWORD error = GetLastError();
                res = STATUS_IO_ERROR;

                switch (error)
                {
                    case ERROR_PATH_NOT_FOUND:
                    case ERROR_FILE_NOT_FOUND:
                    case ERROR_INVALID_ACCESS:
                    case ERROR_INVALID_DRIVE:
                    case ERROR_CANNOT_MAKE:
                        res = STATUS_NOT_FOUND;
                        break;
                    case ERROR_ACCESS_DENIED:
                        res = STATUS_PERMISSION_DENIED;
                        break;
                    case ERROR_TOO_MANY_OPEN_FILES:
                    case ERROR_NOT_ENOUGH_MEMORY:
                    case ERROR_OUTOFMEMORY:
                        res = STATUS_NO_MEM;
                        break;
                    case ERROR_WRITE_PROTECT:
                        res = STATUS_READONLY;
                        break;
                    case ERROR_FILE_EXISTS:
                    case ERROR_ALREADY_EXISTS:
                        res = STATUS_ALREADY_EXISTS;
                        break;
                    case ERROR_DIRECTORY:
                        res = STATUS_IS_DIRECTORY;
                        break;
                    case ERROR_BUFFER_OVERFLOW:
                        res = STATUS_OVERFLOW;
                        break;
                    case ERROR_INVALID_NAME:
                        res = STATUS_INVALID_VALUE;
                        break;
                    default:
                        break;
                }
                return set_error(res);
            }

            // Check that we need to lock the file
            if (mode & FM_LOCK)
            {
                OVERLAPPED ov;
                ov.Internal         = 0;
                ov.InternalHigh     = 0;
                ov.Offset           = 0;
                ov.OffsetHigh       = 0;
                ov.hEvent           = NULL;

                const DWORD flags   = (mode & FM_NOWAIT) ? LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY : LOCKFILE_EXCLUSIVE_LOCK;
                if (!LockFileEx(fd, flags, 0, MAXDWORD, MAXDWORD, &ov))
                {
                    const DWORD code    = GetLastError();
                    status_t res        = STATUS_IO_ERROR;

                    switch (code)
                    {
                        case ERROR_IO_PENDING: res = STATUS_LOCKED; break;
                        default: break;
                    }

                    return set_error(res);
                }
            }

            hFD         = fd;
            nFlags      = fflags | SF_CLOSE;

            return set_error(STATUS_OK);
        }
    #else
        status_t NativeFile::open(const LSPString *path, size_t mode)
        {
            if (path == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

            fattr_t stat;
            if (File::stat(path, &stat) == STATUS_OK)
            {
                if (stat.type == fattr_t::FT_DIRECTORY)
                    return (mode & FM_CREATE) ? STATUS_ALREADY_EXISTS : STATUS_NOT_FOUND;
            }

            int oflags;
            size_t fflags;
            if (mode & FM_READ)
            {
                oflags      = (mode & FM_WRITE) ? O_RDWR : O_RDONLY;
                fflags      = (mode & FM_WRITE) ? SF_READ | SF_WRITE : SF_READ;
            }
            else if (mode & FM_WRITE)
            {
                oflags      = O_WRONLY;
                fflags      = SF_WRITE;
            }
            else
                return set_error(STATUS_INVALID_VALUE);
            if (mode & FM_CREATE)
                oflags     |= O_CREAT;
            if (mode & FM_TRUNC)
                oflags     |= O_TRUNC;
            if (mode & FM_EXCL)
                oflags     |= O_EXCL;

            #if defined(__USE_GNU) && defined(O_DIRECT)
                if (mode & FM_DIRECT)
                    oflags     |= O_DIRECT;
            #endif /* __USE_GNU */

            fhandle_t fd        = ::open(path->get_native(), oflags, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
            if (fd < 0)
            {
                const int code = errno;
                status_t res = STATUS_IO_ERROR;

                switch (code)
                {
                    case EPERM: case EACCES: res = STATUS_PERMISSION_DENIED; break;
                    case EEXIST: res = STATUS_ALREADY_EXISTS; break;
                    case EINVAL: res = STATUS_INVALID_VALUE; break;
                    case EISDIR: res = STATUS_IS_DIRECTORY; break;
                    case ENAMETOOLONG: res = STATUS_OVERFLOW; break;
                    case ENOENT: res = STATUS_NOT_FOUND; break;
                    case ENOMEM: res = STATUS_NO_MEM; break;
                    case ENOTDIR: res = STATUS_NOT_DIRECTORY; break;
                    case EROFS: res = STATUS_READONLY; break;
                    default: break;
                }

                return set_error(res);
            }

            // Check that we need to lock the file
            if (mode & FM_LOCK)
            {
                const int lk_flags  = (mode & FM_NOWAIT) ? LOCK_EX | LOCK_NB : LOCK_EX;
                const int lock_res  = flock(fd, lk_flags);
                if (lock_res != 0)
                {
                    const int code = errno;
                    ::close(fd);

                    status_t res = STATUS_IO_ERROR;
                    switch (code)
                    {
                        case EINTR: res = STATUS_INTERRUPTED; break;
                        case ENOLCK: res = STATUS_NO_MEM; break;
                        case EWOULDBLOCK: res = STATUS_LOCKED; break;
                        default: break;
                    }

                    return set_error(res);
                }
            }


            hFD         = fd;
            nFlags      = fflags | SF_CLOSE;

            return set_error(STATUS_OK);
        }
    #endif /* PLATFORM_WINDOWS */

        status_t NativeFile::open(const Path *path, size_t mode)
        {
            if (path == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);
            return open(path->as_string(), mode);
        }

        status_t NativeFile::wrap(fhandle_t fd, bool close)
        {
            // Check state
            if (hFD != BAD_FD)
                return set_error(STATUS_BAD_STATE);

            hFD     = fd;
            nFlags  = (close) ? SF_READ | SF_WRITE | SF_CLOSE : SF_READ | SF_WRITE;
            return set_error(STATUS_OK);
        }

        status_t NativeFile::wrap(fhandle_t fd, size_t mode, bool close)
        {
            // Check state
            if (hFD != BAD_FD)
                return set_error(STATUS_BAD_STATE);

            size_t flags = (close) ? SF_CLOSE : 0;
            if (mode & FM_READ)
                flags  |= SF_READ;
            if (mode & FM_WRITE)
                flags  |= SF_WRITE;

            hFD     = fd;
            nFlags  = flags;

            return set_error(STATUS_OK);
        }

        ssize_t NativeFile::read(void *dst, size_t count)
        {
            // Check state
            if (hFD == BAD_FD)
                return -set_error(STATUS_BAD_STATE);
            else if (!(nFlags & SF_READ))
                return -set_error(STATUS_PERMISSION_DENIED);

            uint8_t *ptr    = reinterpret_cast<uint8_t *>(dst);
            size_t bread    = 0;
            bool eof        = false;

            // Perofrm read
            #ifdef PLATFORM_WINDOWS
                while (bread < count)
                {
                    size_t to_read      = count - bread;
                    if (to_read >= 0x40000000)
                        to_read             = 0x40000000;
                    DWORD n_read        = 0;

                    if (!ReadFile(hFD, ptr, DWORD(to_read), &n_read, NULL))
                        break;
                    if (n_read <= 0)
                    {
                        eof = true;
                        break;
                    }

                    ptr    += n_read;
                    bread  += n_read;
                }
            #else
                while (bread < count)
                {
                    size_t to_read  = count - bread;
                    size_t n_read   = ::read(hFD, ptr, to_read);

                    if (n_read <= 0)
                    {
                        eof = true;
                        break;
                    }

                    ptr    += n_read;
                    bread  += n_read;
                }

            #endif
            if ((bread > 0) || (count <= 0) || (!eof))
            {
                set_error(STATUS_OK);
                return bread;
            }
            return -set_error(STATUS_EOF);
        }

        ssize_t NativeFile::pread(wsize_t pos, void *dst, size_t count)
        {
            // Check state
            if (hFD == BAD_FD)
                return -set_error(STATUS_BAD_STATE);
            else if (!(nFlags & SF_READ))
                return -set_error(STATUS_PERMISSION_DENIED);

            uint8_t *ptr    = reinterpret_cast<uint8_t *>(dst);
            size_t bread    = 0;
            bool eof        = false;

            #ifdef PLATFORM_WINDOWS
                LARGE_INTEGER off, save;
                off.QuadPart = 0;

                // Obtrain current file pointer
                if (!SetFilePointerEx(hFD, off, &save, FILE_CURRENT))
                    return -set_error(STATUS_IO_ERROR);

                // Change file pointer
                off.QuadPart = pos;
                if (!SetFilePointerEx(hFD, off, NULL, FILE_BEGIN))
                    return -set_error(STATUS_IO_ERROR);

                // Perform positioned read
                while (bread < count)
                {
                    size_t to_read      = count - bread;
                    if (to_read >= 0x40000000)
                        to_read             = 0x40000000;
                    DWORD n_read        = 0;

                    if (!ReadFile(hFD, ptr, DWORD(to_read), &n_read, NULL))
                        break;
                    if (n_read <= 0)
                    {
                        eof = true;
                        break;
                    }

                    ptr    += n_read;
                    bread  += n_read;
                }

                // Restore position
                if (!SetFilePointerEx(hFD, save, NULL, FILE_BEGIN))
                    return -set_error(STATUS_IO_ERROR);
            #else
                // Perform positioned read
                while (bread < count)
                {
                    size_t to_read  = count - bread;
                    size_t n_read   = ::pread(hFD, ptr, to_read, pos);

                    if (n_read <= 0)
                    {
                        eof = true;
                        break;
                    }

                    ptr    += n_read;
                    bread  += n_read;
                    pos    += n_read;
                }
            #endif

            if ((bread > 0) || (count <= 0) || (!eof))
            {
                set_error(STATUS_OK);
                return bread;
            }
            return -set_error(STATUS_EOF);
        }

        ssize_t NativeFile::write(const void *src, size_t count)
        {
            // Check state
            if (hFD == BAD_FD)
                return -set_error(STATUS_BAD_STATE);
            else if (!(nFlags & SF_WRITE))
                return -set_error(STATUS_PERMISSION_DENIED);

            const uint8_t *ptr  = reinterpret_cast<const uint8_t *>(src);
            size_t bwritten     = 0;

            // Perform write
            #ifdef PLATFORM_WINDOWS
                while (bwritten < count)
                {
                    size_t to_write     = count - bwritten;
                    if (to_write >= 0x40000000)
                        to_write            = 0x40000000;
                    DWORD n_written     = 0;

                    if (!WriteFile(hFD, ptr, to_write, &n_written, NULL))
                        break;

                    ptr        += n_written;
                    bwritten   += n_written;
                }
            #else
                while (bwritten < count)
                {
                    size_t to_write     = count - bwritten;
                    ssize_t n_written   = ::write(hFD, ptr, to_write);

                    if (n_written <= 0)
                        break;

                    ptr        += n_written;
                    bwritten   += n_written;
                }
            #endif

            if ((bwritten > 0) || (count <= 0))
            {
                set_error(STATUS_OK);
                return bwritten;
            }

            return -set_error(STATUS_IO_ERROR);
        }

        ssize_t NativeFile::pwrite(wsize_t pos, const void *src, size_t count)
        {
            // Check state
            if (hFD == BAD_FD)
                return -set_error(STATUS_BAD_STATE);
            else if (!(nFlags & SF_WRITE))
                return -set_error(STATUS_PERMISSION_DENIED);

            const uint8_t *ptr  = reinterpret_cast<const uint8_t *>(src);
            size_t bwritten     = 0;

            #ifdef PLATFORM_WINDOWS
                LARGE_INTEGER off, save;
                off.QuadPart = 0;

                // Obtrain current file pointer
                if (!SetFilePointerEx(hFD, off, &save, FILE_CURRENT))
                    return -set_error(STATUS_IO_ERROR);

                // Change file pointer
                off.QuadPart = pos;
                if (!SetFilePointerEx(hFD, off, NULL, FILE_BEGIN))
                    return -set_error(STATUS_IO_ERROR);

                // Perform positioned write
                while (bwritten < count)
                {
                    size_t to_write     = count - bwritten;
                    if (to_write >= 0x40000000)
                        to_write            = 0x40000000;
                    DWORD n_written     = 0;

                    if (!WriteFile(hFD, ptr, to_write, &n_written, NULL))
                        break;

                    ptr        += n_written;
                    bwritten   += n_written;
                }

                // Restore position
                if (!SetFilePointerEx(hFD, save, NULL, FILE_BEGIN))
                    return -set_error(STATUS_IO_ERROR);
            #else
                // Perform positioned write
                while (bwritten < count)
                {
                    size_t to_write     = count - bwritten;
                    size_t n_written    = ::pwrite(hFD, ptr, to_write, pos);

                    if (n_written <= 0)
                        break;

                    ptr        += n_written;
                    bwritten   += n_written;
                    pos        += n_written;
                }
            #endif

            if ((bwritten > 0) || (count <= 0))
            {
                set_error(STATUS_OK);
                return bwritten;
            }

            return -set_error(STATUS_IO_ERROR);
        }

        status_t NativeFile::seek(wssize_t pos, size_t type)
        {
            if (hFD == BAD_FD)
                return set_error(STATUS_BAD_STATE);

            #ifdef PLATFORM_WINDOWS
                DWORD method;
                LARGE_INTEGER off;
                switch (type)
                {
                    case FSK_SET: method  = FILE_BEGIN; break;
                    case FSK_CUR: method  = FILE_CURRENT; break;
                    case FSK_END: method  = FILE_END; break;
                    default: return set_error(STATUS_INVALID_VALUE);
                }
                off.QuadPart = pos;
                if (!SetFilePointerEx(hFD, off, NULL, method))
                    return set_error(STATUS_IO_ERROR);
            #else
                int whence;
                switch (type)
                {
                    case FSK_SET:  whence = SEEK_SET; break;
                    case FSK_CUR:  whence = SEEK_CUR; break;
                    case FSK_END:  whence = SEEK_END; break;
                    default:
                        return set_error(STATUS_BAD_ARGUMENTS);
                }

                if (lseek(hFD, pos, whence) < 0)
                {
                    int code = errno;
                    if (code == ESPIPE)
                        return set_error(STATUS_NOT_SUPPORTED);
                    return set_error(STATUS_IO_ERROR);
                }
            #endif /* PLATFORM_WINDOWS */

            return set_error(STATUS_OK);
        }

        wssize_t NativeFile::position()
        {
            if (hFD == BAD_FD)
                return -set_error(STATUS_BAD_STATE);

            #ifdef PLATFORM_WINDOWS
                LARGE_INTEGER off, pos;
                off.QuadPart = 0;
                if (!SetFilePointerEx(hFD, off, &pos, FILE_CURRENT))
                    return -set_error(STATUS_IO_ERROR);
                wssize_t result = pos.QuadPart;
            #else
                wssize_t result = lseek(hFD, 0, SEEK_CUR);
                if (result < 0)
                    return -set_error(STATUS_IO_ERROR);
            #endif /* PLATFORM_WINDOWS */

            return result;
        }

        wssize_t NativeFile::size()
        {
            if (hFD == BAD_FD)
                return -set_error(STATUS_BAD_STATE);

            #ifdef PLATFORM_WINDOWS
                LARGE_INTEGER sizebuf;
                if (!GetFileSizeEx(hFD, &sizebuf))
                    return -set_error(STATUS_IO_ERROR);
                wssize_t pos    = sizebuf.QuadPart;
            #else
                struct stat statbuf;
                if (fstat(hFD, &statbuf) != 0)
                    return -set_error(STATUS_IO_ERROR);
                wssize_t pos    = statbuf.st_size;
            #endif  /* PLATFORM_WINDOWS */

            set_error(STATUS_OK);
            return pos;
        }

        status_t NativeFile::stat(fattr_t *attr)
        {
            if (hFD == BAD_FD)
                return -set_error(STATUS_BAD_STATE);

            return set_error(File::stat(hFD, attr));
        }

        status_t NativeFile::truncate(wsize_t length)
        {
            // Check state
            if (hFD == BAD_FD)
                return set_error(STATUS_BAD_STATE);
            else if (!(nFlags & SF_WRITE))
                return set_error(STATUS_PERMISSION_DENIED);

            #ifdef PLATFORM_WINDOWS
                LARGE_INTEGER off, pos;

                off.QuadPart = 0;
                if (!SetFilePointerEx(hFD, off, &pos, FILE_CURRENT))
                    return -set_error(STATUS_IO_ERROR);

                off.QuadPart = length;
                if (!SetFilePointerEx(hFD, off, NULL, FILE_BEGIN))
                    return -set_error(STATUS_IO_ERROR);

                status_t res = (SetEndOfFile(hFD)) ? STATUS_OK : STATUS_IO_ERROR;

                if (!SetFilePointerEx(hFD, pos, NULL, FILE_BEGIN))
                    res = STATUS_IO_ERROR;

                return set_error(res);
            #else
                if (ftruncate(hFD, length) != 0)
                    return set_error(STATUS_IO_ERROR);
                return set_error(STATUS_OK);
            #endif  /* PLATFORM_WINDOWS */
        }

        status_t NativeFile::flush()
        {
            // Check state
            if (hFD == BAD_FD)
                return set_error(STATUS_BAD_STATE);
            else if (!(nFlags & SF_WRITE))
                return set_error(STATUS_PERMISSION_DENIED);

            #ifdef PLATFORM_WINDOWS
                if (!FlushFileBuffers(hFD))
                    return set_error(STATUS_IO_ERROR);
            #else
// Not required
//                if (fdatasync(hFD) != 0)
//                    return set_error(STATUS_IO_ERROR);
            #endif  /* PLATFORM_WINDOWS */

            return set_error(STATUS_OK);
        }

        status_t NativeFile::sync()
        {
            // Check state
            if (hFD == BAD_FD)
                return set_error(STATUS_BAD_STATE);
            else if (!(nFlags & SF_WRITE))
                return set_error(STATUS_PERMISSION_DENIED);

            #ifdef PLATFORM_WINDOWS
                if (!FlushFileBuffers(hFD))
                    return set_error(STATUS_IO_ERROR);
            #else
                if (fsync(hFD) != 0)
                    return set_error(STATUS_IO_ERROR);
            #endif  /* PLATFORM_WINDOWS */

            return set_error(STATUS_OK);
        }

        status_t NativeFile::close()
        {
            if (hFD != BAD_FD)
            {
                if (nFlags & SF_CLOSE)
                {
                #ifdef PLATFORM_WINDOWS
                    if (!CloseHandle(hFD))
                        return set_error(STATUS_IO_ERROR);
                #else
                    if (::close(hFD) != 0)
                        return set_error(STATUS_IO_ERROR);
                #endif  /* PLATFORM_WINDOWS */
                }

                hFD     = BAD_FD;
                nFlags  = 0;
            }

            return set_error(STATUS_OK);
        }

        status_t NativeFile::open_temp(io::Path *path, const char *prefix)
        {
            if (prefix == NULL)
                return open_temp(path, static_cast<LSPString *>(NULL));

            LSPString p;
            if (!p.set_utf8(prefix))
                return STATUS_NO_MEM;
            return open_temp(path, &p);
        }

        status_t NativeFile::open_temp(LSPString *path, const char *prefix)
        {
            if (prefix == NULL)
                return open_temp(path, static_cast<LSPString *>(NULL));

            LSPString p;
            if (!p.set_utf8(prefix))
                return STATUS_NO_MEM;
            return open_temp(path, &p);
        }

        status_t NativeFile::open_temp(LSPString *path, const LSPString *prefix)
        {
            if (path == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

            // Open file
            io::Path out;
            status_t res = open_temp(&out, prefix);
            if (res != STATUS_OK)
                return res;
            if ((res = out.get(path)) != STATUS_OK)
            {
                // Close the file and remove it on error
                close();
                out.remove();
            }

            return res;
        }

        status_t NativeFile::open_temp(io::Path *path, const LSPString *prefix)
        {
            if (hFD != BAD_FD)
                return set_error(STATUS_BAD_STATE);
            else if (path == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

            // Make the prefix
            status_t res;
            LSPString name;
            if (prefix != NULL)
            {
                if (!name.set(prefix))
                    return STATUS_NO_MEM;
                if (!name.append('-'))
                    return STATUS_NO_MEM;
            }

            // Obtain the temporary directory
            io::Path tempdir;
            if ((res = system::get_temporary_dir(&tempdir)) != STATUS_OK)
                return res;

            // Create the file in the loop
            io::Path full_path;
            size_t len = name.length();

            while (true)
            {
                // Generate the name of the file
                name.set_length(len);
                int seed = int(system::get_time_millis()) ^ int(rand());
                if (!name.fmt_append_ascii("%08x.tmp", seed))
                    return STATUS_NO_MEM;
                if ((res = full_path.set(&tempdir, &name)) != STATUS_OK)
                    return res;

                // Open temporary file
                res = open(&full_path, io::File::FM_WRITE_NEW | io::File::FM_EXCL);
                if (res == STATUS_OK)
                {
                    full_path.swap(path);
                    return res;
                }
                if (res != STATUS_ALREADY_EXISTS)
                    return res;
            }
        }

    } /* namespace io */
} /* namespace lsp */
