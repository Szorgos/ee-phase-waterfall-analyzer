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
//  [11] FLAC és AIFF dekóder 352,8–768 kHz-en: független íróval készült 24 bites fájlok bitre, tekeréssel
//  [12] Spektrum és csúcsmérő 384 kHz fölött: a felbontás és a szintek a 192 kHz-essel egyeznek, a csúcs nem vész el
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
#include "audio_decoder.hpp"
#include "audio_options.hpp"
#include "file_player.hpp"
#include "load_advisor.hpp"
#include "load_meter.hpp"
#include "out_stage.hpp"
#include "param_block.hpp"
#include "spectrum.hpp"
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
    // A legkisebb választható korlát a 44,1 / 48 kHz-es anyag kétszerezéséhez: 88,2 és 96 kHz
    for (int r : { 88200, 96000 }) {
        json::Value ok;
        json::parse("{\"upsampleMaxRate\":" + std::to_string(r) + "}", ok);
        CHECK(audioopt::fromJson(ok).upsampleMaxRate == static_cast<uint32_t>(r), "legnagyobb célmintavétel: %d Hz választható", r);
    }
}

// ---------------------------------------------------------------- [4]
// Független elvárás: a szabályok a felső korláttal (0 = nincs)
static uint32_t expectTarget(int rule, uint32_t src, const std::vector<uint32_t>& caps, uint32_t maxRate) {
    auto ok = [&](uint32_t r) { return std::find(caps.begin(), caps.end(), r) != caps.end() && (maxRate == 0 || r <= maxRate); };
    uint32_t up = src;
    if (rule != 0 && src <= 192000) {
        if (rule == 1 && ok(src * 2)) up = src * 2;
        if (rule == 2) for (uint32_t m = 2; m <= 16; m *= 2) if (ok(src * m)) up = src * m;
        if (rule == 3) for (uint32_t r : caps) if (ok(r) && r > up) up = r;
    }
    if (up > src || ok(src)) return up;
    // A forrás a korlát fölött: lefelé (Kimenet maximuma: a legnagyobb; különben az azonos családú előbb)
    uint32_t best = 0, family = 0;
    for (uint32_t r : caps) if (ok(r) && r < src) { best = std::max(best, r); if (src % r == 0) family = std::max(family, r); }
    const uint32_t down = rule == 3 ? best : (family ? family : best);
    return down ? down : src;
}

static void testTargets() {
    std::printf("[4] Célmintavétel 705,6 / 768 kHz-es kimenettel és felső korláttal\n");
    const std::vector<uint32_t> caps = { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000 };
    size_t cases = 0, bad = 0;
    for (int rule = 0; rule <= 3; ++rule)
        for (uint32_t src : { 44100u, 48000u, 88200u, 96000u, 176400u, 192000u, 352800u, 384000u })
            for (uint32_t cap : { 0u, 384000u, 192000u }) {
                const uint32_t got = UpsampleEngine::selectTargetRate(src, static_cast<audioopt::UpsampleRule>(rule), caps, {}, false, false,
                                                                     nullptr, cap, /*allowDown=*/true);
                if (got != expectTarget(rule, src, caps, cap)) ++bad;
                ++cases;
            }
    CHECK(bad == 0, "%zu eset, eltérés: %zu (pl. 44,1 kHz kettő hatványa → 705,6 kHz; korláttal 352,8 kHz)", cases, bad);
}

