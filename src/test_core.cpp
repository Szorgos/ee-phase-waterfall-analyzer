// Platformfüggetlen magteszt: FFT spektrum, paraméter-alkalmazás, seqlock, tap körpuffer.
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <future>
#include <iterator>
#include <memory>
#include <limits>
#include <random>
#include <thread>
#include <vector>

#include "spectrum.hpp"
#include "param_block.hpp"
#include "tap_ring.hpp"
#include "mini_json.hpp"
#include "dsp_state.hpp"
#include "self_path.hpp"
#include "ws_bridge.hpp"
#include "visual_source.hpp"
#include "pair_cli.hpp"
#include "sha256.hpp"
#include "update_logic.hpp"
#include "media_url.hpp"
#include "multi_output_config.hpp"
#include "audio_options.hpp"
#include "device_follow.hpp"
#ifndef _WIN32
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include "linux_store.hpp"
#endif

using namespace ee_analyzer;

static constexpr double kPi = 3.14159265358979323846;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; std::printf("  [HIBA] " __VA_ARGS__); std::printf("\n"); } else { std::printf("  [OK]   " __VA_ARGS__); std::printf("\n"); } } while (0)

static size_t bandOf(double hz) { return SpectrumAnalyzer(4096, 160).bandOf(hz); }

/** frames sztereó minta (L = R) a gen(i) értékeivel, darabokban betolva (mint a motor hídja). */
template <typename Gen>
static void pushMono(SpectrumAnalyzer& an, size_t frames, Gen gen) {
    std::vector<float> lr(1024 * 2);
    for (size_t done = 0; done < frames;) {
        const size_t n = std::min<size_t>(1024, frames - done);
        for (size_t i = 0; i < n; ++i) lr[2 * i] = lr[2 * i + 1] = static_cast<float>(gen(done + i));
        an.push(lr.data(), n);
        done += n;
    }
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
    const double farLeakDb = lv[bandOf(100.0)] * 90.0 - 90.0;
    CHECK(farLeakDb < -60.0, "100 Hz-en %.1f dBFS (szivárgás < -60 dB)", farLeakDb);
    CHECK(std::abs(an.phaseDegrees()[peak]) < 1.0, "azonos L/R -> fáziskülönbség %.2f°", an.phaseDegrees()[peak]);

    // L és R ellenfázisban -> 180°
    for (size_t i = 0; i < 4096; ++i) lr[2 * i + 1] = -lr[2 * i];
    an.push(lr.data(), 4096);
    an.compute(sr);
    CHECK(std::abs(std::abs(an.phaseDegrees()[peak]) - 180.0) < 1.0, "ellenfázis -> %.1f°", an.phaseDegrees()[peak]);

    // Csend (a mély elemzés hosszabb ablaka is kiürül)
    pushMono(an, 48000, [](size_t) { return 0.0; });
    an.compute(sr);
    float mx = 0;
    for (float v : an.levels()) mx = std::max(mx, v);
    CHECK(mx == 0.0f, "csendnél minden sáv 0");

    // Tengely: 0 Hz-től, a 10 Hz fölött közel logaritmikus
    CHECK(an.bandEdgeHz(0) == 0.0 && std::abs(an.bandEdgeHz(160) - 24000.0) < 0.5 && bandOf(0.0) == 0 &&
          bandOf(10.0) > 10 && bandOf(10.0) < 20, "sávok 0 Hz – 24 kHz, a 10 Hz a %zu. sáv", bandOf(10.0));

    // Mély sávok: 50 Hz-es hang a helyén, a szivárgás nem ér le a legalsó sávokig
    pushMono(an, 48000, [&](size_t i) { return amp * std::sin(2.0 * kPi * 50.0 * i / sr); });
    an.compute(sr);
    {
        const auto& v = an.levels();
        size_t pk = 0;
        for (size_t i = 1; i < v.size(); ++i) if (v[i] > v[pk]) pk = i;
        const double pkDb = v[pk] * 90.0 - 90.0, at20 = v[bandOf(20.0)] * 90.0 - 90.0, at0 = v[0] * 90.0 - 90.0;
        CHECK(pk + 1 >= bandOf(50.0) && pk <= bandOf(50.0) + 1 && std::abs(pkDb + 6.02) < 1.5,
              "50 Hz: csúcs a %zu. sávban (várt: %zu), %.2f dBFS", pk, bandOf(50.0), pkDb);
        CHECK(at20 < -60.0 && at0 < -70.0, "50 Hz: 20 Hz-en %.1f dBFS, 0 Hz-en %.1f dBFS", at20, at0);
    }
    // 5 Hz-es hang: a 10 Hz alatti tartomány is látszik
    pushMono(an, 48000, [&](size_t i) { return amp * std::sin(2.0 * kPi * 5.0 * i / sr); });
    an.compute(sr);
    {
        const auto& v = an.levels();
        size_t pk = 0;
        for (size_t i = 1; i < v.size(); ++i) if (v[i] > v[pk]) pk = i;
        const double pkDb = v[pk] * 90.0 - 90.0, at40 = v[bandOf(40.0)] * 90.0 - 90.0;
        CHECK(pk + 2 >= bandOf(5.0) && pk <= bandOf(5.0) + 2 && pkDb > -9.0 && at40 < -60.0,
              "5 Hz: csúcs a %zu. sávban (várt: %zu), %.2f dBFS; 40 Hz-en %.1f dBFS", pk, bandOf(5.0), pkDb, at40);
    }
    // Egyenáram (DC eltolás): nem jelenik meg
    pushMono(an, 48000, [](size_t) { return 0.25; });
    an.compute(sr);
    {
        float low = 0.0f;
        for (size_t i = 0; i < bandOf(100.0); ++i) low = std::max(low, an.levels()[i]);
        CHECK(low == 0.0f, "egyenáramú eltolás: 100 Hz alatt nincs jel (%.3f)", low);
    }

    // 44,1 kHz: ugyanazok a sávhatárok (a skála nem csúszik el), a Nyquist fölötti sávok üresek
    const double sr2 = 44100.0;
    for (size_t i = 0; i < 4096; ++i) {
        const float v = static_cast<float>(amp * std::sin(2.0 * kPi * 1000.0 * i / sr2));
        lr[2 * i] = v;
        lr[2 * i + 1] = v;
    }
    an.push(lr.data(), 4096);
    an.compute(sr2);
    size_t peak2 = 0;
    for (size_t i = 1; i < an.levels().size(); ++i) if (an.levels()[i] > an.levels()[peak2]) peak2 = i;
    float above = 0.0f;
    for (size_t i = bandOf(22100.0) + 1; i < an.levels().size(); ++i) above = std::max(above, an.levels()[i]);
    CHECK(peak2 + 1 >= expected && peak2 <= expected + 1 && above == 0.0f,
          "44,1 kHz: 1 kHz a %zu. sávban (várt: %zu), a 22,05 kHz feletti sávok üresek", peak2, expected);
}

static void testParamFile() {
    std::printf("[2] Paraméterfájl (v6, valamint a v5, a v4 és a régi v3 átvétele)\n");
    // v4: változatlan visszaolvasás
    ParamData d = transparentParams();
    d.set(kFlagEq, true);
    d.numBands = 2;
    d.bands[0].type = static_cast<int32_t>(EqBandType::Bell);
    d.bands[0].mode = static_cast<int32_t>(EqFilterMode::BwcMt);
    d.bands[0].slope = 2;
    d.bands[0].freq = 1000.0f; d.bands[0].gainDb = 6.0f; d.bands[0].q = 1.0f;
    d.bands[1].type = static_cast<int32_t>(EqBandType::LadderPass);
    d.bands[1].width = 2.5f;
    d.eqMode = static_cast<int32_t>(EqMode::Spm);
    d.crossfeedFeedDb = 9.5f;
    ParamData r{};
    CHECK(loadParamBytes(reinterpret_cast<const char*>(&d), sizeof(d), kParamLayoutVersion, sizeof(d), r) &&
              std::memcmp(&r, &d, sizeof(d)) == 0, "v6 paraméterblokk bájtra azonosan visszaolvasva");

    // v4 (Magic Touch előtti) fájl: a meglévő beállítások megmaradnak, a Magic Touch alapértéket kap
    ParamData r4{};
    CHECK(loadParamBytes(reinterpret_cast<const char*>(&d), kParamSizeV4, 4, static_cast<uint32_t>(kParamSizeV4), r4) &&
              std::memcmp(&r4, &d, kParamSizeV4) == 0 && r4.magicAmount == 50.0f && r4.magicCompare == 0 &&
              !r4.has(kFlagMagic),
          "v4 paraméterfájl átvétele (Magic Touch: kikapcsolva, 50 %%)");
    // Az A/B összehasonlítás újraindítás után nem marad bekapcsolva; a Karakter és a Tartalom megmarad
    ParamData dm = d;
    dm.set(kFlagMagic, true); dm.magicAmount = 70.0f; dm.magicCompare = 1; dm.magicCharacter = 2; dm.magicContent = 3;
    ParamData rm{};
    CHECK(loadParamBytes(reinterpret_cast<const char*>(&dm), sizeof(dm), kParamLayoutVersion, sizeof(dm), rm) &&
              rm.has(kFlagMagic) && rm.magicAmount == 70.0f && rm.magicCompare == 0 && rm.magicCharacter == 2 &&
              rm.magicContent == 3,
          "Magic Touch beállítás mentve (70 %%, Fényes, Archív), az A/B nem marad bekapcsolva");
    // v5 (Karakter és Tartalom előtti) fájl: a Magic Touch beállítása megmarad, a két választó alapértéket kap
    ParamData r5{};
    CHECK(loadParamBytes(reinterpret_cast<const char*>(&dm), kParamSizeV5, 5, static_cast<uint32_t>(kParamSizeV5), r5) &&
              r5.has(kFlagMagic) && r5.magicAmount == 70.0f && r5.magicCharacter == 0 && r5.magicContent == 0,
          "v5 paraméterfájl átvétele (Karakter: Természetes, Tartalom: Automatikus)");
    // v6 (mélyvágás előtti) fájl: a beállítások megmaradnak, a mélyvágás kikapcsolt
    ParamData r6{};
    CHECK(loadParamBytes(reinterpret_cast<const char*>(&dm), kParamSizeV6, 6, static_cast<uint32_t>(kParamSizeV6), r6) &&
              r6.has(kFlagMagic) && r6.magicAmount == 70.0f && r6.subsonicHz == 0,
          "v6 paraméterfájl átvétele (Mélyvágás: ki)");
    // v7 (mono-keverés előtti) fájl: a beállítások megmaradnak, a mono-keverés 0 %
    ParamData r7{};
    CHECK(loadParamBytes(reinterpret_cast<const char*>(&dm), kParamSizeV7, 7, static_cast<uint32_t>(kParamSizeV7), r7) &&
              r7.magicAmount == 70.0f && r7.monoAmount == 0.0f,
          "v7 paraméterfájl átvétele (Mono-keverés: 0 %%)");
    // v14 (külön bal / jobb EQ előtti) fájl: a beállítások megmaradnak, eqSplit = 0
    ParamData r14{};
    CHECK(loadParamBytes(reinterpret_cast<const char*>(&dm), kParamSizeV14, 14, static_cast<uint32_t>(kParamSizeV14), r14) &&
              r14.eqSplit == 0 && r14.numBandsR == 0,
          "v14 paraméterfájl átvétele (külön bal/jobb: ki)");
    // Tartományon kívüli választó: a megengedett tartományba kerül
    ParamData dx = d;
    dx.magicCharacter = 9; dx.magicContent = -4;
    ParamData rx{};
    CHECK(loadParamBytes(reinterpret_cast<const char*>(&dx), sizeof(dx), kParamLayoutVersion, sizeof(dx), rx) &&
              rx.magicCharacter == 3 && rx.magicContent == 0,
          "hibás Karakter / Tartalom érték a tartományba vágva");

    // v3: a régi RBJ sávok "APO (DR)" módú EasyEffects sávokká válnak
    legacy::ParamDataV3 o{};
    o.preampDb = -4.0f;
    o.flags = kFlagEq | kFlagLimiter;
    o.numBands = 2;
    o.bands[0] = { 0, 105.0f, 5.5f, 0.7f };   // Peaking
    o.bands[1] = { 2, 8000.0f, -3.0f, 0.71f }; // HighShelf
    o.eqMode = 0;
    o.limiterCeilingDb = -1.5f;
    o.eqDecramp = 4;
    o.crossfeedCutoffHz = 650.0f;
    o.crossfeedFeedDb = 6.0f;
    ParamData m{};
    const bool ok = loadParamBytes(reinterpret_cast<const char*>(&o), sizeof(o), 3, sizeof(o), m);
    CHECK(ok && m.numBands == 2 && m.bands[0].type == static_cast<int32_t>(EqBandType::Bell) &&
              m.bands[1].type == static_cast<int32_t>(EqBandType::HiShelf) &&
              m.bands[0].mode == static_cast<int32_t>(EqFilterMode::ApoDr) && m.bands[0].freq == 105.0f &&
              m.bands[1].gainDb == -3.0f && m.preampDb == -4.0f && m.has(kFlagEq) && m.has(kFlagLimiter),
          "v3 sávok, preamp és modulok átvéve");
    CHECK(m.limThresholdDb == -1.5f && m.eqDecramp == 3 && m.crossfeedFcut == 650.0f && m.crossfeedFeedDb == 6.0f,
          "v3 limiter plafon -> küszöb, decramp 4x -> x4, crossfeed átvéve");

    // Határok (EasyEffects tartományok)
    ParamData x{};
    x.numBands = 99;
    x.bands[0].type = 77; x.bands[0].freq = 5.0f; x.bands[0].q = 500.0f;
    x.limOversampling = 40; x.crossfeedFeedDb = 0.0f; x.convIrWidth = 999;
    sanitizeParams(x);
    CHECK(x.numBands == kMaxEqBands && x.bands[0].type == kEqBandTypeCount - 1 && x.bands[0].freq == 10.0f &&
              x.bands[0].q == 100.0f && x.limOversampling == 22 && x.crossfeedFeedDb == 1.0f && x.convIrWidth == 200,
          "tartományon kívüli értékek levágva");
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
            // Kis szünet: 1-2 magos CI-gépen a folyamatosan pörgő író különben kiéheztetné az olvasót
            if ((k & 63u) == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
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
    CHECK(reads > 100 && torn == 0, "%zu sikeres olvasás, %zu szakadt pillanatkép", reads, torn);
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

static void testDspState() {
    std::printf("[6] Hangfeldolgozás állapota (szünet / hiba / másik eszköz / nincs eszköz)\n");
    DspStateInput in;
    in.device = in.apoDevice = "Hangszórók";
    in.pluginAlive = true;
    CHECK(classifyDspState(in) == DspState::Active, "a plugin fut: aktív");
    in.pluginAlive = false;
    CHECK(classifyDspState(in) == DspState::Idle, "ugyanaz az eszköz, nincs hang: szünet (nem hiba)");
    in.playingMs = 2900;
    CHECK(classifyDspState(in) == DspState::Idle, "2,9 s zene plugin nélkül: még szünet (a plugin indulhat)");
    in.playingMs = 3000;
    CHECK(classifyDspState(in) == DspState::NotLoaded, "3 s zene plugin nélkül: hiba");
    in.playingMs = 0;
    in.device = "USB DAC";
    CHECK(classifyDspState(in) == DspState::OtherDevice, "másik eszköz: jelezve, lejátszás nélkül is");
    in.deviceOk = false;
    CHECK(classifyDspState(in) == DspState::NoDevice, "nincs lejátszóeszköz");
    in.deviceOk = true;
    in.apoDevice.clear();
    CHECK(classifyDspState(in) == DspState::Idle, "még sosem futott plugin, nincs hang: szünet");
    in.playingMs = 5000;
    CHECK(classifyDspState(in) == DspState::NotLoaded, "még sosem futott plugin, szól a zene: hiba");

    // A plugin él, de egy másik eszközön: a mostani eszközön nincs Equalizer APO (pl. új USB DAC, 2.6.1)
    DspStateInput o;
    o.device = o.apoDevice = "Hangszórók (USB DAC)";
    o.pluginAlive = true;
    o.apoOnDevice = ApoOnDevice::No;
    CHECK(!pluginServesDevice(true, ApoOnDevice::No), "élő plugin, de a mostani eszközön nincs APO: nem ezt dolgozza fel");
    CHECK(classifyDspState(o) == DspState::OtherDevice, "élő plugin másik eszközön: „másik eszköz”, nem „aktív”");
    o.playingMs = 0;
    o.pluginAlive = false;
    CHECK(classifyDspState(o) == DspState::OtherDevice, "nincs APO az eszközön, plugin sem fut: „másik eszköz” (szünetben is)");
    o.deviceOk = false;
    CHECK(classifyDspState(o) == DspState::NoDevice, "eszköz nélkül a „nincs eszköz” az erősebb");
    o.deviceOk = true;
    o.pluginAlive = true;
    o.apoOnDevice = ApoOnDevice::Unknown;
    CHECK(pluginServesDevice(true, ApoOnDevice::Unknown) && classifyDspState(o) == DspState::Active,
          "ismeretlen (a regisztrációs adatbázis nem olvasható): nincs téves riasztás, a plugin a forrás");
    o.apoOnDevice = ApoOnDevice::Yes;
    CHECK(pluginServesDevice(true, ApoOnDevice::Yes) && classifyDspState(o) == DspState::Active, "APO az eszközön, plugin fut: aktív");
    CHECK(!pluginServesDevice(false, ApoOnDevice::Yes), "APO az eszközön, de nincs élő plugin: nem aktív");

    // Lejátszás felismerése a jelszintből (48 kHz, 10 ms-os blokkok)
    const double sr = 48000.0;
    std::vector<float> tone(480 * 2), quiet(480 * 2, 1e-5f), zero(480 * 2, 0.0f);
    for (size_t i = 0; i < 480; ++i) tone[2 * i] = tone[2 * i + 1] = 0.1f * static_cast<float>(std::sin(2 * 3.14159265 * 440 * i / sr));
    PlaybackDetector pd;
    for (int i = 0; i < 100; ++i) pd.push(quiet.data(), 480, sr);
    CHECK(pd.playingMs() == 0.0, "zajpadló (−100 dBFS) 1 s-ig: nem lejátszás");
    for (int i = 0; i < 350; ++i) pd.push(tone.data(), 480, sr);
    const double after = pd.playingMs();
    for (int i = 0; i < 100; ++i) pd.push(zero.data(), 480, sr);   // 1 s szünet két szám között
    const double gap = pd.playingMs();
    for (int i = 0; i < 100; ++i) pd.push(tone.data(), 480, sr);
    CHECK(std::abs(after - 3500.0) < 1.0 && gap > after && pd.playingMs() > gap,
          "3,5 s zene: %.0f ms; 1 s csend nem szakítja meg (%.0f ms, utána %.0f ms)", after, gap, pd.playingMs());
    for (int i = 0; i < 200; ++i) pd.push(zero.data(), 480, sr);
    CHECK(pd.playingMs() == 0.0, "2 s csend: a lejátszás véget ért");
    for (int i = 0; i < 100; ++i) pd.push(tone.data(), 480, sr);
    pd.idle(2000.0);
    CHECK(pd.playingMs() == 0.0, "2 s adat nélkül (a lehallgatás nem ad mintát): nincs lejátszás");
}

/** A motor saját elérési útja (T28): Linuxon a futó program teljes útja, máshol üres. */
static void testSelfPath() {
    std::printf("[7] A futó program elérési útja (verzióeltérés diagnosztikája)\n");
    const std::string p = selfExePath();
#if defined(__linux__)
    const std::string tail = "/test_core";
    CHECK(p.size() > tail.size() && p.compare(p.size() - tail.size(), tail.size(), tail) == 0 && p[0] == '/',
          "Linux: %s", p.c_str());
#else
    CHECK(p.empty(), "nem Linux: üres (a felület nem ír ki elérési utat)");
#endif
}

/** NaN-védelem a beállításokban: csak -ffast-math nélkül működik (a fordító ott a v == v-t mindig igaznak veszi). */
static void testNanGuard() {
    std::printf("[8] Érvénytelen (NaN) beállítás kiszűrése\n");
    ParamData d = transparentParams();
    volatile float nan = std::numeric_limits<float>::quiet_NaN();
    d.preampDb = nan;
    d.bassScopeHz = nan;
    d.agcTargetLufs = nan;
    sanitizeParams(d);
    CHECK(d.preampDb == 0.0f && d.bassScopeHz == 100.0f && d.agcTargetLufs == -18.0f,
          "NaN -> alapérték (preamp %.1f dB, scope %.1f Hz, célszint %.1f LUFS)", d.preampDb, d.bassScopeHz, d.agcTargetLufs);
}

/** DNS-név olvasása a csomagból, a tömörítő mutatók követésével; hibás csomagnál üres. */
static std::string dnsReadName(const std::vector<uint8_t>& p, size_t& off) {
    std::string out;
    size_t pos = off;
    bool jumped = false;
    for (int guard = 0; guard < 64 && pos < p.size(); ++guard) {
        const uint8_t len = p[pos];
        if (len == 0) {
            if (!jumped) off = pos + 1;
            return out;
        }
        if ((len & 0xC0) == 0xC0) {
            if (pos + 1 >= p.size()) return "";
            if (!jumped) off = pos + 2;
            pos = (static_cast<size_t>(len & 0x3F) << 8) | p[pos + 1];
            jumped = true;
            continue;
        }
        if (pos + 1 + len > p.size()) return "";
        if (!out.empty()) out += '.';
        out.append(reinterpret_cast<const char*>(&p[pos + 1]), len);
        pos += 1 + len;
    }
    return "";
}

struct DnsRecord { std::string name; uint16_t type; std::string target; };

/** A válaszrekordok végigolvasása (név, típus; PTR és SRV esetén a cél neve). */
static std::vector<DnsRecord> dnsReadAnswers(const std::vector<uint8_t>& p) {
    std::vector<DnsRecord> out;
    if (p.size() < 12) return out;
    const int count = (p[6] << 8) | p[7];
    size_t off = 12;
    for (int i = 0; i < count; ++i) {
        DnsRecord r;
        r.name = dnsReadName(p, off);
        if (r.name.empty() || off + 10 > p.size()) return {};
        r.type = static_cast<uint16_t>((p[off] << 8) | p[off + 1]);
        const size_t rdlen = (static_cast<size_t>(p[off + 8]) << 8) | p[off + 9];
        size_t rd = off + 10;
        if (rd + rdlen > p.size()) return {};
        if (r.type == 12) r.target = dnsReadName(p, rd);
        else if (r.type == 33) { rd += 6; r.target = dnsReadName(p, rd); }
        off = off + 10 + rdlen;
        out.push_back(r);
    }
    return off == p.size() ? out : std::vector<DnsRecord>{};
}

static void testMdns() {
    std::printf("[9] mDNS hálózati bejelentés és csomagépítés (RFC 6762)\n");
    MdnsAnnouncer an;
    an.start(47820, "Aqulify Teszt PC");
    CHECK(an.isRunning(), "mDNS hirdető elindult a háttérszálon");
    CHECK(an.servicePort() == 47820, "mDNS port 47820");
    CHECK(an.instanceName() == "Aqulify Teszt PC", "mDNS egyedi példánynév");

    // 1. Érvényes bejelentő csomag felépítése
    auto pkt = an.buildPacket(120);
    CHECK(pkt.size() > 80 && pkt.size() < 512, "csomagméret érvényes (%zu bájt)", pkt.size());
    // DNS fejléc ellenőrzése: ID=0, Flags=0x8400, QD=0, AN=3
    CHECK(pkt[0] == 0x00 && pkt[1] == 0x00, "DNS ID: 0x0000");
    CHECK(pkt[2] == 0x84 && pkt[3] == 0x00, "DNS Flags: 0x8400 (Authoritative Response)");
    CHECK(pkt[4] == 0x00 && pkt[5] == 0x00, "QDCOUNT: 0");
    CHECK(pkt[6] == 0x00 && pkt[7] == 0x03, "ANCOUNT: 3 (PTR, SRV, TXT)");

    // Tartalmi ellenőrzések
    std::string s(pkt.begin(), pkt.end());
    CHECK(s.find("_aqulify") != std::string::npos && s.find("_tcp") != std::string::npos && s.find("local") != std::string::npos,
          "csomag tartalmazza a szolgáltatástípust (_aqulify._tcp.local)");
    CHECK(s.find("port=47820") != std::string::npos, "TXT rekord tartalmazza a portot (port=47820)");
    CHECK(s.find("ver=" AQULIFY_VERSION) != std::string::npos, "TXT rekord tartalmazza a verziót (ver=" AQULIFY_VERSION ")");
    CHECK(s.find("proto=2") != std::string::npos, "TXT rekord tartalmazza a protokollt (proto=2)");
    CHECK(s.find("caps=library,queue,playlists,phone_track") != std::string::npos, "TXT rekord képességek (caps)");

    // 2. Búcsúcsomag (TTL=0)
    auto byePkt = an.buildPacket(0);
    CHECK(byePkt.size() == pkt.size(), "búcsúcsomag mérete megegyezik a bejelentéssel");
    CHECK(byePkt[2] == 0x84 && byePkt[3] == 0x00, "búcsúcsomag flags 0x8400");

    an.stop();
    CHECK(!an.isRunning(), "mDNS hirdető leállt és búcsúcsomagot küldött");

    // 3. Címekkel: A-rekordok (a kliens ezekből oldja fel a gépet) és TXT "addr"; érvénytelen cím kimarad
    MdnsAnnouncer an2;
    an2.start(47820, "Aqulify Teszt PC", { "192.168.1.20", "10.0.0.5", "nem-cim" });
    auto p2 = an2.buildPacket(120);
    const int ancount = (p2[6] << 8) | p2[7];
    CHECK(ancount == 5, "ANCOUNT két címmel: 5 (PTR, SRV, TXT + 2 A-rekord), kapott: %d", ancount);
    CHECK(an2.addresses().size() == 2, "az érvénytelen cím kimaradt (%zu cím)", an2.addresses().size());
    std::string s2(p2.begin(), p2.end());
    CHECK(s2.find("addr=192.168.1.20,10.0.0.5") != std::string::npos, "TXT rekord tartalmazza a címeket (addr=...)");
    const uint8_t a1[] = { 0x00, 0x01, 0x80, 0x01, 0x00, 0x00, 0x00, 0x78, 0x00, 0x04, 192, 168, 1, 20 };
    const uint8_t a2[] = { 0x00, 0x01, 0x80, 0x01, 0x00, 0x00, 0x00, 0x78, 0x00, 0x04, 10, 0, 0, 5 };
    CHECK(std::search(p2.begin(), p2.end(), std::begin(a1), std::end(a1)) != p2.end() &&
          std::search(p2.begin(), p2.end(), std::begin(a2), std::end(a2)) != p2.end(),
          "A-rekordok: típus 1, IN|flush, TTL 120, 4 bájtos cím (192.168.1.20, 10.0.0.5)");
    CHECK(p2.size() < 512, "csomagméret címekkel is érvényes (%zu bájt)", p2.size());
    // A tömörített nevek (mutatók) feloldva: a rekordok a helyes névre szólnak, a csomag végig olvasható
    const auto recs = dnsReadAnswers(p2);
    const std::string inst = "Aqulify Teszt PC._aqulify._tcp.local";
    std::string host;
    for (const auto& r : recs) if (r.type == 33) host = r.target;
    bool namesOk = recs.size() == 5 && recs[0].type == 12 && recs[0].name == "_aqulify._tcp.local" &&
                   recs[0].target == inst && recs[1].type == 33 && recs[1].name == inst && recs[2].type == 16 &&
                   recs[2].name == inst && host.size() > 6 && host.compare(host.size() - 6, 6, ".local") == 0;
    for (size_t i = 3; i < recs.size(); ++i) namesOk = namesOk && recs[i].type == 1 && recs[i].name == host;
    CHECK(namesOk, "a rekordnevek a tömörítés feloldása után helyesek (SRV cél: %s)", host.c_str());
    CHECK(dnsReadAnswers(pkt).size() == 3, "a címek nélküli csomag is végig olvasható (3 rekord)");
    an2.stop();
}

static void testPushPairing() {
    std::printf("[10] Kód nélküli jóváhagyásos párosítási protokoll (8b: push prompt)\n");
    // 1. URL lekérdezési paraméterek dekódolása
    const std::string httpReq = "GET /?pair=request&name=Samsung+Galaxy+S23&client=aqulify_remote&tier=paid HTTP/1.1\r\n";
    CHECK(bridge_detail::queryParam(httpReq, "pair") == "request", "pair=request paraméter felismerése");
    CHECK(bridge_detail::queryParam(httpReq, "name") == "Samsung Galaxy S23", "name paraméter URL dekódolása (szóköz)");
    CHECK(bridge_detail::queryParam(httpReq, "client") == "aqulify_remote", "client paraméter (aqulify_remote)");
    CHECK(bridge_detail::queryParam(httpReq, "tier") == "paid", "tier paraméter (paid fizetős kliens)");

    // 2. Értesítési JSON üzenet felépítése a helyi PC felület számára
    const std::string reqId = "req_a1b2c3d4";
    const std::string name = bridge_detail::cleanName(bridge_detail::queryParam(httpReq, "name"));
    const std::string clientType = bridge_detail::queryParam(httpReq, "client");
    const std::string tier = bridge_detail::queryParam(httpReq, "tier");
    const std::string ip = "192.168.1.85";

    std::string notice = "{\"type\":\"pair_request\",\"requestId\":\"" + reqId +
                         "\",\"name\":\"" + media::jsonEscape(name) +
                         "\",\"ip\":\"" + media::jsonEscape(ip) + "\"";
    if (!clientType.empty()) notice += ",\"client\":\"" + media::jsonEscape(clientType) + "\"";
    if (!tier.empty()) notice += ",\"tier\":\"" + media::jsonEscape(tier) + "\"";
    notice += "}";

    json::Value parsedNotice;
    CHECK(json::parse(notice, parsedNotice) && parsedNotice.type == json::Value::Type::Object, "pair_request JSON értelmezhető");
    CHECK(parsedNotice.get("type") && parsedNotice.get("type")->stringOr("") == "pair_request", "üzenettípus: pair_request");
    CHECK(parsedNotice.get("requestId") && parsedNotice.get("requestId")->stringOr("") == "req_a1b2c3d4", "requestId pontos");
    CHECK(parsedNotice.get("name") && parsedNotice.get("name")->stringOr("") == "Samsung Galaxy S23", "eszköznév pontos");
    CHECK(parsedNotice.get("ip") && parsedNotice.get("ip")->stringOr("") == "192.168.1.85", "kliens IP pontos");
    CHECK(parsedNotice.get("client") && parsedNotice.get("client")->stringOr("") == "aqulify_remote", "kliens típus: aqulify_remote");
    CHECK(parsedNotice.get("tier") && parsedNotice.get("tier")->stringOr("") == "paid", "fizetős szint (tier=paid)");

    // 3. Helyi felület jóváhagyási üzenete (pair_approve)
    const std::string approveMsg = "{\"type\":\"pair_approve\",\"requestId\":\"req_a1b2c3d4\",\"approve\":true}";
    json::Value parsedApprove;
    CHECK(json::parse(approveMsg, parsedApprove), "pair_approve JSON értelmezhető");
    CHECK(parsedApprove.get("type")->stringOr("") == "pair_approve", "üzenettípus: pair_approve");
    CHECK(parsedApprove.get("approve") && parsedApprove.get("approve")->b == true, "jóváhagyás értéke true");

    // 4. Kliens token generálás és tárolási formátum
    const std::string devId = bridge_detail::randomHex(4);
    const std::string token = bridge_detail::randomHex(32);
    CHECK(devId.size() == 8, "eszköz azonosító 8 hexadecimális jegy");
    CHECK(token.size() == 64, "titkos token 64 hexadecimális jegy (256 bit)");
    const std::string hash = bridge_detail::sha1Hex(token);
    CHECK(hash.size() == 40, "tárolt token hash 40 hexadecimális jegy (SHA-1)");
    CHECK(bridge_detail::sameSecret(hash, bridge_detail::sha1Hex(token)), "időzítésbiztos hash egyezés");

    // 5. Elutasítás vagy megszakítás (pair_cancel)
    const std::string cancelMsg = "{\"type\":\"pair_cancel\",\"requestId\":\"req_a1b2c3d4\"}";
    json::Value parsedCancel;
    CHECK(json::parse(cancelMsg, parsedCancel), "pair_cancel JSON értelmezhető");
    CHECK(parsedCancel.get("type")->stringOr("") == "pair_cancel", "üzenettípus: pair_cancel");

    // 6. Harmadik féltől származó távirányító azonnali elutasítása
    const std::string thirdPartyReq = "GET /?pair=request&name=UnauthorizedApp&client=third_party HTTP/1.1\r\n";
    const std::string thirdPartyClient = bridge_detail::cleanName(bridge_detail::queryParam(thirdPartyReq, "client"));
    CHECK(thirdPartyClient != "aqulify_remote", "harmadik féltől származó kliens felismerve és kizárva");
}

/** Élő WebSocket-kapcsolat a hídhoz (valódi socket, kézfogás); kInvalidSocket, ha nem sikerült. */
static net::socket_t wsConnectLive(int port, const std::string& path, std::string* leftover = nullptr, int timeoutMs = 0) {
    net::socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == net::kInvalidSocket) return s;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { net::closeSocket(s); return net::kInvalidSocket; }
    if (timeoutMs > 0) {   // beragadt híd esetén a kézfogás ne várjon örökké
#ifdef _WIN32
        DWORD tv = static_cast<DWORD>(timeoutMs);
#else
        timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
#endif
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    }
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    ::send(s, req.data(), static_cast<int>(req.size()), 0);
    std::string resp;
    char c = 0;
    while (resp.size() < 4096 && resp.find("\r\n\r\n") == std::string::npos) {
        const int n = static_cast<int>(::recv(s, &c, 1, 0));
        if (n <= 0) break;
        resp.push_back(c);
    }
    if (resp.rfind("HTTP/1.1 101", 0) != 0) { net::closeSocket(s); return net::kInvalidSocket; }
    if (leftover) leftover->clear();
    return s;
}

/** A kézfogásra adott teljes HTTP-válasz (fejlécekkel); üres, ha nem sikerült kapcsolódni. */
static std::string wsHandshakeLive(int port, const std::string& path) {
    net::socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == net::kInvalidSocket) return "";
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { net::closeSocket(s); return ""; }
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    ::send(s, req.data(), static_cast<int>(req.size()), 0);
    std::string resp;
    char c = 0;
    while (resp.size() < 4096 && resp.find("\r\n\r\n") == std::string::npos) {
        const int n = static_cast<int>(::recv(s, &c, 1, 0));
        if (n <= 0) break;
        resp.push_back(c);
    }
    net::closeSocket(s);
    return resp;
}

