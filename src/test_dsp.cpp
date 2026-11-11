// DSP modulteszt: az EasyEffects-szel egyező modulok (LSP EQ és limiter, Calf Bass Enhancer, bs2b crossfeed,
// zita-convolver) és a teljes lánc a háttérszállal.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include "aether_chain.hpp"
#include "ebur128.h"
#include "pcm_convert.hpp"
#include <chrono>

#include <generated/iso226/defs.h>
#include <generated/iso226/iso226-2003.h>
#include <generated/iso226/iso226-2023.h>
#include <generated/iso226/fletcher_munson.h>
#include <generated/iso226/robinson_dadson.h>

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

    // Az x6 / x8 megszűnt (a mély, keskeny sávokat rontotta, T06): a mentett 4 / 5 x4-ként töltődik be
    {
        ParamData d = transparentParams();
        d.eqDecramp = 5;
        sanitizeParams(d);
        ParamData d4 = transparentParams();
        d4.eqDecramp = 4;
        sanitizeParams(d4);
        CHECK(d.eqDecramp == 3 && d4.eqDecramp == 3 && transparentParams().eqDecramp == 0,
              "decramp: az alapérték Off, a régi x6 / x8 beállítás x4 lesz");
    }

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
    {
        // APO (DR) mély polc decramp x4 mellett: a grafikon görbéje és csoportkésleltetése mélyen is sima
        // (float számításnál a cos(w) lépcsőzetes volt: kb. 0,8 ms-os ugrások 10-30 Hz között)
        for (int decr : { 3 }) {
            ParamData d = bellParams(0);
            d.eqDecramp = decr;
            d.bands[0].type = static_cast<int32_t>(EqBandType::LoShelf);
            d.bands[0].mode = static_cast<int32_t>(EqFilterMode::ApoDr);
            d.bands[0].freq = 105.0f; d.bands[0].gainDb = 6.0f; d.bands[0].q = 0.7f;
            constexpr size_t N = 64;
            float f[N], mag[N], ph[N];
            for (size_t i = 0; i < N; ++i) f[i] = static_cast<float>(10.0 * std::pow(10.0, i / double(N - 1)));   // 10 .. 100 Hz
            ee::Equalizer::response(d, 48000, f, N, mag, ph);
            double gd[N] = {}, maxJump = 0.0;
            for (size_t i = 0; i + 1 < N; ++i) {
                double dp = ph[i + 1] - ph[i];
                while (dp > 180) dp -= 360;
                while (dp < -180) dp += 360;
                gd[i] = -(dp / 360.0) / (f[i + 1] - f[i]) * 1000.0;
                if (i >= 2) maxJump = std::max(maxJump, std::abs(gd[i] - 2.0 * gd[i - 1] + gd[i - 2]));   // görbület: zajra érzékeny
            }
            CHECK(maxJump < 0.05, "APO mély polc, decramp %s: a csoportkésleltetés sima 10-100 Hz között (legnagyobb törés %.3f ms)",
                  "x4", maxJump);
        }
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
    for (int dc : {0, 1, 2, 3}) { d.eqDecramp = dc; run("decramp váltás"); }
    d.eqMode = 1; d.eqDecramp = 0; run("FIR, decramp ki");
    d.eqDecramp = 3; run("FIR, decramp x4");
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

// ------------------------------------------------------------------ Magic Touch

/** Rózsaszín zaj (Paul Kellet szűrője), determinisztikus. */
static std::vector<float> pinkNoise(size_t n, unsigned seed, double amp) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0.0, 1.0);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    std::vector<float> o(n);
    for (size_t i = 0; i < n; ++i) {
        const double w = g(rng);
        b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759; b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856; b4 = 0.55000 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.0168980;
        o[i] = static_cast<float>(amp * 0.11 * (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362));
        b6 = w * 0.115926;
    }
    return o;
}

struct MagicRun {
    magic::MagicDiag last;
    float maxCut = 0, maxAir = 0, maxDeess = 0, maxWidth = 0, maxMakeup = 0, maxMono = 0;
    float maxTrans = 0, maxTransNow = 0, maxUp = 0, maxExc = 0, maxWarm = 0, maxBody = 0, maxPres = 0, maxLevel = 0;
    float minAir = 0;
    double firstSpeechS = -1;   // mikor lett először beszéd a profil (s)
    float maxHum = 0, maxNr = 0;
};

/** Futtatás 480 mintás blokkokban; az elemzés kb. 30 ms-onként (mint a háttérszál). */
static MagicRun runMagic(magic::MagicTouch& mt, std::vector<float>& L, std::vector<float>& R) {
    MagicRun r;
    size_t blk = 0;
    runBlocks([&](float* a, float* b, size_t m) {
        mt.process(a, b, m);
        if (++blk % 3 == 0) {
            mt.analyze();
            magic::MagicDiag d;
            if (mt.diag(d)) {
                r.last = d;
                r.maxCut = std::max(r.maxCut, -d.lowMidCutDb);
                r.maxAir = std::max(r.maxAir, d.airGainDb);
                r.maxDeess = std::max(r.maxDeess, d.deessNowDb);
                r.maxWidth = std::max(r.maxWidth, d.widthGainDb);
                r.maxMakeup = std::max(r.maxMakeup, std::fabs(d.makeupDb));
                r.maxMono = std::max(r.maxMono, d.monoBass);
                r.maxTrans = std::max(r.maxTrans, d.transientDb);
                r.maxTransNow = std::max(r.maxTransNow, d.transientNowDb);
                r.maxUp = std::max(r.maxUp, std::max(d.upwardDb, d.upwardNowDb));
                r.maxExc = std::max(r.maxExc, d.exciterMix);
                r.maxWarm = std::max(r.maxWarm, d.warmthMix);
                r.maxBody = std::max(r.maxBody, d.bodyDb);
                r.maxPres = std::max(r.maxPres, d.presenceDb);
                r.maxLevel = std::max(r.maxLevel, std::fabs(d.levelDb));
                r.minAir = std::min(r.minAir, d.airGainDb);
                r.maxHum = std::max(r.maxHum, d.humCutDb);
                r.maxNr = std::max(r.maxNr, std::max(d.nrDb, d.nrNowDb));
                if (d.content == magic::kProfSpeech && r.firstSpeechS < 0) r.firstSpeechS = blk * 480.0 / 48000.0;
            }
        }
    }, L, R, 480);
    return r;
}

static double rmsDb(const std::vector<float>& x, size_t from) {
    double e = 0;
    for (size_t i = from; i < x.size(); ++i) e += static_cast<double>(x[i]) * x[i];
    return 10.0 * std::log10(e / static_cast<double>(x.size() - from) + 1e-30);
}

/** A korlátok ellenőrzése; B: a „Bátor” tartomány mértéke (0 = legfeljebb 100 %, 1 = 200 %), ahogy a decide() bővíti őket. */
static void checkLimits(const MagicRun& r, const char* what, float B = 0.0f) {
    const float k = 1.0f + B;
    CHECK(r.maxCut <= magic::kMaxLowMidCutDb * (1.0f + 0.67f * B) + 1e-3f && r.maxAir <= magic::kMaxAirDb * k + 1e-3f &&
          r.maxDeess <= magic::kMaxDeessDb * (1.0f + 0.5f * B) + 1e-3f && r.maxWidth <= magic::kMaxWidthDb * k + 1e-3f &&
          r.maxMakeup <= magic::kMaxMakeupDb * (1.0f + 0.5f * B) + 1e-3f && r.maxMono <= 1.0f &&
          r.maxTrans <= magic::kMaxTransientDb * k + 1e-3f && r.maxTransNow <= magic::kMaxTransientDb * k + 1e-3f &&
          r.maxUp <= magic::kMaxUpwardDb * k + 1e-3f && r.maxExc <= magic::kMaxExciterMix * (1.0f + 0.67f * B) + 1e-4f &&
          r.maxWarm <= magic::kMaxWarmthMix * k + 1e-4f && r.maxBody <= magic::kMaxBodyDb * k + 1e-3f &&
          r.maxPres <= magic::kMaxPresenceDb * k + 1e-3f && r.maxLevel <= magic::kMaxLevelDb + 1e-3f && r.minAir >= -3.0f * k &&
          r.maxHum <= magic::kMaxHumCutDb + 1e-3f && r.maxNr <= magic::kMaxNrDb + 1e-3f,
          "Magic Touch korlátok (%s): vágás %.2f, levegő %.2f, de-esser %.2f, tágítás %.2f, kiegyenlítés %.2f dB, "
          "tranziens %.2f dB, emelés %.2f dB, exciter %.0f %%, meleg %.0f %%, szintkiegyenlítés %.2f dB",
          what, r.maxCut, r.maxAir, r.maxDeess, r.maxWidth, r.maxMakeup, r.maxTransNow, r.maxUp, r.maxExc * 100.0f,
          r.maxWarm * 100.0f, r.maxLevel);
}

/**
 * Beszédszerű jel: zöngés szótagok (impulzussor, f0 100–160 Hz, három formáns), réshangok (zaj 4–8 kHz),
 * szótagok közötti rések és 3–6 szótagonként szünet (determinisztikus).
 */
static std::vector<float> synthSpeech(size_t n, unsigned seed, double sr, double amp) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::normal_distribution<double> g(0.0, 1.0);
    static const double formants[5][3] = { {700, 1220, 2600}, {530, 1840, 2480}, {270, 2290, 3010}, {570, 840, 2410}, {300, 870, 2240} };
    std::vector<float> o(n, 0.0f);
    magic::BiquadD fr[3], fric1, fric2;
    magic::designHighpass(fric1, sr, 4000.0, 0.7071);
    magic::designLowpass(fric2, sr, 8000.0, 0.7071);
    size_t pos = 0, sinceWord = 0, wordLen = 4;
    double phase = 0, glot = 0;
    while (pos < n) {
        const bool fricative = u(rng) < 0.25;
        const size_t len = static_cast<size_t>(sr * (fricative ? 0.06 + 0.06 * u(rng) : 0.12 + 0.14 * u(rng)));
        const int v = static_cast<int>(u(rng) * 5) % 5;
        // a formánsok sávszűrői (RBJ, állandó 0 dB-es csúcs)
        for (int k = 0; k < 3; ++k) {
            const double w = 2 * magic::kPi * formants[v][k] / sr, al = std::sin(w) / (2 * 6.0);
            fr[k].set(al, 0, -al, 1 + al, -2 * std::cos(w), 1 - al);
        }
        const double f0 = 100.0 + 60.0 * u(rng);
        for (size_t i = 0; i < len && pos < n; ++i, ++pos) {
            const double t = static_cast<double>(i) / static_cast<double>(len);
            const double env = std::sin(magic::kPi * t);   // szótag-burkoló
            double y;
            if (fricative) {
                y = 0.5 * fric2.process(fric1.process(g(rng)));
            } else {
                phase += (f0 * (1.0 - 0.15 * t)) / sr;
                const double pulse = phase >= 1.0 ? 1.0 : 0.0;
                if (phase >= 1.0) phase -= 1.0;
                glot = 0.97 * glot + pulse + 0.02 * g(rng);   // tompuló impulzussor
                y = fr[0].process(glot) + 0.5 * fr[1].process(glot) + 0.25 * fr[2].process(glot);
            }
            o[pos] = static_cast<float>(amp * env * y);
        }
        // rés a szótagok között, szünet a szavak (szócsoportok) után
        const bool wordEnd = ++sinceWord >= wordLen;
        if (wordEnd) { sinceWord = 0; wordLen = 3 + static_cast<size_t>(u(rng) * 4); }
        pos += static_cast<size_t>(sr * (wordEnd ? 0.2 + 0.25 * u(rng) : 0.03 + 0.05 * u(rng)));
    }
    return o;
}

/** K-súlyozás nélküli szint (dB) egy szakaszon, sztereó átlag. */
static double segDb(const std::vector<float>& L, const std::vector<float>& R, size_t a, size_t b) {
    double e = 0;
    for (size_t i = a; i < b; ++i) e += 0.5 * (static_cast<double>(L[i]) * L[i] + static_cast<double>(R[i]) * R[i]);
    return 10.0 * std::log10(e / static_cast<double>(b - a) + 1e-30);
}

/** Hangosság (ITU-R BS.1770 K-súlyozás, sztereó), LUFS, a `from` mintától. */
static double lufs(const std::vector<float>& L, const std::vector<float>& R, size_t from, double sr) {
    double e = 0;
    const std::vector<float>* ch[2] = { &L, &R };
    for (int c = 0; c < 2; ++c) {
        magic::BiquadD a, b;
        magic::designKShelf(a, sr);
        magic::designKHighpass(b, sr);
        for (size_t i = 0; i < ch[c]->size(); ++i) {
            const double y = b.process(a.process((*ch[c])[i]));
            if (i >= from) e += y * y;
        }
    }
    return -0.691 + 10.0 * std::log10(e / static_cast<double>(L.size() - from) + 1e-30);
}

/** Sávenergia (dB) egy tartományban: három-három másodrendű felül- és aluláteresztő szűrő (meredek sávszélek). */
static double bandDb(const std::vector<float>& x, size_t from, double sr, double lo, double hi) {
    magic::BiquadD h[3], l[3];
    for (auto& f : h) magic::designHighpass(f, sr, lo, 0.7071);
    for (auto& f : l) magic::designLowpass(f, sr, hi, 0.7071);
    double e = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        double y = x[i];
        for (int k = 0; k < 3; ++k) y = l[k].process(h[k].process(y));
        if (i >= from) e += y * y;
    }
    return 10.0 * std::log10(e + 1e-30);
}

