/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 12 авг. 2022 г.
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

#include <lsp-plug.in/common/endian.h>
#include <lsp-plug.in/fmt/url.h>
#include <lsp-plug.in/fmt/bookmarks.h>
#include <lsp-plug.in/fmt/lnk/types.h>
#include <lsp-plug.in/io/Dir.h>
#include <lsp-plug.in/io/InFileStream.h>
#include <lsp-plug.in/io/InSequence.h>
#include <lsp-plug.in/stdlib/stdlib.h>

namespace lsp
{
    namespace bookmarks
    {
        static inline status_t read_exact(io::IInStream *is, void *dst, size_t count, status_t code)
        {
            wssize_t res = is->read_fully(dst, count);
            if (res < 0)
                return status_t(-res);
            return (res == wssize_t(count)) ? STATUS_OK : code;
        }

        static inline status_t skip_exact(io::IInStream *is, size_t count, status_t code)
        {
            wssize_t res = is->skip(count);
            if (res < 0)
                return status_t(-res);
            return (res == wssize_t(count)) ? STATUS_OK : code;
        }

        static inline size_t utf16_nlen(const lsp_utf16_t *s, size_t limit)
        {
            for (size_t i=0; i<limit; ++i)
                if (s[i] == 0)
                    return i;
            return limit;
        }

        static status_t read_string_data(io::IInStream *is, LSPString *s, bool unicode)
        {
            status_t res;
            lnk::shlink_string_data_t sdata;

            // Read the lenght of the string data
            if ((res = read_exact(is, &sdata, sizeof(sdata), STATUS_CORRUPTED)) != STATUS_OK)
                return res;

            // Obtain the length
            size_t length = LE_TO_CPU(sdata.length);
            if (length == 0)
            {
                s->clear();
                return STATUS_OK;
            }

            // Read in unicode mode
            if (unicode)
            {
                size_t ubytes       = length * sizeof(lsp_utf16_t);
                lsp_utf16_t *us     = reinterpret_cast<lsp_utf16_t *>(malloc(ubytes));
                if (us == NULL)
                    return STATUS_NO_MEM;
                lsp_finally { free(us); };
                if ((res = read_exact(is, us, ubytes, STATUS_CORRUPTED)) != STATUS_OK)
                    return res;
                return (s->set_utf16le(us, length)) ? STATUS_OK : STATUS_NO_MEM;
            }

            // Read in OEM encoding
            char *os            = reinterpret_cast<char *>(malloc(length));
            if (os == NULL)
                return STATUS_NO_MEM;
            lsp_finally { free(os); };
            if ((res = read_exact(is, os, length, STATUS_CORRUPTED)) != STATUS_OK)
                return res;
            return (s->set_native(os, length)) ? STATUS_OK : STATUS_NO_MEM;
        }