static void testDownsampleEngines() {
    std::printf("[4b] Lefelé alakítás (192 kHz -> 96 kHz-es DAC) mindhárom motorral\n");
    const std::vector<uint32_t> dac96 = { 96000 };
    auto pick = [&](uint32_t src, audioopt::UpsampleRule rule, bool down) {
        return UpsampleEngine::selectTargetRate(src, rule, dac96, {}, false, false, nullptr, 0, down);
    };
    CHECK(pick(192000, audioopt::UpsampleRule::MaxOutput, true) == 96000 && pick(176400, audioopt::UpsampleRule::MaxOutput, true) == 96000 &&
          pick(44100, audioopt::UpsampleRule::MaxOutput, true) == 96000,
          "csak 96 kHz-es kimenet: 192 / 176,4 kHz le-, 44,1 kHz felfelé 96 kHz-re");
    CHECK(pick(192000, audioopt::UpsampleRule::Off, true) == 96000 && pick(44100, audioopt::UpsampleRule::Off, true) == 44100,
          "kikapcsolt felfelé alakításnál is lefelé alakít, felfelé nem");
    CHECK(pick(192000, audioopt::UpsampleRule::MaxOutput, false) == 192000,
          "lefelé alakítás kikapcsolva (rendszer): a forrás változatlanul megy a keverőnek");
    const uint32_t src = 192000, dst = 96000;
    const size_t n = src;   // 1 s
    std::vector<float> in(n * 2);
    for (size_t i = 0; i < n; ++i)
        in[2 * i] = in[2 * i + 1] = static_cast<float>(0.5 * std::sin(2 * kPi * 1000.0 * i / src) + 0.3 * std::sin(2 * kPi * 60000.0 * i / src));
    for (int eng = 0; eng <= 2; ++eng) {
        UpsampleEngine e;
        e.init(src, dst, 2, 28, 25.0, 0.95, 1.00, 0.0f, eng);
        std::vector<float> out(dst * 2 + (1u << 16)), left;
        const size_t chunk = 4096;
        for (size_t off = 0; off < n; off += chunk) {
            size_t got = 0;
            e.process(in.data() + off * 2, std::min(chunk, n - off), out.data(), out.size() / 2, got);
            for (size_t i = 0; i < got; ++i) left.push_back(out[2 * i]);
        }
        const size_t m = 32768, from = left.size() > m + 8192 ? left.size() - m - 4096 : 0;
        const double a1k = left.size() > m ? toneAmp(left, from, m, 1000.0, dst) : 0.0;
        const double alias = left.size() > m ? toneAmp(left, from, m, 36000.0, dst) : 1.0;   // 60 kHz tükörképe 96 kHz-en
        CHECK(std::fabs(a1k - 0.5) < 0.01 && alias < 1e-4, "%s: 1 kHz amplitúdó %.4f (0,5), 60 kHz tükörképe %.2e",
              audioopt::upsampleEngineLabel(eng), a1k, alias);
    }
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

// ---------------------------------------------------------------- [11]
// Független író a teszthez: FLAC (a szabvány szerint, tömörítetlen VERBATIM alkeretekkel) és AIFF (24 bit, big-endian).
static uint8_t crc8(const std::vector<uint8_t>& d, size_t from, size_t to) {
    uint8_t c = 0;
    for (size_t i = from; i < to; ++i) {
        c ^= d[i];
        for (int b = 0; b < 8; ++b) c = (c & 0x80) ? static_cast<uint8_t>((c << 1) ^ 0x07) : static_cast<uint8_t>(c << 1);
    }
    return c;
}
static uint16_t crc16(const std::vector<uint8_t>& d, size_t from, size_t to) {
    uint16_t c = 0;
    for (size_t i = from; i < to; ++i) {
        c ^= static_cast<uint16_t>(d[i]) << 8;
        for (int b = 0; b < 8; ++b) c = (c & 0x8000) ? static_cast<uint16_t>((c << 1) ^ 0x8005) : static_cast<uint16_t>(c << 1);
    }
    return c;
}
static void putUtf8(std::vector<uint8_t>& o, uint32_t v) {   // a FLAC keretszáma „UTF-8” kódolással
    if (v < 0x80) { o.push_back(static_cast<uint8_t>(v)); return; }
    if (v < 0x800) { o.push_back(0xC0 | (v >> 6)); o.push_back(0x80 | (v & 0x3F)); return; }
    if (v < 0x10000) { o.push_back(0xE0 | (v >> 12)); o.push_back(0x80 | ((v >> 6) & 0x3F)); o.push_back(0x80 | (v & 0x3F)); return; }
    o.push_back(0xF0 | (v >> 18)); o.push_back(0x80 | ((v >> 12) & 0x3F)); o.push_back(0x80 | ((v >> 6) & 0x3F)); o.push_back(0x80 | (v & 0x3F));
}
/** 24 bites sztereó FLAC (váltakozó L, R minták, −2^23 … 2^23−1), 4096 keretes blokkokkal. */
static void writeFlac24(const std::string& path, uint32_t rate, const std::vector<int32_t>& lr) {
    const uint32_t frames = static_cast<uint32_t>(lr.size() / 2), bs = 4096;
    std::vector<uint8_t> o = { 'f', 'L', 'a', 'C' };
    // STREAMINFO (utolsó metaadat-blokk, 34 bájt)
    o.push_back(0x80); o.push_back(0); o.push_back(0); o.push_back(34);
    o.push_back(bs >> 8); o.push_back(bs & 0xFF); o.push_back(bs >> 8); o.push_back(bs & 0xFF);   // min / max blokk
    for (int i = 0; i < 6; ++i) o.push_back(0);                                                     // min / max keret: ismeretlen
    // 20 bit mintavétel | 3 bit (csatorna − 1) | 5 bit (bitmélység − 1) | 36 bit mintaszám
    const uint64_t packed = (uint64_t(rate) << 44) | (uint64_t(1) << 41) | (uint64_t(23) << 36) | uint64_t(frames);
    for (int i = 7; i >= 0; --i) o.push_back(static_cast<uint8_t>(packed >> (8 * i)));
    for (int i = 0; i < 16; ++i) o.push_back(0);                                                    // MD5: ismeretlen
    for (uint32_t f = 0, fn = 0; f < frames; f += bs, ++fn) {
        const uint32_t n = std::min(bs, frames - f);
        const size_t h = o.size();
        o.push_back(0xFF); o.push_back(0xF8);                          // szinkron, rögzített blokkméret
        o.push_back(0x70);                                             // blokkméret: 16 bites (n−1) a fejléc végén; mintavétel: a STREAMINFO-ból
        o.push_back(0x1C);                                             // 2 független csatorna, 24 bit
        putUtf8(o, fn);
        o.push_back(static_cast<uint8_t>((n - 1) >> 8)); o.push_back(static_cast<uint8_t>((n - 1) & 0xFF));
        o.push_back(crc8(o, h, o.size()));
        for (int ch = 0; ch < 2; ++ch) {
            o.push_back(0x02);                                         // VERBATIM alkeret
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t v = static_cast<uint32_t>(lr[2 * (f + i) + ch]) & 0xFFFFFF;
                o.push_back(v >> 16); o.push_back((v >> 8) & 0xFF); o.push_back(v & 0xFF);
            }
        }
        const uint16_t c = crc16(o, h, o.size());
        o.push_back(c >> 8); o.push_back(c & 0xFF);
    }
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(o.data()), static_cast<std::streamsize>(o.size()));
}
/** 24 bites sztereó AIFF (a mintavétel 80 bites IEEE extended számként). */
static void writeAiff24(const std::string& path, uint32_t rate, const std::vector<int32_t>& lr) {
    const uint32_t frames = static_cast<uint32_t>(lr.size() / 2);
    std::vector<uint8_t> o;
    auto be32 = [&](uint32_t v) { for (int i = 3; i >= 0; --i) o.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    auto be16 = [&](uint16_t v) { o.push_back(v >> 8); o.push_back(v & 0xFF); };
    const uint32_t data = frames * 6;
    o.insert(o.end(), { 'F', 'O', 'R', 'M' }); be32(4 + 26 + 16 + data);
    o.insert(o.end(), { 'A', 'I', 'F', 'F', 'C', 'O', 'M', 'M' }); be32(18);
    be16(2); be32(frames); be16(24);
    int e = 0;                                                         // 80 bites extended: előjel+kitevő (15 bit), 64 bites mantissza
    uint64_t m = rate;
    while (!(m & (uint64_t(1) << 63))) { m <<= 1; ++e; }
    be16(static_cast<uint16_t>(16383 + 63 - e));
    for (int i = 7; i >= 0; --i) o.push_back(static_cast<uint8_t>(m >> (8 * i)));
    o.insert(o.end(), { 'S', 'S', 'N', 'D' }); be32(8 + data); be32(0); be32(0);
    for (int32_t v : lr) { const uint32_t u = static_cast<uint32_t>(v) & 0xFFFFFF; o.push_back(u >> 16); o.push_back((u >> 8) & 0xFF); o.push_back(u & 0xFF); }
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(o.data()), static_cast<std::streamsize>(o.size()));
}