static void testMagicTouch() {
    std::printf("\n[Magic Touch]\n");
    const double sr = 48000.0;
    const size_t n = static_cast<size_t>(sr * 24);
    const size_t tail = static_cast<size_t>(sr * 14);   // a beállás után vizsgált rész

    // 1. Rendben lévő jel (mono rózsaszín zaj): nincs mit javítani, a kimenet érintetlen
    {
        auto mt = std::make_unique<magic::MagicTouch>();
        mt->setControl(100.0f, false, false);
        std::vector<float> L = pinkNoise(n, 1, 0.25), R = L, inL = L;
        const MagicRun r = runMagic(*mt, L, R);
        double maxDiff = 0;
        for (size_t i = tail; i < n; ++i) maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(L[i] - inL[i])));
        CHECK(r.last.valid && maxDiff < 1e-5, "rendben lévő jel érintetlen (eltérés %.2e; vágás %.2f, levegő %.2f dB)",
              maxDiff, r.last.lowMidCutDb, r.last.airGainDb);
        CHECK(r.last.content == magic::kProfMusic && r.last.speechScore < 0.4f && r.last.bandwidthHz > 18000.0f,
              "rendben lévő jel: zene (beszéd-jelleg %.2f), sávszélesség %.0f Hz", r.last.speechScore, r.last.bandwidthHz);
        checkLimits(r, "rendben lévő jel");
    }
    // 2. „Dobozhang”: +6 dB 200 Hz-en -> mély-közép vágás, hangerő-semlegesen
    {
        auto mt = std::make_unique<magic::MagicTouch>();
        mt->setControl(100.0f, false, false);
        std::vector<float> L = pinkNoise(n, 2, 0.2);
        magic::Biquad bump;
        magic::designPeaking(bump, sr, 200.0, 6.0, 1.0);
        for (float& v : L) v = bump.process(v);
        std::vector<float> R = L, inL = L;
        const MagicRun r = runMagic(*mt, L, R);
        magic::BiquadD bp1, bp2;
        magic::designHighpass(bp1, sr, 150.0, 0.7071);
        magic::designLowpass(bp2, sr, 280.0, 0.7071);
        auto band = [&](const std::vector<float>& x) {
            bp1.reset(); bp2.reset();
            double e = 0;
            for (size_t i = 0; i < x.size(); ++i) { const double y = bp2.process(bp1.process(x[i])); if (i >= tail) e += y * y; }
            return 10.0 * std::log10(e);
        };
        const double dBand = band(L) - band(inL), dAll = rmsDb(L, tail) - rmsDb(inL, tail);
        CHECK(r.last.lowMidCutDb < -1.0f && dBand - dAll < -0.5 && std::fabs(dAll) < 0.6,
              "dobozhang: vágás %.2f dB @ %.0f Hz, a 150-280 Hz sáv %.2f dB, teljes szint %.2f dB",
              r.last.lowMidCutDb, r.last.lowMidFreqHz, dBand, dAll);
        checkLimits(r, "dobozhang");
    }
    // 3. Ellenfázisú mély: fokozatos mono basszus (50 Hz-en az oldaljel erősen csökken)
    {
        auto mt = std::make_unique<magic::MagicTouch>();
        mt->setControl(100.0f, false, false);
        std::vector<float> L = pinkNoise(n, 3, 0.1), R = L;
        const std::vector<float> bass = sine(50.0, 0.25, n, sr), hi = sine(1000.0, 0.05, n, sr);
        for (size_t i = 0; i < n; ++i) { L[i] += bass[i] + hi[i]; R[i] += -bass[i] - hi[i]; }
        std::vector<float> inS(n), outS(n);
        for (size_t i = 0; i < n; ++i) inS[i] = 0.5f * (L[i] - R[i]);
        const MagicRun r = runMagic(*mt, L, R);
        for (size_t i = 0; i < n; ++i) outS[i] = 0.5f * (L[i] - R[i]);
        const double d50 = 20 * std::log10(toneAmp(outS, tail, 50.0, sr) / toneAmp(inS, tail, 50.0, sr));
        const double d1k = 20 * std::log10(toneAmp(outS, tail, 1000.0, sr) / toneAmp(inS, tail, 1000.0, sr));
        CHECK(r.last.lfCorr < 0.0f && r.last.monoBass > 0.9f && d50 < -12.0 && std::fabs(d1k) < 0.5,
              "ellenfázisú mély: korreláció %.2f, mono %.2f, oldaljel 50 Hz %.1f dB, 1 kHz %.2f dB",
              r.last.lfCorr, r.last.monoBass, d50, d1k);
        checkLimits(r, "ellenfázisú mély");
    }
    // 4. Keskeny keverés: tágítás 150 Hz felett; bekapcsolt Crossfeed mellett nincs tágítás
    for (int cf = 0; cf < 2; ++cf) {
        auto mt = std::make_unique<magic::MagicTouch>();
        mt->setControl(100.0f, false, cf != 0);
        const std::vector<float> M = pinkNoise(n, 4, 0.2), S = pinkNoise(n, 5, 0.02);
        std::vector<float> L(n), R(n), inS(n), outS(n);
        for (size_t i = 0; i < n; ++i) { L[i] = M[i] + S[i]; R[i] = M[i] - S[i]; inS[i] = S[i]; }
        const MagicRun r = runMagic(*mt, L, R);
        for (size_t i = 0; i < n; ++i) outS[i] = 0.5f * (L[i] - R[i]);
        magic::BiquadD hp;
        magic::designHighpass(hp, sr, 1000.0, 0.7071);
        auto hpDb = [&](const std::vector<float>& x) {
            hp.reset();
            double e = 0;
            for (size_t i = 0; i < x.size(); ++i) { const double y = hp.process(x[i]); if (i >= tail) e += y * y; }
            return 10.0 * std::log10(e);
        };
        const double dS = hpDb(outS) - hpDb(inS);
        if (cf == 0)
            CHECK(r.last.widthGainDb > 0.5f && std::fabs(dS - r.last.widthGainDb) < 0.4,
                  "keskeny keverés (S/M %.1f dB): tágítás %.2f dB, oldaljel 1 kHz felett %.2f dB",
                  r.last.sideMidDb, r.last.widthGainDb, dS);
        else
            CHECK(r.maxWidth < 1e-3f && std::fabs(dS) < 0.05, "keskeny keverés Crossfeeddel: nincs tágítás (%.3f dB)", dS);
        checkLimits(r, "keskeny keverés");
    }
    // 5. Tompa, sziszegő felvétel: a levegő nem emel a zajra
    {
        auto mt = std::make_unique<magic::MagicTouch>();
        mt->setControl(100.0f, false, false);
        std::vector<float> L = pinkNoise(n, 6, 0.25);
        magic::Biquad dull;
        magic::designHighShelf(dull, sr, 5000.0, -12.0, 0.9);
        std::mt19937 rng(7);
        std::normal_distribution<double> g(0.0, 1.0);
        // Szakaszos zene (2 s jel, 0,5 s halk) és állandó sziszegés a halk részekben is
        for (size_t i = 0; i < n; ++i) {
            const bool loud = (i / static_cast<size_t>(sr * 0.5)) % 5 != 4;
            L[i] = dull.process(L[i]) * (loud ? 1.0f : 0.02f) + static_cast<float>(0.003 * g(rng));
        }
        std::vector<float> R = L;
        const MagicRun r = runMagic(*mt, L, R);
        CHECK(r.last.hiss > 0.5f && r.maxAir < 0.3f, "tompa + sziszegés: zaj %.2f, levegő legfeljebb %.2f dB", r.last.hiss, r.maxAir);
        checkLimits(r, "tompa + sziszegés");
    }
    // 6. A/B összehasonlítás: az eredeti jel szól
    {
        auto mt = std::make_unique<magic::MagicTouch>();
        mt->setControl(100.0f, false, false);
        std::vector<float> L = pinkNoise(n / 2, 8, 0.2);
        magic::Biquad bump;
        magic::designPeaking(bump, sr, 200.0, 6.0, 1.0);
        for (float& v : L) v = bump.process(v);
        std::vector<float> R = L;
        runMagic(*mt, L, R);
        mt->setControl(100.0f, true, false);
        std::vector<float> L2 = pinkNoise(static_cast<size_t>(sr), 9, 0.2), R2 = L2, in2 = L2;
        runMagic(*mt, L2, R2);
        double maxDiff = 0;
        for (size_t i = static_cast<size_t>(sr * 0.1); i < L2.size(); ++i)
            maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(L2[i] - in2[i])));
        CHECK(maxDiff < 1e-6, "A/B: az eredeti jel szól (eltérés %.2e)", maxDiff);
    }
    // 8. Archív felvétel (10 kHz felett letört, mono): automatikus felismerés, exciter; élességnek nem számít
    {
        auto mt = std::make_unique<magic::MagicTouch>();
        mt->setControl(100.0f, false, false);
        std::vector<float> L = pinkNoise(n, 11, 0.25);
        magic::Biquad lp[6];
        for (auto& f : lp) magic::designLowpass(f, sr, 10000.0, 0.7071);
        for (float& v : L) for (auto& f : lp) v = f.process(v);
        std::vector<float> R = L, inL = L;
        const MagicRun r = runMagic(*mt, L, R);
        const double dHi = bandDb(L, tail, sr, 13000.0, 20000.0) - bandDb(inL, tail, sr, 13000.0, 20000.0);
        const double dAll = lufs(L, R, tail, sr) - lufs(inL, inL, tail, sr);
        CHECK(r.last.content == magic::kProfArchive && r.last.bandwidthHz < 14500.0f && r.last.harshness < 0.3f &&
                  r.last.exciterMix > 0.05f && dHi > 3.0 && std::fabs(dAll) < 0.5,
              "archív: profil %u, sávszélesség %.0f Hz, élesség %.2f, exciter %.0f %%, 13-20 kHz %+.1f dB, hangosság %+.2f LU",
              r.last.content, r.last.bandwidthHz, r.last.harshness, r.last.exciterMix * 100.0f, dHi, dAll);
        checkLimits(r, "archív");
    }
    // 8b. „Bátor” tartomány (200 %): ugyanazokon a jeleken erősebb beavatkozás, a kibővített korlátokon belül; a
    // rendben lévő jelen (rózsaszín zaj) is van hatás, 100 %-on ott nincs
    {
        struct Case { const char* name; int kind; };
        for (const Case c : { Case{ "rendben lévő jel", 0 }, Case{ "keskeny keverés", 1 }, Case{ "archív", 2 } }) {
            auto make = [&](std::vector<float>& L, std::vector<float>& R) {
                if (c.kind == 0) { L = pinkNoise(n, 1, 0.25); R = L; }
                else if (c.kind == 1) {
                    const std::vector<float> M = pinkNoise(n, 4, 0.2), S = pinkNoise(n, 5, 0.02);
                    L.resize(n); R.resize(n);
                    for (size_t i = 0; i < n; ++i) { L[i] = M[i] + S[i]; R[i] = M[i] - S[i]; }
                } else {
                    L = pinkNoise(n, 11, 0.25);
                    magic::Biquad lp[6];
                    for (auto& f : lp) magic::designLowpass(f, sr, 10000.0, 0.7071);
                    for (float& v : L) for (auto& f : lp) v = f.process(v);
                    R = L;
                }
            };
            double diff[2] = { 0, 0 };
            MagicRun runs[2];
            for (int k = 0; k < 2; ++k) {
                auto mt = std::make_unique<magic::MagicTouch>();
                mt->setControl(k == 0 ? 100.0f : 200.0f, false, false);
                std::vector<float> L, R;
                make(L, R);
                const std::vector<float> inL = L;
                runs[k] = runMagic(*mt, L, R);
                double e = 0, ein = 0;
                for (size_t i = tail; i < n; ++i) { const double d = L[i] - inL[i]; e += d * d; ein += static_cast<double>(inL[i]) * inL[i]; }
                diff[k] = 10.0 * std::log10(e / ein + 1e-30);   // a változás energiája a jelhez képest (dB)
            }
            CHECK(diff[1] > diff[0] + 3.0 && (c.kind != 0 || diff[1] > -40.0),
                  "Bátor (200 %%), %s: a változás %.1f dB a jelhez képest (100 %%-on %.1f dB)", c.name, diff[1], diff[0]);
            checkLimits(runs[1], c.name, 1.0f);
        }
    }
    // 9. Beszéd: automatikus felismerés (legfeljebb kb. 15 s), mélyvágás, szintkiegyenlítés (a második fele 8 dB-lel halkabb)
    {
        auto mt = std::make_unique<magic::MagicTouch>();
        mt->setControl(100.0f, false, false);
        const size_t ns = static_cast<size_t>(sr * 60);
        std::vector<float> L = synthSpeech(ns, 12, sr, 0.3);
        for (size_t i = ns / 2; i < ns; ++i) L[i] *= 0.398f;
        std::vector<float> R = L, inL = L, inR = R;
        const MagicRun r = runMagic(*mt, L, R);
        const size_t a0 = static_cast<size_t>(sr * 18), a1 = ns / 2, b0 = static_cast<size_t>(sr * 40), b1 = ns;
        const double stepIn = segDb(inL, inR, a0, a1) - segDb(inL, inR, b0, b1);
        const double stepOut = segDb(L, R, a0, a1) - segDb(L, R, b0, b1);
        const double dAll = lufs(L, R, a0, sr) - lufs(inL, inR, a0, sr);
        CHECK(r.last.content == magic::kProfSpeech && r.firstSpeechS > 0 && r.firstSpeechS < 16.0 && r.last.lowCut > 0.9f,
              "beszéd: felismerve %.1f s után (beszéd-jelleg %.2f), mélyvágás %.2f", r.firstSpeechS, r.last.speechScore, r.last.lowCut);
        CHECK(stepOut < stepIn - 3.0 && std::fabs(dAll) < 2.0,
              "beszéd szintkiegyenlítés: a szintugrás %.1f -> %.1f dB, hangosság %+.2f LU", stepIn, stepOut, dAll);
        checkLimits(r, "beszéd");
    }
    // 10. Kézi tartalom: Beszéd a zenére (azonnal beszéd profil), Zene a beszédre (nincs mélyvágás, szintkiegyenlítés)
    {
        auto mt = std::make_unique<magic::MagicTouch>();
        mt->setControl(100.0f, false, false, magic::kCharNatural, magic::kContentSpeech);
        std::vector<float> L = pinkNoise(n / 2, 13, 0.2), R = L;
        const MagicRun r = runMagic(*mt, L, R);
        auto mt2 = std::make_unique<magic::MagicTouch>();
        mt2->setControl(100.0f, false, false, magic::kCharNatural, magic::kContentMusic);
        std::vector<float> L2 = synthSpeech(n, 14, sr, 0.3), R2 = L2;
        const MagicRun r2 = runMagic(*mt2, L2, R2);
        CHECK(r.last.content == magic::kProfSpeech && r.last.contentAuto == 0 && r.last.lowCut > 0.9f &&
                  r2.last.content != magic::kProfSpeech && r2.firstSpeechS < 0 && r2.last.lowCut == 0.0f && r2.maxLevel == 0.0f,
              "kézi tartalom: Beszéd (profil %u, mélyvágás %.2f), Zene a beszédre (profil %u, beszéd-jelleg %.2f)",
              r.last.content, r.last.lowCut, r2.last.content, r2.last.speechScore);
    }
    // 11. Túltömörített jel ütésekkel: visszafogott tranziens-kiemelés, a csúcs nem nő −1 dBFS fölé, hangerő-semleges
    {
        auto mt = std::make_unique<magic::MagicTouch>();
        mt->setControl(100.0f, false, false);
        std::vector<float> L = pinkNoise(n, 15, 0.6);
        for (float& v : L) v = 0.5f * std::tanh(3.0f * v);   // „téglafal” master
        const std::vector<float> kick = sine(60.0, 1.0, n, sr);
        const size_t period = static_cast<size_t>(sr * 0.5);
        for (size_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(i % period) / sr;
            L[i] += static_cast<float>(0.3 * std::exp(-t / 0.03) * kick[i]);
        }
        std::vector<float> R = L, inL = L;
        const MagicRun r = runMagic(*mt, L, R);
        float pkIn = 0, pkOut = 0;
        for (size_t i = tail; i < n; ++i) { pkIn = std::max(pkIn, std::fabs(inL[i])); pkOut = std::max(pkOut, std::fabs(L[i])); }
        const double dAll = lufs(L, R, tail, sr) - lufs(inL, inL, tail, sr);
        CHECK((r.last.content == magic::kProfMusicDense || r.last.content == magic::kProfMusic) && r.last.crestDb < 11.0f &&
                  r.maxTransNow > 0.2f && pkOut <= std::max(pkIn, magic::kPeakCeiling) + 1e-4f && std::fabs(dAll) < 0.5,
              "túltömörített: profil %u, csúcstényező %.1f dB, LRA %.1f, tranziens legfeljebb %.2f dB, csúcs %.3f -> %.3f, hangosság %+.2f LU",
              r.last.content, r.last.crestDb, r.last.lra, r.maxTransNow, pkIn, pkOut, dAll);
        checkLimits(r, "túltömörített");
    }
    // 12. Karakter: Meleg (több mély, kevesebb magas), Fényes (levegő), Tágas (a mérsékelten keskeny keverés is tágul)
    {
        auto runChar = [&](int32_t c, std::vector<float>& L, std::vector<float>& R) {
            auto mt = std::make_unique<magic::MagicTouch>();
            mt->setControl(100.0f, false, false, c, magic::kContentAuto);
            return runMagic(*mt, L, R);
        };
        std::vector<float> Lw = pinkNoise(n, 16, 0.2), Rw = Lw, in = Lw;
        const MagicRun rw = runChar(magic::kCharWarm, Lw, Rw);
        const double dLow = bandDb(Lw, tail, sr, 40.0, 140.0) - bandDb(in, tail, sr, 40.0, 140.0);
        const double dHigh = bandDb(Lw, tail, sr, 11000.0, 16000.0) - bandDb(in, tail, sr, 11000.0, 16000.0);
        const double dAll = lufs(Lw, Rw, tail, sr) - lufs(in, in, tail, sr);
        CHECK(rw.last.bodyDb > 1.0f && rw.last.warmthMix > 0.03f && rw.last.airGainDb < -0.5f && dLow - dHigh > 1.5 &&
                  std::fabs(dAll) < 0.5,
              "Meleg: mély polc %+.2f dB, meleg felharmonikusok %.0f %%, levegő %+.2f dB; mély %+.2f, magas %+.2f dB, hangosság %+.2f LU",
              rw.last.bodyDb, rw.last.warmthMix * 100.0f, rw.last.airGainDb, dLow, dHigh, dAll);
        checkLimits(rw, "Meleg");
        // Fényes: tipikus zenei spektrumon (a magas a rózsaszín zajnál tompább), a Természeteshez képest
        std::vector<float> dull = pinkNoise(n, 19, 0.2);
        magic::Biquad hs;
        magic::designHighShelf(hs, sr, 6000.0, -10.0, 0.9);
        for (float& v : dull) v = hs.process(v);
        std::vector<float> Ln = dull, Rn = dull, Lb = dull, Rb = dull;
        const MagicRun rn = runChar(magic::kCharNatural, Ln, Rn);
        const MagicRun rb = runChar(magic::kCharBright, Lb, Rb);
        const double dHiB = bandDb(Lb, tail, sr, 11000.0, 16000.0) - bandDb(Ln, tail, sr, 11000.0, 16000.0);
        CHECK(rb.last.airGainDb > rn.last.airGainDb + 1.0f && dHiB > 0.6,
              "Fényes: levegő %+.2f dB (Természetes: %+.2f dB), 11-16 kHz a Természeteshez képest %+.2f dB",
              rb.last.airGainDb, rn.last.airGainDb, dHiB);
        checkLimits(rb, "Fényes");
        float width[2] = { 0, 0 };
        for (int k = 0; k < 2; ++k) {
            const std::vector<float> M = pinkNoise(n, 17, 0.2), S = pinkNoise(n, 18, 0.056);   // S/M ≈ −11 dB
            std::vector<float> L(n), R(n);
            for (size_t i = 0; i < n; ++i) { L[i] = M[i] + S[i]; R[i] = M[i] - S[i]; }
            width[k] = runChar(k == 0 ? magic::kCharNatural : magic::kCharSpacious, L, R).last.widthGainDb;
        }
        CHECK(width[0] < 0.01f && width[1] > 0.5f, "Tágas: S/M −11 dB-nél tágítás %.2f dB (Természetes: %.2f dB)", width[1], width[0]);
    }
    // 13. De-hum: 50 Hz-es zümmögés és felharmonikusai szakaszos zenében (a halk részekben is szól)
    for (double base : { 50.0, 60.0 }) {
        auto mt = std::make_unique<magic::MagicTouch>();
        mt->setControl(100.0f, false, false);
        // a zümmögés a halk részekben (1,5 s, 4,5 s-onként) jól hallható, a hangosakban a zene alatt van;
        // a felismeréshez a halk részek átlagspektruma kell (kb. 15 s), ezért hosszabb a jel
        const size_t nh = static_cast<size_t>(sr * 40), tailH = static_cast<size_t>(sr * 30);
        std::vector<float> L = pinkNoise(nh, 20, 0.2);
        for (size_t i = 0; i < nh; ++i) if ((i / static_cast<size_t>(sr * 1.5)) % 3 == 2) L[i] *= 0.05f;
        std::vector<float> clean = L;
        const double amps[4] = { 0.006, 0.004, 0.003, 0.002 };
        for (int h = 0; h < 4; ++h) {
            const std::vector<float> s = sine(base * (h + 1), amps[h], nh, sr);
            for (size_t i = 0; i < nh; ++i) L[i] += s[i];
        }
        std::vector<float> R = L, inL = L;
        const MagicRun r = runMagic(*mt, L, R);
        double worst = -1e9;
        for (int h = 0; h < 3; ++h) {
            const double f = base * (h + 1);
            const double hin = toneAmp(inL, tailH, f, sr), hcl = toneAmp(clean, tailH, f, sr), hout = toneAmp(L, tailH, f, sr);
            worst = std::max(worst, 20 * std::log10((hout + 1e-9) / (hin - hcl + 1e-9)));
        }
        const double d1k = bandDb(L, tailH, sr, 800.0, 1250.0) - bandDb(inL, tailH, sr, 800.0, 1250.0);
        CHECK(std::fabs(r.last.humHz - base) < 0.3f && r.last.humCount >= 3 && worst < -10.0 && std::fabs(d1k) < 0.2,
              "de-hum %.0f Hz: felismerve %.2f Hz, %u felharmonikus, legfeljebb %.1f dB vágás; a zümmögés legalább %.1f dB-lel halkabb, 1 kHz %+.2f dB",
              base, r.last.humHz, r.last.humCount, r.last.humCutDb, -worst, d1k);
        checkLimits(r, "de-hum");
    }
    // 14. Sziszegés (állandó magas zajpadló) szakaszos zenében: sávonkénti zajcsökkentés a halk részekben, a hangosakban nem
    {
        auto mt = std::make_unique<magic::MagicTouch>();
        mt->setControl(100.0f, false, false);
        std::vector<float> L = pinkNoise(n, 21, 0.25), hissOnly(n);
        std::mt19937 rng(22);
        std::normal_distribution<double> g(0.0, 1.0);
        magic::BiquadD hhp;
        magic::designHighpass(hhp, sr, 2000.0, 0.7071);
        std::vector<bool> quiet(n);
        for (size_t i = 0; i < n; ++i) {
            quiet[i] = (i / static_cast<size_t>(sr * 0.5)) % 5 == 4;
            hissOnly[i] = static_cast<float>(0.002 * hhp.process(g(rng)));
            L[i] = L[i] * (quiet[i] ? 0.0f : 1.0f) + hissOnly[i];
        }
        std::vector<float> R = L, inL = L;
        const MagicRun r = runMagic(*mt, L, R);
        double eqIn = 0, eqOut = 0, elIn = 0, elOut = 0;
        for (size_t i = n - static_cast<size_t>(sr * 6); i < n; ++i) {   // a beállás után (a zajpadlóhoz 5 s előzmény kell)
            const size_t ph = i % static_cast<size_t>(sr * 2.5);
            if (quiet[i] && ph > sr * 2.1 && ph < sr * 2.45) { eqIn += inL[i] * inL[i]; eqOut += L[i] * L[i]; }
            if (!quiet[i] && ph > sr * 0.2 && ph < sr * 1.8) { elIn += inL[i] * inL[i]; elOut += L[i] * L[i]; }
        }
        const double dq = 10 * std::log10(eqOut / eqIn), dl = 10 * std::log10(elOut / elIn);
        CHECK(r.last.nrMode == 1 && dq < -2.0 && dq > -magic::kMaxNrDb - 0.5 && std::fabs(dl) < 0.5,
              "sziszegés: sávonkénti zajcsökkentés legfeljebb %.1f dB; a halk részekben %+.1f dB, a hangosakban %+.2f dB",
              r.last.nrDb, dq, dl);
        checkLimits(r, "sziszegés");
    }
    // 15. Kattanások archív felvételen: javítás 2,7 ms késleltetéssel; az A/B eredeti jele is késleltetett
    {
        auto mt = std::make_unique<magic::MagicTouch>();
        mt->setControl(100.0f, false, false);
        std::vector<float> clean = pinkNoise(n, 23, 0.25);
        magic::Biquad lp[6];
        for (auto& f : lp) magic::designLowpass(f, sr, 10000.0, 0.7071);
        for (float& v : clean) for (auto& f : lp) v = f.process(v);
        std::vector<float> L = clean;
        std::mt19937 rng(24);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        std::vector<size_t> pos;
        for (size_t t = static_cast<size_t>(sr); t + 10 < n; t += static_cast<size_t>(sr * (0.2 + 0.2 * u(rng)))) {
            const float A = static_cast<float>((u(rng) < 0.5 ? -1 : 1) * (0.1 + 0.3 * u(rng)));
            L[t] += A; L[t + 1] += 0.5f * A;
            pos.push_back(t);
        }
        std::vector<float> R = L, inL = L;
        const MagicRun r = runMagic(*mt, L, R);
        const size_t D = static_cast<size_t>(std::lround(r.last.latencyMs * sr / 1000.0));
        double eIn = 0, eOut = 0;
        for (size_t t : pos) {
            if (t < tail) continue;
            for (size_t k = t - 4; k < t + 6; ++k) {
                eIn += std::pow(inL[k] - clean[k], 2);
                if (k + D < n) eOut += std::pow(L[k + D] - clean[k], 2);
            }
        }
        const double dClick = 10 * std::log10(eOut / eIn);
        CHECK(r.last.latencyMs > 2.0f && r.last.latencyMs < 3.0f && r.last.clickRate > 1.0f && dClick < -10.0,
              "kattanások: %.1f/s felismerve, késleltetés %.1f ms, a kattanások energiája %.1f dB",
              r.last.clickRate, r.last.latencyMs, dClick);
        checkLimits(r, "kattanások");
        // A/B: az eredeti jel szól, a késleltetéssel együtt (nincs időbeli ugrás)
        mt->setControl(100.0f, true, false);
        std::vector<float> L2 = pinkNoise(static_cast<size_t>(sr), 25, 0.2), R2 = L2, in2 = L2;
        runMagic(*mt, L2, R2);
        double maxDiff = 0;
        for (size_t i = static_cast<size_t>(sr * 0.1); i < L2.size(); ++i)
            maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(L2[i] - in2[i - D])));
        CHECK(maxDiff < 1e-6, "A/B késleltetett helyreállítás mellett: az eredeti jel, %zu mintával késleltetve (eltérés %.2e)", D, maxDiff);
    }
    // 16. Zajos beszéd: RNNoise (ha beépült, 48 kHz), különben sávonkénti zajcsökkentés; a beszéd szintje marad
    {
        auto mt = std::make_unique<magic::MagicTouch>();
        mt->setControl(100.0f, false, false, magic::kCharNatural, magic::kContentSpeech);
        const size_t ns = static_cast<size_t>(sr * 40);
        const std::vector<float> sp = synthSpeech(ns, 26, sr, 0.3), noise = pinkNoise(ns, 27, 0.03);
        std::vector<float> L(ns);
        for (size_t i = 0; i < ns; ++i) L[i] = sp[i] + noise[i];
        std::vector<float> R = L, inL = L;
        const MagicRun r = runMagic(*mt, L, R);
        const size_t D = static_cast<size_t>(std::lround(r.last.latencyMs * sr / 1000.0));
        const size_t F = 960;
        double ep = 0, ip = 0, es = 0, is = 0;
        for (size_t k = static_cast<size_t>(sr * 20) / F; (k + 2) * F + D < ns; ++k) {
            double c = 0, x = 0, y = 0;
            for (size_t j = 0; j < F; ++j) {
                const size_t t = k * F + j;
                c += sp[t] * sp[t]; x += inL[t] * inL[t]; y += L[t + D] * L[t + D];
            }
            if (c < 1e-10) { ip += x; ep += y; } else if (c > 1e-3 * F) { is += x; es += y; }
        }
        const double dp = 10 * std::log10(ep / ip), ds = 10 * std::log10(es / is);
        const bool rnn = magic::RnnDenoiser::available();
        CHECK(r.last.nrMode == (rnn ? 2u : 1u) && dp < -2.0 && ds - dp > 1.5,
              "zajos beszéd (%s): késleltetés %.1f ms, a szünetekben %+.1f dB, a beszédben %+.1f dB",
              rnn ? "RNNoise" : "sávonkénti", r.last.latencyMs, dp, ds);
        checkLimits(r, "zajos beszéd");
    }
    // 7. A láncban: 0 %-on ki van kerülve (bitre érintetlen), és a ki-be kapcsolás kattanásmentes
    {
        AetherChain chain;
        chain.setSampleRate(sr);
        ParamData d = transparentParams();
        d.set(kFlagMagic, true);
        d.magicAmount = 0.0f;
        chain.applyParamsRT(d);
        std::vector<float> L = pinkNoise(48000, 10, 0.2), R = L, in = L;
        runBlocks([&](float* a, float* b, size_t m) { chain.process(a, b, m); }, L, R, 480);
        bool same = true;
        for (size_t i = 0; i < L.size(); ++i) same = same && L[i] == in[i];
        CHECK(same, "láncban 0 %%-on: bitre érintetlen");
        std::vector<float> s = sine(440.0, 0.3, 96000, sr), s2 = s;
        size_t blk = 0;
        runBlocks([&](float* a, float* b, size_t m) {
            if (blk == 50) { d.magicAmount = 60.0f; chain.applyParamsRT(d); }
            if (blk == 120) { d.set(kFlagMagic, false); chain.applyParamsRT(d); }
            chain.process(a, b, m);
            ++blk;
        }, s, s2, 480);
        float jump = 0;
        for (size_t i = 1; i < s.size(); ++i) jump = std::max(jump, std::fabs(s[i] - s[i - 1]));
        const float base = static_cast<float>(2.0 * kPi * 440.0 / sr * 0.3);
        CHECK(jump < base * 1.6f, "Magic Touch be- és kikapcsolása kattanásmentes (legnagyobb lépés %.4f, szinusz %.4f)", jump, base);
    }
}

