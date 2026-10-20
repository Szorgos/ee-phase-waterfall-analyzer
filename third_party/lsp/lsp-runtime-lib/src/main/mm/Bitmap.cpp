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

#include <lsp-plug.in/fmt/xpm/xpm.h>
#include <lsp-plug.in/io/InFileStream.h>
#include <lsp-plug.in/io/InMemoryStream.h>
#include <lsp-plug.in/io/InBufStream.h>
#include <lsp-plug.in/mm/Bitmap.h>

#include <private/mm/bitmap/xpm.h>

namespace lsp
{
    namespace mm
    {
        static inline bool is_exact_copy(pixel_format_t a, pixel_format_t b)
        {
            if (a == b)
                return true;
            switch (a)
            {
                case PIXFMT_G1:         return b == PIXFMT_A1;
                case PIXFMT_G2:         return b == PIXFMT_A2;
                case PIXFMT_G4:         return b == PIXFMT_A4;
                case PIXFMT_G8:         return b == PIXFMT_A8;
                case PIXFMT_A1:         return b == PIXFMT_G1;
                case PIXFMT_A2:         return b == PIXFMT_G2;
                case PIXFMT_A4:         return b == PIXFMT_G4;
                case PIXFMT_A8:         return b == PIXFMT_G8;
                default:
                    break;
            }
            return false;
        }

        Bitmap::Bitmap() noexcept
        {
            pData       = NULL;
            nWidth      = 0;
            nHeight     = 0;
            nStride     = 0;
            enFormat    = PIXFMT_RGBA8888;
        }

        Bitmap::Bitmap(const Bitmap & src)
        {
            pData       = NULL;
            nWidth      = 0;
            nHeight     = 0;
            nStride     = 0;
            enFormat    = PIXFMT_RGBA8888;

            set(src);
        }

        Bitmap::Bitmap(const Bitmap * src)
        {
            pData       = NULL;
            nWidth      = 0;
            nHeight     = 0;
            nStride     = 0;
            enFormat    = PIXFMT_RGBA8888;

            set(src);
        }

        Bitmap::Bitmap(Bitmap && src) noexcept
        {
            pData       = lsp::exchange(src.pData, static_cast<uint8_t *>(NULL));
            nWidth      = lsp::exchange(src.nWidth, 0);
            nHeight     = lsp::exchange(src.nHeight, 0);
            nStride     = lsp::exchange(src.nStride, 0);
            enFormat    = lsp::exchange(src.enFormat, PIXFMT_RGBA8888);
        }

        Bitmap::~Bitmap()
        {
            if (pData != NULL)
            {
                free(pData);
                pData       = NULL;
            }
        }

        inline size_t Bitmap::calc_bytes_per_row(pixel_format_t format, size_t cols) noexcept
        {
            const size_t bpp        = device_bits_per_pixel(format);
            return (bpp * cols + 7) >> 3;
        }

        inline size_t Bitmap::calc_stride(pixel_format_t format, size_t cols) noexcept
        {
            const size_t bpp        = device_bits_per_pixel(format);
            return (bpp <= 8) ? (bpp * cols + 7) >> 3 : ((bpp * cols + 31) & (~size_t(0x1f))) >> 3;
        }

        Bitmap & Bitmap::operator = (Bitmap && src) noexcept
        {
            pData       = lsp::exchange(src.pData, static_cast<uint8_t *>(NULL));
            nWidth      = lsp::exchange(src.nWidth, 0);
            nHeight     = lsp::exchange(src.nHeight, 0);
            nStride     = lsp::exchange(src.nStride, 0);
            enFormat    = lsp::exchange(src.enFormat, PIXFMT_RGBA8888);

            return *this;
        }

        Bitmap & Bitmap::operator = (const Bitmap & src)
        {
            set(src);
            return *this;
        }

        Bitmap & Bitmap::operator = (const Bitmap * src)
        {
            set(src);
            return *this;
        }

        void Bitmap::swap(Bitmap & src) noexcept
        {
            lsp::swap(pData, src.pData);
            lsp::swap(nWidth, src.nWidth);
            lsp::swap(nHeight, src.nHeight);
            lsp::swap(nStride, src.nStride);
            lsp::swap(enFormat, src.enFormat);
        }

        void Bitmap::swap(Bitmap * src) noexcept
        {
            lsp::swap(pData, src->pData);
            lsp::swap(nWidth, src->nWidth);
            lsp::swap(nHeight, src->nHeight);
            lsp::swap(nStride, src->nStride);
            lsp::swap(enFormat, src->enFormat);
        }

        void Bitmap::reset()
        {
            if (pData != NULL)
            {
                free(pData);
                pData       = NULL;
            }
            nWidth      = 0;
            nHeight     = 0;
            nStride     = 0;
        }

