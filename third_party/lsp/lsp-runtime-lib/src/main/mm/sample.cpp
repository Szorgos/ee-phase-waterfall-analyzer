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

#include <lsp-plug.in/common/endian.h>
#include <lsp-plug.in/mm/sample.h>
#include <lsp-plug.in/stdlib/string.h>

/*
 * TODO: fix floating-point rounding
 *
 * https://www.cs.cmu.edu/~rbd/papers/cmj-float-to-int.html
 *
 * Most audio programs convert floating point samples to integer samples and write
 * them to a file or an audio output device. I will assume that ‘‘correct’’ behavior
 * is to round to the nearest integer value. Dealing with scale factors and overflow
 * are also important issues, but there is no standard and the best approach may depend
 * on the application. I will limit my discussion to rounding, which is where this bug occurs.
 *
 * The natural way to implement the conversion is to scale each floating point sample
 * to some appropriate range (-32767 to 32767) and assign it to a signed 16-bit integer as follows:
 *      float f; // assume -32768 <= f <= 32767
 *      int i; // could also be “shortint”
 *      i = (int) f; // “(int) f” means “convert f to an int”
 *
 * The default float-to-integer conversion in C does not round to the nearest integer,
 * but instead truncates toward zero. That means that signal values in the open interval (–1.0, 1.0)
 * are all converted to zero (0). This interval is twice as large as the interval mapping to any other
 * integer, and this introduces a nonlinear distortion into the signal. This is not just an issue of
 * truncation versus rounding. It is well known that rounding to the nearest integer can be achieved by adding 0.5
 * and rounding down, but the following C assignment is incorrect:
 *
 *      i = (int)(f + 0.5);
 *
 * C does not round negative numbers down, so values in the interval (-1.5, 0.5) are converted to zero.
 * In contrast, a correct conversion should map only the interval (-0.5, 0.5) to zero.
 * There are several ways to perform rounding for audio, and, surprisingly, proper rounding can be faster
 * than the default conversion in C. The direct implementation is to treat positive and negative numbers as
 * different cases:
 *
 *      float f; // assume -32768 <= f <= 32767
 *      int i; // could also be “shortint”
 *      if(f > 0) { i = (int)(f + 0.5); }
 *      else { i = (int)(f - 0.5); }
 *
 * This code has the problem of taking a branch, which is very slow relative to arithmetic on modern
 * processors. However, this is a good approach if you can combine the rounding with testing for peak
 * values and clipping out-ofrange values, which also treat positive and negative samples separately.
 * An elegant approach, suggested by Phil Burk, the developer of JSyn and co-developer of PortAudio,
 * is to offset the sample values to make them all positive, perform rounding, and then shift back.
 * Note that I add an extra 0.5 before truncating to simulate rounding behavior:
 *
 *      i = (((int) (f + 32768.5)) - 32768)
 *
 */

namespace lsp
{
    namespace mm
    {
        typedef void (*cvt_sample_t) (void *dst, const void *src, size_t samples);

        static inline uint32_t read24bit(const uint8_t *p)
        {
            uint32_t res =
                __IF_LEBE(
                    (uint32_t(p[0])) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16),
                    (uint32_t(p[2])) | (uint32_t(p[1]) << 8) | (uint32_t(p[0]) << 16)
                );
            return res;
        }

        static inline void write24bit(uint8_t *p, uint32_t x)
        {
            __IF_LE(
                p[0] = uint8_t(x);
                p[1] = uint8_t(x >> 8);
                p[2] = uint8_t(x >> 16);
            )
            __IF_BE(
                p[0] = uint8_t(x >> 16);
                p[1] = uint8_t(x >> 8);
                p[2] = uint8_t(x);
            )
        }