/** A tiszta szinusz szintje a mélyvágás után (a lánc teljes útján, a belépő átúsztatás után). */
static double subsonicGainDb(double f, int32_t subHz, double sr) {
    AetherChain chain;
    chain.setSampleRate(sr);
    ParamData d = transparentParams();
    d.set(kFlagMono, true);
    d.subsonicHz = subHz;
    chain.applyParamsRT(d);
    const size_t n = static_cast<size_t>(4.0 * sr);
    std::vector<float> L = sine(f, 0.5, n, sr), R = L;
    runBlocks([&](float* l, float* r, size_t m) { chain.process(l, r, m); }, L, R, 480);
    return 20.0 * std::log10(toneAmp(L, n - static_cast<size_t>(sr), f, sr) / 0.5);
}

/** Véletlen jel a lánc után, a kapcsolás átúsztatása utáni része megegyezik-e a bemenettel (bit-pontosan). */
static bool passesUnchanged(const ParamData& d) {
    AetherChain chain;
    chain.setSampleRate(48000.0);
    chain.applyParamsRT(d);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-0.9f, 0.9f);
    std::vector<float> L(24000), R(24000);
    for (auto& v : L) v = u(rng);
    for (auto& v : R) v = u(rng);
    const std::vector<float> L0 = L, R0 = R;
    runBlocks([&](float* l, float* r, size_t m) { chain.process(l, r, m); }, L, R, 480);
    const size_t from = 4800;   // az első 0,1 s az átúsztatás (5 ms le, 5 ms fel) miatt nem számít
    return std::equal(L.begin() + from, L.end(), L0.begin() + from) && std::equal(R.begin() + from, R.end(), R0.begin() + from);
}

