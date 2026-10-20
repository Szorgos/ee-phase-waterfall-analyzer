/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 19 сент. 2019 г.
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

#include <lsp-plug.in/expr/types.h>
#include <lsp-plug.in/expr/Tokenizer.h>
#include <lsp-plug.in/io/InStringSequence.h>
#include <lsp-plug.in/stdlib/locale.h>
#include <lsp-plug.in/stdlib/math.h>

namespace lsp
{
    namespace expr
    {
        template <class T>
            static inline const char *special_value(const T f)
            {
                if (isinf(f))
                    return (f < 0.0f) ? "-inf" : "inf";
                if (isnan(f))
                    return "nan";
                return NULL;
            }

        void init_value(value_t *dst)
        {
            dst->type       = VT_UNDEF;
            dst->v_str      = NULL;
        }

        void init_value_null(value_t *dst)
        {
            dst->type       = VT_NULL;
            dst->v_str      = NULL;
        }

        void init_value_undef(value_t *dst)
        {
            dst->type       = VT_UNDEF;
            dst->v_str      = NULL;
        }

        void init_value_int(value_t *dst, ssize_t value)
        {
            dst->type       = VT_INT;
            dst->v_int      = value;
        }

        void init_value_float(value_t *dst, double value)
        {
            dst->type       = VT_FLOAT;
            dst->v_float    = value;
        }

        void init_value_bool(value_t *dst, bool value)
        {
            dst->type       = VT_BOOL;
            dst->v_str      = NULL;
            dst->v_bool     = value;
        }

        status_t init_value_string(value_t *dst, const LSPString *value)
        {
            LSPString *str  = value->clone();
            if (str != NULL)
            {
                dst->type       = VT_STRING;
                dst->v_str      = str;
                return STATUS_OK;
            }

            dst->type       = VT_UNDEF;
            dst->v_str      = NULL;

            return STATUS_NO_MEM;
        }

        status_t init_value_string(value_t *dst, const char *value)
        {
            LSPString *str  = new LSPString();
            if (str != NULL)
            {
                if (str->set_utf8(value))
                {
                    dst->type       = VT_STRING;
                    dst->v_str      = str;
                    return STATUS_OK;
                }
                delete str;
            }

            dst->type       = VT_UNDEF;
            dst->v_str      = NULL;

            return STATUS_NO_MEM;
        }

        status_t init_value_string(value_t *dst, const char *value, const char *charset)
        {
            LSPString *str  = new LSPString();
            if (str != NULL)
            {
                if (str->set_native(value, charset))
                {
                    dst->type       = VT_STRING;
                    dst->v_str      = str;
                    return STATUS_OK;
                }
                delete str;
            }

            dst->type       = VT_UNDEF;
            dst->v_str      = NULL;

            return STATUS_NO_MEM;
        }

        inline void destroy_value_internal(value_t *value)
        {
            if (value->type == VT_STRING)
            {
                delete value->v_str;
                value->v_str        = NULL;
            }
        }

        void set_value_null(value_t *dst)
        {
            destroy_value_internal(dst);
            dst->type       = VT_NULL;
            dst->v_str      = NULL;
        }

        void set_value_undef(value_t *dst)
        {
            destroy_value_internal(dst);
            dst->type       = VT_UNDEF;
            dst->v_str      = NULL;
        }

        void set_value_int(value_t *dst, ssize_t value)
        {
            destroy_value_internal(dst);
            dst->type       = VT_INT;
            dst->v_str      = NULL;
            dst->v_int      = value;
        }

        void set_value_float(value_t *dst, double value)
        {
            destroy_value_internal(dst);
            dst->type       = VT_FLOAT;
            dst->v_str      = NULL;
            dst->v_float    = value;
        }

        void set_value_bool(value_t *dst, bool value)
        {
            destroy_value_internal(dst);
            dst->type       = VT_BOOL;
            dst->v_str      = NULL;
            dst->v_bool     = value;
        }

