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
        status_t read_bookmarks_gtk(lltl::parray<bookmark_t> *dst, io::IInSequence *in, size_t origin)
        {
            LSPString tmp;
            status_t res;
            ssize_t split;
            lltl::parray<bookmark_t> vtmp;
            lsp_finally { destroy_bookmarks(&vtmp); };

            while (true)
            {
                // Read line
                res = in->read_line(&tmp, true);
                if (res != STATUS_OK)
                {
                    if (res == STATUS_EOF)
                        break;
                    destroy_bookmarks(&vtmp);
                    return res;
                }

                // Analyze line
                if (!tmp.starts_with_ascii_nocase("file://"))
                    continue;


                // Create bookmark
                bookmark_t *bm  = new bookmark_t();
                if (bm == NULL)
                    return STATUS_NO_MEM;
                bm->origin      = origin;

                // Is there alias?
                split = tmp.index_of(' ');

                // Decode bookmark respectively to the presence of alias
                if ((res = url::decode(&bm->path, &tmp, 7, (split < 0) ? tmp.length() : split)) != STATUS_OK)
                {
                    delete bm;
                    return STATUS_NO_MEM;
                }

                // Obtain the last name in path
                if (split < 0)
                {
                    split = lsp_max(bm->path.rindex_of(FS_CHAR_MAIN), split = bm->path.rindex_of(FS_CHAR_ALT));
                    if (split < 0)
                        split = -1;
                    if (!bm->name.set(&bm->path, split + 1))
                    {
                        delete bm;
                        return STATUS_NO_MEM;
                    }
                }
                else if (!bm->name.set(&tmp, split + 1))
                {
                    delete bm;
                    return STATUS_NO_MEM;
                }

                // Add to list
                if (!vtmp.add(bm))
                {
                    delete bm;
                    return STATUS_NO_MEM;
                }
            }

            dst->swap(&vtmp);

            return STATUS_OK;
        }

        status_t read_bookmarks_gtk(lltl::parray<bookmark_t> *dst, const char *path, const char *charset, size_t origin)
        {
            if ((path == NULL) || (dst == NULL))
                return STATUS_BAD_ARGUMENTS;

            io::InSequence is;
            status_t res;
            lltl::parray<bookmark_t> tmp;
            lsp_finally { destroy_bookmarks(&tmp); };

            if ((res = is.open(path, charset)) == STATUS_OK)
            {
                res = read_bookmarks_gtk(&tmp, &is, origin);
                if (res == STATUS_OK)
                    res = is.close();
                else
                    is.close();
            }

            if (res == STATUS_OK)
                dst->swap(&tmp);

            return res;
        }

        status_t read_bookmarks_gtk(lltl::parray<bookmark_t> *dst, const LSPString *path, const char *charset, size_t origin)
        {
            if ((path == NULL) || (dst == NULL))
                return STATUS_BAD_ARGUMENTS;

            io::InSequence is;
            status_t res;
            lltl::parray<bookmark_t> tmp;
            lsp_finally { destroy_bookmarks(&tmp); };

            if ((res = is.open(path, charset)) == STATUS_OK)
            {
                res = read_bookmarks_gtk(&tmp, &is, origin);
                if (res == STATUS_OK)
                    res = is.close();
                else
                    is.close();
            }

            if (res == STATUS_OK)
                dst->swap(&tmp);

            return res;
        }

        status_t read_bookmarks_gtk(lltl::parray<bookmark_t> *dst, const io::Path *path, const char *charset, size_t origin)
        {
            if ((path == NULL) || (dst == NULL))
                return STATUS_BAD_ARGUMENTS;

            io::InSequence is;
            status_t res;
            lltl::parray<bookmark_t> tmp;
            lsp_finally { destroy_bookmarks(&tmp); };

            if ((res = is.open(path, charset)) == STATUS_OK)
            {
                res = read_bookmarks_gtk(&tmp, &is, origin);
                if (res == STATUS_OK)
                    res = is.close();
                else
                    is.close();
            }

            if (res == STATUS_OK)
                dst->swap(&tmp);

            return res;
        }

        status_t read_bookmarks_gtk2(lltl::parray<bookmark_t> *dst, const char *path, const char *charset)
        {
            return read_bookmarks_gtk(dst, path, charset, BM_GTK2);
        }

        status_t read_bookmarks_gtk2(lltl::parray<bookmark_t> *dst, const LSPString *path, const char *charset)
        {
            return read_bookmarks_gtk(dst, path, charset, BM_GTK2);
        }

        status_t read_bookmarks_gtk2(lltl::parray<bookmark_t> *dst, const io::Path *path, const char *charset)
        {
            return read_bookmarks_gtk(dst, path, charset, BM_GTK2);
        }

        status_t read_bookmarks_gtk2(lltl::parray<bookmark_t> *dst, io::IInSequence *in)
        {
            return read_bookmarks_gtk(dst, in, BM_GTK2);
        }

        status_t read_bookmarks_gtk3(lltl::parray<bookmark_t> *dst, const char *path, const char *charset)
        {
            return read_bookmarks_gtk(dst, path, charset, BM_GTK3);
        }

        status_t read_bookmarks_gtk3(lltl::parray<bookmark_t> *dst, const LSPString *path, const char *charset)
        {
            return read_bookmarks_gtk(dst, path, charset, BM_GTK3);
        }

        status_t read_bookmarks_gtk3(lltl::parray<bookmark_t> *dst, const io::Path *path, const char *charset)
        {
            return read_bookmarks_gtk(dst, path, charset, BM_GTK3);
        }

        status_t read_bookmarks_gtk3(lltl::parray<bookmark_t> *dst, io::IInSequence *in)
        {
            return read_bookmarks_gtk(dst, in, BM_GTK3);
        }

    } /* namespace bookmarks */
} /* namespace lsp */



