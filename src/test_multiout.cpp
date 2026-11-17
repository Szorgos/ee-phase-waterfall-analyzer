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

#include <atomic>
#include <cstdlib>
#include <ctime>
#include <new>
#include <string>
#include "multi_output.hpp"

using namespace ee_analyzer;
using namespace ee_analyzer::multiout;

static int g_fail = 0;

// K9: Valos ideju allokacios szamlalo (a hangszal hivasai alatt 0 uj foglalas engedelyezett)
static thread_local bool g_trackAlloc = false;
static std::atomic<size_t> g_allocCount{ 0 };

void* operator new(std::size_t n) {
    if (g_trackAlloc) g_allocCount.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n == 0 ? 1 : n);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
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


static void testPlumbing() {
    std::printf("\n-- szálak közti átadás és külső lánc --\n");
    // SeqBox: a teljes beállítás egyben megy át
    SeqBox<MultiConfig> box;
    MultiConfig c;
    c.mode = Mode::Crossover;
    c.numOutputs = 3;
    c.outputs[2].delayMs = 7.5f;
    c.outputs[1].chain.set(kFlagEq, true);
    box.write(c);
    MultiConfig got;
    uint32_t seq = 0;
    const bool ok = box.tryRead(got, seq);
    CHECK(ok && seq == 2 && got.mode == Mode::Crossover && got.numOutputs == 3 && got.outputs[2].delayMs == 7.5f &&
          got.outputs[1].chain.has(kFlagEq), "SeqBox: a beállítás sértetlenül átmegy (seq %u)", seq);

    // Gyűrűpuffer: átfordulás, teli és üres eset
    static StereoSpsc<1024> ring;
    std::vector<float> l(700), r(700), ol(700), orr(700);
    bool same = true;
    for (int round = 0; round < 10; ++round) {
        for (int i = 0; i < 700; ++i) { l[i] = float(round * 1000 + i); r[i] = -l[i]; }
        const uint32_t w = ring.write(l.data(), r.data(), 700);
        const uint32_t rd = ring.read(ol.data(), orr.data(), 700);
        same = same && w == 700 && rd == 700 && ol == l && orr == r;
    }
    CHECK(same, "gyűrűpuffer: 10 kör átfordulással, minden keret sértetlen és sorrendhelyes");
    const uint32_t w1 = ring.write(l.data(), r.data(), 700), w2 = ring.write(l.data(), r.data(), 700);
    CHECK(w1 == 700 && w2 == 324 && ring.available() == 1024, "gyűrűpuffer: teli állapotban a többlet eldobódik (%u + %u)", w1, w2);
    ring.skip(1000);
    const uint32_t rd = ring.read(ol.data(), orr.data(), 700);
    CHECK(rd == 24 && ring.available() == 0, "gyűrűpuffer: eldobás után csak a maradék olvasható (%u)", rd);

    // Külső fő lánc: ugyanaz az eredmény, mint a belsővel
    MultiConfig z;
    z.numOutputs = 2;
    z.outputs[0].chain.set(kFlagMono, true);
    z.outputs[0].chain.subsonicHz = 30;
    z.outputs[1].gainDb = -3.0f;
    const auto L = noise(48000, 7), R = noise(48000, 8);
    MultiOutputEngine a;
    a.applyConfigRT(z);
    warmup(a, 256);
    const Outs oa = run(a, L, R, 256);
    AetherChain ext;
    ext.applyParamsRT(z.outputs[0].chain);
    MultiOutputEngine b;
    b.setExternalMainChain(&ext);
    b.applyConfigRT(z);
    warmup(b, 256);
    const Outs ob = run(b, L, R, 256);
    CHECK(oa.L[0] == ob.L[0] && oa.R[1] == ob.R[1], "külső fő lánc (a motor meglévő lánca): bit-azonos a belsővel");
}

// =========================================================================
// Utonkenti EQ (aktiv keresztvalto) befagyasztott tesztek: K1..K10, K12, 7.3
// =========================================================================

template <class CfgT>
concept HasWayCfg = requires(CfgT& c) {
    c.outputs[0].way.on;
    c.outputs[0].way.numBands;
    c.outputs[0].way.bands[0].type;
    c.outputs[0].way.bands[0].freq;
    c.outputs[0].way.bands[0].gainDb;
    c.outputs[0].way.bands[0].q;
};

template <class EngT>
concept HasWayResp = requires(const EngT& e, int k, const std::vector<double>& freqs) {
    { e.wayResponse(k, freqs) };
    { e.sumResponse(freqs) };
};

namespace {

struct AnalyticBiquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    std::complex<double> eval(double f, double sr) const {
        const double w = 2.0 * kPi * f / sr;
        const std::complex<double> zm1 = std::polar(1.0, -w);
        const std::complex<double> zm2 = zm1 * zm1;
        return (b0 + b1 * zm1 + b2 * zm2) / (1.0 + a1 * zm1 + a2 * zm2);
    }
};

AnalyticBiquad makeAnalyticFirstOrder(bool hp, double fc, double sr) {
    const double k = std::tan(kPi * fc / sr);
    const double n = 1.0 / (1.0 + k);
    AnalyticBiquad c;
    c.b0 = hp ? n : k * n;
    c.b1 = hp ? -n : k * n;
    c.b2 = 0.0;
    c.a1 = (k - 1.0) * n;
    c.a2 = 0.0;
    return c;
}

AnalyticBiquad makeAnalyticSecondOrder(bool hp, double fc, double sr, double q) {
    const double w0 = 2.0 * kPi * fc / sr;
    const double cw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    AnalyticBiquad c;
    if (hp) { c.b0 = (1.0 + cw) / (2.0 * a0); c.b1 = -(1.0 + cw) / a0; }
    else { c.b0 = (1.0 - cw) / (2.0 * a0); c.b1 = (1.0 - cw) / a0; }
    c.b2 = c.b0;
    c.a1 = -2.0 * cw / a0;
    c.a2 = (1.0 - alpha) / a0;
    return c;
}

