/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 27 янв. 2016 г.
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
#include <lsp-plug.in/ipc/NativeExecutor.h>

namespace lsp
{
    namespace ipc
    {
        static constexpr size_t POLL_INTERVAL = 20;

        NativeExecutor::NativeExecutor():
            hThread(execute, this)
        {
            // Initialize list
            pHead       = NULL;
            pTail       = NULL;
            atomic_init(nLock);
        }

        NativeExecutor::~NativeExecutor()
        {
        }

        status_t NativeExecutor::start()
        {
            return hThread.start();
        }

        bool NativeExecutor::submit(ITask *task)
        {
            lsp_trace("submit task=%p", task);

            // Update task state to SUBMITTED
            if (!change_task_state(task, ITask::TS_IDLE, ITask::TS_SUBMITTED))
                return false;

            // Try to acquire critical section
            if (!atomic_trylock(nLock))
            {
                set_task_state(task, ITask::TS_IDLE);
                return false;
            }
            lsp_finally { atomic_unlock(nLock); };

            // Critical section acquired, bind new task
            // Check that queue is empty
            if (pTail != NULL)
                link_task(pTail, task);
            else
                pHead   = task;
            pTail   = task;

            return true;
        }

        void NativeExecutor::shutdown()
        {
            lsp_trace("start shutdown");

            // Wait until the queue is empty
            while (true)
            {
                // Try to acquire critical section
                if (atomic_trylock(nLock))
                {
                    // Check that queue is empty
                    if (pHead == NULL)
                        break;
                    // Release critical section
                    atomic_unlock(nLock);
                }

                ipc::Thread::sleep(POLL_INTERVAL);
            }

            // Now there are no pending tasks, terminate thread
            hThread.cancel();
            hThread.join();

            lsp_trace("shutdown complete");
        }

        void NativeExecutor::run()
        {
            while (!ipc::Thread::is_cancelled())
            {
                // Sleep until critical section is acquired
                while (!atomic_trylock(nLock))
                {
                    if (ipc::Thread::sleep(POLL_INTERVAL) == STATUS_CANCELLED)
                        return;
                }

                // Try to get task
                ITask  *task    = pHead;
                if (task == NULL)
                {
                    // Release critical section
                    atomic_unlock(nLock);

                    // Wait for a while
                    if (ipc::Thread::sleep(POLL_INTERVAL) == STATUS_CANCELLED)
                        return;
                }
                else
                {
                    // Remove task from queue
                    pHead           = unlink_task(pHead);
                    if (pHead == NULL)
                        pTail           = NULL;

                    // Release critical section
                    atomic_unlock(nLock);

                    // Execute task
                #ifdef LSP_TRACE
                    lsp_trace("executing task %p", task);
                    const system::time_millis_t start = system::get_time_millis();
                #endif /* LSP_TRACE */

                    run_task(task);

                #ifdef LSP_TRACE
                    const system::time_millis_t end = system::get_time_millis();
                    lsp_trace("executed task %p with code %d, time=%d ms",
                        task, int(task->code()), int(end - start));
                #endif /* LSP_TRACE */
                }
            }
        }

        status_t NativeExecutor::execute(void *params)
        {
            NativeExecutor *self = reinterpret_cast<NativeExecutor *>(params);
            self->run();
            return STATUS_OK;
        }

    } /* namespace ipc */
} /* namespace lsp */
