#pragma once
/**
 * @file upnp_http.hpp
 * @brief UPnP-renderer: kis HTTP/1.1 szerver a helyi hálózat felé – eszközleírás, a három szolgáltatás leírása
 *        (SCPD), SOAP-vezérlés és GENA-események – és a szolgáltatás összerakása (SSDP + HTTP + Renderer).
 *
 * Források: UPnP Device Architecture 1.1 (2008):
 *  - 2. fejezet (Description): az eszközleírás (root, specVersion, device: deviceType, friendlyName, manufacturer,
 *    modelName, UDN, serviceList) és a szolgáltatásleírás (scpd: actionList, serviceStateTable);
 *  - 3. fejezet (Control): POST a controlURL-re, SOAPACTION fejléc, a válasz és a hiba (HTTP 500, UPnPError);
 *  - 4. fejezet (Eventing): SUBSCRIBE (CALLBACK, NT: upnp:event, TIMEOUT: Second-N), megújítás (SID), UNSUBSCRIBE,
 *    a hibák (400, 412), a válasz (SID, TIMEOUT), a kezdő esemény (SEQ 0) és a NOTIFY (NT: upnp:event,
 *    NTS: upnp:propchange, SID, SEQ; a SEQ 4294967295 után 1-re fordul).
 * A szolgáltatások változói és műveletei: AVTransport:1, RenderingControl:1, ConnectionManager:1 Service Template 1.01.
 *
 * Biztonság (a terv 1. része): csak magánhálózati (RFC 1918) interfészen érkező kapcsolatot fogad, és csak abból az
 * alhálózatból, amelyhez az interfész tartozik (opcionálisan csak az engedélyezett vezérlő-címekről). A SOAP-törzs
 * legfeljebb 64 KiB, a fejléc 8 KiB, legfeljebb 16 egyidejű kapcsolat, legfeljebb 30 GENA-feliratkozás lejáró
 * időzítővel, a visszahívási cím csak http:// a kérés alhálózatából (upnp_soap.hpp parseCallback). Hitelesítés
 * nincs (a UPnP-ben nincs ilyen): a helyi hálózaton bárki vezérelheti, ezt a felület kimondja.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "media_session.hpp"
#include "net_compat.hpp"
#include "upnp_renderer.hpp"
#include "upnp_soap.hpp"
#include "upnp_ssdp.hpp"

namespace ee_analyzer::upnp {

// ------------------------------------------------------------------ leírások (UDA 1.1, 2. fejezet)

/** Állandó eszközazonosító (UDN) a gép nevéből: újraindítás után ugyanaz, így a vezérlő megjegyzi. */
inline std::string stableUuid(const std::string& seed) {
    const std::string a = media::hashId("aqulify-upnp-a|" + seed), b = media::hashId("aqulify-upnp-b|" + seed);
    std::string h = a + b;   // 32 hexa jegy
    h[12] = '5';                                                     // verzió (név alapú)
    h[16] = "89ab"[std::stoi(h.substr(16, 1), nullptr, 16) & 3];      // RFC 4122 változat
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20, 12);
}

inline std::string deviceDescription(const std::string& friendlyName, const std::string& uuid) {
    auto svc = [](const char* type, const char* id, const char* name, const char* key) {
        return std::string("<service><serviceType>") + type + "</serviceType><serviceId>urn:upnp-org:serviceId:" + id +
               "</serviceId><SCPDURL>/" + name + ".xml</SCPDURL><controlURL>/ctl/" + key + "</controlURL><eventSubURL>/evt/" +
               key + "</eventSubURL></service>";
    };
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
           "<root xmlns=\"urn:schemas-upnp-org:device-1-0\"><specVersion><major>1</major><minor>1</minor></specVersion>"
           "<device><deviceType>" + std::string(kRendererType) + "</deviceType><friendlyName>" + xmlEscape(friendlyName) +
           "</friendlyName><manufacturer>Aqulify</manufacturer>"
           "<manufacturerURL>https://github.com/Szorgos/ee-phase-waterfall-analyzer</manufacturerURL>"
           "<modelDescription>Aqulify audio player (UPnP MediaRenderer)</modelDescription><modelName>Aqulify</modelName>"
           "<modelNumber>" AQULIFY_VERSION "</modelNumber><UDN>uuid:" + uuid + "</UDN><serviceList>" +
           svc(kAvtType, "AVTransport", "AVTransport", "avt") + svc(kRcsType, "RenderingControl", "RenderingControl", "rcs") +
           svc(kCmType, "ConnectionManager", "ConnectionManager", "cm") + "</serviceList></device></root>\r\n";
}

