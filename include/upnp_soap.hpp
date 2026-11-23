#pragma once
/**
 * @file upnp_soap.hpp
 * @brief UPnP-renderer: a tiszta (hálózat nélkül tesztelhető) részek – XML-bontás és -escape, SOAP-kérés és -válasz,
 *        UPnPError, időformátum, DIDL-Lite, protocolInfo, LastChange-esemény, a médiacímek és visszahívási címek
 *        szabálya.
 *
 * Források (csak a szabványok; mintát más programból nem vettünk át):
 *  - UPnP Device Architecture 1.1 (2008), 3. fejezet (SOAP-vezérlés: boríték, válasz, UPnPError) és 4. fejezet
 *    (GENA: propertyset);
 *  - AVTransport:1 Service Template 1.01 (2.2.x állapotváltozók, 2.4 hibakódok, a LastChange és az időformátum:
 *    H+:MM:SS[.F+] vagy H+:MM:SS[.F0/F1]);
 *  - RenderingControl:1 Service Template 1.01 (a LastChange RCS-névtere, a Volume / Mute channel attribútuma);
 *  - ConnectionManager:1 Service Template 1.01 (protocolInfo: <protocol>:<network>:<contentFormat>:<additionalInfo>);
 *  - ContentDirectory:1 Service Template 1.01, DIDL-Lite (dc:title, dc:creator, upnp:artist, upnp:album,
 *    upnp:albumArtURI, upnp:class, res@protocolInfo, res@duration, res@size).
 *
 * Az XML-bontó szándékosan kicsi és türelmes: a névtér-előtagot eldobja (a vezérlők eltérő előtagokat használnak),
 * a CDATA-t és az előre definiált / numerikus entitásokat kezeli, DOCTYPE-ot és saját entitást nem fogad el (nincs
 * entitás-kiterjesztés), a mélység és a csomópontok száma korlátos.
 */
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "net_compat.hpp"
#include "radio_net.hpp"

namespace ee_analyzer::upnp {

// ------------------------------------------------------------------ XML

/** XML-escape (szöveg és attribútum): & < > " ' */
inline std::string xmlEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (char c : in) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c;
        }
    }
    return out;
}

inline void appendUtf8Cp(std::string& out, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return;   // érvénytelen kódpont: kimarad
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

/** Az előre definiált (&lt; &gt; &amp; &quot; &apos;) és numerikus (&#NN; &#xHH;) entitások feloldása; az ismeretlen marad. */
inline std::string xmlUnescape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '&') { out += in[i]; continue; }
        const size_t semi = in.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) { out += in[i]; continue; }
        const std::string ent = in.substr(i + 1, semi - i - 1);
        if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "amp") out += '&';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (ent.size() > 1 && ent[0] == '#') {
            uint32_t cp = 0;
            bool ok = true;
            const bool hex = ent[1] == 'x' || ent[1] == 'X';
            const size_t from = hex ? 2 : 1;
            if (from >= ent.size()) ok = false;
            for (size_t k = from; ok && k < ent.size(); ++k) {
                const char c = ent[k];
                int d = -1;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
                if (d < 0 || cp > 0x10FFFF) ok = false;
                else cp = cp * (hex ? 16 : 10) + static_cast<uint32_t>(d);
            }
            if (!ok) { out += in[i]; continue; }
            appendUtf8Cp(out, cp);
        } else {
            out += in[i];
            continue;
        }
        i = semi;
    }
    return out;
}

/** Egy XML-elem: név (előtag nélkül), attribútumok (előtag nélküli kulcs, feloldott érték), szöveg, gyerekek. */
struct XmlNode {
    std::string name;
    std::string ns;                                  // a névtér URI-ja, ha az elem (vagy egy őse) megadta
    std::vector<std::pair<std::string, std::string>> attrs;
    std::string text;                                // a közvetlen szövegtartalom (feloldva, CDATA-val együtt)
    std::vector<XmlNode> children;