static void testHiresDecoders() {
    std::printf("[11] FLAC és AIFF dekóder 352,8–768 kHz-en (24 bit, bitre)\n");
    for (uint32_t sr : { 352800u, 384000u, 705600u, 768000u }) {
        std::mt19937 rng(sr ^ 0x5A5A);
        std::uniform_int_distribution<int32_t> u(-8388608, 8388607);
        const uint32_t frames = sr / 4 + 1234;                         // nem kerek blokkszám: az utolsó blokk rövidebb
        std::vector<int32_t> lr(static_cast<size_t>(frames) * 2);
        for (auto& v : lr) v = u(rng);
        for (const char* kind : { "flac", "aiff" }) {
            const std::string path = (fs::temp_directory_path() / ("aq_hires_dec_" + std::to_string(sr) + "." + kind)).string();
            if (std::string(kind) == "flac") writeFlac24(path, sr, lr); else writeAiff24(path, sr, lr);
            std::string err;
            auto d = openDecoder(path, &err);
            bool ok = d && d->sampleRate() == sr && d->channels() == 2 && d->totalFrames() == frames;
            size_t diff = 0, got = 0;
            if (d) {
                std::vector<float> buf(8192 * 2);
                size_t pos = 0;
                for (;;) {
                    const size_t n = d->read(buf.data(), 8192);
                    if (n == 0) break;
                    for (size_t i = 0; i < n * 2 && pos + i < lr.size(); ++i)
                        if (buf[i] != static_cast<float>(lr[pos + i]) / 8388608.0f) ++diff;
                    pos += n * 2;
                }
                got = pos / 2;
                // tekerés a közepére: a minta pontosan ott folytatódik
                const uint64_t mid = frames / 2 + 17;
                float two[2] = {};
                const bool sk = d->seek(mid) && d->read(two, 1) == 1;
                ok = ok && sk && two[0] == static_cast<float>(lr[mid * 2]) / 8388608.0f &&
                     two[1] == static_cast<float>(lr[mid * 2 + 1]) / 8388608.0f;
            }
            CHECK(ok && got == frames && diff == 0, "%s %u Hz: %zu / %u keret, %zu eltérő minta, tekerés pontos (%s)", kind, sr, got,
                  frames, diff, err.c_str());
            d.reset();   // Windowson a nyitott fájl nem törölhető: előbb a dekóder zár
            std::error_code ec;
            fs::remove(path, ec);
        }
    }
    CHECK(isSupportedFile("a.aif") && isSupportedFile("b.AIFF") && isSupportedFile("c.aifc"), "az AIFF kiterjesztéseket a zenetár is ismeri");
}

