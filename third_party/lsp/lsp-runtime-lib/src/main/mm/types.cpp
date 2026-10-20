/*
 * Copyright (C) 2025 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2025 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 16 апр. 2020 г.
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

#include <lsp-plug.in/mm/types.h>

namespace lsp
{
    namespace mm
    {
        size_t sformat_size_of(size_t fmt)
        {
            switch (sformat_format(fmt))
            {
                case SFMT_U8: case SFMT_S8: return sizeof(uint8_t);
                case SFMT_U16: case SFMT_S16: return sizeof(uint16_t);
                case SFMT_U24: case SFMT_S24:
                #if defined(PLATFORM_WINDOWS)
                    return sizeof(uint8_t) * 3;
                #elif defined(PLATFORM_MACOSX)
                    return sizeof(uint8_t) * 3;
                #else
                    return sizeof(uint32_t);
                #endif
                case SFMT_U32: case SFMT_S32:
                    return sizeof(uint32_t);
                case SFMT_F32:
                    return sizeof(f32_t);
                case SFMT_F64:
                    return sizeof(f64_t);
                default:
                    break;
            }

            return 0;
        }

        int sformat_sign(size_t fmt)
        {
            switch (sformat_format(fmt))
            {
                case SFMT_U8:
                case SFMT_U16:
                case SFMT_U24:
                case SFMT_U32:
                    return 0;

                case SFMT_S8:
                case SFMT_S16:
                case SFMT_S24:
                case SFMT_S32:
                case SFMT_F32:
                case SFMT_F64:
                    return 1;

                default:
                    break;
            }
            return -1;
        }

        ssize_t sformat_signed(size_t fmt)
        {
            size_t endian = fmt & SFMT_EMASK;

            switch (sformat_format(fmt))
            {
                case SFMT_U8:   return SFMT_S8  | endian;
                case SFMT_U16:  return SFMT_S16 | endian;
                case SFMT_U24:  return SFMT_S24 | endian;
                case SFMT_U32:  return SFMT_S32 | endian;

                case SFMT_S8:
                case SFMT_S16:
                case SFMT_S24:
                case SFMT_S32:
                case SFMT_F32:
                case SFMT_F64:
                    return fmt;

                default:
                    break;
            }
            return -1;
        }

        ssize_t sformat_unsigned(size_t fmt)
        {
            size_t endian = fmt & SFMT_EMASK;

            switch (sformat_format(fmt))
            {
                case SFMT_S8:   return SFMT_U8  | endian;
                case SFMT_S16:  return SFMT_U16 | endian;
                case SFMT_S24:  return SFMT_U24 | endian;
                case SFMT_S32:  return SFMT_U32 | endian;

                case SFMT_U8:
                case SFMT_U16:
                case SFMT_U24:
                case SFMT_U32:
                case SFMT_F32:
                case SFMT_F64:
                    return fmt;

                default:
                    break;
            }
            return -1;
        }
    } /* namespace mm */
} /* namespace lsp */


