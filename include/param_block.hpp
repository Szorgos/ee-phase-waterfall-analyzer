#pragma once
/**
 * @file param_block.hpp
 * @brief Zármentes paraméterblokk (seqlock) a GUI/híd szál és a valós idejű audio szál között.
 *
 * Kizárólag POD típusokat és címfüggetlen (lock-free) atomikat tartalmaz, ezért
 * folyamatok közötti közös memóriában (Windows: audiodg.exe <-> Aqulify.exe)
 * is biztonságosan használható.
 *
 * A modulok paraméterei az EasyEffects beállításaival egyeznek (ugyanaz a jelentés, mértékegység,
 * tartomány és alapérték), mert ugyanazokat a DSP könyvtárakat hajtják meg:
 *   EQ: LSP Parametric Equalizer x32, Limiter: LSP Limiter Stereo, Bass Enhancer: Calf,
 *   Crossfeed: libbs2b, Convolver: zita-convolver.
 * A Magic Touch (automatikus hangszépítő, magic_touch.hpp) az Aqulify saját modulja.
 */
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ee_analyzer {

constexpr uint32_t kMaxEqBands = 32;

/** A params.bin fájl és a közös memória paraméter-formátumának verziója. */
constexpr uint32_t kParamLayoutVersion = 8;

enum ParamFlags : uint32_t {
    kFlagEq        = 1u << 0,
    kFlagCrossfeed = 1u << 1,
    kFlagConvolver = 1u << 2,
    kFlagLimiter   = 1u << 3,
    kFlagBass      = 1u << 4,
    kFlagMagic     = 1u << 5,   // Magic Touch
    kFlagMono      = 1u << 6,   // Mono és mélyvágás (mono_bass.hpp)
    kFlagInitialized = 1u << 31, // a GUI legalább egyszer beállította
};

/** EasyEffects / LSP sávtípus ("bandTypeLabels") */
enum class EqBandType : int32_t {
    Off = 0, Bell = 1, HiPass = 2, HiShelf = 3, LoPass = 4, LoShelf = 5, Notch = 6,
    Resonance = 7, AllPass = 8, BandPass = 9, LadderPass = 10, LadderRej = 11,
};
constexpr int32_t kEqBandTypeCount = 12;

/** EasyEffects / LSP sáv szűrőmód ("bandModeLabels") */
enum class EqFilterMode : int32_t {
    RlcBt = 0, RlcMt = 1, BwcBt = 2, BwcMt = 3, LrxBt = 4, LrxMt = 5, ApoDr = 6,
};
constexpr int32_t kEqFilterModeCount = 7;

/** EasyEffects / LSP EQ üzemmód ("modeLabels": IIR, FIR, FFT, SPM) */
enum class EqMode : int32_t { Iir = 0, Fir = 1, Fft = 2, Spm = 3 };

struct EqBandPod {
    int32_t type = 1;        // EqBandType
    int32_t mode = 0;        // EqFilterMode
    int32_t slope = 0;       // 0..3 = x1..x4
    float freq = 1000.0f;    // Hz (10 .. 24000)
    float gainDb = 0.0f;     // dB (-36 .. 36)
    float q = 4.36f;         // 0 .. 100
    float width = 4.0f;      // oktáv (0 .. 12): sávszélesség a sáváteresztő/létra típusoknál
    int32_t reserved = 0;
};

/** A teljes DSP beállítás egyetlen másolható pillanatképe. */
struct ParamData {
    float preampDb = 0.0f;       // az EQ bemeneti erősítése (EasyEffects: Equalizer "inputGain")
    uint32_t flags = 0;          // ParamFlags
    int32_t crossfeedPreset = 0; // csak a felületnek: 0 = Bauer bs2b, 1 = Jan Meier, 2 = közeltéri stúdió, 3 = egyedi
    uint32_t numBands = 0;
    EqBandPod bands[kMaxEqBands];

    // --- Equalizer (LSP para_equalizer) ---
    int32_t eqMode = 0;          // EqMode
    int32_t eqDecramp = 3;       // 0 = Off, 1 = x2, 2 = x3, 3 = x4, 4 = x6, 5 = x8 (44,1 kHz-hez viszonyítva)

    // --- Crossfeed (libbs2b) ---
    float crossfeedFcut = 700.0f;  // Hz (300 .. 2000)
    float crossfeedFeedDb = 4.5f;  // dB (1 .. 15)

    // --- Limiter (LSP limiter_stereo) ---
    int32_t limMode = 0;           // 0..11: Herm/Exp/Line × Thin/Wide/Tail/Duck
    int32_t limOversampling = 22;  // 0..22: None, Half x2/16 bit ... Full x8/24 bit, True Peak/16 bit, True Peak/24 bit
    int32_t limDither = 0;         // 0..8: None, 7, 8, 11, 12, 15, 16, 23, 24 bit
    float limThresholdDb = -1.0f;  // -48 .. 0
    float limLookaheadMs = 5.0f;   // 0,1 .. 20
    float limAttackMs = 5.0f;      // 0,25 .. 20
    float limReleaseMs = 5.0f;     // 0,25 .. 20
    int32_t limGainBoost = 0;
    float limStereoLink = 100.0f;  // %
    int32_t limAlr = 0;
    float limAlrAttackMs = 5.0f;   // 0,1 .. 200
    float limAlrReleaseMs = 50.0f; // 10 .. 1000
    float limAlrKneeDb = 0.0f;     // -12 .. 12
    float limAlrKneeSmoothDb = -5.0f; // -48 .. 0
    float limInputGainDb = 0.0f;
    float limOutputGainDb = 0.0f;

    // --- Bass Enhancer (Calf) ---
    float bassAmountDb = 0.0f;     // -100 .. 36 (lineáris "amount" port dB-ben)
    float bassHarmonics = 8.5f;    // 0,1 .. 10 ("drive")
    float bassScopeHz = 100.0f;    // 10 .. 250 ("freq")
    float bassFloorHz = 20.0f;     // 10 .. 120
    float bassBlend = 0.0f;        // -10 .. 10
    int32_t bassFloorActive = 0;
    int32_t bassListen = 0;
    float bassInputGainDb = 0.0f;
    float bassOutputGainDb = 0.0f;

    // --- Convolver (zita-convolver) ---
    int32_t convIrWidth = 100;     // % (0 .. 200)
    int32_t convAutogain = 1;
    float convDryDb = -100.0f;     // -100 (= ki) .. 20
    float convWetDb = 0.0f;        // -100 (= ki) .. 20
    float convInputGainDb = 0.0f;
    float convOutputGainDb = 0.0f;

    // --- Magic Touch (az Aqulify saját modulja, docs/plans/magic-touch.md) ---
    float magicAmount = 50.0f;     // % (0 .. 200): az intenzitás (100 % fölött „Bátor” tartomány)
    int32_t magicCompare = 0;      // A/B: 1 = az eredeti (feldolgozatlan) jel szól, hangerőre kiegyenlítve
    int32_t magicCharacter = 0;    // Karakter: 0 Természetes, 1 Meleg, 2 Fényes, 3 Tágas
    int32_t magicContent = 0;      // Tartalom: 0 Automatikus, 1 Zene, 2 Beszéd, 3 Archív felvétel

    // --- Mono és mélyvágás (mono_bass.hpp, a v7-ben került a végére) ---
    int32_t subsonicHz = 0;        // 0 = ki, 10 / 20 / 30 Hz: 24 dB/oktáves felüláteresztő a teljes jelen
    float monoAmount = 0.0f;       // % (0 .. 100): az oldalsó jel 100 Hz alatti részének mono-keverése (0 = ki)

    bool has(uint32_t f) const { return (flags & f) != 0; }
    void set(uint32_t f, bool on) { flags = on ? (flags | f) : (flags & ~f); }
};

static_assert(std::atomic<uint32_t>::is_always_lock_free, "lock-free atomics kellenek");

/**
 * Egyíró / többolvasó seqlock. Az író (GUI híd) sosem várakozik sokáig,
 * az olvasó (audio szál) sosem blokkol: ha éppen írás zajlik, a régi beállítással megy tovább.
 */
struct ParamBlock {
    std::atomic<uint32_t> seq{0};
    ParamData data;

    void write(const ParamData& d) {
        const uint32_t s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_relaxed); // páratlan: írás folyamatban
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(static_cast<void*>(&data), &d, sizeof(ParamData));
        std::atomic_thread_fence(std::memory_order_release);
        seq.store(s + 2, std::memory_order_release); // páros: kész
    }

    /** @return true, ha konzisztens pillanatképet sikerült olvasni */
    bool tryRead(ParamData& out, uint32_t& outSeq) const {
        const uint32_t s1 = seq.load(std::memory_order_acquire);
        if (s1 & 1u) return false;
        std::memcpy(&out, static_cast<const void*>(&data), sizeof(ParamData));
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t s2 = seq.load(std::memory_order_relaxed);
        if (s1 != s2) return false;
        outSeq = s1;
        return true;
    }

    uint32_t currentSeq() const { return seq.load(std::memory_order_acquire); }
};