    const XmlNode* child(const char* n) const {
        for (const auto& c : children)
            if (c.name == n) return &c;
        return nullptr;
    }
    /** Az első ilyen nevű leszármazott (mélységben). */
    const XmlNode* find(const char* n) const {
        for (const auto& c : children) {
            if (c.name == n) return &c;
            if (const XmlNode* d = c.find(n)) return d;
        }
        return nullptr;
    }
    std::string attr(const char* n) const {
        for (const auto& a : attrs)
            if (a.first == n) return a.second;
        return {};
    }
    std::string childText(const char* n) const {
        const XmlNode* c = child(n);
        return c ? c->text : std::string();
    }
};

namespace xml_detail {
inline std::string localName(const std::string& q) {
    const size_t c = q.find(':');
    return c == std::string::npos ? q : q.substr(c + 1);
}
inline std::string prefixOf(const std::string& q) {
    const size_t c = q.find(':');
    return c == std::string::npos ? std::string() : q.substr(0, c);
}
inline bool isNameChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == ':' || c == '_' || c == '-' || c == '.' ||
           static_cast<unsigned char>(c) >= 0x80;
}

struct Parser {
    const std::string& s;
    size_t p = 0;
    size_t nodes = 0;
    static constexpr int kMaxDepth = 32;
    static constexpr size_t kMaxNodes = 4096;

    explicit Parser(const std::string& src) : s(src) {}

    bool startsWith(const char* t) const { return s.compare(p, std::strlen(t), t) == 0; }
    void skipWs() { while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p; }
    /** Megjegyzés, feldolgozási utasítás; DOCTYPE: hiba (nincs saját entitás). */
    bool skipMisc() {
        for (;;) {
            skipWs();
            if (startsWith("<?")) {
                const size_t e = s.find("?>", p + 2);
                if (e == std::string::npos) return false;
                p = e + 2;
            } else if (startsWith("<!--")) {
                const size_t e = s.find("-->", p + 4);
                if (e == std::string::npos) return false;
                p = e + 3;
            } else if (startsWith("<!DOCTYPE") || startsWith("<!doctype")) {
                return false;
            } else {
                return true;
            }
        }
    }
    bool readName(std::string& out) {
        const size_t b = p;
        while (p < s.size() && isNameChar(s[p])) ++p;
        if (p == b) return false;
        out = s.substr(b, p - b);
        return true;
    }
    /** Egy elem (p a '<'-en áll); nsMap: az előtagok névterei (öröklődik). */
    bool element(XmlNode& n, std::map<std::string, std::string> nsMap, int depth) {
        if (depth > kMaxDepth || ++nodes > kMaxNodes) return false;
        if (p >= s.size() || s[p] != '<') return false;
        ++p;
        std::string qname;
        if (!readName(qname)) return false;
        std::vector<std::pair<std::string, std::string>> raw;
        for (;;) {
            skipWs();
            if (p >= s.size()) return false;
            if (s[p] == '/' || s[p] == '>') break;
            std::string an;
            if (!readName(an)) return false;
            skipWs();
            if (p >= s.size() || s[p] != '=') return false;
            ++p;
            skipWs();
            if (p >= s.size() || (s[p] != '"' && s[p] != '\'')) return false;
            const char q = s[p++];
            const size_t e = s.find(q, p);
            if (e == std::string::npos) return false;
            raw.emplace_back(an, xmlUnescape(s.substr(p, e - p)));
            p = e + 1;
        }
        for (const auto& a : raw) {
            if (a.first == "xmlns") nsMap[""] = a.second;
            else if (a.first.rfind("xmlns:", 0) == 0) nsMap[a.first.substr(6)] = a.second;
            else n.attrs.emplace_back(localName(a.first), a.second);
        }
        n.name = localName(qname);
        auto it = nsMap.find(prefixOf(qname));
        if (it != nsMap.end()) n.ns = it->second;
        if (s[p] == '/') {
            if (p + 1 >= s.size() || s[p + 1] != '>') return false;
            p += 2;
            return true;
        }
        ++p;   // '>'
        for (;;) {
            if (p >= s.size()) return false;
            if (startsWith("</")) {
                p += 2;
                std::string close;
                if (!readName(close)) return false;
                if (close != qname) return false;
                skipWs();
                if (p >= s.size() || s[p] != '>') return false;
                ++p;
                return true;
            }
            if (startsWith("<![CDATA[")) {
                const size_t e = s.find("]]>", p + 9);
                if (e == std::string::npos) return false;
                n.text += s.substr(p + 9, e - p - 9);
                p = e + 3;
                continue;
            }
            if (startsWith("<!--")) {
                const size_t e = s.find("-->", p + 4);
                if (e == std::string::npos) return false;
                p = e + 3;
                continue;
            }
            if (startsWith("<?")) {
                const size_t e = s.find("?>", p + 2);
                if (e == std::string::npos) return false;
                p = e + 2;
                continue;
            }
            if (startsWith("<!")) return false;
            if (s[p] == '<') {
                XmlNode c;
                if (!element(c, nsMap, depth + 1)) return false;
                n.children.push_back(std::move(c));
                continue;
            }
            const size_t e = s.find('<', p);
            if (e == std::string::npos) return false;
            n.text += xmlUnescape(s.substr(p, e - p));
            p = e;
        }
    }
};
} // namespace xml_detail

