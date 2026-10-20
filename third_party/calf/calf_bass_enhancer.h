/* Calf DSP Library - Bass Enhancer (Calf Studio Gear)
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
 * commit a1f87a4) that the "Calf Bass Enhancer" plugin uses, copied without changes to the
 * signal processing:
 *   - dsp::resampleN, dsp::tap_distortion      (src/calf/audio_fx.h, src/audio_fx.cpp)
 *   - bassenhancer_audio_module params_changed() / set_sample_rate() / process()
 *                                             (src/calf/modules_dist.h, src/modules_dist.cpp)
 * Removed: the LV2 port plumbing, the VU meters and the plugin's own bypass crossfade
 * (EasyEffects does not use them; the host switches the effect on and off).
 * The LV2 control ports are plain members of calf_bass_enhancer::params, with the same
 * names, units and defaults as the plugin (metadata.cpp).
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "calf/biquad.h"

namespace dsp {

/// Calf: src/calf/audio_fx.h, src/audio_fx.cpp
class resampleN
{
private:
public:
    uint32_t srate; // sample rate; source for upsampling, target for downsampling
    int factor; // number of added/removed samples, max 16
    int filters; // how many lowpasses should be used, max 4
    double tmp[16];
    dsp::biquad_d2 filter[2][4];
    resampleN()
    {
        factor  = 2;
        srate   = 0;
        filters = 2;
    }
    void set_params(uint32_t sr, int fctr = 2, int fltrs = 2)
    {
        srate   = std::max(2u, sr);
        factor  = std::min(16, std::max(1, fctr));
        filters = std::min(4, std::max(1, fltrs));
        // set all filters
        filter[0][0].set_lp_rbj(std::max(25000., (double)srate / 2), 0.8, (float)srate * factor);
        for (int i = 1; i < filters; i++) {
            filter[0][i].copy_coeffs(filter[0][0]);
            filter[1][i].copy_coeffs(filter[0][0]);
        }
    }
    double *upsample(double sample)
    {
        tmp[0] = sample;
        if (factor > 1) {
            for (int f = 0; f < filters; f++)
                tmp[0] = filter[0][f].process(sample);
            for (int i = 1; i < factor; i++) {
                tmp[i] = 0;
                for (int f = 0; f < filters; f++)
                    tmp[i] = filter[0][f].process(sample);
            }
        }
        return tmp;
    }
    double downsample(double *sample)
    {
        if (factor > 1) {
            for(int i = 0; i < factor; i++) {
                for (int f = 0; f < filters; f++) {
                    sample[i] = filter[1][f].process(sample[i]);
                }
            }
        }
        return sample[0];
    }
};

/// Calf: src/calf/audio_fx.h, src/audio_fx.cpp
class tap_distortion {
private:
    float blend_old, drive_old;
    float meter;
    float rdrive, rbdr, kpa, kpb, kna, knb, ap, an, imr, kc, srct, sq, pwrq;
    int over;
    float prev_med, prev_out;
    resampleN resampler;
public:
    uint32_t srate;
    bool is_active;
    tap_distortion()
    {
        is_active = false;
        srate = 0;
        meter = 0.f;
        rdrive = rbdr = kpa = kpb = kna = knb = ap = an = imr = kc = srct = sq = pwrq = prev_med = prev_out = 0.f;
        drive_old = blend_old = -1.f;
        over = 1;
    }
    void activate()
    {
        is_active = true;
        set_params(0.f, 0.f);
    }
    void deactivate()
    {
        is_active = false;
    }
    void set_params(float blend, float drive)
    {
        // set distortion coeffs
        if ((drive_old != drive) || (blend_old != blend)) {
            rdrive = 12.0f / drive;
            rbdr = rdrive / (10.5f - blend) * 780.0f / 33.0f;
            kpa = D(2.0f * (rdrive*rdrive) - 1.0f) + 1.0f;
            kpb = (2.0f - kpa) / 2.0f;
            ap = ((rdrive*rdrive) - kpa + 1.0f) / 2.0f;
            kc = kpa / D(2.0f * D(2.0f * (rdrive*rdrive) - 1.0f) - 2.0f * rdrive*rdrive);

            srct = (0.1f * srate) / (0.1f * srate + 1.0f);
            sq = kc*kc + 1.0f;
            knb = -1.0f * rbdr / D(sq);
            kna = 2.0f * kc * rbdr / D(sq);
            an = rbdr*rbdr / sq;
            imr = 2.0f * knb + D(2.0f * kna + 4.0f * an - 1.0f);
            pwrq = 2.0f / (imr + 1.0f);

            drive_old = drive;
            blend_old = blend;
        }
    }
    void set_sample_rate(uint32_t sr)
    {
        srate = sr;
        over = srate * 2 > 96000 ? 1 : 2;
        resampler.set_params(srate, over, 2);
    }
    float process(float in)
    {
        double *samples = resampler.upsample((double)in);
        meter = 0.f;
        for (int o = 0; o < over; o++) {
            float proc = samples[o];
            float med;
            if (proc >= 0.0f) {
                med = (D(ap + proc * (kpa - proc)) + kpb) * pwrq;
            } else {
                med = (D(an - proc * (kna + proc)) + knb) * pwrq * -1.0f;
            }
            proc = srct * (med - prev_med + prev_out);
            prev_med = M(med);
            prev_out = M(proc);
            samples[o] = proc;
            meter = std::max(meter, proc);
        }
        float out = (float)resampler.downsample(samples);
        return out;
    }
    float get_distortion_level()
    {
        return meter;
    }
    static inline float M(float x)
    {
        return (fabs(x) > 0.00000001f) ? x : 0.0f;
    }

    static inline float D(float x)
    {
        x = fabs(x);
        return (x > 0.00000001f) ? sqrtf(x) : 0.0f;
    }
};

} // namespace dsp

namespace calf_plugins {

/// Calf: bassenhancer_audio_module (stereo in / stereo out instance, as loaded by EasyEffects)
class calf_bass_enhancer {
public:
    /// LV2 control ports (metadata.cpp: CALF_PORT_PROPS(bassenhancer)), linear values
    struct params {
        float level_in = 1.f;      // "level_in"
        float level_out = 1.f;     // "level_out"
        float amount = 1.f;        // "amount"
        float drive = 8.5f;        // "drive"  (Harmonics)
        float blend = 0.f;         // "blend"  (Blend Harmonics)
        float freq = 100.f;        // "freq"   (Scope)
        float listen = 0.f;        // "listen"
        float floor_active = 0.f;  // "floor_active"
        float floor = 20.f;        // "floor"
    };

    params p;

    calf_bass_enhancer()
    {
        freq_old         = 0.f;
        floor_old        = 0.f;
        floor_active_old = false;
        srate            = 0;
    }

    void set_sample_rate(uint32_t sr)
    {
        srate = sr;
        dist[0].set_sample_rate(sr);
        dist[1].set_sample_rate(sr);
    }

    /// activate(): the plugin calls params_changed(); filters are recomputed only when a value changes
    void activate()
    {
        freq_old = 0.f;
        floor_old = 0.f;
        params_changed();
    }

    void params_changed()
    {
        // set the params of all filters
        if(p.freq != freq_old) {
            lp[0][0].set_lp_rbj(p.freq, 0.707, (float)srate);
            lp[0][1].copy_coeffs(lp[0][0]);
            lp[0][2].copy_coeffs(lp[0][0]);
            lp[0][3].copy_coeffs(lp[0][0]);
            lp[1][0].copy_coeffs(lp[0][0]);
            lp[1][1].copy_coeffs(lp[0][0]);
            lp[1][2].copy_coeffs(lp[0][0]);
            lp[1][3].copy_coeffs(lp[0][0]);
            freq_old = p.freq;
        }
        // set the params of all filters
        if(p.floor != floor_old || (p.floor_active > 0.5f) != floor_active_old) {
            hp[0][0].set_hp_rbj(p.floor, 0.707, (float)srate);
            hp[0][1].copy_coeffs(hp[0][0]);
            hp[1][0].copy_coeffs(hp[0][0]);
            hp[1][1].copy_coeffs(hp[0][0]);
            floor_old = p.floor;
            floor_active_old = p.floor_active > 0.5f;
        }
        // set distortion
        dist[0].set_params(p.blend, p.drive);
        dist[1].set_params(p.blend, p.drive);
    }

    /// Stereo processing (out may equal in)
    void process(const float *inL, const float *inR, float *outL, float *outR, uint32_t numsamples)
    {
        for (uint32_t offset = 0; offset < numsamples; ++offset) {
            // cycle through samples
            float out[2], in[2];
            // stereo in/stereo out
            // handle full stereo
            in[0] = inL[offset];
            in[0] *= p.level_in;
            in[1] = inR[offset];
            in[1] *= p.level_in;
            const int c = 2;

            float proc[2];
            proc[0] = in[0];
            proc[1] = in[1];

            for (int i = 0; i < c; ++i) {
                // all pre filters in chain
                proc[i] = lp[i][1].process(lp[i][0].process(proc[i]));

                // saturate
                proc[i] = dist[i].process(proc[i]);

                // all post filters in chain
                proc[i] = lp[i][2].process(lp[i][3].process(proc[i]));

                if(p.floor_active > 0.5f) {
                    // all H/P post filters in chain
                    proc[i] = hp[i][0].process(hp[i][1].process(proc[i]));
                }
            }

            // full stereo
            if(p.listen > 0.f)
                out[0] = proc[0] * p.amount * p.level_out;
            else
                out[0] = (proc[0] * p.amount + in[0]) * p.level_out;
            outL[offset] = out[0];
            if(p.listen > 0.f)
                out[1] = proc[1] * p.amount * p.level_out;
            else
                out[1] = (proc[1] * p.amount + in[1]) * p.level_out;
            outR[offset] = out[1];
            meter_drive = std::max(dist[0].get_distortion_level() * p.amount,
                                   dist[1].get_distortion_level() * p.amount);
        } // cycle trough samples
        // clean up
        lp[0][0].sanitize();
        lp[1][0].sanitize();
        lp[0][1].sanitize();
        lp[1][1].sanitize();
        lp[0][2].sanitize();
        lp[1][2].sanitize();
        lp[0][3].sanitize();
        lp[1][3].sanitize();
        hp[0][0].sanitize();
        hp[1][0].sanitize();
        hp[0][1].sanitize();
        hp[1][1].sanitize();
    }

    /// The plugin's "meter_drive" output (harmonics level, linear)
    float harmonics_level() const { return meter_drive; }

    void reset()
    {
        for (auto &row : lp) for (auto &f : row) f.reset();
        for (auto &row : hp) for (auto &f : row) f.reset();
    }

private:
    float freq_old, floor_old;
    bool floor_active_old;
    float meter_drive = 0.f;
    dsp::biquad_d2 lp[2][4];
    dsp::biquad_d2 hp[2][2];
    dsp::tap_distortion dist[2];
    uint32_t srate;
};

} // namespace calf_plugins
