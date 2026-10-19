#pragma once
/**
 * @file param_apply.hpp
 * @brief ParamData -> DspPipeline leképezés, valós idejű szálból is hívható
 *        (előre lefoglalt vektorokkal, memóriafoglalás nélkül).
 */
#include <algorithm>
#include "dsp_pipeline.hpp"
#include "param_block.hpp"

namespace ee_analyzer {

/** Egyszer, NEM valós idejű szálból: kapacitás lefoglalása, hogy a későbbi resize ne foglaljon. */
inline void preparePipeline(DspPipeline& p) {
    p.eqChain.filters.reserve(kMaxEqBands);
    p.eqCoeffs.reserve(kMaxEqBands);
    p.eqStatesL.reserve(kMaxEqBands);
    p.eqStatesR.reserve(kMaxEqBands);
}

/** Mintavételi frekvencia beállítása az összes modulon. */
inline void setPipelineSampleRate(DspPipeline& p, double sr) {
    p.sampleRate = sr;
    p.eqChain.sampleRate = sr;
    for (auto& f : p.eqChain.filters) f.sampleRate = sr;
    p.crossfeed.sampleRate = sr;
    p.crossfeed.prepareDelay();
    p.convolver.sampleRate = sr;
    p.limiter.sampleRate = sr;
    p.limiter.updateParameters();
    p.updateEqCoefficients();
    p.reset();
}

inline FilterType toFilterType(int32_t t) {
    switch (static_cast<EqBandType>(t)) {
        case EqBandType::LowShelf: return FilterType::LowShelf;
        case EqBandType::HighShelf: return FilterType::HighShelf;
        case EqBandType::LowPass: return FilterType::LowPass;
        case EqBandType::HighPass: return FilterType::HighPass;
        default: return FilterType::PeakingEQ;
    }
}

/** Paraméter-pillanatkép alkalmazása. Valós idejű szálból is hívható (preparePipeline után). */
inline void applyParams(DspPipeline& p, const ParamData& d) {
    p.preampGainDb = std::clamp(static_cast<double>(d.preampDb), -60.0, 24.0);

    // FIR módban az EQ-t a háttérszálban tervezett FIR végzi (AetherChain), az IIR lánc üres
    const bool iirEq = d.has(kFlagEq) && d.eqMode == static_cast<int32_t>(EqMode::Iir);
    const uint32_t n = iirEq ? std::min(d.numBands, kMaxEqBands) : 0u;
    p.eqChain.filters.resize(n);
    const double nyquist = p.sampleRate * 0.5;
    for (uint32_t i = 0; i < n; ++i) {
        const EqBandPod& b = d.bands[i];
        BiquadFilter& f = p.eqChain.filters[i];
        f.type = toFilterType(b.type);
        f.frequency = std::clamp(static_cast<double>(b.freq), 10.0, nyquist * 0.98);
        f.qFactor = std::clamp(static_cast<double>(b.q), 0.05, 40.0);
        f.gainDb = std::clamp(static_cast<double>(b.gainDb), -30.0, 30.0);
        f.sampleRate = p.sampleRate;
    }
    p.updateEqCoefficients();

    const bool cfOn = d.has(kFlagCrossfeed);
    if (cfOn && !p.crossfeed.enabled) p.crossfeed.reset();
    p.crossfeed.enabled = cfOn;
    p.crossfeed.delayMs = std::clamp(static_cast<double>(d.crossfeedDelayMs), 0.0, 1.0);
    if (d.crossfeedCutoffHz > 0.0f) {
        p.crossfeed.cutoffFreq = std::clamp(static_cast<double>(d.crossfeedCutoffHz), 200.0, 3000.0);
        p.crossfeed.feedLevelDb = std::clamp(static_cast<double>(d.crossfeedFeedDb), 0.5, 20.0);
        p.crossfeed.updateCoefficients();
    } else {
        switch (d.crossfeedPreset) {
            case 1: p.crossfeed.setPreset(CrossfeedPreset::MeierCorda); break;
            case 2: p.crossfeed.setPreset(CrossfeedPreset::StudioNearfield); break;
            default: p.crossfeed.setPreset(CrossfeedPreset::BauerDefault); break;
        }
    }

    // Az egyszerű (közvetlen) FIR konvolver jelzője; az AetherChain a partícionált konvolvert használja.
    p.convolver.enabled = d.has(kFlagConvolver);

    const double ceiling = std::clamp(static_cast<double>(d.limiterCeilingDb), -12.0, 0.0);
    p.limiter.ceilingDb = ceiling;
    p.limiter.thresholdDb = ceiling - 0.1;
    const double rel = std::clamp(static_cast<double>(d.limiterReleaseMs), 1.0, 1000.0);
    if (rel != p.limiter.releaseMs) {
        p.limiter.releaseMs = rel;
        p.limiter.updateParameters();
    }

    const bool limOn = d.has(kFlagLimiter);
    if (limOn && !p.limiter.enabled) p.limiter.reset();
    p.limiter.enabled = limOn;
}

/** Átlátszó (bit-pontosan érintetlen) alapállapot: minden modul kikapcsolva. */
inline ParamData transparentParams() {
    ParamData d{};
    d.preampDb = 0.0f;
    d.flags = 0;
    d.numBands = 0;
    return d;
}

} // namespace ee_analyzer