/** A dokumentum gyökéreleme; false: nem jól formált (vagy DOCTYPE / túl mély / túl nagy). */
inline bool parseXml(const std::string& doc, XmlNode& root) {
    xml_detail::Parser ps(doc);
    // UTF-8 BOM
    if (doc.size() >= 3 && static_cast<unsigned char>(doc[0]) == 0xEF && static_cast<unsigned char>(doc[1]) == 0xBB &&
        static_cast<unsigned char>(doc[2]) == 0xBF)
        ps.p = 3;
    if (!ps.skipMisc()) return false;
    XmlNode r;
    if (!ps.element(r, {}, 0)) return false;
    if (!ps.skipMisc() || ps.p != doc.size()) return false;
    root = std::move(r);
    return true;
}

/** Szöveg a széleken lévő szóközök nélkül. */
inline std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// ------------------------------------------------------------------ SOAP (UDA 1.1, 3.2)

constexpr const char* kAvtType = "urn:schemas-upnp-org:service:AVTransport:1";
constexpr const char* kRcsType = "urn:schemas-upnp-org:service:RenderingControl:1";
constexpr const char* kCmType = "urn:schemas-upnp-org:service:ConnectionManager:1";
constexpr const char* kRendererType = "urn:schemas-upnp-org:device:MediaRenderer:1";
/** A SOAP-törzs legnagyobb mérete (a tervben rögzített korlát). */
constexpr size_t kMaxSoapBody = 64 * 1024;
/** Egy argumentum (pl. a DIDL-Lite metaadat) legnagyobb mérete. */
constexpr size_t kMaxArgBytes = 32 * 1024;

struct SoapRequest {
    std::string serviceType;   // pl. urn:schemas-upnp-org:service:AVTransport:1
    std::string action;        // pl. Play
    std::vector<std::pair<std::string, std::string>> args;   // a megadás sorrendjében, feloldott szöveggel

    bool has(const char* n) const {
        for (const auto& a : args)
            if (a.first == n) return true;
        return false;
    }
    std::string arg(const char* n) const {
        for (const auto& a : args)
            if (a.first == n) return a.second;
        return {};
    }
};

/**
 * A SOAPACTION fejléc ("urn:…:service:AVTransport:1#Play", idézőjellel vagy anélkül) felbontása.
 * false: hiányzik vagy hibás.
 */
inline bool parseSoapActionHeader(const std::string& h, std::string& type, std::string& action) {
    std::string v = trim(h);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    const size_t hash = v.rfind('#');
    if (hash == std::string::npos || hash == 0 || hash + 1 >= v.size()) return false;
    type = v.substr(0, hash);
    action = v.substr(hash + 1);
    return true;
}

/**
 * A SOAP-boríték bontása: Envelope / Body / <művelet> és a gyerekei mint argumentumok. A szolgáltatás típusa a
 * fejlécből (ha megvan), különben a műveletelem névteréből. false: err = "xml", "envelope", "action", "size", "mismatch".
 */
