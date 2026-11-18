#pragma once
/**
 * @file audio_options.hpp
 * @brief A hangkimenet haladó beállításai (a motor szintjén, nem a DSP-lánc része): pufferméret, a hangkártya ébren
 *        tartása, mintavétel, az újramintavételezés minősége. Platformfüggetlen adat + JSON; az alkalmazás
 *        platformonként történik (Linux: PipeWire, macOS: Core Audio). Windowson az Equalizer APO a Windows
 *        hangmotorjában fut, ott ezek nem állíthatók.
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <atomic>
#include <cstdint>
#include <map>
#include <string>

#include "mini_json.hpp"

namespace ee_analyzer::audioopt {

enum class RateMode : int {
    System = 0,   // a rendszer beállítása marad (nem nyúlunk hozzá)
    Follow = 1,   // a forrást követi: a gráf a lejátszott hang mintavételére vált, ha a hangkártya tudja
    Fixed = 2,    // rögzített mintavétel (minden ehhez igazodik)
};

/** A felmintavételezés szabályai (3.2). */
enum class UpsampleRule : int {
    Off = 0,         // Ki (nincs átalakítás, bitpontos)
    Double = 1,      // ×2 (a forrás kétszerese)
    PowerOfTwo = 2,  // Kettő hatványa (a kimenet legnagyobb 2^n többszöröse)
    MaxOutput = 3,   // A kimenet legnagyobb tudott mintavétele
    Custom = 4       // Egyéni táblázat
};

/** A felmintavételezés előbeállításai (3.3). */
enum class UpsamplePreset : int {
    Linear = 0,        // Lineáris fázis (50)
    Intermediate = 1,  // Köztes (ajánlott, 25)
    Minimum = 2,       // Minimumfázis (0)
    Custom = 3         // Egyéni beállítás
};

/** A választható pufferméretek mintában (0: automatikus, a rendszer dönt). */
inline constexpr std::array<int, 8> kBufferChoices{ 0, 64, 128, 256, 512, 1024, 2048, 4096 };
/** A választható rögzített mintavételek. */
inline constexpr std::array<int, 10> kRateChoices{ 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000 };
/** A DSP-lánc legnagyobb mintavétele: fölötte a lánc bitpontosan átenged (az LSP-modulok erre vannak méretezve). */
inline constexpr uint32_t kMaxDspRate = 384000;
/** A felmintavételezés felső korlátjának választható értékei (0: nincs korlát). */
inline constexpr std::array<int, 5> kUpsampleMaxRateChoices{ 0, 176400, 192000, 352800, 384000 };
/** A választható szűrőpontosságok (bit). */
inline constexpr std::array<int, 4> kPrecisionChoices{ 20, 24, 28, 32 };
/** A választható fejtér értékek (dB). */
inline constexpr std::array<float, 4> kHeadroomChoices{ 0.0f, -1.0f, -3.0f, -6.0f };

struct AudioOptions {
    int bufferFrames = 0;        // 0: automatikus
    bool keepAwake = false;      // a hangkártya csendben se aludjon el (pattanás, levágott számeleje ellen)
    RateMode rateMode = RateMode::System;
    int fixedRate = 48000;
    int resampleQuality = -1;    // -1: alapértelmezés (PipeWire: 4); 0 .. 14 (nagyobb: jobb, több processzoridő)
    // A beépített lejátszó
    int replayGain = 0;          // 0: ki (bitpontos), 1: szám, 2: album
    float rgPreampDb = 0.0f;     // -15 .. +15 dB
    bool rgPreventClip = true;   // a csúcs-címke szerint nem vezérel túl
    bool preload = false;        // a fájl lejátszás előtt a memóriába kerül
    bool exclusive = false;      // kizárólagos mód (Windows: WASAPI exclusive; Linux: közvetlen ALSA); a DSP a motorban fut
    bool exclHwVolume = false;   // Windows, kizárólagos módban: a DAC hardveres hangereje (false: digitális, a lánc végén)

