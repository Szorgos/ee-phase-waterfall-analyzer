/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 24 апр. 2024 г.
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

#include <lsp-plug.in/common/atomic.h>
#include <lsp-plug.in/common/debug.h>
#include <lsp-plug.in/io/Path.h>
#include <lsp-plug.in/ipc/SharedMutex.h>
#include <lsp-plug.in/ipc/Thread.h>
#include <lsp-plug.in/runtime/system.h>

#ifdef PLATFORM_WINDOWS
    #include <windows.h>
    #include <synchapi.h>
#else
    #include <errno.h>
    #include <fcntl.h>
    #include <pthread.h>
    #include <sys/file.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif /* PLATFORM_WINDOWS */

namespace lsp
{
    namespace ipc
    {
        #ifdef LSP_ROBUST_MUTEX_SUPPORTED
            static constexpr uint64_t SHMUTEX_INIT_FLAG     = __IF_LEBE(0x786574756D5F6873ULL, 0x73685F6D75746578ULL); // "sh_mutex"

            typedef struct shared_mutex_t
            {
                volatile uint64_t   init_flag;      // Should contain SHMUTEX_INIT_FLAG if mutex has been initialized
                uatomic_t           spin_lock;      // Failover if system does not support flock() on shared memory
                uatomic_t           padding;        // Padding, not used, should be zero
                pthread_mutex_t     mutex;          // Shared mutex data
            } shared_mutex_t;
        #endif /* LSP_ROBUST_MUTEX_SUPPORTED */

        SharedMutex::SharedMutex()
        {
        #ifdef PLATFORM_WINDOWS
            hLock       = NULL;
        #else
            hFD         = -1;

            #ifdef LSP_ROBUST_MUTEX_SUPPORTED
                hLock       = NULL;
            #endif /* LSP_ROBUST_MUTEX_SUPPORTED */
        #endif /* PLATFORM_WINDOWS */

            atomic_store(&nOwner, INVALID_THREAD_ID);
        }

        SharedMutex::~SharedMutex()
        {
            close();
        }

        inline bool SharedMutex::is_opened() const
        {
        #if defined(PLATFORM_WINDOWS) || defined(LSP_ROBUST_MUTEX_SUPPORTED)
            return hLock != NULL;
        #else
            return hFD >= 0;
        #endif /* PLATFORM_WINDOWS, LSP_ROBUST_MUTEX_SUPPORTED */
        }

        status_t SharedMutex::open(const char *name)
        {
            if (is_opened())
               	return STATUS_OPENED;

            LSPString tmp;
        #ifdef PLATFORM_WINDOWS
            if (!tmp.set_utf8(name))
                return STATUS_NO_MEM;
        #else
            if (!tmp.append(FILE_SEPARATOR_C))
                return STATUS_NO_MEM;
            if (!tmp.append_utf8(name))
                return STATUS_NO_MEM;
        #endif /* PLATFORM_WINDOWS */
            return open_internal(&tmp);
        }

        status_t SharedMutex::open(const LSPString *name)
        {
            if (is_opened())
                return STATUS_OPENED;

        #ifdef PLATFORM_WINDOWS
            return open_internal(name);
        #else
            LSPString tmp;

            if (!tmp.append(FILE_SEPARATOR_C))
                return STATUS_NO_MEM;
            if (!tmp.append(name))
                return STATUS_NO_MEM;

            return open_internal(&tmp);
        #endif /* PLATFORM_WINDOWS */
        }

    #ifndef PLATFORM_WINDOWS
        status_t SharedMutex::lock_descriptor(int fd, int flags)
        {
            while (true)
            {
                if (flock(fd, flags) == 0)
                    return STATUS_OK;

                const int error = errno;
                if (error == ENOTSUP)
                    return STATUS_NOT_SUPPORTED;

                switch (error)
                {
                    case EAGAIN: return STATUS_RETRY;
                    case EBADF: return STATUS_IO_ERROR;
                    case EINTR: break;
                    case EINVAL: return STATUS_INVALID_VALUE;
                    case ENOLCK: return STATUS_NO_MEM;
                    default: return STATUS_IO_ERROR;
                }
            }
        }

