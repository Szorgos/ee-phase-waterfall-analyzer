#pragma once
/**
 * @file radio_net.hpp
 * @brief Internetrádió: a hálózati rész tiszta (tesztelhető) segédei.
 *
 *  - parseRadioUrl: csak http:// és https:// cím, felhasználónév nélkül, legfeljebb 2048 karakter, nyomtatható ASCII.
 *  - isPublicV4 / isPublicV6: a cél címe nyilvános-e. Tiltott (SSRF-védelem): a helyi gép (loopback), a magánhálózatok
 *    (10/8, 172.16/12, 192.168/16, 100.64/10, fc00::/7), a link-local (169.254/16, fe80::/10), a csoportos (multicast),
 *    a fenntartott és dokumentációs tartományok, és az IPv6-ba ágyazott ilyen IPv4-címek (::ffff:, 64:ff9b::, 2002::).
 *    Ez a media_url.hpp isAllowedMediaUrl ellentéte: ott egy szűk https-fehérlista, itt bármely nyilvános http(s) cím.
 *  - IcyDemux: az ICY metaadat (icy-metaint) kiválasztása a hangfolyamból, StreamTitle kinyerése.
 *  - sniffFormat: a folyam formátuma a Content-Type és az első bájtok alapján.
 *  - playlistUrls: PLS és egyszerű M3U lejátszási lista (a rádiók gyakran így adják meg a címet); HLS-t nem.
 *  - parseHttpHead: a válasz fejléce (HTTP/1.x, HTTP/2 és a régi „ICY 200 OK” sor).
 */
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace ee_analyzer::radio {

struct RadioUrl {
    bool https = false;
    std::string host;       // kisbetűs; IPv6-nál szögletes zárójel nélkül
    bool ipv6 = false;      // a hoszt IPv6-cím
    uint16_t port = 0;      // 0: a séma alapértelmezése (80 / 443)
    std::string target;     // az út és a lekérdezés, "/"-rel kezdődik (a # rész nélkül)

    uint16_t effectivePort() const { return port ? port : static_cast<uint16_t>(https ? 443 : 80); }
    /** A teljes cím (a kéréshez): séma://hoszt[:port]/út */
    std::string str() const {
        std::string s = https ? "https://" : "http://";
        s += ipv6 ? "[" + host + "]" : host;
        if (port) s += ":" + std::to_string(port);
        return s + target;
    }
};

constexpr size_t kMaxUrlLength = 2048;

/** A rádió címének ellenőrzése és felbontása; false: nem fogadható el (err: rövid kód, pl. "scheme", "host"). */
inline bool parseRadioUrl(const std::string& url, RadioUrl& out, std::string* err = nullptr) {
    auto fail = [&](const char* e) { if (err) *err = e; return false; };
    if (url.empty() || url.size() > kMaxUrlLength) return fail("length");
    for (char c : url) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x21 || uc > 0x7E || std::strchr("\\\"<>^`{|}", c) != nullptr) return fail("chars");
    }
    std::string lower = url.substr(0, 8);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    size_t p;
    RadioUrl u;
    if (lower.rfind("http://", 0) == 0) { u.https = false; p = 7; }
    else if (lower.rfind("https://", 0) == 0) { u.https = true; p = 8; }
    else return fail("scheme");
    const size_t authEnd = url.find_first_of("/?#", p);
    const std::string auth = url.substr(p, (authEnd == std::string::npos ? url.size() : authEnd) - p);
    if (auth.empty()) return fail("host");
    if (auth.find('@') != std::string::npos) return fail("userinfo");
    std::string host, port;
    if (auth[0] == '[') {
        const size_t close = auth.find(']');
        if (close == std::string::npos || close < 3) return fail("host");
        host = auth.substr(1, close - 1);
        for (char c : host)
            if (!std::isxdigit(static_cast<unsigned char>(c)) && c != ':' && c != '.') return fail("host");
        u.ipv6 = true;
        if (close + 1 < auth.size()) {
            if (auth[close + 1] != ':') return fail("host");
            port = auth.substr(close + 2);
        }
    } else {
        const size_t colon = auth.find(':');
        host = auth.substr(0, colon);
        if (colon != std::string::npos) port = auth.substr(colon + 1);
        while (!host.empty() && host.back() == '.') host.pop_back();
        if (host.empty() || host.front() == '.' || host.find("..") != std::string::npos || host.size() > 253) return fail("host");
        for (char& c : host) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if ((c < 'a' || c > 'z') && (c < '0' || c > '9') && c != '-' && c != '.') return fail("host");
        }
    }
    if (!port.empty() || (auth.back() == ':')) {
        if (port.empty() || port.size() > 5) return fail("port");
        unsigned v = 0;
        for (char c : port) {
            if (c < '0' || c > '9') return fail("port");
            v = v * 10 + static_cast<unsigned>(c - '0');
        }
        if (v == 0 || v > 65535) return fail("port");
        u.port = static_cast<uint16_t>(v);
    }
    u.host = host;
    std::string target = authEnd == std::string::npos ? std::string("/") : url.substr(authEnd);
    const size_t hash = target.find('#');
    if (hash != std::string::npos) target.resize(hash);
    if (target.empty() || target[0] == '?') target = "/" + target;
    if (target.find("%00") != std::string::npos) return fail("chars");
    u.target = target;
    out = u;
    return true;
}

