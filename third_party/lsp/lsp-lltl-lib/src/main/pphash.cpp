/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-lltl-lib
 * Created on: 11 мая 2020 г.
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

#include <lsp-plug.in/lltl/pphash.h>
#include <lsp-plug.in/stdlib/stdlib.h>

namespace lsp
{
    namespace lltl
    {
        const iter_vtbl_t raw_pphash::key_iterator_vtbl =
        {
            iter_move,
            iter_get_key,
            iter_compare,
            iter_compare,
            iter_count
        };

        const iter_vtbl_t raw_pphash::value_iterator_vtbl =
        {
            iter_move,
            iter_get_value,
            iter_compare,
            iter_compare,
            iter_count
        };

        const iter_vtbl_t raw_pphash::pair_iterator_vtbl =
        {
            iter_move,
            iter_get_pair,
            iter_compare,
            iter_compare,
            iter_count
        };

        void raw_pphash::destroy_bin(bin_t *bin)
        {
            for (tuple_t *curr = bin->data; curr != NULL; )
            {
                tuple_t *next   = curr->next;
                if (curr->v.key != NULL)
                    alloc.free(curr->v.key);
                ::free(curr);
                curr            = next;
            }
            bin->size   = 0;
            bin->data   = NULL;
        }

        raw_pphash::tuple_t *raw_pphash::find_tuple(const void *key, size_t hash)
        {
            if (bins == NULL)
                return NULL;
            bin_t *bin = &bins[hash & (cap - 1)];

            if (key != NULL)
            {
                for (tuple_t *curr = bin->data; curr != NULL; curr = curr->next)
                {
                    if ((curr->hash == hash) && (cmp.compare(key, curr->v.key, ksize) == 0))
                        return curr;
                }
            }
            else
            {
                for (tuple_t *curr = bin->data; curr != NULL; curr = curr->next)
                {
                    if (curr->v.key == NULL)
                        return curr;
                }
            }
            return NULL;
        }

        raw_pphash::tuple_t *raw_pphash::remove_tuple(const void *key, size_t hash)
        {
            if (bins == NULL)
                return NULL;
            bin_t *bin = &bins[hash & (cap - 1)];

            if (key != NULL)
            {
                for (tuple_t **pcurr = &bin->data; *pcurr != NULL; )
                {
                    tuple_t *curr = *pcurr;
                    if ((curr->hash == hash) && (cmp.compare(key, curr->v.key, ksize) == 0))
                    {
                        *pcurr      = curr->next;
                        curr->next  = NULL;
                        --bin->size;
                        --size;
                        return curr;
                    }
                    pcurr   = &curr->next;
                }
            }
            else
            {
                for (tuple_t **pcurr = &bin->data; *pcurr != NULL; )
                {
                    tuple_t *curr = *pcurr;
                    if (curr->v.key == NULL)
                    {
                        *pcurr      = curr->next;
                        curr->next  = NULL;
                        --bin->size;
                        --size;
                        return curr;
                    }
                    pcurr   = &curr->next;
                }
            }
            return NULL;
        }

        raw_pphash::tuple_t *raw_pphash::create_tuple(const void *key, size_t hash)
        {
            // Allocate tuple
            tuple_t *tuple  = reinterpret_cast<tuple_t *>(::malloc(sizeof(tuple_t)));
            if (tuple == NULL)
                return NULL;

            // Create copy of the key
            void *kcopy     = NULL;
            if (key != NULL)
            {
                if ((kcopy = alloc.clone(key, ksize)) == NULL)
                {
                    ::free(tuple);
                    return NULL;
                }
            }

            // Need to grow?
            if (size >= cap*4)
            {
                if (!grow())
                {
                    ::free(tuple);
                    if (kcopy != NULL)
                        alloc.free(kcopy);
                    return NULL;
                }
            }

            // Initialize tuple
            bin_t *bin      = &bins[hash & (cap - 1)];
            ++bin->size;
            ++size;

            tuple->hash     = hash;
            tuple->v.key    = kcopy;
            tuple->next     = bin->data;
            bin->data       = tuple;

            return tuple;
        }