    // Felmintavételezés a beépített lejátszóban (libsoxr)
    UpsampleRule upsampleRule = UpsampleRule::Off;
    int upsamplePreset = 1;                     // 0: linear, 1: intermediate, 2: minimum, 3: custom
    double upsamplePassbandEnd = 0.95;          // 0.80 .. 0.995 (95%)
    double upsampleStopbandBegin = 1.00;        // 1.00 .. 1.50 (100%)
    int upsamplePrecision = 28;                 // 20, 24, 28, 32 bit
    double upsamplePhase = 25.0;                // 0.0 .. 50.0 (25: köztes)
    float upsampleHeadroomDb = -1.0f;           // 0, -1, -3, -6 dB
    std::map<uint32_t, uint32_t> upsampleCustom; // Egyéni táblázat: forrás -> cél
    int upsampleThreads = 0;                    // 0: automatikus (ajánlott), 1: egy szál, 2: két szál (bal / jobb)
    uint32_t upsampleMaxRate = 0;               // a célmintavétel felső korlátja (0: nincs; a terhelés-tanácsadó állítja)
    int dedicatedCore = -1;                     // a dekóder-szál rögzítése egy magra (-1: ki; csak Linux / Windows)
    bool loadAdvisor = true;                    // a terhelés-tanácsadó szól-e
    // Windows: ASIO-kimenet a beépített lejátszóhoz (a gyártó ASIO-meghajtóján át; a DSP a motorban fut)
    bool asio = false;
    std::string asioDriver;                     // a meghajtó CLSID-je vagy neve (üres: az első telepített)

    bool operator==(const AudioOptions& o) const {
        return bufferFrames == o.bufferFrames && keepAwake == o.keepAwake && rateMode == o.rateMode &&
               fixedRate == o.fixedRate && resampleQuality == o.resampleQuality && replayGain == o.replayGain &&
               rgPreampDb == o.rgPreampDb && rgPreventClip == o.rgPreventClip && preload == o.preload &&
               exclusive == o.exclusive && exclHwVolume == o.exclHwVolume &&
               upsampleRule == o.upsampleRule && upsamplePreset == o.upsamplePreset &&
               std::abs(upsamplePassbandEnd - o.upsamplePassbandEnd) < 1e-4 &&
               std::abs(upsampleStopbandBegin - o.upsampleStopbandBegin) < 1e-4 &&
               upsamplePrecision == o.upsamplePrecision &&
               std::abs(upsamplePhase - o.upsamplePhase) < 1e-4 &&
               std::abs(upsampleHeadroomDb - o.upsampleHeadroomDb) < 1e-3f &&
               upsampleCustom == o.upsampleCustom && upsampleThreads == o.upsampleThreads &&
               upsampleMaxRate == o.upsampleMaxRate && dedicatedCore == o.dedicatedCore && loadAdvisor == o.loadAdvisor &&
               asio == o.asio && asioDriver == o.asioDriver;
    }
    bool operator!=(const AudioOptions& o) const { return !(*this == o); }
};

/** Érvényes értékekre igazítás (ismeretlen pufferméret / mintavétel: a legközelebbi választható). */
inline void sanitize(AudioOptions& o) {
    auto nearestInt = [](int v, const auto& list, int skipZero = 0) {
        int best = list[skipZero], bd = 1 << 30;
        for (size_t i = skipZero; i < list.size(); ++i) {
            const int d = std::abs(list[i] - v);
            if (d < bd) { bd = d; best = list[i]; }
        }
        return best;
    };
    auto nearestFloat = [](float v, const auto& list) {
        float best = list[0], bd = std::abs(list[0] - v);
        for (size_t i = 1; i < list.size(); ++i) {
            const float d = std::abs(list[i] - v);
            if (d < bd) { bd = d; best = list[i]; }
        }
        return best;
    };

    if (o.bufferFrames != 0) o.bufferFrames = nearestInt(o.bufferFrames, kBufferChoices, 1);
    o.fixedRate = nearestInt(o.fixedRate, kRateChoices, 0);
    const int m = static_cast<int>(o.rateMode);
    if (m < 0 || m > 2) o.rateMode = RateMode::System;
    o.resampleQuality = std::clamp(o.resampleQuality, -1, 14);
    o.replayGain = std::clamp(o.replayGain, 0, 2);
    o.rgPreampDb = (o.rgPreampDb == o.rgPreampDb) ? std::clamp(std::round(o.rgPreampDb * 2.0f) / 2.0f, -15.0f, 15.0f) : 0.0f;

    // Felmintavételezés beállításainak érvényesítése
    const int ur = static_cast<int>(o.upsampleRule);
    if (ur < 0 || ur > 4) o.upsampleRule = UpsampleRule::Off;
    o.upsamplePreset = std::clamp(o.upsamplePreset, 0, 3);
    o.upsamplePhase = std::clamp(o.upsamplePhase, 0.0, 50.0);
    o.upsamplePrecision = nearestInt(o.upsamplePrecision, kPrecisionChoices);
    o.upsampleHeadroomDb = nearestFloat(o.upsampleHeadroomDb, kHeadroomChoices);

    o.upsamplePassbandEnd = std::clamp(o.upsamplePassbandEnd, 0.80, 0.995);
    o.upsampleStopbandBegin = std::clamp(o.upsampleStopbandBegin, 1.00, 1.50);
    if (o.upsamplePassbandEnd >= o.upsampleStopbandBegin) {
        o.upsamplePassbandEnd = 0.95;
        o.upsampleStopbandBegin = 1.00;
    }
    if (o.upsampleThreads < 0 || o.upsampleThreads > 2) o.upsampleThreads = 0;
    if (std::find(kUpsampleMaxRateChoices.begin(), kUpsampleMaxRateChoices.end(), static_cast<int>(o.upsampleMaxRate)) ==
        kUpsampleMaxRateChoices.end())
        o.upsampleMaxRate = 0;
    if (o.dedicatedCore < -1 || o.dedicatedCore > 255) o.dedicatedCore = -1;
    // a meghajtó azonosítója: legfeljebb 255 bájt, vezérlőkarakter nélkül (registry-név vagy "{CLSID}")
    if (o.asioDriver.size() > 255) o.asioDriver.clear();
    for (char c : o.asioDriver) {
        if (static_cast<unsigned char>(c) < 0x20) { o.asioDriver.clear(); break; }
    }
}

