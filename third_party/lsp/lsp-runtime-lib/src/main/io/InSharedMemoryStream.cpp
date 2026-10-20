/*
 * Copyright (C) 2023 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2023 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 22 февр. 2023 г.
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

#include <lsp-plug.in/io/InSharedMemoryStream.h>
#include <lsp-plug.in/stdlib/string.h>
#include <stdlib.h>

namespace lsp
{
    namespace io
    {

        InSharedMemoryStream::InSharedMemoryStream()
        {
            pShared = NULL;
            nOffset = 0;
        }


        InSharedMemoryStream::~InSharedMemoryStream()
        {
            release_shared();
        }

        InSharedMemoryStream::InSharedMemoryStream(void *data, size_t size, lsp_memdrop_t drop)
        {
            shared_data_t *shared = static_cast<shared_data_t *>(malloc(sizeof(shared_data_t)));
            if (shared != NULL)
            {
                shared->pData   = static_cast<uint8_t *>(data);
                shared->nSize   = size;
                shared->nRefs   = 1;
                shared->enDrop  = drop;
            }

            pShared = shared;
            nOffset = 0;
        }

        InSharedMemoryStream::InSharedMemoryStream(const void *data, size_t size)
        {
            shared_data_t *shared = static_cast<shared_data_t *>(malloc(sizeof(shared_data_t)));
            if (shared != NULL)
            {
                shared->pData   = reinterpret_cast<uint8_t *>(const_cast<void *>(data));
                shared->nSize   = size;
                shared->nRefs   = 1;
                shared->enDrop  = MEMDROP_NONE;
            }

            pShared = shared;
            nOffset = 0;
        }

        InSharedMemoryStream::InSharedMemoryStream(const InSharedMemoryStream *src)
        {
            pShared     = src->pShared;
            nOffset     = src->nOffset;
            if (pShared != NULL)
                ++pShared->nRefs;
        }

        InSharedMemoryStream::InSharedMemoryStream(const InSharedMemoryStream &src)
        {
            pShared     = src.pShared;
            nOffset     = src.nOffset;
            if (pShared != NULL)
                ++pShared->nRefs;
        }

        InSharedMemoryStream::InSharedMemoryStream(InSharedMemoryStream && src)
        {
            pShared     = src.pShared;
            nOffset     = src.nOffset;
            src.pShared = NULL;
            src.nOffset = 0;
        }

        InSharedMemoryStream & InSharedMemoryStream::operator = (const InSharedMemoryStream & src)
        {
            release_shared();

            pShared     = src.pShared;
            nOffset     = src.nOffset;
            if (pShared != NULL)
                ++pShared->nRefs;
            return *this;
        }

        InSharedMemoryStream & InSharedMemoryStream::operator = (InSharedMemoryStream && src)
        {
            release_shared();

            pShared     = src.pShared;
            nOffset     = src.nOffset;
            src.pShared = NULL;
            src.nOffset = 0;
            return *this;
        }

        void InSharedMemoryStream::release_shared()
        {
            if (pShared == NULL)
                return;
            if ((--pShared->nRefs) == 0)
            {
                switch (pShared->enDrop)
                {
                    case MEMDROP_FREE: free(pShared->pData); break;
                    case MEMDROP_DELETE: delete pShared->pData; break;
                    case MEMDROP_ARR_DELETE: delete [] pShared->pData; break;
                    default: break;
                }
                free(pShared);
            }
            pShared     = NULL;
            nOffset     = 0;
        }

        status_t InSharedMemoryStream::wrap(void *data, size_t size, lsp_memdrop_t drop)
        {
            shared_data_t *shared = static_cast<shared_data_t *>(malloc(sizeof(shared_data_t)));
            if (shared == NULL)
                return STATUS_NO_MEM;

            release_shared();

            shared->pData   = static_cast<uint8_t *>(data);
            shared->nSize   = size;
            shared->nRefs   = 1;
            shared->enDrop  = drop;

            pShared = shared;
            nOffset = 0;

            return STATUS_OK;
        }

        status_t InSharedMemoryStream::wrap(const void *data, size_t size)
        {
            shared_data_t *shared = static_cast<shared_data_t *>(malloc(sizeof(shared_data_t)));
            if (shared == NULL)
                return STATUS_NO_MEM;

            release_shared();

            shared->pData   = reinterpret_cast<uint8_t *>(const_cast<void *>(data));
            shared->nSize   = size;
            shared->nRefs   = 1;
            shared->enDrop  = MEMDROP_NONE;

            pShared = shared;
            nOffset = 0;

            return STATUS_OK;
        }

        void InSharedMemoryStream::wrap(const InSharedMemoryStream &src)
        {
            release_shared();

            pShared     = src.pShared;
            nOffset     = src.nOffset;
            if (pShared != NULL)
                ++pShared->nRefs;
        }

        void InSharedMemoryStream::wrap(const InSharedMemoryStream *src)
        {
            release_shared();

            pShared     = src->pShared;
            nOffset     = src->nOffset;
            if (pShared != NULL)
                ++pShared->nRefs;
        }

        void InSharedMemoryStream::wrap(const InSharedMemoryStream &src, wsize_t offset)
        {
            release_shared();

            pShared     = src.pShared;
            nOffset     = offset;
            if (pShared != NULL)
                ++pShared->nRefs;
        }

        void InSharedMemoryStream::wrap(const InSharedMemoryStream *src, wsize_t offset)
        {
            release_shared();

            pShared     = src->pShared;
            nOffset     = offset;
            if (pShared != NULL)
                ++pShared->nRefs;
        }

        wssize_t InSharedMemoryStream::avail()
        {
            if (pShared == NULL)
                return -set_error(STATUS_NO_DATA);
            return pShared->nSize - nOffset;
        }

        wssize_t InSharedMemoryStream::position()
        {
            if (pShared == NULL)
                return -set_error(STATUS_NO_DATA);
            return nOffset;
        }

        ssize_t InSharedMemoryStream::read(void *dst, size_t count)
        {
            if (pShared == NULL)
                return -set_error(STATUS_NO_DATA);

            size_t avail = pShared->nSize - nOffset;
            if (count > avail)
                count = avail;
            if (count <= 0)
                return -set_error(STATUS_EOF);

            ::memcpy(dst, &pShared->pData[nOffset], count);
            nOffset    += count;
            return count;
        }

        ssize_t InSharedMemoryStream::read_byte()
        {
            if (pShared == NULL)
                return -set_error(STATUS_NO_DATA);
            return (nOffset < pShared->nSize) ? pShared->pData[nOffset++] : -STATUS_EOF;
        }

        wssize_t InSharedMemoryStream::seek(wsize_t position)
        {
            if (pShared == NULL)
                return -set_error(STATUS_NO_DATA);
            if (position > pShared->nSize)
                position = pShared->nSize;

            return nOffset = position;
        }

        wssize_t InSharedMemoryStream::skip(wsize_t amount)
        {
            if (pShared == NULL)
                return -set_error(STATUS_NO_DATA);
            size_t avail = pShared->nSize - nOffset;
            if (avail > amount)
                avail       = amount;
            nOffset    += avail;
            return avail;
        }

        status_t InSharedMemoryStream::close()
        {
            release_shared();
            return STATUS_OK;
        }

        void InSharedMemoryStream::swap(InSharedMemoryStream &src)
        {
            lsp::swap(pShared, src.pShared);
            lsp::swap(nOffset, src.nOffset);
        }

        void InSharedMemoryStream::swap(InSharedMemoryStream *src)
        {
            lsp::swap(pShared, src->pShared);
            lsp::swap(nOffset, src->nOffset);
        }

        void InSharedMemoryStream::take(InSharedMemoryStream &src)
        {
            release_shared();

            pShared         = src.pShared;
            nOffset         = src.nOffset;

            src.pShared     = NULL;
            src.nOffset     = 0;
        }

        void InSharedMemoryStream::take(InSharedMemoryStream *src)
        {
            release_shared();

            pShared         = src->pShared;
            nOffset         = src->nOffset;

            src->pShared    = NULL;
            src->nOffset    = 0;
        }

        status_t InSharedMemoryStream::take(OutMemoryStream &src)
        {
            release_shared();
            if (src.data() == NULL)
                return STATUS_OK;

            status_t res = wrap(const_cast<uint8_t *>(src.data()), src.size(), MEMDROP_FREE);
            if (res == STATUS_OK)
                src.release();
            return res;
        }

        status_t InSharedMemoryStream::take(OutMemoryStream *src)
        {
            release_shared();
            if (src->data() == NULL)
                return STATUS_OK;

            status_t res = wrap(const_cast<uint8_t *>(src->data()), src->size(), MEMDROP_FREE);
            if (res == STATUS_OK)
                src->release();
            return res;
        }

    } /* namespace io */
} /* namespace lsp */


