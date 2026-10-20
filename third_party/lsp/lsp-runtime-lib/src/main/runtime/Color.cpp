/*
 * Copyright (C) 2026 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2026 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 05 нояб. 2015 г.
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

#include <lsp-plug.in/runtime/Color.h>
#include <lsp-plug.in/stdlib/locale.h>
#include <lsp-plug.in/stdlib/math.h>
#include <lsp-plug.in/stdlib/stdio.h>
#include <lsp-plug.in/stdlib/string.h>

#include <ctype.h>
#include <errno.h>

namespace lsp
{
    static constexpr float HSL_RGB_1_3      = 1.0f / 3.0f;
    static constexpr float HSL_RGB_1_6      = 1.0f / 6.0f;
    static constexpr float HSL_RGB_2_3      = 2.0f / 3.0f;
    static constexpr float FLOAT_COL_COEFF  = 255.0f;
    static constexpr float FLOAT_COL_BIAS   = 0.25f;
    static constexpr float FLOAT_RCOL_COEFF = 1.0f / FLOAT_COL_COEFF;
    static constexpr float DEG_TO_RAD       = M_PI / 180.0f;
    static constexpr float RAD_TO_DEG       = 180.0f / M_PI;
    static constexpr float ONE_DIV_360      = 1.0f / 360.0f;
    static constexpr float ONE_DIV_116      = 1.0f / 116.0f;
    static constexpr float ONE_DIV_3        = 1.0f / 3.0f;
    static constexpr float ONE_DIV_2p4      = 1.0f / 2.4f;
    static constexpr float ONE_DIV_1p055    = 1.0f / 1.055f;
    static constexpr float ONE_DIV_12p92    = 1.0f / 12.92f;
    static constexpr float DIV_16_BY_116    = 16.0f / 116.0f;
    static constexpr float DIV_ONE_BY_7p787 = 1.0f / 7.787f;
    static constexpr float ONE_DIV_95p047   = 1.0f / 95.047f;
    static constexpr float ONE_DIV_108p883  = 1.0f / 108.883f;

    static const char *skip_whitespace(const char *src, const char *end) noexcept
    {
        for ( ; src < end; ++src)
        {
            switch (*src)
            {
                case '\n':
                case '\t':
                case '\r':
                case ' ':
                    break;
                case '\0':
                    return end;
                default:
                    return src;
            }
        }

        return src;
    }

    static const char *match_prefix(const char *src, const char *end, const char *prefix) noexcept
    {
        for ( ; src < end; ++src)
        {
            if (*prefix == '\0')
                return src;
            if (tolower(*src) != *(prefix++))
                return end;
        }
        return src;
    }

    static const char *match_char(const char *src, const char *end, char ch) noexcept
    {
        if (src < end)
            return (src[0] == ch) ? &src[1] : end;
        return end;
    }

    static const char *match_chars(const char *src, const char *end, const char *allowed) noexcept
    {
        for ( ; src < end; ++src)
        {
            char c = *src;
            if (c == '\0')
                return src;

            // Lookup list of allowed characters
            for (const char *ck = allowed; *ck != '\0'; ++ck)
            {
                if (*ck == c)
                    return src;
            }
        }
        return src;
    }


    static size_t hex_char(char c) noexcept
    {
        if ((c >= '0') && (c <= '9'))
            return c - '0';
        if ((c >= 'a') && (c <= 'f'))
            return c - 'a' + 10;
        if ((c >= 'A') && (c <= 'F'))
            return c - 'A' + 10;
        return 0xff;
    }

    static const char *skip_hex(const char *ptr, const char *end) noexcept
    {
        for ( ; ptr < end; ++ptr)
        {
            if (hex_char(*ptr) >= 16)
                return ptr;
        }
        return end;
    }

    inline float Color::clamp(float x) noexcept
    {
        return lsp_limit(x, 0.0f, 1.0f);
    }

    Color::Color() noexcept
    {
        rgb.R   = 0.0f;
        rgb.G   = 0.0f;
        rgb.B   = 0.0f;
        hsl.H   = 0.0f;
        hsl.S   = 0.0f;
        hsl.L   = 0.0f;
        xyz.X   = 0.0f;
        xyz.Y   = 0.0f;
        xyz.Z   = 0.0f;
        lab.L   = 0.0f;
        lab.A   = 0.0f;
        lab.B   = 0.0f;
        lch.L   = 0.0f;
        lch.C   = 0.0f;
        lch.H   = 0.0f;
        cmyk.C  = 0.0f;
        cmyk.M  = 0.0f;
        cmyk.Y  = 0.0f;
        cmyk.K  = 0.0f;
        mask    = M_RGB;
        A       = 0.0f;
    }

    Color::Color(float r, float g, float b) noexcept
    {
        set_rgb(r, g, b);
        hsl.H   = 0.0f;
        hsl.S   = 0.0f;
        hsl.L   = 0.0f;
        xyz.X   = 0.0f;
        xyz.Y   = 0.0f;
        xyz.Z   = 0.0f;
        lab.L   = 0.0f;
        lab.A   = 0.0f;
        lab.B   = 0.0f;
        lch.L   = 0.0f;
        lch.C   = 0.0f;
        lch.H   = 0.0f;
        cmyk.C  = 0.0f;
        cmyk.M  = 0.0f;
        cmyk.Y  = 0.0f;
        cmyk.K  = 0.0f;
        A       = 0.0f;
    }

    Color::Color(float r, float g, float b, float a) noexcept
    {
        set_rgba(r, g, b, a);
        hsl.H   = 0.0f;
        hsl.S   = 0.0f;
        hsl.L   = 0.0f;
        xyz.X   = 0.0f;
        xyz.Y   = 0.0f;
        xyz.Z   = 0.0f;
        lab.L   = 0.0f;
        lab.A   = 0.0f;
        lab.B   = 0.0f;
        lch.L   = 0.0f;
        lch.C   = 0.0f;
        lch.H   = 0.0f;
        cmyk.C  = 0.0f;
        cmyk.M  = 0.0f;
        cmyk.Y  = 0.0f;
        cmyk.K  = 0.0f;
    }

    Color::Color(const Color &src) noexcept
    {
        rgb     = src.rgb;
        hsl     = src.hsl;
        xyz     = src.xyz;
        lab     = src.lab;
        lch     = src.lch;
        cmyk    = src.cmyk;
        mask    = src.mask;
        A       = src.A;
    }

    Color::Color(Color &&src) noexcept
    {
        rgb     = src.rgb;
        hsl     = src.hsl;
        xyz     = src.xyz;
        lab     = src.lab;
        lch     = src.lch;
        cmyk    = src.cmyk;
        mask    = src.mask;
        A       = src.A;
    }

    Color &Color::operator = (const Color & src) noexcept
    {
        rgb     = src.rgb;
        hsl     = src.hsl;
        xyz     = src.xyz;
        lab     = src.lab;
        lch     = src.lch;
        cmyk    = src.cmyk;
        mask    = src.mask;
        A       = src.A;

        return *this;
    }

    Color &Color::operator = (Color && src) noexcept
    {
        rgb     = src.rgb;
        hsl     = src.hsl;
        xyz     = src.xyz;
        lab     = src.lab;
        lch     = src.lch;
        cmyk    = src.cmyk;
        mask    = src.mask;
        A       = src.A;

        return *this;
    }

    Color::Color(const Color &src, float a) noexcept
    {
        rgb     = src.rgb;
        hsl     = src.hsl;
        xyz     = src.xyz;
        lab     = src.lab;
        lch     = src.lch;
        cmyk    = src.cmyk;
        mask    = src.mask;
        A       = clamp(a);
    }

    Color::Color(const Color *src) noexcept
    {
        rgb     = src->rgb;
        hsl     = src->hsl;
        xyz     = src->xyz;
        lab     = src->lab;
        lch     = src->lch;
        cmyk    = src->cmyk;
        mask    = src->mask;
        A       = src->A;
    }

    Color::Color(const Color *src, float a) noexcept
    {
        rgb     = src->rgb;
        hsl     = src->hsl;
        xyz     = src->xyz;
        lab     = src->lab;
        lch     = src->lch;
        cmyk    = src->cmyk;
        mask    = src->mask;
        A       = clamp(a);
    }

    Color::Color(uint32_t rgb) noexcept
    {
        set_rgb24(rgb);
        hsl.H   = 0.0f;
        hsl.S   = 0.0f;
        hsl.L   = 0.0f;
        xyz.X   = 0.0f;
        xyz.Y   = 0.0f;
        xyz.Z   = 0.0f;
        lab.L   = 0.0f;
        lab.A   = 0.0f;
        lab.B   = 0.0f;
        lch.L   = 0.0f;
        lch.C   = 0.0f;
        lch.H   = 0.0f;
        cmyk.C  = 0.0f;
        cmyk.M  = 0.0f;
        cmyk.Y  = 0.0f;
        cmyk.K  = 0.0f;
    }

    Color::Color(uint32_t rgb, float a) noexcept
    {
        set_rgb24(rgb);
        hsl.H   = 0.0f;
        hsl.S   = 0.0f;
        hsl.L   = 0.0f;
        xyz.X   = 0.0f;
        xyz.Y   = 0.0f;
        xyz.Z   = 0.0f;
        lab.L   = 0.0f;
        lab.A   = 0.0f;
        lab.B   = 0.0f;
        lch.L   = 0.0f;
        lch.C   = 0.0f;
        lch.H   = 0.0f;
        cmyk.C  = 0.0f;
        cmyk.M  = 0.0f;
        cmyk.Y  = 0.0f;
        cmyk.K  = 0.0f;
        A       = a;
    }

    Color &Color::copy(const Color &c) noexcept
    {
        rgb     = c.rgb;
        hsl     = c.hsl;
        xyz     = c.xyz;
        lab     = c.lab;
        cmyk    = c.cmyk;
        A       = c.A;
        mask    = c.mask;

        return *this;
    }

    Color &Color::copy(const Color *c) noexcept
    {
        rgb     = c->rgb;
        hsl     = c->hsl;
        xyz     = c->xyz;
        lab     = c->lab;
        lch     = c->lch;
        cmyk    = c->cmyk;
        A       = c->A;
        mask    = c->mask;

        return *this;
    }

    Color &Color::copy(const Color &c, float a) noexcept
    {
        rgb     = c.rgb;
        hsl     = c.hsl;
        xyz     = c.xyz;
        lab     = c.lab;
        lch     = c.lch;
        cmyk    = c.cmyk;
        A       = clamp(a);
        mask    = c.mask;

        return *this;
    }

    Color &Color::copy(const Color *c, float a) noexcept
    {
        rgb     = c->rgb;
        hsl     = c->hsl;
        xyz     = c->xyz;
        lab     = c->lab;
        lch     = c->lch;
        cmyk    = c->cmyk;
        A       = clamp(a);
        mask    = c->mask;

        return *this;
    }

    void Color::swap(Color *c) noexcept
    {
        lsp::swap(rgb, c->rgb);
        lsp::swap(hsl, c->hsl);
        lsp::swap(xyz, c->xyz);
        lsp::swap(lab, c->lab);
        lsp::swap(lch, c->lch);
        lsp::swap(cmyk, c->cmyk);
        lsp::swap(A, c->A);
        lsp::swap(mask, c->mask);
    }

    Color &Color::red(float r) noexcept
    {
        calc_rgb().R    = clamp(r);
        mask            = M_RGB;
        return *this;
    }

    Color &Color::green(float g) noexcept
    {
        calc_rgb().G    = clamp(g);
        mask            = M_RGB;
        return *this;
    }

    Color &Color::blue(float b) noexcept
    {
        calc_rgb().B    = clamp(b);
        mask            = M_RGB;
        return *this;
    }

    Color &Color::hue(float h) noexcept
    {
        calc_hsl().H    = clamp(h);
        mask            = M_HSL;
        return *this;
    }

    Color &Color::saturation(float s) noexcept
    {
        calc_hsl().S    = clamp(s);
        mask            = M_HSL;
        return *this;
    }

    Color &Color::lightness(float l) noexcept
    {
        calc_hsl().L    = clamp(l);
        mask            = M_HSL;
        return *this;
    }

    Color &Color::hsl_hue(float h) noexcept
    {
        calc_hsl().H    = clamp(h);
        mask            = M_HSL;
        return *this;
    }

    Color &Color::hsl_saturation(float s) noexcept
    {
        calc_hsl().S    = clamp(s);
        mask            = M_HSL;
        return *this;
    }

    Color &Color::hsl_lightness(float l) noexcept
    {
        calc_hsl().L    = clamp(l);
        mask            = M_HSL;
        return *this;
    }

    Color &Color::alpha(float a) noexcept
    {
        A       = a;
        return *this;
    }

    Color &Color::opacity(float o) noexcept
    {
        A       = 1.0f - o;
        return *this;
    }

    Color &Color::set_rgb24(uint32_t v) noexcept
    {
        rgb.R   = ((v >> 16) & 0xff) * FLOAT_RCOL_COEFF;
        rgb.G   = ((v >> 8) & 0xff) * FLOAT_RCOL_COEFF;
        rgb.B   = (v & 0xff) * FLOAT_RCOL_COEFF;
        A       = 0.0f;
        mask    = M_RGB;

        return *this;
    }

    Color &Color::set_rgba32(uint32_t v) noexcept
    {
        rgb.R   = ((v >> 16) & 0xff) * FLOAT_RCOL_COEFF;
        rgb.G   = ((v >> 8) & 0xff) * FLOAT_RCOL_COEFF;
        rgb.B   = (v & 0xff) * FLOAT_RCOL_COEFF;
        A       = ((v >> 24) & 0xff) * FLOAT_RCOL_COEFF;
        mask    = M_RGB;

        return *this;
    }

    Color &Color::set_dev_rgba32(uint32_t v) noexcept
    {
        rgb.R   = ((v >> 16) & 0xff) * FLOAT_RCOL_COEFF;
        rgb.G   = ((v >> 8) & 0xff) * FLOAT_RCOL_COEFF;
        rgb.B   = (v & 0xff) * FLOAT_RCOL_COEFF;
        A       = 1.0f - ((v >> 24) & 0xff) * FLOAT_RCOL_COEFF;
        mask    = M_RGB;

        return *this;
    }

    Color &Color::set_prgba32(uint32_t v) noexcept
    {
        const uint32_t int_a = ((v >> 24) & 0xff);
        if (int_a > 0)
        {
            const float a = int_a * FLOAT_RCOL_COEFF;
            const float ra = FLOAT_RCOL_COEFF / a;

            rgb.R   = ((v >> 16) & 0xff) * ra;
            rgb.G   = ((v >> 8) & 0xff) * ra;
            rgb.B   = (v & 0xff) * ra;
            A       = 1.0f - a;
        }
        else
        {
            rgb.R   = 0.0f;
            rgb.G   = 0.0f;
            rgb.B   = 0.0f;
            A       = 1.0f;
        }
        mask    = M_RGB;

        return *this;
    }

    Color &Color::set_hsl24(uint32_t v) noexcept
    {
        hsl.H   = ((v >> 16) & 0xff) * FLOAT_RCOL_COEFF;
        hsl.S   = ((v >> 8) & 0xff) * FLOAT_RCOL_COEFF;
        hsl.L   = (v & 0xff) * FLOAT_RCOL_COEFF;
        A       = 0.0f;
        mask    = M_HSL;

        return *this;
    }

    Color &Color::set_hsla32(uint32_t v) noexcept
    {
        hsl.H   = ((v >> 16) & 0xff) * FLOAT_RCOL_COEFF;
        hsl.S   = ((v >> 8) & 0xff) * FLOAT_RCOL_COEFF;
        hsl.L   = (v & 0xff) * FLOAT_RCOL_COEFF;
        A       = ((v >> 24) & 0xff) * FLOAT_RCOL_COEFF;
        mask    = M_HSL;

        return *this;
    }

    bool Color::hsl_to_rgb() const noexcept
    {
        if (!(mask & M_HSL))
            return false;

        // Convert HSL to RGB
        if (hsl.S > 0.0f)
        {
            float Q     = (hsl.L < 0.5f) ? hsl.L * (1.0f + hsl.S) : hsl.L + hsl.S - hsl.L*hsl.S;
            float P     = hsl.L + hsl.L - Q; // 2.0 * L - Q
            float D     = 6.0f * (Q - P);

            float TR    = hsl.H + HSL_RGB_1_3;
            float TG    = hsl.H;
            float TB    = hsl.H - HSL_RGB_1_3;

            if (TR > 1.0f)
                TR  -= 1.0f;
            if (TB < 0.0f)
                TB  += 1.0f;

            if (TR < 0.5f)
                rgb.R       = (TR < HSL_RGB_1_6) ? P + D * TR : Q;
            else
                rgb.R       = (TR < HSL_RGB_2_3) ? P + D * (HSL_RGB_2_3 -  TR) : P;

            if (TG < 0.5f)
                rgb.G       = (TG < HSL_RGB_1_6) ? P + D * TG : Q;
            else
                rgb.G       = (TG < HSL_RGB_2_3) ? P + D * (HSL_RGB_2_3 -  TG) : P;

            if (TB < 0.5f)
                rgb.B       = (TB < HSL_RGB_1_6) ? P + D * TB : Q;
            else
                rgb.B       = (TB < HSL_RGB_2_3) ? P + D * (HSL_RGB_2_3 -  TB) : P;
        }
        else
        {
            rgb.R       = hsl.L;
            rgb.G       = hsl.L;
            rgb.B       = hsl.L;
        }

        mask       |= M_RGB;

        return true;
    }

    bool Color::xyz_to_rgb() const noexcept
    {
        if (!(mask & M_XYZ))
            return false;

        // Convert XYZ to RGB
        float r     = xyz.X *  0.032406f + xyz.Y * -0.015372f + xyz.Z * -0.004986f;
        float g     = xyz.X * -0.009689f + xyz.Y *  0.018758f + xyz.Z *  0.000415f;
        float b     = xyz.X *  0.000557f + xyz.Y * -0.002040f + xyz.Z *  0.010570f;

        r           = (r > 0.0031308f) ? 1.055f * (powf(r, ONE_DIV_2p4)) - 0.055f : ONE_DIV_12p92 * r;
        g           = (g > 0.0031308f) ? 1.055f * (powf(g, ONE_DIV_2p4)) - 0.055f : ONE_DIV_12p92 * g;
        b           = (b > 0.0031308f) ? 1.055f * (powf(b, ONE_DIV_2p4)) - 0.055f : ONE_DIV_12p92 * b;

        rgb.R       = clamp(r);
        rgb.G       = clamp(g);
        rgb.B       = clamp(b);

        mask       |= M_RGB;

        return true;
    }

    bool Color::lab_to_xyz() const noexcept
    {
        if (!(mask & M_LAB))
            return false;

        float y     = (lab.L * ONE_DIV_116) + DIV_16_BY_116;
        float x     = (lab.A * 0.002f) + y;
        float z     = (lab.B * -0.005f) + y;

        float y3    = y*y*y;
        float x3    = x*x*x;
        float z3    = z*z*z;

        y           = (y3 > 0.008856f) ? y3 : (y - DIV_16_BY_116) * DIV_ONE_BY_7p787;
        x           = (x3 > 0.008856f) ? x3 : (x - DIV_16_BY_116) * DIV_ONE_BY_7p787;
        z           = (z3 > 0.008856f) ? z3 : (z - DIV_16_BY_116) * DIV_ONE_BY_7p787;

        xyz.X       = x * 95.047f;
        xyz.Y       = y * 100.0f;
        xyz.Z       = z * 108.883f;

        mask       |= M_XYZ;

        return true;
    }

    bool Color::lch_to_lab() const noexcept
    {
        if (!(mask & M_LCH))
            return false;

        lab.L       = lch.L;
        lab.A       = cosf(lch.H * DEG_TO_RAD) * lch.C;
        lab.B       = sinf(lch.H * DEG_TO_RAD) * lch.C;

        mask       |= M_LAB;

        return true;
    }

    bool Color::cmyk_to_rgb() const noexcept
    {
        if (!(mask & M_CMYK))
            return false;

        float k1    = 1.0f - cmyk.K;
        rgb.R       = k1 - cmyk.C * k1;
        rgb.G       = k1 - cmyk.M * k1;
        rgb.B       = k1 - cmyk.Y * k1;

        mask       |= M_CMYK;

        return true;
    }

    Color::rgb_t &Color::calc_rgb() const noexcept
    {
        // Check if RGB is present
        if (mask & M_RGB)
            return rgb;

        // Try to convert HSL -> RGB
        if (hsl_to_rgb())
            return rgb;

        // Try to convert XYZ -> RGB
        if (xyz_to_rgb())
            return rgb;

        // Try to convert LAB -> XYZ -> RGB
        if (lab_to_xyz())
        {
            if (xyz_to_rgb())
                return rgb;
        }

        // Try to convert LCH -> LAB -> XYZ -> RGB
        if (lch_to_lab())
        {
            if (lab_to_xyz())
            {
                if (xyz_to_rgb())
                    return rgb;
            }
        }

        // Try to convert CMYK -> RGB
        if (cmyk_to_rgb())
            return rgb;

        // Fallback case
        mask |= M_RGB;

        return rgb;
    }

    Color::hsl_t &Color::calc_hsl() const noexcept
    {
        if (mask & M_HSL)
            return hsl;

        // At this moment we can convert color to HSL only from RGB
        calc_rgb();

        float cmax = lsp_max(rgb.R, rgb.G, rgb.B);
        float cmin = lsp_min(rgb.R, rgb.G, rgb.B);
        float d = cmax - cmin;

        hsl.H   = 0.0f;
        hsl.S   = 0.0f;
        hsl.L   = 0.5f * (cmax + cmin);

        // Calculate hue
        if (rgb.R == cmax)
        {
            hsl.H = (rgb.G - rgb.B) / d;
            if (rgb.G < rgb.B)
                hsl.H += 6.0f;
        }
        else if (rgb.G == cmax)
            hsl.H = (rgb.B - rgb.R) / d + 2.0f;
        else if (rgb.B == cmax)
            hsl.H = (rgb.R - rgb.G) / d + 4.0f;

        // Calculate saturation
        if (hsl.L <= 0.5f)
            hsl.S = (hsl.L <= 0.0f) ? 0.0f : 0.5f * d / hsl.L;
        else if (hsl.L > 0.5f)
            hsl.S = (hsl.L < 1.0f) ? 0.5f * d / (1.0f - hsl.L) : 0.0f;

        // Normalize hue
        hsl.H      *= HSL_RGB_1_6;

        mask       |= M_HSL;

        return hsl;
    }

    Color::xyz_t &Color::calc_xyz() const noexcept
    {
        if (mask & M_XYZ)
            return xyz;

        // At this moment we can convert color to XYZ only from RGB
        calc_rgb();

        float r     = (rgb.R > 0.04045f) ? powf((rgb.R + 0.055f) * ONE_DIV_1p055, 2.4f) : rgb.R * ONE_DIV_12p92;
        float g     = (rgb.G > 0.04045f) ? powf((rgb.G + 0.055f) * ONE_DIV_1p055, 2.4f) : rgb.G * ONE_DIV_12p92;
        float b     = (rgb.B > 0.04045f) ? powf((rgb.B + 0.055f) * ONE_DIV_1p055, 2.4f) : rgb.B * ONE_DIV_12p92;

        xyz.X       = r * 41.24f + g * 35.76f + b * 18.05f;
        xyz.Y       = r * 21.26f + g * 71.52f + b *  7.22f;
        xyz.Z       = r *  1.93f + g * 11.92f + b * 95.05f;

        mask       |= M_XYZ;

        return xyz;
    }

    Color::lab_t &Color::calc_lab() const noexcept
    {
        if (mask & M_LAB)
            return lab;

        // At this moment we can convert color to LAB only from XYZ
        calc_xyz();

        float x     = xyz.X * ONE_DIV_95p047;
        float y     = xyz.Y * 0.01f;;
        float z     = xyz.Z * ONE_DIV_108p883;

        x           = (x > 0.008856f) ? powf(x, ONE_DIV_3) : (7.787f * x) + DIV_16_BY_116;
        y           = (y > 0.008856f) ? powf(y, ONE_DIV_3) : (7.787f * y) + DIV_16_BY_116;
        z           = (z > 0.008856f) ? powf(z, ONE_DIV_3) : (7.787f * z) + DIV_16_BY_116;

        lab.L       = (116.0f * y) - 16.0f;
        lab.A       = 500.0f * (x - y);
        lab.B       = 200.0f * (y - z);

        mask       |= M_LAB;

        return lab;
    }

    Color::lch_t &Color::calc_lch() const noexcept
    {
        if (mask & M_LCH)
            return lch;

        // At this moment we can convert color to LCH only from LAB
        calc_lab();

        float h     = atan2f(lab.B, lab.A) * RAD_TO_DEG;
        if (h < 0)
            h          += 360.0f;

        lch.L       = lab.L;
        lch.C       = sqrtf(lab.A * lab.A + lab.B * lab.B);
        lch.H       = h;

        mask       |= M_LCH;

        return lch;
    }

    Color::cmyk_t &Color::calc_cmyk() const noexcept
    {
        if (mask & M_CMYK)
            return cmyk;

        float C = 1.0f - rgb.R;
        float M = 1.0f - rgb.G;
        float Y = 1.0f - rgb.B;

        cmyk.K  = lsp_min(C, M, Y);
        if (cmyk.K >= 1.0f)
        {
            cmyk.C  = 0.0f;
            cmyk.M  = 0.0f;
            cmyk.Y  = 0.0f;
        }
        else
        {
            float rk    = 1.0f / (1.0f - cmyk.K);

            cmyk.C  = (C - cmyk.K) * rk;
            cmyk.M  = (M - cmyk.K) * rk;
            cmyk.Y  = (Y - cmyk.K) * rk;
        }

        mask       |= M_CMYK;

        return cmyk;
    }

    Color &Color::set_rgb(float r, float g, float b) noexcept
    {
        mask    = M_RGB;
        rgb.R   = clamp(r);
        rgb.G   = clamp(g);
        rgb.B   = clamp(b);

        return *this;
    }

    Color &Color::set_rgba(float r, float g, float b, float a) noexcept
    {
        mask    = M_RGB;
        rgb.R   = clamp(r);
        rgb.G   = clamp(g);
        rgb.B   = clamp(b);
        A       = clamp(a);

        return *this;
    }

    Color &Color::set_hsl(float h, float s, float l) noexcept
    {
        mask    = M_HSL;
        hsl.H   = clamp(h);
        hsl.S   = clamp(s);
        hsl.L   = clamp(l);

        return *this;
    }

    Color &Color::set_hsla(float h, float s, float l, float a) noexcept
    {
        mask    = M_HSL;
        hsl.H   = clamp(h);
        hsl.S   = clamp(s);
        hsl.L   = clamp(l);
        A       = clamp(a);

        return *this;
    }

    const Color &Color::get_rgb(float &r, float &g, float &b) const noexcept
    {
        calc_rgb();
        r       = rgb.R;
        g       = rgb.G;
        b       = rgb.B;

        return *this;
    }

    const Color &Color::get_rgba(float &r, float &g, float &b, float &a) const noexcept
    {
        calc_rgb();
        r       = rgb.R;
        g       = rgb.G;
        b       = rgb.B;
        a       = A;

        return *this;
    }

    const Color &Color::get_rgbo(float &r, float &g, float &b, float &o) const noexcept
    {
        calc_rgb();
        r       = rgb.R;
        g       = rgb.G;
        b       = rgb.B;
        o       = 1.0f - A;

        return *this;
    }

    const Color &Color::get_hsl(float &h, float &s, float &l) const noexcept
    {
        calc_hsl();
        h       = hsl.H;
        s       = hsl.S;
        l       = hsl.L;

        return *this;
    }

    const Color &Color::get_hsla(float &h, float &s, float &l, float &a) const noexcept
    {
        calc_hsl();
        h       = hsl.H;
        s       = hsl.S;
        l       = hsl.L;
        a       = A;

        return *this;
    }

    Color &Color::get_rgb(float &r, float &g, float &b) noexcept
    {
        calc_rgb();
        r       = rgb.R;
        g       = rgb.G;
        b       = rgb.B;

        return *this;
    }

    Color &Color::get_rgba(float &r, float &g, float &b, float &a) noexcept
    {
        calc_rgb();
        r       = rgb.R;
        g       = rgb.G;
        b       = rgb.B;
        a       = A;

        return *this;
    }

    Color &Color::get_hsl(float &h, float &s, float &l) noexcept
    {
        calc_hsl();
        h       = hsl.H;
        s       = hsl.S;
        l       = hsl.L;

        return *this;
    }

    Color &Color::get_hsla(float &h, float &s, float &l, float &a) noexcept
    {
        calc_hsl();
        h       = hsl.H;
        s       = hsl.S;
        l       = hsl.L;
        a       = A;

        return *this;
    }


    Color &Color::blend(const Color &c, float alpha) noexcept
    {
        float r1, g1, b1, r2, g2, b2;
        get_rgb(r1, g1, b1);
        c.get_rgb(r2, g2, b2);
        set_rgb(r2 + (r1 - r2) * alpha, g2 + (g1 - g2) * alpha, b2 + (b1 - b2) * alpha);

        return *this;
    }

    Color &Color::blend(float r, float g, float b, float alpha) noexcept
    {
        float r1, g1, b1;
        get_rgb(r1, g1, b1);
        set_rgb(r + (r1 - r) * alpha, g + (g1 - g) * alpha, b + (b1 - b) * alpha);

        return *this;
    }

    Color &Color::blend(const Color &c1, const Color &c2, float alpha) noexcept
    {
        float r1, g1, b1, r2, g2, b2;
        c1.get_rgb(r1, g1, b1);
        c2.get_rgb(r2, g2, b2);
        set_rgb(r2 + (r1 - r2) * alpha, g2 + (g1 - g2) * alpha, b2 + (b1 - b2) * alpha);

        return *this;
    }

    Color &Color::darken(float amount) noexcept
    {
        float r, g, b;
        get_rgb(r, g, b);

        float a = 1.0 - amount;
        set_rgb(r * a, g * a, b * a);

        return *this;
    }

    Color &Color::lighten(float amount) noexcept
    {
        float r, g, b;
        get_rgb(r, g, b);

        float a = 1.0 - amount;
        set_rgb(r + (1.0 - r) * a, g + (1.0 - g) * a, b + (1.0 - b) * a);

        return *this;
    }

    void Color::scale_hsl_lightness(float amount) noexcept
    {
        calc_hsl();
        hsl.L   = clamp(amount * hsl.L);
        mask    = M_HSL;
    }

    void Color::scale_lch_luminance(float amount) noexcept
    {
        calc_lch();
        lch.L   = lsp_limit(amount * lch.L, 0.0f, 100.0f);
        mask    = M_LCH;
    }

    ssize_t Color::format(char *dst, size_t len, size_t tolerance, const float *v, char prefix, bool alpha) noexcept
    {
        if ((dst == NULL) || (tolerance <= 0) || (tolerance > 4))
            return -STATUS_BAD_ARGUMENTS;
        size_t required = (tolerance * (alpha ? 4 : 3)) + 2; // Number of hex characters x number of colors + 2 symbols
        if (len < required)
            return -STATUS_OVERFLOW;

        // Calculate maximum value
        const char *fmt;
        int tol;

        if (alpha)
        {
            switch (tolerance)
            {
                case 1:
                    fmt = "%c%01x%01x%01x%01x";
                    tol = 0xf;
                    break;
                case 3:
                    fmt = "%c%03x%03x%03x%03x";
                    tol = 0xfff;
                    break;
                case 4:
                    fmt = "%c%04x%04x%04x%04x";
                    tol = 0xffff;
                    break;
                default:
                    fmt = "%c%02x%02x%02x%02x";
                    tol = 0xff;
                    break;
            }

            return ::snprintf(
                    dst, len, fmt, prefix,
                    int(v[3] * tol + 0.25f) & tol,
                    int(v[0] * tol + 0.25f) & tol,
                    int(v[1] * tol + 0.25f) & tol,
                    int(v[2] * tol + 0.25f) & tol
                );
        }
        else
        {
            switch (tolerance)
            {
                case 1:
                    fmt = "%c%01x%01x%01x";
                    tol = 0xf;
                    break;
                case 3:
                    fmt = "%c%03x%03x%03x";
                    tol = 0xfff;
                    break;
                case 4:
                    fmt = "%c%04x%04x%04x";
                    tol = 0xffff;
                    break;
                default:
                    fmt = "%c%02x%02x%02x";
                    tol = 0xff;
                    break;
            }

            return ::snprintf(
                    dst, len, fmt, prefix,
                    int(v[0] * tol + 0.25f) & tol,
                    int(v[1] * tol + 0.25f) & tol,
                    int(v[2] * tol + 0.25f) & tol
                );
        }
    }

    ssize_t Color::format_rgb(char *dst, size_t len, size_t tolerance) const noexcept
    {
        float v[3];
        get_rgb(v[0], v[1], v[2]);
        return format(dst, len, tolerance, v, '#', false);
    }

    ssize_t Color::format_hsl(char *dst, size_t len, size_t tolerance) const noexcept
    {
        float v[3];
        get_hsl(v[0], v[1], v[2]);
        return format(dst, len, tolerance, v, '@', false);
    }

    ssize_t Color::format_rgba(char *dst, size_t len, size_t tolerance) const noexcept
    {
        float v[4];
        get_rgba(v[0], v[1], v[2], v[3]);
        return format(dst, len, tolerance, v, '#', true);
    }

    ssize_t Color::format_hsla(char *dst, size_t len, size_t tolerance) const noexcept
    {
        float v[4];
        get_hsla(v[0], v[1], v[2], v[3]);
        return format(dst, len, tolerance, v, '@', true);
    }

    ssize_t Color::format_rgb(LSPString *dst, size_t tolerance) const noexcept
    {
        char tmp[32];
        ssize_t res = format_rgb(tmp, sizeof(tmp), tolerance);
        if (res < 0)
            return res;
        return (dst->set_ascii(tmp)) ? res : -STATUS_NO_MEM;
    }

    ssize_t Color::format_rgba(LSPString *dst, size_t tolerance) const noexcept
    {
        char tmp[32];
        ssize_t res = format_rgba(tmp, sizeof(tmp), tolerance);
        if (res < 0)
            return res;
        return (dst->set_ascii(tmp)) ? res : -STATUS_NO_MEM;
    }

    ssize_t Color::format_hsl(LSPString *dst, size_t tolerance) const noexcept
    {
        char tmp[32];
        ssize_t res = format_hsl(tmp, sizeof(tmp), tolerance);
        if (res < 0)
            return res;
        return (dst->set_ascii(tmp)) ? res : -STATUS_NO_MEM;
    }

    ssize_t Color::format_hsla(LSPString *dst, size_t tolerance) const noexcept
    {
        char tmp[32];
        ssize_t res = format_hsla(tmp, sizeof(tmp), tolerance);
        if (res < 0)
            return res;
        return (dst->set_ascii(tmp)) ? res : -STATUS_NO_MEM;
    }

    status_t Color::parse_hex(float *dst, size_t n, char prefix, const char *src, size_t len) noexcept
    {
        if (src == NULL)
            return STATUS_BAD_ARGUMENTS;

        const char *end = &src[len];

        src = skip_whitespace(src, end);
        if (src == end)
            return STATUS_NO_DATA;
        if (*(src++) != prefix)
            return STATUS_BAD_FORMAT;
        if (src >= end)
            return STATUS_BAD_FORMAT;

        const char *hex  = skip_hex(src, end);
        const char *tail = skip_whitespace(hex, end);
        if (tail != end)
            return STATUS_BAD_FORMAT;
        size_t hex_len   = hex - src;

        // Determine the length of each component
        if (hex_len % n)
            return STATUS_BAD_FORMAT;
        hex_len /= n;
        if ((hex_len <= 0) || (hex_len > 4))
            return STATUS_BAD_FORMAT;
        float norm = 1.0f / ((0x1 << (hex_len << 2)) - 1);

        // Read components
        while (n--)
        {
            size_t v    = 0;
            for (size_t i=0; i<hex_len; ++i, ++src)
                v   = (v << 4) | hex_char(*src);

            *(dst++)    = v * norm;
        }

        return STATUS_OK;
    }

    status_t Color::parse_numeric(float *dst, size_t nmin, size_t nmax, const char *prefix, const char *src, size_t len) noexcept
    {
        // Save and update locale
        SET_LOCALE_SCOPED(LC_NUMERIC, "C");

        status_t res = parse_cnumeric(dst, nmin, nmax, prefix, src, len);

        return res;
    }

    status_t Color::parse_cnumeric(float *dst, size_t nmin, size_t nmax, const char *prefix, const char *src, size_t len) noexcept
    {
        const char *end = &src[len];

        // Parse beginning of statement
        if ((src = skip_whitespace(src, end)) == end)       // whitespace
            return STATUS_NO_DATA;
        if ((src = match_prefix(src, end, prefix)) == end)  // prefix
            return STATUS_BAD_FORMAT;
        if ((src = skip_whitespace(src, end)) == end)       // whitespace
            return STATUS_BAD_FORMAT;
        if ((src = match_char(src, end, '(')) == end)       // (
            return STATUS_BAD_FORMAT;

        // Main loop: parse digits
        size_t count = 0;
        while (src < end)
        {
            // Skip whitespace
            if ((src = skip_whitespace(src, end)) == end)
                return STATUS_BAD_FORMAT;

            // Find next delimiting character
            const char *next = match_chars(src, end, ",) \t\r\n");
            if (next == end)
                return STATUS_BAD_FORMAT;

            // Overflow of number of max possible values?
            if (count >= nmax)
                return STATUS_BAD_FORMAT;

            // Parse floating-point value
            char *eptr = NULL;
            errno = 0;
            dst[count++] = strtof(src, &eptr);
            if ((errno != 0) || (eptr != next) || (eptr == src))
                return STATUS_BAD_FORMAT;

            // Move parse position
            src     = skip_whitespace(next, end);
            if (src == end)
                return STATUS_BAD_FORMAT;
            char c = *(src++);
            if (c == ')')
                break;
            else if (c != ',')
                return STATUS_BAD_FORMAT;
        }

        // Check that we parsed at least nmin floating-point values
        if (count < nmin)
            return STATUS_BAD_FORMAT;

        // Parse end of statement
        if ((src = skip_whitespace(src, end)) != end)       // end-of-line
            return STATUS_BAD_FORMAT;

        return STATUS_OK;
    }

    status_t Color::parse4(const char *src, size_t len) noexcept
    {
        if (src == NULL)
            return STATUS_BAD_ARGUMENTS;

        const char *end = &src[len];
        src = skip_whitespace(src, end);
        if (src >= end)
            return STATUS_NO_DATA;

        return (*src == '@') ? parse_hsla(src, end - src) : parse_rgba(src, end - src);
    }

    status_t Color::parse3(const char *src, size_t len) noexcept
    {
        if (src == NULL)
            return STATUS_BAD_ARGUMENTS;

        const char *end = &src[len];
        src = skip_whitespace(src, end);
        if (src >= end)
            return STATUS_NO_DATA;

        return (*src == '@') ? parse_hsl(src, end - src) : parse_rgb(src, end - src);
    }

    status_t Color::parse_rgba(const char *src, size_t len) noexcept
    {
        float v[4];
        status_t res = parse_hex(v, 4, '#', src, len);
        if (res == STATUS_OK)
            set_rgba(v[1], v[2], v[3], v[0]);
        return res;
    }

    status_t Color::parse_hsla(const char *src, size_t len) noexcept
    {
        float v[4];
        status_t res = parse_hex(v, 4, '@', src, len);
        if (res == STATUS_OK)
            set_hsla(v[1], v[2], v[3], v[0]);
        return res;
    }

    status_t Color::parse_rgb(const char *src, size_t len) noexcept
    {
        float v[3];
        status_t res = parse_hex(v, 3, '#', src, len);
        if (res == STATUS_OK)
            set_rgba(v[0], v[1], v[2], 0.0f);
        return res;
    }

    status_t Color::parse_hsl(const char *src, size_t len) noexcept
    {
        float v[3];
        status_t res = parse_hex(v, 3, '@', src, len);
        if (res == STATUS_OK)
            set_hsla(v[0], v[1], v[2], 0.0f);
        return res;
    }

    status_t Color::parse(const char *src, size_t len) noexcept
    {
        status_t res;
        float v[5];
        if ((res = parse4(src, len)) == STATUS_OK)
            return res;
        if ((res = parse3(src, len)) == STATUS_OK)
            return res;

        // Save and update locale
        SET_LOCALE_SCOPED(LC_NUMERIC, "C");

        // Try to parse different expressions
        if ((res = parse_cnumeric(v, 3, 3, "rgb", src, len)) == STATUS_OK)
            set_rgba(v[0], v[1], v[2], 0.0f);
        else if ((res = parse_cnumeric(v, 4, 4, "rgba", src, len)) == STATUS_OK)
            set_rgba(v[0], v[1], v[2], v[3]);
        else if ((res = parse_cnumeric(v, 3, 3, "hsl", src, len)) == STATUS_OK)
            set_hsla(v[0] * ONE_DIV_360, v[1] * 0.01f, v[2] * 0.005f, 0.0f);
        else if ((res = parse_cnumeric(v, 4, 4, "hsla", src, len)) == STATUS_OK)
            set_hsla(v[0] * ONE_DIV_360, v[1] * 0.01f, v[2] * 0.005f, v[3]);
        else if ((res = parse_cnumeric(v, 3, 3, "xyz", src, len)) == STATUS_OK)
            set_xyza(v[0], v[1], v[2], 0.0f);
        else if ((res = parse_cnumeric(v, 4, 4, "xyza", src, len)) == STATUS_OK)
            set_xyza(v[0], v[1], v[2], v[3]);
        else if ((res = parse_cnumeric(v, 3, 3, "lab", src, len)) == STATUS_OK)
            set_laba(v[0], v[1], v[2], 0.0f);
        else if ((res = parse_cnumeric(v, 4, 4, "laba", src, len)) == STATUS_OK)
            set_laba(v[0], v[1], v[2], v[3]);
        else if ((res = parse_cnumeric(v, 3, 3, "lch", src, len)) == STATUS_OK)
            set_lcha(v[0], v[1], v[2], 0.0f);
        else if ((res = parse_cnumeric(v, 4, 4, "lcha", src, len)) == STATUS_OK)
            set_lcha(v[0], v[1], v[2], v[3]);
        else if ((res = parse_cnumeric(v, 3, 3, "hcl", src, len)) == STATUS_OK)
            set_lcha(v[2], v[1], v[0], 0.0f);
        else if ((res = parse_cnumeric(v, 4, 4, "hcla", src, len)) == STATUS_OK)
            set_lcha(v[2], v[1], v[0], v[3]);
        else if ((res = parse_cnumeric(v, 4, 4, "cmyk", src, len)) == STATUS_OK)
            set_cmyk(v[0], v[1], v[2], v[3]);
        else if ((res = parse_cnumeric(v, 5, 5, "cmyka", src, len)) == STATUS_OK)
            set_cmyka(v[0], v[1], v[2], v[3], v[4]);

        return res;
    }

    ssize_t Color::format3(char *dst, size_t len) const noexcept
    {
        // Save and update locale
        SET_LOCALE_SCOPED(LC_NUMERIC, "C");

        ssize_t res = 0;
        if (is_rgb())
            res = snprintf(dst, len, "rgb(%.4f, %.4f, %.4f)", rgb.R, rgb.G, rgb.B);
        else if (is_hsl())
            res = snprintf(dst, len, "hsl(%.4f, %.4f, %.4f)", hsl.H * 360.0f, hsl.S * 100.0f, hsl.L * 200.0f);
        else if (is_lch())
            res = snprintf(dst, len, "hcl(%.4f, %.4f, %.4f)", lch.H, lch.C, lch.L);
        else if (is_lab())
            res = snprintf(dst, len, "lab(%.4f, %.4f, %.4f)", lab.L, lab.A, lab.B);
        else if (is_xyz())
            res = snprintf(dst, len, "xyz(%.4f, %.4f, %.4f)", xyz.X, xyz.Y, xyz.Z);
        else if (is_cmyk())
            res = snprintf(dst, len, "cmyk(%.4f, %.4f, %.4f, %.4f)", cmyk.C, cmyk.M, cmyk.Y, cmyk.K);
        else
            res = snprintf(dst, len, "rgb(%.4f, %.4f, %.4f)", rgb.R, rgb.G, rgb.B);

        return res;
    }

    ssize_t Color::format4(char *dst, size_t len) const noexcept
    {
        // Save and update locale
        SET_LOCALE_SCOPED(LC_NUMERIC, "C");

        ssize_t res = 0;
        if (is_rgb())
            res = snprintf(dst, len, "rgba(%.4f, %.4f, %.4f, %.4f)", rgb.R, rgb.G, rgb.B, A);
        else if (is_hsl())
            res = snprintf(dst, len, "hsla(%.4f, %.4f, %.4f, %.4f)", hsl.H * 360.0f, hsl.S * 100.0f, hsl.L * 200.0f, A);
        else if (is_lch())
            res = snprintf(dst, len, "hcla(%.4f, %.4f, %.4f, %.4f)", lch.H, lch.C, lch.L, A);
        else if (is_lab())
            res = snprintf(dst, len, "laba(%.4f, %.4f, %.4f, %.4f)", lab.L, lab.A, lab.B, A);
        else if (is_xyz())
            res = snprintf(dst, len, "xyza(%.4f, %.4f, %.4f, %.4f)", xyz.X, xyz.Y, xyz.Z, A);
        else if (is_cmyk())
            res = snprintf(dst, len, "cmyka(%.4f, %.4f, %.4f, %.4f, %.4f)", cmyk.C, cmyk.M, cmyk.Y, cmyk.K, A);
        else
            res = snprintf(dst, len, "rgba(%.4f, %.4f, %.4f, %.4f)", rgb.R, rgb.G, rgb.B, A);

        return res;
    }

    ssize_t Color::format3(LSPString *dst) const noexcept
    {
        if (dst == NULL)
            return -STATUS_BAD_ARGUMENTS;

        char buf[64];
        ssize_t res = format3(buf, sizeof(buf) / sizeof(char));
        if (res < 0)
            return res;

        return (dst->set_ascii(buf, res)) ? res : -STATUS_NO_MEM;
    }

    ssize_t Color::format4(LSPString *dst) const noexcept
    {
        if (dst == NULL)
            return -STATUS_BAD_ARGUMENTS;

        char buf[64];
        ssize_t res = format3(buf, sizeof(buf) / sizeof(char));
        if (res < 0)
            return res;

        return (dst->set_ascii(buf, res)) ? res : -STATUS_NO_MEM;
    }

    status_t Color::parse_rgba(const char *src) noexcept
    {
        return (src != NULL) ? parse_rgba(src, ::strlen(src)) : STATUS_BAD_ARGUMENTS;
    }

    status_t Color::parse_hsla(const char *src) noexcept
    {
        return (src != NULL) ? parse_hsla(src, ::strlen(src)) : STATUS_BAD_ARGUMENTS;
    }

    status_t Color::parse_rgb(const char *src) noexcept
    {
        return (src != NULL) ? parse_rgb(src, ::strlen(src)) : STATUS_BAD_ARGUMENTS;
    }

    status_t Color::parse_hsl(const char *src) noexcept
    {
        return (src != NULL) ? parse_hsl(src, ::strlen(src)) : STATUS_BAD_ARGUMENTS;
    }

    status_t Color::parse4(const char *src) noexcept
    {
        return (src != NULL) ? parse4(src, ::strlen(src)) : STATUS_BAD_ARGUMENTS;
    }

    status_t Color::parse3(const char *src) noexcept
    {
        return (src != NULL) ? parse3(src, ::strlen(src)) : STATUS_BAD_ARGUMENTS;
    }

    status_t Color::parse(const char *src) noexcept
    {
        return (src != NULL) ? parse(src, ::strlen(src)) : STATUS_BAD_ARGUMENTS;
    }

    uint32_t Color::rgb24() const noexcept
    {
        calc_rgb();
        return
            (uint32_t(rgb.R * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 16) |
            (uint32_t(rgb.G * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 8) |
            (uint32_t(rgb.B * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 0);
    }

    uint32_t Color::rgba32() const noexcept
    {
        calc_rgb();
        return
            (uint32_t(A * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 24) |
            (uint32_t(rgb.R * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 16) |
            (uint32_t(rgb.G * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 8) |
            (uint32_t(rgb.B * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 0);
    }

    uint32_t Color::dev_rgba32() const noexcept
    {
        calc_rgb();
        return
            (uint32_t((1.0f - A) * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 24) |
            (uint32_t(rgb.R * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 16) |
            (uint32_t(rgb.G * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 8) |
            (uint32_t(rgb.B * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 0);
    }

    uint32_t Color::prgba32() const noexcept
    {
        calc_rgb();
        const float a   = 1.0f - A;
        return
            (uint32_t(a * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 24) |
            (uint32_t(rgb.R * a * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 16) |
            (uint32_t(rgb.G * a * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 8) |
            (uint32_t(rgb.B * a * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 0);
    }

    uint32_t Color::hsl24() const noexcept
    {
        calc_hsl();
        return
            (uint32_t(hsl.H * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 16) |
            (uint32_t(hsl.S * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 8) |
            (uint32_t(hsl.L * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 0);
    }

    uint32_t Color::hsla32() const noexcept
    {
        calc_hsl();
        return
            (uint32_t(A * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 24) |
            (uint32_t(hsl.H * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 16) |
            (uint32_t(hsl.S * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 8) |
            (uint32_t(hsl.L * FLOAT_COL_COEFF + FLOAT_COL_BIAS) << 0);
    }

    const Color &Color::get_xyz(float &x, float &y, float &z) const noexcept
    {
        calc_xyz();
        x   = xyz.X;
        y   = xyz.Y;
        z   = xyz.Z;

        return *this;
    }

    Color &Color::get_xyz(float &x, float &y, float &z) noexcept
    {
        calc_xyz();
        x   = xyz.X;
        y   = xyz.Y;
        z   = xyz.Z;

        return *this;
    }

    const Color &Color::get_xyza(float &x, float &y, float &z, float &a) const noexcept
    {
        calc_xyz();
        x   = xyz.X;
        y   = xyz.Y;
        z   = xyz.Z;
        a   = clamp(A);

        return *this;
    }

    Color &Color::get_xyza(float &x, float &y, float &z, float &a) noexcept
    {
        calc_xyz();
        x   = xyz.X;
        y   = xyz.Y;
        z   = xyz.Z;
        a   = clamp(A);

        return *this;
    }

    Color &Color::xyz_x(float x) noexcept
    {
        calc_xyz().X    = x;
        mask    = M_XYZ;
        return *this;
    }

    Color &Color::xyz_y(float y) noexcept
    {
        calc_xyz().Y    = y;
        mask    = M_XYZ;
        return *this;
    }

    Color &Color::xyz_z(float z) noexcept
    {
        calc_xyz().Z    = z;
        mask    = M_XYZ;
        return *this;
    }

    Color &Color::set_xyz(float x, float y, float z) noexcept
    {
        xyz.X   = lsp_limit(x, 0.0f, 100.0f);
        xyz.Y   = lsp_limit(y, 0.0f, 100.0f);
        xyz.Z   = lsp_limit(z, 0.0f, 110.0f);
        mask    = M_XYZ;
        return *this;
    }

    Color &Color::set_xyza(float x, float y, float z, float a) noexcept
    {
        xyz.X   = lsp_limit(x, 0.0f, 100.0f);
        xyz.Y   = lsp_limit(y, 0.0f, 100.0f);
        xyz.Z   = lsp_limit(z, 0.0f, 110.0f);
        A       = clamp(a);
        mask    = M_XYZ;
        return *this;
    }

    const Color &Color::get_lab(float &l, float &a, float &b) const noexcept
    {
        calc_lab();
        l       = lab.L;
        a       = lab.A;
        b       = lab.B;
        return *this;
    }

    Color &Color::get_lab(float &l, float &a, float &b) noexcept
    {
        calc_lab();
        l       = lab.L;
        a       = lab.A;
        b       = lab.B;
        return *this;
    }

    const Color &Color::get_laba(float &l, float &a, float &b, float &alpha) const noexcept
    {
        calc_lab();
        l       = lab.L;
        a       = lab.A;
        b       = lab.B;
        alpha   = clamp(A);
        return *this;
    }

    Color &Color::get_laba(float &l, float &a, float &b, float &alpha) noexcept
    {
        calc_lab();
        l       = lab.L;
        a       = lab.A;
        b       = lab.B;
        alpha   = clamp(A);
        return *this;
    }

    Color &Color::lab_l(float l) noexcept
    {
        calc_lab().L    = l;
        mask            = M_LAB;
        return *this;
    }

    Color &Color::lab_a(float a) noexcept
    {
        calc_lab().A    = a;
        mask            = M_LAB;
        return *this;
    }

    Color &Color::lab_b(float b) noexcept
    {
        calc_lab().B    = b;
        mask            = M_LAB;
        return *this;
    }

    Color &Color::set_lab(float l, float a, float b) noexcept
    {
        lab.L   = l;
        lab.A   = a;
        lab.B   = b;
        mask    = M_LAB;
        return *this;
    }

    Color &Color::set_laba(float l, float a, float b, float alpha) noexcept
    {
        lab.L   = l;
        lab.A   = a;
        lab.B   = b;
        A       = clamp(alpha);
        mask    = M_LAB;
        return *this;
    }

    // Check that LCH data is currently present without need of implicit conversion
    const Color &Color::get_lch(float &l, float &c, float &h) const noexcept
    {
        calc_lch();
        l       = lch.L;
        c       = lch.C;
        h       = lch.H;
        return *this;
    }

    Color &Color::get_lch(float &l, float &c, float &h) noexcept
    {
        calc_lch();
        l       = lch.L;
        c       = lch.C;
        h       = lch.H;
        return *this;
    }

    const Color &Color::get_lcha(float &l, float &c, float &h, float &alpha) const noexcept
    {
        calc_lch();
        l       = lch.L;
        c       = lch.C;
        h       = lch.H;
        alpha   = A;
        return *this;
    }

    Color &Color::get_lcha(float &l, float &c, float &h, float &alpha) noexcept
    {
        calc_lch();
        l       = lch.L;
        c       = lch.C;
        h       = lch.H;
        alpha   = A;
        return *this;
    }

    Color &Color::lch_l(float l) noexcept
    {
        calc_lch().L    = l;
        mask            = M_LCH;
        return *this;
    }

    Color &Color::lch_c(float c) noexcept
    {
        calc_lch().C    = c;
        mask            = M_LCH;
        return *this;
    }

    Color &Color::lch_h(float h) noexcept
    {
        calc_lch().H    = h;
        mask            = M_LCH;
        return *this;
    }

    Color &Color::set_lch(float l, float c, float h) noexcept
    {
        lch.L   = l;
        lch.C   = c;
        lch.H   = h;
        mask    = M_LCH;
        return *this;
    }

    Color &Color::set_lcha(float l, float c, float h, float alpha) noexcept
    {
        lch.L   = l;
        lch.C   = c;
        lch.H   = h;
        A       = clamp(alpha);
        mask    = M_LCH;
        return *this;
    }

    const Color &Color::get_hcl(float &h, float &c, float &l) const noexcept
    {
        calc_lch();
        l       = lch.L;
        c       = lch.C;
        h       = lch.H;
        return *this;
    }

    Color &Color::get_hcl(float &h, float &c, float &l) noexcept
    {
        calc_lch();
        l       = lch.L;
        c       = lch.C;
        h       = lch.H;
        return *this;
    }

    const Color &Color::get_hcla(float &h, float &c, float &l, float &alpha) const noexcept
    {
        calc_lch();
        l       = lch.L;
        c       = lch.C;
        h       = lch.H;
        alpha   = clamp(A);
        return *this;
    }

    Color &Color::get_hcla(float &h, float &c, float &l, float &alpha) noexcept
    {
        calc_lch();
        l       = lch.L;
        c       = lch.C;
        h       = lch.H;
        alpha   = clamp(A);
        return *this;
    }

    Color &Color::hcl_l(float l) noexcept
    {
        calc_lch().L    = l;
        mask            = M_LCH;
        return *this;
    }

    Color &Color::hcl_c(float c) noexcept
    {
        calc_lch().C    = c;
        mask            = M_LCH;
        return *this;
    }

    Color &Color::hcl_h(float h) noexcept
    {
        calc_lch().H    = h;
        mask            = M_LCH;
        return *this;
    }

    Color &Color::set_hcl(float h, float c, float l) noexcept
    {
        lch.L   = l;
        lch.C   = c;
        lch.H   = h;
        mask    = M_LCH;
        return *this;
    }

    Color &Color::set_hcla(float h, float c, float l, float alpha) noexcept
    {
        lch.L   = l;
        lch.C   = c;
        lch.H   = h;
        A       = alpha;
        mask    = M_LCH;
        return *this;
    }

    const Color &Color::get_cmyk(float &c, float &m, float &y, float &k) const noexcept
    {
        calc_cmyk();
        c       = cmyk.C;
        m       = cmyk.M;
        y       = cmyk.Y;
        k       = cmyk.K;
        return *this;
    }

    Color &Color::get_cmyk(float &c, float &m, float &y, float &k) noexcept
    {
        calc_cmyk();
        c       = cmyk.C;
        m       = cmyk.M;
        y       = cmyk.Y;
        k       = cmyk.K;
        return *this;
    }

    const Color &Color::get_cmyka(float &c, float &m, float &y, float &k, float &alpha) const noexcept
    {
        calc_cmyk();
        c       = cmyk.C;
        m       = cmyk.M;
        y       = cmyk.Y;
        k       = cmyk.K;
        alpha   = clamp(A);
        return *this;
    }

    Color &Color::get_cmyka(float &c, float &m, float &y, float &k, float &alpha) noexcept
    {
        calc_cmyk();
        c       = cmyk.C;
        m       = cmyk.M;
        y       = cmyk.Y;
        k       = cmyk.K;
        alpha   = clamp(A);
        return *this;
    }

    Color &Color::cyan(float c) noexcept
    {
        calc_cmyk().C   = clamp(c);
        mask            = M_CMYK;
        return *this;
    }

    Color &Color::magenta(float m) noexcept
    {
        calc_cmyk().M   = clamp(m);
        mask            = M_CMYK;
        return *this;
    }

    Color &Color::yellow(float y) noexcept
    {
        calc_cmyk().Y   = clamp(y);
        mask            = M_CMYK;
        return *this;
    }

    Color &Color::black(float k) noexcept
    {
        calc_cmyk().K   = clamp(k);
        mask            = M_CMYK;
        return *this;
    }

    Color &Color::set_cmyk(float c, float m, float y, float k) noexcept
    {
        cmyk.C          = clamp(c);
        cmyk.M          = clamp(m);
        cmyk.Y          = clamp(y);
        cmyk.K          = clamp(k);
        mask            = M_CMYK;
        return *this;
    }

    Color &Color::set_cmyka(float c, float m, float y, float k, float alpha) noexcept
    {
        cmyk.C          = clamp(c);
        cmyk.M          = clamp(m);
        cmyk.Y          = clamp(y);
        cmyk.K          = clamp(k);
        A               = clamp(alpha);
        mask            = M_CMYK;
        return *this;
    }

} /* namespace lsp */
