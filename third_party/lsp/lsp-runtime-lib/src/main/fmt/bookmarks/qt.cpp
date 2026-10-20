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

#include <lsp-plug.in/fmt/url.h>
#include <lsp-plug.in/fmt/bookmarks.h>
#include <lsp-plug.in/fmt/xml/PushParser.h>
#include <lsp-plug.in/io/InFileStream.h>
#include <lsp-plug.in/io/InSequence.h>
#include <lsp-plug.in/io/OutMemoryStream.h>

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
        // Xbel file format parser
        class XbelParser: public xml::IXMLHandler
        {
            private:
                lltl::parray<bookmark_t>   *vList;
                size_t                      nOrigin;
                bookmark_t                 *pCurr;
                bool                        bTitle;
                LSPString                   sPath;

            protected:
                static void flush_os(LSPString *dst, io::OutMemoryStream *os)
                {
                    if (os->size() <= 0)
                        return;
                    dst->append_utf8(reinterpret_cast<const char *>(os->data()), os->size());
                    os->clear();
                }

                static int hexdigit(lsp_wchar_t ch)
                {
                    if ((ch >= '0') && (ch <= '9'))
                        return ch - '0';
                    if ((ch >= 'A') && (ch <= 'F'))
                        return ch - 'A' + 10;
                    if ((ch >= 'a') && (ch <= 'f'))
                        return ch - 'f' + 10;
                    return -1;
                }

                static void get_bookmark_name(LSPString *dst, const LSPString *href)
                {
                    ssize_t idx = lsp_max(href->rindex_of(FS_CHAR_MAIN), href->rindex_of(FS_CHAR_ALT));
                    idx         = lsp_max(idx + 1, ssize_t(0));

                    io::OutMemoryStream os;
                    lsp_finally { os.close(); };

                    for (ssize_t len = href->length(); idx < len; ++idx)
                    {
                        lsp_wchar_t ch = href->at(idx);

                        // Special case?
                        if (ch == '%')
                        {
                            int d1 = hexdigit(href->at(idx+1));
                            int d2 = hexdigit(href->at(idx+2));
                            if ((d1 >= 0) && (d2 >= 0))
                            {
                                os.writeb((d1 << 4) | d2);
                                idx += 2;
                                continue;
                            }
                        }

                        flush_os(dst, &os);
                        dst->append(ch);
                    }

                    flush_os(dst, &os);
                }


            public:
                explicit XbelParser(lltl::parray<bookmark_t> *list, size_t origin)
                {
                    vList       = list;
                    nOrigin     = origin;
                    pCurr       = NULL;
                    bTitle      = false;
                }

            public:
                virtual status_t doctype(const LSPString *doctype, const LSPString *pub, const LSPString *sys)
                {
                    if ((doctype != NULL) && (!doctype->equals_ascii_nocase("xbel")))
                        return STATUS_BAD_FORMAT;
                    return STATUS_OK;
                }

                virtual status_t start_element(const LSPString *name, const LSPString * const *atts)
                {
                    // Append tag name to path
                    if (!sPath.append('/'))
                        return STATUS_NO_MEM;
                    if (!sPath.append(name))
                        return STATUS_NO_MEM;

                    // Check path
                    if (sPath.equals_ascii("/xbel/bookmark"))
                    {
                        LSPString href;

                        // Scan for 'href' attribute
                        for ( ; *atts != NULL; atts += 2)
                            if (atts[0]->equals_ascii("href"))
                            {
                                if (atts[1]->starts_with_ascii("file://"))
                                {
                                    if (!href.set(atts[1], 7))
                                        return STATUS_NO_MEM;
                                }
                                break;
                            }

                        // Do we have a reference?
                        if (href.length() > 0)
                        {
                            // Allocate bookmark descriptor
                            bookmark_t *bm  = new bookmark_t();
                            if (bm == NULL)
                                return STATUS_NO_MEM;
                            if (!vList->add(bm))
                            {
                                delete bm;
                                return STATUS_NO_MEM;
                            }

                            // Initialize bookmark
                            get_bookmark_name(&bm->name, &href);
                            bm->origin      = BM_LSP | nOrigin;
                            bm->path.swap(&href);

                            // Save pointer to bookmark
                            pCurr           = bm;
                            bTitle          = false;
                        }
                    }

                    return STATUS_OK;
                }

                virtual status_t characters(const LSPString *text)
                {
                    if (sPath.equals_ascii("/xbel/bookmark/title"))
                    {
                        if (pCurr == NULL)
                            return STATUS_OK;

                        bool success    = (bTitle) ? pCurr->name.append(text) : pCurr->name.set(text);
                        if (!success)
                            return STATUS_NO_MEM;
                        bTitle          = true;
                    }
                    return STATUS_OK;
                }

                virtual status_t end_element(const LSPString *name)
                {
                    // Forget about current bookmark
                    if (sPath.equals_ascii("/xbel/bookmark"))
                    {
                        pCurr       = NULL;
                        bTitle      = false;
                    }

                    // Reduce path
                    ssize_t idx = sPath.rindex_of('/');
                    sPath.set_length((idx >= 0) ? idx : 0);
                    return STATUS_OK;
                }
        };

        status_t read_bookmarks_qt5(lltl::parray<bookmark_t> *dst, const char *path, const char *charset)
        {
            lltl::parray<bookmark_t> tmp;
            status_t res;

            xml::PushParser p;
            XbelParser h(&tmp, BM_QT5);
            if ((res = p.parse_file(&h, path, charset)) == STATUS_OK)
                dst->swap(&tmp);

            destroy_bookmarks(&tmp);
            return res;
        }

        status_t read_bookmarks_qt5(lltl::parray<bookmark_t> *dst, const LSPString *path, const char *charset)
        {
            lltl::parray<bookmark_t> tmp;
            status_t res;

            xml::PushParser p;
            XbelParser h(&tmp, BM_QT5);
            if ((res = p.parse_file(&h, path, charset)) == STATUS_OK)
                dst->swap(&tmp);

            destroy_bookmarks(&tmp);
            return res;
        }

        status_t read_bookmarks_qt5(lltl::parray<bookmark_t> *dst, const io::Path *path, const char *charset)
        {
            lltl::parray<bookmark_t> tmp;
            status_t res;

            xml::PushParser p;
            XbelParser h(&tmp, BM_QT5);
            if ((res = p.parse_file(&h, path, charset)) == STATUS_OK)
                dst->swap(&tmp);

            destroy_bookmarks(&tmp);
            return res;
        }

        status_t read_bookmarks_qt5(lltl::parray<bookmark_t> *dst, io::IInSequence *in)
        {
            lltl::parray<bookmark_t> tmp;
            status_t res;

            xml::PushParser p;
            XbelParser h(&tmp, BM_QT5);
            if ((res = p.parse_data(&h, in)) == STATUS_OK)
                dst->swap(&tmp);

            destroy_bookmarks(&tmp);
            return res;
        }

    } /* namespace bookmarks */
} /* namespace lsp */
