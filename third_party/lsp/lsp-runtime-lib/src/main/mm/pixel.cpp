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

#include <lsp-plug.in/mm/pixel.h>
#include <lsp-plug.in/stdlib/string.h>

namespace lsp
{
    namespace mm
    {
        size_t  bits_per_pixel(pixel_format_t format) noexcept
        {
            switch (format)
            {
                case PIXFMT_G1:
                case PIXFMT_A1:
                    return 1;
                case PIXFMT_G2:
                case PIXFMT_A2:
                    return 2;
                case PIXFMT_G4:
                case PIXFMT_A4:
                    return 4;
                case PIXFMT_G8:
                case PIXFMT_A8:
                    return 8;
                case PIXFMT_RGB888:
                case PIXFMT_BGR888:
                    return 24;
                case PIXFMT_RGBA8888:
                case PIXFMT_BGRA8888:
                case PIXFMT_PRGBA8888:
                case PIXFMT_PBGRA8888:
                    return 32;
                default: break;
            }
            return 0;
        }

        size_t  device_bits_per_pixel(pixel_format_t format) noexcept
        {
            switch (format)
            {
                case PIXFMT_G1:
                case PIXFMT_A1:
                    return 1;
                case PIXFMT_G2:
                case PIXFMT_A2:
                    return 2;
                case PIXFMT_G4:
                case PIXFMT_A4:
                    return 4;
                case PIXFMT_G8:
                case PIXFMT_A8:
                    return 8;
                case PIXFMT_RGB888:
                case PIXFMT_BGR888:
                    return 24;
                case PIXFMT_RGBA8888:
                case PIXFMT_BGRA8888:
                case PIXFMT_PRGBA8888:
                case PIXFMT_PBGRA8888:
                    return 32;
                default: break;
            }
            return 0;
        }

        const char *pixel_format_name(pixel_format_t format) noexcept
        {
            switch (format)
            {
                case PIXFMT_DEFAULT:    return "default";
                case PIXFMT_G1:         return "G1";
                case PIXFMT_G2:         return "G2";
                case PIXFMT_G4:         return "G4";
                case PIXFMT_G8:         return "G8";
                case PIXFMT_A1:         return "A1";
                case PIXFMT_A2:         return "A2";
                case PIXFMT_A4:         return "A4";
                case PIXFMT_A8:         return "A8";
                case PIXFMT_RGB888:     return "RGB888";
                case PIXFMT_BGR888:     return "RGB888";
                case PIXFMT_RGBA8888:   return "RGBA8888";
                case PIXFMT_BGRA8888:   return "BGRA8888";
                case PIXFMT_PRGBA8888:  return "PRGBA8888";
                case PIXFMT_PBGRA8888:  return "PBGRA8888";
                default: break;
            }
            return "unknown";
        }

        bool is_exact_copy_of_pixel(pixel_format_t dst, pixel_format_t src) noexcept
        {
            if (dst == src)
                return true;
            switch (dst)
            {
                case PIXFMT_G1:         return src == PIXFMT_A1;
                case PIXFMT_G2:         return src == PIXFMT_A2;
                case PIXFMT_G4:         return src == PIXFMT_A4;
                case PIXFMT_G8:         return src == PIXFMT_A8;
                case PIXFMT_A1:         return src == PIXFMT_G1;
                case PIXFMT_A2:         return src == PIXFMT_G2;
                case PIXFMT_A4:         return src == PIXFMT_G4;
                case PIXFMT_A8:         return src == PIXFMT_G8;
                default:
                    break;
            }
            return false;
        }

        static const uint8_t b1tob2_table[] =
        {
            0x00, 0x03, 0x0c, 0x0f,
            0x30, 0x33, 0x3c, 0x3f,
            0xc0, 0xc3, 0xcc, 0xcf,
            0xf0, 0xf3, 0xfc, 0xff,
        };

        static void convert_b1_to_b2(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v;
            for ( ; count >= 8; count -= 8)
            {
                v           = *(src++);
                dst[0]      = b1tob2_table[v & 0x0f];
                dst[1]      = b1tob2_table[v >> 4];
                dst        += 2;
            }
            if (count <= 0)
                return;

            v           = *src;
            if (count >= 4)
            {
                *dst++      = b1tob2_table[v & 0x0f];
                v         >>= 4;
                count      -= 4;
            }
            if (count > 0)
                *dst        = b1tob2_table[v & 0x0f] & uint8_t((1 << (count*2)) - 1);
        }

        static const uint8_t b1tob4_table[] =
        {
            0x00, 0x0f, 0xf0, 0xff
        };

        static void convert_b1_to_b4(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v;
            for ( ; count >= 8; count -= 8)
            {
                v           = *(src++);
                dst[0]      = b1tob4_table[v & 0x03];
                dst[1]      = b1tob4_table[(v >> 2) & 0x03];
                dst[2]      = b1tob4_table[(v >> 4) & 0x03];
                dst[3]      = b1tob4_table[(v >> 6) & 0x03];
                dst        += 4;
            }
            if (count <= 0)
                return;

            v           = *src;
            if (count >= 4)
            {
                dst[0]      = b1tob4_table[v & 0x03];
                dst[1]      = b1tob4_table[(v >> 2) & 0x03];
                v         >>= 4;
                dst        += 2;
                count      -= 4;
            }
            if (count >= 2)
            {
                dst[0]      = b1tob4_table[v & 0x03];
                v         >>= 2;
                ++dst;
                count      -= 2;
            }
            if (count > 0)
                dst[0]      = b1tob4_table[v & 0x03] & 0x0f;
        }

        static void convert_b1_to_b8(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v;
            for ( ; count >= 8; count -= 8)
            {
                v           = *(src++);
                dst[0]      = (~(v & 1)) + 1;
                dst[1]      = (~((v >> 1) & 1) + 1);
                dst[2]      = (~((v >> 2) & 1) + 1);
                dst[3]      = (~((v >> 3) & 1) + 1);
                dst[4]      = (~((v >> 4) & 1) + 1);
                dst[5]      = (~((v >> 5) & 1) + 1);
                dst[6]      = (~((v >> 6) & 1) + 1);
                dst[7]      = (~((v >> 7) & 1) + 1);
                dst        += 8;
            }
            if (count <= 0)
                return;

            v           = *src;
            for (; count > 0; --count)
            {
                *(dst++)    = (~(v & 1)) + 1;
                v         >>= 1;
            }
        }

        static void convert_b1_to_rgb888(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v, c;
            for (; count >= 8; count -= 8)
            {
                v           = *(src++);
                for (size_t j=0; j<8; ++j, v >>= 1)
                {
                    c           = ~(v & 1) + 1;
                    dst[0]      = c;
                    dst[1]      = c;
                    dst[2]      = c;
                    dst        += 3;
                }
            }

            if (count <= 0)
                return;

            v           = *(src++);
            for ( ; count > 0 ; --count, v >>= 1)
            {
                c           = ~(v & 1) + 1;
                dst[0]      = c;
                dst[1]      = c;
                dst[2]      = c;
                dst        += 3;
            }
        }

