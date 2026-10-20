/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 23 окт. 2020 г.
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

#include <lsp-plug.in/resource/DirLoader.h>

namespace lsp
{
    namespace resource
    {
        DirLoader::DirLoader()
        {
            bEnforce        = true;
        }

        DirLoader::~DirLoader()
        {
        }

        status_t DirLoader::set_path(const char *path)
        {
            return sPath.set(path);
        }

        status_t DirLoader::set_path(const LSPString *path)
        {
            return sPath.set(path);
        }

        status_t DirLoader::set_path(const io::Path *path)
        {
            return sPath.set(path);
        }

        bool DirLoader::set_enforce(bool enforce)
        {
            bool old = bEnforce;
            bEnforce = enforce;
            return old;
        }

        status_t DirLoader::build_path(io::Path *dst, const io::Path *name)
        {
            status_t res = dst->set(name);
            if (res == STATUS_OK)
                res     = dst->canonicalize();
            if (res == STATUS_OK)
                res     = dst->remove_root();
            if (res == STATUS_OK)
                res     = dst->set_parent(&sPath);
            return res;
        }

        io::IInStream *DirLoader::read_stream(const io::Path *name)
        {
            if (!bEnforce)
                return ILoader::read_stream(name);

            io::Path tmp;
            nError  = build_path(&tmp, name);

            return (nError == STATUS_OK) ? ILoader::read_stream(&tmp) : NULL;
        }

        ssize_t DirLoader::enumerate(const io::Path *path, resource_t **list)
        {
            if (!bEnforce)
                return ILoader::enumerate(path, list);

            io::Path tmp;
            nError  = build_path(&tmp, path);

            return (nError == STATUS_OK) ? ILoader::enumerate(&tmp, list) : -set_error(nError);
        }

    } /* namespace resource */
} /* namespace lsp */


