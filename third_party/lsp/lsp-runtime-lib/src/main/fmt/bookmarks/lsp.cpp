/*
 * Copyright (C) 2022 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2022 Vladimir Sadovnikov <sadko4u@gmail.com>
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


#include <lsp-plug.in/fmt/url.h>
#include <lsp-plug.in/fmt/bookmarks.h>
#include <lsp-plug.in/fmt/json/Parser.h>
#include <lsp-plug.in/fmt/json/Serializer.h>
#include <lsp-plug.in/io/InFileStream.h>
#include <lsp-plug.in/io/InSequence.h>

#ifdef PLATFORM_WINDOWS
    #define FS_CHAR_MAIN    '\\'
    #define FS_CHAR_ALT     '/'
#else
    #define FS_CHAR_MAIN    '/'
    #define FS_CHAR_ALT     '\\'
#endif /* PLATFORM_WINDOWS */

namespace lsp
{
    namespace bookmarks
    {
        status_t read_json_origin(size_t *origin, json::Parser &p)
        {
            json::event_t ev;

            status_t res = p.read_next(&ev);
            if (res != STATUS_OK)
                return res;
            if (ev.type != json::JE_ARRAY_START)
                return STATUS_CORRUPTED;

            while (true)
            {
                // Check that it is not end of array
                status_t res = p.read_next(&ev);
                if (res != STATUS_OK)
                    return res;
                if (ev.type == json::JE_ARRAY_END)
                    break;
                else if (ev.type != json::JE_STRING)
                    return STATUS_CORRUPTED;

                // Analyze string
                if (ev.sValue.equals_ascii("lsp"))
                    *origin    |= BM_LSP;
                else if (ev.sValue.equals_ascii("gtk2"))
                    *origin    |= BM_GTK2;
                else if (ev.sValue.equals_ascii("gtk3"))
                    *origin    |= BM_GTK3;
                else if (ev.sValue.equals_ascii("qt5"))
                    *origin    |= BM_QT5;
                else if (ev.sValue.equals_ascii("lnk"))
                    *origin    |= BM_LNK;
            }

            return STATUS_OK;
        }

        status_t read_json_item(bookmark_t *item, json::Parser &p)
        {
            json::event_t ev;

            while (true)
            {
                // Check that it is not end of array
                status_t res = p.read_next(&ev);
                if (res != STATUS_OK)
                    return res;
                if (ev.type == json::JE_OBJECT_END)
                    break;
                else if (ev.type != json::JE_PROPERTY)
                    return STATUS_CORRUPTED;

                // Read properties
                if (ev.sValue.equals_ascii("path"))
                {
                    if ((res = p.read_string(&item->path)) != STATUS_OK)
                        return res;
                }
                else if (ev.sValue.equals_ascii("name"))
                {
                    if ((res = p.read_string(&item->name)) != STATUS_OK)
                        return res;
                }
                else if (ev.sValue.equals_ascii("origin"))
                {
                    if ((res = read_json_origin(&item->origin, p)) != STATUS_OK)
                        return res;
                }
                else if ((res = p.skip_next()) != STATUS_OK)
                    return res;
            }

            return STATUS_OK;
        }

        status_t read_json_bookmarks(lltl::parray<bookmark_t> *dst, json::Parser &p)
        {
            json::event_t ev;

            status_t res = p.read_next(&ev);
            if (res != STATUS_OK)
                return res;
            if (ev.type != json::JE_ARRAY_START)
                return STATUS_CORRUPTED;

            while (true)
            {
                // Check that it is not end of array
                status_t res = p.read_next(&ev);
                if (res != STATUS_OK)
                    return res;
                if (ev.type == json::JE_ARRAY_END)
                    break;
                else if (ev.type != json::JE_OBJECT_START)
                    return STATUS_CORRUPTED;

                // Read bookmark item
                bookmark_t *item = new bookmark_t();
                item->origin     = 0;

                res = read_json_item(item, p);
                if (res != STATUS_OK)
                {
                    if (res == STATUS_NULL)
                        res = STATUS_CORRUPTED;
                    delete item;
                    return res;
                }
                else if (!dst->add(item))
                {
                    delete item;
                    return STATUS_NO_MEM;
                }
            }

            return res;
        }

        status_t read_bookmarks(lltl::parray<bookmark_t> *dst, json::Parser &p)
        {
            lltl::parray<bookmark_t> tmp;
            status_t res = read_json_bookmarks(&tmp, p);
            if (res == STATUS_OK)
                res = p.close();

            if (res != STATUS_OK)
                p.close();
            else
                dst->swap(&tmp);
            destroy_bookmarks(&tmp);

            return res;
        }

        status_t read_bookmarks(lltl::parray<bookmark_t> *dst, const char *path, const char *charset)
        {
            if (dst == NULL)
                return STATUS_BAD_ARGUMENTS;

            json::Parser p;
            status_t res = p.open(path, json::JSON_VERSION5, charset);
            return (res == STATUS_OK) ? read_bookmarks(dst, p) : res;
        }

        status_t read_bookmarks(lltl::parray<bookmark_t> *dst, const LSPString *path, const char *charset)
        {
            if (dst == NULL)
                return STATUS_BAD_ARGUMENTS;

            json::Parser p;
            status_t res = p.open(path, json::JSON_VERSION5, charset);
            return (res == STATUS_OK) ? read_bookmarks(dst, p) : res;
        }