std::complex<double> evalAnalyticLR(bool hp, double fc, double sr, int slope, double f) {
    if (slope == 12) {
        const auto s = makeAnalyticFirstOrder(hp, fc, sr).eval(f, sr);
        return s * s;
    }
    if (slope == 48) {
        const auto s1 = makeAnalyticSecondOrder(hp, fc, sr, 0.54119610014619698).eval(f, sr);
        const auto s2 = makeAnalyticSecondOrder(hp, fc, sr, 1.30656296487637652).eval(f, sr);
        return s1 * s2 * s1 * s2;
    }
    const auto s = makeAnalyticSecondOrder(hp, fc, sr, 0.70710678118654752).eval(f, sr);
    return s * s;
}

AnalyticBiquad makeAnalyticEq(EqBandType type, double f0, double gainDb, double q, double sr) {
    AnalyticBiquad c;
    if (std::fabs(gainDb) < 1e-9) return c;
    const double nyq = sr * 0.49;
    f0 = std::clamp(f0, 20.0, std::min(20000.0, nyq));
    q = std::clamp(q, 0.3, 10.0);
    const double w0 = 2.0 * kPi * f0 / sr;
    const double cw = std::cos(w0);
    const double sw = std::sin(w0);
    const double alpha = sw / (2.0 * q);
    const double A = std::pow(10.0, gainDb / 40.0);
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    if (type == EqBandType::Bell) {
        b0 = 1.0 + alpha * A;
        b1 = -2.0 * cw;
        b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A;
        a1 = -2.0 * cw;
        a2 = 1.0 - alpha / A;
    } else if (type == EqBandType::LoShelf) {
        const double tsa = 2.0 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1.0) - (A - 1.0) * cw + tsa);
        b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
        b2 = A * ((A + 1.0) - (A - 1.0) * cw - tsa);
        a0 = (A + 1.0) + (A - 1.0) * cw + tsa;
        a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw);
        a2 = (A + 1.0) + (A - 1.0) * cw - tsa;
    } else if (type == EqBandType::HiShelf) {
        const double tsa = 2.0 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1.0) + (A - 1.0) * cw + tsa);
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
        b2 = A * ((A + 1.0) + (A - 1.0) * cw - tsa);
        a0 = (A + 1.0) - (A - 1.0) * cw + tsa;
        a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw);
        a2 = (A + 1.0) - (A - 1.0) * cw - tsa;
    }
    c.b0 = b0 / a0;
    c.b1 = b1 / a0;
    c.b2 = b2 / a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;
    return c;
}

std::complex<double> evalAnalyticXoverBand(const MultiConfig& c, int k, double f, double sr) {
    if (c.mode != Mode::Crossover) return { 1.0, 0.0 };
    const int b = c.outputs[k].band;
    if (b < 0) return { 1.0, 0.0 };
    const int N = c.numCrossovers;
    const int slope = c.slopeDbPerOct;
    const double apSign = (slope == 12) ? -1.0 : 1.0;
    std::complex<double> h(1.0, 0.0);
    double sign = 1.0;
    for (int j = 0; j < N; ++j) {
        const double fc = c.crossoverHz[j];
        int kind = 3;
        if (b == N) kind = 2;
        else if (j < b) kind = 2;
        else if (j == b) kind = 1;
        if (kind == 1) h *= evalAnalyticLR(false, fc, sr, slope, f);
        else if (kind == 2) {
            h *= evalAnalyticLR(true, fc, sr, slope, f);
            if (slope == 12) sign = -sign;
        } else {
            h *= (evalAnalyticLR(false, fc, sr, slope, f) + apSign * evalAnalyticLR(true, fc, sr, slope, f));
        }
    }
    return h * sign;
}

double wrapDeg(double a, double b) {
    double d = a - b;
    while (d > 180.0) d -= 360.0;
    while (d < -180.0) d += 360.0;
    return std::fabs(d);
}

} // namespace

