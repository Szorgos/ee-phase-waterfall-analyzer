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
#include <string>

#include "mini_json.hpp"

namespace ee_analyzer::audioopt {

enum class RateMode : int {
    System = 0,   // a rendszer beállítása marad (nem nyúlunk hozzá)
    Follow = 1,   // a forrást követi: a gráf a lejátszott hang mintavételére vált, ha a hangkártya tudja
    Fixed = 2,    // rögzített mintavétel (minden ehhez igazodik)
};

/** A választható pufferméretek mintában (0: automatikus, a rendszer dönt). */
inline constexpr std::array<int, 8> kBufferChoices{ 0, 64, 128, 256, 512, 1024, 2048, 4096 };
/** A választható rögzített mintavételek. */
inline constexpr std::array<int, 6> kRateChoices{ 44100, 48000, 88200, 96000, 176400, 192000 };

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
    bool exclusive = false;      // Windows: kizárólagos mód (WASAPI exclusive; a DSP a motorban fut)

    bool operator==(const AudioOptions& o) const {
        return bufferFrames == o.bufferFrames && keepAwake == o.keepAwake && rateMode == o.rateMode &&
               fixedRate == o.fixedRate && resampleQuality == o.resampleQuality && replayGain == o.replayGain &&
               rgPreampDb == o.rgPreampDb && rgPreventClip == o.rgPreventClip && preload == o.preload &&
               exclusive == o.exclusive;
    }
    bool operator!=(const AudioOptions& o) const { return !(*this == o); }
};

/** Érvényes értékekre igazítás (ismeretlen pufferméret / mintavétel: a legközelebbi választható). */
inline void sanitize(AudioOptions& o) {
    auto nearest = [](int v, const auto& list, int skipZero) {
        int best = list[skipZero], bd = 1 << 30;
        for (size_t i = skipZero; i < list.size(); ++i) {
            const int d = std::abs(list[i] - v);
            if (d < bd) { bd = d; best = list[i]; }
        }
        return best;
    };
    if (o.bufferFrames != 0) o.bufferFrames = nearest(o.bufferFrames, kBufferChoices, 1);
    o.fixedRate = nearest(o.fixedRate, kRateChoices, 0);
    const int m = static_cast<int>(o.rateMode);
    if (m < 0 || m > 2) o.rateMode = RateMode::System;
    o.resampleQuality = std::clamp(o.resampleQuality, -1, 14);
    o.replayGain = std::clamp(o.replayGain, 0, 2);
    o.rgPreampDb = (o.rgPreampDb == o.rgPreampDb) ? std::clamp(std::round(o.rgPreampDb * 2.0f) / 2.0f, -15.0f, 15.0f) : 0.0f;
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
    return "{\"buffer\":" + std::to_string(o.bufferFrames) + ",\"keepAwake\":" + (o.keepAwake ? "true" : "false") +
           ",\"rateMode\":\"" + rateModeName(o.rateMode) + "\",\"fixedRate\":" + std::to_string(o.fixedRate) +
           ",\"resampleQuality\":" + std::to_string(o.resampleQuality) +
           ",\"replayGain\":\"" + (o.replayGain == 2 ? "album" : o.replayGain == 1 ? "track" : "off") +
           "\",\"rgPreamp\":" + std::to_string(o.rgPreampDb) + ",\"rgClip\":" + (o.rgPreventClip ? "true" : "false") +
           ",\"preload\":" + (o.preload ? "true" : "false") + ",\"exclusive\":" + (o.exclusive ? "true" : "false") + "}";
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
    sanitize(base);
    return base;
}

/** Az újramintavételezés minősége a hangfolyamokhoz (a lejátszó és a további kimenetek olvassák; -1: alapértelmezés). */
inline std::atomic<int>& resampleQuality() {
    static std::atomic<int> q{ -1 };
    return q;
}

} // namespace ee_analyzer::audioopt
