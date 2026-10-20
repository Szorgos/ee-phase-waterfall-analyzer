/*
 * Copyright (C) 2026 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2026 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 21 янв. 2026 г.
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

#include <lsp-plug.in/fmt/xpm/xpm.h>
#include <lsp-plug.in/fmt/xpm/Parser.h>
#include <lsp-plug.in/io/InFileStream.h>
#include <lsp-plug.in/io/InMemoryStream.h>

#include <private/fmt/xpm/Tokenizer.h>
#include <private/fmt/xpm/XPM1BuiltinParser.h>
#include <private/fmt/xpm/XPM1StreamParser.h>
#include <private/fmt/xpm/XPM2StreamParser.h>
#include <private/fmt/xpm/XPM3BuiltinParser.h>
#include <private/fmt/xpm/XPM3StreamParser.h>

namespace lsp
{
    namespace xpm
    {

        status_t    open(Parser **dst, const char *path)
        {
            io::InFileStream *is = new io::InFileStream();
            if (is == NULL)
                return STATUS_NO_MEM;

            status_t res = is->open(path);
            if (res != STATUS_OK)
            {
                is->close();
                delete is;
                return res;
            }

            return wrap(dst, is, WRAP_CLOSE | WRAP_DELETE);
        }

        status_t    open(Parser **dst, const LSPString *path)
        {
            io::InFileStream *is = new io::InFileStream();
            if (is == NULL)
                return STATUS_NO_MEM;

            status_t res = is->open(path);
            if (res != STATUS_OK)
            {
                is->close();
                delete is;
                return res;
            }

            return wrap(dst, is, WRAP_CLOSE | WRAP_DELETE);
        }

        status_t    open(Parser **dst, const io::Path *path)
        {
            io::InFileStream *is = new io::InFileStream();
            if (is == NULL)
                return STATUS_NO_MEM;

            status_t res = is->open(path);
            if (res != STATUS_OK)
            {
                is->close();
                delete is;
                return res;
            }

            return wrap(dst, is, WRAP_CLOSE | WRAP_DELETE);
        }

        status_t    wrap(Parser **dst, const char *str)
        {
            io::InMemoryStream *is = new io::InMemoryStream();
            if (is == NULL)
                return STATUS_NO_MEM;

            is->wrap(str, strlen(str));
            return wrap(dst, is, WRAP_CLOSE | WRAP_DELETE);
        }

        status_t wrap(Parser **dst, const void *buf, size_t len)
        {
            io::InMemoryStream *is = new io::InMemoryStream();
            if (is == NULL)
                return STATUS_NO_MEM;

            is->wrap(buf, len);
            return wrap(dst, is, WRAP_CLOSE | WRAP_DELETE);
        }

        status_t wrap(Parser **dst, void *buf, size_t len, lsp_memdrop_t drop)
        {
            io::InMemoryStream *is = new io::InMemoryStream();
            if (is == NULL)
                return STATUS_NO_MEM;

            is->wrap(buf, len, drop);
            return wrap(dst, is, WRAP_CLOSE | WRAP_DELETE);
        }

        status_t wrap(Parser **dst, io::IInStream *is, size_t flags)
        {
            if ((dst == NULL) || (is == NULL))
                return STATUS_BAD_ARGUMENTS;

            Tokenizer *tok = new Tokenizer(is, flags);
            if (tok == NULL)
            {
                finalize(is, flags);
                return STATUS_NO_MEM;
            }

            lsp_finally {
                if (tok != NULL)
                {
                    tok->close();
                    delete tok;
                }
            };

            status_t res = tok->init();
            if (res != STATUS_OK)
                return res;

            token_type_t ttype;
            const char *tvalue;
            res = tok->read_token(ttype, tvalue);
            if (res != STATUS_OK)
                return res;

            if (ttype == TOK_DEFINE) // XPM 1
            {
                tok->unread_token();

                XPM1StreamParser * const parser = new XPM1StreamParser(tok);
                if (parser == NULL)
                    return STATUS_NO_MEM;

                tok     = NULL;
                *dst    = parser;

                return STATUS_OK;
            }
            else if (ttype == TOK_XPM2_SIG) // XPM 2
            {
                XPM2StreamParser * const parser = new XPM2StreamParser(tok);
                if (parser == NULL)
                    return STATUS_NO_MEM;

                tok     = NULL;
                *dst    = parser;

                return STATUS_OK;
            }
            else if (ttype == TOK_XPM3_SIG) // XPM 3
            {
                XPM3StreamParser * const parser = new XPM3StreamParser(tok);
                if (parser == NULL)
                    return STATUS_NO_MEM;

                tok     = NULL;
                *dst    = parser;

                return STATUS_OK;
            }

            return STATUS_UNSUPPORTED_FORMAT;
        }

        status_t make_xpm1(
            Parser **dst,
            size_t width, size_t height,
            size_t num_colors, size_t chars_per_pixel,
            const char * const * colors, const char * const * pixels)
        {
            if (dst == NULL)
                return STATUS_BAD_ARGUMENTS;

            header_t hdr;

            hdr.version             = VERSION_XPM1;
            hdr.width               = width;
            hdr.height              = height;
            hdr.num_colors          = num_colors;
            hdr.chars_per_pixel     = chars_per_pixel;
            hdr.x_hotspot           = 0;
            hdr.y_hotspot           = 0;
            hdr.has_extensions      = false;

            XPM1BuiltinParser * parser = new XPM1BuiltinParser(hdr, colors, pixels);
            if (parser == NULL)
                return STATUS_NO_MEM;

            *dst                    = parser;

            return STATUS_OK;
        }

        status_t make_xpm3(Parser **dst, const char * const * lines, size_t count)
        {
            if (dst == NULL)
                return STATUS_BAD_ARGUMENTS;

            XPM3BuiltinParser * parser = new XPM3BuiltinParser(lines, count);
            if (parser == NULL)
                return STATUS_NO_MEM;

            *dst                    = parser;

            return STATUS_OK;
        }


    } /* namespace xpm */
} /* namespace lsp */