/** Szöveg JSON-karakterlánc tartalmaként (idézőjel és visszaper escape; a vezérlőkaraktert a sanitize kiszűri). */
inline std::string jsonStringBody(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out;
}

inline const char* rateModeName(RateMode m) {
    switch (m) {
    case RateMode::Follow: return "follow";
    case RateMode::Fixed: return "fixed";
    default: return "system";
    }
}

inline RateMode rateModeFromName(const std::string& s) {
    if (s == "follow") return RateMode::Follow;
    if (s == "fixed") return RateMode::Fixed;
    return RateMode::System;
}

inline std::string toJson(const AudioOptions& o) {
    std::string s = "{\"buffer\":" + std::to_string(o.bufferFrames) + ",\"keepAwake\":" + (o.keepAwake ? "true" : "false") +
           ",\"rateMode\":\"" + rateModeName(o.rateMode) + "\",\"fixedRate\":" + std::to_string(o.fixedRate) +
           ",\"resampleQuality\":" + std::to_string(o.resampleQuality) +
           ",\"replayGain\":\"" + (o.replayGain == 2 ? "album" : o.replayGain == 1 ? "track" : "off") +
           "\",\"rgPreamp\":" + std::to_string(o.rgPreampDb) + ",\"rgClip\":" + (o.rgPreventClip ? "true" : "false") +
           ",\"preload\":" + (o.preload ? "true" : "false") + ",\"exclusive\":" + (o.exclusive ? "true" : "false") +
           ",\"exclHwVolume\":" + (o.exclHwVolume ? "true" : "false") +
           ",\"upsampleRule\":" + std::to_string(static_cast<int>(o.upsampleRule)) +
           ",\"upsamplePreset\":" + std::to_string(o.upsamplePreset) +
           ",\"upsamplePassbandEnd\":" + std::to_string(o.upsamplePassbandEnd) +
           ",\"upsampleStopbandBegin\":" + std::to_string(o.upsampleStopbandBegin) +
           ",\"upsamplePrecision\":" + std::to_string(o.upsamplePrecision) +
           ",\"upsamplePhase\":" + std::to_string(o.upsamplePhase) +
           ",\"upsampleHeadroomDb\":" + std::to_string(o.upsampleHeadroomDb) +
           ",\"upsampleCustom\":{";
    bool first = true;
    for (const auto& kv : o.upsampleCustom) {
        if (!first) s += ",";
        s += "\"" + std::to_string(kv.first) + "\":" + std::to_string(kv.second);
        first = false;
    }
    s += "},\"upsampleThreads\":" + std::to_string(o.upsampleThreads) +
         ",\"upsampleMaxRate\":" + std::to_string(o.upsampleMaxRate) +
         ",\"dedicatedCore\":" + std::to_string(o.dedicatedCore) +
         ",\"loadAdvisor\":" + (o.loadAdvisor ? "true" : "false") +
         ",\"asio\":" + (o.asio ? "true" : "false") +
         ",\"asioDriver\":\"" + jsonStringBody(o.asioDriver) + "\"}";
    return s;
}

