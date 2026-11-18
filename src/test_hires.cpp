// A nagy felbontás A részének tesztjei (docs: aqulify-mobile/docs/plans/terv-claude-hires-2026-11-17.md):
//  [1] DSP-korlát: 384 kHz fölött a lánc bitpontosan átenged, 384 kHz-en a lánc dolgozik (EQ +6 dB mérve)
//  [2] Kimeneti fokozat: 50 ms késleltetés 352,8 / 384 kHz-en mintapontos (a puffer nem 192 kHz-re méretezett)
//  [3] Beállítások: új mintavételek, új mezők, régi JSON betöltése
//  [4] Célmintavétel-választás 705,6 / 768 kHz-es kimenettel és felső korláttal (független elvárás)
//  [5] Felmintavételezés 1 és 2 szálon: a kimenet bitre azonos (minden párra, fázisra)
//  [6] Terhelésmérő: ismert terhelés mérése
//  [7] Terhelés-tanácsadó: döntési táblázat (független elvárás)
//  [8] Lejátszó: 384 és 768 kHz-es fájl bitpontosan; alulcsordulás-számláló
//  [9] Dedikált mag (Linux): a dekóder-szál a kért magra kerül, a kimenet bitre azonos
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "aether_chain.hpp"
#include "audio_options.hpp"
#include "file_player.hpp"
#include "load_advisor.hpp"
#include "load_meter.hpp"
#include "out_stage.hpp"
#include "param_block.hpp"
#include "upsample_engine.hpp"

using namespace ee_analyzer;
using namespace ee_analyzer::player;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; std::printf("  [HIBA] " __VA_ARGS__); } else { std::printf("  [OK]   " __VA_ARGS__); } std::printf("\n"); } while (0)
static constexpr double kPi = 3.14159265358979323846;

static double toneAmp(const std::vector<float>& x, size_t from, size_t n, double f, double sr) {
    std::complex<double> acc(0, 0);
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * kPi * i / (n - 1));
        acc += w * double(x[from + i]) * std::polar(1.0, -2 * kPi * f * i / sr);
    }
    return 2.0 * std::abs(acc) / (n * 0.5);
}

static ParamData bellEqLimiter() {
    ParamData d = transparentParams();
    d.set(kFlagEq, true);
    d.numBands = 1;
    d.bands[0].type = static_cast<int32_t>(EqBandType::Bell);
    d.bands[0].mode = 0;
    d.bands[0].freq = 1000.0f;
    d.bands[0].gainDb = 6.0f;
    d.bands[0].q = 1.0f;
    d.eqMode = 0;
    return d;
}

// ---------------------------------------------------------------- [1]
static void testDspCap() {
    std::printf("[1] DSP-korlát 384 kHz fölött\n");
    for (uint32_t sr : { 705600u, 768000u }) {
        AetherChain chain;
        chain.setSampleRate(sr);
        chain.startWorker();
        ParamData d = bellEqLimiter();
        d.set(kFlagLimiter, true);
        d.preampDb = -3.0f;
        chain.applyParamsRT(d);
        chain.waitIdle();
        std::mt19937 rng(11);
        std::uniform_real_distribution<float> u(-0.9f, 0.9f);
        std::vector<float> L(sr / 10), R(sr / 10);
        for (auto& v : L) v = u(rng);
        for (auto& v : R) v = u(rng);
        const auto L0 = L, R0 = R;
        for (size_t off = 0; off < L.size(); off += 4096) {
            const size_t m = std::min<size_t>(4096, L.size() - off);
            chain.process(L.data() + off, R.data() + off, m);
        }
        CHECK(chain.dspBypassedForRate(), "%u Hz: a lánc jelzi, hogy 384 kHz fölött nem dolgozik", sr);
        CHECK(L == L0 && R == R0, "%u Hz: bekapcsolt EQ + limiter + előerősítés mellett is bitpontos átvitel", sr);
        chain.stopWorker();
    }
    {
        const uint32_t sr = 384000;
        AetherChain chain;
        chain.setSampleRate(sr);
        chain.startWorker();
        chain.applyParamsRT(bellEqLimiter());
        chain.waitIdle();
        const size_t n = sr / 2;
        std::vector<float> L(n), R(n);
        for (size_t i = 0; i < n; ++i) L[i] = R[i] = float(0.1 * std::sin(2 * kPi * 1000.0 * i / sr));
        for (size_t off = 0; off < n; off += 4096) {
            const size_t m = std::min<size_t>(4096, n - off);
            chain.process(L.data() + off, R.data() + off, m);
        }
        const double g = 20 * std::log10(toneAmp(L, n / 2, n / 4, 1000.0, sr) / 0.1);
        CHECK(!chain.dspBypassedForRate() && std::fabs(g - 6.0) < 0.1, "384 kHz-en a lánc dolgozik: EQ 1 kHz-en %+.3f dB (+6 ± 0,1)", g);
        chain.stopWorker();
    }
}

