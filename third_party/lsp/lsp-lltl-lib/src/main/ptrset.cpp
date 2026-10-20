/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-lltl-lib
 * Created on: 12 нояб. 2023 г.
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

#include <lsp-plug.in/lltl/ptrset.h>

namespace lsp
{
    namespace lltl
    {
        const iter_vtbl_t raw_ptrset::iterator_vtbl =
        {
            raw_ptrset::iter_move,
            raw_ptrset::iter_get,
            raw_ptrset::iter_compare,
            raw_ptrset::iter_compare,
            raw_ptrset::iter_count
        };

        void raw_ptrset::destroy_bin(bin_t *bin)
        {
            if (bin->data != NULL)
            {
                free(bin->data);
                bin->data   = NULL;
            }

            bin->cap    = 0;
            bin->size   = 0;
        }

        raw_ptrset::bin_t *raw_ptrset::next_bin(bin_t *bin)
        {
            while (true)
            {
                if ((++bin) >= &bins[cap])
                    return NULL;
                if (bin->size > 0)
                    return bin;
            }
        }

        raw_ptrset::bin_t *raw_ptrset::prev_bin(bin_t *bin)
        {
            while (true)
            {
                if ((--bin) < bins)
                    return NULL;
                if (bin->size > 0)
                    return bin;
            }
        }

        bool raw_ptrset::grow()
        {
            bin_t *xbin, *ybin;
            size_t ncap, mask;

            // No previous allocations?
            if (cap == 0)
            {
                xbin            = static_cast<bin_t *>(::malloc(ptrset_tuple_items * sizeof(bin_t)));
                if (xbin == NULL)
                    return false; // Very bad things?

                cap             = 0x10;
                bins            = xbin;
                for (size_t i=0; i<cap; ++i, ++xbin)
                {
                    xbin->size      = 0;
                    xbin->cap       = 0;
                    xbin->data      = NULL;
                }

                return true;
            }

            // Create new set with twice increased bin container size
            raw_ptrset tmp;
            ncap            = cap << 1;
            tmp.bins        = static_cast<bin_t *>(::malloc(ncap * sizeof(bin_t)));
            if (tmp.bins == NULL)
                return false; // Very bad things?
            tmp.size        = size;
            tmp.cap         = ncap;
            tmp.hash        = hash;

            for (size_t i=0; i<ncap; ++i)
            {
                bin_t *bin      = &tmp.bins[i];
                bin->size       = 0;
                bin->cap        = 0;
                bin->data       = NULL;
            }
            lsp_finally {
                tmp.flush();
            };

            // Now we need to split data
            mask            = (ncap - 1) ^ (cap - 1); // mask indicates the bit which has been set
            xbin            = &tmp.bins[0];
            ybin            = &tmp.bins[cap];

            for (size_t i=0; i<cap; ++i, ++xbin, ++ybin)
            {
                // Source bin
                bin_t *bin      = &bins[i];

                // Append data to the bin depending on the hash value
                for (size_t i=0; i<bin->size; ++i)
                {
                    void *value         = bin->data[i];
                    size_t hval         = (value != NULL) ? hash.hash(value, sizeof(void *)) : 0;
                    bin_t *dbin         = (hval & mask) ? ybin : xbin;
                    if (!append(dbin, value))
                        return false;
                }
            }

            // Commit new state
            tmp.swap(this);

            return true;
        }

        void raw_ptrset::flush()
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

        void raw_ptrset::clear()
        {
            // Just reset the size value for each bin
            if (bins != NULL)
            {
                for (size_t i=0; i<cap; ++i)
                    destroy_bin(&bins[i]);
            }

            size    = 0;
        }

        void raw_ptrset::swap(raw_ptrset *src)
        {
            raw_ptrset tmp      = *this;
            *this               = *src;
            *src                = tmp;
        }

        ssize_t raw_ptrset::index_of(const bin_t *bin, const void *value)
        {
            if (bin->size <= 0)
                return -1;

            const void *mp;
            ssize_t first = 0, last = bin->size - 1, mid;

            while (first < last)
            {
                mid             = (first + last) >> 1;
                mp              = bin->data[mid];
                if (value < mp)
                    last            = mid - 1;
                else if (value > mp)
                    first           = mid + 1;
                else
                    return mid;
            }

            mp              = bin->data[first];
            return (value == mp) ? first : -1;
        }

        size_t raw_ptrset::toggle_index_of(const bin_t *bin, const void *value)
        {
            const void *mp;
            ssize_t first = 0, last = bin->size - 1, mid;

            while (first < last)
            {
                mid             = (first + last) >> 1;
                mp              = bin->data[mid];
                if (value < mp)
                    last            = mid - 1;
                else if (value > mp)
                    first           = mid + 1;
                else
                    return mid;
            }

            return first;
        }

