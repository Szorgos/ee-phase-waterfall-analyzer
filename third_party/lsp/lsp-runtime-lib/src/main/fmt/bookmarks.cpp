/*
 * Copyright (C) 2020 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2020 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 14 окт. 2019 г.
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

namespace lsp
{
    namespace bookmarks
    {
        void destroy_bookmarks(lltl::parray<bookmark_t> *list)
        {
            if (list == NULL)
                return;

            for (size_t i=0, n=list->size(); i<n; ++i)
            {
                bookmark_t *ptr = list->uget(i);
                if (ptr != NULL)
                    delete ptr;
            }
            list->flush();
        }

        static bool bookmark_exists(const lltl::parray<bookmark_t> *search, const LSPString *path)
        {
            for (size_t i=0, n=search->size(); i<n; ++i)
            {
                const bookmark_t *bm = search->uget(i);
                if ((bm != NULL) && (bm->path.equals(path)))
                    return true;
            }
            return false;
        }

        status_t merge_bookmarks(lltl::parray<bookmark_t> *dst, size_t *changes, const lltl::parray<bookmark_t> *src, bm_origin_t origin)
        {
            if ((dst == NULL) || (src == NULL) || (origin == 0))
                return STATUS_BAD_ARGUMENTS;

            size_t nc = 0;

            // Step 1: check presence of all bookmarks of 'dst' in 'src'
            for (size_t i=0; i<dst->size(); )
            {
                bookmark_t *bm = dst->uget(i);
                if (bm == NULL) // Remove all NULL entries
                {
                    size_t sz = dst->size();
                    if (dst->remove(i))
                        ++nc;
                    else if (dst->size() == sz)
                        return STATUS_NO_MEM;
                    continue;
                }
                else
                    ++i;

                if (bookmark_exists(src, &bm->path))
                {
                    if (!(bm->origin & origin))
                    {
                        bm->origin     |= origin;
                        ++nc;
                    }
                }
                else if (bm->origin & origin)
                {
                    bm->origin     &= ~origin;
                    ++nc;
                }
            }

            // Step 2: check presence of all bookmarks of 'src' in 'dst'
            for (size_t i=0, n=src->size(); i<n; ++i)
            {
                const bookmark_t *bm = src->uget(i);
                if ((bm == NULL) || (bookmark_exists(dst, &bm->path)))
                    continue;

                // Copy bookmark
                bookmark_t *dm = new bookmark_t;
                if (dm == NULL)
                    return STATUS_NO_MEM;

                if ((!dm->path.set(&bm->path)) ||
                    (!dm->name.set(&bm->name)))
                {
                    delete dm;
                    return STATUS_NO_MEM;
                }

                if (!dst->add(dm))
                {
                    delete dm;
                    return STATUS_NO_MEM;
                }

                // Mark with origin + LSP flag
                dm->origin      = (origin | BM_LSP);
                ++nc;
            }

            // Step 3: remove all bookmarks with empty flags
            for (size_t i=0; i<dst->size();)
            {
                bookmark_t *bm = dst->uget(i);
                if (bm->origin == 0)
                {
                    if (!dst->remove(i))
                        return STATUS_NO_MEM;
                    delete bm;
                    ++nc;
                    continue;
                }
                else
                    ++i;
            }

            // Store number of changes
            if (changes != NULL)
                *changes   += nc;

            return STATUS_OK;
        }

    } /* namespace bookmarks */
} /* namespace lsp */



