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

#include <lsp-plug.in/fmt/sfz/IDocumentHandler.h>

namespace lsp
{
    namespace sfz
    {
        IDocumentHandler::IDocumentHandler()
        {
        }

        IDocumentHandler::~IDocumentHandler()
        {
        }

        status_t IDocumentHandler::begin()
        {
            return STATUS_OK;
        }

        status_t IDocumentHandler::control(const char **opcodes, const char **values)
        {
            return STATUS_OK;
        }

        status_t IDocumentHandler::region(const char **opcodes, const char **values)
        {
            return STATUS_OK;
        }

        status_t IDocumentHandler::sample(
            const char *name, io::IInStream *data,
            const char **opcodes, const char **values)
        {
            return STATUS_OK;
        }

        status_t IDocumentHandler::effect(const char **opcodes, const char **values)
        {
            return STATUS_OK;
        }

        status_t IDocumentHandler::curve(const char **opcodes, const char **values)
        {
            return STATUS_OK;
        }

        status_t IDocumentHandler::midi(const char **opcodes, const char **values)
        {
            return STATUS_OK;
        }

        status_t IDocumentHandler::custom_header(const char *name, const char **opcodes, const char **values)
        {
            return STATUS_OK;
        }

        status_t IDocumentHandler::include(PullParser *parser, const char *name)
        {
            return STATUS_OK;
        }

        const char *IDocumentHandler::root_file_name()
        {
            return NULL;
        }

        status_t IDocumentHandler::end(status_t result)
        {
            return STATUS_OK;
        }

    } /* namespace sfz */
} /* namespace lsp */