template <class CfgT = MultiConfig, class EngT = MultiOutputEngine>
static void testWayEqK1K4K5Impl() {
    std::printf("\n-- útonkénti EQ K1 / K4 / K5 --\n");
    if constexpr (HasWayCfg<CfgT>) {
        for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 }) {
            CfgT base{};
            base.mode = Mode::Crossover;
            base.numOutputs = 2;
            base.numCrossovers = 1;
            base.crossoverHz[0] = 1000.0f;
            base.slopeDbPerOct = 24;
            base.outputs[0].band = 0;
            base.outputs[1].band = 1;

            const size_t len = static_cast<size_t>(sr * 1.0);
            std::vector<float> imp(len, 0.0f);
            imp[0] = 1.0f;

            EngT eBase;
            eBase.setSampleRate(sr);
            eBase.applyConfigRT(base);
            const Outs oBase = run(eBase, imp, imp, 1024);

            {
                CfgT cOff = base;
                cOff.outputs[1].way.on = false;
                cOff.outputs[1].way.numBands = 1;
                cOff.outputs[1].way.bands[0].type = static_cast<int32_t>(EqBandType::Bell);
                cOff.outputs[1].way.bands[0].freq = 4000.0f;
                cOff.outputs[1].way.bands[0].gainDb = 6.0f;
                cOff.outputs[1].way.bands[0].q = 1.0f;
                EngT eOff;
                eOff.setSampleRate(sr);
                eOff.applyConfigRT(cOff);
                const Outs oOff = run(eOff, imp, imp, 1024);
                CHECK(oOff.L[0] == oBase.L[0] && oOff.L[1] == oBase.L[1] && oOff.R[1] == oBase.R[1],
                      "%6.0f Hz K4: kikapcsolt way EQ mintára azonos a 2.5.0 alapállapottal", sr);
            }

            CfgT cEq = base;
            cEq.outputs[1].way.on = true;
            cEq.outputs[1].way.numBands = 1;
            cEq.outputs[1].way.bands[0].type = static_cast<int32_t>(EqBandType::Bell);
            cEq.outputs[1].way.bands[0].freq = 4000.0f;
            cEq.outputs[1].way.bands[0].gainDb = 6.0f;
            cEq.outputs[1].way.bands[0].q = 1.0f;

            EngT eEq;
            eEq.setSampleRate(sr);
            eEq.applyConfigRT(cEq);
            const Outs oEq = run(eEq, imp, imp, 1024);

            const double gWay2 = db(resp(oEq.L[1], 4000.0, sr)) - db(resp(oBase.L[1], 4000.0, sr));
            const double gWay2R = db(resp(oEq.R[1], 4000.0, sr)) - db(resp(oBase.R[1], 4000.0, sr));
            double maxWay1Diff = 0.0;
            for (double f : logFreqs(20.0, std::min(16000.0, sr * 0.45), 40))
                maxWay1Diff = std::max(maxWay1Diff, std::fabs(db(resp(oEq.L[0], f, sr)) - db(resp(oBase.L[0], f, sr))));

            const auto eqAnalytic = makeAnalyticEq(EqBandType::Bell, 4000.0, 6.0, 1.0, sr);
            const double exp2k = db(evalAnalyticXoverBand(cEq, 1, 2000.0, sr) * eqAnalytic.eval(2000.0, sr));
            const double got2k = db(resp(oEq.L[1], 2000.0, sr));

            std::printf("K_METRIC: {\"test\":\"K1\",\"sr\":%.0f,\"way2_4k_db\":%.4f,\"way1_change_db\":%.6f,\"way2_2k_err_db\":%.4f}\n",
                        sr, gWay2, maxWay1Diff, std::fabs(got2k - exp2k));
            CHECK(std::fabs(gWay2 - 6.0) <= 0.05 && std::fabs(gWay2R - 6.0) <= 0.05 && std::fabs(got2k - exp2k) <= 0.05,
                  "%6.0f Hz K1: 2. út +6 dB @ 4 kHz (mért %.3f dB, 2 kHz eltérés %.4f dB)", sr, gWay2, std::fabs(got2k - exp2k));
            CHECK(maxWay1Diff < 0.01, "%6.0f Hz K1: 1. út változatlan (max eltérés %.6f dB)", sr, maxWay1Diff);

            {
                CfgT cQ3 = base;
                cQ3.outputs[1].way.on = true;
                cQ3.outputs[1].way.numBands = 1;
                cQ3.outputs[1].way.bands[0].type = static_cast<int32_t>(EqBandType::Bell);
                cQ3.outputs[1].way.bands[0].freq = 4000.0f;
                cQ3.outputs[1].way.bands[0].gainDb = 6.0f;
                cQ3.outputs[1].way.bands[0].q = 3.0f;
                EngT eQ3;
                eQ3.setSampleRate(sr);
                eQ3.applyConfigRT(cQ3);
                const Outs oQ3 = run(eQ3, imp, imp, 1024);
                const double exp2k5 = db(evalAnalyticXoverBand(cQ3, 1, 2500.0, sr) *
                                         makeAnalyticEq(EqBandType::Bell, 4000.0, 6.0, 3.0, sr).eval(2500.0, sr));
                const double got2k5 = db(resp(oQ3.L[1], 2500.0, sr));
                CHECK(std::fabs(got2k5 - exp2k5) <= 0.05, "%6.0f Hz Q=3.0 oldalpont @ 2.5 kHz: %.3f dB (elvárt %.3f dB)",
                      sr, got2k5, exp2k5);
            }
        }

        {
            const double sr = 48000.0;
            CfgT zPlain{};
            zPlain.mode = Mode::Zones;
            zPlain.numOutputs = 2;
            CfgT zWithWay = zPlain;
            zWithWay.outputs[0].way.on = true;
            zWithWay.outputs[0].way.numBands = 1;
            zWithWay.outputs[0].way.bands[0].type = static_cast<int32_t>(EqBandType::Bell);
            zWithWay.outputs[0].way.bands[0].freq = 1000.0f;
            zWithWay.outputs[0].way.bands[0].gainDb = 6.0f;
            zWithWay.outputs[0].way.bands[0].q = 1.0f;

            const auto s = noise(24000, 42);
            EngT eA, eB;
            eA.setSampleRate(sr); eB.setSampleRate(sr);
            eA.applyConfigRT(zPlain); eB.applyConfigRT(zWithWay);
            const Outs oA = run(eA, s, s, 256);
            const Outs oB = run(eB, s, s, 256);
            const bool same = (oA.L[0] == oB.L[0] && oA.R[0] == oB.R[0] && oA.L[1] == oB.L[1]);
            const bool kept = eB.config().outputs[0].way.on && eB.config().outputs[0].way.numBands == 1 &&
                              std::fabs(eB.config().outputs[0].way.bands[0].gainDb - 6.0f) < 1e-4f;
            CHECK(same && kept, "K5: zóna módban a way EQ hatástalan (mintára azonos), de a configban megmarad");
        }
    } else {
        CHECK(false, "K1/K4/K5: OutputConfig::way nincs megvalósítva");
    }
}