/** A JSON-objektum mezői felülírják a megadott alapot (a hiányzó mező marad). */
inline AudioOptions fromJson(const json::Value& v, AudioOptions base = {}) {
    using T = json::Value::Type;
    if (const json::Value* x = v.get("buffer"); x && x->type == T::Number) base.bufferFrames = static_cast<int>(x->num);
    if (const json::Value* x = v.get("keepAwake"); x && x->type == T::Bool) base.keepAwake = x->b;
    if (const json::Value* x = v.get("rateMode"); x && x->type == T::String) base.rateMode = rateModeFromName(x->str);
    if (const json::Value* x = v.get("fixedRate"); x && x->type == T::Number) base.fixedRate = static_cast<int>(x->num);
    if (const json::Value* x = v.get("resampleQuality"); x && x->type == T::Number) base.resampleQuality = static_cast<int>(x->num);
    if (const json::Value* x = v.get("replayGain"); x && x->type == T::String)
        base.replayGain = x->str == "album" ? 2 : x->str == "track" ? 1 : 0;
    if (const json::Value* x = v.get("rgPreamp"); x && x->type == T::Number) base.rgPreampDb = static_cast<float>(x->num);
    if (const json::Value* x = v.get("rgClip"); x && x->type == T::Bool) base.rgPreventClip = x->b;
    if (const json::Value* x = v.get("preload"); x && x->type == T::Bool) base.preload = x->b;
    if (const json::Value* x = v.get("exclusive"); x && x->type == T::Bool) base.exclusive = x->b;
    if (const json::Value* x = v.get("exclHwVolume"); x && x->type == T::Bool) base.exclHwVolume = x->b;

    // Felmintavételezés mezők beolvasása (ha hiányoznak, a base alapértékei maradnak)
    if (const json::Value* x = v.get("upsampleRule"); x && x->type == T::Number)
        base.upsampleRule = static_cast<UpsampleRule>(static_cast<int>(x->num));
    if (const json::Value* x = v.get("upsamplePreset"); x && x->type == T::Number)
        base.upsamplePreset = static_cast<int>(x->num);
    if (const json::Value* x = v.get("upsamplePassbandEnd"); x && x->type == T::Number)
        base.upsamplePassbandEnd = x->num;
    if (const json::Value* x = v.get("upsampleStopbandBegin"); x && x->type == T::Number)
        base.upsampleStopbandBegin = x->num;
    if (const json::Value* x = v.get("upsamplePrecision"); x && x->type == T::Number)
        base.upsamplePrecision = static_cast<int>(x->num);
    if (const json::Value* x = v.get("upsamplePhase"); x && x->type == T::Number)
        base.upsamplePhase = x->num;
    if (const json::Value* x = v.get("upsampleHeadroomDb"); x && x->type == T::Number)
        base.upsampleHeadroomDb = static_cast<float>(x->num);
    if (const json::Value* x = v.get("upsampleCustom"); x && x->type == T::Object) {
        base.upsampleCustom.clear();
        for (const auto& kv : x->obj) {
            try {
                uint32_t src = static_cast<uint32_t>(std::stoul(kv.first));
                if (kv.second.type == T::Number) {
                    base.upsampleCustom[src] = static_cast<uint32_t>(kv.second.num);
                }
            } catch (...) {}
        }
    }
    if (const json::Value* x = v.get("upsampleThreads"); x && x->type == T::Number) base.upsampleThreads = static_cast<int>(x->num);
    if (const json::Value* x = v.get("upsampleMaxRate"); x && x->type == T::Number)
        base.upsampleMaxRate = x->num >= 0 && x->num < 1e7 ? static_cast<uint32_t>(x->num) : 0;
    if (const json::Value* x = v.get("dedicatedCore"); x && x->type == T::Number) base.dedicatedCore = static_cast<int>(x->num);
    if (const json::Value* x = v.get("loadAdvisor"); x && x->type == T::Bool) base.loadAdvisor = x->b;
    if (const json::Value* x = v.get("asio"); x && x->type == T::Bool) base.asio = x->b;
    if (const json::Value* x = v.get("asioDriver"); x && x->type == T::String) base.asioDriver = x->str;

    sanitize(base);
    return base;
}

/** Az újramintavételezés minősége a hangfolyamokhoz (a lejátszó és a további kimenetek olvassák; -1: alapértelmezés). */
inline std::atomic<int>& resampleQuality() {
    static std::atomic<int> q{ -1 };
    return q;
}

} // namespace ee_analyzer::audioopt
