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
#include <functional>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

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

/**
 * Egy streaming szolgáltatás hivatkozásából a megnyitás két módja: az asztali alkalmazásban (ha a gépen van kezelő az
 * appScheme protokollhoz) vagy a webes lejátszóban. Az appUrl és a webUrl csak fix szövegből és ellenőrzött
 * azonosítóból (betű, szám, kötőjel) áll, vagy (Apple Music) a már ellenőrzött cím változatlan részéből.
 */
struct ServiceTarget {
    std::string appScheme;   // pl. "tidal", "spotify" (üres: nincs alkalmazás-hivatkozás)
    std::string appUrl;
    std::string webUrl;
};

namespace media_url_detail {

/** Az útvonal részei a hoszt után, a ? és # nélkül, üres részek nélkül. */
inline std::vector<std::string> pathSegments(const std::string& url) {
    std::vector<std::string> out;
    const size_t start = url.find('/', 8);
    if (start == std::string::npos) return out;
    std::string path = url.substr(start + 1);
    const size_t cut = path.find_first_of("?#");
    if (cut != std::string::npos) path.resize(cut);
    size_t pos = 0;
    while (pos <= path.size()) {
        const size_t next = path.find('/', pos);
        const std::string seg = path.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        if (!seg.empty()) out.push_back(seg);
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return out;
}

inline bool validId(const std::string& id, bool allowDash) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id)
        if (!std::isalnum(static_cast<unsigned char>(c)) && !(allowDash && c == '-')) return false;
    return true;
}

inline bool oneOf(const std::string& s, std::initializer_list<const char*> list) {
    for (const char* x : list)
        if (s == x) return true;
    return false;
}

} // namespace media_url_detail

/**
 * A már ellenőrzött (isAllowedMediaUrl) cím szolgáltatás-hivatkozása. false: nincs alkalmazás-hivatkozása (pl.
 * YouTube, SoundCloud, Deezer: az asztali alkalmazásuknak nincs dokumentált mélyhivatkozása), vagy nem felismert
 * alakú cím; ilyenkor változatlanul nyílik meg.
 *  - TIDAL: tidal.com, www.tidal.com, listen.tidal.com; /browse/<típus>/<id>, /<típus>/<id>[/u]
 *    → tidal://<típus>/<id> (hivatalos app; Linuxon pl. a SONE is ezt kezeli) | listen.tidal.com/<típus>/<id>
 *  - Spotify: open.spotify.com/[intl-xx/]<típus>/<id> → spotify:<típus>:<id> | open.spotify.com/<típus>/<id>
 *  - Qobuz: open.qobuz.com vagy play.qobuz.com/<típus>/<id>, www.qobuz.com/<nyelv>/album/<név>/<id>
 *    → qobuzapp://<típus>/<id> | play.qobuz.com/<típus>/<id>
 *  - Apple Music: music.apple.com/… → music://music.apple.com/… | változatlan
 */
inline bool serviceTarget(const std::string& url, ServiceTarget& out) {
    using namespace media_url_detail;
    std::string host;
    if (!isAllowedMediaUrl(url, &host)) return false;
    std::vector<std::string> seg = pathSegments(url);

    if (host == "tidal.com" || host == "www.tidal.com" || host == "listen.tidal.com") {
        if (!seg.empty() && seg[0] == "browse") seg.erase(seg.begin());
        if (seg.size() < 2 || !oneOf(seg[0], { "track", "album", "playlist", "artist", "mix", "video" }) ||
            !validId(seg[1], true))
            return false;
        out = { "tidal", "tidal://" + seg[0] + "/" + seg[1], "https://listen.tidal.com/" + seg[0] + "/" + seg[1] };
        return true;
    }
    if (host == "open.spotify.com") {
        if (!seg.empty() && seg[0].rfind("intl-", 0) == 0) seg.erase(seg.begin());
        if (seg.size() < 2 || !oneOf(seg[0], { "track", "album", "playlist", "artist", "episode", "show" }) ||
            !validId(seg[1], false))
            return false;
        out = { "spotify", "spotify:" + seg[0] + ":" + seg[1], "https://open.spotify.com/" + seg[0] + "/" + seg[1] };
        return true;
    }
    if (host == "open.qobuz.com" || host == "play.qobuz.com") {
        if (seg.size() < 2 || !oneOf(seg[0], { "track", "album", "playlist", "artist" }) || !validId(seg[1], false))
            return false;
        out = { "qobuzapp", "qobuzapp://" + seg[0] + "/" + seg[1], "https://play.qobuz.com/" + seg[0] + "/" + seg[1] };
        return true;
    }
    if (host == "www.qobuz.com" || host == "qobuz.com") {
        // A bolt albumoldala: /<nyelv>/album/<név>/<azonosító>
        const auto it = std::find(seg.begin(), seg.end(), std::string("album"));
        if (it == seg.end() || seg.back() == "album" || !validId(seg.back(), false)) return false;
        out = { "qobuzapp", "qobuzapp://album/" + seg.back(), "https://play.qobuz.com/album/" + seg.back() };
        return true;
    }
    if (host == "music.apple.com") {
        if (seg.size() < 2) return false;
        out = { "music", "music://" + url.substr(8), url };
        return true;
    }
    return false;
}

/**
 * Mit nyisson meg a gép egy már ellenőrzött címhez: a szolgáltatás asztali alkalmazásában, ha a gépen van kezelő a
 * protokolljához (hasHandler, pl. "tidal"), különben a webes lejátszóban; minden más cím változatlan.
 */
inline std::string mediaOpenTarget(const std::string& url, const std::function<bool(const std::string&)>& hasHandler) {
    ServiceTarget t;
    if (!serviceTarget(url, t)) return url;
    return (!t.appScheme.empty() && hasHandler && hasHandler(t.appScheme)) ? t.appUrl : t.webUrl;
}

} // namespace ee_analyzer