/** Maszkolt szöveges keret (a kliens minden keretét maszkolja, RFC 6455). */
static void wsSendText(net::socket_t s, const std::string& text) {
    std::string f;
    f.push_back(static_cast<char>(0x81));
    if (text.size() < 126) {
        f.push_back(static_cast<char>(0x80 | text.size()));
    } else {
        f.push_back(static_cast<char>(0x80 | 126));
        f.push_back(static_cast<char>((text.size() >> 8) & 0xFF));
        f.push_back(static_cast<char>(text.size() & 0xFF));
    }
    const unsigned char mask[4] = { 0x12, 0x34, 0x56, 0x78 };
    for (unsigned char m : mask) f.push_back(static_cast<char>(m));
    for (size_t i = 0; i < text.size(); ++i) f.push_back(static_cast<char>(text[i] ^ mask[i % 4]));
    ::send(s, f.data(), static_cast<int>(f.size()), 0);
}

static void testEngineQuit() {
    std::printf("[11] Motor leállítása az ablak bezárásakor (engine_quit): a saját gép igen, a távirányító nem\n");
    std::atomic<int> quits{ 0 };
    const std::string token = bridge_detail::randomHex(32);
    const std::string remoteDoc = "{\"enabled\":true,\"port\":47870,\"devices\":[{\"id\":\"0a0b0c0d\",\"name\":\"Teszt\",\"hash\":\"" +
                                  bridge_detail::sha1Hex(token) + "\",\"created\":0,\"last\":0}]}";
    BridgeCallbacks cb;
    cb.pullAudio = [](float*, size_t) -> size_t { return 0; };
    cb.sampleRate = [] { return 48000.0; };
    cb.getParams = [] { return transparentParams(); };
    cb.setParams = [](const ParamData&) {};
    cb.loadRemote = [&] { return remoteDoc; };
    cb.saveRemote = [](const std::string&) { return true; };
    cb.quitEngine = [&] { ++quits; };
    WebSocketBridge bridge(47869);
    const bool started = bridge.start(cb);
    CHECK(started, "a híd elindul (127.0.0.1:47869)");
    if (!started) return;
    auto waitQuit = [&](int want, int ms) {
        for (int i = 0; i < ms / 20 && quits.load() < want; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return quits.load();
    };

    // Jóváhagyásos párosítás, amíg a gépen nincs nyitva a felület: 503 és a pontos ok (no-window); idegen alkalmazás: client
    int remotePort = 0;
    std::string noWin;
    for (int port = 47870; port < 47875 && noWin.empty(); ++port) {
        noWin = wsHandshakeLive(port, "/?pair=request&client=aqulify_remote&name=Teszt");
        if (!noWin.empty()) remotePort = port;
    }
    CHECK(noWin.rfind("HTTP/1.1 503", 0) == 0 && noWin.find("\r\nX-Aqulify-Reason: no-window\r\n") != std::string::npos,
          "párosítási kérés nyitott ablak nélkül: 503, ok: no-window");
    const std::string foreign = remotePort ? wsHandshakeLive(remotePort, "/?pair=request&client=mas_app&name=X") : "";
    CHECK(foreign.rfind("HTTP/1.1 403", 0) == 0 && foreign.find("\r\nX-Aqulify-Reason: client\r\n") != std::string::npos,
          "idegen alkalmazás párosítási kérése: 403, ok: client");

    // Távirányító (párosított eszköz, hálózati port): a kérést a híd figyelmen kívül hagyja
    net::socket_t r = net::kInvalidSocket;
    for (int port = 47870; port < 47875 && r == net::kInvalidSocket; ++port)
        r = wsConnectLive(port, "/?token=" + token + "&client=aqulify_remote&name=Teszt");
    CHECK(r != net::kInvalidSocket, "párosított távoli eszköz csatlakozik (token)");
    if (r != net::kInvalidSocket) {
        wsSendText(r, "{\"type\":\"engine_quit\"}");
        CHECK(waitQuit(1, 500) == 0, "távirányítóról érkező engine_quit: a motor NEM áll le");
        net::closeSocket(r);
    }

    // A saját gépen futó felület (127.0.0.1): a motor leállítását kéri
    net::socket_t l = wsConnectLive(47869, "/");
    CHECK(l != net::kInvalidSocket, "helyi felület csatlakozik (127.0.0.1)");
    if (l != net::kInvalidSocket) {
        wsSendText(l, "{\"type\":\"engine_quit\"}");
        CHECK(waitQuit(1, 2000) == 1, "helyi engine_quit: a motor leállítása pontosan egyszer hívódik");
        net::closeSocket(l);
    }
    bridge.stop();
}

/** A következő szöveges keret a szervertől (maszkolatlan), legfeljebb ms ideig várva; üres, ha nem jött. */
static std::string wsReadText(net::socket_t s, int ms) {
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(ms);
#else
    timeval tv{ ms / 1000, (ms % 1000) * 1000 };
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    std::string buf;
    auto need = [&](size_t n) {
        char tmp[4096];
        while (buf.size() < n) {
            const int r = static_cast<int>(::recv(s, tmp, sizeof(tmp), 0));
            if (r <= 0) return false;
            buf.append(tmp, static_cast<size_t>(r));
        }
        return true;
    };
    for (int guard = 0; guard < 50; ++guard) {
        if (!need(2)) return "";
        const int op = static_cast<unsigned char>(buf[0]) & 0x0F;
        size_t len = static_cast<unsigned char>(buf[1]) & 0x7F, off = 2;
        if (len == 126) { if (!need(4)) return ""; len = (static_cast<unsigned char>(buf[2]) << 8) | static_cast<unsigned char>(buf[3]); off = 4; }
        else if (len == 127) { if (!need(10)) return ""; len = 0; for (int i = 2; i < 10; ++i) len = (len << 8) | static_cast<unsigned char>(buf[i]); off = 10; }
        if (!need(off + len)) return "";
        std::string payload = buf.substr(off, len);
        buf.erase(0, off + len);
        if (op == 1) return payload;
    }
    return "";
}

/** A szöveges keretek közül az első, amelyik tartalmazza a mintát (ms-en belül). */
static std::string wsWaitFor(net::socket_t s, const std::string& needle, int ms) {
#ifdef _WIN32
    DWORD tv = 100;
#else
    timeval tv{ 0, 100000 };
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    std::string buf;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        char tmp[4096];
        const int r = static_cast<int>(::recv(s, tmp, sizeof(tmp), 0));
        if (r > 0) buf.append(tmp, static_cast<size_t>(r));
        while (buf.size() >= 2) {
            const int op = static_cast<unsigned char>(buf[0]) & 0x0F;
            size_t len = static_cast<unsigned char>(buf[1]) & 0x7F, off = 2;
            if (len == 126) {
                if (buf.size() < 4) break;
                len = (static_cast<unsigned char>(buf[2]) << 8) | static_cast<unsigned char>(buf[3]);
                off = 4;
            } else if (len == 127) {
                if (buf.size() < 10) break;
                len = 0;
                for (int i = 2; i < 10; ++i) len = (len << 8) | static_cast<unsigned char>(buf[i]);
                off = 10;
            }
            if (buf.size() < off + len) break;
            std::string payload = buf.substr(off, len);
            buf.erase(0, off + len);
            if (op == 1 && payload.find(needle) != std::string::npos) return payload;
        }
        if (r == 0) break;
    }
    return "";
}

[[maybe_unused]] static std::string orig_wsWaitFor(net::socket_t s, const std::string& needle, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        const std::string t = wsReadText(s, 300);
        if (t.find(needle) != std::string::npos) return t;
    }
    return "";
}

/** A beérkező nyers bájtok gyűjtése ms ideig (a keretek fejlécével együtt; mintakereséshez). */
static std::string recvFor(net::socket_t s, int ms) {
    std::string out;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
#ifdef _WIN32
        DWORD tv = 100;
#else
        timeval tv{ 0, 100000 };
#endif
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        char tmp[4096];
        const int r = static_cast<int>(::recv(s, tmp, sizeof(tmp), 0));
        if (r > 0) out.append(tmp, static_cast<size_t>(r));
        else if (r == 0) break;
    }
    return out;
}

/** "kulcs":"érték" szövegmező az első előfordulásnál (nyers bájtokban is működik). */
static std::string rawField(const std::string& raw, const std::string& key) {
    const std::string pat = "\"" + key + "\":\"";
    const size_t p = raw.find(pat);
    if (p == std::string::npos) return "";
    const size_t e = raw.find('"', p + pat.size());
    return e == std::string::npos ? "" : raw.substr(p + pat.size(), e - p - pat.size());
}

static std::string jsonField(const std::string& doc, const std::string& key) {
    json::Value v;
    if (!json::parse(doc, v) || !v.get(key.c_str())) return "";
    return v.get(key.c_str())->stringOr("");
}