        bool raw_pphash::grow()
        {
            bin_t *xbin, *ybin;
            size_t ncap, mask;

            // No previous allocations?
            if (cap == 0)
            {
                xbin            = reinterpret_cast<bin_t *>(::malloc(0x10 * sizeof(bin_t)));
                if (xbin == NULL)
                    return false; // Very bad things?

                cap             = 0x10;
                bins            = xbin;
                for (size_t i=0; i<cap; ++i, ++xbin)
                {
                    xbin->size      = 0;
                    xbin->data      = NULL;
                }

                return true;
            }

            // Twice increase the capacity of hash
            ncap            = cap << 1;
            xbin            = reinterpret_cast<bin_t *>(::realloc(bins, ncap * sizeof(bin_t)));
            if (xbin == NULL)
                return false; // Very bad things?

            // Now we need to split data
            mask            = (ncap - 1) ^ (cap - 1); // mask indicates the bit which has been set
            bins            = xbin;
            ybin            = &xbin[cap];

            for (size_t i=0; i<cap; ++i, ++xbin, ++ybin)
            {
                // There is no data in ybin by default
                ybin->size      = 0;
                ybin->data      = 0;

                // Migrate items from xbin list to ybin list
                for (tuple_t **pcurr = &xbin->data; *pcurr != NULL; )
                {
                    tuple_t *curr   = *pcurr;
                    if (curr->hash & mask)
                    {
                        *pcurr          = curr->next;
                        curr->next      = ybin->data;
                        ybin->data      = curr;
                        --xbin->size;
                        ++ybin->size;
                    }
                    else
                        pcurr           = &curr->next;
                }
            }

            // Split success
            cap         = ncap;

            return true;
        }

        void raw_pphash::flush()
        {
            // Drop all bins
            if (bins != NULL)
            {
                for (size_t i=0; i<cap; ++i)
                    destroy_bin(&bins[i]);
                ::free(bins);
                bins    = NULL;
            }

            size    = 0;
            cap     = 0;
        }

        void raw_pphash::clear()
        {
            // Just reset the size value for each bin
            if (bins != NULL)
            {
                for (size_t i=0; i<cap; ++i)
                    destroy_bin(&bins[i]);
            }

            size    = 0;
        }

        void raw_pphash::swap(raw_pphash *src)
        {
            raw_pphash tmp  = *this;
            *this           = *src;
            *src            = tmp;
        }

        void *raw_pphash::get(const void *key, void *dfl)
        {
            size_t h        = (key != NULL) ? hash.hash(key, ksize) : 0;
            tuple_t *tuple  = find_tuple(key, h);
            return (tuple != NULL) ? tuple->v.value : dfl;
        }

        void *raw_pphash::key(const void *key, void *dfl)
        {
            size_t h        = (key != NULL) ? hash.hash(key, ksize) : 0;
            tuple_t *tuple  = find_tuple(key, h);
            return (tuple != NULL) ? tuple->v.key : dfl;
        }

        void **raw_pphash::wbget(const void *key)
        {
            size_t h        = (key != NULL) ? hash.hash(key, ksize) : 0;
            tuple_t *tuple  = find_tuple(key, h);
            return (tuple != NULL) ? &tuple->v.value : NULL;
        }

        void **raw_pphash::put(const void *key, void *value, void **ov)
        {
            size_t h        = (key != NULL) ? hash.hash(key, ksize) : 0;

            // Find tuple
            tuple_t *tuple  = find_tuple(key, h);
            if (tuple != NULL)
            {
                if (ov != NULL)
                    *ov         = tuple->v.value;
                tuple->v.value  = value;
                return &tuple->v.value;
            }

            // Not found, allocate new tuple
            tuple           = create_tuple(key, h);
            if (tuple == NULL)
                return NULL;

            tuple->v.value  = value;
            if (ov != NULL)
                *ov         = NULL;

            return &tuple->v.value;
        }