        status_t init_value(value_t *dst, const value_t *src)
        {
            if (src == NULL)
            {
                dst->type       = VT_NULL;
                dst->v_str      = NULL;
                return STATUS_OK;
            }
            else if ((src->type == VT_STRING) && (src->v_str != NULL))
            {
                dst->v_str      = src->v_str->clone();
                if (dst->v_str == NULL)
                {
                    dst->type       = VT_UNDEF;
                    return STATUS_NO_MEM;
                }

                dst->type       = VT_STRING;
            }
            else
                *dst        = *src;
            return STATUS_OK;
        }

        void init_value_move(value_t *dst, value_t *src)
        {
            *dst            = *src;
            src->type       = VT_UNDEF;
            src->v_str      = NULL;
        }

        status_t set_value_string(value_t *dst, const LSPString *value)
        {
            if (value == NULL)
            {
                set_value_null(dst);
                return STATUS_OK;
            }
            else if (dst->type == VT_STRING)
                return (dst->v_str->set(value)) ? STATUS_OK : STATUS_NO_MEM;

            LSPString *copy = value->clone();
            if (copy == NULL)
                return STATUS_NO_MEM;

            dst->type       = VT_STRING;
            dst->v_str      = copy;
            return STATUS_OK;
        }

        status_t set_value_string(value_t *dst, const char *value)
        {
            if (value == NULL)
            {
                set_value_null(dst);
                return STATUS_OK;
            }
            else if (dst->type == VT_STRING)
                return (dst->v_str->set_utf8(value)) ? STATUS_OK : STATUS_NO_MEM;

            LSPString *tmp = new LSPString();
            if (tmp == NULL)
                return STATUS_NO_MEM;
            if (!tmp->set_utf8(value))
            {
                delete tmp;
                return STATUS_NO_MEM;
            }

            dst->type       = VT_STRING;
            dst->v_str      = tmp;
            return STATUS_OK;
        }

        status_t set_value_string(value_t *dst, const char *value, const char *charset)
        {
            if (value == NULL)
            {
                set_value_null(dst);
                return STATUS_OK;
            }
            else if (dst->type == VT_STRING)
                return (dst->v_str->set_native(value, charset)) ? STATUS_OK : STATUS_NO_MEM;

            LSPString *tmp = new LSPString();
            if (tmp == NULL)
                return STATUS_NO_MEM;
            if (!tmp->set_native(value, charset))
            {
                delete tmp;
                return STATUS_NO_MEM;
            }

            dst->type       = VT_STRING;
            dst->v_str      = tmp;
            return STATUS_OK;
        }

        status_t copy_value(value_t *dst, const value_t *src)
        {
            if (src == NULL)
            {
                set_value_null(dst);
                return STATUS_OK;
            }
            else if (src->type == VT_STRING)
            {
                if (dst->type == VT_STRING)
                {
                    if (!dst->v_str->set(src->v_str))
                        return STATUS_NO_MEM;
                }
                else
                {
                    LSPString *copy = src->v_str->clone();
                    if (copy == NULL)
                        return STATUS_NO_MEM;

                    dst->type       = VT_STRING;
                    dst->v_str      = copy;
                }
            }
            else
            {
                if (dst->type == VT_STRING)
                    delete dst->v_str;

                *dst        = *src;
            }
            return STATUS_OK;
        }

        void move_value(value_t *dst, value_t *src)
        {
            if (src->type == VT_STRING)
            {
                if (dst->type == VT_STRING)
                    delete dst->v_str;

                dst->type   = VT_STRING;
                dst->v_str  = src->v_str;
            }
            else
            {
                if (dst->type == VT_STRING)
                    delete dst->v_str;

                *dst        = *src;
            }

            src->type   = VT_UNDEF;
            src->v_str  = NULL;
        }

        void swap_value(value_t *dst, value_t *src)
        {
            lsp::swap(*dst, *src);
        }

        void destroy_value(value_t *value)
        {
            destroy_value_internal(value);
            value->type     = VT_UNDEF;
        }

        status_t fetch_int(ssize_t *dst, const value_t *v)
        {
            if (v->type != VT_INT)
                return STATUS_BAD_TYPE;
            *dst = v->v_int;
            return STATUS_OK;
        }