/** Az átirányítás (Location) feloldása a mostani címhez képest; false: nem értelmezhető. */
inline bool resolveLocation(const RadioUrl& base, const std::string& loc, RadioUrl& out, std::string* err = nullptr) {
    if (loc.empty()) { if (err) *err = "redirect"; return false; }
    std::string lower = loc.substr(0, 8);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0) return parseRadioUrl(loc, out, err);
    if (loc.rfind("//", 0) == 0) return parseRadioUrl((base.https ? "https:" : "http:") + loc, out, err);
    // más séma (ftp:, file:, javascript: …): a kettőspont az első / ? # előtt
    const size_t colon = loc.find(':'), delim = loc.find_first_of("/?#");
    if (colon != std::string::npos && (delim == std::string::npos || colon < delim)) { if (err) *err = "scheme"; return false; }
    RadioUrl b = base;
    if (loc[0] == '/') b.target = loc;
    else {
        std::string dir = b.target.substr(0, b.target.find('?'));
        dir = dir.substr(0, dir.rfind('/') + 1);
        b.target = dir + loc;
    }
    return parseRadioUrl(b.str(), out, err);
}

// ------------------------------------------------------------------ címek osztályozása (SSRF-védelem)

/** IPv4 (a bájtok hálózati sorrendben: a[0].a[1].a[2].a[3]) nyilvános-e. */
inline bool isPublicV4(const uint8_t a[4]) {
    const uint32_t v = (uint32_t(a[0]) << 24) | (uint32_t(a[1]) << 16) | (uint32_t(a[2]) << 8) | a[3];
    struct Net { uint32_t base; int bits; };
    static constexpr Net kBlocked[] = {
        { 0x00000000u, 8 },    // 0.0.0.0/8 „ez a hálózat”
        { 0x0A000000u, 8 },    // 10/8 magán
        { 0x64400000u, 10 },   // 100.64/10 szolgáltatói NAT
        { 0x7F000000u, 8 },    // 127/8 loopback
        { 0xA9FE0000u, 16 },   // 169.254/16 link-local
        { 0xAC100000u, 12 },   // 172.16/12 magán
        { 0xC0000000u, 24 },   // 192.0.0/24 IETF
        { 0xC0000200u, 24 },   // 192.0.2/24 dokumentáció
        { 0xC0586300u, 24 },   // 192.88.99/24 6to4 továbbító
        { 0xC0A80000u, 16 },   // 192.168/16 magán
        { 0xC6120000u, 15 },   // 198.18/15 mérés
        { 0xC6336400u, 24 },   // 198.51.100/24 dokumentáció
        { 0xCB007100u, 24 },   // 203.0.113/24 dokumentáció
        { 0xE0000000u, 4 },    // 224/4 multicast
        { 0xF0000000u, 4 },    // 240/4 fenntartott + 255.255.255.255
    };
    for (const Net& n : kBlocked) {
        const uint32_t mask = n.bits == 0 ? 0u : ~0u << (32 - n.bits);
        if ((v & mask) == n.base) return false;
    }
    return true;
}

