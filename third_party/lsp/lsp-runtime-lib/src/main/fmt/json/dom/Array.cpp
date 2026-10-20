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

#include <lsp-plug.in/fmt/json/dom/Array.h>
#include <lsp-plug.in/io/InStringSequence.h>
#include <lsp-plug.in/expr/Tokenizer.h>
#include <lsp-plug.in/expr/types.h>

namespace lsp
{
    namespace json
    {
        bool Array::valid() const
        {
            return is_array();
        }

        status_t Array::create()
        {
            node_t *node    = new node_t();
            if (node == NULL)
                return STATUS_NO_MEM;

            node->refs      = 1;
            node->type      = JN_ARRAY;
            node->pArray    = new lltl::parray<node_t>();
            if (node->pArray == NULL)
            {
                delete node;
                return STATUS_NO_MEM;
            }

            release_ref(pNode);
            pNode           = node;

            return STATUS_OK;
        }

        Array *Array::allocate()
        {
            Array *res = new Array();
            if (res == NULL)
                return NULL;
            else if (res->create() == STATUS_OK)
                return res;

            delete res;
            return NULL;
        }

        Array Array::build()
        {
            Array res;
            res.create();
            return res;
        }

        size_t Array::size() const
        {
            return (is_array()) ? pNode->pArray->size() : 0;
        }

        size_t Array::capacity() const
        {
            return (is_array()) ? pNode->pArray->capacity() : 0;
        }

        Node Array::get(size_t index)
        {
            if (!is_array())
                return Node();

            node_t *node = pNode->pArray->get(index);
            return Node(node);
        }

        status_t Array::add(const Node *node)
        {
            if (!is_array())
                return STATUS_BAD_TYPE;

            Node null;
            if (node == NULL)
                node = &null;
            node_t *ref = node->make_ref();
            if (ref == NULL)
                return STATUS_NO_MEM;
            if (pNode->pArray->add(ref))
                return STATUS_OK;

            release_ref(ref);
            return STATUS_NO_MEM;
        }

        status_t Array::add(const Node &node)
        {
            return add(&node);
        }

        status_t Array::append(const Node *node)
        {
            if (!is_array())
                return STATUS_BAD_TYPE;

            Node null;
            if (node == NULL)
                node = &null;
            node_t *ref = node->make_ref();
            if (ref == NULL)
                return STATUS_NO_MEM;
            if (pNode->pArray->append(ref))
                return STATUS_OK;

            release_ref(ref);
            return STATUS_NO_MEM;
        }

        status_t Array::append(const Node &node)
        {
            return append(&node);
        }

        status_t Array::prepend(const Node *node)
        {
            if (!is_array())
                return STATUS_BAD_TYPE;

            Node null;
            if (node == NULL)
                node = &null;
            node_t *ref = node->make_ref();
            if (ref == NULL)
                return STATUS_NO_MEM;
            if (pNode->pArray->prepend(ref))
                return STATUS_OK;

            release_ref(ref);
            return STATUS_NO_MEM;
        }

        status_t Array::prepend(const Node &node)
        {
            return prepend(&node);
        }

        status_t Array::insert(size_t index, const Node *node)
        {
            if (!is_array())
                return STATUS_BAD_TYPE;

            Node null;
            if (node == NULL)
                node = &null;
            node_t *ref = node->make_ref();
            if (ref == NULL)
                return STATUS_NO_MEM;
            if (pNode->pArray->insert(index, ref))
                return STATUS_OK;

            release_ref(ref);
            return STATUS_NO_MEM;
        }

        status_t Array::insert(size_t index, const Node &node)
        {
            return insert(index, &node);
        }

        status_t Array::remove(size_t index)
        {
            if (!is_array())
                return STATUS_BAD_TYPE;

            node_t *ref = pNode->pArray->remove(index);
            if (ref == NULL)
                return STATUS_NOT_FOUND;

            release_ref(ref);
            return STATUS_OK;
        }

        status_t Array::remove_n(size_t index, size_t count)
        {
            if (!is_array())
                return STATUS_BAD_TYPE;

            lltl::parray<node_t> rm;
            if (!pNode->pArray->remove_n(index, count, &rm))
                return STATUS_NO_MEM;

            for (size_t i=0, n=rm.size(); i<n; ++i)
            {
                node_t *ref = rm.uget(i);
                release_ref(ref);
            }

            return STATUS_OK;
        }
    }
}