/** Átlátszó (bit-pontosan érintetlen) alapállapot: minden modul kikapcsolva. */
inline ParamData transparentParams() {
    ParamData d{};
    d.preampDb = 0.0f;
    d.flags = 0;
    d.numBands = 0;
    return d;
}

/** Értékhatárok (EasyEffects beállítási tartományai); a hídból és a fájlból érkező adatra is. */
inline void sanitizeParams(ParamData& d) {
    auto cl = [](float& v, float lo, float hi, float def) { v = (v == v) ? std::clamp(v, lo, hi) : def; };
    auto ci = [](int32_t& v, int32_t lo, int32_t hi) { v = std::clamp(v, lo, hi); };
    cl(d.preampDb, -36.0f, 36.0f, 0.0f);
    if (d.numBands > kMaxEqBands) d.numBands = kMaxEqBands;
    for (uint32_t i = 0; i < kMaxEqBands; ++i) {
        EqBandPod& b = d.bands[i];
        ci(b.type, 0, kEqBandTypeCount - 1);
        ci(b.mode, 0, kEqFilterModeCount - 1);
        ci(b.slope, 0, 3);
        cl(b.freq, 10.0f, 24000.0f, 1000.0f);
        cl(b.gainDb, -36.0f, 36.0f, 0.0f);
        cl(b.q, 0.0f, 100.0f, 4.36f);
        cl(b.width, 0.0f, 12.0f, 4.0f);
    }
    ci(d.eqMode, 0, 3);
    ci(d.eqDecramp, 0, 5);
    ci(d.crossfeedPreset, 0, 3);
    cl(d.crossfeedFcut, 300.0f, 2000.0f, 700.0f);
    cl(d.crossfeedFeedDb, 1.0f, 15.0f, 4.5f);
    ci(d.limMode, 0, 11);
    ci(d.limOversampling, 0, 22);
    ci(d.limDither, 0, 8);
    cl(d.limThresholdDb, -48.0f, 0.0f, -1.0f);
    cl(d.limLookaheadMs, 0.1f, 20.0f, 5.0f);
    cl(d.limAttackMs, 0.25f, 20.0f, 5.0f);
    cl(d.limReleaseMs, 0.25f, 20.0f, 5.0f);
    cl(d.limStereoLink, 0.0f, 100.0f, 100.0f);
    cl(d.limAlrAttackMs, 0.1f, 200.0f, 5.0f);
    cl(d.limAlrReleaseMs, 10.0f, 1000.0f, 50.0f);
    cl(d.limAlrKneeDb, -12.0f, 12.0f, 0.0f);
    cl(d.limAlrKneeSmoothDb, -48.0f, 0.0f, -5.0f);
    cl(d.limInputGainDb, -36.0f, 36.0f, 0.0f);
    cl(d.limOutputGainDb, -36.0f, 36.0f, 0.0f);
    cl(d.bassAmountDb, -100.0f, 36.0f, 0.0f);
    cl(d.bassHarmonics, 0.1f, 10.0f, 8.5f);
    cl(d.bassScopeHz, 10.0f, 250.0f, 100.0f);
    cl(d.bassFloorHz, 10.0f, 120.0f, 20.0f);
    cl(d.bassBlend, -10.0f, 10.0f, 0.0f);
    cl(d.bassInputGainDb, -36.0f, 36.0f, 0.0f);
    cl(d.bassOutputGainDb, -36.0f, 36.0f, 0.0f);
    ci(d.convIrWidth, 0, 200);
    cl(d.convDryDb, -100.0f, 20.0f, -100.0f);
    cl(d.convWetDb, -100.0f, 20.0f, 0.0f);
    cl(d.convInputGainDb, -36.0f, 36.0f, 0.0f);
    cl(d.convOutputGainDb, -36.0f, 36.0f, 0.0f);
    cl(d.magicAmount, 0.0f, 200.0f, 50.0f);
    ci(d.magicCompare, 0, 1);
    ci(d.magicCharacter, 0, 3);
    ci(d.magicContent, 0, 3);
    if (d.subsonicHz != 10 && d.subsonicHz != 20 && d.subsonicHz != 30) d.subsonicHz = 0;
    cl(d.monoAmount, 0.0f, 100.0f, 0.0f);
}

