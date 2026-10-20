/*
 * Copyright (C) 2023 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2023 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 23 февр. 2023 г.
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

#include <lsp-plug.in/common/debug.h>
#include <lsp-plug.in/fmt/sfz/DocumentProcessor.h>

namespace lsp
{
    namespace sfz
    {
        DocumentProcessor::DocumentProcessor()
        {
            nScopes     = -1;
            pScope      = NULL;

            for (size_t i=0; i<SC_TOTAL; ++i)
            {
                scope_data_t *s = &vScopes[i];
                s->enType       = SC_NONE;
                s->enOther      = OT_CUSTOM;
                s->sName        = NULL;
                s->pPrev        = NULL;
            }
        }

        DocumentProcessor::~DocumentProcessor()
        {
            close();
        }

        status_t DocumentProcessor::open(const char *path)
        {
            if (nScopes >= 0)
                return STATUS_OPENED;

            io::Path tmp;
            status_t res = tmp.set(path);
            if (res == STATUS_OK)
                res = open(&tmp);

            return res;
        }

        status_t DocumentProcessor::open(const LSPString *path)
        {
            if (nScopes >= 0)
                return STATUS_OPENED;

            io::Path tmp;
            status_t res = tmp.set(path);
            if (res == STATUS_OK)
                res = open(&tmp);

            return res;
        }

        status_t DocumentProcessor::open(const io::Path *path)
        {
            if (nScopes >= 0)
                return STATUS_OPENED;

            // Get last name of the file path
            LSPString last;
            status_t res = path->get_last(&last);
            if (res != STATUS_OK)
                return res;

            // Create and open parser
            PullParser *pp = new PullParser();
            if (pp == NULL)
                return STATUS_NO_MEM;
            lsp_finally {
                if (pp != NULL)
                {
                    pp->close();
                    delete pp;
                }
            };
            if ((res = pp->open(path)) != STATUS_OK)
                return res;

            // Create the document entry
            document_t *doc = create_document();
            if (doc == NULL)
                return STATUS_NO_MEM;
            lsp_finally { destroy_document(doc); };

            // Initialize document
            if ((doc->sPath = last.clone_utf8()) == NULL)
                return STATUS_NO_MEM;
            if (!vTree.add(doc))
                return STATUS_NO_MEM;
            lsp::swap(doc->pParser, pp);
            doc->nWFlags    = WRAP_CLOSE | WRAP_DELETE;
            doc             = NULL;

            // Mark processor open
            nScopes         = 0;

            return res;
        }

        status_t DocumentProcessor::wrap(const char *str)
        {
            if (nScopes >= 0)
                return STATUS_OPENED;
            PullParser *pp = new PullParser();
            if (pp == NULL)
                return STATUS_NO_MEM;
            status_t res = pp->wrap(str);
            if (res != STATUS_OK)
            {
                pp->close();
                delete pp;
                return res;
            }
            res = wrap(pp, WRAP_CLOSE | WRAP_DELETE);
            if (res != STATUS_OK)
            {
                pp->close();
                delete pp;
            }
            return res;
        }

        status_t DocumentProcessor::wrap(const void *buf, size_t len)
        {
            if (nScopes >= 0)
                return STATUS_OPENED;
            PullParser *pp = new PullParser();
            if (pp == NULL)
                return STATUS_NO_MEM;
            status_t res = pp->wrap(buf, len);
            if (res != STATUS_OK)
            {
                pp->close();
                delete pp;
                return res;
            }
            res = wrap(pp, WRAP_CLOSE | WRAP_DELETE);
            if (res != STATUS_OK)
            {
                pp->close();
                delete pp;
            }
            return res;
        }

        status_t DocumentProcessor::wrap(const LSPString *str)
        {
            if (nScopes >= 0)
                return STATUS_OPENED;
            PullParser *pp = new PullParser();
            if (pp == NULL)
                return STATUS_NO_MEM;
            status_t res = pp->wrap(str);
            if (res != STATUS_OK)
            {
                pp->close();
                delete pp;
                return res;
            }
            res = wrap(pp, WRAP_CLOSE | WRAP_DELETE);
            if (res != STATUS_OK)
            {
                pp->close();
                delete pp;
            }
            return res;
        }

        status_t DocumentProcessor::wrap(io::IInStream *is, size_t flags)
        {
            if (nScopes >= 0)
                return STATUS_OPENED;
            PullParser *pp = new PullParser();
            if (pp == NULL)
                return STATUS_NO_MEM;
            status_t res = pp->wrap(is, flags);
            if (res != STATUS_OK)
            {
                pp->close();
                delete pp;
                return res;
            }
            res = wrap(pp, WRAP_CLOSE | WRAP_DELETE);
            if (res != STATUS_OK)
            {
                pp->close();
                delete pp;
            }

            return res;
        }

        status_t DocumentProcessor::wrap(PullParser *parser, size_t flags)
        {
            if (nScopes >= 0)
                return STATUS_OPENED;

            document_t *doc     = create_document();
            if (doc == NULL)
                return STATUS_NO_MEM;
            lsp_finally { destroy_document(doc); };

            // Initialize document
            if (!vTree.add(doc))
                return STATUS_NO_MEM;
            doc->pParser        = parser;
            doc->nWFlags        = flags;
            doc->sPath          = NULL;
            doc                 = NULL;

            // Mark processor open
            nScopes             = 0;

            return STATUS_OK;
        }

        status_t DocumentProcessor::close()
        {
            if (nScopes < 0)
                return STATUS_OK;

            // Destroy documents
            status_t res = STATUS_OK;
            for (size_t i=vTree.size(); i > 0; )
                res = update_status(res, destroy_document(vTree.uget(--i)));
            vTree.flush();

            // Destroy defined variables if present
            drop_hash(&vVars);

            // Cleanup scopes
            for (size_t i=0; i<SC_TOTAL; ++i)
                clear_scope(&vScopes[i]);

            // Reset scope and open flag
            nScopes     = -1;
            pScope      = NULL;

            return res;
        }

        status_t DocumentProcessor::process_header(IDocumentHandler *handler, event_t *ev)
        {
            // Obtain the type and sub-type of the scope
            scope_t scope = SC_OTHER;
            other_t other = OT_CUSTOM;

            if (ev->name.equals_ascii(HDR_REGION))
                scope   = SC_REGION;
            else if (ev->name.equals_ascii(HDR_GROUP))
                scope   = SC_GROUP;
            else if (ev->name.equals_ascii(HDR_MASTER))
                scope   = SC_MASTER;
            else if (ev->name.equals_ascii(HDR_GLOBAL))
                scope   = SC_GLOBAL;
            else if (ev->name.equals_ascii(HDR_CONTROL))
                scope   = SC_CONTROL;
            else
            {
                if (ev->name.equals_ascii(HDR_CURVE))
                    other   = OT_CURVE;
                else if (ev->name.equals_ascii(HDR_EFFECT))
                    other   = OT_EFFECT;
                else if (ev->name.equals_ascii(HDR_SAMPLE))
                    other   = OT_SAMPLE;
                else if (ev->name.equals_ascii(HDR_MIDI))
                    other   = OT_MIDI;
            }

            // Switch to the desired scope
            status_t res = switch_scope(handler, scope);
            if (res != STATUS_OK)
                return res;

            // Complete the scope
            pScope->enOther     = other;
            if ((scope == SC_OTHER) && (other == OT_CUSTOM))
            {
                if ((pScope->sName = ev->name.clone_utf8()) == NULL)
                    return STATUS_NO_MEM;
            }

            return STATUS_OK;
        }

        status_t DocumentProcessor::process_opcode(event_t *ev)
        {
            // We can not do anything without the scope
            if (pScope == NULL)
                return STATUS_CORRUPTED;

            // Process the opcode and the value
            char *opcode        = process_value(&ev->name);
            if (opcode == NULL)
                return STATUS_NO_MEM;
            lsp_finally { free(opcode); };

            char *value         = process_value(&ev->value);
            if (value == NULL)
                return STATUS_NO_MEM;
            if (!pScope->vAllocated.add(value))
            {
                free(value);
                return STATUS_NO_MEM;
            }

            // Put the opcode to the list
            char *old_value     = NULL;
            if (!pScope->vOpcodes.put(opcode, value, &old_value))
                return STATUS_NO_MEM;

            return STATUS_OK;
        }

        status_t DocumentProcessor::process_include(IDocumentHandler *handler, event_t *ev)
        {
            // Obtain the name of the file
            const char *fname = ev->name.get_utf8();
            if (fname == NULL)
                return STATUS_NO_MEM;

            // Ensure that file wasn't previously present
            for (size_t i=0, n=vTree.size(); i<n; ++i)
            {
                document_t *doc = vTree.uget(i);
                const char *path = (doc->sPath != NULL) ? doc->sPath : handler->root_file_name();
                if (path == NULL)
                    continue;
                if (strcmp(fname, path) == 0)
                    return STATUS_OVERFLOW;
            }

            // Now we can allocate new document
            document_t *doc = create_document();
            if (doc == NULL)
                return STATUS_NO_MEM;
            lsp_finally { destroy_document(doc); };

            // Save file name
            if ((doc->sPath = strdup(fname)) == NULL)
                return STATUS_NO_MEM;

            // Allocate parser
            if ((doc->pParser = new PullParser()) == NULL)
                return STATUS_NO_MEM;
            doc->nWFlags    = WRAP_CLOSE | WRAP_DELETE;

            // Call the handler for opening the file
            status_t res = handler->include(doc->pParser, doc->sPath);
            if (res != STATUS_OK)
                return res;

            // Put the document to the tree
            if (!vTree.push(doc))
                return STATUS_NO_MEM;

            // Return success
            doc     = NULL;

            return STATUS_OK;
        }

        status_t DocumentProcessor::process_define(IDocumentHandler *handler, document_t *doc, event_t *ev)
        {
            const char *var_name = ev->name.get_utf8(1);
            if (var_name == NULL)
                return STATUS_NO_MEM;

            char *var_value = ev->value.clone_utf8();
            if (var_value == NULL)
                return STATUS_NO_MEM;
            lsp_finally {
                if (var_value != NULL)
                    free(var_value);
            };

            return (vVars.put(var_name, var_value, &var_value)) ? STATUS_OK : STATUS_NO_MEM;
        }

        status_t DocumentProcessor::process_sample_data(IDocumentHandler *handler, event_t *ev)
        {
            // This should happen only for specific state
            if ((pScope == NULL) || (pScope->enType != SC_OTHER) || (pScope->enOther != OT_SAMPLE))
                return STATUS_CORRUPTED;

            // Already exists?
            if (((pScope->sName != NULL) || (pScope->sData.references()) > 0))
                return STATUS_CORRUPTED;

            // Bind the sample name
            if ((pScope->sName = ev->name.clone_utf8()) == NULL)
                return STATUS_NO_MEM;
            pScope->sData.wrap(ev->blob);

            return STATUS_OK;
        }

        status_t DocumentProcessor::main_loop(IDocumentHandler *handler)
        {
            // Main stuff
            status_t res;
            event_t ev;
            while (!vTree.is_empty())
            {
                // Obtain the current document
                document_t *doc = vTree.last();
                if (doc == NULL)
                    return STATUS_CORRUPTED;

                // Fetch next event
                if ((res = doc->pParser->next(&ev)) != STATUS_OK)
                {
                    // If not end of file, then we have troubles
                    if (res != STATUS_EOF)
                        return res;

                    // End of file, close current document first
                    res = destroy_document(doc);
                    if (!vTree.pop())
                    {
                        if (res == STATUS_OK)
                            res = STATUS_NO_MEM;
                    }
                    if (res != STATUS_OK)
                        return res;

                    // Then, continue the loop
                    continue;
                }

                // Dispatch event
                switch (ev.type)
                {
                    case EVENT_COMMENT:
                        res = STATUS_OK;
                        break;
                    case EVENT_HEADER:
                        res = process_header(handler, &ev);
                        break;
                    case EVENT_OPCODE:
                        res = process_opcode(&ev);
                        break;
                    case EVENT_INCLUDE:
                        res = process_include(handler, &ev);
                        break;
                    case EVENT_DEFINE:
                        res = process_define(handler, doc, &ev);
                        break;
                    case EVENT_SAMPLE:
                        res = process_sample_data(handler, &ev);
                        break;
                    case EVENT_NONE:
                    default:
                        res = STATUS_CORRUPTED;
                        break;
                }

                // Dispatch event
                if (res != STATUS_OK)
                    return res;
            }

            return STATUS_OK;
        }

        status_t DocumentProcessor::process(IDocumentHandler *handler)
        {
            // Check status and arguments
            if (handler == NULL)
                return STATUS_BAD_ARGUMENTS;
            if (nScopes < 0)
                return STATUS_CLOSED;

            // Notify handler about the start of document
            status_t res = handler->begin();
            if (res != STATUS_OK)
                return res;
            lsp_finally {
                if (handler != NULL)
                    handler->end(res);
            };

            // Perform main loop and cleanup
            res = main_loop(handler);
            if (res == STATUS_OK)
                res     = switch_scope(handler, SC_NONE);

            // Notify handler about end of document
            status_t res2 = handler->end(res);
            handler = NULL;
            return (res == STATUS_OK) ? res2 : res;
        }

        DocumentProcessor::document_t *DocumentProcessor::create_document()
        {
            document_t *doc = new document_t;
            doc->sPath      = NULL;
            doc->pParser    = NULL;
            doc->nWFlags    = WRAP_NONE;

            return doc;
        }

        status_t DocumentProcessor::destroy_document(document_t *document)
        {
            status_t res = STATUS_OK;
            if (document == NULL)
                return res;

            // Destroy parser if present
            if (document->pParser)
            {
                if (document->nWFlags & WRAP_CLOSE)
                    res = document->pParser->close();
                if (document->nWFlags & WRAP_DELETE)
                    delete document->pParser;

                document->pParser   = NULL;
                document->nWFlags   = WRAP_NONE;
            }

            // Destroy path if present
            if (document->sPath != NULL)
            {
                free(document->sPath);
                document->sPath     = NULL;
            }

            // Delete the document
            delete document;

            return res;
        }

        status_t DocumentProcessor::init_scope(scope_data_t *scope, scope_t type, scope_data_t *prev)
        {
            clear_scope(scope);
            scope->enType   = type;
            scope->pPrev    = prev;

            if (prev == NULL)
                return STATUS_OK;

            // Do not copy contents from the CONTROL scope or to ANY scope
            if ((prev->enType == SC_CONTROL) || (prev->enType == SC_OTHER))
                return STATUS_OK;

            // Copy the opcodes from the source scope
            lltl::parray<char> vk, vv;
            if (!prev->vOpcodes.items(&vk, &vv))
                return STATUS_NO_MEM;

            for (size_t i=0, n=vk.size(); i<n; ++i)
            {
                char *k = vk.uget(i);
                char *v = vv.uget(i);
                if ((k == NULL) || (v == NULL))
                    return STATUS_CORRUPTED;
                if (!scope->vOpcodes.create(k, v))
                {
                    clear_scope(scope);
                    return STATUS_NO_MEM;
                }
            }

            return STATUS_OK;
        }

        void DocumentProcessor::clear_scope(scope_data_t *scope)
        {
            if (scope == NULL)
                return;

            scope->vOpcodes.clear();
            for (size_t i=0, n=scope->vAllocated.size(); i<n; ++i)
            {
                char *str = scope->vAllocated.uget(i);
                if (str != NULL)
                    free(str);
            }
            scope->vAllocated.clear();
            if (scope->sName!= NULL)
            {
                free(scope->sName);
                scope->sName      = NULL;
            }

            scope->sData.close();

            scope->enType       = SC_NONE;
            scope->enOther      = OT_CUSTOM;
            scope->pPrev        = NULL;
        }

        void DocumentProcessor::drop_hash(lltl::pphash<char, char> *data)
        {
            if (data == NULL)
                return;

            lltl::parray<char> vv;
            data->values(&vv);
            data->flush();

            for (size_t i=0, n=vv.size(); i<n; ++i)
            {
                char *str = vv.uget(i);
                if (str != NULL)
                    free(str);
            }
            vv.flush();
        }

        DocumentProcessor::scope_t DocumentProcessor::get_scope(const LSPString *header)
        {
            if (header->equals_ascii(HDR_CONTROL))
                return SC_CONTROL;
            if (header->equals_ascii(HDR_GLOBAL))
                return SC_GLOBAL;
            if (header->equals_ascii(HDR_MASTER))
                return SC_MASTER;
            if (header->equals_ascii(HDR_GROUP))
                return SC_GROUP;
            if (header->equals_ascii(HDR_REGION))
                return SC_REGION;

            return SC_OTHER;
        }

        status_t DocumentProcessor::dispatch_scope(IDocumentHandler *handler, scope_data_t *scope)
        {
            switch (scope->enType)
            {
                case SC_NONE:
                case SC_GLOBAL:
                case SC_MASTER:
                case SC_GROUP:
                    return STATUS_OK;
                default:
                    break;
            }

            // Prepare lists of opcodes
            lltl::parray<char> vk, vv;
            if (!scope->vOpcodes.items(&vk, &vv))
                return STATUS_NO_MEM;
            if (!vk.add(static_cast<char *>(NULL)))
                return STATUS_NO_MEM;
            if (!vv.add(static_cast<char *>(NULL)))
                return STATUS_NO_MEM;

            const char **opcodes    = const_cast<const char **>(vk.array());
            const char **values     = const_cast<const char **>(vv.array());

            // Process main scopes
            switch (scope->enType)
            {
                case SC_CONTROL:
                    return handler->control(opcodes, values);
                case SC_REGION:
                    return handler->region(opcodes, values);
                default: // SC_OTHER
                    break;
            }

            // Process sub-types of SC_OTHER scope
            switch (scope->enOther)
            {
                case OT_SAMPLE:
                    return handler->sample(scope->sName, &scope->sData, opcodes, values);
                case OT_MIDI:
                    return handler->midi(opcodes, values);
                case OT_CURVE:
                    return handler->curve(opcodes, values);
                case OT_EFFECT:
                    return handler->effect(opcodes, values);
                default: // OT_CUSTOM
                    break;
            }

            return handler->custom_header(scope->sName, opcodes, values);
        }

        status_t DocumentProcessor::switch_scope(IDocumentHandler *handler, scope_t scope)
        {
            status_t res;

            // Walk back if there is necessity
            while ((pScope != NULL) && (pScope->enType >= scope))
            {
                // Call the dispatcher
                if (pScope->enType != SC_CONTROL)
                {
                    if ((res = dispatch_scope(handler, pScope)) != STATUS_OK)
                        return res;
                }

                // Free the scope
                scope_data_t *prev = pScope->pPrev;
                clear_scope(pScope);
                --nScopes;
                pScope          = prev;
            }

            // Allocate new scope and link to previous
            if (nScopes >= SC_TOTAL)
                return STATUS_OVERFLOW;

            // Special case: we're switching from SC_CONTROL scope
            if ((pScope != NULL) && (pScope->enType == SC_CONTROL))
            {
                // Call the dispatcher
                if ((res = dispatch_scope(handler, pScope)) != STATUS_OK)
                    return res;
            }

            // Check that we switched to SC_NONE scope
            if (scope == SC_NONE)
                return STATUS_OK;

            // Allocate new scope
            scope_data_t *s = &vScopes[nScopes++];
            if ((res = init_scope(s, scope, pScope)) != STATUS_OK)
            {
                --nScopes;
                clear_scope(s);
                return res;
            }

            // Update the current scope pointer
            pScope      = s;

            return STATUS_OK;
        }

        char *DocumentProcessor::process_value(const LSPString *value)
        {
            LSPString res;

            for (size_t i=0, n=value->length(); i < n; )
            {
                lsp_wchar_t ch = value->char_at(i++);
                if (ch == '$')
                {
                    // Substitute variable
                    size_t first = i;
                    bool found = false;
                    while (i < n)
                    {
                        // New variable name?
                        char ch = value->char_at(i);
                        if (ch == '$')
                            break;

                        // Find the variable and substitute if it was found
                        const char *var = value->get_utf8(first, ++i);
                        const char *val = vVars.get(var);
                        if (val != NULL)
                        {
                            if (!res.append_utf8(val))
                                return NULL;
                            found = true;
                            break;
                        }
                    }

                    // No substitution was made?
                    if (!found)
                    {
                        if (!res.append(value, first-1, i))
                            return NULL;
                    }
                }
                else
                {
                    if (!res.append(ch))
                        return NULL;
                }
            }

            return res.clone_utf8();
        }

    } /* namespace sfz */
} /* namespace lsp */




