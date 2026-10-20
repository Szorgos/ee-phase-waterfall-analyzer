/*
 * Copyright (C) 2020 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2020 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 26 окт. 2020 г.
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

#include <lsp-plug.in/resource/Environment.h>
#include <lsp-plug.in/lltl/parray.h>

namespace lsp
{
    namespace resource
    {
        Environment::Environment()
        {
        }

        Environment::~Environment()
        {
            lltl::parray<LSPString> values;
            vEnv.values(&values);
            vEnv.flush();

            // Destroy strings
            for (size_t i=0; i<values.size(); ++i)
                delete values.uget(i);
        }

        Environment *Environment::clone()
        {
            lltl::parray<LSPString> vk, vv;
            if (!vEnv.items(&vk, &vv))
                return NULL;

            // Copy keys and values
            Environment *env = new Environment();
            if (env == NULL)
                return NULL;
            lsp_finally {
                if (env != NULL)
                    delete env;
            };

            for (size_t i=0, n=vk.size(); i<n; ++i)
            {
                // Obtain key and copy of the value
                LSPString *k = vk.uget(i);
                if (k == NULL)
                    continue;
                LSPString *v = vv.uget(i);
                if (v == NULL)
                    continue;

                // Add to collection
                if (!(v = v->clone()))
                    return NULL;

                if (!env->vEnv.put(k, v, &v))
                {
                    delete v;
                    return NULL;
                }

                // Delete previously stored value
                if (v != NULL)
                    delete v;
            }

            return release_ptr(env);
        }

        const LSPString *Environment::get(const char *key) const
        {
            if (key == NULL)
                return NULL;
            LSPString k;
            return (k.set_utf8(key)) ? get(&k) : NULL;
        }

        const LSPString *Environment::get(const LSPString *key) const
        {
            if (key == NULL)
                return NULL;
            return vEnv.get(key);
        }

        const LSPString *Environment::get(const LSPString *key, const LSPString *dfl) const
        {
            if (key == NULL)
                return NULL;
            const LSPString *res = vEnv.get(key);
            return (res != NULL) ? res : dfl;
        }

        const char *Environment::get_utf8(const char *key) const
        {
            if (key == NULL)
                return NULL;

            const LSPString *res = get(key);
            return (res != NULL) ? res->get_utf8() : NULL;
        }

        const char *Environment::get_utf8(const char *key, const char *dfl) const
        {
            if (key == NULL)
                return NULL;

            LSPString k;
            if (!k.set_utf8(key))
                return NULL;

            const LSPString *v = vEnv.get(&k);
            return (v != NULL) ? v->get_utf8() : dfl;
        }

        const char *Environment::get_utf8(const LSPString *key) const
        {
            if (key == NULL)
                return NULL;

            const LSPString *res = get(key);
            return (res != NULL) ? res->get_utf8() : NULL;
        }

        bool Environment::contains(const char *key) const
        {
            LSPString k;
            return (k.set_utf8(key)) ? vEnv.contains(&k) : false;
        }

        bool Environment::contains(const LSPString *key) const
        {
            return vEnv.contains(key);
        }

        status_t Environment::set(const char *key, const char *value)
        {
            if ((key == NULL) || (value == NULL))
                return STATUS_INVALID_VALUE;
            LSPString k, v;
            return ((k.set_utf8(key)) && (v.set_utf8(value))) ? set(&k, &v) : STATUS_NO_MEM;
        }

        status_t Environment::set(const char *key, const LSPString *value)
        {
            if ((key == NULL) || (value == NULL))
                return STATUS_INVALID_VALUE;
            LSPString k;
            return (k.set_utf8(key)) ? set(&k, value) : STATUS_NO_MEM;
        }

        status_t Environment::set(const LSPString *key, const char *value)
        {
            if ((key == NULL) || (value == NULL))
                return STATUS_INVALID_VALUE;
            LSPString v;
            return (v.set_utf8(value)) ? set(key, &v) : STATUS_NO_MEM;
        }

        status_t Environment::set(const LSPString *key, const LSPString *value)
        {
            if ((key == NULL) || (value == NULL))
                return STATUS_INVALID_VALUE;

            LSPString *ns = value->clone();
            if (ns == NULL)
                return STATUS_NO_MEM;
            vEnv.put(key, ns, &ns);
            if (ns != NULL)
                delete ns;

            return STATUS_OK;
        }


        status_t Environment::remove(const char *key)
        {
            if (key == NULL)
                return STATUS_INVALID_VALUE;

            LSPString k;
            return (k.set_utf8(key)) ? remove(&k) : STATUS_NO_MEM;
        }

        status_t Environment::remove(const LSPString *key)
        {
            LSPString *old = NULL;
            if (!vEnv.remove(key, &old))
                return STATUS_NOT_FOUND;

            if (old != NULL)
                delete old;
            return STATUS_OK;
        }
    }
}