        ssize_t raw_ptrset::insert_index_of(const bin_t *bin, const void *value)
        {
            if (bin->size <= 0)
                return 0;

            const void *mp;
            ssize_t first = 0, last = bin->size - 1, mid;

            while (first < last)
            {
                mid             = (first + last) >> 1;
                mp              = bin->data[mid];
                if (value < mp)
                    last            = mid - 1;
                else if (value > mp)
                    first           = mid + 1;
                else
                    return -1;
            }

            mp              = bin->data[first];
            if (value == mp)
                return -1;
            return (value < mp) ? first : first + 1;
        }

        bool raw_ptrset::insert(bin_t *bin, void *value, size_t index)
        {
            // Grow the size of the bin
            if (bin->size >= bin->cap)
            {
                size_t new_cap      = lsp_max(bin->cap + (bin->cap >> 1), ptrset_tuple_items >> 1);
                void **new_ptr      = reinterpret_cast<void **>(realloc(bin->data, sizeof(value) * new_cap));
                if (new_ptr == NULL)
                    return false;

                bin->data           = new_ptr;
                bin->cap            = new_cap;
            }

            // Insert the value
            if (index < bin->size)
                memmove(&bin->data[index+1], &bin->data[index], (bin->size - index)*sizeof(void *));
            bin->data[index]    = value;
            ++bin->size;
            return true;
        }

        bool raw_ptrset::append(bin_t *bin, void *value)
        {
            // Grow the size of the bin
            if (bin->size >= bin->cap)
            {
                size_t new_cap      = lsp_max(bin->cap + (bin->cap >> 1), ptrset_tuple_items >> 1);
                void **new_ptr      = reinterpret_cast<void **>(realloc(bin->data, sizeof(value) * new_cap));
                if (new_ptr == NULL)
                    return false;

                bin->data           = new_ptr;
                bin->cap            = new_cap;
            }

            // Append the value
            bin->data[bin->size++]    = value;
            return true;
        }

        void raw_ptrset::remove(bin_t *bin, size_t index)
        {
            if (index >= bin->size)
                return;

            size_t new_size = bin->size - 1;
            if (index < new_size)
                memmove(&bin->data[index], &bin->data[index+1], (new_size - index)*sizeof(void *));
            bin->size       = new_size;
        }

        void *raw_ptrset::get(const void *value, void *dfl)
        {
            if (bins == NULL)
                return dfl;

            size_t hval         = (value != NULL) ? hash.hash(value, sizeof(void *)) : 0;
            bin_t *bin          = &bins[hval & (cap - 1)];
            ssize_t idx         = index_of(bin, value);
            return (idx < 0) ? dfl : bin->data[idx];
        }

        bool raw_ptrset::contains(const void *value)
        {
            if (bins == NULL)
                return false;

            size_t hval         = (value != NULL) ? hash.hash(value, sizeof(void *)) : 0;
            bin_t *bin          = &bins[hval & (cap - 1)];
            ssize_t idx         = index_of(bin, value);

            return idx >= 0;
        }

        void *raw_ptrset::any()
        {
            if (size <= 0)
                return NULL;

            for (size_t i=0; i<cap; ++i)
            {
                bin_t *bin = &bins[i];
                if (bin->size > 0)
                    return bin->data[0];
            }

            return NULL;
        }

        bool raw_ptrset::put(void *value)
        {
            // Grow the size of the set if it is too small
            size_t hval         = (value != NULL) ? hash.hash(value, sizeof(void *)) : 0;
            bin_t *bin          = (bins != NULL) ? &bins[hval & (cap - 1)] : NULL;
            if ((bin == NULL) || (bin->size >= (ptrset_tuple_items << 1)))
            {
                if (!grow())
                    return false;
                bin                 = &bins[hval & (cap - 1)];
            }

            // Perform binary search of the insert position
            ssize_t idx         = insert_index_of(bin, value);
            if (idx < 0)
                return false;

            if (!insert(bin, value, idx))
                return false;

            ++size;
            return true;
        }

