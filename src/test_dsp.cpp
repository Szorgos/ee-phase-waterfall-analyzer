// DSP modulteszt: partícionált konvolver, FIR EQ tervezés, Bass Enhancer, teljes lánc háttérszállal.
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

#include "aether_chain.hpp"

using namespace ee_analyzer;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; std::printf("  [HIBA] " __VA_ARGS__); } else { std::printf("  [OK]   " __VA_ARGS__); } std::printf("\n"); } while (0)

static constexpr double kPi = 3.14159265358979323846;

/** Goertzel: adott frekvencia amplitúdója a jelben. */
static double toneAmp(const std::vector<float>& x, size_t from, double f, double sr) {
    const size_t n = x.size() - from;
    std::complex<double> acc(0, 0);
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * kPi * i / (n - 1));
        acc += w * x[from + i] * std::polar(1.0, -2 * kPi * f * i / sr);
    }
    return 2.0 * std::abs(acc) / (n * 0.5);
}

static void testConvolver() {
    std::printf("[1] Partícionált konvolver = közvetlen konvolúció\n");
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.0f, 0.3f);
    std::vector<float> hl(5000), hr(3333);
    for (size_t i = 0; i < hl.size(); ++i) hl[i] = nd(rng) * std::exp(-static_cast<float>(i) / 900.0f);
    for (size_t i = 0; i < hr.size(); ++i) hr[i] = nd(rng) * std::exp(-static_cast<float>(i) / 500.0f);
    const size_t N = 20000;
    std::vector<float> xl(N), xr(N);
    for (auto& v : xl) v = nd(rng);
    for (auto& v : xr) v = nd(rng);

    PartitionedConvolver pc;
    pc.build(hl, hr);
    std::vector<float> yl = xl, yr = xr;
    // változó blokkméretek, mint egy valódi hosztban
    size_t off = 0, k = 0;
    const size_t sizes[] = { 1, 480, 37, 256, 1023, 64, 999 };
    while (off < N) {
        const size_t m = std::min(sizes[k++ % 7], N - off);
        pc.process(yl.data() + off, yr.data() + off, m);
        off += m;
    }
    double maxErr = 0.0;
    for (size_t n = 0; n < N; n += 7) {
        double rl = 0, rr = 0;
        for (size_t i = 0; i < hl.size() && i <= n; ++i) rl += static_cast<double>(hl[i]) * xl[n - i];
        for (size_t i = 0; i < hr.size() && i <= n; ++i) rr += static_cast<double>(hr[i]) * xr[n - i];
        maxErr = std::max({ maxErr, std::abs(rl - yl[n]), std::abs(rr - yr[n]) });
    }
    CHECK(maxErr < 2e-4, "max eltérés a közvetlen konvolúciótól: %.2e (L 5000, R 3333 tap, vegyes blokkméret)", maxErr);

    // Nulla késleltetés: egységimpulzusra az IR jön ki az első mintától
    PartitionedConvolver pc2;
    pc2.build(hl, {});
    std::vector<float> il(6000, 0.0f), ir(6000, 0.0f);
    il[0] = ir[0] = 1.0f;
    pc2.process(il.data(), ir.data(), il.size());
    double e = 0;
    for (size_t i = 0; i < hl.size(); ++i) e = std::max(e, static_cast<double>(std::abs(il[i] - hl[i])));
    CHECK(e < 1e-5 && std::abs(ir[100] - hl[100]) < 1e-5, "impulzusválasz minta-pontosan, 0 késleltetéssel (eltérés %.1e, mono IR mindkét csatornán)", e);
}

static std::vector<BiquadFilter> testFilters(double sr) {
    return { BiquadFilter(FilterType::LowShelf, 105.0, 0.71, 5.5, sr),
             BiquadFilter(FilterType::PeakingEQ, 1000.0, 1.0, 6.0, sr),
             BiquadFilter(FilterType::PeakingEQ, 3500.0, 2.0, -4.0, sr),
             BiquadFilter(FilterType::HighShelf, 10000.0, 0.71, 2.0, sr) };
}

static double firMagDb(const std::vector<float>& h, double f, double sr) {
    std::complex<double> acc(0, 0);
    for (size_t i = 0; i < h.size(); ++i) acc += static_cast<double>(h[i]) * std::polar(1.0, -2 * kPi * f * i / sr);
    return 20 * std::log10(std::abs(acc));
}

