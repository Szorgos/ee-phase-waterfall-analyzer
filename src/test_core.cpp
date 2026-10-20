// Platformfüggetlen magteszt: FFT spektrum, paraméter-alkalmazás, seqlock, tap körpuffer.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "spectrum.hpp"
#include "param_apply.hpp"
#include "tap_ring.hpp"
#include "mini_json.hpp"

using namespace ee_analyzer;

static constexpr double kPi = 3.14159265358979323846;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; std::printf("  [HIBA] " __VA_ARGS__); std::printf("\n"); } else { std::printf("  [OK]   " __VA_ARGS__); std::printf("\n"); } } while (0)

static size_t bandOf(double hz, size_t bands = 160) {
    return static_cast<size_t>((std::log10(hz) - std::log10(20.0)) / (std::log10(20000.0) - std::log10(20.0)) * bands);
}

static void testSpectrum() {
    std::printf("[1] FFT spektrum\n");
    const double sr = 48000.0;
    SpectrumAnalyzer an(4096, 160);
    std::vector<float> lr(4096 * 2);
    const double amp = 0.5; // -6.02 dBFS
    for (size_t i = 0; i < 4096; ++i) {
        const float v = static_cast<float>(amp * std::sin(2.0 * kPi * 1000.0 * i / sr));
        lr[2 * i] = v;
        lr[2 * i + 1] = v;
    }
    an.push(lr.data(), 4096);
    an.compute(sr);
    const auto& lv = an.levels();
    size_t peak = 0;
    for (size_t i = 1; i < lv.size(); ++i) if (lv[i] > lv[peak]) peak = i;
    const size_t expected = bandOf(1000.0);
    const double peakDb = lv[peak] * 90.0 - 90.0;
    CHECK(peak + 1 >= expected && peak <= expected + 1, "1 kHz csúcs a %zu. sávban (várt: %zu)", peak, expected);
    CHECK(std::abs(peakDb - (-6.02)) < 1.5, "csúcsszint %.2f dBFS (várt ~ -6.02)", peakDb);
    const double far = lv[bandOf(100.0)] * 90.0 - 90.0;
    CHECK(far < -60.0, "100 Hz-en %.1f dBFS (szivárgás < -60 dB)", far);
    CHECK(std::abs(an.phaseDegrees()[peak]) < 1.0, "azonos L/R -> fáziskülönbség %.2f°", an.phaseDegrees()[peak]);

    // L és R ellenfázisban -> 180°
    for (size_t i = 0; i < 4096; ++i) lr[2 * i + 1] = -lr[2 * i];
    an.push(lr.data(), 4096);
    an.compute(sr);
    CHECK(std::abs(std::abs(an.phaseDegrees()[peak]) - 180.0) < 1.0, "ellenfázis -> %.1f°", an.phaseDegrees()[peak]);

    // Csend
    std::fill(lr.begin(), lr.end(), 0.0f);
    an.push(lr.data(), 4096);
    an.compute(sr);
    float mx = 0;
    for (float v : an.levels()) mx = std::max(mx, v);
    CHECK(mx == 0.0f, "csendnél minden sáv 0");
}

static void testTransparentAndEq() {
    std::printf("[2] DSP lánc paraméterezése\n");
    DspPipeline p(48000.0);
    preparePipeline(p);
    setPipelineSampleRate(p, 48000.0);
    applyParams(p, transparentParams());

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> L(1024), R(1024);
    for (auto& v : L) v = dist(rng);
    for (auto& v : R) v = dist(rng);
    std::vector<float> L0 = L, R0 = R;
    p.processBlock(L.data(), R.data(), L.size());
    CHECK(std::memcmp(L.data(), L0.data(), L.size() * 4) == 0 && std::memcmp(R.data(), R0.data(), R.size() * 4) == 0,
          "alapállapot bit-pontosan átlátszó");

    // +6 dB peaking 1 kHz-en: szinusz erősítése
    ParamData d = transparentParams();
    d.set(kFlagEq, true);
    d.numBands = 1;
    d.bands[0] = { static_cast<int32_t>(EqBandType::Peaking), 1000.0f, 6.0f, 1.0f };
    const size_t cap = p.eqChain.filters.capacity();
    applyParams(p, d);
    CHECK(p.eqChain.filters.capacity() == cap, "applyParams nem foglal memóriát (kapacitás %zu)", cap);
    p.reset();
    std::vector<float> s(48000), s2(48000);
    for (size_t i = 0; i < s.size(); ++i) s[i] = s2[i] = static_cast<float>(0.1 * std::sin(2.0 * kPi * 1000.0 * i / 48000.0));
    p.processBlock(s.data(), s2.data(), s.size());
    double rms = 0;
    for (size_t i = 24000; i < 48000; ++i) rms += s[i] * s[i];
    rms = std::sqrt(rms / 24000);
    const double gainDb = 20 * std::log10(rms / (0.1 / std::sqrt(2.0)));
    CHECK(std::abs(gainDb - 6.0) < 0.1, "1 kHz +6 dB peaking mért erősítése %.2f dB", gainDb);

    // Preamp + limiter
    d.preampDb = -3.0f;
    d.set(kFlagLimiter, true);
    applyParams(p, d);
    CHECK(std::abs(p.preampGainDb + 3.0) < 1e-9 && p.limiter.enabled && !p.crossfeed.enabled, "preamp/limiter/crossfeed jelzők");

    // Mintavételi frekvencia váltás 44.1 kHz-re
    setPipelineSampleRate(p, 44100.0);
    applyParams(p, d);
    CHECK(p.eqChain.filters[0].sampleRate == 44100.0, "SR váltás továbbjut a szűrőkre");
}