inline bool parseSoapRequest(const std::string& body, const std::string& soapActionHeader, SoapRequest& out,
                             std::string* err = nullptr) {
    auto fail = [&](const char* e) { if (err) *err = e; return false; };
    if (body.size() > kMaxSoapBody) return fail("size");
    XmlNode root;
    if (!parseXml(body, root)) return fail("xml");
    if (root.name != "Envelope") return fail("envelope");
    const XmlNode* b = root.child("Body");
    if (!b || b->children.empty()) return fail("envelope");
    const XmlNode& act = b->children.front();
    SoapRequest r;
    r.action = act.name;
    r.serviceType = act.ns;
    std::string hType, hAction;
    if (parseSoapActionHeader(soapActionHeader, hType, hAction)) {
        if (hAction != r.action) return fail("mismatch");
        r.serviceType = hType;
    }
    if (r.action.empty() || r.serviceType.empty()) return fail("action");
    for (const auto& c : act.children) {
        if (c.text.size() > kMaxArgBytes) return fail("size");
        r.args.emplace_back(c.name, c.text);
    }
    out = std::move(r);
    return true;
}

/** A sikeres válasz borítéka (UDA 1.1, 3.2.2): <u:{action}Response> a kimenő argumentumokkal. */
inline std::string soapResponse(const std::string& serviceType, const std::string& action,
                                const std::vector<std::pair<std::string, std::string>>& outArgs) {
    std::string s = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
                    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" +
                    action + "Response xmlns:u=\"" + xmlEscape(serviceType) + "\">";
    for (const auto& a : outArgs) s += "<" + a.first + ">" + xmlEscape(a.second) + "</" + a.first + ">";
    s += "</u:" + action + "Response></s:Body></s:Envelope>\r\n";
    return s;
}

/** A hibakódok leírása (UDA 1.1 3.2.2 és a szolgáltatássablonok hibatáblái). */
inline const char* errorText(int code) {
    switch (code) {
    case 401: return "Invalid Action";
    case 402: return "Invalid Args";
    case 501: return "Action Failed";
    case 600: return "Argument Value Invalid";
    case 601: return "Argument Value Out of Range";
    case 701: return "Transition not available";
    case 702: return "No contents";
    case 704: return "Format not supported for playback";
    case 706: return "Invalid connection reference";
    case 710: return "Seek mode not supported";
    case 711: return "Illegal seek target";
    case 712: return "Play mode not supported";
    case 714: return "Illegal MIME-type";
    case 716: return "Resource not found";
    case 717: return "Play speed not supported";
    case 718: return "Invalid InstanceID";
    default: return "Error";
    }
}

/** A hibaválasz borítéka (UDA 1.1, 3.2.2: s:Fault, faultcode s:Client, detail / UPnPError); a HTTP-állapot 500. */
inline std::string soapFault(int code, const std::string& desc = "") {
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
           "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
           "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><s:Fault>"
           "<faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>"
           "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>" +
           std::to_string(code) + "</errorCode><errorDescription>" +
           xmlEscape(desc.empty() ? errorText(code) : desc) +
           "</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>\r\n";
}

// ------------------------------------------------------------------ idő (AVTransport:1, 2.2.15 / 2.2.23)

/** Másodperc -> "H:MM:SS" (egész másodpercre lefelé; a negatív és a NaN 0:00:00). */
inline std::string formatTime(double sec) {
    if (!(sec > 0.0)) sec = 0.0;
    if (sec > 3600.0 * 999999) sec = 3600.0 * 999999;
    const uint64_t t = static_cast<uint64_t>(std::floor(sec));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu:%02u:%02u", static_cast<unsigned long long>(t / 3600),
                  static_cast<unsigned>((t / 60) % 60), static_cast<unsigned>(t % 60));
    return buf;
}

/**
 * "H+:MM:SS[.F+]" vagy "H+:MM:SS[.F0/F1]" (az elején egy '+' megengedett) -> másodperc. A perc és a másodperc
 * 0–59; false: hibás alak (negatív érték, hiányzó mező, betű).
 */