        status_t fetch_float(float *dst, const value_t *v)
        {
            if (v->type != VT_FLOAT)
                return STATUS_BAD_TYPE;
            *dst = v->v_float;
            return STATUS_OK;
        }

        status_t fetch_float(double *dst, const value_t *v)
        {
            if (v->type != VT_FLOAT)
                return STATUS_BAD_TYPE;
            *dst = v->v_float;
            return STATUS_OK;
        }

        status_t fetch_bool(bool *dst, const value_t *v)
        {
            if (v->type != VT_BOOL)
                return STATUS_BAD_TYPE;
            *dst = v->v_bool;
            return STATUS_OK;
        }

        status_t fetch_string(LSPString *dst, const value_t *v)
        {
            if (v->type != VT_STRING)
                return STATUS_BAD_TYPE;
            return (dst->set(v->v_str)) ? STATUS_OK : STATUS_NO_MEM;
        }

        status_t fetch_as_int(ssize_t *dst, const value_t *v)
        {
            if (v->type == VT_INT)
            {
                *dst    = v->v_int;
                return STATUS_OK;
            }

            // Perform cast
            value_t tmp;
            init_value(&tmp);
            lsp_finally { destroy_value(&tmp); };

            status_t res = cast_int(&tmp, v);
            if (res == STATUS_OK)
                *dst    = tmp.v_int;

            return res;
        }

        status_t fetch_as_float(float *dst, const value_t *v)
        {
            if (v->type == VT_FLOAT)
            {
                *dst    = v->v_float;
                return STATUS_OK;
            }

            // Perform cast
            value_t tmp;
            init_value(&tmp);
            lsp_finally { destroy_value(&tmp); };

            status_t res = cast_float(&tmp, v);
            if (res == STATUS_OK)
                *dst    = tmp.v_float;

            return res;
        }

        status_t fetch_as_float(double *dst, const value_t *v)
        {
            if (v->type == VT_FLOAT)
            {
                *dst    = v->v_float;
                return STATUS_OK;
            }

            // Perform cast
            value_t tmp;
            init_value(&tmp);
            lsp_finally { destroy_value(&tmp); };

            status_t res = cast_float(&tmp, v);
            if (res == STATUS_OK)
                *dst    = tmp.v_float;

            return res;
        }

        status_t fetch_as_bool(bool *dst, const value_t *v)
        {
            if (v->type == VT_BOOL)
            {
                *dst    = v->v_bool;
                return STATUS_OK;
            }

            // Perform cast
            value_t tmp;
            init_value(&tmp);
            lsp_finally { destroy_value(&tmp); };

            status_t res = cast_bool(&tmp, v);
            if (res == STATUS_OK)
                *dst    = tmp.v_bool;

            return res;
        }

        status_t fetch_as_string(LSPString *dst, const value_t *v)
        {
            if (v->type == VT_STRING)
                return (dst->set(v->v_str)) ? STATUS_OK : STATUS_NO_MEM;

            // Perform cast
            value_t tmp;
            init_value(&tmp);
            lsp_finally { destroy_value(&tmp); };

            status_t res = cast_string(&tmp, v);
            if (res == STATUS_OK)
                dst->swap(tmp.v_str);

            return res;
        }

        status_t cast_value(value_t *v, value_type_t type)
        {
            switch (type)
            {
                case VT_INT: return cast_int(v);
                case VT_FLOAT: return cast_float(v);
                case VT_BOOL: return cast_bool(v);
                case VT_STRING: return cast_string(v);
                case VT_NULL:
                    set_value_null(v);
                    return STATUS_OK;
                case VT_UNDEF:
                    set_value_undef(v);
                    return STATUS_OK;
            }

            return STATUS_BAD_TYPE;
        }

        status_t cast_value(value_t *dst, const value_t *v, value_type_t type)
        {
            status_t res = copy_value(dst, v);
            if (res != STATUS_OK)
                return res;

            switch (type)
            {
                case VT_INT: return cast_int(dst);
                case VT_FLOAT: return cast_float(dst);
                case VT_BOOL: return cast_bool(dst);
                case VT_STRING: return cast_string(dst);
                case VT_NULL:
                    set_value_null(dst);
                    return STATUS_OK;
                case VT_UNDEF:
                    set_value_undef(dst);
                    return STATUS_OK;
            }

            return STATUS_BAD_TYPE;
        }

