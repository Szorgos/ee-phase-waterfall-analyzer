/*
 * Copyright (C) 2026 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2026 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-lltl-lib
 * Created on: 3 янв. 2026 г.
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

#include <lsp-plug.in/lltl/ddeque.h>

namespace lsp
{
    namespace lltl
    {
        const iter_vtbl_t raw_ddeque::iterator_vtbl =
        {
            iter_move,
            iter_get,
            iter_compare,
            iter_compare,
            iter_count
        };

        void raw_ddeque::forward_copy(void *dst, const void *src, size_t count)
        {
            memcpy(dst, src, count * nSizeOf);
        }

        void raw_ddeque::reverse_copy(void *dst, const void *src, size_t count)
        {
            uint8_t *dptr           = static_cast<uint8_t *>(dst) + count * nSizeOf;
            const uint8_t *sptr     = static_cast<const uint8_t *>(src);

            for (size_t i=0; i<count; ++i)
            {
                dptr                   -= nSizeOf;
                memcpy(dptr, sptr, nSizeOf);
                sptr                   += nSizeOf;
            }
        }

        void raw_ddeque::init(size_t n_sizeof, size_t chunk_capacity)
        {
            pHead           = NULL;
            pTail           = NULL;
            pUnused         = NULL;
            nItems          = 0;
            nChunks         = 0;
            nUnused         = 0;
            nSizeOf         = n_sizeof;
            nChunkSize      = chunk_capacity;
        }

        void raw_ddeque::free_chunk_list(chunk_t *head)
        {
            for (chunk_t *curr = head; curr != NULL; )
            {
                chunk_t * const next = curr->pNext;
                free(curr);
                curr = next;
            }
        }

        raw_ddeque::chunk_t *raw_ddeque::alloc_chunk_list(size_t count)
        {
            // Allocate first chunk
            chunk_t *head       = alloc_chunk();
            if (head == NULL)
                return NULL;
            lsp_finally { free_chunk_list(head); };

            // Allocate rest chunks
            chunk_t *tail = head;
            for (size_t i=1; i<count; ++i)
            {
                chunk_t * const curr = alloc_chunk();
                if (curr == NULL)
                    return NULL;

                curr->pPrev         = tail;
                tail->pNext         = curr;
                tail                = curr;
            }

            head->pPrev         = tail;     // Store pointer to the tail in the first element

            return release_ptr(head);
        }

        raw_ddeque::chunk_t *raw_ddeque::acquire_unused_chunk_list(size_t count)
        {
            chunk_t * const head    = pUnused;
            chunk_t *tail           = head;
            for (size_t i=1; i<count; ++i)
                tail                    = tail->pNext;

            pUnused                 = tail->pNext;
            tail->pNext             = NULL;
            if (pUnused != NULL)
                pUnused->pPrev          = NULL;

            head->pPrev             = tail;     // Store pointer to the tail in the first element
            nUnused                -= count;

            return head;
        }

        raw_ddeque::chunk_t *raw_ddeque::acquire_chunk_list(size_t count)
        {
            if (count <= nUnused)
                return acquire_unused_chunk_list(count);

            // Allocate chunk list
            const size_t to_alloc   = count - nUnused;
            chunk_t * const allocated = alloc_chunk_list(to_alloc);
            if (allocated == NULL)
                return NULL;
            if (nUnused <= 0)
            {
                nChunks                += to_alloc;
                return allocated;
            }

            // Add unused chunks
            chunk_t * const unused  = pUnused;
            pUnused                 = NULL;
            nUnused                 = 0;

            chunk_t * tail          = unused;
            while (tail->pNext != NULL)
                tail                    = tail->pNext;

            tail->pNext             = allocated;
            unused->pPrev           = allocated->pPrev;     // Store pointer to the tail in the first element
            allocated->pPrev        = tail;
            nChunks                += to_alloc;

            return unused;
        }

        void raw_ddeque::clear()
        {
            if (pTail != NULL)
            {
                if (pUnused != NULL)
                {
                    pTail->pNext    = pUnused;
                    pUnused->pPrev  = pTail;
                }

                pUnused         = pHead;
                nUnused         = nChunks;
            }

            // Clear state
            pHead           = NULL;
            pTail           = NULL;
            nItems          = 0;
        }

        void raw_ddeque::truncate()
        {
            free_chunk_list(pUnused);
            pUnused         = NULL;
            nChunks        -= nUnused;
            nUnused         = 0;
        }

        void raw_ddeque::flush()
        {
            free_chunk_list(pHead);
            free_chunk_list(pUnused);

            // Reset state
            pHead           = NULL;
            pTail           = NULL;
            pUnused         = NULL;
            nItems          = 0;
            nChunks         = 0;
            nUnused         = 0;
        }

        raw_ddeque::chunk_t *raw_ddeque::acquire_chunk()
        {
            chunk_t *chunk      = pUnused;
            if (chunk == NULL)
            {
                chunk = alloc_chunk();
                if (chunk != NULL)
                    ++nChunks;
                return chunk;
            }

            pUnused             = chunk->pNext;
            if (pUnused != NULL)
                pUnused->pPrev      = NULL;

            chunk->pNext        = NULL;
            chunk->pPrev        = NULL;
            --nUnused;

            return chunk;
        }

        void raw_ddeque::release_chunk(chunk_t * chunk)
        {
            chunk->nHead        = 0;
            chunk->nTail        = 0;

            chunk->pNext        = pUnused;
            chunk->pPrev        = NULL;

            if (pUnused != NULL)
                pUnused->pPrev      = chunk;
            pUnused             = chunk;
            ++nUnused;
        }

        raw_ddeque::chunk_t *raw_ddeque::alloc_chunk()
        {
            chunk_t *chunk          = static_cast<chunk_t *>(malloc(sizeof(chunk_t) + nChunkSize * nSizeOf));
            if (chunk == NULL)
                return chunk;

            chunk->pNext            = NULL;
            chunk->pPrev            = NULL;
            chunk->nHead            = 0;
            chunk->nTail            = 0;

            return chunk;
        }

        size_t raw_ddeque::chunks_count(size_t count)
        {
            return (count + nChunkSize - 1) / nChunkSize;
        }

        bool raw_ddeque::reserve(size_t capacity)
        {
            // Estimate current capacity
            const size_t cap        = nChunks * nChunkSize;
            if (cap >= capacity)
                return true;

            // Allocate the desired number of chunks
            const size_t to_alloc       = capacity - cap;
            const size_t n_chunks       = chunks_count(to_alloc);
            chunk_t * const allocated   = alloc_chunk_list(n_chunks);
            if (allocated == NULL)
                return false;

            if (pUnused != NULL)
                pUnused->pPrev              = allocated->pPrev;
            allocated->pPrev->pNext     = pUnused;
            allocated->pPrev            = NULL;
            pUnused                     = allocated;
            nChunks                    += n_chunks;
            nUnused                    += n_chunks;

            return true;
        }

        void *raw_ddeque::push_back(const void *data)
        {
            // Check that we have tail chunk
            if (pTail == NULL)
            {
                pTail               = acquire_chunk();
                if (pTail == NULL)
                    return NULL;

                pHead               = pTail;
            }

            // Check that tail chunk has place to store data
            if (pTail->nTail >= nChunkSize)
            {
                chunk_t *chunk      = acquire_chunk();
                if (chunk == NULL)
                    return NULL;

                chunk->pPrev        = pTail;
                pTail->pNext        = chunk;
                pTail               = chunk;
            }

            // Place data
            uint8_t * const dst     = &pTail->vData[(pTail->nTail++) * nSizeOf];
            memcpy(dst, data, nSizeOf);
            ++nItems;
            return dst;
        }

        void *raw_ddeque::push_back()
        {
            // Check that we have tail chunk
            if (pTail == NULL)
            {
                pTail               = acquire_chunk();
                if (pTail == NULL)
                    return NULL;

                pHead               = pTail;
            }

            // Check that tail chunk has place to store data
            if (pTail->nTail >= nChunkSize)
            {
                chunk_t *chunk      = acquire_chunk();
                if (chunk == NULL)
                    return NULL;

                chunk->pPrev        = pTail;
                pTail->pNext        = chunk;
                pTail               = chunk;
            }

            // Place data
            uint8_t * const dst     = &pTail->vData[(pTail->nTail++) * nSizeOf];
            ++nItems;
            return dst;
        }

        void *raw_ddeque::push_front(const void *data)
        {
            // Check that we have tail chunk
            if (pHead == NULL)
            {
                pHead               = acquire_chunk();
                if (pHead == NULL)
                    return NULL;

                pHead->nHead        = nChunkSize;
                pHead->nTail        = nChunkSize;

                pTail               = pHead;
            }

            // Check that head chunk has place to store data
            if (pHead->nHead <= 0)
            {
                chunk_t *chunk      = acquire_chunk();
                if (chunk == NULL)
                    return NULL;

                chunk->nHead        = nChunkSize;
                chunk->nTail        = nChunkSize;

                chunk->pNext        = pHead;
                pHead->pPrev        = chunk;
                pHead               = chunk;
            }

            // Place data
            uint8_t * const dst     = &pHead->vData[(--pHead->nHead) * nSizeOf];
            memcpy(dst, data, nSizeOf);
            ++nItems;
            return dst;
        }

        void *raw_ddeque::push_front()
        {
            // Check that we have tail chunk
            if (pHead == NULL)
            {
                pHead               = acquire_chunk();
                if (pHead == NULL)
                    return NULL;

                pHead->nHead        = nChunkSize;
                pHead->nTail        = nChunkSize;

                pTail               = pHead;
            }

            // Check that head chunk has place to store data
            if (pHead->nHead <= 0)
            {
                chunk_t *chunk      = acquire_chunk();
                if (chunk == NULL)
                    return NULL;

                chunk->nHead        = nChunkSize;
                chunk->nTail        = nChunkSize;

                chunk->pNext        = pHead;
                pHead->pPrev        = chunk;
                pHead               = chunk;
            }

            // Place data
            uint8_t * const dst     = &pHead->vData[(--pHead->nHead) * nSizeOf];
            ++nItems;
            return dst;
        }

        void *raw_ddeque::first()
        {
            if (nItems <= 0)
                return NULL;

            return &pHead->vData[pHead->nHead * nSizeOf];
        }

        void *raw_ddeque::first(void *data)
        {
            if (nItems <= 0)
                return NULL;

            void * const src = &pHead->vData[pHead->nHead * nSizeOf];
            memcpy(data, src, nSizeOf);
            return src;
        }

        void *raw_ddeque::last()
        {
            if (nItems <= 0)
                return NULL;

            return &pTail->vData[(pTail->nTail - 1) * nSizeOf];
        }

        void *raw_ddeque::last(void * data)
        {
            if (nItems <= 0)
                return NULL;

            void * const src = &pTail->vData[(pTail->nTail - 1) * nSizeOf];
            memcpy(data, src, nSizeOf);
            return src;
        }

        raw_ddeque::chunk_t *raw_ddeque::preallocate_back(size_t count)
        {
            size_t n_chunks;
            chunk_t *chunks;

            // Check if deque is empty
            if (pTail == NULL)
            {
                n_chunks                = chunks_count(count);
                chunks                  = acquire_chunk_list(n_chunks);
                if (chunks == NULL)
                    return NULL;

                pTail                   = chunks->pPrev;
                pHead                   = chunks;
                chunks->pPrev           = NULL;     // Remove pointer to the tail

                return chunks;
            }

            // Check that tail has enough elements to store
            if ((pTail->nTail + count) < nChunkSize)
                return pTail;

            // Allocate extra chunks and place them after tail
            n_chunks                = chunks_count(count + pTail->nTail - nChunkSize);
            chunks                  = acquire_chunk_list(n_chunks);
            if (chunks == NULL)
                return NULL;

            chunk_t * const old_tail= pTail;
            old_tail->pNext         = chunks;
            pTail                   = chunks->pPrev;
            chunks->pPrev           = old_tail;

            return old_tail;
        }

        raw_ddeque::chunk_t *raw_ddeque::preallocate_front(size_t count)
        {
            size_t n_chunks;
            chunk_t *chunks;

            // Check if deque is empty
            if (pHead == NULL)
            {
                n_chunks                = chunks_count(count);
                chunks                  = acquire_chunk_list(n_chunks);
                if (chunks == NULL)
                    return NULL;

                pTail                   = chunks->pPrev;
                pHead                   = chunks;
                chunks->pPrev           = NULL;     // Remove pointer to the tail

                pHead->nTail            = nChunkSize;
                pHead->nHead            = nChunkSize;

                return chunks;
            }

            // Check that head has enough elements to store
            if (pHead->nHead >= count)
                return pHead;

            // Allocate extra chunks and place them before head
            n_chunks                = chunks_count(count - pHead->nHead);
            chunks                  = acquire_chunk_list(n_chunks);
            if (chunks == NULL)
                return NULL;

            chunk_t * const old_head= pHead;
            chunk_t * const tail    = chunks->pPrev;

            tail->pNext             = old_head;
            old_head->pPrev         = tail;
            pHead                   = chunks;
            chunks->pPrev           = NULL;

            return old_head;
        }

        bool raw_ddeque::push_back(const void *data, size_t count)
        {
            chunk_t *tail           = preallocate_back(count);
            if (tail == NULL)
                return false;

            // Update size
            nItems                 += count;

            // Copy first block
            const uint8_t *src      = static_cast<const uint8_t *>(data);
            size_t to_copy          = lsp_min(count, nChunkSize - tail->nTail);

            forward_copy(&tail->vData[tail->nTail * nSizeOf], src, to_copy);
            tail->nTail            += to_copy;
            src                    += to_copy * nSizeOf;
            count                  -= to_copy;

            // Copy rest blocks
            while (count > 0)
            {
                tail                    = tail->pNext;
                to_copy                 = lsp_min(count, nChunkSize);

                forward_copy(tail->vData, src, to_copy);
                tail->nTail             = to_copy;
                src                    += to_copy * nSizeOf;
                count                  -= to_copy;
            }

            return true;
        }

        bool raw_ddeque::prepend(const void *data, size_t count)
        {
            chunk_t *head           = preallocate_front(count);
            if (head == NULL)
                return false;

            // Update size
            nItems                 += count;

            // Copy first block
            const uint8_t *src      = static_cast<const uint8_t *>(data) + count * nSizeOf;
            size_t to_copy          = lsp_min(count, head->nHead);

            src                    -= to_copy * nSizeOf;
            head->nHead            -= to_copy;
            forward_copy(&head->vData[head->nHead * nSizeOf], src, to_copy);
            count                  -= to_copy;

            // Copy rest blocks
            while (count > 0)
            {
                head                    = head->pPrev;
                to_copy                 = lsp_min(count, nChunkSize);

                src                    -= to_copy * nSizeOf;
                head->nTail             = nChunkSize;
                head->nHead             = nChunkSize - to_copy;
                forward_copy(&head->vData[head->nHead * nSizeOf], src, to_copy);
                count                  -= to_copy;
            }

            return true;
        }

        bool raw_ddeque::push_front(const void *data, size_t count)
        {
            chunk_t *head           = preallocate_front(count);
            if (head == NULL)
                return false;

            // Update size
            nItems                 += count;

            // Copy first block
            const uint8_t *src      = static_cast<const uint8_t *>(data);
            size_t to_copy          = lsp_min(count, head->nHead);

            head->nHead            -= to_copy;
            reverse_copy(&head->vData[head->nHead * nSizeOf], src, to_copy);
            src                    += to_copy * nSizeOf;
            count                  -= to_copy;

            // Copy rest blocks
            while (count > 0)
            {
                head                    = head->pPrev;
                to_copy                 = lsp_min(count, nChunkSize);

                head->nTail             = nChunkSize;
                head->nHead             = nChunkSize - to_copy;
                reverse_copy(&head->vData[head->nHead * nSizeOf], src, to_copy);
                src                    += to_copy * nSizeOf;
                count                  -= to_copy;
            }

            return true;
        }

        bool raw_ddeque::pop_back(void *data)
        {
            if (nItems <= 0)
                return false;

            memcpy(data, &pTail->vData[(--pTail->nTail) * nSizeOf], nSizeOf);
            --nItems;

            if (pTail->nTail <= pTail->nHead)
            {
                chunk_t * const chunk       = pTail;
                pTail                       = pTail->pPrev;
                if (pTail == NULL)
                    pHead                       = NULL;
                else
                    pTail->pNext                = NULL;
                release_chunk(chunk);
            }

            return true;
        }

        bool raw_ddeque::pop_back()
        {
            if (nItems <= 0)
                return false;

            --nItems;

            if ((--pTail->nTail) <= pTail->nHead)
            {
                chunk_t * const chunk       = pTail;
                pTail                       = pTail->pPrev;
                if (pTail == NULL)
                    pHead                       = NULL;
                else
                    pTail->pNext                = NULL;
                release_chunk(chunk);
            }

            return true;
        }

        bool raw_ddeque::pop_front(void *data)
        {
            if (nItems <= 0)
                return false;

            --nItems;

            memcpy(data, &pHead->vData[(pHead->nHead++) * nSizeOf], nSizeOf);

            if (pHead->nHead >= pHead->nTail)
            {
                chunk_t * const chunk       = pHead;
                pHead                       = pHead->pNext;
                if (pHead == NULL)
                    pTail                       = NULL;
                else
                    pHead->pPrev                = NULL;
                release_chunk(chunk);
            }

            return true;
        }

        bool raw_ddeque::pop_front()
        {
            if (nItems <= 0)
                return false;

            --nItems;

            if ((++pHead->nHead) >= pHead->nTail)
            {
                chunk_t * const chunk       = pHead;
                pHead                       = pHead->pNext;
                if (pHead == NULL)
                    pTail                       = NULL;
                else
                    pHead->pPrev                = NULL;
                release_chunk(chunk);
            }

            return true;
        }

        void *raw_ddeque::get(size_t index)
        {
            const size_t half = nItems >> 1;
            if (index < half)
            {
                // Search from the head
                for (chunk_t * curr = pHead; curr != NULL; curr = curr->pNext)
                {
                    const size_t used   = curr->nTail - curr->nHead;
                    if (index < used)
                        return &curr->vData[(curr->nHead + index) * nSizeOf];

                    index          -= used;
                }
            }
            else if (index < nItems)
            {
                index = nItems - index - 1;

                // Search from the tail
                for (chunk_t * curr = pTail; curr != NULL; curr = curr->pPrev)
                {
                    const size_t used   = curr->nTail - curr->nHead;
                    if (index < used)
                        return &curr->vData[(curr->nTail - index - 1) * nSizeOf];

                    index          -= used;
                }
            }

            return NULL;
        }

        ssize_t raw_ddeque::index_of(const void *ptr)
        {
            size_t index = 0;
            const uint8_t *xptr = static_cast<const uint8_t *>(ptr);
            const size_t end = nChunkSize * nSizeOf;

            // Search from the head
            for (const chunk_t * curr = pHead; curr != NULL; curr = curr->pNext)
            {
                // Check that pointer belongs to this chunk
                const size_t used   = curr->nTail - curr->nHead;
                if ((xptr >= curr->vData) && (xptr < &curr->vData[end]))
                {
                    size_t offset = (xptr - curr->vData) / nSizeOf;
                    if (offset < curr->nHead)
                        return -2;
                    if (offset >= curr->nTail)
                        return -3;

                    return (xptr == &curr->vData[offset * nSizeOf]) ? index + offset - curr->nHead : -4;
                }

                index          += used;
            }

            return -1;
        }

        size_t raw_ddeque::pop_front(void *data, size_t count)
        {
            count = lsp_min(count, nItems);
            if (count <= 0)
                return count;

            uint8_t *dst            = static_cast<uint8_t *>(data);

            for (size_t offset=0; offset<count; )
            {
                const size_t to_copy    = lsp_min(count - offset, pHead->nTail - pHead->nHead);

                // Copy data
                forward_copy(dst, &pHead->vData[pHead->nHead * nSizeOf], to_copy);
                dst                    += to_copy * nSizeOf;
                pHead->nHead           += to_copy;
                offset                 += to_copy;

                // Advance head if needed
                if (pHead->nHead >= pHead->nTail)
                {
                    chunk_t * const chunk       = pHead;
                    pHead                       = pHead->pNext;
                    if (pHead == NULL)
                        pTail                       = NULL;
                    else
                        pHead->pPrev                = NULL;
                    release_chunk(chunk);
                }
            }

            nItems                 -= count;

            return count;
        }

        size_t raw_ddeque::take_back(void *data, size_t count)
        {
            count = lsp_min(count, nItems);
            if (count <= 0)
                return count;

            uint8_t *dst            = static_cast<uint8_t *>(data) + count * nSizeOf;

            for (size_t offset=0; offset<count; )
            {
                const size_t to_copy    = lsp_min(count - offset, pTail->nTail - pTail->nHead);

                // Copy data
                dst                    -= to_copy * nSizeOf;
                pTail->nTail           -= to_copy;
                forward_copy(dst, &pTail->vData[pTail->nTail * nSizeOf], to_copy);
                offset                 += to_copy;

                // Advance tail if needed
                if (pTail->nTail <= pTail->nHead)
                {
                    chunk_t * const chunk       = pTail;
                    pTail                       = pTail->pPrev;
                    if (pTail == NULL)
                        pHead                       = NULL;
                    else
                        pTail->pNext                = NULL;
                    release_chunk(chunk);
                }
            }

            nItems                 -= count;

            return count;
        }

        size_t raw_ddeque::pop_back(void *data, size_t count)
        {
            count = lsp_min(count, nItems);
            if (count <= 0)
                return count;

            uint8_t *dst            = static_cast<uint8_t *>(data);

            for (size_t offset=0; offset<count; )
            {
                const size_t to_copy    = lsp_min(count - offset, pTail->nTail - pTail->nHead);

                // Copy data
                pTail->nTail           -= to_copy;
                reverse_copy(dst, &pTail->vData[pTail->nTail * nSizeOf], to_copy);
                dst                    += to_copy * nSizeOf;
                offset                 += to_copy;

                // Advance tail if needed
                if (pTail->nTail <= pTail->nHead)
                {
                    chunk_t * const chunk       = pTail;
                    pTail                       = pTail->pPrev;
                    if (pTail == NULL)
                        pHead                       = NULL;
                    else
                        pTail->pNext                = NULL;
                    release_chunk(chunk);
                }
            }

            nItems                 -= count;

            return count;
        }

        raw_iterator raw_ddeque::iter()
        {
            if (nItems <= 0)
                return raw_iterator::INVALID;

            return raw_iterator {
                &iterator_vtbl,
                this,
                pHead,
                0,
                pHead->nHead,
                0,
                false
            };
        }

        raw_iterator raw_ddeque::riter()
        {
            if (nItems <= 0)
                return raw_iterator::INVALID;

            return raw_iterator {
                &iterator_vtbl,
                this,
                pTail,
                nItems - 1,
                pTail->nTail - 1,
                0,
                true
            };
        }

        void raw_ddeque::iter_move(raw_iterator *i, ssize_t n)
        {
            chunk_t *chunk      = static_cast<chunk_t *>(i->item);
            size_t offset       = i->offset;
            size_t index        = i->index;

            while (n > 0)
            {
                const size_t to_do  = lsp_min(chunk->nTail - offset, size_t(n));
                offset             += to_do;
                index              += to_do;
                if (offset >= chunk->nTail)
                {
                    chunk               = chunk->pNext;
                    if (chunk == NULL)
                    {
                        *i                  = raw_iterator::INVALID;
                        return;
                    }
                    offset              = chunk->nHead;
                }

                n                  -= to_do;
            }

            while (n < 0)
            {
                if (offset <= chunk->nHead)
                {
                    chunk               = chunk->pPrev;
                    if (chunk == NULL)
                    {
                        *i                  = raw_iterator::INVALID;
                        return;
                    }
                    offset              = chunk->nTail;
                }

                const size_t to_do  = lsp_min(offset - chunk->nHead, size_t(-n));
                offset             -= to_do;
                index              -= to_do;
                n                  += to_do;
            }

            i->item             = chunk;
            i->offset           = offset;
            i->index            = index;
        }

        void *raw_ddeque::iter_get(raw_iterator *i)
        {
            raw_ddeque *self    = static_cast<raw_ddeque *>(i->container);
            chunk_t *chunk      = static_cast<chunk_t *>(i->item);
            return &chunk->vData[i->offset * self->nSizeOf];
        }

        ssize_t raw_ddeque::iter_compare(const raw_iterator *a, const raw_iterator *b)
        {
            return a->index - b->index;
        }

        size_t raw_ddeque::iter_count(const raw_iterator *i)
        {
            raw_ddeque *self    = static_cast<raw_ddeque *>(i->container);
            return self->nItems;
        }

    } /* namespace lltl */
} /* namespace lsp */