static void testRepairReplacesDevice() {
    std::printf("[11b] Újrapárosítás: ugyanaz a telefon nem gyűlik a gép listájában; a telefon törölheti a saját párosítását\n");
    std::mutex mu;
    std::string saved;
    const std::string remoteDoc =
        "{\"enabled\":true,\"port\":47880,\"devices\":["
        "{\"id\":\"0a0b0c0d\",\"name\":\"Teszt-telefon\",\"hash\":\"" + std::string(40, 'a') + "\",\"created\":0,\"last\":0},"
        "{\"id\":\"1a1b1c1d\",\"name\":\"Masik\",\"hash\":\"" + std::string(40, 'b') + "\",\"created\":0,\"last\":0}]}";
    BridgeCallbacks cb;
    cb.pullAudio = [](float*, size_t) -> size_t { return 0; };
    cb.sampleRate = [] { return 48000.0; };
    cb.getParams = [] { return transparentParams(); };
    cb.setParams = [](const ParamData&) {};
    cb.loadRemote = [&] { return remoteDoc; };
    cb.saveRemote = [&](const std::string& s) { std::lock_guard<std::mutex> g(mu); saved = s; return true; };
    WebSocketBridge bridge(47879);
    const bool started = bridge.start(cb);
    CHECK(started, "a híd elindul (127.0.0.1:47879)");
    if (!started) return;
    auto savedDoc = [&] { std::lock_guard<std::mutex> g(mu); return saved; };
    auto count = [](const std::string& doc, const std::string& needle) {
        size_t n = 0;
        for (size_t p = doc.find(needle); p != std::string::npos; p = doc.find(needle, p + 1)) ++n;
        return n;
    };

    net::socket_t local = wsConnectLive(47879, "/");
    CHECK(local != net::kInvalidSocket, "helyi felület csatlakozik");
    if (local == net::kInvalidSocket) { bridge.stop(); return; }
    const std::string app = "0123456789abcdef0123456789abcdef";
    int remotePort = 0;
    std::string token;
    std::vector<std::string> machines;
    for (int round = 0; round < 2; ++round) {
        wsSendText(local, "{\"type\":\"remote_new_code\"}");
        std::string code;
        for (int i = 0; i < 10 && code.empty(); ++i) code = jsonField(wsWaitFor(local, "\"code\":\"", 2000), "code");
        net::socket_t r = net::kInvalidSocket;
        std::string first;
        for (int port = remotePort ? remotePort : 47880; port < 47885 && r == net::kInvalidSocket; ++port) {
            r = wsConnectLive(port, "/?pair=" + code + "&client=aqulify_remote&name=Teszt-telefon&app=" + app, &first);
            if (r != net::kInvalidSocket) remotePort = port;
        }
        CHECK(r != net::kInvalidSocket, "%d. párosítás kóddal, ugyanazzal az alkalmazás-azonosítóval", round + 1);
        if (r == net::kInvalidSocket) break;
        // A "paired" üzenet (benne a token és a gép azonosítója) a kézfogással együtt vagy közvetlenül utána érkezik
        const std::string raw = first + recvFor(r, 400);
        token = rawField(raw, "token");
        machines.push_back(rawField(raw, "machine"));
        net::closeSocket(r);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const std::string doc = savedDoc();
    CHECK(count(doc, "\"name\":\"Teszt-telefon\"") == 1 && count(doc, "\"app\":\"" + app + "\"") == 1,
          "két párosítás után egy bejegyzés maradt (a régi, azonosító nélküli azonos nevű is lecserélődött)");
    CHECK(doc.find("\"name\":\"Masik\"") != std::string::npos, "a másik telefon párosítása megmaradt");
    CHECK(machines.size() == 2 && machines[0].size() == 16 && machines[0] == machines[1] &&
          jsonField(doc, "machine") == machines[0],
          "a gép azonosítója (machine) a párosításkor megjön, állandó és el van mentve");

    // Már párosított telefon csatlakozásakor is megkapja a gép azonosítóját
    {
        std::string first;
        net::socket_t t = (remotePort && token.size() == 64) ? wsConnectLive(remotePort, "/?token=" + token, &first)
                                                            : net::kInvalidSocket;
        const std::string got = t == net::kInvalidSocket ? "" : first + recvFor(t, 400);
        const std::string m = rawField(got, "machine");
        CHECK(!machines.empty() && !m.empty() && m == machines[0],
              "tokennel csatlakozáskor a gép azonosítója is megjön");
        CHECK(!rawField(got, "computer").empty() && rawField(got, "computer") == MdnsAnnouncer::computerName(),
              "tokennel csatlakozáskor a gép neve (computer) is megjön");
        if (t != net::kInvalidSocket) net::closeSocket(t);
    }

    // A telefon törli a saját párosítását (a telefonon a gép törlésekor)
    net::socket_t me = (remotePort && token.size() == 64) ? wsConnectLive(remotePort, "/?token=" + token) : net::kInvalidSocket;
    CHECK(me != net::kInvalidSocket, "a párosított telefon csatlakozik a kapott tokennel");
    if (me != net::kInvalidSocket) {
        wsSendText(me, "{\"type\":\"unpair_self\"}");
        std::string after;
        for (int i = 0; i < 50; ++i) {
            after = savedDoc();
            if (after.find("Teszt-telefon") == std::string::npos) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(after.find("Teszt-telefon") == std::string::npos && after.find("\"name\":\"Masik\"") != std::string::npos,
              "unpair_self: a telefon párosítása törlődött, a másiké megmaradt");
        net::closeSocket(me);
    }
    net::closeSocket(local);
    bridge.stop();
}

static void testUpdate() {
    std::printf("[12] Frissítés: SHA-256, verziók, letöltési címek; az update_install csak a saját gépről\n");
    // SHA-256: a FIPS 180-4 tesztvektorai
    CHECK(Sha256::hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256: üres bemenet");
    CHECK(Sha256::hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256: \"abc\"");
    const std::string m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(Sha256::hex(m448.data(), m448.size()) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "SHA-256: 448 bites üzenet (a kitöltés miatt két blokk)");
    {
        Sha256 h;
        const std::string chunk(1000, 'a');
        for (int i = 0; i < 1000; ++i) h.update(chunk.data(), chunk.size());
        CHECK(h.hexDigest() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
              "SHA-256: egymillió 'a', 1000 bájtos darabokban");
    }
    {
        std::string data(5000, '\0');
        for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>(i * 31 + 7);
        Sha256 h;
        size_t off = 0, step = 1;
        while (off < data.size()) {
            const size_t k = std::min(step, data.size() - off);
            h.update(data.data() + off, k);
            off += k;
            step = step * 3 % 97 + 1;
        }
        CHECK(h.hexDigest() == Sha256::hex(data.data(), data.size()), "SHA-256: változó méretű darabokban ugyanaz, mint egyben");
    }

    // Verziók és címek
    CHECK(update::validVersion("2.1.1") && update::validVersion("10.0.12") && !update::validVersion("2.1") &&
          !update::validVersion("2.1.1.0") && !update::validVersion("v2.1.1") && !update::validVersion("2..1") &&
          !update::validVersion("2.1.1/../x") && !update::validVersion("12345.0.0") && !update::validVersion(""),
          "érvényes verzió csak X.Y.Z számjegyekkel (előtag, útvonal, hiányzó vagy túl hosszú rész nem)");
    CHECK(update::compareVersions("2.1.10", "2.1.9") > 0 && update::compareVersions("2.2.0", "2.10.0") < 0 &&
          update::compareVersions("2.1.1", "2.1.1") == 0 && update::compareVersions("3.0.0", "2.99.99") > 0,
          "verzió-összevetés számként (2.1.10 > 2.1.9, 2.2.0 < 2.10.0)");
    CHECK(update::installerUrl("2.1.1") ==
              "https://github.com/Szorgos/ee-phase-waterfall-analyzer/releases/download/v2.1.1/Aqulify-Setup-2.1.1.exe" &&
          update::checksumUrl("2.1.1") == update::installerUrl("2.1.1") + ".sha256",
          "a letöltési cím a saját kiadásé (rögzített tároló)");
    CHECK(update::dmgUrl("2.1.1") ==
              "https://github.com/Szorgos/ee-phase-waterfall-analyzer/releases/download/v2.1.1/Aqulify-2.1.1-macOS.dmg" &&
          update::dmgChecksumUrl("2.1.1") == update::dmgUrl("2.1.1") + ".sha256",
          "a macOS-lemezkép címe a saját kiadásé (a kiadás fájlneve szerint)");

    // Linux: a telepítés módjának felismerése (hamis fájlrendszerrel)
    auto fakeFs = [](std::vector<std::string> files, std::vector<std::pair<std::string, std::string>> dirEntries) {
        update::FsProbe fs;
        fs.exists = [files](const std::string& p) { return std::find(files.begin(), files.end(), p) != files.end(); };
        fs.list = [dirEntries](const std::string& d) {
            std::vector<std::string> out;
            for (const auto& e : dirEntries)
                if (e.first == d) out.push_back(e.second);
            return out;
        };
        return fs;
    };
    using update::LinuxInstall;
    const std::string usrBin = "/usr/bin/aqulify-engine";
    CHECK(update::detectLinuxInstall("/home/u/.local/bin/aqulify-engine", fakeFs({}, {})) == LinuxInstall::Script &&
          update::detectLinuxInstall("/usr/local/bin/aqulify-engine", fakeFs({ "/var/lib/rpm" }, {})) == LinuxInstall::Script,
          "Linux: a saját mappából (~/.local) és a /usr/local alól futó motor az install.sh-é");
    CHECK(update::detectLinuxInstall(usrBin, fakeFs({ "/var/lib/dpkg/info/aqulify.list" }, {})) == LinuxInstall::Deb &&
          update::detectLinuxInstall(usrBin, fakeFs({ "/var/lib/dpkg/info/aqulify:amd64.list" }, {})) == LinuxInstall::Deb,
          "Linux: Debian / Ubuntu csomag (a dpkg nyilvántartása szerint)");
    CHECK(update::detectLinuxInstall(usrBin, fakeFs({}, { { "/var/lib/pacman/local", "glibc-2.40-1" },
                                                          { "/var/lib/pacman/local", "aqulify-2.1.1-1" } })) == LinuxInstall::Arch,
          "Linux: Arch csomag (a pacman nyilvántartása szerint)");
    CHECK(update::detectLinuxInstall(usrBin, fakeFs({ "/usr/lib/sysimage/rpm" },
                                                    { { "/etc/yum.repos.d", "fedora.repo" },
                                                      { "/etc/yum.repos.d", "_copr:copr.fedorainfracloud.org:nlali:aqulify.repo" } })) ==
                  LinuxInstall::Copr &&
              update::detectLinuxInstall(usrBin, fakeFs({ "/var/lib/rpm" }, { { "/etc/yum.repos.d", "fedora.repo" } })) ==
                  LinuxInstall::Rpm,
          "Linux: Fedora – COPR-tárolóból, illetve kézzel telepített rpm");
    CHECK(update::detectLinuxInstall(usrBin, fakeFs({}, {})) == LinuxInstall::Unknown &&
          update::detectLinuxInstall("", fakeFs({}, {})) == LinuxInstall::Unknown &&
          std::string(update::linuxInstallName(LinuxInstall::Copr)) == "copr" &&
          std::string(update::linuxInstallName(LinuxInstall::Script)) == "script",
          "Linux: ismeretlen csomagkezelő vagy útvonal → unknown; a nevek a felületnek");
    const std::string empty = Sha256::hex("", 0);
    const std::string upper = "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855";
    CHECK(update::parseChecksum(upper + "  Aqulify-Setup-2.1.1.exe\n") == empty &&
          update::parseChecksum("\xEF\xBB\xBF" + upper) == empty && update::parseChecksum("abc  x.exe").empty() &&
          update::parseChecksum("<html>Not Found</html>").empty() && update::parseChecksum("").empty(),
          "a .sha256 fájl: az első szó kisbetűsítve (BOM nélkül); hibás tartalomnál üres");

    // A híd: az update_install a helyi felületről eljut a motorhoz, a távirányítóról nem
    std::mutex gm;
    std::string got;
    std::atomic<int> calls{ 0 };
    const std::string token = bridge_detail::randomHex(32);
    const std::string remoteDoc = "{\"enabled\":true,\"port\":47877,\"devices\":[{\"id\":\"0a0b0c0e\",\"name\":\"Teszt\",\"hash\":\"" +
                                  bridge_detail::sha1Hex(token) + "\",\"created\":0,\"last\":0}]}";
    BridgeCallbacks cb;
    cb.pullAudio = [](float*, size_t) -> size_t { return 0; };
    cb.sampleRate = [] { return 48000.0; };
    cb.getParams = [] { return transparentParams(); };
    cb.setParams = [](const ParamData&) {};
    cb.loadRemote = [&] { return remoteDoc; };
    cb.saveRemote = [](const std::string&) { return true; };
    cb.updateInstall = [&](const std::string& v, std::string&) {
        std::lock_guard<std::mutex> lk(gm);
        got = v;
        ++calls;
        return true;
    };
    WebSocketBridge bridge(47876);
    const bool started = bridge.start(cb);
    CHECK(started, "a híd elindul (127.0.0.1:47876)");
    if (!started) return;
    auto waitCalls = [&](int want, int ms) {
        for (int i = 0; i < ms / 20 && calls.load() < want; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return calls.load();
    };
    net::socket_t r = net::kInvalidSocket;
    for (int port = 47877; port < 47882 && r == net::kInvalidSocket; ++port)
        r = wsConnectLive(port, "/?token=" + token + "&client=aqulify_remote&name=Teszt");
    CHECK(r != net::kInvalidSocket, "párosított távoli eszköz csatlakozik (token)");
    if (r != net::kInvalidSocket) {
        wsSendText(r, "{\"type\":\"update_install\",\"version\":\"2.2.0\"}");
        CHECK(waitCalls(1, 500) == 0, "távirányítóról érkező update_install: a motor NEM tölt le és NEM telepít");
        net::closeSocket(r);
    }
    net::socket_t l = wsConnectLive(47876, "/");
    CHECK(l != net::kInvalidSocket, "helyi felület csatlakozik (127.0.0.1)");
    if (l != net::kInvalidSocket) {
        wsSendText(l, "{\"type\":\"update_install\",\"version\":\"2.2.0\"}");
        const int n = waitCalls(1, 2000);
        std::string v;
        {
            std::lock_guard<std::mutex> lk(gm);
            v = got;
        }
        CHECK(n == 1 && v == "2.2.0", "helyi update_install: a motor pontosan egyszer kapja meg, a kért verzióval");
        net::closeSocket(l);
    }
    bridge.stop();
}


static void testOpenUrl() {
    std::printf("[13] Média-hivatkozások (open_url): címellenőrzés, híd, gyakoriságkorlát, értesítés\n");

    // 1. isAllowedMediaUrl egységtesztek
    std::string h;
    CHECK(isAllowedMediaUrl("https://www.youtube.com/watch?v=abc", &h) && h == "www.youtube.com", "URL: www.youtube.com");
    CHECK(isAllowedMediaUrl("https://youtu.be/abc?si=x", &h) && h == "youtu.be", "URL: youtu.be");
    CHECK(isAllowedMediaUrl("https://music.youtube.com/watch?v=abc", &h) && h == "music.youtube.com", "URL: music.youtube.com");
    CHECK(isAllowedMediaUrl("https://tidal.com/browse/track/123?u", &h) && h == "tidal.com", "URL: tidal.com");
    CHECK(isAllowedMediaUrl("https://open.spotify.com/track/abc?si=x", &h) && h == "open.spotify.com", "URL: open.spotify.com");
    CHECK(isAllowedMediaUrl("HTTPS://YouTube.com/x", &h) && h == "youtube.com", "URL: HTTPS://YouTube.com/x (kis-nagybetű)");

    // Elutasítások
    CHECK(!isAllowedMediaUrl("http://youtube.com/x"), "URL elutasítás: http");
    CHECK(!isAllowedMediaUrl("file:///C:/x"), "URL elutasítás: file");
    CHECK(!isAllowedMediaUrl("javascript:alert(1)"), "URL elutasítás: javascript");
    CHECK(!isAllowedMediaUrl("tidal://track/1"), "URL elutasítás: tidal://");
    CHECK(!isAllowedMediaUrl("ms-msdt:/id x"), "URL elutasítás: ms-msdt");
    CHECK(!isAllowedMediaUrl("https://youtube.com.evil.com/"), "URL elutasítás: aldomain támadás");
    CHECK(!isAllowedMediaUrl("https://evilyoutube.com/"), "URL elutasítás: előtag");
    CHECK(!isAllowedMediaUrl("https://user@youtube.com/"), "URL elutasítás: @ a hosztban");
    CHECK(!isAllowedMediaUrl("https://youtube.com:8080/"), "URL elutasítás: port a hosztban");
    CHECK(!isAllowedMediaUrl("https://192.168.1.1/"), "URL elutasítás: IP-cím");
    CHECK(!isAllowedMediaUrl("https://[::1]/"), "URL elutasítás: IPv6");
    CHECK(!isAllowedMediaUrl("https://xn--ytube-xxx.com/"), "URL elutasítás: punycode");
    CHECK(!isAllowedMediaUrl("https://youtube.com/foo bar"), "URL elutasítás: szóköz");
    CHECK(!isAllowedMediaUrl("https://youtube.com/foo\\bar"), "URL elutasítás: visszaperjel");
    CHECK(!isAllowedMediaUrl("https://youtube.com/foo%00bar"), "URL elutasítás: %%00");
    // Kódolás nélkül nem megengedett URL-karakterek (Windowson a böngésző parancssorába kerülne)
    for (const char* bad : { "https://youtube.com/watch?v=\"x", "https://youtube.com/<script>", "https://youtube.com/a>b",
                             "https://youtube.com/a^b", "https://youtube.com/a|b", "https://youtube.com/a`b",
                             "https://youtube.com/a{b", "https://youtube.com/a}b" })
        CHECK(!isAllowedMediaUrl(bad), "URL elutasítás: nem URL-karakter (%s)", bad + 20);
    // Hamis hoszt a töredékben / lekérdezésben / útvonalban, kódolt pont, hexadecimális IP, kötőjeles előtag
    CHECK(!isAllowedMediaUrl("https://evil.com#.youtube.com") && !isAllowedMediaUrl("https://evil.com?.youtube.com") &&
          !isAllowedMediaUrl("https://evil.com/.youtube.com") && !isAllowedMediaUrl("https://youtube.com%2eevil.com/") &&
          !isAllowedMediaUrl("https://0x7f000001/") && !isAllowedMediaUrl("https://-youtube.com/") &&
          !isAllowedMediaUrl("https:///youtube.com") && !isAllowedMediaUrl("https:youtube.com"),
          "URL elutasítás: hamis hoszt a #/?/ után, kódolt pont, hexa IP, kötőjeles előtag, hiányzó hoszt");
    CHECK(isAllowedMediaUrl("https://YOUTUBE.COM./x", &h) && h == "youtube.com" &&
          isAllowedMediaUrl("https://youtube.com#@evil.com", &h) && h == "youtube.com",
          "URL: záró pont és töredék (a hoszt youtube.com)");
    CHECK(!isAllowedMediaUrl(std::string("https://youtube.com/") + std::string(2035, 'a')), "URL elutasítás: 2049 karakter");

    // Szolgáltatás-hivatkozás: asztali alkalmazás, ha van kezelő a protokolljához, különben a webes lejátszó
    const auto none = [](const std::string&) { return false; };
    const auto all = [](const std::string&) { return true; };
    CHECK(mediaOpenTarget("https://tidal.com/browse/track/123?u", none) == "https://listen.tidal.com/track/123" &&
          mediaOpenTarget("https://tidal.com/browse/track/123?u", all) == "tidal://track/123",
          "Tidal szám: web -> listen.tidal.com/track/123, alkalmazás -> tidal://track/123");
    CHECK(mediaOpenTarget("https://tidal.com/track/55/u", all) == "tidal://track/55" &&
          mediaOpenTarget("https://listen.tidal.com/album/456", none) == "https://listen.tidal.com/album/456" &&
          mediaOpenTarget("https://tidal.com/browse/playlist/6f1c2a3b-0000-4d5e-9f00-123456789abc", all) ==
              "tidal://playlist/6f1c2a3b-0000-4d5e-9f00-123456789abc" &&
          mediaOpenTarget("https://www.tidal.com/browse/artist/7#x", none) == "https://listen.tidal.com/artist/7",
          "Tidal: /track/<id>/u, listen.tidal.com album, playlist UUID, www + töredék");
    CHECK(mediaOpenTarget("https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC?si=abc", all) ==
              "spotify:track:4uLU6hMCjMI75M1A2tKUQC" &&
          mediaOpenTarget("https://open.spotify.com/intl-de/album/1DFixLWuPkv3KT3TnV35m3", all) ==
              "spotify:album:1DFixLWuPkv3KT3TnV35m3" &&
          mediaOpenTarget("https://open.spotify.com/intl-de/album/1DFixLWuPkv3KT3TnV35m3?si=x", none) ==
              "https://open.spotify.com/album/1DFixLWuPkv3KT3TnV35m3",
          "Spotify: spotify:<típus>:<id> (intl- előtag és si nélkül), különben open.spotify.com");
    CHECK(mediaOpenTarget("https://open.qobuz.com/track/416397342", all) == "qobuzapp://track/416397342" &&
          mediaOpenTarget("https://play.qobuz.com/album/0060254757893", none) == "https://play.qobuz.com/album/0060254757893" &&
          mediaOpenTarget("https://www.qobuz.com/gb-en/album/some-name-artist/0060254757893", all) ==
              "qobuzapp://album/0060254757893",
          "Qobuz: qobuzapp://<típus>/<id>, a bolt albumoldala is; különben play.qobuz.com");
    CHECK(mediaOpenTarget("https://music.apple.com/hu/album/name/1440857781?i=1440857795", all) ==
              "music://music.apple.com/hu/album/name/1440857781?i=1440857795" &&
          mediaOpenTarget("https://music.apple.com/hu/album/name/1440857781", none) ==
              "https://music.apple.com/hu/album/name/1440857781",
          "Apple Music: music://, különben változatlan");
    // Csak a szolgáltatás saját protokolljára kérdez (a Tidal-kezelő nem nyitja meg a Spotify-címet)
    const auto onlyTidal = [](const std::string& s) { return s == "tidal"; };
    CHECK(mediaOpenTarget("https://open.spotify.com/track/abc", onlyTidal) == "https://open.spotify.com/track/abc" &&
          mediaOpenTarget("https://tidal.com/browse/track/9", onlyTidal) == "tidal://track/9",
          "a kezelő-kérdés a szolgáltatás saját protokolljára szól");
    CHECK(mediaOpenTarget("https://tidal.com/", all) == "https://tidal.com/" &&
          mediaOpenTarget("https://tidal.com/browse/podcast/1", all) == "https://tidal.com/browse/podcast/1" &&
          mediaOpenTarget("https://tidal.com/browse/track/1_2", all) == "https://tidal.com/browse/track/1_2" &&
          mediaOpenTarget("https://tidal.com/browse/track/", all) == "https://tidal.com/browse/track/" &&
          mediaOpenTarget("https://open.spotify.com/track/a-b", all) == "https://open.spotify.com/track/a-b" &&
          mediaOpenTarget("https://spotify.link/xyz", all) == "https://spotify.link/xyz" &&
          mediaOpenTarget("https://www.qobuz.com/gb-en/label/x", all) == "https://www.qobuz.com/gb-en/label/x" &&
          mediaOpenTarget("https://www.youtube.com/watch?v=abc", all) == "https://www.youtube.com/watch?v=abc" &&
          mediaOpenTarget("https://www.deezer.com/hu/track/123", all) == "https://www.deezer.com/hu/track/123" &&
          mediaOpenTarget("https://evil.com/browse/track/1", all) == "https://evil.com/browse/track/1",
          "nem felismert alak, ismeretlen típus, rossz azonosító, rövidlink, alkalmazás-hivatkozás nélküli szolgáltatás "
          "és tiltott cím: változatlan");
    CHECK(!isAllowedMediaUrl(""), "URL elutasítás: üres szöveg");

    // 2. Élő hídteszt: openUrl nélküli híd -> nincs open_url a caps-ban, és unsupported hiba
    {
        BridgeCallbacks cb;
        cb.pullAudio = [](float*, size_t) -> size_t { return 0; };
        cb.sampleRate = [] { return 48000.0; };
        cb.getParams = [] { return transparentParams(); };
        cb.setParams = [](const ParamData&) {};
        WebSocketBridge bridge(47888);
        const bool started = bridge.start(cb);
        CHECK(started, "a híd elindul openUrl nélkül (127.0.0.1:47888)");
        if (started) {
            std::string first;
            net::socket_t l = wsConnectLive(47888, "/", &first);
            CHECK(l != net::kInvalidSocket, "helyi kliens csatlakozik az openUrl nélküli hídhoz");
            if (l != net::kInvalidSocket) {
                const std::string state = first + recvFor(l, 400);
                CHECK(state.find("\"open_url\"") == std::string::npos, "openUrl nélkül a caps NEM tartalmazza az \"open_url\"-t");
                wsSendText(l, "{\"type\":\"open_url\",\"url\":\"https://www.youtube.com/watch?v=123\"}");
                const std::string rep = recvFor(l, 400);
                CHECK(rawField(rep, "error") == "unsupported", "openUrl nélkül open_url -> error: unsupported");
                net::closeSocket(l);
            }
            bridge.stop();
        }
    }

    // 3. Élő hídteszt: openUrl-lel felvértezett híd
    {
        std::mutex gm;
        std::string gotUrl;
        std::atomic<int> calls{ 0 };
        const std::string token = bridge_detail::randomHex(32);
        const std::string remoteDoc = "{\"enabled\":true,\"port\":47889,\"devices\":[{\"id\":\"0a0b0c0d\",\"name\":\"Mobil\",\"hash\":\"" +
                                      bridge_detail::sha1Hex(token) + "\",\"created\":0,\"last\":0}]}";
        BridgeCallbacks cb;
        cb.pullAudio = [](float*, size_t) -> size_t { return 0; };
        cb.sampleRate = [] { return 48000.0; };
        cb.getParams = [] { return transparentParams(); };
        cb.setParams = [](const ParamData&) {};
        cb.loadRemote = [&] { return remoteDoc; };
        cb.saveRemote = [](const std::string&) { return true; };
        cb.openUrl = [&](const std::string& u) {
            std::lock_guard<std::mutex> lk(gm);
            gotUrl = u;
            ++calls;
            return true;
        };
        std::atomic<bool> tidalApp{ false };
        cb.hasUrlHandler = [&](const std::string& scheme) { return scheme == "tidal" && tidalApp.load(); };

        WebSocketBridge bridge(47888);
        const bool started = bridge.start(cb);
        CHECK(started, "a híd elindul openUrl-lel (127.0.0.1:47888)");
        if (started) {
            // Helyi felület csatlakozik
            std::string localFirst;
            net::socket_t local = wsConnectLive(47888, "/", &localFirst);
            CHECK(local != net::kInvalidSocket, "helyi felület csatlakozik (127.0.0.1:47888)");
            if (local != net::kInvalidSocket) {
                const std::string state = localFirst + recvFor(local, 400);
                CHECK(state.find("\"open_url\"") != std::string::npos, "a caps tartalmazza az \"open_url\"-t");
            }

            // Párosított távoli kliens csatlakozik
            net::socket_t remote = net::kInvalidSocket;
            for (int port = 47889; port < 47894 && remote == net::kInvalidSocket; ++port) {
                remote = wsConnectLive(port, "/?token=" + token + "&client=aqulify_remote&name=Mobil");
            }
            CHECK(remote != net::kInvalidSocket, "párosított telefon csatlakozik (token)");

            if (remote != net::kInvalidSocket) {
                // 3a: Érvényes URL -> ok: true, a cb.openUrl lefut pontosan a megadott URL-lel
                wsSendText(remote, "{\"type\":\"open_url\",\"url\":\"https://www.youtube.com/watch?v=good\"}");
                const std::string rep1 = recvFor(remote, 400);
                CHECK(rep1.find("\"type\":\"open_url_result\",\"ok\":true") != std::string::npos, "érvényes URL -> ok: true");
                std::string u1;
                {
                    std::lock_guard<std::mutex> lk(gm);
                    u1 = gotUrl;
                }
                CHECK(calls.load() == 1 && u1 == "https://www.youtube.com/watch?v=good", "cb.openUrl pontosan egyszer futott az URL-lel");

                // A helyi felület megkapja a remote_opened_url üzenetet a hoszttal, a teljes URL nélkül
                const std::string notify = recvFor(local, 400);
                CHECK(notify.find("\"type\":\"remote_opened_url\"") != std::string::npos &&
                      rawField(notify, "host") == "www.youtube.com" &&
                      notify.find("watch?v=good") == std::string::npos,
                      "a helyi felület megkapta a remote_opened_url üzenetet a hoszttal, a teljes URL nélkül");

                // 3b: 2 másodpercen belül újra küldve -> busy
                wsSendText(remote, "{\"type\":\"open_url\",\"url\":\"https://www.youtube.com/watch?v=good2\"}");
                const std::string rep2 = recvFor(remote, 400);
                CHECK(rawField(rep2, "error") == "busy", "2 s-on belül ismétlés -> error: busy");
                CHECK(calls.load() == 1, "busy esetén a callback nem hívódott újra");

                // 3c: Várakozás után rossz cím -> nem hívódik, error: url
                std::this_thread::sleep_for(std::chrono::milliseconds(2100));
                wsSendText(remote, "{\"type\":\"open_url\",\"url\":\"https://evil.com/malicious\"}");
                const std::string rep3 = recvFor(remote, 400);
                CHECK(rawField(rep3, "error") == "url", "tiltott tartomány -> error: url");
                CHECK(calls.load() == 1, "rossz címnél a callback nem hívódott");

                // 3d: Tidal-szám alkalmazás nélkül -> a webes lejátszó; alkalmazással -> tidal://
                auto lastUrl = [&] { std::lock_guard<std::mutex> lk(gm); return gotUrl; };
                wsSendText(remote, "{\"type\":\"open_url\",\"url\":\"https://tidal.com/browse/track/123?u\"}");
                recvFor(remote, 400);
                CHECK(calls.load() == 2 && lastUrl() == "https://listen.tidal.com/track/123",
                      "Tidal alkalmazás nélkül: a webes lejátszó nyílik meg (listen.tidal.com/track/123)");
                std::this_thread::sleep_for(std::chrono::milliseconds(2100));
                tidalApp = true;
                wsSendText(remote, "{\"type\":\"open_url\",\"url\":\"https://tidal.com/browse/track/123?u\"}");
                recvFor(remote, 400);
                CHECK(calls.load() == 3 && lastUrl() == "tidal://track/123",
                      "Tidal alkalmazással: az alkalmazás nyílik meg (tidal://track/123)");

                net::closeSocket(remote);
            }
            // Név nélküli kliens (a helyi felület): az értesítésben üres eszköznév megy (a felület a saját nyelvén írja ki)
            if (local != net::kInvalidSocket) {
                recvFor(local, 200);   // a korábbi üzenetek elnyelése
                wsSendText(local, "{\"type\":\"open_url\",\"url\":\"https://youtu.be/abc\"}");
                const std::string got = recvFor(local, 400);
                CHECK(got.find("\"type\":\"open_url_result\",\"ok\":true") != std::string::npos &&
                      got.find("\"type\":\"remote_opened_url\",\"device\":\"\",\"host\":\"youtu.be\"") != std::string::npos,
                      "név nélküli kliensnél üres eszköznév (nem magyar szöveg) az értesítésben");
            }
            if (local != net::kInvalidSocket) net::closeSocket(local);
            bridge.stop();
        }
    }
}

/** Sima TCP-kapcsolat a 127.0.0.1-re (a soros tesztporthoz). */
static net::socket_t tcpConnectLive(int port) {
    net::socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == net::kInvalidSocket) return s;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { net::closeSocket(s); return net::kInvalidSocket; }
    return s;
}

static void rawSend(net::socket_t s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        const long long n = net::sendSome(s, data.data() + off, data.size() - off);   // bontott kapcsolatnál nincs SIGPIPE
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

/** A géptől érkező sorok ms ideig; closed: a gép közben bontotta a kapcsolatot (ekkor az olvasás leáll). */
static std::vector<std::string> lineReadFor(net::socket_t s, int ms, bool* closed = nullptr) {
    std::vector<std::string> lines;
    std::string buf;
    if (closed) *closed = false;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
#ifdef _WIN32
        DWORD tv = 100;
#else
        timeval tv{ 0, 100000 };
#endif
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        char tmp[4096];
        const int r = static_cast<int>(::recv(s, tmp, sizeof(tmp), 0));
        if (r > 0) {
            buf.append(tmp, static_cast<size_t>(r));
            for (size_t nl; (nl = buf.find('\n')) != std::string::npos;) {
                lines.push_back(buf.substr(0, nl));
                buf.erase(0, nl + 1);
            }
        } else if (r == 0) {
            if (closed) *closed = true;
            break;
        } else if (!net::wouldBlock()) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAETIMEDOUT) continue;
#endif
            if (closed) *closed = true;   // pl. a gép bontott (connection reset)
            break;
        }
    }
    return lines;
}

static bool hasType(const std::vector<std::string>& lines, const std::string& type) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return jsonField(l, "type") == type; });
}

