// A közvetlen ALSA-kimenet tesztje (player_output_alsa.hpp) hangkártya nélkül: az ALSA „null” eszközével ugyanaz a
// kódút fut (megnyitás, formátum, szál, DSP-lánc, kattanásmentes indulás), csak a hang nem szól sehol. A valódi DAC-os
// próba (eszközfoglalás a PipeWire-től, hw: eszköz) élő próba, nem itt.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "chain_player_dsp.hpp"
#include "file_player.hpp"
#include "player_output_alsa.hpp"
#include "player_output_select.hpp"

using namespace ee_analyzer;
using namespace ee_analyzer::player;
namespace fs = std::filesystem;

static int g_fail = 0;
static void CHECK(bool ok, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::printf("  [%s] ", ok ? "OK" : "HIBA");
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
    if (!ok) ++g_fail;
}

/** 16 bites sztereó WAV; a minták float megfelelőjét (x / 32768) visszaadja. */
static std::vector<float> writeWav(const fs::path& p, uint32_t rate, size_t frames) {
    std::vector<int16_t> s(frames * 2);
    std::vector<float> f(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const int16_t l = static_cast<int16_t>(((i * 37) % 2000) - 1000 + 1);   // az első minta sem nulla
        const int16_t r = static_cast<int16_t>(((i * 53) % 3000) - 1500 + 7);
        s[2 * i] = l; s[2 * i + 1] = r;
        f[2 * i] = l / 32768.0f; f[2 * i + 1] = r / 32768.0f;
    }
    std::ofstream o(p, std::ios::binary);
    auto u32 = [&](uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { o.write(reinterpret_cast<const char*>(&v), 2); };
    o.write("RIFF", 4); u32(static_cast<uint32_t>(36 + s.size() * 2)); o.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
    o.write("data", 4); u32(static_cast<uint32_t>(s.size() * 2));
    o.write(reinterpret_cast<const char*>(s.data()), static_cast<std::streamsize>(s.size() * 2));
    return f;
}

/** A DSP kimenetét (az első cap keretet) rögzítő burkoló a valódi lánc körül. */
class SpyDsp : public ExclusiveDsp {
public:
    SpyDsp(ChainPlayerDsp& inner, size_t cap) : inner_(inner), cap_(cap) {}
    void prepare(uint32_t rate, uint32_t block, int bits) override { inner_.prepare(rate, block, bits); prepared = rate; }
    void process(float* s, size_t n) override {
        inner_.process(s, n);
        std::lock_guard<std::mutex> lk(m);
        for (size_t i = 0; i < n * 2 && got.size() < cap_ * 2; ++i) got.push_back(s[i]);
    }
    std::mutex m;
    std::vector<float> got;
    std::atomic<uint32_t> prepared{0};

private:
    ChainPlayerDsp& inner_;
    size_t cap_;
};

/** Megosztott (PipeWire helyett) próba-kimenet: csak a megnyitást jegyzi. */
class NullShared : public PlayerOutput {
public:
    bool open(uint32_t r, RenderFn, std::string*) override { rate = r; ++opens; return true; }
    void close() override {}
    void setActive(bool) override {}
    void lock() override { m.lock(); }
    void unlock() override { m.unlock(); }
    std::mutex m;
    std::atomic<int> opens{0};
    uint32_t rate = 0;
};

static bool waitUntil(const std::function<bool()>& f, int ms = 5000) {
    for (int i = 0; i < ms / 5; ++i) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    return f();
}

int main() {
    std::printf("[1] Eszköznév -> kártyaszám\n");
    CHECK(alsaCardIndex("hw:1,0") == 1 && alsaCardIndex("plughw:2,0") == 2 && alsaCardIndex("hw:3") == 3,
          "hw:N,M / plughw:N,M / hw:N");
    CHECK(alsaCardIndex("null") == -1 && alsaCardIndex("default") == -1 && alsaCardIndex("hw:CARD=nincsilyen,DEV=0") < 0,
          "nem hardveres eszköz és ismeretlen kártyanév: -1 / hiba");
    const auto cards = listAlsaCards();
    std::printf("  (hangkártyák ezen a gépen: %zu; alapértelmezett közvetlen eszköz: \"%s\")\n", cards.size(),
                defaultAlsaDevice().c_str());

    std::printf("[2] Lejátszás kizárólagos módban az ALSA null eszközére, a DSP-láncon át\n");
    const fs::path wav = fs::temp_directory_path() / "aq_alsa_test.wav";
    const size_t frames = 22050;
    const std::vector<float> src = writeWav(wav, 44100, frames);
    ChainPlayerDsp chain(transparentParams());
    const size_t lead = 44100u * FilePlayer::kRateSettleMs / 1000;
    SpyDsp spy(chain, lead + frames + 4096);
    auto* alsa = new AlsaPlayerOutput("null");
    alsa->setExclusiveDsp(&spy);
    auto* shared = new NullShared;
    auto* sel = new SelectablePlayerOutput(std::unique_ptr<PlayerOutput>(shared), std::unique_ptr<PlayerOutput>(alsa));
    sel->setExclusive(true);
    {
        FilePlayer p{ std::unique_ptr<PlayerOutput>(sel) };
        std::string err;
        const bool opened = p.open(wav.string(), &err);
        CHECK(opened && sel->exclusiveActive() && shared->opens == 0, "megnyílt a közvetlen kimeneten (%s)",
              opened ? alsa->describe().c_str() : err.c_str());
        CHECK(spy.prepared == 44100, "a DSP-lánc a fájl mintavételére készült fel (%u Hz)", spy.prepared.load());
        CHECK(sel->settlesOnRateChange(), "a kimenet jelzi, hogy a DAC-nak át kell állnia (kattanásmentes indulás)");
        const bool done = waitUntil([&] { std::lock_guard<std::mutex> lk(spy.m); return spy.got.size() >= (lead + frames) * 2; });
        std::vector<float> got;
        { std::lock_guard<std::mutex> lk(spy.m); got = spy.got; }
        const bool zeros = got.size() >= lead * 2 && std::all_of(got.begin(), got.begin() + lead * 2, [](float v) { return v == 0.0f; });
        const bool exact = done && got.size() >= (lead + frames) * 2 && std::equal(src.begin(), src.end(), got.begin() + lead * 2);
        CHECK(done && zeros && exact, "a DSP kimenete: %zu keret csend, utána a fájl bitpontosan (átlátszó lánc)", lead);
        CHECK(waitUntil([&] { return alsa->consumedFrames() > 0; }), "a kimenet halad (megszólaltatott keretek: %llu)",
              static_cast<unsigned long long>(alsa->consumedFrames()));
        CHECK(alsa->quantExact(), "a float -> egész átalakítás bitpontos volt");
        p.stop();
        CHECK(!alsa->isOpen(), "leállításkor a közvetlen kimenet bezárul");
    }

    std::printf("[3] Visszalépés: a közvetlen eszköz nem nyitható meg\n");
    {
        auto* alsa2 = new AlsaPlayerOutput("hw:99,0");
        auto* shared2 = new NullShared;
        auto* sel2 = new SelectablePlayerOutput(std::unique_ptr<PlayerOutput>(shared2), std::unique_ptr<PlayerOutput>(alsa2));
        sel2->setExclusive(true);
        FilePlayer p{ std::unique_ptr<PlayerOutput>(sel2) };
        const bool opened = p.open(wav.string());
        CHECK(opened && !sel2->exclusiveActive() && shared2->opens == 1 && !sel2->fallback().empty(),
              "nem létező eszköz: a lejátszás a megosztott kimeneten szól, az ok: \"%s\"", sel2->fallback().c_str());
        CHECK(!sel2->settlesOnRateChange(), "megosztott kimeneten nincs kattanásmentes csend");
    }
    {
        auto* shared3 = new NullShared;
        auto* sel3 = new SelectablePlayerOutput(std::unique_ptr<PlayerOutput>(shared3), std::make_unique<AlsaPlayerOutput>("null"));
        FilePlayer p{ std::unique_ptr<PlayerOutput>(sel3) };
        p.open(wav.string());
        CHECK(!sel3->exclusiveActive() && shared3->opens == 1 && sel3->fallback().empty(),
              "kizárólagos mód kérése nélkül a megosztott kimenet szól, visszalépés nélkül");
    }
    std::printf("[4] Valós idejű prioritás nélküli futás (RLIMIT_RTPRIO = 0)\n");
    {
        struct rlimit rlOrig{};
        const bool hadLimit = (getrlimit(RLIMIT_RTPRIO, &rlOrig) == 0);
        if (hadLimit) {
            struct rlimit rlZero = rlOrig;
            rlZero.rlim_cur = 0;
            setrlimit(RLIMIT_RTPRIO, &rlZero);
        }
        auto* alsa4 = new AlsaPlayerOutput("null");
        auto* shared4 = new NullShared;
        auto* sel4 = new SelectablePlayerOutput(std::unique_ptr<PlayerOutput>(shared4), std::unique_ptr<PlayerOutput>(alsa4));
        sel4->setExclusive(true);
        FilePlayer p{ std::unique_ptr<PlayerOutput>(sel4) };
        const bool opened = p.open(wav.string());
        const bool progressed = opened && waitUntil([&] { return alsa4->consumedFrames() > 0; });
        CHECK(opened && progressed, "RT-jog nélkül is elindul és halad a közvetlen lejátszás (RT prioritás: %d)",
              alsa4->realtimePriority());
        p.stop();
        if (hadLimit) setrlimit(RLIMIT_RTPRIO, &rlOrig);
    }
    fs::remove(wav);
    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
