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
#include "live_upsample.hpp"

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
// Lefelé alakítás elvárása: csak ha a kimenet a forrást nem tudja; „Kimenet maximuma”: a legnagyobb kisebb ráta,
// a többi szabály: az egész arányú (azonos családú) ráta előbb, különben a legnagyobb kisebb (0: nincs)
static uint32_t computeExpectedDownRate(int rule, uint32_t srcRate, const std::vector<uint32_t>& supported) {
    uint32_t best = 0, family = 0;
    for (uint32_t r : supported) {
        if (r >= srcRate) continue;
        best = std::max(best, r);
        if (srcRate % r == 0) family = std::max(family, r);
    }
    if (rule == 3) return best;
    return family > 0 ? family : best;
}

static uint32_t computeExpectedUpRate(int rule, uint32_t srcRate, const std::vector<uint32_t>& supported, const std::map<uint32_t, uint32_t>& customTable);

static uint32_t computeExpectedTargetRate(int rule, uint32_t srcRate, const std::vector<uint32_t>& supported, const std::map<uint32_t, uint32_t>& customTable) {
    const uint32_t up = rule == 0 ? 0 : computeExpectedUpRate(rule, srcRate, supported, customTable);
    if (up > 0) return up;
    if (std::find(supported.begin(), supported.end(), srcRate) != supported.end()) return 0;
    if (rule == 4) {
        auto it = customTable.find(srcRate);
        if (it != customTable.end() && it->second < srcRate &&
            std::find(supported.begin(), supported.end(), it->second) != supported.end()) return it->second;
    }
    return computeExpectedDownRate(rule, srcRate, supported);
}

static uint32_t computeExpectedUpRate(int rule, uint32_t srcRate, const std::vector<uint32_t>& supported, const std::map<uint32_t, uint32_t>& customTable) {
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
                const uint32_t expectedOut = (target == 0) ? src : target;
                // Független elvárás összevetése a termékkód UpsampleEngine::selectTargetRate függvényével
                const uint32_t actualOut = ee_analyzer::UpsampleEngine::selectTargetRate(
                    src, static_cast<ee_analyzer::audioopt::UpsampleRule>(rule), caps, customTable, false, false, nullptr, 0, /*allowDown=*/true);
                // Lefelé alakítás nélkül (a rendszer keverője alakít): soha nem kisebb a cél
                const uint32_t noDown = ee_analyzer::UpsampleEngine::selectTargetRate(
                    src, static_cast<ee_analyzer::audioopt::UpsampleRule>(rule), caps, customTable, false, false);
                if (noDown < src) allValid = false;
                // Lefelé csak akkor alakíthat, ha a kimenet a forrást nem tudja; csak támogatott (vagy eredeti) rátára válthat!
                const bool srcSupported = std::find(caps.begin(), caps.end(), src) != caps.end();
                if ((actualOut < src && srcSupported) || actualOut != expectedOut) {
                    allValid = false;
                }
                if (actualOut != src && std::find(caps.begin(), caps.end(), actualOut) == caps.end()) {
                    allValid = false;
                }
                ++testsCount;
            }
        }
    }

    CHECK(allValid && testsCount >= 240, "K2 szabolymátrix: %zu eset ellenőrizve, lefelé csak nem támogatott forrásnál, csak támogatott cél", testsCount);
}

