/*
 * Copyright (C) 2026 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2026 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 25 янв. 2026 г.
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

#include <lsp-plug.in/mm/IColorMap.h>

namespace lsp
{
    namespace mm
    {
        IColorMap::IColorMap()
        {
        }

        IColorMap::~IColorMap()
        {
            vNested.flush();
        }

        bool IColorMap::resolve(Color & c, const char *name) const noexcept
        {
            if (name == NULL)
                return false;

            for (lltl::iterator<const IColorMap> it = vNested.values(); it; ++it)
            {
                const IColorMap *map = it.get();
                if ((map != NULL) && (map->resolve(c, name)))
                    return true;
            }
            return false;
        }

        bool IColorMap::resolve(Color & c, const LSPString & name) const noexcept
        {
            for (lltl::iterator<const IColorMap> it = vNested.values(); it; ++it)
            {
                const IColorMap *map = it.get();
                if ((map != NULL) && (map->resolve(c, name)))
                    return true;
            }
            return false;
        }

    } /* namespace mm */
} /* namespace lsp */
