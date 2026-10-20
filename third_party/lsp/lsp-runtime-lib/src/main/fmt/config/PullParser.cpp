/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 29 апр. 2020 г.
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

#include <lsp-plug.in/fmt/config/PullParser.h>
#include <lsp-plug.in/io/InFileStream.h>
#include <lsp-plug.in/io/InStringSequence.h>
#include <lsp-plug.in/io/InSequence.h>
#include <lsp-plug.in/stdlib/locale.h>
#include <lsp-plug.in/stdlib/math.h>

#include <errno.h>
#include <stdlib.h>
#include <limits.h>

namespace lsp
{
    namespace config
    {
        typedef struct type_t
        {
            const char *prefix;
            size_t flags;
        } type_t;

        static const type_t all_types[] =
        {
            { "i32:",   SF_TYPE_I32     },
            { "u32:",   SF_TYPE_U32     },
            { "f32:",   SF_TYPE_F32     },
            { "i64:",   SF_TYPE_I64     },
            { "u64:",   SF_TYPE_U64     },
            { "f64:",   SF_TYPE_F64     },
            { "bool:",  SF_TYPE_BOOL    },
            { "str:",   SF_TYPE_STR     },
            { "blob:",  SF_TYPE_BLOB    },
            { NULL,     0               }
        };
        
        PullParser::PullParser()
        {
            pIn         = NULL;
            nWFlags     = 0;
        }
        
        PullParser::~PullParser()
        {
            do_close();
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

            pIn             = seq;
            nWFlags         = flags;

            return STATUS_OK;
        }

        status_t PullParser::do_close()
        {
            // Release input sequence
            if (pIn == NULL)
                return STATUS_OK;

            status_t res = STATUS_OK;
            if (nWFlags & WRAP_CLOSE)
                res         = update_status(res, pIn->close());

            if (nWFlags & WRAP_DELETE)
                delete pIn;

            pIn     = NULL;

            return res;
        }

        status_t PullParser::close()
        {
            return do_close();
        }

        status_t PullParser::next(param_t *ev)
        {
            if (pIn == NULL)
                return STATUS_CLOSED;

            status_t result = STATUS_OK;

            while (true)
            {
                // Clear buffer
                sLine.clear();
                sKey.clear();
                sValue.clear();

                // Read line from file
                result      = pIn->read_line(&sLine, true);
                if (result != STATUS_OK)
                    break;

                // Parse the line
                size_t flags    = 0;
                result          = parse_line(flags);
                if (result == STATUS_OK)
                {
                    result  = commit_param(&sKey, &sValue, flags);
                    if ((result == STATUS_OK) && (ev != NULL))
                        result = (ev->copy(&sParam)) ? STATUS_OK : STATUS_NO_MEM;
                    break;
                }
                else if (result != STATUS_SKIP)
                    break;
            }

            return result;
        }

        const param_t *PullParser::current() const
        {
            return ((sParam.flags & SF_TYPE_MASK) != SF_NONE) ? &sParam : NULL;
        }

        status_t PullParser::current(param_t *ev) const
        {
            if (pIn == NULL)
                return STATUS_CLOSED;

            if ((sParam.flags & SF_TYPE_MASK) == SF_NONE)
                return STATUS_NO_DATA;

            if ((ev != NULL) && (!ev->copy(&sParam)))
                return STATUS_NO_MEM;

            return STATUS_OK;
        }

        bool PullParser::skip_spaces(size_t &off)
        {
            size_t len      = sLine.length();

            while (off < len)
            {
                lsp_wchar_t ch = sLine.at(off);
                if ((ch != ' ') && (ch != '\t') && (ch != '\r'))
                    break;
                off ++;
            }

            return ((off >= len) || (sLine.at(off) == '#')); // Return true if line is void
        }

        status_t PullParser::read_key(size_t &off)
        {
            size_t len      = sLine.length();

            while (off < len)
            {
                lsp_wchar_t ch = sLine.at(off++);

                if (((ch >= 'a') && (ch <= 'z')) ||
                    ((ch >= 'A') && (ch <= 'Z')) ||
                    (ch == '_') || (ch == '/'))
                {
                    if (!sKey.append(ch))
                        return STATUS_NO_MEM;
                }
                else if ((ch >= '0') && (ch <= '9'))
                {
                    if (sKey.is_empty())
                        return STATUS_BAD_FORMAT;
                    if (!sKey.append(ch))
                        return STATUS_NO_MEM;
                }
                else
                {
                    --off;
                    break;
                }
            }

            // Validate that key should start with '/' or not contain any '/' character
            if ((sKey.is_empty()) || (sKey.index_of('/') > 0))
                return STATUS_BAD_FORMAT;

            return STATUS_OK;
        }

