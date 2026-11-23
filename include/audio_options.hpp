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
#include "net_receiver.hpp"

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

/** A felmintavételezés motorja (0 = libsoxr, 1 = saját 64 bites FFT + Kaiser-sinc, 2 = saját 106 bites Double-Double FFT + Kaiser-sinc). */
enum class UpsampleEngineType : int {
    Soxr = 0,
    Fft64 = 1,
    FftDD = 2,
};

inline const char* upsampleEngineLabel(int e) {
    return e == 2 ? "Aqulify FFT-DD" : e == 1 ? "Aqulify FFT64" : "libsoxr";
}

inline const char* upsampleConverterCode(int e) {
    return e == 2 ? "fftdd" : e == 1 ? "fft64" : "libsoxr";
}

/** A felmintavételezés előbeállításai (3.3). */
enum class UpsamplePreset : int {
    Linear = 0,        // Lineáris fázis (50)
    Intermediate = 1,  // Köztes (ajánlott, 25)
    Minimum = 2,       // Minimumfázis (0)
    Custom = 3,        // Egyéni beállítás
    Reference = 4      // Referencia: 99,7% sávszélesség, 32 bites pontosság, lineáris fázis (a legtöbb számítás)
};
inline constexpr int kUpsamplePresetReference = 4;
/** A sávszélesség (passband) felső határa: a Referencia előbeállítás értéke. */
inline constexpr double kUpsamplePassbandMax = 0.997;

/** A választható pufferméretek mintában (0: automatikus, a rendszer dönt). */
inline constexpr std::array<int, 8> kBufferChoices{ 0, 64, 128, 256, 512, 1024, 2048, 4096 };
/** A választható rögzített mintavételek. */
inline constexpr std::array<int, 10> kRateChoices{ 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000 };
/** A DSP-lánc legnagyobb mintavétele: fölötte a lánc bitpontosan átenged (az LSP-modulok erre vannak méretezve). */
inline constexpr uint32_t kMaxDspRate = 384000;
/** A felmintavételezés felső korlátjának választható értékei (0: nincs korlát). */
inline constexpr std::array<int, 7> kUpsampleMaxRateChoices{ 0, 88200, 96000, 176400, 192000, 352800, 384000 };
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

    // Felmintavételezés a beépített lejátszóban és opcionálisan a rendszerhangon (libsoxr, 64 bites FFT64 vagy 106 bites FFT-DD)
    UpsampleRule upsampleRule = UpsampleRule::Off;
    int upsampleEngine = 0;                     // 0: libsoxr (alapértelmezett), 1: Aqulify FFT64 (64 bit), 2: Aqulify FFT-DD (106 bit)
    // Lefelé alakítás a beépített lejátszóban, ha a kimenet a forrás mintavételét nem tudja (pl. 192 kHz-es fájl 96 kHz-es
    // DAC-on): -1: a rendszer keverője (alapértelmezett, az eddigi viselkedés), 0–2: a választott motor (mint upsampleEngine)
    int downsampleEngine = -1;
    bool upsampleSystemAudio = false;           // false: csak beépített lejátszó (alap); true: külső rendszerhang is
    std::string systemCaptureDevice;            // Windows virtuális kábel / választható forráseszköz (üres: automatikus)
    int upsamplePreset = 1;                     // 0: linear, 1: intermediate, 2: minimum, 3: custom, 4: reference
    double upsamplePassbandEnd = 0.95;          // 0.80 .. 0.997 (95%)
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
    // Névtelen hibajelentés (beépített hibajelentés): alapból KI. Kikapcsolva a helyi jelentés csak a következő rendes
    // indításig marad meg; bekapcsolva a motor elküldi a rendellenes előző futás jelentését. Hozzájárulás, ezért az
    // indításkori visszaállítástól függetlenül él.
    bool errorReports = false;
    bool errorReportsAsked = false;   // az első indításkori kérdésre már válaszolt-e a felhasználó
    // Hálózati vevő (net_receiver.hpp): alapból KI. Hálózati szolgáltatás, ezért (a hibajelentéshez hasonlóan) az
    // indításkori visszaállítástól függetlenül él. A név üresen az alapnév ("Aqulify").
    bool netRecv = false;
    std::string netRecvName;
    // UPnP / DLNA lejátszó (upnp_bridge.hpp): alapból KI; szolgáltatás, mindig visszaáll (mint a hálózati vevő).
    // A név üresen „Aqulify (gépnév)”. Fix hangerő: a vezérlő hangereje nem hat (bitpontos). upnpAllow: ha nem üres,
    // csak ezekről az IPv4-címekről fogad vezérlést (vesszővel elválasztva).
    bool upnp = false;
    std::string upnpName;
    bool upnpFixedVolume = false;
    std::string upnpAllow;

    bool operator==(const AudioOptions& o) const {
        return bufferFrames == o.bufferFrames && keepAwake == o.keepAwake && rateMode == o.rateMode &&
               fixedRate == o.fixedRate && resampleQuality == o.resampleQuality && replayGain == o.replayGain &&
               rgPreampDb == o.rgPreampDb && rgPreventClip == o.rgPreventClip && preload == o.preload &&
               exclusive == o.exclusive && exclHwVolume == o.exclHwVolume &&
               upsampleRule == o.upsampleRule && upsampleEngine == o.upsampleEngine && downsampleEngine == o.downsampleEngine &&
               upsampleSystemAudio == o.upsampleSystemAudio && systemCaptureDevice == o.systemCaptureDevice &&
               upsamplePreset == o.upsamplePreset &&
               std::abs(upsamplePassbandEnd - o.upsamplePassbandEnd) < 1e-4 &&
               std::abs(upsampleStopbandBegin - o.upsampleStopbandBegin) < 1e-4 &&
               upsamplePrecision == o.upsamplePrecision &&
               std::abs(upsamplePhase - o.upsamplePhase) < 1e-4 &&
               std::abs(upsampleHeadroomDb - o.upsampleHeadroomDb) < 1e-3f &&
               upsampleCustom == o.upsampleCustom && upsampleThreads == o.upsampleThreads &&
               upsampleMaxRate == o.upsampleMaxRate && dedicatedCore == o.dedicatedCore && loadAdvisor == o.loadAdvisor &&
               asio == o.asio && asioDriver == o.asioDriver && errorReports == o.errorReports &&
               errorReportsAsked == o.errorReportsAsked && netRecv == o.netRecv && netRecvName == o.netRecvName &&
               upnp == o.upnp && upnpName == o.upnpName && upnpFixedVolume == o.upnpFixedVolume && upnpAllow == o.upnpAllow;
    }
    bool operator!=(const AudioOptions& o) const { return !(*this == o); }
};