        status_t Bitmap::init(pixel_format_t format, size_t width, size_t height)
        {
            if (format == PIXFMT_DEFAULT)
                return STATUS_INVALID_VALUE;
            if ((height <= 0) || (width <= 0))
            {
                reset();
                enFormat    = format;
                return STATUS_OK;
            }

            const size_t stride     = calc_stride(format, width);
            const size_t to_alloc   = stride * height;
            uint8_t * data          = static_cast<uint8_t *>(realloc(pData, to_alloc));
            if (data == NULL)
                return STATUS_NO_MEM;

            // Cleanup data
            bzero(data, to_alloc);

            // Commit data
            pData       = data;
            nWidth      = width;
            nHeight     = height;
            nStride     = stride;
            enFormat    = format;

            return STATUS_OK;
        }

        status_t Bitmap::set(const Bitmap & src)
        {
            if (src.pData == NULL)
            {
                reset();
                enFormat    = src.enFormat;
                return STATUS_OK;
            }

            // Compute parameters of the new bitmap
            uint8_t * data          = static_cast<uint8_t *>(realloc(pData, src.nHeight * src.nStride));
            if (data == NULL)
                return STATUS_NO_MEM;
            lsp_finally {
                if (data != NULL)
                    free(data);
            };

            // Copy data
            memcpy(data, src.pData, src.nHeight * src.nStride);

            pData                   = data;
            nWidth                  = src.nWidth;
            nHeight                 = src.nHeight;
            nStride                 = src.nStride;
            enFormat                = src.enFormat;

            return STATUS_OK;
        }

        status_t Bitmap::set(const Bitmap * src)
        {
            if (src == NULL)
                return STATUS_BAD_ARGUMENTS;
            return set(*src);
        }

        status_t Bitmap::load(const char *path, pixel_format_t format, IColorMap *map)
        {
            if (path == NULL)
                return STATUS_BAD_ARGUMENTS;

            io::InFileStream ifs;
            status_t res = ifs.open(path);
            if (res != STATUS_OK)
                return res;

            res = update_status(res, do_load(&ifs, format, map));
            return update_status(res, ifs.close());
        }

        status_t Bitmap::load(const LSPString *path, pixel_format_t format, IColorMap *map)
        {
            return (path != NULL) ? load(*path, format, map) : STATUS_BAD_ARGUMENTS;
        }

        status_t Bitmap::load(const LSPString & path, pixel_format_t format, IColorMap *map)
        {
            io::InFileStream ifs;
            status_t res = ifs.open(&path);
            if (res != STATUS_OK)
                return res;

            res = update_status(res, do_load(&ifs, format, map));
            return update_status(res, ifs.close());
        }

        status_t Bitmap::load(const io::Path *path, pixel_format_t format, IColorMap *map)
        {
            return (path != NULL) ? load(*path, format, map) : STATUS_BAD_ARGUMENTS;
        }

        status_t Bitmap::load(const io::Path & path, pixel_format_t format, IColorMap *map)
        {
            io::InFileStream ifs;
            status_t res = ifs.open(&path);
            if (res != STATUS_OK)
                return res;

            res = update_status(res, do_load(&ifs, format, map));
            return update_status(res, ifs.close());
        }

        status_t Bitmap::load(const void *data, size_t size, pixel_format_t format, IColorMap *map)
        {
            if (data == NULL)
                return STATUS_BAD_ARGUMENTS;

            io::InMemoryStream ims;
            ims.wrap(data, size);

            status_t res = do_load(&ims, format, map);
            return update_status(res, ims.close());
        }

        status_t Bitmap::load(io::IInStream *is, pixel_format_t format, IColorMap *map)
        {
            io::InBufStream ibs;
            status_t res = ibs.wrap(is, WRAP_NONE);
            if (res != STATUS_OK)
                return res;

            res = update_status(res, do_load(&ibs, format, map));
            return update_status(res, ibs.close());
        }


        status_t Bitmap::do_load(io::IInStream *is, pixel_format_t format, IColorMap *map)
        {
            status_t res = load_xpm(is, format, map);
            if ((res == STATUS_OK) || (res != STATUS_UNSUPPORTED_FORMAT))
                return res;

            return STATUS_UNSUPPORTED_FORMAT;
        }

