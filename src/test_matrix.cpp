/**
 * @file test_matrix.cpp
 * @brief Teszt-mátrix: a motor fő invariánsai több mintavételi frekvencián (44,1 / 48 / 96 / 192 kHz) és
 *        hoszt-blokkméreten (64 / 256 / 480 / 512 / 1024 minta).
 *
 * Ellenőrzött állítások:
 *   1. Átlátszó lánc: bit-pontosan érintetlen a kimenet, minden kombinációban.
 *   2. Blokkméret-függetlenség: a bekapcsolt modulok (EQ, Bass, Crossfeed, Limiter, Mono) kimenete bit-azonos,
 *      függetlenül attól, hány mintás blokkokban kapja a bemenetet (kettőhatvány blokkok; a konvolver külön tesztelt).
 *   3. Harang-erősítés: +6 dB 1 kHz-en (±0,3 dB).
 *   4. Mélyvágás: 20 Hz-en −6 dB (LR4, ±0,3 dB).
 *   5. Mono-keverés 100 %: az oldalsó 50 Hz-es szint −24,6 dB (±0,5 dB), a középső érintetlen.
 */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "aether_chain.hpp"

using namespace ee_analyzer;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; std::printf("  [HIBA] " __VA_ARGS__); } else { std::printf("  [OK]   " __VA_ARGS__); } std::printf("\n"); } while (0)

static constexpr double kPi = 3.14159265358979323846;

/** Goertzel-jellegű amplitúdó (Hann-ablakkal) a jel egy frekvenciáján. */
static double toneAmp(const std::vector<float>& x, size_t from, double f, double sr) {
    const size_t n = x.size() - from;
    std::complex<double> acc(0, 0);
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * kPi * i / (n - 1));
        acc += w * x[from + i] * std::polar(1.0, -2 * kPi * f * i / sr);
    }
    return 2.0 * std::abs(acc) / (n * 0.5);
}

static std::vector<float> sine(double f, double amp, size_t n, double sr) {
    std::vector<float> s(n);
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<float>(amp * std::sin(2.0 * kPi * f * i / sr));
    return s;
}

/**
 * Feldolgozás adott blokkmérettel. A bemenet elé 24576 minta (a 64..1024-es blokkok közös többszöröse) bemelegítés
 * kerül, hogy az átúsztatás és a modulok állapota azonos legyen, mielőtt a mért jel indul.
 */
static void runChain(const ParamData& d, double sr, size_t block, std::vector<float>& L, std::vector<float>& R) {
    AetherChain chain;
    chain.setSampleRate(sr);
    chain.applyParamsRT(d);
    const size_t warm = 24576;
    std::vector<float> wl(warm, 0.0f), wr(warm, 0.0f);
    for (size_t off = 0; off < warm; off += block) chain.process(wl.data() + off, wr.data() + off, std::min(block, warm - off));
    for (size_t off = 0; off < L.size(); off += block) chain.process(L.data() + off, R.data() + off, std::min(block, L.size() - off));
}

static ParamData transparent() { return transparentParams(); }

static ParamData eqBell(double f, double gainDb, double q) {
    ParamData d = transparent();
    d.set(kFlagEq, true);
    d.numBands = 1;
    d.bands[0].type = static_cast<int32_t>(EqBandType::Bell);
    d.bands[0].mode = static_cast<int32_t>(EqFilterMode::RlcBt);
    d.bands[0].freq = static_cast<float>(f);
    d.bands[0].gainDb = static_cast<float>(gainDb);
    d.bands[0].q = static_cast<float>(q);
    return d;
}

static ParamData subsonic(int32_t hz) {
    ParamData d = transparent();
    d.set(kFlagMono, true);
    d.subsonicHz = hz;
    return d;
}

static ParamData mono100() {
    ParamData d = transparent();
    d.set(kFlagMono, true);
    d.monoAmount = 100.0f;
    return d;
}

/** Tiszta szinusz szintje a feldolgozás után (dB, az 1,0 amplitúdóhoz képest a bemenet szerint). */
static double gainDb(const ParamData& d, double f, double sr, size_t block) {
    const size_t n = static_cast<size_t>(sr * 1.5);
    std::vector<float> L = sine(f, 0.5, n, sr), R = L;
    runChain(d, sr, block, L, R);
    const size_t from = n - static_cast<size_t>(sr * 0.5);
    return 20.0 * std::log10(toneAmp(L, from, f, sr) / 0.5);
}