template <class CfgT = MultiConfig, class EngT = MultiOutputEngine>
static void testWayEqK2ShelvesImpl() {
    std::printf("\n-- útonkénti EQ K2 (polcszűrők) --\n");
    if constexpr (HasWayCfg<CfgT>) {
        const double sr = 96000.0;
        const size_t len = static_cast<size_t>(sr * 1.0);
        std::vector<float> imp(len, 0.0f);
        imp[0] = 1.0f;

        CfgT base{};
        base.mode = Mode::Crossover;
        base.numOutputs = 2;
        base.numCrossovers = 1;
        base.crossoverHz[0] = 800.0f;
        base.slopeDbPerOct = 24;
        base.outputs[0].band = 0;
        base.outputs[1].band = 1;

        EngT eBase;
        eBase.setSampleRate(sr);
        eBase.applyConfigRT(base);
        const Outs oBase = run(eBase, imp, imp, 1024);

        CfgT c = base;
        c.outputs[0].way.on = true;
        c.outputs[0].way.numBands = 1;
        c.outputs[0].way.bands[0].type = static_cast<int32_t>(EqBandType::LoShelf);
        c.outputs[0].way.bands[0].freq = 60.0f;
        c.outputs[0].way.bands[0].gainDb = -6.0f;
        c.outputs[0].way.bands[0].q = 0.70710678f;

        c.outputs[1].way.on = true;
        c.outputs[1].way.numBands = 1;
        c.outputs[1].way.bands[0].type = static_cast<int32_t>(EqBandType::HiShelf);
        c.outputs[1].way.bands[0].freq = 8000.0f;
        c.outputs[1].way.bands[0].gainDb = 3.0f;
        c.outputs[1].way.bands[0].q = 0.70710678f;

        EngT e;
        e.setSampleRate(sr);
        e.applyConfigRT(c);
        const Outs o = run(e, imp, imp, 1024);

        const double ls60 = db(resp(o.L[0], 60.0, sr)) - db(resp(oBase.L[0], 60.0, sr));
        const double ls15 = db(resp(o.L[0], 15.0, sr)) - db(resp(oBase.L[0], 15.0, sr));
        const double hs8k = db(resp(o.L[1], 8000.0, sr)) - db(resp(oBase.L[1], 8000.0, sr));
        const double hs24k = db(resp(o.L[1], 24000.0, sr)) - db(resp(oBase.L[1], 24000.0, sr));

        std::printf("K_METRIC: {\"test\":\"K2\",\"ls_60hz_db\":%.4f,\"ls_15hz_db\":%.4f,\"hs_8khz_db\":%.4f,\"hs_24khz_db\":%.4f}\n",
                    ls60, ls15, hs8k, hs24k);
        CHECK(std::fabs(ls60 + 3.0) <= 0.1 && std::fabs(ls15 + 6.0) <= 0.1,
              "K2 LowShelf -6 dB @ 60 Hz: 60 Hz-en %.3f dB (-3.00), 15 Hz-en %.3f dB (-6.00)", ls60, ls15);
        CHECK(std::fabs(hs8k - 1.5) <= 0.1 && std::fabs(hs24k - 3.0) <= 0.1,
              "K2 HighShelf +3 dB @ 8 kHz: 8 kHz-en %.3f dB (+1.50), 24 kHz-en %.3f dB (+3.00)", hs8k, hs24k);
    } else {
        CHECK(false, "K2: OutputConfig::way nincs megvalósítva");
    }
}

static void testWayEqK3LeakRegression() {
    std::printf("\n-- útonkénti EQ K3 (szivárgás-regresszió) --\n");
    const double sr = 48000.0;
    const auto s = noise(static_cast<size_t>(sr * 0.3), 19);

    MultiConfig cleanXover;
    cleanXover.mode = Mode::Crossover;
    cleanXover.numOutputs = 2;
    cleanXover.numCrossovers = 1;
    cleanXover.crossoverHz[0] = 500.0f;
    cleanXover.slopeDbPerOct = 24;
    cleanXover.outputs[0].band = 0;
    cleanXover.outputs[1].band = 1;

    MultiOutputEngine eRef;
    eRef.setSampleRate(sr);
    eRef.applyConfigRT(cleanXover);
    warmup(eRef, 256);
    const Outs oRef = run(eRef, s, s, 256);

    MultiConfig zDirty = cleanXover;
    zDirty.mode = Mode::Zones;
    zDirty.outputs[1].chain.set(kFlagEq, true);
    zDirty.outputs[1].chain.preampDb = -6.0f;
    zDirty.outputs[1].chain.numBands = 1;
    zDirty.outputs[1].chain.bands[0].type = static_cast<int32_t>(EqBandType::Bell);
    zDirty.outputs[1].chain.bands[0].mode = static_cast<int32_t>(EqFilterMode::RlcBt);
    zDirty.outputs[1].chain.bands[0].freq = 1000.0f;
    zDirty.outputs[1].chain.bands[0].gainDb = 6.0f;
    zDirty.outputs[1].chain.bands[0].q = 1.0f;

    MultiOutputEngine eTest;
    eTest.setSampleRate(sr);
    eTest.applyConfigRT(zDirty);
    run(eTest, s, s, 256);

    MultiConfig xoverAfter = zDirty;
    xoverAfter.mode = Mode::Crossover;
    eTest.applyConfigRT(xoverAfter);
    warmup(eTest, 256);
    const Outs oTest = run(eTest, s, s, 256);

    double worst = 0.0;
    for (int k = 0; k < 2; ++k)
        for (size_t i = 0; i < s.size(); ++i)
            worst = std::max(worst, std::max(std::fabs(double(oTest.L[k][i]) - oRef.L[k][i]),
                                             std::fabs(double(oTest.R[k][i]) - oRef.R[k][i])));
    std::printf("K_METRIC: {\"test\":\"K3\",\"max_leak_diff\":%.9g}\n", worst);
    CHECK(oTest.L[0] == oRef.L[0] && oTest.L[1] == oRef.L[1] && oTest.R[1] == oRef.R[1],
          "K3: zóna láncán lévő +6 dB EQ / -6 dB preamp NEM szivárog keresztváltó módba (eltérés %.3g)", worst);
}

