/*
 * Copyright (C) 2026 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2026 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 1 мая 2020 г.
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

#include <lsp-plug.in/fmt/config/Serializer.h>
#include <lsp-plug.in/io/OutFileStream.h>
#include <lsp-plug.in/io/OutSequence.h>
#include <lsp-plug.in/io/OutStringSequence.h>
#include <lsp-plug.in/stdlib/locale.h>

namespace lsp
{
    namespace config
    {
        
        Serializer::Serializer()
        {
            pOut        = NULL;
            nWFlags     = 0;
        }
        
        Serializer::~Serializer()
        {
            do_close();
        }

        status_t Serializer::write_key(const LSPString *key)
        {
            ssize_t idx = -1;

            // Validate key
            for (size_t i=0, n=key->length(); i<n; ++i)
            {
                lsp_wchar_t ch = key->at(i);
                if (((ch >= 'a') && (ch <= 'z')) ||
                    ((ch >= 'A') && (ch <= 'Z')) ||
                    ((ch >= '0') && (ch <= '9')) ||
                    (ch == '_'))
                    continue;
                else if (ch == '/')
                {
                    if (idx < 0)
                    {
                        if (i != 0)
                            return STATUS_INVALID_VALUE;
                        idx = i;
                    }
                    continue;
                }

                return STATUS_INVALID_VALUE;
            }

            // Write key
            status_t res = pOut->write(key);
            if (res == STATUS_OK)
                res = pOut->write_ascii(" = ");

            return res;
        }

        status_t Serializer::write_uint(uint64_t v, size_t flags)
        {
            status_t res;
            char tmp[64];

            ::snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
            tmp[sizeof(tmp)-1] = '\0';

            if (flags & SF_QUOTED)
            {
                if ((res = pOut->write('\"')) != STATUS_OK)
                    return res;
                if ((res = pOut->write_ascii(tmp)) != STATUS_OK)
                    return res;
                res = pOut->write_ascii("\"\n");
            }
            else
            {
                if ((res = pOut->write_ascii(tmp)) != STATUS_OK)
                    return res;
                res = pOut->write('\n');
            }

            return res;
        }

        status_t Serializer::write_int(int64_t v, size_t flags)
        {
            status_t res;
            char tmp[64];

            snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
            tmp[sizeof(tmp)-1] = '\0';

            if (flags & SF_QUOTED)
            {
                if ((res = pOut->write('\"')) != STATUS_OK)
                    return res;
                if ((res = pOut->write_ascii(tmp)) != STATUS_OK)
                    return res;
                res = pOut->write_ascii("\"\n");
            }
            else
            {
                if ((res = pOut->write_ascii(tmp)) != STATUS_OK)
                    return res;
                res = pOut->write('\n');
            }

            return res;
        }

        status_t Serializer::write_float(double v, size_t flags)
        {
            // Save and update locale
            SET_LOCALE_SCOPED(LC_NUMERIC, "C");

            status_t res;
            char tmp[64];
            const char *fmt = "%f";

            // Analyze format
            if (flags & SF_DECIBELS)
            {
                switch (flags & SF_PREC_MASK)
                {
                    case SF_PREC_SHORT: fmt = "%.1f db"; break;
                    case SF_PREC_LONG: fmt = "%.4f db"; break;
                    case SF_PREC_SCI: fmt = "%e db"; break;
                    default: fmt = "%.2f db"; break;
                }
            }
            else
            {
                switch (flags & SF_PREC_MASK)
                {
                    case SF_PREC_SHORT: fmt = "%.2f"; break;
                    case SF_PREC_LONG: fmt = "%.10f"; break;
                    case SF_PREC_SCI: fmt = "%e"; break;
                    default: fmt = "%.5f"; break;
                }
            }
            ::snprintf(tmp, sizeof(tmp), fmt, v);
            tmp[sizeof(tmp)-1] = '\0';

            // Write value
            if (flags & SF_QUOTED)
            {
                if ((res = pOut->write('\"')) != STATUS_OK)
                    return res;
                if ((res = pOut->write_ascii(tmp)) != STATUS_OK)
                    return res;
                res = pOut->write_ascii("\"\n");
            }
            else
            {
                if ((res = pOut->write_ascii(tmp)) != STATUS_OK)
                    return res;
                res = pOut->write('\n');
            }

            return res;
        }

        status_t Serializer::write_bool(bool v, size_t flags)
        {
            status_t res;

            // Write value
            if (flags & SF_QUOTED)
            {
                if ((res = pOut->write('\"')) != STATUS_OK)
                    return res;
                if ((res = pOut->write_ascii((v) ? "true" : "false")) != STATUS_OK)
                    return res;
                res = pOut->write_ascii("\"\n");
            }
            else
            {
                if ((res = pOut->write_ascii((v) ? "true" : "false")) != STATUS_OK)
                    return res;
                res = pOut->write('\n');
            }

            return res;
        }

        status_t Serializer::open(const char *path, const char *charset)
        {
            if (pOut != NULL)
                return STATUS_BAD_STATE;
            else if (path == NULL)
                return STATUS_BAD_ARGUMENTS;

            io::OutFileStream *ofs = new io::OutFileStream();
            if (ofs == NULL)
                return STATUS_NO_MEM;
            status_t res = ofs->open(path, io::File::FM_WRITE | io::File::FM_TRUNC | io::File::FM_CREATE);
            if (res == STATUS_OK)
            {
                res     = wrap(ofs, WRAP_CLOSE | WRAP_DELETE, charset);
                if (res == STATUS_OK)
                    return res;
                ofs->close();
            }
            delete ofs;

            return res;
        }

        status_t Serializer::open(const LSPString *path, const char *charset)
        {
            if (pOut != NULL)
                return STATUS_BAD_STATE;
            else if (path == NULL)
                return STATUS_BAD_ARGUMENTS;

            io::OutFileStream *ofs = new io::OutFileStream();
            if (ofs == NULL)
                return STATUS_NO_MEM;
            status_t res = ofs->open(path, io::File::FM_WRITE | io::File::FM_TRUNC | io::File::FM_CREATE);
            if (res == STATUS_OK)
            {
                res     = wrap(ofs, WRAP_CLOSE | WRAP_DELETE, charset);
                if (res == STATUS_OK)
                    return res;
                ofs->close();
            }
            delete ofs;

            return res;
        }

        status_t Serializer::open(const io::Path *path, const char *charset)
        {
            if (pOut != NULL)
                return STATUS_BAD_STATE;
            else if (path == NULL)
                return STATUS_BAD_ARGUMENTS;

            io::OutFileStream *ofs = new io::OutFileStream();
            if (ofs == NULL)
                return STATUS_NO_MEM;
            status_t res = ofs->open(path, io::File::FM_WRITE | io::File::FM_TRUNC | io::File::FM_CREATE);
            if (res == STATUS_OK)
            {
                res     = wrap(ofs, WRAP_CLOSE | WRAP_DELETE, charset);
                if (res == STATUS_OK)
                    return res;
                ofs->close();
            }
            delete ofs;

            return res;
        }

        status_t Serializer::wrap(LSPString *str)
        {
            if (pOut != NULL)
                return STATUS_BAD_STATE;
            else if (str == NULL)
                return STATUS_BAD_ARGUMENTS;

            io::OutStringSequence *seq = new io::OutStringSequence(str, false);
            if (seq == NULL)
                return STATUS_NO_MEM;

            status_t res = wrap(seq, WRAP_CLOSE | WRAP_DELETE);
            if (res == STATUS_OK)
                return res;

            seq->close();
            delete seq;

            return res;
        }

        status_t Serializer::wrap(io::IOutStream *os, size_t flags, const char *charset)
        {
            if (pOut != NULL)
                return STATUS_BAD_STATE;
            else if (os == NULL)
                return STATUS_BAD_ARGUMENTS;

            io::OutSequence *seq = new io::OutSequence();
            if (seq == NULL)
                return STATUS_NO_MEM;

            status_t res = seq->wrap(os, flags, charset);
            if (res == STATUS_OK)
            {
                if ((res = wrap(seq, WRAP_CLOSE | WRAP_DELETE)) == STATUS_OK)
                    return res;
                seq->close();
            }

            delete seq;
            return res;
        }

        status_t Serializer::wrap(io::IOutSequence *seq, size_t flags)
        {
            if (pOut != NULL)
                return STATUS_BAD_STATE;
            else if (seq == NULL)
                return STATUS_BAD_ARGUMENTS;

            pOut            = seq;
            nWFlags         = flags;

            return STATUS_OK;
        }

        status_t Serializer::do_close()
        {
            if (pOut == NULL)
                return STATUS_OK;

            // Close handles
            status_t res = STATUS_OK;
            if (nWFlags & WRAP_CLOSE)
                res     = update_status(res, pOut->close());
            if (nWFlags & WRAP_DELETE)
                delete pOut;

            pOut = NULL;

            return res;
        }

        status_t Serializer::close()
        {
            return do_close();
        }

        status_t Serializer::write_comment(const LSPString *v)
        {
            if (pOut == NULL)
                return STATUS_CLOSED;

            status_t res;

            size_t first = 0;
            while (true)
            {
                if ((res = pOut->write_ascii("# ")) != STATUS_OK)
                    return res;
                ssize_t split = v->index_of(first, '\n');
                if (split < 0)
                {
                    if ((res = pOut->write(v, first)) == STATUS_OK)
                        res = pOut->write('\n');
                    return res;
                }
                else
                {
                    if ((res = pOut->write(v, first, split)) != STATUS_OK)
                        return res;
                    if ((res = pOut->write('\n')) != STATUS_OK)
                        return res;
                    first = split + 1;
                }
            }
        }

        status_t Serializer::write_escaped(const LSPString *key, size_t flags)
        {
            status_t res = STATUS_OK;

            if (flags & SF_QUOTED)
            {
                if ((res = pOut->write('\"')) != STATUS_OK)
                    return res;
            }

            size_t first = 0, last = 0, len = key->length();
            while (last < len)
            {
                // Analyze character
                lsp_wchar_t ch = key->char_at(last);
                switch (ch)
                {
                    case '\n':  ch = 'n'; break;
                    case '\r':  ch = 'r'; break;
                    case '\t':  ch = 't'; break;

                    case '\"':
                    case '\\':
                        break;

                    case '#':
                    case ' ':
                        if (!(flags & SF_QUOTED))
                            break;
                        ++last;
                        continue;

                    default:
                        ++last;
                        continue;
                }

                // Emit characters in queue
                if (first < last)
                {
                    if ((res = pOut->write(key, first, last)) != STATUS_OK)
                        return res;
                }

                // Emit escaped character
                if ((res = pOut->write('\\')) != STATUS_OK)
                    return res;
                if ((res = pOut->write(ch)) != STATUS_OK)
                    return res;
                first   = ++last;
            }

            // Write tail
            if (first < last)
                res = pOut->write(key, first, last);

            if (flags & SF_QUOTED)
            {
                if ((res = pOut->write('\"')) != STATUS_OK)
                    return res;
            }

            return res;
        }

        status_t Serializer::write_comment(const char *v)
        {
            LSPString tmp;
            if (!tmp.set_utf8(v))
                return STATUS_NO_MEM;
            return write_comment(&tmp);
        }

        status_t Serializer::writeln()
        {
            return (pOut != NULL) ? pOut->write('\n') : STATUS_CLOSED;
        }

        status_t Serializer::write(const char *key, const value_t *v, size_t flags)
        {
            LSPString tmp;
            if (!tmp.set_utf8(key))
                return STATUS_NO_MEM;
            return write(&tmp, v, flags);
        }

        status_t Serializer::write(const LSPString *key, const value_t *v, size_t flags)
        {
            switch (flags & SF_TYPE_MASK)
            {
                case SF_TYPE_I32:   return write_i32(key, v->i32, flags);
                case SF_TYPE_U32:   return write_u32(key, v->u32, flags);
                case SF_TYPE_I64:   return write_i64(key, v->i64, flags);
                case SF_TYPE_U64:   return write_u64(key, v->u64, flags);
                case SF_TYPE_F32:   return write_f32(key, v->f32, flags);
                case SF_TYPE_F64:   return write_f64(key, v->f64, flags);
                case SF_TYPE_BOOL:  return write_bool(key, v->bval, flags);
                case SF_TYPE_STR:   return write_string(key, v->str, flags);
                case SF_TYPE_BLOB:  return write_blob(key, &v->blob, flags);
                default: break;
            }

            return STATUS_BAD_TYPE;
        }

        status_t Serializer::write(const config::param_t * param)
        {
            if (param == NULL)
                return STATUS_BAD_ARGUMENTS;
            return write(*param);
        }

        status_t Serializer::write(const config::param_t & param)
        {
            if (param.flags & SF_COMMENT)
            {
                status_t res = write_comment(&param.comment);
                if (res != STATUS_OK)
                    return res;
            }
            return write(&param.name, &param.v, param.flags);
        }

        status_t Serializer::write_i32(const char *key, int32_t value, size_t flags)
        {
            LSPString tmp;
            if (!tmp.set_utf8(key))
                return STATUS_NO_MEM;
            return write_i32(&tmp, value, flags);
        }

        status_t Serializer::write_i32(const LSPString *key, int32_t value, size_t flags)
        {
            if (pOut == NULL)
                return STATUS_CLOSED;
            status_t res = write_key(key);
            if (res != STATUS_OK)
                return res;

            if (flags & SF_TYPE_SET)
            {
                if ((res = pOut->write_ascii("i32:")) != STATUS_OK)
                    return res;
            }

            return write_int(value, flags);
        }

        status_t Serializer::write_u32(const char *key, uint32_t value, size_t flags)
        {
            LSPString tmp;
            if (!tmp.set_utf8(key))
                return STATUS_NO_MEM;
            return write_u32(&tmp, value, flags);
        }

        status_t Serializer::write_u32(const LSPString *key, uint32_t value, size_t flags)
        {
            if (pOut == NULL)
                return STATUS_CLOSED;
            status_t res = write_key(key);
            if (res != STATUS_OK)
                return res;

            if (flags & SF_TYPE_SET)
            {
                if ((res = pOut->write_ascii("u32:")) != STATUS_OK)
                    return res;
            }

            return write_uint(value, flags);
        }

        status_t Serializer::write_f32(const char *key, float value, size_t flags)
        {
            LSPString tmp;
            if (!tmp.set_utf8(key))
                return STATUS_NO_MEM;
            return write_f32(&tmp, value, flags);
        }

        status_t Serializer::write_f32(const LSPString *key, float value, size_t flags)
        {
            if (pOut == NULL)
                return STATUS_CLOSED;
            status_t res = write_key(key);
            if (res != STATUS_OK)
                return res;

            if (flags & SF_TYPE_SET)
            {
                if ((res = pOut->write_ascii("f32:")) != STATUS_OK)
                    return res;
            }

            return write_float(value, flags);
        }

        status_t Serializer::write_i64(const char *key, int64_t value, size_t flags)
        {
            LSPString tmp;
            if (!tmp.set_utf8(key))
                return STATUS_NO_MEM;
            return write_i64(&tmp, value, flags);
        }

        status_t Serializer::write_i64(const LSPString *key, int64_t value, size_t flags)
        {
            if (pOut == NULL)
                return STATUS_CLOSED;
            status_t res = write_key(key);
            if (res != STATUS_OK)
                return res;

            if (flags & SF_TYPE_SET)
            {
                if ((res = pOut->write_ascii("i64:")) != STATUS_OK)
                    return res;
            }

            return write_int(value, flags);
        }

        status_t Serializer::write_u64(const char *key, uint64_t value, size_t flags)
        {
            LSPString tmp;
            if (!tmp.set_utf8(key))
                return STATUS_NO_MEM;
            return write_u64(&tmp, value, flags);
        }

        status_t Serializer::write_u64(const LSPString *key, uint64_t value, size_t flags)
        {
            if (pOut == NULL)
                return STATUS_CLOSED;
            status_t res = write_key(key);
            if (res != STATUS_OK)
                return res;

            if (flags & SF_TYPE_SET)
            {
                if ((res = pOut->write_ascii("u64:")) != STATUS_OK)
                    return res;
            }

            return write_uint(value, flags);
        }

        status_t Serializer::write_f64(const char *key, double value, size_t flags)
        {
            LSPString tmp;
            if (!tmp.set_utf8(key))
                return STATUS_NO_MEM;
            return write_f64(&tmp, value, flags);
        }

        status_t Serializer::write_f64(const LSPString *key, double value, size_t flags)
        {
            if (pOut == NULL)
                return STATUS_CLOSED;
            status_t res = write_key(key);
            if (res != STATUS_OK)
                return res;

            if (flags & SF_TYPE_SET)
            {
                if ((res = pOut->write_ascii("f64:")) != STATUS_OK)
                    return res;
            }

            return write_float(value, flags);
        }

        status_t Serializer::write_bool(const char *key, bool value, size_t flags)
        {
            LSPString tmp;
            if (!tmp.set_utf8(key))
                return STATUS_NO_MEM;
            return write_bool(&tmp, value, flags);
        }

        status_t Serializer::write_bool(const LSPString *key, bool value, size_t flags)
        {
            if (pOut == NULL)
                return STATUS_CLOSED;
            status_t res = write_key(key);
            if (res != STATUS_OK)
                return res;

            if (flags & SF_TYPE_SET)
            {
                if ((res = pOut->write_ascii("bool:")) != STATUS_OK)
                    return res;
            }

            return write_bool(value, flags);
        }

        status_t Serializer::write_string_impl(const LSPString *key, const LSPString *v, size_t flags)
        {
            if (pOut == NULL)
                return STATUS_CLOSED;
            status_t res = write_key(key);
            if (res != STATUS_OK)
                return res;

            if (flags & SF_TYPE_SET)
            {
                if ((res = pOut->write_ascii("str:")) != STATUS_OK)
                    return res;
            }

            if ((res = write_escaped(v, flags)) != STATUS_OK)
                return res;

            return pOut->write('\n');
        }

        status_t Serializer::write_string(const LSPString *key, const LSPString *v, size_t flags)
        {
            return write_string_impl(key, v, flags);
        }

        status_t Serializer::write_string(const LSPString *key, const char *v, size_t flags)
        {
            LSPString tmp;
            if (!tmp.set_utf8(v))
                return STATUS_NO_MEM;
            return write_string_impl(key, &tmp, flags);
        }

        status_t Serializer::write_string(const char *key, const LSPString *v, size_t flags)
        {
            LSPString tmp;
            if (!tmp.set_utf8(key))
                return STATUS_NO_MEM;
            return write_string_impl(&tmp, v, flags);
        }

        status_t Serializer::write_string(const char *key, const char *v, size_t flags)
        {
            LSPString tmp1, tmp2;
            if ((!tmp1.set_utf8(key)) || (!tmp2.set_utf8(v)))
                return STATUS_NO_MEM;
            return write_string_impl(&tmp1, &tmp2, flags);
        }

        status_t Serializer::write_blob(const LSPString *key, const blob_t *v, size_t flags)
        {
            if (pOut == NULL)
                return STATUS_CLOSED;
            status_t res = write_key(key);
            if (res != STATUS_OK)
                return res;

            if ((res = pOut->write_ascii("blob:")) != STATUS_OK)
                return res;
            if ((res = pOut->write('\"')) != STATUS_OK)
                return res;

            // Write blob data
            LSPString tmp;
            if (v->ctype != NULL)
            {
                if (!tmp.set_utf8(v->ctype))
                    return STATUS_NO_MEM;
            }
            if (!tmp.append(':'))
                return STATUS_NO_MEM;
            if (!tmp.fmt_append_ascii("%llu:", (unsigned long long)v->length))
                return STATUS_NO_MEM;
            if ((res = write_escaped(&tmp, 0)) != STATUS_OK)
                return res;
            if (!tmp.set_utf8(v->data))
                return STATUS_NO_MEM;
            if ((res = write_escaped(&tmp, 0)) != STATUS_OK)
                return res;
            res = pOut->write_ascii("\"\n");

            return res;
        }

        status_t Serializer::write_blob(const char *key, const blob_t *v, size_t flags)
        {
            LSPString tmp;
            if (!tmp.set_utf8(key))
                return STATUS_NO_MEM;
            return write_blob(&tmp, v, flags);
        }

    } /* namespace config */
} /* namespace lsp */
