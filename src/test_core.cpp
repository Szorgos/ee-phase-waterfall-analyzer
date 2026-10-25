// Platformfüggetlen magteszt: FFT spektrum, paraméter-alkalmazás, seqlock, tap körpuffer.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "spectrum.hpp"
#include "param_block.hpp"
#include "tap_ring.hpp"
#include "mini_json.hpp"
#include "dsp_state.hpp"
#include "self_path.hpp"

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
    const double far = lv[bandOf(100.0)] * 90.0 - 90.0;
    CHECK(far < -60.0, "100 Hz-en %.1f dBFS (szivárgás < -60 dB)", far);
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
    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