/** A tmpfile teljes tartalma. */
static std::string readAllFile(std::FILE* f) {
    std::string s;
    std::rewind(f);
    char b[512];
    size_t n;
    while ((n = std::fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    return s;
}

static void testPairCli() {
    std::printf("[15] Párosítás kijelző nélkül (--pair, pair_cli.hpp)\n");
    using namespace paircli;
    // 1. Keretezés: a kliens kerete maszkolt, a szerverét a FrameReader darabokban is összerakja
    {
        const uint8_t mask[4] = { 1, 2, 3, 4 };
        const std::string f = encodeClientText("ab", mask);
        CHECK(f.size() == 8 && static_cast<unsigned char>(f[0]) == 0x81 && static_cast<unsigned char>(f[1]) == (0x80 | 2) &&
              f[6] == ('a' ^ 1) && f[7] == ('b' ^ 2), "rövid kliens keret: FIN+szöveg, maszkbit, maszkolt adat");
        const std::string big(70000, 'x');
        const std::string fb = encodeClientText(big, mask);
        CHECK(static_cast<unsigned char>(fb[1]) == (0x80 | 127) && fb.size() == 2 + 8 + 4 + big.size(), "64 bites hossz 65535 fölött");
        auto server = [](int op, const std::string& p) {
            std::string s;
            s.push_back(static_cast<char>(0x80 | op));
            if (p.size() < 126) s.push_back(static_cast<char>(p.size()));
            else { s.push_back(126); s.push_back(static_cast<char>(p.size() >> 8)); s.push_back(static_cast<char>(p.size() & 0xFF)); }
            return s + p;
        };
        const std::string payload(300, 'y');
        const std::string stream = server(0x9, "") + server(0x2, "bin") + server(0x1, payload) + server(0x1, "{}") + server(0x8, "");
        FrameReader r;
        std::string out;
        bool closed = false;
        std::vector<std::string> got;
        for (size_t i = 0; i < stream.size(); i += 7) {          // 7 bájtos darabokban érkezik
            r.push(stream.data() + i, std::min<size_t>(7, stream.size() - i));
            while (r.next(out, &closed)) got.push_back(out);
        }
        CHECK(got.size() == 2 && got[0] == payload && got[1] == "{}", "szerver keretei darabokban: a ping és a bináris kimarad");
        CHECK(closed, "a záró keretet jelzi");
    }
    // 2. A "remote" üzenet értelmezése és a kiírás
    {
        PairInfo p;
        CHECK(parseRemote("{\"type\":\"remote\",\"enabled\":true,\"listening\":true,\"port\":47820,\"code\":\"123456\","
                          "\"codeSeconds\":290,\"addresses\":[\"192.168.1.20\",\"10.0.0.5\"]}", p) &&
              p.enabled && p.listening && p.port == 47820 && p.code == "123456" && p.codeSeconds == 290 && p.addresses.size() == 2,
              "remote üzenet: kapcsolók, port, kód, címek");
        CHECK(!parseRemote("{\"type\":\"state\"}", p) && !parseRemote("nem json", p), "más üzenet / hibás JSON: nem remote");
        PairInfo v = p;
        v.addresses = { "172.18.48.1", "192.168.1.50" };
        const std::string vt = formatPairInfo(v, false);
        CHECK(vt.find("192.168.1.50:47820, 172.18.48.1:47820") != std::string::npos,
              "címsorrend: az otthoni hálózat elöl, a virtuális (172.16–31.) hátul");
        const std::string txt = formatPairInfo(p, true);
        CHECK(txt.find("Párosító kód: 123456") != std::string::npos && txt.find("192.168.1.20:47820") != std::string::npos &&
              txt.find("5 percig") != std::string::npos && txt.find("most bekapcsoltam") != std::string::npos,
              "kiírás: kód, cím:port, érvényesség percben, a bekapcsolás jelzése");
    }
    // 3. Élő próba a valódi híddal: a hálózati elérés ki van kapcsolva, a --pair bekapcsolja és kódot kér
    {
        BridgeCallbacks cb;
        cb.pullAudio = [](float*, size_t) -> size_t { return 0; };
        cb.sampleRate = [] { return 48000.0; };
        cb.getParams = [] { return transparentParams(); };
        cb.setParams = [](const ParamData&) {};
        std::string saved;
        cb.loadRemote = [] { return std::string("{\"enabled\":false,\"port\":47911,\"bluetooth\":false,\"devices\":[]}"); };
        cb.saveRemote = [&](const std::string& s) { saved = s; return true; };
        WebSocketBridge bridge(47910);
        const bool started = bridge.start(cb);
        CHECK(started, "a híd elindul (127.0.0.1:47910)");
        if (started) {
            std::FILE* out = std::tmpfile();
            const int rc = run({ 47910 }, "indítsd el", out);
            const std::string txt = out ? readAllFile(out) : "";
            if (out) std::fclose(out);
            std::printf("  --pair kimenete:\n%s", txt.c_str());
            CHECK(rc == 0, "--pair: 0-s kilépési kód");
            const char* label = "Párosító kód: ";
            const size_t at = txt.find(label);
            const std::string code = at == std::string::npos ? "" : txt.substr(at + std::strlen(label), 6);
            CHECK(code.size() == 6 && std::all_of(code.begin(), code.end(), ::isdigit), "6 jegyű kód kiírva");
            CHECK(txt.find("most bekapcsoltam") != std::string::npos, "jelzi, hogy a hálózati elérést bekapcsolta");
            CHECK(saved.find("\"enabled\":true") != std::string::npos, "a hálózati elérés bekapcsolva és elmentve");
            // A kiírt kóddal a telefon párosodhat a hálózati porton (kódos párosítás)
            std::string resp;
            for (int i = 0; i < 20 && resp.empty(); ++i) {
                resp = wsHandshakeLive(47911, "/?pair=" + code + "&client=aqulify_remote&tier=paid&name=PairCli");
                if (resp.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            CHECK(resp.rfind("HTTP/1.1 101", 0) == 0, "a kiírt kóddal a párosítás elfogadva (101)");
            bridge.stop();
        }
        // Nincs futó motor ezen a porton: 1-es kód és útmutatás
        std::FILE* out2 = std::tmpfile();
        const int rc2 = run({ 47912 }, "Indítsd el a motort.", out2);
        const std::string txt2 = out2 ? readAllFile(out2) : "";
        if (out2) std::fclose(out2);
        CHECK(rc2 == 1 && txt2.find("A motor nem fut") != std::string::npos && txt2.find("Indítsd el a motort.") != std::string::npos,
              "nem futó motor: 1-es kód és útmutatás");
    }
}

static void testBluetoothLine() {
    std::printf("[14] Bluetooth-tartalék: soros keretezés, első soros token, ugyanazok az üzenetek (bt_link.hpp)\n");

    // 1. Sorokra bontás (a valódi LineSplitter)
    {
        btlink::LineSplitter sp;
        std::vector<std::string> out;
        const std::string a = "{\"a\":1}\n{\"b\":2}\r\n\n{\"c\":";
        CHECK(sp.push(a.data(), a.size(), out) && out.size() == 2 && out[0] == "{\"a\":1}" && out[1] == "{\"b\":2}",
              "két teljes sor kijön, a \\r lekerül, az üres sor kimarad");
        const std::string b = "3}\n";
        CHECK(sp.push(b.data(), b.size(), out) && out.size() == 3 && out[2] == "{\"c\":3}",
              "a két részletben érkező sor egyben jön ki");
        btlink::LineSplitter big;
        std::vector<std::string> o2;
        const std::string max(btlink::kMaxLineBytes, 'x');
        CHECK(big.push(max.data(), max.size(), o2), "pontosan 1 MiB-os sor még elfogadható");
        CHECK(!big.push("y", 1, o2), "1 MiB-nál hosszabb sor: hiba (a kapcsolat bomlik)");
    }
    // 2. Kimenő sor: nyers sortörés nem maradhat benne, a JSON ugyanaz marad
    {
        const std::string payload = "{\"type\":\"state\",\n\"name\":\"a\\nb\"}";
        const std::string line = btlink::toLine(payload);
        json::Value v;
        CHECK(line.back() == '\n' && line.find('\n') == line.size() - 1, "a kimenő üzenet pontosan egy sor");
        CHECK(json::parse(line.substr(0, line.size() - 1), v) && jsonField(line.substr(0, line.size() - 1), "name") == "a\nb",
              "a sorrá alakított üzenet ugyanazt a JSON-t adja (a szövegbeli sortörés megmarad)");
    }
    // 3. Az első sor értelmezése
    {
        const auto ok = btlink::parseHello("{\"type\":\"hello\",\"token\":\"abc\",\"app\":\"x\"}");
        CHECK(ok.ok && ok.token == "abc" && ok.app == "x", "hello tokennel: elfogadható alak");
        CHECK(!btlink::parseHello("{\"type\":\"ping\",\"token\":\"abc\"}").ok, "nem hello: elutasítva");
        CHECK(!btlink::parseHello("{\"type\":\"hello\"}").ok, "hello token nélkül: elutasítva");
        CHECK(!btlink::parseHello("nem json").ok, "nem JSON: elutasítva");
    }
    // 4. Tokenpróbálgatás elleni zár
    {
        btlink::AuthGate g;
        const auto t0 = std::chrono::steady_clock::now();
        bool started = false;
        for (int i = 0; i < btlink::kMaxAuthFails - 1; ++i) started = g.fail(t0) || started;
        CHECK(!started && !g.locked(t0), "%d hibás próbálkozás után még nincs zárolás", btlink::kMaxAuthFails - 1);
        CHECK(g.fail(t0) && g.locked(t0 + std::chrono::seconds(btlink::kAuthLockSeconds - 1)),
              "%d. hibás próbálkozás: zárolás %d másodpercig", btlink::kMaxAuthFails, btlink::kAuthLockSeconds);
        CHECK(!g.locked(t0 + std::chrono::seconds(btlink::kAuthLockSeconds + 1)), "a zárolás lejár");
    }
    CHECK(btlink::formatAddress(0x001A7DDA7113ULL) == "00:1A:7D:DA:71:13", "Bluetooth-cím szövegként");

    // 4b. Az SDP-bejegyzés megújítása (a gép alvása után a Windows elfelejtheti): valódi rádióval, ha van
    {
        net::startup();   // a hidak leálláskor lezárják a Winsockot; ez a próba híd nélkül nyit socketet
        btlink::RfcommListener l;
        if (l.open()) {
            const std::string addr = l.address();
            CHECK(l.refresh() && l.refresh(), "SDP-bejegyzés kétszer egymás után megújítható");
            CHECK(l.isOpen() && l.address() == addr, "megújítás után ugyanaz a figyelő és cím");
            CHECK(l.acceptOne() == net::kInvalidSocket && !l.broken(), "várakozó kapcsolat nélkül nem számít hibának");
            l.close();
            CHECK(!l.isOpen() && !l.refresh(), "bezárás után nincs megújítás");
        } else {
            std::printf("  (nincs Bluetooth-rádió / nem Windows: a megújítás csak false-t ad)\n");
            CHECK(!l.refresh() && !l.broken(), "rádió nélkül a megújítás false, hiba nincs");
        }
        net::cleanup();
    }

    // 5. Élő próba a valódi híddal: a soros tesztporton ugyanaz a kód fut, mint a Bluetoothról jövő kapcsolatnál
    const std::string token = bridge_detail::randomHex(32);
    const std::string remoteDoc = "{\"enabled\":true,\"port\":47897,\"devices\":[{\"id\":\"0b0c0d0e\",\"name\":\"BT-teszt\",\"hash\":\"" +
                                  bridge_detail::sha1Hex(token) + "\",\"created\":0,\"last\":0}]}";
    BridgeCallbacks cb;
    double phase = 0.0;
    bool give = false;
    cb.pullAudio = [&](float* lr, size_t maxFrames) -> size_t {   // folyamatos hang: legyen mit spektrumként küldeni
        give = !give;   // a híd addig húz, amíg kap: minden második hívás üres, így egy körben egy adag jön
        if (!give) return 0;
        const size_t n = std::min<size_t>(maxFrames, 480);
        for (size_t i = 0; i < n; ++i) {
            lr[2 * i] = lr[2 * i + 1] = static_cast<float>(0.3 * std::sin(phase));
            phase += 2.0 * 3.14159265358979 * 1000.0 / 48000.0;
        }
        return n;
    };
    cb.sampleRate = [] { return 48000.0; };
    cb.getParams = [] { return transparentParams(); };
    cb.setParams = [](const ParamData&) {};
    cb.loadRemote = [&] { return remoteDoc; };
    cb.saveRemote = [](const std::string&) { return true; };
    WebSocketBridge bridge(47896);
    bridge.setLineTestPort(47902);
    const bool started = bridge.start(cb);
    CHECK(started, "a híd elindul (127.0.0.1:47896, soros tesztport: 47902)");
    if (!started) return;

    // Jó token: az első sor után "machine" és "state", a rögtön utána küldött ping is választ kap
    net::socket_t s = tcpConnectLive(47902);
    CHECK(s != net::kInvalidSocket, "soros kapcsolat a tesztportra");
    if (s != net::kInvalidSocket) {
        rawSend(s, "{\"type\":\"hello\",\"client\":\"aqulify-remote\",\"token\":\"" + token + "\"}\n{\"type\":\"ping\",\"t\":7}\n");
        const auto lines = lineReadFor(s, 800);
        CHECK(hasType(lines, "machine") && hasType(lines, "state"), "jó token: megjön a machine és a state üzenet");
        const auto m = std::find_if(lines.begin(), lines.end(), [](const std::string& l) { return jsonField(l, "type") == "machine"; });
        CHECK(m != lines.end() && jsonField(*m, "machine").size() == 16 &&
                  jsonField(*m, "computer") == MdnsAnnouncer::computerName(),
              "a machine üzenetben a gép azonosítója és neve (computer) is megvan, mint a WebSocketen");
        const bool pong = std::any_of(lines.begin(), lines.end(), [](const std::string& l) {
            json::Value v;
            return json::parse(l, v) && jsonField(l, "type") == "pong" && v.get("t") && v.get("t")->numberOr(0) == 7.0;
        });
        CHECK(pong, "a hello után azonnal küldött ping: pong ugyanazzal a t-vel");
        CHECK(std::all_of(lines.begin(), lines.end(), [](const std::string& l) { json::Value v; return json::parse(l, v); }),
              "minden beérkező sor önálló, érvényes JSON");

        rawSend(s, "{\"type\":\"get_bt_info\"}\n{\"type\":\"subscribe\",\"spectrum\":1,\"fps\":60}\n");
        const auto more = lineReadFor(s, 1200);
        const auto info = std::find_if(more.begin(), more.end(), [](const std::string& l) { return jsonField(l, "type") == "bt_info"; });
        json::Value iv;
        CHECK(info != more.end() && json::parse(*info, iv) && iv.get("available") &&
                  iv.get("available")->type == json::Value::Type::Bool && iv.get("addr"),
              "get_bt_info: bt_info válasz available és addr mezővel");
        if (info != more.end() && iv.get("available") && iv.get("available")->b) {
            // Van Bluetooth-rádió (pl. Windowson): a cím "AA:BB:CC:DD:EE:FF" alakú, és nem csupa nulla
            const std::string addr = jsonField(*info, "addr");
            bool shape = addr.size() == 17;
            for (size_t i = 0; shape && i < addr.size(); ++i)
                shape = (i % 3 == 2) ? addr[i] == ':' : std::isxdigit(static_cast<unsigned char>(addr[i])) != 0;
            CHECK(shape && addr != "00:00:00:00:00:00", "a Bluetooth-csatorna fut: a gép Bluetooth-címe érvényes alakú");
        } else {
            std::printf("  [--]   ezen a gépen nincs Bluetooth-csatorna (available:false): a cím nem ellenőrizhető\n");
        }
        CHECK(!hasType(more, "spectrum"), "Bluetoothon nincs spektrum (subscribe után sem)");
        CHECK(hasType(more, "status"), "az állapotüzenet (status) Bluetoothon is megy");
    }
    // Ellenőrzés: ugyanekkor a helyi WebSocket-felület kap spektrumot (vagyis a spektrum ténylegesen készül)
    {
        net::socket_t l = wsConnectLive(47896, "/");
        CHECK(l != net::kInvalidSocket && !wsWaitFor(l, "\"type\":\"spectrum\"", 1500).empty(),
              "összevetés: a helyi felület ugyanekkor kap spektrumot");
        // A párosítás visszavonása a gépen: a Bluetoothon csatlakozott telefon is lekapcsolódik
        if (l != net::kInvalidSocket) {
            wsSendText(l, "{\"type\":\"remote_revoke\",\"id\":\"0b0c0d0e\"}");
            bool closed = false;
            if (s != net::kInvalidSocket) lineReadFor(s, 1500, &closed);
            CHECK(closed, "a párosítás visszavonása után a gép bontja a soros kapcsolatot");
            net::closeSocket(l);
        }
    }
    if (s != net::kInvalidSocket) net::closeSocket(s);

    // Rossz esetek: rossz token, nem hello első sor, túl hosszú sor, hiányzó első sor
    auto refused = [&](const std::string& first, const char* what) {
        net::socket_t r = tcpConnectLive(47902);
        if (r == net::kInvalidSocket) { CHECK(false, "%s: nem sikerült kapcsolódni", what); return; }
        rawSend(r, first);
        bool closed = false;
        const auto lines = lineReadFor(r, 1500, &closed);
        CHECK(lines.size() == 1 && jsonField(lines[0], "type") == "auth" && lines[0].find("\"ok\":false") != std::string::npos && closed,
              "%s: {\"type\":\"auth\",\"ok\":false}, majd bontás", what);
        net::closeSocket(r);
    };
    refused("{\"type\":\"hello\",\"token\":\"" + token + "\"}\n", "visszavont token");
    refused("{\"type\":\"hello\",\"token\":\"" + std::string(64, 'f') + "\"}\n", "ismeretlen token");
    refused("{\"type\":\"get_state\"}\n", "nem hello az első sor");
    {
        net::socket_t r = tcpConnectLive(47902);
        bool closed = false;
        if (r != net::kInvalidSocket) {
            rawSend(r, std::string(btlink::kMaxLineBytes + 16, 'x'));
            const auto lines = lineReadFor(r, 2000, &closed);
            CHECK(lines.empty() && closed, "túl hosszú első sor: válasz nélküli bontás");
            net::closeSocket(r);
        }
    }
    {
        net::socket_t r = tcpConnectLive(47902);
        bool closed = false;
        if (r != net::kInvalidSocket) {
            const auto lines = lineReadFor(r, (btlink::kHelloTimeoutSeconds + 2) * 1000, &closed);
            CHECK(lines.empty() && closed, "ha %d mp alatt nem jön első sor: bontás", btlink::kHelloTimeoutSeconds);
            net::closeSocket(r);
        }
    }
    // Eddig 3 hibás próbálkozás; még kettő, és a következő kapcsolat válasz nélkül, azonnal bomlik (zárolás)
    refused("{\"type\":\"hello\",\"token\":\"" + std::string(64, 'e') + "\"}\n", "4. hibás token");
    refused("{\"type\":\"hello\",\"token\":\"" + std::string(64, 'd') + "\"}\n", "5. hibás token");
    {
        net::socket_t r = tcpConnectLive(47902);
        bool closed = false;
        if (r != net::kInvalidSocket) {
            rawSend(r, "{\"type\":\"hello\",\"token\":\"" + std::string(64, 'c') + "\"}\n");
            const auto lines = lineReadFor(r, 1000, &closed);
            CHECK(lines.empty() && closed, "zárolás alatt: a kapcsolat válasz nélkül, azonnal bomlik");
            net::closeSocket(r);
        }
    }
    bridge.stop();
}


static void testMdnsBrowser() {
    std::printf("[17] mDNS szolgáltatás-felderítő és csomag-értelmező (MdnsBrowser)\n");
    // 1. Cím-szűrés: RFC 1918, link-local és loopback elfogadva, publikus és kívüli eldobva
    CHECK(MdnsBrowser::isPrivateOrLinkLocal("192.168.1.1"), "192.168.x privát cím elfogadva");
    CHECK(MdnsBrowser::isPrivateOrLinkLocal("10.0.0.5"), "10.x privát cím elfogadva");
    CHECK(MdnsBrowser::isPrivateOrLinkLocal("172.16.0.1"), "172.16.x privát cím elfogadva");
    CHECK(MdnsBrowser::isPrivateOrLinkLocal("172.31.255.254"), "172.31.x privát cím elfogadva");
    CHECK(MdnsBrowser::isPrivateOrLinkLocal("169.254.10.20"), "169.254.x link-local elfogadva");
    CHECK(MdnsBrowser::isPrivateOrLinkLocal("127.0.0.1"), "127.0.0.1 loopback elfogadva");
    CHECK(!MdnsBrowser::isPrivateOrLinkLocal("8.8.8.8"), "8.8.8.8 nyilvános cím eldobva");
    CHECK(!MdnsBrowser::isPrivateOrLinkLocal("1.1.1.1"), "1.1.1.1 nyilvános cím eldobva");
    CHECK(!MdnsBrowser::isPrivateOrLinkLocal("172.15.255.255"), "172.15.x ablakon kívüli cím eldobva");
    CHECK(!MdnsBrowser::isPrivateOrLinkLocal("172.32.0.1"), "172.32.x ablakon kívüli cím eldobva");

    // 2. Query csomag felépítése
    auto q = MdnsBrowser::buildQuery("_aqulify._tcp.local");
    CHECK(q.size() > 20, "query csomag létrejött (%zu bájt)", q.size());
    std::string qs(q.begin(), q.end());
    CHECK(qs.find("_aqulify") != std::string::npos, "query tartalmazza a szolgáltatásnevet");

    // 3. Valódi MdnsAnnouncer által generált csomag értelmezése
    MdnsAnnouncer an;
    an.start(47820, "Aqulify Teszt PC", { "192.168.1.25", "10.0.0.8" }, "mach_test_1234");
    auto pkt = an.buildPacket(120);
    an.stop();

    auto list = MdnsBrowser::parsePacket(pkt.data(), pkt.size());
    CHECK(list.size() == 1, "egy szolgáltatás felderítve (találatok száma: %zu)", list.size());
    if (!list.empty()) {
        const auto& s = list[0];
        CHECK(s.kind == "engine", "szolgáltatás fajtája engine");
        CHECK(s.name == "Aqulify Teszt PC", "szolgáltatás neve egyezik: %s", s.name.c_str());
        CHECK(s.port == 47820, "port 47820");
        CHECK(s.addrs.size() == 2, "2 cím felderítve");
        CHECK(s.txt.count("proto") && s.txt.at("proto") == "2", "TXT proto=2");
        CHECK(s.txt.count("id") && s.txt.at("id") == "mach_test_1234", "TXT machineId megvan");
    }

    // 4. Sérült és csonka csomagok ellenőrzése: nem dobhat kivételt, nem olvas túl
    for (size_t len = 0; len < pkt.size(); ++len) {
        auto trList = MdnsBrowser::parsePacket(pkt.data(), len);
        (void)trList;
    }
    CHECK(true, "csonka csomagok minden bájthosszon (0-%zu) biztonságosan lekezelve", pkt.size() - 1);

    std::vector<uint8_t> corrupted = pkt;
    if (corrupted.size() > 20) {
        corrupted[14] = 0xC0;
        corrupted[15] = 14; // önmagára mutató pointer
        auto cList = MdnsBrowser::parsePacket(corrupted.data(), corrupted.size());
        (void)cList;
        CHECK(true, "ciklikus DNS mutató nem okoz végtelen ciklust");
    }

    // 5. Élő próbája két elszigetelt motorral: az egyik discover kérése megtalálja a másikat
    BridgeCallbacks cbA, cbB;
    cbA.pullAudio = [](float*, size_t) -> size_t { return 0; };
    cbA.sampleRate = [] { return 48000.0; };
    cbB.pullAudio = [](float*, size_t) -> size_t { return 0; };
    cbB.sampleRate = [] { return 48000.0; };

    WebSocketBridge bridgeA(47891);
    WebSocketBridge bridgeB(47892);
    bridgeA.start(cbA);
    bridgeB.start(cbB);

    net::socket_t sb = wsConnectLive(47892, "/");
    if (sb != net::kInvalidSocket) {
        wsSendText(sb, "{\"type\":\"set_remote\",\"enabled\":true,\"name\":\"Távoli Aqulify\"}");
        wsWaitFor(sb, "\"enabled\":true", 1000);
    }

    net::socket_t sa = wsConnectLive(47891, "/");
    if (sa != net::kInvalidSocket) {
        wsSendText(sa, "{\"type\":\"discover\",\"timeoutMs\":600}");
        std::string discResp = wsWaitFor(sa, "\"type\":\"discovered\"", 2500);
        CHECK(!discResp.empty(), "discover válasz megérkezett a helyi klienshez");
        CHECK(discResp.find("\"engines\"") != std::string::npos, "válasz tartalmaz engines listát");
        CHECK(discResp.find("\"shares\"") != std::string::npos, "válasz tartalmaz shares listát");
        net::closeSocket(sa);
    }
    if (sb != net::kInvalidSocket) net::closeSocket(sb);

    bridgeA.stop();
    bridgeB.stop();
}

static void testRemoteRelay() {
    std::printf("[18] Távvezérlés és átjátszó (relay) réteg (RemoteRelayManager)\n");

    // 1. Célok tárolása és JSON szerializáció: token soha nem szivárog ki
    std::vector<RemoteTarget> tgts = {
        {"t_pi", "Pi Nappali", "127.0.0.1", 47925, "1122334455667788112233445566778811223344556677881122334455667788", "mach_pi", 1000}
    };
    std::string pubJson = relay_detail::targetsToJson(tgts, false);
    CHECK(pubJson.find("token") == std::string::npos, "kimenő targets üzenetben SOHA nincs token");
    CHECK(pubJson.find("Pi Nappali") != std::string::npos, "targets üzenet tartalmazza a nevet");
    CHECK(pubJson.find("t_pi") != std::string::npos, "targets üzenet tartalmazza az id-t");

    std::string privJson = relay_detail::targetsToJson(tgts, true);
    CHECK(privJson.find("1122334455667788") != std::string::npos, "belső mentés tartalmazza a tokent");
    auto parsedTgts = relay_detail::targetsFromJson(privJson);
    CHECK(parsedTgts.size() == 1 && parsedTgts[0].token == tgts[0].token, "targetsFromJson visszaolvassa a tokent");

    // 2. Két motor példány valódi socketekkel: Host B (távoli) és Host A (helyi)
    std::string aTargetsDoc = "";

    BridgeCallbacks cbA, cbB;
    cbA.pullAudio = [](float*, size_t) -> size_t { return 0; };
    cbA.sampleRate = [] { return 48000.0; };
    cbA.loadTargets = [&] { return aTargetsDoc; };
    cbA.saveTargets = [&](const std::string& j) { aTargetsDoc = j; return true; };

    cbB.pullAudio = [](float*, size_t) -> size_t { return 0; };
    cbB.sampleRate = [] { return 48000.0; };

    WebSocketBridge bridgeA(47893);
    WebSocketBridge bridgeB(47894);
    bridgeA.start(cbA);
    bridgeB.start(cbB);

    net::socket_t sB = wsConnectLive(47894, "/");
    int bPort = 47915;
    if (sB != net::kInvalidSocket) {
        wsSendText(sB, "{\"type\":\"set_remote\",\"enabled\":true,\"port\":47915}");
        std::string remMsg = wsWaitFor(sB, "\"enabled\":true", 2000);
        json::Value rVal;
        if (json::parse(remMsg, rVal) && rVal.get("port")) {
            bPort = static_cast<int>(rVal.get("port")->numberOr(47915));
        }
    }

    net::socket_t sA = wsConnectLive(47893, "/");
    CHECK(sA != net::kInvalidSocket, "helyi kliens csatlakozik Host A-hoz");

    if (sA != net::kInvalidSocket && sB != net::kInvalidSocket) {
        // Hibás kód tesztje
        wsSendText(sA, "{\"type\":\"target_pair\",\"host\":\"127.0.0.1\",\"port\":" + std::to_string(bPort) + ",\"code\":\"000000\",\"name\":\"Rossz\"}");
        std::string errResp = wsWaitFor(sA, "\"target_pair_result\"", 2000);
        CHECK(errResp.find("\"ok\":false") != std::string::npos, "hibás kód elutasítva (401 unauthorized)");

        // Kérjünk új kódot Host B-től és olvassuk ki
        wsSendText(sB, "{\"type\":\"remote_new_code\"}");
        std::string codeMsg = wsWaitFor(sB, "\"code\":\"", 2000);
        std::string code = jsonField(codeMsg, "code");
        CHECK(code.size() == 6, "Host B adott párosító kódot: %s", code.c_str());

        // Sikeres párosítás Host A-ból
        wsSendText(sA, "{\"type\":\"target_pair\",\"host\":\"127.0.0.1\",\"port\":" + std::to_string(bPort) + ",\"code\":\"" + code + "\",\"name\":\"Távoli Gép\"}");
        std::string pairResp = wsWaitFor(sA, "\"target_pair_result\"", 3000);
        CHECK(pairResp.find("\"ok\":true") != std::string::npos, "sikeres párosítás Host B-vel");
        CHECK(pairResp.find("token") == std::string::npos, "a párosítási válaszban SOHA nem szerepel token");

        std::string targetId;
        json::Value prDoc;
        if (json::parse(pairResp, prDoc) && prDoc.get("target")) {
            const auto* tid = prDoc.get("target")->get("id");
            if (tid) targetId = tid->stringOr("");
        }
        CHECK(!targetId.empty(), "targetId megérkezett: %s", targetId.c_str());

        // Mentett célok lekérdezése
        wsSendText(sA, "{\"type\":\"targets\"}");
        std::string tgtsResp = wsWaitFor(sA, "\"type\":\"targets\"", 2000);
        CHECK(tgtsResp.find("Távoli Gép") != std::string::npos, "targets listában szerepel a célgép");
        CHECK(tgtsResp.find("token") == std::string::npos, "targets válaszban SOHA nincs token");

        // Átjátszó (relay) teszt: kapcsolódás GET /relay?target=<targetId>
        net::socket_t rly = wsConnectLive(47893, "/relay?target=" + targetId);
        CHECK(rly != net::kInvalidSocket, "kapcsolódás a relay végponthoz sikeres");
        if (rly != net::kInvalidSocket) {
            std::string st = wsWaitFor(rly, "\"type\":", 2000);
            CHECK(!st.empty(), "átjátszón keresztül megérkezett a távoli üzenet");

            wsSendText(rly, "{\"type\":\"get_state\"}");
            std::string stateResp = wsWaitFor(rly, "\"type\":\"state\"", 2000);
            CHECK(!stateResp.empty(), "átjátszón küldött get_state kérésre válaszolt a távoli motor");

            net::closeSocket(rly);
        }

        // Ismeretlen target-id tesztje
        net::socket_t unkSock = wsConnectLive(47893, "/relay?target=non_existent");
        if (unkSock != net::kInvalidSocket) {
            std::string rerr = wsWaitFor(unkSock, "\"relay_error\"", 1000);
            CHECK(rerr.find("not_found") != std::string::npos, "ismeretlen célra relay_error not_found érkezik");
            net::closeSocket(unkSock);
        }

        // Cél elfelejtése
        wsSendText(sA, "{\"type\":\"target_forget\",\"id\":\"" + targetId + "\"}");
        std::string fgtResp = wsWaitFor(sA, "\"target_forget_result\"", 2000);
        CHECK(fgtResp.find("\"ok\":true") != std::string::npos, "cél elfelejtve");

        // Elfelejtett cél után a relay már nem csatlakozik
        net::socket_t deadRly = wsConnectLive(47893, "/relay?target=" + targetId);
        if (deadRly != net::kInvalidSocket) {
            std::string rerr = wsWaitFor(deadRly, "\"relay_error\"", 1000);
            CHECK(rerr.find("not_found") != std::string::npos, "elfelejtett célra nem csatlakozik, not_found");
            net::closeSocket(deadRly);
        }

        net::closeSocket(sA);
    }
    if (sB != net::kInvalidSocket) net::closeSocket(sB);

    bridgeA.stop();
    bridgeB.stop();
}


static void testRemoteAdmin() {
    std::printf("[19] Teljes jogú távirányító (admin jogosultságok, visszavonás és fej nélküli párosítás)\n");

    std::string savedRemote = "";
    BridgeCallbacks cb;
    cb.pullAudio = [](float*, size_t) -> size_t { return 0; };
    cb.sampleRate = [] { return 48000.0; };
    cb.audioOptionsJson = [] { return "{\"volume\":1.0}"; };
    cb.setAudioOptions = [](const json::Value&) {};
    cb.libraryRequest = [](const std::string& type, const json::Value&, bool remote) -> std::string {
        if (remote && (type == "library_set_roots" || type == "library_browse" || type == "library_rescan")) {
            return "{\"error\":\"remote\"}";
        }
        return "{\"ok\":true,\"type\":\"" + type + "\"}";
    };
    cb.saveRemote = [&](const std::string& j) { savedRemote = j; return true; };
    cb.loadRemote = [&] { return savedRemote; };

    WebSocketBridge bridge(47895);
    const bool started = bridge.start(cb);
    CHECK(started, "a híd elindul a 47895-ös porton");
    if (!started) return;

    net::socket_t sLocal = wsConnectLive(47895, "/");
    CHECK(sLocal != net::kInvalidSocket, "helyi kliens csatlakozik a 47895-ös hídhoz");
    if (sLocal == net::kInvalidSocket) {
        bridge.stop();
        return;
    }

    // Távoli elérés bekapcsolása a helyi vezérlőcsatornán
    int remPort = 47916;
    wsSendText(sLocal, "{\"type\":\"set_remote\",\"enabled\":true,\"port\":47916}");
    std::string remMsg = wsWaitFor(sLocal, "\"enabled\":true", 2000);
    json::Value rVal;
    if (json::parse(remMsg, rVal) && rVal.get("port")) {
        remPort = static_cast<int>(rVal.get("port")->numberOr(47916));
    }

    // 1. Normál (nem admin) távoli kliens párosítása
    wsSendText(sLocal, "{\"type\":\"remote_new_code\"}");
    std::string code1Msg = wsWaitFor(sLocal, "\"code\":\"", 2000);
    std::string code1 = jsonField(code1Msg, "code");
    CHECK(code1.size() == 6, "párosító kód normál távoli kliensnek: %s", code1.c_str());

    net::socket_t sRemote1 = wsConnectLive(remPort, "/?pair=" + code1 + "&client=aqulify_desktop&name=RemoteDesktop1");
    CHECK(sRemote1 != net::kInvalidSocket, "RemoteDesktop1 sikeresen csatlakozott");

    std::string paired1 = wsWaitFor(sRemote1, "\"type\":\"paired\"", 2000);
    std::string dev1Id = jsonField(paired1, "id");
    CHECK(!dev1Id.empty(), "RemoteDesktop1 eszköz azonosító: %s", dev1Id.c_str());

    // 2. Nem-admin távoli kérések: tiltva
    wsSendText(sRemote1, "{\"type\":\"library_set_roots\",\"roots\":[\"/mnt/zene\"]}");
    std::string r1 = wsWaitFor(sRemote1, "\"remote\"", 2000);
    CHECK(!r1.empty() && r1.find("\"error\":\"remote\"") != std::string::npos,
          "nem-admin távoli kliens nem állíthat zenetár gyökereket (remote)");

    wsSendText(sRemote1, "{\"type\":\"set_audio_opts\",\"opts\":{}}");
    std::string r2 = wsWaitFor(sRemote1, "\"remote\"", 2000);
    CHECK(!r2.empty() && r2.find("\"error\":\"remote\"") != std::string::npos,
          "nem-admin távoli kliens nem állíthat hangopciókat (remote)");

    // Hamis admin mező a kérésben: nem ad jogot
    wsSendText(sRemote1, "{\"type\":\"library_set_roots\",\"admin\":true,\"roots\":[\"/mnt/zene\"]}");
    std::string r3 = wsWaitFor(sRemote1, "\"remote\"", 2000);
    CHECK(!r3.empty() && r3.find("\"error\":\"remote\"") != std::string::npos,
          "hamis 'admin':true a kérés JSON-ben nem ad admin jogot");

    // 3. Admin jog megadása a gazdagépről a már élő kapcsolaton
    wsSendText(sLocal, "{\"type\":\"remote_set_admin\",\"id\":\"" + dev1Id + "\",\"admin\":true}");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!bridge.pairingOpen(), "az admin kapcsoló után a párosítási ablak nem nyílik meg (pairingOpen hamis)");

    wsSendText(sRemote1, "{\"type\":\"library_set_roots\",\"roots\":[\"/mnt/zene\"]}");
    std::string rAdmin1 = wsWaitFor(sRemote1, "\"type\":\"library_set_roots\"", 2000);
    CHECK(!rAdmin1.empty() && rAdmin1.find("\"type\":\"library_set_roots\"") != std::string::npos,
          "admin távoli kliens sikeresen beállíthat zenetár gyökereket");

    wsSendText(sRemote1, "{\"type\":\"set_audio_opts\",\"opts\":{}}");
    std::string rAdmin2 = wsWaitFor(sRemote1, "\"audio_opts\"", 2000);
    CHECK(!rAdmin2.empty() && rAdmin2.find("\"audio_opts\"") != std::string::npos,
          "admin távoli kliens sikeresen beállíthat hangopciókat");

    // 4. Admin jog azonnali visszavonása a gazdagépről
    wsSendText(sLocal, "{\"type\":\"remote_set_admin\",\"id\":\"" + dev1Id + "\",\"admin\":false}");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    wsSendText(sRemote1, "{\"type\":\"library_set_roots\",\"roots\":[\"/mnt/zene\"]}");
    std::string rRevoked1 = wsWaitFor(sRemote1, "\"remote\"", 2000);
    CHECK(!rRevoked1.empty() && rRevoked1.find("\"error\":\"remote\"") != std::string::npos,
          "admin visszavonása után azonnal tiltva a már élő kapcsolaton (library)");

    wsSendText(sRemote1, "{\"type\":\"set_audio_opts\",\"opts\":{}}");
    std::string rRevoked2 = wsWaitFor(sRemote1, "\"remote\"", 2000);
    CHECK(!rRevoked2.empty() && rRevoked2.find("\"error\":\"remote\"") != std::string::npos,
          "admin visszavonása után azonnal tiltva a már élő kapcsolaton (audio_opts)");

    net::closeSocket(sRemote1);

    // Egyetlen kódkérés, majd várakozás az új, érvényes kódra. Újra kérni nem szabad: minden kérés új kódot nyit, és
    // az előzőt érvényteleníti (a régi állapotüzenetek üres vagy régi kódja miatt különben a régebbi kódot kapnánk).
    auto requestNewCode = [&](const char* request, const std::string& previous) -> std::string {
        wsSendText(sLocal, request);
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (std::chrono::steady_clock::now() < until) {
            const std::string c = jsonField(wsWaitFor(sLocal, "\"code\":\"", 500), "code");
            if (c.size() == 6 && c != previous) return c;
        }
        return {};
    };

    // 5. Párosítás fej nélkül --admin flaggel
    const std::string code2 = requestNewCode("{\"type\":\"remote_new_code\",\"admin\":true}", code1);
    CHECK(code2.size() == 6, "párosító kód admin flaggel: %s", code2.c_str());

    net::socket_t sRemote2 = wsConnectLive(remPort, "/?pair=" + code2 + "&client=aqulify_desktop&name=RemoteDesktopAdmin");
    CHECK(sRemote2 != net::kInvalidSocket, "RemoteDesktopAdmin sikeresen csatlakozott");

    if (sRemote2 != net::kInvalidSocket) {
        std::string paired2 = wsWaitFor(sRemote2, "\"type\":\"paired\"", 2000);
        CHECK(!paired2.empty(), "RemoteDesktopAdmin párosítás sikeres");

        wsSendText(sRemote2, "{\"type\":\"library_set_roots\",\"roots\":[\"/mnt/zene\"]}");
        std::string rHeadless = wsWaitFor(sRemote2, "\"type\":\"library_set_roots\"", 2000);
        CHECK(!rHeadless.empty() && rHeadless.find("\"type\":\"library_set_roots\"") != std::string::npos,
              "fej nélküli --admin párosítással az eszköz azonnal teljes jogú");

        net::closeSocket(sRemote2);
    }

    // 6. nextPairAdmin_ nem ragad be: admin kód kérése után normál kód nyitása nem ad admin jogot
    const std::string codeAdmin = requestNewCode("{\"type\":\"remote_new_code\",\"admin\":true}", code2);
    CHECK(codeAdmin.size() == 6, "új admin párosító kód: %s", codeAdmin.c_str());

    // Most felülírjuk egy normál kóddal
    const std::string cNorm = requestNewCode("{\"type\":\"remote_new_code\"}", codeAdmin);
    CHECK(cNorm.size() == 6, "új normál párosító kód: %s", cNorm.c_str());

    net::socket_t sRemoteNorm = wsConnectLive(remPort, "/?pair=" + cNorm + "&client=aqulify_desktop&name=RemoteNorm");
    if (sRemoteNorm != net::kInvalidSocket) {
        std::string pNorm = wsWaitFor(sRemoteNorm, "\"type\":\"paired\"", 2000);
        CHECK(!pNorm.empty(), "RemoteNorm párosítás sikeres");
        wsSendText(sRemoteNorm, "{\"type\":\"library_set_roots\",\"roots\":[\"/mnt/zene\"]}");
        std::string rNorm = wsWaitFor(sRemoteNorm, "\"remote\"", 2000);
        CHECK(!rNorm.empty() && rNorm.find("\"error\":\"remote\"") != std::string::npos,
              "normál kód után az eszköz nem kap admin jogot (nextPairAdmin_ nem ragadt be)");
        net::closeSocket(sRemoteNorm);
    }

    // 7. Admin jog megmarad újraindítás után (saveRemoteConfig kiírja az admin mezőt)
    CHECK(savedRemote.find("\"admin\":true") != std::string::npos, "a mentett konfiguráció tartalmazza az admin:true mezőt");
    net::closeSocket(sLocal);
    bridge.stop();

    // Újraindítás ugyanazzal a perzisztens tárolóval
    WebSocketBridge bridge2(47895);
    const bool started2 = bridge2.start(cb);
    CHECK(started2, "a 2. híd elindul a mentett konfigurációval");

    net::socket_t sLocal2 = wsConnectLive(47895, "/");
    CHECK(sLocal2 != net::kInvalidSocket, "helyi kliens csatlakozik az újraindított hídhoz");

    std::string remMsg2;
    wsSendText(sLocal2, "{\"type\":\"get_remote\"}");
    remMsg2 = wsWaitFor(sLocal2, "\"devices\":", 2000);
    CHECK(remMsg2.find("\"admin\":true") != std::string::npos, "újraindítás után az admin eszköz megőrizte az admin jogát");

    // 8. Híd szála nem blokkol: elérhetetlen célra (192.0.2.1) indított párosítás közben a híd egy másik klienst <0.5s alatt kiszolgál
    wsSendText(sLocal2, "{\"type\":\"target_pair\",\"host\":\"192.0.2.1\",\"port\":47820,\"code\":\"123456\",\"name\":\"Unreachable\"}");

    auto tConnStart = std::chrono::steady_clock::now();
    net::socket_t sOther = wsConnectLive(47895, "/");
    auto tConnMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tConnStart).count();
    CHECK(sOther != net::kInvalidSocket && tConnMs < 500, "új kliens azonnal csatlakozhat háttérbeli kapcsolódás alatt (csatlakozás: %ld ms)", (long)tConnMs);
    if (sOther != net::kInvalidSocket) {
        auto tPingStart = std::chrono::steady_clock::now();
        wsSendText(sOther, "{\"type\":\"ping\",\"t\":99}");
        std::string pong = wsWaitFor(sOther, "\"pong\"", 500);
        auto tPingMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tPingStart).count();
        CHECK(!pong.empty() && tPingMs < 500,
              "elérhetetlen címre párosítás közben a másik kliens <0.5s alatt választ kap (eltelt: %ld ms)", (long)tPingMs);
        net::closeSocket(sOther);
    }

    net::closeSocket(sLocal2);
    bridge2.stop();
}

static void testVersionCheck() {
    std::printf("[20] Verzióellenőrzés (aqulify-versions.json, a kliens verziója, frissítési jelzés)\n");
    using namespace vercheck;

    // 1. A verziófájl értelmezése
    Latest l = parseVersions("{\"desktop\":\"2.3.0\",\"android\":\"1.0.1\",\"protocol\":2,\"min_client_protocol\":2}");
    CHECK(l.ok && l.desktop == "2.3.0" && l.android == "1.0.1" && l.protocol == 2 && l.minClientProtocol == 2,
          "érvényes verziófájl: minden mező beolvasva");
    CHECK(!parseVersions("").ok, "üres válasz: nem érvényes");
    CHECK(!parseVersions("<html>404</html>").ok, "nem JSON (pl. hibaoldal): nem érvényes");
    CHECK(!parseVersions("{\"desktop\":\"2.3\"}").ok, "hibás asztali verzió (X.Y): nem érvényes");
    CHECK(!parseVersions("{\"desktop\":\"2.3.0-beta\"}").ok, "hibás asztali verzió (utótag): nem érvényes");
    CHECK(!parseVersions("{\"android\":\"1.0.1\"}").ok, "asztali verzió nélkül: nem érvényes");
    l = parseVersions("{\"desktop\":\"2.3.0\",\"android\":\"<script>\",\"protocol\":-5}");
    CHECK(l.ok && l.android.empty() && l.protocol == 0, "hibás android-verzió és negatív protokoll: kimarad, a többi érvényes");
    CHECK(!parseVersions("{\"desktop\":\"2.3.0\",\"x\":\"" + std::string(kMaxBytes, 'a') + "\"}").ok,
          "64 KB-nál nagyobb válasz: nem érvényes");

    // 2. Összevetés, kliensadatok tisztítása
    CHECK(isNewer("2.3.0", "2.2.1") && isNewer("2.10.0", "2.9.9") && !isNewer("2.3.0", "2.3.0") && !isNewer("2.2.9", "2.3.0"),
          "verzió-összevetés (számonként, nem szövegként)");
    CHECK(!isNewer("abc", "2.3.0") && !isNewer("2.3.0", ""), "érvénytelen verzió: nem frissebb");
    CHECK(cleanClientVersion("1.0.1") == "1.0.1" && cleanClientVersion("1.0.1\"x").empty() &&
              cleanClientVersion(std::string(100, '1')).empty(),
          "kliensverzió: csak X.Y.Z marad meg");
    CHECK(cleanPlatform("android") == "android" && cleanPlatform("desktop") == "desktop" && cleanPlatform("ios").empty(),
          "platform: csak android / desktop");
    CHECK(fetchWithCurl("http://example.invalid/x").empty() && fetchWithCurl("https://x'; rm -rf /'").empty(),
          "curl-letöltés: nem https vagy idézőjeles cím el sem indul");

    // 3. Az ellenőrző állapota
    Checker chk;
    CHECK(chk.statusJson().empty(), "adat nélkül nincs latest / update_available mező");
    chk.setCurrentForTest("2.2.1");
    chk.apply(parseVersions("{\"desktop\":\"2.3.0\",\"android\":\"1.0.1\",\"protocol\":2,\"min_client_protocol\":2}"));
    std::string st = chk.statusJson();
    CHECK(st.find("\"update_available\":{\"ver\":\"2.3.0\"}") != std::string::npos && st.find("\"android\":\"1.0.1\"") != std::string::npos,
          "régebbi saját verziónál update_available: 2.3.0 (%s)", st.c_str());
    chk.setCurrentForTest("2.3.0");
    st = chk.statusJson();
    CHECK(st.find("update_available") == std::string::npos && st.find("\"latest\"") != std::string::npos,
          "azonos verziónál nincs update_available, a latest megmarad");

    // 4. A híd: a kliens verziója a párosított eszköznél, mentés, a gép saját ellenőrzése a remote üzenetben
    std::string savedRemote;
    BridgeCallbacks cb;
    cb.pullAudio = [](float*, size_t) -> size_t { return 0; };
    cb.sampleRate = [] { return 48000.0; };
    cb.saveRemote = [&](const std::string& j) { savedRemote = j; return true; };
    cb.loadRemote = [&] { return savedRemote; };
    Checker chk2;
    chk2.setCurrentForTest("2.2.1");
    chk2.apply(parseVersions("{\"desktop\":\"2.3.0\",\"protocol\":2,\"min_client_protocol\":2}"));
    cb.versionStatus = [&chk2] { return chk2.statusJson(); };

    WebSocketBridge bridge(47897);
    const bool started = bridge.start(cb);
    CHECK(started, "a híd elindul a 47897-es porton");
    if (!started) return;
    net::socket_t sLocal = wsConnectLive(47897, "/");
    CHECK(sLocal != net::kInvalidSocket, "helyi kliens csatlakozik");
    if (sLocal == net::kInvalidSocket) { bridge.stop(); return; }

    wsSendText(sLocal, "{\"type\":\"hello\",\"client\":\"t\"}");
    const std::string state = wsWaitFor(sLocal, "\"type\":\"state\"", 2000);
    CHECK(state.find("\"min_client_protocol\":2") != std::string::npos, "a state üzenetben min_client_protocol: 2");

    wsSendText(sLocal, "{\"type\":\"set_remote\",\"enabled\":true,\"port\":47918}");
    std::string rem = wsWaitFor(sLocal, "\"enabled\":true", 2000);
    int remPort = 47918;
    if (json::Value rv; json::parse(rem, rv) && rv.get("port")) remPort = static_cast<int>(rv.get("port")->numberOr(47918));
    CHECK(rem.find("\"update_available\":{\"ver\":\"2.3.0\"}") != std::string::npos,
          "a remote üzenetben a gép saját ellenőrzése: update_available 2.3.0");

    wsSendText(sLocal, "{\"type\":\"remote_new_code\"}");
    std::string code;
    for (int i = 0; i < 6 && code.size() != 6; ++i) code = jsonField(wsWaitFor(sLocal, "\"code\":\"", 500), "code");
    CHECK(code.size() == 6, "párosító kód: %s", code.c_str());
    net::socket_t sPhone = wsConnectLive(remPort, "/?pair=" + code + "&client=aqulify_remote&name=Telefon");
    CHECK(sPhone != net::kInvalidSocket, "a telefon párosodik");
    if (sPhone != net::kInvalidSocket) {
        CHECK(!wsWaitFor(sPhone, "\"type\":\"paired\"", 2000).empty(), "paired üzenet");
        // hibás verzió: nem tárolja
        wsSendText(sPhone, "{\"type\":\"hello\",\"client\":\"aqulify-remote\",\"app_version\":\"1.0\\\"x\",\"platform\":\"android\"}");
        wsWaitFor(sPhone, "\"type\":\"state\"", 2000);
        wsSendText(sLocal, "{\"type\":\"get_remote\"}");
        std::string r1 = wsWaitFor(sLocal, "\"devices\":[{", 2000);
        CHECK(!r1.empty() && r1.find("\"ver\":\"1.0") == std::string::npos && r1.find("\"platform\"") == std::string::npos,
              "hibás app_version: nem kerül az eszközlistába");
        // érvényes verzió
        wsSendText(sPhone, "{\"type\":\"hello\",\"client\":\"aqulify-remote\",\"app_version\":\"1.0.1\",\"platform\":\"android\"}");
        wsWaitFor(sPhone, "\"type\":\"state\"", 2000);
        wsSendText(sLocal, "{\"type\":\"get_remote\"}");
        std::string r2 = wsWaitFor(sLocal, "\"ver\":\"1.0.1\"", 2000);
        CHECK(!r2.empty() && r2.find("\"platform\":\"android\"") != std::string::npos,
              "érvényes app_version és platform: az eszközlistában ver 1.0.1, android");
        CHECK(savedRemote.find("\"ver\":\"1.0.1\"") != std::string::npos, "a verzió a mentett beállításba is bekerül");
        net::closeSocket(sPhone);
    }
    net::closeSocket(sLocal);
    bridge.stop();

    // 5. Újraindítás után is megvan
    WebSocketBridge bridge2(47897);
    CHECK(bridge2.start(cb), "a 2. híd elindul a mentett beállítással");
    net::socket_t s2 = wsConnectLive(47897, "/");
    if (s2 != net::kInvalidSocket) {
        wsSendText(s2, "{\"type\":\"get_remote\"}");
        CHECK(!wsWaitFor(s2, "\"ver\":\"1.0.1\"", 2000).empty(), "újraindítás után az eszköz verziója megmaradt");
        net::closeSocket(s2);
    }
    bridge2.stop();

    // 6. A --pair kiírja a frissebb verziót
    paircli::PairInfo pi;
    CHECK(paircli::parseRemote("{\"type\":\"remote\",\"code\":\"123456\",\"update_available\":{\"ver\":\"2.3.0\"}}", pi) &&
              pi.updateVer == "2.3.0" && paircli::formatPairInfo(pi, false).find("frissebb Aqulify: 2.3.0") != std::string::npos,
          "--pair: a frissebb verzió a kiírásban");
    CHECK(paircli::parseRemote("{\"type\":\"remote\",\"update_available\":{\"ver\":\"x;y\"}}", pi) && pi.updateVer.empty(),
          "--pair: hibás verziót nem ír ki");
}

static void testBridgeLoudnessStereo() {
    std::printf("[16] Híd: Loudness és Stereo Tools üzenetek, állapot, A/B és érvényesítés\n");
    std::mutex mu;
    ParamData cur = transparentParams();
    BridgeCallbacks cb;
    cb.pullAudio = [](float*, size_t) -> size_t { return 0; };
    cb.sampleRate = [] { return 48000.0; };
    cb.getParams = [&] { std::lock_guard<std::mutex> lk(mu); return cur; };
    cb.setParams = [&](const ParamData& p) { std::lock_guard<std::mutex> lk(mu); cur = p; };

    WebSocketBridge bridge(47898);
    const bool started = bridge.start(cb);
    CHECK(started, "a híd elindul a 47898-as porton");
    if (!started) return;

    std::string first;
    net::socket_t s = wsConnectLive(47898, "/", &first);
    CHECK(s != net::kInvalidSocket, "helyi kliens csatlakozik a hídhoz");
    if (s == net::kInvalidSocket) {
        bridge.stop();
        return;
    }

    // 1. Kezdeti állapotüzenet ellenőrzése
    std::string state = wsWaitFor(s, "\"modules\"", 2000);
    CHECK(state.find("\"loudness\":0") != std::string::npos, "állapot: modules.loudness = 0");
    CHECK(state.find("\"stereo\":0") != std::string::npos, "állapot: modules.stereo = 0");
    CHECK(state.find("\"loudness\":{\"volume\":") != std::string::npos, "állapot: loudness blokk jelen van");
    CHECK(state.find("\"stereo\":{\"inputGain\":") != std::string::npos, "állapot: stereo blokk jelen van");

    auto getCur = [&] { std::lock_guard<std::mutex> lk(mu); return cur; };

    // 2. Loudness bekapcsolása (set_module_loudness)
    wsSendText(s, "{\"type\":\"set_module_loudness\",\"val\":1}");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(getCur().has(kFlagLoudness), "set_module_loudness: kFlagLoudness aktív");

    // 3. Loudness paraméterek beállítása és sanitizeParams (határértékek levágása)
    wsSendText(s, "{\"type\":\"set_loudness\",\"volume\":-99.0,\"inputGain\":45.0,\"outputGain\":-40.0,\"std\":9,\"mode\":12,\"hclip\":1,\"hclipRange\":50.0,\"approx\":7}");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ParamData p = getCur();
    CHECK(p.loudVolumeDb == -83.0f, "loudVolumeDb határértékekre levágva (-83 dB)");
    CHECK(p.loudInputDb == 36.0f, "loudInputDb határértékekre levágva (+36 dB)");
    CHECK(p.loudOutputDb == -36.0f, "loudOutputDb határértékekre levágva (-36 dB)");
    CHECK(p.loudStd == 4, "loudStd határértékekre levágva (4: ISO 226:2023)");
    CHECK(p.loudMode == 7, "loudMode határértékekre levágva (7: FFT 16384)");
    CHECK(p.loudHclip == 1, "loudHclip beállítva (1)");
    CHECK(p.loudHclipRangeDb == 24.0f, "loudHclipRangeDb határértékekre levágva (24 dB)");
    CHECK(p.loudApprox == 4, "loudApprox határértékekre levágva (4: Best)");

    // 4. Loudness A/B összehasonlítás (set_loudness_compare)
    wsSendText(s, "{\"type\":\"set_loudness_compare\",\"compare\":1}");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(getCur().loudCompare == 1, "set_loudness_compare: loudCompare = 1");

    // 5. Loudness kikapcsolása: az A/B-nek is ki kell kapcsolnia!
    wsSendText(s, "{\"type\":\"set_module_loudness\",\"val\":0}");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!getCur().has(kFlagLoudness), "set_module_loudness: kFlagLoudness inaktív");
    CHECK(getCur().loudCompare == 0, "modul kikapcsolásakor loudCompare = 0 (A/B törlődött)");

    // 6. Stereo Tools bekapcsolása (set_module_stereo)
    wsSendText(s, "{\"type\":\"set_module_stereo\",\"val\":1}");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(getCur().has(kFlagStereoTools), "set_module_stereo: kFlagStereoTools aktív");

    // 7. Stereo Tools paraméterek és sanitizeParams
    wsSendText(s, "{\"type\":\"set_stereo\",\"inputGain\":-10.0,\"outputGain\":3.0,\"balanceIn\":-2.5,\"balanceOut\":1.8,"
                  "\"softclip\":1,\"mutel\":1,\"muter\":0,\"phasel\":0,\"phaser\":1,\"mode\":8,"
                  "\"slev\":-5.0,\"sbal\":0.2,\"mlev\":2.0,\"mpan\":-0.4,\"stereoBase\":-1.5,"
                  "\"delay\":25.0,\"scLevel\":150.0,\"stereoPhase\":400.0,\"dry\":-50.0,\"wet\":-120.0}");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    p = getCur();
    CHECK(p.stInputGainDb == -10.0f, "stInputGainDb érvényesült (-10 dB)");
    CHECK(p.stOutputGainDb == 3.0f, "stOutputGainDb érvényesült (+3 dB)");
    CHECK(p.stBalanceIn == -1.0f, "stBalanceIn határértékekre levágva (-1.0)");
    CHECK(p.stBalanceOut == 1.0f, "stBalanceOut határértékekre levágva (+1.0)");
    CHECK(p.stSoftclip == 1 && p.stMutel == 1 && p.stMuter == 0 && p.stPhasel == 0 && p.stPhaser == 1,
          "logikai kapcsolók (softclip, mutel, muter, phasel, phaser) helyesek");
    CHECK(p.stMode == 6, "stMode határértékekre levágva (6)");
    CHECK(p.stSlevDb == -5.0f && p.stMlevDb == 2.0f, "slev és mlev érvényesült");
    CHECK(p.stStereoBase == -1.0f, "stStereoBase határértékekre levágva (-1.0)");
    CHECK(p.stDelayMs == 20.0f, "stDelayMs határértékekre levágva (+20 ms)");
    CHECK(p.stScLevel == 100.0f, "stScLevel határértékekre levágva (100.0)");
    CHECK(p.stStereoPhase == 360.0f, "stStereoPhase határértékekre levágva (360.0)");
    CHECK(p.stDryDb == -50.0f, "stDryDb érvényesült (-50 dB)");
    CHECK(p.stWetDb == -100.0f, "stWetDb határértékekre levágva (-100 dB)");

    // 8. Stereo Tools A/B összehasonlítás (set_stereo_compare)
    wsSendText(s, "{\"type\":\"set_stereo_compare\",\"compare\":1}");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(getCur().stCompare == 1, "set_stereo_compare: stCompare = 1");

    // 9. Stereo Tools kikapcsolása: az A/B-nek is ki kell kapcsolnia!
    wsSendText(s, "{\"type\":\"set_module_stereo\",\"val\":0}");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!getCur().has(kFlagStereoTools), "set_module_stereo: kFlagStereoTools inaktív");
    CHECK(getCur().stCompare == 0, "modul kikapcsolásakor stCompare = 0 (A/B törlődött)");

    net::closeSocket(s);
    bridge.stop();
}

