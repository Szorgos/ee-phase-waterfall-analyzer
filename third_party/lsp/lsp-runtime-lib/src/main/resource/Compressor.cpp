/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 1 мар. 2021 г.
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

#include <lsp-plug.in/stdlib/string.h>
#include <lsp-plug.in/stdlib/stdio.h>
#include <lsp-plug.in/common/debug.h>
#include <lsp-plug.in/io/OutSequence.h>
#include <lsp-plug.in/io/OutBitStream.h>
#include <lsp-plug.in/common/bits.h>
#include <lsp-plug.in/common/alloc.h>
#include <lsp-plug.in/resource/Compressor.h>
#include <lsp-plug.in/resource/OutProxyStream.h>

namespace lsp
{
    enum compress_cmd_t
    {
        COMMAND_APPEND_7B,
        COMMAND_APPEND_8B,

        COMMAND_TOTAL,
        COMMAND_MAX     = COMMAND_TOTAL - 1
    };

    namespace resource
    {
        Compressor::Compressor()
        {
            nSegment        = 0;
            nOffset         = 0;
//            hFD             = fopen("/tmp/compressor.log", "w");
        }

        Compressor::~Compressor()
        {
            close();
        }

        status_t Compressor::close()
        {
            // Drop nodes
            sBuffer.destroy();

            // Drop entries
            for (size_t i=0, n=vEntries.size(); i < n; ++i)
            {
                raw_resource_t *res = vEntries.uget(i);
                if (res == NULL)
                    continue;
                if (res->name != NULL)
                    free(const_cast<char *>(res->name));
                res->name   = NULL;
            }
            vEntries.flush();

            // Drop buffer data
            sTemp.drop();

            status_t res    = sTemp.close();
            res             = update_status(res, sOut.close());
            res             = update_status(res, sOS.close());

            return res;
        }

        status_t Compressor::init(size_t log_buf_size, io::IOutStream *os, size_t flags)
        {
            status_t res;

            if (sBuffer.data != NULL)
                return STATUS_BAD_STATE;

            if ((res = sBuffer.init(log_buf_size)) != STATUS_OK)
                return res;

            if ((res = sOS.wrap(os, flags)) != STATUS_OK)
                return res;

            if ((res = sOut.wrap(&sOS)) != STATUS_OK)
                return res;

            return STATUS_OK;
        }

        status_t Compressor::alloc_entry(raw_resource_t **r, io::Path *path, resource_type_t type)
        {
            if (path->is_empty())
                return STATUS_BAD_ARGUMENTS;

            ssize_t         index = -1, sindex = -1;
            LSPString       item;
            status_t        res;

            while (true)
            {
                // Get first item and remove it
                if ((res = path->remove_first(&item)) != STATUS_OK)
                    return res;

                // Lookup for existing directory/create yet another one
                raw_resource_t *found       = NULL;
                for (size_t i=0, n=vEntries.size(); i<n; ++i)
                {
                    raw_resource_t *ent         = vEntries.uget(i);
                    if ((ent == NULL) || (ent->parent != index) || (ent->name == NULL))
                        continue;
                    if (item.equals_utf8(ent->name))
                    {
                        found                   = ent;
                        sindex                  = i;
                        break;
                    }
                }

                // Last entry?
                if (path->is_empty())
                {
                    // Item already exists?
                    if (found != NULL)
                        return STATUS_ALREADY_EXISTS;

                    // Add new resource entry
                    found   = vEntries.add();
                    if (found == NULL)
                        return STATUS_NO_MEM;

                    // Initialize
                    found->type     = type;
                    found->parent   = int32_t(index);
                    found->segment  = -1;
                    found->offset   = -1;
                    found->length   = 0;
                    found->name     = item.clone_utf8();

                    if (found->name == NULL)
                        return STATUS_NO_MEM;

                    // Return the pointer
                    if (r != NULL)
                        *r              = found;
                    return STATUS_OK;
                }
                else
                {
                    // Need to create new one?
                    if (found == NULL)
                    {
                        // Allocate item
                        sindex          = vEntries.size();
                        if ((found = vEntries.add()) == NULL)
                            return STATUS_NO_MEM;

                        found->type     = RES_DIR;
                        found->parent   = int32_t(index);
                        found->segment  = -1;
                        found->offset   = -1;
                        found->length   = 0;
                        found->name     = item.clone_utf8();

                        if (found->name == NULL)
                            return STATUS_NO_MEM;
                    }
                    else if (found->type != RES_DIR)
                        return STATUS_BAD_TYPE; // Bad entry type

                    // Update position
                    index       = sindex;
                }
            }
        }

