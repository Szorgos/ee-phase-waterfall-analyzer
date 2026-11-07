#pragma once
/**
 * @file startup_restore.hpp
 * @brief Indításkori visszaállítás: mit állítson vissza a motor (lánc, több kimenet, hangkimenet), és kérdezzen-e
 *        minden indításkor. A beállítás a restore.json fájlban van (platformonként a konfigurációs mappában).
 *
 * Módok: "ask" – a felület kérdez (a motor addig az alapállapotban marad), "always" – a mentett részek
 * automatikusan visszaállnak, "never" – sosem. Az egyes részek (chain, multi, audio) külön kapcsolhatók.
 */
#include <string>

#include "mini_json.hpp"

namespace ee_analyzer::startup_restore {

enum class Mode { Ask, Always, Never };

struct Parts {
    bool chain = true;   // DSP-lánc és az impulzusválasz
    bool multi = true;   // több kimenet (2–4. kimenet, a zónák lánca és IR-je)
    bool audio = true;   // haladó hangkimenet (puffer, mintavétel, ébren tartás, újramintavételezés, ReplayGain)
};

struct Pref {
    Mode mode = Mode::Ask;
    Parts parts;
};

inline const char* modeName(Mode m) {
    switch (m) {
    case Mode::Always: return "always";
    case Mode::Never: return "never";
    default: return "ask";
    }
}

inline Mode modeFromName(const std::string& s) {
    if (s == "always") return Mode::Always;
    if (s == "never") return Mode::Never;
    return Mode::Ask;
}

inline bool anyPart(const Parts& p) { return p.chain || p.multi || p.audio; }

/** A restore.json szövege; hiba vagy hiányzó fájl esetén az alapértelmezés (kérdez, minden rész). */
inline Pref parsePref(const std::string& doc) {
    Pref p;
    json::Value v;
    if (doc.empty() || !json::parse(doc, v) || v.type != json::Value::Type::Object) return p;
    if (const json::Value* m = v.get("mode"); m && m->type == json::Value::Type::String) p.mode = modeFromName(m->str);
    if (const json::Value* o = v.get("parts"); o && o->type == json::Value::Type::Object) {
        auto flag = [&](const char* k, bool& dst) {
            if (const json::Value* x = o->get(k); x && x->type == json::Value::Type::Bool) dst = x->b;
        };
        flag("chain", p.parts.chain);
        flag("multi", p.parts.multi);
        flag("audio", p.parts.audio);
    }
    return p;
}

inline std::string partsJson(const Parts& p) {
    return std::string("{\"chain\":") + (p.chain ? "true" : "false") + ",\"multi\":" + (p.multi ? "true" : "false") +
           ",\"audio\":" + (p.audio ? "true" : "false") + "}";
}

inline std::string serializePref(const Pref& p) {
    return std::string("{\"mode\":\"") + modeName(p.mode) + "\",\"parts\":" + partsJson(p.parts) + "}";
}

/** A felület üzenetéből érkező részek (hiányzó kulcs: a meglévő érték marad). */
inline Parts partsFromJson(const json::Value* o, const Parts& base) {
    Parts p = base;
    if (!o || o->type != json::Value::Type::Object) return p;
    auto flag = [&](const char* k, bool& dst) {
        if (const json::Value* x = o->get(k); x && x->type == json::Value::Type::Bool) dst = x->b;
    };
    flag("chain", p.chain);
    flag("multi", p.multi);
    flag("audio", p.audio);
    return p;
}

/**
 * A felület állapot-üzenete: pending = a felület most kérdezi a részeket; available = van-e mentett adat az adott
 * részhez (a felület csak ezeket mutatja).
 */
inline std::string statusMessage(bool pending, const Pref& p, bool availChain, bool availMulti, bool availAudio) {
    return std::string("{\"type\":\"restore\",\"pending\":") + (pending ? "true" : "false") + ",\"mode\":\"" +
           modeName(p.mode) + "\",\"parts\":" + partsJson(p.parts) + ",\"available\":" +
           partsJson(Parts{ availChain, availMulti, availAudio }) + "}";
}

} // namespace ee_analyzer::startup_restore
