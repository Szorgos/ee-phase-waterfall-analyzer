/*
 * Copyright (C) 2020 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2020 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 24 апр. 2019 г.
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

#include <lsp-plug.in/common/debug.h>
#include <lsp-plug.in/ipc/Library.h>

#ifdef PLATFORM_WINDOWS
    #include <windows.h>
#endif /* PLATFORM_WINDOWS */

namespace lsp
{
    namespace ipc
    {
        int Library::hTag    = 0x12345678;
        
        Library::Library()
        {
            hDlSym      = NULL;
            nLastError  = STATUS_OK;
        }
        
        Library::~Library()
        {
            close();
        }
    
        status_t Library::open(const char *path, bool new_namespace)
        {
            if (path == NULL)
                return nLastError = STATUS_BAD_ARGUMENTS;
            LSPString tmp;
            if (!tmp.set_utf8(path))
                return nLastError = STATUS_NO_MEM;
            return open(&tmp);
        }

        status_t Library::open(const LSPString *path, bool new_namespace)
        {
            if (path == NULL)
                return nLastError = STATUS_BAD_ARGUMENTS;
            if (hDlSym != NULL)
                return nLastError = STATUS_OPENED;

            #ifdef PLATFORM_WINDOWS
                const WCHAR *str  = path->get_utf16();
                if (str == NULL)
                    return STATUS_NO_MEM;
                HMODULE handle  = ::LoadLibraryW(str);

                if (handle == NULL)
                    return nLastError = STATUS_NOT_FOUND;
            #else
                const char *str  = path->get_utf8();
                if (str == NULL)
                    return STATUS_NO_MEM;

            #ifdef _GNU_SOURCE
                void * const handle    = (new_namespace) ?
                    ::dlmopen(LM_ID_NEWLM, str, RTLD_NOW) :
                    ::dlopen(str, RTLD_NOW);
            #else
                void * const handle    = ::dlopen(str, RTLD_NOW);
            #endif /* _GNU_SOURCE */
                if (handle == NULL)
                {
                    lsp_warn("Error loading module %s: %s", path->get_native(), ::dlerror());
                    return nLastError = STATUS_NOT_FOUND;
                }
            #endif

            hDlSym  = handle;
            return nLastError = STATUS_OK;
        }

        status_t Library::open(const io::Path *path, bool new_namespace)
        {
            if (path == NULL)
                return nLastError = STATUS_BAD_ARGUMENTS;
            return open(path->as_string());
        }

        void *Library::import(const char *name)
        {
            if (name == NULL)
            {
                nLastError = STATUS_BAD_ARGUMENTS;
                return NULL;
            }
            if (hDlSym == NULL)
            {
                nLastError = STATUS_BAD_STATE;
                return NULL;
            }

            #ifdef PLATFORM_WINDOWS
                void *ptr   = reinterpret_cast<void *>(::GetProcAddress(hDlSym, name));
            #else
                void *ptr   = ::dlsym(hDlSym, name);
            #endif

            if (ptr == NULL)
                nLastError  = STATUS_NOT_FOUND;
            else
                nLastError  = STATUS_OK;

            return ptr;
        }

        void *Library::import(const LSPString *name)
        {
            if (name == NULL)
            {
                nLastError = STATUS_BAD_ARGUMENTS;
                return NULL;
            }
            if (hDlSym == NULL)
            {
                nLastError = STATUS_BAD_STATE;
                return NULL;
            }
            return import(name->get_utf8());
        }

        status_t Library::close()
        {
            if (hDlSym == NULL)
                return nLastError = STATUS_OK;
        #ifdef PLATFORM_WINDOWS
            ::FreeLibrary(hDlSym);
        #else
            ::dlclose(hDlSym);
        #endif

            hDlSym      = NULL;
            return nLastError = STATUS_OK;
        }

        void Library::swap(Library *dst)
        {
            lsp::swap(dst->hDlSym, hDlSym);
            lsp::swap(dst->nLastError, nLastError);
        }

        status_t Library::get_module_file(LSPString *path, const void *ptr)
        {
            if (path == NULL)
                return STATUS_BAD_ARGUMENTS;

#ifdef PLATFORM_WINDOWS
            // Get module handle
            HMODULE hm = NULL;

            if (!::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(ptr),
                &hm))
                return STATUS_NOT_FOUND;

            // Get the path to the module
            DWORD capacity = PATH_MAX + 1, length = 0;
            WCHAR *xpath = NULL;
            lsp_finally {
                if (xpath != NULL)
                    ::free(xpath);
            };

            while (true)
            {
                // Allocate the buffer
                WCHAR *new_xpath = reinterpret_cast<WCHAR *>(::realloc(xpath, capacity * sizeof(WCHAR)));
                if (new_xpath == NULL)
                    return STATUS_NO_MEM;
                xpath   = new_xpath;

                // Try to obtain the file name
                length = ::GetModuleFileNameW(hm, xpath, capacity);
                if (length == 0)
                    return STATUS_NOT_FOUND;

                // Analyze result
                DWORD error = GetLastError();
                if (error == ERROR_SUCCESS)
                    break;
                else if (error != ERROR_INSUFFICIENT_BUFFER)
                    return STATUS_UNKNOWN_ERR;

                // Increase capacity by 1.5
                capacity += (capacity >> 1);
            }

            if (!path->set_utf16(xpath, length))
                return STATUS_NO_MEM;
#else
            Dl_info dli;
            int res     = ::dladdr(const_cast<void *>(ptr), &dli);
            if ((res == 0) || (dli.dli_fname == NULL))
                return STATUS_NOT_FOUND;

            if (!path->set_native(dli.dli_fname))
                return STATUS_NO_MEM;
#endif /* PLATFORM_WINDOWS */

            return STATUS_OK;
        }

        status_t Library::get_module_file(io::Path *path, const void *ptr)
        {
            if (path == NULL)
                return STATUS_BAD_ARGUMENTS;

            LSPString tmp;
            status_t res = get_module_file(&tmp, ptr);
            if (res != STATUS_OK)
                return res;

            return path->set(&tmp);
        }

        bool Library::valid_library_name(const char *path)
        {
            if (path == NULL)
                return false;
            io::Path tmp;
            if (tmp.set(path) != STATUS_OK)
                return false;
            return valid_library_name(&tmp);
        }

        bool Library::valid_library_name(const LSPString *path)
        {
            if (path == NULL)
                return false;

            io::Path tmp;
            if (tmp.set(path) != STATUS_OK)
                return false;

            return valid_library_name(&tmp);
        }

        bool Library::valid_library_name(const io::Path *path)
        {
            if (path == NULL)
                return false;
            LSPString tmp;
            if (path->get_last(&tmp) != STATUS_OK)
                return false;

            #if !FILE_SYSTEM_CASE_SENSE
                tmp.tolower();
            #endif

            LSPString ext;
            if (!ext.set_utf8(FILE_LIBRARY_EXT_S))
                return false;

            return tmp.ends_with(&ext);
        }

    } /* namespace io */
} /* namespace lsp */