static void testMonoBass() {
    std::printf("[9] Mélyvágás (10 / 15 / 20 / 25 / 30 / 35 Hz, 24 dB/oktáves) és a kikapcsolt modul\n");
    const double sr = 48000.0;
    for (int32_t hz : { 10, 15, 20, 25, 30, 35 }) {
        const double gc = subsonicGainDb(hz, hz, sr), g2c = subsonicGainDb(2.0 * hz, hz, sr);
        const double g4c = subsonicGainDb(4.0 * hz, hz, sr);
        CHECK(std::abs(gc + 6.0) < 0.3 && g2c > -0.7 && g2c < 0.0 && std::abs(g4c) < 0.1,
              "%d Hz: fc-n %.2f dB (LR4: −6 dB), 2·fc-n %.2f dB, 4·fc-n %.2f dB", hz, gc, g2c, g4c);
    }
    const double g = subsonicGainDb(100.0, 30, sr);
    CHECK(std::abs(g) < 0.1, "30 Hz-es vágás mellett 100 Hz szintje %.3f dB (a hangzás érintetlen)", g);
    const double g35 = subsonicGainDb(140.0, 35, sr);
    CHECK(std::abs(g35) < 0.1, "35 Hz-es vágás mellett 140 Hz szintje %.3f dB (a hangzás érintetlen)", g35);

    for (int32_t v : { 15, 25, 35 }) {
        ParamData ok = transparentParams();
        ok.subsonicHz = v;
        sanitizeParams(ok);
        CHECK(ok.subsonicHz == v, "a köztes %d Hz-es vágás érvényes (megmarad)", v);
    }
    for (int32_t v : { 5, 12, 40, -10 }) {
        ParamData bad = transparentParams();
        bad.subsonicHz = v;
        sanitizeParams(bad);
        CHECK(bad.subsonicHz == 0, "érvénytelen vágási frekvencia (%d Hz) kikapcsolva", v);
    }

    ParamData on0 = transparentParams();
    on0.set(kFlagMono, true);
    CHECK(passesUnchanged(on0), "modul bekapcsolva, de vágás ki: bit-pontosan átlátszó");
    ParamData offHz = transparentParams();
    offHz.subsonicHz = 20;
    CHECK(passesUnchanged(offHz), "modul kikapcsolva (a vágás 20 Hz-re állítva): bit-pontosan átlátszó");

    // Mono-keverés: a bal csatornán csak szinusz szól, így M = S = a jel fele. A középső csatorna (M) nem változhat,
    // az oldalsó (S) 100 Hz alatt a HP4 szerint csökken (50 Hz: −24,6 dB, 100 Hz: −6 dB), 1 kHz-en nem.
    auto monoLevels = [&](double f, double amount, double& mDb, double& sDb) {
        AetherChain chain;
        chain.setSampleRate(sr);
        ParamData d = transparentParams();
        d.set(kFlagMono, true);
        d.monoAmount = static_cast<float>(amount);
        chain.applyParamsRT(d);
        const size_t n = static_cast<size_t>(4.0 * sr);
        std::vector<float> L = sine(f, 0.5, n, sr), R(n, 0.0f);
        runBlocks([&](float* l, float* r, size_t m) { chain.process(l, r, m); }, L, R, 480);
        std::vector<float> M(n), S(n);
        for (size_t i = 0; i < n; ++i) { M[i] = 0.5f * (L[i] + R[i]); S[i] = 0.5f * (L[i] - R[i]); }
        const size_t from = n - static_cast<size_t>(sr);
        mDb = 20.0 * std::log10(toneAmp(M, from, f, sr) / 0.25);
        sDb = 20.0 * std::log10(toneAmp(S, from, f, sr) / 0.25);
    };
    double m50, s50, m100, s100, m1k, s1k, m50h, s50h;
    monoLevels(50.0, 100.0, m50, s50);
    monoLevels(100.0, 100.0, m100, s100);
    monoLevels(1000.0, 100.0, m1k, s1k);
    monoLevels(50.0, 50.0, m50h, s50h);
    CHECK(std::abs(s50 + 24.6) < 0.5 && std::abs(s100 + 6.0) < 0.3 && std::abs(s1k) < 0.1,
          "mono 100%%: oldalsó szint 50 Hz %.2f dB, 100 Hz %.2f dB, 1 kHz %.2f dB (LR4, fc 100 Hz)", s50, s100, s1k);
    CHECK(std::abs(m50) < 0.05 && std::abs(m100) < 0.05 && std::abs(m1k) < 0.05,
          "mono 100%%: a középső csatorna érintetlen (50 Hz %.3f, 100 Hz %.3f, 1 kHz %.3f dB)", m50, m100, m1k);
    // A keverés komplex összeg: a HP4 fázisa 50 Hz-en kb. −87°, ezért az eredő amplitúdó |0,5 + 0,5·H|
    const double x = 50.0 / 100.0;
    const std::complex<double> hp2 = -x * x / std::complex<double>(1.0 - x * x, std::sqrt(2.0) * x);
    const std::complex<double> h = hp2 * hp2;   // Linkwitz–Riley 4.: két azonos szakasz
    const double expect = 20.0 * std::log10(std::abs(0.5 + 0.5 * h));
    CHECK(std::abs(s50h - expect) < 0.1 && std::abs(m50h) < 0.05,
          "mono 50%%: 50 Hz-es oldalsó szint %.2f dB (várt %.2f dB, a fázissal együtt)", s50h, expect);

    // Állítható határfrekvencia: 200 Hz-nél a 200 Hz-es oldalsó jel −6 dB (LR4 fc), a 100 Hz-es kb. −24,6 dB,
    // 50 Hz-nél a 100 Hz-es már csak kb. −0,5 dB (egy oktávval a határ fölött); a középső csatorna érintetlen
    auto monoAt = [&](double f, double fc, double& mDb, double& sDb) {
        AetherChain chain;
        chain.setSampleRate(sr);
        ParamData d = transparentParams();
        d.set(kFlagMono, true);
        d.monoAmount = 100.0f;
        d.monoFreqHz = static_cast<float>(fc);
        chain.applyParamsRT(d);
        const size_t n = static_cast<size_t>(4.0 * sr);
        std::vector<float> L = sine(f, 0.5, n, sr), R(n, 0.0f);
        runBlocks([&](float* l, float* r, size_t m) { chain.process(l, r, m); }, L, R, 480);
        std::vector<float> M(n), S(n);
        for (size_t i = 0; i < n; ++i) { M[i] = 0.5f * (L[i] + R[i]); S[i] = 0.5f * (L[i] - R[i]); }
        const size_t from = n - static_cast<size_t>(sr);
        mDb = 20.0 * std::log10(toneAmp(M, from, f, sr) / 0.25);
        sDb = 20.0 * std::log10(toneAmp(S, from, f, sr) / 0.25);
    };
    double m200, s200, m100f, s100f, m100l, s100l;
    monoAt(200.0, 200.0, m200, s200);
    monoAt(100.0, 200.0, m100f, s100f);
    monoAt(100.0, 50.0, m100l, s100l);
    const double x4 = 100.0 / 50.0;
    const std::complex<double> hp4 = std::pow(-x4 * x4 / std::complex<double>(1.0 - x4 * x4, std::sqrt(2.0) * x4), 2);
    const double s100Expect = 20.0 * std::log10(std::abs(hp4));
    CHECK(std::abs(s200 + 6.0) < 0.3 && std::abs(s100f + 24.6) < 0.5 && std::abs(s100l - s100Expect) < 0.1,
          "mono határ 200 Hz: oldalsó 200 Hz %.2f dB, 100 Hz %.2f dB; határ 50 Hz: 100 Hz %.2f dB (várt %.2f)",
          s200, s100f, s100l, s100Expect);
    CHECK(std::abs(m200) < 0.05 && std::abs(m100f) < 0.05 && std::abs(m100l) < 0.05,
          "állítható határnál is érintetlen a középső csatorna (%.3f / %.3f / %.3f dB)", m200, m100f, m100l);
    ParamData badHz = transparentParams();
    badHz.monoFreqHz = 1000.0f;
    sanitizeParams(badHz);
    ParamData nanHz = transparentParams();
    nanHz.monoFreqHz = std::nanf("");
    sanitizeParams(nanHz);
    CHECK(badHz.monoFreqHz == 250.0f && nanHz.monoFreqHz == 100.0f, "érvénytelen határfrekvencia: 1000 Hz -> 250 Hz, NaN -> 100 Hz");
}

/** Konvolver be- és kikapcsolása: kikapcsolva a jel az átúsztatás után azonnal érintetlen (mint az EQ-nál). */
static void testConvolverToggle() {
    std::printf("[10] Konvolver ki-bekapcsolása: átúsztatás, azonnali bypass\n");
    const double sr = 48000.0;
    AetherChain chain;
    chain.setSampleRate(sr);
    chain.startWorker();
    IrData ir;
    ir.sampleRate = 48000;
    ir.name = "teszt";
    ir.left.assign(2000, 0.0f);
    ir.left[10] = 0.5f;
    chain.submitIr(ir);
    ParamData on = transparentParams();
    on.set(kFlagConvolver, true);
    on.convAutogain = 0;
    chain.applyParamsRT(on);
    std::vector<float> z(480, 0.0f), z2(480, 0.0f);
    chain.process(z.data(), z2.data(), z.size());
    const bool idle = chain.waitIdle();
    for (int i = 0; i < 3; ++i) chain.process(z.data(), z2.data(), z.size());
    CHECK(idle && chain.convolverActive(), "konvolver bekapcsolva, IR betöltve");

    ParamData off = transparentParams();
    chain.applyParamsRT(off);
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> u(-0.9f, 0.9f);
    std::vector<float> L(9600), R(9600);
    for (auto& v : L) v = u(rng);
    for (auto& v : R) v = u(rng);
    const std::vector<float> L0 = L, R0 = R;
    runBlocks([&](float* l, float* r, size_t m) { chain.process(l, r, m); }, L, R, 480);
    const size_t from = 4800;   // az átúsztatás (5 ms le, 5 ms fel) után
    CHECK(std::equal(L.begin() + from, L.end(), L0.begin() + from) && std::equal(R.begin() + from, R.end(), R0.begin() + from),
          "konvolver kikapcsolva: a jel azonnal, bit-pontosan érintetlen (nincs utánhangzás)");
}

/** Konvolver futtatása egy jelen (A/B-vel vagy anélkül); a háttérszál a blokkok előtt betölti az IR-t. */
static std::vector<float> convRunAb(bool compare, const std::vector<float>& x, size_t block) {
    AetherChain chain;
    chain.setSampleRate(48000.0);
    chain.startWorker();
    IrData ir;
    ir.sampleRate = 48000;
    ir.name = "teszt";
    ir.left.assign(64, 0.0f);
    ir.left[0] = 0.5f;   // tiszta, felezett másolat: a kimenet a bemenet 0,5-szerese, zita-késleltetéssel
    chain.submitIr(ir);
    ParamData d = transparentParams();
    d.set(kFlagConvolver, true);
    d.convAutogain = 0;
    d.convCompare = compare ? 1 : 0;
    chain.applyParamsRT(d);
    std::vector<float> warm(block, 0.0f), warm2(block, 0.0f);
    for (int i = 0; i < 4; ++i) chain.process(warm.data(), warm2.data(), block);
    chain.waitIdle();
    for (int i = 0; i < 4; ++i) chain.process(warm.data(), warm2.data(), block);
    std::vector<float> L = x, R = x;
    runBlocks([&](float* l, float* r, size_t m) { chain.process(l, r, m); }, L, R, block);
    return L;
}

/** A legnagyobb korrelációjú eltolás: b[i + lag] ~ a[i] (a és b azonos hosszú, a futó része számít). */
static long bestLag(const std::vector<float>& a, const std::vector<float>& b, size_t from, long maxLag) {
    long best = 0;
    double bestC = -1e30;
    for (long k = 0; k <= maxLag; ++k) {
        double c = 0;
        for (size_t i = from; i + k < b.size(); ++i) c += static_cast<double>(a[i]) * b[i + k];
        if (c > bestC) { bestC = c; best = k; }
    }
    return best;
}

/** A/B összehasonlítás: az eredeti hangerőre kiegyenlített, és időben illeszkedik a feldolgozotthoz. */
static void testAbCompare() {
    std::printf("[11] A/B összehasonlítás: hangerő-kiegyenlítés és időbeli illeszkedés\n");
    const double sr = 48000.0;
    std::mt19937 rng(21);
    std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    std::vector<float> x(48000);
    for (auto& v : x) v = u(rng);

    // Konvolver: a feldolgozott (wet) jel és az A/B-ben szóló eredeti (dry, szintre igazítva)
    const size_t block = 480;
    const std::vector<float> wet = convRunAb(false, x, block);
    const std::vector<float> ab = convRunAb(true, x, block);
    const size_t from = 24000;
    const long lag = bestLag(x, wet, from, 2000);
    CHECK(lag == static_cast<long>(ee::ZitaInstance::latencyFor(static_cast<uint32_t>(block))),
          "konvolver: a feldolgozott jel késleltetése %ld minta (a blokkméret %zu: %zu)", lag, block,
          ee::ZitaInstance::latencyFor(static_cast<uint32_t>(block)));
    const std::vector<float> wet384 = convRunAb(false, x, 384);
    const long lag384 = bestLag(x, wet384, from, 2000);
    CHECK(lag384 == static_cast<long>(ee::ZitaInstance::latencyFor(384)) && lag384 == 128,
          "konvolver: 384-es blokknál a késleltetés %ld minta (várt 128)", lag384);
    double num = 0, den = 0;
    for (size_t i = from; i < x.size(); ++i) { num += (ab[i] - wet[i]) * (ab[i] - wet[i]); den += wet[i] * wet[i]; }
    CHECK(num / den < 0.02, "konvolver A/B: az eredeti az átlagos hangerőn a feldolgozottal egyezik (eltérés %.1f dB)",
          10.0 * std::log10(num / den));

    // EQ (IIR, nincs késleltetés): a kimenet a bemenet skálázott másolata, a feldolgozott hangerőn
    ParamData d = bellParams(0);
    d.eqCompare = 1;
    AetherChain chain;
    chain.setSampleRate(sr);
    chain.applyParamsRT(d);
    std::vector<float> L = x, R = x;
    runBlocks([&](float* l, float* r, size_t m) { chain.process(l, r, m); }, L, R, block);
    double xy = 0, xx = 0;
    for (size_t i = from; i < x.size(); ++i) { xy += static_cast<double>(L[i]) * x[i]; xx += static_cast<double>(x[i]) * x[i]; }
    const double c = xy / xx;   // a legjobb skálázás a bemenetre
    double res = 0, tot = 0;
    for (size_t i = from; i < x.size(); ++i) { res += (L[i] - c * x[i]) * (L[i] - c * x[i]); tot += L[i] * L[i]; }
    CHECK(res / tot < 1e-4, "EQ A/B: a kimenet a bemenet %.3f-szorosa (maradék %.1e)", c, res / tot);

    // A feldolgozott EQ-szint (A/B nélkül) és az A/B-ben szóló szint egyezik
    ParamData d0 = bellParams(0);
    AetherChain chain0;
    chain0.setSampleRate(sr);
    chain0.applyParamsRT(d0);
    std::vector<float> P = x, P2 = x;
    runBlocks([&](float* l, float* r, size_t m) { chain0.process(l, r, m); }, P, P2, block);
    double ep = 0, ea = 0;
    for (size_t i = from; i < x.size(); ++i) { ep += static_cast<double>(P[i]) * P[i]; ea += static_cast<double>(L[i]) * L[i]; }
    CHECK(std::abs(10.0 * std::log10(ea / ep)) < 0.3, "EQ A/B: az eredeti hangereje a feldolgozotthoz képest %.2f dB",
          10.0 * std::log10(ea / ep));

    // Bass Enhancer és Mono: A/B-ben a kimenet a bemenet skálázott másolata (késleltetés nélkül), a feldolgozott hangerején
    auto abRun = [&](uint32_t flag, bool compare) {
        ParamData p = transparentParams();
        p.set(flag, true);
        if (flag == kFlagBass) { p.bassAmountDb = 12.0f; p.bassCompare = compare ? 1 : 0; }
        else { p.monoAmount = 100.0f; p.monoFreqHz = 150.0f; p.monoCompare = compare ? 1 : 0; }
        AetherChain ch;
        ch.setSampleRate(sr);
        ch.applyParamsRT(p);
        std::vector<float> A = x, B(x.size());
        for (size_t i = 0; i < x.size(); ++i) B[i] = 0.3f * x[(i + 4800) % x.size()];   // a jobb csatorna más: van oldalsó jel
        runBlocks([&](float* l, float* r, size_t m) { ch.process(l, r, m); }, A, B, block);
        return A;
    };
    for (uint32_t flag : { static_cast<uint32_t>(kFlagBass), static_cast<uint32_t>(kFlagMono) }) {
        const char* name = flag == kFlagBass ? "Bass Enhancer" : "Mono";
        const std::vector<float> proc = abRun(flag, false), orig = abRun(flag, true);
        double sxy = 0, sxx = 0;
        for (size_t i = from; i < x.size(); ++i) { sxy += static_cast<double>(orig[i]) * x[i]; sxx += static_cast<double>(x[i]) * x[i]; }
        const double k = sxy / sxx;
        double r2 = 0, t2 = 0, e1 = 0, e2 = 0;
        for (size_t i = from; i < x.size(); ++i) {
            r2 += (orig[i] - k * x[i]) * (orig[i] - k * x[i]);
            t2 += static_cast<double>(orig[i]) * orig[i];
            e1 += static_cast<double>(proc[i]) * proc[i];
            e2 += static_cast<double>(orig[i]) * orig[i];
        }
        double dp = 0;   // a feldolgozott jel valóban eltér a bemenettől (különben a teszt semmit nem mérne)
        for (size_t i = from; i < x.size(); ++i) dp += (proc[i] - x[i]) * (proc[i] - x[i]);
        // A szintkiegyenlítés lassan követ (300 ms), ezért a Bass Enhancernél a skálázás nem teljesen állandó: a maradék
        // így is két nagyságrenddel kisebb, mint a feldolgozott jel eltérése a bemenettől
        CHECK(r2 / t2 < 1e-3 && dp / sxx > 100.0 * r2 / t2 && std::abs(10.0 * std::log10(e2 / e1)) < 0.3,
              "%s A/B: a kimenet a bemenet %.3f-szorosa (maradék %.1e; a feldolgozotté %.1e), hangerő-eltérés a feldolgozotthoz %.2f dB",
              name, k, r2 / t2, dp / sxx, 10.0 * std::log10(e2 / e1));
    }
}