        status_t read_bookmarks(lltl::parray<bookmark_t> *dst, const io::Path *path, const char *charset)
        {
            if (dst == NULL)
                return STATUS_BAD_ARGUMENTS;

            json::Parser p;
            status_t res = p.open(path, json::JSON_VERSION5, charset);
            return (res == STATUS_OK) ? read_bookmarks(dst, p) : res;
        }

        status_t read_bookmarks(lltl::parray<bookmark_t> *dst, io::IInSequence *in)
        {
            if (dst == NULL)
                return STATUS_BAD_ARGUMENTS;

            json::Parser p;
            lltl::parray<bookmark_t> tmp;
            status_t res = p.wrap(in, json::JSON_VERSION5, WRAP_NONE);
            return (res == STATUS_OK) ? read_bookmarks(dst, p) : res;
        }

        void init_settings(json::serial_flags_t *s)
        {
            s->version      = json::JSON_VERSION5;
            s->identifiers  = false;
            s->ident        = ' ';
            s->padding      = 4;
            s->separator    = true;
            s->multiline    = true;
        }

        status_t save_item(const bookmark_t *item, json::Serializer &s)
        {
            status_t res;
            if ((res = s.start_object()) != STATUS_OK)
                return res;
            {
                // Path
                if ((res = s.write_property("path")) != STATUS_OK)
                    return res;
                if ((res = s.write_string(&item->path)) != STATUS_OK)
                    return res;

                // Name
                if ((res = s.write_property("name")) != STATUS_OK)
                    return res;
                if ((res = s.write_string(&item->name)) != STATUS_OK)
                    return res;

                // Origin
                if ((res = s.write_property("origin")) != STATUS_OK)
                    return res;
                bool ml = s.set_multiline(false);
                if ((res = s.start_array()) != STATUS_OK)
                    return res;
                {
                    if ((item->origin & BM_LSP) && ((res = s.write_string("lsp")) != STATUS_OK))
                        return res;
                    if ((item->origin & BM_GTK2) && ((res = s.write_string("gtk2")) != STATUS_OK))
                        return res;
                    if ((item->origin & BM_GTK3) && ((res = s.write_string("gtk3")) != STATUS_OK))
                        return res;
                    if ((item->origin & BM_QT5) && ((res = s.write_string("qt5")) != STATUS_OK))
                        return res;
                    if ((item->origin & BM_LNK) && ((res = s.write_string("lnk")) != STATUS_OK))
                        return res;
                }
                if ((res = s.end_array()) != STATUS_OK)
                    return res;
                s.set_multiline(ml);
            }

            if ((res = s.end_object()) != STATUS_OK)
                return res;

            return res;
        }

        status_t save_bookmarks(const lltl::parray<bookmark_t> *src, json::Serializer &s)
        {
            static const char *comment = "\n"
                    " * This file contains list of bookmarked directories.\n"
                    " * \n"
                    " ";

            // Write header
            status_t res = s.write_comment(comment);
            if (res == STATUS_OK)
                res = s.writeln();
            if (res == STATUS_OK)
                res = s.start_array();

            // Serialize body
            if (res == STATUS_OK)
            {
                for (size_t i=0, n=src->size(); i<n; ++i)
                {
                    const bookmark_t *bm = src->uget(i);
                    if ((bm == NULL) || (bm->origin == 0)) // Skip bookmarks with empty origin
                        continue;
                    if ((res = save_item(bm, s)) != STATUS_OK)
                        break;
                }
            }

            if (res == STATUS_OK)
                res = s.end_array();

            // Close serializer
            if (res == STATUS_OK)
                res = s.close();
            else
                s.close();

            return res;
        }

        status_t save_bookmarks(const lltl::parray<bookmark_t> *src, const char *path, const char *charset)
        {
            json::Serializer s;
            json::serial_flags_t flags;
            init_settings(&flags);
            status_t res = s.open(path, &flags, charset);
            return (res == STATUS_OK) ? save_bookmarks(src, s) : res;
        }

        status_t save_bookmarks(const lltl::parray<bookmark_t> *src, const LSPString *path, const char *charset)
        {
            json::Serializer s;
            json::serial_flags_t flags;
            init_settings(&flags);
            status_t res = s.open(path, &flags, charset);
            return (res == STATUS_OK) ? save_bookmarks(src, s) : res;
        }

        status_t save_bookmarks(const lltl::parray<bookmark_t> *src, const io::Path *path, const char *charset)
        {
            json::Serializer s;
            json::serial_flags_t flags;
            init_settings(&flags);
            status_t res = s.open(path, &flags, charset);
            return (res == STATUS_OK) ? save_bookmarks(src, s) : res;
        }

        status_t save_bookmarks(const lltl::parray<bookmark_t> *src, io::IOutSequence *out)
        {
            json::Serializer s;
            json::serial_flags_t flags;
            init_settings(&flags);
            status_t res = s.wrap(out, &flags, WRAP_NONE);
            return (res == STATUS_OK) ? save_bookmarks(src, s) : res;
        }

    } /* namespace bookmarks */
} /* namespace lsp */

