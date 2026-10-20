/*
 * Copyright (C) 2023 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2023 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 23 февр. 2021 г.
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


#include <lsp-plug.in/fmt/json/dom/String.h>
#include <lsp-plug.in/io/InStringSequence.h>
#include <lsp-plug.in/expr/Tokenizer.h>
#include <lsp-plug.in/expr/types.h>
#include <lsp-plug.in/stdlib/locale.h>

namespace lsp
{
    namespace json
    {
        bool String::valid() const
        {
            return is_string();
        }

        status_t String::create()
        {
            node_t *node    = new node_t();
            if (node == NULL)
                return STATUS_NO_MEM;

            node->refs      = 1;
            node->type      = JN_STRING;
            node->sValue    = new LSPString();
            if (node->sValue == NULL)
            {
                delete node;
                return STATUS_NO_MEM;
            }

            release_ref(pNode);
            pNode           = node;

            return STATUS_OK;
        }

        status_t String::create(const LSPString *value)
        {
            if (value == NULL)
                return STATUS_BAD_ARGUMENTS;

            node_t *node    = new node_t();
            if (node == NULL)
                return STATUS_NO_MEM;

            node->refs      = 1;
            node->type      = JN_STRING;
            node->sValue    = value->clone();
            if (node->sValue == NULL)
            {
                delete node;
                return STATUS_NO_MEM;
            }

            release_ref(pNode);
            pNode           = node;

            return STATUS_OK;
        }

        status_t String::create(const char *value, const char *charset)
        {
            if (value == NULL)
                return STATUS_BAD_ARGUMENTS;

            node_t *node    = new node_t();
            if (node == NULL)
                return STATUS_NO_MEM;

            node->refs      = 1;
            node->type      = JN_STRING;
            node->sValue    = new LSPString();
            if (node->sValue == NULL)
            {
                delete node;
                return STATUS_NO_MEM;
            }
            if (!node->sValue->set_native(value, charset))
            {
                delete node->sValue;
                delete node;
                return STATUS_NO_MEM;
            }

            release_ref(pNode);
            pNode           = node;

            return STATUS_OK;
        }

        String *String::allocate()
        {
            String *res = new String();
            if (res == NULL)
                return NULL;
            else if (res->create() == STATUS_OK)
                return res;

            delete res;
            return NULL;
        }

        String *String::allocate(const LSPString *value)
        {
            if (value == NULL)
                return NULL;

            String *res = new String();
            if (res == NULL)
                return NULL;
            else if (res->create(value) == STATUS_OK)
                return res;

            delete res;
            return NULL;
        }

        String *String::allocate(const char *value, const char *charset)
        {
            if (value == NULL)
                return NULL;

            String *res = new String();
            if (res == NULL)
                return NULL;
            else if (res->create(value, charset) == STATUS_OK)
                return res;

            delete res;
            return NULL;
        }

        String String::build()
        {
            String res;
            res.create();
            return res;
        }

        String String::build(const LSPString *value)
        {
            String res;
            res.create(value);
            return res;
        }

        String String::build(const char *value, const char *charset)
        {
            String res;
            res.create(value, charset);
            return res;
        }

        status_t String::get(LSPString *dst) const
        {
            if (dst == NULL)
                return STATUS_BAD_ARGUMENTS;
            if (pNode == NULL)
                return STATUS_OK;

            switch (pNode->type)
            {
                case JN_INT:
                    return (dst->fmt_ascii("%lld", (long long)pNode->nValue) > 0) ? STATUS_OK : STATUS_NO_MEM;
                case JN_DOUBLE:
                {
                    SET_LOCALE_SCOPED(LC_NUMERIC, "C");
                    return (dst->fmt_ascii("%f", (long long)pNode->fValue) > 0) ? STATUS_OK : STATUS_NO_MEM;
                }
                case JN_BOOL:
                    return (dst->set_ascii((pNode->bValue) ? "true" : "false")) ? STATUS_OK : STATUS_NO_MEM;
                case JN_STRING:
                    return (dst->set(pNode->sValue)) ? STATUS_OK : STATUS_NO_MEM;
                default:
                    break;
            }

            return STATUS_BAD_TYPE;
        }

        status_t String::sget(LSPString *dst) const
        {
            if (dst == NULL)
                return STATUS_BAD_ARGUMENTS;
            if (!is_string())
                return STATUS_BAD_TYPE;

            return (dst->set(pNode->sValue)) ? STATUS_OK : STATUS_NO_MEM;
        }

        status_t String::cast()
        {
            return cast_to_string();
        }

        status_t String::set(const LSPString *value)
        {
            if (value == NULL)
                return STATUS_BAD_ARGUMENTS;

            LSPString *tmp  = value->clone();
            if (tmp == NULL)
                return STATUS_NO_MEM;

            node_t *node = clear_node(pNode);
            if (node == NULL)
            {
                delete tmp;
                return STATUS_NO_MEM;
            }

            pNode           = node;
            pNode->type     = JN_STRING;
            pNode->sValue   = tmp;

            return STATUS_OK;
        }

        status_t String::set(const char *value, const char *charset)
        {
            if (value == NULL)
                return STATUS_BAD_ARGUMENTS;

            LSPString *tmp  = new LSPString();
            if (tmp == NULL)
                return STATUS_NO_MEM;
            if (!tmp->set_native(value, charset))
            {
                delete tmp;
                return STATUS_NO_MEM;
            }

            node_t *node = clear_node(pNode);
            if (node == NULL)
            {
                delete tmp;
                return STATUS_NO_MEM;
            }

            pNode           = node;
            pNode->type     = JN_STRING;
            pNode->sValue   = tmp;

            return STATUS_OK;
        }

        status_t String::sset(const LSPString *value)
        {
            if (value == NULL)
                return STATUS_BAD_ARGUMENTS;
            if (!is_string())
                return STATUS_BAD_TYPE;

            return (pNode->sValue->set(value)) ? STATUS_OK : STATUS_NO_MEM;
        }

        status_t String::sset(const char *value, const char *charset)
        {
            if (value == NULL)
                return STATUS_BAD_ARGUMENTS;
            if (!is_string())
                return STATUS_BAD_TYPE;

            return (pNode->sValue->set_native(value, charset)) ? STATUS_OK : STATUS_NO_MEM;
        }
    } /* namespace json */
} /* namespace lsp */