/** EQ késleltetés lineáris fázisú módban: a jelentett érték a valódi (az impulzusválasz csúcsa), Decramp mellett is. */
static void testEqLatency() {
    std::printf("[15] EQ késleltetés (FIR / FFT, Decramp mellett is) és az A/B időbeli illeszkedése\n");
    const uint32_t sr = 48000;
    const int32_t apo = static_cast<int32_t>(EqFilterMode::ApoDr);
    const char* decName[4] = { "Off", "x2", "x3", "x4" };
    auto peakOf = [&](const ParamData& d, size_t& lat) {
        ee::Equalizer eq;
        eq.setSampleRate(sr);
        eq.update(d);
        std::vector<float> z(480, 0.0f), z2(480, 0.0f);
        for (int i = 0; i < 100; ++i) eq.process(z.data(), z2.data(), 480);
        lat = eq.latency();
        const size_t n = 1u << 14;
        std::vector<float> L(n, 0.0f), R(n, 0.0f);
        L[0] = R[0] = 1.0f;
        runBlocks([&](float* l, float* r, size_t m) { eq.process(l, r, m); }, L, R, 480);
        size_t p = 0;
        for (size_t i = 1; i < n; ++i) if (std::abs(L[i]) > std::abs(L[p])) p = i;
        return p;
    };
    for (int dc = 0; dc < 4; ++dc) {
        size_t lat = 0;
        const size_t p = peakOf(bellParams(apo, 1, dc), lat);
        CHECK(lat == p, "FIR, Decramp %s: jelentett késleltetés %zu minta, az impulzusválasz csúcsa %zu", decName[dc], lat, p);
    }
    size_t latFft = 0;
    const size_t pFft = peakOf(bellParams(apo, 2, 0), latFft);
    CHECK(latFft == pFft, "FFT: jelentett késleltetés %zu minta, az impulzusválasz csúcsa %zu", latFft, pFft);

    // FIR + Decramp x4: az A/B-ben szóló eredeti jel ugyanannyit késik, mint a feldolgozott (nincs időugrás váltáskor)
    std::mt19937 rng(15);
    std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    std::vector<float> x(2 * sr);
    for (auto& v : x) v = u(rng);
    auto run = [&](bool compare) {
        ParamData d = bellParams(apo, 1, 3);
        d.eqCompare = compare ? 1 : 0;
        AetherChain chain;
        chain.setSampleRate(sr);
        chain.applyParamsRT(d);
        std::vector<float> L = x, R = x;
        runBlocks([&](float* l, float* r, size_t m) { chain.process(l, r, m); }, L, R, 480);
        return L;
    };
    const std::vector<float> proc = run(false), orig = run(true);
    const long ahead = bestLag(orig, proc, sr, 3000), behind = bestLag(proc, orig, sr, 3000);
    CHECK(ahead == 0 && behind == 0, "FIR x4 A/B: az eredeti és a feldolgozott jel között %ld / %ld minta eltolás (várt 0)",
          ahead, behind);
}

/** Rövid távú (3 s-os) EBU R128 hangosság a jel végén: a libebur128 referencia-mérése. */
static double shortTermLufs(const std::vector<float>& L, const std::vector<float>& R, size_t from, double sr) {
    ebur128_state* st = ebur128_init(2, static_cast<unsigned long>(sr), EBUR128_MODE_S);
    const size_t frames = L.size() - from;
    std::vector<float> il(2 * frames);
    for (size_t i = 0; i < frames; ++i) { il[2 * i] = L[from + i]; il[2 * i + 1] = R[from + i]; }
    ebur128_add_frames_float(st, il.data(), frames);
    double out = 0.0;
    ebur128_loudness_shortterm(st, &out);
    ebur128_destroy(&st);
    return out;
}

/** Az Autogain-lánc futtatása; a gain nyomvonala blokkonként (480 minta). */
static void runAutogain(const std::vector<float>& in, double target, double maxDb, std::vector<float>& L,
                        std::vector<float>& R, std::vector<double>* trace) {
    AetherChain chain;
    chain.setSampleRate(48000.0);
    ParamData d = transparentParams();
    d.set(kFlagAutogain, true);
    d.agcTargetLufs = static_cast<float>(target);
    d.agcMaxGainDb = static_cast<float>(maxDb);
    chain.applyParamsRT(d);
    L = in;
    R = in;
    for (size_t off = 0; off < L.size(); off += 480) {
        const size_t m = std::min<size_t>(480, L.size() - off);
        chain.process(L.data() + off, R.data() + off, m);
        if (trace) trace->push_back(chain.autogainDb());
    }
}

static void testAutogain() {
    std::printf("[14] Autogain (EBU R128 rövid távú hangosság, libebur128)\n");
    const double sr = 48000.0;
    // Referencia: 1 kHz-es szinusz, A = 0,1 (−20 dBFS), mindkét csatornán. BS.1770 szerint
    // L = −0,691 + 10·log10(2 · A²/2 · |H_K(1 kHz)|²), ahol H_K a K-súlyozás (két biquad, 48 kHz-es együtthatók).
    const std::complex<double> zi = std::polar(1.0, -2.0 * kPi * 1000.0 / 48000.0);   // z⁻¹ az 1 kHz-en
    auto biquad = [&](double b0, double b1, double b2, double a1, double a2) {
        return (b0 + b1 * zi + b2 * zi * zi) / (1.0 + a1 * zi + a2 * zi * zi);
    };
    const std::complex<double> kGain = biquad(1.53512485958697, -2.69169618940638, 1.19839281085285,
                                              -1.69065929318241, 0.73248077421585)     // 1. szakasz: magasárkád
                                     * biquad(1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621);   // 2. szakasz: RLB
    const double kDb = 20.0 * std::log10(std::abs(kGain));
    const double refLufs = -0.691 + 20.0 * std::log10(0.1) + kDb;
    const size_t n = static_cast<size_t>(12.0 * sr);
    const std::vector<float> tone = sine(1000.0, 0.1, n, sr);
    const double inLufs = shortTermLufs(tone, tone, n - static_cast<size_t>(3.0 * sr), sr);
    CHECK(std::abs(inLufs - refLufs) < 0.05, "libebur128 referencia: %.2f LUFS (BS.1770 számítás: %.2f LUFS, K-súlyozás 1 kHz-en %+.2f dB)",
          inLufs, refLufs, kDb);

    // Célra igazítás: a gain a cél és a bemenet különbsége, a kimenet hangossága a célszinten (−18 LUFS) áll.
    std::vector<float> L, R;
    std::vector<double> trace;
    runAutogain(tone, -18.0, 6.0, L, R, &trace);
    const double outLufs = shortTermLufs(L, R, n - static_cast<size_t>(3.0 * sr), sr);
    const double expGain = -18.0 - refLufs;
    CHECK(std::abs(outLufs + 18.0) < 0.2 && std::abs(trace.back() - expGain) < 0.1,
          "célszint: kimenet %.2f LUFS (cél −18), gain %.2f dB (várt %+.2f dB)", outLufs, trace.back(), expGain);

    // Halk jel (−40 dBFS): a gain a +6 dB-es határig nő, legfeljebb 1 dB/s sebességgel
    const std::vector<float> quiet = sine(1000.0, 0.01, n, sr);
    runAutogain(quiet, -18.0, 6.0, L, R, &trace);
    const double at5 = trace[499];   // t = 5,0 s (a mérés 3 s után indul, így kb. +2 dB)
    CHECK(std::abs(trace.back() - 6.0) < 0.05 && std::abs(at5 - 2.0) < 0.3,
          "halk jel: gain 5 s-nál %.2f dB (várt kb. +2), a végén %.2f dB (a határ +6 dB)", at5, trace.back());

    // Csend: amint a 3 s-os ablak teljesen néma, a gain megáll
    std::vector<float> gap = sine(1000.0, 0.1, static_cast<size_t>(5.0 * sr), sr);
    gap.resize(n, 0.0f);
    runAutogain(gap, -18.0, 6.0, L, R, &trace);
    const double at8 = trace[799], at10 = trace[999];
    CHECK(std::abs(at10 - at8) < 0.01, "csend után a gain nem változik (8 s: %.3f dB, 10 s: %.3f dB)", at8, at10);

    // A bekapcsolás utáni első 3 s-ban (mérés nélkül) a jel bit-pontosan átmegy
    std::vector<float> early;
    runAutogain(tone, -18.0, 6.0, early, R, nullptr);
    bool exact = true;
    for (size_t i = 2400; i < 2 * 48000 && exact; ++i) exact = early[i] == tone[i];
    CHECK(exact, "bekapcsolás után az első 2 s bit-pontosan átmegy (a mérés még nem indult)");

    ParamData bad = transparentParams();
    bad.agcTargetLufs = -50.0f;
    bad.agcMaxGainDb = 40.0f;
    sanitizeParams(bad);
    CHECK(bad.agcTargetLufs == -30.0f && bad.agcMaxGainDb == 12.0f, "érvénytelen célszint és határ a tartományba szorítva");

    // A v9 paraméterfájl (Autogain nélkül) betöltése: a modul kikapcsolva, az alapértékek érvényesek
    ParamData old9 = transparentParams();
    old9.agcTargetLufs = -23.0f;   // a v9-ben nincs mező: ez nem kerülhet a fájlba
    std::vector<char> bytes(sizeof(ParamData));
    std::memcpy(bytes.data(), &old9, sizeof(ParamData));
    ParamData loaded{};
    const bool ok = loadParamBytes(bytes.data(), kParamSizeV9, 9, static_cast<uint32_t>(kParamSizeV9), loaded);
    CHECK(ok && !loaded.has(kFlagAutogain) && loaded.agcTargetLufs == -18.0f && loaded.agcMaxGainDb == 6.0f,
          "v9 paraméterfájl betöltése: Autogain ki, célszint −18 LUFS, határ 6 dB");

    // A v10 paraméterfájl (a mono határfrekvencia és a Bass / Mono A/B nélkül): 100 Hz (mint addig), A/B ki
    ParamData old10 = transparentParams();
    old10.agcTargetLufs = -23.0f;   // ez már a v10 része
    old10.monoFreqHz = 200.0f;      // ez nem: a v10-ben nincs ilyen mező
    old10.bassCompare = 1;
    std::memcpy(bytes.data(), &old10, sizeof(ParamData));
    ParamData loaded10{};
    const bool ok10 = loadParamBytes(bytes.data(), kParamSizeV10, 10, static_cast<uint32_t>(kParamSizeV10), loaded10);
    CHECK(ok10 && loaded10.agcTargetLufs == -23.0f && loaded10.monoFreqHz == 100.0f && loaded10.bassCompare == 0 &&
          loaded10.monoCompare == 0, "v10 paraméterfájl betöltése: célszint megmarad, mono határ 100 Hz, Bass / Mono A/B ki");
}

