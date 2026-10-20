/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 24 окт. 2022 г.
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

#include <lsp-plug.in/common/alloc.h>
#include <lsp-plug.in/common/endian.h>
#include <lsp-plug.in/fmt/lspc/ChunkReader.h>
#include <lsp-plug.in/fmt/lspc/ChunkWriter.h>
#include <lsp-plug.in/fmt/lspc/util/path.h>
#include <lsp-plug.in/stdlib/stdlib.h>
#include <lsp-plug.in/stdlib/string.h>

namespace lsp
{
    namespace lspc
    {
        //---------------------------------------------------------------------
        // Allocation/deallocation
        LSP_RUNTIME_LIB_PUBLIC
        path_entry_t *alloc_path(size_t path_len)
        {
            // Allocate entry
            size_t hdr_size     = align_size(sizeof(path_entry_t), sizeof(umword_t));
            size_t path_size    = align_size(path_len + 1, sizeof(umword_t));

            uint8_t *bytes      = static_cast<uint8_t *>(malloc(hdr_size + path_size));
            if (bytes == NULL)
                return NULL;

            path_entry_t *res   = reinterpret_cast<path_entry_t *>(bytes);
            char *str           = reinterpret_cast<char *>(&bytes[hdr_size]);

            // Fill entry with data
            res->path           = str;
            res->flags          = 0;
            res->chunk_id       = 0;

            return res;
        }

        LSP_RUNTIME_LIB_PUBLIC
        path_entry_t *alloc_path(const char *path, size_t flags, chunk_id_t reference_id)
        {
            if (path == NULL)
                return NULL;

            // Allocate entry
            size_t path_len     = strlen(path) + 1;
            path_entry_t *res   = alloc_path(path_len);
            if (res == NULL)
                return NULL;

            // Fill entry with data
            res->flags          = uint32_t(flags);
            res->chunk_id       = reference_id;
            memcpy(res->path, path, path_len);

            return res;
        }

        LSP_RUNTIME_LIB_PUBLIC
        path_entry_t *alloc_path(const io::Path *path, size_t flags, chunk_id_t reference_id)
        {
            if (path == NULL)
                return NULL;
            return alloc_path(path->as_utf8(), flags, reference_id);
        }

        LSP_RUNTIME_LIB_PUBLIC
        path_entry_t *alloc_path(const LSPString *path, size_t flags, chunk_id_t reference_id)
        {
            if (path == NULL)
                return NULL;
            return alloc_path(path->get_utf8(), flags, reference_id);
        }

        LSP_RUNTIME_LIB_PUBLIC
        void free_path(path_entry_t * path)
        {
            if (path != NULL)
                free(path);
        }

        //---------------------------------------------------------------------
        // Write operations
        LSP_RUNTIME_LIB_PUBLIC
        status_t write_path(chunk_id_t *chunk_id, File *file, const path_entry_t *path)
        {
            if (path == NULL)
                return STATUS_BAD_ARGUMENTS;

            // Create chunk and remember it's size
            status_t res;
            lspc::ChunkWriter *cw = file->write_chunk(LSPC_CHUNK_PATH);
            if (cw == NULL)
                return STATUS_BAD_STATE;
            lsp_finally { delete cw; };
            chunk_id_t res_chunk_id = cw->unique_id();

            // Create header
            size_t path_size    = strlen(path->path);
            if (path_size > UINT16_MAX)
                return STATUS_OVERFLOW;

            chunk_path_t cp;
            bzero(&cp, sizeof(cp));

            cp.common.size      = sizeof(cp);
            cp.common.version   = 0;
            cp.path_size        = path_size;
            cp.flags            = path->flags;
            cp.chunk_id         = path->chunk_id;

            // Convert endianess
            cp.path_size       = CPU_TO_BE(cp.path_size);
            cp.flags           = CPU_TO_BE(cp.flags);
            cp.chunk_id        = CPU_TO_BE(cp.chunk_id);

            if ((res = cw->write_header(&cp)) != STATUS_OK)
                return res;
            if ((res = cw->write(path->path, path_size)) != STATUS_OK)
                return res;
            if ((res = cw->close()) != STATUS_OK)
                return res;

            // Return result
            if (chunk_id != NULL)
                *chunk_id           = res_chunk_id;

            return STATUS_OK;
        }

        LSP_RUNTIME_LIB_PUBLIC
        status_t write_path(chunk_id_t *chunk_id, File *file, const char *path, size_t flags, chunk_id_t reference_id)
        {
            if (path == NULL)
                return STATUS_BAD_ARGUMENTS;

            // Convert path to unix-like path string
            LSPString tmp;
            if (!tmp.set_utf8(path))
                return STATUS_NO_MEM;
            tmp.replace_all('\\', '/');

            // Write the entry
            path_entry_t pe;
            pe.path     = const_cast<char *>(tmp.get_utf8());
            pe.flags    = uint32_t(flags);
            pe.chunk_id = reference_id;

            return write_path(chunk_id, file, &pe);
        }

        LSP_RUNTIME_LIB_PUBLIC
        status_t write_path(chunk_id_t *chunk_id, File *file, const LSPString *path, size_t flags, chunk_id_t reference_id)
        {
            if (path == NULL)
                return STATUS_BAD_ARGUMENTS;

            // Convert path to unix-like path string
            LSPString tmp;
            if (!tmp.set(path))
                return STATUS_NO_MEM;
            tmp.replace_all('\\', '/');

            // Write the entry
            path_entry_t pe;
            pe.path     = const_cast<char *>(tmp.get_utf8());
            pe.flags    = uint32_t(flags);
            pe.chunk_id = reference_id;

            return write_path(chunk_id, file, &pe);
        }

