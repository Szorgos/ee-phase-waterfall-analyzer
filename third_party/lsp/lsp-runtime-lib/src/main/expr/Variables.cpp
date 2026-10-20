/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 19 сент. 2019 г.
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

#include <lsp-plug.in/expr/types.h>
#include <lsp-plug.in/expr/Variables.h>
#include <lsp-plug.in/common/debug.h>

namespace lsp
{
    namespace expr
    {
        Variables::Variables()
        {
            pResolver       = NULL;
        }
        
        Variables::Variables(Resolver *r)
        {
            pResolver       = r;
        }
        
        Variables::~Variables()
        {
            clear();
        }
    
        status_t Variables::set_int(const char *name, ssize_t value)
        {
            if (name == NULL)
                return STATUS_BAD_ARGUMENTS;
            LSPString key;
            if (!key.set_utf8(name))
                return STATUS_NO_MEM;

            value_t v;
            v.type      = VT_INT;
            v.v_int     = value;
            return set(&key, &v);
        }

        status_t Variables::set_float(const char *name, double value)
        {
            if (name == NULL)
                return STATUS_BAD_ARGUMENTS;
            LSPString key;
            if (!key.set_utf8(name))
                return STATUS_NO_MEM;

            value_t v;
            v.type      = VT_FLOAT;
            v.v_float   = value;
            return set(&key, &v);
        }

        status_t Variables::set_bool(const char *name, bool value)
        {
            if (name == NULL)
                return STATUS_BAD_ARGUMENTS;
            LSPString key;
            if (!key.set_utf8(name))
                return STATUS_NO_MEM;

            value_t v;
            v.type      = VT_BOOL;
            v.v_bool    = value;
            return set(&key, &v);
        }

        status_t Variables::set_string(const char *name, const char *value, const char *charset)
        {
            if (name == NULL)
                return STATUS_BAD_ARGUMENTS;

            LSPString key;
            if (!key.set_utf8(name))
                return STATUS_NO_MEM;

            value_t v;
            if (value == NULL)
            {
                v.type          = VT_NULL;
                v.v_str         = NULL;
                return set(&key, &v);
            }

            status_t res = init_value_string(&v, value, charset);
            if (res == STATUS_OK)
                res = set_move(&key, &v);

            destroy_value(&v);
            return res;
        }

        status_t Variables::set_string(const char *name, const LSPString *value)
        {
            if (name == NULL)
                return STATUS_BAD_ARGUMENTS;

            LSPString key;
            if (!key.set_utf8(name))
                return STATUS_NO_MEM;

            value_t v;
            if (value == NULL)
            {
                v.type          = VT_NULL;
                v.v_str         = NULL;
                return set(&key, &v);
            }

            status_t res = init_value_string(&v, value);
            if (res == STATUS_OK)
                res = set_move(&key, &v);

            destroy_value(&v);
            return res;
        }

        status_t Variables::set_null(const char *name)
        {
            if (name == NULL)
                return STATUS_BAD_ARGUMENTS;
            LSPString key;
            if (!key.set_utf8(name))
                return STATUS_NO_MEM;

            value_t v;
            v.type          = VT_NULL;
            v.v_int         = 0;
            return set(&key, &v);
        }

        status_t Variables::set_int(const LSPString *name, ssize_t value)
        {
            value_t v;
            v.type          = VT_INT;
            v.v_int         = value;
            return set(name, &v);
        }

        status_t Variables::set_float(const LSPString *name, double value)
        {
            value_t v;
            v.type          = VT_FLOAT;
            v.v_float       = value;
            return set(name, &v);
        }

        status_t Variables::set_bool(const LSPString *name, bool value)
        {
            value_t v;
            v.type          = VT_BOOL;
            v.v_bool        = value;
            return set(name, &v);
        }

        status_t Variables::set_string(const LSPString *name, const char *value, const char *charset)
        {
            value_t v;

            if (value == NULL)
            {
                v.type      = VT_NULL;
                v.v_str     = NULL;
                return set(name, &v);
            }

            status_t res    = init_value_string(&v, value, charset);
            if (res == STATUS_OK)
                res             = set_move(name, &v);

            destroy_value(&v);
            return res;
        }

        status_t Variables::set_string(const LSPString *name, const LSPString *value)
        {
            value_t v;
            v.type          = VT_STRING;
            v.v_str         = const_cast<LSPString *>(value);

            return set(name, &v);
        }

        status_t Variables::set_null(const LSPString *name)
        {
            value_t v;
            v.type          = VT_NULL;
            v.v_int         = 0;
            return set(name, &v);
        }

        status_t Variables::set(const char *name, const value_t *value)
        {
            if (name == NULL)
                return STATUS_BAD_ARGUMENTS;
            LSPString key;
            if (!key.set_utf8(name))
                return STATUS_NO_MEM;

            return set(&key, value);
        }

        status_t Variables::unset(const char *name, value_t *value)
        {
            if (name == NULL)
                return STATUS_BAD_ARGUMENTS;
            LSPString key;
            if (!key.set_utf8(name))
                return STATUS_NO_MEM;

            return unset(&key, value);
        }

