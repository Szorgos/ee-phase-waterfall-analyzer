#pragma once
/**
 * @file auto_eq.hpp
 * @brief Automatikus szobakorrekció (Auto-EQ), célgörbe illesztés, gain limitek, többpontos átlagolás.
 */

#include "smoothing.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace aqulify::room {

struct AutoEqBand {
    int type = 1;         // 1 = Peaking (Bell), 3 = Highshelf, 5 = Lowshelf
    float freq = 1000.0f;
    float gainDb = 0.0f;
    float q = 1.0f;
};

struct AutoEqResult {
    std::vector<AutoEqBand> bands;
    float peakErrorDb = 0.0f;
    float rmsErrorDb = 0.0f;
    float targetPreampDb = 0.0f;
};

/**
 * Több mérési pozíció energiaspektrumának átlagolása (Power Averaging).
 * Elkerüli a fáziskioltásokat.
 */
inline std::vector<FreqPoint> powerAveragePositions(const std::vector<std::vector<FreqPoint>>& positions) {
    if (positions.empty()) return {};
    if (positions.size() == 1) return positions[0];

    const size_t numPoints = positions[0].size();
    std::vector<FreqPoint> avg = positions[0];

    for (size_t i = 0; i < numPoints; ++i) {
        double sumPower = 0.0;
        size_t count = 0;
        for (const auto& pos : positions) {
            if (i < pos.size()) {
                sumPower += std::pow(10.0, pos[i].magDb / 10.0);
                count++;
            }
        }
        if (count > 0 && sumPower > 1e-12) {
            avg[i].magDb = static_cast<float>(10.0 * std::log10(sumPower / count));
        }
    }

    return avg;
}

/**
 * Egy másodfokú peaking vagy shelf szűrő átvitele analóg prototípus szerint (dB).
 */
inline float evalFilterDb(const AutoEqBand& b, float f) {
    if (b.type == 0) return 0.0f;
    const double w = static_cast<double>(f) / std::max(1.0f, b.freq);
    const double q = std::max(0.1f, b.q);
    const double g = b.gainDb;

    if (b.type == 1) { // Peaking
        const double A = std::pow(10.0, g / 40.0);
        const double num_re = 1.0 - w * w;
        const double num_im = w * (A / q);
        const double den_re = 1.0 - w * w;
        const double den_im = w / (A * q);
        const double mag2 = (num_re * num_re + num_im * num_im) / std::max(1e-12, (den_re * den_re + den_im * den_im));
        return static_cast<float>(10.0 * std::log10(std::max(1e-12, mag2)));
    } else if (b.type == 3) { // Highshelf
        return static_cast<float>(g * (w * w) / (1.0 + w * w));
    } else if (b.type == 5) { // Lowshelf
        return static_cast<float>(g / (1.0 + w * w));
    }
    return 0.0f;
}

/**
 * Célgörbéhez illesztett parametrikus korrekció számítása.
 * @param measured Mért (és simított, kalibrált) frekvenciamenet
 * @param target Célgörbe (ha üres, 0 dB vízszintes cél)
 * @param maxBands Legfeljebb ennyi parametrikus sáv (alapból 10, max 32)
 * @param maxBoostDb Legnagyobb megengedett emelés (alapértelmezett +3.0 dB hangszóró-védelemhez)
 * @param minCutDb Legnagyobb megengedett vágás (pl. -24.0 dB szobamódusokhoz)
 */
inline AutoEqResult calculateAutoEq(const std::vector<FreqPoint>& measured,
                                    const std::vector<FreqPoint>& target = {},
                                    uint32_t maxBands = 10,
                                    float maxBoostDb = 3.0f,
                                    float minCutDb = -24.0f) {
    AutoEqResult res;
    if (measured.empty()) return res;

    const size_t n = measured.size();
    std::vector<float> errorCurve(n, 0.0f);
    std::vector<float> freqs(n);

    // Különbség képzése a célgörbéhez képest
    for (size_t i = 0; i < n; ++i) {
        freqs[i] = measured[i].freq;
        float targetDb = 0.0f;
        if (!target.empty()) {
            if (freqs[i] <= target.front().freq) targetDb = target.front().magDb;
            else if (freqs[i] >= target.back().freq) targetDb = target.back().magDb;
            else {
                size_t lo = 0, hi = target.size() - 1;
                while (hi - lo > 1) {
                    const size_t mid = (lo + hi) >> 1;
                    if (target[mid].freq <= freqs[i]) lo = mid;
                    else hi = mid;
                }
                const double logF = std::log10(freqs[i]);
                const double logL = std::log10(target[lo].freq);
                const double logH = std::log10(target[hi].freq);
                const double t = (logH > logL) ? ((logF - logL) / (logH - logL)) : 0.0;
                targetDb = static_cast<float>(target[lo].magDb + t * (target[hi].magDb - target[lo].magDb));
            }
        }

        // Korrekciós igény = Target - Measured
        float needed = targetDb - measured[i].magDb;
        // Gain limitek érvényesítése (hangszóró- és klippelésvédelem)
        needed = std::clamp(needed, minCutDb, maxBoostDb);
        errorCurve[i] = needed;
    }

    // Iteratív sávcsökkentő / szűrőillesztés a reziduális hibára
    std::vector<float> residual = errorCurve;

    for (uint32_t bIdx = 0; bIdx < maxBands; ++bIdx) {
        // Legnagyobb abszolút hiba keresése
        size_t maxIdx = 0;
        float maxErr = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            if (std::abs(residual[i]) > std::abs(maxErr)) {
                maxErr = residual[i];
                maxIdx = i;
            }
        }

        if (std::abs(maxErr) < 0.25f) break; // Elértük a kívánt toleranciát

        const float peakF = freqs[maxIdx];
        const float peakGain = maxErr;

        // Q becslése félérték-szélességből
        const float halfVal = peakGain * 0.5f;
        float leftF = 20.0f, rightF = 20000.0f;

        for (int i = static_cast<int>(maxIdx) - 1; i >= 0; --i) {
            if ((peakGain > 0.0f && residual[i] <= halfVal) || (peakGain < 0.0f && residual[i] >= halfVal)) {
                leftF = freqs[i];
                break;
            }
        }
        for (size_t i = maxIdx + 1; i < n; ++i) {
            if ((peakGain > 0.0f && residual[i] <= halfVal) || (peakGain < 0.0f && residual[i] >= halfVal)) {
                rightF = freqs[i];
                break;
            }
        }

        const double bwOct = std::log2(std::max(1.05, static_cast<double>(rightF) / std::max(1.0f, leftF)));
        const double qEst = std::sqrt(std::pow(2.0, bwOct)) / std::max(0.01, std::pow(2.0, bwOct) - 1.0);
        const float q = std::clamp(static_cast<float>(qEst), 0.5f, 10.0f);

        AutoEqBand band;
        band.type = 1; // Peaking
        band.freq = peakF;
        band.gainDb = std::clamp(peakGain, minCutDb, maxBoostDb);
        band.q = q;

        res.bands.push_back(band);

        // Reziduum frissítése
        for (size_t i = 0; i < n; ++i) {
            residual[i] -= evalFilterDb(band, freqs[i]);
        }
    }

    // Csúcs- és RMS hiba kiszámítása
    float peakErr = 0.0f;
    double sumSq = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const float e = std::abs(residual[i]);
        peakErr = std::max(peakErr, e);
        sumSq += static_cast<double>(e) * e;
    }
    res.peakErrorDb = peakErr;
    res.rmsErrorDb = static_cast<float>(std::sqrt(sumSq / std::max<size_t>(1, n)));

    return res;
}

} // namespace aqulify::room