inline bool parseTime(const std::string& in, double& out) {
    std::string s = trim(in);
    if (!s.empty() && s[0] == '+') s.erase(0, 1);
    if (s.empty() || s.size() > 32) return false;
    const size_t c1 = s.find(':');
    if (c1 == std::string::npos) return false;
    const size_t c2 = s.find(':', c1 + 1);
    if (c2 == std::string::npos || s.find(':', c2 + 1) != std::string::npos) return false;
    auto digits = [](const std::string& t, uint64_t& v, size_t maxLen) {
        if (t.empty() || t.size() > maxLen) return false;
        v = 0;
        for (char c : t) {
            if (c < '0' || c > '9') return false;
            v = v * 10 + static_cast<uint64_t>(c - '0');
        }
        return true;
    };
    uint64_t h, m, sec;
    if (!digits(s.substr(0, c1), h, 9)) return false;
    const std::string mm = s.substr(c1 + 1, c2 - c1 - 1);
    if (mm.size() != 2 || !digits(mm, m, 2) || m > 59) return false;
    std::string rest = s.substr(c2 + 1);
    std::string frac;
    const size_t dot = rest.find('.');
    if (dot != std::string::npos) {
        frac = rest.substr(dot + 1);
        rest = rest.substr(0, dot);
    }
    if (rest.size() != 2 || !digits(rest, sec, 2) || sec > 59) return false;
    double f = 0.0;
    if (dot != std::string::npos) {
        const size_t slash = frac.find('/');
        if (slash != std::string::npos) {
            uint64_t a, b;
            if (!digits(frac.substr(0, slash), a, 9) || !digits(frac.substr(slash + 1), b, 9) || b == 0 || a >= b) return false;
            f = static_cast<double>(a) / static_cast<double>(b);
        } else {
            uint64_t a;
            if (!digits(frac, a, 9)) return false;
            f = static_cast<double>(a) / std::pow(10.0, static_cast<double>(frac.size()));
        }
    }
    out = static_cast<double>(h) * 3600.0 + static_cast<double>(m) * 60.0 + static_cast<double>(sec) + f;
    return true;
}

// ------------------------------------------------------------------ protocolInfo és formátumok

/** A protocolInfo 3. mezője (contentFormat, a MIME-típus kisbetűvel, paraméterek nélkül); üres: hibás. */
inline std::string protocolMime(const std::string& protocolInfo) {
    const size_t a = protocolInfo.find(':');
    if (a == std::string::npos) return {};
    const size_t b = protocolInfo.find(':', a + 1);
    if (b == std::string::npos) return {};
    const size_t c = protocolInfo.find(':', b + 1);
    if (c == std::string::npos) return {};
    std::string m = protocolInfo.substr(b + 1, c - b - 1);
    m = m.substr(0, m.find(';'));
    for (char& ch : m) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return trim(m);
}

/**
 * A MIME-típushoz tartozó fájlkiterjesztés a beépített dekóderek szerint (audio_decoder.hpp); "" ismeretlen,
 * "-" ismert, de nem játszható (pl. AAC MP4-ben, HLS).
 */
inline std::string extForMime(const std::string& mime) {
    static const std::pair<const char*, const char*> kMap[] = {
        { "audio/flac", "flac" }, { "audio/x-flac", "flac" },
        { "audio/wav", "wav" }, { "audio/x-wav", "wav" }, { "audio/wave", "wav" }, { "audio/vnd.wave", "wav" },
        { "audio/aiff", "aiff" }, { "audio/x-aiff", "aiff" },
        { "audio/mpeg", "mp3" }, { "audio/mp3", "mp3" }, { "audio/mpeg3", "mp3" }, { "audio/x-mpeg", "mp3" },
        { "audio/ogg", "ogg" }, { "application/ogg", "ogg" }, { "audio/x-ogg", "ogg" }, { "audio/vorbis", "ogg" },
        { "audio/opus", "opus" },
        { "audio/x-alac", "m4a" }, { "audio/alac", "m4a" }, { "audio/x-m4a", "m4a" }, { "audio/m4a", "m4a" },
        { "audio/mp4", "m4a" }, { "audio/x-caf", "caf" },
        { "audio/x-wavpack", "wv" }, { "audio/wavpack", "wv" },
        { "audio/dsf", "dsf" }, { "audio/x-dsf", "dsf" }, { "audio/dsd", "dsf" }, { "audio/x-dsd", "dsf" },
        { "audio/dff", "dff" }, { "audio/x-dff", "dff" },
        { "application/vnd.apple.mpegurl", "-" }, { "application/x-mpegurl", "-" }, { "audio/l16", "-" },
    };
    for (const auto& e : kMap)
        if (mime == e.first) return e.second;
    return {};
}

