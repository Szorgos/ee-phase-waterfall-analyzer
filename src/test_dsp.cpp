// DSP modulteszt: az EasyEffects-szel egyező modulok (LSP EQ és limiter, Calf Bass Enhancer, bs2b crossfeed,
// zita-convolver) és a teljes lánc a háttérszállal.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "aether_chain.hpp"

using namespace ee_analyzer;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; std::printf("  [HIBA] " __VA_ARGS__); } else { std::printf("  [OK]   " __VA_ARGS__); } std::printf("\n"); } while (0)

static constexpr double kPi = 3.14159265358979323846;

/** Goertzel: adott frekvencia amplitúdója a jelben (Hann ablakkal). */
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

/** Feldolgozás változó blokkokban (mint egy hoszt). */
template <class F>
static void runBlocks(F&& fn, std::vector<float>& L, std::vector<float>& R, size_t block) {
    for (size_t off = 0; off < L.size(); off += block) {
        const size_t m = std::min(block, L.size() - off);
        fn(L.data() + off, R.data() + off, m);
    }
}

static ParamData bellParams(int32_t mode, int32_t eqMode = 0, int32_t decramp = 0) {
    ParamData d = transparentParams();
    d.set(kFlagEq, true);
    d.numBands = 1;
    d.bands[0].type = static_cast<int32_t>(EqBandType::Bell);
    d.bands[0].mode = mode;
    d.bands[0].freq = 1000.0f;
    d.bands[0].gainDb = 6.0f;
    d.bands[0].q = 1.0f;
    d.eqMode = eqMode;
    d.eqDecramp = decramp;
    return d;
}

static void testEqualizer() {
    std::printf("[1] Equalizer (LSP para_equalizer)\n");
    const double sr = 48000.0;
    // Decramp: a 44,1 kHz-es referenciához viszonyított túlmintavételezés (calc_decramping)
    CHECK(ee::Equalizer::decrampFactor(0, 48000) == 1 && ee::Equalizer::decrampFactor(1, 48000) == 2 &&
              ee::Equalizer::decrampFactor(3, 48000) == 4 && ee::Equalizer::decrampFactor(4, 48000) == 6 &&
              ee::Equalizer::decrampFactor(5, 48000) == 8 && ee::Equalizer::decrampFactor(3, 96000) == 2 &&
              ee::Equalizer::decrampFactor(2, 44100) == 3 && ee::Equalizer::decrampFactor(5, 192000) == 2,
          "decramp tényezők (48k: 1/2/4/6/8, 96k x4 -> 2, 44,1k x3 -> 3, 192k x8 -> 2)");

    const char* modeNames[] = { "IIR", "FIR", "FFT", "SPM" };
    for (int eqMode = 0; eqMode < 4; ++eqMode) {
        for (int fm : { 0, 6 }) {
            ee::Equalizer eq;
            eq.setSampleRate(48000);
            eq.update(bellParams(fm, eqMode, eqMode <= 1 ? 3 : 0));
            std::vector<float> L = sine(1000.0, 0.1, 96000, sr), R = L;
            runBlocks([&](float* l, float* r, size_t m) { eq.process(l, r, m); }, L, R, 512);
            const double g = 20 * std::log10(toneAmp(L, 48000, 1000.0, sr) / 0.1);
            CHECK(std::abs(g - 6.0) < 0.25, "%s, %s: +6 dB harang 1 kHz-en mérve %.2f dB (késleltetés %zu minta)",
                  modeNames[eqMode], fm == 0 ? "RLC (BT)" : "APO (DR)", g, eq.latency());
        }
    }

    // Frekvenciamenet (a felület görbéje) = mért erősítés
    {
        ParamData d = bellParams(0);
        d.bands[0].type = static_cast<int32_t>(EqBandType::HiPass);
        d.bands[0].freq = 200.0f;
        d.bands[0].slope = 1; // x2
        float f[3] = { 50.0f, 1000.0f, 5000.0f }, mag[3], ph[3];
        ee::Equalizer::response(d, 48000, f, 3, mag, ph);
        ee::Equalizer eq;
        eq.setSampleRate(48000);
        eq.update(d);
        std::vector<float> L = sine(50.0, 0.1, 96000, sr), R = L;
        runBlocks([&](float* l, float* r, size_t m) { eq.process(l, r, m); }, L, R, 480);
        const double g = 20 * std::log10(toneAmp(L, 48000, 50.0, sr) / 0.1);
        CHECK(std::abs(mag[0] - g) < 0.5 && mag[0] < -30.0 && std::abs(mag[1]) < 0.5,
              "felüláteresztő 200 Hz x2: görbe 50 Hz-en %.1f dB, mérve %.1f dB, 1 kHz-en %.2f dB", mag[0], g, mag[1]);
    }

    // Sávszélességes típus (létra): a width paraméter adja a két sarokfrekvenciát (center / 2^(w/2), center * 2^(w/2));
    // a "Ladder-rej" a sávon kívül alkalmazza az erősítést
    {
        ParamData d = bellParams(0);
        d.bands[0].type = static_cast<int32_t>(EqBandType::LadderRej);
        d.bands[0].gainDb = -12.0f;
        d.bands[0].width = 2.0f; // 500 .. 2000 Hz
        float f[3] = { 100.0f, 1000.0f, 8000.0f }, mag[3], ph[3];
        ee::Equalizer::response(d, 48000, f, 3, mag, ph);
        CHECK(mag[0] < -10.0 && mag[2] < -10.0 && std::abs(mag[1]) < 2.0,
              "Ladder-rej 1 kHz ±1 oktáv, -12 dB: 100 Hz %.1f / 1 kHz %.1f / 8 kHz %.1f dB", mag[0], mag[1], mag[2]);
        d.bands[0].type = static_cast<int32_t>(EqBandType::LadderPass);
        ee::Equalizer::response(d, 48000, f, 3, mag, ph);
        CHECK(mag[1] < -10.0 && std::abs(mag[0]) < 2.0 && std::abs(mag[2]) < 2.0,
              "Ladder-pass ugyanígy: 100 Hz %.1f / 1 kHz %.1f / 8 kHz %.1f dB", mag[0], mag[1], mag[2]);
    }
}