        wssize_t Compressor::write_entry(raw_resource_t *r, io::IInStream *is)
        {
            if (sBuffer.data == NULL)
                return -STATUS_BAD_STATE;

            // Clear data
            sTemp.clear();
            wssize_t flength    = is->sink(&sTemp);
            if (flength < 0)
                return flength;

            status_t res = STATUS_OK;
            const uint8_t *head = sTemp.data();
            const uint8_t *tail = &head[flength];
            size_t offset = 0;

//            IF_TRACE(
//                wssize_t coffset    = sOS.position();
//                size_t octets       = 0;
//                size_t replays      = 0;
//                size_t repeats      = 0;
//            )

            while (head < tail)
            {
                // Estimate the length of match
                const size_t length = sBuffer.lookup(&offset, head, tail-head);

                // Estimate size of output
                const size_t est1   = est_uint(sBuffer.size() + *head, 5, 5) * length; // How many bits used to emit octet command
                const size_t est2   = (length > 0) ? est_uint(offset, 5, 5) + est_uint(length - 1, 5, 5) : est1 + 1;    // How many bits used to encode buffer replay command

                if (est2 < est1) // Prefer buffer replay over octet emission
                {
                    const size_t repeats    = calc_repeats(&head[length], tail);

//                    fprintf(hFD, "BUFFER replays=%d, off=%d, length=%d, sequence=",
//                        int(repeats), int(offset), int(length));
//                    for (size_t i=0; i<length; ++i)
//                        fprintf(hFD, "%02x ", head[i]);
//                    for (size_t i=0; i<repeats; ++i)
//                        fprintf(hFD, "%02x ", head[length - 1]);
//                    fprintf(hFD, "\n");

                    // REPLAY BUFFER
                    // Emit Offset
                    if ((res = emit_uint(offset, 5, 5)) != STATUS_OK)
                        break;
                    // Emit Length - 1
                    if ((res = emit_uint(length - 1, 5, 5)) != STATUS_OK)
                        break;
                    // Emit Repeat counter
                    if ((res = emit_uint(repeats, 0, 4)) != STATUS_OK)
                        break;

                    // Append data to buffer
                    sBuffer.append(head, length + lsp_min(repeats, REPEAT_BUF_MAX));
                    head           += length + repeats;

//                    IF_TRACE(
//                        ++replays;
//                        if (rep)
//                            ++repeats;
//                    );
                }
                else
                {
                    const size_t repeats    = calc_repeats(&head[1], tail);

//                    fprintf(hFD, "OCTET  replays=%d, sequence=%02x ",
//                        int(repeats), int(*head));
//                    for (size_t i=0; i<repeats; ++i)
//                        fprintf(hFD, "%02x ", int(*head));
//                    fprintf(hFD, "\n");

                    // EMIT OCTET
                    // Emit Value
                    if ((res = emit_uint(sBuffer.size() + *head, 5, 5)) != STATUS_OK)
                        break;
                    // Emit Repeat counter
                    if ((res = emit_uint(repeats, 0, 4)) != STATUS_OK)
                        break;

                    // Append data to buffer
                    sBuffer.append(head, 1 + lsp_min(repeats, REPEAT_BUF_MAX));
                    head           += 1 + repeats;
//                    IF_TRACE(
//                        ++octets;
//                        if (rep)
//                            ++repeats;
//                    );
                }
            }

            // Flush the bit sequence
            if (res != STATUS_OK)
                return -res;

            // Output stats
//            IF_TRACE(
//                size_t cbytes   = sOS.position() - coffset;
//
//                lsp_trace("  octets: %d, replays: %d, repeats: %d",
//                        int(octets), int(replays), int(repeats));
//                lsp_trace("  original size: %d, compressed size: %d, ratio: %.2f",
//                        int(flength), int(cbytes), double(flength) / double(cbytes));
//            )

            // Remember the actual coordinates of the entry within data array
            r->segment          = int32_t(nSegment);
            r->offset           = int32_t(nOffset);
            r->length           = int32_t(flength);
            nOffset            += flength;

//            lsp_trace("  compressed entry segment=%d, offset=%d length=%d",
//                    int(r->segment), int(r->offset), int(r->length));

            return r->length;
        }

