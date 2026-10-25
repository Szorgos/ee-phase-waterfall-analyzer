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

    // APO mély polc nagy belső mintavételnél (decramp x6 / x8 = 288 / 384 kHz): a mélyeken ugyanazt adja, mint
    // decramp nélkül (float együttható-számítással 20 Hz-en +0,8 dB volt az eltérés)
    {
        ParamData d = bellParams(6);
        d.bands[0].type = static_cast<int32_t>(EqBandType::LoShelf);
        d.bands[0].freq = 105.0f;
        d.bands[0].q = 0.7f;
        double ref = 0.0, worst = 0.0;
        for (int dc : { 0, 4, 5 }) {
            d.eqDecramp = dc;
            ee::Equalizer eq;
            eq.setSampleRate(48000);
            eq.update(d);
            std::vector<float> L = sine(20.0, 0.1, 192000, sr), R = L;
            runBlocks([&](float* l, float* r, size_t m) { eq.process(l, r, m); }, L, R, 512);
            const double g = 20 * std::log10(toneAmp(L, 96000, 20.0, sr) / 0.1);
            if (dc == 0) ref = g; else worst = std::max(worst, std::abs(g - ref));
        }
        CHECK(worst < 0.3, "APO mély polc 105 Hz +6 dB, decramp x6 / x8: 20 Hz-en legfeljebb %.2f dB eltérés a decramp nélkülitől", worst);
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
        // APO (DR) mély polc decramp x4 / x8 mellett: a grafikon görbéje és csoportkésleltetése mélyen is sima
        // (float számításnál a cos(w) lépcsőzetes volt: kb. 0,8 ms-os ugrások 10-30 Hz között)
        for (int decr : { 3, 5 }) {
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
                  decr == 3 ? "x4" : "x8", maxJump);
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
    std::printf("[9] Mélyvágás (10 / 20 / 30 Hz, 24 dB/oktáves) és a kikapcsolt modul\n");
    const double sr = 48000.0;
    for (int32_t hz : { 10, 20, 30 }) {
        const double gc = subsonicGainDb(hz, hz, sr), g2c = subsonicGainDb(2.0 * hz, hz, sr);
        const double g4c = subsonicGainDb(4.0 * hz, hz, sr);
        CHECK(std::abs(gc + 6.0) < 0.3 && g2c > -0.7 && g2c < 0.0 && std::abs(g4c) < 0.1,
              "%d Hz: fc-n %.2f dB (LR4: −6 dB), 2·fc-n %.2f dB, 4·fc-n %.2f dB", hz, gc, g2c, g4c);
    }
    const double g = subsonicGainDb(100.0, 30, sr);
    CHECK(std::abs(g) < 0.1, "30 Hz-es vágás mellett 100 Hz szintje %.3f dB (a hangzás érintetlen)", g);

    ParamData bad = transparentParams();
    bad.subsonicHz = 15;
    sanitizeParams(bad);
    CHECK(bad.subsonicHz == 0, "érvénytelen vágási frekvencia (15 Hz) kikapcsolva");

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
    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