template <class CfgT = MultiConfig, class EngT = MultiOutputEngine>
static void testWayEqK6K7Impl() {
    std::printf("\n-- útonkénti EQ K6 / K7 (nulla késleltetés és wayResponse egyezés) --\n");
    if constexpr (HasWayCfg<CfgT> && HasWayResp<EngT>) {
        const double sr = 48000.0;
        const size_t len = static_cast<size_t>(sr * 1.0);
        std::vector<float> imp(len, 0.0f);
        imp[0] = 1.0f;

        {
            CfgT c{};
            c.mode = Mode::Crossover;
            c.numOutputs = 2;
            c.numCrossovers = 1;
            c.crossoverHz[0] = 800.0f;
            c.slopeDbPerOct = 24;
            c.outputs[0].band = 0;
            c.outputs[1].band = 1;

            EngT eNoEq;
            eNoEq.setSampleRate(sr);
            eNoEq.applyConfigRT(c);
            const Outs oNo = run(eNoEq, imp, imp, 1024);

            std::vector<float> sum(len, 0.0f);
            for (size_t i = 0; i < len; ++i) sum[i] = oNo.L[0][i] + oNo.L[1][i];
            double flatErr = 0.0;
            for (double f : logFreqs(20.0, 20000.0, 120)) flatErr = std::max(flatErr, std::fabs(db(resp(sum, f, sr))));

            CfgT cWide = c;
            cWide.outputs[0].band = -1;
            EngT eWideNo;
            eWideNo.setSampleRate(sr);
            eWideNo.applyConfigRT(cWide);
            const Outs oWideNo = run(eWideNo, imp, imp, 1024);

            cWide.outputs[0].way.on = true;
            cWide.outputs[0].way.numBands = 2;
            cWide.outputs[0].way.bands[0].type = static_cast<int32_t>(EqBandType::Bell);
            cWide.outputs[0].way.bands[0].freq = 1000.0f;
            cWide.outputs[0].way.bands[0].gainDb = 3.0f;
            cWide.outputs[0].way.bands[0].q = 1.0f;
            cWide.outputs[0].way.bands[1].type = static_cast<int32_t>(EqBandType::HiShelf);
            cWide.outputs[0].way.bands[1].freq = 6000.0f;
            cWide.outputs[0].way.bands[1].gainDb = -4.0f;
            cWide.outputs[0].way.bands[1].q = 0.7071f;
            EngT eWideEq;
            eWideEq.setSampleRate(sr);
            eWideEq.applyConfigRT(cWide);
            const Outs oWideEq = run(eWideEq, imp, imp, 1024);

            auto peakIdx = [](const std::vector<float>& v) {
                return static_cast<size_t>(std::max_element(v.begin(), v.begin() + 2048,
                    [](float a, float b) { return std::fabs(a) < std::fabs(b); }) - v.begin());
            };
            const size_t pNo = peakIdx(oWideNo.L[0]);
            const size_t pEq = peakIdx(oWideEq.L[0]);
            std::printf("K_METRIC: {\"test\":\"K6\",\"peak_no_eq\":%zu,\"peak_with_eq\":%zu,\"flat_sum_err_db\":%.5f}\n",
                        pNo, pEq, flatErr);
            CHECK(pNo == pEq && flatErr <= 0.01,
                  "K6: nulla késleltetés (csúcs %zu == %zu) és sík összeg EQ nélkül (%.5f dB <= 0.01 dB)", pNo, pEq, flatErr);
        }

        {
            CfgT c{};
            c.mode = Mode::Crossover;
            c.numOutputs = 3;
            c.numCrossovers = 2;
            c.crossoverHz = { { 150.0f, 2500.0f, 8000.0f } };
            c.slopeDbPerOct = 24;
            for (int k = 0; k < 3; ++k) c.outputs[k].band = k;
            c.outputs[1].gainDb = -2.5f;
            c.outputs[1].invert = true;
            c.outputs[2].delayMs = 0.75f;

            c.outputs[0].way.on = true;
            c.outputs[0].way.numBands = 2;
            c.outputs[0].way.bands[0].type = static_cast<int32_t>(EqBandType::Bell);
            c.outputs[0].way.bands[0].freq = 80.0f;
            c.outputs[0].way.bands[0].gainDb = -5.0f;
            c.outputs[0].way.bands[0].q = 2.5f;
            c.outputs[0].way.bands[1].type = static_cast<int32_t>(EqBandType::LoShelf);
            c.outputs[0].way.bands[1].freq = 50.0f;
            c.outputs[0].way.bands[1].gainDb = 3.0f;
            c.outputs[0].way.bands[1].q = 0.8f;

            c.outputs[2].way.on = true;
            c.outputs[2].way.numBands = 1;
            c.outputs[2].way.bands[0].type = static_cast<int32_t>(EqBandType::HiShelf);
            c.outputs[2].way.bands[0].freq = 6000.0f;
            c.outputs[2].way.bands[0].gainDb = -3.5f;
            c.outputs[2].way.bands[0].q = 1.2f;

            EngT e;
            e.setSampleRate(sr);
            e.applyConfigRT(c);
            const Outs o = run(e, imp, imp, 1024);

            std::vector<double> freqs;
            for (int m = 0;; ++m) {
                const double f = 30.0 * std::pow(2.0, m / 12.0);
                if (f > 16000.1) break;
                freqs.push_back(f);
            }

            double worstDb = 0.0, worstDeg = 0.0;
            for (int k = 0; k < 3; ++k) {
                const auto wr = e.wayResponse(k, freqs);
                if (wr.size() != freqs.size()) { worstDb = 999.0; break; }
                for (size_t i = 0; i < freqs.size(); ++i) {
                    const auto meas = resp(o.L[k], freqs[i], sr);
                    const double magM = db(meas), magW = db(wr[i]);
                    worstDb = std::max(worstDb, std::fabs(magM - magW));
                    if (magM > -60.0) {
                        const double phM = std::arg(meas) * 180.0 / kPi;
                        const double phW = std::arg(wr[i]) * 180.0 / kPi;
                        worstDeg = std::max(worstDeg, wrapDeg(phM, phW));
                    }
                }
            }
            const auto srVec = e.sumResponse(freqs);
            double sumDbErr = 0.0, sumDegErr = 0.0;
            if (srVec.size() != freqs.size()) sumDbErr = 999.0;
            else {
                std::vector<float> sumImp(len, 0.0f);
                for (int k = 0; k < 3; ++k) for (size_t i = 0; i < len; ++i) sumImp[i] += o.L[k][i];
                for (size_t i = 0; i < freqs.size(); ++i) {
                    const auto meas = resp(sumImp, freqs[i], sr);
                    sumDbErr = std::max(sumDbErr, std::fabs(db(meas) - db(srVec[i])));
                    if (db(meas) > -60.0) {
                        sumDegErr = std::max(sumDegErr, wrapDeg(std::arg(meas) * 180.0 / kPi, std::arg(srVec[i]) * 180.0 / kPi));
                    }
                }
            }
            std::printf("K_METRIC: {\"test\":\"K7\",\"way_max_db\":%.5f,\"way_max_deg\":%.4f,\"sum_max_db\":%.5f,\"sum_max_deg\":%.4f}\n",
                        worstDb, worstDeg, sumDbErr, sumDegErr);
            CHECK(worstDb <= 0.05 && worstDeg <= 1.0 && sumDbErr <= 0.05 && sumDegErr <= 1.0,
                  "K7: wayResponse/sumResponse egyezik a mért IR-rel (max %.4f dB / %.3f fok)", std::max(worstDb, sumDbErr), std::max(worstDeg, sumDegErr));
        }
    } else {
        CHECK(false, "K6/K7: OutputConfig::way vagy wayResponse nincs megvalósítva");
    }
}

