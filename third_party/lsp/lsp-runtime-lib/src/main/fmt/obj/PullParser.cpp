/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 21 апр. 2017 г.
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

#include <lsp-plug.in/fmt/obj/PullParser.h>

#include <lsp-plug.in/common/debug.h>
#include <lsp-plug.in/io/InFileStream.h>
#include <lsp-plug.in/io/InStringSequence.h>
#include <lsp-plug.in/io/InSequence.h>
#include <lsp-plug.in/stdlib/locale.h>

#include <errno.h>

namespace lsp
{
    namespace obj
    {
        PullParser::PullParser()
        {
            pIn             = NULL;
            nWFlags         = 0;
            pBuffer         = NULL;
            nBufOff         = 0;
            nBufLen         = 0;
            nLines          = 0;
            nVx             = 0;
            nParVx          = 0;
            nTexVx          = 0;
            nNormVx         = 0;
            sEvent.type     = EV_NONE;
        }

        PullParser::~PullParser()
        {
            close();
        }

        status_t PullParser::open(const char *path, const char *charset)
        {
            if (pIn != NULL)
                return STATUS_BAD_STATE;
            else if (path == NULL)
                return STATUS_BAD_ARGUMENTS;

            io::InFileStream *ifs = new io::InFileStream();
            if (ifs == NULL)
                return STATUS_NO_MEM;
            status_t res = ifs->open(path);
            if (res == STATUS_OK)
            {
                res     = wrap(ifs, WRAP_CLOSE | WRAP_DELETE, charset);
                if (res == STATUS_OK)
                    return res;
                ifs->close();
            }
            delete ifs;

            return res;
        }

        status_t PullParser::open(const LSPString *path, const char *charset)
        {
            if (pIn != NULL)
                return STATUS_BAD_STATE;
            else if (path == NULL)
                return STATUS_BAD_ARGUMENTS;

            io::InFileStream *ifs = new io::InFileStream();
            if (ifs == NULL)
                return STATUS_NO_MEM;
            status_t res = ifs->open(path);
            if (res == STATUS_OK)
            {
                res     = wrap(ifs, WRAP_CLOSE | WRAP_DELETE, charset);
                if (res == STATUS_OK)
                    return res;
                ifs->close();
            }
            delete ifs;

            return res;
        }

        status_t PullParser::open(const io::Path *path, const char *charset)
        {
            if (pIn != NULL)
                return STATUS_BAD_STATE;
            else if (path == NULL)
                return STATUS_BAD_ARGUMENTS;

            io::InFileStream *ifs = new io::InFileStream();
            if (ifs == NULL)
                return STATUS_NO_MEM;
            status_t res = ifs->open(path);
            if (res == STATUS_OK)
            {
                res     = wrap(ifs, WRAP_CLOSE | WRAP_DELETE, charset);
                if (res == STATUS_OK)
                    return res;
                ifs->close();
            }
            delete ifs;

            return res;
        }

        status_t PullParser::wrap(const char *str, const char *charset)
        {
            if (pIn != NULL)
                return STATUS_BAD_STATE;
            else if (str == NULL)
                return STATUS_BAD_ARGUMENTS;

            io::InStringSequence *seq = new io::InStringSequence();
            if (seq == NULL)
                return STATUS_NO_MEM;

            status_t res = seq->wrap(str, charset);
            if (res == STATUS_OK)
            {
                if ((res = wrap(seq, WRAP_CLOSE | WRAP_DELETE)) == STATUS_OK)
                    return res;
                seq->close();
            }

            delete seq;
            return res;
        }

        status_t PullParser::wrap(const LSPString *str)
        {
            if (pIn != NULL)
                return STATUS_BAD_STATE;
            else if (str == NULL)
                return STATUS_BAD_ARGUMENTS;

            io::InStringSequence *seq = new io::InStringSequence();
            if (seq == NULL)
                return STATUS_NO_MEM;

            status_t res = seq->wrap(str);
            if (res == STATUS_OK)
            {
                if ((res = wrap(seq, WRAP_CLOSE | WRAP_DELETE)) == STATUS_OK)
                    return res;
                seq->close();
            }

            delete seq;
            return res;
        }

        status_t PullParser::wrap(io::IInStream *is, size_t flags, const char *charset)
        {
            if (pIn != NULL)
                return STATUS_BAD_STATE;
            else if (is == NULL)
                return STATUS_BAD_ARGUMENTS;

            io::InSequence *seq = new io::InSequence();
            if (seq == NULL)
                return STATUS_NO_MEM;

            status_t res = seq->wrap(is, flags, charset);
            if (res == STATUS_OK)
            {
                if ((res = wrap(seq, WRAP_CLOSE | WRAP_DELETE)) == STATUS_OK)
                    return res;
                seq->close();
            }

            delete seq;
            return res;
        }