static void testLimiter() {
    std::printf("[2] Limiter (LSP limiter_stereo)\n");
    const double sr = 48000.0;
    ee::Limiter lim;
    lim.setSampleRate(48000);
    ParamData d = transparentParams();
    d.limThresholdDb = -1.0f;
    d.limOversampling = 22; // True Peak / 24 bit
    lim.update(d);
    std::vector<float> L = sine(997.0, 2.0, 96000, sr), R = sine(997.0, 1.0, 96000, sr);
    runBlocks([&](float* l, float* r, size_t m) { lim.process(l, r, m); }, L, R, 480);
    float pk = 0;
    for (size_t i = 24000; i < L.size(); ++i) pk = std::max({ pk, std::abs(L[i]), std::abs(R[i]) });
    const double pkDb = 20 * std::log10(pk);
    CHECK(pkDb < -0.9 && pkDb > -1.6, "+6 dBFS szinusz -> csúcs %.2f dBFS (küszöb -1 dB)", pkDb);
    CHECK(lim.gainReductionDb() > 6.0, "erősítéscsökkentés %.1f dB", lim.gainReductionDb());
    // Stereo link 100%: a jobb csatorna ugyanannyit csillapodik
    const double rl = toneAmp(R, 48000, 997.0, sr) / toneAmp(L, 48000, 997.0, sr);
    CHECK(std::abs(20 * std::log10(rl) + 6.02) < 0.3, "100%%-os csatolás: R/L arány %.2f dB (várt -6,02)", 20 * std::log10(rl));
    CHECK(lim.latency() > 0, "késleltetés %zu minta (lookahead + túlmintavételezés)", lim.latency());

    // Gain boost: a küszöb 0 dBFS-re emelve
    d.limGainBoost = 1;
    lim.update(d);
    L = sine(997.0, 2.0, 96000, sr); R = L;
    runBlocks([&](float* l, float* r, size_t m) { lim.process(l, r, m); }, L, R, 480);
    pk = 0;
    for (size_t i = 24000; i < L.size(); ++i) pk = std::max(pk, std::abs(L[i]));
    CHECK(20 * std::log10(pk) > -0.6 && 20 * std::log10(pk) < 0.3, "gain boost: csúcs %.2f dBFS", 20 * std::log10(pk));

    // Csend alatt nincs csillapítás
    ee::Limiter lim2;
    lim2.setSampleRate(48000);
    d.limGainBoost = 0;
    lim2.update(d);
    std::vector<float> q = sine(440.0, 0.25, 48000, sr), q2 = q;
    std::vector<float> ql = q, qr = q;
    runBlocks([&](float* l, float* r, size_t m) { lim2.process(l, r, m); }, ql, qr, 256);
    const double g = 20 * std::log10(toneAmp(ql, 24000, 440.0, sr) / 0.25);
    CHECK(std::abs(g) < 0.05, "küszöb alatti jel érintetlen (%.3f dB)", g);
}