        bool raw_ptrset::toggle(void *value)
        {
            // Call put() if there are no bins
            if (bins == NULL)
                return put(value);

            // Insert value if the bin is empty
            size_t hval         = (value != NULL) ? hash.hash(value, sizeof(void *)) : 0;
            bin_t *bin          = &bins[hval & (cap - 1)];
            if (bin->size <= 0)
            {
                if (!insert(bin, value, 0))
                    return false;
                ++size;
                return true;
            }

            // Find the place to update/insert the item
            size_t idx          = toggle_index_of(bin, value);
            const void *mp      = bin->data[idx];
            if (value == mp)
            {
                remove(bin, idx);
                --size;
                return true;
            }
            else if (value > mp)
                ++idx;

            if (!insert(bin, value, idx))
                return false;

            if (bin->size >= (ptrset_tuple_items << 1))
                grow();

            ++size;
            return true;
        }

        bool raw_ptrset::remove(const void *value)
        {
            if (bins == NULL)
                return false;

            // Find tuple
            size_t hval         = (value != NULL) ? hash.hash(value, sizeof(void *)) : 0;
            bin_t *bin          = &bins[hval & (cap - 1)];
            ssize_t idx         = index_of(bin, value);
            if (idx < 0)
                return false;

            remove(bin, idx);
            --size;

            return true;
        }

        bool raw_ptrset::values(raw_parray *v)
        {
            raw_parray kv;

            // Initialize collection
            kv.init();
            if (!kv.grow(size))
                return false;

            // Make a snapshot
            for (size_t i=0; i<cap; ++i)
            {
                bin_t *bin = &bins[i];
                if (bin->size <= 0)
                    continue;

                if (!kv.append(bin->size, bin->data))
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

        raw_iterator raw_ptrset::iter(const iter_vtbl_t *vtbl)
        {
            if (size <= 0)
                return raw_iterator::INVALID;

            // Find first item and return iterator record
            for (size_t i=0; i<cap; ++i)
            {
                bin_t *bin  = &bins[i];
                if ((bin->data != NULL) && (bin->size > 0))
                    return raw_iterator {
                        vtbl,
                        this,
                        bin,
                        0,
                        0,
                        0,
                        false
                    };
            }

            return raw_iterator::INVALID;
        }

        raw_iterator raw_ptrset::riter(const iter_vtbl_t *vtbl)
        {
            if (size <= 0)
                return raw_iterator::INVALID;

            // Find last item and return iterator record
            for (size_t i=cap; i>0; )
            {
                bin_t *bin  = &bins[--i];
                if ((bin->data != NULL) && (bin->size > 0))
                    return raw_iterator {
                        vtbl,
                        this,
                        bin,
                        size - 1,
                        bin->size - 1,
                        0,
                        true
                    };
            }

            return raw_iterator::INVALID;
        }

        void raw_ptrset::iter_move(raw_iterator *i, ssize_t n)
        {
            // Ensure that we don't get out of bounds
            raw_ptrset *self    = static_cast<raw_ptrset *>(i->container);
            ssize_t new_idx     = i->index + n;
            if ((new_idx < 0) || (size_t(new_idx) >= self->size))
            {
                *i              = raw_iterator::INVALID;
                return;
            }

            bin_t *bin;
            size_t advance;

            // Iterate forward
            while (n > 0)
            {
                // Try to advance inside of the bin
                bin             = static_cast<bin_t *>(i->item);
                if (size_t(i->offset + n) < bin->size)
                {
                    i->index       += n;
                    i->offset      += n;
                    break;
                }

                // Advance to the next bin
                advance         = bin->size - i->offset;
                bin             = self->next_bin(bin);
                if (bin == NULL)
                {
                    *i = raw_iterator::INVALID;
                    return;
                }

                n              -= advance;
                i->item         = bin;
                i->index       += advance;
                i->offset       = 0;
            }

            // Iterate backward
            while (n < 0)
            {
                // Try to advance inside of the bin
                bin             = static_cast<bin_t *>(i->item);
                if (ssize_t(i->offset + n) >= 0)
                {
                    i->index       += n;
                    i->offset      += n;
                    break;
                }

                // Advance to the next bin
                advance         = i->offset + 1;
                bin             = self->prev_bin(bin);
                if (bin == NULL)
                {
                    *i = raw_iterator::INVALID;
                    return;
                }

                n              += advance;
                i->item         = bin;
                i->index       -= advance;
                i->offset       = bin->size - 1;
            }
        }

        void *raw_ptrset::iter_get(raw_iterator *i)
        {
            bin_t *bin = static_cast<bin_t *>(i->item);
            return bin->data[i->offset];
        }

        ssize_t raw_ptrset::iter_compare(const raw_iterator *a, const raw_iterator *b)
        {
            return a->index - b->index;
        }

        size_t raw_ptrset::iter_count(const raw_iterator *i)
        {
            raw_ptrset *self  = static_cast<raw_ptrset *>(i->container);
            return self->size;
        }

    } /* namespace lltl */
} /* namespace lsp */