/** A fájl első bájtjai alapján a kiterjesztés (a MIME-típus megbízhatatlan lehet); "" ismeretlen. */
inline std::string sniffExt(const uint8_t* p, size_t n) {
    auto at = [&](size_t off, const char* t) { const size_t l = std::strlen(t); return n >= off + l && std::memcmp(p + off, t, l) == 0; };
    if (at(0, "fLaC")) return "flac";
    if (at(0, "RIFF") && at(8, "WAVE")) return "wav";
    if (at(0, "RF64") || (at(0, "BW64"))) return "wav";
    if (at(0, "FORM") && (at(8, "AIFF") || at(8, "AIFC"))) return "aiff";
    if (at(0, "FRM8")) return "dff";
    if (at(0, "DSD ")) return "dsf";
    if (at(0, "caff")) return "caf";
    if (at(0, "wvpk")) return "wv";
    if (at(4, "ftyp")) return "m4a";
    if (at(0, "OggS")) {
        for (size_t i = 0; i + 8 <= std::min<size_t>(n, 128); ++i)
            if (std::memcmp(p + i, "OpusHead", 8) == 0) return "opus";
        return "ogg";
    }
    if (at(0, "ID3")) return "mp3";
    if (n >= 2 && p[0] == 0xFF && (p[1] & 0xE0) == 0xE0 && ((p[1] >> 1) & 3) != 0) return "mp3";
    return {};
}

/** A Sink protocolInfo (ConnectionManager GetProtocolInfo): csak amit a dekóderek tényleg lejátszanak. */
inline std::string sinkProtocolInfo() {
    static const char* kMimes[] = {
        "audio/flac", "audio/x-flac", "audio/wav", "audio/x-wav", "audio/wave", "audio/aiff", "audio/x-aiff",
        "audio/mpeg", "audio/ogg", "application/ogg", "audio/opus", "audio/x-alac", "audio/x-m4a", "audio/mp4",
        "audio/x-caf", "audio/x-wavpack", "audio/dsf", "audio/x-dsf", "audio/dff", "audio/x-dff",
        "audio/aac", "audio/aacp",   // csak élő folyamként (internetrádió), a rádió dekóderével
    };
    std::string s;
    for (const char* m : kMimes) {
        if (!s.empty()) s += ",";
        s += std::string("http-get:*:") + m + ":*";
    }
    return s;
}

// ------------------------------------------------------------------ DIDL-Lite

struct DidlRes {
    std::string url, protocolInfo;
    double duration = 0.0;   // másodperc (0: ismeretlen)
    uint64_t size = 0;       // bájt (0: ismeretlen)
};

struct DidlItem {
    std::string title, artist, album, artUri, upnpClass;
    std::vector<DidlRes> res;
    /** Élő folyam (internetrádió): az upnp:class szerint. */
    bool broadcast() const { return upnpClass.find("audioBroadcast") != std::string::npos; }
};

