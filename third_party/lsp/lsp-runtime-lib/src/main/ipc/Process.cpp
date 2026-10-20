/*
 * Copyright (C) 2026 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2026 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 24 июл. 2019 г.
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
#include <lsp-plug.in/ipc/Process.h>
#include <lsp-plug.in/io/OutFileStream.h>
#include <lsp-plug.in/io/InFileStream.h>
#include <lsp-plug.in/runtime/system.h>

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#if defined(PLATFORM_WINDOWS)
    #include <windows.h>
    #include <processthreadsapi.h>
    #include <namedpipeapi.h>
    #include <shlwapi.h>
    #include <synchapi.h>
    #include <processenv.h>
#else
    #include <spawn.h>
    #include <sys/stat.h>
    #include <sys/wait.h>
    
    #ifndef _GNU_SOURCE
        extern char **environ;    // Define environment variables
    #endif /* _GNU_SOURCE */
#endif /* PLATFORM_WINDOWS */

#if defined(PLATFORM_POSIX) && defined(_GNU_SOURCE)
    #define PLATFORM_HAS_EXECVPE
#endif


namespace lsp
{
    namespace ipc
    {
        
#if defined(PLATFORM_WINDOWS) || (!defined(PLATFORM_HAS_EXECVPE))
    static status_t find_executable(LSPString & program, const LSPString & command)
    {
        if (command.index_of(FILE_SEPARATOR_C) >= 0)
            return STATUS_NOT_FOUND;

        // Lookup PATH
        LSPString path;
        status_t res = system::get_env_var("PATH", &path);
        if (res != STATUS_OK)
            return res;

        ssize_t start = 0, count = path.length(), found = 0;
        do
        {
            // Lookup for separator
            found               = path.index_of(start, ';');
            const ssize_t next  = (found >= 0) ? found : count;

            if (start < next)
            {
                if (!program.set(&path, start, found))
                    return STATUS_NO_MEM;
                if (!program.ends_with(FILE_SEPARATOR_C))
                {
                    if (!program.append(FILE_SEPARATOR_C))
                        return STATUS_NO_MEM;
                }
                if (!program.append(&command))
                    return STATUS_NO_MEM;

            #ifdef PLATFORM_WINDOWS
                if (PathFileExistsW(program.get_utf16()))
                    return STATUS_OK;

                // Try .EXE and .COM extensions
                if ((!program.ends_with_ascii_nocase(".exe")) &&
                    (!program.ends_with_ascii_nocase(".com")))
                {
                    // Try ".EXE"
                    if (!program.append_ascii(".exe"))
                        return STATUS_NO_MEM;
                    if (PathFileExistsW(program.get_utf16()))
                        return STATUS_OK;

                    // Try ".COM"
                    program.set_length(program.length() - 4);
                    if (!program.append_ascii(".com"))
                        return STATUS_NO_MEM;
                    if (PathFileExistsW(program.get_utf16()))
                        return STATUS_OK;
                }
            #else
                struct stat st;
                if (stat(program.get_native(), &st) == 0)
                {
                    if (S_ISREG(st.st_mode) && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
                        return STATUS_OK;
                }
            #endif /* PLATFORM_WINDOWS */
            }

            start   = found + 1;
        } while (found >= 0);

        return STATUS_NOT_FOUND;
    }
#endif /* PLATFORM_HAS_EXECPE */

        Process::Process()
        {
            nStatus             = PSTATUS_CREATED;
            nExitCode           = 0;

#ifdef PLATFORM_WINDOWS
            nPID                = 0;
            hProcess            = NULL;
            hStdIn              = NULL;
            hStdOut             = NULL;
            hStdErr             = NULL;
#else
            nPID                = 0;
            hStdIn              = -1;
            hStdOut             = -1;
            hStdErr             = -1;
#endif

            pStdIn              = NULL;
            pStdOut             = NULL;
            pStdErr             = NULL;

            if (copy_env() != STATUS_OK)
                nStatus             = PSTATUS_ERROR;
        }
        
        Process::~Process()
        {
            destroy_args(&vArgs);
            destroy_env(&vEnv);
            close_handles();

            if (pStdIn != NULL)
            {
                pStdIn->close();
                delete pStdIn;
                pStdIn  = NULL;
            }

            if (pStdOut != NULL)
            {
                pStdOut->close();
                delete pStdOut;
                pStdOut  = NULL;
            }

            if (pStdErr != NULL)
            {
                pStdErr->close();
                delete pStdErr;
                pStdErr  = NULL;
            }

#ifdef PLATFORM_WINDOWS
            if (hProcess != NULL)
            {
                ::CloseHandle(hProcess);
                hProcess    = NULL;
            }
#endif /* PLATFORM_WINDOWS */
        }
    
        void Process::destroy_args(lltl::parray<LSPString> *args)
        {
            for (size_t i=0, n=args->size(); i<n; ++i)
                delete args->uget(i);
            args->flush();
        }

        void Process::destroy_env(lltl::parray<envvar_t> *env)
        {
            for (size_t i=0, n=env->size(); i<n; ++i)
                delete env->uget(i);
            env->flush();
        }

        status_t Process::set_command(const LSPString *cmd)
        {
            if (nStatus != PSTATUS_CREATED)
                return STATUS_BAD_STATE;

            if (cmd == NULL)
            {
                sCommand.clear();
                return STATUS_OK;
            }

            return (sCommand.set(cmd)) ? STATUS_OK : STATUS_NO_MEM;
        }

        status_t Process::set_command(const char *cmd)
        {
            if (nStatus != PSTATUS_CREATED)
                return STATUS_BAD_STATE;

            if (cmd == NULL)
            {
                sCommand.clear();
                return STATUS_OK;
            }

            return (sCommand.set_utf8(cmd)) ? STATUS_OK : STATUS_NO_MEM;
        }

        size_t Process::args() const
        {
            return vArgs.size();
        }

        status_t Process::add_arg(const LSPString *value)
        {
            if (value == NULL)
                return STATUS_BAD_ARGUMENTS;
            if (nStatus != PSTATUS_CREATED)
                return STATUS_BAD_STATE;

            LSPString *arg = new LSPString();
            if (arg == NULL)
                return STATUS_NO_MEM;

            if (!arg->set(value))
            {
                delete arg;
                return STATUS_NO_MEM;
            }
            if (!vArgs.add(arg))
            {
                delete arg;
                return STATUS_NO_MEM;
            }

            return STATUS_OK;
        }

        status_t Process::add_arg(const char *value)
        {
            if (value == NULL)
                return STATUS_BAD_ARGUMENTS;
            if (nStatus != PSTATUS_CREATED)
                return STATUS_BAD_STATE;

            LSPString *arg = new LSPString();
            if (arg == NULL)
                return STATUS_NO_MEM;

            if (!arg->set_utf8(value))
            {
                delete arg;
                return STATUS_NO_MEM;
            }
            if (!vArgs.add(arg))
            {
                delete arg;
                return STATUS_NO_MEM;
            }

            return STATUS_OK;
        }

        status_t Process::set_arg(size_t index, const LSPString *value)
        {
            if (value == NULL)
                return STATUS_BAD_ARGUMENTS;
            if (nStatus != PSTATUS_CREATED)
                return STATUS_BAD_STATE;

            LSPString *ptr = vArgs.get(index);
            if (ptr == NULL)
                return STATUS_BAD_ARGUMENTS;
            return (ptr->set(value)) ? STATUS_OK : STATUS_NO_MEM;
        }

        status_t Process::set_arg(size_t index, const char *value)
        {
            if (value == NULL)
                return STATUS_BAD_ARGUMENTS;
            if (nStatus != PSTATUS_CREATED)
                return STATUS_BAD_STATE;

            LSPString *ptr = vArgs.get(index);
            if (ptr == NULL)
                return STATUS_BAD_ARGUMENTS;
            return (ptr->set_utf8(value)) ? STATUS_OK : STATUS_NO_MEM;
        }

        status_t Process::get_arg(size_t index, LSPString *value)
        {
            LSPString *ptr = vArgs.get(index);
            if (ptr == NULL)
                return STATUS_BAD_ARGUMENTS;
            if (value == NULL)
                return STATUS_OK;
            return (value->set(ptr)) ? STATUS_OK : STATUS_NO_MEM;
        }

        status_t Process::get_arg(size_t index, char **value)
        {
            LSPString *ptr = vArgs.get(index);
            if (ptr == NULL)
                return STATUS_BAD_ARGUMENTS;
            if (value == NULL)
                return STATUS_OK;

            char *dup = ptr->clone_utf8();
            if (dup == NULL)
                return STATUS_NO_MEM;

            *value      = dup;
            return STATUS_OK;
        }

        status_t Process::remove_arg(size_t index, LSPString *value)
        {
            if (nStatus != PSTATUS_CREATED)
                return STATUS_BAD_STATE;

            LSPString *ptr = vArgs.get(index);
            if (ptr == NULL)
                return STATUS_BAD_ARGUMENTS;
            if (value != NULL)
                value->swap(ptr);

            vArgs.remove(index);
            delete ptr;
            return STATUS_OK;
        }

        status_t Process::remove_arg(size_t index, char **value)
        {
            if (nStatus != PSTATUS_CREATED)
                return STATUS_BAD_STATE;

            LSPString *ptr = vArgs.get(index);
            if (ptr == NULL)
                return STATUS_BAD_ARGUMENTS;
            if (value != NULL)
            {
                char *dup = ptr->clone_utf8();
                if (dup == NULL)
                    return STATUS_NO_MEM;

                *value = dup;
            }

            vArgs.remove(index);
            delete ptr;
            return STATUS_OK;
        }

        status_t Process::insert_arg(size_t index, const LSPString *value)
        {
            if (nStatus != PSTATUS_CREATED)
                return STATUS_BAD_STATE;
            if (value == NULL)
                return STATUS_BAD_ARGUMENTS;

            LSPString *arg = new LSPString();
            if (arg == NULL)
                return STATUS_NO_MEM;

            if ((!arg->set(value)) || (!vArgs.insert(index, arg)))
            {
                delete arg;
                return STATUS_NO_MEM;
            }

            return STATUS_OK;
        }

        status_t Process::insert_arg(size_t index, const char *value)
        {
            if (nStatus != PSTATUS_CREATED)
                return STATUS_BAD_STATE;
            if (value == NULL)
                return STATUS_BAD_ARGUMENTS;

            LSPString *arg = new LSPString();
            if (arg == NULL)
                return STATUS_NO_MEM;

            if ((!arg->set_utf8(value)) || (!vArgs.insert(index, arg)))
            {
                delete arg;
                return STATUS_NO_MEM;
            }

            return STATUS_OK;
        }

        status_t Process::clear_args()
        {
            if (nStatus != PSTATUS_CREATED)
                return STATUS_BAD_STATE;
            destroy_args(&vArgs);
            return STATUS_OK;
        }

        size_t Process::envs() const
        {
            return vEnv.size();
        }

        status_t Process::set_env(const LSPString *key, const LSPString *value)
        {
            if (nStatus != PSTATUS_CREATED)
                return STATUS_BAD_STATE;
            if ((key == NULL) || (value == NULL))
                return STATUS_BAD_ARGUMENTS;
            if (key->index_of('=') >= 0)
                return STATUS_BAD_FORMAT;

            envvar_t *var;
            for (size_t i=0, n=vEnv.size(); i<n; ++i)
            {
                var = vEnv.uget(i);
                if (var->name.equals(key))
                    return (var->value.set(value)) ? STATUS_OK : STATUS_NO_MEM;
            }

            if ((var = new envvar_t) == NULL)
                return STATUS_NO_MEM;

            if ((!var->name.set(key)) || (!var->value.set(value)) || (!vEnv.add(var)))
            {
                delete var;
                return STATUS_NO_MEM;
            }

            return STATUS_OK;
        }

        status_t Process::set_env(const char *key, const char *value)
        {
            if (nStatus != PSTATUS_CREATED)
                return STATUS_BAD_STATE;
            if ((key == NULL) || (value == NULL))
                return STATUS_BAD_ARGUMENTS;
            if (strchr(key, '=') != NULL)
                return STATUS_BAD_FORMAT;

            LSPString k, v;
            if ((!k.set_utf8(key)) || (!v.set_utf8(value)))
                return STATUS_NO_MEM;
            return set_env(&k, &v);
        }

        status_t Process::remove_env(const LSPString *key, LSPString *value)
        {
            if (nStatus != PSTATUS_CREATED)
                return STATUS_BAD_STATE;
            if (key == NULL)
                return STATUS_BAD_ARGUMENTS;

            for (size_t i=0, n=vEnv.size(); i<n; ++i)
            {
                envvar_t *var = vEnv.uget(i);
                if (var->name.equals(key))
                {
                    if (value != NULL)
                        value->swap(&var->value);
                    delete var;
                    vEnv.qremove(i);
                    return STATUS_OK;
                }
            }

            return STATUS_NOT_FOUND;
        }

        status_t Process::remove_env(const char *key, char **value)
        {
            if (nStatus != PSTATUS_CREATED)
                return STATUS_BAD_STATE;
            if (key == NULL)
                return STATUS_BAD_ARGUMENTS;

            LSPString k;
            if (!k.set_utf8(key))
                return STATUS_NO_MEM;

            for (size_t i=0, n=vEnv.size(); i<n; ++i)
            {
                envvar_t *var = vEnv.uget(i);
                if (var->name.equals(&k))
                {
                    if (value != NULL)
                    {
                        char *dup       = var->value.clone_utf8();
                        if (dup == NULL)
                            return STATUS_NO_MEM;
                        *value          = dup;
                    }
                    delete var;
                    vEnv.qremove(i);
                    return STATUS_OK;
                }
            }

            return STATUS_NOT_FOUND;
        }

        status_t Process::remove_env(const char *key, LSPString *value)
        {
            if (nStatus != PSTATUS_CREATED)
                return STATUS_BAD_STATE;
            if (key == NULL)
                return STATUS_BAD_ARGUMENTS;

            LSPString k;
            if (!k.set_utf8(key))
                return STATUS_NO_MEM;

            for (size_t i=0, n=vEnv.size(); i<n; ++i)
            {
                envvar_t *var = vEnv.uget(i);
                if (!var->name.equals(&k))
                    continue;

                if (value != NULL)
                    value->swap(&var->value);
                delete var;
                vEnv.qremove(i);
                return STATUS_OK;
            }

            return STATUS_NOT_FOUND;
        }

        status_t Process::get_env(const LSPString *key, LSPString *value)
        {
            if (key == NULL)
                return STATUS_BAD_ARGUMENTS;

            if (nStatus != PSTATUS_CREATED)
                return STATUS_BAD_STATE;

            for (size_t i=0, n=vEnv.size(); i<n; ++i)
            {
                envvar_t *var = vEnv.uget(i);
                if (!var->name.equals(key))
                    continue;

                if (value != NULL)
                {
                    if (!value->set(&var->value))
                        return STATUS_NO_MEM;
                }
                return STATUS_OK;
            }

            return STATUS_NOT_FOUND;
        }

        status_t Process::get_env(const char *key, LSPString *value)
        {
            if (key == NULL)
                return STATUS_BAD_ARGUMENTS;

            LSPString k;
            if (!k.set_utf8(key))
                return STATUS_NO_MEM;

            for (size_t i=0, n=vEnv.size(); i<n; ++i)
            {
                envvar_t *var = vEnv.uget(i);
                if (!var->name.equals(&k))
                    continue;

                if (value != NULL)
                {
                    if (!value->set(&var->value))
                        return STATUS_NO_MEM;
                }
                return STATUS_OK;
            }

            return STATUS_NOT_FOUND;
        }

        status_t Process::get_env(const char *key, char **value)
        {
            if (key == NULL)
                return STATUS_BAD_ARGUMENTS;

            LSPString k;
            if (!k.set_utf8(key))
                return STATUS_NO_MEM;

            for (size_t i=0, n=vEnv.size(); i<n; ++i)
            {
                envvar_t *var = vEnv.uget(i);
                if (!var->name.equals(&k))
                    continue;

                if (value != NULL)
                {
                    char *dup       = var->value.clone_utf8();
                    if (dup == NULL)
                        return STATUS_NO_MEM;
                    *value          = dup;
                }
                return STATUS_OK;
            }

            return STATUS_NOT_FOUND;
        }

        status_t Process::read_env(size_t idx, LSPString *key, LSPString *value)
        {
            if ((key == NULL) && (value == NULL))
                return STATUS_BAD_ARGUMENTS;

            envvar_t *var = vEnv.get(idx);
            if (var == NULL)
                return STATUS_BAD_ARGUMENTS;

            LSPString tk, tv;
            if (key != NULL)
            {
                if (!tk.set(&var->name))
                    return STATUS_NO_MEM;
                if (value != NULL)
                {
                    if (!tv.set(&var->value))
                        return STATUS_NO_MEM;
                    value->swap(&tv);
                }

                key->swap(&tk);
                return STATUS_OK;
            }

            if (!tv.set(&var->value))
                return STATUS_NO_MEM;

            value->swap(&tv);
            return STATUS_OK;
        }

        status_t Process::read_env(size_t idx, char **key, char **value)
        {
            if ((key == NULL) && (value == NULL))
                return STATUS_BAD_ARGUMENTS;

            envvar_t *var = vEnv.get(idx);
            if (var == NULL)
                return STATUS_BAD_ARGUMENTS;

            char *tk, *tv;

            if (key != NULL)
            {
                if ((tk = var->name.clone_utf8()) == NULL)
                    return STATUS_NO_MEM;

                if (value != NULL)
                {
                    if ((tv = var->value.clone_utf8()) == NULL)
                    {
                        ::free(tk);
                        return STATUS_NO_MEM;
                    }

                    *value = tv;
                }

                *key = tk;
                return STATUS_OK;
            }

            if ((tv = var->value.clone_utf8()) == NULL)
                return STATUS_NO_MEM;

            *value = tv;
            return STATUS_OK;
        }

        status_t Process::clear_env()
        {
            if (nStatus != PSTATUS_CREATED)
                return STATUS_BAD_STATE;
            destroy_env(&vEnv);
            return STATUS_OK;
        }

        size_t Process::status()
        {
            wait(0); // Try to update status
            return nStatus;
        }

        bool Process::exited()
        {
            return status() == PSTATUS_EXITED;
        }

        bool Process::running()
        {
            if (nStatus == PSTATUS_RUNNING_ERROR)
                return true;
            return status() == PSTATUS_RUNNING;
        }

        bool Process::valid()
        {
            return status() != PSTATUS_ERROR;
        }

        ssize_t Process::process_id() const
        {
            switch (nStatus)
            {
                case PSTATUS_RUNNING:
                case PSTATUS_EXITED:
                    return nPID;
                default:
                    break;
            }
            return -1;
        }

        status_t Process::exit_code(int *code)
        {
            if (code == NULL)
                return STATUS_BAD_ARGUMENTS;

            if (nStatus != PSTATUS_EXITED)
                return STATUS_BAD_STATE;

            *code   = nExitCode;
            return STATUS_OK;
        }

#ifdef PLATFORM_WINDOWS
        status_t Process::append_arg_escaped(LSPString *dst, const LSPString *value)
        {
            if (value->is_empty())
                return (dst->append_ascii("\"\"")) ? STATUS_OK : STATUS_NO_MEM;

            for (size_t i=0, n=value->length(); i<n; ++i)
            {
                lsp_wchar_t ch = value->char_at(i);
                switch (ch)
                {
                    case ' ':
                        if (!dst->append_ascii("\" \"", 3))
                            return STATUS_NO_MEM;
                        break;
                    case '"':
                        if (!dst->append_ascii("\\\"", 2))
                            return STATUS_NO_MEM;
                        break;
                    default:
                        if (!dst->append(ch))
                            return STATUS_NO_MEM;
                        break;
                }
            }

            return STATUS_OK;
        }

        status_t Process::build_argv(LSPString *dst)
        {
            status_t res = append_arg_escaped(dst, &sCommand);
            if (res != STATUS_OK)
                return res;

            for (size_t i=0, n=vArgs.size(); i<n; ++i)
            {
                LSPString *arg = vArgs.uget(i);
                if (!dst->append(' '))
                    return STATUS_NO_MEM;
                res = append_arg_escaped(dst, arg);
                if (res != STATUS_OK)
                    return res;
            }

            return STATUS_OK;
        }

        status_t Process::build_envp(LSPString *dst)
        {
            for (size_t i=0, n=vEnv.size(); i<n; ++i)
            {
                envvar_t *env = vEnv.uget(i);
                if (!dst->append(&env->name))
                    return STATUS_NO_MEM;
                if (!dst->append('='))
                    return STATUS_NO_MEM;
                if (!dst->append(&env->value))
                    return STATUS_NO_MEM;
                if (!dst->append('\0'))
                    return STATUS_NO_MEM;
            }

            // Note that an ANSI environment block is terminated by two zero bytes:
            // one for the last string, one more to terminate the block. A Unicode
            // environment block is terminated by four zero bytes: two for the last
            // string, two more to terminate the block.
            return (dst->append('\0')) ? STATUS_OK : STATUS_NO_MEM;
        }

        status_t Process::launch()
        {
            if (nStatus != PSTATUS_CREATED)
                return STATUS_BAD_STATE;
            if (sCommand.is_empty())
                return STATUS_BAD_STATE;

            // Obtain the command string
            status_t res;
            LSPString xcmd;
            res = find_executable(xcmd, sCommand);
            if ((res != STATUS_OK) && (res != STATUS_NOT_FOUND))
                return res;
            LSPString & program = (res == STATUS_OK) ? xcmd : sCommand;

            // Form argv and envp
            LSPString argv, envp;
            if ((res = build_argv(&argv)) != STATUS_OK)
                return res;
            if ((res = build_envp(&envp)) != STATUS_OK)
                return res;

            // Launch child process
            STARTUPINFOW si;
            PROCESS_INFORMATION pi;

            ZeroMemory( &si, sizeof(STARTUPINFOW) );
            ZeroMemory( &pi, sizeof(PROCESS_INFORMATION) );
            si.cb       = sizeof(si);

            // Override STDIN, STDOUT, STDERR
            if (hStdIn != NULL)
            {
                si.dwFlags     |= STARTF_USESTDHANDLES;
                si.hStdInput    = hStdIn;
            }
            if (hStdOut != NULL)
            {
                si.dwFlags     |= STARTF_USESTDHANDLES;
                si.hStdOutput   = hStdOut;
            }
            if (hStdErr != NULL)
            {
                si.dwFlags     |= STARTF_USESTDHANDLES;
                si.hStdError    = hStdErr;
            }

            // Prepare buffers
            WCHAR *wargv        = argv.clone_utf16();
            if (wargv == NULL)
                return STATUS_NO_MEM;

            WCHAR *wenvp        = envp.clone_utf16();
            if (wenvp == NULL)
            {
                ::free(wargv);
                return STATUS_NO_MEM;
            }

            // Start the child process.
            if( !::CreateProcessW(
                program.get_utf16(),    // Module name (use command line)
                wargv,                  // Command line
                NULL,                   // Process handle not inheritable
                NULL,                   // Thread handle not inheritable
                FALSE,                  // Set handle inheritance to FALSE
                CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT, // Use unicode environment
                wenvp,                  // Set-up environment block
                NULL,                   // Use parent's starting directory
                &si,                    // Pointer to STARTUPINFO structure
                &pi                     // Pointer to PROCESS_INFORMATION structure
            ))
            {
                int error = ::GetLastError();
                ::fprintf(stderr, "Failed to create child process (%d)\n", error);
                ::fflush(stderr);

                ::free(wenvp);
                ::free(wargv);

                nStatus   = PSTATUS_RUNNING_ERROR;
                switch (error)
                {
                    case ERROR_FILE_NOT_FOUND:
                    case ERROR_PATH_NOT_FOUND:
                        nExitCode = STATUS_NOT_FOUND;
                        break;
                    default:
                        nExitCode = STATUS_UNKNOWN_ERR;
                        break;
                }

                return res;
            }

            // Free resources and close redirected file handles
            ::free(wenvp);
            ::free(wargv);
            close_handles();

            // Update state
            hProcess    = pi.hProcess;
            nPID        = pi.dwProcessId;
            nStatus     = PSTATUS_RUNNING;

            return res;
        }

        status_t Process::wait(wssize_t millis)
        {
            // If we're simulating some process that failed to start as running,
            // then we need to update the status and just return.
            // Otherwise we need to reject any wait() request since process is in
            // bad state.
            if (nStatus == PSTATUS_RUNNING_ERROR)
            {
                nStatus = PSTATUS_EXITED;
                return STATUS_OK;
            }
            else if (nStatus != PSTATUS_RUNNING)
                return STATUS_BAD_STATE;

            // Wait for completion of the process
            DWORD res = ::WaitForSingleObject(hProcess, (millis < 0) ? INFINITE : millis);
            switch (res)
            {
                case WAIT_OBJECT_0:
                {
                    // Obtain exit status of process
                    DWORD code  = 0;
                    ::GetExitCodeProcess(hProcess, &code);
                    ::CloseHandle(hProcess);
                    hProcess    = NULL;

                    // Update state
                    nStatus     = PSTATUS_EXITED;
                    nExitCode   = code;
                    return STATUS_OK;
                }

                case WAIT_TIMEOUT:
                    return STATUS_OK;

                default:
                    break;
            }

            return STATUS_UNKNOWN_ERR;
        }

        void Process::close_handles()
        {
            if (hStdIn != NULL)
            {
                ::CloseHandle(hStdIn);
                hStdIn          = NULL;
            }
            if (hStdOut != NULL)
            {
                ::CloseHandle(hStdOut);
                hStdOut         = NULL;
            }
            if (hStdErr != NULL)
            {
                ::CloseHandle(hStdErr);
                hStdErr         = NULL;
            }
        }

        io::IOutStream *Process::get_stdin()
        {
            if ((nStatus != PSTATUS_CREATED) || (pStdIn != NULL))
                return pStdIn;

            HANDLE hRead = NULL, hWrite = NULL;
            if (!::CreatePipe(&hRead, &hWrite, NULL, 0))
                return NULL;

            // Create stream and wrap
            io::OutFileStream *strm = new io::OutFileStream();
            if ((strm == NULL) || (strm->wrap_native(hWrite, true) != STATUS_OK))
            {
                delete strm;
                ::CloseHandle(hRead);
                ::CloseHandle(hWrite);
                return NULL;
            }

            // All is OK
            pStdIn  = strm;
            hStdIn  = hRead;

            return pStdIn;
        }

        io::IInStream *Process::get_stdout()
        {
            if ((nStatus != PSTATUS_CREATED) || (pStdOut != NULL))
                return pStdOut;

            HANDLE hRead = NULL, hWrite = NULL;
            if (!::CreatePipe(&hRead, &hWrite, NULL, 0))
                return NULL;

            // Create stream and wrap
            io::InFileStream *strm = new io::InFileStream();
            if ((strm == NULL) || (strm->wrap_native(hRead, true) != STATUS_OK))
            {
                delete strm;
                ::CloseHandle(hRead);
                ::CloseHandle(hWrite);
                return NULL;
            }

            // All is OK
            pStdOut = strm;
            hStdOut = hWrite;

            return pStdOut;
        }

        io::IInStream *Process::get_stderr()
        {
            if ((nStatus != PSTATUS_CREATED) || (pStdErr != NULL))
                return pStdErr;

            HANDLE hRead = NULL, hWrite = NULL;
            if (!::CreatePipe(&hRead, &hWrite, NULL, 0))
                return NULL;

            // Create stream and wrap
            io::InFileStream *strm = new io::InFileStream();
            if ((strm == NULL) || (strm->wrap_native(hRead, true) != STATUS_OK))
            {
                delete strm;
                ::CloseHandle(hRead);
                ::CloseHandle(hWrite);
                return NULL;
            }

            // All is OK
            pStdErr = strm;
            hStdErr = hWrite;

            return pStdErr;
        }
#else
        static void drop_data(lltl::parray<char> *v)
        {
            for (size_t i=0, n=v->size(); i<n; ++i)
            {
                char *ptr = v->uget(i);
                if (ptr != NULL)
                    ::free(ptr);
            }
            v->flush();
        }

        void Process::close_handles()
        {
            if (hStdIn >= 0)
            {
                ::close(hStdIn);
                hStdIn          = -1;
            }
            if (hStdOut >= 0)
            {
                ::close(hStdOut);
                hStdOut         = -1;
            }
            if (hStdErr >= 0)
            {
                ::close(hStdErr);
                hStdErr         = -1;
            }
        }

        status_t Process::build_argv(lltl::parray<char> *dst)
        {
            char *s;

            // Add command as argv[0]
            if ((s = sCommand.clone_native()) == NULL)
                return STATUS_NO_MEM;
            if (!dst->add(s))
                return STATUS_NO_MEM;

            // Add all other arguments
            for (size_t i=0, n=vArgs.size(); i<n; ++i)
            {
                LSPString *arg = vArgs.uget(i);
                if (arg == NULL)
                    continue;

                if ((s = arg->clone_native()) == NULL)
                    return STATUS_NO_MEM;
                if (!dst->add(s))
                {
                    ::free(s);
                    return STATUS_NO_MEM;
                }
            }

            // Add terminator
            return (dst->add(static_cast<char *>(NULL))) ? STATUS_OK : STATUS_NO_MEM;
        }

        status_t Process::build_envp(lltl::parray<char> *dst)
        {
            char *s;

            LSPString tmp;
            for (size_t i=0, n=vEnv.size(); i<n; ++i)
            {
                envvar_t *var = vEnv.uget(i);
                if (var == NULL)
                    continue;
                if (!tmp.set(&var->name))
                    return STATUS_NO_MEM;
                if (!tmp.append('='))
                    return STATUS_NO_MEM;
                if (!tmp.append(&var->value))
                    return STATUS_NO_MEM;

                if ((s = tmp.clone_native()) == NULL)
                    return STATUS_NO_MEM;

                if (!dst->add(s))
                {
                    ::free(s);
                    return STATUS_NO_MEM;
                }
            }
            return (dst->add(static_cast<char *>(NULL))) ? STATUS_OK : STATUS_NO_MEM;
        }

        status_t Process::spawn_process(const char *cmd, char * const *argv, char * const *envp)
        {
            lsp_trace("Creating child process using posix_spawn...");

            // Initialize spawn routines
            posix_spawnattr_t attr;
            if (::posix_spawnattr_init(&attr))
                return STATUS_UNKNOWN_ERR;

            #if defined(__USE_GNU) || defined(POSIX_SPAWN_USEVFORK)
                // Prever vfork() over fork()
                if (::posix_spawnattr_setflags(&attr, POSIX_SPAWN_USEVFORK))
                {
                    ::posix_spawnattr_destroy(&attr);
                    return STATUS_UNKNOWN_ERR;
                }
            #endif /* __USE_GNU */

            posix_spawn_file_actions_t actions;
            if (::posix_spawn_file_actions_init(&actions))
            {
                ::posix_spawnattr_destroy(&attr);
                return STATUS_UNKNOWN_ERR;
            }

            // Override STDIN, STDOUT, STDERR
            if (hStdIn >= 0)
            {
                if (::posix_spawn_file_actions_adddup2(&actions, hStdIn, STDIN_FILENO))
                {
                    ::posix_spawnattr_destroy(&attr);
                    return STATUS_UNKNOWN_ERR;
                }
                if (::posix_spawn_file_actions_addclose(&actions, hStdIn))
                {
                    ::posix_spawnattr_destroy(&attr);
                    return STATUS_UNKNOWN_ERR;
                }
            }

            if (hStdOut >= 0)
            {
                if (::posix_spawn_file_actions_adddup2(&actions, hStdOut, STDOUT_FILENO))
                {
                    ::posix_spawnattr_destroy(&attr);
                    return STATUS_UNKNOWN_ERR;
                }
                if (::posix_spawn_file_actions_addclose(&actions, hStdOut))
                {
                    ::posix_spawnattr_destroy(&attr);
                    return STATUS_UNKNOWN_ERR;
                }
            }

            if (hStdErr >= 0)
            {
                if (::posix_spawn_file_actions_adddup2(&actions, hStdErr, STDERR_FILENO))
                {
                    ::posix_spawnattr_destroy(&attr);
                    return STATUS_UNKNOWN_ERR;
                }
                if (::posix_spawn_file_actions_addclose(&actions, hStdErr))
                {
                    ::posix_spawnattr_destroy(&attr);
                    return STATUS_UNKNOWN_ERR;
                }
            }

            // Perform posix_spawn()
            pid_t pid;
            status_t res = STATUS_OK;
            while (true)
            {
                int x = ::posix_spawnp(&pid, cmd, &actions, &attr, argv, envp);
                switch (x)
                {
                    case 0: break;
                    case EAGAIN: continue;
                    case ENOMEM: res = STATUS_NO_MEM; break;
                    default: res = STATUS_UNKNOWN_ERR; break;
                }
                break;
            }

            // Success execution?
            if (res == STATUS_OK)
            {
                nPID        = pid;
                nStatus     = PSTATUS_RUNNING;
            }

            ::posix_spawn_file_actions_destroy(&actions);
            ::posix_spawnattr_destroy(&attr);

            return res;
        }

        void Process::execvpe_process(const char *cmd, char * const *argv, char * const *envp, bool soft_exit)
        {
            // Override STDIN, STDOUT, STDERR
            if (hStdIn >= 0)
            {
                ::dup2(hStdIn, STDIN_FILENO);
                ::close(hStdIn);
                hStdIn = -1;
            }

            if (hStdOut >= 0)
            {
                ::dup2(hStdOut, STDOUT_FILENO);
                ::close(hStdOut);
                hStdOut = -1;
            }

            if (hStdErr >= 0)
            {
                ::dup2(hStdErr, STDERR_FILENO);
                ::close(hStdErr);
                hStdErr = -1;
            }

            // Launch the process
#ifndef PLATFORM_HAS_EXECVPE
            ::execve(cmd, argv, envp);
#else
            if (strchr(cmd, FILE_SEPARATOR_C) != NULL)
                ::execve(cmd, argv, envp);
            else
                ::execvpe(cmd, argv, envp);
#endif /* PLATFORM_HAS_EXECVPE */

            lsp_trace("exec failed for pid=%d\n", int(getpid()));

            // Return error only if ::execvpe failed
            if (soft_exit)
                ::_exit(STATUS_UNKNOWN_ERR);
            else
                ::exit(STATUS_UNKNOWN_ERR);
        }

        status_t Process::vfork_process(const char *cmd, char * const *argv, char * const *envp)
        {
            lsp_trace("Creating child process using vfork()...");
            errno           = 0;

        #ifdef PLATFORM_MACOSX
            #if defined(COMPILER_CLANG)
                #pragma clang diagnostic push
                #pragma clang diagnostic ignored "-Wdeprecated-declarations"
            #elif defined(COMPILER_GCC)
                #pragma GCC diagnostic push
                #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
            #endif
        #endif /* PLATFORM_MACOSX */

            // vfork() is deprecated in MacOS, we'll hide deprectation warning for that as
            // vfork() is a fallback case for posix_spawn.
            pid_t pid       = ::vfork();

        #ifdef PLATFORM_MACOSX
            #if defined(COMPILER_CLANG)
                #pragma clang diagnostic pop
            #elif defined(COMPILER_GCC)
                #pragma GCC diagnostic pop
            #endif
        #endif /* PLATFORM_MACOSX */

            // Failed to fork()?
            if (pid < 0)
            {
                int code        = errno;
                switch (code)
                {
                    case ENOMEM: return STATUS_NO_MEM;
                    case EAGAIN: return STATUS_NO_MEM;
                    default: return STATUS_UNKNOWN_ERR;
                }
            }

            // The child process stuff
            if (pid == 0)
                execvpe_process(cmd, argv, envp, true);

            // The parent process stuff
            nPID        = pid;
            nStatus     = PSTATUS_RUNNING;

            return STATUS_OK;
        }

        status_t Process::fork_process(const char *cmd, char * const *argv, char * const *envp)
        {
            lsp_trace("Creating child process using fork()...");
            errno           = 0;
            pid_t pid       = ::fork();

            // Failed to fork()?
            if (pid < 0)
            {
                int code        = errno;
                switch (code)
                {
                    case ENOMEM: return STATUS_NO_MEM;
                    case EAGAIN: return STATUS_NO_MEM;
                    default: return STATUS_UNKNOWN_ERR;
                }
            }

            // The child process stuff
            if (pid == 0)
                execvpe_process(cmd, argv, envp, false);

            // The parent process stuff
            nPID        = pid;
            nStatus     = PSTATUS_RUNNING;

            return STATUS_OK;
        }

        status_t Process::launch()
        {
            if (nStatus != PSTATUS_CREATED)
                return STATUS_BAD_STATE;
            if (sCommand.is_empty())
                return STATUS_BAD_STATE;

            status_t res;
            lltl::parray<char> argv;
            lltl::parray<char> envp;
            lsp_finally {
                drop_data(&argv);
                drop_data(&envp);
            };

            // Make execution command
        #ifndef PLATFORM_HAS_EXECVPE
            LSPString xcmd;
            res = find_executable(xcmd, sCommand);
            if ((res != STATUS_OK) && (res != STATUS_NOT_FOUND))
                return res;

            char *cmd = (res == STATUS_OK) ? xcmd.clone_native() : sCommand.clone_native();
        #else
            char *cmd = sCommand.clone_native();
        #endif /* PLATFORM_HAS_EXECVPE */
            if (cmd == NULL)
                return STATUS_NO_MEM;
            lsp_finally { free(cmd); };

            // Form argv
            if ((res = build_argv(&argv)) != STATUS_OK)
                return res;

            // Form envp
            res = build_envp(&envp);
            if (res == STATUS_OK)
            {
                res = STATUS_UNKNOWN_ERR;

                // Different behaviour, depending on POSIX_SPAWN_USEVFORK presence
                #if defined(__USE_GNU) || defined(POSIX_SPAWN_USEVFORK)
                    res    = spawn_process(cmd, argv.array(), envp.array());
                    if (res != STATUS_OK)
                        res    = vfork_process(cmd, argv.array(), envp.array());
                #else
                    res    = vfork_process(cmd, argv.array(), envp.array());
                    if (res != STATUS_OK)
                        res    = spawn_process(cmd, argv.array(), envp.array());
                #endif /* __USE_GNU */

                if (res != STATUS_OK)
                    res    = fork_process(cmd, argv.array(), envp.array());
            }

            // Close redirected file handles
            if (res == STATUS_OK)
                close_handles();

            return res;
        }

        status_t Process::wait(wssize_t millis)
        {
            if (nStatus != PSTATUS_RUNNING)
                return STATUS_BAD_STATE;

            int status;

            if (millis < 0)
            {
                do
                {
                    // Wait for child process
                    pid_t pid = ::waitpid(nPID, &status, WUNTRACED | WCONTINUED);
                    if (pid < 0)
                    {
                        status = errno;
                        if (status == EINTR)
                            continue;
                        return STATUS_UNKNOWN_ERR;
                    }
                } while ((!WIFEXITED(status)) && (!WIFSIGNALED(status)));

                nStatus     = PSTATUS_EXITED;
                nExitCode   = WEXITSTATUS(status);
            }
            else if (millis == 0)
            {
                // Wait for child process
                pid_t pid = ::waitpid(nPID, &status, WUNTRACED | WCONTINUED | WNOHANG);
                if (pid < 0)
                {
                    status = errno;
                    return (status == EINTR) ? STATUS_OK : STATUS_UNKNOWN_ERR;
                }

                // Child has exited?
                if ((pid == nPID) && ((WIFEXITED(status)) || (WIFSIGNALED(status))))
                {
                    nStatus     = PSTATUS_EXITED;
                    nExitCode   = WEXITSTATUS(status);
                }
            }
            else
            {
                struct timespec ts;
                wssize_t deadline, left;
                ::clock_gettime(CLOCK_REALTIME, &ts);
                deadline    = millis + (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

                while (true)
                {
                    // Wait for child process
                    pid_t pid = ::waitpid(nPID, &status, WUNTRACED | WCONTINUED | WNOHANG);
                    if (pid < 0)
                    {
                        status = errno;
                        if (status == EINTR)
                            continue;
                        return STATUS_UNKNOWN_ERR;
                    }

                    // Child process has exited?
                    if ((pid == nPID) && ((WIFEXITED(status)) || (WIFSIGNALED(status))))
                        break;

                    // Get time
                    ::clock_gettime(CLOCK_REALTIME, &ts);
                    left    = deadline - (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
                    if (left <= 0)
                        return STATUS_OK; // Just leave, no changes

                    // Perform short sleep
                    ts.tv_sec     = 0;
                    ts.tv_nsec    = ((left > 50) ? 50 : left) * 1000000;
                    ::nanosleep(&ts, NULL);
                }

                nStatus     = PSTATUS_EXITED;
                nExitCode   = WEXITSTATUS(status);
            }

            return STATUS_OK;
        }

        io::IOutStream *Process::get_stdin()
        {
            if ((nStatus != PSTATUS_CREATED) || (pStdIn != NULL))
                return pStdIn;

            int fd[2]; // rw
            if (::pipe(fd) != 0)
                return NULL;

            // Create stream and wrap
            io::OutFileStream *strm = new io::OutFileStream();
            if ((strm == NULL) || (strm->wrap_native(fd[1], true) != STATUS_OK))
            {
                delete strm;
                ::close(fd[0]);
                ::close(fd[1]);
                return NULL;
            }

            // All is OK
            pStdIn  = strm;
            hStdIn  = fd[0];

            return pStdIn;
        }

        io::IInStream *Process::get_stdout()
        {
            if ((nStatus != PSTATUS_CREATED) || (pStdOut != NULL))
                return pStdOut;

            int fd[2]; // rw
            if (::pipe(fd) != 0)
                return NULL;

            // Create stream and wrap
            io::InFileStream *strm = new io::InFileStream();
            if ((strm == NULL) || (strm->wrap_native(fd[0], true) != STATUS_OK))
            {
                delete strm;
                ::close(fd[0]);
                ::close(fd[1]);
                return NULL;
            }

            // All is OK
            pStdOut = strm;
            hStdOut = fd[1];

            return pStdOut;
        }

        io::IInStream *Process::get_stderr()
        {
            if ((nStatus != PSTATUS_CREATED) || (pStdErr != NULL))
                return pStdErr;

            int fd[2]; // rw
            if (::pipe(fd) != 0)
                return NULL;

            // Create stream and wrap
            io::InFileStream *strm = new io::InFileStream();
            if ((strm == NULL) || (strm->wrap_native(fd[0], true) != STATUS_OK))
            {
                delete strm;
                ::close(fd[0]);
                ::close(fd[1]);
                return NULL;
            }

            // All is OK
            pStdErr = strm;
            hStdErr = fd[1];

            return pStdErr;
        }


#endif /* PLATFORM_WINDOWS */

        status_t Process::copy_env()
        {
            lltl::parray<envvar_t> env;
            LSPString k, v;

        #ifdef PLATFORM_WINDOWS
            for (WCHAR *item = GetEnvironmentStringsW(); (*item) != 0; )
            {
                size_t len  = wcslen(item);

                // Fetch environment variable
                if (!k.set_utf16(item, len))
                {
                    destroy_env(&env);
                    return STATUS_NO_MEM;
                }
                item    += (len + 1); // length + terminating character
        #else
            for (char **item=environ; *item != NULL; ++item)
            {
                // Fetch environment variable
                if (!k.set_native(*item))
                {
                    destroy_env(&env);
                    return STATUS_NO_MEM;
                }
        #endif /* PLATFORM_WINDOWS */

                // Parse record
                ssize_t idx = k.index_of('=');
                if (idx >= 0)
                {
                    if (!v.set(&k, idx+1))
                    {
                        destroy_env(&env);
                        return STATUS_NO_MEM;
                    }
                    if (!k.truncate(idx))
                    {
                        destroy_env(&env);
                        return STATUS_NO_MEM;
                    }
                }

                // Allocate && add env var
                envvar_t *var = new envvar_t();
                if ((var == NULL) || (!env.add(var)))
                {
                    destroy_env(&env);
                    return STATUS_NO_MEM;
                }

                // Store value to env
                var->name.swap(&k);
                var->value.swap(&v);

            } // for

            // Commit result
            vEnv.swap(&env);
            destroy_env(&env);

            return STATUS_OK;
        }
    } /* namespace ipc */
} /* namespace lsp */