// ---------------------------------------------------------------- [12]
/** Ugyanaz a jel (hangok 100 / 230 / 1000 / 5000 / 15000 Hz-en) a megadott mintavételen, 16 ms-os darabokban. */
static SpectrumAnalyzer analyzeTones(double sr, int chunks) {
    SpectrumAnalyzer a(4096, 160);
    a.setSampleRate(sr);
    const size_t n = static_cast<size_t>(sr * 0.016);
    std::vector<float> lr(n * 2);
    uint64_t t = 0;
    for (int c = 0; c < chunks; ++c) {
        for (size_t i = 0; i < n; ++i, ++t) {
            const double x = static_cast<double>(t) / sr;
            const double v = 0.2 * std::sin(2 * kPi * 100 * x) + 0.2 * std::sin(2 * kPi * 230 * x) + 0.1 * std::sin(2 * kPi * 1000 * x) +
                             0.05 * std::sin(2 * kPi * 5000 * x) + 0.05 * std::sin(2 * kPi * 15000 * x);
            lr[2 * i] = lr[2 * i + 1] = static_cast<float>(v);
        }
        a.push(lr.data(), n);
        a.compute(sr);
    }
    return a;
}

static void testAnalyzerHighRate() {
    std::printf("[12] Spektrum és csúcsmérő 384 kHz fölött\n");
    // a referencia a saját családból: 44,1-es család 176,4 kHz, 48-as család 192 kHz (ott a régi, tizedelés nélküli út)
    const SpectrumAnalyzer ref441 = analyzeTones(176400.0, 30), ref48 = analyzeTones(192000.0, 30);
    for (double sr : { 352800.0, 384000.0, 705600.0, 768000.0 }) {
        const SpectrumAnalyzer& ref = (static_cast<long>(sr) % 44100 == 0) ? ref441 : ref48;
        const SpectrumAnalyzer a = analyzeTones(sr, 30);
        double worst = 0.0;
        for (size_t b = 0; b < ref.numBands(); ++b) {
            if (ref.bandCenterHz(b) > 20000.0) break;
            if (ref.levels()[b] < 0.25f && a.levels()[b] < 0.25f) continue;   // csak a jelet hordozó sávok
            worst = std::max(worst, std::abs(static_cast<double>(a.levels()[b] - ref.levels()[b])));
        }
        // a szintek 0..1 skálán, 90 dB-re: 0,02 ≈ 1,8 dB
        CHECK(worst < 0.02, "%.1f kHz: a spektrum a saját család referenciájával egyezik (legnagyobb eltérés %.3f ≈ %.1f dB)", sr / 1000.0,
              worst, worst * 90.0);
        // a 100 és a 230 Hz-es hang közötti sáv (kb. 160 Hz) nem lehet magasabb, mint a referencián: a felbontás nem romlik
        const size_t mid = a.bandOf(160.0);
        CHECK(a.levels()[mid] <= ref.levels()[mid] + 0.01f, "%.1f kHz: a 100 és 230 Hz közötti völgy %.2f (referencia %.2f)", sr / 1000.0,
              a.levels()[mid], ref.levels()[mid]);
    }
    // Csúcsmérő: egy 16 ms-os darab elején 1 ms-os, 0,9-es burst, utána csend -> a mérő látja
    for (double sr : { 192000.0, 768000.0 }) {
        SpectrumAnalyzer a(4096, 160);
        a.setSampleRate(sr);
        const size_t n = static_cast<size_t>(sr * 0.016), burst = static_cast<size_t>(sr * 0.001);
        std::vector<float> lr(n * 2, 0.0f);
        for (size_t i = 0; i < burst; ++i) lr[2 * i] = lr[2 * i + 1] = (i & 1) ? 0.9f : -0.9f;
        a.push(lr.data(), n);
        a.compute(sr);
        const float pk = a.peakDb();
        const bool expectSeen = sr > 192000.0;   // 192 kHz-en a régi viselkedés marad (a csúszó ablak 21 ms)
        if (expectSeen) CHECK(pk > -1.5f && pk < 0.0f, "%.0f kHz: a darab elején lévő csúcsot a mérő látja (%.1f dBFS)", sr / 1000.0, pk);
        else CHECK(pk > -1.5f, "%.0f kHz: a csúcsmérő a régi módon (%.1f dBFS)", sr / 1000.0, pk);
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
    testDownsampleEngines();
    testThreadsBitIdentical();
    testLoadMeter();
    testAdvisor();
    testPlayerHighRate();
    testDedicatedCore();
    testHiresDecoders();
    testAnalyzerHighRate();
    std::printf(g_fail ? "\nHIBA: %d ellenőrzés nem sikerült\n" : "\nMINDEN TESZT SIKERES (0 hiba)\n", g_fail);
    return g_fail ? 1 : 0;
}