/**
 * Egy előbeállítás szűrőértékeinek beírása (a felület gombjai ugyanezt teszik, a telefon és a parancssori átalakító ezt
 * hívja). A fejtér a felhasználóé marad. Az Egyéni (3) nem változtat semmit.
 */
inline void applyUpsamplePreset(AudioOptions& o, int preset) {
    if (preset < 0 || preset > kUpsamplePresetReference || preset == 3) return;
    o.upsamplePreset = preset;
    o.upsampleStopbandBegin = 1.00;
    if (preset == kUpsamplePresetReference) {
        o.upsamplePassbandEnd = kUpsamplePassbandMax;
        o.upsamplePrecision = 32;
        o.upsamplePhase = 50.0;
        return;
    }
    o.upsamplePassbandEnd = 0.95;
    o.upsamplePrecision = 28;
    o.upsamplePhase = preset == 0 ? 50.0 : preset == 2 ? 0.0 : 25.0;
}

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
    if (o.upsampleEngine < 0 || o.upsampleEngine > 2) o.upsampleEngine = 0;
    if (o.downsampleEngine < -1 || o.downsampleEngine > 2) o.downsampleEngine = -1;
    if (o.systemCaptureDevice.size() > 255) o.systemCaptureDevice.clear();
    for (char c : o.systemCaptureDevice) {
        if (static_cast<unsigned char>(c) < 0x20) { o.systemCaptureDevice.clear(); break; }
    }
    o.upsamplePreset = std::clamp(o.upsamplePreset, 0, kUpsamplePresetReference);
    o.upsamplePhase = std::clamp(o.upsamplePhase, 0.0, 50.0);
    o.upsamplePrecision = nearestInt(o.upsamplePrecision, kPrecisionChoices);
    o.upsampleHeadroomDb = nearestFloat(o.upsampleHeadroomDb, kHeadroomChoices);

    o.upsamplePassbandEnd = std::clamp(o.upsamplePassbandEnd, 0.80, kUpsamplePassbandMax);
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
    o.netRecvName = aq::netrecv::sanitizeName(o.netRecvName);
    // UPnP: a név vezérlőkarakter nélkül, legfeljebb 64 bájt (UTF-8 határon); a címlista csak számjegy, pont, elválasztó
    {
        std::string n;
        for (unsigned char c : o.upnpName)
            if (c >= 0x20 && c != 0x7F) n += static_cast<char>(c);
        if (n.size() > 64) {
            size_t cut = 64;
            while (cut > 0 && (static_cast<unsigned char>(n[cut]) & 0xC0) == 0x80) --cut;
            n.resize(cut);
        }
        const size_t b = n.find_first_not_of(' ');
        o.upnpName = b == std::string::npos ? std::string() : n.substr(b, n.find_last_not_of(' ') - b + 1);
        std::string a;
        for (char c : o.upnpAllow)
            if ((c >= '0' && c <= '9') || c == '.' || c == ',' || c == ' ') a += c;
        o.upnpAllow = a.substr(0, 512);
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
           ",\"upsampleEngine\":" + std::to_string(o.upsampleEngine) +
           ",\"downsampleEngine\":" + std::to_string(o.downsampleEngine) +
           ",\"upsampleSystemAudio\":" + (o.upsampleSystemAudio ? "true" : "false") +
           ",\"systemCaptureDevice\":\"" + jsonStringBody(o.systemCaptureDevice) + "\"" +
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
         ",\"asioDriver\":\"" + jsonStringBody(o.asioDriver) + "\"" +
         ",\"errorReports\":" + (o.errorReports ? "true" : "false") +
         ",\"errorReportsAsked\":" + (o.errorReportsAsked ? "true" : "false") +
         ",\"netRecv\":" + (o.netRecv ? "true" : "false") +
         ",\"netRecvName\":\"" + jsonStringBody(o.netRecvName) + "\"" +
         ",\"upnp\":" + (o.upnp ? "true" : "false") +
         ",\"upnpName\":\"" + jsonStringBody(o.upnpName) + "\"" +
         ",\"upnpFixedVolume\":" + (o.upnpFixedVolume ? "true" : "false") +
         ",\"upnpAllow\":\"" + jsonStringBody(o.upnpAllow) + "\"}";
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
    if (const json::Value* x = v.get("upsampleEngine"); x && x->type == T::Number)
        base.upsampleEngine = static_cast<int>(x->num);
    if (const json::Value* x = v.get("downsampleEngine"); x && x->type == T::Number)
        base.downsampleEngine = static_cast<int>(x->num);
    if (const json::Value* x = v.get("upsampleSystemAudio"); x && x->type == T::Bool)
        base.upsampleSystemAudio = x->b;
    if (const json::Value* x = v.get("systemCaptureDevice"); x && x->type == T::String)
        base.systemCaptureDevice = x->str;
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
    if (const json::Value* x = v.get("errorReports"); x && x->type == T::Bool) base.errorReports = x->b;
    if (const json::Value* x = v.get("errorReportsAsked"); x && x->type == T::Bool) base.errorReportsAsked = x->b;
    if (const json::Value* x = v.get("netRecv"); x && x->type == T::Bool) base.netRecv = x->b;
    if (const json::Value* x = v.get("netRecvName"); x && x->type == T::String) base.netRecvName = x->str.substr(0, 256);
    if (const json::Value* x = v.get("upnp"); x && x->type == T::Bool) base.upnp = x->b;
    if (const json::Value* x = v.get("upnpName"); x && x->type == T::String) base.upnpName = x->str.substr(0, 256);
    if (const json::Value* x = v.get("upnpFixedVolume"); x && x->type == T::Bool) base.upnpFixedVolume = x->b;
    if (const json::Value* x = v.get("upnpAllow"); x && x->type == T::String) base.upnpAllow = x->str.substr(0, 1024);

    sanitize(base);
    return base;
}

/** Az újramintavételezés minősége a hangfolyamokhoz (a lejátszó és a további kimenetek olvassák; -1: alapértelmezés). */
inline std::atomic<int>& resampleQuality() {
    static std::atomic<int> q{ -1 };
    return q;
}

} // namespace ee_analyzer::audioopt
