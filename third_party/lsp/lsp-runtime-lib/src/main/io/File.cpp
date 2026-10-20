/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 6 мар. 2019 г.
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

#include <lsp-plug.in/io/File.h>
#include <lsp-plug.in/io/NativeFile.h>
#include <lsp-plug.in/common/debug.h>
#include <lsp-plug.in/stdlib/stdio.h>

#ifdef PLATFORM_WINDOWS
    #include <windows.h>
    #include <fileapi.h>
    #include <io.h>
    #include <winbase.h>
#else
    #include <sys/stat.h>
    #include <errno.h>
    #include <unistd.h>
    #include <fcntl.h>
#endif /* PLATFORM_WINDOWS */

namespace lsp
{
    namespace io
    {

    #ifdef PLATFORM_WINDOWS
        template <typename S>
        static inline void decode_file_type(fattr_t *attr, const S *hfi)
        {
            attr->type      = fattr_t::FT_REGULAR;
            if (hfi->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                attr->type      = fattr_t::FT_DIRECTORY;
            else if (hfi->dwFileAttributes & FILE_ATTRIBUTE_DEVICE)
                attr->type      = fattr_t::FT_BLOCK;

            attr->blk_size  = 4096;
            attr->size      = (wsize_t(hfi->nFileSizeHigh) << 32) | hfi->nFileSizeLow;
            attr->inode     = 0;
            attr->ctime     = ((wsize_t(hfi->ftCreationTime.dwHighDateTime) << 32) | hfi->ftCreationTime.dwLowDateTime) / 10000;
            attr->mtime     = ((wsize_t(hfi->ftLastWriteTime.dwHighDateTime) << 32) | hfi->ftLastWriteTime.dwLowDateTime) / 10000;
            attr->atime     = ((wsize_t(hfi->ftLastAccessTime.dwHighDateTime) << 32) | hfi->ftLastAccessTime.dwLowDateTime) / 10000;
        }
    #else
        static inline void decode_file_type(fattr_t *attr, const struct stat *sb)
        {
            // Decode file type
            switch (sb->st_mode & S_IFMT) {
                case S_IFBLK:  attr->type = fattr_t::FT_BLOCK;      break;
                case S_IFCHR:  attr->type = fattr_t::FT_CHARACTER;  break;
                case S_IFDIR:  attr->type = fattr_t::FT_DIRECTORY;  break;
                case S_IFIFO:  attr->type = fattr_t::FT_FIFO;       break;
                case S_IFLNK:  attr->type = fattr_t::FT_SYMLINK;    break;
                case S_IFREG:  attr->type = fattr_t::FT_REGULAR;    break;
                case S_IFSOCK: attr->type = fattr_t::FT_SOCKET;     break;
                default:       attr->type = fattr_t::FT_UNKNOWN;    break;
            }

            attr->blk_size  = sb->st_blksize;
            attr->size      = sb->st_size;
            attr->inode     = sb->st_ino;

            // In C headers st_atime, st_mtime and st_ctime macros are defined for backward comparibility
            // to provide access to field tv_sec of st_atim, st_mtim and st_ctim structures.
            // In MacOS, these fields have another names: st_atimespec, st_ctimespec and st_mtimespec
            #if defined(PLATFORM_MACOSX)
                attr->ctime     = (sb->st_ctimespec.tv_sec * 1000LL) + (sb->st_ctimespec.tv_nsec / 1000000);
                attr->mtime     = (sb->st_mtimespec.tv_sec * 1000LL) + (sb->st_mtimespec.tv_nsec / 1000000);
                attr->atime     = (sb->st_atimespec.tv_sec * 1000LL) + (sb->st_atimespec.tv_nsec / 1000000);
            #elif defined(st_ctime) || defined(st_mtime) || defined(st_atime)
                attr->ctime     = (sb->st_ctim.tv_sec * 1000LL) + (sb->st_ctim.tv_nsec / 1000000);
                attr->mtime     = (sb->st_mtim.tv_sec * 1000LL) + (sb->st_mtim.tv_nsec / 1000000);
                attr->atime     = (sb->st_atim.tv_sec * 1000LL) + (sb->st_atim.tv_nsec / 1000000);
            #else
                attr->ctime     = sb->st_ctime * 1000LL;
                attr->mtime     = sb->st_mtime * 1000LL;
                attr->atime     = sb->st_atime * 1000LL;
            #endif
        }
    #endif /* PLATFORM_POSIX */

        File::File()
        {
            nErrorCode  = STATUS_OK;
        }
        
        File::~File()
        {
        }

        ssize_t File::read(void *dst, size_t count)
        {
            return -set_error(STATUS_NOT_SUPPORTED);
        }

        ssize_t File::pread(wsize_t pos, void *dst, size_t count)
        {
            return -set_error(STATUS_NOT_SUPPORTED);
        }

        ssize_t File::write(const void *src, size_t count)
        {
            return -set_error(STATUS_NOT_SUPPORTED);
        }

        ssize_t File::pwrite(wsize_t pos, const void *src, size_t count)
        {
            return -set_error(STATUS_NOT_SUPPORTED);
        }

        status_t File::seek(wssize_t pos, size_t type)
        {
            return set_error(STATUS_NOT_SUPPORTED);
        }

        wssize_t File::position()
        {
            return -set_error(STATUS_NOT_SUPPORTED);
        }

        wssize_t File::size()
        {
            return -set_error(STATUS_NOT_SUPPORTED);
        }

        status_t File::stat(fattr_t *attr)
        {
            return -set_error(STATUS_NOT_SUPPORTED);
        }

        status_t File::truncate(wsize_t length)
        {
            return set_error(STATUS_NOT_SUPPORTED);
        }

        status_t File::flush()
        {
            return set_error(STATUS_NOT_SUPPORTED);
        }

        status_t File::sync()
        {
            return set_error(STATUS_NOT_SUPPORTED);
        }

        status_t File::close()
        {
            return set_error(STATUS_OK);
        }
    
        status_t File::stat(const char *path, fattr_t *attr)
        {
            if ((path == NULL) || (attr == NULL))
                return STATUS_BAD_ARGUMENTS;

            LSPString spath;
            if (!spath.set_utf8(path))
                return STATUS_NO_MEM;
            return stat(&spath, attr);
        }


        status_t File::stat(const Path *path, fattr_t *attr)
        {
            if ((path == NULL) || (attr == NULL))
                return STATUS_BAD_ARGUMENTS;
            return stat(path->as_string(), attr);
        }

        status_t File::sym_stat(const char *path, fattr_t *attr)
        {
            if ((path == NULL) || (attr == NULL))
                return STATUS_BAD_ARGUMENTS;

            LSPString spath;
            if (!spath.set_utf8(path))
                return STATUS_NO_MEM;
            return sym_stat(&spath, attr);
        }

        status_t File::sym_stat(const Path *path, fattr_t *attr)
        {
            if ((path == NULL) || (attr == NULL))
                return STATUS_BAD_ARGUMENTS;
            return sym_stat(path->as_string(), attr);
        }

        status_t File::stat(FILE *fd, fattr_t *attr)
        {
            if (fd == NULL)
                return STATUS_BAD_ARGUMENTS;

            #ifdef PLATFORM_WINDOWS
                return stat((HANDLE)::_get_osfhandle(::_fileno(fd)), attr);
            #else
                return stat(fileno(fd), attr);
            #endif
        }

        status_t File::stat(const LSPString *path, fattr_t *attr)
        {
            if ((path == NULL) || (attr == NULL))
                return STATUS_BAD_ARGUMENTS;

            #ifdef PLATFORM_WINDOWS
                WIN32_FIND_DATAW hfi;

                HANDLE dh   = ::FindFirstFileW(path->get_utf16(), &hfi);
                if (dh == INVALID_HANDLE_VALUE)
                {
                    DWORD err = ::GetLastError();
                    switch (err)
                    {
                        case ERROR_PATH_NOT_FOUND:
                        case ERROR_FILE_NOT_FOUND:
                        case ERROR_INVALID_ACCESS:
                        case ERROR_INVALID_DRIVE:
                        case ERROR_CANNOT_MAKE:
                            return STATUS_NOT_FOUND;
                        case ERROR_ACCESS_DENIED:
                            return STATUS_PERMISSION_DENIED;
                        case ERROR_TOO_MANY_OPEN_FILES:
                        case ERROR_NOT_ENOUGH_MEMORY:
                        case ERROR_OUTOFMEMORY:
                            return STATUS_NO_MEM;
                        case ERROR_FILE_EXISTS:
                        case ERROR_ALREADY_EXISTS:
                        case ERROR_DIRECTORY:
                            return STATUS_ALREADY_EXISTS;
                        case ERROR_BUFFER_OVERFLOW:
                            return STATUS_OVERFLOW;
                        case ERROR_INVALID_NAME:
                            return STATUS_INVALID_VALUE;
                        default: break;
                    }

                    return STATUS_IO_ERROR;
                }
                ::FindClose(dh);

                decode_file_type(attr, &hfi);
            #else
                struct stat sb;
                const char * const s = path->get_native();
                if (::lstat(s, &sb) != 0)
                {
                    const int code = errno;
                    lsp_trace("lstat() falied for %s: errno=%d", s, code);

                    switch (code)
                    {
                        case EACCES: return STATUS_PERMISSION_DENIED;
                        case EBADF: return STATUS_INVALID_VALUE;
                        case ENAMETOOLONG: return STATUS_OVERFLOW;
                        case EOVERFLOW: return STATUS_OVERFLOW;
                        case ENOENT: return STATUS_NOT_FOUND;
                        case ENOMEM: return STATUS_NO_MEM;
                        default: break;
                    }
                    return STATUS_IO_ERROR;
                }

                decode_file_type(attr, &sb);
            #endif /* PLATFORM_WINDOWS */

            return STATUS_OK;
        }

        status_t File::stat(fhandle_t fd, fattr_t *attr)
        {
            if (attr == NULL)
                return STATUS_BAD_ARGUMENTS;

        #ifdef PLATFORM_WINDOWS
            BY_HANDLE_FILE_INFORMATION hfi;

            if (!::GetFileInformationByHandle(fd, &hfi))
            {
                DWORD err = ::GetLastError();

                switch (err)
                {
                    case ERROR_PATH_NOT_FOUND:
                    case ERROR_FILE_NOT_FOUND:
                    case ERROR_INVALID_ACCESS:
                    case ERROR_INVALID_DRIVE:
                    case ERROR_CANNOT_MAKE:
                        return STATUS_NOT_FOUND;
                    case ERROR_ACCESS_DENIED:
                        return STATUS_PERMISSION_DENIED;
                    case ERROR_TOO_MANY_OPEN_FILES:
                    case ERROR_NOT_ENOUGH_MEMORY:
                    case ERROR_OUTOFMEMORY:
                        return STATUS_NO_MEM;
                    case ERROR_FILE_EXISTS:
                    case ERROR_ALREADY_EXISTS:
                    case ERROR_DIRECTORY:
                        return STATUS_ALREADY_EXISTS;
                    case ERROR_BUFFER_OVERFLOW:
                        return STATUS_OVERFLOW;
                    case ERROR_INVALID_NAME:
                        return STATUS_INVALID_VALUE;
                    default: break;
                }
                return STATUS_IO_ERROR;
            }

            decode_file_type(attr, &hfi);
        #else
            struct stat sb;
            if (::fstat(fd, &sb) != 0)
            {
                int code = errno;
                switch (code)
                {
                    case EACCES: return STATUS_PERMISSION_DENIED;
                    case EBADF: return STATUS_INVALID_VALUE;
                    case ENAMETOOLONG: return STATUS_OVERFLOW;
                    case EOVERFLOW: return STATUS_OVERFLOW;
                    case ENOENT: return STATUS_NOT_FOUND;
                    case ENOMEM: return STATUS_NO_MEM;
                    default: break;
                }
                return STATUS_IO_ERROR;
            }

            decode_file_type(attr, &sb);
        #endif  /* PLATFORM_WINDOWS */

            return STATUS_OK;
        }

        status_t File::sym_stat(const LSPString *path, fattr_t *attr)
        {
            if ((path == NULL) || (attr == NULL))
                return STATUS_BAD_ARGUMENTS;

            #ifdef PLATFORM_WINDOWS
                WIN32_FIND_DATAW hfi;

                HANDLE dh   = ::FindFirstFileW(path->get_utf16(), &hfi);
                if (dh == INVALID_HANDLE_VALUE)
                {
                    DWORD err = ::GetLastError();
                    switch (err)
                    {
                        case ERROR_PATH_NOT_FOUND:
                        case ERROR_FILE_NOT_FOUND:
                        case ERROR_INVALID_ACCESS:
                        case ERROR_INVALID_DRIVE:
                        case ERROR_CANNOT_MAKE:
                            return STATUS_NOT_FOUND;
                        case ERROR_ACCESS_DENIED:
                            return STATUS_PERMISSION_DENIED;
                        case ERROR_TOO_MANY_OPEN_FILES:
                        case ERROR_NOT_ENOUGH_MEMORY:
                        case ERROR_OUTOFMEMORY:
                            return STATUS_NO_MEM;
                        case ERROR_FILE_EXISTS:
                        case ERROR_ALREADY_EXISTS:
                        case ERROR_DIRECTORY:
                            return STATUS_ALREADY_EXISTS;
                        case ERROR_BUFFER_OVERFLOW:
                            return STATUS_OVERFLOW;
                        case ERROR_INVALID_NAME:
                            return STATUS_INVALID_VALUE;
                        default: break;
                    }

                    return STATUS_IO_ERROR;
                }
                ::FindClose(dh);

                decode_file_type(attr, &hfi);
            #else
                struct stat sb;
                const char * const s = path->get_native();
                if (::stat(s, &sb) != 0)
                {
                    const int code = errno;
                    lsp_trace("stat falied for %s: errno=%d", s, code);

                    switch (code)
                    {
                        case EACCES: return STATUS_PERMISSION_DENIED;
                        case EBADF: return STATUS_INVALID_VALUE;
                        case ENAMETOOLONG: return STATUS_OVERFLOW;
                        case EOVERFLOW: return STATUS_OVERFLOW;
                        case ENOENT: return STATUS_NOT_FOUND;
                        case ENOMEM: return STATUS_NO_MEM;
                        default: break;
                    }
                    return STATUS_IO_ERROR;
                }

                decode_file_type(attr, &sb);
            #endif /* PLATFORM_WINDOWS */

            return STATUS_OK;
        }

        status_t File::remove(const char *path)
        {
            if (path == NULL)
                return STATUS_BAD_ARGUMENTS;

            LSPString spath;
            if (!spath.set_utf8(path))
                return STATUS_NO_MEM;
            return remove(&spath);
        }

        status_t File::remove(const Path *path)
        {
            if (path == NULL)
                return STATUS_BAD_ARGUMENTS;
            return remove(path->as_string());
        }

        status_t File::remove(const LSPString *path)
        {
            if (path == NULL)
                return STATUS_BAD_ARGUMENTS;

#ifdef PLATFORM_WINDOWS
            if (::DeleteFileW(path->get_utf16()))
                return STATUS_OK;

            // Analyze error code
            DWORD code = ::GetLastError();
            switch (code)
            {
                case ERROR_ACCESS_DENIED:
                {
                    fattr_t attr;
                    status_t res = stat(path, &attr);
                    if ((res == STATUS_OK) && (attr.type == fattr_t::FT_DIRECTORY))
                        return STATUS_IS_DIRECTORY;
                    return STATUS_PERMISSION_DENIED;
                }
                case ERROR_PATH_NOT_FOUND:
                    return STATUS_NOT_FOUND;
                default:
                    return STATUS_IO_ERROR;
            }
#else
            // Try to remove file
            if (::unlink(path->get_native()) == 0)
                return STATUS_OK;

            // Analyze error code
            int code = errno;
            lsp_trace("code=%d", int(code));
            switch (code)
            {
                case EACCES:
                    return STATUS_PERMISSION_DENIED;
                case EPERM:
                {
                    fattr_t attr;
                    status_t res = stat(path, &attr);
                    if ((res == STATUS_OK) && (attr.type == fattr_t::FT_DIRECTORY))
                        return STATUS_IS_DIRECTORY;
                    return STATUS_PERMISSION_DENIED;
                }
                case EDQUOT:
                case ENOSPC:
                    return STATUS_OVERFLOW;
                case EISDIR:
                    return STATUS_IS_DIRECTORY;
                case EFAULT:
                case EINVAL:
                case ENAMETOOLONG:
                    return STATUS_BAD_ARGUMENTS;
                case ENOTDIR:
                    return STATUS_BAD_TYPE;
                case ENOENT:
                    return STATUS_NOT_FOUND;
                case ENOTEMPTY:
                    return STATUS_NOT_EMPTY;
                default:
                    return STATUS_IO_ERROR;
            }
#endif /* PLATFORM_WINDOWS */
            return STATUS_OK;
        }

        status_t File::rename(const char *from, const char *to)
        {
            if ((from == NULL) || (to == NULL))
                return STATUS_BAD_ARGUMENTS;

            LSPString f, t;
            if (!f.set_utf8(from))
                return STATUS_NO_MEM;
            if (!t.set_utf8(to))
                return STATUS_NO_MEM;
            return rename(&f, &t);
        }

        status_t File::rename(const LSPString *from, const char *to)
        {
            if ((from == NULL) || (to == NULL))
                return STATUS_BAD_ARGUMENTS;

            LSPString t;
            if (!t.set_utf8(to))
                return STATUS_NO_MEM;
            return rename(from, &t);
        }

        status_t File::rename(const Path *from, const char *to)
        {
            if ((from == NULL) || (to == NULL))
                return STATUS_BAD_ARGUMENTS;

            LSPString t;
            if (!t.set_utf8(to))
                return STATUS_NO_MEM;
            return rename(from->as_string(), &t);
        }

        status_t File::rename(const char *from, const LSPString *to)
        {
            if ((from == NULL) || (to == NULL))
                return STATUS_BAD_ARGUMENTS;

            LSPString f;
            if (!f.set_native(from))
                return STATUS_NO_MEM;
            return rename(&f, to);
        }

        status_t File::rename(const Path *from, const LSPString *to)
        {
            if ((from == NULL) || (to == NULL))
                return STATUS_BAD_ARGUMENTS;
            return rename(from->as_string(), to);
        }

        status_t File::rename(const char *from, const Path *to)
        {
            if ((from == NULL) || (to == NULL))
                return STATUS_BAD_ARGUMENTS;

            LSPString f;
            if (!f.set_utf8(from))
                return STATUS_NO_MEM;
            return rename(&f, to->as_string());
        }

        status_t File::rename(const LSPString *from, const Path *to)
        {
            if ((from == NULL) || (to == NULL))
                return STATUS_BAD_ARGUMENTS;

            return rename(from, to->as_string());
        }

        status_t File::rename(const Path *from, const Path *to)
        {
            if ((from == NULL) || (to == NULL))
                return STATUS_BAD_ARGUMENTS;

            return rename(from->as_string(), to->as_string());
        }

        status_t File::rename(const LSPString *from, const LSPString *to)
        {
            if ((from == NULL) || (to == NULL))
                return STATUS_BAD_ARGUMENTS;

#ifdef PLATFORM_WINDOWS
            if (::MoveFileExW(from->get_utf16(), to->get_utf16(), MOVEFILE_REPLACE_EXISTING))
                return STATUS_OK;

            // Analyze error code
            DWORD code = ::GetLastError();
            switch (code)
            {
                case ERROR_ACCESS_DENIED:
                    return STATUS_PERMISSION_DENIED;
                case ERROR_PATH_NOT_FOUND:
                    return STATUS_NOT_FOUND;
                default:
                    break;;
            }
#else
            // Try to remove file
            if (::rename(from->get_native(), to->get_native()) == 0)
                return STATUS_OK;

            switch (errno)
            {
                case EPERM:
                case EACCES:
                case EROFS:
                case EXDEV:
                    return STATUS_PERMISSION_DENIED;
                case EBUSY:
                    return STATUS_LOCKED;
                case EFAULT:
                    return STATUS_BAD_PATH;
                case EINVAL:
                    return STATUS_INVALID_VALUE;
                case EISDIR:
                    return STATUS_IS_DIRECTORY;
                case EDQUOT:
                case ELOOP:
                case EMLINK:
                    return STATUS_OVERFLOW;
                case ENAMETOOLONG:
                    return STATUS_BAD_ARGUMENTS;
                case ENOENT:
                    return STATUS_NOT_FOUND;
                case ENOMEM:
                case ENOSPC:
                    return STATUS_NO_MEM;
                case ENOTDIR:
                    return STATUS_NOT_DIRECTORY;
                case ENOTEMPTY:
                case EEXIST:
                    return STATUS_ALREADY_EXISTS;
                default:
                    break;
            }
#endif /* PLATFORM_WINDOWS */

            return STATUS_IO_ERROR;
        }

        wssize_t File::copy(const char *from, const char *to, size_t io_buf_size)
        {
            if ((from == NULL) || (to == NULL))
                return -STATUS_BAD_ARGUMENTS;

            LSPString f, t;
            if (!f.set_utf8(from))
                return -STATUS_NO_MEM;
            if (!t.set_utf8(to))
                return -STATUS_NO_MEM;
            return copy(&f, &t, io_buf_size);
        }

        wssize_t File::copy(const LSPString *from, const char *to, size_t io_buf_size)
        {
            if ((from == NULL) || (to == NULL))
                return -STATUS_BAD_ARGUMENTS;

            LSPString t;
            if (!t.set_utf8(to))
                return -STATUS_NO_MEM;
            return copy(from, &t, io_buf_size);
        }

        wssize_t File::copy(const Path *from, const char *to, size_t io_buf_size)
        {
            if ((from == NULL) || (to == NULL))
                return -STATUS_BAD_ARGUMENTS;

            LSPString t;
            if (!t.set_utf8(to))
                return -STATUS_NO_MEM;
            return copy(from->as_string(), &t, io_buf_size);
        }

        wssize_t File::copy(const char *from, const LSPString *to, size_t io_buf_size)
        {
            if ((from == NULL) || (to == NULL))
                return -STATUS_BAD_ARGUMENTS;

            LSPString f;
            if (!f.set_native(from))
                return -STATUS_NO_MEM;
            return copy(&f, to, io_buf_size);
        }

        wssize_t File::copy(const Path *from, const LSPString *to, size_t io_buf_size)
        {
            if ((from == NULL) || (to == NULL))
                return -STATUS_BAD_ARGUMENTS;
            return copy(from->as_string(), to, io_buf_size);
        }

        wssize_t File::copy(const char *from, const Path *to, size_t io_buf_size)
        {
            if ((from == NULL) || (to == NULL))
                return -STATUS_BAD_ARGUMENTS;

            LSPString f;
            if (!f.set_utf8(from))
                return -STATUS_NO_MEM;
            return copy(&f, to->as_string(), io_buf_size);
        }

        wssize_t File::copy(const LSPString *from, const Path *to, size_t io_buf_size)
        {
            if ((from == NULL) || (to == NULL))
                return -STATUS_BAD_ARGUMENTS;

            return copy(from, to->as_string(), io_buf_size);
        }

        wssize_t File::copy(const Path *from, const Path *to, size_t io_buf_size)
        {
            if ((from == NULL) || (to == NULL))
                return -STATUS_BAD_ARGUMENTS;

            return copy(from->as_string(), to->as_string(), io_buf_size);
        }

        wssize_t File::copy(const LSPString *from, const LSPString *to, size_t io_buf_size)
        {
            io::NativeFile src, dst;
            status_t res = STATUS_OK, xres;
            ssize_t copied = 0;

            // Open source file
            if ((res = src.open(from, File::FM_READ)) == STATUS_OK)
            {
                // Open destination file
                if ((res = dst.open(to, File::FM_READWRITE_NEW)) == STATUS_OK)
                {
                    // Allocate I/O buffer
                    io_buf_size     = lsp_max(io_buf_size, 0x100U);
                    uint8_t *buf    = static_cast<uint8_t *>(malloc(io_buf_size));
                    if (buf != NULL)
                    {
                        // Perform copy
                        do
                        {
                            // Read block
                            ssize_t nread = src.read(buf, io_buf_size);
                            if (nread < 0)
                            {
                                res = (nread == -STATUS_EOF) ? STATUS_OK : status_t(-nread);
                                break;
                            }

                            // Write block to destination file
                            for (ssize_t i=0; i<nread; ++i)
                            {
                                ssize_t nwritten = dst.write(&buf[i], nread - i);
                                if (nwritten < 0)
                                {
                                    res     = status_t(-nwritten);
                                    break;
                                }

                                i += nwritten;
                            }

                            // Update number of copied bytes
                            copied     += nread;
                        } while (res == STATUS_OK);

                        // Free allocated buffer
                        free(buf);
                    }
                    else
                        res = STATUS_NO_MEM;

                    // Close destination file
                    xres = dst.close();
                    if (res == STATUS_OK)
                        res = xres;
                }

                // Close source file
                xres = src.close();
                if (res == STATUS_OK)
                    res = xres;
            }

            return (res == STATUS_OK) ? copied : -res;
        }

        status_t File::mkparent(const char *path)
        {
            io::Path tmp;
            status_t res;
            if ((res = tmp.set(path)) != STATUS_OK)
                return res;
            if ((res = tmp.remove_last()) != STATUS_OK)
                return res;

            return tmp.mkdir();
        }

        status_t File::mkparent(const LSPString *path)
        {
            io::Path tmp;
            status_t res;
            if ((res = tmp.set(path)) != STATUS_OK)
                return res;
            if ((res = tmp.remove_last()) != STATUS_OK)
                return res;

            return tmp.mkdir();
        }

        status_t File::mkparent(const io::Path *path)
        {
            return path->mkparent();
        }

        status_t File::mkparent(const char *path, bool recursive)
        {
            io::Path tmp;
            status_t res;
            if ((res = tmp.set(path)) != STATUS_OK)
                return res;
            if ((res = tmp.remove_last()) != STATUS_OK)
                return res;

            return tmp.mkdir(recursive);
        }

        status_t File::mkparent(const LSPString *path, bool recursive)
        {
            io::Path tmp;
            status_t res;
            if ((res = tmp.set(path)) != STATUS_OK)
                return res;
            if ((res = tmp.remove_last()) != STATUS_OK)
                return res;

            return tmp.mkdir(recursive);
        }

        status_t File::mkparent(const io::Path *path, bool recursive)
        {
            return path->mkparent(recursive);
        }
    } /* namespace io */
} /* namespace lsp */