        status_t cast_int(value_t *v)
        {
            switch (v->type)
            {
                case VT_INT:    return STATUS_OK;
                case VT_FLOAT:  v->v_int = ssize_t(v->v_float); break;
                case VT_BOOL:   v->v_int = (v->v_bool) ? 1 : 0; break;
                case VT_STRING:
                {
                    // Parse integer/float number as string and cast to integer
                    io::InStringSequence s(v->v_str);
                    Tokenizer t(&s);
                    ssize_t ivalue;

                    switch (t.get_token(TF_GET))
                    {
                        case TT_IVALUE: ivalue = t.int_value(); break;
                        case TT_FVALUE: ivalue = t.int_value(); break;
                        case TT_TRUE:   ivalue = 1; break;
                        case TT_FALSE:  ivalue = 0; break;
                        default:
                            delete v->v_str;
                            v->type     = VT_UNDEF;
                            return STATUS_OK;
                    }

                    if (t.get_token(TF_GET) != TT_EOF)
                        return STATUS_BAD_FORMAT;
                    delete v->v_str;
                    v->v_int    = ivalue;
                    break;
                }
                case VT_NULL:
                case VT_UNDEF:
                    return STATUS_OK;
                default:
                    return STATUS_BAD_TYPE;
            }

            v->type     = VT_INT;
            return STATUS_OK;
        }

        status_t cast_float(value_t *v)
        {
            switch (v->type)
            {
                case VT_INT:    v->v_float = v->v_int; break;
                case VT_FLOAT:  return STATUS_OK;
                case VT_BOOL:   v->v_float = (v->v_bool) ? 1.0 : 0.0; break;
                case VT_STRING:
                {
                    // Parse integer/float number as string and cast to integer
                    io::InStringSequence s(v->v_str);
                    Tokenizer t(&s);
                    double fvalue;

                    switch (t.get_token(TF_GET))
                    {
                        case TT_IVALUE: fvalue = t.int_value(); break;
                        case TT_FVALUE: fvalue = t.float_value(); break;
                        case TT_TRUE:   fvalue = 1.0; break;
                        case TT_FALSE:  fvalue = 0.0; break;
                        default:
                            delete v->v_str;
                            v->type     = VT_UNDEF;
                            return STATUS_OK;
                    }

                    if (t.get_token(TF_GET) != TT_EOF)
                        return STATUS_BAD_FORMAT;
                    delete v->v_str;
                    v->v_float  = fvalue;
                    break;
                }
                case VT_NULL:
                case VT_UNDEF:
                    return STATUS_OK;
                default:
                    return STATUS_BAD_TYPE;
            }

            v->type     = VT_FLOAT;
            return STATUS_OK;
        }

        status_t cast_bool(value_t *v)
        {
            switch (v->type)
            {
                case VT_INT:    v->v_bool = v->v_int != 0; break;
                case VT_FLOAT:  v->v_bool = (v->v_float >= 0.5f) || (v->v_float <= -0.5f); break;
                case VT_BOOL:   return STATUS_OK;
                case VT_STRING:
                {
                    // Parse integer/float number as string and cast to integer
                    io::InStringSequence s(v->v_str);
                    Tokenizer t(&s);
                    bool bvalue;

                    switch (t.get_token(TF_GET))
                    {
                        case TT_IVALUE: bvalue = t.int_value() != 0; break;
                        case TT_FVALUE: bvalue = (t.float_value() > 0.5f) || (t.float_value() <= -0.5f); break;
                        case TT_TRUE:   bvalue = true; break;
                        case TT_FALSE:  bvalue = false; break;
                        default:
                            delete v->v_str;
                            v->type     = VT_UNDEF;
                            return STATUS_OK;
                    }

                    if (t.get_token(TF_GET) != TT_EOF)
                        return STATUS_BAD_FORMAT;
                    delete v->v_str;
                    v->v_bool   = bvalue;
                    break;
                }
                case VT_NULL:
                case VT_UNDEF:
                    return STATUS_OK;
                default:
                    return STATUS_BAD_TYPE;
            }

            v->type     = VT_BOOL;
            return STATUS_OK;
        }