inline bool isLoopbackV4(const uint8_t a[4]) { return a[0] == 127; }
inline bool isLoopbackV6(const uint8_t a[16]) {
    static const uint8_t one[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    if (std::memcmp(a, one, 16) == 0) return true;
    static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
    return std::memcmp(a, mapped, 12) == 0 && a[12] == 127;
}

/** IPv6 nyilvános-e: csak a 2000::/3 globális tartomány, a dokumentációs / Teredo / beágyazott tiltott IPv4 nélkül. */
inline bool isPublicV6(const uint8_t a[16]) {
    static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
    if (std::memcmp(a, mapped, 12) == 0) return isPublicV4(a + 12);                    // ::ffff:a.b.c.d
    static const uint8_t nat64[12] = { 0, 0x64, 0xFF, 0x9B, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (std::memcmp(a, nat64, 12) == 0) return isPublicV4(a + 12);                     // 64:ff9b::/96
    if ((a[0] & 0xE0) != 0x20) return false;                                           // csak 2000::/3
    if (a[0] == 0x20 && a[1] == 0x01 && a[2] == 0x0D && a[3] == 0xB8) return false;    // 2001:db8::/32 dokumentáció
    if (a[0] == 0x20 && a[1] == 0x01 && a[2] == 0x00 && a[3] == 0x00) return false;    // 2001::/32 Teredo
    if (a[0] == 0x20 && a[1] == 0x02) return isPublicV4(a + 2);                        // 2002::/16 6to4
    return true;
}

// ------------------------------------------------------------------ HTTP-fejléc

struct HttpHead {
    int status = 0;
    std::map<std::string, std::string> headers;   // kisbetűs kulcs -> érték (ismétlődőnél az utolsó)
    std::string get(const std::string& k) const {
        auto it = headers.find(k);
        return it == headers.end() ? std::string() : it->second;
    }
};

/** A válasz fejléce (a záró üres sor nélkül vagy azzal); false: nem HTTP-válasz. */
inline bool parseHttpHead(const std::string& block, HttpHead& out) {
    HttpHead h;
    size_t pos = 0;
    bool first = true;
    while (pos < block.size()) {
        size_t eol = block.find('\n', pos);
        if (eol == std::string::npos) eol = block.size();
        std::string line = block.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first) {
            first = false;
            // "HTTP/1.1 200 OK", "HTTP/2 200", "ICY 200 OK"
            const size_t sp = line.find(' ');
            if (sp == std::string::npos) return false;
            const std::string proto = line.substr(0, sp);
            if (proto.rfind("HTTP/", 0) != 0 && proto != "ICY") return false;
            if (line.size() < sp + 4) return false;
            int code = 0;
            for (size_t i = sp + 1; i < sp + 4; ++i) {
                if (line[i] < '0' || line[i] > '9') return false;
                code = code * 10 + (line[i] - '0');
            }
            h.status = code;
            continue;
        }
        if (line.empty()) break;
        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) continue;
        std::string key = line.substr(0, colon);
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        size_t vs = colon + 1;
        while (vs < line.size() && (line[vs] == ' ' || line[vs] == '\t')) ++vs;
        std::string val = line.substr(vs);
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
        h.headers[key] = val;
    }
    if (h.status == 0) return false;
    out = std::move(h);
    return true;
}

// ------------------------------------------------------------------ szöveg tisztítása

/** UTF-8-e; ha nem, Latin-1-ként alakítja át. Vezérlőkarakterek nélkül, legfeljebb maxBytes bájt. */
inline std::string cleanText(const std::string& in, size_t maxBytes = 256) {
    auto isUtf8 = [](const std::string& s) {
        size_t i = 0;
        while (i < s.size()) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            const size_t n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 99;
            if (n == 99 || i + n >= s.size()) return false;
            for (size_t k = 1; k <= n; ++k)
                if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
            i += n + 1;
        }
        return true;
    };
    std::string s;
    if (isUtf8(in)) s = in;
    else
        for (unsigned char c : in) {
            if (c < 0x80) s += static_cast<char>(c);
            else { s += static_cast<char>(0xC0 | (c >> 6)); s += static_cast<char>(0x80 | (c & 0x3F)); }
        }
    std::string out;
    for (unsigned char c : s) out += (c < 0x20 || c == 0x7F) ? ' ' : static_cast<char>(c);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    size_t b = 0;
    while (b < out.size() && out[b] == ' ') ++b;
    out.erase(0, b);
    if (out.size() > maxBytes) {
        size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;   // ne vágjunk karaktert ketté
        out.resize(cut);
    }
    return out;
}