        bool sample_endian_swap(void *buf, size_t samples, size_t format)
        {
            size_t fmt = sformat_endian(format);
            if (fmt == __IF_LEBE(SFMT_LE, SFMT_BE))
                return true;
            else if (fmt != __IF_LEBE(SFMT_BE, SFMT_LE))
                return false;

            switch (sformat_format(format))
            {
                case SFMT_U8:
                case SFMT_S8:
                    break;
                case SFMT_U16:
                case SFMT_S16:
                    ::lsp::byte_swap(static_cast<uint16_t *>(buf), samples);
                    break;
                case SFMT_U32:
                case SFMT_S32:
                    ::lsp::byte_swap(static_cast<uint32_t *>(buf), samples);
                    break;
                case SFMT_F32:
                    ::lsp::byte_swap(static_cast<f32_t *>(buf), samples);
                    break;
                case SFMT_F64:
                    ::lsp::byte_swap(static_cast<f64_t *>(buf), samples);
                    break;

                case SFMT_U24: case SFMT_S24:
                {
                    uint8_t x, *ptr = static_cast<uint8_t *>(buf);
                    while (samples--)
                    {
                        x       = ptr[0];
                        ptr[0]  = ptr[2];
                        ptr[2]  = x;
                        ptr    += 3;
                    }
                    break;
                }

                default:
                    return false;
            }

            return true;
        }

        #define CVT_SHIFT(TYPE)         (uint32_t(1 << (sizeof(TYPE)*8 - 1)))
        #define CVT_RANGE(TYPE)         (uint32_t(1 << (sizeof(TYPE)*8 - 1))-1)

        // Integer conversions
        #define CVT_UI_TO_UI(DTYPE, STYPE, SHIFT) \
            for (STYPE *sptr = static_cast<STYPE *>(src); samples > 0; --samples, ++sptr, ++dptr) \
                *dptr   = (DTYPE)((*sptr) SHIFT);

        #define CVT_UI_TO_SI(DTYPE, STYPE, SHIFT) \
            for (STYPE *sptr = static_cast<STYPE *>(src); samples > 0; --samples, ++sptr, ++dptr) \
                *dptr   = (DTYPE)((*sptr - CVT_SHIFT(STYPE)) SHIFT);

        #define CVT_SI_TO_UI(DTYPE, STYPE, SHIFT) \
            for (STYPE *sptr = static_cast<STYPE *>(src); samples > 0; --samples, ++sptr, ++dptr) \
                *dptr   = (DTYPE)((*sptr + CVT_SHIFT(STYPE)) SHIFT);

        #define CVT_SI_TO_SI(DTYPE, STYPE, SHIFT) \
            for (STYPE *sptr = static_cast<STYPE *>(src); samples > 0; --samples, ++sptr, ++dptr) \
                *dptr   = (DTYPE)((*sptr) SHIFT);

        #define CVT_UI_TO_FX(DTYPE, STYPE) \
            for (STYPE *sptr = static_cast<STYPE *>(src); samples > 0; --samples, ++sptr, ++dptr) \
                *dptr   = ((STYPE)(*sptr - CVT_SHIFT(STYPE))) * (DTYPE(1.0 / CVT_RANGE(STYPE)));

        #define CVT_SI_TO_FX(DTYPE, STYPE) \
            for (STYPE *sptr = static_cast<STYPE *>(src); samples > 0; --samples, ++sptr, ++dptr) \
                *dptr   = ((STYPE)(*sptr)) * (DTYPE(1.0 / CVT_RANGE(STYPE)));

        #define CVT_UI_TO_XI(DTYPE, STYPE, SHIFT) \
            if (sign) \
                CVT_UI_TO_SI(DTYPE, STYPE, SHIFT) \
            else \
                CVT_UI_TO_UI(DTYPE, STYPE, SHIFT)

        #define CVT_SI_TO_XI(DTYPE, STYPE, SHIFT) \
            if (sign) \
                CVT_SI_TO_SI(DTYPE, STYPE, SHIFT) \
            else \
                CVT_SI_TO_UI(DTYPE, STYPE, SHIFT)

        // Integer 24-bit conversions
        #define CVT_U24_TO_UI(DTYPE, SHIFT) \
            for (uint8_t *sptr = static_cast<uint8_t *>(src); samples > 0; --samples, sptr += 3, ++dptr) \
                *dptr   = (DTYPE)(read24bit(sptr) SHIFT);

        #define CVT_U24_TO_SI(DTYPE, SHIFT) \
            for (uint8_t *sptr = static_cast<uint8_t *>(src); samples > 0; --samples, sptr += 3, ++dptr) \
                *dptr   = (DTYPE)((read24bit(sptr) - 0x800000) SHIFT);

        #define CVT_S24_TO_UI(DTYPE, SHIFT) \
            for (uint8_t *sptr = static_cast<uint8_t *>(src); samples > 0; --samples, sptr += 3, ++dptr) \
                *dptr   = (DTYPE)((read24bit(sptr) + 0x800000) SHIFT);