        static void convert_g1_to_rgba8888(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v, c;

            for (; count >= 8; count -= 8)
            {
                v           = *(src++);
                for (size_t j=0; j<8; ++j, v >>= 1)
                {
                    c           = ~(v & 1) + 1;
                    dst[0]      = c;
                    dst[1]      = c;
                    dst[2]      = c;
                    dst[3]      = 0xff;
                    dst        += 4;
                }
            }

            if (count <= 0)
                return;

            v           = *(src++);
            for ( ; count > 0 ; --count, v >>= 1)
            {
                c           = ~(v & 1) + 1;
                dst[0]      = c;
                dst[1]      = c;
                dst[2]      = c;
                dst[3]      = 0xff;
                dst        += 4;
            }
        }

        static void convert_a1_to_rgba8888(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v;
            for (; count >= 8; count -= 8)
            {
                v           = *(src++);
                for (size_t j=0; j<8; ++j, v >>= 1)
                {
                    dst[0]      = 0xff;
                    dst[1]      = 0xff;
                    dst[2]      = 0xff;
                    dst[3]      = ~(v & 1) + 1;

                    dst        += 4;
                }
            }

            if (count <= 0)
                return;

            v           = *(src++);
            for ( ; count > 0 ; --count, v >>= 1)
            {
                dst[0]      = 0xff;
                dst[1]      = 0xff;
                dst[2]      = 0xff;
                dst[3]      = ~(v & 1) + 1;
                dst        += 4;
            }
        }

        static void convert_b1_to_prgba8888(uint8_t *dptr, const uint8_t *src, size_t count)
        {
            uint32_t v;
            uint32_t *dst       = reinterpret_cast<uint32_t *>(dptr);

            for (; count >= 8; count -= 8)
            {
                v           = *(src++);
                for (size_t j=0; j<8; ++j, v >>= 1)
                    *(dst++)        = ~(v & 1) + 1;
            }

            if (count <= 0)
                return;

            v           = *(src++);
            for ( ; count > 0 ; --count, v >>= 1)
                *(dst++)        = ~(v & 1) + 1;
        }

        static pixel_conversion_t pixel_convert_function_for_g1(pixel_format_t dst_fmt) noexcept
        {
            switch (dst_fmt)
            {
                case PIXFMT_G2:
                case PIXFMT_A2:
                    return convert_b1_to_b2;
                case PIXFMT_G4:
                case PIXFMT_A4:
                    return convert_b1_to_b4;
                case PIXFMT_G8:
                case PIXFMT_A8:
                    return convert_b1_to_b8;
                case PIXFMT_RGB888:
                case PIXFMT_BGR888:
                    return convert_b1_to_rgb888;
                case PIXFMT_RGBA8888:
                case PIXFMT_BGRA8888:
                    return convert_g1_to_rgba8888;
                case PIXFMT_PRGBA8888:
                case PIXFMT_PBGRA8888:
                    return convert_b1_to_prgba8888;

                default:
                    break;
            }
            return NULL;
        }

        static pixel_conversion_t pixel_convert_function_for_a1(pixel_format_t dst_fmt) noexcept
        {
            switch (dst_fmt)
            {
                case PIXFMT_G2:
                case PIXFMT_A2:
                    return convert_b1_to_b2;
                case PIXFMT_G4:
                case PIXFMT_A4:
                    return convert_b1_to_b4;
                case PIXFMT_G8:
                case PIXFMT_A8:
                    return convert_b1_to_b8;
                case PIXFMT_RGB888:
                case PIXFMT_BGR888:
                    return convert_b1_to_rgb888;
                case PIXFMT_RGBA8888:
                case PIXFMT_BGRA8888:
                    return convert_a1_to_rgba8888;
                case PIXFMT_PRGBA8888:
                case PIXFMT_PBGRA8888:
                    return convert_b1_to_prgba8888;

                default:
                    break;
            }
            return NULL;
        }

        static void convert_b2_to_b1(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint16_t v;
            for ( ; count >= 8; count -= 8)
            {
                v           = src[0] | (uint16_t(src[1]) << 8);
                v           = ((v & 0x8888) >> 2) | ((v & 0x2222) >> 1);
                v           = (v & 0x0303) | ((v & 0x3030) >> 2);
                *(dst++)    = uint8_t((v & 0x0f) | (v >> 4));
                src        += 2;
            }
            if (count <= 0)
                return;

            v           = src[0];
            if (count >= 4)
                v          |= (uint16_t(src[1]) << 8);

            uint16_t c  = 0;
            for (size_t i=0; i<count; ++i)
                c          |= ((v >> (i*2 + 1)) & 1) << i;

            *dst        = uint8_t(c);
        }

        static const uint8_t b2tob4_table[] =
        {
            0x00, 0x05, 0x0a, 0x0f,
            0x50, 0x55, 0x5a, 0x5f,
            0xa0, 0xa5, 0xaa, 0xaf,
            0xf0, 0xf5, 0xfa, 0xff,
        };

        static void convert_b2_to_b4(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v;
            for ( ; count >= 4; count -= 4)
            {
                v           = *(src++);
                dst[0]      = b2tob4_table[v & 0x0f];
                dst[1]      = b2tob4_table[v >> 4];
                dst        += 2;
            }
            if (count <= 0)
                return;

            v           = *src;
            if (count >= 2)
            {
                *(dst++)    = b2tob4_table[v & 0x0f];
                count      -= 2;
                v         >>= 4;
            }

            if (count > 0)
                *dst        = b2tob4_table[v & 0x03];
        }

        static const uint8_t b2tob8_table[] =
        {
            0x00, 0x55, 0xaa, 0xff
        };

        static void convert_b2_to_b8(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v;
            for ( ; count >= 4; count -= 4)
            {
                v           = *(src++);
                dst[0]      = b2tob8_table[v & 0x03];
                dst[1]      = b2tob8_table[(v >> 2) & 0x03];
                dst[2]      = b2tob8_table[(v >> 4) & 0x03];
                dst[3]      = b2tob8_table[(v >> 6) & 0x03];
                dst        += 4;
            }
            if (count <= 0)
                return;

            v           = *(src++);
            for ( ; count > 0; --count)
            {
                *(dst++)    = b2tob8_table[v & 0x03];
                v         >>= 2;
            }
        }

        static void convert_b2_to_r8b8g8(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v, c;
            for (; count >= 4; count -= 4)
            {
                v           = *(src++);

                c           = b2tob8_table[v & 0x03];
                dst[0]      = c;
                dst[1]      = c;
                dst[2]      = c;

                c           = b2tob8_table[(v >> 2) & 0x03];
                dst[3]      = c;
                dst[4]      = c;
                dst[5]      = c;

                c           = b2tob8_table[(v >> 4) & 0x03];
                dst[6]      = c;
                dst[7]      = c;
                dst[8]      = c;

                c           = b2tob8_table[v >> 6];
                dst[9]      = c;
                dst[10]     = c;
                dst[11]     = c;

                dst        += 12;
            }

            if (count <= 0)
                return;

            v           = *src;
            for ( ; count > 0 ; --count)
            {
                c           = b2tob8_table[v & 0x03];
                dst[0]      = c;
                dst[1]      = c;
                dst[2]      = c;
                dst        += 3;
                v         >>= 2;
            }
        }

        static const uint32_t g2tor8b8g8a8_table[] =
        {
            __IF_LE(0xff000000, 0xff555555, 0xffaaaaaa, 0xffffffff,)
            __IF_BE(0x000000ff, 0x555555ff, 0xaaaaaaff, 0xffffffff,)
        };