// =========================================================================
// Utonkenti EQ (K11 tarolo es K13 hid) befagyasztott tesztek
// =========================================================================

template <class CfgT>
concept HasCoreWayCfg = requires(CfgT& c) {
    c.outputs[0].way.on;
    c.outputs[0].way.numBands;
    c.outputs[0].way.bands[0].type;
    c.outputs[0].way.bands[0].freq;
    c.outputs[0].way.bands[0].gainDb;
    c.outputs[0].way.bands[0].q;
};

template <class CbT, class CfgT>
concept HasBridgeWayCb = HasCoreWayCfg<CfgT> && requires(CbT& cb) {
    cb.getMultiOut;
    cb.setWayEq;
};

#ifndef _WIN32
template <class CfgT = multiout::MultiConfig>
static void testMultiOutStorageImpl() {
    std::printf("[21] multiout.bin v2 / v1 (2.5.0) / v11 (2.0.7) tárolóteszt (K11)\n");
    namespace fs = std::filesystem;

    const std::string tmpRoot = "/tmp/aq_k11_store_" + std::to_string(::getpid());
    std::error_code ec;
    fs::remove_all(tmpRoot, ec);
    fs::create_directories(tmpRoot, ec);

    const char* prevXdg = std::getenv("XDG_CONFIG_HOME");
    const std::string savedXdg = prevXdg ? prevXdg : "";
    const bool hadXdg = (prevXdg != nullptr);
#ifdef __APPLE__
    const char* prevHome = std::getenv("HOME");
    const std::string savedHome = prevHome ? prevHome : "";
    ::setenv("HOME", tmpRoot.c_str(), 1);
    const std::string appDir = tmpRoot + "/Library/Application Support/Aqulify";
    fs::create_directories(appDir, ec);
#else
    ::setenv("XDG_CONFIG_HOME", tmpRoot.c_str(), 1);
    const std::string appDir = tmpRoot + "/aqulify";
    fs::create_directories(appDir, ec);
#endif
    const std::string binPath = appDir + "/multiout.bin";

    auto restoreEnv = [&] {
#ifdef __APPLE__
        if (!savedHome.empty()) ::setenv("HOME", savedHome.c_str(), 1);
#else
        if (hadXdg) ::setenv("XDG_CONFIG_HOME", savedXdg.c_str(), 1);
        else ::unsetenv("XDG_CONFIG_HOME");
#endif
        fs::remove_all(tmpRoot, ec);
    };

    // 1. v2 iras-olvasas oda-vissza (hdr[1] == 2)
    {
        CfgT orig{};
        orig.enabled = true;
        orig.mode = multiout::Mode::Crossover;
        orig.numOutputs = 3;
        orig.numCrossovers = 2;
        orig.crossoverHz = { 120.0f, 2400.0f, 5000.0f };
        orig.slopeDbPerOct = 48;
        orig.outputs[0].band = 0;
        orig.outputs[1].band = 1;
        orig.outputs[1].gainDb = -2.5f;
        orig.outputs[1].delayMs = 1.25f;
        orig.outputs[2].band = 2;
        orig.outputs[2].invert = true;

        if constexpr (HasCoreWayCfg<CfgT>) {
            orig.outputs[1].way.on = true;
            orig.outputs[1].way.numBands = 2;
            orig.outputs[1].way.bands[0].type = static_cast<int32_t>(EqBandType::Bell);
            orig.outputs[1].way.bands[0].freq = 4000.0f;
            orig.outputs[1].way.bands[0].gainDb = 5.5f;
            orig.outputs[1].way.bands[0].q = 2.0f;
            orig.outputs[1].way.bands[1].type = static_cast<int32_t>(EqBandType::LoShelf);
            orig.outputs[1].way.bands[1].freq = 80.0f;
            orig.outputs[1].way.bands[1].gainDb = -6.0f;
            orig.outputs[1].way.bands[1].q = 0.707f;
        }

        const std::array<std::string, 3> tgts = { "dac_b", "dac_c", "" };
        const bool okSave = linux_store::saveMultiOut(orig, tgts);
        CHECK(okSave, "K11: saveMultiOut sikeres");

        uint32_t hdr[4] = {};
        {
            std::ifstream f(binPath, std::ios::binary);
            f.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
        }
        std::printf("K_METRIC: {\"test\":\"K11_v2\",\"hdr1\":%u,\"bytes\":%zu}\n",
                    hdr[1], static_cast<size_t>(fs::file_size(binPath, ec)));
        CHECK(hdr[0] == linux_store::kMultiMagic && hdr[1] == 2u,
              "K11: multiout.bin fejléc verziója hdr[1]=%u (elvárt 2)", hdr[1]);

        CfgT loaded{};
        std::array<std::string, 3> gotTgts{};
        const bool okLoad = linux_store::loadMultiOut(loaded, gotTgts);
        const bool sameRaw = okLoad && (std::memcmp(&loaded, &orig, sizeof(orig)) == 0) && gotTgts == tgts;
        CHECK(sameRaw, "K11: multiout.bin v2 visszaolvasás bitre azonos");
    }

    // 2. 2.5.0-s (v1) fajl visszafele kompatibilis betoltese
    {
        struct OldOutputV1 {
            bool enabled = true;
            int32_t band = -1;
            float gainDb = 0.0f;
            float delayMs = 0.0f;
            bool invert = false;
            ParamData chain = transparentParams();
        };
        struct OldMultiV1 {
            bool enabled = false;
            multiout::Mode mode = multiout::Mode::Zones;
            int32_t numOutputs = 2;
            int32_t numCrossovers = 1;
            std::array<float, multiout::kMaxCrossovers> crossoverHz = { 80.0f, 1000.0f, 5000.0f };
            int32_t slopeDbPerOct = 24;
            std::array<OldOutputV1, multiout::kMaxOutputs> outputs{};
        };

        OldMultiV1 oldCfg{};
        oldCfg.enabled = true;
        oldCfg.mode = multiout::Mode::Crossover;
        oldCfg.numOutputs = 2;
        oldCfg.numCrossovers = 1;
        oldCfg.crossoverHz[0] = 650.0f;
        oldCfg.slopeDbPerOct = 24;
        oldCfg.outputs[0].band = 0;
        oldCfg.outputs[0].gainDb = -1.5f;
        oldCfg.outputs[1].band = 1;
        oldCfg.outputs[1].delayMs = 2.0f;
        oldCfg.outputs[1].invert = true;

        const uint32_t hdr[4] = {
            linux_store::kMultiMagic, 1u, kParamLayoutVersion, static_cast<uint32_t>(sizeof(OldMultiV1))
        };
        std::vector<char> bytes(sizeof(hdr) + sizeof(OldMultiV1));
        std::memcpy(bytes.data(), hdr, sizeof(hdr));
        std::memcpy(bytes.data() + sizeof(hdr), &oldCfg, sizeof(OldMultiV1));
        const std::array<std::string, 3> v1Tgts = { "sink_v1", "", "" };
        for (const auto& t : v1Tgts) {
            const uint32_t n = static_cast<uint32_t>(t.size());
            bytes.insert(bytes.end(), reinterpret_cast<const char*>(&n), reinterpret_cast<const char*>(&n) + 4);
            bytes.insert(bytes.end(), t.data(), t.data() + n);
        }
        {
            std::ofstream f(binPath, std::ios::binary | std::ios::trunc);
            f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }

        CfgT loaded{};
        std::array<std::string, 3> gotTgts{};
        const bool okV1 = linux_store::loadMultiOut(loaded, gotTgts);
        bool wayClean = false;
        if constexpr (HasCoreWayCfg<CfgT>) {
            wayClean = true;
            for (int k = 0; k < multiout::kMaxOutputs; ++k)
                wayClean = wayClean && !loaded.outputs[k].way.on && loaded.outputs[k].way.numBands == 0;
        }
        CHECK(okV1 && wayClean && loaded.enabled && loaded.mode == multiout::Mode::Crossover &&
              std::fabs(loaded.crossoverHz[0] - 650.0f) < 1e-4f && loaded.outputs[1].invert &&
              std::fabs(loaded.outputs[1].delayMs - 2.0f) < 1e-4f && gotTgts[0] == "sink_v1",
              "K11: 2.5.0-s (v1) multiout.bin betöltődik, way EQ kikapcsolva, adatok változatlanok");
    }

    // 3. 2.0.7-es (v11) fajl visszafele kompatibilis betoltese
    {
        linux_store::legacy::MultiConfigV11 v11{};
        v11.enabled = true;
        v11.mode = multiout::Mode::Crossover;
        v11.numOutputs = 2;
        v11.numCrossovers = 1;
        v11.crossoverHz = { 1200.0f, 3000.0f, 6000.0f };
        v11.slopeDbPerOct = 12;
        v11.outputs[0].enabled = true; v11.outputs[0].band = 0;
        v11.outputs[1].enabled = true; v11.outputs[1].band = 1;
        v11.outputs[1].gainDb = -4.0f;
        const ParamData defP = transparentParams();
        std::memcpy(v11.outputs[0].chain, &defP, kParamSizeV11);
        std::memcpy(v11.outputs[1].chain, &defP, kParamSizeV11);

        const uint32_t hdr[4] = {
            linux_store::kMultiMagic, 1u, 11u, static_cast<uint32_t>(sizeof(linux_store::legacy::MultiConfigV11))
        };
        std::vector<char> bytes(sizeof(hdr) + sizeof(v11));
        std::memcpy(bytes.data(), hdr, sizeof(hdr));
        std::memcpy(bytes.data() + sizeof(hdr), &v11, sizeof(v11));
        for (int i = 0; i < 3; ++i) {
            const uint32_t n = 0;
            bytes.insert(bytes.end(), reinterpret_cast<const char*>(&n), reinterpret_cast<const char*>(&n) + 4);
        }
        {
            std::ofstream f(binPath, std::ios::binary | std::ios::trunc);
            f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }

        CfgT loaded{};
        std::array<std::string, 3> gotTgts{};
        const bool okV11 = linux_store::loadMultiOut(loaded, gotTgts);
        bool wayClean = false;
        if constexpr (HasCoreWayCfg<CfgT>) {
            wayClean = true;
            for (int k = 0; k < multiout::kMaxOutputs; ++k)
                wayClean = wayClean && !loaded.outputs[k].way.on && loaded.outputs[k].way.numBands == 0;
        }
        CHECK(okV11 && wayClean && loaded.slopeDbPerOct == 12 &&
              std::fabs(loaded.crossoverHz[0] - 1200.0f) < 1e-4f &&
              std::fabs(loaded.outputs[1].gainDb + 4.0f) < 1e-4f,
              "K11: 2.0.7-es (v11) multiout.bin betöltődik, way EQ kikapcsolva, adatok változatlanok");
    }

    // 4. Serult es rovid fajlok elutasitasa osszeomlas nelkul
    {
        std::vector<char> junk = { 'A', 'M', 'O', 'T', 2, 0, 0, 0, 15, 0 };
        {
            std::ofstream f(binPath, std::ios::binary | std::ios::trunc);
            f.write(junk.data(), static_cast<std::streamsize>(junk.size()));
        }
        CfgT loaded{};
        std::array<std::string, 3> gotTgts{};
        CHECK(!linux_store::loadMultiOut(loaded, gotTgts), "K11: rövid fájl elutasítva összeomlás nélkül");

        const uint32_t badHdr[4] = { 0xDEADBEEFu, 2u, kParamLayoutVersion, sizeof(CfgT) };
        std::vector<char> badBytes(sizeof(badHdr) + sizeof(CfgT), 0);
        std::memcpy(badBytes.data(), badHdr, sizeof(badHdr));
        {
            std::ofstream f(binPath, std::ios::binary | std::ios::trunc);
            f.write(badBytes.data(), static_cast<std::streamsize>(badBytes.size()));
        }
        CHECK(!linux_store::loadMultiOut(loaded, gotTgts), "K11: sérült magic elutasítva összeomlás nélkül");
    }

    restoreEnv();
}
#endif