static void testFirDesign() {
    std::printf("[2] FIR EQ tervezés (8192 tap, 48 kHz)\n");
    const double sr = 48000;
    auto filters = testFilters(sr);
    FilterChain chain;
    chain.filters = filters;
    const double freqs[] = { 60, 105, 300, 1000, 3500, 8000, 12000, 16000 };
    struct M { FirPhase p; const char* name; size_t expLat; };
    for (M m : { M{ FirPhase::Linear, "lineáris", 4096 }, M{ FirPhase::Intermediate, "köztes", 409 }, M{ FirPhase::Minimum, "minimál", 0 } }) {
        size_t lat = 999999;
        auto h = designEqFir(filters, sr, m.p, 8192, &lat);
        double worst = 0;
        for (double f : freqs) worst = std::max(worst, std::abs(firMagDb(h, f, sr) - chain.magnitudeDbAt(f)));
        size_t peak = 0;
        for (size_t i = 0; i < h.size(); ++i) if (std::abs(h[i]) > std::abs(h[peak])) peak = i;
        CHECK(worst < 0.35 && lat == m.expLat, "%s: amplitúdóhiba max %.3f dB, késleltetés %zu minta, csúcs a %zu. mintánál",
              m.name, worst, lat, peak);
        if (m.p == FirPhase::Linear) {
            double asym = 0;
            for (size_t i = 1; i < 4096; ++i) asym = std::max(asym, static_cast<double>(std::abs(h[4096 - i] - h[4096 + i])));
            CHECK(asym < 1e-6, "lineáris fázis: szimmetrikus impulzus (aszimmetria %.1e)", asym);
        }
        if (m.p == FirPhase::Minimum) {
            double e = 0, e100 = 0;
            for (size_t i = 0; i < h.size(); ++i) { e += h[i] * h[i]; if (i < 100) e100 += h[i] * h[i]; }
            CHECK(e100 / e > 0.9, "minimálfázis: az energia %.1f%%-a az első 100 mintában (nincs előcsengés)", 100 * e100 / e);
        }
    }
}

static void testBass() {
    std::printf("[3] Bass Enhancer\n");
    const double sr = 48000;
    std::vector<float> x(sr), y;
    for (size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.3 * std::sin(2 * kPi * 50 * i / sr));
    for (int type = 0; type < 3; ++type) {
        BassEnhancer b;
        b.enabled = true; b.sampleRate = sr; b.crossoverHz = 80; b.harmonicsDb = 6; b.subcutHz = 30;
        b.type = static_cast<BassEnhancer::Harmonics>(type);
        b.update();
        std::vector<float> l = x, r = x;
        b.process(l.data(), r.data(), l.size());
        const double a1 = toneAmp(l, 24000, 50, sr), a2 = toneAmp(l, 24000, 100, sr), a3 = toneAmp(l, 24000, 150, sr);
        const char* names[] = { "2.+3.", "csak 2.", "csak 3." };
        const bool ok = (type == 0 && a2 > 0.02 && a3 > 0.01) || (type == 1 && a2 > 0.02 && a3 < a2 * 0.1) ||
                        (type == 2 && a3 > 0.01 && a2 < a3 * 0.1);
        CHECK(ok, "%s felharmonikus: 50 Hz %.3f, 100 Hz %.3f, 150 Hz %.3f", names[type], a1, a2, a3);
    }
    BassEnhancer b;
    b.enabled = true; b.sampleRate = sr; b.harmonicsDb = 0; b.subcutHz = 30; b.update();
    std::vector<float> l(sr), r(sr);
    for (size_t i = 0; i < l.size(); ++i) l[i] = r[i] = static_cast<float>(0.3 * std::sin(2 * kPi * 15 * i / sr));
    b.process(l.data(), r.data(), l.size());
    const double a15 = toneAmp(l, 24000, 15, sr);
    CHECK(20 * std::log10(a15 / 0.3) < -12, "mélyvágás (30 Hz): 15 Hz csillapítása %.1f dB", 20 * std::log10(a15 / 0.3));
}