        wssize_t Compressor::create_file(const char *name, io::IInStream *is)
        {
            io::Path tmp;
            raw_resource_t *r = NULL;

            status_t res = tmp.set(name);
            if (res == STATUS_OK)
                res     = alloc_entry(&r, &tmp, RES_FILE);

            return (res == STATUS_OK) ? write_entry(r, is) : - res;
        }

        wssize_t Compressor::create_file(const LSPString *name, io::IInStream *is)
        {
            io::Path tmp;
            raw_resource_t *r = NULL;

            status_t res = tmp.set(name);
            if (res == STATUS_OK)
                res     = alloc_entry(&r, &tmp, RES_FILE);

            return (res == STATUS_OK) ? write_entry(r, is) : - res;
        }

        wssize_t Compressor::create_file(const io::Path *name, io::IInStream *is)
        {
            io::Path tmp;
            raw_resource_t *r = NULL;

            status_t res = tmp.set(name);
            if (res == STATUS_OK)
                res     = alloc_entry(&r, &tmp, RES_FILE);

            return (res == STATUS_OK) ? write_entry(r, is) : - res;
        }

        status_t Compressor::create_dir(const char *name)
        {
            io::Path tmp;

            status_t res = tmp.set(name);
            if (res == STATUS_OK)
                res     = alloc_entry(NULL, &tmp, RES_DIR);

            return res;
        }

        status_t Compressor::create_dir(const LSPString *name)
        {
            io::Path tmp;

            status_t res = tmp.set(name);
            if (res == STATUS_OK)
                res     = alloc_entry(NULL, &tmp, RES_DIR);

            return res;
        }

        status_t Compressor::create_dir(const io::Path *name)
        {
            io::Path tmp;

            status_t res = tmp.set(name);
            if (res == STATUS_OK)
                res     = alloc_entry(NULL, &tmp, RES_DIR);

            return res;
        }

        status_t Compressor::emit_uint(size_t value, size_t initial, size_t stepping)
        {
            status_t res;
            size_t bits     = initial;

            while (true)
            {
                size_t max  = (1 << bits);
                if (value < max)
                    break;
                if ((res = sOut.bwrite(true)) != STATUS_OK)
                    return res;

                value      -= max;
                bits       += stepping;
            }

            if ((res = sOut.bwrite(false)) != STATUS_OK)
                return res;

            return (bits > 0) ? sOut.writev(lsp::fixed_int(value), bits) : STATUS_OK;
        }

        size_t Compressor::est_uint(size_t value, size_t initial, size_t stepping)
        {
            size_t bits     = initial;
            size_t est      = 1;

            while (true)
            {
                size_t max  = (1 << bits);
                if (value < max)
                    break;
                est        ++;
                value      -= max;
                bits       += stepping;
            }

            return est + bits;
        }

        size_t Compressor::calc_repeats(const uint8_t *head, const uint8_t *tail)
        {
            uint8_t b       = head[-1];
            const uint8_t *s= head;
            while ((s < tail) && (*s == b))
                ++s;

            return s - head;
        }

        status_t Compressor::flush()
        {
            status_t res = sOut.flush();
            if (res != STATUS_OK)
                return res;

            nSegment        = sOS.position();
            nOffset         = 0;
            sBuffer.clear();    // Clear state of the buffer

            return STATUS_OK;
        }
    } /* namespaec resource */
} /* namespace lsp */


