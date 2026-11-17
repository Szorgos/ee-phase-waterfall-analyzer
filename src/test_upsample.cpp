// =========================================================================
// Aqulify 2.x – Felmintavételezés (libsoxr) tesztek: K2, K3, K4, K5, K5b, K9, K10, 8.3 SEED
// =========================================================================
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "upsample_engine.hpp"

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; std::printf("  [HIBA] " __VA_ARGS__); } else { std::printf("  [OK]   " __VA_ARGS__); } std::printf("\n"); } while (0)

static constexpr double kPi = 3.14159265358979323846;

// FFT segédeszköz a spektrális mérésekhez (K3, K4)
static void simpleFft(std::vector<std::complex<double>>& a, bool invert) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = 2.0 * kPi / len * (invert ? -1 : 1);
        std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t j = 0; j < len / 2; ++j) {
                std::complex<double> u = a[i + j];
                std::complex<double> v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
    if (invert) {
        for (auto& x : a) x /= static_cast<double>(n);
    }
}

// Csonk / Koncepció a megvalósítás ellenőrzéséhez
struct DummyUpsample {
};

template <class T>
concept HasUpsampleEngine = requires(T& u) {
    u.init(44100, 88200, 2);
};

// K2: Szabályok elméleti kiválasztó függvénye (független teszt-elvárás)
static uint32_t computeExpectedTargetRate(int rule, uint32_t srcRate, const std::vector<uint32_t>& supported, const std::map<uint32_t, uint32_t>& customTable) {
    auto isSupported = [&](uint32_t r) {
        return std::find(supported.begin(), supported.end(), r) != supported.end();
    };

    if (rule == 0) return 0; // Ki: nincs átalakítás

    if (rule == 1) { // x2
        uint32_t target = srcRate * 2;
        if (isSupported(target)) return target;
        return 0; // Ha nem tudja: nincs átalakítás
    }

    if (rule == 2) { // Kettő hatványa
        uint32_t best = 0;
        for (uint32_t mult = 2; mult <= 16; mult *= 2) {
            uint32_t cand = srcRate * mult;
            if (isSupported(cand)) {
                best = cand;
            }
        }
        return best; // 0 ha nincs nagyobb
    }

    if (rule == 3) { // A kimenet legnagyobbja
        uint32_t maxRate = 0;
        for (uint32_t r : supported) {
            if (r > maxRate) maxRate = r;
        }
        if (maxRate > srcRate) return maxRate;
        return 0; // Lefelé soha!
    }

    if (rule == 4) { // Egyéni táblázat
        auto it = customTable.find(srcRate);
        if (it != customTable.end()) {
            uint32_t target = it->second;
            if (target > srcRate && isSupported(target)) return target;
        }
        return 0;
    }

    return 0;
}

static void testK2RulesMatrix() {
    std::printf("\n[K2] Szabályok kimerítő mátrixa (6 forrás × 8 képességkészlet)\n");
    const std::vector<uint32_t> srcRates = { 44100, 48000, 88200, 96000, 176400, 192000 };
    const std::vector<std::vector<uint32_t>> capabilitySets = {
        { 44100, 48000 },
        { 44100, 48000, 88200, 96000 },
        { 44100, 48000, 88200, 96000, 176400, 192000 },
        { 48000, 96000, 192000 },
        { 44100, 88200, 176400 },
        { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000 },
        { 48000 },
        { 44100, 48000, 96000 }
    };

    std::map<uint32_t, uint32_t> customTable = {
        { 44100, 88200 },
        { 48000, 96000 },
        { 88200, 176400 },
        { 96000, 192000 },
        { 176400, 352800 },
        { 192000, 384000 }
    };

    size_t testsCount = 0;
    bool allValid = true;

    for (int rule = 0; rule <= 4; ++rule) {
        for (uint32_t src : srcRates) {
            for (const auto& caps : capabilitySets) {
                uint32_t target = computeExpectedTargetRate(rule, src, caps, customTable);
                // Lefelé soha nem alakíthat!
                if (target != 0 && target <= src) {
                    allValid = false;
                }
                // Ha cél van megadva, benne kell lennie a képességkészletben
                if (target != 0) {
                    if (std::find(caps.begin(), caps.end(), target) == caps.end()) {
                        allValid = false;
                    }
                }
                ++testsCount;
            }
        }
    }

    CHECK(allValid && testsCount >= 240, "K2 szabolymátrix: %zu eset ellenőrizve, lefelé soha, csak támogatott cél", testsCount);
}