        static void convert_g2_to_r8b8g8a8(uint8_t *dptr, const uint8_t *src, size_t count)
        {
            uint8_t v;
            uint32_t *dst   = reinterpret_cast<uint32_t *>(dptr);
            for (; count >= 4; count -= 4)
            {
                v           = *(src++);
                dst[0]      = g2tor8b8g8a8_table[v & 0x03];
                dst[1]      = g2tor8b8g8a8_table[(v >> 2) & 0x03];
                dst[2]      = g2tor8b8g8a8_table[(v >> 4) & 0x03];
                dst[3]      = g2tor8b8g8a8_table[v >> 6];
                dst        += 4;
            }

            if (count <= 0)
                return;

            v           = *src;
            for ( ; count > 0 ; --count)
            {
                *(dst++)    = g2tor8b8g8a8_table[v & 0x03];
                v         >>= 2;
            }
        }

        static const uint32_t a2tor8b8g8a8_table[] =
        {
            __IF_LE(0x00ffffff, 0x55ffffff, 0xaaffffff, 0xffffffff,)
            __IF_BE(0xffffff00, 0xffffff55, 0xffffffaa, 0xffffffff,)
        };

        static void convert_a2_to_r8b8g8a8(uint8_t *dptr, const uint8_t *src, size_t count)
        {
            uint8_t v;
            uint32_t *dst   = reinterpret_cast<uint32_t *>(dptr);
            for (; count >= 4; count -= 4)
            {
                v           = *(src++);
                dst[0]      = a2tor8b8g8a8_table[v & 0x03];
                dst[1]      = a2tor8b8g8a8_table[(v >> 2) & 0x03];
                dst[2]      = a2tor8b8g8a8_table[(v >> 4) & 0x03];
                dst[3]      = a2tor8b8g8a8_table[v >> 6];
                dst        += 4;
            }

            if (count <= 0)
                return;

            v           = *src;
            for ( ; count > 0 ; --count)
            {
                *(dst++)    = a2tor8b8g8a8_table[v & 0x03];
                v         >>= 2;
            }
        }

        static const uint32_t a2topr8b8g8a8_table[] =
        {
            0x00000000, 0x55555555, 0xaaaaaaaa, 0xffffffff,
        };

        static void convert_a2_to_pr8b8g8a8(uint8_t *dptr, const uint8_t *src, size_t count)
        {
            uint8_t v;
            uint32_t *dst   = reinterpret_cast<uint32_t *>(dptr);
            for (; count >= 4; count -= 4)
            {
                v           = *(src++);
                dst[0]      = a2topr8b8g8a8_table[v & 0x03];
                dst[1]      = a2topr8b8g8a8_table[(v >> 2) & 0x03];
                dst[2]      = a2topr8b8g8a8_table[(v >> 4) & 0x03];
                dst[3]      = a2topr8b8g8a8_table[v >> 6];
                dst        += 4;
            }

            if (count <= 0)
                return;

            v           = *src;
            for ( ; count > 0 ; --count)
            {
                *(dst++)    = a2topr8b8g8a8_table[v & 0x03];
                v         >>= 2;
            }
        }

        static pixel_conversion_t pixel_convert_function_for_g2(pixel_format_t dst_fmt) noexcept
        {
            switch (dst_fmt)
            {
                case PIXFMT_G1:
                case PIXFMT_A1:
                    return convert_b2_to_b1;
                case PIXFMT_G4:
                case PIXFMT_A4:
                    return convert_b2_to_b4;
                case PIXFMT_G8:
                case PIXFMT_A8:
                    return convert_b2_to_b8;
                case PIXFMT_RGB888:
                case PIXFMT_BGR888:
                    return convert_b2_to_r8b8g8;
                case PIXFMT_RGBA8888:
                case PIXFMT_BGRA8888:
                case PIXFMT_PRGBA8888:
                case PIXFMT_PBGRA8888:
                    return convert_g2_to_r8b8g8a8;

                default:
                    break;
            }
            return NULL;
        }

        static pixel_conversion_t pixel_convert_function_for_a2(pixel_format_t dst_fmt) noexcept
        {
            switch (dst_fmt)
            {
                case PIXFMT_G1:
                case PIXFMT_A1:
                    return convert_b2_to_b1;
                case PIXFMT_G4:
                case PIXFMT_A4:
                    return convert_b2_to_b4;
                case PIXFMT_G8:
                case PIXFMT_A8:
                    return convert_b2_to_b8;
                case PIXFMT_RGB888:
                case PIXFMT_BGR888:
                    return convert_b2_to_r8b8g8;
                case PIXFMT_RGBA8888:
                case PIXFMT_BGRA8888:
                    return convert_a2_to_r8b8g8a8;
                case PIXFMT_PRGBA8888:
                case PIXFMT_PBGRA8888:
                    return convert_a2_to_pr8b8g8a8;

                default:
                    break;
            }
            return NULL;
        }

        static void convert_b4_to_b1(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v, a, b, c, d;
            for ( ; count >= 8; count -= 8)
            {
                a           = src[0];
                b           = src[1];
                c           = src[2];
                d           = src[3];

                *(dst++)    =
                    ((a & 0x08) >> 3) | ((a & 0x80) >> 6) |
                    ((b & 0x08) >> 1) | ((b & 0x80) >> 4) |
                    ((c & 0x08) << 1) | ((c & 0x80) >> 2) |
                    ((d & 0x08) << 3) | (d & 0x80);

                src        += 4;
            }
            if (count <= 0)
                return;

            v           = 0;
            d           = 0;
            if (count >= 4)
            {
                a           = src[0];
                b           = src[1];
                v          |=
                    ((a & 0x08) >> 3) | ((a & 0x80) >> 6) |
                    ((b & 0x08) >> 1) | ((b & 0x80) >> 4);
                count      -= 4;
                d          += 4;
                src        += 2;
            }
            if (count >= 2)
            {
                a           = *(src++);
                v          |= (((a & 0x08) >> 3) | ((a & 0x80) >> 6)) << d;
                count      -= 2;
                d          += 2;
            }
            if (count >= 1)
            {
                a           = *src;
                v          |= ((a & 0x08) >> 3) << d;
            }

            *dst        = v;
        }

        static void convert_b4_to_b2(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v, a, b;
            for ( ; count >= 4; count -= 4)
            {
                a           = src[0];
                b           = src[1];

                *(dst++)    =
                    ((a & 0x0c) >> 2) | ((a & 0xc0) >> 4) |
                    ((b & 0x0c) << 2) | (b & 0xc0);

                src        += 2;
            }
            if (count <= 0)
                return;

            v           = 0;
            b           = 0;
            if (count >= 2)
            {
                a           = *(src++);
                v          |= ((a & 0x0c) >> 2) | ((a & 0xc0) >> 4);
                b          += 4;
                count      -= 2;
            }
            if (count >= 1)
            {
                a           = *src;
                v          |= ((a & 0x0c) >> 2) << b;
            }

            *dst        = v;
        }

        static const uint8_t b4tob8_table[] =
        {
            0x00, 0x11, 0x22, 0x33,
            0x44, 0x55, 0x66, 0x77,
            0x88, 0x99, 0xaa, 0xbb,
            0xcc, 0xdd, 0xee, 0xff
        };