/** Mono-keverés: az oldalsó (S) és a középső (M) szint a bemenethez képest (bal csatornán szinusz). */
static void monoLevels(const ParamData& d, double f, double sr, size_t block, double& mDb, double& sDb) {
    const size_t n = static_cast<size_t>(sr * 1.5);
    std::vector<float> L = sine(f, 0.5, n, sr), R(n, 0.0f);
    runChain(d, sr, block, L, R);
    std::vector<float> M(n), S(n);
    for (size_t i = 0; i < n; ++i) { M[i] = 0.5f * (L[i] + R[i]); S[i] = 0.5f * (L[i] - R[i]); }
    const size_t from = n - static_cast<size_t>(sr * 0.5);
    mDb = 20.0 * std::log10(toneAmp(M, from, f, sr) / 0.25);
    sDb = 20.0 * std::log10(toneAmp(S, from, f, sr) / 0.25);
}

/** Véletlen bemenet. */
static void noise(size_t n, std::vector<float>& L, std::vector<float>& R) {
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> u(-0.4f, 0.4f);
    L.assign(n, 0.0f); R.assign(n, 0.0f);
    for (auto& v : L) v = u(rng);
    for (auto& v : R) v = u(rng);
}

int main() {
    std::printf("========================================================\n");
    std::printf(" Aqulify teszt-mátrix: mintavételi frekvencia × blokkméret\n");
    std::printf("========================================================\n");
    const double rates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };
    const size_t blocks[] = { 64, 256, 480, 512, 1024 };

    for (double sr : rates) {
        std::printf("\n-- %.1f kHz --\n", sr / 1000.0);

        // 1. Átlátszó lánc: bit-pontos
        {
            std::vector<float> L, R;
            noise(static_cast<size_t>(sr * 0.25), L, R);
            const std::vector<float> L0 = L, R0 = R;
            bool exact = true;
            for (size_t b : blocks) {
                std::vector<float> l = L0, r = R0;
                runChain(transparent(), sr, b, l, r);
                exact = exact && l == L0 && r == R0;
            }
            CHECK(exact, "átlátszó lánc: bit-pontosan érintetlen, minden blokkméreten");
        }

        // 2. Blokkméret-függetlenség (bekapcsolt modulok)
        {
            ParamData d = eqBell(1000.0, 6.0, 1.0);
            d.set(kFlagBass, true); d.bassAmountDb = 6.0f;
            d.set(kFlagCrossfeed, true);
            d.set(kFlagLimiter, true); d.limOversampling = 0;
            d.set(kFlagMono, true); d.monoAmount = 100.0f; d.subsonicHz = 20;
            std::vector<float> L, R;
            noise(static_cast<size_t>(sr * 0.25) / 1024 * 1024, L, R);
            std::vector<float> l0 = L, r0 = R;
            runChain(d, sr, 1024, l0, r0);
            double worst = 0.0;
            for (size_t b : { size_t(64), size_t(256), size_t(512) }) {
                std::vector<float> l = L, r = R;
                runChain(d, sr, b, l, r);
                for (size_t i = 0; i < l.size(); ++i) worst = std::max(worst, std::max(std::fabs(double(l[i]) - l0[i]), std::fabs(double(r[i]) - r0[i])));
            }
            CHECK(worst == 0.0, "bekapcsolt modulok (EQ, Bass, Crossfeed, Limiter, Mono): a kimenet blokkmérettől független (legnagyobb eltérés %.3g)", worst);
        }

        for (size_t b : blocks) {
            const double g6 = gainDb(eqBell(1000.0, 6.0, 1.0), 1000.0, sr, b);
            CHECK(std::fabs(g6 - 6.0) < 0.3, "%5.0f Hz, blokk %4zu: +6 dB harang 1 kHz-en %.2f dB", sr, b, g6);

            const double s20 = gainDb(subsonic(20), 20.0, sr, b);
            CHECK(std::fabs(s20 + 6.0) < 0.3, "%5.0f Hz, blokk %4zu: 20 Hz-es vágás 20 Hz-en %.2f dB (LR4: −6 dB)", sr, b, s20);

            double mDb = 0, sDb = 0;
            monoLevels(mono100(), 50.0, sr, b, mDb, sDb);
            CHECK(std::fabs(sDb + 24.6) < 0.5 && std::fabs(mDb) < 0.05,
                  "%5.0f Hz, blokk %4zu: mono 100%%: oldalsó 50 Hz-en %.2f dB, középső %.3f dB", sr, b, sDb, mDb);
        }
    }

    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
