#pragma once
/**
 * @file deconv.hpp
 * @brief Dekonvolúció, impulzusválasz (IR) kinyerése, polaritás, késleltetés és ablakozás.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace aqulify::room {

struct DeconvResult {
    std::vector<float> ir;          // Kinyert impulzusválasz
    size_t peakIndex = 0;           // A fő impulzus mintapozíciója
    float peakValue = 0.0f;         // A csúcs értéke (előjellel)
    float delayMs = 0.0f;           // Mért késleltetés milliszekundumban
    bool invertedPolarity = false;  // Fordított polaritás észlelve
    float snrDb = 0.0f;             // Becsült jel-zaj viszony dB-ben
};

/**
 * Szoba felvételének dekonvolválása a referencia sweep jellel illesztett szűrővel (Matched filter).
 * Kiszámítja az impulzusválaszt, a mintaszintű késleltetést, a polaritást és az SNR-t.
 */
inline DeconvResult deconvolve(const std::vector<float>& recorded,
                               const std::vector<float>& referenceSweep,
                               uint32_t sampleRate) {
    DeconvResult res;
    if (recorded.empty() || referenceSweep.empty() || sampleRate == 0) return res;

    const size_t recLen = recorded.size();
    const size_t refLen = referenceSweep.size();
    if (recLen < refLen) return res;
    const size_t irLen = recLen - refLen + 1;
    res.ir.assign(irLen, 0.0f);

    double refEnergy = 0.0;
    for (float v : referenceSweep) refEnergy += static_cast<double>(v) * v;
    if (refEnergy < 1e-12) return res;
    const double invEnergy = 1.0 / refEnergy;

    // Keresztkorreláció / illesztett szűrő (Matched filter)
    for (size_t i = 0; i < irLen; ++i) {
        double sum = 0.0;
        for (size_t k = 0; k < refLen; ++k) {
            sum += static_cast<double>(recorded[i + k]) * static_cast<double>(referenceSweep[k]);
        }
        res.ir[i] = static_cast<float>(sum * invEnergy);
    }

    // Csúcskeresés
    float maxAbs = 0.0f;
    for (size_t i = 0; i < irLen; ++i) {
        const float a = std::abs(res.ir[i]);
        if (a > maxAbs) {
            maxAbs = a;
            res.peakIndex = i;
            res.peakValue = res.ir[i];
        }
    }

    res.delayMs = static_cast<float>(res.peakIndex) / sampleRate * 1000.0f;
    res.invertedPolarity = (res.peakValue < 0.0f);

    // Zajszint becslése a csúcs előtti és utáni szakaszból
    double noisePower = 0.0;
    size_t noiseSamples = 0;
    const size_t guard = static_cast<size_t>(sampleRate * 0.005); // 5 ms
    for (size_t i = 0; i < irLen; ++i) {
        if (i + guard < res.peakIndex || i > res.peakIndex + guard * 10) {
            noisePower += static_cast<double>(res.ir[i]) * res.ir[i];
            noiseSamples++;
        }
    }
    if (noiseSamples > 0 && noisePower > 1e-12) {
        const double avgNoise = noisePower / noiseSamples;
        res.snrDb = static_cast<float>(10.0 * std::log10(std::max(1e-12, (maxAbs * maxAbs) / avgNoise)));
    } else {
        res.snrDb = 90.0f;
    }

    return res;
}

/**
 * Időablakozás alkalmazása az impulzusválaszra:
 * A közvetlen hangot és a korai visszaverődéseket izolálja, csökkenti a szoba távoli zengését.
 */
inline std::vector<float> applyWindow(const std::vector<float>& ir, size_t peakIdx,
                                      uint32_t sampleRate, double leftMs = 3.0,
                                      double rightMs = 150.0) {
    if (ir.empty() || sampleRate == 0) return {};
    std::vector<float> windowed = ir;
    const size_t n = ir.size();

    const size_t leftSamples = static_cast<size_t>(std::round(leftMs * 0.001 * sampleRate));
    const size_t rightSamples = static_cast<size_t>(std::round(rightMs * 0.001 * sampleRate));

    // Bal oldal nullázása és Hann fade-in
    const size_t startFade = (peakIdx > leftSamples) ? (peakIdx - leftSamples) : 0;
    for (size_t i = 0; i < startFade; ++i) windowed[i] = 0.0f;
    for (size_t i = startFade; i < peakIdx; ++i) {
        const double t = static_cast<double>(i - startFade) / (peakIdx - startFade);
        const double w = 0.5 * (1.0 - std::cos(M_PI * t));
        windowed[i] *= static_cast<float>(w);
    }

    // Jobb oldal Hann fade-out és utána nullázás
    const size_t endFade = std::min(n, peakIdx + rightSamples);
    for (size_t i = peakIdx; i < endFade; ++i) {
        const double t = static_cast<double>(i - peakIdx) / (endFade - peakIdx);
        const double w = 0.5 * (1.0 + std::cos(M_PI * t));
        windowed[i] *= static_cast<float>(w);
    }
    for (size_t i = endFade; i < n; ++i) windowed[i] = 0.0f;

    return windowed;
}

} // namespace aqulify::room