/** A params.bin v4 mérete: a Magic Touch mezői előtti rész (v5-ben ezek a végére kerültek). */
constexpr size_t kParamSizeV4 = offsetof(ParamData, magicAmount);
/** A params.bin v5 mérete: a Karakter és a Tartalom választó előtti rész (v6-ban ezek a végére kerültek). */
constexpr size_t kParamSizeV5 = offsetof(ParamData, magicCharacter);
/** A params.bin v6 mérete: a mélyvágás (v7-ben) mezője előtti rész. */
constexpr size_t kParamSizeV6 = offsetof(ParamData, subsonicHz);
/** A params.bin v7 mérete: a mono-keverés (v8-ban) mezője előtti rész. */
constexpr size_t kParamSizeV7 = offsetof(ParamData, monoAmount);

// ------------------------------------------------------------------ régi (v2/v3) params.bin átvétele

namespace legacy {
struct EqBandPodV3 { int32_t type; float freq; float gainDb; float q; };
struct ParamDataV3 {
    float preampDb; uint32_t flags; int32_t crossfeedPreset; uint32_t numBands; EqBandPodV3 bands[kMaxEqBands];
    int32_t eqMode; float crossfeedCutoffHz; float crossfeedFeedDb; float limiterCeilingDb; float limiterReleaseMs;
    float bassCrossoverHz; float bassHarmonicsDb; int32_t bassType; float bassSubcutHz; float convGainDb;
    int32_t convPhaseMode; int32_t convMaxTaps;
    int32_t limiterOversample; float limiterLookaheadMs; float crossfeedDelayMs; int32_t eqDecramp;
};
} // namespace legacy