static void testChain() {
    std::printf("[4] Teljes lánc háttérszállal (FIR EQ, konvolver, keresztúsztatás)\n");
    const double sr = 48000;
    AetherChain c;
    c.setSampleRate(sr);
    c.startWorker();

    ParamData d = transparentParams();
    d.set(kFlagEq, true);
    d.eqMode = static_cast<int32_t>(EqMode::FirLinear);
    d.numBands = 1;
    d.bands[0] = { static_cast<int32_t>(EqBandType::Peaking), 1000.0f, 6.0f, 1.0f };
    c.applyParamsRT(d);
    CHECK(c.waitIdle(), "háttérszál elkészítette a FIR-t");

    std::vector<float> l(sr), r(sr);
    auto run = [&](double f) {
        for (size_t i = 0; i < l.size(); ++i) l[i] = r[i] = static_cast<float>(0.1 * std::sin(2 * kPi * f * i / sr));
        c.process(l.data(), r.data(), l.size());
    };
    run(1000);
    CHECK(c.firEqActive() && c.eqLatencySamples() == 4096, "lineáris FIR aktív, késleltetés %zu minta (%.1f ms)",
          c.eqLatencySamples(), c.eqLatencySamples() * 1000.0 / sr);
    const double g = 20 * std::log10(toneAmp(l, 24000, 1000, sr) / 0.1);
    CHECK(std::abs(g - 6.0) < 0.2, "1 kHz +6 dB FIR-rel mérve: %.2f dB", g);

    // Vissza IIR-re: a FIR leáll, az IIR veszi át
    d.eqMode = static_cast<int32_t>(EqMode::Iir);
    c.applyParamsRT(d);
    c.waitIdle();
    run(1000);
    const double g2 = 20 * std::log10(toneAmp(l, 24000, 1000, sr) / 0.1);
    CHECK(!c.firEqActive() && std::abs(g2 - 6.0) < 0.1, "IIR módban: %.2f dB, FIR kikapcsolva", g2);

    // Konvolver: késleltetett impulzus IR (eredeti fázis) -> a jel 100 mintával később jelenik meg
    std::vector<float> irv(1000, 0.0f);
    irv[100] = 0.5f;
    c.submitIr(IrData::fromInterleaved(irv.data(), 1000, 1, 48000, "teszt.wav"));
    d.set(kFlagEq, false);
    d.set(kFlagConvolver, true);
    d.convPhaseMode = 1;
    d.convGainDb = 0.0f;
    c.applyParamsRT(d);
    c.waitIdle();
    std::fill(l.begin(), l.end(), 0.0f);
    std::fill(r.begin(), r.end(), 0.0f);
    c.process(l.data(), r.data(), 2048); // keresztúsztatás lefut
    std::fill(l.begin(), l.end(), 0.0f);
    l[0] = 1.0f;
    c.process(l.data(), r.data(), 2048);
    size_t pk = 0;
    for (size_t i = 0; i < 2048; ++i) if (std::abs(l[i]) > std::abs(l[pk])) pk = i;
    CHECK(c.convolverActive() && pk == 100 && std::abs(l[pk] - 1.0f) < 1e-3,
          "konvolver: impulzus a %zu. mintán, szint %.3f (normalizált 0 dB csúcs)", pk, l[pk]);

    // Minimálfázisú átalakítás: a késleltetés eltűnik
    d.convPhaseMode = 0;
    c.applyParamsRT(d);
    c.waitIdle();
    std::fill(l.begin(), l.end(), 0.0f);
    c.process(l.data(), r.data(), 2048);
    std::fill(l.begin(), l.end(), 0.0f);
    l[0] = 1.0f;
    c.process(l.data(), r.data(), 2048);
    pk = 0;
    for (size_t i = 0; i < 2048; ++i) if (std::abs(l[i]) > std::abs(l[pk])) pk = i;
    CHECK(pk < 3, "minimálfázisúvá alakított IR csúcsa a %zu. mintán (előtte 100)", pk);

    // Kikapcsolás: a lánc újra átlátszó
    d.set(kFlagConvolver, false);
    c.applyParamsRT(d);
    c.waitIdle();
    c.process(l.data(), r.data(), 2048);
    std::vector<float> t(4096), t0;
    for (size_t i = 0; i < t.size(); ++i) t[i] = static_cast<float>(std::sin(i * 0.01));
    t0 = t;
    std::vector<float> tr = t;
    c.process(t.data(), tr.data(), t.size());
    double diff = 0;
    for (size_t i = 0; i < t.size(); ++i) diff = std::max(diff, static_cast<double>(std::abs(t[i] - t0[i])));
    CHECK(!c.convolverActive() && diff == 0.0, "minden kikapcsolva -> bit-pontosan átlátszó (eltérés %.1e)", diff);

    // Limiter a láncban: -6 dB plafon, 0.9 amplitúdójú szinusz
    {
        ParamData ld = transparentParams();
        ld.limiterCeilingDb = -6.0f;
        ld.set(kFlagLimiter, true);
        c.applyParamsRT(ld);
        std::vector<float> a(9600), b(9600);
        for (size_t i = 0; i < a.size(); ++i) a[i] = b[i] = static_cast<float>(0.9 * std::sin(2 * kPi * 440 * i / sr));
        c.process(a.data(), b.data(), a.size());
        float mx = 0;
        for (size_t i = 4800; i < a.size(); ++i) mx = std::max(mx, std::abs(a[i]));
        CHECK(20 * std::log10(mx) <= -5.9 && c.gainReductionDb() < -4.5, "limiter: csúcs %.2f dBFS (plafon -6), GR %.1f dB",
              20 * std::log10(mx), c.gainReductionDb());
        c.applyParamsRT(transparentParams());
    }

    // Újramintavételezés: 96 kHz-es IR 48 kHz-re
    std::vector<float> s96(9600);
    for (size_t i = 0; i < s96.size(); ++i) s96[i] = static_cast<float>(std::sin(2 * kPi * 1000 * i / 96000.0));
    auto s48 = resampleIr(s96, 96000, 48000);
    double err = 0;
    for (size_t i = 200; i < s48.size() - 200; ++i) err = std::max(err, std::abs(s48[i] - std::sin(2 * kPi * 1000 * i / 48000.0)));
    CHECK(s48.size() == 4800 && err < 1e-3, "IR újramintavételezés 96k -> 48k: %zu minta, hiba %.1e", s48.size(), err);
    c.stopWorker();
}

