/*
 * Copyright (C) 2021 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2021 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 14 февр. 2021 г.
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

#include <lsp-plug.in/fmt/json/dom/Object.h>
#include <lsp-plug.in/io/InStringSequence.h>
#include <lsp-plug.in/expr/Tokenizer.h>
#include <lsp-plug.in/expr/types.h>

namespace lsp
{
    namespace json
    {
        bool Object::valid() const
        {
            return is_object();
        }

        status_t Object::create()
        {
            node_t *node    = new node_t();
            if (node == NULL)
                return STATUS_NO_MEM;

            node->refs      = 1;
            node->type      = JN_OBJECT;
            node->pObject   = new lltl::pphash<LSPString, node_t>();
            if (node->pObject == NULL)
            {
                delete node;
                return STATUS_NO_MEM;
            }

            release_ref(pNode);
            pNode           = node;

            return STATUS_OK;
        }

        Object *Object::allocate()
        {
            Object *res = new Object();
            if (res == NULL)
                return NULL;
            else if (res->create() == STATUS_OK)
                return res;

            delete res;
            return NULL;
        }

        Object Object::build()
        {
            Object res;
            res.create();
            return res;
        }

        size_t Object::size() const
        {
            return (is_object()) ? pNode->pObject->size() : 0;
        }

        size_t Object::capacity() const
        {
            return (is_object()) ? pNode->pObject->capacity() : 0;
        }

        Node Object::get(const char *field)
        {
            LSPString tmp;
            if (!tmp.set_utf8(field))
                return Node();
            return get(&tmp);
        }

        Node Object::get(const LSPString *field)
        {
            if (!is_object())
                return Node();

            node_t *ref = pNode->pObject->get(field);
            return Node(ref);
        }

        bool Object::contains(const char *field) const
        {
            LSPString tmp;
            if (!tmp.set_utf8(field))
                return false;
            return contains(&tmp);
        }

        bool Object::contains(const LSPString *field) const
        {
            return (is_object()) ? pNode->pObject->contains(field) : false;
        }

        status_t Object::remove(const char *field)
        {
            LSPString tmp;
            if (!tmp.set_utf8(field))
                return STATUS_NO_MEM;
            return remove(&tmp);
        }

        status_t Object::remove(const LSPString *field)
        {
            if (!is_object())
                return STATUS_NOT_FOUND;

            node_t *ref = NULL;
            if (!pNode->pObject->remove(field, &ref))
                return STATUS_NOT_FOUND;

            release_ref(ref);
            return STATUS_OK;
        }

        status_t Object::set(const char *field, const Node *node)
        {
            LSPString tmp;
            if (!tmp.set_utf8(field))
                return STATUS_NO_MEM;
            return set(&tmp, node);
        }

        status_t Object::set(const LSPString *field, const Node *node)
        {
            if (!is_object())
                return STATUS_NOT_FOUND;

            Node null;
            if (node == NULL)
                node = &null;

            node_t *nref = node->make_ref();
            if (nref == NULL)
                return STATUS_NO_MEM;

            node_t *oref = NULL;
            if (!pNode->pObject->put(field, nref, &oref))
            {
                release_ref(nref);
                return STATUS_NO_MEM;
            }

            release_ref(oref);
            return STATUS_OK;
        }

        status_t Object::set(const char *field, const Node &node)
        {
            return set(field, &node);
        }

        status_t Object::set(const LSPString *field, const Node &node)
        {
            return set(field, &node);
        }

        status_t Object::fields(lltl::parray<LSPString> *list)
        {
            if (!is_object())
                return STATUS_BAD_TYPE;
            if (!pNode->pObject->keys(list))
                return STATUS_NO_MEM;
            return STATUS_OK;
        }

    }
}


