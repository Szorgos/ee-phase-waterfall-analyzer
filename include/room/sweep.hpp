#pragma once
/**
 * @file sweep.hpp
 * @brief Logaritmikus szinusz sweep és inverz szűrő generátor szobaméréshez (Farina-módszer).
 */

#include <cmath>
#include <cstdint>
#include <vector>
#include <algorithm>

namespace aqulify::room {

/**
 * Logaritmikus sine sweep előállítása.
 * @param f1 Kezdőfrekvencia Hz-ben (pl. 20.0)
 * @param f2 Végfrekvencia Hz-ben (pl. 20000.0)
 * @param durationSec Időtartam másodpercben (pl. 3.0 .. 10.0)
 * @param sampleRate Mintavételi frekvencia (pl. 48000)
 * @param amplitude Csúcsamplitúdó (pl. 0.1 .. 0.5; alapértelmezetten -20 dBFS = 0.1)
 * @param fadeMs Fade-in és fade-out hossza milliszekundumban (kattogás ellen)
 */
inline std::vector<float> generateLogSweep(double f1, double f2, double durationSec,
                                           uint32_t sampleRate, float amplitude = 0.1f,
                                           double fadeMs = 20.0) {
    if (f1 <= 0.0 || f2 <= f1 || durationSec <= 0.0 || sampleRate == 0) return {};
    const size_t numSamples = static_cast<size_t>(std::round(durationSec * sampleRate));
    std::vector<float> sweep(numSamples);

    const double T = durationSec;
    const double w1 = 2.0 * M_PI * f1;
    const double w2 = 2.0 * M_PI * f2;
    const double logRatio = std::log(w2 / w1);

    for (size_t i = 0; i < numSamples; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        // Farina képlet: fázis = (w1 * T / ln(w2/w1)) * (exp(t/T * ln(w2/w1)) - 1)
        const double phase = (w1 * T / logRatio) * (std::exp((t / T) * logRatio) - 1.0);
        sweep[i] = amplitude * static_cast<float>(std::sin(phase));
    }

    // Fade-in és fade-out Hann ablakkal
    const size_t fadeSamples = static_cast<size_t>(std::round(fadeMs * 0.001 * sampleRate));
    for (size_t i = 0; i < fadeSamples && i < numSamples / 2; ++i) {
        const double w = 0.5 * (1.0 - std::cos(M_PI * static_cast<double>(i) / fadeSamples));
        sweep[i] *= static_cast<float>(w);
        sweep[numSamples - 1 - i] *= static_cast<float>(w);
    }

    return sweep;
}

/**
 * Logaritmikus sweep inverz szűrőjének előállítása.
 * Az inverz szűrő a sweep időbeli megfordítása, egy -6 dB/oktávos burkolóval skálázva,
 * így a sweeppel vett konvolúciója egy egységimpulzust (Dirac) eredményez.
 */
inline std::vector<float> generateInverseFilter(double f1, double f2, double durationSec,
                                                uint32_t sampleRate, float amplitude = 0.1f,
                                                double fadeMs = 20.0) {
    auto sweep = generateLogSweep(f1, f2, durationSec, sampleRate, 1.0f, fadeMs);
    const size_t N = sweep.size();
    if (N == 0) return {};

    std::vector<float> inv(N);
    const double T = durationSec;
    const double logRatio = std::log(f2 / f1);

    // Időben megfordított sweep modulálva az inverz spektrális burkolóval: m(t) = exp(-t/T * ln(f2/f1))
    for (size_t i = 0; i < N; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double env = std::exp(-(t / T) * logRatio);
        inv[N - 1 - i] = static_cast<float>(sweep[i] * env);
    }

    // Normalizálás úgy, hogy az impulzus amplitúdója pontosan 1.0 legyen
    double energy = 0.0;
    for (size_t i = 0; i < N; ++i) {
        energy += static_cast<double>(sweep[i]) * inv[i];
    }
    if (std::abs(energy) > 1e-12) {
        const float scale = static_cast<float>(1.0 / (energy * amplitude));
        for (auto& v : inv) v *= scale;
    }

    return inv;
}

} // namespace aqulify::room