namespace scpd_detail {
struct Arg { const char* name; bool out; const char* var; };
struct Var { const char* name; const char* type; bool events; std::vector<const char*> allowed; int min = -1, max = -1; };

inline std::string build(const std::vector<std::pair<const char*, std::vector<Arg>>>& actions, const std::vector<Var>& vars) {
    std::string s = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
                    "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\"><specVersion><major>1</major><minor>1</minor></specVersion>"
                    "<actionList>";
    for (const auto& a : actions) {
        s += std::string("<action><name>") + a.first + "</name>";
        if (!a.second.empty()) {
            s += "<argumentList>";
            for (const auto& g : a.second)
                s += std::string("<argument><name>") + g.name + "</name><direction>" + (g.out ? "out" : "in") +
                     "</direction><relatedStateVariable>" + g.var + "</relatedStateVariable></argument>";
            s += "</argumentList>";
        }
        s += "</action>";
    }
    s += "</actionList><serviceStateTable>";
    for (const auto& v : vars) {
        s += std::string("<stateVariable sendEvents=\"") + (v.events ? "yes" : "no") + "\"><name>" + v.name +
             "</name><dataType>" + v.type + "</dataType>";
        if (!v.allowed.empty()) {
            s += "<allowedValueList>";
            for (const char* x : v.allowed) s += std::string("<allowedValue>") + x + "</allowedValue>";
            s += "</allowedValueList>";
        }
        if (v.min >= 0)
            s += "<allowedValueRange><minimum>" + std::to_string(v.min) + "</minimum><maximum>" + std::to_string(v.max) +
                 "</maximum><step>1</step></allowedValueRange>";
        s += "</stateVariable>";
    }
    return s + "</serviceStateTable></scpd>\r\n";
}
} // namespace scpd_detail

/** AVTransport:1 (Service Template 1.01, 2.2 és 2.4): a kötelező műveletek, valamint Pause, SetNextAVTransportURI,
 *  SetPlayMode és GetCurrentTransportActions. Felvétel nincs (NOT_IMPLEMENTED). */
inline std::string scpdAvTransport() {
    using namespace scpd_detail;
    const Arg id{ "InstanceID", false, "A_ARG_TYPE_InstanceID" };
    return build(
        { { "SetAVTransportURI", { id, { "CurrentURI", false, "AVTransportURI" }, { "CurrentURIMetaData", false, "AVTransportURIMetaData" } } },
          { "SetNextAVTransportURI", { id, { "NextURI", false, "NextAVTransportURI" }, { "NextURIMetaData", false, "NextAVTransportURIMetaData" } } },
          { "GetMediaInfo", { id, { "NrTracks", true, "NumberOfTracks" }, { "MediaDuration", true, "CurrentMediaDuration" },
                              { "CurrentURI", true, "AVTransportURI" }, { "CurrentURIMetaData", true, "AVTransportURIMetaData" },
                              { "NextURI", true, "NextAVTransportURI" }, { "NextURIMetaData", true, "NextAVTransportURIMetaData" },
                              { "PlayMedium", true, "PlaybackStorageMedium" }, { "RecordMedium", true, "RecordStorageMedium" },
                              { "WriteStatus", true, "RecordMediumWriteStatus" } } },
          { "GetTransportInfo", { id, { "CurrentTransportState", true, "TransportState" },
                                  { "CurrentTransportStatus", true, "TransportStatus" }, { "CurrentSpeed", true, "TransportPlaySpeed" } } },
          { "GetPositionInfo", { id, { "Track", true, "CurrentTrack" }, { "TrackDuration", true, "CurrentTrackDuration" },
                                 { "TrackMetaData", true, "CurrentTrackMetaData" }, { "TrackURI", true, "CurrentTrackURI" },
                                 { "RelTime", true, "RelativeTimePosition" }, { "AbsTime", true, "AbsoluteTimePosition" },
                                 { "RelCount", true, "RelativeCounterPosition" }, { "AbsCount", true, "AbsoluteCounterPosition" } } },
          { "GetDeviceCapabilities", { id, { "PlayMedia", true, "PossiblePlaybackStorageMedia" },
                                       { "RecMedia", true, "PossibleRecordStorageMedia" },
                                       { "RecQualityModes", true, "PossibleRecordQualityModes" } } },
          { "GetTransportSettings", { id, { "PlayMode", true, "CurrentPlayMode" }, { "RecQualityMode", true, "CurrentRecordQualityMode" } } },
          { "Stop", { id } },
          { "Play", { id, { "Speed", false, "TransportPlaySpeed" } } },
          { "Pause", { id } },
          { "Seek", { id, { "Unit", false, "A_ARG_TYPE_SeekMode" }, { "Target", false, "A_ARG_TYPE_SeekTarget" } } },
          { "Next", { id } },
          { "Previous", { id } },
          { "SetPlayMode", { id, { "NewPlayMode", false, "CurrentPlayMode" } } },
          { "GetCurrentTransportActions", { id, { "Actions", true, "CurrentTransportActions" } } } },
        { { "TransportState", "string", false, { "STOPPED", "PLAYING", "TRANSITIONING", "PAUSED_PLAYBACK", "NO_MEDIA_PRESENT" } },
          { "TransportStatus", "string", false, { "OK", "ERROR_OCCURRED" } },
          { "PlaybackStorageMedium", "string", false, { "NETWORK", "NONE" } },
          { "RecordStorageMedium", "string", false, { "NOT_IMPLEMENTED" } },
          { "PossiblePlaybackStorageMedia", "string", false, {} },
          { "PossibleRecordStorageMedia", "string", false, {} },
          { "CurrentPlayMode", "string", false, { "NORMAL" } },
          { "TransportPlaySpeed", "string", false, { "1" } },
          { "RecordMediumWriteStatus", "string", false, { "NOT_IMPLEMENTED" } },
          { "CurrentRecordQualityMode", "string", false, { "NOT_IMPLEMENTED" } },
          { "PossibleRecordQualityModes", "string", false, {} },
          { "NumberOfTracks", "ui4", false, {}, 0, 1 },
          { "CurrentTrack", "ui4", false, {}, 0, 1 },
          { "CurrentTrackDuration", "string", false, {} },
          { "CurrentMediaDuration", "string", false, {} },
          { "CurrentTrackMetaData", "string", false, {} },
          { "CurrentTrackURI", "string", false, {} },
          { "AVTransportURI", "string", false, {} },
          { "AVTransportURIMetaData", "string", false, {} },
          { "NextAVTransportURI", "string", false, {} },
          { "NextAVTransportURIMetaData", "string", false, {} },
          { "RelativeTimePosition", "string", false, {} },
          { "AbsoluteTimePosition", "string", false, {} },
          { "RelativeCounterPosition", "i4", false, {} },
          { "AbsoluteCounterPosition", "i4", false, {} },
          { "CurrentTransportActions", "string", false, {} },
          { "LastChange", "string", true, {} },
          { "A_ARG_TYPE_SeekMode", "string", false, { "ABS_TIME", "REL_TIME", "TRACK_NR" } },
          { "A_ARG_TYPE_SeekTarget", "string", false, {} },
          { "A_ARG_TYPE_InstanceID", "ui4", false, {} } });
}