// IR-profilok: a tár fájlformátuma (ir.bin, 'AIRF') és a GUI "AEIS" üzenete
static void testIrLibraryFormat() {
    std::printf("\n[IR-tár] fájlformátum és mentési üzenet\n");
    constexpr uint32_t kMaxFrames = 262144;   // ws_bridge.hpp: kBridgeMaxIrFrames
    IrData ir;
    ir.sampleRate = 48000;
    ir.name = "Terem ő.wav";
    ir.left = { 1.0f, 0.5f, -0.25f, 0.0f };
    ir.right = { 0.9f, std::nanf(""), 0.1f, 0.0f };
    IrData back;
    const std::string bytes = encodeIrFile(ir);
    CHECK(decodeIrFile(bytes, back, kMaxFrames) && back.name == ir.name && back.sampleRate == 48000 &&
          back.channels() == 2 && back.frames() == 4 && back.left == ir.left && back.right[0] == 0.9f && back.right[1] == 0.0f,
          "AIRF oda-vissza: név, mintavétel, 2 csatorna, minták (NaN -> 0)");
    IrData mono = ir;
    mono.right.clear();
    CHECK(decodeIrFile(encodeIrFile(mono), back, kMaxFrames) && back.channels() == 1 && back.left == ir.left,
          "AIRF oda-vissza: mono");
    CHECK(!decodeIrFile(bytes.substr(0, bytes.size() - 1), back, kMaxFrames) && !decodeIrFile(bytes, back, 3) &&
          !decodeIrFile("", back, kMaxFrames),
          "csonka fájl, túl hosszú IR és üres fájl elutasítva");

    CHECK(isValidIrId("0123456789abcdef") && !isValidIrId("") && !isValidIrId("../x") && !isValidIrId("ABC") &&
          !isValidIrId(std::string(33, 'a')) && isValidIrId(std::string(32, 'a')),
          "azonosító: csak 0-9 és a-z, 1-32 karakter (fájlnévként biztonságos)");

    // "AEIS" | 1 | azonosítóhossz | azonosító | "AEIR" | 1 | sr | csatornák | frame-ek | névhossz | név | minták
    auto u32 = [](std::string& s, uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); };
    auto storeMsg = [&](const std::string& id, uint32_t ver) {
        std::string aeir = "AEIR";
        u32(aeir, 1); u32(aeir, 44100); u32(aeir, 1); u32(aeir, 2); u32(aeir, 2);
        aeir += "ab";
        const float smp[2] = { 0.25f, -0.5f };
        aeir.append(reinterpret_cast<const char*>(smp), sizeof(smp));
        std::string m = "AEIS";
        u32(m, ver); u32(m, static_cast<uint32_t>(id.size()));
        return m + id + aeir;
    };
    std::string id;
    IrData got;
    CHECK(parseIrStoreMessage(storeMsg("a1b2", 1), id, got, kMaxFrames) && id == "a1b2" && got.name == "ab" &&
          got.sampleRate == 44100 && got.frames() == 2 && got.left[1] == -0.5f,
          "AEIS üzenet: azonosító és IR beolvasva");
    CHECK(!parseIrStoreMessage(storeMsg("../a", 1), id, got, kMaxFrames) &&
          !parseIrStoreMessage(storeMsg("a1b2", 2), id, got, kMaxFrames) &&
          !parseIrStoreMessage(storeMsg("a1b2", 1).substr(0, 30), id, got, kMaxFrames),
          "AEIS üzenet: rossz azonosító, ismeretlen verzió és csonka üzenet elutasítva");
}

/** Kimeneti fokozat: csatornánkénti késleltetés / szint / polaritás és dither; semleges beállításnál bitpontos. */
static void testOutStage() {
    std::printf("\n[Kimeneti fokozat]\n");
    const size_t n = 48000;
    auto run = [&](const ParamData& d, std::vector<float>& L, std::vector<float>& R) {
        AetherChain chain;
        chain.setSampleRate(48000);
        chain.applyParamsRT(d);
        std::vector<float> zl(4800, 0.0f), zr(4800, 0.0f);
        for (int k = 0; k < 3; ++k) chain.process(zl.data(), zr.data(), zl.size());   // az úsztatott bekapcsolás lezajlik
        for (size_t off = 0; off < L.size(); off += 512) chain.process(L.data() + off, R.data() + off, std::min<size_t>(512, L.size() - off));
    };
    // 1. semleges csatornák + dither, más modul nélkül: a jel érintetlen (bitpontos)
    std::vector<float> L(n), R(n), L0, R0;
    for (size_t i = 0; i < n; ++i) { L[i] = 0.3f * std::sin(0.013f * i) + 1e-7f * (i % 7); R[i] = 0.2f * std::cos(0.021f * i); }
    L0 = L; R0 = R;
    ParamData d = transparentParams();
    d.set(kFlagOutStage, true);
    d.ditherMode = 1;
    d.ditherBits = 16;
    run(d, L, R);
    size_t firstDiff = n;
    for (size_t i = 0; i < n && firstDiff == n; ++i) if (L[i] != L0[i] || R[i] != R0[i]) firstDiff = i;
    CHECK(firstDiff == n, "semleges csatornák és dither: a kimenet bitpontosan azonos a bemenettel (első eltérés: %zu, %.9g / %.9g)",
          firstDiff, firstDiff < n ? L[firstDiff] : 0.0f, firstDiff < n ? L0[firstDiff] : 0.0f);
    // 2. késleltetés: 1 ms a bal csatornán -> az impulzus 48 mintával később
    std::vector<float> iL(n, 0.0f), iR(n, 0.0f);
    iL[1000] = 0.5f; iR[1000] = 0.5f;
    d = transparentParams();
    d.set(kFlagOutStage, true);
    d.chDelayMs[0] = 1.0f;
    d.chGainDb[1] = -6.0f;
    d.chInvert[1] = 1;
    run(d, iL, iR);
    const size_t pl = static_cast<size_t>(std::max_element(iL.begin(), iL.end(), [](float a, float b) { return std::abs(a) < std::abs(b); }) - iL.begin());
    const size_t pr = static_cast<size_t>(std::max_element(iR.begin(), iR.end(), [](float a, float b) { return std::abs(a) < std::abs(b); }) - iR.begin());
    CHECK(pl == 1048 && std::abs(iL[pl] - 0.5f) < 1e-6f, "bal csatorna 1 ms késleltetés: az impulzus a %zu. mintán (várt 1048)", pl);
    CHECK(pr == 1000 && std::abs(iR[pr] + 0.5f * 0.501187f) < 1e-4f, "jobb csatorna -6 dB, fordítva: %.4f (várt -0,2506)", iR[pr]);
    // 3. dither 16 bitre: módosított jelnél a minták a 16 bites rácson vannak, a zaj kicsi
    for (int mode = 1; mode <= 2; ++mode) {
        std::vector<float> a(n), b(n);
        for (size_t i = 0; i < n; ++i) { a[i] = 0.25f * std::sin(0.01f * i); b[i] = 0.0f; }
        d = transparentParams();
        d.set(kFlagOutStage, true);
        d.chGainDb[0] = d.chGainDb[1] = -1.0f;
        d.ditherMode = mode;
        d.ditherBits = 16;
        run(d, a, b);
        bool grid = true;
        double e2 = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const double q = a[i] * 32768.0, qb = b[i] * 32768.0;
            if (std::abs(q - std::round(q)) > 1e-3 || std::abs(qb - std::round(qb)) > 1e-3) grid = false;
            e2 += qb * qb;
        }
        const double rmsLsb = std::sqrt(e2 / n);
        CHECK(grid && rmsLsb > 0.2 && rmsLsb < (mode == 1 ? 1.0 : 2.0),
              "%s dither 16 bit: a minták a 16 bites rácson, a csend zaja %.2f LSB (RMS)", mode == 1 ? "TPDF" : "zajformált", rmsLsb);
    }
    // 4. a régi (v11) paraméterfájl betöltése: a kimeneti fokozat alapértéken (ki)
    ParamData v = transparentParams();
    v.set(kFlagEq, true);
    v.chDelayMs[0] = 5.0f;   // a v11-es méretben ez a mező nincs benne
    ParamData out;
    const bool ok = loadParamBytes(reinterpret_cast<const char*>(&v), sizeof(v), 11, kParamSizeV11, out);
    CHECK(ok && out.has(kFlagEq) && !out.has(kFlagOutStage) && out.chDelayMs[0] == 0.0f && out.ditherBits == 24,
          "v11-es params.bin: betöltve, a kimeneti fokozat alapértéken");
}

/** A kimeneti hangerő (setOutputGain; a Windows-lejátszó kizárólagos módja): 1-nél bitpontos, különben pontos szorzó. */
static void testOutputGain() {
    std::printf("\n--- Kimeneti hangerő (kizárólagos mód, digitális hangerő) ---\n");
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> u(-0.9f, 0.9f);
    auto noise = [&](std::vector<float>& L, std::vector<float>& R) {
        for (auto& v : L) v = u(rng);
        for (auto& v : R) v = u(rng);
    };
    AetherChain chain;
    chain.setSampleRate(48000.0);
    chain.applyParamsRT(transparentParams());
    std::vector<float> L(9600), R(9600);
    noise(L, R);
    std::vector<float> L0 = L, R0 = R;
    runBlocks([&](float* l, float* r, size_t m) { chain.process(l, r, m); }, L, R, 480);
    CHECK(L == L0 && R == R0, "1-es hangerő: a kimenet bitpontosan a bemenet");

    chain.setOutputGain(0.5f);
    noise(L, R);
    L0 = L;
    R0 = R;
    runBlocks([&](float* l, float* r, size_t m) { chain.process(l, r, m); }, L, R, 480);
    bool half = true, ramp = std::abs(L[0]) > std::abs(0.5f * L0[0]) || L0[0] == 0.0f;
    for (size_t i = 480; i < L.size(); ++i)
        if (L[i] != 0.5f * L0[i] || R[i] != 0.5f * R0[i]) half = false;
    CHECK(half && ramp, "0,5-ös hangerő: az első blokkban úszik, utána pontosan fele");

    ParamData d = transparentParams();
    d.set(kFlagOutStage, true);
    d.ditherMode = 1;
    d.ditherBits = 16;
    chain.applyParamsRT(d);
    noise(L, R);
    runBlocks([&](float* l, float* r, size_t m) { chain.process(l, r, m); }, L, R, 480);
    bool grid = true;
    for (size_t i = 4800; i < L.size(); ++i) {
        const double q = L[i] * 32768.0;
        if (std::abs(q - std::round(q)) > 1e-3) grid = false;
    }
    CHECK(grid, "csökkentett hangerő + dither 16 bit: a minták a 16 bites rácson (a lánc egyébként érintetlen)");

    chain.applyParamsRT(transparentParams());
    chain.setOutputGain(1.0f);
    noise(L, R);
    runBlocks([&](float* l, float* r, size_t m) { chain.process(l, r, m); }, L, R, 480);   // úszás vissza 1-re
    noise(L, R);
    L0 = L;
    R0 = R;
    runBlocks([&](float* l, float* r, size_t m) { chain.process(l, r, m); }, L, R, 480);
    CHECK(L == L0 && R == R0, "vissza 1-re: ismét bitpontos");
}

/**
 * float → egész PCM (pcm_convert.hpp; a Windows-lejátszó kizárólagos módja): bájtsorrend, a 24 bites minta helye a
 * 32 bites keretben, kerekítés, vágás, és hogy a rácson lévő minta (pl. dekódolt 16 bites fájl) bitpontosan megy át.
 */
static void testPcmConvert() {
    std::printf("\n--- float → egész PCM (kizárólagos mód kimenete) ---\n");
    auto rd16 = [](const uint8_t* b, size_t i) { int16_t s; std::memcpy(&s, b + i * 2, 2); return static_cast<long>(s); };
    auto rd24 = [](const uint8_t* b, size_t i) {
        int32_t s = b[i * 3] | (b[i * 3 + 1] << 8) | (b[i * 3 + 2] << 16);
        if (s & 0x800000) s -= 0x1000000;
        return static_cast<long>(s);
    };
    auto rd32 = [](const uint8_t* b, size_t i) { int32_t s; std::memcpy(&s, b + i * 4, 4); return static_cast<long long>(s); };

    // 1. ismert értékek, mind a négy formátumban
    const float x[] = { 0.0f, 0.5f, -0.5f, -1.0f, 1.0f, 1.0f / 32768.0f, -1.0f / 32768.0f };
    const size_t n = sizeof(x) / sizeof(x[0]);
    uint8_t b[4 * 16];
    bool ex = pcm::floatToInt(x, b, n, 16, 16);
    CHECK(rd16(b, 0) == 0 && rd16(b, 1) == 16384 && rd16(b, 2) == -16384 && rd16(b, 3) == -32768 && rd16(b, 4) == 32767 &&
          rd16(b, 5) == 1 && rd16(b, 6) == -1 && !ex,
          "16 bit: 0 / ±0,5 / -1 / +1 (vágva 32767-re) / ±1 LSB helyes, a vágás miatt nem bitpontos");
    ex = pcm::floatToInt(x, b, n, 24, 24);
    CHECK(rd24(b, 1) == 4194304 && rd24(b, 2) == -4194304 && rd24(b, 3) == -8388608 && rd24(b, 4) == 8388607 &&
          rd24(b, 5) == 256 && b[3] == 0x00 && b[4] == 0x00 && b[5] == 0x40 && !ex,
          "24 bit (3 bájt): értékek és kis-endián bájtsorrend (0,5 = 00 00 40)");
    ex = pcm::floatToInt(x, b, n, 32, 24);
    CHECK(rd32(b, 1) == 0x40000000LL && rd32(b, 3) == -2147483648LL && rd32(b, 4) == 0x7FFFFF00LL && rd32(b, 5) == 256LL * 256 &&
              (rd32(b, 5) & 0xFF) == 0 && !ex,
          "24 bit 32 bites keretben: a minta a felső 24 biten, az alsó bájt 0");
    ex = pcm::floatToInt(x, b, n, 32, 32);
    CHECK(rd32(b, 1) == 0x40000000LL && rd32(b, 3) == -2147483648LL && rd32(b, 4) == 2147483647LL && rd32(b, 5) == 65536 && !ex,
          "32 bit: ±0,5, -1, +1 (vágva), 1/32768 = 65536");

    // 2. kerekítés a legközelebbire, és a nem rácsra eső érték jelzése
    const float r[] = { 0.3f };
    ex = pcm::floatToInt(r, b, 1, 16, 16);
    CHECK(rd16(b, 0) == std::lround(0.3f * 32768.0) && !ex, "16 bit: 0,3 → %ld (legközelebbi), nem bitpontos", rd16(b, 0));

    // 3. dekódolt 16 és 24 bites hang: minden formátumban bitpontosan, a vágás határain is
    std::mt19937 rng(5);
    std::uniform_int_distribution<int> u16(-32768, 32767), u24(-8388608, 8388607);
    std::vector<float> s16(4096), s24(4096);
    for (size_t i = 0; i < s16.size(); ++i) {
        s16[i] = static_cast<float>(u16(rng)) / 32768.0f;
        s24[i] = static_cast<float>(u24(rng)) / 8388608.0f;
    }
    s16[0] = -1.0f;                       // -32768
    s16[1] = 32767.0f / 32768.0f;         // a legnagyobb pozitív kód
    s24[0] = -1.0f;
    s24[1] = 8388607.0f / 8388608.0f;
    std::vector<uint8_t> o(s16.size() * 4);
    bool ok16 = pcm::floatToInt(s16.data(), o.data(), s16.size(), 16, 16);
    for (size_t i = 0; i < s16.size(); ++i) ok16 = ok16 && rd16(o.data(), i) == std::lround(s16[i] * 32768.0);
    bool ok16in32 = pcm::floatToInt(s16.data(), o.data(), s16.size(), 32, 32);
    for (size_t i = 0; i < s16.size(); ++i) ok16in32 = ok16in32 && rd32(o.data(), i) == std::llround(s16[i] * 32768.0) * 65536;
    bool ok24 = pcm::floatToInt(s24.data(), o.data(), s24.size(), 24, 24);
    for (size_t i = 0; i < s24.size(); ++i) ok24 = ok24 && rd24(o.data(), i) == std::lround(s24[i] * 8388608.0);
    bool ok24in32 = pcm::floatToInt(s24.data(), o.data(), s24.size(), 32, 24);
    for (size_t i = 0; i < s24.size(); ++i) ok24in32 = ok24in32 && rd32(o.data(), i) == std::llround(s24[i] * 8388608.0) * 256;
    CHECK(ok16 && ok16in32, "16 bites hang → 16 és 32 bit: minden minta bitpontosan (a -32768 és 32767 határon is)");
    CHECK(ok24 && ok24in32, "24 bites hang → 24 és 24/32 bit: minden minta bitpontosan (a határokon is)");
    const bool lossy = pcm::floatToInt(s24.data(), o.data(), s24.size(), 16, 16);
    CHECK(!lossy, "24 bites hang → 16 bit: nem bitpontosként jelezve (a jelút ezt mutatja)");
}