        status_t PullParser::wrap(io::IInSequence *seq, size_t flags)
        {
            if (pIn != NULL)
                return STATUS_BAD_STATE;
            else if (seq == NULL)
                return STATUS_BAD_ARGUMENTS;

            // Allocate memory
            lsp_wchar_t *b  = static_cast<lsp_wchar_t *>(::malloc(IO_BUF_SIZE * sizeof(lsp_wchar_t)));
            if (b == NULL)
                return STATUS_NO_MEM;

            // Initialize state
            pIn             = seq;
            nWFlags         = flags;
            pBuffer         = b;
            nBufOff         = 0;
            nBufLen         = 0;
            nLines          = 0;
            nVx             = 0;
            nParVx          = 0;
            nTexVx          = 0;
            nNormVx         = 0;
            sEvent.type     = EV_NONE;

            return STATUS_OK;
        }

        status_t PullParser::close()
        {
            status_t res = STATUS_OK;

            // Reset internal state
            if (pBuffer != NULL)
            {
                ::free(pBuffer);
                pBuffer = NULL;
            }
            nBufOff         = 0;
            nBufLen         = 0;
            nLines          = 0;

            // Release input sequence
            if (pIn != NULL)
            {
                if (nWFlags & WRAP_CLOSE)
                    res         = pIn->close();

                if (nWFlags & WRAP_DELETE)
                    delete pIn;

                pIn     = NULL;
            }

            return res;
        }

        const event_t *PullParser::current() const
        {
            return (sEvent.type != EV_NONE) ? &sEvent : NULL;
        };

        status_t PullParser::current(event_t *ev) const
        {
            if (pIn == NULL)
                return STATUS_CLOSED;
            if (sEvent.type == EV_NONE)
                return STATUS_NO_DATA;
            return copy_event(ev);
        }

        status_t PullParser::next(event_t *ev)
        {
            if (pIn == NULL)
                return STATUS_CLOSED;
            status_t res = read_event();
            if ((res == STATUS_OK) && (ev != NULL))
                res = copy_event(ev);
            return res;
        }

        status_t PullParser::copy_event(event_t *ev) const
        {
            // Set values to target structure
            ev->type    = sEvent.type;
            ev->vertex  = sEvent.vertex;

            if (!ev->name.set(&sEvent.name))
                return STATUS_NO_MEM;
            if (!ev->ivertex.set(&sEvent.ivertex))
                return STATUS_NO_MEM;
            if (!ev->inormal.set(&sEvent.inormal))
                return STATUS_NO_MEM;
            if (!ev->itexcoord.set(&sEvent.itexcoord))
                return STATUS_NO_MEM;

            return STATUS_OK;
        }

        status_t PullParser::read_event()
        {
            status_t res        = STATUS_OK;

            // Clear current event
            sEvent.type         = EV_NONE;
            sEvent.vertex.x     = 0.0f;
            sEvent.vertex.y     = 0.0f;
            sEvent.vertex.z     = 0.0f;
            sEvent.vertex.w     = 0.0f;
            sEvent.name.clear();
            sEvent.ivertex.clear();
            sEvent.inormal.clear();
            sEvent.itexcoord.clear();

            // Read lines
            while ((res = read_line()) == STATUS_OK)
            {
                // Check that line is not empty
                const char *l = skip_spaces(sLine.get_utf8());
                if ((l == NULL) || (*l == '\0'))
                    continue;

                // Parse line
                if ((res = parse_line(l)) != STATUS_OK)
                    return res;

                // Got event?
                if (sEvent.type != EV_NONE)
                    return STATUS_OK;
            }

            return res;
        }

