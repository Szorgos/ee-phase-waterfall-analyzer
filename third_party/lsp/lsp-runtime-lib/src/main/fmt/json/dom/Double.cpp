/*
 * Copyright (C) 2021 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2021 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 13 февр. 2021 г.
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

#include <lsp-plug.in/fmt/json/dom/Double.h>
#include <lsp-plug.in/io/InStringSequence.h>
#include <lsp-plug.in/expr/Tokenizer.h>
#include <lsp-plug.in/expr/types.h>

namespace lsp
{
    namespace json
    {
        bool Double::valid() const
        {
            return is_double();
        }

        status_t Double::create()
        {
            return Double::create(0.0);
        }

        status_t Double::create(double value)
        {
            node_t *node    = new node_t();
            if (node == NULL)
                return STATUS_NO_MEM;

            node->refs      = 1;
            node->type      = JN_DOUBLE;
            node->fValue    = value;

            release_ref(pNode);
            pNode           = node;

            return STATUS_OK;
        }

        Double *Double::allocate(double value)
        {
            Double *res = new Double();
            if (res == NULL)
                return NULL;
            else if (res->create(value) == STATUS_OK)
                return res;

            delete res;
            return NULL;
        }

        Double Double::build(double value)
        {
            Double res;
            res.create(value);
            return res;
        }

        double Double::get() const
        {
            if (pNode == NULL)
                return 0.0;

            switch (pNode->type)
            {
                case JN_INT: return pNode->nValue;
                case JN_DOUBLE: return pNode->fValue;
                case JN_BOOL: return (pNode->bValue) ? 1.0 : 0.0;
                case JN_STRING: {
                    io::InStringSequence is(pNode->sValue);
                    expr::Tokenizer t(&is);
                    double fvalue;

                    switch (t.get_token(expr::TF_GET))
                    {
                        case expr::TT_IVALUE: fvalue = t.int_value(); break;
                        case expr::TT_FVALUE: fvalue = t.float_value(); break;
                        case expr::TT_TRUE:   fvalue = 1.0; break;
                        case expr::TT_FALSE:  fvalue = 0.0; break;
                        default:
                            return 0.0;
                    }

                    if (t.get_token(expr::TF_GET) != expr::TT_EOF)
                        return 0.0;

                    return fvalue;
                }

                default:
                    break;
            }

            return 0.0;
        }

        double Double::sget() const
        {
            return (is_double()) ? pNode->fValue : 0;
        }

        status_t Double::cast()
        {
            return cast_to_int();
        }

        status_t Double::set(double value)
        {
            node_t *node = clear_node(pNode);
            if (node == NULL)
                return STATUS_NO_MEM;

            pNode           = node;
            pNode->type     = JN_DOUBLE;
            pNode->fValue   = value;

            return STATUS_OK;
        }

        status_t Double::sset(double value)
        {
            if (!is_double())
                return STATUS_BAD_TYPE;

            pNode->fValue   = value;
            return STATUS_OK;
        }

    } /* namespace json */
} /* namespace lsp */