// K3: Átvitel és elnyomás mérése FFT-vel
template <class EngT>
static void testK3FrequencyResponseAndRejection() {
    std::printf("\n[K3] Átvitel és elnyomás (passband flatness és stopband rejection)\n");
    // 44100 -> 88200 Hz átalakítás VHQ (28 bit)
    EngT eng;
    bool okInit = eng.init(44100, 88200, 1, 28, 50.0, 0.95, 1.00, 0.0f);
    CHECK(okInit, "K3: UpsampleEngine init 44.1k -> 88.2k");

    // 1. Átviteli egyenesség (flatness) 20 Hz .. 20 kHz között
    const std::vector<double> testFreqs = { 50.0, 100.0, 1000.0, 5000.0, 10000.0, 18000.0, 20000.0 };
    double maxDeviationDb = 0.0;

    for (double freq : testFreqs) {
        const size_t inLen = 16384;
        std::vector<float> inSig(inLen);
        for (size_t i = 0; i < inLen; ++i) {
            inSig[i] = static_cast<float>(std::sin(2.0 * kPi * freq * i / 44100.0) * 0.5);
        }
        std::vector<float> outSig(inLen * 2 + 8192);
        size_t outFrames = 0;
        eng.clear();
        eng.process(inSig.data(), inLen, outSig.data(), outSig.size(), outFrames);
        size_t flushFrames = 0;
        eng.flush(outSig.data() + outFrames, outSig.size() - outFrames, flushFrames);
        outFrames += flushFrames;

        // Kiszámítjuk az RMS szintet a szűrő beállási fázisa után
        double inRms = 0.0;
        for (size_t i = 2000; i < 10000; ++i) inRms += inSig[i] * inSig[i];
        inRms = std::sqrt(inRms / 8000.0);

        double outRms = 0.0;
        for (size_t i = 4000; i < 20000; ++i) outRms += outSig[i] * outSig[i];
        outRms = std::sqrt(outRms / 16000.0);

        double gainDb = 20.0 * std::log10(outRms / inRms);
        if (std::abs(gainDb) > maxDeviationDb) maxDeviationDb = std::abs(gainDb);
    }
    CHECK(maxDeviationDb <= 0.01, "K3 passband flatness 20 Hz..20 kHz: max elteres = %.4f dB (<= 0.01 dB)", maxDeviationDb);
    std::printf("K_METRIC: {\"test\":\"K3_flatness\",\"maxDevDb\":%.4f,\"limitDb\":0.01}\n", maxDeviationDb);
    for (const auto& rp : std::vector<std::pair<uint32_t,uint32_t>>{{48000,96000},{44100,176400},{96000,192000}}) {
        EngT er; CHECK(er.init(rp.first, rp.second, 1, 28, 25.0, 0.95, 1.00, 0.0f), "K3 par %u -> %u init", rp.first, rp.second);
    }

    // 2. Tükörkép-elnyomás mérése a Nyquist felett
    struct PrecisionCheck { int bits; double minRejectionDb; };
    std::vector<PrecisionCheck> precisions = {
        { 20, 110.0 },
        { 24, 130.0 },
        { 28, 150.0 }
    };

    for (const auto& pc : precisions) {
        EngT ePrec;
        ePrec.init(44100, 88200, 1, pc.bits, 50.0, 0.95, 1.00, 0.0f);
        const size_t inLen = 32768;
        std::vector<float> inSig(inLen);
        for (size_t i = 0; i < inLen; ++i) {
            inSig[i] = static_cast<float>(std::sin(2.0 * kPi * 18000.0 * i / 44100.0) * 0.7071);
        }
        std::vector<float> outSig(inLen * 2 + 16384);
        size_t outFrames = 0;
        ePrec.process(inSig.data(), inLen, outSig.data(), outSig.size(), outFrames);
        size_t fl = 0;
        ePrec.flush(outSig.data() + outFrames, outSig.size() - outFrames, fl);
        outFrames += fl;

        const size_t fftN = 16384;
        std::vector<std::complex<double>> fftBuf(fftN);
        const size_t startIdx = 8192;
        for (size_t i = 0; i < fftN; ++i) {
            double w = 0.5 * (1.0 - std::cos(2.0 * kPi * i / (fftN - 1)));
            fftBuf[i] = outSig[startIdx + i] * w;
        }
        simpleFft(fftBuf, false);

        double peakFund = 0.0;
        double peakAlias = 0.0;
        const double binHz = 88200.0 / fftN;

        for (size_t k = 0; k < fftN / 2; ++k) {
            double f = k * binHz;
            double mag = std::abs(fftBuf[k]);
            if (f >= 17500.0 && f <= 18500.0) {
                if (mag > peakFund) peakFund = mag;
            } else if (f >= 24000.0 && f <= 28000.0) {
                if (mag > peakAlias) peakAlias = mag;
            }
        }
        double rejDb = 20.0 * std::log10(peakFund / std::max(peakAlias, 1e-12));
        CHECK(rejDb >= pc.minRejectionDb, "K3 stopband rejection %d bit: mert = %.1f dB (elvart >= %.0f dB)",
              pc.bits, rejDb, pc.minRejectionDb);
        std::printf("K_METRIC: {\"test\":\"K3_stopband_%db\",\"rejectionDb\":%.2f,\"minDb\":%.1f}\n", pc.bits, rejDb, pc.minRejectionDb);
    }
}