static double computeVendorRefDb(const lsp::freq_curve_t& c, double volumeDb, double freqHz) {
    const double PHONS_MIN = -83.0;
    const double vol = std::clamp(volumeDb - PHONS_MIN, static_cast<double>(c.amin), static_cast<double>(c.amax)) - c.amin;
    const double range = c.amax - c.amin;
    const double step = range / (c.curves - 1);
    int nc = static_cast<int>(vol / step);
    if (nc >= static_cast<int>(c.curves - 1)) nc = static_cast<int>(c.curves - 2);
    const double frac = (vol / step - nc);

    const double rfmin = 1.0 / c.fmin;
    const double df = std::log(c.fmax * rfmin);
    const double kdf = (c.hdots - 1) / df;

    const int16_t* a = c.data[nc];
    const int16_t* b = c.data[nc + 1];

    if (freqHz <= c.fmin) {
        return ((1.0 - frac) * a[0] + frac * b[0]) / 100.0;
    }
    if (freqHz >= c.fmax) {
        return ((1.0 - frac) * a[c.hdots - 1] + frac * b[c.hdots - 1]) / 100.0;
    }

    const double fpos = std::log(freqHz * rfmin) * kdf;
    int idx = static_cast<int>(fpos);
    if (idx < 0) idx = 0;
    if (idx >= static_cast<int>(c.hdots - 1)) idx = static_cast<int>(c.hdots - 2);
    const double ffrac = fpos - idx;

    const double valA = (1.0 - frac) * a[idx] + frac * b[idx];
    const double valB = (1.0 - frac) * a[idx + 1] + frac * b[idx + 1];
    const double valCentibels = (1.0 - ffrac) * valA + ffrac * valB;
    return valCentibels / 100.0;
}

