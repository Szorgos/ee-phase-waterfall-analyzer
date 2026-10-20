/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 21 окт. 2022 г.
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

#include <lsp-plug.in/fmt/lspc/ChunkReaderStream.h>

namespace lsp
{
    namespace lspc
    {
        ChunkReaderStream::ChunkReaderStream(ChunkReader *reader, bool free)
        {
            pReader = reader;
            bDelete = free;
        }

        ChunkReaderStream::~ChunkReaderStream()
        {
            do_close();
        }

        void ChunkReaderStream::do_close()
        {
            if (pReader == NULL)
                return;
            if (bDelete)
                delete pReader;
            pReader = NULL;
        }

        wssize_t ChunkReaderStream::position()
        {
            if (pReader == NULL)
                return -set_error(STATUS_CLOSED);
            wssize_t res = pReader->position();
            if (res >= 0)
            {
                set_error(STATUS_OK);
                return res;
            }
            return -set_error(status_t(-res));
        }

        ssize_t ChunkReaderStream::read(void *dst, size_t count)
        {
            if (pReader == NULL)
                return -set_error(STATUS_CLOSED);
            ssize_t res = pReader->read(dst, count);
            if (res > 0)
            {
                set_error(STATUS_OK);
                return res;
            }
            else if (res == 0)
                return -set_error(STATUS_EOF);
            return -set_error(status_t(-res));
        }

        ssize_t ChunkReaderStream::read_byte()
        {
            if (pReader == NULL)
                return -set_error(STATUS_CLOSED);
            uint8_t buf;
            ssize_t res = pReader->read(&buf, sizeof(buf));
            if (res > 0)
            {
                set_error(STATUS_OK);
                return buf;
            }
            else if (res == 0)
                return -set_error(STATUS_EOF);
            return -set_error(status_t(-res));
        }

        wssize_t ChunkReaderStream::skip(wsize_t amount)
        {
            if (pReader == NULL)
                return -set_error(STATUS_CLOSED);
            ssize_t res = pReader->skip(amount);
            if (res >= 0)
            {
                set_error(STATUS_OK);
                return res;
            }
            return -set_error(status_t(-res));
        }

        status_t ChunkReaderStream::close()
        {
            do_close();
            return set_error(STATUS_OK);
        }
    } /* namespace lspc */
} /* namespace lsp */