/** Az első item (vagy container) adatai; false: nem DIDL-Lite (vagy üres / hibás). */
inline bool parseDidl(const std::string& xmlText, DidlItem& out) {
    if (trim(xmlText).empty()) return false;
    XmlNode root;
    if (!parseXml(xmlText, root) || root.name != "DIDL-Lite") return false;
    const XmlNode* it = root.child("item");
    if (!it) it = root.child("container");
    if (!it) return false;
    DidlItem d;
    d.title = radio::cleanText(trim(it->childText("title")), 256);
    std::string artist;
    for (const auto& c : it->children) {
        if (c.name == "artist" && artist.empty()) artist = trim(c.text);   // az első upnp:artist (role nélkül vagy bármi)
    }
    if (artist.empty()) artist = trim(it->childText("creator"));
    d.artist = radio::cleanText(artist, 256);
    d.album = radio::cleanText(trim(it->childText("album")), 256);
    d.artUri = trim(it->childText("albumArtURI"));
    d.upnpClass = trim(it->childText("class"));
    for (const auto& c : it->children) {
        if (c.name != "res" || d.res.size() >= 16) continue;
        DidlRes r;
        r.url = trim(c.text);
        r.protocolInfo = c.attr("protocolInfo");
        double dur = 0;
        if (parseTime(c.attr("duration"), dur)) r.duration = dur;
        const std::string sz = c.attr("size");
        uint64_t v = 0;
        bool ok = !sz.empty() && sz.size() <= 19;
        for (char ch : sz) {
            if (ch < '0' || ch > '9') { ok = false; break; }
            v = v * 10 + static_cast<uint64_t>(ch - '0');
        }
        if (ok) r.size = v;
        d.res.push_back(std::move(r));
    }
    out = std::move(d);
    return true;
}

/** A DIDL-Lite <res>-e, amely ehhez a címhez tartozik (vagy az első); nullptr: nincs. */
inline const DidlRes* didlResFor(const DidlItem& d, const std::string& url) {
    for (const auto& r : d.res)
        if (r.url == url) return &r;
    return d.res.empty() ? nullptr : &d.res.front();
}

// ------------------------------------------------------------------ események (UDA 1.1, 4.3; AVT / RCS LastChange)

constexpr const char* kAvtEventNs = "urn:schemas-upnp-org:metadata-1-0/AVT/";
constexpr const char* kRcsEventNs = "urn:schemas-upnp-org:metadata-1-0/RCS/";

struct EventVar {
    std::string name, value, channel;   // channel: a RenderingControl Volume / Mute változójánál "Master"
};

/** A LastChange értéke (még escape nélkül): <Event xmlns="…"><InstanceID val="0"><Név val="…"/>…</InstanceID></Event> */
inline std::string lastChangeXml(const char* ns, const std::vector<EventVar>& vars) {
    std::string s = std::string("<Event xmlns=\"") + ns + "\"><InstanceID val=\"0\">";
    for (const auto& v : vars) {
        s += "<" + v.name;
        if (!v.channel.empty()) s += " channel=\"" + xmlEscape(v.channel) + "\"";
        s += " val=\"" + xmlEscape(v.value) + "\"/>";
    }
    return s + "</InstanceID></Event>";
}

/** A GENA NOTIFY törzse (UDA 1.1, 4.3.2): e:propertyset, változónként egy e:property. */
inline std::string propertySet(const std::vector<std::pair<std::string, std::string>>& props) {
    std::string s = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
                    "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">";
    for (const auto& p : props) s += "<e:property><" + p.first + ">" + xmlEscape(p.second) + "</" + p.first + "></e:property>";
    return s + "</e:propertyset>\r\n";
}

// ------------------------------------------------------------------ címszabály (a terv 1. része, „Hálózati biztonság”)

/** Egy helyi IPv4-interfész: cím és maszk (hoszt-bájtsorrendben). */
struct Iface {
    uint32_t ip = 0, mask = 0;
    std::string name;
    bool contains(uint32_t a) const { return mask != 0 && (a & mask) == (ip & mask); }
};

inline uint32_t v4(const uint8_t a[4]) {
    return (uint32_t(a[0]) << 24) | (uint32_t(a[1]) << 16) | (uint32_t(a[2]) << 8) | a[3];
}
inline bool parseV4(const std::string& s, uint32_t& out) {
    unsigned b[4];
    char tail;
    if (std::sscanf(s.c_str(), "%u.%u.%u.%u%c", &b[0], &b[1], &b[2], &b[3], &tail) != 4) return false;
    for (unsigned x : b)
        if (x > 255) return false;
    for (char c : s)
        if ((c < '0' || c > '9') && c != '.') return false;
    out = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
    return true;
}
inline std::string v4Text(uint32_t a) {
    return std::to_string(a >> 24) + "." + std::to_string((a >> 16) & 255) + "." + std::to_string((a >> 8) & 255) + "." +
           std::to_string(a & 255);
}
/** RFC 1918 magánhálózat (10/8, 172.16/12, 192.168/16). */
inline bool isPrivateV4(uint32_t a) {
    return (a >> 24) == 10 || (a >> 20) == 0xAC1 || (a >> 16) == 0xC0A8;
}