        LSP_RUNTIME_LIB_PUBLIC
        status_t write_path(chunk_id_t *chunk_id, File *file, const io::Path *path, size_t flags, chunk_id_t reference_id)
        {
            if (path == NULL)
                return STATUS_BAD_ARGUMENTS;

            // Convert path to unix-like path string
            LSPString tmp;
            status_t res = path->get(&tmp);
            if (res != STATUS_OK)
                return res;
            tmp.replace_all('\\', '/');

            // Write the entry
            path_entry_t pe;
            pe.path     = const_cast<char *>(tmp.get_utf8());
            pe.flags    = uint32_t(flags);
            pe.chunk_id = reference_id;

            return write_path(chunk_id, file, &pe);
        }

        //---------------------------------------------------------------------
        // Read operations
        LSP_RUNTIME_LIB_PUBLIC
        status_t read_path(chunk_id_t chunk_id, File *file, path_entry_t **path)
        {
            // Get the chunk
            if (file == NULL)
                return STATUS_BAD_ARGUMENTS;
            ChunkReader *rd = file->read_chunk(chunk_id, LSPC_CHUNK_PATH);
            if (rd == NULL)
                return STATUS_NOT_FOUND;
            lsp_finally { delete rd; };

            // Read the header
            chunk_path_t hdr;
            ssize_t read = rd->read_header(&hdr, sizeof(hdr));
            if (read < 0)
                return status_t(-read);
            else if (read != sizeof(hdr))
                return STATUS_CORRUPTED;
            else if (hdr.common.version != 0)
                return STATUS_NOT_SUPPORTED;

            // Nothing to return?
            if (path == NULL)
                return STATUS_OK;

            // Convert the header
            hdr.path_size   = BE_TO_CPU(hdr.path_size);
            hdr.flags       = BE_TO_CPU(hdr.flags);
            hdr.chunk_id    = BE_TO_CPU(hdr.chunk_id);

            // Allocate path entry
            path_entry_t *pe= alloc_path(hdr.path_size);
            if (pe == NULL)
                return STATUS_NO_MEM;
            lsp_finally {
                if (pe != NULL)
                    free_path(pe);
            };

            pe->flags       = hdr.flags;
            pe->chunk_id    = hdr.chunk_id;

            // Read the path string
            read = rd->read(pe->path, hdr.path_size);
            if (read < 0)
                return status_t(-read);
            else if (read != hdr.path_size)
                return STATUS_CORRUPTED;
            pe->path[hdr.path_size] = '\0';    // Do not forget the trailing zero

            // Close the reader
            status_t res    = rd->close();
            if (res != STATUS_OK)
                return res;

            // Return the result
            *path           = pe;
            pe             = NULL;

            return STATUS_OK;
        }

        LSP_RUNTIME_LIB_PUBLIC
        status_t read_path(chunk_id_t chunk_id, File *file, char **path, size_t *flags, chunk_id_t *reference_id)
        {
            // Read path entry
            path_entry_t *entry = NULL;
            status_t res = read_path(chunk_id, file, &entry);
            if (res != STATUS_OK)
                return res;
            else if (entry == NULL)
                return STATUS_NO_MEM;
            lsp_finally { free_path(entry); };

            // Pass the arguments to the caller
            if (path != NULL)
            {
                char *p     = strdup(entry->path);
                if (p == NULL)
                    return STATUS_NO_MEM;
                *path       = p;
            }
            if (flags != NULL)
                *flags      = entry->flags;
            if (reference_id != NULL)
                *reference_id   = entry->chunk_id;

            return STATUS_OK;
        }

        LSP_RUNTIME_LIB_PUBLIC
        status_t read_path(chunk_id_t chunk_id, File *file, io::Path *path, size_t *flags, chunk_id_t *reference_id)
        {
            // Read path entry
            path_entry_t *entry = NULL;
            status_t res = read_path(chunk_id, file, &entry);
            if (res != STATUS_OK)
                return res;
            else if (entry == NULL)
                return STATUS_NO_MEM;
            lsp_finally { free_path(entry); };

            // Pass the arguments to the caller
            if (path != NULL)
            {
                if ((res = path->set(entry->path)) != STATUS_OK)
                    return res;
            }
            if (flags != NULL)
                *flags      = entry->flags;
            if (reference_id != NULL)
                *reference_id   = entry->chunk_id;

            return STATUS_OK;
        }

        status_t read_path(chunk_id_t chunk_id, File *file, LSPString *path, size_t *flags, chunk_id_t *reference_id)
        {
            // Read path entry
            path_entry_t *entry = NULL;
            status_t res = read_path(chunk_id, file, &entry);
            if (res != STATUS_OK)
                return res;
            else if (entry == NULL)
                return STATUS_NO_MEM;
            lsp_finally { free_path(entry); };

            // Pass the arguments to the caller
            if (path != NULL)
            {
                if (!path->set_utf8(entry->path))
                    return STATUS_NO_MEM;
            }
            if (flags != NULL)
                *flags      = entry->flags;
            if (reference_id != NULL)
                *reference_id   = entry->chunk_id;

            return STATUS_OK;
        }
    } /* namespace lspc */
} /* namespace lsp */