        #define CVT_S24_TO_SI(DTYPE, SHIFT) \
            for (uint8_t *sptr = static_cast<uint8_t *>(src); samples > 0; --samples, sptr += 3, ++dptr) \
                *dptr   = (DTYPE)(read24bit(sptr) SHIFT);

        #define CVT_U24_TO_FX(DTYPE) \
            for (uint8_t *sptr = static_cast<uint8_t *>(src); samples > 0; --samples, sptr += 3, ++dptr) \
                *dptr   = (int32_t(read24bit(sptr) - 0x800000) * (DTYPE(1.0)/0x7fffff));

        #define CVT_S24_TO_FX(DTYPE) \
            for (uint8_t *sptr = static_cast<uint8_t *>(src); samples > 0; --samples, sptr += 3, ++dptr) \
                *dptr   = ((int32_t(read24bit(sptr) << 8) >> 8) * (DTYPE(1.0)/0x7fffff));

        #define CVT_U24_TO_XI(DTYPE, SHIFT) \
            if (sign) \
                CVT_U24_TO_SI(DTYPE, SHIFT) \
            else \
                CVT_U24_TO_UI(DTYPE, SHIFT)

        #define CVT_S24_TO_XI(DTYPE, SHIFT) \
            if (sign) \
                CVT_S24_TO_SI(DTYPE, SHIFT) \
            else \
                CVT_S24_TO_UI(DTYPE, SHIFT)


        #define CVT_UI_TO_UI24(STYPE, SHIFT) \
            for (STYPE *sptr = static_cast<STYPE *>(src); samples > 0; --samples, ++sptr, dptr += 3) \
                write24bit(dptr, uint32_t((*sptr) SHIFT));

        #define CVT_UI_TO_SI24(STYPE, SHIFT) \
            for (STYPE *sptr = static_cast<STYPE *>(src); samples > 0; --samples, ++sptr, dptr += 3) \
                write24bit(dptr, uint32_t((*sptr - CVT_SHIFT(STYPE)) SHIFT));

        #define CVT_SI_TO_UI24(STYPE, SHIFT) \
            for (STYPE *sptr = static_cast<STYPE *>(src); samples > 0; --samples, ++sptr, dptr += 3) \
                write24bit(dptr, uint32_t((*sptr + CVT_SHIFT(STYPE)) SHIFT));

        #define CVT_SI_TO_SI24(STYPE, SHIFT) \
            for (STYPE *sptr = static_cast<STYPE *>(src); samples > 0; --samples, ++sptr, dptr += 3) \
                write24bit(dptr, uint32_t((*sptr) SHIFT));

        #define CVT_UI_TO_XI24(STYPE, SHIFT) \
            if (sign) \
                CVT_UI_TO_SI24(STYPE, SHIFT) \
            else \
                CVT_UI_TO_UI24(STYPE, SHIFT)

        #define CVT_SI_TO_XI24(STYPE, SHIFT) \
            if (sign) \
                CVT_SI_TO_SI24(STYPE, SHIFT) \
            else \
                CVT_SI_TO_UI24(STYPE, SHIFT)

        #define CVT_SI24_TO_UI24() \
            for (uint8_t *sptr = static_cast<uint8_t *>(src); samples > 0; --samples, sptr += 3, dptr += 3) \
                write24bit(dptr, read24bit(sptr) + 0x800000);

        #define CVT_UI24_TO_SI24() \
            for (uint8_t *sptr = static_cast<uint8_t *>(src); samples > 0; --samples, sptr += 3, dptr += 3) \
                write24bit(dptr, read24bit(sptr) - 0x800000);

        // Float conversions
        #define CVT_F32_TO_UI(STYPE, DTYPE) \
            for (f32_t *sptr = static_cast<f32_t *>(src); samples > 0; --samples, ++sptr, ++dptr) \
                *dptr   = (STYPE)(*sptr * f32_t(CVT_RANGE(DTYPE))) + (DTYPE)CVT_SHIFT(DTYPE);
        #define CVT_F32_TO_SI(DTYPE) \
            for (f32_t *sptr = static_cast<f32_t *>(src); samples > 0; --samples, ++sptr, ++dptr) \
                *dptr   = (DTYPE)(*sptr * f32_t(CVT_RANGE(DTYPE)));