        status_t cast_string(value_t *v)
        {
            LSPString tmp;

            switch (v->type)
            {
                case VT_INT:
                    if (!tmp.fmt_ascii("%ld", long(v->v_int)))
                        return STATUS_NO_MEM;
                    break;
                case VT_FLOAT:
                {
                    const char *special = special_value(v->v_float);
                    if (special)
                    {
                        if (!tmp.set_ascii(special))
                            return STATUS_NO_MEM;
                    }
                    else
                    {
                        // Update locale
                        SET_LOCALE_SCOPED(LC_NUMERIC, "C");
                        if (!tmp.fmt_ascii("%f", double(v->v_float)))
                            return STATUS_NO_MEM;
                    }
                    break;
                }
                case VT_BOOL:
                    if (!tmp.set_ascii((v->v_bool) ? "true" : "false"))
                        return STATUS_NO_MEM;
                    break;
                case VT_STRING:
                    return STATUS_OK;
                case VT_NULL:
                case VT_UNDEF:
                    return STATUS_OK;
                default:
                    return STATUS_BAD_TYPE;
            }

            LSPString *ns = tmp.release();
            if (ns == NULL)
                return STATUS_NO_MEM;

            v->type     = VT_STRING;
            v->v_str    = ns;

            return STATUS_OK;
        }

        status_t cast_string_ext(value_t *v)
        {
            LSPString tmp;

            switch (v->type)
            {
                case VT_INT:
                    if (!tmp.fmt_ascii("%ld", long(v->v_int)))
                        return STATUS_NO_MEM;
                    break;
                case VT_FLOAT:
                {
                    const char *special = special_value(v->v_float);
                    if (special)
                    {
                        if (!tmp.set_ascii(special))
                            return STATUS_NO_MEM;
                    }
                    else
                    {
                        // Update locale
                        SET_LOCALE_SCOPED(LC_NUMERIC, "C");
                        if (!tmp.fmt_ascii("%f", double(v->v_float)))
                            return STATUS_NO_MEM;
                    }
                    break;
                }
                case VT_BOOL:
                    if (!tmp.set_ascii((v->v_bool) ? "true" : "false"))
                        return STATUS_NO_MEM;
                    break;
                case VT_STRING:
                    return STATUS_OK;
                case VT_NULL:
                    if (!tmp.set_ascii("null"))
                        return STATUS_NO_MEM;
                    break;
                case VT_UNDEF:
                    if (!tmp.set_ascii("undef"))
                        return STATUS_NO_MEM;
                    break;
                default:
                    return STATUS_BAD_TYPE;
            }

            LSPString *ns = tmp.release();
            if (ns == NULL)
                return STATUS_NO_MEM;

            v->type     = VT_STRING;
            v->v_str    = ns;

            return STATUS_OK;
        }

        status_t cast_numeric(value_t *v)
        {
            switch (v->type)
            {
                case VT_INT:    return STATUS_OK;
                case VT_FLOAT:  return STATUS_OK;
                case VT_BOOL:
                    v->type     = VT_INT;
                    v->v_int    = (v->v_bool) ? 1 : 0;
                    break;
                case VT_STRING:
                {
                    // Parse integer/float number as string and cast to integer
                    value_t xv;
                    io::InStringSequence s(v->v_str);
                    Tokenizer t(&s);

                    switch (t.get_token(TF_GET))
                    {
                        case TT_IVALUE:
                            xv.type     = VT_INT;
                            xv.v_int    = t.int_value();
                            break;
                        case TT_FVALUE:
                            xv.type     = VT_FLOAT;
                            xv.v_float  = t.float_value();
                            break;
                        case TT_TRUE:
                            xv.type     = VT_INT;
                            xv.v_int    = 1;
                            break;
                        case TT_FALSE:
                            xv.type     = VT_INT;
                            xv.v_int    = 0;
                            break;
                        default:
                            delete v->v_str;
                            v->type     = VT_UNDEF;
                            return STATUS_OK;
                    }

                    if (t.get_token(TF_GET) != TT_EOF)
                        return STATUS_BAD_FORMAT;

                    delete v->v_str;
                    *v      = xv;
                    break;
                }
                case VT_NULL:
                case VT_UNDEF:
                    return STATUS_OK;
                default:
                    return STATUS_BAD_TYPE;
            }
            return STATUS_OK;
        }