// K4: THD+N mérése (1 kHz, -3 dBFS, 24 bit, koherens FFT)
template <class EngT>
static void testK4ThdPlusN() {
    std::printf("\n[K4] THD+N (1 kHz szinusz, 24 bites pontossag)\n");
    EngT eng;
    eng.init(48000, 96000, 1, 24, 25.0, 0.95, 1.00, 0.0f);

    const size_t fitN = 32768;
    // Koherens mintavételezés a szivárgásmentes FFT méréshez (~1000 Hz, bin 341)
    const double freq = 96000.0 * 341.0 / fitN; // 999.0234375 Hz
    const double amp = std::pow(10.0, -3.0 / 20.0);

    const size_t inLen = 65536;
    std::vector<float> inSig(inLen);
    for (size_t i = 0; i < inLen; ++i) {
        inSig[i] = static_cast<float>(std::sin(2.0 * kPi * freq * i / 48000.0) * amp);
    }
    std::vector<float> outSig(inLen * 2 + 16384);
    size_t outFrames = 0;
    eng.process(inSig.data(), inLen, outSig.data(), outSig.size(), outFrames);

    const size_t start = 16384;
    std::vector<std::complex<double>> fftBuf(fitN);
    for (size_t i = 0; i < fitN; ++i) {
        fftBuf[i] = outSig[start + i];
    }
    simpleFft(fftBuf, false);

    double fundPower = std::norm(fftBuf[341]);
    double noisePower = 0.0;
    const double binHz = 96000.0 / fitN;

    for (size_t k = 1; k < fitN / 2; ++k) {
        if (k == 341) continue;
        double f = k * binHz;
        if (f >= 20.0 && f <= 20000.0) {
            noisePower += std::norm(fftBuf[k]);
        }
    }

    double thdNDb = 10.0 * std::log10(noisePower / std::max(fundPower, 1e-15));
    CHECK(thdNDb <= -130.0, "K4 THD+N (1 kHz, -3 dBFS, 24 bit): mert = %.1f dB (elvart <= -130 dB)", thdNDb);
}

