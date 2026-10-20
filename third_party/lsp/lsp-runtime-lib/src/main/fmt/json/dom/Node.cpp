/*
 * Copyright (C) 2021 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2021 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 31 янв. 2021 г.
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

#include <lsp-plug.in/fmt/json/dom/Node.h>
#include <lsp-plug.in/io/InStringSequence.h>
#include <lsp-plug.in/expr/Tokenizer.h>
#include <lsp-plug.in/stdlib/locale.h>

namespace lsp
{
    namespace json
    {
        Node::Node(node_t *node)
        {
            pNode       = node;
            if (node != NULL)
                node->refs++;
        }

        Node::Node(const Node &src)
        {
            pNode       = NULL;
            copy_ref(&src);
        }

        Node::Node(const Node *src)
        {
            pNode       = NULL;
            copy_ref(src);
        }

        Node::~Node()
        {
            if (pNode != NULL)
            {
                release_ref(pNode);
                pNode = NULL;
            }
        }

        status_t Node::create()
        {
            node_t *node    = new node_t();
            if (node == NULL)
                return STATUS_NO_MEM;

            node->refs      = 1;
            node->type      = JN_NULL;
            node->pData     = NULL;

            release_ref(pNode);
            pNode           = node;

            return STATUS_OK;
        }

        Node Node::build()
        {
            Node res;
            res.create();
            return res;
        }

        Node *Node::allocate()
        {
            Node *res = new Node();
            if (res == NULL)
                return NULL;
            else if (res->create() == STATUS_OK)
                return res;

            delete res;
            return NULL;
        }

        void Node::copy_ref(const Node *src)
        {
            if (this == src)
                return;
            else if (this->pNode == src->pNode)
                return;

            // Increment number of references for source node
            node_t *ref = src->make_ref();

            // Release self reference and replace with new one
            release_ref(pNode);
            pNode       = ref;
        }

        Node::node_t *Node::make_ref() const
        {
            if (pNode != NULL)
            {
                pNode->refs ++;
                return pNode;
            }

            // Need to allocate the node
            if ((pNode = new node_t()) == NULL)
                return NULL;

            pNode->refs = 2; // self and exported
            pNode->type = JN_NULL;
            pNode->pData= NULL;

            return pNode;
        }

        void Node::undef_node(node_t *node)
        {
            if (node == NULL)
                return;

            // Destroy object
            switch (node->type)
            {
                case JN_NULL:
                    break;

                case JN_INT:
                    node->nValue    = 0;
                    break;

                case JN_DOUBLE:
                    node->fValue    = 0.0;
                    break;

                case JN_BOOL:
                    node->bValue    = false;
                    break;

                case JN_STRING:
                {
                    LSPString *s = node->sValue;
                    if (s == NULL)
                        break;

                    node->sValue = NULL;
                    delete s;
                    break;
                }

                case JN_ARRAY:
                {
                    lltl::parray<node_t> *v = node->pArray;
                    if (v == NULL)
                        break;

                    node->pArray = NULL;
                    for (size_t i=0, n=v->size(); i<n; ++i)
                        release_ref(v->uget(i));
                    delete v;

                    break;
                }

                case JN_OBJECT:
                {
                    lltl::pphash<LSPString, node_t> *h = node->pObject;
                    if (h == NULL)
                        break;

                    node->pObject = NULL;
                    lltl::parray<node_t> v;
                    if (h->values(&v))
                    {
                        for (size_t i=0, n=v.size(); i<n; ++i)
                            release_ref(v.uget(i));
                        v.flush();
                    }
                    delete h;
                    break;
                }
            }

            // Clear type
            node->type  = JN_NULL;
        }

        Node::node_t *Node::clear_node(node_t *node)
        {
            if (node != NULL)
            {
                undef_node(node);
                return node;
            }

            node = new node_t;
            if (node != NULL)
            {
                node->type  = JN_NULL;
                node->refs  = 1;
                node->pData = NULL;
            }
            return node;
        }

        void Node::release_ref(node_t *node)
        {
            // Check the value
            if ((node == NULL) || (--node->refs) > 0)
                return;

            // Undef node data and free node
            undef_node(node);
            delete node;
        }

        bool Node::of_type(const Node *src) const
        {
            return type() == src->type();
        }

        bool Node::of_type(const Node &src) const
        {
            return type() == src.type();
        }

        bool Node::of_type(node_type_t ntype) const
        {
            return type() == ntype;
        }

        void Node::move_data(node_t *dst, node_t *src)
        {
            switch (src->type)
            {
                case JN_NULL:   dst->pData   = src->pData;   break;
                case JN_INT:    dst->nValue  = src->nValue;  break;
                case JN_DOUBLE: dst->fValue  = src->fValue;  break;
                case JN_BOOL:   dst->bValue  = src->bValue;  break;
                case JN_STRING: dst->sValue  = src->sValue;  break;
                case JN_ARRAY:  dst->pArray  = src->pArray;  break;
                case JN_OBJECT: dst->pObject = src->pObject; break;
                default: break;
            }
            dst->type   = src->type;

            src->pData      = NULL;
            src->type       = JN_NULL;
        }

        status_t Node::cast_to(node_type_t dtype)
        {
            // Check that type matches
            switch (dtype)
            {
                case JN_NULL:
                    undef_node(pNode);
                    return STATUS_OK;

                case JN_INT:    return cast_to_int();
                case JN_DOUBLE: return cast_to_double();
                case JN_BOOL:   return cast_to_bool();
                case JN_STRING: return cast_to_string();
                case JN_ARRAY:  return cast_to_array();
                case JN_OBJECT: return cast_to_object();

                default:
                    break;
            }

            return STATUS_BAD_TYPE;
        }

        status_t Node::cast_to_int()
        {
            ssize_t ivalue;

            switch (type())
            {
                case JN_NULL:   ivalue = 0; break;
                case JN_INT:    return STATUS_OK;
                case JN_DOUBLE: ivalue = pNode->fValue;  break;
                case JN_BOOL:   ivalue = (pNode->bValue) ? 1 : 0; break;
                case JN_STRING:
                {
                    // Parse integer/float number as string and cast to integer
                    io::InStringSequence s(pNode->sValue);
                    expr::Tokenizer t(&s);

                    switch (t.get_token(expr::TF_GET))
                    {
                        case expr::TT_IVALUE: ivalue = t.int_value(); break;
                        case expr::TT_FVALUE: ivalue = t.float_value(); break;
                        case expr::TT_TRUE:   ivalue = 1; break;
                        case expr::TT_FALSE:  ivalue = 0; break;
                        default:
                            return STATUS_BAD_FORMAT;
                    }

                    if (t.get_token(expr::TF_GET) != expr::TT_EOF)
                        return STATUS_BAD_FORMAT;
                    break;
                }

                case JN_ARRAY: ivalue = pNode->pArray->size(); break;
                case JN_OBJECT: ivalue = pNode->pObject->size(); break;

                default:
                    return STATUS_BAD_TYPE;
            }

            // Update node value
            node_t *node    = clear_node(pNode);
            if (node == NULL)
                return STATUS_NO_MEM;

            pNode           = node;
            pNode->type     = JN_INT;
            pNode->nValue   = ivalue;

            return STATUS_OK;
        }

        status_t Node::cast_to_double()
        {
            double fvalue;

            switch (type())
            {
                case JN_NULL:   fvalue = 0.0f; break;
                case JN_INT:    fvalue = pNode->nValue;  break;
                case JN_DOUBLE: return STATUS_OK;
                case JN_BOOL:   fvalue = (pNode->bValue) ? 1.0f : 0.0f; break;
                case JN_STRING:
                {
                    // Parse integer/float number as string and cast to integer
                    io::InStringSequence s(pNode->sValue);
                    expr::Tokenizer t(&s);

                    switch (t.get_token(expr::TF_GET))
                    {
                        case expr::TT_IVALUE: fvalue = t.int_value(); break;
                        case expr::TT_FVALUE: fvalue = t.float_value(); break;
                        case expr::TT_TRUE:   fvalue = 1.0f; break;
                        case expr::TT_FALSE:  fvalue = 0.0f; break;
                        default:
                            return STATUS_BAD_FORMAT;
                    }

                    if (t.get_token(expr::TF_GET) != expr::TT_EOF)
                        return STATUS_BAD_FORMAT;
                    break;
                }

                case JN_ARRAY: fvalue  = pNode->pArray->size(); break;
                case JN_OBJECT: fvalue = pNode->pObject->size(); break;

                default:
                    return STATUS_BAD_TYPE;
            }

            // Update node value
            node_t *node    = clear_node(pNode);
            if (node == NULL)
                return STATUS_NO_MEM;

            pNode->type     = JN_DOUBLE;
            pNode->fValue   = fvalue;

            return STATUS_OK;
        }

        status_t Node::cast_to_bool()
        {
            bool bvalue;

            switch (type())
            {
                case JN_NULL:   bvalue = false; break;
                case JN_INT:    bvalue = pNode->nValue != 0;  break;
                case JN_DOUBLE: bvalue = (pNode->fValue >= 0.5) || (pNode->fValue <= -0.5); break;
                case JN_BOOL:   return STATUS_OK;
                case JN_STRING:
                {
                    // Parse integer/float number as string and cast to integer
                    io::InStringSequence s(pNode->sValue);
                    expr::Tokenizer t(&s);

                    switch (t.get_token(expr::TF_GET))
                    {
                        case expr::TT_IVALUE: bvalue = t.int_value() != 0; break;
                        case expr::TT_FVALUE: bvalue = (t.float_value() >= 0.5) || (t.float_value() <= -0.5); break;
                        case expr::TT_TRUE:   bvalue = true; break;
                        case expr::TT_FALSE:  bvalue = false; break;
                        default:
                            return STATUS_BAD_FORMAT;
                    }

                    if (t.get_token(expr::TF_GET) != expr::TT_EOF)
                        return STATUS_BAD_FORMAT;
                    break;
                }

                case JN_ARRAY: bvalue  = pNode->pArray->size() > 0; break;
                case JN_OBJECT: bvalue = pNode->pObject->size() > 0; break;

                default:
                    return STATUS_BAD_TYPE;
            }

            // Update node value
            node_t *node    = clear_node(pNode);
            if (node == NULL)
                return STATUS_NO_MEM;

            pNode->type     = JN_BOOL;
            pNode->bValue   = bvalue;

            return STATUS_OK;
        }

        status_t Node::cast_to_string()
        {
            LSPString tmp;

            switch (type())
            {
                case JN_NULL: break;
                case JN_INT:
                    if (!tmp.fmt_ascii("%ld", long(pNode->nValue)))
                        return STATUS_NO_MEM;
                    break;
                case JN_DOUBLE:
                {
                    SET_LOCALE_SCOPED(LC_NUMERIC, "C");
                    if (!tmp.fmt_ascii("%f", double(pNode->fValue)))
                        return STATUS_NO_MEM;
                    break;
                }
                case JN_BOOL:
                    if (!tmp.set_ascii((pNode->bValue) ? "true" : "false"))
                        return STATUS_NO_MEM;
                    break;
                case JN_STRING: return STATUS_OK;

                case JN_ARRAY: return STATUS_BAD_TYPE;  // TODO
                case JN_OBJECT: return STATUS_BAD_TYPE; // TODO

                default:
                    return STATUS_BAD_TYPE;
            }

            LSPString *ns = tmp.release();
            if (ns == NULL)
                return STATUS_NO_MEM;

            // Update node value
            node_t *node    = clear_node(pNode);
            if (node == NULL)
                return STATUS_NO_MEM;

            pNode->type     = JN_STRING;
            pNode->sValue   = ns;

            return STATUS_OK;
        }

        status_t Node::cast_to_array()
        {
            lltl::parray<node_t> *tmp;
            node_t *child;

            switch (type())
            {
                case JN_ARRAY: return STATUS_OK;
                default:
                    break;
            }

            // Create array and add new node to array
            child = new node_t;
            if (child == NULL)
                return STATUS_NO_MEM;

            tmp = new lltl::parray<node_t>();
            if (tmp == NULL)
            {
                delete child;
                return STATUS_NO_MEM;
            }
            if (!tmp->add(child))
            {
                delete child;
                delete tmp;
                return STATUS_NO_MEM;
            }

            // Transfer data
            if (pNode != NULL)
                move_data(child, pNode);
            else
            {
                // Update node value
                node_t *node    = clear_node(pNode);
                if (node == NULL)
                {
                    delete child;
                    delete tmp;
                    return STATUS_NO_MEM;
                }
                pNode           = node;
            }

            // Update node value
            pNode->type     = JN_ARRAY;
            pNode->pArray   = tmp;

            return STATUS_OK;
        }

        status_t Node::cast_to_object()
        {
            LSPString key;
            lltl::pphash<LSPString, node_t> *tmp;
            node_t *child;

            switch (type())
            {
                case JN_ARRAY: return STATUS_OK;
                default:
                    break;
            }

            // Create array and add new node to array
            child = new node_t;
            if (child == NULL)
                return STATUS_NO_MEM;

            tmp = new lltl::pphash<LSPString, node_t>();
            if (tmp == NULL)
            {
                delete child;
                return STATUS_NO_MEM;
            }
            if (!tmp->create(&key, child))
            {
                delete child;
                delete tmp;
                return STATUS_NO_MEM;
            }

            // Transfer data
            if (pNode != NULL)
                move_data(child, pNode);
            else
            {
                // Update node value
                node_t *node    = clear_node(pNode);
                if (node == NULL)
                {
                    delete child;
                    delete tmp;
                    return STATUS_NO_MEM;
                }
                pNode           = node;
            }

            // Update node value
            pNode->type     = JN_OBJECT;
            pNode->pObject  = tmp;

            return STATUS_OK;
        }

        Integer Node::as_int() const
        {
            return Integer(this);
        }

        Double Node::as_double() const
        {
            return Double(this);
        }

        Boolean Node::as_bool() const
        {
            return Boolean(this);
        }

        String Node::as_string() const
        {
            return String(this);
        }

        Array Node::as_array() const
        {
            return Array(this);
        }

        Object Node::as_object() const
        {
            return Object(this);
        }

        const char *Node::stype() const
        {
            size_t type = (pNode == NULL) ? JN_NULL : pNode->type;
            switch (type)
            {
                case JN_NULL:   return "NULL";
                case JN_INT:    return "INT";
                case JN_DOUBLE: return "DOUBLE";
                case JN_BOOL:   return "BOOL";
                case JN_STRING: return "STRING";
                case JN_ARRAY:  return "ARRAY";
                case JN_OBJECT: return "OBJECT";
                default: break;
            }

            return "UNKNOWN (corrupted)";
        }

    } /* namespace json */
} /* namespace lsp */


