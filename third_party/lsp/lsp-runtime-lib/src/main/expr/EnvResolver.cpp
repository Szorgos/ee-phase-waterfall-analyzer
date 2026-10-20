/*
 * Copyright (C) 2023 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2023 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-plugins-sampler
 * Created on: 25 янв. 2023 г.
 *
 * lsp-plugins-sampler is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * lsp-plugins-sampler is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with lsp-plugins-sampler. If not, see <https://www.gnu.org/licenses/>.
 */

#include <lsp-plug.in/expr/EnvResolver.h>
#include <lsp-plug.in/expr/types.h>
#include <lsp-plug.in/runtime/system.h>

namespace lsp
{
    namespace expr
    {
        EnvResolver::EnvResolver()
        {
        }

        EnvResolver::~EnvResolver()
        {
        }

        status_t EnvResolver::resolve(value_t *value, const char *name, size_t num_indexes, const ssize_t *indexes)
        {
            if (num_indexes > 0)
            {
                expr::set_value_null(value);
                return STATUS_OK;
            }

            LSPString var;
            status_t res = system::get_env_var(name, &var);
            if (res != STATUS_OK)
            {
                if (res == STATUS_NOT_FOUND)
                {
                    expr::set_value_null(value);
                    return STATUS_OK;
                }
                return res;
            }

            return expr::set_value_string(value, &var);
        }

        status_t EnvResolver::resolve(value_t *value, const LSPString *name, size_t num_indexes, const ssize_t *indexes)
        {
            if (num_indexes > 0)
            {
                expr::set_value_null(value);
                return STATUS_OK;
            }

            LSPString var;
            status_t res = system::get_env_var(name, &var);
            if (res != STATUS_OK)
            {
                if (res == STATUS_NOT_FOUND)
                {
                    expr::set_value_null(value);
                    return STATUS_OK;
                }
                return res;
            }

            return expr::set_value_string(value, &var);
        }

    } /* namespace expr */
} /* namespace lsp */


