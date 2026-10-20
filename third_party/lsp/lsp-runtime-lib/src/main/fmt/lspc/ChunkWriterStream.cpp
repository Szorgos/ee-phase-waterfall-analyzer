/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 20 окт. 2022 г.
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

#include <lsp-plug.in/fmt/lspc/ChunkWriterStream.h>

namespace lsp
{
    namespace lspc
    {
        ChunkWriterStream::ChunkWriterStream(ChunkWriter *writer, bool free)
        {
            pWriter = writer;
            bDelete = free;
        }

        ChunkWriterStream::~ChunkWriterStream()
        {
            do_close();
        }

        void ChunkWriterStream::do_close()
        {
            if (pWriter == NULL)
                return;
            if (bDelete)
                delete pWriter;
            pWriter = NULL;
        }

        wssize_t ChunkWriterStream::position()
        {
            if (pWriter == NULL)
                return -set_error(STATUS_CLOSED);
            wssize_t res = pWriter->position();
            if (res >= 0)
            {
                set_error(STATUS_OK);
                return res;
            }
            return -set_error(status_t(-res));
        }

        ssize_t ChunkWriterStream::write(const void *buf, size_t count)
        {
            if (pWriter == NULL)
                return -set_error(STATUS_CLOSED);
            status_t res = pWriter->write(buf, count);
            if (res != STATUS_OK)
                return -set_error(res);
            set_error(STATUS_OK);
            return count;
        }

        ssize_t ChunkWriterStream::writeb(int v)
        {
            if (pWriter == NULL)
                return -set_error(STATUS_CLOSED);
            uint8_t buf = v;
            status_t res = pWriter->write(&buf, sizeof(buf));
            if (res != STATUS_OK)
                return -set_error(res);
            set_error(STATUS_OK);
            return sizeof(buf);
        }

        status_t ChunkWriterStream::flush()
        {
            if (pWriter == NULL)
                return -set_error(STATUS_CLOSED);
            return set_error(pWriter->flush());
        }

        status_t ChunkWriterStream::close()
        {
            do_close();
            return set_error(STATUS_OK);
        }

    } /* namespace lspc */
} /* namespace lsp */
