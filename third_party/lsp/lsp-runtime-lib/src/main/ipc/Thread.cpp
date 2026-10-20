/*
 * Copyright (C) 2026 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2026 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 25 февр. 2019 г.
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
#include <lsp-plug.in/common/atomic.h>
#include <lsp-plug.in/ipc/Thread.h>

#include <time.h>
#include <errno.h>
#include <unistd.h>

#if defined(PLATFORM_WINDOWS)
    #include <windows.h>
    #include <processthreadsapi.h>
#elif defined(PLATFORM_LINUX)
    #include <sched.h>
    #include <sys/syscall.h>
#elif defined(PLATFORM_SOLARIS)
    #include <pthread.h>
#elif defined(PLATFORM_FREEBSD)
    #include <sched.h>
    #include <sys/thr.h>
#elif defined(PLATFORM_NETBSD)
    #include <sched.h>
    #include <lwp.h>
#elif defined(PLATFORM_MACOSX)
    #include <mach/mach.h>
    #include <mach/mach_error.h>
    #include <sched.h>
#elif defined(PLATFORM_DRAGONFLYBSD)
    #include <sched.h>
    #include <lwp.h>
#elif defined(PLATFORM_HAIKU)
    #include <kernel/OS.h>
#endif /* PLATFORM_WINDOWS */

namespace lsp
{
    namespace ipc
    {
        __thread Thread *Thread::pThis = NULL;
        
    #if defined(PLATFORM_WINDOWS)
        #define CLR_HANDLE(hThread) hThread     = INVALID_HANDLE_VALUE;
    #else
        #define CLR_HANDLE(hThread) hThread     = 0;
    #endif

        Thread::Thread()
        {
            atomic_store(&enState, TS_CREATED);
            nResult             = STATUS_OK;
            bCancelled          = false;
            CLR_HANDLE(hThread);
            sBinding.proc       = NULL;
            sBinding.arg        = NULL;
            sBinding.runnable   = NULL;
        }
        
        Thread::Thread(thread_proc_t proc)
        {
            atomic_store(&enState, TS_CREATED);
            nResult             = STATUS_OK;
            bCancelled          = false;
            CLR_HANDLE(hThread);
            sBinding.proc       = proc;
            sBinding.arg        = NULL;
            sBinding.runnable   = NULL;
        }

        Thread::Thread(thread_proc_t proc, void *arg)
        {
            atomic_store(&enState, TS_CREATED);
            nResult             = STATUS_OK;
            bCancelled          = false;
            CLR_HANDLE(hThread);
            sBinding.proc       = proc;
            sBinding.runnable   = NULL;
            sBinding.arg        = arg;
        }

        Thread::Thread(IRunnable *runnable)
        {
            atomic_store(&enState, TS_CREATED);
            nResult             = STATUS_OK;
            bCancelled          = false;
            CLR_HANDLE(hThread);
            sBinding.proc       = NULL;
            sBinding.arg        = NULL;
            sBinding.runnable   = runnable;
        }

        Thread::~Thread()
        {
    #if defined(PLATFORM_WINDOWS)
            if (hThread != INVALID_HANDLE_VALUE)
                CloseHandle(hThread);
    #endif /* PLATFORM_WINDOWS */

            CLR_HANDLE(hThread);
        }

        status_t Thread::run()
        {
            if (sBinding.proc != NULL)
                return sBinding.proc(sBinding.arg);
            else if (sBinding.runnable != NULL)
                return sBinding.runnable->run();
            return STATUS_OK;
        }

        status_t Thread::cancel()
        {
            switch (atomic_load(&enState))
            {
                case TS_PENDING:
                case TS_RUNNING:
                    break;
                default:
                    return STATUS_BAD_STATE;
            }

            bCancelled  = true;
            return STATUS_OK;
        }
    
    #if defined(PLATFORM_WINDOWS)
        DWORD WINAPI Thread::thread_launcher(_In_ LPVOID lpParameter)
        {
            Thread *_this = reinterpret_cast<Thread *>(lpParameter);
            pThis           = _this;

            // Wait until we are ready to launch
            while (!atomic_cas(&_this->enState, TS_PENDING, TS_RUNNING)) {}

            // Execute the thread
            status_t res    = _this->run();
            pThis           = NULL;

            // Commit the 'FINISHED' status
            int state;
            do
            {
                state       = atomic_load(&_this->enState);
            } while (!atomic_cas(&_this->enState, state, TS_FINISHED));

            _this->nResult  = res;
            return 0;
        }

        status_t Thread::start()
        {
            DWORD tid;
            HANDLE thandle = CreateThread(NULL, 0, thread_launcher, this, 0, &tid);
            if (thandle == INVALID_HANDLE_VALUE)
                return STATUS_UNKNOWN_ERR;

            hThread     = thandle;
            atomic_store(&enState, TS_PENDING);
            return STATUS_OK;
        }