        void **raw_pphash::create(const void *key, void *value)
        {
            size_t h        = (key != NULL) ? hash.hash(key, ksize) : 0;

            // Find tuple
            tuple_t *tuple  = find_tuple(key, h);
            if (tuple != NULL)
                return NULL;

            // Create new tuple
            tuple           = create_tuple(key, h);
            if (tuple == NULL)
                return NULL;

            tuple->v.value  = value;

            return &tuple->v.value;
        }

        void **raw_pphash::replace(const void *key, void *value, void **ov)
        {
            size_t h        = (key != NULL) ? hash.hash(key, ksize) : 0;

            // Find tuple
            tuple_t *tuple  = find_tuple(key, h);
            if (tuple == NULL)
                return NULL;

            if (ov != NULL)
                *ov         = tuple->v.value;
            tuple->v.value  = value;
            return &tuple->v.value;
        }

        bool raw_pphash::remove(const void *key, void **ov)
        {
            size_t h        = (key != NULL) ? hash.hash(key, ksize) : 0;

            // Find tuple
            tuple_t *tuple  = remove_tuple(key, h);
            if (tuple == NULL)
                return false;

            if (ov != NULL)
                *ov         = tuple->v.value;

            // Free tuple data
            if (tuple->v.key != NULL)
                alloc.free(tuple->v.key);
            ::free(tuple);
            return true;
        }

        bool raw_pphash::keys(raw_parray *k) const
        {
            raw_parray kt;

            // Initialize collection
            kt.init();
            if (!kt.grow(size))
                return false;

            // Make a snapshot
            for (size_t i=0; i<cap; ++i)
                for (tuple_t *t = bins[i].data; t != NULL; t = t->next)
                {
                    if (!kt.append(t->v.key))
                    {
                        kt.flush();
                        return false;
                    }
                }

            // Return collection data
            kt.swap(k);
            kt.flush();

            return true;
        }

        bool raw_pphash::values(raw_parray *v) const
        {
            raw_parray kv;

            // Initialize collection
            kv.init();
            if (!kv.grow(size))
                return false;

            // Make a snapshot
            for (size_t i=0; i<cap; ++i)
                for (tuple_t *t = bins[i].data; t != NULL; t = t->next)
                {
                    if (!kv.append(t->v.value))
                    {
                        kv.flush();
                        return false;
                    }
                }

            // Return collection data
            kv.swap(v);
            kv.flush();

            return true;
        }

        bool raw_pphash::items(raw_parray *k, raw_parray *v) const
        {
            raw_parray kt, vt;

            // Initialize collections
            kt.init();
            vt.init();

            if (!kt.grow(size))
                return false;
            if (!vt.grow(size))
            {
                kt.flush();
                return false;
            }

            // Make a snapshot
            for (size_t i=0; i<cap; ++i)
            {
                tuple_t *t = bins[i].data;
                for (; t != NULL; t = t->next)
                {
                    if ((!kt.append(t->v.key)) ||
                        (!vt.append(t->v.value)))
                    {
                        kt.flush();
                        vt.flush();
                        return false;
                    }
                }
            }

            // Return collection data
            kt.swap(k);
            vt.swap(v);

            kt.flush();
            vt.flush();

            return true;
        }

        raw_iterator raw_pphash::iter(const iter_vtbl_t *vtbl)
        {
            if (size <= 0)
                return raw_iterator::INVALID;

            // Find first item and return iterator record
            for (size_t i=0; i<cap; ++i)
            {
                bin_t *bin  = &bins[i];
                if (bin->data != NULL)
                    return raw_iterator {
                        vtbl,
                        this,
                        bin->data,
                        0,
                        i,
                        0,
                        false
                    };
            }

            return raw_iterator::INVALID;
        }