// ---------------------------------------------------------------- [2]
static void testOutStageDelay() {
    std::printf("[2] Kimeneti fokozat késleltetése nagy mintavételen\n");
    for (uint32_t sr : { 352800u, 384000u }) {
        outstage::OutStage os;
        ParamData d = transparentParams();
        d.set(kFlagOutStage, true);
        d.chDelayMs[0] = d.chDelayMs[1] = 50.0f;
        os.setSampleRate(sr);
        os.update(d);
        const size_t expect = static_cast<size_t>(std::lround(0.050 * sr));
        const size_t n = expect + 4096;
        std::vector<float> L(n, 0.0f), R(n, 0.0f);
        L[0] = R[0] = 0.5f;
        os.processChannels(L.data(), R.data(), n);
        size_t pk = 0;
        for (size_t i = 0; i < n; ++i) if (std::fabs(L[i]) > std::fabs(L[pk])) pk = i;
        CHECK(pk == expect && std::fabs(L[pk] - 0.5f) < 1e-6f, "%u Hz: 50 ms = %zu minta, az impulzus a(z) %zu. mintán", sr, expect, pk);
    }
}

// ---------------------------------------------------------------- [3]
static void testOptions() {
    std::printf("[3] Beállítások\n");
    const std::vector<int> want = { 352800, 384000, 705600, 768000 };
    bool all = true;
    for (int r : want) all = all && std::find(audioopt::kRateChoices.begin(), audioopt::kRateChoices.end(), r) != audioopt::kRateChoices.end();
    CHECK(all, "a választható mintavételek között 352,8 / 384 / 705,6 / 768 kHz");
    audioopt::AudioOptions o;
    o.upsampleThreads = 2;
    o.dedicatedCore = 1;
    o.upsampleMaxRate = 384000;
    o.loadAdvisor = false;
    json::Value v;
    CHECK(json::parse(audioopt::toJson(o), v), "JSON-be írható");
    const auto back = audioopt::fromJson(v);
    CHECK(back.upsampleThreads == 2 && back.dedicatedCore == 1 && back.upsampleMaxRate == 384000 && !back.loadAdvisor,
          "új mezők oda-vissza: szálak 2, mag 1, felső korlát 384 kHz, tanácsadó ki");
    json::Value old;
    json::parse("{\"buffer\":256,\"rateMode\":\"follow\",\"upsampleRule\":1}", old);
    const auto legacy = audioopt::fromJson(old);
    CHECK(legacy.bufferFrames == 256 && legacy.upsampleThreads == 0 && legacy.dedicatedCore == -1 &&
              legacy.upsampleMaxRate == 0 && legacy.loadAdvisor,
          "régi JSON: új mezők alapértéken (automatikus szál, nincs dedikált mag, nincs korlát, tanácsadó be)");
    json::Value bad;
    json::parse("{\"upsampleThreads\":7,\"dedicatedCore\":-5,\"upsampleMaxRate\":12345}", bad);
    const auto s = audioopt::fromJson(bad);
    CHECK(s.upsampleThreads == 0 && s.dedicatedCore == -1 && s.upsampleMaxRate == 0, "érvénytelen értékek alapértékre igazítva");
}