        status_t PullParser::read_line()
        {
            // Clear previous line contents
            sLine.clear();

            while (true)
            {
                // Ensure that there is data in buffer
                if (nBufOff >= nBufLen)
                {
                    // No data in the buffer, read from input stream
                    ssize_t n = pIn->read(pBuffer, IO_BUF_SIZE);
                    if (n <= 0)
                    {
                        if (sLine.length() > 0)
                            return STATUS_OK;

                        return status_t(-n);
                    }
                    nBufLen     = n;
                    nBufOff     = 0;
                }

                // Scan for line ending character
                size_t tail = nBufOff;
                while (tail < nBufLen)
                {
                    lsp_wchar_t ch = pBuffer[tail++];
                    if (ch == '\n') // Found!
                        break;
                }

                // Append data to string and update buffer state
                sLine.append(&pBuffer[nBufOff], tail - nBufOff);
                nBufOff     = tail;

                // Now analyze last string character
                size_t len  = sLine.length();
                if (sLine.last() != '\n') // Not end of line?
                    continue;
                sLine.set_length(--len);
                if (sLine.last() == '\r') // Remove carriage-return symbol if it is present
                    sLine.set_length(--len);

                // Compute number of terminating '\\' characters
                ssize_t slashes = 0, xoff = len-1;
                while ((xoff >= 0) && (sLine.char_at(xoff) == '\\'))
                {
                    ++slashes;
                    --xoff;
                }

                // Line has been split into multiple lines?
                if (slashes & 1)
                {
                    sLine.set_length(--len);
                    continue;
                }

                // Alright, now we have complete line and can return it
                return eliminate_comments();
            }

            return STATUS_NOT_IMPLEMENTED;
        }

        status_t PullParser::eliminate_comments()
        {
            size_t len = sLine.length(), r=0, w=0;
            bool slash = false;

            while (r < len)
            {
                lsp_wchar_t ch = sLine.at(r);
                if (slash)
                {
                    ++r;
                    if ((ch != '#') && (ch != '\\'))
                        sLine.set_at(w++, '\\');
                    sLine.set_at(w++, ch);
                    slash   = false;
                    continue;
                }
                else if (ch == '#')
                {
                    sLine.set_length(r);
                    return STATUS_OK;
                }
                else if (ch == '\\')
                {
                    slash = true;
                    ++r;
                    continue;
                }

                if (r != w)
                    sLine.set_at(w, ch);
                ++r;
                ++w;
            }

            if (slash)
                sLine.set_at(w++, '\\');
            sLine.set_length(w);

            return STATUS_OK;
        }

        inline bool PullParser::is_space(char ch)
        {
            return (ch == ' ') || (ch == '\t');
        }

        bool PullParser::prefix_match(const char *s, const char *prefix)
        {
            while (*prefix != '\0')
            {
                if (*(s++) != *(prefix++))
                    return false;
            }
            return is_space(*s);
        }

        const char *PullParser::skip_spaces(const char *s)
        {
            if (s == NULL)
                return NULL;

            while (true)
            {
                char ch = *s;
                if ((ch == '\0') || (!is_space(ch)))
                    return s;
                s++;
            }
        }

        bool PullParser::end_of_line(const char *s)
        {
            if (s == NULL)
                return true;

            while (true)
            {
                char ch = *s;
                if (ch == '\0')
                    return true;
                if (!is_space(ch))
                    return false;
                s++;
            }
        }

        bool PullParser::parse_float(float *dst, const char **s)
        {
            if (*s == NULL)
                return false;

            // Save and update locale
            SET_LOCALE_SCOPED(LC_NUMERIC, "C");

            // Parse floating-point value
            errno = 0;
            char *ptr = NULL;
            float result = strtof(*s, &ptr);
            bool success = ((errno == 0) && (ptr > *s));
            if (success)
            {
                *dst    = result;
                *s      = ptr;
            }

            return success;
        }

        bool PullParser::parse_int(ssize_t *dst, const char **s)
        {
            if ((*s == NULL) || (**s == '\0') || (**s == ' '))
                return false;

            errno = 0;
            char *ptr = NULL;
            long result = strtol(*s, &ptr, 10);
            if ((errno != 0) || (ptr == *s))
                return false;
            *dst    = result;
            *s      = ptr;
            return true;
        }

