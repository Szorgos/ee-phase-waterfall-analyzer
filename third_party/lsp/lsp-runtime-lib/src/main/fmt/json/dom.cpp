/*
 * Copyright (C) 2024 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2024 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 15 февр. 2021 г.
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

#include <lsp-plug.in/fmt/json/dom.h>
#include <lsp-plug.in/fmt/json/Parser.h>
#include <lsp-plug.in/fmt/json/Serializer.h>

namespace lsp
{
    namespace json
    {
        //---------------------------------------------------------------------
        // JSON DOM deserialization routines
        status_t dom_drop_stack(lltl::parray<Node> *stack, status_t res)
        {
            for (size_t i=0, n=stack->size(); i<n; ++i)
            {
                Node *node = stack->uget(i);
                if (node != NULL)
                    delete node;
            }
            stack->flush();

            return res;
        }

        status_t dom_parse_item(Node **dst, lltl::parray<Node> *stack, const event_t *ev)
        {
            Node *out = NULL;

            switch (ev->type)
            {
                case JE_OBJECT_START:
                    out     = json::Object::allocate();
                    break;
                case JE_ARRAY_START:
                    out     = json::Array::allocate();
                    break;
                case JE_STRING:
                    out     = json::String::allocate(&ev->sValue);
                    break;
                case JE_INTEGER:
                    out     = json::Integer::allocate(ev->iValue);
                    break;
                case JE_DOUBLE:
                    out     = json::Double::allocate(ev->fValue);
                    break;
                case JE_BOOL:
                    out     = json::Boolean::allocate(ev->bValue);
                    break;
                case JE_NULL:
                    out     = json::Node::allocate();
                    break;
                default:
                    return STATUS_BAD_FORMAT;
            }

            // Check result
            if (out == NULL)
                return STATUS_NO_MEM;
            *dst = out;

            return STATUS_OK;
        }

        status_t dom_parse(Parser *p, Node *node, bool strict)
        {
            Node out, *xnode = NULL;
            event_t ev;
            status_t res;
            lltl::parray<Node> stack;

            // Initialize tree
            do
            {
                Node *last = stack.last();
                Node::node_type_t type = (last != NULL) ? last->type() : Node::JN_NULL;

                // Read next event
                if ((res = p->read_next(&ev)) != STATUS_OK)
                    return dom_drop_stack(&stack, res);

                switch (type)
                {
                    case Node::JN_ARRAY:
                    {
                        // Pre-check tokens
                        if (ev.type == JE_ARRAY_END)
                        {
                            stack.pop();
                            if (stack.is_empty())
                                out.assign(last);
                            delete last;
                            break;
                        }
                        else if ((ev.type == JE_OBJECT_END) || (ev.type == JE_PROPERTY))
                            return dom_drop_stack(&stack, STATUS_BAD_FORMAT);

                        Array ao(last);

                        // Parse simple item from event
                        if ((res = dom_parse_item(&xnode, &stack, &ev)) != STATUS_OK)
                            return dom_drop_stack(&stack, res);

                        if ((res = ao.add(xnode)) != STATUS_OK)
                        {
                            delete xnode;
                            return dom_drop_stack(&stack, res);
                        }

                        break;
                    }
                    case Node::JN_OBJECT:
                    {
                        // Pre-check tokens
                        if (ev.type == JE_OBJECT_END)
                        {
                            stack.pop();
                            if (stack.is_empty())
                                out.assign(last);
                            delete last;
                            break;
                        }
                        else if (ev.type != JE_PROPERTY)
                            return dom_drop_stack(&stack, STATUS_BAD_FORMAT);

                        // Validate and remember the property name
                        Object jo(last);
                        if (jo.contains(&ev.sValue)) // Check that object already contains such key
                            return dom_drop_stack(&stack, STATUS_BAD_FORMAT);
                        LSPString key;
                        if (!key.set(&ev.sValue))
                            return dom_drop_stack(&stack, STATUS_NO_MEM);

                        // Read next event
                        if ((res = p->read_next(&ev)) != STATUS_OK)
                            return dom_drop_stack(&stack, res);

                        // Parse simple item from event
                        if ((res = dom_parse_item(&xnode, &stack, &ev)) != STATUS_OK)
                            return dom_drop_stack(&stack, res);
                        if ((res = jo.set(&key, xnode)) != STATUS_OK)
                        {
                            delete xnode;
                            return dom_drop_stack(&stack, res);
                        }

                        break;
                    }

                    default:
                    {
                        // Parse simple item
                        if ((res = dom_parse_item(&xnode, &stack, &ev)) != STATUS_OK)
                            return dom_drop_stack(&stack, res);

                        break;
                    }
                }

                // Add extra node to the stack if it is present
                if (xnode != NULL)
                {
                    if ((xnode->is_array()) || (xnode->is_object()))
                    {
                        if (!stack.push(xnode))
                        {
                            delete xnode;
                            return dom_drop_stack(&stack, STATUS_NO_MEM);
                        }
                    }
                    else
                        delete xnode;
                    xnode   = NULL;
                }

            } while (stack.size() > 0);

            // Check for EOF
            if (strict)
            {
                if ((res = p->read_next(&ev)) != STATUS_EOF)
                    return dom_drop_stack(&stack, STATUS_BAD_FORMAT);
            }

            // Assign node and return
            node->assign(out);

            return STATUS_OK;
        }

        status_t dom_load(const char *path, Node *node, json_version_t version, const char *charset)
        {
            Parser p;
            Node tmp;
            status_t res = p.open(path, version, charset);
            if (res == STATUS_OK)
                res     = dom_parse(&p, &tmp, true);

            if (res == STATUS_OK)
                res = p.close();
            else
                p.close();

            if (res == STATUS_OK)
                node->assign(tmp);

            return res;
        }

        status_t dom_load(const LSPString *path, Node *node, json_version_t version, const char *charset)
        {
            Parser p;
            Node tmp;
            status_t res = p.open(path, version, charset);
            if (res == STATUS_OK)
                res     = dom_parse(&p, &tmp, true);

            if (res == STATUS_OK)
                res = p.close();
            else
                p.close();

            if (res == STATUS_OK)
                node->assign(tmp);

            return res;
        }

        status_t dom_load(const io::Path *path, Node *node, json_version_t version, const char *charset)
        {
            Parser p;
            Node tmp;
            status_t res = p.open(path, version, charset);
            if (res == STATUS_OK)
                res     = dom_parse(&p, &tmp, true);

            if (res == STATUS_OK)
                res = p.close();
            else
                p.close();

            if (res == STATUS_OK)
                node->assign(tmp);

            return res;
        }

        size_t build_wflags(size_t flags)
        {
            size_t wflags = 0;
            if (flags & DOM_CLOSE)
                wflags     |= WRAP_CLOSE;
            if (flags & DOM_DELETE)
                wflags     |= WRAP_DELETE;
            return wflags;
        }

        status_t dom_parse(io::IInStream *is, Node *node, json_version_t version, size_t flags, const char *charset)
        {
            Parser p;
            Node tmp;

            size_t wflags   = build_wflags(flags);
            status_t res    = p.wrap(is, version, wflags, charset);
            if (res == STATUS_OK)
                res = dom_parse(&p, &tmp, !(flags & DOM_FLEXIBLE));

            if (res == STATUS_OK)
                res = p.close();
            else
                p.close();

            if (res == STATUS_OK)
                node->assign(tmp);

            return res;
        }

        status_t dom_parse(const LSPString *data, Node *node, json_version_t version)
        {
            Parser p;
            Node tmp;

            status_t res    = p.wrap(data, version);
            if (res == STATUS_OK)
                res = dom_parse(&p, &tmp, true);

            if (res == STATUS_OK)
                res = p.close();
            else
                p.close();

            if (res == STATUS_OK)
                node->assign(tmp);

            return res;
        }

        status_t dom_parse(const char *data, Node *node, json_version_t version, size_t flags, const char *charset)
        {
            Parser p;
            Node tmp;

            status_t res    = p.wrap(data, version, charset);
            if (res == STATUS_OK)
                res = dom_parse(&p, &tmp, true);

            if (res == STATUS_OK)
                res = p.close();
            else
                p.close();

            if (res == STATUS_OK)
                node->assign(tmp);

            return res;
        }

        status_t dom_parse(io::IInSequence *is, Node *node, json_version_t version, size_t flags)
        {
            Parser p;
            Node tmp;

            size_t wflags   = build_wflags(flags);
            status_t res    = p.wrap(is, version, wflags);
            if (res == STATUS_OK)
                res = dom_parse(&p, &tmp, !(flags & DOM_FLEXIBLE));

            if (res == STATUS_OK)
                res = p.close();
            else
                p.close();

            if (res == STATUS_OK)
                node->assign(tmp);

            return res;
        }

        //---------------------------------------------------------------------
        // JSON DOM serialization routines
        status_t dom_serialize(Serializer *s, const Node *node)
        {
            status_t res;

            switch (node->type())
            {
                case Node::JN_NULL:     return s->write_null();
                case Node::JN_INT:      return s->write_int(lsp::fixed_int(node->as_int().get()));
                case Node::JN_DOUBLE:   return s->write_double(node->as_double().get());
                case Node::JN_BOOL:     return s->write_bool(node->as_bool().get());
                case Node::JN_STRING:
                {
                    LSPString str;
                    if ((res = node->as_string().get(&str)) != STATUS_OK)
                        return res;
                    return s->write_string(&str);
                }

                case Node::JN_ARRAY:
                {
                    // Start the array
                    if ((res = s->start_array()) != STATUS_OK)
                        return res;

                    // Serialize the array
                    Array av = node;
                    for (size_t i=0, n=av.size(); i<n; ++i)
                    {
                        Node an = av.get(i);
                        if ((res = dom_serialize(s, &an)) != STATUS_OK)
                            return res;
                    }

                    // Complete the array
                    return s->end_array();
                }

                case Node::JN_OBJECT:
                {
                    // Start the object
                    if ((res = s->start_object()) != STATUS_OK)
                        return res;

                    lltl::parray<LSPString> fields;

                    // Serialize the object
                    Object ov = node;
                    if ((res = ov.fields(&fields)) != STATUS_OK)
                        return res;

                    // Sort list of fields
                    fields.qsort();

                    for (size_t i=0, n=fields.size(); i<n; ++i)
                    {
                        const LSPString *field = fields.uget(i);
                        if (field == NULL)
                            return STATUS_CORRUPTED;

                        // Property name
                        if ((res = s->write_property(field)) != STATUS_OK)
                            return res;

                        // Property value
                        Node on = ov.get(field);
                        if ((res = dom_serialize(s, &on)) != STATUS_OK)
                            return res;
                    }

                    // Complete the object
                    return s->end_object();
                }

                default:
                    break;
            }

            return STATUS_CORRUPTED;
        }

        status_t dom_save(const char *path, const Node *node, const serial_flags_t *settings, const char *charset)
        {
            Serializer s;

            status_t res    = s.open(path, settings, charset);
            if (res == STATUS_OK)
                res = dom_serialize(&s, node);

            status_t xres   = s.close();
            return (res != STATUS_OK) ? res : xres;
        }

        status_t dom_save(const LSPString *path, const Node *node, const serial_flags_t *settings, const char *charset)
        {
            Serializer s;

            status_t res    = s.open(path, settings, charset);
            if (res == STATUS_OK)
                res = dom_serialize(&s, node);

            status_t xres   = s.close();
            return (res != STATUS_OK) ? res : xres;
        }

        status_t dom_save(const io::Path *path, const Node *node, const serial_flags_t *settings, const char *charset)
        {
            Serializer s;

            status_t res    = s.open(path, settings, charset);
            if (res == STATUS_OK)
                res = dom_serialize(&s, node);

            status_t xres   = s.close();
            return (res != STATUS_OK) ? res : xres;
        }

        status_t dom_write(io::IOutStream *os, const Node *node, const serial_flags_t *settings, size_t flags, const char *charset)
        {
            Serializer s;

            status_t res    = s.wrap(os, settings, flags, charset);
            if (res == STATUS_OK)
                res = dom_serialize(&s, node);

            status_t xres   = s.close();
            return (res != STATUS_OK) ? res : xres;
        }

        status_t dom_write(LSPString *data, const Node *node, const serial_flags_t *settings)
        {
            Serializer s;

            status_t res    = s.wrap(data, settings);
            if (res == STATUS_OK)
                res = dom_serialize(&s, node);

            status_t xres   = s.close();
            return (res != STATUS_OK) ? res : xres;
        }

        status_t dom_write(io::IOutSequence *os, const Node *node, const serial_flags_t *settings, size_t flags)
        {
            Serializer s;

            status_t res    = s.wrap(os, settings, flags);
            if (res == STATUS_OK)
                res = dom_serialize(&s, node);

            status_t xres   = s.close();
            return (res != STATUS_OK) ? res : xres;
        }

    } /* namespace json */
} /* namespace lsp */

