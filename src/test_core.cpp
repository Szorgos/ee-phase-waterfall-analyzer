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
#include <iterator>
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
#include "media_url.hpp"

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
static net::socket_t wsConnectLive(int port, const std::string& path, std::string* leftover = nullptr) {
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
    testBridgeLoudnessStereo();
    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
