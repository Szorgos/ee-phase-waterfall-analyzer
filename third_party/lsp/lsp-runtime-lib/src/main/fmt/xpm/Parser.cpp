/*
 * Copyright (C) 2026 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2026 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 20 янв. 2026 г.
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

#include <lsp-plug.in/fmt/xpm/Parser.h>

namespace lsp
{
    namespace xpm
    {
        Parser::Parser()
        {
        }

        Parser::~Parser()
        {
        }

        status_t Parser::read_header(header_t *dst)
        {
            return STATUS_NOT_IMPLEMENTED;
        }

        status_t Parser::read_color(Color *dst)
        {
            return STATUS_NOT_IMPLEMENTED;
        }

        status_t Parser::read_line(char *dst)
        {
            return STATUS_NOT_IMPLEMENTED;
        }

        status_t Parser::read_ext(Extension *dst)
        {
            return STATUS_NOT_IMPLEMENTED;
        }

        status_t Parser::close()
        {
            return STATUS_NOT_IMPLEMENTED;
        }

    } /* namespace xpm */
} /* namespace lsp */
