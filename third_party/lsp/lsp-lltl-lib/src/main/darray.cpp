/*
 * Copyright (C) 2026 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2026 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-lltl-lib
 * Created on: 7 апр. 2020 г.
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

#include <lsp-plug.in/lltl/darray.h>
#include <lsp-plug.in/stdlib/stdlib.h>

namespace lsp
{
    namespace lltl
    {
        inline size_t nonzero(size_t count, size_t n) { return ((count + n) > 0) ? n : 1; }

        const iter_vtbl_t raw_darray::iterator_vtbl =
        {
            iter_move,
            iter_get,
            iter_compare,
            iter_compare,
            iter_count
        };

        void raw_darray::init(size_t n_sizeof)
        {
            nItems      = 0;
            vItems      = NULL;
            nCapacity   = 0;
            nSizeOf     = n_sizeof;
        }

        bool raw_darray::grow(size_t capacity)
        {
            if (capacity < 32)
                capacity        = 32;

            // Do aligned (re)allocation
            uint8_t *ptr    = reinterpret_cast<uint8_t *>(::realloc(vItems, nSizeOf * capacity));
            if (ptr == NULL)
                return false;

            // Update pointer and capacity
            vItems          = ptr;
            nCapacity       = capacity;
            return true;
        }

        bool raw_darray::truncate(size_t capacity)
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
            uint8_t *ptr    = reinterpret_cast<uint8_t *>(::realloc(vItems, nSizeOf * capacity));
            if (ptr == NULL)
                return false;

            // Update pointer, capacity and size
            vItems          = ptr;
            nCapacity       = capacity;
            if (nItems > capacity)
                nItems          = capacity;
            return true;
        }

        uint8_t *raw_darray::slice(size_t idx, size_t size)
        {
            if (size <= 0)
                return NULL;

            size_t tail = idx + size;
            return (tail <= nItems) ? &vItems[idx * nSizeOf] : NULL;
        }

        uint8_t *raw_darray::get_n(size_t idx, size_t size, void *dst)
        {
            uint8_t *res = static_cast<uint8_t *>(dst);
            if (size <= 0)
                return res;

            size_t tail = idx + size;
            if (tail > nItems)
                return NULL;

            ::memmove(res, &vItems[idx * nSizeOf], size * nSizeOf);
            return res;
        }

        bool raw_darray::xswap(size_t i1, size_t i2)
        {
            if ((i1 >= nItems) || (i2 >= nItems))
                return false;
            if (i1 != i2)
                uswap(i1, i2);
            return true;
        }

        void raw_darray::uswap(size_t i1, size_t i2)
        {
            uint8_t buf[0x200];
            uint8_t *a = &vItems[i1 * nSizeOf];
            uint8_t *b = &vItems[i2 * nSizeOf];

            for (size_t i=0; i<nSizeOf; i += sizeof(buf))
            {
                // Size of block
                size_t n = nSizeOf - i;
                if (n > sizeof(buf))
                    n = sizeof(buf);

                // Perform swap
                ::memcpy(buf, a, n);
                ::memmove(a, b, n);
                ::memcpy(b, buf, n);

                // Update pointers
                a  += sizeof(buf);
                b  += sizeof(buf);
            }
        }

        uint8_t *raw_darray::append(size_t n)
        {
			size_t count    = nonzero(nItems, n);
            size_t size     = nItems + count;
            if (size > nCapacity)
            {
                size_t dn = nCapacity + count;
                if (!grow(dn + (dn >> 1)))
                    return NULL;
            }

            uint8_t *ptr    = &vItems[nItems * nSizeOf];
            nItems         += n;
            return ptr;
        }

        uint8_t *raw_darray::append(size_t n, const void *src)
        {
            size_t count    = nonzero(nItems, n);
            size_t size     = nItems + count;
            if (size > nCapacity)
            {
                size_t dn = nCapacity + count;
                if (!grow(dn + (dn >> 1)))
                    return NULL;
            }

            uint8_t *ptr    = &vItems[nItems * nSizeOf];
            ::memcpy(ptr, src, n * nSizeOf);
            nItems         += n;
            return ptr;
        }

        uint8_t *raw_darray::set(size_t n, const void *src)
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

            ::memcpy(vItems, src, n * nSizeOf);
            nItems          = n;
            return vItems;
        }

        uint8_t *raw_darray::iset(size_t idx, size_t n, const void *src)
        {
            if ((idx + n) > nItems)
                return NULL;

            uint8_t *dst = &vItems[idx * nSizeOf];
            ::memcpy(dst, src, n * nSizeOf);
            return dst;
        }

        uint8_t *raw_darray::insert(size_t index, size_t n)
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
            uint8_t *res    = &vItems[index * nSizeOf];
            if (index < nItems)
                ::memmove(&res[n*nSizeOf], res, (nItems - index) * nSizeOf);
            nItems         += n;
            return res;
        }

        uint8_t *raw_darray::insert(size_t index, size_t n, const void *src)
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
            uint8_t *res    = &vItems[index * nSizeOf];
            if (index < nItems)
                ::memmove(&res[n*nSizeOf], res, (nItems - index) * nSizeOf);
            ::memcpy(res, src, n * nSizeOf);

            nItems         += n;
            return res;
        }

        void raw_darray::swap(raw_darray *src)
        {
            raw_darray tmp;
            tmp     = *this;
            *this   = *src;
            *src    = tmp;
        }

        void raw_darray::flush()
        {
            if (vItems != NULL)
            {
                ::free(vItems);
                vItems      = NULL;
            }
            nCapacity   = 0;
            nItems      = 0;
        }

        ssize_t raw_darray::index_of(const void *ptr)
        {
            if (ptr == NULL)
                return -1;
            uint8_t *src = static_cast<uint8_t *>(const_cast<void *>(ptr));
            if (src < vItems) // Pointer before array
                return -2;

            size_t off  = (src - vItems) / nSizeOf;
            if (off >= nItems)
                return -3;

            return (&vItems[off * nSizeOf] == src) ? off : -1;
        }

        bool raw_darray::premove(const void *ptr, size_t n)
        {
            if (ptr == NULL)
                return false;
            uint8_t *src = static_cast<uint8_t *>(const_cast<void *>(ptr));
            if (src < vItems) // Pointer before array?
                return false;

            size_t off  = (src - vItems) / nSizeOf;
            size_t last = off + n;
            if ((last > nItems) || (src != &vItems[off * nSizeOf]))
                return false;

            if (last < nItems)
                ::memmove(src, &vItems[last * nSizeOf], (nItems - last) * nSizeOf);
            nItems     -= n;
            return true;
        }

        uint8_t *raw_darray::premove(const void *ptr, size_t n, void *dst)
        {
            if (ptr == NULL)
                return NULL;
            uint8_t *src = static_cast<uint8_t *>(const_cast<void *>(ptr));
            if (src < vItems) // Pointer before array
                return NULL;

            size_t off  = src - vItems;
            size_t cap  = nItems * nSizeOf;

            if ((off >= cap) || (off % nSizeOf))
                return NULL;

            size_t last = off + n * nSizeOf;
            ::memmove(dst, src, n * nSizeOf);
            if (last < cap)
                ::memmove(src, &vItems[last], cap - last);
            nItems     -= n;
            return static_cast<uint8_t *>(dst);
        }

        uint8_t *raw_darray::premove(const void *ptr, size_t n, raw_darray *cs)
        {
            if (ptr == NULL)
                return NULL;
            uint8_t *src = static_cast<uint8_t *>(const_cast<void *>(ptr));
            if (src < vItems) // Pointer before array?
                return NULL;

            size_t off  = (src - vItems) / nSizeOf;
            size_t last = off + n;
            if ((last > nItems) || (src != &vItems[off * nSizeOf]))
                return NULL;

            uint8_t *res = cs->append(n, src);
            if (res)
            {
                if (last < nItems)
                    ::memmove(src, &vItems[last * nSizeOf], (nItems - last) * nSizeOf);
                nItems     -= n;
            }
            return res;
        }

        bool raw_darray::iremove(size_t idx, size_t n)
        {
            size_t last = idx + n;
            if (last > nItems)
                return false;
            if (last < nItems)
                ::memmove(&vItems[idx * nSizeOf], &vItems[last * nSizeOf], (nItems - last) * nSizeOf);
            nItems     -= n;
            return true;
        }

        uint8_t    *raw_darray::iremove(size_t idx, size_t n, void *dst)
        {
            size_t last = idx + n;
            if (last > nItems)
                return NULL;

            uint8_t *src = &vItems[idx * nSizeOf];
            ::memmove(dst, src, n * nSizeOf);
            if (last < nItems)
                ::memmove(src, &vItems[last * nSizeOf], (nItems - last) * nSizeOf);
            nItems     -= n;
            return static_cast<uint8_t *>(dst);
        }

        uint8_t *raw_darray::iremove(size_t idx, size_t n, raw_darray *cs)
        {
            size_t last = idx + n;
            if (last > nItems)
                return NULL;

            uint8_t *src    = &vItems[idx * nSizeOf];
            uint8_t *res    = cs->append(n, src);
            if (res)
            {
                if (last < nItems)
                    ::memmove(src, &vItems[last * nSizeOf], (nItems - last) * nSizeOf);
                nItems     -= n;
            }
            return res;
        }

        void *raw_darray::qremove(size_t idx)
        {
            if (idx >= nItems)
                return NULL;
            void *res   = &vItems[idx * nSizeOf];
            size_t size = nItems - 1;
            if (idx < size)
                ::memmove(res, &vItems[size * nSizeOf], nSizeOf);
            nItems      = size;
            return res;
        }

        void *raw_darray::qpremove(const void *ptr)
        {
            ssize_t idx = index_of(ptr);
            if (idx < 0)
                return NULL;

            void *res   = &vItems[idx * nSizeOf];
            ssize_t size= nItems - 1;
            if (idx < size)
                ::memmove(res, &vItems[size * nSizeOf], nSizeOf);
            nItems      = size;
            return res;
        }

        uint8_t *raw_darray::pop(size_t n)
        {
            if (nItems < n)
                return NULL;

            nItems -= n;
            return &vItems[nItems * nSizeOf];
        }

        uint8_t *raw_darray::pop(size_t n, void *dst)
        {
            if (nItems < n)
                return NULL;

            nItems         -= n;
            size_t size     = nItems * nSizeOf;
            uint8_t *src    = &vItems[size];
            ::memcpy(dst, src, n * nSizeOf);

            return static_cast<uint8_t *>(dst);
        }

        uint8_t *raw_darray::pop(size_t n, raw_darray *cs)
        {
            if (nItems < n)
                return NULL;

            size_t size     = nItems - n;
            uint8_t *res    = cs->append(n, &vItems[size * nSizeOf]);
            if (res)
                nItems          = size;

            return res;
        }

        int raw_darray::closure_cmp(const void *a, const void *b, void *c)
        {
            sort_closure_t *sc = static_cast<sort_closure_t *>(c);
            ssize_t res = sc->compare(a, b, sc->size);
            return (res > 0) ? 1 : (res < 0) ? -1 : 0;
        }

        int raw_darray::raw_cmp(const void *a, const void *b, void *c)
        {
            cmp_func_t f = reinterpret_cast<cmp_func_t>(c);
            ssize_t res = f(a, b);
            return (res > 0) ? 1 : (res < 0) ? -1 : 0;
        }

        void raw_darray::qsort(sort_closure_t *c)
        {
            lsp::qsort_r(vItems, nItems, nSizeOf, closure_cmp, c);
        }

        void raw_darray::qsort(cmp_func_t f)
        {
            union
            {
                cmp_func_t f;
                void *p;
            } xf;
            xf.f = f;
            lsp::qsort_r(vItems, nItems, nSizeOf, raw_cmp, xf.p);
        }

        void raw_darray::ssort(sort_closure_t *c)
        {
            lsp::ssort_r(vItems, nItems, nSizeOf, closure_cmp, c);
        }

        void raw_darray::ssort(cmp_func_t f)
        {
            union
            {
                cmp_func_t f;
                void *p;
            } xf;
            xf.f = f;
            lsp::ssort_r(vItems, nItems, nSizeOf, raw_cmp, xf.p);
        }

        raw_iterator raw_darray::iter()
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

        raw_iterator raw_darray::riter()
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

        void raw_darray::iter_move(raw_iterator *i, ssize_t n)
        {
            raw_darray *self    = static_cast<raw_darray *>(i->container);
            ssize_t off = i->index + n;
            if ((off >= 0) && (size_t(off) < self->nItems))
                i->index        = off;
            else
                *i              = raw_iterator::INVALID;
        }

        void *raw_darray::iter_get(raw_iterator *i)
        {
            raw_darray *self = static_cast<raw_darray *>(i->container);
            return (i->index < self->nItems) ? &self->vItems[i->index * self->nSizeOf] : NULL;
        }

        ssize_t raw_darray::iter_compare(const raw_iterator *a, const raw_iterator *b)
        {
            return a->index - b->index;
        }

        size_t raw_darray::iter_count(const raw_iterator *i)
        {
            raw_darray *self    = static_cast<raw_darray *>(i->container);
            return self->nItems;
        }

    } /* namespace lltl */
} /* namespace lsp */