static void testCrossfeed() {
    std::printf("[3] Crossfeed (libbs2b)\n");
    const double sr = 48000.0;
    struct P { const char* name; float fcut, feed; };
    for (P p : { P{ "Bauer bs2b", 700.0f, 4.5f }, P{ "Jan Meier", 650.0f, 9.5f }, P{ "közeltéri stúdió", 600.0f, 3.5f } }) {
        ee::Crossfeed cf;
        cf.setSampleRate(48000);
        ParamData d = transparentParams();
        d.crossfeedFcut = p.fcut;
        d.crossfeedFeedDb = p.feed;
        cf.update(d);
        // Csak bal csatorna, 40 Hz: az áthallás szintje = feed
        std::vector<float> L = sine(40.0, 0.2, 96000, sr), R(96000, 0.0f);
        runBlocks([&](float* l, float* r, size_t m) { cf.process(l, r, m); }, L, R, 512);
        const double x = 20 * std::log10(toneAmp(R, 48000, 40.0, sr) / toneAmp(L, 48000, 40.0, sr));
        // Monó jel szintje mélyen változatlan
        cf.reset();
        std::vector<float> M = sine(40.0, 0.2, 96000, sr), M2 = M;
        runBlocks([&](float* l, float* r, size_t m) { cf.process(l, r, m); }, M, M2, 512);
        const double mono = 20 * std::log10(toneAmp(M, 48000, 40.0, sr) / 0.2);
        CHECK(std::abs(x + p.feed) < 0.6 && std::abs(mono) < 0.3,
              "%s (%.0f Hz / %.1f dB): áthallás 40 Hz-en %.2f dB, monó szint %.2f dB", p.name, p.fcut, p.feed, x, mono);
    }
}

static void testBass() {
    std::printf("[4] Bass Enhancer (Calf)\n");
    const double sr = 48000.0;
    ee::BassEnhancer be;
    be.setSampleRate(48000);
    ParamData d = transparentParams();
    d.bassAmountDb = 0.0f;   // EasyEffects alapértékek
    d.bassHarmonics = 8.5f;
    d.bassScopeHz = 100.0f;
    be.update(d);
    std::vector<float> L = sine(50.0, 0.3, 96000, sr), R = L;
    runBlocks([&](float* l, float* r, size_t m) { be.process(l, r, m); }, L, R, 512);
    const double h2 = toneAmp(L, 48000, 100.0, sr), h3 = toneAmp(L, 48000, 150.0, sr);
    CHECK(h2 > 1e-3 || h3 > 1e-3, "50 Hz -> felharmonikusok: 100 Hz %.1f dB, 150 Hz %.1f dB",
          20 * std::log10(h2 / 0.3 + 1e-12), 20 * std::log10(h3 / 0.3 + 1e-12));
    be.reset();
    std::vector<float> H = sine(3000.0, 0.3, 96000, sr), H2 = H;
    runBlocks([&](float* l, float* r, size_t m) { be.process(l, r, m); }, H, H2, 512);
    const double g = 20 * std::log10(toneAmp(H, 48000, 3000.0, sr) / 0.3);
    CHECK(std::abs(g) < 0.3, "3 kHz (a scope felett) szintje %.2f dB", g);
    // Listen: csak a generált felharmonikusok szólnak
    d.bassListen = 1;
    be.update(d);
    std::vector<float> Q = sine(3000.0, 0.3, 48000, sr), Q2 = Q;
    runBlocks([&](float* l, float* r, size_t m) { be.process(l, r, m); }, Q, Q2, 512);
    CHECK(toneAmp(Q, 24000, 3000.0, sr) < 0.01, "listen: a 3 kHz-es bemenet nem hallatszik");
}

