/*
 * Copyright (C) 2024 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2024 Vladimir Sadovnikov <sadko4u@gmail.com>
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

#include <lsp-plug.in/common/types.h>
#include <lsp-plug.in/ipc/IExecutor.h>

namespace lsp
{
    namespace ipc
    {
        IExecutor::IExecutor()
        {
        }

        IExecutor::~IExecutor()
        {
        }

        void IExecutor::run_task(ITask *task)
        {
            atomic_store(&task->nState, ITask::TS_RUNNING);
            task->nCode     = 0;

            task->nCode     = task->run();

            atomic_store(&task->nState, ITask::TS_COMPLETED);

            // Run callback method
            task_finished(task);
        }

        void IExecutor::task_finished(ITask *task)
        {
            // Call nested executor (if any)
            if (task->pExecutor != NULL)
                task->pExecutor->task_finished(task);
        }

        bool IExecutor::submit(ITask *task)
        {
            return false;
        }

        void IExecutor::shutdown()
        {
        }
    } /* namespace lsp */
} /* namespace lsp */
