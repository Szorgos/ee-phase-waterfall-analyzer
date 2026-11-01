/**
 * @file test_multiout.cpp
 * @brief Több kimenet (multi_output.hpp): független zónák és Linkwitz–Riley aktív keresztváltó.
 *
 * Ellenőrzött állítások (44,1 / 48 / 96 kHz):
 *   1. Alapállapot (zónák, alap láncok): minden kimenet bit-pontosan a bemenet.
 *   2. Független zónák: az 1. kimeneten +6 dB-es harang 1 kHz-en, a 2. kimenet ugyanekkor bit-pontosan érintetlen.
 *   3. Keresztváltó 12 / 24 / 48 dB/okt, 2–4 sáv: a sávok a vágási pontokon −6 dB-esek, a meredekség egy oktávval
 *      odébb a névleges, és az összes sáv összege 20 Hz – 20 kHz között amplitúdóban egyenes (±0,05 dB).
 *   4. Késleltetés: mintapontos (impulzus), erősítés és polaritás pontos; kikapcsolt kimenet néma.
 *   5. A kimenet a hoszt blokkméretétől független (64 / 256 / 480 / 1024 / 4096 minta).
 */
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

#include "multi_output.hpp"

using namespace ee_analyzer;
using namespace ee_analyzer::multiout;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; std::printf("  [HIBA] " __VA_ARGS__); } else { std::printf("  [OK]   " __VA_ARGS__); } std::printf("\n"); } while (0)

struct Outs {
    std::vector<std::vector<float>> L, R;
};

/** Bemelegítés (24576 = a 64…4096-os blokkok közös többszöröse): a lánc induláskori átúsztatása blokkfüggő. */
static void warmup(MultiOutputEngine& e, size_t block) {
    const size_t n = 24576;
    std::vector<float> z(n, 0.0f);
    const int nOut = e.config().numOutputs;
    std::vector<std::vector<float>> l(nOut, std::vector<float>(n)), r(nOut, std::vector<float>(n));
    std::vector<float*> pl(nOut), pr(nOut);
    for (size_t off = 0; off < n; off += block) {
        const size_t m = std::min(block, n - off);
        for (int k = 0; k < nOut; ++k) { pl[k] = l[k].data() + off; pr[k] = r[k].data() + off; }
        e.process(z.data() + off, z.data() + off, pl.data(), pr.data(), m);
    }
}

/** Feldolgozás adott blokkmérettel; a kimenetek pufferei. */
static Outs run(MultiOutputEngine& e, const std::vector<float>& inL, const std::vector<float>& inR, size_t block) {
    const int nOut = e.config().numOutputs;
    Outs o;
    o.L.assign(nOut, std::vector<float>(inL.size()));
    o.R.assign(nOut, std::vector<float>(inR.size()));
    std::vector<float*> pl(nOut), pr(nOut);
    for (size_t off = 0; off < inL.size(); off += block) {
        const size_t m = std::min(block, inL.size() - off);
        for (int k = 0; k < nOut; ++k) { pl[k] = o.L[k].data() + off; pr[k] = o.R[k].data() + off; }
        e.process(inL.data() + off, inR.data() + off, pl.data(), pr.data(), m);
    }
    return o;
}

static std::vector<float> noise(size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-0.4f, 0.4f);
    std::vector<float> v(n);
    for (auto& x : v) x = u(rng);
    return v;
}

/** Komplex frekvenciaválasz egy impulzusválaszból (DFT egy frekvencián). */
static std::complex<double> resp(const std::vector<float>& h, double f, double sr) {
    std::complex<double> acc(0, 0);
    for (size_t i = 0; i < h.size(); ++i) acc += static_cast<double>(h[i]) * std::polar(1.0, -2.0 * kPi * f * i / sr);
    return acc;
}
static double db(std::complex<double> c) { return 20.0 * std::log10(std::max(1e-30, std::abs(c))); }

static std::vector<double> logFreqs(double lo, double hi, int n) {
    std::vector<double> f(n);
    for (int i = 0; i < n; ++i) f[i] = lo * std::pow(hi / lo, i / double(n - 1));
    return f;
}

static void testTransparent(double sr) {
    MultiOutputEngine e;
    e.setSampleRate(sr);
    MultiConfig c;
    c.numOutputs = 4;
    e.applyConfigRT(c);
    const auto L = noise(static_cast<size_t>(sr * 0.25), 1), R = noise(L.size(), 2);
    const Outs o = run(e, L, R, 480);
    bool exact = true;
    for (int k = 0; k < 4; ++k) exact = exact && o.L[k] == L && o.R[k] == R;
    CHECK(exact, "%5.0f Hz: alapállapot, 4 zóna: minden kimenet bit-pontosan a bemenet", sr);
}

