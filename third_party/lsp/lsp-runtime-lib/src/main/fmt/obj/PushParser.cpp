/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 22 апр. 2020 г.
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

#include <lsp-plug.in/fmt/obj/PushParser.h>

namespace lsp
{
    namespace obj
    {
        
        PushParser::PushParser()
        {
        }
        
        PushParser::~PushParser()
        {
        }
    
        status_t PushParser::parse_file(IObjHandler *handler, const char *path, const char *charset)
        {
            IObjHandler stub;
            status_t res = sParser.open(path, charset);
            if (res == STATUS_OK)
                res = parse_document((handler != NULL) ? handler : &stub);
            if (res == STATUS_OK)
                res = sParser.close();
            else
                sParser.close();
            return res;
        }

        status_t PushParser::parse_file(IObjHandler *handler, const LSPString *path, const char *charset)
        {
            IObjHandler stub;
            status_t res = sParser.open(path, charset);
            if (res == STATUS_OK)
                res = parse_document((handler != NULL) ? handler : &stub);
            if (res == STATUS_OK)
                res = sParser.close();
            else
                sParser.close();
            return res;
        }

        status_t PushParser::parse_file(IObjHandler *handler, const io::Path *path, const char *charset)
        {
            IObjHandler stub;
            status_t res = sParser.open(path, charset);
            if (res == STATUS_OK)
                res = parse_document((handler != NULL) ? handler : &stub);
            if (res == STATUS_OK)
                res = sParser.close();
            else
                sParser.close();
            return res;
        }

        status_t PushParser::parse_data(IObjHandler *handler, io::IInStream *is, size_t flags, const char *charset)
        {
            IObjHandler stub;
            status_t res = sParser.wrap(is, flags, charset);
            if (res == STATUS_OK)
                res = parse_document((handler != NULL) ? handler : &stub);
            if (res == STATUS_OK)
                res = sParser.close();
            else
                sParser.close();
            return res;
        }

        status_t PushParser::parse_data(IObjHandler *handler, const char *str, const char *charset)
        {
            IObjHandler stub;
            status_t res = sParser.wrap(str, charset);
            if (res == STATUS_OK)
                res = parse_document((handler != NULL) ? handler : &stub);
            if (res == STATUS_OK)
                res = sParser.close();
            else
                sParser.close();
            return res;
        }

        status_t PushParser::parse_data(IObjHandler *handler, const LSPString *str)
        {
            IObjHandler stub;
            status_t res = sParser.wrap(str);
            if (res == STATUS_OK)
                res = parse_document((handler != NULL) ? handler : &stub);
            if (res == STATUS_OK)
                res = sParser.close();
            else
                sParser.close();
            return res;
        }

        status_t PushParser::parse_data(IObjHandler *handler, io::IInSequence *seq, size_t flags)
        {
            IObjHandler stub;
            status_t res = sParser.wrap(seq, flags);
            if (res == STATUS_OK)
                res = parse_document((handler != NULL) ? handler : &stub);
            if (res == STATUS_OK)
                res = sParser.close();
            else
                sParser.close();
            return res;
        }

        status_t PushParser::parse_document(IObjHandler *handler)
        {
            ssize_t object_id;
            status_t res = STATUS_OK;
            size_t nobj = 0;

            while ((res = sParser.next()) == STATUS_OK)
            {
                // Fetch event
                const event_t *ev = sParser.current();
                if (ev == NULL)
                    return STATUS_BAD_STATE;
                
                // Analyze event
                switch (ev->type)
                {
                    case EV_OBJECT:
                        if (nobj++)
                        {
                            if ((res = handler->end_object()) != STATUS_OK)
                                return res;
                        }
                        if ((res = handler->begin_object(&ev->name)) != STATUS_OK)
                            return res;
                        break;

                    case EV_VERTEX:
                        if ((object_id = handler->add_vertex(ev->vertex.x, ev->vertex.y, ev->vertex.z, ev->vertex.w)) < 0)
                            return status_t(-object_id);
                        break;

                    case EV_PVERTEX:
                        if ((object_id = handler->add_param_vertex(ev->vertex.x, ev->vertex.y, ev->vertex.z, ev->vertex.w)) < 0)
                            return status_t(-object_id);
                        break;

                    case EV_NORMAL:
                        if ((object_id = handler->add_normal(ev->normal.dx, ev->normal.dy, ev->normal.dz, ev->normal.dw)) < 0)
                            return status_t(-object_id);
                        break;

                    case EV_TEXCOORD:
                        if ((object_id = handler->add_texture_vertex(ev->texcoord.u, ev->texcoord.v, ev->texcoord.w)) < 0)
                            return status_t(-object_id);
                        break;

                    case EV_FACE:
                        if ((object_id = handler->add_face(ev->ivertex.array(), ev->inormal.array(), ev->itexcoord.array(), ev->ivertex.size())) < 0)
                            return status_t(-object_id);
                        break;

                    case EV_LINE:
                        if ((object_id = handler->add_line(ev->ivertex.array(), ev->itexcoord.array(), ev->ivertex.size())) < 0)
                            return status_t(-object_id);
                        break;

                    case EV_POINT:
                        if ((object_id = handler->add_points(ev->ivertex.array(), ev->ivertex.size())) < 0)
                            return status_t(-object_id);
                        break;

                    default:
                        return STATUS_UNSUPPORTED_FORMAT;
                }
            }

            if (res == STATUS_EOF)
                res = STATUS_OK;

            // Post-process data
            if (res == STATUS_OK)
            {
                if (nobj)
                {
                    if ((res = handler->end_object()) != STATUS_OK)
                        return res;
                }

                res = handler->end_of_data();
            }

            return res;
        }
    } /* namespace obj */
} /* namespace lsp */