// ------------------------------------------------------------------ ICY metaadat

/** A metaadat-blokk StreamTitle='…'; értéke (tisztítva); üres: nincs. */
inline std::string icyStreamTitle(const std::string& meta) {
    const std::string key = "StreamTitle='";
    const size_t p = meta.find(key);
    if (p == std::string::npos) return {};
    const size_t s = p + key.size();
    size_t e = meta.find("';", s);
    if (e == std::string::npos) e = meta.rfind('\'');
    if (e == std::string::npos || e < s) return {};
    std::string v = meta.substr(s, e - s);
    const size_t nul = v.find('\0');
    if (nul != std::string::npos) v.resize(nul);
    return cleanText(v);
}

/**
 * Az ICY-folyam szétválasztása: metaint bájt hang, utána 1 hosszbájt (×16) és annyi bájt metaadat. metaint = 0: nincs
 * metaadat, minden bájt hang. A blokk legfeljebb 255×16 = 4080 bájt (ennél több nem is jelezhető).
 */
class IcyDemux {
public:
    explicit IcyDemux(size_t metaint = 0) : metaint_(metaint), left_(metaint) {}
    /**
     * A bejövő bájtok: a hang az audio végére kerül, a kész metaadat-blokkok a metas-ba. metaAt (ha nem nullptr): minden
     * blokkhoz az audio mérete a blokk helyén (ettől a hangbájttól érvényes a cím).
     */
    void feed(const uint8_t* p, size_t n, std::vector<uint8_t>& audio, std::vector<std::string>& metas,
              std::vector<size_t>* metaAt = nullptr) {
        if (metaint_ == 0) { audio.insert(audio.end(), p, p + n); return; }
        size_t i = 0;
        while (i < n) {
            if (state_ == 0) {
                const size_t take = std::min(left_, n - i);
                audio.insert(audio.end(), p + i, p + i + take);
                i += take;
                left_ -= take;
                if (left_ == 0) state_ = 1;
            } else if (state_ == 1) {
                metaLeft_ = static_cast<size_t>(p[i++]) * 16;
                meta_.clear();
                if (metaLeft_ == 0) { state_ = 0; left_ = metaint_; }
                else state_ = 2;
            } else {
                const size_t take = std::min(metaLeft_, n - i);
                meta_.append(reinterpret_cast<const char*>(p + i), take);
                i += take;
                metaLeft_ -= take;
                if (metaLeft_ == 0) {
                    metas.push_back(meta_);
                    if (metaAt) metaAt->push_back(audio.size());
                    state_ = 0;
                    left_ = metaint_;
                }
            }
        }
    }

private:
    size_t metaint_;
    size_t left_;
    int state_ = 0;   // 0: hang, 1: hosszbájt, 2: metaadat
    size_t metaLeft_ = 0;
    std::string meta_;
};

// ------------------------------------------------------------------ formátum és lejátszási lista

enum class StreamFormat { Unknown, Mp3, Ogg, Flac, Aac, Playlist, Hls, Html };

inline const char* formatName(StreamFormat f) {
    switch (f) {
    case StreamFormat::Mp3: return "mp3";
    case StreamFormat::Ogg: return "ogg";
    case StreamFormat::Flac: return "flac";
    case StreamFormat::Aac: return "aac";
    case StreamFormat::Playlist: return "playlist";
    case StreamFormat::Hls: return "hls";
    case StreamFormat::Html: return "html";
    default: return "unknown";
    }
}

