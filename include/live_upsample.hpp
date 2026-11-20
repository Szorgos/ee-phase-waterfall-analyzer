#pragma once
/**
 * @file live_upsample.hpp
 * @brief Felmintavételezés az élő úton (Linux): ami más programból (pl. MPD) szól az Aqulify eszközre.
 *
 * Az élő út egyetlen PipeWire-gráf, egy mintavételen; a libsoxr itt nem tud a forrás és a DAC közé állni. Ezért
 * itt a gráf mintavétele emelkedik a célra (clock.force-rate), és a forrás hangfolyamát a PipeWire saját átalakítója
 * hozza fel. A szabály és a felső korlát ugyanaz, mint a beépített lejátszónál; a szűrőbeállítások (fázis,
 * sávszélesség, pontosság, fejtér) itt NEM érvényesek, ezt a felület kiírja.
 *
 * Ebben a fájlban csak a tiszta (rendszerhívás nélküli) részek vannak, hogy tesztelhetők legyenek: a pactl és a
 * /proc/asound szövegének értelmezése és a célmintavétel döntése.
 */
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "audio_options.hpp"
#include "upsample_engine.hpp"

namespace ee_analyzer {
namespace liveups {

/** Egy hangfolyam, amely egy nyelőre szól (pactl list sink-inputs). */
struct SinkInput {
    std::string id;
    std::string sinkId;
    std::string nodeName;
    std::string appName;
    uint32_t rate = 0;
    bool corked = false;
};

inline std::string trimLeft(const std::string& s) {
    const size_t f = s.find_first_not_of(" \t");
    return f == std::string::npos ? std::string() : s.substr(f);
}

/** Egy `kulcs = "érték"` tulajdonságsor értéke (üres, ha nem ez a kulcs). */
inline bool propValue(const std::string& line, const char* key, std::string& out) {
    const std::string k = std::string(key) + " = \"";
    if (line.rfind(k, 0) != 0) return false;
    const size_t b = k.size(), e = line.find('"', b);
    if (e == std::string::npos) return false;
    out = line.substr(b, e - b);
    return true;
}

/** „Sample Specification: s16le 2ch 44100Hz” → 44100 (0, ha nem értelmezhető). */
inline uint32_t rateFromSpec(const std::string& spec) {
    const size_t hz = spec.rfind("Hz");
    if (hz == std::string::npos) return 0;
    size_t b = hz;
    while (b > 0 && spec[b - 1] >= '0' && spec[b - 1] <= '9') --b;
    if (b == hz) return 0;
    const unsigned long v = std::strtoul(spec.substr(b, hz - b).c_str(), nullptr, 10);
    return (v >= 8000 && v <= 768000) ? static_cast<uint32_t>(v) : 0;
}

/** A `LC_ALL=C pactl list sink-inputs` kimenetének értelmezése. */
inline std::vector<SinkInput> parseSinkInputs(const std::string& txt) {
    std::vector<SinkInput> v;
    size_t pos = 0;
    while (pos < txt.size()) {
        size_t eol = txt.find('\n', pos);
        if (eol == std::string::npos) eol = txt.size();
        std::string line = trimLeft(txt.substr(pos, eol - pos));
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("Sink Input #", 0) == 0) {
            v.emplace_back();
            v.back().id = line.substr(12);
            continue;
        }
        if (v.empty()) continue;
        SinkInput& s = v.back();
        std::string val;
        if (line.rfind("Sink: ", 0) == 0) s.sinkId = line.substr(6);
        else if (line.rfind("Corked: ", 0) == 0) s.corked = line.substr(8) == "yes";
        else if (line.rfind("Sample Specification: ", 0) == 0) s.rate = rateFromSpec(line);
        else if (propValue(line, "node.name", val)) s.nodeName = val;
        else if (propValue(line, "application.name", val)) s.appName = val;
    }
    return v;
}

/** A `pactl list short sinks` kimenetéből a megadott nevű nyelő azonosítója (üres, ha nincs). */
inline std::string sinkIdByName(const std::string& shortTxt, const std::string& name) {
    size_t pos = 0;
    while (pos < shortTxt.size()) {
        size_t eol = shortTxt.find('\n', pos);
        if (eol == std::string::npos) eol = shortTxt.size();
        const std::string line = shortTxt.substr(pos, eol - pos);
        pos = eol + 1;
        const size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) t2 = line.size();
        if (line.substr(t1 + 1, t2 - t1 - 1) == name) return line.substr(0, t1);
    }
    return {};
}

/** Saját hangfolyam-e (lejátszó, ébren tartás, zónák): ezek nem forrásai az élő útnak. */
inline bool isOwnStream(const SinkInput& s) {
    return s.nodeName.rfind("aqulify", 0) == 0 || s.appName == "Aqulify";
}

/**
 * Az élő út forrásmintavétele: a saját nyelőnkre szóló, nem szüneteltetett külső hangfolyamok közül a legnagyobb
 * mintavétel (0: nincs ilyen). Több eltérő forrásnál a legnagyobb szerint dönt (abból lesz a legkevésbé rossz).
 */
