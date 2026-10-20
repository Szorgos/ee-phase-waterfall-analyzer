/*
 * Copyright (C) 2020 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2020 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-lltl-lib
 * Created on: 17 авг. 2020 г.
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

#include <lsp-plug.in/lltl/iterator.h>

namespace lsp
{
    namespace lltl
    {
        void invalid_iter_move(raw_iterator *i, ssize_t n)
        {
        }

        void *invalid_iter_get(raw_iterator *i)
        {
            return NULL;
        }

        ssize_t invalid_iter_compare(const raw_iterator *a, const raw_iterator *b)
        {
            return (a->container != NULL) || (a->container != b->container);
        }

        ssize_t invalid_iter_diff(const raw_iterator *a, const raw_iterator *b)
        {
            return 0;
        }

        size_t invalid_iter_count(const raw_iterator *i)
        {
            return 0;
        }

        const iter_vtbl_t raw_iterator::invalid_vtbl =
        {
            invalid_iter_move,
            invalid_iter_get,
            invalid_iter_compare,
            invalid_iter_diff,
            invalid_iter_count
        };

        const raw_iterator raw_iterator::INVALID =
        {
            &invalid_vtbl,
            NULL,
            NULL,
            0,
            0,
            0,
            false
        };

        void raw_iterator::advance(ssize_t n)
        {
            if (reversive)
                n       = -n;
            vtable->move(this, n);
        }

        iter_cmp_result_t raw_iterator::compare_to(const raw_iterator *b) const
        {
            // Do usual stuff if container matches
            ssize_t res;
            if (container == b->container)
                res = vtable->cmp(this, b);
            else if (b->container == NULL) // Compare with invalid
                res = container != NULL;
            else
                return iter_cmp_result_t { 0, false };

            return (reversive) ?
                iter_cmp_result_t { -res, true } :
                iter_cmp_result_t {  res, true };
        }

        ssize_t raw_iterator::diff(const raw_iterator *b) const
        {
            if (container == b->container)
                return vtable->diff(this, b);

            if (container == NULL)
                return (b->container != NULL) ? b->remaining() : 0;
            return (b->container == NULL) ? remaining() : 0;
        }

        size_t raw_iterator::remaining() const
        {
            return (reversive) ? index + 1 : vtable->count(this) - index;
        }

        size_t raw_iterator::max_advance() const
        {
            if (container == NULL)
                return 0;
            return (reversive) ? index : vtable->count(this) - index - 1;
        }

        bool raw_iterator::valid() const
        {
            return container != NULL;
        }

    } /* namespace lltl */
} /* namespace lsp */
