#pragma once
/**
 * @file media_url.hpp
 * @brief Megosztott média-hivatkozások (URL) biztonsági ellenőrzése.
 *
 * Csak engedélyezett zenei és videós szolgáltatók https:// címeit engedélyezi,
 * kizárva a helyi címeket, alternatív protokollokat, felhasználóneveket,
 * egyedi portokat és egyéb potenciálisan veszélyes bemeneteket.
 */

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>
#include <string>

namespace ee_analyzer {

/**
 * Ellenőrzi, hogy a megadott URL megnyitható-e biztonságosan a gépen média-hivatkozásként.
 *
 * @param url A vizsgálandó teljes hivatkozás.
 * @param hostOut Ha nem nullptr, sikeres ellenőrzés esetén a kinyert hosztnevet kapja meg (kisbetűsítve).
 * @return true, ha az URL biztonságos és a fehérlistán szereplő tartományhoz tartozik; egyébként false.
 */
inline bool isAllowedMediaUrl(const std::string& url, std::string* hostOut = nullptr) {
    // Hosszkorlát: legfeljebb 2048 karakter, nem lehet üres
    if (url.empty() || url.size() > 2048) {
        return false;
    }

    // Csak nyomtatható ASCII karakterek (0x21 - 0x7E), szóköz nélkül. Tiltva a kódolás nélkül URL-ben nem megengedett
    // karakterek is (RFC 3986): \ " < > ^ ` { | } – a cím Windowson a böngésző parancssorába kerül.
    // Ugyanez a lista a telefonon: MediaUrl.kt (aqulify-mobile); a kettőnek egyeznie kell.
    for (char c : url) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x21 || uc > 0x7E || std::strchr("\\\"<>^`{|}", c) != nullptr) {
            return false;
        }
    }

    // Kódolt null bájt tilalma
    if (url.find("%00") != std::string::npos) {
        return false;
    }

    // Séma pontosan https:// (kisbetűsen vizsgálva)
    if (url.size() < 8) {
        return false;
    }
    std::string scheme = url.substr(0, 8);
    for (char& c : scheme) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (scheme != "https://") {
        return false;
    }

    // Hoszt kinyerése: a // utáni rész az első '/', '?' vagy '#' jelig
    size_t hostEnd = url.find_first_of("/?#", 8);
    if (hostEnd == std::string::npos) {
        hostEnd = url.size();
    }
    std::string host = url.substr(8, hostEnd - 8);
    if (host.empty()) {
        return false;
    }

    // Tiltott elemek a hosztban: @ (felhasználónév), : (port), [ (IPv6 cím)
    if (host.find('@') != std::string::npos || host.find(':') != std::string::npos || host.front() == '[') {
        return false;
    }

    // Kisbetűsítés
    std::string h = host;
    for (char& c : h) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    // Punycode tilalma (hamis, megtévesztő karakterek ellen)
    if (h.find("xn--") != std::string::npos) {
        return false;
    }

    // IP-cím vizsgálata: ha csak számokból és pontokból áll, elutasítjuk
    bool allDigitsDots = true;
    for (char c : h) {
        if ((c < '0' || c > '9') && c != '.') {
            allDigitsDots = false;
            break;
        }
    }
    if (allDigitsDots) {
        return false;
    }

    // Záró pontok levágása
    while (!h.empty() && h.back() == '.') {
        h.pop_back();
    }
    if (h.empty() || h.front() == '.' || h.find("..") != std::string::npos) {
        return false;
    }

    // Megengedett karakterek a hosztnévben (betűk, számok, kötőjel, pont)
    for (char c : h) {
        if ((c < 'a' || c > 'z') && (c < '0' || c > '9') && c != '-' && c != '.') {
            return false;
        }
    }

    // Engedélyezett tartományok fehérlistája (a telefonon: MediaUrl.ALLOWED_DOMAINS; a kettőnek egyeznie kell)
    static constexpr const char* kAllowedDomains[] = {
        "youtube.com",
        "youtu.be",
        "music.youtube.com",
        "tidal.com",
        "open.spotify.com",
        "spotify.link",
        "soundcloud.com",
        "on.soundcloud.com",
        "deezer.com",
        "deezer.page.link",
        "link.deezer.com",
        "music.apple.com",
        "qobuz.com",
        "bandcamp.com",
        "mixcloud.com"
    };

    for (const char* dom : kAllowedDomains) {
        const size_t domLen = std::strlen(dom);
        // Pontos egyezés
        if (h == dom) {
            if (hostOut) *hostOut = h;
            return true;
        }
        // Aldomain egyezés: '.' + tartomány a hoszt végén
        if (h.size() > domLen + 1 &&
            h.compare(h.size() - domLen, domLen, dom) == 0 &&
            h[h.size() - domLen - 1] == '.') {
            if (hostOut) *hostOut = h;
            return true;
        }
    }

    return false;
}