template <class CbT = BridgeCallbacks, class CfgT = multiout::MultiConfig>
static void testBridgeWayEqImpl() {
    std::printf("[22] Híd: set_way_eq, broadcast, validáció, set_multiout megőrzés és admin jogosultság (K13)\n");
    if constexpr (HasBridgeWayCb<CbT, CfgT>) {
        std::mutex mu;
        CfgT curMulti{};
        curMulti.enabled = true;
        curMulti.mode = multiout::Mode::Crossover;
        curMulti.numOutputs = 2;
        curMulti.numCrossovers = 1;
        curMulti.crossoverHz[0] = 500.0f;
        curMulti.slopeDbPerOct = 24;
        curMulti.outputs[0].band = 0;
        curMulti.outputs[1].band = 1;
        std::array<std::string, 3> curSinks = { "dac_b", "", "" };
        std::string savedRemote;

        auto buildMockMultiStatus = [&] {
            std::lock_guard<std::mutex> lk(mu);
            std::string j = "{";
            j += "\"supported\":true,";
            j += "\"enabled\":" + std::string(curMulti.enabled ? "true" : "false") + ",";
            j += "\"mode\":\"" + std::string(curMulti.mode == multiout::Mode::Crossover ? "crossover" : "zones") + "\",";
            j += "\"slope\":" + std::to_string(curMulti.slopeDbPerOct) + ",";
            j += "\"crossovers\":[" + std::to_string(curMulti.crossoverHz[0]) + "],";
            j += "\"outputs\":[";
            for (int k = 0; k < curMulti.numOutputs; ++k) {
                if (k > 0) j += ",";
                const auto& o = curMulti.outputs[k];
                j += "{\"band\":" + std::to_string(o.band) + ",\"gain\":" + std::to_string(o.gainDb) + ",\"way\":{";
                j += "\"on\":" + std::string(o.way.on ? "true" : "false") + ",\"bands\":[";
                for (int b = 0; b < o.way.numBands; ++b) {
                    if (b > 0) j += ",";
                    const auto& bd = o.way.bands[b];
                    const char* tn = bd.type == static_cast<int32_t>(EqBandType::LoShelf) ? "lowshelf"
                                   : bd.type == static_cast<int32_t>(EqBandType::HiShelf) ? "highshelf" : "bell";
                    j += "{\"type\":\"" + std::string(tn) + "\",\"freq\":" + std::to_string(bd.freq) +
                         ",\"gain\":" + std::to_string(bd.gainDb) + ",\"q\":" + std::to_string(bd.q) + "}";
                }
                j += "]}}";
            }
            j += "]}";
            return j;
        };

        ParamData curP = transparentParams();
        CbT cb{};
        cb.pullAudio = [](float*, size_t) -> size_t { return 0; };
        cb.sampleRate = [] { return 48000.0; };
        cb.getParams = [&] { std::lock_guard<std::mutex> lk(mu); return curP; };
        cb.setParams = [&](const ParamData& p) { std::lock_guard<std::mutex> lk(mu); curP = p; };
        cb.saveRemote = [&](const std::string& s) { std::lock_guard<std::mutex> lk(mu); savedRemote = s; return true; };
        cb.loadRemote = [&] { std::lock_guard<std::mutex> lk(mu); return savedRemote; };
        cb.getMultiOut = [&] { std::lock_guard<std::mutex> lk(mu); return curMulti; };
        cb.multiOutStatus = buildMockMultiStatus;
        cb.setMultiOut = [&](const CfgT& c, const std::array<std::string, 3>& s) {
            std::lock_guard<std::mutex> lk(mu);
            curMulti = c;
            curSinks = s;
        };
        cb.setWayEq = [&](int output, const decltype(curMulti.outputs[0].way)& w) {
            std::lock_guard<std::mutex> lk(mu);
            if (output >= 0 && output < multiout::kMaxOutputs) {
                curMulti.outputs[output].way = w;
                multiout::sanitize(curMulti);
            }
        };

        WebSocketBridge bridge(47899);
        const bool started = bridge.start(cb);
        CHECK(started, "K13: a híd elindul a 47899-es porton");
        if (!started) return;

        net::socket_t s1 = wsConnectLive(47899, "/");
        net::socket_t s2 = wsConnectLive(47899, "/");
        CHECK(s1 != net::kInvalidSocket && s2 != net::kInvalidSocket, "K13: két helyi kliens csatlakozik");
        if (s1 == net::kInvalidSocket || s2 == net::kInvalidSocket) {
            if (s1 != net::kInvalidSocket) net::closeSocket(s1);
            if (s2 != net::kInvalidSocket) net::closeSocket(s2);
            bridge.stop();
            return;
        }
        wsWaitFor(s1, "\"modules\"", 1000);
        wsWaitFor(s2, "\"modules\"", 1000);

        wsSendText(s1, "{\"type\":\"set_way_eq\",\"output\":1,\"on\":true,\"bands\":[{\"type\":\"bell\",\"freq\":4000,\"gain\":5.5,\"q\":1.5}]}");
        std::string b1 = wsWaitFor(s1, "\"type\":\"multiout\"", 2000);
        std::string b2 = wsWaitFor(s2, "\"type\":\"multiout\"", 2000);
        const bool ok1 = !b1.empty() && b1.find("\"way\":{\"on\":true") != std::string::npos && b1.find("4000") != std::string::npos;
        const bool ok2 = !b2.empty() && b2.find("\"way\":{\"on\":true") != std::string::npos && b2.find("4000") != std::string::npos;
        CHECK(ok1 && ok2, "K13: set_way_eq broadcast mindkét klienshez eljut");

        auto snapshotBefore = [&] { std::lock_guard<std::mutex> lk(mu); return curMulti; };
        const CfgT refGood = snapshotBefore();

        const char* badMsgs[] = {
            "{\"type\":\"set_way_eq\",\"output\":9,\"on\":true,\"bands\":[]}",
            "{\"type\":\"set_way_eq\",\"output\":1,\"bands\":[]}",
            "{\"type\":\"set_way_eq\",\"output\":1,\"on\":true,\"bands\":[{\"type\":\"bell\",\"freq\":\"abc\",\"gain\":2,\"q\":1}]}",
            "{\"type\":\"set_way_eq\",\"output\":1,\"on\":true,\"bands\":[{\"type\":\"bandpass\",\"freq\":1000,\"gain\":2,\"q\":1}]}"
        };
        bool allRejected = true;
        for (const char* m : badMsgs) {
            wsSendText(s1, m);
            std::string err = wsWaitFor(s1, "\"way_eq_error\"", 1500);
            std::lock_guard<std::mutex> lk(mu);
            if (err.empty() || std::memcmp(&curMulti, &refGood, sizeof(refGood)) != 0) allRejected = false;
        }
        CHECK(allRejected, "K13: hibás set_way_eq üzenetek elutasítva (way_eq_error), az állapot változatlan");

        wsSendText(s1, "{\"type\":\"set_multiout\",\"enabled\":1,\"mode\":\"crossover\",\"slope\":24,\"crossovers\":[600],"
                       "\"outputs\":[{\"band\":0,\"gain\":0,\"delay\":0,\"invert\":0,\"enabled\":1},"
                       "{\"sink\":\"dac_b\",\"band\":1,\"gain\":-1,\"delay\":0,\"invert\":0,\"enabled\":1}]}");
        std::string moRes = wsWaitFor(s1, "\"type\":\"multiout\"", 2000);
        bool preserved = !moRes.empty();
        {
            std::lock_guard<std::mutex> lk(mu);
            preserved = preserved && curMulti.outputs[1].way.on && curMulti.outputs[1].way.numBands == 1 &&
                        std::fabs(curMulti.outputs[1].way.bands[0].freq - 4000.0f) < 1e-3f;
        }
        CHECK(preserved, "K13: set_multiout (way mező nélkül) megőrzi az útonkénti EQ-t");

        wsSendText(s1, "{\"type\":\"set_remote\",\"enabled\":true,\"port\":47920}");
        wsWaitFor(s1, "\"enabled\":true", 2000);
        wsSendText(s1, "{\"type\":\"remote_new_code\"}");
        std::string code;
        const auto codeUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (std::chrono::steady_clock::now() < codeUntil) {
            std::string codeMsg = wsWaitFor(s1, "\"code\":\"", 500);
            code = jsonField(codeMsg, "code");
            if (code.size() == 6) break;
        }
        CHECK(code.size() == 6, "K13: párosító kód lekérve (%s)", code.c_str());
        if (code.size() == 6) {
            net::socket_t sRem = wsConnectLive(47920, "/?pair=" + code + "&client=aqulify_desktop&name=Guest");
            CHECK(sRem != net::kInvalidSocket, "K13: távoli nem-admin kliens csatlakozott");
            if (sRem != net::kInvalidSocket) {
                wsWaitFor(sRem, "\"type\":\"paired\"", 2000);
                wsSendText(sRem, "{\"type\":\"set_way_eq\",\"output\":1,\"on\":false,\"bands\":[]}");
                std::string rErr = wsWaitFor(sRem, "\"error\":\"remote\"", 2000);
                bool unch = false;
                {
                    std::lock_guard<std::mutex> lk(mu);
                    unch = !rErr.empty() && curMulti.outputs[1].way.on && curMulti.outputs[1].way.numBands == 1;
                }
                CHECK(unch, "K13: távoli nem-admin kliens set_way_eq kérése elutasítva (remote), állapot változatlan");
                net::closeSocket(sRem);
            }
        }

        net::closeSocket(s1);
        net::closeSocket(s2);
        bridge.stop();
    } else {
        CHECK(false, "K13: BridgeCallbacks::setWayEq / OutputConfig::way nincs megvalósítva");
    }
}

