/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 21 апр. 2017 г.
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

#include <lsp-plug.in/fmt/obj/IObjHandler.h>

namespace lsp
{
    namespace obj
    {
        IObjHandler::~IObjHandler()
        {
        }

        status_t IObjHandler::begin_object(const char *name)
        {
            return STATUS_OK;
        }

        status_t IObjHandler::begin_object(const LSPString *name)
        {
            return begin_object(name->get_utf8());
        }

        status_t IObjHandler::end_object()
        {
            return STATUS_OK;
        }

        status_t IObjHandler::end_of_data()
        {
            return STATUS_OK;
        }

        ssize_t IObjHandler::add_vertex(float x, float y, float z, float w)
        {
            return 0;
        }

        ssize_t IObjHandler::add_param_vertex(float x, float y, float z, float w)
        {
            return 0;
        }

        ssize_t IObjHandler::add_normal(float dx, float dy, float dz, float dn)
        {
            return 0;
        }

        ssize_t IObjHandler::add_texture_vertex(float u, float v, float w)
        {
            return 0;
        }

        ssize_t IObjHandler::add_face(const index_t *vv, const index_t *vn, const index_t *vt, size_t n)
        {
            return 0;
        }

        ssize_t IObjHandler::add_points(const index_t *vv, size_t n)
        {
            return 0;
        }

        ssize_t IObjHandler::add_line(const index_t *vv, const index_t *vt, size_t n)
        {
            return 0;
        }

    } /* namespace obj */
} /* namespace lsp */