        status_t PullParser::read_type(size_t &off, size_t &flags)
        {
            // Check type prefix
            for (const type_t *p = all_types; p->prefix != NULL; ++p)
            {
                if (sLine.contains_at_ascii(off, p->prefix))
                {
                    flags      |= p->flags | SF_TYPE_SET;
                    off        += ::strlen(p->prefix);
                    return STATUS_OK;
                }
            }

            return STATUS_OK;
        }

        status_t PullParser::read_value(size_t &off, size_t &flags)
        {
            size_t len      = sLine.length();
            ssize_t trim    = -1;

            // Check that value is quoted
            if (sLine.char_at(off) == '"')
            {
                flags          |= SF_QUOTED;
                ++off;
            }

            // Check that type is present
            while (off < len)
            {
                lsp_wchar_t ch = sLine.at(off++);

                switch (ch)
                {
                    // Escape sequence
                    case '\\':
                    {
                        if (off >= len)
                            return (sLine.append(ch)) ? STATUS_OK : STATUS_NO_MEM;

                        ch = sLine.at(off++);

                        switch (ch)
                        {
                            case 'n':   ch = '\n';  break;
                            case 'r':   ch = '\r';  break;
                            case 't':   ch = '\t';  break;
                            case '\\':
                            case ' ':
                            case '\"':
                            case '#':
                                break;
                            default:
                                if (!sValue.append('\\'))
                                    return STATUS_NO_MEM;
                                break;
                        }
                        if (!sValue.append(ch))
                            return STATUS_NO_MEM;
                        break;
                    }

                    // Comment
                    case '#':
                        if (flags & SF_QUOTED)
                        {
                            if (!sValue.append(ch))
                                return STATUS_NO_MEM;
                            trim    = -1;
                        }
                        else
                            off = len;  // Move to end of line

                        break;

                    // Spaces
                    case ' ':
                    case '\t':
                        if ((!(flags & SF_QUOTED)) && (trim < 0))
                            trim    = sValue.length();

                        if (!sValue.append(ch))
                            return STATUS_NO_MEM;
                        break;

                    // Quotes
                    case '\"':
                        if (!(flags & SF_QUOTED))
                            return STATUS_BAD_FORMAT;
                        return (skip_spaces(off)) ? STATUS_OK : STATUS_BAD_FORMAT;

                    // End of line
                    case '\n':
                        if (flags & SF_QUOTED)
                            return STATUS_BAD_FORMAT;
                        return STATUS_OK;

                    // Other characters
                    default:
                        if (!sValue.append(ch))
                            return STATUS_NO_MEM;
                        trim    = -1;
                        break;
                }
            }

            if (flags & SF_QUOTED)
                return STATUS_BAD_FORMAT;
            else if (trim >= 0)
                sValue.set_length(trim);

            return STATUS_OK;
        }

        status_t PullParser::parse_line(size_t &flags)
        {
            sKey.clear();
            sValue.clear();
            flags           = 0;

            // Empty line?
            size_t off=0;
            if (skip_spaces(off))
                return STATUS_SKIP;

            // Fetch the key value
            status_t res    = read_key(off);
            if (res != STATUS_OK) // Error while parsing line
                return res;
            if (skip_spaces(off))
                return STATUS_BAD_FORMAT;

            // Analyze character
            lsp_wchar_t ch = sLine.at(off++);
            if ((sKey.is_empty()) || (ch != '='))
                return STATUS_BAD_FORMAT;

            // Fetch the value type
            if (skip_spaces(off))
                return STATUS_OK;
            res             = read_type(off, flags);
            if (res != STATUS_OK)
                return res;

            // Fetch the value's value
            if (skip_spaces(off))
                return STATUS_OK;
            res             = read_value(off, flags);
            if (res != STATUS_OK)
                return res;

            // Ensure that line ends correctly
            return (skip_spaces(off)) ? STATUS_OK : STATUS_BAD_FORMAT;
        }