        status_t PullParser::parse_line(const char *s)
        {
            status_t result = ((nLines++) > 0) ? STATUS_CORRUPTED_FILE : STATUS_BAD_FORMAT;

            switch (*(s++))
            {
                case 'b': // bmat, bevel
                    if (prefix_match(s, "mat")) // bmat
                        return STATUS_OK;
                    else if (prefix_match(s, "evel")) // bevel
                        return STATUS_OK;
                    break;

                case 'c': // cstype, curv, curv2, con, c_interp, ctech
                    if (prefix_match(s, "stype")) // cstype
                        return STATUS_OK;
                    else if (prefix_match(s, "urv")) // curv
                        return STATUS_OK;
                    else if (prefix_match(s, "urv2")) // curv2
                        return STATUS_OK;
                    else if (prefix_match(s, "on")) // con
                        return STATUS_OK;
                    else if (prefix_match(s, "_interp")) // c_interp
                        return STATUS_OK;
                    else if (prefix_match(s, "tech")) // ctech
                        return STATUS_OK;
                    break;

                case 'd': // deg, d_interp
                    if (prefix_match(s, "eg")) // deg
                        return STATUS_OK;
                    else if (prefix_match(s, "_interp")) // d_interp
                        return STATUS_OK;
                    break;

                case 'e': // end
                    if (prefix_match(s, "nd")) // end
                        return STATUS_OK;
                    break;

                case 'f': // f
                    if (is_space(*s)) // f - face
                    {
                        // Parse face
                        while (true)
                        {
                            ssize_t v = 0, vt = 0, vn = 0;

                            // Parse indexes
                            s   = skip_spaces(s);
                            if (!parse_int(&v, &s))
                                break;
                            if (*s == '/')
                            {
                                ++s;
                                if (!parse_int(&vt, &s))
                                    vt  = 0;
                                if (*s == '/')
                                {
                                    ++s;
                                    if (!parse_int(&vn, &s))
                                        vn = 0;
                                }
                            }

                            // Ensure that indexes are correct
                            v   = (v < 0) ? nVx + v : v - 1;
                            if ((v < 0) || (v >= nVx))
                                return result;

                            vt  = (vt < 0) ? nTexVx + vt : vt - 1;
                            if ((vt < -1) || (vt >= nTexVx))
                                return result;

                            vn  = (vn < 0) ? nNormVx + vn : vn - 1;
                            if ((vn < -1) || (vn >= nNormVx))
                                return result;

                            // Add items to lists
                            if (!sEvent.ivertex.add(&v))
                                return STATUS_NO_MEM;
                            if (!sEvent.itexcoord.add(&vt))
                                return STATUS_NO_MEM;
                            if (!sEvent.inormal.add(&vn))
                                return STATUS_NO_MEM;
                        }

                        if (!end_of_line(s))
                            return result;

                        // Check face parameters
                        if (sEvent.ivertex.size() < 3)
                            return STATUS_BAD_FORMAT;

                        sEvent.type = EV_FACE;
                        return STATUS_OK;
                    }
                    break;

                case 'g': // g
                    if (is_space(*s)) // g
                        return STATUS_OK;
                    break;

                case 'h': // hole
                    if (prefix_match(s, "ole")) // hole
                        return STATUS_OK;
                    break;

                case 'l': // l, lod
                    if (prefix_match(s, "od")) // lod
                        return STATUS_OK;

                    if (is_space(*s)) // l - line
                    {
                        // Parse line
                        while (true)
                        {
                            ssize_t v = 0, vt = 0;

                            // Parse indexes
                            s   = skip_spaces(s);
                            if (!parse_int(&v, &s))
                                break;
                            if (*(s++) != '/')
                                return result;
                            if (!parse_int(&vt, &s))
                                vt = 0;

                            // Ensure that indexes are correct
                            v   = (v < 0) ? nVx + v : v - 1;
                            if ((v < 0) || (v >= nVx))
                                return result;

                            vt  = (vt < 0) ? nTexVx + vt : vt - 1;
                            if ((vt <= -1) || (vt >= nTexVx))
                                return result;

                            // Add items to lists
                            if (!sEvent.ivertex.add(&v))
                                return STATUS_NO_MEM;
                            if (!sEvent.itexcoord.add(&vt))
                                return STATUS_NO_MEM;
                        }

                        if (!end_of_line(s))
                            return result;

                        // Check line parameters
                        if (sEvent.ivertex.size() < 2)
                            return STATUS_BAD_FORMAT;

                        // Call parser to handle data
                        sEvent.type = EV_LINE;
                        return STATUS_OK;
                    }
                    break;

                case 'm': // mg, mtllib
                    if (prefix_match(s, "g")) // mg
                        return STATUS_OK;
                    else if (prefix_match(s, "tllib")) // mtllib
                        return STATUS_OK;
                    break;

                case 'o': // o
                    if (is_space(*s)) // o
                    {
                        s   = skip_spaces(s + 1);
                        if (!sEvent.name.set_utf8(s))
                            return STATUS_NO_MEM;

                        sEvent.type     = EV_OBJECT;
                        return STATUS_OK;
                    }
                    break;

                case 'p': // p, parm
                    if (is_space(*s)) // p
                    {
                        // Parse point
                        while (true)
                        {
                            ssize_t v = 0;

                            // Parse indexes
                            s   = skip_spaces(s);
                            if (!parse_int(&v, &s))
                                break;

                            // Ensure that indexes are correct
                            v   = (v < 0) ? nVx + v : v - 1;
                            if ((v < 0) || (v >= nVx))
                                return result;

                            // Add items to lists
                            if (!sEvent.ivertex.add(&v))
                                return STATUS_NO_MEM;
                        }

                        // Check that we reached end of line
                        if (!end_of_line(s))
                            return result;

                        sEvent.type     = EV_POINT;
                        return STATUS_OK;
                    }
                    else if (prefix_match(s, "arm")) // parm
                        return STATUS_OK;
                    break;

                case 's': // s, step, surf, scrv, sp, shadow_obj, stech
                    if (is_space(*s)) // s
                        return STATUS_OK;
                    else if (prefix_match(s, "tep")) // step
                        return STATUS_OK;
                    else if (prefix_match(s, "urf")) // surf
                        return STATUS_OK;
                    else if (prefix_match(s, "rcv")) // srcv
                        return STATUS_OK;
                    else if (prefix_match(s, "p")) // sp
                        return STATUS_OK;
                    else if (prefix_match(s, "hadow_obj")) // shadow_obj
                        return STATUS_OK;
                    else if (prefix_match(s, "tech")) // stech
                        return STATUS_OK;
                    break;

                case 't': // trim, trace_obj
                    if (prefix_match(s, "rim")) // trim
                        return STATUS_OK;
                    else if (prefix_match(s, "race_obj")) // trace_obj
                        return STATUS_OK;
                    break;

                case 'u': // usemtl
                    if (prefix_match(s, "semtl")) // usemtl
                        return STATUS_OK;
                    break;

                case 'v': // v, vt, vn, vp
                    if (is_space(*s)) // v
                    {
                        s   = skip_spaces(s+1);
                        if (!parse_float(&sEvent.vertex.x, &s))
                            return result;
                        s   = skip_spaces(s);
                        if (!parse_float(&sEvent.vertex.y, &s))
                            return result;
                        s   = skip_spaces(s);
                        if (!parse_float(&sEvent.vertex.z, &s))
                            sEvent.vertex.z     = 0.0f; // Extension, strictly required in obj format, for our case facilitated
                        s   = skip_spaces(s);
                        if (!parse_float(&sEvent.vertex.w, &s))
                            sEvent.vertex.w     = 1.0f;

                        if (!end_of_line(s))
                            return result;

                        ++nVx;
                        sEvent.type     = EV_VERTEX;
                        return STATUS_OK;
                    }
                    else if (prefix_match(s, "n")) // vn
                    {
                        s   = skip_spaces(s+2);
                        if (!parse_float(&sEvent.normal.dx, &s))
                            return result;
                        s   = skip_spaces(s);
                        if (!parse_float(&sEvent.normal.dy, &s))
                            return result;
                        s   = skip_spaces(s);
                        if (!parse_float(&sEvent.normal.dz, &s))
                            sEvent.normal.dz    = 0.0f; // Extension, strictly required in obj format, for our case facilitated
                        sEvent.normal.dw    = 0.0f;

                        if (!end_of_line(s))
                            return result;

                        ++nNormVx;
                        sEvent.type     = EV_NORMAL;
                        return STATUS_OK;
                    }
                    else if (prefix_match(s, "p")) // vp
                    {
                        s   = skip_spaces(s+2);
                        if (parse_float(&sEvent.vertex.x, &s))
                            return result;
                        s   = skip_spaces(s);
                        if (!parse_float(&sEvent.vertex.y, &s))
                            sEvent.vertex.y     = 0.0f;
                        sEvent.vertex.z     = 0.0f;
                        s   = skip_spaces(s);
                        if (!parse_float(&sEvent.vertex.w, &s))
                            sEvent.vertex.w     = 1.0f;

                        if (!end_of_line(s))
                            return result;

                        ++nParVx;
                        sEvent.type     = EV_PVERTEX;
                        return STATUS_OK;
                    }
                    else if (prefix_match(s, "t")) // vt
                    {
                        s   = skip_spaces(s+2);
                        if (!parse_float(&sEvent.texcoord.u, &s))
                            return result;
                        s   = skip_spaces(s);
                        if (!parse_float(&sEvent.texcoord.v, &s))
                            sEvent.texcoord.v   = 0.0f;
                        s   = skip_spaces(s);
                        if (!parse_float(&sEvent.texcoord.w, &s))
                            sEvent.texcoord.w   = 0.0f;

                        if (!end_of_line(s))
                            return result;

                        ++nTexVx;
                        sEvent.type     = EV_TEXCOORD;
                        return STATUS_OK;
                    }
                    break;

                default:
                    break;
            }

            return result;
        }

    } /* namespace obj */
} /* namespace lsp */