/** RenderingControl:1 (Service Template 1.01): ListPresets, SelectPreset (kötelező), Get/SetVolume, Get/SetMute. */
inline std::string scpdRenderingControl() {
    using namespace scpd_detail;
    const Arg id{ "InstanceID", false, "A_ARG_TYPE_InstanceID" };
    const Arg ch{ "Channel", false, "A_ARG_TYPE_Channel" };
    return build({ { "ListPresets", { id, { "CurrentPresetNameList", true, "PresetNameList" } } },
                   { "SelectPreset", { id, { "PresetName", false, "A_ARG_TYPE_PresetName" } } },
                   { "GetMute", { id, ch, { "CurrentMute", true, "Mute" } } },
                   { "SetMute", { id, ch, { "DesiredMute", false, "Mute" } } },
                   { "GetVolume", { id, ch, { "CurrentVolume", true, "Volume" } } },
                   { "SetVolume", { id, ch, { "DesiredVolume", false, "Volume" } } } },
                 { { "PresetNameList", "string", false, {} },
                   { "LastChange", "string", true, {} },
                   { "Mute", "boolean", false, {} },
                   { "Volume", "ui2", false, {}, 0, 100 },
                   { "A_ARG_TYPE_Channel", "string", false, { "Master" } },
                   { "A_ARG_TYPE_InstanceID", "ui4", false, {} },
                   { "A_ARG_TYPE_PresetName", "string", false, { "FactoryDefaults" } } });
}

/** ConnectionManager:1 (Service Template 1.01): a kötelező műveletek (PrepareForConnection / ConnectionComplete nincs). */
inline std::string scpdConnectionManager() {
    using namespace scpd_detail;
    return build({ { "GetProtocolInfo", { { "Source", true, "SourceProtocolInfo" }, { "Sink", true, "SinkProtocolInfo" } } },
                   { "GetCurrentConnectionIDs", { { "ConnectionIDs", true, "CurrentConnectionIDs" } } },
                   { "GetCurrentConnectionInfo", { { "ConnectionID", false, "A_ARG_TYPE_ConnectionID" },
                                                   { "RcsID", true, "A_ARG_TYPE_RcsID" },
                                                   { "AVTransportID", true, "A_ARG_TYPE_AVTransportID" },
                                                   { "ProtocolInfo", true, "A_ARG_TYPE_ProtocolInfo" },
                                                   { "PeerConnectionManager", true, "A_ARG_TYPE_ConnectionManager" },
                                                   { "PeerConnectionID", true, "A_ARG_TYPE_ConnectionID" },
                                                   { "Direction", true, "A_ARG_TYPE_Direction" },
                                                   { "Status", true, "A_ARG_TYPE_ConnectionStatus" } } } },
                 { { "SourceProtocolInfo", "string", true, {} },
                   { "SinkProtocolInfo", "string", true, {} },
                   { "CurrentConnectionIDs", "string", true, {} },
                   { "A_ARG_TYPE_ConnectionStatus", "string", false,
                     { "OK", "ContentFormatMismatch", "InsufficientBandwidth", "UnreliableChannel", "Unknown" } },
                   { "A_ARG_TYPE_ConnectionManager", "string", false, {} },
                   { "A_ARG_TYPE_Direction", "string", false, { "Input", "Output" } },
                   { "A_ARG_TYPE_ProtocolInfo", "string", false, {} },
                   { "A_ARG_TYPE_ConnectionID", "i4", false, {} },
                   { "A_ARG_TYPE_AVTransportID", "i4", false, {} },
                   { "A_ARG_TYPE_RcsID", "i4", false, {} } });
}