        status_t cast_non_string(value_t *v)
        {
            if (v->type != VT_STRING)
                return STATUS_OK;

            // Parse integer/float number as string and cast to integer
            value_t xv;
            io::InStringSequence s(v->v_str);
            Tokenizer t(&s);

            switch (t.get_token(TF_GET))
            {
                case TT_IVALUE:
                    xv.type     = VT_INT;
                    xv.v_int    = t.int_value();
                    break;
                case TT_FVALUE:
                    xv.type     = VT_FLOAT;
                    xv.v_float  = t.float_value();
                    break;
                case TT_TRUE:
                    xv.type     = VT_BOOL;
                    xv.v_bool   = true;
                    break;
                case TT_FALSE:
                    xv.type     = VT_BOOL;
                    xv.v_bool   = 0;
                    break;
                default:
                    delete v->v_str;
                    v->type     = VT_UNDEF;
                    return STATUS_OK;
            }

            if (t.get_token(TF_GET) != TT_EOF)
                return STATUS_BAD_FORMAT;

            delete v->v_str;
            *v      = xv;

            return STATUS_OK;
        }

        status_t cast_int(value_t *dst, const value_t *v)
        {
            status_t res = copy_value(dst, v);
            if (res == STATUS_OK)
                res         = cast_int(dst);
            return res;
        }

        status_t cast_float(value_t *dst, const value_t *v)
        {
            status_t res = copy_value(dst, v);
            if (res == STATUS_OK)
                res         = cast_float(dst);
            return res;
        }

        status_t cast_bool(value_t *dst, const value_t *v)
        {
            status_t res = copy_value(dst, v);
            if (res == STATUS_OK)
                res         = cast_bool(dst);
            return res;
        }

        status_t cast_string(value_t *dst, const value_t *v)
        {
            status_t res = copy_value(dst, v);
            if (res == STATUS_OK)
                res         = cast_string(dst);
            return res;
        }

        status_t cast_string_ext(value_t *dst, const value_t *v)
        {
            status_t res = copy_value(dst, v);
            if (res == STATUS_OK)
                res         = cast_string_ext(dst);
            return res;
        }

        status_t cast_numeric(value_t *dst, const value_t *v)
        {
            status_t res = copy_value(dst, v);
            if (res == STATUS_OK)
                res         = cast_numeric(dst);
            return res;
        }

        status_t cast_non_string(value_t *dst, const value_t *v)
        {
            if (v->type != VT_STRING)
                return STATUS_OK;

            // Parse integer/float number as string and cast to integer
            value_t xv;
            io::InStringSequence s(v->v_str);
            Tokenizer t(&s);

            switch (t.get_token(TF_GET))
            {
                case TT_IVALUE:
                    xv.type     = VT_INT;
                    xv.v_int    = t.int_value();
                    break;
                case TT_FVALUE:
                    xv.type     = VT_FLOAT;
                    xv.v_float  = t.float_value();
                    break;
                case TT_TRUE:
                    xv.type     = VT_BOOL;
                    xv.v_bool   = true;
                    break;
                case TT_FALSE:
                    xv.type     = VT_BOOL;
                    xv.v_bool   = false;
                    break;
                default:
                    xv.type     = VT_UNDEF;
                    break;
            }

            if (t.get_token(TF_GET) != TT_EOF)
                return STATUS_BAD_FORMAT;

            return copy_value(dst, &xv);
        }

    } /* namespace expr */
} /* namespace lsp */