        status_t Variables::resolve(value_t *value, const char *name, size_t num_indexes, const ssize_t *indexes)
        {
            if (name == NULL)
                return STATUS_BAD_ARGUMENTS;

            LSPString key;
            if (!key.set_utf8(name))
                return STATUS_NO_MEM;

            return resolve(value, &key, num_indexes, indexes);
        }

        status_t Variables::resolve(value_t *value, const LSPString *name, size_t num_indexes, const ssize_t *indexes)
        {
            // Need to form indexes?
            LSPString tmp;
            const LSPString *search;

            if (num_indexes > 0)
            {
                if (!tmp.set(name))
                    return STATUS_NO_MEM;
                for (size_t i=0; i<num_indexes; ++i)
                {
                    if (!tmp.fmt_append_ascii("_%ld", long(indexes[i])))
                        return STATUS_NO_MEM;
                }
                search = &tmp;
            }
            else
                search = name;

            // Lookup the cache
            ssize_t idx     = index_of_var(search);
            if (idx >= 0)
            {
                variable_t *var = vVars.uget(idx);
                int cmp = search->compare_to(&var->name);
                if (cmp == 0)
                {
                    if (value != NULL)
                        return copy_value(value, &var->value);
                    return STATUS_OK;
                }
                else if (cmp > 0)
                    ++idx;
            }
            else
                idx = 0;

            // No Resolver?
            if (pResolver == NULL)
                return STATUS_NOT_FOUND;

            // Resolve from underlying resolver
            value_t v;
            init_value(&v);
            lsp_finally { destroy_value(&v); };
            status_t res = pResolver->resolve(&v, name, num_indexes, indexes);
            if (res != STATUS_OK)
                return res;

            // Save variable to cache
            res = insert_var(search, &v, idx);
            if ((res == STATUS_OK) && (value != NULL))
                res = copy_value(value, &v);

            return res;
        }

        status_t Variables::call(value_t *value, const char *name, size_t num_args, const value_t *args)
        {
            if (name == NULL)
                return STATUS_BAD_ARGUMENTS;

            LSPString key;
            if (!key.set_utf8(name))
                return STATUS_NO_MEM;

            return call(value, &key, num_args, args);
        }

        status_t Variables::call(value_t *value, const LSPString *name, size_t num_args, const value_t *args)
        {
            // Lookup the cache
            ssize_t idx     = index_of_func(name);
            if (idx < 0)
            {
                if (pResolver != NULL)
                    return pResolver->call(value, name, num_args, args);

                return Resolver::call(value, name, num_args, args);
            }

            // Ensure than name of function matches
            user_func_t *func = vFunc.uget(idx);
            if (!name->equals(&func->name))
            {
                if (pResolver != NULL)
                    return pResolver->call(value, name, num_args, args);

                return Resolver::call(value, name, num_args, args);
            }

            // Execute function
            return func->func(func->context, value, num_args, args);
        }

        ssize_t Variables::index_of_var(const LSPString *name)
        {
            const variable_t *var;
            ssize_t first = 0, last = vVars.size() - 1, mid;
            if (last < 0)
                return -1;

            while (first < last)
            {
                mid             = (first + last) >> 1;
                var             = vVars.uget(mid);
                int cmp         = name->compare_to(&var->name);

                if (cmp < 0)
                    last            = mid - 1;
                else if (cmp > 0)
                    first           = mid + 1;
                else
                    return mid;
            }

            return first;
        }

        ssize_t Variables::index_of_func(const LSPString *name)
        {
            const user_func_t *f;
            ssize_t first = 0, last = vFunc.size() - 1, mid;
            if (last < 0)
                return -1;

            while (first < last)
            {
                mid             = (first + last) >> 1;
                f               = vFunc.uget(mid);
                int cmp         = name->compare_to(&f->name);

                if (cmp < 0)
                    last            = mid - 1;
                else if (cmp > 0)
                    first           = mid + 1;
                else
                    return mid;
            }

            return first;
        }

        status_t Variables::insert_var(const LSPString *name, const value_t *value, size_t idx)
        {
            variable_t *var = new variable_t;
            if (var == NULL)
                return STATUS_NO_MEM;
            if (!var->name.set(name))
            {
                delete var;
                return STATUS_NO_MEM;
            }

            status_t res = init_value(&var->value, value);
            if (res == STATUS_OK)
                res = (vVars.insert(idx, var)) ? STATUS_OK : STATUS_NO_MEM;
            if (res == STATUS_OK)
                return res;

            destroy_value(&var->value);
            delete var;
            return res;
        }

        status_t Variables::insert_var_move(const LSPString *name, value_t *value, size_t idx)
        {
            variable_t *var = new variable_t;
            if (var == NULL)
                return STATUS_NO_MEM;
            if (!var->name.set(name))
            {
                delete var;
                return STATUS_NO_MEM;
            }

            init_value_move(&var->value, value);
            status_t res = (vVars.insert(idx, var)) ? STATUS_OK : STATUS_NO_MEM;
            if (res == STATUS_OK)
                return res;

            destroy_value(&var->value);
            delete var;
            return res;
        }

