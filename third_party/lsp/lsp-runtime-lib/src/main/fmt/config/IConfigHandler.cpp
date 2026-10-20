/*
 * Copyright (C) 2020 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2020 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 30 апр. 2020 г.
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

#include <lsp-plug.in/fmt/config/IConfigHandler.h>

namespace lsp
{
    namespace config
    {
        
        IConfigHandler::IConfigHandler()
        {
        }
        
        IConfigHandler::~IConfigHandler()
        {
        }

        status_t IConfigHandler::start()
        {
            return STATUS_OK;
        }

        status_t IConfigHandler::finish(status_t res)
        {
            return res;
        }

        status_t IConfigHandler::handle(const param_t *param)
        {
            if (param == NULL)
                return STATUS_BAD_ARGUMENTS;

            switch (param->flags & SF_TYPE_MASK)
            {
                case SF_TYPE_I32: return handle_i32(&param->name, param->v.i32, param->flags);
                case SF_TYPE_U32: return handle_u32(&param->name, param->v.u32, param->flags);
                case SF_TYPE_I64: return handle_i64(&param->name, param->v.i64, param->flags);
                case SF_TYPE_U64: return handle_u64(&param->name, param->v.u64, param->flags);
                case SF_TYPE_F32: return handle_f32(&param->name, param->v.f32, param->flags);
                case SF_TYPE_F64: return handle_f64(&param->name, param->v.f64, param->flags);
                case SF_TYPE_BOOL: return handle_bool(&param->name, param->v.bval, param->flags);
                case SF_TYPE_BLOB: return handle_blob(&param->name, &param->v.blob, param->flags);
                case SF_TYPE_STR:
                {
                    LSPString tmp;
                    if (!tmp.set_utf8(param->v.str))
                        return STATUS_NO_MEM;
                    return handle_string(&param->name, &tmp, param->flags);
                }

                default:
                    break;
            }

            return STATUS_BAD_TYPE;
        }

        status_t IConfigHandler::handle_i32(const LSPString *name, int32_t value, size_t flags)
        {
            return STATUS_OK;
        }

        status_t IConfigHandler::handle_u32(const LSPString *name, uint32_t value, size_t flags)
        {
            return STATUS_OK;
        }

        status_t IConfigHandler::handle_f32(const LSPString *name, float value, size_t flags)
        {
            return STATUS_OK;
        }

        status_t IConfigHandler::handle_i64(const LSPString *name, int64_t value, size_t flags)
        {
            return STATUS_OK;
        }

        status_t IConfigHandler::handle_u64(const LSPString *name, uint64_t value, size_t flags)
        {
            return STATUS_OK;
        }

        status_t IConfigHandler::handle_f64(const LSPString *name, double value, size_t flags)
        {
            return STATUS_OK;
        }

        status_t IConfigHandler::handle_string(const LSPString *name, const LSPString *value, size_t flags)
        {
            return STATUS_OK;
        }

        status_t IConfigHandler::handle_bool(const LSPString *name, bool value, size_t flags)
        {
            return STATUS_OK;
        }

        status_t IConfigHandler::handle_blob(const LSPString *name, const blob_t *value, size_t flags)
        {
            return STATUS_OK;
        }
    
    } /* namespace config */
} /* namespace lsp */