/** Egy streaming szolgáltatás hivatkozásának tartalma (pl. track / album / playlist + azonosító). */
struct ServiceLink {
    std::string type;   // track, album, playlist, artist, mix, video
    std::string id;     // csak [A-Za-z0-9-], legfeljebb 64 karakter
};

/**
 * A már ellenőrzött (isAllowedMediaUrl) tidal.com-os cím tartalma: a megosztott címek alakjai
 * https://tidal.com/browse/track/123?u, https://tidal.com/track/123/u, https://listen.tidal.com/album/456,
 * https://tidal.com/browse/playlist/<uuid>. false: nem ilyen cím (akkor változatlanul nyílik meg).
 */
inline bool parseTidalLink(const std::string& url, ServiceLink& out) {
    std::string host;
    if (!isAllowedMediaUrl(url, &host)) return false;
    if (host != "tidal.com" && host != "www.tidal.com" && host != "listen.tidal.com") return false;
    const size_t pathStart = url.find('/', 8);
    if (pathStart == std::string::npos) return false;
    std::string path = url.substr(pathStart + 1);
    const size_t cut = path.find_first_of("?#");
    if (cut != std::string::npos) path.resize(cut);
    if (path.rfind("browse/", 0) == 0) path.erase(0, 7);
    const size_t slash = path.find('/');
    if (slash == std::string::npos) return false;
    std::string type = path.substr(0, slash);
    std::string id = path.substr(slash + 1);
    const size_t idEnd = id.find('/');
    if (idEnd != std::string::npos) id.resize(idEnd);
    static constexpr const char* kTypes[] = { "track", "album", "playlist", "artist", "mix", "video" };
    if (std::find_if(std::begin(kTypes), std::end(kTypes), [&](const char* t) { return type == t; }) == std::end(kTypes))
        return false;
    if (id.empty() || id.size() > 64) return false;
    for (char c : id)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') return false;
    out.type = type;
    out.id = id;
    return true;
}

/**
 * Mit nyisson meg a gép egy már ellenőrzött címhez. A tidal.com-os címek: ha a gépen van asztali alkalmazás a
 * "tidal:" protokollhoz, akkor "tidal://<típus>/<azonosító>" (az alkalmazásban nyílik meg), különben a webes
 * lejátszó (https://listen.tidal.com/<típus>/<azonosító>), nem a bemutatóoldal. Minden más cím változatlan.
 * A kimenet csak a fix típusnevekből és a [A-Za-z0-9-] azonosítóból áll, így nem kell újra ellenőrizni.
 */
inline std::string mediaOpenTarget(const std::string& url, bool tidalAppAvailable) {
    ServiceLink l;
    if (!parseTidalLink(url, l)) return url;
    return tidalAppAvailable ? "tidal://" + l.type + "/" + l.id : "https://listen.tidal.com/" + l.type + "/" + l.id;
}

} // namespace ee_analyzer
