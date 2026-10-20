/*
 * Copyright (C) 2024 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2024 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-lltl-lib
 * Created on: 19 июн. 2024 г.
 *
 * lsp-lltl-lib is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * lsp-lltl-lib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with lsp-lltl-lib. If not, see <https://www.gnu.org/licenses/>.
 */

#include <lsp-plug.in/lltl/state.h>
#include <lsp-plug.in/common/atomic.h>

namespace lsp
{
    namespace lltl
    {
        void raw_state::init()
        {
            init_bins();
            deleter2    = NULL;
            deleter1    = NULL;
        }

        void raw_state::init(deleter1_t deleter)
        {
            init_bins();
            set_deleter(deleter);
        }

        void raw_state::init(deleter2_t deleter, void *arg)
        {
            init_bins();
            set_deleter(deleter, arg);
        }

        void raw_state::set_deleter(deleter1_t deleter)
        {
            deleter2    = NULL;
            deleter1    = deleter;
        }

        void raw_state::set_deleter(deleter2_t deleter, void *arg)
        {
            deleter2    = deleter;
            params      = arg;
        }

        void raw_state::init_bins()
        {
            for (size_t i=0; i<B_TOTAL; ++i)
                bins[i]     = NULL;
        }

        void raw_state::cleanup(void *garbage)
        {
            if (garbage == NULL)
                return;

            if (deleter2 != NULL)
                deleter2(garbage, params);
            else if (deleter1 != NULL)
                deleter1(garbage);
        }

        void raw_state::clear()
        {
            for (size_t i=0; i<B_TOTAL; ++i)
            {
                void *ptr = atomic_swap(&bins[i], NULL);
                cleanup(ptr);
            }
        }

        void raw_state::destroy()
        {
            clear();

            deleter2    = NULL;
            deleter1    = NULL;
        }

        void raw_state::gc()
        {
            // Cleanup garbage
            void *garbage = atomic_swap(&bins[B_GARBAGE], NULL);
            cleanup(garbage);
        }

        void raw_state::push(void *new_state)
        {
            // Cleanup garbage
            void *garbage = atomic_swap(&bins[B_GARBAGE], NULL);
            cleanup(garbage);

            // Replace pending state
            garbage = atomic_swap(&bins[B_PENDING], new_state);
            cleanup(garbage);
        }

        void *raw_state::pull()
        {
            // Cleanup garbage
            void *garbage = atomic_swap(&bins[B_GARBAGE], NULL);
            cleanup(garbage);

            // Ensure that state didn't change
            void *new_state = atomic_swap(&bins[B_PENDING], NULL);
            if (new_state == NULL)
                return atomic_load(&bins[B_STATE]);

            // Update state
            garbage = atomic_swap(&bins[B_STATE], new_state);
            cleanup(garbage);

            return new_state;
        }

        void *raw_state::get()
        {
            // Do not update state if garbage is not clean
            if (atomic_load(&bins[B_GARBAGE]) != NULL)
                return atomic_load(&bins[B_STATE]);

            // Read pending state
            void *new_state = atomic_swap(&bins[B_PENDING], NULL);
            if (new_state == NULL)
                return atomic_load(&bins[B_STATE]);

            // Replace old state with new one
            void *garbage   = atomic_swap(&bins[B_STATE], new_state);
            if (garbage == NULL)
                return new_state;

            // Push garbage to queue
            atomic_swap(&bins[B_GARBAGE], garbage);
            return new_state;
        }

        bool raw_state::set(void *new_state)
        {
            // Do not update state if garbage is not clean
            if (atomic_load(&bins[B_GARBAGE]) != NULL)
                return false;

            // Read pending state
            void *garbage = atomic_swap(&bins[B_PENDING], new_state);
            if (garbage == NULL)
                return true;

            // Push garbage to queue
            atomic_swap(&bins[B_GARBAGE], garbage);
            return true;
        }

        void *raw_state::current() const
        {
            return atomic_load(&bins[B_STATE]);
        }

        bool raw_state::pending() const
        {
            return atomic_load(&bins[B_PENDING]) != NULL;
        }

    } /* namespace lltl */
} /* namespace lsp */
