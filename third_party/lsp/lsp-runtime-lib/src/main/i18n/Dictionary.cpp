/*
 * Copyright (C) 2023 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2023 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 26 февр. 2020 г.
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
#include <lsp-plug.in/i18n/Dictionary.h>
#include <lsp-plug.in/i18n/JsonDictionary.h>

namespace lsp
{
    namespace i18n
    {
        Dictionary::Dictionary(resource::ILoader *loader)
        {
            pLoader     = loader;
        }

        Dictionary::~Dictionary()
        {
            clear();
        }

        status_t Dictionary::load_json(IDictionary **dict, const io::Path *path)
        {
            JsonDictionary *d = new JsonDictionary();
            status_t res;

            if (pLoader != NULL)
            {
                io::IInStream *is = pLoader->read_stream(path);
                if (is != NULL)
                {
                    res     = d->init(is);
                    is->close();
                    delete is;
                }
                else
                    res     = pLoader->last_error();
            }
            else
                res = d->init(path);

            if (res == STATUS_OK)
                *dict = d;
            else
                delete d;

            return res;
        }

        status_t Dictionary::create_child(IDictionary **dict, const LSPString *path)
        {
            LSPString xp;
            if (!xp.append(&sPath))
                return STATUS_NO_MEM;
            if (!xp.append('/'))
                return STATUS_NO_MEM;
            if (!xp.append(path))
                return STATUS_NO_MEM;

            Dictionary *d = new Dictionary(pLoader);
            status_t res = d->init(&xp);
            if (res != STATUS_OK)
                delete d;
            else
                *dict = d;
            return res;
        }

        status_t Dictionary::load_dictionary(const LSPString *id, IDictionary **dict)
        {
            status_t res;
            io::Path path;

            if ((res = path.set(&sPath)) != STATUS_OK)
                return res;
            if ((res = path.append_child(id)) != STATUS_OK)
                return res;
            if ((res = path.append(".json")) != STATUS_OK)
                return res;

            // Prefer builtin over external
            lsp_debug("Trying to load file %s...", path.as_native());
            res = load_json(dict, &path);
            lsp_debug("Overall status = %d", int(res));

            return res;
        }

        status_t Dictionary::lookup(const LSPString *key, LSPString *value)
        {
            if (key == NULL)
                return STATUS_INVALID_VALUE;

            LSPString id, subkey;
            ssize_t idx = key->index_of('.');
            if (idx < 0)
            {
                if (!id.set(key))
                    return STATUS_NO_MEM;
            }
            else
            {
                if (!id.set(key, 0, idx))
                    return STATUS_NO_MEM;
                if (!subkey.set(key, idx+1))
                    return STATUS_NO_MEM;
            }

            // Perform binary search of the item
            ssize_t first = 0, last = vNodes.size()-1;
            while (first <= last)
            {
                ssize_t curr = (first + last) >> 1;
                node_t *node = vNodes.uget(curr);
                int cmp = node->sKey.compare_to(&id);

                if (cmp > 0)
                    last    = curr - 1;
                else if (cmp < 0)
                    first   = curr + 1;
                else
                {
                    if (id.is_empty())
                        return STATUS_NOT_FOUND;
                    return (node->pDict != NULL) ? node->pDict->lookup(&subkey, value) : STATUS_NOT_FOUND;
                }
            }

            // Dictionary identifier was specified?
            if (id.is_empty())
                return STATUS_NOT_FOUND;

            // Dictionary not found, try to create new one
            IDictionary *dict = NULL;
            status_t res = load_dictionary(&id, &dict);
            if (res == STATUS_NOT_FOUND)
                res = create_child(&dict, &id);

            // Add node to list of nodes
            if (res != STATUS_OK)
                return res;

            node_t *child = new node_t;
            if ((child == NULL) || (!vNodes.insert(first, child)))
            {
                delete dict;
                return STATUS_NO_MEM;
            }

            child->sKey.swap(&id);
            child->pDict        = dict;

            return dict->lookup(&subkey, value);
        }

        status_t Dictionary::lookup(const LSPString *key, IDictionary **value)
        {
            if (key == NULL)
                return STATUS_INVALID_VALUE;

            ssize_t idx = key->index_of('.');
            LSPString id, subkey;

            if (idx > 0)
            {
                if (!id.set(key, 0, idx))
                    return STATUS_NO_MEM;
                if (!subkey.set(key, idx+1))
                    return STATUS_NO_MEM;
            }
            else if (!id.set(key))
                return STATUS_NO_MEM;

            // Perform binary search of the dictionary
            IDictionary *dict = NULL;
            ssize_t first = 0, last = vNodes.size()-1;
            while (first <= last)
            {
                ssize_t curr = (first + last) >> 1;
                node_t *node = vNodes.uget(curr);
                int cmp = node->sKey.compare_to(&id);

                if (cmp > 0)
                    last    = curr - 1;
                else if (cmp < 0)
                    first   = curr + 1;
                else
                {
                    if (node->pDict == NULL)
                        return STATUS_NOT_FOUND;
                    dict = node->pDict;
                    break;
                }
            }

            // Dictionary object not found?
            if (dict == NULL)
            {
                // Try to load/create node
                bool root    = false;
                status_t res = load_dictionary(&id, &dict);
                if (res == STATUS_NOT_FOUND)
                {
                    res         = create_child(&dict, &id);
                    root        = true;
                }
                if (res != STATUS_OK)
                    return res;

                // Add node to list of nodes
                node_t *child = new node_t;
                if ((child == NULL) || (!vNodes.insert(first, child)))
                {
                    delete dict;
                    return STATUS_NO_MEM;
                }

                child->sKey.swap(&id);
                child->pDict        = dict;
                child->bRoot        = root;

                // There is no path element defined more?
                if ((child->bRoot) && (idx < 0))
                    return STATUS_NOT_FOUND;
            }

            if (idx > 0)
                return dict->lookup(&subkey, value);

            *value = dict;
            return STATUS_OK;
        }

        status_t Dictionary::get_value(size_t index, LSPString *key, LSPString *value)
        {
            node_t *node = vNodes.get(index);
            return (node == NULL) ? STATUS_NOT_FOUND : STATUS_BAD_TYPE;
        }

        status_t Dictionary::get_child(size_t index, LSPString *key, IDictionary **dict)
        {
            node_t *node = vNodes.get(index);
            if ((node == NULL) || (node->pDict == NULL))
                return STATUS_NOT_FOUND;

            if ((key != NULL) && (!key->set(&node->sKey)))
                return STATUS_NO_MEM;

            if (dict != NULL)
                *dict = node->pDict;

            return STATUS_OK;
        }

        size_t Dictionary::size()
        {
            return vNodes.size();
        }

        status_t Dictionary::Dictionary::init(const LSPString *path)
        {
            lsp_trace("Init dictionary path: %s" , path->get_utf8());
            return (sPath.set(path)) ? STATUS_OK : STATUS_NO_MEM;
        }

        void  Dictionary::Dictionary::clear()
        {
            for (size_t i=0, n=vNodes.size(); i<n; ++i)
            {
                node_t *node = vNodes.uget(i);
                if (node == NULL)
                    continue;

                if (node->pDict != NULL)
                    delete node->pDict;
                delete node;
            }

            vNodes.flush();
        }

    } /* namespace i18n */
} /* namespace lsp */
