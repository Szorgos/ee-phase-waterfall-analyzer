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
#include "dsd.hpp"
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

/** Sony DSF v1.01 fájl összeállítása tesztekhez. */
static void writeDsfTestFile(const fs::path& path, uint32_t dsdRate, uint32_t channels,
                             const std::vector<std::vector<uint8_t>>& chBytesMsb) {
    constexpr uint32_t kBlockSize = 4096;
    const size_t rawPerCh = chBytesMsb[0].size();
    const uint64_t sampleCount = static_cast<uint64_t>(rawPerCh) * 8ULL;
    const size_t numBlocks = (rawPerCh + kBlockSize - 1) / kBlockSize;
    const uint64_t dataPayloadBytes = static_cast<uint64_t>(numBlocks) * kBlockSize * channels;
    const uint64_t dataChunkSize = 12ULL + dataPayloadBytes;
    const uint64_t totalFileSize = 28ULL + 52ULL + dataChunkSize;

    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(totalFileSize));
    auto w4 = [&](const char* id4) { out.insert(out.end(), id4, id4 + 4); };
    auto le32 = [&](uint32_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    };
    auto le64 = [&](uint64_t v) {
        le32(static_cast<uint32_t>(v & 0xFFFFFFFFULL));
        le32(static_cast<uint32_t>(v >> 32));
    };
    w4("DSD "); le64(28); le64(totalFileSize); le64(0);
    w4("fmt "); le64(52); le32(1); le32(0); le32(channels == 1 ? 1u : 2u); le32(channels);
    le32(dsdRate); le32(1); le64(sampleCount); le32(kBlockSize); le32(0);
    w4("data"); le64(dataChunkSize);
    for (size_t blk = 0; blk < numBlocks; ++blk) {
        for (uint32_t c = 0; c < channels; ++c) {
            for (uint32_t i = 0; i < kBlockSize; ++i) {
                const size_t idx = blk * kBlockSize + i;
                const uint8_t b = (idx < rawPerCh) ? dsd::reverseBits8(chBytesMsb[c][idx]) : 0x00u;
                out.push_back(b);
            }
        }
    }
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
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

template <class O>
concept HasSupportedRates = requires(const O& o) {
    o.supportedRates();
    o.actualRate();
};

template <class AlsaT = AlsaPlayerOutput>
static void testAlsaSupportedRatesK11() {
    std::printf("\n[K11] Kimeneti képességlekérdezés (supportedRates, actualRate)\n");
    if constexpr (HasSupportedRates<AlsaT>) {
        auto alsa = std::make_unique<AlsaT>("null");
        const auto rates = alsa->supportedRates();
        CHECK(!rates.empty(), "ALSA null: supportedRates nem üres (%zu mintavétel)", rates.size());
        CHECK(alsa->actualRate() == 0, "ALSA inaktív: actualRate == 0");
    } else {
        CHECK(false, "K11: PlayerOutput::supportedRates vagy actualRate nincs megvalósítva");
    }
}

/**
 * Foglalt eszköz szimulálása: a megnyitás cseréjével (a valódi eszköz foglaltságát a kernel dönti el, azt itt nem
 * lehet előállítani). Azt bizonyítja, hogy az open() / openDop() időkorláton belül hibával visszatér, és nem akad el.
 */