        static status_t fetch_bookmark_path(LSPString *path, const io::Path *location, io::IInStream *is)
        {
            status_t res;
            LSPString li_base, li_ubase, name, rel_path, work_dir, arguments, icon_location;
            io::Path xpath;

            // Read LNK header
            lnk::shlink_header_t hdr;
            if ((res = read_exact(is, &hdr, sizeof(hdr), STATUS_BAD_FORMAT)) != STATUS_OK)
                return res;

            // Validate LNK header and signature
            if (LE_TO_CPU(hdr.size) != sizeof(hdr))
                return STATUS_BAD_FORMAT;
            if ((LE_TO_CPU(hdr.clsid.parts[0]) != lnk::SHLINK_FILE_HEADER_GUID.parts[0]) ||
                (LE_TO_CPU(hdr.clsid.parts[1]) != lnk::SHLINK_FILE_HEADER_GUID.parts[1]) ||
                (LE_TO_CPU(hdr.clsid.parts[2]) != lnk::SHLINK_FILE_HEADER_GUID.parts[2]) ||
                (LE_TO_CPU(hdr.clsid.parts[3]) != lnk::SHLINK_FILE_HEADER_GUID.parts[3]))
                return STATUS_BAD_FORMAT;
            uint32_t flags = LE_TO_CPU(hdr.flags);

            // Skip the record if it doesn't reference the directory
            if (!(LE_TO_CPU(hdr.file_attributes) & lnk::SHLINK_FILE_ATTRIBUTE_DIRECTORY))
                return STATUS_SKIP;

            // Skip the IDList if it is present
            if (flags & lnk::SHLINK_HAS_LINK_TARGET_IDLIST)
            {
                lnk::shlink_idlist_t idlist;
                if ((res = read_exact(is, &idlist, sizeof(idlist), STATUS_CORRUPTED)) != STATUS_OK)
                    return res;
                if ((res = skip_exact(is, LE_TO_CPU(idlist.size), STATUS_CORRUPTED)) != STATUS_OK)
                    return res;
            }

            // Process the LINKINFO structure
            if (flags & lnk::SHLINK_HAS_LINK_INFO)
            {
                lnk::shlink_link_info_t li;
                lnk::shlink_link_info_opt_t lio;
                size_t li_offset        = 0;

                bzero(&lio, sizeof(lio));
                // Mandatory header
                if ((res = read_exact(is, &li, sizeof(li), STATUS_CORRUPTED)) != STATUS_OK)
                    return res;
                li_offset              += sizeof(li);

                // Parse fields of mandatory header and read optional header
                uint32_t li_size        = LE_TO_CPU(li.size);
                uint32_t li_hdr_size    = LE_TO_CPU(li.header_size);
                uint32_t li_flags       = LE_TO_CPU(li.flags);
                if (li_size < sizeof(li))
                    return STATUS_CORRUPTED;
                if (li_hdr_size < sizeof(li))
                    return STATUS_CORRUPTED;
                else if (li_hdr_size > sizeof(li))
                {
                    // Read optional header
                    size_t left         = li_hdr_size - sizeof(li);
                    size_t extra        = lsp_min(left, sizeof(lio));
                    if ((res = read_exact(is, &lio, extra, STATUS_CORRUPTED)) != STATUS_OK)
                        return res;
                    if ((res = skip_exact(is, left - extra, STATUS_CORRUPTED)) != STATUS_OK)
                        return res;
                    li_offset              += left;
                }

                // Read the rest data from the link info
                const size_t li_rest    = li_size - li_offset;
                uint8_t *li_data        = reinterpret_cast<uint8_t *>(malloc(li_rest));
                if (li_data == NULL)
                    return STATUS_NO_MEM;
                lsp_finally { free(li_data); };
                if ((res = read_exact(is, li_data, li_rest, STATUS_CORRUPTED)) != STATUS_OK)
                    return res;

                // Process the link info data suffix paths
                LSPString li_suffix, li_usuffix;
                if (li_flags & lnk::SHLINK_LINK_INFO_COMMON_NETWORK_RELATIVE_LINK_AND_PATH_SUFFIX)
                {
                    // Read the CommonPathSuffixOffset
                    uint32_t li_suffix_off  = LE_TO_CPU(li.common_path_suffix_offset);
                    if (li_suffix_off != 0)
                    {
                        if (li_suffix_off < li_offset)
                            return STATUS_CORRUPTED;
                        const char *li_suffix_ptr    = reinterpret_cast<const char *>(&li_data[li_suffix_off - li_offset]);
                        size_t li_suffix_len    = strnlen(li_suffix_ptr, li_rest);
                        if (!li_suffix.set_native(li_suffix_ptr, li_suffix_len))
                            return STATUS_NO_MEM;
                    }

                    // Read the CommonPathSuffixUnicode
                    uint32_t li_usuffix_off     = LE_TO_CPU(lio.local_base_path_suffix_offset_unicode);
                    if (li_usuffix_off != 0)
                    {
                        if (li_usuffix_off < li_offset)
                            return STATUS_CORRUPTED;
                        const lsp_utf16_t *li_usuffix_ptr   = reinterpret_cast<const lsp_utf16_t *>(&li_data[li_usuffix_off - li_offset]);
                        size_t li_usuffix_len   = utf16_nlen(li_usuffix_ptr, li_rest/sizeof(lsp_utf16_t));
                        if (!li_suffix.set_utf16le(li_usuffix_ptr, li_usuffix_len))
                            return STATUS_NO_MEM;
                    }
                }

                // Process the link info data base paths
                if (li_flags & lnk::SHLINK_LINK_INFO_VOLUME_ID_AND_LOCAL_BASE_PATH)
                {
                    // Read the LocalBasePath
                    uint32_t li_base_off    = LE_TO_CPU(li.local_base_path_offset);
                    if (li_base_off != 0)
                    {
                        if (li_base_off < li_offset)
                            return STATUS_CORRUPTED;
                        const char *li_base_ptr = reinterpret_cast<const char *>(&li_data[li_base_off - li_offset]);
                        size_t li_base_len      = strnlen(li_base_ptr, li_rest);
                        if (!li_base.set_native(li_base_ptr, li_base_len))
                            return STATUS_NO_MEM;
                        if (!li_base.append(&li_suffix))
                            return STATUS_NO_MEM;
                    }

                    // Read the LocalBasePathUnicode
                    uint32_t li_ubase_off   = LE_TO_CPU(lio.local_base_path_offset_unicode);
                    if (li_ubase_off != 0)
                    {
                        if (li_ubase_off < li_offset)
                            return STATUS_CORRUPTED;
                        const lsp_utf16_t *li_ubase_ptr     = reinterpret_cast<const lsp_utf16_t *>(&li_data[li_ubase_off - li_offset]);
                        size_t li_ubase_len     = utf16_nlen(li_ubase_ptr, li_rest/sizeof(lsp_utf16_t));
                        if (!li_ubase.set_utf16le(li_ubase_ptr, li_ubase_len))
                            return STATUS_NO_MEM;
                        if (!li_ubase.append(&li_usuffix))
                            return STATUS_NO_MEM;
                    }
                }
            } // SHLINK_HAS_LINK_INFO

            // Process the STRING_DATA section
            const bool unicode = flags & lnk:: SHLINK_IS_UNICODE;
            if (flags & lnk::SHLINK_HAS_NAME)
            {
                if ((res = read_string_data(is, &name, unicode)) != STATUS_OK)
                    return res;
            }
            if (flags & lnk::SHLINK_HAS_RELATIVE_PATH)
            {
                if ((res = read_string_data(is, &rel_path, unicode)) != STATUS_OK)
                    return res;
            }
            if (flags & lnk::SHLINK_HAS_WORKING_DIR)
            {
                if ((res = read_string_data(is, &work_dir, unicode)) != STATUS_OK)
                    return res;
            }
            if (flags & lnk::SHLINK_HAS_ARGUMENTS)
            {
                if ((res = read_string_data(is, &arguments, unicode)) != STATUS_OK)
                    return res;
            }
            if (flags & lnk::SHLINK_HAS_ICON_LOCATION)
            {
                if ((res = read_string_data(is, &icon_location, unicode)) != STATUS_OK)
                    return res;
            }

            // We don't need to parse the rest data, just analyze what we got
            // Check that unicode LocalBasePath is present
            if (li_ubase.length() > 0)
            {
                li_ubase.swap(path);
                return STATUS_OK;
            }
            // Check that OEM LocalBasePath is present
            if (li_base.length() > 0)
            {
                li_base.swap(path);
                return STATUS_OK;
            }
            // Check if some string data has been present...
            if (work_dir.length() > 0)
            {
                if (!work_dir.append(&rel_path))
                    return STATUS_NO_MEM;
                if ((res = xpath.set(&work_dir)) != STATUS_OK)
                    return res;
                if ((res = xpath.canonicalize()) != STATUS_OK)
                    return res;
                return xpath.get(path);
            }
            if (rel_path.length() > 0)
            {
                if ((res = location->get_parent(&xpath)) != STATUS_OK)
                    return res;
                if ((res = xpath.append_child(&rel_path)) != STATUS_OK)
                    return res;
                if ((res = xpath.canonicalize()) != STATUS_OK)
                    return res;
                return xpath.get(path);
            }

            // Actually, we didn't find any proper fields to fetch the path to the bookmarks
            return STATUS_SKIP;
        }