// K3: Átvitel és elnyomás mérése FFT-vel mind a 4 kötelező mintavételi párra
template <class EngT>
static void testK3FrequencyResponseAndRejection() {
    std::printf("\n[K3] Átvitel és elnyomás (passband flatness és stopband rejection, 4 pár)\n");
    struct RatePair {
        uint32_t src;
        uint32_t dst;
        std::vector<double> passFreqs;
        double aliasToneHz;
        double stopLowHz;
        double stopHighHz;
    };
    const std::vector<RatePair> pairs = {
        { 44100,  88200, { 20.0, 50.0, 100.0, 1000.0, 5000.0, 10000.0, 18000.0, 20000.0 }, 18000.0, 24000.0, 43000.0 },
        { 48000,  96000, { 20.0, 50.0, 100.0, 1000.0, 5000.0, 10000.0, 18000.0, 22000.0 }, 19000.0, 26000.0, 47000.0 },
        { 44100, 176400, { 20.0, 50.0, 100.0, 1000.0, 5000.0, 10000.0, 18000.0, 20000.0 }, 18000.0, 24000.0, 87000.0 },
        { 96000, 192000, { 20.0, 50.0, 100.0, 1000.0, 5000.0, 10000.0, 20000.0, 30000.0, 44000.0 }, 38000.0, 52000.0, 95000.0 },
    };
    struct PrecisionCheck { int bits; double minRejectionDb; };
    const std::vector<PrecisionCheck> precisions = {
        { 20, 110.0 },
        { 24, 130.0 },
        { 28, 150.0 },
    };

    for (const auto& rp : pairs) {
        const size_t ratio = rp.dst / rp.src;
        EngT eng;
        bool okInit = eng.init(rp.src, rp.dst, 1, 28, 50.0, 0.95, 1.00, 0.0f);
        CHECK(okInit, "K3: UpsampleEngine init %u -> %u", rp.src, rp.dst);

        // 1. Átviteli egyenesség (flatness) 20 Hz .. sávszélesség határáig (<= 0.01 dB)
        double maxDeviationDb = 0.0;
        for (double freq : rp.passFreqs) {
            // Egész számú periódus a mérési ablakban, hogy 20 Hz-en se legyen ablakolási hiba
            const size_t cycles = std::max<size_t>(4, static_cast<size_t>(std::llround(freq * 0.2)));
            const size_t winIn = static_cast<size_t>(std::llround(static_cast<double>(cycles) * rp.src / freq));
            const size_t padIn = 4096;
            const size_t inLen = winIn + 2 * padIn;
            std::vector<float> inSig(inLen);
            for (size_t i = 0; i < inLen; ++i) {
                inSig[i] = static_cast<float>(std::sin(2.0 * kPi * freq * i / rp.src) * 0.5);
            }
            std::vector<float> outSig(inLen * ratio + 16384);
            size_t outFrames = 0;
            eng.clear();
            eng.process(inSig.data(), inLen, outSig.data(), outSig.size(), outFrames);
            size_t flushFrames = 0;
            eng.flush(outSig.data() + outFrames, outSig.size() - outFrames, flushFrames);
            outFrames += flushFrames;

            double inRms = 0.0;
            for (size_t i = padIn; i < padIn + winIn; ++i) inRms += static_cast<double>(inSig[i]) * inSig[i];
            inRms = std::sqrt(inRms / static_cast<double>(winIn));

            const size_t padOut = padIn * ratio;
            const size_t winOut = winIn * ratio;
            double outRms = 0.0;
            for (size_t i = padOut; i < padOut + winOut && i < outFrames; ++i) outRms += static_cast<double>(outSig[i]) * outSig[i];
            outRms = std::sqrt(outRms / static_cast<double>(winOut));

            double gainDb = 20.0 * std::log10(outRms / std::max(inRms, 1e-12));
            if (std::abs(gainDb) > maxDeviationDb) maxDeviationDb = std::abs(gainDb);
        }
        CHECK(maxDeviationDb <= 0.01, "K3 passband flatness %u -> %u (20 Hz..%.0f Hz): max elteres = %.4f dB (<= 0.01 dB)",
              rp.src, rp.dst, rp.passFreqs.back(), maxDeviationDb);
        if (rp.src == 44100 && rp.dst == 88200) {
            std::printf("K_METRIC: {\"test\":\"K3_flatness\",\"maxDevDb\":%.4f,\"limitDb\":0.01}\n", maxDeviationDb);
        }
        std::printf("K_METRIC: {\"test\":\"K3_flatness_%u_%u\",\"maxDevDb\":%.4f,\"limitDb\":0.01}\n", rp.src, rp.dst, maxDeviationDb);

        // 2. Tükörkép-elnyomás mérése a forrás Nyquist-frekvenciája fölött minden pontossággal
        for (const auto& pc : precisions) {
            EngT ePrec;
            ePrec.init(rp.src, rp.dst, 1, pc.bits, 50.0, 0.95, 1.00, 0.0f);
            const size_t inLen = 32768;
            std::vector<float> inSig(inLen);
            for (size_t i = 0; i < inLen; ++i) {
                inSig[i] = static_cast<float>(std::sin(2.0 * kPi * rp.aliasToneHz * i / rp.src) * 0.7071);
            }
            std::vector<float> outSig(inLen * ratio + 16384);
            size_t outFrames = 0;
            ePrec.process(inSig.data(), inLen, outSig.data(), outSig.size(), outFrames);
            size_t fl = 0;
            ePrec.flush(outSig.data() + outFrames, outSig.size() - outFrames, fl);
            outFrames += fl;

            const size_t fftN = 16384;
            std::vector<std::complex<double>> fftBuf(fftN);
            const size_t startIdx = 8192 * ratio;
            for (size_t i = 0; i < fftN; ++i) {
                double w = 0.5 * (1.0 - std::cos(2.0 * kPi * i / (fftN - 1)));
                fftBuf[i] = outSig[startIdx + i] * w;
            }
            simpleFft(fftBuf, false);

            double peakFund = 0.0;
            double peakAlias = 0.0;
            const double binHz = static_cast<double>(rp.dst) / fftN;
            for (size_t k = 0; k < fftN / 2; ++k) {
                double f = k * binHz;
                double mag = std::abs(fftBuf[k]);
                if (f >= rp.aliasToneHz - 500.0 && f <= rp.aliasToneHz + 500.0) {
                    if (mag > peakFund) peakFund = mag;
                } else if (f >= rp.stopLowHz && f <= rp.stopHighHz) {
                    if (mag > peakAlias) peakAlias = mag;
                }
            }
            double rejDb = 20.0 * std::log10(peakFund / std::max(peakAlias, 1e-12));
            CHECK(rejDb >= pc.minRejectionDb, "K3 stopband rejection %u -> %u (%d bit): mert = %.1f dB (elvart >= %.0f dB)",
                  rp.src, rp.dst, pc.bits, rejDb, pc.minRejectionDb);
            if (rp.src == 44100 && rp.dst == 88200) {
                std::printf("K_METRIC: {\"test\":\"K3_stopband_%db\",\"rejectionDb\":%.2f,\"minDb\":%.1f}\n", pc.bits, rejDb, pc.minRejectionDb);
            }
            std::printf("K_METRIC: {\"test\":\"K3_stopband_%u_%u_%db\",\"rejectionDb\":%.2f,\"minDb\":%.1f}\n",
                        rp.src, rp.dst, pc.bits, rejDb, pc.minRejectionDb);
        }
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

// K13: élő út (más programból, pl. MPD-ből az Aqulify eszközre szóló hang) – a pactl / /proc/asound szövegének
// értelmezése és a gráf célmintavételének döntése (live_upsample.hpp)
static void testK13LiveUpsample() {
    using namespace ee_analyzer;
    std::printf("\n[K13] Élő út: forrásmintavétel, DAC-mintavételek, döntés\n");
    const std::string shortSinks =
        "57\talsa_output.usb-BurrBrown_from_Texas_Instruments_USB_AUDIO_DAC-00.analog-stereo\tPipeWire\ts16le 2ch 44100Hz\tRUNNING\n"
        "61\taqulify_sink\tPipeWire\tfloat32le 2ch 48000Hz\tRUNNING\n";
    const std::string inputs =
        "Sink Input #70\n"
        "\tDriver: PipeWire\n"
        "\tSink: 57\n"
        "\tSample Specification: float32le 2ch 48000Hz\n"
        "\tCorked: no\n"
        "\tProperties:\n"
        "\t\tnode.name = \"aqulify_sink_output\"\n"
        "\t\tmedia.role = \"DSP\"\n"
        "\n"
        "Sink Input #75\n"
        "\tDriver: PipeWire\n"
        "\tSink: 61\n"
        "\tSample Specification: s16le 2ch 44100Hz\n"
        "\tCorked: no\n"
        "\tProperties:\n"
        "\t\tapplication.name = \"Music Player Daemon\"\n"
        "\t\tnode.name = \"Music Player Daemon\"\n"
        "\n"
        "Sink Input #80\n"
        "\tSink: 61\n"
        "\tSample Specification: float32le 2ch 192000Hz\n"
        "\tCorked: no\n"
        "\tProperties:\n"
        "\t\tapplication.name = \"Aqulify\"\n"
        "\t\tnode.name = \"aqulify_sink_keepalive\"\n"
        "\n"
        "Sink Input #81\n"
        "\tSink: 61\n"
        "\tSample Specification: s32le 2ch 96000Hz\n"
        "\tCorked: yes\n"
        "\tProperties:\n"
        "\t\tnode.name = \"Firefox\"\n";
    const std::string own = liveups::sinkIdByName(shortSinks, "aqulify_sink");
    CHECK(own == "61", "K13: a saját nyelő azonosítója a pactl short listából (%s)", own.c_str());
    const auto in = liveups::parseSinkInputs(inputs);
    CHECK(in.size() == 4 && in[1].rate == 44100 && in[1].sinkId == "61" && !in[1].corked && in[3].corked,
          "K13: sink-inputs értelmezése (%zu db, MPD %u Hz)", in.size(), in.size() > 1 ? in[1].rate : 0u);
    const uint32_t src = liveups::liveSourceRate(in, own);
    CHECK(src == 44100, "K13: forrás = MPD 44100 (saját ébren tartó és szüneteltetett kihagyva): %u", src);
    CHECK(liveups::liveSourceRate(in, "57") == 0, "K13: a DAC-ra közvetlenül szóló hang nem forrása az élő útnak");
    CHECK(liveups::liveSourceRate(in, "") == 0, "K13: ismeretlen saját nyelőnél nincs forrás");

    const std::string sinks =
        "Sink #57\n\tState: RUNNING\n\tName: alsa_output.usb-BurrBrown\n\tProperties:\n"
        "\t\talsa.card = \"1\"\n\t\tdevice.api = \"alsa\"\n"
        "Sink #61\n\tName: aqulify_sink\n\tProperties:\n\t\tnode.virtual = \"true\"\n";
    CHECK(liveups::alsaCardOfSink(sinks, "57") == 1, "K13: a DAC ALSA-kártyaszáma a pactl list sinks-ből");
    CHECK(liveups::alsaCardOfSink(sinks, "61") == -1, "K13: virtuális nyelőnek nincs kártyaszáma");

    // PCM2704-szerű USB DAC: csak 32/44,1/48 kHz (a Capture rész nem számít)
    const std::string stream0Fixed =
        "BurrBrown from Texas Instruments USB AUDIO  DAC at usb-3f980000.usb-1.3, full speed : USB Audio\n\n"
        "Playback:\n  Status: Running\n  Interface 1\n    Altset 1\n    Format: S16_LE\n    Channels: 2\n"
        "    Rates: 32000, 44100, 48000\n"
        "Capture:\n  Interface 2\n    Altset 1\n    Rates: 8000, 16000, 96000\n";
    const auto rFixed = liveups::parseStream0PlaybackRates(stream0Fixed);
    CHECK((rFixed == std::vector<uint32_t>{ 32000, 44100, 48000 }), "K13: stream0 diszkrét lejátszási mintavételek (%zu db)", rFixed.size());
    const std::string stream0Cont =
        "Playback:\n  Interface 1\n    Altset 1\n    Rates: 44100 - 192000 (continuous)\n"
        "    Altset 2\n    Rates: 44100, 48000, 88200, 96000\n";
    const auto rCont = liveups::parseStream0PlaybackRates(stream0Cont);
    CHECK((rCont == std::vector<uint32_t>{ 44100, 48000, 88200, 96000, 176400, 192000 }),
          "K13: stream0 folytonos tartomány a szabványos mintavételekre bontva (%zu db)", rCont.size());

    const std::vector<uint32_t> fallback{ 44100, 48000, 88200, 96000, 176400, 192000 };
    audioopt::AudioOptions o;
    o.rateMode = audioopt::RateMode::Follow;
    o.upsampleRule = audioopt::UpsampleRule::Double;
    auto d = liveups::decide(44100, o, rFixed, fallback, false);
    CHECK(d.forceRate == 0 && d.reason == "dac_unsupported",
          "K13: csak 48 kHz-ig tudó DAC-nál nincs kényszer (88,2 kHz-et a DAC nem tud): %u, %s", d.forceRate, d.reason.c_str());
    d = liveups::decide(44100, o, rCont, fallback, false);
    CHECK(d.forceRate == 88200 && d.reason.empty(), "K13: ×2 → 88200 a 192 kHz-es DAC-nál: %u", d.forceRate);
    d = liveups::decide(44100, o, {}, fallback, false);
    CHECK(d.forceRate == 88200, "K13: ismeretlen DAC-nál a beépített lejátszóéval azonos lista dönt: %u", d.forceRate);
    o.upsampleRule = audioopt::UpsampleRule::PowerOfTwo;
    o.upsampleMaxRate = 96000;
    d = liveups::decide(48000, o, rCont, fallback, false);
    CHECK(d.forceRate == 96000, "K13: legnagyobb ×2ⁿ a felső korláttal (96 kHz): %u", d.forceRate);
    o.upsampleMaxRate = 0;
    d = liveups::decide(48000, o, rCont, fallback, true);
    CHECK(d.forceRate == 0 && d.reason == "player", "K13: amíg a beépített lejátszó szól, az élő út nem kényszerít");
    d = liveups::decide(0, o, rCont, fallback, false);
    CHECK(d.forceRate == 0 && d.reason == "no_source", "K13: forrás nélkül nincs kényszer");
    o.rateMode = audioopt::RateMode::Fixed;
    d = liveups::decide(44100, o, rCont, fallback, false);
    CHECK(d.forceRate == 0 && d.reason == "rate_mode", "K13: fix mintavételnél a felhasználó választása erősebb");
    o.rateMode = audioopt::RateMode::System;
    d = liveups::decide(44100, o, rCont, fallback, false);
    CHECK(d.forceRate == 176400, "K13: rendszer-mintavételnél is működik (×2ⁿ → 176400): %u", d.forceRate);
    o.upsampleRule = audioopt::UpsampleRule::Off;
    d = liveups::decide(44100, o, rCont, fallback, false);
    CHECK(d.forceRate == 0 && d.reason == "off", "K13: kikapcsolva nincs kényszer");
    o.upsampleRule = audioopt::UpsampleRule::Double;
    d = liveups::decide(352800, o, rCont, fallback, false);
    CHECK(d.forceRate == 0 && d.reason == "source_rate", "K13: 192 kHz fölötti forrásnál nincs átalakítás");
}

// ---------------------------------------------------------------------------------------------------------------
// Szűrő a DAC rátáján (két lépés: a felhasználó szűrőjével 2× fel, átlátszó lépéssel vissza)
// ---------------------------------------------------------------------------------------------------------------

/** A teljes jel átalakítása (process 4096-os adagokban, ahogy a lejátszó hívja, majd a kiürítés lépésenként). */
static std::vector<float> runEngineAll(ee_analyzer::UpsampleEngine& e, const std::vector<float>& in, double ratio,
                                       double* delayMidStream = nullptr) {
    std::vector<float> out;
    const size_t frames = in.size() / 2;
    std::vector<float> buf;
    for (size_t pos = 0; pos < frames; pos += 4096) {
        const size_t n = std::min<size_t>(4096, frames - pos);
        const size_t cap = static_cast<size_t>(std::ceil(n * ratio)) + 256;
        buf.resize(cap * 2);
        size_t got = 0;
        e.process(in.data() + pos * 2, n, buf.data(), cap, got);
        out.insert(out.end(), buf.begin(), buf.begin() + got * 2);
        if (delayMidStream && pos >= frames / 2 && *delayMidStream == 0.0) *delayMidStream = e.delayFrames();
    }
    buf.resize(16384 * 2);
    for (int i = 0; i < 1000; ++i) {
        size_t got = 0;
        e.flush(buf.data(), 16384, got);
        if (got == 0) break;
        out.insert(out.end(), buf.begin(), buf.begin() + got * 2);
    }
    return out;
}

/** Egy csatorna amplitúdója az f frekvencián (egész periódusú ablakon vetítve). */
static double toneAmp(const std::vector<float>& x, int ch, size_t start, size_t n, double f, double sr) {
    double s = 0.0, c = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double v = x[2 * (start + i) + ch];
        const double ph = 2.0 * kPi * f * static_cast<double>(start + i) / sr;
        s += v * std::sin(ph);
        c += v * std::cos(ph);
    }
    return 2.0 * std::sqrt(s * s + c * c) / static_cast<double>(n);
}

static void testFilterOnlyEngine() {
    using namespace ee_analyzer;
    std::printf("\n[SZ] Szűrő a DAC rátáján: UpsampleEngine::initFilterOnly (44,1 -> 88,2 -> 44,1 kHz)\n");
    const uint32_t sr = 44100;
    const size_t n = sr * 3;
    const double fHi = 21000.0;   // a felhasználói szűrő átmeneti sávjában (áteresztő sáv vége itt 0,90 = 19,85 kHz)
    std::vector<float> x(n * 2);
    for (size_t i = 0; i < n; ++i) {
        x[2 * i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 1000.0 * i / sr));
        x[2 * i + 1] = static_cast<float>(0.5 * std::sin(2.0 * kPi * fHi * i / sr));
    }
    audioopt::AudioOptions o;
    o.upsampleRule = audioopt::UpsampleRule::Double;
    o.upsamplePassbandEnd = 0.90;
    o.upsampleStopbandBegin = 1.0;
    o.upsamplePhase = 50.0;
    o.upsampleHeadroomDb = -1.0f;

    UpsampleEngine e;
    CHECK(e.initFilterOnly(sr, 2, o) && e.isActive() && e.isFilterOnly() && e.dstRate() == 2 * sr,
          "SZ: initFilterOnly 44,1 kHz: aktív, két lépés, köztes 88,2 kHz");
    double delay = 0.0;
    const std::vector<float> y = runEngineAll(e, x, 1.0, &delay);
    const size_t outFrames = y.size() / 2;
    CHECK(outFrames + 2 >= n && outFrames <= n + 2, "SZ: kimeneti keretszám = bemeneti (%zu ~ %zu)", outFrames, n);
    CHECK(e.processedFrames() == outFrames, "SZ: processedFrames = a kimenetre adott keretek (%llu)",
          (unsigned long long)e.processedFrames());
    CHECK(delay > 0.0 && delay < sr, "SZ: késleltetés folyam közben %.1f keret (> 0, a két lépésé együtt)", delay);
    const double g = std::pow(10.0, -1.0 / 20.0);
    const double a1k = toneAmp(y, 0, sr, sr, 1000.0, sr);
    const double dev1k = 20.0 * std::log10(a1k / (0.5 * g));
    CHECK(std::abs(dev1k) <= 0.1, "SZ: 1 kHz egységnyi átvitel a -1 dB fejtérrel: eltérés %.4f dB (<= 0,1 dB)", dev1k);

    // Referencia: ugyanez a felhasználói szűrő egy lépésben (44,1 -> 88,2 kHz) – a 21 kHz-es csillapítása
    UpsampleEngine ref;
    ref.initWithOptions(sr, 2 * sr, 2, o);
    const std::vector<float> yr = runEngineAll(ref, x, 2.0);
    const double aHiRef = toneAmp(yr, 1, 2 * sr, 2 * sr, fHi, 2.0 * sr);
    const double aHi = toneAmp(y, 1, sr, sr, fHi, sr);
    const double refDb = 20.0 * std::log10(aHiRef / (0.5 * g));
    const double gotDb = 20.0 * std::log10(aHi / (0.5 * g));
    CHECK(refDb < -3.0 && std::abs(gotDb - refDb) <= 0.5,
          "SZ: 21 kHz a felhasználói szűrő szerint csillapítva: %.2f dB (egy lépésben 88,2 kHz-re: %.2f dB)", gotDb, refDb);

    // A második lépés egyedül (a felhasználói szűrő nélkül) a 21 kHz-et átengedné: a csillapítás a felhasználóé
    {
        audioopt::AudioOptions wide = o;
        wide.upsamplePassbandEnd = 0.997;
        UpsampleEngine w;
        w.initFilterOnly(sr, 2, wide);
        const std::vector<float> yw = runEngineAll(w, x, 1.0);
        const double wDb = 20.0 * std::log10(toneAmp(yw, 1, sr, sr, fHi, sr) / (0.5 * g));
        CHECK(std::abs(wDb) <= 0.1, "SZ: széles (99,7 %%) felhasználói szűrővel a 21 kHz átmegy: %.3f dB", wDb);
    }

    // Minimum- és lineáris fázis: az impulzus előtti energia (előcsengés) csak a lineárisnál van
    {
        std::vector<float> imp(sr * 2, 0.0f);
        const size_t at = sr / 2;
        imp[2 * at] = imp[2 * at + 1] = 0.5f;
        audioopt::AudioOptions lin = o, mp = o;
        lin.upsamplePhase = 50.0;
        mp.upsamplePhase = 0.0;
        UpsampleEngine el, em;
        el.initFilterOnly(sr, 2, lin);
        em.initFilterOnly(sr, 2, mp);
        const std::vector<float> yl = runEngineAll(el, imp, 1.0);
        const std::vector<float> ym = runEngineAll(em, imp, 1.0);
        double maxDiff = 0.0;
        for (size_t i = 0; i < std::min(yl.size(), ym.size()) / 2; ++i)
            maxDiff = std::max(maxDiff, static_cast<double>(std::abs(yl[2 * i] - ym[2 * i])));
        // A válasz csúcsa előtti és utáni energia aránya: lineáris fázisnál szimmetrikus (~1), minimumfázisnál a
        // csúcs előtt alig van (nincs előcsengés)
        auto preToPost = [](const std::vector<float>& y) {
            size_t pk = 0;
            for (size_t i = 0; i < y.size() / 2; ++i) if (std::abs(y[2 * i]) > std::abs(y[2 * pk])) pk = i;
            double pre = 0.0, post = 0.0;
            for (size_t k = 1; k <= 2000 && k <= pk && pk + k < y.size() / 2; ++k) {
                pre += static_cast<double>(y[2 * (pk - k)]) * y[2 * (pk - k)];
                post += static_cast<double>(y[2 * (pk + k)]) * y[2 * (pk + k)];
            }
            return pre / std::max(post, 1e-30);
        };
        const double rL = preToPost(yl), rM = preToPost(ym);
        CHECK(maxDiff > 1e-3, "SZ: minimum- és lineáris fázisú felhasználói szűrő eltérő kimenetet ad (max. eltérés %.4f)", maxDiff);
        CHECK(rL > 0.5 && rL < 2.0 && rM < 0.2, "SZ: csúcs előtti / utáni energia: lineáris %.3f (~1), minimumfázisú %.3f (< 0,2)", rL, rM);
    }

    // Aqulify FFT64 motorral is (a felhasználó motorja az első lépésben)
    {
        audioopt::AudioOptions f = o;
        f.upsampleEngine = 1;
        f.upsamplePassbandEnd = 0.95;
        UpsampleEngine ef;
        CHECK(ef.initFilterOnly(sr, 2, f) && ef.isFilterOnly() && ef.engineType() == 1, "SZ: FFT64 motorral is két lépés");
        const std::vector<float> yf = runEngineAll(ef, x, 1.0);
        const double dF = 20.0 * std::log10(toneAmp(yf, 0, sr, sr, 1000.0, sr) / (0.5 * g));
        CHECK(yf.size() / 2 + 2 >= n && yf.size() / 2 <= n + 2 && std::abs(dF) <= 0.1,
              "SZ: FFT64: %zu keret, 1 kHz eltérés %.4f dB", yf.size() / 2, dF);
    }

    // Seek után (clear) újra tiszta állapot; a rendes (egylépéses) motor nem lesz kétlépéses
    {
        UpsampleEngine plain;
        plain.initWithOptions(sr, 2 * sr, 2, o);
        CHECK(!plain.isFilterOnly(), "SZ: a rendes felmintavételezés egy lépésben marad");
        e.clear();
        CHECK(e.delayFrames() >= 0.0, "SZ: clear() után a késleltetés érvényes (%.1f)", e.delayFrames());
    }
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
    testK13LiveUpsample();
    testFilterOnlyEngine();

    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
