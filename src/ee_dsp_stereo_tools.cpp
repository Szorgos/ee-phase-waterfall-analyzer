#include "ee_dsp.hpp"
#include "calf_stereo_tools.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace ee_analyzer::ee {

static inline float dbToLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}

struct StereoTools::Impl {
    calf_plugins::calf_stereo_tools calf;
    float inGain = 1.0f;
    float outGain = 1.0f;
    float dry = 0.0f;
    float wet = 1.0f;
    uint32_t sr = 0;
};

StereoTools::StereoTools() : p_(std::make_unique<Impl>()) {}
StereoTools::~StereoTools() = default;

void StereoTools::setSampleRate(uint32_t sr) {
    if (sr == p_->sr) return;
    p_->calf = calf_plugins::calf_stereo_tools{};
    p_->sr = sr;
    p_->calf.set_sample_rate(sr);
}

void StereoTools::update(const ParamData& d) {
    auto& c = p_->calf.p;
    c.balance_in = d.stBalanceIn;
    c.balance_out = d.stBalanceOut;
    c.softclip = d.stSoftclip ? 1.0f : 0.0f;
    c.mutel = d.stMutel ? 1.0f : 0.0f;
    c.muter = d.stMuter ? 1.0f : 0.0f;
    c.phasel = d.stPhasel ? 1.0f : 0.0f;
    c.phaser = d.stPhaser ? 1.0f : 0.0f;
    c.mode = std::clamp(d.stMode, 0, 6);
    c.slev = dbToLinear(d.stSlevDb);
    c.sbal = d.stSbal;
    c.mlev = dbToLinear(d.stMlevDb);
    c.mpan = d.stMpan;
    c.stereo_base = d.stStereoBase;
    c.delay = d.stDelayMs;
    c.sc_level = d.stScLevel;
    c.stereo_phase = d.stStereoPhase;
    p_->calf.params_changed();

    p_->inGain = dbToLinear(d.stInputGainDb);
    p_->outGain = dbToLinear(d.stOutputGainDb);

    // EasyEffects: util::minimum_db_d_level = -100.0 dB alatt 0.0F
    p_->dry = (d.stDryDb <= -100.0f) ? 0.0f : dbToLinear(d.stDryDb);
    p_->wet = (d.stWetDb <= -100.0f) ? 0.0f : dbToLinear(d.stWetDb);
}

void StereoTools::process(float* L, float* R, size_t n) {
    if (p_->sr == 0 || n == 0) return;

    // 1. Input gain (a plugin előtt)
    if (p_->inGain != 1.0f) {
        for (size_t i = 0; i < n; ++i) {
            L[i] *= p_->inGain;
            R[i] *= p_->inGain;
        }
    }

    // 2. Calf stereo_audio_module futtatása és dry/wet keverés
    if (p_->dry == 0.0f && p_->wet == 1.0f) {
        p_->calf.process(L, R, L, R, static_cast<uint32_t>(n));
    } else {
        constexpr size_t kStackChunk = 512;
        float origL[kStackChunk], origR[kStackChunk];
        size_t processed = 0;
        while (processed < n) {
            const size_t chunk = std::min(n - processed, kStackChunk);
            for (size_t i = 0; i < chunk; ++i) {
                origL[i] = L[processed + i];
                origR[i] = R[processed + i];
            }
            p_->calf.process(&L[processed], &R[processed], &L[processed], &R[processed], static_cast<uint32_t>(chunk));
            for (size_t i = 0; i < chunk; ++i) {
                L[processed + i] = p_->wet * L[processed + i] + p_->dry * origL[i];
                R[processed + i] = p_->wet * R[processed + i] + p_->dry * origR[i];
            }
            processed += chunk;
        }
    }

    // 3. Output gain (a keverés után)
    if (p_->outGain != 1.0f) {
        for (size_t i = 0; i < n; ++i) {
            L[i] *= p_->outGain;
            R[i] *= p_->outGain;
        }
    }

    phaseCorrelation_.store(p_->calf.phase_correlation(), std::memory_order_relaxed);
}

void StereoTools::reset() {
    p_->calf.reset();
}

} // namespace ee_analyzer::ee