        status_t PullParser::commit_param(const LSPString *key, const LSPString *value, size_t flags)
        {
            param_t tmp;
            status_t res    = STATUS_OK;

            // Copy parameter name
            if (!tmp.name.set(key))
                return STATUS_NO_MEM;
            tmp.comment.clear();

            // If type is explicitly set
            if (flags & SF_TYPE_SET)
            {
                switch (flags & SF_TYPE_MASK)
                {
                    case SF_TYPE_I32: res = parse_int32(value, &tmp.v.i32); break;
                    case SF_TYPE_U32: res = parse_uint32(value, &tmp.v.u32); break;
                    case SF_TYPE_F32: res = parse_float(value, &tmp.v.f32, &flags); break;
                    case SF_TYPE_I64: res = parse_int64(value, &tmp.v.i64); break;
                    case SF_TYPE_U64: res = parse_uint64(value, &tmp.v.u64); break;
                    case SF_TYPE_F64: res = parse_double(value, &tmp.v.f64, &flags); break;
                    case SF_TYPE_BOOL: res = parse_bool(value, &tmp.v.bval); break;
                    case SF_TYPE_STR:
                        if ((tmp.v.str = value->clone_utf8()) == NULL)
                            res     = STATUS_NO_MEM;
                        break;
                    case SF_TYPE_BLOB:
                        tmp.v.blob.ctype  = NULL;
                        tmp.v.blob.data   = NULL;
                        res = parse_blob(value, &tmp.v.blob);
                        break;
                    default:
                        return STATUS_UNKNOWN_ERR;
                }
                tmp.flags       = flags;
                if (res == STATUS_OK)
                    sParam.swap(&tmp);
                return res;
            }

            // Type is not explicitly set
            if (!(flags & SF_QUOTED))
            {
                if (value->index_of('.') < 0)
                {
                    // Try to parse as boolean
                    if ((res = parse_bool(value, &tmp.v.bval)) == STATUS_OK)
                    {
                        tmp.flags     = flags | SF_TYPE_BOOL;
                        sParam.swap(&tmp);
                        return STATUS_OK;
                    }

                    // Try to parse as integer
                    if ((res = parse_int32(value, &tmp.v.i32)) == STATUS_OK)
                    {
                        tmp.flags     = flags | SF_TYPE_I32;
                        sParam.swap(&tmp);
                        return STATUS_OK;
                    }
                }

                // Try to parse as float
                if ((res = parse_float(value, &tmp.v.f32, &flags)) == STATUS_OK)
                {
                    tmp.flags     = flags | SF_TYPE_F32;
                    sParam.swap(&tmp);
                    return STATUS_OK;
                }
            }

            // Return as a string
            if ((tmp.v.str = value->clone_utf8()) == NULL)
                res     = STATUS_NO_MEM;
            tmp.flags   = flags | SF_TYPE_STR;

            sParam.swap(&tmp);
            return STATUS_OK;
        }

        status_t PullParser::parse_int32(const LSPString *str, int32_t *dst)
        {
            const char *s = str->get_utf8();
            if (s == NULL)
                return STATUS_NO_MEM;
            else if (*s == '\0')
                return STATUS_BAD_FORMAT;

            errno = 0;
            char *end = NULL;
            long value = ::strtol(s, &end, 10);

            if ((errno != 0) || (*end != '\0'))
                return STATUS_BAD_FORMAT;

            *dst = int32_t(value);
            return STATUS_OK;
        }

        status_t PullParser::parse_uint32(const LSPString *str, uint32_t *dst)
        {
            const char *s = str->get_utf8();
            if (s == NULL)
                return STATUS_NO_MEM;
            else if (*s == '\0')
                return STATUS_BAD_FORMAT;

            errno = 0;
            char *end = NULL;
            unsigned long value = ::strtoul(s, &end, 10);

            if ((errno != 0) || (*end != '\0'))
                return STATUS_BAD_FORMAT;

            *dst = uint32_t(value);
            return STATUS_OK;
        }

        status_t PullParser::parse_int64(const LSPString *str, int64_t *dst)
        {
            const char *s = str->get_utf8();
            if (s == NULL)
                return STATUS_NO_MEM;
            else if (*s == '\0')
                return STATUS_BAD_FORMAT;

            errno = 0;
            char *end = NULL;
            #ifdef PLATFORM_BSD
                long value = ::strtol(s, &end, 10);
            #else
                long long value = ::strtoll(s, &end, 10);
            #endif /* PLATFORM_BSD */

            if ((errno != 0) || (*end != '\0'))
                return STATUS_BAD_FORMAT;

            *dst = value;
            return STATUS_OK;
        }

