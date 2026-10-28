#pragma once
/**
 * @file media_session.hpp
 * @brief „Most szól” és lejátszásvezérlés: a gépen futó lejátszó (pl. a TIDAL alkalmazás) adatai és parancsai.
 *
 * A lejátszót a rendszer saját médiavezérlésén át érjük el (Windows: SMTC, Linux: MPRIS, macOS: MediaRemote),
 * ugyanúgy, mint a billentyűzet médiagombjai: a hang útja és a lejátszó működése nem változik, a hangot az Aqulify
 * a szokott módon dolgozza fel. Ez a fájl a platformfüggetlen adatmodell és a vezérlési protokoll JSON alakja.
 */
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace ee_analyzer::media {

struct Info {
    bool available = false;        // van-e vezérelhető lejátszó
    std::string app;               // a lejátszó neve (pl. "TIDAL")
    std::string title, artist, album;
    double duration = 0.0;         // másodperc (0: ismeretlen)
    double position = 0.0;         // másodperc, a positionAt időpontban
    std::chrono::steady_clock::time_point positionAt{};
    bool playing = false;
    bool canPlay = false, canPause = false, canNext = false, canPrevious = false, canSeek = false;
    std::string artId;             // a borító azonosítója (üres: nincs); változik, ha a borító változik
};

/** Engedélyezett parancsok a "media" üzenetben. */
inline bool validAction(const std::string& a) {
    return a == "play" || a == "pause" || a == "toggle" || a == "next" || a == "previous" || a == "seek";
}

inline std::string jsonEscape(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else out += static_cast<char>(c);
    }
    return out;
}

/** A "status" üzenet "media" mezője; a pozíció a küldés pillanatára számolva. */
inline std::string toJson(const Info& m) {
    if (!m.available) return "{\"available\":false}";
    double pos = m.position;
    if (m.playing && m.positionAt.time_since_epoch().count() != 0)
        pos += std::chrono::duration<double>(std::chrono::steady_clock::now() - m.positionAt).count();
    if (m.duration > 0.0 && pos > m.duration) pos = m.duration;
    if (pos < 0.0) pos = 0.0;
    char num[96];
    std::snprintf(num, sizeof(num), "\"duration\":%.3f,\"position\":%.3f", m.duration, pos);
    std::string s = "{\"available\":true,\"app\":\"" + jsonEscape(m.app) + "\",\"title\":\"" + jsonEscape(m.title) +
                    "\",\"artist\":\"" + jsonEscape(m.artist) + "\",\"album\":\"" + jsonEscape(m.album) + "\",";
    s += num;
    s += ",\"playing\":";
    s += m.playing ? "true" : "false";
    s += ",\"can\":{\"play\":";
    s += m.canPlay ? "true" : "false";
    s += ",\"pause\":";
    s += m.canPause ? "true" : "false";
    s += ",\"next\":";
    s += m.canNext ? "true" : "false";
    s += ",\"previous\":";
    s += m.canPrevious ? "true" : "false";
    s += ",\"seek\":";
    s += m.canSeek ? "true" : "false";
    s += "},\"art\":\"" + jsonEscape(m.artId) + "\"}";
    return s;
}

/** Egyszerű, gyors kivonat a borító azonosításához (FNV-1a, 64 bit). */
inline std::string hashId(const std::string& data) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

/** Ismert lejátszó neve az azonosítójából (Windows AUMID, MPRIS név, macOS csomagazonosító); üres, ha ismeretlen. */
inline std::string knownApp(const std::string& id) {
    std::string low;
    for (char c : id) low += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    struct Known { const char* key; const char* name; };
    static const Known known[] = {
        { "tidal", "TIDAL" }, { "spotify", "Spotify" }, { "qobuz", "Qobuz" }, { "deezer", "Deezer" },
        { "applemusic", "Apple Music" }, { "com.apple.music", "Apple Music" }, { "foobar", "foobar2000" },
        { "msedge", "Microsoft Edge" }, { "chrome", "Google Chrome" }, { "chromium", "Chromium" },
        { "firefox", "Firefox" }, { "vlc", "VLC" }, { "roon", "Roon" }, { "strawberry", "Strawberry" },
        { "rhythmbox", "Rhythmbox" }, { "elisa", "Elisa" }, { "mpv", "mpv" }, { "zunemusic", "Media Player" },
    };
    for (const auto& k : known)
        if (low.find(k.key) != std::string::npos) return k.name;
    return {};
}

/** A lejátszó alkalmazás olvasható neve: ismert név, különben az azonosító értelmes része. */
inline std::string friendlyApp(const std::string& id) {
    if (std::string k = knownApp(id); !k.empty()) return k;
    // az azonosító utolsó, értelmes része (pl. "C:\\...\\player.exe" -> "player", "org.x.Player" -> "Player")
    std::string s = id;
    const size_t slash = s.find_last_of("\\/");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    if (s.size() > 4 && (s.compare(s.size() - 4, 4, ".exe") == 0 || s.compare(s.size() - 4, 4, ".EXE") == 0)) s.resize(s.size() - 4);
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < s.size()) s = s.substr(dot + 1);
    const size_t bang = s.find('!');
    if (bang != std::string::npos) s = s.substr(0, bang);
    return s;
}

} // namespace ee_analyzer::media