template <class CfgT = MultiConfig, class EngT = MultiOutputEngine>
static void testWayEqK8K9K10K12Impl() {
    std::printf("\n-- útonkénti EQ K8 / K9 / K10 / K12 --\n");
    if constexpr (HasWayCfg<CfgT>) {
        const double sr = 48000.0;
        constexpr int kMaxWayB = static_cast<int>(sizeof(CfgT{}.outputs[0].way.bands) / sizeof(CfgT{}.outputs[0].way.bands[0]));

        {
            CfgT c{};
            c.mode = Mode::Crossover;
            c.numOutputs = 2;
            c.numCrossovers = 1;
            c.crossoverHz[0] = 200.0f;
            c.slopeDbPerOct = 24;
            c.outputs[0].band = 0;
            c.outputs[1].band = 1;

            auto secondDiffPeak = [&](const std::vector<float>& x, size_t a, size_t b) {
                double pk = 0.0;
                for (size_t i = std::max<size_t>(2, a); i < std::min(x.size(), b); ++i) {
                    const double d2 = double(x[i]) - 2.0 * x[i - 1] + x[i - 2];
                    pk = std::max(pk, std::fabs(d2));
                }
                return pk;
            };

            const size_t n = 4800;
            std::vector<float> sine(n), oL0(n), oL1(n), oR0(n), oR1(n);
            for (size_t i = 0; i < n; ++i) sine[i] = static_cast<float>(0.4 * std::sin(2.0 * kPi * 1000.0 * i / sr));
            float* pl[2] = { oL0.data(), oL1.data() };
            float* pr[2] = { oR0.data(), oR1.data() };

            EngT eRef;
            eRef.setSampleRate(sr);
            eRef.applyConfigRT(c);
            eRef.process(sine.data(), sine.data(), pl, pr, n);
            const double baseD2 = secondDiffPeak(oL1, 1000, n - 100);

            EngT eSw;
            eSw.setSampleRate(sr);
            eSw.applyConfigRT(c);
            eSw.process(sine.data(), sine.data(), pl, pr, n / 2);

            CfgT c2 = c;
            c2.outputs[1].way.on = true;
            c2.outputs[1].way.numBands = 1;
            c2.outputs[1].way.bands[0].type = static_cast<int32_t>(EqBandType::Bell);
            c2.outputs[1].way.bands[0].freq = 1000.0f;
            c2.outputs[1].way.bands[0].gainDb = 6.0f;
            c2.outputs[1].way.bands[0].q = 2.0f;
            eSw.applyConfigRT(c2);

            float* pl2[2] = { oL0.data() + n / 2, oL1.data() + n / 2 };
            float* pr2[2] = { oR0.data() + n / 2, oR1.data() + n / 2 };
            eSw.process(sine.data() + n / 2, sine.data() + n / 2, pl2, pr2, n - n / 2);
            const double swD2 = secondDiffPeak(oL1, n / 2 - 4, n / 2 + 512);
            const double ratio = swD2 / std::max(1e-12, baseD2);
            std::printf("K_METRIC: {\"test\":\"K8\",\"base_d2\":%.6g,\"switch_d2\":%.6g,\"ratio\":%.4f}\n", baseD2, swD2, ratio);
            CHECK(ratio <= 2.0, "K8: váltás közbeni 2. differencia csúcs %.3fx <= 2.0x (alap %.4g, váltás %.4g)", ratio, baseD2, swD2);
        }

        {
            CfgT c{};
            c.mode = Mode::Crossover;
            c.numOutputs = 4;
            c.numCrossovers = 3;
            c.crossoverHz = { { 80.0f, 500.0f, 3500.0f } };
            c.slopeDbPerOct = 24;
            for (int k = 0; k < 4; ++k) {
                c.outputs[k].band = k;
                c.outputs[k].way.on = true;
                c.outputs[k].way.numBands = kMaxWayB;
                for (int b = 0; b < kMaxWayB; ++b) {
                    c.outputs[k].way.bands[b].type = static_cast<int32_t>(EqBandType::Bell);
                    c.outputs[k].way.bands[b].freq = 40.0f * std::pow(1.7f, float(b));
                    c.outputs[k].way.bands[b].gainDb = (b % 2 == 0) ? 2.5f : -3.0f;
                    c.outputs[k].way.bands[b].q = 1.4f;
                }
            }
            EngT e;
            e.setSampleRate(sr);
            std::vector<float> in(1024, 0.1f);
            std::vector<std::vector<float>> bufL(4, std::vector<float>(1024)), bufR(4, std::vector<float>(1024));
            float* pl[4] = { bufL[0].data(), bufL[1].data(), bufL[2].data(), bufL[3].data() };
            float* pr[4] = { bufR[0].data(), bufR[1].data(), bufR[2].data(), bufR[3].data() };
            e.applyConfigRT(c);
            e.process(in.data(), in.data(), pl, pr, 1024);

            c.outputs[2].way.bands[0].gainDb = -5.0f;
            c.outputs[3].way.bands[1].freq = 5200.0f;
            g_allocCount.store(0, std::memory_order_relaxed);
            g_trackAlloc = true;
            e.applyConfigRT(c);
            e.process(in.data(), in.data(), pl, pr, 1024);
            e.process(in.data(), in.data(), pl, pr, 480);
            g_trackAlloc = false;
            const size_t allocs = g_allocCount.load(std::memory_order_relaxed);
            std::printf("K_METRIC: {\"test\":\"K9\",\"rt_allocs\":%zu}\n", allocs);
            CHECK(allocs == 0, "K9: hangszálon (applyConfigRT + process) 0 foglalás (mért %zu)", allocs);
        }

        {
            CfgT c{};
            c.mode = Mode::Crossover;
            c.numOutputs = 3;
            c.numCrossovers = 2;
            c.crossoverHz = { { 120.0f, 3000.0f, 8000.0f } };
            c.slopeDbPerOct = 48;
            for (int k = 0; k < 3; ++k) c.outputs[k].band = k;
            c.outputs[1].way.on = true;
            c.outputs[1].way.numBands = 3;
            c.outputs[1].way.bands[0].type = static_cast<int32_t>(EqBandType::Bell);
            c.outputs[1].way.bands[0].freq = 800.0f; c.outputs[1].way.bands[0].gainDb = -4.5f; c.outputs[1].way.bands[0].q = 2.0f;
            c.outputs[1].way.bands[1].type = static_cast<int32_t>(EqBandType::LoShelf);
            c.outputs[1].way.bands[1].freq = 200.0f; c.outputs[1].way.bands[1].gainDb = 3.0f; c.outputs[1].way.bands[1].q = 0.707f;
            c.outputs[1].way.bands[2].type = static_cast<int32_t>(EqBandType::HiShelf);
            c.outputs[1].way.bands[2].freq = 2500.0f; c.outputs[1].way.bands[2].gainDb = -2.0f; c.outputs[1].way.bands[2].q = 1.2f;

            const auto L = noise(static_cast<size_t>(sr * 0.25), 31), R = noise(L.size(), 32);
            Outs ref;
            { EngT e; e.setSampleRate(sr); e.applyConfigRT(c); warmup(e, 4096); ref = run(e, L, R, 4096); }
            double worst = 0.0;
            for (size_t b : { size_t(1), size_t(7), size_t(64), size_t(256), size_t(1024) }) {
                EngT e; e.setSampleRate(sr); e.applyConfigRT(c); warmup(e, b);
                const Outs o = run(e, L, R, b);
                for (int k = 0; k < 3; ++k)
                    for (size_t i = 0; i < L.size(); ++i)
                        worst = std::max(worst, std::max(std::fabs(double(o.L[k][i]) - ref.L[k][i]),
                                                         std::fabs(double(o.R[k][i]) - ref.R[k][i])));
            }
            std::printf("K_METRIC: {\"test\":\"K10\",\"block_diff\":%.9g}\n", worst);
            CHECK(worst == 0.0, "K10: útonkénti EQ blokkmérettől (1..4096) független (eltérés %.3g)", worst);
        }

        {
            CfgT c{};
            c.outputs[0].way.on = true;
            c.outputs[0].way.numBands = 99;
            c.outputs[0].way.bands[0].type = 999;
            c.outputs[0].way.bands[0].freq = NAN;
            c.outputs[0].way.bands[0].gainDb = 12.0f;
            c.outputs[0].way.bands[0].q = INFINITY;
            c.outputs[0].way.bands[1].type = static_cast<int32_t>(EqBandType::HiPass);
            c.outputs[0].way.bands[1].freq = 5.0f;
            c.outputs[0].way.bands[1].gainDb = -99.0f;
            c.outputs[0].way.bands[1].q = -5.0f;
            c.outputs[0].way.bands[2].type = static_cast<int32_t>(EqBandType::Bell);
            c.outputs[0].way.bands[2].freq = 50000.0f;
            c.outputs[0].way.bands[2].gainDb = 6.5f;
            c.outputs[0].way.bands[2].q = 50.0f;

            sanitize(c);
            const auto& w = c.outputs[0].way;
            auto validType = [](int32_t t) {
                return t == static_cast<int32_t>(EqBandType::Bell) ||
                       t == static_cast<int32_t>(EqBandType::LoShelf) ||
                       t == static_cast<int32_t>(EqBandType::HiShelf);
            };
            bool ok = w.numBands == kMaxWayB;
            for (int i = 0; i < w.numBands; ++i) {
                const auto& b = w.bands[i];
                ok = ok && validType(b.type) &&
                     std::isfinite(b.freq) && b.freq >= 20.0f && b.freq <= 20000.0f &&
                     std::isfinite(b.gainDb) && b.gainDb >= -18.0f && b.gainDb <= 6.0f &&
                     std::isfinite(b.q) && b.q >= 0.3f && b.q <= 10.0f;
            }
            CHECK(ok, "K12: sanitize numBands=%d (<=10), érvényes típusok és tartományok (+6 dB korlát)", w.numBands);
        }
    } else {
        CHECK(false, "K8/K9/K10/K12: OutputConfig::way nincs megvalósítva");
    }
}