static void testTruePeakAndMore() {
    std::printf("[5] True peak limiter, crossfeed ITD, EQ decramp, túlmintavételező\n");
    const double sr = 48000;
    // Mintaközi csúcs: fs/4 szinusz 45°-os fázissal -> a minták 0.707*A-n, a valódi csúcs A
    auto runLim = [&](int os, double& gr, double& truePeakDb) {
        TruePeakLimiter lim;
        lim.prepare(sr);
        lim.enabled = true;
        lim.configure(-3.0, 50.0, 5.0, os);
        std::vector<float> l(9600), r(9600);
        for (size_t i = 0; i < l.size(); ++i) l[i] = r[i] = static_cast<float>(0.9 * std::sin(kPi / 2 * i + kPi / 4));
        lim.process(l.data(), r.data(), l.size());
        gr = lim.gainReductionDb();
        double mx = 0;
        for (size_t i = 4800; i < l.size(); ++i) mx = std::max(mx, static_cast<double>(std::abs(l[i])));
        truePeakDb = 20 * std::log10(mx * std::sqrt(2.0)); // fs/4 szinusz: valódi csúcs = minta-csúcs * sqrt(2)
    };
    double gr1, tp1, gr4, tp4;
    runLim(1, gr1, tp1);
    runLim(4, gr4, tp4);
    CHECK(tp1 > -3.0 + 2.0, "1x (mintaérték) detektor: a valódi csúcs %.2f dBTP, túllépi a -3 dB plafont (várt viselkedés)", tp1);
    CHECK(tp4 <= -2.8 && gr4 < -1.5, "4x true peak detektor: valódi csúcs %.2f dBTP (plafon -3), GR %.2f dB", tp4, gr4);

    // Lookahead: hirtelen 20 dB-es ugrás -> a kimenet sosem lépi túl a plafont, és nincs kemény vágás
    {
        TruePeakLimiter lim;
        lim.prepare(sr);
        lim.enabled = true;
        lim.configure(-6.0, 80.0, 5.0, 4);
        std::vector<float> l(24000), r(24000);
        for (size_t i = 0; i < l.size(); ++i) {
            const double a = i < 12000 ? 0.05 : 0.95;
            l[i] = r[i] = static_cast<float>(a * std::sin(2 * kPi * 997 * i / sr));
        }
        lim.process(l.data(), r.data(), l.size());
        const float ceil = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
        size_t atCeil = 0;
        float mx = 0;
        for (float v : l) { mx = std::max(mx, std::abs(v)); if (std::abs(v) >= ceil * 0.9999f) ++atCeil; }
        CHECK(mx <= ceil + 1e-6f && atCeil < 5, "lookahead 5 ms: csúcs %.2f dBFS (plafon -6), plafonra vágott minta: %zu", 20 * std::log10(mx), atCeil);
        // késleltetés = lookahead + 3 minta
        TruePeakLimiter l2;
        l2.prepare(sr);
        l2.enabled = true;
        l2.configure(0.0, 50.0, 2.0, 4);
        std::vector<float> a(512, 0.0f), b(512, 0.0f);
        a[0] = b[0] = 0.5f;
        l2.process(a.data(), b.data(), a.size());
        size_t pk = 0;
        for (size_t i = 0; i < a.size(); ++i) if (std::abs(a[i]) > std::abs(a[pk])) pk = i;
        CHECK(pk == l2.latencySamples() && pk == 96 + 3, "késleltetés: %zu minta (2 ms lookahead + 3)", pk);
    }

    // Crossfeed ITD: az áthalló jel késleltetve érkezik
    {
        CrossfeedProcessor cf(sr);
        cf.prepareDelay();
        cf.delayMs = 0.5;
        cf.updateCoefficients();
        std::vector<float> l(256, 0.0f), r(256, 0.0f);
        l[0] = 1.0f;
        cf.processBlock(l.data(), r.data(), l.size());
        size_t first = 0;
        while (first < r.size() && std::abs(r[first]) < 1e-7f) ++first;
        CHECK(first == 24, "ITD 0,5 ms: a jobb csatornán az áthallás a %zu. mintánál indul (48 kHz: 24)", first);
    }

    // Decramp: 15 kHz-es +6 dB magas polc 48 kHz-en; referencia = 768 kHz-re tervezett (közel analóg) szűrő
    {
        BiquadFilter ref(FilterType::HighShelf, 15000, 0.71, 6.0, 768000);
        const double refDb = 20 * std::log10(std::abs(ref.evaluateTransferFunction(19000)));
        double err[2];
        int idx = 0;
        for (int dc : { 1, 4 }) {
            AetherChain c;
            c.setSampleRate(sr);
            ParamData d = transparentParams();
            d.set(kFlagEq, true);
            d.eqDecramp = dc;
            d.numBands = 1;
            d.bands[0] = { static_cast<int32_t>(EqBandType::HighShelf), 15000.0f, 6.0f, 0.71f };
            c.applyParamsRT(d);
            std::vector<float> l(24000), r(24000);
            for (size_t i = 0; i < l.size(); ++i) l[i] = r[i] = static_cast<float>(0.1 * std::sin(2 * kPi * 19000 * i / sr));
            c.process(l.data(), r.data(), l.size());
            err[idx++] = 20 * std::log10(toneAmp(l, 12000, 19000, sr) / 0.1) - refDb;
        }
        CHECK(std::abs(err[1]) < 0.25 && std::abs(err[0]) > std::abs(err[1]),
              "decramp: 19 kHz-en eltérés az analóg menettől: 1x %.2f dB, 4x %.2f dB", err[0], err[1]);
    }

    // Túlmintavételező: sík átvitel a hallható sávban
    {
        Oversampler os;
        double worst = 0;
        for (double f : { 100.0, 1000.0, 10000.0, 18000.0, 19500.0 }) {
            std::vector<float> l(24000), r(24000);
            for (size_t i = 0; i < l.size(); ++i) l[i] = r[i] = static_cast<float>(0.1 * std::sin(2 * kPi * f * i / sr));
            os.reset();
            os.process(l.data(), r.data(), l.size(), 4, [](float*, float*, size_t) {});
            worst = std::max(worst, std::abs(20 * std::log10(toneAmp(l, 12000, f, sr) / 0.1)));
        }
        CHECK(worst < 0.1, "4x túlmintavételező: max. eltérés 100 Hz - 19,5 kHz: %.3f dB, késleltetés %.1f minta",
              worst, os.latencySamples(4));
    }
}

int main() {
    std::printf("========================================================\n");
    std::printf(" AetherAudio DSP modulteszt\n");
    std::printf("========================================================\n");
    testConvolver();
    testFirDesign();
    testBass();
    testChain();
    testTruePeakAndMore();
    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
