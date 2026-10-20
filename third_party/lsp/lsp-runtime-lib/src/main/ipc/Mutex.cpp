/*
 * Copyright (C) 2024 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2024 Vladimir Sadovnikov <sadko4u@gmail.com>
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

#include <lsp-plug.in/ipc/Mutex.h>
#include <errno.h>

#ifdef PLATFORM_WINDOWS
    #include <windows.h>
    #include <synchapi.h>
#endif /* PLATFORM_WINDOWS */

namespace lsp
{
    namespace ipc
    {
#if defined(PLATFORM_WINDOWS)

        namespace detail
        {
            struct CRITICAL_SECTION: public ::CRITICAL_SECTION
            {
            };
        } /* namespace detail */

        Mutex::Mutex()
        {
            hMutex      = new detail::CRITICAL_SECTION;
            if (hMutex != NULL)
                InitializeCriticalSectionAndSpinCount(hMutex, 0x10);
        }

        Mutex::~Mutex()
        {
            if (hMutex != NULL)
            {
                DeleteCriticalSection(hMutex);
                delete hMutex;
            }
        }

        bool Mutex::lock() const
        {
            EnterCriticalSection(hMutex);
            return true;
        }

        bool Mutex::try_lock() const
        {
            return TryEnterCriticalSection(hMutex);
        }

        bool Mutex::unlock() const
        {
            HANDLE thread = reinterpret_cast<HANDLE>(GetCurrentThreadId());
            if (hMutex->OwningThread != thread)
                return false;
            if (hMutex->RecursionCount <= 0)
                return false;

            LeaveCriticalSection(hMutex);
            return true;
        }

#elif defined(PLATFORM_LINUX)
        static inline int futex(
            int *addr,
            int futex_op,
            int val,
            const struct timespec *timeout,
            volatile int *addr2,
            int val3)
        {
            return syscall(SYS_futex, addr, futex_op, val, timeout, addr2, val3);
        }

        bool Mutex::lock() const
        {
            int res;

            // Check that we already own the mutex
            pthread_t tid   = pthread_self();
            if (nThreadId == tid)
            {
                ++nLocks;
                return true;
            }

            // Perform the wait until another thread releases the mutex
            while (!atomic_cas(&nLock, 1, 0))
            {
                // Issue wait
                res = futex(
                    &nLock,         // addr: address
                    FUTEX_WAIT,     // futex_op: WAIT for the notification related to the address
                    0,              // val: value to test for
                    NULL,           // timeout: infinite wait
                    NULL,           // addr2: unused
                    0);             // val3: unused
                if ((res == ENOSYS) || (res == EAGAIN))
                    sched_yield();
            }

            // Update lock state
            nThreadId       = tid;
            ++nLocks;

            return true;
        }

        bool Mutex::try_lock() const
        {
            // Check that we already own the mutex
            pthread_t tid   = pthread_self();
            if (nThreadId == tid)
            {
                ++nLocks;
                return true;
            }

            if (!atomic_cas(&nLock, 1, 0))
                return false;

            // Update lock state
            nThreadId       = tid;
            ++nLocks;

            return true;
        }

        bool Mutex::unlock() const
        {
            if (nThreadId != pthread_self())
                return false;

            // Update the lock state
            if (--nLocks)
                return true;
            nThreadId       = -1;

            if (atomic_cas(&nLock, 0, 1))
            {
                futex(
                    &nLock,         // addr: address
                    FUTEX_WAKE,     // futex_op: WAKEUP listeners at the specified address
                    1,              // val: Number of listeners to wake up
                    NULL,           // timeout: unused
                    0,              // addr2: unused
                    0);             // val3: unused
            }

            return true;
        }

#else
        Mutex::Mutex()
        {
            pthread_mutexattr_t attr;
            pthread_mutexattr_init(&attr);
            pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&sMutex, &attr);
            pthread_mutexattr_destroy(&attr);
        }

        Mutex::~Mutex()
        {
            pthread_mutex_destroy(&sMutex);
        }
#endif /* PLATFORM_LINUX */

    } /* namespace ipc */
} /* namespace lsp */