static void testConvolver() {
    std::printf("[5] Convolver (zita-convolver)\n");
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.0f, 0.3f);
    std::vector<float> hl(5000), hr(5000);
    for (size_t i = 0; i < hl.size(); ++i) hl[i] = nd(rng) * std::exp(-static_cast<float>(i) / 900.0f);
    for (size_t i = 0; i < hr.size(); ++i) hr[i] = nd(rng) * std::exp(-static_cast<float>(i) / 500.0f);

    // Kernel: 100%-os szélesség és autogain nélkül változatlan
    ee::ConvKernel k = ee::prepareConvKernel(hl, hr, 48000, 48000, 100, false);
    CHECK(k.valid() && k.left == hl && k.right == hr, "kernel 100%% szélességgel, autogain nélkül változatlan");
    ee::ConvKernel k0 = ee::prepareConvKernel(hl, hr, 48000, 48000, 0, false);
    CHECK(std::abs(k0.left[100] - (hl[100] + hr[100])) < 1e-6, "0%% szélesség: L = LL + RR (monó)");
    ee::ConvKernel ka = ee::prepareConvKernel(hl, hr, 48000, 48000, 100, true);
    double pl = 0, pr = 0;
    for (size_t i = 0; i < ka.left.size(); ++i) { pl += ka.left[i] * ka.left[i]; pr += ka.right[i] * ka.right[i]; }
    CHECK(std::abs(std::max(pl, pr) - 1.0) < 1e-3 || std::max(pl, pr) < 1.0, "autogain: legnagyobb energia %.3f", std::max(pl, pr));
    ee::ConvKernel kr = ee::prepareConvKernel(hl, {}, 44100, 48000, 100, false);
    CHECK(kr.rate == 48000 && kr.left.size() > 5300 && kr.left == kr.right, "44,1 -> 48 kHz (SpeexDSP), monó -> sztereó: %zu minta",
          kr.left.size());

    const size_t N = 24000;
    std::vector<float> xl(N), xr(N);
    for (auto& v : xl) v = nd(rng);
    for (auto& v : xr) v = nd(rng);
    for (uint32_t q : { 256u, 480u }) {
        ee::ZitaInstance zi;
        const bool ok = zi.init(k, q);
        std::vector<float> yl = xl, yr = xr;
        if (ok) runBlocks([&](float* l, float* r, size_t m) { if (m == q) zi.process(l, r, m); else std::fill_n(l, m, 0.0f), std::fill_n(r, m, 0.0f); }, yl, yr, q);
        // Késleltetés keresése (kettőhatvány blokknál 0, különben a FIFO miatt több)
        size_t best = 0;
        double bestErr = 1e9;
        for (size_t lag = 0; lag <= 512; ++lag) {
            double err = 0;
            for (size_t n = 6000; n < 6200; ++n) {
                double rl = 0;
                for (size_t i = 0; i < hl.size() && i <= n - lag; ++i) rl += static_cast<double>(hl[i]) * xl[n - lag - i];
                err = std::max(err, std::abs(rl - yl[n]));
            }
            if (err < bestErr) { bestErr = err; best = lag; }
            if (err < 1e-4) break;
        }
        double maxErr = 0;
        for (size_t n = 8000; n < N - 1000; n += 13) {
            double rl = 0, rr = 0;
            for (size_t i = 0; i < hl.size() && i <= n - best; ++i) rl += static_cast<double>(hl[i]) * xl[n - best - i];
            for (size_t i = 0; i < hr.size() && i <= n - best; ++i) rr += static_cast<double>(hr[i]) * xr[n - best - i];
            maxErr = std::max({ maxErr, std::abs(rl - yl[n]), std::abs(rr - yr[n]) });
        }
        CHECK(ok && maxErr < 2e-4 && (q != 256 || best == 0),
              "blokk %u: eltérés a közvetlen konvolúciótól %.2e, késleltetés %zu minta", q, maxErr, best);
    }
}