static void testZones(double sr) {
    MultiOutputEngine e;
    e.setSampleRate(sr);
    MultiConfig c;
    c.numOutputs = 2;
    ParamData& d = c.outputs[0].chain;
    d.set(kFlagEq, true);
    d.numBands = 1;
    d.bands[0].type = static_cast<int32_t>(EqBandType::Bell);
    d.bands[0].mode = static_cast<int32_t>(EqFilterMode::RlcBt);
    d.bands[0].freq = 1000.0f;
    d.bands[0].gainDb = 6.0f;
    d.bands[0].q = 1.0f;
    e.applyConfigRT(c);
    const size_t n = static_cast<size_t>(sr * 1.0);
    std::vector<float> s(n);
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<float>(0.25 * std::sin(2.0 * kPi * 1000.0 * i / sr));
    const Outs o = run(e, s, s, 256);
    // a második fél másodperc szintje (az átúsztatás után)
    auto level = [&](const std::vector<float>& x) {
        double a = 0, b = 0;
        for (size_t i = n / 2; i < n; ++i) { a += double(x[i]) * x[i]; b += double(s[i]) * s[i]; }
        return 10.0 * std::log10(a / b);
    };
    const double g1 = level(o.L[0]);
    CHECK(std::fabs(g1 - 6.0) < 0.3, "%5.0f Hz: 1. zóna (EQ +6 dB harang 1 kHz-en): %.2f dB", sr, g1);
    CHECK(o.L[1] == s && o.R[1] == s, "%5.0f Hz: 2. zóna (saját, alap lánc): bit-pontosan érintetlen", sr);
}

/** Keresztváltó impulzusválaszai sávonként. */
static std::vector<std::vector<float>> xoverImpulses(double sr, int slope, const std::vector<float>& fcs, size_t len) {
    const int bands = static_cast<int>(fcs.size()) + 1;
    MultiOutputEngine e;
    e.setSampleRate(sr);
    MultiConfig c;
    c.mode = Mode::Crossover;
    c.numOutputs = bands;
    c.numCrossovers = static_cast<int32_t>(fcs.size());
    for (size_t j = 0; j < fcs.size(); ++j) c.crossoverHz[j] = fcs[j];
    c.slopeDbPerOct = slope;
    for (int k = 0; k < bands; ++k) c.outputs[k].band = k;
    e.applyConfigRT(c);
    std::vector<float> imp(len, 0.0f);
    imp[0] = 1.0f;
    const Outs o = run(e, imp, imp, 1024);
    return o.L;
}

static void testCrossover(double sr, int slope, const std::vector<float>& fcs) {
    const size_t len = static_cast<size_t>(sr * 1.0);   // 1 s impulzusválasz (20 Hz-ig elég)
    const auto h = xoverImpulses(sr, slope, fcs, len);
    const int bands = static_cast<int>(h.size());
    std::vector<float> sum(len, 0.0f);
    for (const auto& b : h) for (size_t i = 0; i < len; ++i) sum[i] += b[i];
    double worst = 0.0;
    for (double f : logFreqs(20.0, std::min(20000.0, sr * 0.45), 120)) worst = std::max(worst, std::fabs(db(resp(sum, f, sr))));
    std::string fl;
    for (float f : fcs) fl += (fl.empty() ? "" : "/") + std::to_string(static_cast<int>(f));
    CHECK(worst < 0.05, "%5.0f Hz, LR %2d dB/okt, %d sáv (%s Hz): a sávok összege egyenes, legnagyobb eltérés %.4f dB",
          sr, slope, bands, fl.c_str(), worst);
    // a vágási pontokon a két szomszédos sáv −6 dB; egy oktávval a vágási pont alatt a felső sáv a névleges meredekségű
    for (size_t j = 0; j < fcs.size(); ++j) {
        const double lo = db(resp(h[j], fcs[j], sr)), hi = db(resp(h[j + 1], fcs[j], sr));
        // LR2-nél (12 dB/okt) a szomszédos vágási pontok enyhe meredeksége pár tized dB-lel belenyúl
        const double tol = slope == 12 && fcs.size() > 1 ? 0.4 : 0.15;
        CHECK(std::fabs(lo + 6.02) < tol && std::fabs(hi + 6.02) < tol,
              "%5.0f Hz, LR %2d: %g Hz-en az alsó sáv %.2f dB, a felső %.2f dB (−6 dB)", sr, slope, fcs[j], lo, hi);
    }
    if (fcs.size() == 1) {
        // egy oktávval lejjebb a felső sáv csillapítása: LR2 ≈ −14, LR4 ≈ −25, LR8 ≈ −48 dB (a névleges meredekség
        // aszimptotikus; egy oktávnyira a pontos érték a szűrő képletéből: 20·log10(|H(j/2)|))
        const double w = 0.5;   // f / fc
        double expect = 0.0;
        if (slope == 12) expect = 20.0 * std::log10((w * w) / (1.0 + w * w));
        else if (slope == 24) expect = 20.0 * std::log10((w * w) * (w * w) / (1.0 + std::pow(w, 4)));
        else expect = 20.0 * std::log10(std::pow(w, 8) / (1.0 + std::pow(w, 8)));
        const double got = db(resp(h[1], fcs[0] * 0.5, sr));
        CHECK(std::fabs(got - expect) < 0.5, "%5.0f Hz, LR %2d: egy oktávval lejjebb a felső sáv %.1f dB (elméleti %.1f dB)",
              sr, slope, got, expect);
    }
}

