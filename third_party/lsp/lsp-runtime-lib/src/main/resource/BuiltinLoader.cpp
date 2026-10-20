/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 15 мар. 2021 г.
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

#include <lsp-plug.in/common/debug.h>
#include <lsp-plug.in/lltl/darray.h>
#include <lsp-plug.in/resource/BuiltinLoader.h>
#include <lsp-plug.in/resource/Decompressor.h>

namespace lsp
{
    namespace resource
    {
        BuiltinLoader::BuiltinLoader()
        {
            pData       = NULL;
            nDataSize   = 0;
            pCatalog    = NULL;
            nCatSize    = 0;
            nBufRank    = 0;
        }

        BuiltinLoader::~BuiltinLoader()
        {
            pData       = NULL;
            nDataSize   = 0;
            pCatalog    = NULL;
            nCatSize    = 0;
            nBufRank    = 0;
        }

        status_t BuiltinLoader::init(
            const void *data, size_t data_size,
            const raw_resource_t *catalog, size_t catalog_size,
            size_t log_buf_size
        )
        {
            pData       = reinterpret_cast<const uint8_t *>(data);
            nDataSize   = data_size;
            pCatalog    = catalog;
            nCatSize    = catalog_size;
            nBufRank    = log_buf_size;

            return STATUS_OK;
        }

        status_t BuiltinLoader::find_entry(ssize_t *out, const io::Path *path)
        {
            status_t res;
            ssize_t index = -1;
            LSPString item;
            io::Path tmp;

            // Copy path data
            if ((res = tmp.set(path)) != STATUS_OK)
                return res;

            while (true)
            {
                // Get first item and remove it
                if ((res = tmp.remove_first(&item)) != STATUS_OK)
                    return res;

                // Lookup for existing directory/create yet another one
                const raw_resource_t *found = NULL;
                for (size_t i=0; i<nCatSize; ++i)
                {
                    const raw_resource_t *ent   = &pCatalog[i];

                    if ((ent == NULL) || (ent->parent != index) || (ent->name == NULL))
                        continue;
                    if (item.equals_utf8(ent->name))
                    {
                        found                   = ent;
                        index                   = i;
                        break;
                    }
                }

                // Check that item exists
                if (found == NULL)
                    return STATUS_NOT_FOUND;

                // Last entry?
                if (tmp.is_empty())
                {
                    *out    = index;
                    return STATUS_OK;
                }
                else if (found->type != RES_DIR)
                    return STATUS_NOT_FOUND;
            }
        }

        io::IInStream *BuiltinLoader::read_stream(const io::Path *name)
        {
            ssize_t index = 0;

            // Find entry
            status_t res = find_entry(&index, name);
            if (res != STATUS_OK)
            {
                set_error(res);
                return NULL;
            }

            // Check type
            const raw_resource_t *ent = &pCatalog[index];
            if (ent->type != RES_FILE)
            {
                set_error(STATUS_IS_DIRECTORY);
                return NULL;
            }

            // Create decompressor
            Decompressor *d = new Decompressor();
            if (d == NULL)
            {
                set_error(STATUS_NO_MEM);
                return NULL;
            }

            // Initialize decompressor and skip the desired amount of data to access the entry
            res = d->init(&pData[ent->segment], nDataSize - ent->segment, ent->offset + ent->length, nBufRank);
            if (res == STATUS_OK)
            {
                wssize_t skipped = d->skip(ent->offset);
                if (skipped != ent->offset)
                    res = (skipped < 0) ? status_t(-skipped) : STATUS_CORRUPTED;
            }

            if (res == STATUS_OK)
                return d;

            delete d;
            set_error(res);
            return NULL;
        }

        ssize_t BuiltinLoader::enumerate(const io::Path *path, resource_t **list)
        {
            ssize_t index = 0;
            lltl::darray<resource_t> xlist;
            const raw_resource_t *ent;

            // Root directory?
            if ((path->is_empty()) || (path->equals(FILE_SEPARATOR_S)))
                index = -1;
            else
            {
                // Find entry and check that it is of directory type
                status_t res = find_entry(&index, path);
                if (res != STATUS_OK)
                    return -set_error(res);

                ent = &pCatalog[index];
                if (ent->type != RES_DIR)
                    return -set_error(STATUS_NOT_DIRECTORY);
            }

            // Now create list of nested items
            for (size_t i=0; i<nCatSize; ++i)
            {
                ent = &pCatalog[i];
                if ((ent == NULL) || (ent->parent != index) || (ent->name == NULL))
                    continue;

                resource_t *item = xlist.add();
                if (item == NULL)
                    return -set_error(STATUS_NO_MEM);

                strncpy(item->name, ent->name, RESOURCE_NAME_MAX);
                item->name[RESOURCE_NAME_MAX - 1] = '\0';
                item->type      = ent->type;
            }

            // Return result
            index = xlist.size();
            *list = xlist.release();

            set_error(STATUS_OK);
            return index;
        }
    } /* namespace resource */
} /* namespace lsp */