        raw_iterator raw_pphash::riter(const iter_vtbl_t *vtbl)
        {
            if (size <= 0)
                return raw_iterator::INVALID;

            // Find last item and return iterator record
            for (size_t i=cap; i>0; )
            {
                bin_t *bin  = &bins[--i];
                tuple_t *tuple = bin->data;
                if (tuple == NULL)
                    continue;

                // Find last tuple in list
                while (tuple->next != NULL)
                    tuple   = tuple->next;

                return raw_iterator {
                    vtbl,
                    this,
                    tuple,
                    size - 1,
                    i,
                    0,
                    true
                };
            }

            return raw_iterator::INVALID;
        }

        raw_pphash::tuple_t *raw_pphash::prev_tuple(bin_t *bin, const tuple_t *tuple)
        {
            tuple_t *prev   = NULL;
            for (tuple_t *t = bin->data; t != tuple; t = t->next)
                prev = t;

            return prev;
        }

        void raw_pphash::iter_move(raw_iterator *i, ssize_t n)
        {
            // Ensure that we don't get out of bounds
            raw_pphash *self    = static_cast<raw_pphash *>(i->container);
            ssize_t new_idx     = i->index + n;
            if ((new_idx < 0) || (size_t(new_idx) >= self->size))
            {
                *i = raw_iterator::INVALID;
                return;
            }

            // Iterate forward
            bin_t *bin;
            tuple_t *tuple;

            while (n > 0)
            {
                tuple  = static_cast<tuple_t *>(i->item);

                // Try to advance in the bin list
                i->item = (tuple != NULL) ? tuple->next : NULL;
                if (i->item != NULL)
                {
                    ++i->index;
                    --n;
                    continue;
                }

                // Try to advance the bin
                if ((++i->offset) >= self->cap)
                {
                    *i = raw_iterator::INVALID;
                    return;
                }

                // Obtain new bin
                bin         = &self->bins[i->offset];
                if (bin->size < size_t(n))
                {
                    n          -= bin->size;
                    i->index   += bin->size;
                    continue;
                }

                // Get item from the bin
                i->item     = bin->data;
                ++i->index;
                --n;
            }

            // Iterate backward
            while (n < 0)
            {
                tuple           = static_cast<tuple_t *>(i->item);
                bin             = &self->bins[i->offset];

                // Try to advance in the bin list
                i->item         = prev_tuple(bin, tuple);
                if (i->item != NULL)
                {
                    --i->index;
                    ++n;
                    continue;
                }

                // Try to advance the bin
                if ((i->offset--) <= 0)
                {
                    *i = raw_iterator::INVALID;
                    return;
                }

                // Obtain new bin
                bin             = &self->bins[i->offset];
                if (bin->size < size_t(-n))
                {
                    n          += bin->size;
                    i->index   -= bin->size;
                    continue;
                }
            }
        }

        void *raw_pphash::iter_get_key(raw_iterator *i)
        {
            tuple_t *tuple = static_cast<tuple_t *>(i->item);
            return tuple->v.key;
        }

        void *raw_pphash::iter_get_value(raw_iterator *i)
        {
            tuple_t *tuple = static_cast<tuple_t *>(i->item);
            return tuple->v.value;
        }

        void *raw_pphash::iter_get_pair(raw_iterator *i)
        {
            tuple_t *tuple = static_cast<tuple_t *>(i->item);
            return &tuple->v;
        }

        ssize_t raw_pphash::iter_compare(const raw_iterator *a, const raw_iterator *b)
        {
            return a->index - b->index;
        }

        size_t raw_pphash::iter_count(const raw_iterator *i)
        {
            raw_pphash *self  = static_cast<raw_pphash *>(i->container);
            return self->size;
        }

    } /* namespace lltl */
} /* namespace lsp */