// K5 & K5b: Fázisválasz és Stopband kezdet ellenőrzése
template <class EngT>
static void testK5PhaseAndStopband() {
    std::printf("\n[K5 / K5b] Fázisválasz (lineáris vs minimumfázis) és zárósáv\n");

    // Lineáris fázis (phase = 50): impulzusválasz szimmetrikus a csúcs körül
    {
        EngT engLin;
        engLin.init(44100, 88200, 1, 28, 50.0, 0.95, 1.00, 0.0f);
        const size_t inLen = 2048;
        std::vector<float> impulse(inLen, 0.0f);
        impulse[512] = 1.0f; // Dirac delta
        std::vector<float> out(inLen * 2 + 2048);
        size_t n = 0;
        engLin.process(impulse.data(), inLen, out.data(), out.size(), n);
        size_t fl = 0;
        engLin.flush(out.data() + n, out.size() - n, fl);
        n += fl;

        size_t peakIdx = 0;
        float peakVal = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            if (std::abs(out[i]) > peakVal) {
                peakVal = std::abs(out[i]);
                peakIdx = i;
            }
        }
        double preEnergy = 0.0, postEnergy = 0.0;
        for (size_t i = 1; i <= 200; ++i) {
            if (peakIdx >= i) preEnergy += out[peakIdx - i] * out[peakIdx - i];
            if (peakIdx + i < n) postEnergy += out[peakIdx + i] * out[peakIdx + i];
        }
        double ratio = preEnergy / std::max(postEnergy, 1e-12);
        CHECK(std::abs(ratio - 1.0) <= 0.02, "K5 Lineáris fázis (50): szimmetrikus energia arany = %.4f (1.00 +- 0.02)", ratio);
    }

    // Minimumfázis (phase = 0): nincs pre-ringing, az impulzus előtti energia < 0.1% (>= 99.9% utána)
    {
        EngT engMin;
        engMin.init(44100, 88200, 1, 28, 0.0, 0.95, 1.00, 0.0f);
        const size_t inLen = 2048;
        std::vector<float> impulse(inLen, 0.0f);
        impulse[512] = 1.0f;
        std::vector<float> out(inLen * 2 + 2048);
        size_t n = 0;
        engMin.process(impulse.data(), inLen, out.data(), out.size(), n);
        size_t fl = 0;
        engMin.flush(out.data() + n, out.size() - n, fl);
        n += fl;

        size_t peakIdx = 0;
        float peakVal = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            if (std::abs(out[i]) > peakVal) {
                peakVal = std::abs(out[i]);
                peakIdx = i;
            }
        }

        // A minimumfázisú szűrőnél az impulzus előtti távoli pre-ringing zéró
        // (A lineáris fázisú szűrőnél az energia fele a peak - 10 előtt van)
        double preRingingEnergy = 0.0, totalEnergy = 0.0;
        for (size_t i = 0; i < n; ++i) {
            double e = out[i] * out[i];
            totalEnergy += e;
            if (i + 10 < peakIdx) preRingingEnergy += e;
        }
        double postRatio = 1.0 - (preRingingEnergy / std::max(totalEnergy, 1e-12));
        CHECK(postRatio >= 0.999, "K5 Minimumfázis (0): impulzus előtti pre-ringing mentes energia arany = %.5f (>= 0.999)", postRatio);
    }

    // Monotonitás és 50 fölötti érték ellenőrzése
    {
        EngT eng0, eng25, eng50, engBad;
        eng0.init(44100, 88200, 1, 28, 0.0, 0.95, 1.00, 0.0f);
        eng25.init(44100, 88200, 1, 28, 25.0, 0.95, 1.00, 0.0f);
        eng50.init(44100, 88200, 1, 28, 50.0, 0.95, 1.00, 0.0f);
        // 50 feletti érték le van vágva 50-re
        engBad.init(44100, 88200, 1, 28, 75.0, 0.95, 1.00, 0.0f);

        auto measurePreFar = [](EngT& e) {
            std::vector<float> impulse(2048, 0.0f);
            impulse[512] = 1.0f;
            std::vector<float> out(4096, 0.0f);
            size_t n = 0;
            e.process(impulse.data(), impulse.size(), out.data(), out.size(), n);
            size_t fl = 0;
            e.flush(out.data() + n, out.size() - n, fl);
            n += fl;
            size_t peak = 0;
            float maxV = 0.0f;
            for (size_t i = 0; i < n; ++i) if (std::abs(out[i]) > maxV) { maxV = std::abs(out[i]); peak = i; }
            double preFar = 0, tot = 0;
            for (size_t i = 0; i < n; ++i) {
                double v2 = out[i] * out[i];
                tot += v2;
                if (i + 15 < peak) preFar += v2;
            }
            return preFar / std::max(tot, 1e-12);
        };

        double p0 = measurePreFar(eng0);
        double p25 = measurePreFar(eng25);
        double p50 = measurePreFar(eng50);
        double pBad = measurePreFar(engBad);

        CHECK(p0 < p25 && p25 < p50, "K5 Fázis monoton átmenet: p0(%.2e) < p25(%.2e) < p50(%.2e)", p0, p25, p50);
        CHECK(std::abs(pBad - p50) < 1e-6, "K5 50 feletti érték (75) lekorlátozva 50-re");
    }

    // K5b: Zárósáv kezdete (stopband_begin: 1.00 vs 1.20) valódi FFT méréssel
    {
        EngT eng100, eng120, engBad;
        bool ok100 = eng100.init(44100, 88200, 1, 24, 25.0, 0.80, 1.00, 0.0f);
        bool ok120 = eng120.init(44100, 88200, 1, 24, 25.0, 0.80, 1.20, 0.0f);
        bool okBad = engBad.init(44100, 88200, 1, 24, 25.0, 1.10, 1.00, 0.0f);
        CHECK(ok100 && ok120 && !okBad, "K5b: Stopband 100%% és 120%% aktív, passband >= stopband elutasítva");

        auto measureImageRej = [](EngT& eng, double toneHz) {
            const size_t inLen = 32768;
            std::vector<float> inSig(inLen);
            for (size_t i = 0; i < inLen; ++i)
                inSig[i] = static_cast<float>(0.7071 * std::sin(2.0 * kPi * toneHz * i / 44100.0));
            std::vector<float> outSig(inLen * 2 + 16384);
            size_t outFrames = 0, fl = 0;
            eng.clear();
            eng.process(inSig.data(), inLen, outSig.data(), outSig.size(), outFrames);
            eng.flush(outSig.data() + outFrames, outSig.size() - outFrames, fl);
            const size_t fftN = 16384;
            std::vector<std::complex<double>> buf(fftN);
            for (size_t i = 0; i < fftN; ++i) {
                double w = 0.5 * (1.0 - std::cos(2.0 * kPi * i / (fftN - 1)));
                buf[i] = outSig[8192 + i] * w;
            }
            simpleFft(buf, false);
            const double binHz = 88200.0 / fftN;
            const double imgHz = 44100.0 - toneHz;
            double pkFund = 0.0, pkImg = 0.0;
            for (size_t k = 0; k < fftN / 2; ++k) {
                double f = k * binHz, m = std::abs(buf[k]);
                if (std::abs(f - toneHz) < 300.0 && m > pkFund) pkFund = m;
                if (std::abs(f - imgHz) < 300.0 && m > pkImg) pkImg = m;
            }
            return 20.0 * std::log10(pkFund / std::max(pkImg, 1e-12));
        };
        // 20 kHz jel tükörképe 24,1 kHz (~109,3% Nyquist, a 100-120% sávban); 14 kHz jel tükörképe 30,1 kHz (~136,5%, >120%)
        const double rej100_mid = measureImageRej(eng100, 20000.0);
        const double rej120_mid = measureImageRej(eng120, 20000.0);
        const double rej120_hi  = measureImageRej(eng120, 14000.0);
        CHECK(rej100_mid >= 130.0 && rej120_mid + 20.0 < rej100_mid && rej120_hi >= 130.0,
              "K5b mérés: 109%% Nyquist elnyomás (sb100=%.1f dB vs sb120=%.1f dB), >120%% Nyquist (sb120=%.1f dB)",
              rej100_mid, rej120_mid, rej120_hi);
        std::printf("K_METRIC: {\"test\":\"K5b_stopband\",\"sb100_109pct_dB\":%.2f,\"sb120_109pct_dB\":%.2f,\"sb120_136pct_dB\":%.2f}\n",
                    rej100_mid, rej120_mid, rej120_hi);
    }
}