// ---------------------------------------------------------------- [4]
// Független elvárás: a szabályok a felső korláttal (0 = nincs)
static uint32_t expectTarget(int rule, uint32_t src, const std::vector<uint32_t>& caps, uint32_t maxRate) {
    auto ok = [&](uint32_t r) { return std::find(caps.begin(), caps.end(), r) != caps.end() && (maxRate == 0 || r <= maxRate); };
    if (rule == 0 || src > 192000) return src;
    if (rule == 1) return ok(src * 2) ? src * 2 : src;
    if (rule == 2) { uint32_t b = src; for (uint32_t m = 2; m <= 16; m *= 2) if (ok(src * m)) b = src * m; return b; }
    if (rule == 3) { uint32_t b = src; for (uint32_t r : caps) if (ok(r) && r > b) b = r; return b; }
    return src;
}

static void testTargets() {
    std::printf("[4] Célmintavétel 705,6 / 768 kHz-es kimenettel és felső korláttal\n");
    const std::vector<uint32_t> caps = { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000 };
    size_t cases = 0, bad = 0;
    for (int rule = 0; rule <= 3; ++rule)
        for (uint32_t src : { 44100u, 48000u, 88200u, 96000u, 176400u, 192000u, 352800u, 384000u })
            for (uint32_t cap : { 0u, 384000u, 192000u }) {
                const uint32_t got = UpsampleEngine::selectTargetRate(src, static_cast<audioopt::UpsampleRule>(rule), caps, {}, false, false,
                                                                     nullptr, cap);
                if (got != expectTarget(rule, src, caps, cap)) ++bad;
                ++cases;
            }
    CHECK(bad == 0, "%zu eset, eltérés: %zu (pl. 44,1 kHz kettő hatványa → 705,6 kHz; korláttal 352,8 kHz)", cases, bad);
}

// ---------------------------------------------------------------- [5]
static std::vector<float> runUps(int threads, uint32_t src, uint32_t dst, double phase, const std::vector<float>& in) {
    UpsampleEngine e;
    e.setThreads(threads);
    e.init(src, dst, 2, 28, phase, 0.95, 1.00, -1.0f);
    std::vector<float> out(static_cast<size_t>(4096 * 2 * (double(dst) / src)) + (1u << 16)), all;
    const size_t chunk = 4096;
    for (size_t off = 0; off < in.size() / 2; off += chunk) {
        const size_t m = std::min(chunk, in.size() / 2 - off);
        size_t got = 0;
        e.process(in.data() + off * 2, m, out.data(), out.size() / 2, got);
        all.insert(all.end(), out.begin(), out.begin() + got * 2);
    }
    size_t f = 0;
    e.flush(out.data(), out.size() / 2, f);
    all.insert(all.end(), out.begin(), out.begin() + f * 2);
    return all;
}

static void testThreadsBitIdentical() {
    std::printf("[5] Felmintavételezés 1 és 2 szálon: bitre azonos kimenet\n");
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> u(-0.8f, 0.8f);
    for (auto [src, dst] : std::vector<std::pair<uint32_t, uint32_t>>{ {44100, 88200}, {48000, 96000}, {44100, 705600}, {48000, 768000}, {96000, 192000} })
        for (double ph : { 0.0, 25.0, 50.0 }) {
            std::vector<float> in(src / 4 * 2);
            for (size_t i = 0; i < in.size(); i += 2) { in[i] = u(rng); in[i + 1] = 0.3f * std::sin(float(i) * 0.01f); }
            const auto a = runUps(1, src, dst, ph, in), b = runUps(2, src, dst, ph, in);
            const bool same = a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
            CHECK(same && !a.empty(), "%u → %u, fázis %.0f: %zu minta, bitre azonos", src, dst, ph, a.size());
        }
}