static void testBridgeMultiOutRemoteAuth() {
    std::printf("[23] Híd: set_multiout távoli jogosultság (nem-admin elutasítva, admin elfogadva)\n");
    std::mutex mu;
    multiout::MultiConfig curMulti{};
    curMulti.enabled = false;
    curMulti.mode = multiout::Mode::Zones;
    curMulti.numOutputs = 2;
    curMulti.crossoverHz[0] = 500.0f;
    std::array<std::string, 3> curSinks{};
    std::string savedRemote;
    int setCalls = 0;

    ParamData curP = transparentParams();
    BridgeCallbacks cb{};
    cb.pullAudio = [](float*, size_t) -> size_t { return 0; };
    cb.sampleRate = [] { return 48000.0; };
    cb.getParams = [&] { std::lock_guard<std::mutex> lk(mu); return curP; };
    cb.setParams = [&](const ParamData& p) { std::lock_guard<std::mutex> lk(mu); curP = p; };
    cb.saveRemote = [&](const std::string& s) { std::lock_guard<std::mutex> lk(mu); savedRemote = s; return true; };
    cb.loadRemote = [&] { std::lock_guard<std::mutex> lk(mu); return savedRemote; };
    cb.getMultiOut = [&] { std::lock_guard<std::mutex> lk(mu); return curMulti; };
    cb.setMultiOut = [&](const multiout::MultiConfig& c, const std::array<std::string, 3>& s) {
        std::lock_guard<std::mutex> lk(mu);
        curMulti = c;
        curSinks = s;
        ++setCalls;
    };
    cb.multiOutStatus = [&] {
        std::lock_guard<std::mutex> lk(mu);
        return std::string("{\"supported\":true,\"enabled\":") + (curMulti.enabled ? "true" : "false") +
               ",\"mode\":\"" + (curMulti.mode == multiout::Mode::Crossover ? "crossover" : "zones") +
               "\",\"crossovers\":[" + std::to_string(curMulti.crossoverHz[0]) + "]}";
    };

    WebSocketBridge bridge(47900);
    const bool started = bridge.start(cb);
    CHECK(started, "a híd elindul a 47900-as porton");
    if (!started) return;

    net::socket_t sLocal = wsConnectLive(47900, "/");
    CHECK(sLocal != net::kInvalidSocket, "helyi kliens csatlakozik a 47900-as portra");
    if (sLocal == net::kInvalidSocket) { bridge.stop(); return; }
    wsWaitFor(sLocal, "\"modules\"", 1000);

    wsSendText(sLocal, "{\"type\":\"set_remote\",\"enabled\":true,\"port\":47921}");
    std::string remOn = wsWaitFor(sLocal, "\"enabled\":true", 2000);
    CHECK(!remOn.empty(), "távoli elérés bekapcsolva (47921)");

    wsSendText(sLocal, "{\"type\":\"remote_new_code\"}");
    std::string code;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (std::chrono::steady_clock::now() < until) {
        std::string msg = wsWaitFor(sLocal, "\"code\":\"", 500);
        code = jsonField(msg, "code");
        if (code.size() == 6) break;
    }
    CHECK(code.size() == 6, "párosító kód lekérve: %s", code.c_str());

    if (code.size() == 6) {
        net::socket_t sRem = wsConnectLive(47921, "/?pair=" + code + "&client=aqulify_desktop&name=MultiGuest");
        CHECK(sRem != net::kInvalidSocket, "távoli kliens csatlakozik");
        if (sRem != net::kInvalidSocket) {
            std::string paired = wsWaitFor(sRem, "\"type\":\"paired\"", 2000);
            std::string devId = jsonField(paired, "id");
            CHECK(!devId.empty(), "párosított eszköz ID: %s", devId.c_str());

            const char* moReq = "{\"type\":\"set_multiout\",\"enabled\":1,\"mode\":\"crossover\",\"slope\":24,"
                                "\"crossovers\":[800],\"outputs\":[{\"band\":0,\"gain\":-3},{\"sink\":\"dac_b\",\"band\":1,\"gain\":2}]}";

            // 1. Nem-admin tavoli kliens -> elutasitva, beallitas valtozatlan
            wsSendText(sRem, moReq);
            std::string errMsg = wsWaitFor(sRem, "\"error\":\"remote\"", 2000);
            bool unchanged = false;
            {
                std::lock_guard<std::mutex> lk(mu);
                unchanged = !errMsg.empty() && setCalls == 0 && !curMulti.enabled &&
                            curMulti.mode == multiout::Mode::Zones &&
                            std::fabs(curMulti.crossoverHz[0] - 500.0f) < 1e-3f;
            }
            CHECK(unchanged, "nem-admin távoli kliens set_multiout kérése elutasítva (remote), állapot változatlan");

            // 2. Admin jog megadasa utan -> elfogadva
            wsSendText(sLocal, "{\"type\":\"remote_set_admin\",\"id\":\"" + devId + "\",\"admin\":true}");
            std::this_thread::sleep_for(std::chrono::milliseconds(80));

            wsSendText(sRem, moReq);
            std::string okMsg = wsWaitFor(sRem, "\"type\":\"multiout\"", 2000);
            bool accepted = false;
            {
                std::lock_guard<std::mutex> lk(mu);
                accepted = !okMsg.empty() && setCalls == 1 && curMulti.enabled &&
                           curMulti.mode == multiout::Mode::Crossover &&
                           std::fabs(curMulti.crossoverHz[0] - 800.0f) < 1e-3f;
            }
            CHECK(accepted, "admin távoli kliens set_multiout kérése elfogadva, konfiguráció frissült");
            net::closeSocket(sRem);
        }
    }

    net::closeSocket(sLocal);
    bridge.stop();
}