static void testLoudness() {
    std::printf("\n--- Loudness Compensator (ISO 226, LSP loud_comp) ---\n");
    const double sr = 48000.0;

    // 1. Késleltetés ellenőrzése minden módban
    {
        ee::Loudness loud;
        loud.setSampleRate(48000);
        ParamData d = transparentParams();
        d.set(kFlagLoudness, true);

        // IIR mód: 0 minta késleltetés
        d.loudMode = 0;
        loud.update(d);
        CHECK(loud.latency() == 0, "IIR mód késleltetése pontosan 0 minta (Zero Latency)");

        // FFT módok (256, 512, 1024, 2048, 4096, 8192, 16384)
        const size_t expectedRanks[7] = { 256, 512, 1024, 2048, 4096, 8192, 16384 };
        for (int m = 1; m <= 7; ++m) {
            d.loudMode = m;
            loud.update(d);
            CHECK(loud.latency() == expectedRanks[m - 1],
                  "FFT mód %d (%zu minta) késleltetése pontosan %zu minta", m, expectedRanks[m - 1], loud.latency());
        }
    }

    // 2. Referencia-egyezés teszt: a várt érték ugyanazokból a vendorolt LSP-táblákból jön (include/generated/iso226/*.h)
    {
        struct StdRef {
            int stdIdx;
            const char* name;
            const lsp::freq_curve_t* curve;
        };
        const StdRef standards[4] = {
            { 1, "ISO 226:2003", &lsp::iso226_2003_curve },
            { 2, "Fletcher-Munson", &lsp::fletcher_munson_curve },
            { 3, "Robinson-Dadson", &lsp::robinson_dadson_curve },
            { 4, "ISO 226:2023", &lsp::iso226_2023_curve }
        };
        const double testFreqs[4] = { 50.0, 100.0, 1000.0, 10000.0 };
        const double testVols[2] = { -30.0, -10.0 };

        for (const auto& s : standards) {
            for (double vol : testVols) {
                for (double f : testFreqs) {
                    const double expectedDb = computeVendorRefDb(*s.curve, vol, f);

                    // A) FFT 4096 mód mérése (szigorú ±0.10 dB tűrés)
                    double fftMeasuredDb = 0.0;
                    {
                        ee::Loudness loudFft;
                        loudFft.setSampleRate(48000);
                        ParamData d = transparentParams();
                        d.set(kFlagLoudness, true);
                        d.loudStd = s.stdIdx;
                        d.loudMode = 7; // FFT 16384 (maximális felbontás a vendor referencia összevetéshez)
                        d.loudVolumeDb = static_cast<float>(vol);
                        loudFft.update(d);

                        std::vector<float> L = sine(f, 0.1, 96000, sr), R = L;
                        runBlocks([&](float* l, float* r, size_t m) { loudFft.process(l, r, m); }, L, R, 512);
                        fftMeasuredDb = 20.0 * std::log10(toneAmp(L, 48000, f, sr) / 0.1);
                    }
                    const double fftDiff = std::abs(fftMeasuredDb - expectedDb);
                    CHECK(fftDiff <= 0.10,
                          "Vendor ref FFT [%s, vol %.0f dB, %.0f Hz]: várt %+.2f dB, mért %+.2f dB (eltérés %.2f dB <= 0.10 dB)",
                          s.name, vol, f, expectedDb, fftMeasuredDb, fftDiff);

                    // B) IIR mód mérése (64 sávos Linkwitz-Riley átfedési ripple miatt indokolt ±1.50 dB tűrés)
                    double iirMeasuredDb = 0.0;
                    {
                        ee::Loudness loudIir;
                        loudIir.setSampleRate(48000);
                        ParamData d = transparentParams();
                        d.set(kFlagLoudness, true);
                        d.loudStd = s.stdIdx;
                        d.loudMode = 0; // IIR mód
                        d.loudApprox = 2; // Normal (32 szűrő, alapérték)
                        d.loudVolumeDb = static_cast<float>(vol);
                        loudIir.update(d);

                        std::vector<float> L = sine(f, 0.1, 96000, sr), R = L;
                        runBlocks([&](float* l, float* r, size_t m) { loudIir.process(l, r, m); }, L, R, 512);
                        iirMeasuredDb = 20.0 * std::log10(toneAmp(L, 48000, f, sr) / 0.1);
                    }
                    const double iirDiff = std::abs(iirMeasuredDb - expectedDb);
                    CHECK(iirDiff <= 1.00,
                          "Vendor ref IIR Normal [%s, vol %.0f dB, %.0f Hz]: várt %+.2f dB, mért %+.2f dB (eltérés %.2f dB <= 1.00 dB)",
                          s.name, vol, f, expectedDb, iirMeasuredDb, iirDiff);
                }
            }
        }

        // Flat szabvány (loudStd = 0): minden frekvencián a várt szint a beállított hangerő (volumeDb)
        for (double vol : { -30.0, -10.0 }) {
            for (double f : testFreqs) {
                ee::Loudness loudFlat;
                loudFlat.setSampleRate(48000);
                ParamData d = transparentParams();
                d.set(kFlagLoudness, true);
                d.loudStd = 0; // Flat
                d.loudMode = 5; // FFT 4096 (alapérték)
                d.loudVolumeDb = static_cast<float>(vol);
                loudFlat.update(d);

                std::vector<float> L = sine(f, 0.1, 96000, sr), R = L;
                runBlocks([&](float* l, float* r, size_t m) { loudFlat.process(l, r, m); }, L, R, 512);
                const double g = 20.0 * std::log10(toneAmp(L, 48000, f, sr) / 0.1);
                CHECK(std::abs(g - vol) <= 0.05,
                      "Flat szabvány [vol %.0f dB, %.0f Hz]: várt %+.2f dB, mért %+.2f dB (eltérés %.2f dB <= 0.05 dB)",
                      vol, f, vol, g, std::abs(g - vol));
            }
        }
    }

    // 3. Kattanásmentesség és késleltetésváltás időbeli illeszkedése
    {
        for (size_t blk : { 256, 480, 1024 }) {
            AetherChain chain;
            chain.setSampleRate(sr);
            const size_t seg = 12000;
            std::vector<float> L = sine(1000.0, 0.5, seg * 24, sr), R = L;
            ParamData d = transparentParams();
            size_t pos = 0;
            double worstJump = 0.0;
            const char* worstStep = "";

            auto step = [&](const char* name) {
                std::vector<float> l(L.begin() + pos, L.begin() + pos + seg);
                std::vector<float> r(R.begin() + pos, R.begin() + pos + seg);
                chain.applyParamsRT(d);
                runBlocks([&](float* a, float* b, size_t m) { chain.process(a, b, m); }, l, r, blk);
                const double j = maxJump(l, 0, seg);
                if (j > worstJump) { worstJump = j; worstStep = name; }
                std::copy(l.begin(), l.end(), L.begin() + pos);
                pos += seg;
            };

            step("Alap állapot");
            d.set(kFlagLoudness, true); d.loudVolumeDb = -20.0f; d.loudStd = 1; d.loudMode = 0; d.loudApprox = 2; step("Loudness be (IIR Normal)");
            d.loudStd = 4; step("Szabvány -> ISO 226:2023");
            d.loudStd = 2; step("Szabvány -> Fletcher-Munson");
            d.loudStd = 3; step("Szabvány -> Robinson-Dadson");
            d.loudStd = 0; step("Szabvány -> Flat");
            d.loudStd = 1; d.loudMode = 1; step("Mód -> FFT 256");
            d.loudMode = 2; step("Mód -> FFT 512");
            d.loudMode = 3; step("Mód -> FFT 1024");
            d.loudMode = 4; step("Mód -> FFT 2048");
            d.loudMode = 5; step("Mód -> FFT 4096");
            d.loudMode = 6; step("Mód -> FFT 8192");
            d.loudMode = 7; step("Mód -> FFT 16384");
            d.loudMode = 5; step("Mód -> FFT 4096");
            d.loudMode = 0; d.loudApprox = 0; step("Mód -> IIR Fastest");
            d.loudApprox = 4; step("IIR -> Best");
            d.set(kFlagLoudness, false); step("Loudness ki");

            CHECK(worstJump < 0.03,
                  "Kattanásmentesség (blokk %zu): modul- és módváltások ugrás nélkül (legnagyobb ugrás %.4f @ %s)",
                  blk, worstJump, worstStep);
        }

        // Késleltetésváltás időbeli illeszkedése (IIR [0] -> FFT 4096 [4096]):
        {
            AetherChain chain;
            chain.setSampleRate(sr);
            ParamData d = transparentParams();
            d.set(kFlagLoudness, true);
            d.loudVolumeDb = -10.0f;
            d.loudStd = 4;
            d.loudMode = 0; // IIR (0 latency)
            chain.applyParamsRT(d);

            std::vector<float> a = sine(1000.0, 0.5, 48000, sr), b = a;
            runBlocks([&](float* x, float* y, size_t m) { chain.process(x, y, m); }, a, b, 512);

            // Váltás FFT 4096-ra (késleltetés 4096 minta)
            d.loudMode = 5;
            chain.applyParamsRT(d);
            std::vector<float> a2 = sine(1000.0, 0.5, 48000, sr), b2 = a2;
            runBlocks([&](float* x, float* y, size_t m) { chain.process(x, y, m); }, a2, b2, 512);

            const double j = maxJump(a2, 0, a2.size());
            CHECK(j < 0.03, "Loudness késleltetésváltás (IIR -> FFT 4096) időben illeszkedik, ugrás nélkül (legnagyobb ugrás %.4f)", j);
        }
    }

    // 4. Szélsőértékek tesztelése (sanitizeParams, néma és túlvezérelt jel)
    {
        ParamData d{};
        d.loudVolumeDb = std::numeric_limits<float>::quiet_NaN();
        d.loudInputDb = std::numeric_limits<float>::infinity();
        d.loudOutputDb = -std::numeric_limits<float>::infinity();
        d.loudStd = 999;
        d.loudMode = -100;
        d.loudHclip = 42;
        d.loudHclipRangeDb = std::numeric_limits<float>::quiet_NaN();
        d.loudCompare = -5;
        d.loudApprox = 99;
        sanitizeParams(d);

        CHECK(d.loudVolumeDb == 0.0f, "sanitizeParams: NaN hangerő -> alapérték (0.0 dB)");
        CHECK(d.loudInputDb == 36.0f, "sanitizeParams: +Inf bemeneti szint -> plafon (+36.0 dB)");
        CHECK(d.loudOutputDb == -36.0f, "sanitizeParams: -Inf kimeneti szint -> padló (-36.0 dB)");
        CHECK(d.loudStd == 4, "sanitizeParams: túl nagy szabvány-index -> 4 (ISO 226:2023)");
        CHECK(d.loudMode == 0, "sanitizeParams: negatív mód-index -> 0 (IIR)");
        CHECK(d.loudHclip == 1, "sanitizeParams: 42 hclip -> 1");
        CHECK(d.loudHclipRangeDb == 6.0f, "sanitizeParams: NaN hclip range -> alapérték (6.0 dB)");
        CHECK(d.loudCompare == 0, "sanitizeParams: negatív compare -> 0");
        CHECK(d.loudApprox == 4, "sanitizeParams: túl nagy approx -> 4 (Best)");
        d.loudMode = 99; sanitizeParams(d);
        CHECK(d.loudMode == 7, "sanitizeParams: túl nagy mode -> 7 (FFT 16384)");

        // Néma jel feldolgozása: kimenet bit-pontosan 0
        {
            ee::Loudness loud;
            loud.setSampleRate(48000);
            ParamData p = transparentParams();
            p.set(kFlagLoudness, true);
            loud.update(p);

            std::vector<float> L(4800, 0.0f), R(4800, 0.0f);
            runBlocks([&](float* l, float* r, size_t m) { loud.process(l, r, m); }, L, R, 256);
            bool allZero = true;
            for (float v : L) if (v != 0.0f) { allZero = false; break; }
            for (float v : R) if (v != 0.0f) { allZero = false; break; }
            CHECK(allZero, "Extrém teszt: néma bemenetre a kimenet bit-pontosan 0 marad");
        }

        // Túlvezérelt jel (amplitúdó = 10.0) clipping védelemmel
        {
            ee::Loudness loud;
            loud.setSampleRate(48000);
            ParamData p = transparentParams();
            p.set(kFlagLoudness, true);
            p.loudVolumeDb = 0.0f;
            p.loudHclip = 1;
            p.loudHclipRangeDb = 0.0f; // 0 dB térköz
            loud.update(p);

            std::vector<float> L = sine(200.0, 10.0, 9600, sr), R = L;
            runBlocks([&](float* l, float* r, size_t m) { loud.process(l, r, m); }, L, R, 256);
            float maxVal = 0.0f;
            bool finite = true;
            for (float v : L) {
                if (!std::isfinite(v)) finite = false;
                maxVal = std::max(maxVal, std::abs(v));
            }
            CHECK(finite && loud.isClipping() && maxVal <= 1.05f * loud.outputGain(),
                  "Extrém teszt: túlvezérelt jel (10.0) hard-clipping korlátban marad (csúcs %.2f <= %.2f)",
                  maxVal, loud.outputGain());
        }

        // 4b. Specifikus teszt A: Feldolgozási sorrend - hard-clipping az outGain ELŐTT (EasyEffects modell)
        {
            ee::Loudness loud;
            loud.setSampleRate(48000);
            ParamData p = transparentParams();
            p.set(kFlagLoudness, true);
            p.loudMode = 0;              // IIR mód (azonnali lecsengés)
            p.loudVolumeDb = 0.0f;       // Flat görbe (0 dB kompenzáció)
            p.loudHclip = 1;             // Hard-clipping aktív
            p.loudHclipRangeDb = 0.0f;   // 0 dB térköz -> küszöb = 1.0
            p.loudOutputDb = 6.0206f;    // +6 dB kimeneti erősítés (szorzó = 2.0)
            loud.update(p);

            // Bemenet: 10.0 amplitúdójú szinusz.
            // Helyes EasyEffects sorrend (DSP -> hclip -> outGain):
            // 1) DSP kimenet = 10.0
            // 2) Hard-clip levágja 1.0-ra
            // 3) outGain megszorozza 2.0-val -> kimeneti csúcs = 2.00!
            // Hibás korábbi sorrend (DSP -> outGain -> hclip):
            // outGain után 20.0, amit a hclip a végén 1.0-ra vágott volna -> csúcs = 1.00!
            std::vector<float> L = sine(1000.0, 10.0, 9600, sr), R = L;
            runBlocks([&](float* l, float* r, size_t m) { loud.process(l, r, m); }, L, R, 256);

            float maxVal = 0.0f;
            for (float v : L) maxVal = std::max(maxVal, std::abs(v));

            CHECK(std::abs(maxVal - 2.0f) <= 0.05f && loud.isClipping(),
                  "Specifikus teszt A [Sorrend]: mért csúcs %.2f ~= 2.00 (helyes: hclip -> outGain; javítás előtt 1.00 lett volna)",
                  maxVal);

            loud.reset(); // Kiüríti a szűrő puffereit és törli a latching flaget
            CHECK(!loud.isClipping(), "Specifikus teszt A [clearClipping]: reset() után inaktív");

            // Túlvezérlés újra egy rövid blokkal:
            std::vector<float> Lov = sine(1000.0, 5.0, 512, sr), Rov = Lov;
            runBlocks([&](float* l, float* r, size_t m) { loud.process(l, r, m); }, Lov, Rov, 256);
            CHECK(loud.isClipping(), "Specifikus teszt A [clearClipping]: túlvezérlésre bekapcsol");
            loud.clearClipping();
            CHECK(!loud.isClipping(), "Specifikus teszt A [clearClipping]: clearClipping() törli a latching jelzést");

            std::vector<float> Lnorm = sine(1000.0, 0.2, 4800, sr), Rnorm = Lnorm;
            runBlocks([&](float* l, float* r, size_t m) { loud.process(l, r, m); }, Lnorm, Rnorm, 256);
            CHECK(!loud.isClipping(), "Specifikus teszt A [clearClipping]: normál jelnél inaktív marad");
        }

        // 4c. Specifikus teszt B: Hard-clip küszöb FFT módban az LSP sqrt(min*max) képlete alapján
        {
            ee::Loudness loud;
            loud.setSampleRate(48000);
            ParamData p = transparentParams();
            p.set(kFlagLoudness, true);
            p.loudStd = 0;               // Flat mód (vFreqApply_ minden ponton = vol)
            p.loudMode = 5;              // FFT 4096
            p.loudVolumeDb = 6.0206f;    // +6 dB erősítés -> min = 2.0, max = 2.0, sqrt(min*max) = 2.0
            p.loudHclip = 1;             // Hard-clipping aktív
            p.loudHclipRangeDb = 0.0f;   // 0 dB térköz -> küszöb = 1.0 * sqrt(min*max) = 2.0
            p.loudInputDb = 0.0f;
            p.loudOutputDb = 0.0f;
            loud.update(p);

            // Bemenet: 10.0 amplitúdójú szinusz.
            // A kimenet le lesz vágva a küszöbre: 2.0!
            std::vector<float> L = sine(1000.0, 10.0, 19200, sr), R = L;
            runBlocks([&](float* l, float* r, size_t m) { loud.process(l, r, m); }, L, R, 256);

            float maxVal = 0.0f;
            for (size_t i = 8192; i < L.size(); ++i) maxVal = std::max(maxVal, std::abs(L[i]));

            CHECK(loud.isClipping() && std::abs(maxVal - 2.0f) <= 0.05f,
                  "Specifikus teszt B [FFT clip küszöb Flat]: mért csúcs %.2f levágva pontosan 2.00-ra",
                  maxVal);

            // ISO 226:2003 görbe -30 dB mellett:
            // -30 dB hangerőnél az erősítés 50 Hz-en -17.87 dB (~0.128), a minimum pedig ~0.042 (-27.5 dB).
            // Régi küszöb: maxVal ~ 0.128.
            // Új küszöb: sqrt(min * max) = sqrt(0.042 * 0.128) = 0.0733.
            p.loudStd = 1;               // ISO 226:2003
            p.loudVolumeDb = -30.0f;     // -30 dB
            p.loudHclip = 1;
            p.loudHclipRangeDb = 0.0f;   // 0 dB térköz -> küszöb = sqrt(min*max) = 0.0733
            loud.update(p);

            // Bemenet: 50 Hz-es szinusz 0.80 amplitúdóval.
            // 50 Hz-en a kompenzáció erősítése ~0.128 -> vágás nélkül a kimeneti csúcs: 0.80 * 0.128 = 0.102.
            // A régi kódnál (küszöb = max ~0.128): 0.102 < 0.128 -> NINCS vágás, kimenet csúcsa = 0.102, isClipping() = false!
            // Az új kódnál (küszöb = sqrt(min*max) = 0.0733): 0.102 > 0.0733 -> VAN vágás, kimenet csúcsa = 0.0733, isClipping() = true!
            std::vector<float> L2 = sine(50.0, 0.8, 38400, sr), R2 = L2;
            runBlocks([&](float* l, float* r, size_t m) { loud.process(l, r, m); }, L2, R2, 256);

            float maxVal2 = 0.0f;
            for (size_t i = 8192; i < L2.size(); ++i) maxVal2 = std::max(maxVal2, std::abs(L2[i]));

            CHECK(loud.isClipping() && maxVal2 <= 0.076f && maxVal2 >= 0.071f,
                  "Specifikus teszt B [FFT clip küszöb sqrt(min*max)]: mért csúcs %.4f levágva sqrt(min*max)=0.0733-ra (javítás előtt 0.102 maradt volna)",
                  maxVal2);
        }

        // 4d. Specifikus teszt C: Felső polc (HISHELF) tartományának válasza IIR módban (freqs_[n-2] felett, az fmax-on belül)
        {
            ee::Loudness loudIir;
            loudIir.setSampleRate(48000);
            ParamData p = transparentParams();
            p.set(kFlagLoudness, true);
            p.loudStd = 1;               // ISO 226:2003
            p.loudMode = 0;              // IIR mód
            p.loudApprox = 2;            // Normal (32 szűrő, alapérték: freqs_[30] ~ 11459 Hz, fmax = 12719 Hz)
            p.loudVolumeDb = -30.0f;
            loudIir.update(p);

            // A legfelső (HISHELF) szűrő sávja (12000 Hz: freqs_[30] ~ 11459 Hz felett, fmax = 12719 Hz alatt)
            const double exp12k = computeVendorRefDb(lsp::iso226_2003_curve, -30.0, 12000.0);
            std::vector<float> L = sine(12000.0, 0.1, 96000, sr), R = L;
            runBlocks([&](float* l, float* r, size_t m) { loudIir.process(l, r, m); }, L, R, 512);
            const double meas12k = 20.0 * std::log10(toneAmp(L, 48000, 12000.0, sr) / 0.1);
            const double diff12k = std::abs(meas12k - exp12k);

            CHECK(diff12k <= 1.00,
                  "Specifikus teszt C [Felső polc sáv, ISO 226:2003, 12 kHz]: várt %+.2f dB, mért %+.2f dB (eltérés %.2f dB <= 1.00 dB)",
                  exp12k, meas12k, diff12k);
        }
    }

    // 5. CPU-mérés: 48 kHz, 128 mintás blokk, IIR, FFT 4096 és FFT 16384 (10 másodperc sztereó hang)
    {
        const size_t totalSamples = 480000; // 10.0 másodperc
        const size_t blk = 128;
        const double durationSec = static_cast<double>(totalSamples) / sr;

        struct CpuCase {
            int mode;
            int approx;
            const char* name;
        };
        const CpuCase cases[] = {
            { 0, 0, "IIR Fastest (16 szűrő, slope 1)" },
            { 0, 1, "IIR Low (24 szűrő, slope 2)" },
            { 0, 2, "IIR Normal (32 szűrő, slope 3, alapérték)" },
            { 0, 3, "IIR High (48 szűrő, slope 4)" },
            { 0, 4, "IIR Best (64 szűrő, slope 5)" },
            { 1, 0, "FFT 256" },
            { 2, 0, "FFT 512" },
            { 3, 0, "FFT 1024" },
            { 4, 0, "FFT 2048" },
            { 5, 0, "FFT 4096 (EasyEffects alapérték)" },
            { 6, 0, "FFT 8192" },
            { 7, 0, "FFT 16384 (Maximális felbontás)" }
        };

        std::vector<float> origL = sine(440.0, 0.2, totalSamples, sr);
        std::vector<float> origR = origL;

        for (const auto& c : cases) {
            ee::Loudness loud;
            loud.setSampleRate(48000);
            ParamData d = transparentParams();
            d.set(kFlagLoudness, true);
            d.loudMode = c.mode;
            d.loudApprox = c.approx;
            d.loudStd = 1;
            d.loudVolumeDb = -20.0f;
            loud.update(d);

            std::vector<float> L = origL, R = origR;
            runBlocks([&](float* l, float* r, size_t m) { loud.process(l, r, m); }, L, R, blk);

            L = origL; R = origR;
            const auto t0 = std::chrono::high_resolution_clock::now();
            runBlocks([&](float* l, float* r, size_t m) { loud.process(l, r, m); }, L, R, blk);
            const auto t1 = std::chrono::high_resolution_clock::now();

            const double elapsedUs = std::chrono::duration<double, std::micro>(t1 - t0).count();
            const double elapsedSec = elapsedUs / 1e6;
            const double cpuPercent = (elapsedSec / durationSec) * 100.0;

            CHECK(cpuPercent < 20.0,
                  "CPU-mérés [48 kHz, blokk 128, %s]: %.2f ms / %.1f s -> %.2f %% RT",
                  c.name, elapsedSec * 1000.0, durationSec, cpuPercent);
        }
    }

    // 6. Bemeneti és kimeneti hangerő kioltás
    {
        ee::Loudness loud;
        loud.setSampleRate(48000);
        ParamData d = transparentParams();
        d.set(kFlagLoudness, true);
        d.loudVolumeDb = 0.0f;
        d.loudInputDb = 3.0f;
        d.loudOutputDb = -3.0f;
        loud.update(d);

        std::vector<float> L = sine(1000.0, 0.2, 96000, sr), R = L;
        runBlocks([&](float* l, float* r, size_t m) { loud.process(l, r, m); }, L, R, 512);
        const double g = 20.0 * std::log10(toneAmp(L, 48000, 1000.0, sr) / 0.2);
        CHECK(std::abs(g) < 0.2, "Input +3 dB és Output -3 dB kioltja egymást: eredő %.2f dB", g);
    }

    // 7. Mintavételi frekvenciák (44.1, 48, 88.2, 96, 192 kHz)
    {
        const uint32_t rates[5] = { 44100, 48000, 88200, 96000, 192000 };
        bool allOk = true;
        for (uint32_t testSr : rates) {
            ee::Loudness loud;
            loud.setSampleRate(testSr);
            ParamData d = transparentParams();
            d.set(kFlagLoudness, true);
            d.loudVolumeDb = -20.0f;
            loud.update(d);

            std::vector<float> L = sine(100.0, 0.2, testSr, static_cast<double>(testSr)), R = L;
            runBlocks([&](float* l, float* r, size_t m) { loud.process(l, r, m); }, L, R, 256);
            for (float v : L) {
                if (!std::isfinite(v)) { allOk = false; break; }
            }
        }
        CHECK(allOk, "Mintavételi frekvenciák (44.1 / 48 / 88.2 / 96 / 192 kHz) stabilak, véges kimenet");
    }

    // 8. Teljes lánc integráció: kikapcsolva bitpontos regresszió
    {
        AetherChain chain;
        chain.setSampleRate(48000);
        ParamData d = transparentParams();
        chain.applyParamsRT(d);

        std::vector<float> L = sine(440.0, 0.5, 96000, sr), R = L;
        std::vector<float> L0 = L, R0 = R;
        runBlocks([&](float* l, float* r, size_t m) { chain.process(l, r, m); }, L, R, 512);
        CHECK(L == L0 && R == R0, "Loudness kikapcsolva: a teljes lánc bitpontosan érintetlen marad");
    }

    // 9. Teljes lánc A/B teszt késleltetés-illesztéssel
    {
        AetherChain chain;
        chain.setSampleRate(48000);
        ParamData d = transparentParams();
        d.set(kFlagLoudness, true);
        d.loudMode = 7; // FFT 16384 mód
        d.loudVolumeDb = -20.0f;
        d.loudCompare = 1; // A/B összehasonlítás: az eredeti ág szól
        chain.applyParamsRT(d);

        std::vector<float> L = sine(1000.0, 0.3, 96000, sr), R = L;
        runBlocks([&](float* l, float* r, size_t m) { chain.process(l, r, m); }, L, R, 512);

        const double g = 20.0 * std::log10(toneAmp(L, 48000, 1000.0, sr) / 0.3);
        CHECK(std::abs(g - (-20.0)) < 1.0, "Loudness A/B összehasonlítás: a feldolgozott szinthez (-20 dB) kiegyenlített eredeti jel szól (%.2f dB)", g);
    }

    // 10. Paraméterblokk v12 -> v13 visszafelé kompatibilitás (loadParamBytes)
    {
        std::vector<uint8_t> v12Bytes(kParamSizeV12, 0);
        ParamData v12Origin{};
        v12Origin.preampDb = 4.25f;
        v12Origin.set(kFlagEq, true);
        v12Origin.set(kFlagLimiter, true);
        v12Origin.limThresholdDb = -3.5f;
        v12Origin.ditherMode = 1;
        v12Origin.chDelayMs[0] = 1.2f;
        std::memcpy(v12Bytes.data(), &v12Origin, kParamSizeV12);

        ParamData loaded{};
        const bool ok = loadParamBytes(reinterpret_cast<const char*>(v12Bytes.data()), v12Bytes.size(), 12, kParamSizeV12, loaded);
        CHECK(ok, "loadParamBytes sikeresen betöltötte a valódi v12-es paraméterblokkot");
        CHECK(loaded.preampDb == 4.25f && loaded.has(kFlagEq) && loaded.has(kFlagLimiter) &&
              loaded.limThresholdDb == -3.5f && loaded.ditherMode == 1 && loaded.chDelayMs[0] == 1.2f,
              "v12 régi mezők épek és változatlanok maradtak a betöltés után");
        CHECK(!loaded.has(kFlagLoudness) && loaded.loudVolumeDb == 0.0f && loaded.loudInputDb == 0.0f &&
              loaded.loudOutputDb == 0.0f && loaded.loudStd == 1 && loaded.loudMode == 5 &&
              loaded.loudApprox == 2 &&
              loaded.loudHclip == 0 && loaded.loudHclipRangeDb == 6.0f && loaded.loudCompare == 0,
              "v12 betöltésekor az új Loudness mezők alapértéken vannak (Loudness kikapcsolva, std=1, mode=5, approx=2, hclipRange=6)");
    }
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
    testMagicTouch();
    testMonoBass();
    testConvolverToggle();
    testAbCompare();
    testAutogain();
    testEqLatency();
    testIrLibraryFormat();
    testOutStage();
    testOutputGain();
    testPcmConvert();
    testLoudness();
    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