// ---------------------------------------------------------------- [6]
static void testLoadMeter() {
    std::printf("[6] Terhelésmérő\n");
    const unsigned cores = load::logicalCores();
    load::ProcessLoadMeter meter;
    meter.sample();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const double idle = meter.sample();
    std::atomic<bool> stop{false};
    std::thread busy([&] { volatile double x = 0; while (!stop) x += std::sqrt(x + 1.0); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    meter.sample();
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    const double one = meter.sample();
    stop = true;
    busy.join();
    const double expect = 100.0 / cores;   // egy teljesen terhelt mag a gép kapacitásához mérve
    std::printf("     magok: %u, nyugalmi: %.1f %%, egy terhelt mag: %.1f %% (elvárt ~%.1f %%)\n", cores, idle, one, expect);
    CHECK(cores >= 1 && idle < 0.5 * expect, "nyugalomban a mért terhelés kicsi (%.1f %% < %.1f %%)", idle, 0.5 * expect);
    CHECK(std::fabs(one - expect) < 0.35 * expect, "egy terhelt magnál %.1f %% (elvárt %.1f %% ± 35 %%)", one, expect);
}

// ---------------------------------------------------------------- [7]
static void testAdvisor() {
    std::printf("[7] Terhelés-tanácsadó: döntési táblázat\n");
    using load::Step;
    struct Case { const char* name; double cpu; double sustained; uint64_t newUnderruns; unsigned cores; int threads; int prec;
                  int rule; uint32_t target; uint32_t maxRate; bool warn; Step step; };
    const std::vector<Case> cases = {
        { "nyugalom",                       12, 60, 0, 8, 0, 28, 1,  88200, 0,      false, Step::None },
        { "magas, de rövid ideig",          45,  5, 0, 8, 0, 28, 1,  88200, 0,      false, Step::None },
        { "magas, tartós, 8 mag",           45, 25, 0, 8, 0, 28, 1,  88200, 0,      true,  Step::TwoThreads },
        { "magas, tartós, 2 mag",           45, 25, 0, 2, 0, 28, 1,  88200, 0,      true,  Step::Precision24 },
        { "már 2 szál",                     45, 25, 0, 8, 2, 28, 1,  88200, 0,      true,  Step::Precision24 },
        { "2 szál, 24 bit, 768 kHz",        45, 25, 0, 8, 2, 24, 3, 768000, 0,      true,  Step::LowerTarget },
        { "2 szál, 24 bit, 88,2 kHz",       45, 25, 0, 8, 2, 24, 1,  88200, 0,      true,  Step::UpsampleOff },
        { "alulcsordulás, rövid terhelés",  20,  1, 3, 8, 0, 28, 1,  88200, 0,      true,  Step::TwoThreads },
        { "felmintavételezés nélkül magas", 45, 25, 0, 8, 0, 28, 0,  44100, 0,      true,  Step::None },
        { "korlát már 192 kHz",             45, 25, 0, 2, 2, 24, 3, 192000, 192000, true,  Step::UpsampleOff },
    };
    for (const auto& c : cases) {
        audioopt::AudioOptions o;
        o.upsampleThreads = c.threads;
        o.upsamplePrecision = c.prec;
        o.upsampleRule = static_cast<audioopt::UpsampleRule>(c.rule);
        o.upsampleMaxRate = c.maxRate;
        load::Snapshot s{ c.cpu, c.sustained, c.newUnderruns };
        const auto a = load::advise(s, load::Config{ 30.0, 20.0 }, o, c.cores, c.target);
        CHECK(a.warn == c.warn && a.step == c.step, "%-34s → figyelmeztet: %s, lépés: %s", c.name, a.warn ? "igen" : "nem", load::stepName(a.step));
    }
    // A lépések alkalmazása és visszavonása
    audioopt::AudioOptions o;
    o.upsampleRule = audioopt::UpsampleRule::MaxOutput;
    o.upsamplePrecision = 28;
    auto o1 = load::applyStep(o, Step::TwoThreads, 768000);
    auto o2 = load::applyStep(o1, Step::Precision24, 768000);
    auto o3 = load::applyStep(o2, Step::LowerTarget, 768000);
    auto o4 = load::applyStep(o3, Step::UpsampleOff, 384000);
    CHECK(o1.upsampleThreads == 2 && o2.upsamplePrecision == 24 && o3.upsampleMaxRate == 384000 &&
              o4.upsampleRule == audioopt::UpsampleRule::Off,
          "lépések: 2 szál → 24 bit → felső korlát 384 kHz → felmintavételezés ki");
    CHECK(load::applyStep(o, Step::LowerTarget, 768000).upsampleMaxRate == 384000 &&
              load::applyStep(o, Step::LowerTarget, 352800).upsampleMaxRate == 192000,
          "a korlát lépcsőzetesen csökken: 768 kHz-ről 384, 352,8 kHz-ről 192 kHz-re");
}

// ---------------------------------------------------------------- [8]
static void writeWavFloat(const std::string& path, uint32_t rate, const std::vector<float>& x) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t data = static_cast<uint32_t>(x.size() * 4);
    f.write("RIFF", 4); u32(36 + data); f.write("WAVEfmt ", 8); u32(16); u16(3); u16(2); u32(rate);
    u32(rate * 8); u16(8); u16(32); f.write("data", 4); u32(data);
    f.write(reinterpret_cast<const char*>(x.data()), data);
}

class PullOutput : public PlayerOutput {
public:
    bool open(uint32_t sr, RenderFn r, std::string*) override { std::lock_guard<std::mutex> l(m); rate = sr; render = std::move(r); opened = true; return true; }
    void close() override { std::lock_guard<std::mutex> l(m); render = nullptr; opened = false; }
    void setActive(bool a) override { std::lock_guard<std::mutex> l(m); active = a; }
    void lock() override { m.lock(); }
    void unlock() override { m.unlock(); }
    std::vector<uint32_t> supportedRates() const override { return caps; }
    bool pull(float* s, size_t n) { std::lock_guard<std::mutex> l(m); if (!opened || !active || !render) return false; render(s, n); return true; }
    std::vector<uint32_t> caps = { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000 };
    std::mutex m;
    uint32_t rate = 0;
    bool opened = false, active = false;
    RenderFn render;
};

static void testPlayerHighRate() {
    std::printf("[8] Lejátszó nagy mintavételen\n");
    for (uint32_t sr : { 384000u, 768000u }) {
        std::mt19937 rng(sr);
        std::uniform_real_distribution<float> u(-0.9f, 0.9f);
        std::vector<float> x(sr / 2 * 2);
        for (auto& v : x) v = u(rng);
        const std::string path = (fs::temp_directory_path() / ("aq_hires_" + std::to_string(sr) + ".wav")).string();
        writeWavFloat(path, sr, x);
        auto outp = std::make_unique<PullOutput>();
        PullOutput* o = outp.get();
        FilePlayer p(std::move(outp));
        std::string err;
        CHECK(p.open(path, &err), "%u Hz-es fájl megnyitva (%s)", sr, err.c_str());
        for (int i = 0; i < 2000 && (p.state() != FilePlayer::State::Playing); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::vector<float> got;
        std::vector<float> blk(2048 * 2);
        while (got.size() < x.size()) {
            for (int i = 0; i < 3000 && p.bufferedFrames() < 2048 && !p.decodeAtEnd(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const size_t k = std::min<size_t>(2048, (x.size() - got.size()) / 2);
            if (!o->pull(blk.data(), k)) break;
            got.insert(got.end(), blk.begin(), blk.begin() + k * 2);
        }
        CHECK(o->rate == sr, "a kimenet %u Hz-en nyílt (nincs átalakítás)", o->rate);
        CHECK(got == x, "%u Hz: %zu minta bitre azonos a fájllal", sr, got.size());
        p.stop();
        fs::remove(path);
    }
    // Alulcsordulás: egyetlen render-hívás több keretet kér, mint a puffer egésze (a fájl még nem ért véget)
    {
        const uint32_t sr = 768000;
        std::vector<float> x(sr * 3 * 2, 0.25f);
        const std::string path = (fs::temp_directory_path() / "aq_hires_under.wav").string();
        writeWavFloat(path, sr, x);
        auto outp = std::make_unique<PullOutput>();
        PullOutput* o = outp.get();
        FilePlayer p(std::move(outp));
        p.open(path, nullptr);
        for (int i = 0; i < 2000 && (p.state() != FilePlayer::State::Playing); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const uint64_t before = p.underruns();
        std::vector<float> big(static_cast<size_t>(sr) * 2 * 2);
        o->pull(big.data(), big.size() / 2);
        const uint64_t after = p.underruns();
        CHECK(before == 0 && after >= 1, "alulcsordulás-számláló: előtte %llu, utána %llu", (unsigned long long)before, (unsigned long long)after);
        p.stop();
        fs::remove(path);
    }
}

// ---------------------------------------------------------------- [9]
static void testDedicatedCore() {
    std::printf("[9] Dedikált mag\n");
#if defined(__linux__) || defined(_WIN32)
    const unsigned cores = load::logicalCores();
    const int core = cores > 1 ? 1 : 0;
    std::atomic<int> seen{-2};
    std::thread t([&] { seen = load::pinCurrentThread(core) ? load::currentThreadAffinitySingle() : -3; });
    t.join();
    CHECK(seen.load() == core, "egy szál a(z) %d. magra rögzíthető (visszaolvasva: %d)", core, seen.load());
#else
    CHECK(!load::pinCurrentThread(0), "macOS: a rögzítés nem érhető el (a lehetőség nem látszik)");
#endif
}

// ---------------------------------------------------------------- [10]
static void testTrackerJson() {
    std::printf("[10] Terhelés-tanácsadó az állapotüzenetben\n");
    load::Tracker t(30.0);
    audioopt::AudioOptions o;
    o.upsampleRule = audioopt::UpsampleRule::Double;
    const std::string j = t.json(o, 88200, 0, false, 1, -1);
    json::Value v;
    const bool ok = j.size() > 2 && json::parse("{" + j.substr(1) + "}", v);
    const json::Value* la = ok ? v.get("loadAdvisor") : nullptr;
    CHECK(la && la->get("cpu") && la->get("warn") && la->get("step") && la->get("cores") && la->get("dspBypassed"),
          "értelmezhető JSON a kötelező mezőkkel: %s", j.c_str());
    CHECK(la && la->get("warn") && !la->get("warn")->b, "induláskor nincs figyelmeztetés");
    // Hangkimaradás: a következő mérésnél (≥ 1 s) figyelmeztet
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    t.json(o, 88200, 0, false, 1, -1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    const std::string j2 = t.json(o, 88200, 3, false, 1, -1);
    json::Value v2;
    json::parse("{" + j2.substr(1) + "}", v2);
    const json::Value* la2 = v2.get("loadAdvisor");
    CHECK(la2 && la2->get("warn") && la2->get("warn")->b && la2->get("recentUnderruns") && la2->get("recentUnderruns")->num >= 3,
          "3 új hangkimaradás után figyelmeztet: %s", j2.c_str());
    audioopt::AudioOptions off = o;
    off.loadAdvisor = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    const std::string j3 = t.json(off, 88200, 9, false, 1, -1);
    CHECK(j3.find("\"warn\":false") != std::string::npos, "kikapcsolt tanácsadó nem figyelmeztet");
}

int main() {
    std::printf("Aqulify – nagy felbontás, A rész\n");
    testTrackerJson();
    testDspCap();
    testOutStageDelay();
    testOptions();
    testTargets();
    testThreadsBitIdentical();
    testLoadMeter();
    testAdvisor();
    testPlayerHighRate();
    testDedicatedCore();
    std::printf(g_fail ? "\nHIBA: %d ellenőrzés nem sikerült\n" : "\nMINDEN TESZT SIKERES (0 hiba)\n", g_fail);
    return g_fail ? 1 : 0;
}