static void testSeqlock() {
    std::printf("[3] Seqlock paraméterblokk (egyidejű író/olvasó)\n");
    ParamBlock blk;
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        ParamData d{};
        for (uint32_t k = 1; !stop; ++k) {
            d.numBands = kMaxEqBands;
            d.preampDb = static_cast<float>(k);
            for (auto& b : d.bands) { b.freq = static_cast<float>(k); b.gainDb = static_cast<float>(k); }
            blk.write(d);
        }
    });
    size_t reads = 0, torn = 0;
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < until) {
        ParamData d;
        uint32_t seq;
        if (blk.tryRead(d, seq) && seq != 0) { // seq 0 = még senki nem írt (alapértékek)
            ++reads;
            for (auto& b : d.bands) if (b.freq != d.preampDb || b.gainDb != d.preampDb) { ++torn; break; }
        }
    }
    stop = true;
    writer.join();
    CHECK(reads > 1000 && torn == 0, "%zu sikeres olvasás, %zu szakadt pillanatkép", reads, torn);
}

static void testTapRing() {
    std::printf("[4] Felülíró tap körpuffer\n");
    auto ring = std::make_unique<StereoTapRing<1024>>();
    std::vector<float> l(300), r(300), out(2048 * 2);
    uint64_t rp = 0;
    for (int i = 0; i < 300; ++i) { l[i] = static_cast<float>(i); r[i] = static_cast<float>(-i); }
    ring->write(l.data(), r.data(), 300);
    size_t n = readTapRing(*ring, rp, out.data(), 2048);
    CHECK(n == 300 && out[2 * 299] == 299.0f && out[2 * 299 + 1] == -299.0f, "300 frame visszaolvasva");
    for (int k = 0; k < 10; ++k) ring->write(l.data(), r.data(), 300); // 3000 frame: lemaradás
    n = readTapRing(*ring, rp, out.data(), 2048);
    CHECK(n == 256 && out[2 * 255] == 299.0f, "lemaradáskor a legfrissebb 256 frame-re ugrik (%zu)", n);
    n = readTapRing(*ring, rp, out.data(), 2048);
    CHECK(n == 0, "utána nincs új adat");
}

static void testJson() {
    std::printf("[5] JSON vezérlőüzenetek\n");
    json::Value v;
    const bool ok = json::parse(R"({"type":"set_eq","bands":[{"type":"lowshelf","freq":105,"gain":5.5,"q":0.71},{"type":"peaking","freq":3500,"gain":-4,"q":2}]})", v);
    CHECK(ok && v.get("bands") && v.get("bands")->arr.size() == 2, "set_eq üzenet értelmezve");
    CHECK(ok && v.get("bands")->arr[0].get("type")->stringOr("") == "lowshelf" &&
          std::abs(v.get("bands")->arr[1].get("gain")->numberOr(0) + 4.0) < 1e-9, "sávmezők helyesek");
    json::Value bad;
    CHECK(!json::parse("{\"type\":", bad), "hibás JSON elutasítva");
}

int main() {
    std::printf("========================================================\n");
    std::printf(" AetherAudio magteszt (Windows/Linux közös kód)\n");
    std::printf("========================================================\n");
    testSpectrum();
    testTransparentAndEq();
    testSeqlock();
    testTapRing();
    testJson();
    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