        static void convert_b4_to_b8(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v;
            for ( ; count >= 2; count -= 2)
            {
                v           = *(src++);
                dst[0]      = b4tob8_table[v & 0x0f];
                dst[1]      = b4tob8_table[v >> 4];
                dst        += 2;
            }
            if (count <= 0)
                return;

            v           = *src;
            *dst        = b4tob8_table[v & 0x0f];
        }

        static void convert_b4_to_rgb888(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v, c1, c2;
            for (; count >= 2; count -= 2)
            {
                v           = *(src++);
                c1          = b4tob8_table[v & 0x0f];
                c2          = b4tob8_table[v >> 4];
                dst[0]      = c1;
                dst[1]      = c1;
                dst[2]      = c1;
                dst[3]      = c2;
                dst[4]      = c2;
                dst[5]      = c2;
                dst        += 6;
            }

            if (count <= 0)
                return;

            v           = *src++;
            c1          = b4tob8_table[v & 0x0f];
            dst[0]      = c1;
            dst[1]      = c1;
            dst[2]      = c1;
        }

        static const uint32_t g4tor8b8g8a8_table[] =
        {
            __IF_LE(
                0xff000000, 0xff111111, 0xff222222, 0xff333333,
                0xff444444, 0xff555555, 0xff666666, 0xff777777,
                0xff888888, 0xff999999, 0xffaaaaaa, 0xffbbbbbb,
                0xffcccccc, 0xffdddddd, 0xffeeeeee, 0xffffffff,
            )
            __IF_BE(
                0x000000ff, 0x111111ff, 0x222222ff, 0x333333ff,
                0x444444ff, 0x555555ff, 0x666666ff, 0x777777ff,
                0x888888ff, 0x999999ff, 0xaaaaaaff, 0xbbbbbbff,
                0xccccccff, 0xddddddff, 0xeeeeeeff, 0xffffffff,
            )
        };

        static void convert_g4_to_rgba8888(uint8_t *dptr, const uint8_t *src, size_t count)
        {
            uint8_t v;
            uint32_t *dst   = reinterpret_cast<uint32_t *>(dptr);
            for (; count >= 2; count -= 2)
            {
                v           = *(src++);
                dst[0]      = g4tor8b8g8a8_table[v & 0x0f];
                dst[1]      = g4tor8b8g8a8_table[v >> 4];
                dst        += 2;
            }

            if (count <= 0)
                return;

            v           = *src++;
            dst[0]      = g4tor8b8g8a8_table[v & 0x0f];
        }

        static const uint32_t a4tor8b8g8a8_table[] =
        {
            __IF_LE(
                0x00ffffff, 0x11ffffff, 0x22ffffff, 0x33ffffff,
                0x44ffffff, 0x55ffffff, 0x66ffffff, 0x77ffffff,
                0x88ffffff, 0x99ffffff, 0xaaffffff, 0xbbffffff,
                0xccffffff, 0xddffffff, 0xeeffffff, 0xffffffff,
            )
            __IF_BE(
                0xffffff00, 0xffffff11, 0xffffff22, 0xffffff33,
                0xffffff44, 0xffffff55, 0xffffff66, 0xffffff77,
                0xffffff88, 0xffffff99, 0xffffffaa, 0xffffffbb,
                0xffffffcc, 0xffffffdd, 0xffffffee, 0xffffffff,
            )
        };

        static void convert_a4_to_rgba8888(uint8_t *dptr, const uint8_t *src, size_t count)
        {
            uint8_t v;
            uint32_t *dst   = reinterpret_cast<uint32_t *>(dptr);
            for (; count >= 2; count -= 2)
            {
                v           = *(src++);
                dst[0]      = a4tor8b8g8a8_table[v & 0x0f];
                dst[1]      = a4tor8b8g8a8_table[v >> 4];
                dst        += 2;
            }

            if (count <= 0)
                return;

            v           = *src++;
            dst[0]      = a4tor8b8g8a8_table[v & 0x0f];
        }

        static const uint32_t a4topr8b8g8a8_table[] =
        {
            0x00000000, 0x11111111, 0x22222222, 0x33333333,
            0x44444444, 0x55555555, 0x66666666, 0x77777777,
            0x88888888, 0x99999999, 0xaaaaaaaa, 0xbbbbbbbb,
            0xcccccccc, 0xdddddddd, 0xeeeeeeee, 0xffffffff,
        };

        static void convert_a4_to_prgba8888(uint8_t *dptr, const uint8_t *src, size_t count)
        {
            uint8_t v;
            uint32_t *dst   = reinterpret_cast<uint32_t *>(dptr);
            for (; count >= 2; count -= 2)
            {
                v           = *(src++);
                dst[0]      = a4topr8b8g8a8_table[v & 0x0f];
                dst[1]      = a4topr8b8g8a8_table[v >> 4];
                dst        += 2;
            }

            if (count <= 0)
                return;

            v           = *src++;
            dst[0]      = a4topr8b8g8a8_table[v & 0x0f];
        }

        static pixel_conversion_t pixel_convert_function_for_g4(pixel_format_t dst_fmt) noexcept
        {
            switch (dst_fmt)
            {
                case PIXFMT_G1:
                case PIXFMT_A1:
                    return convert_b4_to_b1;
                case PIXFMT_G2:
                case PIXFMT_A2:
                    return convert_b4_to_b2;
                case PIXFMT_G8:
                case PIXFMT_A8:
                    return convert_b4_to_b8;
                case PIXFMT_RGB888:
                case PIXFMT_BGR888:
                    return convert_b4_to_rgb888;
                case PIXFMT_RGBA8888:
                case PIXFMT_BGRA8888:
                    return convert_g4_to_rgba8888;
                case PIXFMT_PRGBA8888:
                case PIXFMT_PBGRA8888:
                    return convert_g4_to_rgba8888;

                default:
                    break;
            }
            return NULL;
        }

        static pixel_conversion_t pixel_convert_function_for_a4(pixel_format_t dst_fmt) noexcept
        {
            switch (dst_fmt)
            {
                case PIXFMT_G1:
                case PIXFMT_A1:
                    return convert_b4_to_b1;
                case PIXFMT_G2:
                case PIXFMT_A2:
                    return convert_b4_to_b2;
                case PIXFMT_G8:
                case PIXFMT_A8:
                    return convert_b4_to_b8;
                case PIXFMT_RGB888:
                case PIXFMT_BGR888:
                    return convert_b4_to_rgb888;
                case PIXFMT_RGBA8888:
                case PIXFMT_BGRA8888:
                    return convert_a4_to_rgba8888;
                case PIXFMT_PRGBA8888:
                case PIXFMT_PBGRA8888:
                    return convert_a4_to_prgba8888;

                default:
                    break;
            }
            return NULL;
        }

        static void convert_b8_to_b1(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v, c;
            for ( ; count >= 8; count -= 8)
            {
                *(dst++)    =
                    (src[0] >> 7) |
                    ((src[1] & 0x80) >> 6) |
                    ((src[2] & 0x80) >> 5) |
                    ((src[3] & 0x80) >> 4) |
                    ((src[4] & 0x80) >> 3) |
                    ((src[5] & 0x80) >> 2) |
                    ((src[6] & 0x80) >> 1) |
                    (src[7] & 0x80);
                src        += 8;
            }
            if (count <= 0)
                return;

            v           = 0;
            c           = 0;
            if (count >= 4)
            {
                v          |=
                    (src[0] >> 7) |
                    ((src[1] & 0x80) >> 6) |
                    ((src[2] & 0x80) >> 5) |
                    ((src[3] & 0x80) >> 4);
                src        += 4;
                c          += 4;
                count      -= 4;
            }
            if (count >= 2)
            {
                v          |=
                    ((src[0] >> 7) |
                    ((src[1] & 0x80) >> 6)) << c;
                src        += 2;
                c          += 2;
                count      -= 2;
            }
            if (count >= 1)
            {
                v          |= (src[0] >> 7) << c;
            }
            *dst        = v;
        }