// ------------------------------------------------------------------ HTTP-kérés

struct HttpRequest {
    std::string method, path;
    std::map<std::string, std::string> headers;   // kisbetűs kulcs
    std::string body;
    std::string get(const char* k) const {
        auto it = headers.find(k);
        return it == headers.end() ? std::string() : it->second;
    }
};

/** A kérés fejlécének bontása (a záró üres sor nélkül); false: hibás. */
inline bool parseRequestHead(const std::string& head, HttpRequest& r) {
    const size_t eol = head.find("\r\n");
    const std::string first = head.substr(0, eol);
    const size_t a = first.find(' '), b = first.rfind(' ');
    if (a == std::string::npos || b == a || first.compare(b + 1, 5, "HTTP/") != 0) return false;
    r.method = first.substr(0, a);
    r.path = first.substr(a + 1, b - a - 1);
    if (r.path.empty() || r.path[0] != '/') {
        // abszolút alak (http://ip:port/út): csak az út kell
        const size_t p = r.path.find("://");
        if (p == std::string::npos) return false;
        const size_t s = r.path.find('/', p + 3);
        r.path = s == std::string::npos ? "/" : r.path.substr(s);
    }
    const size_t q = r.path.find('?');
    if (q != std::string::npos) r.path.resize(q);
    size_t pos = eol == std::string::npos ? head.size() : eol + 2;
    while (pos < head.size()) {
        size_t e = head.find("\r\n", pos);
        if (e == std::string::npos) e = head.size();
        const std::string line = head.substr(pos, e - pos);
        pos = e + 2;
        const size_t c = line.find(':');
        if (c == std::string::npos || c == 0) continue;
        std::string k = line.substr(0, c);
        for (char& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        r.headers[k] = trim(line.substr(c + 1));
    }
    return true;
}

// ------------------------------------------------------------------ GENA (UDA 1.1, 4. fejezet)

/** "Second-N" / "infinite" -> másodperc a [60, 1800] tartományban (alapból 1800). */
inline int parseGenaTimeout(const std::string& v) {
    std::string s = trim(v);
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s.rfind("second-", 0) == 0) {
        const std::string n = s.substr(7);
        if (!n.empty() && n.size() <= 9 && n.find_first_not_of("0123456789") == std::string::npos)
            return std::clamp(std::stoi(n), 60, 1800);
    }
    return 1800;
}

class Gena {
public:
    static constexpr size_t kMaxSubs = 30;
    /** Az új feliratkozó kezdő eseményének tartalma (service: 0 AVT, 1 RCS, 2 CM): változó -> érték. */
    using Initial = std::function<std::vector<std::pair<std::string, std::string>>(int service)>;

    explicit Gena(Initial initial) : initial_(std::move(initial)) { th_ = std::thread([this] { loop(); }); }
    ~Gena() {
        {
            std::lock_guard<std::mutex> lk(m_);
            quit_ = true;
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }

    /** Új feliratkozás; 0: rendben (sid, timeout), különben a HTTP-hibakód. A kezdő eseményt a queueInitial küldi. */
    int subscribe(int service, const radio::RadioUrl& cb, int timeout, std::string& sid) {
        std::lock_guard<std::mutex> lk(m_);
        expireLocked();
        if (subs_.size() >= kMaxSubs) return 503;
        Sub s;
        s.service = service;
        s.cb = cb;
        s.expires = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
        sid = "uuid:" + randomUuid();
        subs_[sid] = s;
        return 0;
    }
    int renew(int service, const std::string& sid, int timeout) {
        std::lock_guard<std::mutex> lk(m_);
        expireLocked();
        auto it = subs_.find(sid);
        if (it == subs_.end() || it->second.service != service) return 412;
        it->second.expires = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
        return 0;
    }
    int unsubscribe(int service, const std::string& sid) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = subs_.find(sid);
        if (it == subs_.end() || it->second.service != service) return 412;
        subs_.erase(it);
        return 0;
    }
    /** A kezdő esemény (SEQ 0) a válasz elküldése után. */
    void queueInitial(int service, const std::string& sid) {
        const auto props = initial_ ? initial_(service) : std::vector<std::pair<std::string, std::string>>{};
        std::lock_guard<std::mutex> lk(m_);
        pushLocked(sid, propertySet(props));
        cv_.notify_all();
    }
    /** Esemény minden feliratkozónak (a LastChange-dzsel). */
    void broadcast(int service, const std::vector<std::pair<std::string, std::string>>& props) {
        const std::string body = propertySet(props);
        std::lock_guard<std::mutex> lk(m_);
        for (const auto& s : subs_)
            if (s.second.service == service) pushLocked(s.first, body);
        cv_.notify_all();
    }
    size_t count() const { std::lock_guard<std::mutex> lk(m_); return subs_.size(); }
    void clear() { std::lock_guard<std::mutex> lk(m_); subs_.clear(); queue_.clear(); }

private:
    struct Sub {
        int service = 0;
        radio::RadioUrl cb;
        std::chrono::steady_clock::time_point expires;
        uint32_t seq = 0;
        int failures = 0;
    };
    struct Item { std::string sid, body; };

