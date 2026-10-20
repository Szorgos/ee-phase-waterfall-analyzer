/*
 * Copyright (C) 2023 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2023 Vladimir Sadovnikov <sadko4u@gmail.com>
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

#include <lsp-plug.in/fmt/json/dom/Boolean.h>
#include <lsp-plug.in/io/InStringSequence.h>
#include <lsp-plug.in/expr/Tokenizer.h>
#include <lsp-plug.in/expr/types.h>

namespace lsp
{
    namespace json
    {
        bool Boolean::valid() const
        {
            return is_bool();
        }

        status_t Boolean::create()
        {
            return Boolean::create(false);
        }

        status_t Boolean::create(bool value)
        {
            node_t *node    = new node_t();
            if (node == NULL)
                return STATUS_NO_MEM;

            node->refs      = 1;
            node->type      = JN_BOOL;
            node->bValue    = value;

            release_ref(pNode);
            pNode           = node;

            return STATUS_OK;
        }

        Boolean *Boolean::allocate(bool value)
        {
            Boolean *res = new Boolean();
            if (res == NULL)
                return NULL;
            else if (res->create(value) == STATUS_OK)
                return res;

            delete res;
            return NULL;
        }

        Boolean Boolean::build(bool value)
        {
            Boolean res;
            res.create(value);
            return res;
        }

        bool Boolean::get() const
        {
            if (pNode == NULL)
                return false;

            switch (pNode->type)
            {
                case JN_INT: return pNode->nValue != 0;
                case JN_DOUBLE: return pNode->fValue != 0.0f;
                case JN_BOOL: return pNode->bValue;
                case JN_STRING: {
                    io::InStringSequence is(pNode->sValue);
                    expr::Tokenizer t(&is);
                    bool bvalue;

                    switch (t.get_token(expr::TF_GET))
                    {
                        case expr::TT_IVALUE: bvalue = t.int_value() != 0; break;
                        case expr::TT_FVALUE: bvalue = t.float_value() != 0.0f; break;
                        case expr::TT_TRUE:   bvalue = true; break;
                        case expr::TT_FALSE:  bvalue = false; break;
                        default:
                            return false;
                    }

                    if (t.get_token(expr::TF_GET) != expr::TT_EOF)
                        return false;

                    return bvalue;
                }

                default:
                    break;
            }

            return false;
        }

        bool Boolean::sget() const
        {
            return (is_bool()) ? pNode->bValue : false;
        }

        status_t Boolean::cast()
        {
            return cast_to_int();
        }

        status_t Boolean::set(bool value)
        {
            node_t *node = clear_node(pNode);
            if (node == NULL)
                return STATUS_NO_MEM;

            pNode           = node;
            pNode->type     = JN_BOOL;
            pNode->bValue   = value;

            return STATUS_OK;
        }

        status_t Boolean::sset(bool value)
        {
            if (!is_bool())
                return STATUS_BAD_TYPE;

            pNode->bValue   = value;
            return STATUS_OK;
        }

    } /* namespace json */
} /* namespace lsp */