        status_t Bitmap::resize(size_t width, size_t height)
        {
            if ((nWidth == width) && (nHeight == height))
                return STATUS_OK;

            // Compute parameters of the new bitmap
            const size_t stride     = calc_stride(enFormat, width);
            const size_t to_alloc   = stride * height;
            uint8_t * data          = static_cast<uint8_t *>(malloc(to_alloc));
            if (data == NULL)
                return STATUS_NO_MEM;
            lsp_finally {
                if (data != NULL)
                    free(data);
            };

            // Preserve previous picture if possible
            if ((pData != NULL) && (nWidth > 0) && (nHeight > 0))
            {
                const size_t to_copy    = lsp_min(stride, nStride);
                const size_t to_rows    = lsp_min(height, nHeight);

                for (size_t i=0; i<to_rows; ++i)
                {
                    memcpy(&data[i*stride], &pData[i*nStride], to_copy);
                    bzero(&data[i*stride + to_copy], to_copy - nStride);
                }
                bzero(&data[to_rows * stride], (height - nHeight) * stride);
            }
            else
                bzero(data, to_alloc);

            lsp::swap(pData, data);
            nWidth      = width;
            nHeight     = height;
            nStride     = stride;

            return STATUS_OK;
        }

        status_t Bitmap::convert_to(Bitmap & dst, pixel_format_t format) const
        {
            if (enFormat == format)
                return STATUS_OK;
            else if (format == PIXFMT_DEFAULT)
                return STATUS_INVALID_VALUE;

            // Get pixel conversion function
            const pixel_conversion_t conversion = pixel_convert_function(format, enFormat);
            if (conversion == NULL)
                return STATUS_UNSUPPORTED_FORMAT;

            // Check if we need to perform reallocation
            const size_t stride     = calc_stride(format, nWidth);
            if (stride * dst.nHeight == nStride * nHeight)
            {
                // Perform conversion without reallocation
                for (size_t r = 0; r < nHeight; ++r)
                    conversion(&dst.pData[r*nStride], &pData[r*nStride], nWidth);
            }
            else
            {
                // Need to re-allocate memory
                const size_t bpr        = calc_bytes_per_row(format, nWidth);
                const size_t to_alloc   = stride * nHeight;
                uint8_t * data          = static_cast<uint8_t *>(malloc(to_alloc));
                if (data == NULL)
                    return STATUS_NO_MEM;
                lsp_finally {
                    if (data != NULL)
                        free(data);
                };

                // Perform conversion
                for (size_t r = 0; r < nHeight; ++r)
                {
                    conversion(&data[r*stride], &pData[r*nStride], nWidth);
                    bzero(&data[r*stride + bpr], stride - bpr);
                }

                // Commit new data
                lsp::swap(dst.pData, data);
            }

            dst.nStride     = stride;
            dst.nWidth      = nWidth;
            dst.nHeight     = nHeight;
            dst.enFormat    = format;

            return STATUS_OK;
        }

        status_t Bitmap::convert_to(Bitmap *dst, pixel_format_t format) const
        {
            return (dst != NULL) ? convert_to(*dst, format) : STATUS_BAD_ARGUMENTS;
        }

        status_t Bitmap::convert(pixel_format_t format)
        {
            if (enFormat == format)
                return STATUS_OK;
            else if (format == PIXFMT_DEFAULT)
                return STATUS_INVALID_VALUE;

            // Check that no changes are required to be made
            if ((nWidth <= 0) || (nHeight <= 0) || (is_exact_copy(enFormat, format)))
            {
                enFormat        = format;
                return STATUS_OK;
            }

            // Get pixel conversion function
            const pixel_conversion_t conversion = pixel_convert_function(format, enFormat);
            if (conversion == NULL)
                return STATUS_UNSUPPORTED_FORMAT;

            // Check if we need to perform reallocation
            const size_t stride     = calc_stride(format, nWidth);
            if (stride == nStride)
            {
                // Perform conversion without reallocation
                for (size_t r = 0; r < nHeight; ++r)
                    conversion(&pData[r*nStride], &pData[r*nStride], nWidth);
            }
            else
            {
                // Need to re-allocate memory
                const size_t bpr        = calc_bytes_per_row(format, nWidth);
                const size_t to_alloc   = stride * nHeight;
                uint8_t * data          = static_cast<uint8_t *>(malloc(to_alloc));
                if (data == NULL)
                    return STATUS_NO_MEM;
                lsp_finally {
                    if (data != NULL)
                        free(data);
                };

                // Perform conversion
                for (size_t r = 0; r < nHeight; ++r)
                {
                    conversion(&data[r*stride], &pData[r*nStride], nWidth);
                    bzero(&data[r*stride + bpr], stride - bpr);
                }

                // Commit new data
                lsp::swap(pData, data);
            }

            nStride         = stride;
            enFormat        = format;

            return STATUS_OK;
        }

        status_t Bitmap::convert_from(const Bitmap & src, pixel_format_t format)
        {
            return src.convert_to(*this, format);
        }

        status_t Bitmap::convert_from(const Bitmap * src, pixel_format_t format)
        {
            return (src != NULL) ? src->convert_to(*this, format) : STATUS_BAD_ARGUMENTS;
        }

    } /* namespace mm */
} /* namespace lsp */