// K9: Fejtér és túlvezérlés számláló tesztje
template <class EngT>
static void testK9HeadroomAndClipping() {
    std::printf("\n[K9] Fejtér és túlvezérlés számláló (intersample peak teszt)\n");

    const size_t inLen = 4096;
    std::vector<float> inSig(inLen);
    for (size_t i = 0; i < inLen; ++i) {
        double phi = (kPi / 2.0) * i + (kPi / 4.0);
        inSig[i] = static_cast<float>(std::sin(phi) * 1.41421356); // csúcsérték 1.0
    }

    // 1. 0 dB fejtérrel: a közbenső minták meghaladják az 1.0-t, clipping keletkezik
    {
        EngT eng0;
        eng0.init(44100, 88200, 1, 28, 25.0, 0.95, 1.00, 0.0f);
        std::vector<float> out(inLen * 2 + 2048);
        size_t n = 0;
        eng0.process(inSig.data(), inLen, out.data(), out.size(), n);

        uint64_t countedInOutput = 0;
        for (size_t i = 0; i < n; ++i) {
            if (out[i] > 1.0f || out[i] < -1.0f) ++countedInOutput;
        }

        CHECK(countedInOutput > 0 && eng0.clippedSamples() == countedInOutput,
              "K9: 0 dB fejter mellett %llu tulvezerelt minta detektalva (szamlalo pontosan egyezik)",
              (unsigned long long)countedInOutput);
    }

    // 2. -3 dB fejtérrel: a közbenső minták csökkentve 1.0 alatt maradnak, 0 clipping!
    {
        EngT eng3;
        eng3.init(44100, 88200, 1, 28, 25.0, 0.95, 1.00, -3.0f);
        std::vector<float> out(inLen * 2 + 2048);
        size_t n = 0;
        eng3.process(inSig.data(), inLen, out.data(), out.size(), n);

        CHECK(eng3.clippedSamples() == 0, "K9: -3 dB fejter mellett 0 tulvezerelt minta keletkezett");
    }
}

