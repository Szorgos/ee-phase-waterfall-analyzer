/*
 * Copyright (C) 2024 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2024 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 16 сент. 2019 г.
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

#include <lsp-plug.in/expr/Resolver.h>
#include <lsp-plug.in/expr/functions.h>
#include <lsp-plug.in/stdlib/string.h>

namespace lsp
{
    namespace expr
    {
        typedef struct builtin_func_t
        {
            const char *name;
            const stdfunc_t func;
        } builtin_func_t;
        
        static const builtin_func_t builtin_functions[] =
        {
            { "avg", stdfunc_avg },
            { "max", stdfunc_max },
            { "min", stdfunc_min },
            { "rms", stdfunc_rms },
        };

        stdfunc_t Resolver::find_std_func(const char *name)
        {
            ssize_t first = 0, last = sizeof(builtin_functions)/sizeof(builtin_func_t);
            while (first <= last)
            {
                ssize_t mid = (first + last) >> 1;
                const builtin_func_t *f = &builtin_functions[mid];

                int res = strcmp(name, f->name);
                if (res == 0)
                    return f->func;
                else if (res < 0)
                    last = mid - 1;
                else // if (res < 0)
                    first = mid + 1;
            }

            return NULL;
        }

        Resolver::Resolver()
        {
        }
        
        Resolver::~Resolver()
        {
        }
    
        status_t Resolver::resolve(value_t *value, const char *name, size_t num_indexes, const ssize_t *indexes)
        {
            set_value_null(value);
            return STATUS_OK;
        }

        status_t Resolver::resolve(value_t *value, const LSPString *name, size_t num_indexes, const ssize_t *indexes)
        {
            return resolve(value, name->get_utf8(), num_indexes, indexes);
        }

        status_t Resolver::call(value_t *value, const char *name, size_t num_args, const value_t *args)
        {
            stdfunc_t func = find_std_func(name);
            if (func != NULL)
                return func(value, num_args, args);

            return STATUS_NOT_FOUND;
        }

        status_t Resolver::call(value_t *value, const LSPString *name, size_t num_args, const value_t *args)
        {
            return Resolver::call(value, name->get_utf8(), num_args, args);
        }

    } /* namespace expr */
} /* namespace lsp */