static void testBusyDevice() {
    std::printf("\n[6] Foglalt hangkártya: az open() időkorláton belül hibával tér vissza (szimulált megnyitás)\n");
    using clk = std::chrono::steady_clock;
    const auto ms = [](clk::time_point a) { return std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - a).count(); };
    const AlsaPlayerOutput::RenderFn silence = [](float* b, size_t n) { std::fill(b, b + n * 2, 0.0f); };
    const AlsaPlayerOutput::RenderDopFn dopSilence = [](int32_t* b, size_t n) { std::fill(b, b + n * 2, 0); };

    {   // a) mindig foglalt: az újrapróbálás után hibával tér vissza
        AlsaPlayerOutput a("null");
        std::atomic<int> calls{0};
        a.setOpenHooksForTest([&](snd_pcm_t**, const char*) { ++calls; return -EBUSY; }, 300, 5000);
        std::string err;
        auto t0 = clk::now();
        const bool ok = a.open(44100, silence, &err);
        const auto took = ms(t0);
        CHECK(!ok && took >= 250 && took < 2000, "open(): foglalt eszköz -> hiba %lld ms alatt (%d próba, ok: \"%s\")",
              static_cast<long long>(took), calls.load(), err.c_str());
        CHECK(calls > 3 && !a.isOpen(), "közben többször újrapróbálta (%d), és a kimenet nincs nyitva", calls.load());
        std::string derr;
        t0 = clk::now();
        const bool okDop = a.openDop(705600, dopSilence, &derr);
        CHECK(!okDop && derr == "busy" && ms(t0) < 2000, "openDop(): foglalt eszköz -> \"busy\" (%s)", derr.c_str());
    }
    {   // b) röviddel később felszabadul: az újrapróbálás megnyitja, és szól
        AlsaPlayerOutput a("null");
        std::atomic<int> calls{0};
        a.setOpenHooksForTest(
            [&](snd_pcm_t** p, const char* dev) {
                if (++calls <= 3) return -EBUSY;
                return snd_pcm_open(p, dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
            },
            2000, 5000);
        std::string err;
        const bool ok = a.open(44100, silence, &err);
        CHECK(ok && a.isOpen() && calls == 4, "felszabaduló eszköz: a %d. próbára megnyílt (%s)", calls.load(), ok ? "" : err.c_str());
        a.setActive(true);
        CHECK(waitUntil([&] { return a.consumedFrames() > 0; }), "az újrapróbálással megnyitott kimenet halad (blokkoló írás)");
        a.close();
        CHECK(!a.isOpen(), "bezáráskor a kimenet bezárul");
    }
    {   // c) a megnyitás elakad (mintha a kernelben aludna): a hívó az időkorlátnál visszatér, a szál később maga takarít
        AlsaPlayerOutput a("null");
        a.setOpenHooksForTest(
            [&](snd_pcm_t** p, const char* dev) {
                std::this_thread::sleep_for(std::chrono::milliseconds(800));
                return snd_pcm_open(p, dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
            },
            3000, 250);
        std::string err;
        auto t0 = clk::now();
        const bool ok = a.open(44100, silence, &err);
        const auto took = ms(t0);
        CHECK(!ok && took >= 200 && took < 700, "elakadt megnyitás: az open() %lld ms után visszatér (ok: \"%s\")",
              static_cast<long long>(took), err.c_str());
        CHECK(!a.isOpen(), "a késve megnyílt eszközt a szál elengedi, nem marad nyitva");
        t0 = clk::now();
        a.close();   // join: a szál a megnyitás végén kilép
        CHECK(ms(t0) < 2000 && !a.isOpen(), "a close() a szál kilépése után visszatér (%lld ms)", static_cast<long long>(ms(t0)));
        a.setOpenHooksForTest({}, 3000, 10000);
        CHECK(a.open(44100, silence, &err) && a.isOpen(), "utána a valódi megnyitás (null eszköz) rendben működik");
    }
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

    std::printf("[5] Natív DSD (DoP 1.1) közvetlen ALSA-kimeneten (null eszköz)\n");
    const fs::path dsfPath = fs::temp_directory_path() / "aq_alsa_dop_test.dsf";
    constexpr uint32_t kDsdRate = 2822400;
    constexpr size_t kDopFrames = 4096;
    constexpr size_t kBytesPerCh = kDopFrames * 2;
    std::vector<uint8_t> leftDsd(kBytesPerCh), rightDsd(kBytesPerCh);
    for (size_t i = 0; i < kBytesPerCh; ++i) {
        leftDsd[i] = static_cast<uint8_t>((i * 37u + 0x12u) & 0xFFu);
        rightDsd[i] = static_cast<uint8_t>((i * 73u + 0xA5u) & 0xFFu);
    }
    writeDsfTestFile(dsfPath, kDsdRate, 2, { leftDsd, rightDsd });

    {
        auto* alsaDop = new AlsaPlayerOutput("null");
        std::mutex dopGotMutex;
        std::vector<int32_t> dopGot;
        alsaDop->setDopTap([&](const int32_t* d, size_t n) {
            std::lock_guard<std::mutex> lk(dopGotMutex);
            dopGot.insert(dopGot.end(), d, d + n * 2);
        });
        auto* sharedDop = new NullShared;
        auto* selDop = new SelectablePlayerOutput(std::unique_ptr<PlayerOutput>(sharedDop), std::unique_ptr<PlayerOutput>(alsaDop));
        selDop->setExclusive(true);
        FilePlayer p{ std::unique_ptr<PlayerOutput>(selDop) };
        p.setDsdDop(true);
        std::string dopErr;
        const bool opened = p.open(dsfPath.string(), &dopErr);
        CHECK(opened && p.isDopActive() && alsaDop->isDopMode() && selDop->exclusiveActive() && sharedDop->opens == 0,
              "DoP megnyitás sikeres a közvetlen ALSA kimeneten (null eszköz): %s",
              opened ? alsaDop->describe().c_str() : dopErr.c_str());
        CHECK(p.dopDsdRate() == kDsdRate, "DoP DSD-ráta egyezik (%u Hz)", p.dopDsdRate());

        const size_t lead = static_cast<size_t>(p.leadInFrames());
        CHECK(lead > 0 && (lead % 2) == 0, "DoP indítási csend: %zu keret (páros keretszám)", lead);

        const bool gotEnough = waitUntil([&] {
            std::lock_guard<std::mutex> lk(dopGotMutex);
            return dopGot.size() >= (lead + 512) * 2;
        });
        CHECK(gotEnough, "DoP keretek megérkeztek a kimenetre");

        std::vector<int32_t> samples;
        {
            std::lock_guard<std::mutex> lk(dopGotMutex);
            samples = dopGot;
        }
        bool leadSilenceOk = samples.size() >= lead * 2;
        bool nonZeroSilence = true;
        for (size_t f = 0; leadSilenceOk && f < lead; ++f) {
            const uint8_t wantMarker = (f % 2 == 0) ? 0x05u : 0xFAu;
            const int32_t wantWord = dsd::packDopSample(dsd::kDsdSilenceByte, dsd::kDsdSilenceByte, wantMarker);
            const int32_t wl = samples[f * 2], wr = samples[f * 2 + 1];
            if (wl == 0 || wr == 0) nonZeroSilence = false;
            if (wl != wantWord || wr != wantWord) leadSilenceOk = false;
        }
        CHECK(leadSilenceOk && nonZeroSilence,
              "DoP indítási csend: minden keret csomagolt 0xAA (0x05AAAA / 0xFAAAAA), soha nem nulla PCM");

        // Csend utáni első keretek: adat a váltakozó jelölővel (fázis 0-val indul)
        bool payloadMarkersOk = samples.size() >= (lead + 64) * 2;
        for (size_t f = 0; payloadMarkersOk && f < 64; ++f) {
            const uint8_t wantMarker = (f % 2 == 0) ? 0x05u : 0xFAu;
            uint8_t ml = 0, mr = 0, b0l = 0, b1l = 0, b0r = 0, b1r = 0;
            dsd::unpackDopSample(samples[(lead + f) * 2], ml, b0l, b1l);
            dsd::unpackDopSample(samples[(lead + f) * 2 + 1], mr, b0r, b1r);
            if (ml != wantMarker || mr != wantMarker) payloadMarkersOk = false;
        }
        CHECK(payloadMarkersOk, "DoP adatkeretek: a jelölők szabályosan váltakoznak (0x05 / 0xFA)");

        p.stop();
        CHECK(!alsaDop->isOpen(), "Leállításkor az ALSA DoP kimenet bezárult");
    }

    // Visszalépés tesztje DoP esetén: nem nyitható közvetlen eszköz -> visszalépés PCM-re
    {
        auto* alsaFail = new AlsaPlayerOutput("hw:99,0");
        auto* sharedFail = new NullShared;
        auto* selFail = new SelectablePlayerOutput(std::unique_ptr<PlayerOutput>(sharedFail), std::unique_ptr<PlayerOutput>(alsaFail));
        selFail->setExclusive(true);
        FilePlayer p{ std::unique_ptr<PlayerOutput>(selFail) };
        p.setDsdDop(true);
        const bool opened = p.open(dsfPath.string());
        CHECK(opened && !p.isDopActive() && !p.dopFallbackReason().empty() && sharedFail->opens == 1,
              "Nem nyitható DoP eszköz: visszalépés PCM-re a megosztott kimeneten (ok: \"%s\")",
              p.dopFallbackReason().c_str());
    }
    // Visszalépés tesztje: kizárólagos mód nincs bekapcsolva -> openDop elutasítva ("notAllowed") -> visszalépés PCM-re
    {
        auto* sharedNoExcl = new NullShared;
        auto* selNoExcl = new SelectablePlayerOutput(std::unique_ptr<PlayerOutput>(sharedNoExcl), std::make_unique<AlsaPlayerOutput>("null"));
        selNoExcl->setExclusive(false);
        FilePlayer p{ std::unique_ptr<PlayerOutput>(selNoExcl) };
        p.setDsdDop(true);
        const bool opened = p.open(dsfPath.string());
        CHECK(opened && !p.isDopActive() && p.dopFallbackReason() == "notAllowed" && sharedNoExcl->opens == 1,
              "Kizárólagos mód nélkül a DoP nem engedélyezett: visszalépés PCM-re (ok: \"%s\")",
              p.dopFallbackReason().c_str());
    }
    fs::remove(dsfPath);
    testAlsaSupportedRatesK11();
    testBusyDevice();
    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