/**
 * Egy médiacím (a feloldott cím) megengedett-e, ha a SOAP-kérés a `via` interfészen jött:
 *  - nyilvános cím (radio_net.hpp isPublicV4 / isPublicV6): igen;
 *  - RFC 1918 magáncím: csak ha a `via` alhálózatában van;
 *  - minden más (loopback, link-local 169.254/16 és fe80::/10, multicast, fenntartott, ULA, a szolgáltatói NAT) nem.
 * Az IPv6-ba ágyazott IPv4 (::ffff:a.b.c.d) az IPv4-szabály szerint.
 */
inline bool mediaAddrAllowed(int family, const uint8_t* a, const Iface& via) {
    if (family == 4) {
        if (radio::isPublicV4(a)) return true;
        const uint32_t x = v4(a);
        return isPrivateV4(x) && via.contains(x) && isPrivateV4(via.ip);
    }
    if (family == 6) {
        static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
        if (std::memcmp(a, mapped, 12) == 0) return mediaAddrAllowed(4, a + 12, via);
        return radio::isPublicV6(a);
    }
    return false;
}

/**
 * A médiacím alaki ellenőrzése (a névfeloldás előtt): csak http(s), felhasználónév nélkül; ha a hoszt IP-cím, a
 * mediaAddrAllowed is. "" rendben (out: a felbontott cím), különben a hiba kódja: "url", "userinfo", "scheme",
 * "blocked".
 */
inline std::string checkMediaUrl(const std::string& url, const Iface& via, radio::RadioUrl& out) {
    std::string err;
    radio::RadioUrl u;
    if (!radio::parseRadioUrl(url, u, &err)) return err == "userinfo" || err == "scheme" ? err : std::string("url");
    if (u.ipv6) {
        uint8_t b[16];
        if (inet_pton(AF_INET6, u.host.c_str(), b) != 1) return "url";
        if (!mediaAddrAllowed(6, b, via)) return "blocked";
    } else {
        uint32_t x;
        bool literal = true;
        for (char c : u.host)
            if ((c < '0' || c > '9') && c != '.') literal = false;
        if (literal) {
            if (!parseV4(u.host, x)) return "url";
            const uint8_t b[4] = { uint8_t(x >> 24), uint8_t(x >> 16), uint8_t(x >> 8), uint8_t(x) };
            if (!mediaAddrAllowed(4, b, via)) return "blocked";
        } else if (u.host == "localhost" || (u.host.size() > 10 && u.host.compare(u.host.size() - 10, 10, ".localhost") == 0)) {
            return "blocked";
        }
    }
    out = u;
    return "";
}

/**
 * A GENA visszahívási cím (CALLBACK: <http://…>) szabálya: csak http://, IPv4-cím (névfeloldás nélkül), a kérés
 * interfészének alhálózatából (a loopback nem). Több cím esetén az első megfelelő; false: nincs ilyen.
 */
inline bool parseCallback(const std::string& header, const Iface& via, radio::RadioUrl& out) {
    size_t p = 0;
    while ((p = header.find('<', p)) != std::string::npos) {
        const size_t e = header.find('>', p);
        if (e == std::string::npos) return false;
        const std::string url = header.substr(p + 1, e - p - 1);
        p = e + 1;
        radio::RadioUrl u;
        if (!radio::parseRadioUrl(url, u, nullptr) || u.https || u.ipv6) continue;
        uint32_t x;
        if (!parseV4(u.host, x)) continue;
        if (!isPrivateV4(x) || !via.contains(x)) continue;
        out = u;
        return true;
    }
    return false;
}

} // namespace ee_analyzer::upnp