        status_t PullParser::parse_uint64(const LSPString *str, uint64_t *dst)
        {
            const char *s = str->get_utf8();
            if (s == NULL)
                return STATUS_NO_MEM;
            else if (*s == '\0')
                return STATUS_BAD_FORMAT;

            errno = 0;
            char *end = NULL;
            #ifdef PLATFORM_BSD
                unsigned long value = ::strtoul(s, &end, 10);
            #else
                unsigned long long value = ::strtoull(s, &end, 10);
            #endif /* PLATFORM_BSD */

            if ((errno != 0) || (*end != '\0'))
                return STATUS_BAD_FORMAT;

            *dst = value;
            return STATUS_OK;
        }

        status_t PullParser::parse_float(const LSPString *str, float *dst, size_t *flags)
        {
            const char *s = str->get_utf8();
            if (s == NULL)
                return STATUS_NO_MEM;
            else if (*s == '\0')
                return STATUS_BAD_FORMAT;

            // Save and update locale
            SET_LOCALE_SCOPED(LC_NUMERIC, "C");

            // Parse float
            errno = 0;
            char *end   = NULL;
            float value = ::strtof(s, &end);
            size_t xf   = 0;

            bool success = (errno == 0);
            if ((success) && (end != NULL))
            {
                // Skip spaces
                while (((*end) == ' ') || ((*end) == '\t'))
                    ++ end;
                if (((end[0] == 'd') || (end[0] == 'D')) &&
                    ((end[1] == 'b') || (end[1] == 'B')))
                {
                    xf     |= SF_DECIBELS;
                    end    += 2;
                }
                // Skip spaces
                while (((*end) == ' ') || ((*end) == '\t'))
                    ++ end;
                if (*end != '\0')
                    success = false;
            }

            // Return result
            if (!success)
                return STATUS_BAD_FORMAT;

            *dst        = value;
            *flags     |= xf;
            return STATUS_OK;
        }

        status_t PullParser::parse_double(const LSPString *str, double *dst, size_t *flags)
        {
            const char *s = str->get_utf8();
            if (s == NULL)
                return STATUS_NO_MEM;
            else if (*s == '\0')
                return STATUS_BAD_FORMAT;

            // Save and update locale
            SET_LOCALE_SCOPED(LC_NUMERIC, "C");

            // Parse float
            errno = 0;
            char *end       = NULL;
            double value    = ::strtod(s, &end);
            size_t xf       = 0;

            bool success = (errno == 0);
            if ((success) && (end != NULL))
            {
                // Skip spaces
                while (((*end) == ' ') || ((*end) == '\t'))
                    ++ end;
                if (((end[0] == 'd') || (end[0] == 'D')) &&
                    ((end[1] == 'b') || (end[1] == 'B')))
                {
                    xf     |= SF_DECIBELS;
                    end    += 2;
                }
                // Skip spaces
                while (((*end) == ' ') || ((*end) == '\t'))
                    ++ end;
                if (*end != '\0')
                    success = false;
            }

            // Return result
            if (!success)
                return STATUS_BAD_FORMAT;

            *dst        = value;
            *flags     |= xf;
            return STATUS_OK;
        }

        status_t PullParser::parse_bool(const LSPString *str, bool *dst)
        {
            if (str->length() <= 0)
                return STATUS_BAD_FORMAT;

            // Parse boolean value
            if (str->equals_ascii_nocase("true"))
            {
                *dst    = true;
                return STATUS_OK;
            }
            else if (str->equals_ascii_nocase("false"))
            {
                *dst    = false;
                return STATUS_OK;
            }

            return STATUS_BAD_FORMAT;
        }

        status_t PullParser::parse_blob(const LSPString *str, blob_t *dst)
        {
            ssize_t pos, idx = str->index_of(':');
            if (idx < 0)
                return STATUS_BAD_FORMAT;

            // Content type is present
            if (idx > 0)
            {
                dst->ctype  = str->clone_utf8(ssize_t(0), idx);
                if (dst->ctype == NULL)
                    return STATUS_NO_MEM;
            }
            pos         = idx + 1;

            // Require length field and parse
            idx = str->index_of(pos, ':');
            if (idx <= pos)
                return STATUS_BAD_FORMAT;

            LSPString length;
            if (!length.set(str, pos, idx))
                return STATUS_NO_MEM;
            uint64_t size;
            status_t res = parse_uint64(&length, &size);
            if (res != STATUS_OK)
                return res;
            dst->length     = size_t(size);

            // Copy data
            dst->data   = str->clone_utf8(idx + 1);
            if (dst->data == NULL)
                return STATUS_NO_MEM;

            return STATUS_OK;
        }
    
    } /* namespace config */
} /* namespace lsp */