    std::string randomUuid() {
        std::uniform_int_distribution<uint32_t> d;
        char buf[40];
        const uint32_t a = d(rng_), b = d(rng_), c = d(rng_), e = d(rng_);
        std::snprintf(buf, sizeof(buf), "%08x-%04x-4%03x-%04x-%04x%08x", a, b >> 16, b & 0xFFF, 0x8000 | (c & 0x3FFF), c >> 16, e);
        return buf;
    }
    void expireLocked() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = subs_.begin(); it != subs_.end();) {
            if (it->second.expires <= now) it = subs_.erase(it);
            else ++it;
        }
    }
    void pushLocked(const std::string& sid, const std::string& body) {
        if (queue_.size() >= 256) queue_.pop_front();
        queue_.push_back({ sid, body });
    }

    /** NOTIFY egy feliratkozónak (3 s kapcsolódás, 3 s válasz); igaz: 2xx. */
    static bool deliver(const radio::RadioUrl& cb, const std::string& sid, uint32_t seq, const std::string& body) {
        uint32_t ip;
        if (!parseV4(cb.host, ip)) return false;
        net::socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == net::kInvalidSocket) return false;
        net::setNonBlocking(s, true);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(cb.effectivePort());
        a.sin_addr.s_addr = htonl(ip);
        ::connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s, &w);
        timeval tv{ 3, 0 };
#ifdef _WIN32
        const int r = ::select(0, nullptr, &w, nullptr, &tv);
#else
        const int r = ::select(s + 1, nullptr, &w, nullptr, &tv);
#endif
        int soErr = 0;
#ifdef _WIN32
        int el = sizeof(soErr);
#else
        socklen_t el = sizeof(soErr);
#endif
        if (r <= 0 || getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &el) != 0 || soErr != 0) {
            net::closeSocket(s);
            return false;
        }
        net::setNonBlocking(s, false);
        const std::string req = "NOTIFY " + cb.target + " HTTP/1.1\r\nHOST: " + cb.host + ":" + std::to_string(cb.effectivePort()) +
                                "\r\nCONTENT-TYPE: text/xml; charset=\"utf-8\"\r\nNT: upnp:event\r\nNTS: upnp:propchange\r\nSID: " + sid +
                                "\r\nSEQ: " + std::to_string(seq) + "\r\nCONTENT-LENGTH: " + std::to_string(body.size()) +
                                "\r\nCONNECTION: close\r\n\r\n" + body;
        size_t off = 0;
        while (off < req.size()) {
            const long long n = net::sendSome(s, req.data() + off, req.size() - off);
            if (n <= 0) { net::closeSocket(s); return false; }
            off += static_cast<size_t>(n);
        }
        char buf[64] = { 0 };
        bool ok = false;
        if (waitReadable(s, 3000)) {
            const long long n = net::recvSome(s, buf, sizeof(buf) - 1);
            ok = n >= 12 && std::strncmp(buf, "HTTP/1.", 7) == 0 && buf[9] == '2';
        }
        net::closeSocket(s);
        return ok;
    }

    void loop() {
        std::unique_lock<std::mutex> lk(m_);
        while (!quit_) {
            cv_.wait_for(lk, std::chrono::seconds(5), [this] { return quit_ || !queue_.empty(); });
            if (quit_) break;
            expireLocked();
            if (queue_.empty()) continue;
            Item it = std::move(queue_.front());
            queue_.pop_front();
            auto s = subs_.find(it.sid);
            if (s == subs_.end()) continue;
            const radio::RadioUrl cb = s->second.cb;
            const uint32_t seq = s->second.seq;
            s->second.seq = seq == 0xFFFFFFFFu ? 1 : seq + 1;   // UDA 1.1, 4.3.2: a 0 csak a kezdő eseményé
            lk.unlock();
            const bool ok = deliver(cb, it.sid, seq, it.body);
            lk.lock();
            s = subs_.find(it.sid);
            if (s == subs_.end()) continue;
            s->second.failures = ok ? 0 : s->second.failures + 1;
            if (s->second.failures >= 5) subs_.erase(s);   // elérhetetlen vezérlő: ne tartsa fel a többit
        }
    }

    Initial initial_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::thread th_;
    bool quit_ = false;
    std::map<std::string, Sub> subs_;
    std::deque<Item> queue_;
    std::mt19937 rng_{ std::random_device{}() };
};

