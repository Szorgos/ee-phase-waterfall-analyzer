/*
 * Copyright (C) 2026 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2026 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-lltl-lib
 * Created on: 8 апр. 2020 г.
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

#include <lsp-plug.in/lltl/parray.h>
#include <lsp-plug.in/stdlib/stdlib.h>

namespace lsp
{
    namespace lltl
    {
        inline size_t nonzero(size_t count, size_t n) { return ((count + n) > 0) ? n : 1; }

        const iter_vtbl_t raw_parray::iterator_vtbl =
        {
            iter_move,
            iter_get,
            iter_compare,
            iter_compare,
            iter_count
        };

        void raw_parray::init()
        {
            nItems      = 0;
            vItems      = NULL;
            nCapacity   = 0;
        }

        bool raw_parray::grow(size_t capacity)
        {
            if (capacity < 32)
                capacity        = 32;

            // Do aligned (re)allocation
            void **ptr      = reinterpret_cast<void **>(::realloc(vItems, sizeof(void *) * capacity));
            if (ptr == NULL)
                return false;

            // Update pointer and capacity
            vItems          = ptr;
            nCapacity       = capacity;
            return true;
        }

        bool raw_parray::truncate(size_t capacity)
        {
            if (capacity < 32)
            {
                if (capacity == 0)
                {
                    flush();
                    return true;
                }
                capacity        = 32;
            }
            if (nCapacity <= capacity)
                return true;

            // Do aligned (re)allocation
            void **ptr      = reinterpret_cast<void **>(::realloc(vItems, sizeof(void *) * capacity));
            if (ptr == NULL)
                return false;

            // Update pointer, capacity and size
            vItems          = ptr;
            nCapacity       = capacity;
            if (nItems > capacity)
                nItems          = capacity;
            return true;
        }

        void raw_parray::flush()
        {
            if (vItems != NULL)
            {
                ::free(vItems);
                vItems      = NULL;
            }
            nCapacity   = 0;
            nItems      = 0;
        }

        void raw_parray::swap(raw_parray *src)
        {
            raw_parray tmp = *this;
            *this   = *src;
            *src    = tmp;
        }

        bool raw_parray::xswap(size_t i1, size_t i2)
        {
            if ((i1 >= nItems) || (i2 >= nItems))
                return false;
            if (i1 != i2)
            {
                void *tmp   = vItems[i1];
                vItems[i1]  = vItems[i2];
                vItems[i2]  = tmp;
            }
            return true;
        }

        ssize_t raw_parray::index_of(const void *ptr)
        {
            for (size_t i=0; i<nItems; ++i)
                if (vItems[i] == ptr)
                    return i;
            return -1;
        }

        void **raw_parray::slice(size_t idx, size_t size)
        {
            if (size <= 0)
                return NULL;

            size_t tail = idx + size;
            return (tail <= nItems) ? &vItems[idx] : NULL;
        }

        void **raw_parray::get_n(size_t idx, size_t size, void **dst)
        {
            size_t tail = idx + size;
            if (tail > nItems)
                return NULL;

            ::memcpy(dst, &vItems[idx], size * sizeof(void *));
            return dst;
        }

        void **raw_parray::set(size_t n, void **src)
        {
            size_t count    = (n > 0) ? n : 1;
            if (count > nCapacity)
            {
                if (!grow(count))
                    return NULL;
            }
            else if (count < (nCapacity >> 1))
            {
                if (!truncate(count))
                    return NULL;
            }

            ::memcpy(vItems, src, n * sizeof(void *));
            nItems          = n;
            return vItems;
        }

        void **raw_parray::iset(size_t idx, size_t n, void **src)
        {
            if ((idx + n) > nItems)
                return NULL;

            void **dst      = &vItems[idx];
            ::memcpy(dst, src, n * sizeof(void *));
            return dst;
        }

        void **raw_parray::append(size_t n)
        {
            size_t count    = nonzero(nItems, n);
            size_t size     = nItems + count;
            if (size > nCapacity)
            {
                size_t dn = nCapacity + count;
                if (!grow(dn + (dn >> 1)))
                    return NULL;
            }

            void **ptr      = &vItems[nItems];
            nItems         += n;
            return ptr;
        }

        void **raw_parray::append(void *ptr)
        {
            size_t size     = nItems + 1;
            if (size > nCapacity)
            {
                size_t dn = nCapacity + 1;
                if (!grow(dn + (dn >> 1)))
                    return NULL;
            }

            void **res      = &vItems[nItems];
            nItems          = size;
            *res            = ptr;
            return res;
        }

        void **raw_parray::append(size_t n, void **src)
        {
            size_t count    = nonzero(nItems, n);
            size_t size     = nItems + count;
            if (size > nCapacity)
            {
                size_t dn = nCapacity + count;
                if (!grow(dn + (dn >> 1)))
                    return NULL;
            }

            void **res      = &vItems[nItems];
            nItems         += n;
            ::memcpy(res, src, n * sizeof(void *));
            return res;
        }

        void **raw_parray::insert(size_t index, size_t n)
        {
            if ((index < 0) || (index > nItems))
                return NULL;

            size_t count    = nonzero(nItems, n);
            size_t size     = nItems + count;
            if (size > nCapacity)
            {
                size_t dn = nCapacity + count;
                if (!grow(dn + (dn >> 1)))
                    return NULL;
            }
            void **res      = &vItems[index];
            if (index < nItems)
                ::memmove(&res[n], res, (nItems - index) * sizeof(void *));
            nItems         += n;
            return res;
        }

        void **raw_parray::insert(size_t index, void *ptr)
        {
            if ((index < 0) || (index > nItems))
                return NULL;
            if ((nItems + 1) > nCapacity)
            {
                size_t dn = nCapacity + 1;
                if (!grow(dn + (dn >> 1)))
                    return NULL;
            }

            void **res = &vItems[index];
            if (index < nItems)
                ::memmove(&res[1], res, (nItems - index) * sizeof(void *));

            nItems          ++;
            *res            = ptr;
            return res;
        }

        void **raw_parray::insert(size_t index, size_t n, void **src)
        {
            if ((index < 0) || (index > nItems))
                return NULL;

            size_t count    = nonzero(nItems, n);
            size_t size     = nItems + count;
            if (size > nCapacity)
            {
                size_t dn = nCapacity + count;
                if (!grow(dn + (dn >> 1)))
                    return NULL;
            }

            void **res = &vItems[index];
            if (index < nItems)
                ::memmove(&res[n], res, (nItems - index) * sizeof(void *));

            nItems         += n;
            ::memcpy(res, src, n * sizeof(void *));
            return res;
        }

        void *raw_parray::pop()
        {
            return (nItems > 0) ? vItems[--nItems] : NULL;
        }

        void **raw_parray::pop(void **dst)
        {
            if (nItems <= 0)
                return NULL;
            *dst = vItems[--nItems];
            return dst;
        }

        void **raw_parray::pop(size_t n)
        {
            return (nItems >= n) ? &vItems[nItems -= n] : NULL;
        }

        void **raw_parray::pop(size_t n, void **dst)
        {
            if (nItems < n)
                return NULL;
            nItems -= n;
            ::memcpy(dst, &vItems[nItems], n * sizeof(void *));
            return dst;
        }

        void **raw_parray::pop(size_t n, raw_parray *cs)
        {
            if (nItems < n)
                return NULL;

            size_t size = nItems - n;
            void **res = cs->append(n, &vItems[size]);
            if (res)
                nItems = size;
            return res;
        }

        void *raw_parray::premove(const void *ptr)
        {
            ssize_t idx = index_of(ptr);
            if (idx < 0)
                return NULL;
            size_t tail = idx + 1;
            if (tail < nItems)
                ::memmove(&vItems[idx], &vItems[tail], (nItems - tail) * sizeof(void *));
            nItems     -= 1;
            return const_cast<void *>(ptr);
        }

        bool raw_parray::premove(const void *ptr, size_t n)
        {
            ssize_t idx = index_of(ptr);
            if (idx < 0)
                return false;
            size_t tail = idx + n;
            if (tail > nItems)
                return false;

            if (tail < nItems)
                ::memmove(&vItems[idx], &vItems[tail], (nItems - tail) * sizeof(void *));
            nItems     -= n;
            return true;
        }

        void **raw_parray::premove(const void *ptr, size_t n, void **dst)
        {
            ssize_t idx = index_of(ptr);
            if (idx < 0)
                return NULL;
            size_t tail = idx + n;
            if (tail > nItems)
                return NULL;

            ::memcpy(dst, &vItems[idx], n * sizeof(void *));
            if (tail < nItems)
                ::memmove(&vItems[idx], &vItems[tail], (nItems - tail) * sizeof(void *));
            nItems     -= n;
            return dst;
        }

        void **raw_parray::premove(const void *ptr, size_t n, raw_parray *cs)
        {
            ssize_t idx = index_of(ptr);
            if (idx < 0)
                return NULL;
            size_t tail = idx + n;
            if (tail > nItems)
                return NULL;

            void **res = cs->append(n, &vItems[idx]);
            if (res)
            {
                if (tail < nItems)
                    ::memmove(&vItems[idx], &vItems[tail], (nItems - tail) * sizeof(void *));
                nItems     -= n;
            }
            return res;
        }

        void *raw_parray::iremove(size_t idx)
        {
            size_t last = idx + 1;
            if (last > nItems)
                return NULL;
            void *res   = vItems[idx];
            if (last < nItems)
                ::memmove(&vItems[idx], &vItems[last], (nItems - last) * sizeof(void *));
            nItems     -= 1;
            return res;
        }

        void *raw_parray::qremove(size_t idx)
        {
            if (idx >= nItems)
                return NULL;
            void *res   = vItems[idx];
            size_t size = nItems - 1;
            if (idx < size)
                vItems[idx] = vItems[size];
            nItems      = size;
            return res;
        }

        void *raw_parray::qpremove(const void *ptr)
        {
            ssize_t idx = index_of(ptr);
            if (idx < 0)
                return NULL;

            void *res   = vItems[idx];
            ssize_t size= nItems - 1;
            if (idx < size)
                vItems[idx] = vItems[size];
            nItems      = size;
            return res;
        }

        bool raw_parray::iremove(size_t idx, size_t n)
        {
            size_t last = idx + n;
            if (last > nItems)
                return false;
            if (last < nItems)
                ::memmove(&vItems[idx], &vItems[last], (nItems - last) * sizeof(void *));
            nItems     -= n;
            return true;
        }

        void **raw_parray::iremove(size_t idx, size_t n, void **dst)
        {
            size_t last = idx + n;
            if (last > nItems)
                return NULL;
            ::memcpy(dst, &vItems[idx], n * sizeof(void *));
            if (last < nItems)
                ::memmove(&vItems[idx], &vItems[last], (nItems - last) * sizeof(void *));
            nItems     -= n;
            return dst;
        }

        void **raw_parray::iremove(size_t idx, size_t n, raw_parray *cs)
        {
            size_t last = idx + n;
            if (last > nItems)
                return NULL;

            void **res = cs->append(n, &vItems[idx]);
            if (res)
            {
                if (last < nItems)
                    ::memmove(&vItems[idx], &vItems[last], (nItems - last) * sizeof(void *));
                nItems     -= n;
            }
            return res;
        }

        int raw_parray::closure_cmp(const void *a, const void *b, void *c)
        {
            sort_closure_t *sc = static_cast<sort_closure_t *>(c);
            const void * const *_a = static_cast<const void * const *>(a);
            const void * const *_b = static_cast<const void * const *>(b);
            ssize_t res = sc->compare(*_a, *_b, sc->size);
            return (res > 0) ? 1 : (res < 0) ? -1 : 0;
        }

        int raw_parray::raw_cmp(const void *a, const void *b, void *c)
        {
            cmp_func_t f = reinterpret_cast<cmp_func_t>(c);
            const void * const *_a = static_cast<const void * const *>(a);
            const void * const *_b = static_cast<const void * const *>(b);
            ssize_t res = f(*_a, *_b);
            return (res > 0) ? 1 : (res < 0) ? -1 : 0;
        }

        void raw_parray::qsort(sort_closure_t *c)
        {
            lsp::qsort_r(vItems, nItems, sizeof(void *), closure_cmp, c);
        }

        void raw_parray::qsort(cmp_func_t f)
        {
            union
            {
                cmp_func_t f;
                void *p;
            } xf;
            xf.f = f;
            lsp::qsort_r(vItems, nItems, sizeof(void *), raw_cmp, xf.p);
        }

        void raw_parray::ssort(sort_closure_t *c)
        {
            lsp::ssort_r(vItems, nItems, sizeof(void *), closure_cmp, c);
        }

        void raw_parray::ssort(cmp_func_t f)
        {
            union
            {
                cmp_func_t f;
                void *p;
            } xf;
            xf.f = f;
            lsp::ssort_r(vItems, nItems, sizeof(void *), raw_cmp, xf.p);
        }

        raw_iterator raw_parray::iter()
        {
            if (nItems <= 0)
                return raw_iterator::INVALID;

            return raw_iterator {
                &iterator_vtbl,
                this,
                NULL,
                0,
                0,
                0,
                false
            };
        }

        raw_iterator raw_parray::riter()
        {
            if (nItems <= 0)
                return raw_iterator::INVALID;

            return raw_iterator {
                &iterator_vtbl,
                this,
                NULL,
                nItems - 1,
                0,
                0,
                true
            };
        }

        void raw_parray::iter_move(raw_iterator *i, ssize_t n)
        {
            raw_parray *self    = static_cast<raw_parray *>(i->container);
            ssize_t off = i->index + n;
            if ((off >= 0) && (size_t(off) < self->nItems))
                i->index        = off;
            else
                *i              = raw_iterator::INVALID;
        }

        void *raw_parray::iter_get(raw_iterator *i)
        {
            raw_parray *self = static_cast<raw_parray *>(i->container);
            return (i->index < self->nItems) ? self->vItems[i->index] : NULL;
        }

        ssize_t raw_parray::iter_compare(const raw_iterator *a, const raw_iterator *b)
        {
            return a->index - b->index;
        }

        size_t raw_parray::iter_count(const raw_iterator *i)
        {
            raw_parray *self    = static_cast<raw_parray *>(i->container);
            return self->nItems;
        }

    } /* namespace lltl */
} /* namespace lsp */