inline uint32_t liveSourceRate(const std::vector<SinkInput>& inputs, const std::string& ownSinkId) {
    uint32_t best = 0;
    if (ownSinkId.empty()) return 0;
    for (const auto& s : inputs) {
        if (s.sinkId != ownSinkId || s.corked || isOwnStream(s)) continue;
        best = std::max(best, s.rate);
    }
    return best;
}

/** A `pactl list sinks` kimenetéből az adott azonosítójú nyelő ALSA-kártyaszáma (-1: nem ALSA vagy nincs). */
inline int alsaCardOfSink(const std::string& txt, const std::string& sinkId) {
    if (sinkId.empty()) return -1;
    bool in = false;
    size_t pos = 0;
    while (pos < txt.size()) {
        size_t eol = txt.find('\n', pos);
        if (eol == std::string::npos) eol = txt.size();
        std::string line = trimLeft(txt.substr(pos, eol - pos));
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("Sink #", 0) == 0) {
            in = line.substr(6) == sinkId;
            continue;
        }
        std::string val;
        if (in && (propValue(line, "alsa.card", val) || propValue(line, "api.alsa.card", val))) {
            if (val.empty() || !std::all_of(val.begin(), val.end(), [](char c) { return c >= '0' && c <= '9'; })) return -1;
            return std::atoi(val.c_str());
        }
    }
    return -1;
}

/** A szabványos PCM-mintavételek (a folytonos tartományú USB-s hangkártyáknál ezekből választunk). */
inline const std::vector<uint32_t>& standardRates() {
    static const std::vector<uint32_t> r{ 32000, 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000 };
    return r;
}

/**
 * Az USB-s hangkártya lejátszási mintavételei a /proc/asound/cardN/stream0 szövegéből (csak a „Playback:” rész).
 * „Rates: 32000, 44100, 48000” vagy „Rates: 8000 - 192000 (continuous)”. Üres: nem USB-s kártya vagy nincs adat.
 */
inline std::vector<uint32_t> parseStream0PlaybackRates(const std::string& txt) {
    std::vector<uint32_t> out;
    bool playback = false;
    size_t pos = 0;
    while (pos < txt.size()) {
        size_t eol = txt.find('\n', pos);
        if (eol == std::string::npos) eol = txt.size();
        std::string line = trimLeft(txt.substr(pos, eol - pos));
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("Playback:", 0) == 0) { playback = true; continue; }
        if (line.rfind("Capture:", 0) == 0) { playback = false; continue; }
        if (!playback || line.rfind("Rates:", 0) != 0) continue;
        const std::string list = line.substr(6);
        if (list.find('-') != std::string::npos) {   // folytonos tartomány
            const size_t dash = list.find('-');
            const unsigned long lo = std::strtoul(list.substr(0, dash).c_str(), nullptr, 10);
            const unsigned long hi = std::strtoul(list.substr(dash + 1).c_str(), nullptr, 10);
            for (uint32_t r : standardRates())
                if (r >= lo && r <= hi) out.push_back(r);
            continue;
        }
        size_t p = 0;
        while (p < list.size()) {
            size_t c = list.find(',', p);
            if (c == std::string::npos) c = list.size();
            const unsigned long r = std::strtoul(list.substr(p, c - p).c_str(), nullptr, 10);
            if (r >= 8000 && r <= 768000) out.push_back(static_cast<uint32_t>(r));
            p = c + 1;
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

/** A döntés eredménye: a gráf kért mintavétele (0: nincs kényszer) és az ok (a felületnek). */
struct Decision {
    uint32_t forceRate = 0;
    uint32_t srcRate = 0;
    std::string reason;   // "" (aktív), "off", "rate_mode", "player", "no_source", "dac_unsupported", "source_rate"
};

/**
 * Célmintavétel az élő útra. Fix mintavételnél nincs kényszer (a felhasználó választása az erősebb); amíg a
 * beépített lejátszó szól, az dönt (a saját libsoxr-útjával). dacRates: a DAC tudott mintavételei (üres: ismeretlen,
 * ekkor a fallback lista számít, ugyanaz, amit a beépített lejátszó használ).
 */
inline Decision decide(uint32_t srcRate, const audioopt::AudioOptions& o, const std::vector<uint32_t>& dacRates,
                       const std::vector<uint32_t>& fallback, bool playerOpen) {
    Decision d;
    d.srcRate = srcRate;
    if (o.upsampleRule == audioopt::UpsampleRule::Off) { d.reason = "off"; return d; }
    if (o.rateMode == audioopt::RateMode::Fixed) { d.reason = "rate_mode"; return d; }
    if (playerOpen) { d.reason = "player"; return d; }
    if (srcRate == 0) { d.reason = "no_source"; return d; }
    std::string reason;
    const uint32_t t = UpsampleEngine::selectTargetRate(srcRate, o.upsampleRule, dacRates.empty() ? fallback : dacRates,
                                                        o.upsampleCustom, false, false, &reason, o.upsampleMaxRate);
    d.reason = reason;
    if (t > srcRate) d.forceRate = t;
    return d;
}

} // namespace liveups
} // namespace ee_analyzer
