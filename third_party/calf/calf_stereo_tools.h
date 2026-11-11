/* Calf DSP Library - Stereo Tools (Calf Studio Gear)
 *
 * Copyright (C) 2001-2010 Krzysztof Foltman, Markus Schmidt, Thor Harald Johansen and others
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA  02110-1301  USA
 *
 * ---------------------------------------------------------------------------
 * Aqulify: this file contains the parts of Calf (https://github.com/calf-studio-gear/calf,
 * commit a1f87a4) that the "Calf Stereo Tools" plugin uses, copied without changes to the
 * signal processing:
 *   - stereo_audio_module params_changed() / set_sample_rate() / process()
 *     (src/calf/modules_tools.h, src/modules_tools.cpp, src/metadata.cpp)
 * Removed: the LV2 port plumbing, the VU meters and the plugin's own bypass crossfade
 * (EasyEffects does not use them; the host switches the effect on and off).
 * The control parameters are members of calf_stereo_tools::params, with the same
 * names, units and defaults as the plugin (metadata.cpp).
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace calf_plugins {

struct calf_stereo_tools_params {
    float balance_in = 0.0f;     // -1.0 .. 1.0 (PF_UNIT_COEF)
    float balance_out = 0.0f;    // -1.0 .. 1.0 (PF_UNIT_COEF)
    float softclip = 0.0f;       // 0.0 vagy 1.0 (PF_BOOL)
    float mutel = 0.0f;          // 0.0 vagy 1.0 (PF_BOOL)
    float muter = 0.0f;          // 0.0 vagy 1.0 (PF_BOOL)
    float phasel = 0.0f;         // 0.0 vagy 1.0 (PF_BOOL)
    float phaser = 0.0f;         // 0.0 vagy 1.0 (PF_BOOL)
    int mode = 0;                // 0..6: LR>LR, LR>MS, MS>LR, LR>LL, LR>RR, LR>L+R, LR>RL
    float slev = 1.0f;           // 0.015625 .. 64.0 (lineáris erősítés, EasyEffects -36 .. +36 dB)
    float sbal = 0.0f;           // -1.0 .. 1.0 (PF_UNIT_COEF)
    float mlev = 1.0f;           // 0.015625 .. 64.0 (lineáris erősítés, EasyEffects -36 .. +36 dB)
    float mpan = 0.0f;           // -1.0 .. 1.0 (PF_UNIT_COEF)
    float stereo_base = 0.0f;    // -1.0 .. 1.0 (PF_UNIT_COEF)
    float delay = 0.0f;          // -20.0 .. 20.0 ms (PF_UNIT_MSEC)
    float sc_level = 1.0f;       // 1.0 .. 100.0 (S/C Level)
    float stereo_phase = 0.0f;   // 0.0 .. 360.0 deg (PF_UNIT_DEG)
};

class calf_stereo_tools {
public:
    calf_stereo_tools_params p;

    calf_stereo_tools() {
        _phase = -1.0f;
        _sc_level = -1.0f;
        _phase_cos_coef = 1.0f;
        _phase_sin_coef = 0.0f;
        _inv_atan_shape = 1.0f;
    }

    void set_sample_rate(uint32_t sr) {
        srate = std::max(2u, sr);
        // Calf modules_tools.cpp: buffer_size = (int)(srate * 0.05 * 2.f);
        buffer_size = static_cast<unsigned int>(srate * 0.05 * 2.0);
        if (buffer_size < 4) buffer_size = 4;
        buffer_size -= buffer_size % 2; // páros elemszám
        buffer.assign(buffer_size, 0.0f);
        pos = 0;
        meter_phase = 0.0f;
    }

    void params_changed() {
        if (p.stereo_phase != _phase) {
            _phase = p.stereo_phase;
            _phase_cos_coef = static_cast<float>(std::cos(_phase / 180.0 * M_PI));
            _phase_sin_coef = static_cast<float>(std::sin(_phase / 180.0 * M_PI));
        }
        if (p.sc_level != _sc_level) {
            _sc_level = p.sc_level;
            _inv_atan_shape = static_cast<float>(1.0 / std::atan(_sc_level));
        }
    }

    void process(const float* inL, const float* inR, float* outL, float* outR, uint32_t numsamples) {
        if (buffer.empty()) return;

        for (uint32_t i = 0; i < numsamples; ++i) {
            float L = inL[i];
            float R = inR[i];

            // balance in
            L *= (1.0f - std::max(0.0f, p.balance_in));
            R *= (1.0f + std::min(0.0f, p.balance_in));

            // softclip
            if (p.softclip > 0.5f) {
                R = _inv_atan_shape * std::atan(R * _sc_level);
                L = _inv_atan_shape * std::atan(L * _sc_level);
            }

            // modes
            const float slev = p.slev;
            const float sbal = 1.0f + p.sbal; // 0 -> 2
            const float mlev = p.mlev;
            const float mpan = 1.0f + p.mpan; // 0 -> 2

            float l = 0.0f, r = 0.0f, m = 0.0f, s = 0.0f;

            switch (p.mode) {
                case 0:
                    // LR > LR
                    m = (L + R) * 0.5f;
                    s = (L - R) * 0.5f;
                    l = m * mlev * std::min(1.0f, 2.0f - mpan) + s * slev * std::min(1.0f, 2.0f - sbal);
                    r = m * mlev * std::min(1.0f, mpan)       - s * slev * std::min(1.0f, sbal);
                    L = l;
                    R = r;
                    break;
                case 1:
                    // LR > MS
                    l = L * std::min(1.0f, 2.0f - sbal);
                    r = R * std::min(1.0f, sbal);
                    L = 0.5f * (l + r) * mlev;
                    R = 0.5f * (l - r) * slev;
                    break;
                case 2:
                    // MS > LR
                    l = L * mlev * std::min(1.0f, 2.0f - mpan) + R * slev * std::min(1.0f, 2.0f - sbal);
                    r = L * mlev * std::min(1.0f, mpan)       - R * slev * std::min(1.0f, sbal);
                    L = l;
                    R = r;
                    break;
                case 3:
                    // LR > LL
                    R = L;
                    break;
                case 4:
                    // LR > RR
                    L = R;
                    break;
                case 5:
                    // LR > L+R
                    L = (L + R) * 0.5f;
                    R = L;
                    break;
                case 6:
                    // LR > RL
                    l = L;
                    L = R;
                    R = l;
                    m = (L + R) * 0.5f;
                    s = (L - R) * 0.5f;
                    l = m * mlev * std::min(1.0f, 2.0f - mpan) + s * slev * std::min(1.0f, 2.0f - sbal);
                    r = m * mlev * std::min(1.0f, mpan)       - s * slev * std::min(1.0f, sbal);
                    L = l;
                    R = r;
                    break;
                default:
                    break;
            }

            // mute
            L *= (1.0f - std::floor(p.mutel + 0.5f));
            R *= (1.0f - std::floor(p.muter + 0.5f));

            // phase
            L *= (2.0f * (1.0f - std::floor(p.phasel + 0.5f))) - 1.0f;
            R *= (2.0f * (1.0f - std::floor(p.phaser + 0.5f))) - 1.0f;

            // delay: összefésült puffer [L, R, L, R, ...]
            buffer[pos]     = L;
            buffer[pos + 1] = R;

            int nbuf = static_cast<int>(srate * (std::fabs(p.delay) / 1000.0f));
            nbuf -= nbuf % 2;
            if (p.delay > 0.0f) {
                R = buffer[(pos - nbuf + 1 + buffer_size) % buffer_size];
            } else if (p.delay < 0.0f) {
                L = buffer[(pos - nbuf + buffer_size) % buffer_size];
            }

            // stereo base
            float sb = p.stereo_base;
            if (sb < 0.0f) sb *= 0.5f;

            float __l = L + sb * L - sb * R;
            float __r = R + sb * R - sb * L;

            L = __l;
            R = __r;

            // stereo phase
            __l = L * _phase_cos_coef - R * _phase_sin_coef;
            __r = L * _phase_sin_coef + R * _phase_cos_coef;

            L = __l;
            R = __r;

            pos = (pos + 2) % buffer_size;

            // balance out
            L *= (1.0f - std::max(0.0f, p.balance_out));
            R *= (1.0f + std::min(0.0f, p.balance_out));

            outL[i] = L;
            outR[i] = R;

            // phase correlation meter (Calf formula)
            if (std::abs(L) > 0.001f && std::abs(R) > 0.001f) {
                meter_phase = std::abs(std::abs(L + R) > 1e-9f ? std::sin(std::abs((L - R) / (L + R))) : 0.0f);
            } else {
                meter_phase = 0.0f;
            }
        }
    }

    float phase_correlation() const { return meter_phase; }

    void reset() {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        pos = 0;
        meter_phase = 0.0f;
    }

private:
    uint32_t srate = 0;
    std::vector<float> buffer;
    unsigned int pos = 0;
    unsigned int buffer_size = 0;
    float _phase = -1.0f;
    float _phase_sin_coef = 0.0f;
    float _phase_cos_coef = 1.0f;
    float _sc_level = -1.0f;
    float _inv_atan_shape = 1.0f;
    float meter_phase = 0.0f;
};

} // namespace calf_plugins