// ------------------------------------------------------------------ a szolgáltatás (SSDP + HTTP + Renderer)

class UpnpServer {
public:
    struct Config {
        std::string name;                 // a vezérlőkben látszó név
        std::string uuid;                 // az eszköz azonosítója (stableUuid)
        int port = 49494;                 // a HTTP-port (foglaltnál a következő 20 közül az első szabad)
        std::vector<uint32_t> allow;      // ha nem üres: csak ezekről a vezérlő-címekről
    };
    static constexpr int kMaxConnections = 16;
    static constexpr size_t kMaxHeadBytes = 8 * 1024;

    explicit UpnpServer(Renderer& r) : r_(r), gena_([this](int svc) { return initialProps(svc); }) {
        r_.setEventSink([this](int service, const std::string& xml) { gena_.broadcast(service, { { "LastChange", xml } }); });
    }
    ~UpnpServer() {
        stop();
        r_.setEventSink(nullptr);
    }
    UpnpServer(const UpnpServer&) = delete;
    UpnpServer& operator=(const UpnpServer&) = delete;

    /** Indítás; false: a port nem nyitható (err). */
    bool start(const Config& c, std::string& err) {
        stop();
        if (!net::startup()) { err = "socket"; return false; }
        cfg_ = c;
        for (int p = c.port; p < c.port + 20 && listener_ == net::kInvalidSocket; ++p) {
            listener_ = openListener(p);
            if (listener_ != net::kInvalidSocket) port_ = p;
        }
        if (listener_ == net::kInvalidSocket) { err = "port"; net::cleanup(); return false; }
        running_ = true;
        acceptTh_ = std::thread([this] { acceptLoop(); });
        ssdp_.start(ssdp::targets(c.uuid, kRendererType, { kAvtType, kRcsType, kCmType }),
                    [this](const std::string& ip) { return "http://" + ip + ":" + std::to_string(port_) + "/description.xml"; });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        ssdp_.stop();
        if (acceptTh_.joinable()) acceptTh_.join();
        {
            std::lock_guard<std::mutex> lk(connM_);
            for (auto s : open_) ::shutdown(s, 2);
        }
        for (;;) {
            std::list<std::thread> ths;
            {
                std::lock_guard<std::mutex> lk(connM_);
                ths.swap(threads_);
                done_.clear();
            }
            if (ths.empty()) break;
            for (auto& t : ths)
                if (t.joinable()) t.join();
        }
        net::closeSocket(listener_);
        listener_ = net::kInvalidSocket;
        gena_.clear();
        net::cleanup();
    }