        #define CVT_F32_TO_XI(STYPE, DTYPE) \
            if (sign) \
                CVT_F32_TO_SI(STYPE) \
            else \
                CVT_F32_TO_UI(STYPE, DTYPE)


        // Double conversions
        #define CVT_F64_TO_UI(STYPE, DTYPE) \
            for (f64_t *sptr = static_cast<f64_t *>(src); samples > 0; --samples, ++sptr, ++dptr) \
                *dptr   = (STYPE)(*sptr * f64_t(CVT_RANGE(DTYPE))) + (DTYPE)CVT_SHIFT(DTYPE);
        #define CVT_F64_TO_SI(DTYPE) \
            for (f64_t *sptr = static_cast<f64_t *>(src); samples > 0; --samples, ++sptr, ++dptr) \
                *dptr   = (DTYPE)(*sptr * f64_t(CVT_RANGE(DTYPE)));

        #define CVT_F64_TO_XI(STYPE, DTYPE) \
            if (sign) \
                CVT_F64_TO_SI(STYPE) \
            else \
                CVT_F64_TO_UI(STYPE, DTYPE)

        // Floating-point conversions
        #define CVT_FX_TO_SI24(STYPE) \
            for (STYPE *sptr = static_cast<STYPE *>(src); samples > 0; --samples, ++sptr, dptr += 3) \
                write24bit(dptr, int32_t(*sptr * 0x7fffff));
        #define CVT_FX_TO_UI24(STYPE) \
            for (STYPE *sptr = static_cast<STYPE *>(src); samples > 0; --samples, ++sptr, dptr += 3) \
                write24bit(dptr, int32_t(*sptr * 0x7fffff) - 0x800000);
        #define CVT_FX_TO_XI24(STYPE) \
            if (sign) \
                CVT_FX_TO_SI24(STYPE) \
            else \
                CVT_FX_TO_UI24(STYPE)

        #define CVT_FX_TO_UI32(STYPE) \
            for (STYPE *sptr = static_cast<STYPE *>(src); samples > 0; --samples, ++sptr, ++dptr) \
                *dptr   = (int32_t)(*sptr * f64_t(CVT_RANGE(uint32_t))) + (uint32_t)CVT_SHIFT(uint32_t);
        #define CVT_FX_TO_SI32(STYPE) \
            for (STYPE *sptr = static_cast<STYPE *>(src); samples > 0; --samples, ++sptr, ++dptr) \
                *dptr   = (int32_t)(*sptr * f64_t(CVT_RANGE(uint32_t)));

        #define CVT_FX_TO_XI32(STYPE) \
            if (sign) \
                CVT_FX_TO_SI32(STYPE) \
            else \
                CVT_FX_TO_UI32(STYPE)

        #define CVT_FX_TO_FX(DTYPE, STYPE) \
            for (STYPE *sptr = static_cast<STYPE *>(src); samples > 0; --samples, ++sptr, ++dptr) \
                *dptr   = (DTYPE)(*sptr);


        bool convert_to_8bit(void *dst, void *src, size_t samples, size_t to, size_t from)
        {
            int sign = sformat_sign(to);
            if (sign < 0)
                return false;
            uint8_t *dptr = static_cast<uint8_t *>(dst);

            switch (sformat_format(from))
            {
                case SFMT_U8:
                    if (sign)   CVT_UI_TO_SI(uint8_t, uint8_t, )
                    else        ::memcpy(dptr, src, samples * sizeof(uint8_t));
                    return true;
                case SFMT_S8:
                    if (sign)   ::memcpy(dptr, src, samples * sizeof(uint8_t));
                    else        CVT_SI_TO_UI(uint8_t, uint8_t, )
                    return true;

                case SFMT_U16: CVT_UI_TO_XI(uint8_t, uint16_t, >> 8)    return true;
                case SFMT_S16: CVT_SI_TO_XI(uint8_t, uint16_t, >> 8)    return true;
                case SFMT_U24: CVT_U24_TO_XI(uint8_t, >> 16)            return true;
                case SFMT_S24: CVT_S24_TO_XI(uint8_t, >> 16)            return true;
                case SFMT_U32: CVT_UI_TO_XI(uint8_t, uint32_t, >> 24)   return true;
                case SFMT_S32: CVT_SI_TO_XI(uint8_t, uint32_t, >> 24)   return true;
                case SFMT_F32: CVT_F32_TO_XI(int8_t, uint8_t)           return true;
                case SFMT_F64: CVT_F64_TO_XI(int8_t, uint8_t)           return true;

                default:
                    break;
            }

            return false;
        }