template <class O>
concept HasUpsampleOptions = requires(O& o) {
    o.upsampleRule;
    o.upsamplePhase;
    o.upsamplePrecision;
};

// Hibajelentés 2026-11-19 (Fedora, FiiO BTR7, 2.6.0): DSD lejátszásakor (DoP és natív) a lejátszásgombok nem hatottak,
// és a motor nem állt le. Ok: a vizualizáció forrása DSD alatt minden hívásra „csendet” adott (mindig maxFrames keretet),
// a híd hanglehúzó ciklusa ezért soha nem ért véget.

static void testVisualSourceDsd() {
    std::printf("[24] A vizualizáció forrása: közvetlen DSD-kimenetnél a híd nem kap hangot\n");
    std::vector<float> buf(2 * 256, 1.0f);
    int exclCalls = 0, engCalls = 0;
    auto excl = [&](float* d, size_t n) -> size_t { ++exclCalls; std::fill_n(d, n * 2, 0.25f); return n; };
    auto eng = [&](float* d, size_t n) -> size_t { ++engCalls; std::fill_n(d, n * 2, 0.5f); return n; };
    for (bool ex : { false, true }) {
        exclCalls = engCalls = 0;
        const size_t got = pullVisualAudio(true, ex, buf.data(), 256, excl, eng);
        CHECK(got == 0 && exclCalls == 0 && engCalls == 0,
              "közvetlen DSD-kimenet (kizárólagos jelző: %d): 0 keret, egyik lehallgatópontot sem olvassa (kapott: %u)",
              ex ? 1 : 0, static_cast<unsigned>(got));
    }
    exclCalls = engCalls = 0;
    CHECK(pullVisualAudio(false, true, buf.data(), 256, excl, eng) == 256 && exclCalls == 1 && engCalls == 0 && buf[0] == 0.25f,
          "kizárólagos PCM-kimenet: a lejátszó lehallgatópontja");
    exclCalls = engCalls = 0;
    CHECK(pullVisualAudio(false, false, buf.data(), 256, excl, eng) == 256 && engCalls == 1 && exclCalls == 0 && buf[0] == 0.5f,
          "egyébként: a motor lehallgatópontja");
}

static void testBridgeEndlessAudio() {
    std::printf("[25] Mindig adatot adó hangforrás mellett is válaszol a híd, és gyorsan leáll\n");
    // Őrszem: ha a híd beragad, a teszt ne fusson örökké (a beragadt szálat leállítani nem lehet, ezért kilépés)
    std::atomic<bool> done{ false };
    std::thread guard([&done] {
        for (int i = 0; i < 300 && !done.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!done.load()) {
            std::printf("  [HIBA] a híd 30 s után sem engedte tovább a tesztet (beragadt) – a teszt megszakítva\n");
            std::fflush(stdout);
            std::_Exit(2);
        }
    });
    constexpr double kSr = 352800.0;   // a DSD256 keretrátája natív DSD_U32_BE kimenetnél (a bejelentő DAC-ja)
    std::atomic<uint64_t> pulled{ 0 };
    BridgeCallbacks cb;
    cb.pullAudio = [&](float* lr, size_t maxFrames) -> size_t {   // soha nem fogy ki (mint a hibás DSD-ág)
        std::fill_n(lr, maxFrames * 2, 0.0f);
        pulled += maxFrames;
        return maxFrames;
    };
    cb.sampleRate = [] { return 352800.0; };
    cb.getParams = [] { return transparentParams(); };
    cb.setParams = [](const ParamData&) {};
    cb.loadRemote = [] { return std::string("{\"enabled\":false,\"port\":47931,\"bluetooth\":false,\"devices\":[]}"); };
    cb.saveRemote = [](const std::string&) { return true; };
    auto bridge = std::make_unique<WebSocketBridge>(47930);
    const bool started = bridge->start(cb);
    CHECK(started, "a híd elindul (127.0.0.1:47930)");
    if (started) {
        net::socket_t s = wsConnectLive(47930, "/", nullptr, 3000);
        CHECK(s != net::kInvalidSocket, "a felület 3 s-on belül csatlakozik");
        if (s != net::kInvalidSocket) {
            wsSendText(s, "{\"type\":\"ping\",\"t\":42}");
            CHECK(!wsWaitFor(s, "\"type\":\"pong\"", 2000).empty(), "parancs (ping) → válasz (pong) 2 s-on belül");
            CHECK(!wsWaitFor(s, "\"type\":\"status\"", 2000).empty(), "az állapotüzenet (status) megjön");
            // Valós idejű keret: a híd legfeljebb ~kétszer annyi hangot húz, amennyi valós időben keletkezik
            const uint64_t p0 = pulled.load();
            const auto t0 = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const double rate = static_cast<double>(pulled.load() - p0) / secs;
            CHECK(rate <= 2.5 * kSr && rate >= 0.5 * kSr,
                  "a lehúzás üteme a valós időhöz kötött: %.0f keret/s (elvárt %.0f–%.0f)", rate, 0.5 * kSr, 2.5 * kSr);
            net::closeSocket(s);
        }
        auto fut = std::async(std::launch::async, [&] { bridge->stop(); });
        const bool stopped = fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        CHECK(stopped, "a híd 2 s-on belül leáll (stop)");
        if (!stopped) {
            std::printf("  [HIBA] a híd leállítása beragadt – a teszt megszakítva\n");
            std::fflush(stdout);
            std::_Exit(2);
        }
    }
    bridge.reset();
    done = true;
    guard.join();
}

// 2.7.0 hibajelentés (AQ-4, Windows): bedugott USB DAC lett az alapértelmezett eszköz, de a lejátszó a régi, továbbra
// is érvényes beépített hangkártyán szólt tovább (csak az eszköz megszűnésére nyitott újra). A követés döntési logikája:
static void testDefaultDeviceFollow() {
    std::printf("[26] A lejátszó követi az alapértelmezett lejátszóeszközt (bedugott USB DAC), a villogást kivárja\n");
    using F = player::DefaultDeviceFollow<std::string>;
    using ms = std::chrono::milliseconds;
    const auto t0 = F::Clock::time_point{} + std::chrono::hours(1);
    {
        F f(ms(1500));
        f.opened("realtek");
        CHECK(!f.observe("realtek", t0) && !f.observe("realtek", t0 + ms(5000)), "ugyanaz az eszköz: nincs váltás");
        CHECK(!f.observe("dac", t0 + ms(6000)), "új alapértelmezett eszköz: az első észleléskor még nem vált");
        CHECK(!f.observe("dac", t0 + ms(7000)), "1 s múlva még nem vált (a rendszer még válthat)");
        CHECK(f.observe("dac", t0 + ms(7500)) && f.pending() == "dac", "1,5 s tartós változás után vált, az új eszközre");
        f.opened("dac");
        CHECK(!f.observe("dac", t0 + ms(9000)) && f.current() == "dac", "az új eszközön megnyitva: nincs további váltás");
    }
    {
        // bedugáskor a rendszer oda-vissza vált (a hibajelentés naplójában: DAC → beépített → DAC)
        F f(ms(1500));
        f.opened("realtek");
        bool any = false;
        any |= f.observe("dac", t0);
        any |= f.observe("realtek", t0 + ms(1000));   // vissza: a várakozás törlődik
        any |= f.observe("dac", t0 + ms(2000));       // újra: innen számít
        any |= f.observe("dac", t0 + ms(3000));
        CHECK(!any, "villogó váltás közben nem nyit újra");
        CHECK(f.observe("dac", t0 + ms(3500)), "a villogás után 1,5 s-mal vált (az utolsó változástól számítva)");
    }
    {
        F f(ms(1500));
        f.opened("realtek");
        bool any = f.observe("dac", t0);
        any |= f.observe("", t0 + ms(1000));           // pillanatnyilag nincs alapértelmezett eszköz
        any |= f.observe("dac", t0 + ms(2000));
        CHECK(!any && f.observe("dac", t0 + ms(3500)), "üres azonosító (nincs eszköz) nem vált ki váltást, a várakozás újraindul");
    }
    {
        F f(ms(1500));
        f.opened("realtek");
        CHECK(!f.observe("", t0) && !f.observe("", t0 + ms(5000)), "tartósan nincs alapértelmezett eszköz: nem nyit újra");
        bool any = f.observe("dac1", t0 + ms(6000));
        any |= f.observe("dac2", t0 + ms(7000));      // másik új eszköz: a várakozás újraindul
        any |= f.observe("dac2", t0 + ms(8000));
        CHECK(!any && f.observe("dac2", t0 + ms(8500)) && f.pending() == "dac2",
              "két új eszköz egymás után: az utolsótól számít, arra vált");
    }
    {
        F f(ms(1500));   // még semmin nincs nyitva (a megnyitás nem sikerült): a követés nem kér újranyitást
        CHECK(!f.observe("dac", t0) && !f.observe("dac", t0 + ms(5000)), "nyitott hangfolyam nélkül nincs váltás");
    }
}

template <class OptT = audioopt::AudioOptions>
static void testUpsampleOptionsK17() {
    std::printf("\n[K17] AudioOptions felmintavételezés beállítások és mentés (K17)\n");
    if constexpr (HasUpsampleOptions<OptT>) {
        // 1. Régi JSON betöltése (upsample kulcsok nélkül): alapértelmezett, kikapcsolt állapot
        json::Value vLegacy;
        json::parse("{\"buffer\":512,\"keepAwake\":true,\"rateMode\":\"follow\",\"fixedRate\":96000}", vLegacy);
        OptT oLegacy = audioopt::fromJson(vLegacy);
        CHECK(oLegacy.bufferFrames == 512 && oLegacy.keepAwake && oLegacy.fixedRate == 96000,
              "Régi JSON betöltése: meglévő mezők épek");
        CHECK(static_cast<int>(oLegacy.upsampleRule) == 0,
              "Régi JSON: felmintavételezés kikapcsolva (alapértelmezett)");

        // 2. Hibás értékek korrigálása (sanitize)
        OptT oBad;
        oBad.upsamplePhase = 75.0; // > 50 -> max 50
        oBad.upsamplePrecision = 17; // 17 -> legközelebbi (20)
        oBad.upsampleHeadroomDb = 5.0f; // > 0 -> 0 vagy -1
        oBad.upsamplePassbandEnd = 1.2;
        oBad.upsampleStopbandBegin = 1.0; // passband >= stopband -> hiba, korrigálva
        audioopt::sanitize(oBad);
        CHECK(oBad.upsamplePhase <= 50.0 && oBad.upsamplePrecision >= 20 &&
              oBad.upsamplePassbandEnd < oBad.upsampleStopbandBegin,
              "Érvénytelen értékek korrigálása: fázis <= 50, pontosság valid, passband < stopband");

        // 3. Oda-vissza mentés és egyezés
        OptT oGood;
        oGood.upsampleRule = static_cast<decltype(oGood.upsampleRule)>(1); // x2
        oGood.upsamplePreset = 1;
        oGood.upsamplePhase = 25.0;
        oGood.upsamplePrecision = 28;
        oGood.upsamplePassbandEnd = 0.95;
        oGood.upsampleStopbandBegin = 1.0;
        oGood.upsampleHeadroomDb = -1.0f;
        std::string j = audioopt::toJson(oGood);
        json::Value vGood;
        json::parse(j, vGood);
        OptT oRestored = audioopt::fromJson(vGood);
        CHECK(oGood == oRestored, "AudioOptions felmintavételezés oda-vissza mentés azonos");
    } else {
        CHECK(false, "K17: audioopt::AudioOptions felmintavételezés mezők nincsenek megvalósítva");
    }
}

int main() {
    std::printf("========================================================\n");
    std::printf(" Aqulify magteszt (Windows/Linux közös kód)\n");
    std::printf("========================================================\n");
    testSpectrum();
    testParamFile();
    testSeqlock();
    testTapRing();
    testJson();
    testDspState();
    testSelfPath();
    testNanGuard();
    testMdns();
    testPushPairing();
    testEngineQuit();
    testRepairReplacesDevice();
    testUpdate();
    testOpenUrl();
    testBluetoothLine();
    testPairCli();
    testBridgeLoudnessStereo();
    testMdnsBrowser();
    testRemoteRelay();
    testRemoteAdmin();
    testVersionCheck();
#ifndef _WIN32
    testMultiOutStorageImpl();
#endif
    testBridgeWayEqImpl();
    testBridgeMultiOutRemoteAuth();
    testUpsampleOptionsK17();
    testVisualSourceDsd();
    testBridgeEndlessAudio();
    testDefaultDeviceFollow();
    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
