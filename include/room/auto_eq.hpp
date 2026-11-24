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

struct AutoEqLimits {
    float fullCorrectionMaxHz = 500.0f;  // teljes erejű korrekció felső határa (Hz)
    float highMaxGainDb = 2.5f;          // felső határ fölött legnagyobb eltérés (±dB)
    float highMaxQ = 1.0f;               // felső határ fölött csak széles szűrő (Q <= 1,0)
    float maxBoostQ = 3.0f;              // emelésnél legnagyobb Q (keskeny kioltást nem tölt fel)
    float maxCutQ = 10.0f;               // vágásnál legnagyobb Q
    float rolloffDropDb = 6.0f;          // hangszóró alsó határ: -6 dB a 300 Hz–3 kHz átlaghoz képest
    bool enforceRolloffGuard = true;     // alsó határ alatt tilos emelni
    bool enforceTotalBoostLimit = true;  // a sávok összegzett emelése se lépje túl a maxBoostDb-t
};

struct AutoEqResult {
    std::vector<AutoEqBand> bands;
    float peakErrorDb = 0.0f;
    float rmsErrorDb = 0.0f;
    float targetPreampDb = 0.0f;
    float speakerLowCutoffHz = 20.0f;
    std::vector<float> skippedDips;      // kihagyott mély, keskeny kioltások frekvenciái (Hz)
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
 * Célgörbéhez illesztett parametrikus korrekció számítása biztonsági határokkal.
 * @param measured Mért (és simított, kalibrált) frekvenciamenet
 * @param target Célgörbe (ha üres, 0 dB vízszintes cél)
 * @param maxBands Legfeljebb ennyi parametrikus sáv (alapból 10, max 32)
 * @param maxBoostDb Legnagyobb megengedett emelés (összegzetten is, alapból +3.0 dB)
 * @param minCutDb Legnagyobb megengedett vágás (pl. -24.0 dB szobamódusokhoz)
 * @param limits Konfigurálható korrekciós határok (felső határ, alsó határ, keskeny emelés, összegzett korlát)
 */
inline AutoEqResult calculateAutoEq(const std::vector<FreqPoint>& measured,
                                    const std::vector<FreqPoint>& target = {},
                                    uint32_t maxBands = 10,
                                    float maxBoostDb = 3.0f,
                                    float minCutDb = -24.0f,
                                    const AutoEqLimits& limits = AutoEqLimits{}) {
    AutoEqResult res;
    if (measured.empty()) return res;

    const size_t n = measured.size();
    std::vector<float> errorCurve(n, 0.0f);
    std::vector<float> rawNeeded(n, 0.0f);
    std::vector<float> freqs(n);

    // 1. Hangszóró alsó határának becslése: -6 dB pont a 300 Hz – 3 kHz átlaghoz képest
    double midSum = 0.0;
    int midCnt = 0;
    for (const auto& p : measured) {
        if (p.freq >= 300.0f && p.freq <= 3000.0f) {
            midSum += p.magDb;
            ++midCnt;
        }
    }
    const float midRefDb = midCnt > 0 ? static_cast<float>(midSum / midCnt) : 0.0f;
    float lowCutoffHz = measured.front().freq;
    if (limits.enforceRolloffGuard) {
        const float thresh = midRefDb - limits.rolloffDropDb;
        lowCutoffHz = 300.0f;
        for (const auto& p : measured) {
            if (p.freq > 300.0f) break;
            if (p.magDb >= thresh) {
                lowCutoffHz = p.freq;
                break;
            }
        }
    }
    res.speakerLowCutoffHz = lowCutoffHz;

    // 2. Különbség képzése a célgörbéhez képest
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
        rawNeeded[i] = needed;
        // A hangszóró saját alsó határa alatt tilos emelni
        if (limits.enforceRolloffGuard && freqs[i] < lowCutoffHz && needed > 0.0f) {
            needed = 0.0f;
        }
        // Gain limitek érvényesítése (hangszóró- és klippelésvédelem)
        needed = std::clamp(needed, minCutDb, maxBoostDb);
        errorCurve[i] = needed;
    }

    // 3. Iteratív szűrőillesztés a reziduális hibára
    std::vector<float> residual = errorCurve;
    std::vector<float> totalEq(n, 0.0f);
    std::vector<bool> masked(n, false);