/**
 * Paraméterfájl tartalmának betöltése (a 12 bájtos fejléc után).
 * v8: közvetlen másolat. v7: a mono-keverés nélkül (alap: 0 %), v6: a mélyvágás nélkül (alap: ki), v5: ugyanaz a Karakter
 * és a Tartalom nélkül, v4: a Magic Touch mezői nélkül (ezek alapértéket kapnak). v2/v3: a régi saját DSP beállításai a
 * megfelelő EasyEffects paraméterekre: a régi RBJ biquad sávok "APO (DR)" szűrőmódot kapnak (ugyanaz a hangolás), a többi
 * az alapértéket.
 */
inline bool loadParamBytes(const char* bytes, size_t len, uint32_t version, uint32_t size, ParamData& out) {
    if (size > len) return false;
    if (version == kParamLayoutVersion || version == 7 || version == 6 || version == 5 || version == 4) {
        const size_t want = version == kParamLayoutVersion ? sizeof(ParamData) : version == 7 ? kParamSizeV7
                          : version == 6 ? kParamSizeV6 : version == 5 ? kParamSizeV5 : kParamSizeV4;
        if (size != want) return false;
        ParamData d{};
        std::memcpy(static_cast<void*>(&d), bytes, want);
        d.magicCompare = 0;   // az A/B összehasonlítás nem marad bekapcsolva újraindítás után
        sanitizeParams(d);
        out = d;
        return true;
    }
    if (version < 2 || version > 3) return false;
    if (size < offsetof(legacy::ParamDataV3, eqMode) + sizeof(int32_t) || size > sizeof(legacy::ParamDataV3)) return false;
    legacy::ParamDataV3 o{};
    o.crossfeedCutoffHz = 700.0f; o.crossfeedFeedDb = 4.5f; o.limiterCeilingDb = -1.0f;
    o.limiterLookaheadMs = 5.0f; o.eqDecramp = 4;
    std::memcpy(&o, bytes, size);
    ParamData d{};
    d.preampDb = o.preampDb;
    d.flags = o.flags & (kFlagEq | kFlagCrossfeed | kFlagConvolver | kFlagLimiter | kFlagBass | kFlagInitialized);
    d.numBands = std::min(o.numBands, kMaxEqBands);
    static const int32_t typeMap[5] = { 1 /*Peaking->Bell*/, 5 /*LowShelf*/, 3 /*HighShelf*/, 4 /*LowPass*/, 2 /*HighPass*/ };
    for (uint32_t i = 0; i < d.numBands; ++i) {
        const legacy::EqBandPodV3& b = o.bands[i];
        EqBandPod& n = d.bands[i];
        n.type = typeMap[std::clamp(b.type, 0, 4)];
        n.mode = static_cast<int32_t>(EqFilterMode::ApoDr);
        n.slope = 0;
        n.freq = b.freq;
        n.gainDb = b.gainDb;
        n.q = b.q;
    }
    d.eqMode = o.eqMode == 0 ? 0 : 1;
    d.eqDecramp = o.eqDecramp >= 8 ? 5 : o.eqDecramp >= 4 ? 3 : o.eqDecramp >= 2 ? 1 : 0;
    d.crossfeedPreset = std::clamp(o.crossfeedPreset, 0, 2);
    d.crossfeedFcut = o.crossfeedCutoffHz;
    d.crossfeedFeedDb = o.crossfeedFeedDb;
    d.limThresholdDb = o.limiterCeilingDb;
    d.limLookaheadMs = o.limiterLookaheadMs;
    sanitizeParams(d);
    out = d;
    return true;
}

} // namespace ee_analyzer