        static void convert_b8_to_b2(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v, c;
            for ( ; count >= 4; count -= 4)
            {
                *(dst++)    =
                    (src[0] >> 6) |
                    ((src[1] & 0xc0) >> 4) |
                    ((src[2] & 0xc0) >> 2) |
                    (src[3] & 0xc0);
                src        += 4;
            }
            if (count <= 0)
                return;

            v           = 0;
            c           = 0;
            if (count >= 2)
            {
                v          |=
                    (src[0] >> 6) |
                    ((src[1] & 0xc0) >> 4);
                src        += 2;
                count      -= 2;
                c          += 4;
            }
            if (count >= 1)
                v          |= (src[0] >> 6) << c;
            *dst        = v;
        }

        static void convert_b8_to_b4(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t c;
            for ( ; count >= 2; count -= 2)
            {
                *(dst++)    =
                    (src[0] >> 4) |
                    (src[1] & 0xf0);
                src        += 2;
            }
            if (count <= 0)
                return;

            c           = src[0];
            *dst        = c >> 4;
        }

        static void convert_b8_to_rgb888(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v;
            for (; count > 0; --count)
            {
                v           = *(src++);
                dst[0]      = v;
                dst[1]      = v;
                dst[2]      = v;
                dst        += 3;
            }
        }

        static void convert_g8_to_rgba8888(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v;
            for (; count > 0; --count)
            {
                v           = *(src++);
                dst[0]      = v;
                dst[1]      = v;
                dst[2]      = v;
                dst[3]      = 0xff;
                dst        += 4;
            }
        }

        static void convert_a8_to_rgba8888(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v;
            for (; count > 0; --count)
            {
                v           = *(src++);
                dst[0]      = 0xff;
                dst[1]      = 0xff;
                dst[2]      = 0xff;
                dst[3]      = v;
                dst        += 4;
            }
        }

        static void convert_a8_to_prgba8888(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v;
            for (; count > 0; --count)
            {
                v           = *(src++);
                dst[0]      = v;
                dst[1]      = v;
                dst[2]      = v;
                dst[3]      = v;
                dst        += 4;
            }
        }

        static pixel_conversion_t pixel_convert_function_for_g8(pixel_format_t dst_fmt) noexcept
        {
            switch (dst_fmt)
            {
                case PIXFMT_G1:
                case PIXFMT_A1:
                    return convert_b8_to_b1;
                case PIXFMT_G2:
                case PIXFMT_A2:
                    return convert_b8_to_b2;
                case PIXFMT_G4:
                case PIXFMT_A4:
                    return convert_b8_to_b4;
                case PIXFMT_RGB888:
                case PIXFMT_BGR888:
                    return convert_b8_to_rgb888;
                case PIXFMT_RGBA8888:
                case PIXFMT_BGRA8888:
                case PIXFMT_PRGBA8888:
                case PIXFMT_PBGRA8888:
                    return convert_g8_to_rgba8888;

                default:
                    break;
            }
            return NULL;
        }

        static pixel_conversion_t pixel_convert_function_for_a8(pixel_format_t dst_fmt) noexcept
        {
            switch (dst_fmt)
            {
                case PIXFMT_G1:
                case PIXFMT_A1:
                    return convert_b8_to_b1;
                case PIXFMT_G2:
                case PIXFMT_A2:
                    return convert_b8_to_b2;
                case PIXFMT_G4:
                case PIXFMT_A4:
                    return convert_b8_to_b4;
                case PIXFMT_RGB888:
                case PIXFMT_BGR888:
                    return convert_b8_to_rgb888;
                case PIXFMT_RGBA8888:
                case PIXFMT_BGRA8888:
                    return convert_a8_to_rgba8888;
                case PIXFMT_PRGBA8888:
                case PIXFMT_PBGRA8888:
                    return convert_a8_to_prgba8888;

                default:
                    break;
            }
            return NULL;
        }

        static inline const uint8_t lightness(uint8_t r, uint8_t g, uint8_t b)
        {
            const uint16_t cmax  = lsp_max(r, g, b);
            const uint16_t cmin  = lsp_min(r, g, b);
            return uint8_t((cmax + cmin) >> 1);
        }

        static void convert_rgb888_to_b1(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v;
            for ( ; count >= 8; count -= 8)
            {
                *(dst++)    =
                    (lightness(src[0], src[1], src[2]) >> 7) |
                    ((lightness(src[3], src[4], src[5]) & 0x80) >> 6) |
                    ((lightness(src[6], src[7], src[8]) & 0x80) >> 5) |
                    ((lightness(src[9], src[10], src[11]) & 0x80) >> 4) |
                    ((lightness(src[12], src[13], src[14]) & 0x80) >> 3) |
                    ((lightness(src[15], src[16], src[17]) & 0x80) >> 2) |
                    ((lightness(src[18], src[18], src[20]) & 0x80) >> 1) |
                    (lightness(src[21], src[22], src[23]) & 0x80);
                src        += 24;
            }
            if (count <= 0)
                return;

            v           = 0;
            for (size_t i=0; i<count; ++i)
            {
                v          |= (lightness(src[0], src[1], src[2]) >> 7) << i;
                src        += 3;
            }
            *(dst++)    = v;
        }

        static void convert_rgb888_to_b2(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v;
            for ( ; count >= 4; count -= 4)
            {
                *(dst++)    =
                    (lightness(src[0], src[1], src[2]) >> 6) |
                    ((lightness(src[3], src[4], src[5]) & 0xc0) >> 4) |
                    ((lightness(src[6], src[7], src[8]) & 0xc0) >> 2) |
                    (lightness(src[9], src[10], src[11]) & 0xc0);
                src        += 12;
            }
            if (count <= 0)
                return;

            v           = 0;
            for (size_t i=0; i<count; ++i)
            {
                v          |= (lightness(src[0], src[1], src[2]) >> 6) << (i*2);
                src        += 3;
            }
            *(dst++)    = v;
        }

        static void convert_rgb888_to_b4(uint8_t *dst, const uint8_t *src, size_t count)
        {
            for ( ; count >= 2; count -= 2)
            {
                *(dst++)    =
                    (lightness(src[0], src[1], src[2]) >> 4) |
                    (lightness(src[3], src[4], src[5]) & 0xf0);
                src        += 6;
            }
            if (count <= 0)
                return;

            *dst        = lightness(src[0], src[1], src[2]) >> 4;
        }

        static void convert_rgb888_to_b8(uint8_t *dst, const uint8_t *src, size_t count)
        {
            for ( ; count > 0; --count)
            {
                *(dst++)    = lightness(src[0], src[1], src[2]);
                src        += 3;
            }
        }

