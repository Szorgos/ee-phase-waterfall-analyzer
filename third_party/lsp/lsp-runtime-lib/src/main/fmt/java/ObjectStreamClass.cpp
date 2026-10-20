/*
 * Copyright (C) 2020 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2020 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 29 авг. 2019 г.
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

#include <lsp-plug.in/fmt/java/defs.h>
#include <lsp-plug.in/fmt/java/ObjectStreamClass.h>

namespace lsp
{
    namespace java
    {
        const char *ObjectStreamClass::CLASS_NAME  = "java.io.ObjectStreamClass";
        
        ObjectStreamClass::ObjectStreamClass(): Object(CLASS_NAME)
        {
            pParent     = NULL;
            pRawName    = NULL;
            nSuid       = 0;
            nFlags      = 0;
            nFields     = 0;
            nClasses      = 0;
            nSizeOf     = 0;
            vFields     = NULL;
            vClasses      = NULL;
        }
        
        ObjectStreamClass::~ObjectStreamClass()
        {
            if (vFields != NULL)
            {
                for (size_t i=0; i<nFields; ++i)
                {
                    if (vFields[i] != NULL)
                    {
                        delete vFields[i];
                        vFields[i] = NULL;
                    }
                }
                ::free(vFields);
                vFields = NULL;
            }
            if (vClasses != NULL)
            {
                for (size_t i=0; i<nClasses; ++i)
                    if (vClasses[i] != NULL)
                        vClasses[i] = NULL;
                ::free(vClasses);
                vClasses = NULL;
            }
            if (pRawName != NULL)
            {
                ::free(pRawName);
                pRawName = NULL;
            }
        }
    
    } /* namespace java */
} /* namespace lsp */