static void testChain() {
    std::printf("[6] Teljes lánc (háttérszál, konvolver csere)\n");
    const double sr = 48000.0;
    AetherChain chain;
    chain.setSampleRate(sr);
    chain.startWorker();

    // Alapállapot: bit-pontosan átlátszó
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(-0.9f, 0.9f);
    std::vector<float> L(4800), R(4800);
    for (auto& v : L) v = u(rng);
    for (auto& v : R) v = u(rng);
    std::vector<float> L0 = L, R0 = R;
    runBlocks([&](float* l, float* r, size_t m) { chain.process(l, r, m); }, L, R, 480);
    CHECK(L == L0 && R == R0, "kikapcsolt modulok: bit-pontosan érintetlen");

    // EQ + preamp
    ParamData d = bellParams(0);
    d.preampDb = -6.0f;
    chain.applyParamsRT(d);
    std::vector<float> s = sine(1000.0, 0.1, 48000, sr), s2 = s;
    runBlocks([&](float* l, float* r, size_t m) { chain.process(l, r, m); }, s, s2, 480);
    const double g = 20 * std::log10(toneAmp(s, 24000, 1000.0, sr) / 0.1);
    CHECK(std::abs(g) < 0.3, "-6 dB preamp + 6 dB harang: %.2f dB", g);

    // Konvolver: késleltetett, felezett impulzus
    IrData ir;
    ir.sampleRate = 48000;
    ir.name = "teszt";
    ir.left.assign(2000, 0.0f);
    ir.left[10] = 0.5f;
    chain.submitIr(ir);
    d = transparentParams();
    d.set(kFlagConvolver, true);
    d.convAutogain = 0;
    chain.applyParamsRT(d);
    std::vector<float> z(480, 0.0f), z2(480, 0.0f);
    chain.process(z.data(), z2.data(), z.size()); // a blokkméret átadása a háttérszálnak
    const bool idle = chain.waitIdle();
    for (int i = 0; i < 3; ++i) chain.process(z.data(), z2.data(), z.size()); // átvétel + keresztúsztatás
    std::vector<float> x(4800, 0.0f), x2(4800, 0.0f);
    x[1000] = 1.0f;
    x2[1000] = 1.0f;
    runBlocks([&](float* l, float* r, size_t m) { chain.process(l, r, m); }, x, x2, 480);
    // 480 nem kettő hatványa: 256-os zita blokk + FIFO -> a válasz késhet
    size_t at = 0;
    for (size_t i = 0; i < x.size(); ++i) if (std::abs(x[i]) > 0.25f) { at = i; break; }
    CHECK(idle && chain.convolverActive() && std::abs(x[at] - 0.5f) < 1e-4 && std::abs(x2[at] - 0.5f) < 1e-4 && at >= 1010,
          "impulzus: 0,5 amplitúdó a %zu. mintán (IR késleltetés 10 + blokkillesztés %zu)", at, at - 1010);

    // Dry / wet keverés (EasyEffects: out = wet * konvolvált + dry * bemenet)
    d.convDryDb = 0.0f;
    d.convWetDb = -100.0f;
    chain.applyParamsRT(d);
    std::vector<float> w = sine(500.0, 0.2, 9600, sr), w2 = w;
    runBlocks([&](float* l, float* r, size_t m) { chain.process(l, r, m); }, w, w2, 480);
    const double gd = 20 * std::log10(toneAmp(w, 4800, 500.0, sr) / 0.2);
    CHECK(std::abs(gd) < 0.05, "csak dry: %.3f dB", gd);

    // Kikapcsolás
    d.set(kFlagConvolver, false);
    chain.applyParamsRT(d);
    chain.waitIdle();
    for (int i = 0; i < 3; ++i) chain.process(z.data(), z2.data(), z.size());
    CHECK(!chain.convolverActive(), "konvolver kikapcsolva");

    // Limiter a láncban
    d = transparentParams();
    d.set(kFlagLimiter, true);
    chain.applyParamsRT(d);
    std::vector<float> h = sine(997.0, 1.5, 48000, sr), h2 = h;
    runBlocks([&](float* l, float* r, size_t m) { chain.process(l, r, m); }, h, h2, 480);
    float pk = 0;
    for (size_t i = 24000; i < h.size(); ++i) pk = std::max(pk, std::abs(h[i]));
    CHECK(20 * std::log10(pk) < -0.9 && chain.gainReductionDb() < -3.0, "limiter: csúcs %.2f dBFS, GR %.1f dB",
          20 * std::log10(pk), chain.gainReductionDb());
    chain.stopWorker();
}

/** Legnagyobb második differencia (ugrás / kattanás mértéke) a jelben. */
static double maxJump(const std::vector<float>& x, size_t from, size_t to) {
    double mx = 0;
    for (size_t i = std::max<size_t>(from, 2); i < to; ++i) mx = std::max(mx, std::abs(double(x[i]) - 2.0 * x[i - 1] + x[i - 2]));
    return mx;
}