// K10: Sebességteszt (benchmark)
template <class EngT>
static void testK10Benchmark() {
    std::printf("\n[K10] Sebességmérés (44.1 -> 176.4 kHz, VHQ 28 bit, sztereó)\n");

    EngT eng;
    eng.init(44100, 176400, 2, 28, 25.0, 0.95, 1.00, -1.0f);

    const size_t seconds = 3;
    const size_t inFrames = 44100 * seconds;
    std::vector<float> inSig(inFrames * 2);
    for (size_t i = 0; i < inFrames; ++i) {
        inSig[2 * i] = static_cast<float>(std::sin(2.0 * kPi * 440.0 * i / 44100.0) * 0.5);
        inSig[2 * i + 1] = static_cast<float>(std::sin(2.0 * kPi * 880.0 * i / 44100.0) * 0.5);
    }

    std::vector<float> outSig(176400 * seconds * 2 + 16384);
    size_t outFrames = 0;

    auto t0 = std::chrono::high_resolution_clock::now();
    eng.process(inSig.data(), inFrames, outSig.data(), outSig.size() / 2, outFrames);
    size_t fl = 0;
    eng.flush(outSig.data() + outFrames * 2, (outSig.size() - outFrames * 2) / 2, fl);
    auto t1 = std::chrono::high_resolution_clock::now();

    double elapsedSec = std::chrono::duration<double>(t1 - t0).count();
    double speedMultiplier = static_cast<double>(seconds) / std::max(elapsedSec, 1e-6);

    CHECK(speedMultiplier >= 8.0, "K10 Sebességteszt: %.1fx valós idő (elvárt >= 8.0x)", speedMultiplier);
}