        bool convert_to_16bit(void *dst, void *src, size_t samples, size_t to, size_t from)
        {
            int sign = sformat_sign(to);
            if (sign < 0)
                return false;
            uint16_t *dptr = static_cast<uint16_t *>(dst);

            switch (sformat_format(from))
            {
                case SFMT_U8:  CVT_UI_TO_XI(uint16_t, uint8_t, << 8)    return true;
                case SFMT_S8:  CVT_SI_TO_XI(uint16_t, uint8_t, << 8)    return true;

                case SFMT_U16:
                    if (sign)   CVT_UI_TO_SI(uint16_t, uint16_t, )
                    else        ::memcpy(dptr, src, samples * sizeof(uint16_t));
                    return true;
                case SFMT_S16:
                    if (sign)   ::memcpy(dptr, src, samples * sizeof(uint16_t));
                    else        CVT_SI_TO_UI(uint16_t, uint16_t, )
                    return true;

                case SFMT_U24: CVT_U24_TO_XI(uint16_t, >> 8)            return true;
                case SFMT_S24: CVT_S24_TO_XI(uint16_t, >> 8)            return true;
                case SFMT_U32: CVT_UI_TO_XI(uint16_t, uint32_t, >> 16)  return true;
                case SFMT_S32: CVT_SI_TO_XI(uint16_t, uint32_t, >> 16)  return true;
                case SFMT_F32: CVT_F32_TO_XI(int16_t, uint16_t)         return true;
                case SFMT_F64: CVT_F64_TO_XI(int16_t, uint16_t)         return true;

                default:
                    break;
            }

            return false;
        }

        bool convert_to_24bit(void *dst, void *src, size_t samples, size_t to, size_t from)
        {
            int sign = sformat_sign(to);
            if (sign < 0)
                return false;
            uint8_t *dptr = static_cast<uint8_t *>(dst);

            switch (sformat_format(from))
            {
                case SFMT_U8:  CVT_UI_TO_XI24(uint8_t, << 16)           return true;
                case SFMT_S8:  CVT_SI_TO_XI24(uint8_t, << 16)           return true;

                case SFMT_U16: CVT_UI_TO_XI24(uint16_t, << 8)           return true;
                case SFMT_S16: CVT_SI_TO_XI24(uint16_t, << 8)           return true;

                case SFMT_U24:
                    if (sign)   CVT_UI24_TO_SI24()
                    else        ::memcpy(dptr, src, samples * sizeof(uint8_t) * 3);
                    return true;
                case SFMT_S24:
                    if (sign)   ::memcpy(dptr, src, samples * sizeof(uint8_t) * 3);
                    else        CVT_SI24_TO_UI24()
                    return true;

                case SFMT_U32: CVT_UI_TO_XI24(uint32_t, >> 8)           return true;
                case SFMT_S32: CVT_SI_TO_XI24(uint32_t, >> 8)           return true;
                case SFMT_F32: CVT_FX_TO_XI24(f32_t)                    return true;
                case SFMT_F64: CVT_FX_TO_XI24(f64_t)                    return true;

                default:
                    break;
            }

            return false;
        }

        bool convert_to_32bit(void *dst, void *src, size_t samples, size_t to, size_t from)
        {
            int sign = sformat_sign(to);
            if (sign < 0)
                return false;
            uint32_t *dptr = static_cast<uint32_t *>(dst);

            switch (sformat_format(from))
            {
                case SFMT_U8:  CVT_UI_TO_XI(uint32_t, uint8_t, << 24)   return true;
                case SFMT_S8:  CVT_SI_TO_XI(uint32_t, uint8_t, << 24)   return true;

                case SFMT_U16: CVT_UI_TO_XI(uint32_t, uint16_t, << 16)  return true;
                case SFMT_S16: CVT_SI_TO_XI(uint32_t, uint16_t, << 16)  return true;

                case SFMT_U24: CVT_U24_TO_XI(uint32_t, << 8)            return true;
                case SFMT_S24: CVT_S24_TO_XI(uint32_t, << 8)            return true;

                case SFMT_U32:
                    if (sign)   CVT_UI_TO_SI(uint32_t, uint32_t, )
                    else        ::memcpy(dptr, src, samples * sizeof(uint32_t));
                    return true;
                case SFMT_S32:
                    if (sign)   ::memcpy(dptr, src, samples * sizeof(uint32_t));
                    else        CVT_SI_TO_UI(uint32_t, uint32_t, )
                    return true;

                case SFMT_F32: CVT_FX_TO_XI32(f32_t)                    return true;
                case SFMT_F64: CVT_FX_TO_XI32(f64_t)                    return true;

                default:
                    break;
            }

            return false;
        }