/** A folyam formátuma: előbb az első bájtok (megbízhatóbb), ha azok nem döntenek, a Content-Type. */
inline StreamFormat sniffFormat(const std::string& contentType, const uint8_t* p, size_t n) {
    auto starts = [&](const char* s) { const size_t l = std::strlen(s); return n >= l && std::memcmp(p, s, l) == 0; };
    std::string text(reinterpret_cast<const char*>(p), std::min<size_t>(n, 512));
    size_t ws = 0;
    while (ws < text.size() && (text[ws] == ' ' || text[ws] == '\r' || text[ws] == '\n' || text[ws] == '\t' ||
                                static_cast<unsigned char>(text[ws]) == 0xEF || static_cast<unsigned char>(text[ws]) == 0xBB ||
                                static_cast<unsigned char>(text[ws]) == 0xBF))
        ++ws;
    const std::string t = text.substr(ws);
    std::string tl = t;
    for (char& c : tl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (starts("OggS")) return StreamFormat::Ogg;
    if (starts("fLaC")) return StreamFormat::Flac;
    if (starts("ID3")) return StreamFormat::Mp3;
    if (n >= 2 && p[0] == 0xFF) {
        if ((p[1] & 0xF6) == 0xF0) return StreamFormat::Aac;                     // ADTS: layer = 00
        if ((p[1] & 0xE0) == 0xE0 && ((p[1] >> 1) & 3) != 0) return StreamFormat::Mp3;
    }
    if (tl.find("#ext-x-") != std::string::npos) return StreamFormat::Hls;
    if (tl.rfind("[playlist]", 0) == 0 || tl.rfind("#extm3u", 0) == 0 || tl.rfind("http://", 0) == 0 ||
        tl.rfind("https://", 0) == 0)
        return StreamFormat::Playlist;
    if (tl.rfind("<", 0) == 0) return StreamFormat::Html;
    std::string ct = contentType.substr(0, contentType.find(';'));
    for (char& c : ct) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!ct.empty() && ct.back() == ' ') ct.pop_back();
    if (ct == "audio/mpeg" || ct == "audio/mp3" || ct == "audio/mpeg3") return StreamFormat::Mp3;
    if (ct == "audio/ogg" || ct == "application/ogg" || ct == "audio/opus" || ct == "audio/vorbis") return StreamFormat::Ogg;
    if (ct == "audio/flac" || ct == "audio/x-flac") return StreamFormat::Flac;
    if (ct == "audio/aac" || ct == "audio/aacp" || ct == "audio/x-aac" || ct == "audio/mp4" || ct == "audio/x-m4a")
        return StreamFormat::Aac;
    if (ct == "application/vnd.apple.mpegurl") return StreamFormat::Hls;
    if (ct == "audio/x-scpls" || ct == "audio/x-mpegurl" || ct == "audio/mpegurl" || ct == "application/x-mpegurl" ||
        ct == "application/pls+xml")
        return StreamFormat::Playlist;
    if (ct == "text/html") return StreamFormat::Html;
    return StreamFormat::Unknown;
}

/** A PLS (FileN=…) vagy M3U (nem # kezdetű sorok) címei, a megadás sorrendjében; legfeljebb 16. */
inline std::vector<std::string> playlistUrls(const std::string& body) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < body.size() && out.size() < 16) {
        size_t eol = body.find('\n', pos);
        if (eol == std::string::npos) eol = body.size();
        std::string line = body.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t b = 0;
        while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
        line.erase(0, b);
        if (line.empty() || line[0] == '#' || line[0] == '[') continue;
        std::string lower = line;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower.rfind("file", 0) == 0) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            line = line.substr(eq + 1);
            lower = lower.substr(eq + 1);
        } else if (lower.find('=') != std::string::npos && lower.rfind("http", 0) != 0) {
            continue;   // PLS egyéb sora (Title1=, Length1=, NumberOfEntries=, Version=)
        }
        if (lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0) out.push_back(line);
    }
    return out;
}

/** "Előadó - Cím" szétbontása (az ICY StreamTitle szokásos alakja); ha nincs " - ", az egész a cím. */
inline void splitArtistTitle(const std::string& s, std::string& artist, std::string& title) {
    const size_t p = s.find(" - ");
    if (p == std::string::npos || p == 0 || p + 3 >= s.size()) { artist.clear(); title = s; return; }
    artist = s.substr(0, p);
    title = s.substr(p + 3);
}

} // namespace ee_analyzer::radio