    bool running() const { return running_.load(); }
    int port() const { return port_; }
    bool ssdpJoined() const { return ssdp_.joined(); }
    size_t subscriptions() const { return gena_.count(); }

private:
    static net::socket_t openListener(int port) {
        net::socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == net::kInvalidSocket) return s;
#ifndef _WIN32
        int opt = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#else
        BOOL excl = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
#endif
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);   // a kapcsolatot a helyi cím és a partner alapján szűrjük (acceptLoop)
        a.sin_port = htons(static_cast<uint16_t>(port));
        if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(s, 16) != 0) {
            net::closeSocket(s);
            return net::kInvalidSocket;
        }
        return s;
    }

    std::vector<std::pair<std::string, std::string>> initialProps(int svc) {
        if (svc == 2) return { { "SourceProtocolInfo", "" }, { "SinkProtocolInfo", sinkProtocolInfo() }, { "CurrentConnectionIDs", "0" } };
        return { { "LastChange", r_.fullLastChange(svc) } };
    }

    void acceptLoop() {
        while (running_.load()) {
            if (!waitReadable(listener_, 200)) {
                reap();
                continue;
            }
            sockaddr_in peer{};
#ifdef _WIN32
            int pl = sizeof(peer);
#else
            socklen_t pl = sizeof(peer);
#endif
            net::socket_t c = ::accept(listener_, reinterpret_cast<sockaddr*>(&peer), &pl);
            if (c == net::kInvalidSocket) continue;
            sockaddr_in local{};
#ifdef _WIN32
            int ll = sizeof(local);
#else
            socklen_t ll = sizeof(local);
#endif
            getsockname(c, reinterpret_cast<sockaddr*>(&local), &ll);
            const uint32_t lip = ntohl(local.sin_addr.s_addr), pip = ntohl(peer.sin_addr.s_addr);
            // a helyi cím egy magánhálózati interfészé, a partner ugyanabból az alhálózatból (és az engedélyezettek közül)
            Iface via;
            bool ok = false;
            for (const auto& f : privateIfaces())
                if (f.ip == lip && f.contains(pip)) { via = f; ok = true; break; }
            if (ok && !cfg_.allow.empty()) ok = std::find(cfg_.allow.begin(), cfg_.allow.end(), pip) != cfg_.allow.end();
            {
                std::lock_guard<std::mutex> lk(connM_);
                if (!ok || static_cast<int>(open_.size()) >= kMaxConnections) {
                    net::closeSocket(c);
                    continue;
                }
                open_.insert(c);
                auto done = std::make_shared<std::atomic<bool>>(false);
                done_.push_back(done);
                threads_.emplace_back([this, c, via, pip, done] {
                    serve(c, via, v4Text(pip));
                    {
                        std::lock_guard<std::mutex> lk2(connM_);
                        open_.erase(c);
                    }
                    net::closeSocket(c);
                    *done = true;
                });
            }
        }
    }

    /** A befejezett kapcsolatszálak begyűjtése. */
    void reap() {
        std::lock_guard<std::mutex> lk(connM_);
        auto t = threads_.begin();
        auto d = done_.begin();
        while (t != threads_.end()) {
            if (**d) {
                if (t->joinable()) t->join();
                t = threads_.erase(t);
                d = done_.erase(d);
            } else {
                ++t;
                ++d;
            }
        }
    }

    static bool sendAll(net::socket_t s, const std::string& data) {
        size_t off = 0;
        while (off < data.size()) {
            const long long n = net::sendSome(s, data.data() + off, data.size() - off);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }

    static std::string response(int code, const char* reason, const std::string& ctype, const std::string& body,
                                const std::string& extra, bool head, bool keep) {
        std::string s = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\nDATE: " + ssdp::httpDate() +
                        "\r\nSERVER: " + ssdp::serverHeader() + "\r\nCONTENT-LENGTH: " + std::to_string(body.size()) + "\r\n";
        if (!ctype.empty()) s += "CONTENT-TYPE: " + ctype + "\r\n";
        s += extra;
        s += keep ? "CONNECTION: keep-alive\r\n\r\n" : "CONNECTION: close\r\n\r\n";
        if (!head) s += body;
        return s;
    }

    static int serviceOf(const std::string& key) {
        return key == "avt" ? 0 : key == "rcs" ? 1 : key == "cm" ? 2 : -1;
    }

    void serve(net::socket_t c, const Iface& via, const std::string& peer) {
        std::string buf;
        int served = 0;
        while (running_.load() && served < 100) {
            // a fejléc (legfeljebb 15 s várakozás a következő kérésre)
            size_t he;
            int idle = 0;
            while ((he = buf.find("\r\n\r\n")) == std::string::npos) {
                if (buf.size() > kMaxHeadBytes || !running_.load()) return;
                if (!waitReadable(c, 250)) {
                    if ((idle += 250) >= 15000) return;
                    continue;
                }
                char tmp[4096];
                const long long n = net::recvSome(c, tmp, sizeof(tmp));
                if (n <= 0) return;
                buf.append(tmp, static_cast<size_t>(n));
            }
            HttpRequest rq;
            const bool okHead = parseRequestHead(buf.substr(0, he), rq);
            buf.erase(0, he + 4);
            if (!okHead) { sendAll(c, response(400, "Bad Request", "", "", "", false, false)); return; }
            if (!rq.get("transfer-encoding").empty()) { sendAll(c, response(411, "Length Required", "", "", "", false, false)); return; }
            size_t len = 0;
            const std::string cl = rq.get("content-length");
            if (!cl.empty()) {
                if (cl.size() > 7 || cl.find_first_not_of("0123456789") != std::string::npos) {
                    sendAll(c, response(400, "Bad Request", "", "", "", false, false));
                    return;
                }
                len = static_cast<size_t>(std::stoul(cl));
            }
            if (len > kMaxSoapBody) { sendAll(c, response(413, "Payload Too Large", "", "", "", false, false)); return; }
            idle = 0;
            while (buf.size() < len) {
                if (!running_.load()) return;
                if (!waitReadable(c, 250)) {
                    if ((idle += 250) >= 15000) return;
                    continue;
                }
                char tmp[4096];
                const long long n = net::recvSome(c, tmp, sizeof(tmp));
                if (n <= 0) return;
                buf.append(tmp, static_cast<size_t>(n));
            }
            rq.body = buf.substr(0, len);
            buf.erase(0, len);
            std::string connHdr = rq.get("connection");
            for (char& ch : connHdr) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            const bool keep = connHdr.find("close") == std::string::npos;
            std::string after;   // GENA: a kezdő esemény a válasz után
            int afterSvc = -1;
            const std::string out = handle(rq, via, peer, keep, after, afterSvc);
            if (!sendAll(c, out)) return;
            if (!after.empty()) gena_.queueInitial(afterSvc, after);
            ++served;
            if (!keep) return;
        }
    }

    std::string handle(const HttpRequest& rq, const Iface& via, const std::string& peer, bool keep, std::string& initSid,
                       int& initSvc) {
        const bool head = rq.method == "HEAD";
        const std::string xml = "text/xml; charset=\"utf-8\"";
        if (rq.method == "GET" || head) {
            if (rq.path == "/description.xml") return response(200, "OK", xml, deviceDescription(cfg_.name, cfg_.uuid), "", head, keep);
            if (rq.path == "/AVTransport.xml") return response(200, "OK", xml, scpdAvTransport(), "", head, keep);
            if (rq.path == "/RenderingControl.xml") return response(200, "OK", xml, scpdRenderingControl(), "", head, keep);
            if (rq.path == "/ConnectionManager.xml") return response(200, "OK", xml, scpdConnectionManager(), "", head, keep);
            return response(404, "Not Found", "", "", "", head, keep);
        }
        if (rq.method == "POST" && rq.path.rfind("/ctl/", 0) == 0) {
            const int svc = serviceOf(rq.path.substr(5));
            if (svc < 0) return response(404, "Not Found", "", "", "", false, keep);
            SoapRequest sr;
            std::string perr;
            if (!parseSoapRequest(rq.body, rq.get("soapaction"), sr, &perr))
                return response(500, "Internal Server Error", xml, soapFault(perr == "size" ? 402 : 401), "EXT:\r\n", false, keep);
            static const char* kTypes[] = { kAvtType, kRcsType, kCmType };
            const std::string want = kTypes[svc];
            if (sr.serviceType.compare(0, want.size() - 1, want, 0, want.size() - 1) != 0)   // a verziószám előtti rész egyezzen
                return response(500, "Internal Server Error", xml, soapFault(401), "EXT:\r\n", false, keep);
            Renderer::Args outArgs;
            const int code = r_.handle(sr, via, peer, outArgs);
            if (code) return response(500, "Internal Server Error", xml, soapFault(code), "EXT:\r\n", false, keep);
            return response(200, "OK", xml, soapResponse(sr.serviceType, sr.action, outArgs), "EXT:\r\n", false, keep);
        }
        if ((rq.method == "SUBSCRIBE" || rq.method == "UNSUBSCRIBE") && rq.path.rfind("/evt/", 0) == 0) {
            const int svc = serviceOf(rq.path.substr(5));
            if (svc < 0) return response(404, "Not Found", "", "", "", false, keep);
            const std::string sid = rq.get("sid"), cbh = rq.get("callback"), nt = rq.get("nt");
            if (rq.method == "UNSUBSCRIBE") {
                if (!cbh.empty() || !nt.empty()) return response(400, "Bad Request", "", "", "", false, keep);
                if (sid.empty() || gena_.unsubscribe(svc, sid)) return response(412, "Precondition Failed", "", "", "", false, keep);
                return response(200, "OK", "", "", "", false, keep);
            }
            const int timeout = parseGenaTimeout(rq.get("timeout"));
            if (!sid.empty()) {
                if (!cbh.empty() || !nt.empty()) return response(400, "Bad Request", "", "", "", false, keep);
                if (gena_.renew(svc, sid, timeout)) return response(412, "Precondition Failed", "", "", "", false, keep);
                return response(200, "OK", "", "", "SID: " + sid + "\r\nTIMEOUT: Second-" + std::to_string(timeout) + "\r\n", false, keep);
            }
            radio::RadioUrl cb;
            if (nt != "upnp:event" || !parseCallback(cbh, via, cb)) return response(412, "Precondition Failed", "", "", "", false, keep);
            std::string newSid;
            const int e = gena_.subscribe(svc, cb, timeout, newSid);
            if (e) return response(503, "Service Unavailable", "", "", "", false, keep);
            initSid = newSid;
            initSvc = svc;
            return response(200, "OK", "", "", "SID: " + newSid + "\r\nTIMEOUT: Second-" + std::to_string(timeout) + "\r\n", false, keep);
        }
        return response(405, "Method Not Allowed", "", "", "", head, keep);
    }

    Renderer& r_;
    Gena gena_;
    SsdpAnnouncer ssdp_;
    Config cfg_;
    std::atomic<bool> running_{ false };
    net::socket_t listener_ = net::kInvalidSocket;
    int port_ = 0;
    std::thread acceptTh_;
    std::mutex connM_;
    std::set<net::socket_t> open_;
    std::list<std::thread> threads_;
    std::list<std::shared_ptr<std::atomic<bool>>> done_;
};

} // namespace ee_analyzer::upnp