static void testDelayGainPolarity(double sr) {
    MultiOutputEngine e;
    e.setSampleRate(sr);
    MultiConfig c;
    c.numOutputs = 3;
    c.outputs[0].delayMs = 3.0f;
    c.outputs[1].gainDb = -6.0f;
    c.outputs[1].invert = true;
    c.outputs[2].enabled = false;
    e.applyConfigRT(c);
    std::vector<float> imp(static_cast<size_t>(sr * 0.1), 0.0f);
    imp[0] = 1.0f;
    const Outs o = run(e, imp, imp, 256);
    const size_t want = static_cast<size_t>(std::lround(0.003 * sr));
    size_t at = 0;
    for (size_t i = 0; i < o.L[0].size(); ++i) if (o.L[0][i] != 0.0f) { at = i; break; }
    CHECK(at == want && o.L[0][at] == 1.0f && o.R[0][at] == 1.0f, "%5.0f Hz: 3 ms késleltetés = %zu minta (elvárt %zu)", sr, at, want);
    const float g = o.L[1][0];
    CHECK(std::fabs(g + 0.501187f) < 1e-5f, "%5.0f Hz: −6 dB és polaritásfordítás: %.6f (elvárt −0,501187)", sr, g);
    bool silent = std::all_of(o.L[2].begin(), o.L[2].end(), [](float x) { return x == 0.0f; });
    CHECK(silent, "%5.0f Hz: kikapcsolt kimenet néma", sr);
}

static void testBlockIndependence(double sr) {
    MultiConfig c;
    c.mode = Mode::Crossover;
    c.numOutputs = 3;
    c.numCrossovers = 2;
    c.crossoverHz = { { 120.0f, 3000.0f, 8000.0f } };
    c.slopeDbPerOct = 48;
    for (int k = 0; k < 3; ++k) c.outputs[k].band = k;
    c.outputs[2].delayMs = 1.7f;
    c.outputs[0].chain.set(kFlagMono, true);
    c.outputs[0].chain.subsonicHz = 25;
    const auto L = noise(static_cast<size_t>(sr * 0.3), 3), R = noise(L.size(), 4);
    Outs ref;
    {
        MultiOutputEngine e;
        e.setSampleRate(sr);
        e.applyConfigRT(c);
        warmup(e, 4096);
        ref = run(e, L, R, 4096);
    }
    double worst = 0.0;
    for (size_t b : { size_t(64), size_t(256), size_t(480), size_t(1024) }) {
        MultiOutputEngine e;
        e.setSampleRate(sr);
        e.applyConfigRT(c);
        warmup(e, b);
        const Outs o = run(e, L, R, b);
        for (int k = 0; k < 3; ++k)
            for (size_t i = 0; i < L.size(); ++i)
                worst = std::max(worst, std::max(std::fabs(double(o.L[k][i]) - ref.L[k][i]), std::fabs(double(o.R[k][i]) - ref.R[k][i])));
    }
    CHECK(worst == 0.0, "%5.0f Hz: keresztváltó + mélyvágás + késleltetés: blokkmérettől független (eltérés %.3g)", sr, worst);
}

static void testSanitize() {
    MultiConfig c;
    c.numOutputs = 9;
    c.numCrossovers = 7;
    c.slopeDbPerOct = 36;
    c.crossoverHz = { { 5000.0f, 100.0f, NAN } };
    c.outputs[0].gainDb = 99.0f;
    c.outputs[1].delayMs = -5.0f;
    c.outputs[2].band = 12;
    sanitize(c);
    CHECK(c.numOutputs == 4 && c.numCrossovers == 3 && c.slopeDbPerOct == 24, "érvénytelen darabszám és meredekség: 4 kimenet, 3 pont, 24 dB/okt");
    CHECK(c.crossoverHz[0] <= c.crossoverHz[1] && c.crossoverHz[1] <= c.crossoverHz[2], "a vágási pontok növekvő sorrendben (%g / %g / %g)",
          c.crossoverHz[0], c.crossoverHz[1], c.crossoverHz[2]);
    CHECK(c.outputs[0].gainDb == 12.0f && c.outputs[1].delayMs == 0.0f && c.outputs[2].band == 3, "erősítés, késleltetés és sáv a tartományban");
}

int main() {
    std::printf("========================================================\n");
    std::printf(" Aqulify több kimenet: független zónák és aktív keresztváltó\n");
    std::printf("========================================================\n");
    for (double sr : { 44100.0, 48000.0, 96000.0 }) {
        std::printf("\n-- %.1f kHz --\n", sr / 1000.0);
        testTransparent(sr);
        testZones(sr);
        for (int slope : { 12, 24, 48 }) {
            testCrossover(sr, slope, { 80.0f });
            testCrossover(sr, slope, { 150.0f, 2500.0f });
            testCrossover(sr, slope, { 60.0f, 500.0f, 4000.0f });
        }
        testDelayGainPolarity(sr);
        testBlockIndependence(sr);
    }
    std::printf("\n-- beállítások ellenőrzése --\n");
    testSanitize();
    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
