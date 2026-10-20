/*
 * Copyright (C) 2023 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2023 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-lltl-lib
 * Created on: 30 июл. 2020 г.
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

#include <lsp-plug.in/lltl/bitset.h>
#include <lsp-plug.in/stdlib/stdlib.h>
#include <lsp-plug.in/stdlib/string.h>

namespace lsp
{
    namespace lltl
    {
        bitset::bitset()
        {
            nSize       = 0;
            nCapacity   = 0;
            vData       = NULL;
        }

        bitset::~bitset()
        {
            flush();
        }

        void bitset::flush()
        {
            if (vData != NULL)
            {
                ::free(vData);
                vData       = NULL;
            }

            nSize       = 0;
            nCapacity   = 0;
        }

        bool bitset::resize(size_t size)
        {
            if (size == 0)
            {
                flush();
                return true;
            }

            size_t cap  = (size + UMWORD_BITS-1) / UMWORD_BITS;
            size_t bits = size % UMWORD_BITS;

            // Need to realloc data?
            if (cap != nCapacity)
            {
                umword_t *buf   = static_cast<umword_t *>(::realloc(vData, cap * sizeof(umword_t)));
                if (buf == NULL)
                    return false;
                if (cap > nCapacity)
                    ::bzero(&buf[nCapacity], (cap - nCapacity) * sizeof(umword_t));
                vData           = buf;
                nCapacity       = cap;
            }

            // Clear unused bits
            if (bits > 0)
                vData[nCapacity-1] &= ~(UMWORD_MAX << bits);
            nSize               = size;
            return true;
        }

        void bitset::clear()
        {
            nSize       = 0;
            if (vData != NULL)
                ::bzero(vData, nCapacity * sizeof(umword_t));
        }

        bool bitset::get(size_t index) const
        {
            if (index >= nSize)
                return false;

            size_t cap      = index / UMWORD_BITS;
            umword_t mask   = umword_t(1) << (index % UMWORD_BITS);
            return vData[cap] & mask;
        }

        void bitset::set_all()
        {
            if (vData == NULL)
                return;

            ::memset(vData, 0xff, nCapacity * sizeof(umword_t));
            size_t bits         = nSize % UMWORD_BITS;
            if (bits > 0)
                vData[nCapacity-1]  = ~(UMWORD_MAX << bits);
        }

        bool bitset::set(size_t index)
        {
            if (index >= nSize)
                return false;

            umword_t *w     = &vData[index / UMWORD_BITS];
            umword_t mask   = umword_t(1) << (index % UMWORD_BITS);
            bool prev       = (*w) & mask;
            *w             |= mask;
            return prev;
        }

        bool bitset::set(size_t index, bool value)
        {
            if (index >= nSize)
                return false;

            umword_t *w     = &vData[index / UMWORD_BITS];
            umword_t mask   = umword_t(1) << (index % UMWORD_BITS);
            bool prev       = (*w) & mask;
            *w              = (value) ? (*w) | mask: (*w) & (~mask);
            return prev;
        }

        size_t bitset::set(size_t index, size_t count)
        {
            if (index >= nSize)
                return 0;
            if ((index + count) > nSize)
                count           = nSize - index;
            if (count == 0)
                return 0;

            size_t total    = count;
            umword_t *w     = &vData[index / UMWORD_BITS];

            // Head part
            size_t off  = index % UMWORD_BITS;
            if (off > 0)
            {
                size_t bits = off + count - 1;
                if (bits < UMWORD_BITS)
                {
                    umword_t mask   = (UMWORD_MAX >> (UMWORD_BITS - count)) << off;
                    *w             |= mask;
                    return total;
                }
                else
                {
                    umword_t mask   = UMWORD_MAX << off;
                    *w             |= mask;
                    count          -= (UMWORD_BITS - off);
                    ++w;
                }
            }

            // Middle part
            size_t full     = count / UMWORD_BITS;
            if (full > 0)
            {
                ::memset(w, 0xff, full * sizeof(umword_t));
                count          -= full * UMWORD_BITS;
                w              += full;
            }

            // Tail part
            if (count > 0)
            {
                umword_t mask   = UMWORD_MAX >> (UMWORD_BITS - count);
                *w             |= mask;
            }

            return total;
        }

        size_t bitset::set(size_t index, size_t count, const bool *values)
        {
            if (index >= nSize)
                return 0;
            if ((index + count) > nSize)
                count           = nSize - index;

            umword_t *w     = &vData[index / UMWORD_BITS];
            umword_t mask   = umword_t(1) << (index % UMWORD_BITS);

            for (size_t i=0; i<count; ++values, ++i)
            {
                *w              = (*values) ? *w | mask : *w & (~mask);
                if (!(mask <<= 1))
                {
                    ++w;
                    mask            = 1;
                }
            }

            return count;
        }

        void bitset::unset_all()
        {
            if (vData == NULL)
                return;

            ::bzero(vData, nCapacity * sizeof(umword_t));
        }

        bool bitset::unset(size_t index)
        {
            if (index >= nSize)
                return false;

            umword_t *w     = &vData[index / UMWORD_BITS];
            umword_t mask   = umword_t(1) << (index % UMWORD_BITS);
            bool prev       = (*w) & mask;
            *w             &= (~mask);
            return prev;
        }

        size_t bitset::unset(size_t index, size_t count)
        {
            if (index >= nSize)
                return 0;
            if ((index + count) > nSize)
                count           = nSize - index;
            if (count == 0)
                return 0;

            size_t total    = count;
            umword_t *w     = &vData[index / UMWORD_BITS];

            // Head part
            size_t off  = index % UMWORD_BITS;
            if (off > 0)
            {
                size_t bits = off + count;
                if (bits < UMWORD_BITS)
                {
                    umword_t mask   = (UMWORD_MAX >> (UMWORD_BITS - count)) << off;
                    *w             &= ~mask;
                    return total;
                }
                else
                {
                    umword_t mask   = UMWORD_MAX << off;
                    *w             &= ~mask;
                    count          -= (UMWORD_BITS - off);
                    ++w;
                }
            }

            // Middle part
            size_t full = count / UMWORD_BITS;
            if (full > 0)
            {
                ::bzero(w, full * sizeof(umword_t));
                count          -= full * UMWORD_BITS;
                w              += full;
            }

            // Tail part
            if (count > 0)
            {
                umword_t mask   = UMWORD_MAX << count;
                *w             &= mask;
            }

            return total;
        }

        void bitset::toggle_all()
        {
            if (vData == NULL)
                return;

            umword_t *w  = vData;
            size_t count = nSize;
            for (; count >= UMWORD_BITS; count -= UMWORD_BITS, ++w)
                *w         ^= UMWORD_MAX;

            if (count > 0)
            {
                umword_t mask   = UMWORD_MAX >> (UMWORD_BITS - count);
                *w             ^= mask;
            }
        }

        bool bitset::toggle(size_t index)
        {
            if (index >= nSize)
                return false;

            umword_t *w     = &vData[index / UMWORD_BITS];
            umword_t mask   = umword_t(1) << (index % UMWORD_BITS);
            bool prev       = (*w) & mask;
            *w             ^= mask;
            return prev;
        }

        size_t bitset::toggle(size_t index, size_t count)
        {
            if (index >= nSize)
                return 0;
            if ((index + count) > nSize)
                count           = nSize - index;
            if (count == 0)
                return 0;

            size_t total    = count;
            umword_t *w     = &vData[index / UMWORD_BITS];

            // Head part
            size_t off  = index % UMWORD_BITS;
            if (off > 0)
            {
                size_t bits = off + count;
                if (bits < UMWORD_BITS)
                {
                    umword_t mask   = (UMWORD_MAX >> (UMWORD_BITS - count)) << off;
                    *w             ^= mask;
                    return total;
                }
                else
                {
                    umword_t mask   = UMWORD_MAX << off;
                    *w             ^= mask;
                    count          -= (UMWORD_BITS - off);
                    ++w;
                }
            }

            // Middle part
            for (; count >= UMWORD_BITS; count -= UMWORD_BITS, ++w)
                *w         ^= UMWORD_MAX;

            // Tail part
            if (count > 0)
            {
                umword_t mask   = UMWORD_MAX >> (UMWORD_BITS - count);
                *w             ^= mask;
            }

            return total;
        }

        void bitset::swap(bitset *dst)
        {
            lsp::swap(nSize, dst->nSize);
            lsp::swap(nCapacity, dst->nCapacity);
            lsp::swap(vData, dst->vData);
        }

    } /* namespace lltl */
} /* namespace lsp */