        static void convert_rgb888_to_bgr888(uint8_t *dst, const uint8_t *src, size_t count)
        {
            for ( ; count > 0; --count)
            {
                dst[0]      = src[2];
                dst[1]      = src[1];
                dst[2]      = src[0];
                src        += 3;
                dst        += 3;
            }
        }

        static void convert_rgb888_to_rgba8888(uint8_t *dst, const uint8_t *src, size_t count)
        {
            for ( ; count > 0; --count)
            {
                dst[0]      = src[0];
                dst[1]      = src[1];
                dst[2]      = src[2];
                dst[3]      = 0xff;
                src        += 3;
                dst        += 4;
            }
        }

        static void convert_rgb888_to_bgra8888(uint8_t *dst, const uint8_t *src, size_t count)
        {
            for ( ; count > 0; --count)
            {
                dst[0]      = src[2];
                dst[1]      = src[1];
                dst[2]      = src[0];
                dst[3]      = 0xff;
                src        += 3;
                dst        += 4;
            }
        }

        static void convert_bgr888_to_rgba8888(uint8_t *dst, const uint8_t *src, size_t count)
        {
            for ( ; count > 0; --count)
            {
                dst[0]      = src[2];
                dst[1]      = src[1];
                dst[2]      = src[0];
                dst[3]      = 0xff;
                src        += 3;
                dst        += 4;
            }
        }

        static pixel_conversion_t pixel_convert_function_for_rgb888(pixel_format_t dst_fmt) noexcept
        {
            switch (dst_fmt)
            {
                case PIXFMT_G1:
                case PIXFMT_A1:
                    return convert_rgb888_to_b1;
                case PIXFMT_G2:
                case PIXFMT_A2:
                    return convert_rgb888_to_b2;
                case PIXFMT_G4:
                case PIXFMT_A4:
                    return convert_rgb888_to_b4;
                case PIXFMT_G8:
                case PIXFMT_A8:
                    return convert_rgb888_to_b8;
                case PIXFMT_BGR888:
                    return convert_rgb888_to_bgr888;
                case PIXFMT_RGBA8888:
                case PIXFMT_PRGBA8888:
                    return convert_rgb888_to_rgba8888;

                case PIXFMT_BGRA8888:
                case PIXFMT_PBGRA8888:
                    return convert_rgb888_to_bgra8888;

                default:
                    break;
            }
            return NULL;
        }

        static pixel_conversion_t pixel_convert_function_for_bgr888(pixel_format_t dst_fmt) noexcept
        {
            switch (dst_fmt)
            {
                case PIXFMT_G1:
                case PIXFMT_A1:
                    return convert_rgb888_to_b1;
                case PIXFMT_G2:
                case PIXFMT_A2:
                    return convert_rgb888_to_b2;
                case PIXFMT_G4:
                case PIXFMT_A4:
                    return convert_rgb888_to_b4;
                case PIXFMT_G8:
                case PIXFMT_A8:
                    return convert_rgb888_to_b8;
                case PIXFMT_BGR888:
                    return convert_rgb888_to_bgr888;
                case PIXFMT_RGBA8888:
                case PIXFMT_PRGBA8888:
                    return convert_bgr888_to_rgba8888;
                case PIXFMT_BGRA8888:
                case PIXFMT_PBGRA8888:
                    return convert_rgb888_to_rgba8888;

                default:
                    break;
            }
            return NULL;
        }

        static void convert_rgba8888_to_g1(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v;
            for ( ; count >= 8; count -= 8)
            {
                *(dst++)    =
                    (lightness(src[0], src[1], src[2]) >> 7) |
                    ((lightness(src[4], src[5], src[6]) & 0x80) >> 6) |
                    ((lightness(src[8], src[9], src[10]) & 0x80) >> 5) |
                    ((lightness(src[12], src[13], src[14]) & 0x80) >> 4) |
                    ((lightness(src[16], src[17], src[18]) & 0x80) >> 3) |
                    ((lightness(src[20], src[21], src[22]) & 0x80) >> 2) |
                    ((lightness(src[24], src[25], src[26]) & 0x80) >> 1) |
                    (lightness(src[28], src[29], src[30]) & 0x80);
                src        += 32;
            }
            if (count <= 0)
                return;

            v           = 0;
            for (size_t i=0; i<count; ++i)
            {
                v          |= (lightness(src[0], src[1], src[2]) >> 7) << i;
                src        += 4;
            }
            *dst        = v;
        }

        static void convert_rgba8888_to_a1(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v;
            for ( ; count >= 8; count -= 8)
            {
                *(dst++)    =
                    (src[3] >> 7) |
                    ((src[7] & 0x80) >> 6) |
                    ((src[11] & 0x80) >> 5) |
                    ((src[15] & 0x80) >> 4) |
                    ((src[19] & 0x80) >> 3) |
                    ((src[23] & 0x80) >> 2) |
                    ((src[27] & 0x80) >> 1) |
                    (src[31] & 0x80);
                src        += 32;
            }
            if (count <= 0)
                return;

            v           = 0;
            for (size_t i=0; i<count; ++i)
            {
                v          |= (src[3] >> 7) << i;
                src        += 4;
            }
            *dst        = v;
        }

        static void convert_rgba8888_to_g2(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v;
            for ( ; count >= 4; count -= 4)
            {
                *(dst++)    =
                    (lightness(src[0], src[1], src[2]) >> 6) |
                    ((lightness(src[4], src[5], src[6]) & 0xc0) >> 4) |
                    ((lightness(src[8], src[9], src[10]) & 0xc0) >> 2) |
                    (lightness(src[12], src[13], src[14]) & 0xc0);
                src        += 16;
            }
            if (count <= 0)
                return;

            v           = 0;
            for (size_t i=0; i<count; ++i)
            {
                v          |= (lightness(src[0], src[1], src[2]) >> 6) << (i*2);
                src        += 4;
            }
            *dst        = v;
        }

        static void convert_rgba8888_to_a2(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v;
            for ( ; count >= 4; count -= 4)
            {
                *(dst++)    =
                    (src[3] >> 6) |
                    ((src[7] & 0xc0) >> 4) |
                    ((src[11] & 0xc0) >> 2) |
                    (src[15] & 0xc0);
                src        += 16;
            }
            if (count <= 0)
                return;

            v           = 0;
            for (size_t i=0; i<count; ++i)
            {
                v          |= (src[3] >> 6) << (i*2);
                src        += 4;
            }
            *dst        = v;
        }

        static void convert_rgba8888_to_g4(uint8_t *dst, const uint8_t *src, size_t count)
        {
            for ( ; count >= 2; count -= 2)
            {
                *(dst++)    =
                    (lightness(src[0], src[1], src[2]) >> 4) |
                    (lightness(src[4], src[5], src[6]) & 0xf0);
                src        += 8;
            }
            if (count <= 0)
                return;

            *dst        = lightness(src[0], src[1], src[2]) >> 4;
        }

        static void convert_rgba8888_to_a4(uint8_t *dst, const uint8_t *src, size_t count)
        {
            for ( ; count >= 2; count -= 2)
            {
                *(dst++)    =
                    (src[3] >> 4) |
                    (src[7] & 0xf0);
                src        += 8;
            }
            if (count <= 0)
                return;

            *dst        = src[3] >> 4;
        }