        #ifdef LSP_ROBUST_MUTEX_SUPPORTED
            status_t SharedMutex::lock_memory(int fd, shared_mutex_t *mutex)
            {
                status_t res = lock_descriptor(fd, LOCK_EX);
                if ((res == STATUS_OK) || (res == STATUS_NOT_SUPPORTED))
                {
                    // Try to acquire spin-lock instead of flock
                    while (atomic_swap(&mutex->spin_lock, 1) != 0)
                        ipc::Thread::yield();

                    res     = STATUS_OK;
                }

                return res;
            }

            status_t SharedMutex::unlock_memory(int fd, shared_mutex_t *mutex)
            {
                status_t res = lock_descriptor(fd, LOCK_UN);
                if ((res == STATUS_OK) || (res == STATUS_NOT_SUPPORTED))
                {
                    // Release spin lock
                    atomic_swap(&mutex->spin_lock, 0);
                    res     = STATUS_OK;
                }

                return res;
            }
        #endif /* LSP_ROBUST_MUTEX_SUPPORTED */

    #endif /* PLATFORM_WINDOWS */

        status_t SharedMutex::open_internal(const LSPString *name)
        {
        #if defined(PLATFORM_WINDOWS)
            const WCHAR *path = name->get_utf16();
            if (path == NULL)
                return STATUS_NO_MEM;

            hLock = CreateMutexW(NULL, FALSE, path);
            return (hLock != NULL) ? STATUS_OK : STATUS_IO_ERROR;
        #elif defined(LSP_ROBUST_MUTEX_SUPPORTED)
            int error;
            const char *path = (name != NULL) ? name->get_native() : NULL;
            if (path == NULL)
                return STATUS_NO_MEM;

            static constexpr int open_mode =
                S_IRUSR | S_IWUSR |
                S_IRGRP | S_IWGRP |
                S_IROTH | S_IWOTH;

            // Create shared memory segment
            int fd = shm_open(path, O_RDWR | O_CREAT, open_mode);
            if (fd < 0)
            {
                error = errno;
                switch (error)
                {
                    case EACCES: return STATUS_PERMISSION_DENIED;
                    case EEXIST: return STATUS_ALREADY_EXISTS;
                    case EINVAL: return STATUS_INVALID_VALUE;
                    case EMFILE: return STATUS_OVERFLOW;
                    case ENFILE: return STATUS_OVERFLOW;
                    case ENAMETOOLONG: return STATUS_TOO_BIG;
                    case ENOENT: return STATUS_NOT_FOUND;
                    case ENOMEM: return STATUS_NO_MEM;
                    default: return STATUS_IO_ERROR;
                }
            }
            lsp_finally {
                if (fd >= 0)
                    ::close(fd);
            };

            // Reserve memory for the mutex
            if (ftruncate(fd, sizeof(shared_mutex_t)) != 0)
            {
                error = errno;
                switch (error)
                {
                    case EACCES: return STATUS_PERMISSION_DENIED;
                    case EFAULT: return STATUS_UNKNOWN_ERR;
                    case EFBIG: return STATUS_TOO_BIG;
                    case EINVAL: return STATUS_INVALID_VALUE;
                    case EINTR: return STATUS_INTERRUPTED;
                    case EIO: return STATUS_IO_ERROR;
                    case EISDIR: return STATUS_IS_DIRECTORY;
                    case EPERM: return STATUS_PERMISSION_DENIED;
                    case EROFS: return STATUS_READONLY;
                    case ETXTBSY: return STATUS_PERMISSION_DENIED;
                    case EBADF: return STATUS_IO_ERROR;
                    default: return STATUS_IO_ERROR;
                }
            }

            // Map the mutex memory
            shared_mutex_t *shmutex     = static_cast<shared_mutex_t *>(mmap(0, sizeof(shared_mutex_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
            if (shmutex == MAP_FAILED)
            {
                error = errno;
                switch (error)
                {
                    case EACCES: return STATUS_PERMISSION_DENIED;
                    case EAGAIN: return STATUS_RETRY;
                    case EPERM: return STATUS_PERMISSION_DENIED;
                    case EFBIG: return STATUS_TOO_BIG;
                    case EEXIST: return STATUS_ALREADY_EXISTS;
                    case ENOMEM: return STATUS_NO_MEM;
                    case EOVERFLOW: return STATUS_OVERFLOW;
                    default: return STATUS_IO_ERROR;
                }
            }
            lsp_finally {
                if (shmutex != NULL)
                    munmap(shmutex, sizeof(shared_mutex_t));
            };

            // Ensure that mutex is initialized
            if (shmutex->init_flag != SHMUTEX_INIT_FLAG)
            {
                // Lock the shared memory segment to initialize mutex atomically
                status_t lock_res = lock_memory(fd, shmutex);
                if (lock_res != STATUS_OK)
                    return lock_res;

                // Unlock file descriptor on exit out of the scope
                bool need_unlock = true;
                lsp_finally {
                    if (need_unlock)
                        unlock_memory(fd, shmutex);
                };

                if (shmutex->init_flag != SHMUTEX_INIT_FLAG)
                {
                    // Mutex is not initialized, initialize it
                    pthread_mutexattr_t attr;
                    if ((error = pthread_mutexattr_init(&attr)) != 0)
                        return STATUS_UNKNOWN_ERR;

                    if ((error = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED)) != 0)
                    {
                        switch (error)
                        {
                            case EINVAL: return STATUS_INVALID_VALUE;
                            case ENOTSUP: return STATUS_NOT_IMPLEMENTED;
                            default: return STATUS_UNKNOWN_ERR;
                        }
                    }
                    if ((error = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST)) != 0)
                        return STATUS_UNKNOWN_ERR;
                    if ((error = pthread_mutex_init(&shmutex->mutex, &attr)) != 0)
                        return STATUS_UNKNOWN_ERR;

                    // Mark the mutex being initialized
                    shmutex->init_flag  = SHMUTEX_INIT_FLAG;
                }

                // Unlock memory
                need_unlock     = false;
                lock_res        = unlock_memory(fd, shmutex);
                if (lock_res != STATUS_OK)
                    return lock_res;
            }

            // Now we have shared mutex and file descriptor, store them
            hFD     = fd;
            hLock   = release_ptr(shmutex);

            fd      = -1;

            return STATUS_OK;
        #else
            // Open lock file
            io::Path tmp;
            status_t res = system::get_system_temporary_dir(&tmp);
            if (res != STATUS_OK)
                return res;

            // Use separate directory to not to confuse with another applications' files
            if ((res = tmp.append_child("shmutex")) != STATUS_OK)
                return res;

            if ((res = tmp.mkdir()) != STATUS_OK)
            {
                if (res != STATUS_ALREADY_EXISTS)
                    return res;
            }

            // Try to open or create file
            if ((res = tmp.append(name)) != STATUS_OK)
                return res;

            int fd = ::open(tmp.as_native(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
            if (fd < 0)
            {
                int code = errno;
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

                return res;
            }
            lsp_finally {
                if (fd >= 0)
                    ::close(fd);
            };

            // Now we have shared mutex and file descriptor, store them
            hFD     = fd;
            fd      = -1;

            return STATUS_OK;
        #endif /* PLATFORM_WINDOWS, LSP_ROBUST_MUTEX_SUPPORTED */
        }

        status_t SharedMutex::close()
        {
            if (!is_opened())
                return STATUS_OK;

            status_t res = STATUS_OK;

        #ifdef PLATFORM_WINDOWS
            // Release lock if it is set
            if (atomic_swap(&nOwner, INVALID_THREAD_ID) != INVALID_THREAD_ID)
            {
                if (!ReleaseMutex(hLock))
                    res     = update_status(res, STATUS_IO_ERROR);
            }

            if (!CloseHandle(hLock))
                res     = update_status(res, STATUS_IO_ERROR);
            hLock = NULL;
        #elif defined(LSP_ROBUST_MUTEX_SUPPORTED)
            // Release lock if it is set
            if (atomic_swap(&nOwner, INVALID_THREAD_ID) != INVALID_THREAD_ID)
            {
                int error = pthread_mutex_unlock(&hLock->mutex);
                if (error != 0)
                    res     = update_status(res, STATUS_IO_ERROR);
            }

            // Unmap memory
            munmap(hLock, sizeof(shared_mutex_t));
            hLock = NULL;

            // Close file descriptor
            if (hFD >= 0)
            {
                ::close(hFD);
                hFD     = -1;
            }
        #else
            // Release lock if it is set
            if (atomic_swap(&nOwner, INVALID_THREAD_ID) != INVALID_THREAD_ID)
                lock_descriptor(hFD, LOCK_UN);

            // Close file descriptor
            ::close(hFD);
            hFD     = -1;
        #endif /* PLATFORM_WINDOWS, LSP_ROBUST_MUTEX_SUPPORTED */

            return res;
        }

        status_t SharedMutex::lock()
        {
            if (!is_opened())
                return STATUS_CLOSED;

            // Check that we don't lock the mutex again
            const thread_id_t tid = Thread::current_thread_id();
            if (atomic_load(&nOwner) == tid)
                return STATUS_LOCKED;

        #if defined(PLATFORM_WINDOWS)
            DWORD res = WaitForSingleObject(hLock, INFINITE);
            switch (res)
            {
                case WAIT_OBJECT_0:
                case WAIT_ABANDONED:
                    atomic_store(&nOwner, tid);
                    return STATUS_OK;
                case WAIT_TIMEOUT:
                    return STATUS_TIMED_OUT;
                case WAIT_FAILED:
                    return STATUS_UNKNOWN_ERR;
                default:
                    break;
            }

            return STATUS_UNKNOWN_ERR;
        #elif defined(LSP_ROBUST_MUTEX_SUPPORTED)
            int error = pthread_mutex_lock(&hLock->mutex);
            switch (error)
            {
                case 0: break;
                case EDEADLK: return STATUS_BAD_STATE;
                case EBUSY: return STATUS_LOCKED;
                case EOWNERDEAD:
                {
                    pthread_mutex_consistent(&hLock->mutex);
                    break;
                }
                default: return STATUS_UNKNOWN_ERR;
            }

            atomic_store(&nOwner, tid);

            return STATUS_OK;
        #else
            status_t res = lock_descriptor(hFD, LOCK_EX);
            if (res != STATUS_OK)
                return res;

            // Now we need to spin to ensure that our thread owns the lock
            while (true)
            {
                if (atomic_cas(&nOwner, INVALID_THREAD_ID, tid))
                    return STATUS_OK;

                // Wait for a while
                ipc::Thread::sleep(1);
            }

            return res;
        #endif /* PLATFORM_WINDOWS, LSP_ROBUST_MUTEX_SUPPORTED */
        }

        status_t SharedMutex::lock(system::time_millis_t delay)
        {
            if (!is_opened())
                return STATUS_CLOSED;

            // Check that we don't lock the mutex again
            const thread_id_t tid = Thread::current_thread_id();
            if (atomic_load(&nOwner) == tid)
                return STATUS_LOCKED;

        #ifdef PLATFORM_WINDOWS
            DWORD res = WaitForSingleObject(hLock, delay);
            switch (res)
            {
                case WAIT_OBJECT_0:
                case WAIT_ABANDONED:
                    atomic_store(&nOwner, tid);
                    return STATUS_OK;
                case WAIT_TIMEOUT:
                    return STATUS_TIMED_OUT;
                case WAIT_FAILED:
                    return STATUS_UNKNOWN_ERR;
                default:
                    break;
            }

            return STATUS_UNKNOWN_ERR;
        #elif defined(LSP_ROBUST_MUTEX_SUPPORTED)
            // sem_timedwait() is the same as sem_wait(), except that abs_timeout specifies a limit on the
            // amount of time that the call should block if the decrement cannot be immediately performed.
            // The abs_timeout argument points to a structure that specifies an absolute timeout in seconds
            // and nanoseconds since the Epoch, 1970-01-01 00:00:00 +0000 (UTC).
            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);

            timeout.tv_sec  += delay / 1000;
            timeout.tv_nsec += (delay % 1000) * 1000000;
            if (timeout.tv_nsec >= 1000000000)
            {
                timeout.tv_sec     += 1;
                timeout.tv_nsec    -= 1000000000;
            }

            int error = pthread_mutex_timedlock(&hLock->mutex, &timeout);
            switch (error)
            {
                case 0: break;
                case EDEADLK: return STATUS_BAD_STATE;
                case EBUSY:
                    return STATUS_LOCKED;
                case ETIMEDOUT: return STATUS_TIMED_OUT;
                case EOWNERDEAD:
                {
                    pthread_mutex_consistent(&hLock->mutex);
                    break;
                }
                default: return STATUS_UNKNOWN_ERR;
            }

            atomic_store(&nOwner, tid);

            return STATUS_OK;
        #else
            // Since we can not lock file without using signals, we need to simulate the timed wait with a loop
            const system::time_millis_t deadline = system::get_time_millis() + delay;
            while (true)
            {
                status_t res = lock_descriptor(hFD, LOCK_EX | LOCK_NB);
                if (res == STATUS_OK)
                    break;

                // Check that we didn't reach deadline
                const system::time_millis_t ctime = system::get_time_millis();
                if (ctime >= deadline)
                    return STATUS_TIMED_OUT;

                // Wait for a while
                ipc::Thread::yield();
            }

            // Now we need to spin to ensure that our thread owns the lock
            while (true)
            {
                if (atomic_cas(&nOwner, 0, tid))
                    return STATUS_OK;

                // Check that we didn't reach deadline
                const system::time_millis_t ctime = system::get_time_millis();
                if (ctime >= deadline)
                {
                    lock_descriptor(hFD, LOCK_UN);
                    return STATUS_TIMED_OUT;
                }

                // Wait for a while
                ipc::Thread::sleep(1);
            }
        #endif /* PLATFORM_WINDOWS, LSP_ROBUST_MUTEX_SUPPORTED */
        }

        status_t SharedMutex::try_lock()
        {
            if (!is_opened())
                return STATUS_CLOSED;

            // Check that we don't lock the mutex again
            const thread_id_t tid = Thread::current_thread_id();
            if (atomic_load(&nOwner) == tid)
                return STATUS_LOCKED;

        #ifdef PLATFORM_WINDOWS
            DWORD res = WaitForSingleObject(hLock, 0);
            switch (res)
            {
                case WAIT_OBJECT_0:
                case WAIT_ABANDONED:
                    atomic_store(&nOwner, tid);
                    return STATUS_OK;
                case WAIT_TIMEOUT:
                    return STATUS_RETRY;
                case WAIT_FAILED:
                    return STATUS_UNKNOWN_ERR;
                default:
                    break;
            }

            return STATUS_UNKNOWN_ERR;
        #elif defined(LSP_ROBUST_MUTEX_SUPPORTED)
            int error = pthread_mutex_trylock(&hLock->mutex);
            switch (error)
            {
                case 0: break;
                case EDEADLK: return STATUS_BAD_STATE;
                case EBUSY: return STATUS_RETRY;
                case EOWNERDEAD:
                {
                    pthread_mutex_consistent(&hLock->mutex);
                    break;
                }
                default: return STATUS_UNKNOWN_ERR;
            }

            atomic_store(&nOwner, tid);

            return STATUS_OK;
        #else
            status_t res = lock_descriptor(hFD, LOCK_EX | LOCK_NB);
            if (res != STATUS_OK)
                return res;

            // Now we need to lock the spin
            if (atomic_cas(&nOwner, INVALID_THREAD_ID, tid))
                return STATUS_OK;

            // Unlock descriptor
            lock_descriptor(hFD, LOCK_UN);
            return STATUS_RETRY;
        #endif /* PLATFORM_WINDOWS, LSP_ROBUST_MUTEX_SUPPORTED */
        }

        status_t SharedMutex::unlock()
        {
            if (!is_opened())
                return STATUS_CLOSED;

            // Check that we own the lock the mutex
            const thread_id_t tid = Thread::current_thread_id();
            if (atomic_load(&nOwner) != tid)
                return STATUS_BAD_STATE;

        #ifdef PLATFORM_WINDOWS
            if (!ReleaseMutex(hLock))
                return STATUS_UNKNOWN_ERR;
        #elif defined(LSP_ROBUST_MUTEX_SUPPORTED)
            int error   = pthread_mutex_unlock(&hLock->mutex);
            switch (error)
            {
                case 0: break;
                case EDEADLK: return STATUS_BAD_STATE;
                case EBUSY: return STATUS_RETRY;
                case EPERM: return STATUS_PERMISSION_DENIED;
                default: return STATUS_UNKNOWN_ERR;
            }
        #else
            status_t res    = lock_descriptor(hFD, LOCK_UN);
            if (res != STATUS_OK)
                return res;
        #endif /* PLATFORM_WINDOWS, LSP_ROBUST_MUTEX_SUPPORTED */

            atomic_store(&nOwner, INVALID_THREAD_ID);
            return STATUS_OK;
        }

    } /* namespace ipc */
} /* namespace lsp */


