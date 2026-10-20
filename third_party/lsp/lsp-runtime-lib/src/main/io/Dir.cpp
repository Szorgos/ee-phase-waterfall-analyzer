/*
 * Copyright (C) 2024 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2024 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 18 мар. 2019 г.
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

#include <lsp-plug.in/io/Dir.h>

#ifdef PLATFORM_WINDOWS
    #include <windows.h>
    #include <fileapi.h>
    #include <winbase.h>
#else
    #include <sys/stat.h>
    #include <errno.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <limits.h>
#endif /* PLATFORM_WINDOWS */

namespace lsp
{
    namespace io
    {
    #ifdef PLATFORM_WINDOWS
        typedef struct dirhandle_t
        {
            HANDLE              hHandle;        // Directory handle
            WIN32_FIND_DATAW    sData;         // Last data read
            status_t            nPending;       // Pending error code
        } dirhandle_t;
    #endif /* PLATFORM_WINDOWS */
        
        static inline bool check_closed(dirhandle_t *dir)
        {
            return dir == NULL;
        }

        Dir::Dir()
        {
            nErrorCode  = STATUS_OK;
            hDir        = NULL;
        }
        
        Dir::~Dir()
        {
            close();
        }

        status_t Dir::open(const char *path)
        {
            if (!check_closed(hDir))
                return set_error(STATUS_BAD_STATE);
            else if (path == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

            LSPString spath;
            if (!spath.set_utf8(path))
                return set_error(STATUS_NO_MEM);
            return open (&spath);
        }

        status_t Dir::open(const LSPString *path)
        {
            if (!check_closed(hDir))
                return set_error(STATUS_BAD_STATE);
            else if (path == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

            if (sPath.set(path) != STATUS_OK)
                return set_error(STATUS_NO_MEM);

        #ifdef PLATFORM_WINDOWS
            // Create dir handle
            dirhandle_t *dir = reinterpret_cast<dirhandle_t *>(malloc(sizeof(dirhandle_t)));
            if (dir == NULL)
                return STATUS_NO_MEM;
            lsp_finally {
                if (dir != NULL)
                    free(dir);
            };
            bzero(dir, sizeof(dirhandle_t));

            // Create search mask
            Path mask;
            status_t res = mask.set(path);
            if (res == STATUS_OK)
                res = mask.append_child("*");
            if (res != STATUS_OK)
            {
                sPath.clear();
                return set_error(res);
            }

            // Call API for FindFirst
            status_t pending    = STATUS_OK;
            HANDLE dh           = ::FindFirstFileW(mask.as_string()->get_utf16(), &dir->sData);
            if (dh == INVALID_HANDLE_VALUE)
            {
                DWORD err = ::GetLastError();
                switch (err)
                {
                    case ERROR_NO_MORE_FILES:
                    case ERROR_FILE_NOT_FOUND:
                        pending     = STATUS_EOF;
                        break;
                    case ERROR_PATH_NOT_FOUND:
                    case ERROR_INVALID_DRIVE:
                        return set_error(STATUS_NOT_DIRECTORY);
                    case ERROR_NOT_READY:           return set_error(STATUS_NO_DATA);
                    case ERROR_TOO_MANY_OPEN_FILES: return set_error(STATUS_OVERFLOW);
                    case ERROR_OUTOFMEMORY:
                    case ERROR_NOT_ENOUGH_MEMORY:
                        return set_error(STATUS_NO_MEM);
                    case ERROR_INVALID_ACCESS:
                    case ERROR_ACCESS_DENIED:
                        return set_error(STATUS_PERMISSION_DENIED);
                    default:
                        return set_error(STATUS_UNKNOWN_ERR);
                }
            }

            // Commit state
            dir->nPending   = pending;
            dir->hHandle    = dh;
            lsp::swap(hDir, dir);
        #else
            DIR *dh = ::opendir(path->get_native());
            if (dh == NULL)
            {
                sPath.clear();
                int error = errno;
                switch (error)
                {
                    case EACCES: return set_error(STATUS_PERMISSION_DENIED);
                    case EMFILE:
                    case ENFILE: return set_error(STATUS_TOO_BIG);
                    case ENOENT: return set_error(STATUS_NOT_FOUND);
                    case ENOMEM: return set_error(STATUS_NO_MEM);
                    case ENOTDIR: return set_error(STATUS_BAD_TYPE);
                    default:
                        return set_error(STATUS_UNKNOWN_ERR);
                }
            }

            hDir        = dh;
        #endif /* PLATFORM_WINDOWS */

            return set_error(STATUS_OK);
        }

        status_t Dir::open(const Path *path)
        {
            if (!check_closed(hDir))
                return set_error(STATUS_BAD_STATE);
            else if (path == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);
            return open(path->as_string());
        }

        status_t Dir::rewind()
        {
            if (check_closed(hDir))
                return set_error(STATUS_BAD_STATE);

        #ifdef PLATFORM_WINDOWS
            // Create search mask
            Path mask;
            status_t res = mask.set(&sPath);
            if (res == STATUS_OK)
                res = mask.append_child("*");
            if (res != STATUS_OK)
            {
                sPath.clear();
                return set_error(res);
            }

            // Call API for FindFirst
            status_t pending    = STATUS_OK;
            HANDLE dh   = ::FindFirstFileW(mask.as_string()->get_utf16(), &hDir->sData);
            if (dh == INVALID_HANDLE_VALUE)
            {
                DWORD err = ::GetLastError();
                switch (err)
                {
                    case ERROR_NO_MORE_FILES:
                    case ERROR_FILE_NOT_FOUND:
                        pending     = STATUS_EOF;
                        break;
                    case ERROR_PATH_NOT_FOUND:
                    case ERROR_INVALID_DRIVE:
                        return set_error(STATUS_NOT_DIRECTORY);
                    case ERROR_NOT_READY:           return set_error(STATUS_NO_DATA);
                    case ERROR_TOO_MANY_OPEN_FILES: return set_error(STATUS_OVERFLOW);
                    case ERROR_OUTOFMEMORY:
                    case ERROR_NOT_ENOUGH_MEMORY:
                        return set_error(STATUS_NO_MEM);
                    case ERROR_INVALID_ACCESS:
                    case ERROR_ACCESS_DENIED:
                        return set_error(STATUS_PERMISSION_DENIED);
                    default:
                        return set_error(STATUS_UNKNOWN_ERR);
                }
            }

            // Close current  handle
            if ((hDir->hHandle != INVALID_HANDLE_VALUE))
                ::FindClose(hDir->hHandle);

            // Replace closed handle by new handle
            hDir->hHandle   = dh;
            hDir->nPending  = pending;
        #else
            ::rewinddir(hDir);
        #endif /* PLATFORM_WINDOWS */

            return set_error(STATUS_OK);
        }

        status_t Dir::read(LSPString *path, bool full)
        {
            if (check_closed(hDir))
                return set_error(STATUS_BAD_STATE);
            else if (path == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

            LSPString out;

        #ifdef PLATFORM_WINDOWS
            if (hDir->nPending != STATUS_OK)
                return set_error(hDir->nPending);
            else if (hDir->hHandle == INVALID_HANDLE_VALUE)
                return set_error(STATUS_BAD_STATE);

            // Set result
            if (!out.set_utf16(hDir->sData.cFileName))
                return set_error(STATUS_NO_MEM);

            // Perform next iteration
            if (!::FindNextFileW(hDir->hHandle, &hDir->sData))
            {
                DWORD err = ::GetLastError();
                switch (err)
                {
                    case ERROR_NO_MORE_FILES:
                    case ERROR_FILE_NOT_FOUND:
                        hDir->nPending    = STATUS_EOF;
                        break;
                    case ERROR_OUTOFMEMORY:
                    case ERROR_NOT_ENOUGH_MEMORY:
                        return set_error(STATUS_NO_MEM);
                    default:
                        hDir->nPending    = STATUS_UNKNOWN_ERR;
                        break;
                }
            }
        #else
            // Read directory
            errno = 0;
            struct dirent *dent = ::readdir(hDir);
            if (dent == NULL)
            {
                if (errno == 0)
                    return set_error(STATUS_EOF);
                return set_error(STATUS_UNKNOWN_ERR);
            }

            // Return value
            if (!out.set_native(dent->d_name))
                return set_error(STATUS_NO_MEM);

        #endif /* PLATFORM_WINDOWS */
            if (full)
            {
                Path tmp;
                status_t res = tmp.set(&sPath);
                if (res == STATUS_OK)
                    res = tmp.append_child(&out);
                if (res == STATUS_OK)
                    res = (out.set(tmp.as_string())) ? STATUS_OK : STATUS_NO_MEM;
                if (res != STATUS_OK)
                    set_error(res);
            }

            path->swap(&out);
            return set_error(STATUS_OK);
        }

        status_t Dir::read(Path *path, bool full)
        {
            if (path == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

            LSPString xpath;
            status_t res = read(&xpath, false);
            if (res == STATUS_OK)
            {
                if (full)
                {
                    Path tmp;
                    res = tmp.set(&sPath);
                    if (res == STATUS_OK)
                        res = tmp.append_child(&xpath);
                    if (res == STATUS_OK)
                        path->take(&tmp);
                }
                else
                    res = path->set(&xpath);
            }
            return set_error(res);
        }

        status_t Dir::reads(LSPString *path, fattr_t *attr, bool full)
        {
            if (check_closed(hDir))
                return set_error(STATUS_BAD_STATE);
            else if (path == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

            LSPString out;

        #ifdef PLATFORM_WINDOWS
            if (hDir->nPending != STATUS_OK)
                return set_error(hDir->nPending);
            else if (hDir->hHandle == INVALID_HANDLE_VALUE)
                return set_error(STATUS_BAD_STATE);

            // Set result
            if (!out.set_utf16(hDir->sData.cFileName))
                return set_error(STATUS_NO_MEM);

            // Decode file state
            // Decode file type
            attr->type      = fattr_t::FT_REGULAR;
            if (hDir->sData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                attr->type      = fattr_t::FT_DIRECTORY;
            else if (hDir->sData.dwFileAttributes & FILE_ATTRIBUTE_DEVICE)
                attr->type      = fattr_t::FT_BLOCK;

            attr->blk_size  = 4096;
            attr->size      = (wsize_t(hDir->sData.nFileSizeHigh) << 32) | hDir->sData.nFileSizeLow;
            attr->inode     = 0;
            attr->ctime     = ((wsize_t(hDir->sData.ftCreationTime.dwHighDateTime) << 32) | hDir->sData.ftCreationTime.dwLowDateTime) / 10000;
            attr->mtime     = ((wsize_t(hDir->sData.ftLastWriteTime.dwHighDateTime) << 32) | hDir->sData.ftLastWriteTime.dwLowDateTime) / 10000;
            attr->atime     = ((wsize_t(hDir->sData.ftLastAccessTime.dwHighDateTime) << 32) | hDir->sData.ftLastAccessTime.dwLowDateTime) / 10000;

            // Perform next iteration
            if (!::FindNextFileW(hDir->hHandle, &hDir->sData))
            {
                DWORD err = ::GetLastError();
                switch (err)
                {
                    case ERROR_NO_MORE_FILES:
                    case ERROR_FILE_NOT_FOUND:
                        hDir->nPending  = STATUS_EOF;
                        break;
                    default:
                        hDir->nPending  = STATUS_UNKNOWN_ERR;
                        break;
                }
            }
        #else
            // Read directory
            errno = 0;
            struct dirent *dent = ::readdir(hDir);
            if (dent == NULL)
            {
                if (errno == 0)
                    return set_error(STATUS_EOF);
                return set_error(STATUS_UNKNOWN_ERR);
            }

            // Stat the record
            struct stat sb;
        #if ((_POSIX_C_SOURCE >= 200809L) || defined(_ATFILE_SOURCE))
            int code = ::fstatat(::dirfd(hDir), dent->d_name, &sb, AT_SYMLINK_NOFOLLOW);
        #else
            LSPString xpath, xname;
            if (!xname.set_native(dent->d_name))
                return set_error(STATUS_NO_MEM);
            if (!xpath.set(sPath.as_string()))
                return set_error(STATUS_NO_MEM);
            if (!xpath.append(FILE_SEPARATOR_C))
                return set_error(STATUS_NO_MEM);
            if (!xpath.append(&xname))
                return set_error(STATUS_NO_MEM);
            int code = ::lstat(xpath.get_native(), &sb);
        #endif
            if (code != 0)
            {
                code = errno;
                switch (code)
                {
                    case EACCES: return set_error(STATUS_PERMISSION_DENIED);
                    case EBADF: return set_error(STATUS_INVALID_VALUE);
                    case ENAMETOOLONG: return set_error(STATUS_OVERFLOW);
                    case EOVERFLOW: return set_error(STATUS_OVERFLOW);
                    case ENOENT: return set_error(STATUS_NOT_FOUND);
                    case ENOMEM: return set_error(STATUS_NO_MEM);
                    default: break;
                }
                return set_error(STATUS_IO_ERROR);
            }

            // Return value
            if (!out.set_native(dent->d_name))
                return set_error(STATUS_NO_MEM);

            // Decode file type
            switch (sb.st_mode & S_IFMT) {
                case S_IFBLK:  attr->type = fattr_t::FT_BLOCK;      break;
                case S_IFCHR:  attr->type = fattr_t::FT_CHARACTER;  break;
                case S_IFDIR:  attr->type = fattr_t::FT_DIRECTORY;  break;
                case S_IFIFO:  attr->type = fattr_t::FT_FIFO;       break;
                case S_IFLNK:  attr->type = fattr_t::FT_SYMLINK;    break;
                case S_IFREG:  attr->type = fattr_t::FT_REGULAR;    break;
                case S_IFSOCK: attr->type = fattr_t::FT_SOCKET;     break;
                default:       attr->type = fattr_t::FT_UNKNOWN;    break;
            }

            attr->blk_size  = sb.st_blksize;
            attr->size      = sb.st_size;
            attr->inode     = sb.st_ino;

            // In C headers st_atime, st_mtime and st_ctime macros are defined for backward comparibility
            // to provide access to field tv_sec of st_atim, st_mtim and st_ctim structures.
            // In MacOS, these fields have another names: st_atimespec, st_ctimespec and st_mtimespec
            #if defined(PLATFORM_MACOSX)
                attr->ctime     = (sb.st_ctimespec.tv_sec * 1000LL) + (sb.st_ctimespec.tv_nsec / 1000000);
                attr->mtime     = (sb.st_mtimespec.tv_sec * 1000LL) + (sb.st_mtimespec.tv_nsec / 1000000);
                attr->atime     = (sb.st_atimespec.tv_sec * 1000LL) + (sb.st_atimespec.tv_nsec / 1000000);
            #elif defined(st_ctime) || defined(st_mtime) || defined(st_atime)
                attr->ctime     = (sb.st_ctim.tv_sec * 1000LL) + (sb.st_ctim.tv_nsec / 1000000);
                attr->mtime     = (sb.st_mtim.tv_sec * 1000LL) + (sb.st_mtim.tv_nsec / 1000000);
                attr->atime     = (sb.st_atim.tv_sec * 1000LL) + (sb.st_atim.tv_nsec / 1000000);
            #else
                attr->ctime     = sb.st_ctime * 1000LL;
                attr->mtime     = sb.st_mtime * 1000LL;
                attr->atime     = sb.st_atime * 1000LL;
            #endif
        #endif /* PLATFORM_WINDOWS */

            if (full)
            {
                Path tmp;
                status_t res = tmp.set(&sPath);
                if (res == STATUS_OK)
                    res = tmp.append_child(&out);
                if (res == STATUS_OK)
                    res = (out.set(tmp.as_string())) ? STATUS_OK : STATUS_NO_MEM;
                if (res != STATUS_OK)
                    set_error(res);
            }

            path->swap(&out);

            return set_error(STATUS_OK);
        }

        status_t Dir::reads(Path *path, fattr_t *attr, bool full)
        {
            if (path == NULL)
                return set_error(STATUS_BAD_ARGUMENTS);

            LSPString xpath;
            fattr_t xattr;

            status_t res = reads(&xpath, &xattr, false);
            if (res == STATUS_OK)
            {
                if (full)
                {
                    Path tmp;
                    res = tmp.set(&sPath);
                    if (res == STATUS_OK)
                        res = tmp.append_child(&xpath);
                    if (res == STATUS_OK)
                        path->take(&tmp);
                }
                else
                    res = path->set(&xpath);

                if (res == STATUS_OK)
                    *attr = xattr;
            }
            return set_error(res);
        }

        status_t Dir::close()
        {
            if (check_closed(hDir))
                return set_error(STATUS_BAD_STATE);

            status_t res    = STATUS_OK;
        #ifdef PLATFORM_WINDOWS
            if (hDir != NULL)
            {
                if ((hDir->hHandle != NULL) &&
                    (hDir->hHandle != INVALID_HANDLE_VALUE))
                {
                    if (!::FindClose(hDir->hHandle))
                        res = STATUS_UNKNOWN_ERR;
                    hDir->hHandle   = NULL;
                }
                free(hDir);
                hDir    = NULL;
            }
        #else
            if (::closedir(hDir) != 0)
            {
                int error = errno;
                res = (error == EBADF) ? STATUS_BAD_STATE : STATUS_IO_ERROR;
            }
            hDir    = NULL;
        #endif /* PLATFORM_WINDOWS */

            return set_error(res);
        }

        status_t Dir::stat(const char *path, fattr_t *attr)
        {
            if ((path == NULL) || (attr == NULL))
                return set_error(STATUS_BAD_ARGUMENTS);
            if (check_closed(hDir))
                return set_error(STATUS_BAD_STATE);

            Path xpath;
            status_t res = xpath.set(&sPath);
            if (res == STATUS_OK)
                res = xpath.append_child(path);
            if (res != STATUS_OK)
                return set_error(res);
            return set_error(File::stat(xpath.as_string(), attr));
        }

        status_t Dir::stat(const LSPString *path, fattr_t *attr)
        {
            if ((path == NULL) || (attr == NULL))
                return set_error(STATUS_BAD_ARGUMENTS);
            if (check_closed(hDir))
                return set_error(STATUS_BAD_STATE);

            Path xpath;
            status_t res = xpath.set(&sPath);
            if (res == STATUS_OK)
                res = xpath.append_child(path);
            if (res != STATUS_OK)
                return set_error(res);
            return set_error(File::stat(&xpath, attr));
        }

        status_t Dir::stat(const Path *path, fattr_t *attr)
        {
            if ((path == NULL) || (attr == NULL))
                return set_error(STATUS_BAD_ARGUMENTS);
            if (check_closed(hDir))
                return set_error(STATUS_BAD_STATE);

            Path xpath;
            status_t res = xpath.set(&sPath);
            if (res == STATUS_OK)
                res = xpath.append_child(path);
            if (res != STATUS_OK)
                return set_error(res);
            return set_error(File::stat(xpath.as_string(), attr));
        }

        status_t Dir::sym_stat(const char *path, fattr_t *attr)
        {
            if ((path == NULL) || (attr == NULL))
                return set_error(STATUS_BAD_ARGUMENTS);
            if (check_closed(hDir))
                return set_error(STATUS_BAD_STATE);

            Path xpath;
            status_t res = xpath.set(&sPath);
            if (res == STATUS_OK)
                res = xpath.append_child(path);
            if (res != STATUS_OK)
                return set_error(res);
            return set_error(File::sym_stat(xpath.as_string(), attr));
        }

        status_t Dir::sym_stat(const LSPString *path, fattr_t *attr)
        {
            if ((path == NULL) || (attr == NULL))
                return set_error(STATUS_BAD_ARGUMENTS);
            if (check_closed(hDir))
                return set_error(STATUS_BAD_STATE);

            Path xpath;
            status_t res = xpath.set(&sPath);
            if (res == STATUS_OK)
                res = xpath.append_child(path);
            if (res != STATUS_OK)
                return set_error(res);
            return set_error(File::sym_stat(xpath.as_string(), attr));
        }

        status_t Dir::sym_stat(const Path *path, fattr_t *attr)
        {
            if ((path == NULL) || (attr == NULL))
                return set_error(STATUS_BAD_ARGUMENTS);
            if (check_closed(hDir))
                return set_error(STATUS_BAD_STATE);

            Path xpath;
            status_t res = xpath.set(&sPath);
            if (res == STATUS_OK)
                res = xpath.append_child(path);
            if (res != STATUS_OK)
                return set_error(res);
            return set_error(File::sym_stat(xpath.as_string(), attr));
        }

        status_t Dir::create(const char *path)
        {
            if (path == NULL)
                return STATUS_BAD_ARGUMENTS;

            LSPString spath;
            if (!spath.set_utf8(path))
                return STATUS_NO_MEM;
            return create(&spath);
        }

        status_t Dir::create(const Path *path)
        {
            if (path == NULL)
                return STATUS_BAD_ARGUMENTS;
            return create(path->as_string());
        }

        status_t Dir::create(const LSPString *path)
        {
            if (path == NULL)
                return STATUS_BAD_ARGUMENTS;

        #ifdef PLATFORM_WINDOWS
            const WCHAR *xp = path->get_utf16();
            if (::CreateDirectoryW(xp, NULL))
                return STATUS_OK;

            // Analyze error code
            DWORD code = ::GetLastError();
            switch (code)
            {
                case ERROR_ALREADY_EXISTS:
                    return STATUS_OK;
                case ERROR_PATH_NOT_FOUND:
                    return STATUS_NOT_FOUND;
                default:
                    return STATUS_IO_ERROR;
            }
        #else
            // Try to create directory
            if (::mkdir(path->get_native(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) == 0)
                return STATUS_OK;

            // Analyze error code
            int code = errno;
            switch (code)
            {
                case EACCES:
                case EPERM:
                    return STATUS_PERMISSION_DENIED;
                case EDQUOT:
                case ENOSPC:
                    return STATUS_OVERFLOW;
                case ENOTDIR:
                    return STATUS_BAD_HIERARCHY;
                case EFAULT:
                case EINVAL:
                case ENAMETOOLONG:
                    return STATUS_BAD_ARGUMENTS;
                case EEXIST: // pathname already exists (not necessarily as a directory).  This includes the case where pathname is a symbolic link, dangling or not.
                {
                    fattr_t attr;
                    File::sym_stat(path, &attr);
                    if (attr.type == fattr_t::FT_DIRECTORY)
                        return STATUS_OK;
                    return STATUS_ALREADY_EXISTS;
                }
                case ENOENT: // A directory component in pathname does not exist or is a dangling symbolic link.
                    return STATUS_NOT_FOUND;
                default:
                    return STATUS_IO_ERROR;
            }
        #endif /* PLATFORM_WINDOWS */
            return STATUS_OK;
        }

        status_t Dir::remove(const char *path)
        {
            if (path == NULL)
                return STATUS_BAD_ARGUMENTS;

            LSPString spath;
            if (!spath.set_utf8(path))
                return STATUS_NO_MEM;
            return remove(&spath);
        }

        status_t Dir::remove(const Path *path)
        {
            if (path == NULL)
                return STATUS_BAD_ARGUMENTS;
            return remove(path->as_string());
        }

        status_t Dir::remove(const LSPString *path)
        {
            if (path == NULL)
                return STATUS_BAD_ARGUMENTS;

        #ifdef PLATFORM_WINDOWS
            if (::RemoveDirectoryW(path->get_utf16()))
                return STATUS_OK;

            // Analyze error code
            DWORD code = ::GetLastError();
            switch (code)
            {
                case ERROR_PATH_NOT_FOUND:
                    return STATUS_NOT_FOUND;
                case ERROR_DIR_NOT_EMPTY:
                    return STATUS_NOT_EMPTY;
                case ERROR_SHARING_VIOLATION:
                    return STATUS_PERMISSION_DENIED;
                default:
                    return STATUS_IO_ERROR;
            }
        #else
            // Try to remove directory
            if (::rmdir(path->get_native()) == 0)
                return STATUS_OK;

            // Analyze error code
            int code = errno;
            switch (code)
            {
                case EACCES:
                case EPERM:
                    return STATUS_PERMISSION_DENIED;
                case EDQUOT:
                case ENOSPC:
                    return STATUS_OVERFLOW;
                case EFAULT:
                case EINVAL:
                case ENAMETOOLONG:
                    return STATUS_BAD_ARGUMENTS;
                case ENOTDIR:
                    return STATUS_NOT_DIRECTORY;
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

        status_t Dir::get_current(LSPString *path)
        {
            if (path == NULL)
                return STATUS_BAD_ARGUMENTS;

        #ifdef PLATFORM_WINDOWS
            DWORD len = ::GetCurrentDirectoryW(0, NULL);
            if (len == 0)
                return STATUS_UNKNOWN_ERR;

            WCHAR *buf = reinterpret_cast<WCHAR *>(::malloc(sizeof(WCHAR) * (len + 1)));
            if (buf == NULL)
                return STATUS_NO_MEM;

            len = ::GetCurrentDirectoryW(len, buf);
            if (len == 0)
            {
                ::free(buf);
                return STATUS_UNKNOWN_ERR;
            }

            status_t res = (path->set_utf16(buf, len)) ? STATUS_OK : STATUS_NO_MEM;
            ::free(buf);
            return res;
        #else
            char *spath = reinterpret_cast<char *>(malloc(PATH_MAX * sizeof(char)));
            if (spath == NULL)
                return STATUS_NO_MEM;
            lsp_finally { free(spath); };

            char *p = ::getcwd(spath, PATH_MAX);
            if (p == NULL)
            {
                int code = errno;
                switch (code)
                {
                    case EACCES:
                    case EPERM:
                        return STATUS_PERMISSION_DENIED;
                    case ENOENT:
                        return STATUS_REMOVED;
                    case ENAMETOOLONG:
                        return STATUS_OVERFLOW;
                    case ENOMEM:
                        return STATUS_NO_MEM;
                    default:
                        return STATUS_IO_ERROR;
                }
            }

            return (path->set_native(p)) ? STATUS_OK : STATUS_NO_MEM;
        #endif /* PLATFORM_WINDOWS */
        }

        status_t Dir::get_current(Path *path)
        {
            if (path == NULL)
                return STATUS_BAD_ARGUMENTS;
            LSPString tmp;
            status_t res = get_current(&tmp);
            if (res == STATUS_OK)
                path->take(&tmp);
            return res;
        }
    
    } /* namespace io */
} /* namespace lsp */
