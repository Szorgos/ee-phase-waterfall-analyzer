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

#include <lsp-plug.in/fmt/json/dom/Integer.h>
#include <lsp-plug.in/io/InStringSequence.h>
#include <lsp-plug.in/expr/Tokenizer.h>
#include <lsp-plug.in/expr/types.h>

namespace lsp
{
    namespace json
    {
        bool Integer::valid() const
        {
            return is_int();
        }

        status_t Integer::create()
        {
            return Integer::create(0);
        }

        status_t Integer::create(ssize_t value)
        {
            node_t *node    = new node_t();
            if (node == NULL)
                return STATUS_NO_MEM;

            node->refs      = 1;
            node->type      = JN_INT;
            node->nValue    = value;

            release_ref(pNode);
            pNode           = node;

            return STATUS_OK;
        }

        Integer *Integer::allocate(ssize_t value)
        {
            Integer *res = new Integer();
            if (res == NULL)
                return NULL;
            else if (res->create(value) == STATUS_OK)
                return res;

            delete res;
            return NULL;
        }

        Integer Integer::build(ssize_t value)
        {
            Integer res;
            res.create(value);
            return res;
        }

        ssize_t Integer::get() const
        {
            if (pNode == NULL)
                return 0;

            switch (pNode->type)
            {
                case JN_INT: return pNode->nValue;
                case JN_DOUBLE: return pNode->fValue;
                case JN_BOOL: return (pNode->bValue) ? 1 : 0;
                case JN_STRING: {
                    io::InStringSequence is(pNode->sValue);
                    expr::Tokenizer t(&is);
                    ssize_t ivalue;

                    switch (t.get_token(expr::TF_GET))
                    {
                        case expr::TT_IVALUE: ivalue = t.int_value(); break;
                        case expr::TT_FVALUE: ivalue = t.int_value(); break;
                        case expr::TT_TRUE:   ivalue = 1; break;
                        case expr::TT_FALSE:  ivalue = 0; break;
                        default:
                            return 0;
                    }

                    if (t.get_token(expr::TF_GET) != expr::TT_EOF)
                        return 0;

                    return ivalue;
                }

                default:
                    break;
            }

            return 0;
        }

        ssize_t Integer::sget() const
        {
            return (is_int()) ? pNode->nValue : 0;
        }

        status_t Integer::cast()
        {
            return cast_to_int();
        }

        status_t Integer::set(ssize_t value)
        {
            node_t *node = clear_node(pNode);
            if (node == NULL)
                return STATUS_NO_MEM;

            pNode           = node;
            pNode->type     = JN_INT;
            pNode->nValue   = value;

            return STATUS_OK;
        }

        status_t Integer::sset(ssize_t value)
        {
            if (!is_int())
                return STATUS_BAD_TYPE;

            pNode->nValue   = value;
            return STATUS_OK;
        }
    }
}