        static void convert_rgba8888_to_g8(uint8_t *dst, const uint8_t *src, size_t count)
        {
            for ( ; count > 0; --count)
            {
                *(dst++)    = lightness(src[0], src[1], src[2]);
                src        += 4;
            }
        }

        static void convert_rgba8888_to_a8(uint8_t *dst, const uint8_t *src, size_t count)
        {
            for ( ; count > 0; --count)
            {
                *(dst++)    = src[3];
                src        += 4;
            }
        }

        static void convert_rgba8888_to_rgb888(uint8_t *dst, const uint8_t *src, size_t count)
        {
            for ( ; count > 0; --count)
            {
                dst[0]      = src[0];
                dst[1]      = src[1];
                dst[2]      = src[2];
                src        += 4;
                dst        += 3;
            }
        }

        static void convert_rgba8888_to_bgr888(uint8_t *dst, const uint8_t *src, size_t count)
        {
            for ( ; count > 0; --count)
            {
                dst[0]      = src[2];
                dst[1]      = src[1];
                dst[2]      = src[0];
                src        += 4;
                dst        += 3;
            }
        }

        constexpr uint32_t k_alpha = 0x1011;

        static void convert_rgba8888_to_prgba8888(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint32_t v;
            for ( ; count > 0; --count)
            {
                v           = src[3] * k_alpha;
                dst[0]      = uint8_t((v * src[0]) >> 20);
                dst[1]      = uint8_t((v * src[1]) >> 20);
                dst[2]      = uint8_t((v * src[2]) >> 20);
                dst[3]      = src[3];
                src        += 4;
                dst        += 4;
            }
        }

        static void convert_rgba8888_to_pbgra8888(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint32_t v;
            for ( ; count > 0; --count)
            {
                v           = src[3] * k_alpha;
                dst[0]      = uint8_t((v * src[2]) >> 20);
                dst[1]      = uint8_t((v * src[1]) >> 20);
                dst[2]      = uint8_t((v * src[0]) >> 20);
                dst[3]      = src[3];
                src        += 4;
                dst        += 4;
            }
        }

        static pixel_conversion_t pixel_convert_function_for_rgba8888(pixel_format_t dst_fmt) noexcept
        {
            switch (dst_fmt)
            {
                case PIXFMT_G1:
                    return convert_rgba8888_to_g1;
                case PIXFMT_A1:
                    return convert_rgba8888_to_a1;
                case PIXFMT_G2:
                    return convert_rgba8888_to_g2;
                case PIXFMT_A2:
                    return convert_rgba8888_to_a2;
                case PIXFMT_G4:
                    return convert_rgba8888_to_g4;
                case PIXFMT_A4:
                    return convert_rgba8888_to_a4;
                case PIXFMT_G8:
                    return convert_rgba8888_to_g8;
                case PIXFMT_A8:
                    return convert_rgba8888_to_a8;
                case PIXFMT_RGB888:
                    return convert_rgba8888_to_rgb888;
                case PIXFMT_BGR888:
                    return convert_rgba8888_to_bgr888;
                case PIXFMT_PRGBA8888:
                    return convert_rgba8888_to_prgba8888;
                case PIXFMT_PBGRA8888:
                    return convert_rgba8888_to_pbgra8888;

                default:
                    break;
            }
            return NULL;
        }

        static pixel_conversion_t pixel_convert_function_for_bgra8888(pixel_format_t dst_fmt) noexcept
        {
            switch (dst_fmt)
            {
                case PIXFMT_G1:
                    return convert_rgba8888_to_g1;
                case PIXFMT_A1:
                    return convert_rgba8888_to_a1;
                case PIXFMT_G2:
                    return convert_rgba8888_to_g2;
                case PIXFMT_A2:
                    return convert_rgba8888_to_a2;
                case PIXFMT_G4:
                    return convert_rgba8888_to_g4;
                case PIXFMT_A4:
                    return convert_rgba8888_to_a4;
                case PIXFMT_G8:
                    return convert_rgba8888_to_g8;
                case PIXFMT_A8:
                    return convert_rgba8888_to_a8;
                case PIXFMT_RGB888:
                    return convert_rgba8888_to_bgr888;
                case PIXFMT_BGR888:
                    return convert_rgba8888_to_rgb888;
                case PIXFMT_PRGBA8888:
                    return convert_rgba8888_to_pbgra8888;
                case PIXFMT_PBGRA8888:
                    return convert_rgba8888_to_prgba8888;

                default:
                    break;
            }
            return NULL;
        }