        bool convert_to_f32(void *dst, void *src, size_t samples, size_t to, size_t from)
        {
            f32_t *dptr = static_cast<f32_t *>(dst);

            switch (sformat_format(from))
            {
                case SFMT_U8:  CVT_UI_TO_FX(f32_t, int8_t)              return true;
                case SFMT_S8:  CVT_SI_TO_FX(f32_t, int8_t)              return true;
                case SFMT_U16: CVT_UI_TO_FX(f32_t, int16_t)             return true;
                case SFMT_S16: CVT_SI_TO_FX(f32_t, int16_t)             return true;

                case SFMT_U24: CVT_U24_TO_FX(f32_t)                     return true;
                case SFMT_S24: CVT_S24_TO_FX(f32_t)                     return true;

                case SFMT_U32: CVT_UI_TO_FX(f32_t, int32_t)             return true;
                case SFMT_S32: CVT_SI_TO_FX(f32_t, int32_t)             return true;

                case SFMT_F32:
                    ::memcpy(dptr, src, samples * sizeof(f32_t));
                    return true;
                case SFMT_F64: CVT_FX_TO_FX(f32_t, f64_t)               return true;

                default:
                    break;
            }

            return false;
        }

        bool convert_to_f64(void *dst, void *src, size_t samples, size_t to, size_t from)
        {
            f64_t *dptr = static_cast<f64_t *>(dst);

            switch (sformat_format(from))
            {
                case SFMT_U8:  CVT_UI_TO_FX(f64_t, int8_t)              return true;
                case SFMT_S8:  CVT_SI_TO_FX(f64_t, int8_t)              return true;

                case SFMT_U16: CVT_UI_TO_FX(f64_t, int16_t)             return true;
                case SFMT_S16: CVT_SI_TO_FX(f64_t, int16_t)             return true;

                case SFMT_U24: CVT_U24_TO_FX(f64_t)                     return true;
                case SFMT_S24: CVT_S24_TO_FX(f64_t)                     return true;

                case SFMT_U32: CVT_UI_TO_FX(f64_t, int32_t)             return true;
                case SFMT_S32: CVT_SI_TO_FX(f64_t, int32_t)             return true;

                case SFMT_F32: CVT_FX_TO_FX(f64_t, f32_t)               return true;
                case SFMT_F64:
                    ::memcpy(dptr, src, samples * sizeof(f64_t));
                    return true;

                default:
                    break;
            }

            return false;
        }

        bool convert_samples(void *dst, void *src, size_t samples, size_t to, size_t from)
        {
            // Convert source sample endianess
            if (!sample_endian_swap(src, samples, from))
                return false;

            // Apply sample conversion
            switch (sformat_format(to))
            {
                case SFMT_U8:
                case SFMT_S8:
                    return convert_to_8bit(dst, src, samples, to, from);

                case SFMT_U16:
                case SFMT_S16:
                    return convert_to_16bit(dst, src, samples, to, from);

                case SFMT_U24:
                case SFMT_S24:
                    return convert_to_24bit(dst, src, samples, to, from);

                case SFMT_U32:
                case SFMT_S32:
                    return convert_to_32bit(dst, src, samples, to, from);

                case SFMT_F32:
                    return convert_to_f32(dst, src, samples, to, from);

                case SFMT_F64:
                    return convert_to_f64(dst, src, samples, to, from);

                default:
                    break;
            }

            // Convert target sample endianess
            if (!sample_endian_swap(dst, samples, to))
                return false;

            return false;
        }


    } /* namespace mm */
} /* namespace lsp */