        status_t Variables::insert_func(const LSPString *name, function_t func, void *context, size_t idx)
        {
            user_func_t *f = new user_func_t;
            if (f == NULL)
                return STATUS_NO_MEM;
            if (!f->name.set(name))
            {
                delete f;
                return STATUS_NO_MEM;
            }

            f->func         = func;
            f->context      = context;

            if (vFunc.insert(idx, f))
                return STATUS_OK;

            delete f;
            return STATUS_NO_MEM;
        }

        status_t Variables::set(const LSPString *name, const value_t *value)
        {
            if (name == NULL)
                return STATUS_BAD_ARGUMENTS;

            ssize_t idx     = index_of_var(name);
            if (idx >= 0)
            {
                variable_t *var = vVars.uget(idx);
                int cmp = name->compare_to(&var->name);
                if (cmp == 0)
                    return copy_value(&var->value, value);
                else if (cmp > 0)
                    ++idx;
            }
            else
                idx = 0;

            // Add non-existing value
            return insert_var(name, value, idx);
        }

        status_t Variables::set_move(const LSPString *name, value_t *value)
        {
            if (name == NULL)
                return STATUS_BAD_ARGUMENTS;

            ssize_t idx     = index_of_var(name);
            if (idx >= 0)
            {
                variable_t *var = vVars.uget(idx);
                int cmp = name->compare_to(&var->name);
                if (cmp == 0)
                {
                    move_value(&var->value, value);
                    return STATUS_OK;
                }
                else if (cmp > 0)
                    ++idx;
            }
            else
                idx = 0;

            // Add non-existing value
            return insert_var_move(name, value, idx);
        }

        status_t Variables::unset(const LSPString *name, value_t *value)
        {
            if (name == NULL)
                return STATUS_BAD_ARGUMENTS;

            // Lookup for variable
            ssize_t idx     = index_of_var(name);
            if (idx < 0)
                return STATUS_NOT_FOUND;

            variable_t *var = vVars.uget(idx);
            if (!name->equals(&var->name))
                return STATUS_NOT_FOUND;

            // Need to copy value?
            if (value != NULL)
            {
                status_t res = copy_value(value, &var->value);
                if (res != STATUS_OK)
                    return res;
            }

            // Remove stored value
            vVars.remove(idx);
            destroy_value(&var->value);
            delete var;

            return STATUS_OK;
        }

        void Variables::clear_vars()
        {
            for (size_t i=0, n=vVars.size(); i<n; ++i)
            {
                variable_t *var = vVars.uget(i);
                if (var != NULL)
                {
                    destroy_value(&var->value);
                    delete var;
                }
            }
            vVars.flush();
        }

        void Variables::clear_func()
        {
            for (size_t i=0, n=vFunc.size(); i<n; ++i)
            {
                user_func_t *func = vFunc.uget(i);
                if (func != NULL)
                    delete func;
            }
            vFunc.flush();
        }

        void Variables::clear()
        {
            clear_vars();
            clear_func();
        }

        status_t Variables::bind_func(const char *name, function_t func, void *context)
        {
            if (name == NULL)
                return STATUS_BAD_ARGUMENTS;
            LSPString key;
            if (!key.set_utf8(name))
                return STATUS_NO_MEM;

            return bind_func(&key, func, context);
        }

        status_t Variables::bind_func(const LSPString *name, function_t func, void *context)
        {
            if (name == NULL)
                return STATUS_BAD_ARGUMENTS;

            ssize_t idx     = index_of_func(name);
            if (idx >= 0)
            {
                user_func_t *f = vFunc.uget(idx);
                int cmp = name->compare_to(&f->name);
                if (cmp == 0)
                {
                    if (!f->name.set(name))
                        return STATUS_NO_MEM;
                    f->func     = func;
                    f->context  = context;
                }
                else if (cmp > 0)
                    ++idx;
            }
            else
                idx = 0;

            // Add non-existing value
            return insert_func(name, func, context, idx);
        }

        status_t Variables::unbind_func(const char *name)
        {
            if (name == NULL)
                return STATUS_BAD_ARGUMENTS;
            LSPString key;
            if (!key.set_utf8(name))
                return STATUS_NO_MEM;

            return unbind_func(&key);
        }

        status_t Variables::unbind_func(const LSPString *name)
        {
            if (name == NULL)
                return STATUS_BAD_ARGUMENTS;

            // Lookup for data
            ssize_t idx     = index_of_func(name);
            if (idx < 0)
                return STATUS_NOT_FOUND;

            user_func_t *func = vFunc.uget(idx);
            if (!name->equals(&func->name))
                return STATUS_NOT_FOUND;

            // Remove function
            vFunc.remove(idx);
            delete func;

            return STATUS_OK;
        }

    } /* namespace expr */
} /* namespace lsp */