template <class CfgT = MultiConfig, class EngT = MultiOutputEngine>
static void testWayEqSeed50Impl() {
    unsigned seed = static_cast<unsigned>(std::time(nullptr));
    if (const char* env = std::getenv("AQ_CHECK_SEED"); env && *env) seed = static_cast<unsigned>(std::strtoul(env, nullptr, 10));
    std::printf("\nAQ_CHECK_SEED=%u\n", seed);
    std::printf("-- útonkénti EQ 7.3 véletlen-mag teszt (50 eset) --\n");

    if constexpr (HasWayCfg<CfgT>) {
        constexpr int kMaxWayB = static_cast<int>(sizeof(CfgT{}.outputs[0].way.bands) / sizeof(CfgT{}.outputs[0].way.bands[0]));
        std::mt19937 rng(seed);
        const std::array<double, 4> srs = { 44100.0, 48000.0, 96000.0, 192000.0 };
        const std::array<int, 3> slopes = { 12, 24, 48 };
        const std::array<EqBandType, 3> types = { EqBandType::Bell, EqBandType::LoShelf, EqBandType::HiShelf };

        double worstGlobalErr = 0.0;
        double worstOtherErr = 0.0;
        int passedCases = 0;

        for (int tc = 0; tc < 50; ++tc) {
            const double sr = srs[rng() % srs.size()];
            const int nOut = 2 + static_cast<int>(rng() % 3);
            const int nXo = 1 + static_cast<int>(rng() % (nOut - 1));
            const int slope = slopes[rng() % slopes.size()];

            CfgT base{};
            base.mode = Mode::Crossover;
            base.numOutputs = nOut;
            base.numCrossovers = nXo;
            base.slopeDbPerOct = slope;
            for (int j = 0; j < nXo; ++j) {
                const double lo = 80.0 * std::pow(4.0, j);
                base.crossoverHz[j] = static_cast<float>(lo + std::uniform_real_distribution<double>(0.0, lo)(rng));
            }
            for (int k = 0; k < nOut; ++k) base.outputs[k].band = std::min(k, nXo);

            const int targetWay = static_cast<int>(rng() % nOut);
            const int nBands = 1 + static_cast<int>(rng() % kMaxWayB);
            CfgT cEq = base;
            cEq.outputs[targetWay].way.on = true;
            cEq.outputs[targetWay].way.numBands = nBands;
            for (int b = 0; b < nBands; ++b) {
                auto& band = cEq.outputs[targetWay].way.bands[b];
                band.type = static_cast<int32_t>(types[rng() % types.size()]);
                band.freq = static_cast<float>(std::exp(std::uniform_real_distribution<double>(std::log(25.0), std::log(16000.0))(rng)));
                band.gainDb = static_cast<float>(std::uniform_real_distribution<double>(-18.0, 6.0)(rng));
                band.q = static_cast<float>(std::uniform_real_distribution<double>(0.3, 10.0)(rng));
            }

            const size_t len = static_cast<size_t>(sr * 0.5);
            std::vector<float> imp(len, 0.0f);
            imp[0] = 1.0f;

            EngT eBase, eEq;
            eBase.setSampleRate(sr); eEq.setSampleRate(sr);
            eBase.applyConfigRT(base); eEq.applyConfigRT(cEq);
            const Outs oBase = run(eBase, imp, imp, 1024);
            const Outs oEq = run(eEq, imp, imp, 1024);

            bool caseOk = true;
            for (double f : logFreqs(30.0, 16000.0, 24)) {
                std::complex<double> hAna = evalAnalyticXoverBand(cEq, targetWay, f, sr);
                for (int b = 0; b < nBands; ++b) {
                    const auto& bd = cEq.outputs[targetWay].way.bands[b];
                    hAna *= makeAnalyticEq(static_cast<EqBandType>(bd.type), bd.freq, bd.gainDb, bd.q, sr).eval(f, sr);
                }
                const double expDb = db(hAna);
                const double measDb = db(resp(oEq.L[targetWay], f, sr));
                const double err = std::fabs(measDb - expDb);
                worstGlobalErr = std::max(worstGlobalErr, err);
                if (err > 0.05) caseOk = false;
            }
            for (int k = 0; k < nOut; ++k) {
                if (k == targetWay) continue;
                for (size_t i = 0; i < len; ++i)
                    worstOtherErr = std::max(worstOtherErr, std::fabs(double(oEq.L[k][i]) - oBase.L[k][i]));
            }
            if (worstOtherErr > 1e-6) caseOk = false;
            if (caseOk) ++passedCases;
        }
        std::printf("K_METRIC: {\"test\":\"SEED50\",\"seed\":%u,\"passed\":%d,\"max_err_db\":%.5f,\"other_way_diff\":%.3g}\n",
                    seed, passedCases, worstGlobalErr, worstOtherErr);
        CHECK(passedCases == 50 && worstGlobalErr <= 0.05 && worstOtherErr == 0.0,
              "7.3 AQ_CHECK_SEED=%u: 50/50 véletlen eset egyezik az analitikus elvárással (átment %d/50, max %.4f dB, más út %.3g)",
              seed, passedCases, worstGlobalErr, worstOtherErr);
    } else {
        CHECK(false, "7.3 AQ_CHECK_SEED: OutputConfig::way nincs megvalósítva");
    }
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
    testPlumbing();
    std::printf("\n-- beállítások ellenőrzése --\n");
    testSanitize();
    testWayEqK1K4K5Impl();
    testWayEqK2ShelvesImpl();
    testWayEqK3LeakRegression();
    testWayEqK6K7Impl();
    testWayEqK8K9K10K12Impl();
    testWayEqSeed50Impl();
    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
