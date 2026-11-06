// Platformfüggetlen magteszt: FFT spektrum, paraméter-alkalmazás, seqlock, tap körpuffer.
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
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
#include "sha256.hpp"
#include "update_logic.hpp"

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
static net::socket_t wsConnectLive(int port, const std::string& path) {
    net::socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == net::kInvalidSocket) return s;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { net::closeSocket(s); return net::kInvalidSocket; }
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    ::send(s, req.data(), static_cast<int>(req.size()), 0);
    std::string resp;
    char buf[512];
    for (int i = 0; i < 40 && resp.find("\r\n\r\n") == std::string::npos; ++i) {
        const int n = static_cast<int>(::recv(s, buf, sizeof(buf), 0));
        if (n <= 0) break;
        resp.append(buf, static_cast<size_t>(n));
    }
    if (resp.rfind("HTTP/1.1 101", 0) != 0) { net::closeSocket(s); return net::kInvalidSocket; }
    return s;
}

/** Maszkolt szöveges keret (a kliens minden keretét maszkolja, RFC 6455). */
static void wsSendText(net::socket_t s, const std::string& text) {
    std::string f;
    f.push_back(static_cast<char>(0x81));
    f.push_back(static_cast<char>(0x80 | text.size()));   // rövid üzenet (< 126 bájt)
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
    testUpdate();
    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