    auto recordSkippedDip = [&](float freq) {
        for (float existing : res.skippedDips) {
            if (std::fabs(std::log2(freq / std::max(1.0f, existing))) < 0.35f) return;
        }
        res.skippedDips.push_back(freq);
    };

    const uint32_t maxAttempts = std::max<uint32_t>(maxBands * 8, 24);
    for (uint32_t attempt = 0; attempt < maxAttempts && res.bands.size() < maxBands; ++attempt) {
        // Legnagyobb abszolút hiba keresése a nem maszkolt pontokon
        size_t maxIdx = 0;
        float maxErr = 0.0f;
        bool found = false;
        for (size_t i = 0; i < n; ++i) {
            if (masked[i]) continue;
            if (std::abs(residual[i]) > std::abs(maxErr)) {
                maxErr = residual[i];
                maxIdx = i;
                found = true;
            }
        }

        if (!found || std::abs(maxErr) < 0.25f) break; // Elértük a kívánt toleranciát

        const float peakF = freqs[maxIdx];
        const float peakGain = maxErr;

        // Q becslése félérték-szélességből
        const float halfVal = peakGain * 0.5f;
        size_t leftIdx = 0, rightIdx = n - 1;
        float leftF = freqs.front(), rightF = freqs.back();

        for (int i = static_cast<int>(maxIdx) - 1; i >= 0; --i) {
            if ((peakGain > 0.0f && residual[i] <= halfVal) || (peakGain < 0.0f && residual[i] >= halfVal)) {
                leftIdx = static_cast<size_t>(i);
                leftF = freqs[i];
                break;
            }
        }
        for (size_t i = maxIdx + 1; i < n; ++i) {
            if ((peakGain > 0.0f && residual[i] <= halfVal) || (peakGain < 0.0f && residual[i] >= halfVal)) {
                rightIdx = i;
                rightF = freqs[i];
                break;
            }
        }

        const double bwOct = std::log2(std::max(1.05, static_cast<double>(rightF) / std::max(1.0f, leftF)));
        const double qEst = std::sqrt(std::pow(2.0, bwOct)) / std::max(0.01, std::pow(2.0, bwOct) - 1.0);

        auto maskSpan = [&]() {
            const size_t lo = (leftIdx < maxIdx) ? leftIdx : (maxIdx > 0 ? maxIdx - 1 : 0);
            const size_t hi = (rightIdx > maxIdx) ? rightIdx : std::min(n - 1, maxIdx + 1);
            for (size_t i = lo; i <= hi; ++i) masked[i] = true;
        };

        // Hangszóró alsó határa alatt nincs emelés
        if (peakGain > 0.0f && limits.enforceRolloffGuard && peakF < lowCutoffHz) {
            maskSpan();
            continue;
        }

        // Pozitív igénynél a clampelt plató hamis, túl kicsi Q-t adna: a valódi kioltás szélességét a nyers igényen mérjük
        if (peakGain > 0.0f && peakF <= limits.fullCorrectionMaxHz) {
            size_t rawPeakIdx = maxIdx;
            float rawPeakVal = rawNeeded[maxIdx];
            const size_t sLo = (leftIdx < maxIdx) ? leftIdx : (maxIdx > 0 ? maxIdx - 1 : 0);
            const size_t sHi = (rightIdx > maxIdx) ? rightIdx : std::min(n - 1, maxIdx + 1);
            for (size_t i = sLo; i <= sHi; ++i) {
                if (rawNeeded[i] > rawPeakVal) {
                    rawPeakVal = rawNeeded[i];
                    rawPeakIdx = i;
                }
            }
            const float rawHalf = rawPeakVal * 0.5f;
            float rLeftF = freqs.front(), rRightF = freqs.back();
            for (int i = static_cast<int>(rawPeakIdx) - 1; i >= 0; --i) {
                if (rawNeeded[i] <= rawHalf) { rLeftF = freqs[i]; break; }
            }
            for (size_t i = rawPeakIdx + 1; i < n; ++i) {
                if (rawNeeded[i] <= rawHalf) { rRightF = freqs[i]; break; }
            }
            const double rawBwOct = std::log2(std::max(1.05, static_cast<double>(rRightF) / std::max(1.0f, rLeftF)));
            const double rawQEst = std::sqrt(std::pow(2.0, rawBwOct)) / std::max(0.01, std::pow(2.0, rawBwOct) - 1.0);
            if (rawQEst > static_cast<double>(limits.maxBoostQ)) {
                if (rawPeakVal >= 3.5f) recordSkippedDip(freqs[rawPeakIdx]);
                // A teljes kioltás környékét lemaszkoljuk, hogy a plató szélére se kerüljön emelés
                size_t mLo = rawPeakIdx, mHi = rawPeakIdx;
                while (mLo > 0 && rawNeeded[mLo - 1] > 0.5f) --mLo;
                while (mHi + 1 < n && rawNeeded[mHi + 1] > 0.5f) ++mHi;
                for (size_t i = mLo; i <= mHi; ++i) masked[i] = true;
                continue;
            }
        }

        float bandGain = peakGain;
        float bandQ = static_cast<float>(qEst);

        if (peakF > limits.fullCorrectionMaxHz) {
            // Felső határ fölött keskeny (fésűszűrő) eltéréseket nem korrigálunk; csak széles, kis sáv engedett
            if (qEst > static_cast<double>(limits.highMaxQ) * 1.5 || limits.highMaxGainDb < 0.25f) {
                maskSpan();
                continue;
            }
            bandQ = std::clamp(static_cast<float>(qEst), 0.5f, limits.highMaxQ);
            const float hiMaxBoost = std::min(maxBoostDb, limits.highMaxGainDb);
            const float hiMinCut = std::max(minCutDb, -limits.highMaxGainDb);
            bandGain = std::clamp(peakGain, hiMinCut, hiMaxBoost);
            if (std::abs(bandGain) < 0.35f) {
                maskSpan();
                continue;
            }
        } else if (peakGain > 0.0f) {
            // Mély, keskeny kioltás: EQ nem tölti fel (csak a terhelés nőne)
            if (qEst > static_cast<double>(limits.maxBoostQ)) {
                if (rawNeeded[maxIdx] >= 3.5f) recordSkippedDip(peakF);
                maskSpan();
                continue;
            }
            bandQ = std::clamp(static_cast<float>(qEst), 0.5f, limits.maxBoostQ);
            bandGain = std::clamp(peakGain, 0.0f, maxBoostDb);
        } else {
            bandQ = std::clamp(static_cast<float>(qEst), 0.5f, limits.maxCutQ);
            bandGain = std::clamp(peakGain, minCutDb, 0.0f);
        }

        AutoEqBand band;
        band.type = 1; // Peaking
        band.freq = peakF;
        band.gainDb = bandGain;
        band.q = bandQ;

        // Összegzett emelés korlátozása: az egymást átfedő sávok összege se lépje túl a maxBoostDb-t
        if (limits.enforceTotalBoostLimit && band.gainDb > 0.0f) {
            auto maxTotalWith = [&](float gTest) {
                AutoEqBand tmp = band;
                tmp.gainDb = gTest;
                float pk = 0.0f;
                for (size_t i = 0; i < n; ++i) {
                    const float v = totalEq[i] + evalFilterDb(tmp, freqs[i]);
                    if (v > pk) pk = v;
                }
                return pk;
            };
            if (maxTotalWith(band.gainDb) > maxBoostDb) {
                float lo = 0.0f, hi = band.gainDb;
                for (int it = 0; it < 14; ++it) {
                    const float mid = 0.5f * (lo + hi);
                    if (maxTotalWith(mid) <= maxBoostDb) lo = mid;
                    else hi = mid;
                }
                band.gainDb = lo;
                if (band.gainDb < 0.25f) {
                    maskSpan();
                    continue;
                }
            }
        }

        res.bands.push_back(band);

        // Reziduum és összegzett EQ frissítése
        for (size_t i = 0; i < n; ++i) {
            const float fDb = evalFilterDb(band, freqs[i]);
            totalEq[i] += fDb;
            residual[i] -= fDb;
        }
    }

    // 4. Csúcs- és RMS hiba, valamint szükséges előerősítés (preamp) számítása
    float peakErr = 0.0f;
    double sumSq = 0.0;
    float maxPositiveEq = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float e = std::abs(residual[i]);
        peakErr = std::max(peakErr, e);
        sumSq += static_cast<double>(e) * e;
        if (totalEq[i] > maxPositiveEq) maxPositiveEq = totalEq[i];
    }
    res.peakErrorDb = peakErr;
    res.rmsErrorDb = static_cast<float>(std::sqrt(sumSq / std::max<size_t>(1, n)));
    res.targetPreampDb = maxPositiveEq > 0.01f ? -maxPositiveEq : 0.0f;

    return res;
}

} // namespace aqulify::room