        static status_t read_bookmark_path(LSPString *path, io::Dir *dh, LSPString *child)
        {
            status_t res;
            io::InFileStream is;
            io::Path p;

            if ((res = dh->get_path(&p)) != STATUS_OK)
                return res;
            if ((res = p.append_child(child)) != STATUS_OK)
                return res;
            if ((res = is.open(&p)) != STATUS_OK)
                return res;

            res = fetch_bookmark_path(path, &p, &is);
            status_t res2 = is.close();

            return (res != STATUS_OK) ? res : res2;
        }

        static status_t read_bookmarks_lnk(lltl::parray<bookmark_t> *dst, io::Dir *dh)
        {
            LSPString fname;
            status_t res;
            lltl::parray<bookmark_t> list;
            lsp_finally { destroy_bookmarks(&list); };

            // Read directory entries
            while ((res = dh->read(&fname)) == STATUS_OK)
            {
                if (!fname.ends_with_ascii_nocase(".lnk"))
                    continue;

                // Create bookmark
                bookmark_t *bm  = new bookmark_t();
                if (bm == NULL)
                    return STATUS_NO_MEM;
                lsp_finally {
                    if (bm != NULL)
                        delete bm;
                };

                // Set the value
                bm->origin      = BM_LNK;
                if (!bm->name.set(&fname, 0, fname.length() - 4)) // All except the '.lnk' extension
                    return STATUS_NO_MEM;
                if ((res = read_bookmark_path(&bm->path, dh, &fname)) != STATUS_OK)
                {
                    if ((res == STATUS_BAD_FORMAT) ||
                        (res == STATUS_CORRUPTED) ||
                        (res == STATUS_SKIP))
                        continue;
                    return res;
                }

                // Add bookmark to list if we have successfully read the path
                if (!list.add(bm))
                    return STATUS_NO_MEM;
                bm              = NULL; // Release reference to avoid bookmark object deletion
            }

            // Commit result and return
            if (res != STATUS_EOF)
                return res;

            list.swap(dst);
            return STATUS_OK;
        }

        status_t read_bookmarks_lnk(lltl::parray<bookmark_t> *dst, const char *path)
        {
            io::Dir dh;
            status_t res = dh.open(path);
            if (res != STATUS_OK)
                return res;
            lsp_finally { dh.close(); };

            return read_bookmarks_lnk(dst, &dh);
        }

        status_t read_bookmarks_lnk(lltl::parray<bookmark_t> *dst, const LSPString *path)
        {
            io::Dir dh;
            status_t res = dh.open(path);
            if (res != STATUS_OK)
                return res;
            lsp_finally { dh.close(); };

            return read_bookmarks_lnk(dst, &dh);
        }

        status_t read_bookmarks_lnk(lltl::parray<bookmark_t> *dst, const io::Path *path)
        {
            io::Dir dh;
            status_t res = dh.open(path);
            if (res != STATUS_OK)
                return res;
            lsp_finally { dh.close(); };

            return read_bookmarks_lnk(dst, &dh);
        }

    } /* namespace bookmarks */
} /* namespace lsp */