        static inline const uint8_t prgba_lightness(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
        {
            if (a <= 0)
                return 0;

            const uint32_t k    = 0xfffff / a;
            const uint32_t xr   = (r * k) >> 12;
            const uint32_t xg   = (g * k) >> 12;
            const uint32_t xb   = (b * k) >> 12;
            const uint32_t cmax = lsp_max(xr, xg, xb);
            const uint32_t cmin = lsp_min(xr, xg, xb);
            return uint8_t((cmax + cmin) >> 1);
        }

        static void convert_prgba8888_to_g1(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v;
            for ( ; count >= 8; count -= 8)
            {
                *(dst++)    =
                    (prgba_lightness(src[0], src[1], src[2], src[3]) >> 7) |
                    ((prgba_lightness(src[4], src[5], src[6], src[7]) & 0x80) >> 6) |
                    ((prgba_lightness(src[8], src[9], src[10], src[11]) & 0x80) >> 5) |
                    ((prgba_lightness(src[12], src[13], src[14], src[15]) & 0x80) >> 4) |
                    ((prgba_lightness(src[16], src[17], src[18], src[19]) & 0x80) >> 3) |
                    ((prgba_lightness(src[20], src[21], src[22], src[23]) & 0x80) >> 2) |
                    ((prgba_lightness(src[24], src[25], src[26], src[27]) & 0x80) >> 1) |
                    (prgba_lightness(src[28], src[29], src[30], src[31]) & 0x80);
                src        += 32;
            }
            if (count <= 0)
                return;

            v           = 0;
            for (size_t i=0; i<count; ++i)
            {
                v          |= (prgba_lightness(src[0], src[1], src[2], src[3]) >> 7) << i;
                src        += 4;
            }
            *dst        = v;
        }

        static void convert_prgba8888_to_g2(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t v;
            for ( ; count >= 4; count -= 4)
            {
                *(dst++)    =
                    (prgba_lightness(src[0], src[1], src[2], src[3]) >> 6) |
                    ((prgba_lightness(src[4], src[5], src[6], src[7]) & 0xc0) >> 4) |
                    ((prgba_lightness(src[8], src[9], src[10], src[11]) & 0xc0) >> 2) |
                    (prgba_lightness(src[12], src[13], src[14], src[15]) & 0xc0);
                src        += 16;
            }
            if (count <= 0)
                return;

            v           = 0;
            for (size_t i=0; i<count; ++i)
            {
                v          |= (prgba_lightness(src[0], src[1], src[2], src[3]) >> 6) << (i*2);
                src        += 4;
            }
            *dst        = v;
        }

        static void convert_prgba8888_to_g4(uint8_t *dst, const uint8_t *src, size_t count)
        {
            for ( ; count >= 2; count -= 2)
            {
                *(dst++)    =
                    (prgba_lightness(src[0], src[1], src[2], src[3]) >> 4) |
                    (prgba_lightness(src[4], src[5], src[6], src[7]) & 0xf0);
                src        += 8;
            }
            if (count <= 0)
                return;

            *dst        = prgba_lightness(src[0], src[1], src[2], src[3]) >> 4;
        }

        static void convert_prgba8888_to_g8(uint8_t *dst, const uint8_t *src, size_t count)
        {
            for ( ; count > 0; --count)
            {
                *(dst++)    = prgba_lightness(src[0], src[1], src[2], src[3]);
                src        += 4;
            }
        }

        static void convert_prgba8888_to_rgb888(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t a;
            uint32_t k;
            for ( ; count > 0; --count)
            {
                a           = src[3];
                if (a != 0)
                {
                    k           = 0xfffff / a;
                    dst[0]      = (src[0] * k) >> 12;
                    dst[1]      = (src[1] * k) >> 12;
                    dst[2]      = (src[2] * k) >> 12;
                }
                else
                {
                    dst[0]      = 0;
                    dst[1]      = 0;
                    dst[2]      = 0;
                }
                src        += 4;
                dst        += 3;
            }
        }

        static void convert_prgba8888_to_bgr888(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t a;
            uint32_t k;
            for ( ; count > 0; --count)
            {
                a           = src[3];
                if (a != 0)
                {
                    k           = 0xfffff / a;
                    dst[0]      = (src[2] * k) >> 12;
                    dst[1]      = (src[1] * k) >> 12;
                    dst[2]      = (src[0] * k) >> 12;
                }
                else
                {
                    dst[0]      = 0;
                    dst[1]      = 0;
                    dst[2]      = 0;
                }
                src        += 4;
                dst        += 3;
            }
        }

        static void convert_prgba8888_to_rgba8888(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t a;
            uint32_t k;
            for ( ; count > 0; --count)
            {
                a           = src[3];
                if (a != 0)
                {
                    k           = 0xfffff / a;
                    dst[0]      = (src[0] * k) >> 12;
                    dst[1]      = (src[1] * k) >> 12;
                    dst[2]      = (src[2] * k) >> 12;
                }
                else
                {
                    dst[0]      = 0;
                    dst[1]      = 0;
                    dst[2]      = 0;
                }
                dst[3]      = a;
                src        += 4;
                dst        += 4;
            }
        }

        static void convert_prgba8888_to_bgra8888(uint8_t *dst, const uint8_t *src, size_t count)
        {
            uint8_t a;
            uint32_t k;
            for ( ; count > 0; --count)
            {
                a           = src[3];
                if (a != 0)
                {
                    k           = 0xfffff / a;
                    dst[0]      = (src[2] * k) >> 12;
                    dst[1]      = (src[1] * k) >> 12;
                    dst[2]      = (src[0] * k) >> 12;
                }
                else
                {
                    dst[0]      = 0;
                    dst[1]      = 0;
                    dst[2]      = 0;
                }
                dst[3]      = a;
                src        += 4;
                dst        += 4;
            }
        }

        static pixel_conversion_t pixel_convert_function_for_prgba8888(pixel_format_t dst_fmt) noexcept
        {
            switch (dst_fmt)
            {
                case PIXFMT_G1:
                    return convert_prgba8888_to_g1;
                case PIXFMT_A1:
                    return convert_rgba8888_to_a1;
                case PIXFMT_G2:
                    return convert_prgba8888_to_g2;
                case PIXFMT_A2:
                    return convert_rgba8888_to_a2;
                case PIXFMT_G4:
                    return convert_prgba8888_to_g4;
                case PIXFMT_A4:
                    return convert_rgba8888_to_a4;
                case PIXFMT_G8:
                    return convert_prgba8888_to_g8;
                case PIXFMT_A8:
                    return convert_rgba8888_to_a8;
                case PIXFMT_RGB888:
                    return convert_prgba8888_to_rgb888;
                case PIXFMT_BGR888:
                    return convert_prgba8888_to_bgr888;
                case PIXFMT_RGBA8888:
                    return convert_prgba8888_to_rgba8888;
                case PIXFMT_BGRA8888:
                    return convert_prgba8888_to_bgra8888;

                default:
                    break;
            }
            return NULL;
        }


        static pixel_conversion_t pixel_convert_function_for_pbgra8888(pixel_format_t dst_fmt) noexcept
        {
            switch (dst_fmt)
            {
                case PIXFMT_G1:
                    return convert_prgba8888_to_g1;
                case PIXFMT_A1:
                    return convert_rgba8888_to_a1;
                case PIXFMT_G2:
                    return convert_prgba8888_to_g2;
                case PIXFMT_A2:
                    return convert_rgba8888_to_a2;
                case PIXFMT_G4:
                    return convert_prgba8888_to_g4;
                case PIXFMT_A4:
                    return convert_rgba8888_to_a4;
                case PIXFMT_G8:
                    return convert_prgba8888_to_g8;
                case PIXFMT_A8:
                    return convert_rgba8888_to_a8;
                case PIXFMT_RGB888:
                    return convert_prgba8888_to_bgr888;
                case PIXFMT_BGR888:
                    return convert_prgba8888_to_rgb888;
                case PIXFMT_RGBA8888:
                    return convert_prgba8888_to_bgra8888;
                case PIXFMT_BGRA8888:
                    return convert_prgba8888_to_rgba8888;

                default:
                    break;
            }
            return NULL;
        }

        pixel_conversion_t pixel_convert_function(pixel_format_t dst_fmt, pixel_format_t src_fmt) noexcept
        {
            switch (src_fmt)
            {
                case PIXFMT_G1:         return pixel_convert_function_for_g1(dst_fmt);
                case PIXFMT_A1:         return pixel_convert_function_for_a1(dst_fmt);
                case PIXFMT_G2:         return pixel_convert_function_for_g2(dst_fmt);
                case PIXFMT_A2:         return pixel_convert_function_for_a2(dst_fmt);
                case PIXFMT_G4:         return pixel_convert_function_for_g4(dst_fmt);
                case PIXFMT_A4:         return pixel_convert_function_for_a4(dst_fmt);
                case PIXFMT_G8:         return pixel_convert_function_for_g8(dst_fmt);
                case PIXFMT_A8:         return pixel_convert_function_for_a8(dst_fmt);
                case PIXFMT_RGB888:     return pixel_convert_function_for_rgb888(dst_fmt);
                case PIXFMT_BGR888:     return pixel_convert_function_for_bgr888(dst_fmt);
                case PIXFMT_RGBA8888:   return pixel_convert_function_for_rgba8888(dst_fmt);
                case PIXFMT_BGRA8888:   return pixel_convert_function_for_bgra8888(dst_fmt);
                case PIXFMT_PRGBA8888:  return pixel_convert_function_for_prgba8888(dst_fmt);
                case PIXFMT_PBGRA8888:  return pixel_convert_function_for_pbgra8888(dst_fmt);
                default:
                    break;
            }

            return NULL;
        }

        void convert_pixels(uint8_t *dst, const uint8_t *src, pixel_format_t dst_fmt, pixel_format_t src_fmt, size_t count) noexcept
        {
            const pixel_conversion_t func = pixel_convert_function(dst_fmt, src_fmt);
            if (func != NULL)
            {
                func(dst, src, count);
                return;
            }
            if (dst != src)
                memcpy(dst, src, device_bits_per_pixel(dst_fmt) * count);
        }

    } /* namespace mm */
} /* namespace lsp */
