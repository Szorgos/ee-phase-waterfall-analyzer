#pragma once
/**
 * @file smoothing.hpp
 * @brief Frakcionális oktáv simítás (1/1 .. 1/48 oktáv) energiaspektrumon.
 */

#include <algorithm>
#include <cmath>
#ifndef M_PI   // szigorú (-std=c++17) módban a Windows / macOS fordító nem adja
#define M_PI 3.14159265358979323846
#endif
#include <complex>
#include <cstdint>
#include <vector>

namespace aqulify::room {

struct FreqPoint {
    float freq = 0.0f;
    float magDb = 0.0f;
    float phaseDeg = 0.0f;
};

/**
 * Frakcionális oktáv simítás egy pontsorozatra vagy spektrumra.
 * @param rawPoints Bemeneti (freq, magDb) pontok
 * @param octaveFraction Az oktáv tört része (pl. 1, 3, 6, 12, 24, 48)
 */
inline std::vector<FreqPoint> smoothSpectrum(const std::vector<FreqPoint>& rawPoints,
                                             int octaveFraction = 6) {
    if (rawPoints.empty()) return {};
    if (octaveFraction <= 0) return rawPoints;

    std::vector<FreqPoint> out = rawPoints;
    const size_t n = rawPoints.size();
    const double factor = std::pow(2.0, 1.0 / (2.0 * octaveFraction));

    // Lineáris energiák előkészítése
    std::vector<double> power(n);
    for (size_t i = 0; i < n; ++i) {
        power[i] = std::pow(10.0, rawPoints[i].magDb / 10.0);
    }

    for (size_t i = 0; i < n; ++i) {
        const double f0 = rawPoints[i].freq;
        const double fMin = f0 / factor;
        const double fMax = f0 * factor;

        double sumPower = 0.0;
        double sumWeights = 0.0;

        for (size_t k = 0; k < n; ++k) {
            const double f = rawPoints[k].freq;
            if (f >= fMin && f <= fMax) {
                // Háromszög vagy Gauss simító súly
                const double dist = std::abs(std::log2(f / f0));
                const double maxDist = std::log2(factor);
                const double w = std::max(0.0, 1.0 - dist / maxDist);
                sumPower += power[k] * w;
                sumWeights += w;
            }
        }

        if (sumWeights > 1e-12) {
            const double avgPower = sumPower / sumWeights;
            out[i].magDb = static_cast<float>(10.0 * std::log10(std::max(1e-12, avgPower)));
        }
    }

    return out;
}

/**
 * Mikrofon-kalibráció érvényesítése és interpolációja a mérési pontokra.
 * @param measured A mért frekvenciapontok
 * @param micCal A mikrofon kalibrációs pontjai (freq, corrDb)
 */
inline void applyMicCalibration(std::vector<FreqPoint>& measured,
                                const std::vector<FreqPoint>& micCal) {
    if (measured.empty() || micCal.empty()) return;

    for (auto& pt : measured) {
        // Log-frekvenciás lineáris interpoláció
        if (pt.freq <= micCal.front().freq) {
            pt.magDb -= micCal.front().magDb;
            continue;
        }
        if (pt.freq >= micCal.back().freq) {
            pt.magDb -= micCal.back().magDb;
            continue;
        }

        size_t lo = 0, hi = micCal.size() - 1;
        while (hi - lo > 1) {
            const size_t mid = (lo + hi) >> 1;
            if (micCal[mid].freq <= pt.freq) lo = mid;
            else hi = mid;
        }

        const double logF = std::log10(pt.freq);
        const double logL = std::log10(micCal[lo].freq);
        const double logH = std::log10(micCal[hi].freq);
        const double t = (logH > logL) ? ((logF - logL) / (logH - logL)) : 0.0;

        const float corr = static_cast<float>(micCal[lo].magDb + t * (micCal[hi].magDb - micCal[lo].magDb));
        pt.magDb -= corr; // A mikrofon saját kiemelését levonjuk
    }
}

/**
 * Komplex frekvenciaválasz (amplitúdómenet dB-ben) számítása impulzusválaszból (DFT logaritmikus frekvenciákon).
 */
inline std::vector<FreqPoint> calculateFrequencyResponse(const std::vector<float>& ir,
                                                         uint32_t sampleRate,
                                                         int numPoints = 240) {
    std::vector<FreqPoint> out;
    if (ir.empty() || sampleRate == 0) return out;
    out.reserve(numPoints);
    const double fMin = 20.0, fMax = std::min(20000.0, sampleRate * 0.48);
    for (int i = 0; i < numPoints; ++i) {
        const double f = fMin * std::pow(fMax / fMin, static_cast<double>(i) / std::max(1, numPoints - 1));
        std::complex<double> acc(0, 0);
        for (size_t k = 0; k < ir.size(); ++k) {
            acc += static_cast<double>(ir[k]) * std::polar(1.0, -2.0 * M_PI * f * k / sampleRate);
        }
        float magDb = static_cast<float>(20.0 * std::log10(std::max(1e-12, std::abs(acc))));
        out.push_back({ static_cast<float>(f), magDb, 0.0f });
    }
    return out;
}

} // namespace aqulify::room
