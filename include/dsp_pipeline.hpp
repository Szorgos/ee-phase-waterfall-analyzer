#pragma once
#include <vector>
#include <memory>
#include <cmath>
#include "biquad_analyzer.hpp"
#include "crossfeed.hpp"
#include "limiter.hpp"
#include "convolver.hpp"

namespace ee_analyzer {

/**
 * @brief Futásidejű Biquad IIR állapot (Direct Form II Transposed)
 * Numerikusan a legstabilabb valós idejű szűrőstruktúra lebegőpontos hardveren.
 */
struct BiquadState {
    double s1 = 0.0;
    double s2 = 0.0;

    inline float process(float in, const BiquadCoefficients& c) {
        // Direct Form II Transposed:
        // y[n] = b0 * x[n] + s1
        // s1   = b1 * x[n] - a1 * y[n] + s2
        // s2   = b2 * x[n] - a2 * y[n]
        const double x = static_cast<double>(in);
        const double y = c.b0 * x + s1;
        s1 = c.b1 * x - c.a1 * y + s2;
        s2 = c.b2 * x - c.a2 * y;
        return static_cast<float>(y);
    }

    void reset() {
        s1 = 0.0;
        s2 = 0.0;
    }
};

/**
 * @brief Egységesített DSP feldolgozási lánc (Unified EasyEffects Pipeline)
 * Sorrend: Preamp -> Parametric EQ -> Crossfeed -> Convolver -> Studio Limiter
 */
class DspPipeline {
public:
    double sampleRate = 48000.0;
    double preampGainDb = 0.0; // dB
    bool bypassAll = false;

    // Modulok
    FilterChain eqChain;
    CrossfeedProcessor crossfeed;
    FirConvolver convolver;
    StudioLimiter limiter;

    // EQ szűrő állapotok (bal és jobb csatorna külön-külön minden sávhoz)
    std::vector<BiquadState> eqStatesL;
    std::vector<BiquadState> eqStatesR;
    std::vector<BiquadCoefficients> eqCoeffs;

    DspPipeline(double sr = 48000.0)
        : sampleRate(sr), crossfeed(sr), convolver(sr), limiter(sr) {
        eqChain.sampleRate = sr;
    }

    /**
     * @brief EQ sávok hozzáadása vagy módosítása után szűrőegyütthatók és állapotok frissítése
     */
    void updateEqCoefficients() {
        const size_t numFilters = eqChain.filters.size();
        eqCoeffs.resize(numFilters);
        eqStatesL.resize(numFilters);
        eqStatesR.resize(numFilters);

        for (size_t i = 0; i < numFilters; ++i) {
            eqCoeffs[i] = eqChain.filters[i].calculateCoefficients();
        }
    }

    /**
     * @brief Sztereó audio blokk valós idejű feldolgozása
     * @param left Bal csatorna lebegőpontos minták
     * @param right Jobb csatorna lebegőpontos minták
     * @param numSamples Minták száma a blokkban (pl. 64, 128, 256 vagy 512)
     */
    void processBlock(float* left, float* right, size_t numSamples) {
        if (bypassAll) return;

        // 1. Preamp erősítés (lineáris szorzó)
        const float linearPreamp = static_cast<float>(std::pow(10.0, preampGainDb / 20.0));
        if (std::abs(preampGainDb) > 0.001) {
            for (size_t i = 0; i < numSamples; ++i) {
                left[i] *= linearPreamp;
                right[i] *= linearPreamp;
            }
        }

        // 2. Parametrikus EQ (IIR Biquad lánc csatornánként)
        const size_t numFilters = eqCoeffs.size();
        for (size_t f = 0; f < numFilters; ++f) {
            const auto& c = eqCoeffs[f];
            auto& stateL = eqStatesL[f];
            auto& stateR = eqStatesR[f];

            for (size_t i = 0; i < numSamples; ++i) {
                left[i] = stateL.process(left[i], c);
                right[i] = stateR.process(right[i], c);
            }
        }

        // 3. Crossfeed (fejhallgatós sztereó áthallás)
        if (crossfeed.enabled) {
            crossfeed.processBlock(left, right, numSamples);
        }

        // 4. FIR Convolver (akusztikai korrekció / fejhallgató IR)
        if (convolver.enabled && !convolver.impulseResponse.empty()) {
            convolver.processBlock(left, right, numSamples);
        }

        // 5. Studio Limiter & Soft-Clipper (végső csúcsérték és DAC védelem)
        if (limiter.enabled) {
            limiter.processBlock(left, right, numSamples);
        }
    }

    /**
     * @brief Belső állapotok és pufferek nullázása
     */
    void reset() {
        for (auto& s : eqStatesL) s.reset();
        for (auto& s : eqStatesR) s.reset();
        crossfeed.reset();
        convolver.reset();
        limiter.reset();
    }
};

} // namespace ee_analyzer