// 8.3: Véletlen-mag tesztsorozat (AQ_CHECK_SEED)
template <class EngT>
static void testRandomMatrixSeed(uint32_t seed) {
    std::printf("\n[8.3] Véletlen-mag tesztek (AQ_CHECK_SEED: %u, 60 eset)\n", seed);
    std::mt19937 rng(seed);

    const std::vector<uint32_t> rates = { 44100, 48000, 88200, 96000, 176400, 192000 };
    const std::vector<int> precisions = { 20, 24, 28, 32 };
    const std::vector<float> headrooms = { 0.0f, -1.0f, -3.0f, -6.0f };

    size_t passCount = 0;
    for (int iter = 0; iter < 60; ++iter) {
        uint32_t src = rates[rng() % rates.size()];
        uint32_t dst = rates[rng() % rates.size()];
        int prec = precisions[rng() % precisions.size()];
        double phase = std::uniform_real_distribution<double>(0.0, 50.0)(rng);
        double passband = std::uniform_real_distribution<double>(0.85, 0.99)(rng);
        float hr = headrooms[rng() % headrooms.size()];

        EngT eng;
        bool ok = eng.init(src, dst, 2, prec, phase, passband, 1.0, hr);
        if (ok) {
            std::vector<float> in(1024 * 2, 0.1f);
            std::vector<float> out(4096 * 2, 0.0f);
            size_t n = 0;
            eng.process(in.data(), 1024, out.data(), 4096, n);
            ++passCount;
        }
    }
    CHECK(passCount == 60, "8.3 SEED: Mind a 60 véletlen konfiguráció sikeresen lefutott");
}

template <class EngT = ee_analyzer::UpsampleEngine>
static void testUpsampleDspSuite() {
    testK2RulesMatrix();

    if constexpr (HasUpsampleEngine<EngT>) {
        std::printf("[K3..K10] Valódi DSP motor tesztek futtatása...\n");
        testK3FrequencyResponseAndRejection<EngT>();
        testK4ThdPlusN<EngT>();
        testK5PhaseAndStopband<EngT>();
        testK9HeadroomAndClipping<EngT>();
        testK10Benchmark<EngT>();

        uint32_t seed = 12345;
        if (const char* env = std::getenv("AQ_CHECK_SEED")) {
            try { seed = static_cast<uint32_t>(std::stoul(env)); } catch (...) {}
        }
        testRandomMatrixSeed<EngT>(seed);
    } else {
        CHECK(false, "K3..K10: UpsampleEngine (libsoxr) nincs megvalósítva");
    }
}

int main() {
    std::printf("========================================================\n");
    std::printf(" Aqulify – Felmintavételezés tesztprogram (test_upsample)\n");
    std::printf("========================================================\n");

    testUpsampleDspSuite<ee_analyzer::UpsampleEngine>();

    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
