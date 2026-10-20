/*
 * Copyright (C) 2026 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2026 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 4 янв. 2026 г.
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

#include <lsp-plug.in/ipc/Condition.h>

#ifdef PLATFORM_WINDOWS
    #include <windows.h>
    #include <synchapi.h>
#else
    #include <errno.h>
    #include <sys/time.h>
    #include <sched.h>
#endif /* PLATFORM_WINDOWS */

namespace lsp
{
    namespace ipc
    {
#if defined(PLATFORM_WINDOWS)

        namespace detail
        {
            struct condition_t
            {
                ::CRITICAL_SECTION      sMutex;
                ::CONDITION_VARIABLE    sCondition;
            };
        } /* namespace detail */

        Condition::Condition()
        {
            pState  = new detail::condition_t;
            if (pState != NULL)
            {
                InitializeCriticalSectionAndSpinCount(&pState->sMutex, 0x10);
                InitializeConditionVariable(&pState->sCondition);
            }
        }

        Condition::~Condition()
        {
            if (pState != NULL)
            {
                DeleteCriticalSection(&pState->sMutex);
                delete pState;
            }
        }

        bool Condition::lock() const
        {
            if (pState == NULL)
                return false;
            EnterCriticalSection(&pState->sMutex);
            return true;
        }

        bool Condition::try_lock() const
        {
            return (pState != NULL) ? TryEnterCriticalSection(&pState->sMutex) : false;
        }

        bool Condition::unlock() const
        {
            if (pState == NULL)
                return false;

            HANDLE thread = reinterpret_cast<HANDLE>(GetCurrentThreadId());
            if (pState->sMutex.OwningThread != thread)
                return false;
            if (pState->sMutex.RecursionCount <= 0)
                return false;

            LeaveCriticalSection(&pState->sMutex);
            return true;
        }

        bool Condition::notify() const
        {
            if (pState == NULL)
                return false;
            WakeConditionVariable(&pState->sCondition);
            return true;
        }

        bool Condition::notify_all() const
        {
            if (pState == NULL)
                return false;
            WakeAllConditionVariable(&pState->sCondition);
            return true;
        }

        status_t Condition::wait()
        {
            if (pState == NULL)
                return STATUS_BAD_STATE;

            if (SleepConditionVariableCS(&pState->sCondition, &pState->sMutex, INFINITE))
                return STATUS_OK;

            const DWORD result = GetLastError();
            return (result == ERROR_TIMEOUT) ? STATUS_TIMED_OUT: STATUS_UNKNOWN_ERR;
        }


        status_t Condition::wait(system::time_millis_t millis)
        {
            if (pState == NULL)
                return STATUS_BAD_STATE;

            do
            {
                const DWORD timeout = (millis >= INFINITE) ? INFINITE - 1 : DWORD(millis);
                if (SleepConditionVariableCS(&pState->sCondition, &pState->sMutex, DWORD(millis)))
                    return STATUS_OK;

                const DWORD result = GetLastError();
                if (result != ERROR_TIMEOUT)
                    return STATUS_UNKNOWN_ERR;

                millis             -= timeout;
            } while (millis > 0);

            return STATUS_TIMEOUT;
        }


#else
        Condition::Condition()
        {
            pthread_mutexattr_t attr;
            pthread_mutexattr_init(&attr);
            pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&sMutex, &attr);
            pthread_mutexattr_destroy(&attr);

            pthread_condattr_t cond_attr;
            pthread_condattr_init(&cond_attr);
            pthread_cond_init(&sCondition, &cond_attr);
            pthread_condattr_destroy(&cond_attr);
        }

        Condition::~Condition()
        {
            pthread_cond_destroy(&sCondition);
            pthread_mutex_destroy(&sMutex);
        }

        bool Condition::lock() const
        {
            while (true)
            {
                switch (pthread_mutex_lock(&sMutex))
                {
                    case 0: return true;
                    case EBUSY:
                        sched_yield();
                        break;
                    default: return false;
                }
            }
        }

        status_t Condition::wait(system::time_millis_t millis)
        {
            struct timeval now;
            struct timespec deadline;

            // Set-up the fire time
            gettimeofday(&now, NULL);
            deadline.tv_nsec    = now.tv_usec * 1000 + (millis % 1000) * 1000000;
            deadline.tv_sec     = now.tv_sec + (millis / 1000) + (deadline.tv_nsec / 1000000000);
            deadline.tv_nsec   %= 1000000000;

            // Perform wait
            int result          = pthread_cond_timedwait(&sCondition, &sMutex, &deadline);
            switch (result)
            {
                case 0: return STATUS_OK;
                case ETIMEDOUT: return STATUS_TIMED_OUT;
                case EPERM: return STATUS_BAD_STATE;
                default: break;
            }

            return STATUS_UNKNOWN_ERR;
        }

        status_t Condition::wait()
        {
            int result          = pthread_cond_wait(&sCondition, &sMutex);
            switch (result)
            {
                case 0: return STATUS_OK;
                case ETIMEDOUT: return STATUS_TIMED_OUT;
                case EPERM: return STATUS_BAD_STATE;
                default: break;
            }

            return STATUS_UNKNOWN_ERR;
        }

#endif /* PLATFORM_LINUX */

    } /* namespace ipc */
} /* namespace lsp */