        status_t Thread::join()
        {
            switch (atomic_load(&enState))
            {
                case TS_CREATED:
                    return STATUS_BAD_STATE;
                case TS_PENDING:
                case TS_RUNNING:
                {
                    DWORD res = WaitForSingleObject(hThread, INFINITE);
                    if ((res != WAIT_OBJECT_0) && (res != WAIT_ABANDONED))
                        return STATUS_UNKNOWN_ERR;
                    break;
                }
                case TS_FINISHED:
                    return STATUS_OK;
                default:
                    return STATUS_BAD_STATE;
            }
            return STATUS_OK;
        }

        status_t Thread::sleep(wsize_t millis)
        {
            if (pThis == NULL)
            {
                while (millis > 0)
                {
                    DWORD interval  = (millis > 500) ? 500 : millis;
                    Sleep(interval);
                    millis         -= interval;
                }
            }
            else
            {
                if (pThis->bCancelled)
                    return STATUS_CANCELLED;

                while (millis > 0)
                {
                    if (pThis->bCancelled)
                        return STATUS_CANCELLED;

                    DWORD interval  = (millis > 100) ? 100 : millis;
                    Sleep(interval);
                    millis         -= interval;
                }
            }

            return STATUS_OK;
        }

        void Thread::yield()
        {
            SwitchToThread();
        }

    #else
        void *Thread::thread_launcher(void *arg)
        {
            Thread *_this   = reinterpret_cast<Thread *>(arg);
            pThis           = _this;

            // Cleanup cancellation state
            pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
            pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

            // Wait until we are ready to launch
            while (!atomic_cas(&_this->enState, TS_PENDING, TS_RUNNING)) {}

            // Execute the thread
            status_t res    = _this->run();

            // Commit the 'FINISHED' status
            int state;
            do
            {
                state       = atomic_load(&_this->enState);
            } while (!atomic_cas(&_this->enState, state, TS_FINISHED));

            _this->nResult  = res;
            return NULL;
        }

        status_t Thread::start()
        {
            pthread_t tid;
            if (pthread_create(&tid, NULL, &thread_launcher, this) != 0)
                return STATUS_UNKNOWN_ERR;

            hThread     = tid;
            atomic_store(&enState, TS_PENDING);
            return STATUS_OK;
        }

        status_t Thread::join()
        {
            switch (atomic_load(&enState))
            {
                case TS_CREATED:
                    return STATUS_BAD_STATE;
                case TS_PENDING:
                case TS_RUNNING:
                    if (pthread_join(hThread, NULL) != 0)
                        return STATUS_UNKNOWN_ERR;
                    break;
                case TS_FINISHED:
                    return STATUS_OK;
                default:
                    return STATUS_BAD_STATE;
            }
            return STATUS_OK;
        }

        status_t Thread::sleep(wsize_t millis)
        {
            struct timespec req, rem;

            if (pThis == NULL)
            {
                req.tv_sec  = millis / 1000;
                req.tv_nsec = (millis % 1000) * 1000000;

                while (::nanosleep(&req, &rem) != 0)
                {
                    int code = errno;
                    if (code != EINTR)
                        return STATUS_UNKNOWN_ERR;
                    req = rem;
                }
            }
            else
            {
                if (pThis->bCancelled)
                    return STATUS_CANCELLED;

                while (millis > 0)
                {
                    if (pThis->bCancelled)
                        return STATUS_CANCELLED;

                    wsize_t interval  = (millis > 100) ? 100 : millis;
                    req.tv_sec  = 0;
                    req.tv_nsec = interval * 1000000;

                    while (::nanosleep(&req, &rem) != 0)
                    {
                        int code = errno;
                        if (code != EINTR)
                            return STATUS_UNKNOWN_ERR;

                        if (pThis->bCancelled)
                            return STATUS_CANCELLED;
                        req = rem;
                    }

                    millis     -= interval;
                }
            }

            return STATUS_OK;
        }

        void Thread::yield()
        {
            sched_yield();
        }

    #endif /* PLATFORM_WINDOWS */


        thread_id_t Thread::current_thread_id()
        {
            thread_id_t result      = INVALID_THREAD_ID;

        #if defined(PLATFORM_WINDOWS)
            return GetCurrentThreadId();
        #elif defined(PLATFORM_LINUX)
            result                  = syscall( __NR_gettid );
        #elif defined(PLATFORM_SOLARIS)
            result                  = pthread_self();
        #elif defined(PLATFORM_MACOSX)
            const mach_port_t port  = mach_thread_self();
            mach_port_deallocate(mach_task_self(), port);
            result                  = port;
        #elif defined(PLATFORM_NETBSD)
            result                  = _lwp_self();
        #elif defined(PLATFORM_FREEBSD)
            long lwpid;
            thr_self( &lwpid );
            result                  = lwpid;
        #elif defined(PLATFORM_DRAGONFLYBSD)
            result                  = lwp_gettid();
        #elif defined (PLATFORM_HAIKU)
            result                  = find_thread(NULL);
        #else
            #error "need to implement Thread::current_thread_id"
        #endif
            return result;
        }

    } /* namespace ipc */
} /* namespace lsp */