static void testDeclick(size_t kBlk) {
    std::printf("[7] Kattanásmentes váltások (%zu mintás blokkok), preamp csak bekapcsolt EQ-val\n", kBlk);
    const double sr = 48000.0;
    AetherChain chain;
    chain.setSampleRate(sr);
    // 1 kHz, 0,5 amplitúdó: a szinusz második differenciája ~0,0086; egy ugrás ennél nagyságrenddel nagyobb
    const size_t seg = 12000;
    std::vector<float> L = sine(1000.0, 0.5, seg * 23, sr), R = L;
    ParamData d = transparentParams();
    size_t pos = 0;
    double worst = 0;
    const char* worstAt = "";
    auto run = [&](const char* what) {
        std::vector<float> l(L.begin() + pos, L.begin() + pos + seg), r(R.begin() + pos, R.begin() + pos + seg);
        chain.applyParamsRT(d);
        runBlocks([&](float* a, float* b, size_t m) { chain.process(a, b, m); }, l, r, kBlk);
        const double j = maxJump(l, 0, seg);
        if (j > worst) { worst = j; worstAt = what; }
        std::copy(l.begin(), l.end(), L.begin() + pos);
        pos += seg;
    };
    run("alap");
    d = bellParams(0, 0, 3); d.preampDb = -6.0f; run("EQ be (IIR, decramp x4)");
    d.eqMode = 1; run("FIR mód");
    d.eqMode = 2; run("FFT mód");
    d.eqMode = 3; run("SPM mód");
    d.eqMode = 2; run("SPM -> FFT");
    d.eqMode = 1; run("FFT -> FIR");
    d.eqMode = 3; run("FIR -> SPM");
    d.eqMode = 0; run("IIR mód");
    for (int dc : {0, 1, 2, 4, 5, 3}) { d.eqDecramp = dc; run("decramp váltás"); }
    d.eqMode = 1; d.eqDecramp = 0; run("FIR, decramp ki");
    d.eqDecramp = 5; run("FIR, decramp x8");
    d.eqMode = 0; d.eqDecramp = 3; run("IIR, decramp x4");
    d.set(kFlagLimiter, true); run("limiter be");
    d.limOversampling = 0; run("limiter túlmintavételezés ki");
    d.set(kFlagLimiter, false); d.set(kFlagBass, true); run("limiter ki, bass be");
    d.set(kFlagBass, false); d.set(kFlagEq, false); run("EQ ki");
    CHECK(worst < 0.03, "modul- és módváltások ugrás nélkül (legnagyobb második differencia %.4f, %s)", worst, worstAt);

    // FIR módú, változatlanul futó EQ mellett egy másik modul kapcsolása: nem kell kivárni az EQ
    // ~128 ms-os késleltetését, csak a le-fel úsztatás (és a blokk maradéka) néma
    {
        AetherChain c2;
        c2.setSampleRate(sr);
        ParamData f = bellParams(0, 1, 3);
        f.preampDb = -6.0f;   // +6 dB harang: ne érje el a ±1,0 vágást
        std::vector<float> a = sine(1000.0, 0.5, 48000, sr), b = a;
        c2.applyParamsRT(f);
        runBlocks([&](float* x, float* y, size_t m) { c2.process(x, y, m); }, a, b, kBlk);
        f.set(kFlagBass, true);
        std::vector<float> a2 = sine(1000.0, 0.5, 24000, sr), b2 = a2;
        c2.applyParamsRT(f);
        runBlocks([&](float* x, float* y, size_t m) { c2.process(x, y, m); }, a2, b2, kBlk);
        size_t run = 0, longest = 0;
        for (float v : a2) { run = (v == 0.0f) ? run + 1 : 0; longest = std::max(longest, run); }
        CHECK(longest < kBlk + 64 && maxJump(a2, 0, a2.size()) < 0.03,
              "FIR EQ mellett bass be: néma szakasz %zu minta (blokk %zu), ugrás nélkül", longest, kBlk);
    }

    // Kikapcsolt EQ: a preamp sem hat (bypass = érintetlen jel)
    d = transparentParams();
    d.preampDb = -6.5f;
    std::vector<float> s = sine(1000.0, 0.1, 24000, sr), s2 = s;
    chain.applyParamsRT(d);
    runBlocks([&](float* a, float* b, size_t m) { chain.process(a, b, m); }, s, s2, 480);
    const double g = 20 * std::log10(toneAmp(s, 12000, 1000.0, sr) / 0.1);
    CHECK(std::abs(g) < 0.01, "EQ kikapcsolva, preamp -6,5 dB: %.3f dB (érintetlen)", g);
}

int main() {
    std::printf("========================================================\n");
    std::printf(" Aqulify DSP modulteszt (EasyEffects könyvtárak)\n");
    std::printf("========================================================\n");
    testEqualizer();
    testLimiter();
    testCrossfeed();
    testBass();
    testConvolver();
    testChain();
    testDeclick(480);
    testDeclick(1024);
    testDeclick(256);
    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
