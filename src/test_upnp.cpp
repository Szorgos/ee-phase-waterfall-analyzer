// A UPnP / DLNA renderer tesztje: a tiszta részek (XML, SOAP-boríték, hibaválasz, időformátum, DIDL-Lite,
// protocolInfo, LastChange, a médiacím és a visszahívási cím szabálya, SSDP-csomagok), az állapotgép egy hamis
// lejátszóval (hibakódok, hangerő, fix hangerő, események), és – ha a gépnek van magánhálózati címe – a valódi
// letöltés (HTTP Range-dzsel folytatva) és a teljes szolgáltatás (leírás, SOAP és GENA a HTTP-n át) egy helyi
// próbaszerverrel. A hangfájl: tests/data/player_stereo44.flac.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "http_track_decoder.hpp"
#include "net_compat.hpp"
#include "upnp_http.hpp"
#include "upnp_renderer.hpp"
#include "upnp_soap.hpp"
#include "upnp_ssdp.hpp"

using namespace ee_analyzer;
using namespace ee_analyzer::upnp;
using Clock = std::chrono::steady_clock;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; std::printf("  [HIBA] " __VA_ARGS__); } else { std::printf("  [OK]   " __VA_ARGS__); } std::printf("\n"); std::fflush(stdout); } while (0)

static const std::string kData = std::getenv("AQ_TEST_DATA_DIR") ? std::getenv("AQ_TEST_DATA_DIR") : AQ_TEST_DATA_DIR;

static std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static bool contains(const std::string& s, const std::string& t) { return s.find(t) != std::string::npos; }
static bool waitFor(const std::function<bool()>& f, int ms) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return f();
}

static Iface lan(uint32_t ip, int bits) {
    Iface f;
    f.ip = ip;
    f.mask = bits ? ~0u << (32 - bits) : 0;
    return f;
}
static uint32_t ipOf(const char* s) { uint32_t v = 0; parseV4(s, v); return v; }

static std::string soapEnv(const char* type, const char* action, const std::string& args, const char* prefix = "s") {
    return std::string("<?xml version=\"1.0\"?><") + prefix + ":Envelope xmlns:" + prefix +
           "=\"http://schemas.xmlsoap.org/soap/envelope/\"><" + prefix + ":Body><u:" + action + " xmlns:u=\"" + type + "\">" + args +
           "</u:" + action + "></" + prefix + ":Body></" + prefix + ":Envelope>";
}

// ------------------------------------------------------------------------------------------------ hamis lejátszó

class FakePlayer final : public PlayerPort {
public:
    mutable std::mutex m;
    std::string openedPath, openedUrl;
    uint64_t token = 0;
    St st = St::Idle;
    double pos = 0, dur = 0, lastSeek = -1;
    int vol = 50, mute = 0, setVolCalls = 0, drops = 0, opens = 0;

    bool openFile(const std::string& path, uint64_t t, std::string&) override {
        std::lock_guard<std::mutex> lk(m);
        openedPath = path; token = t; st = St::Playing; pos = 0; dur = 1.0; ++opens;
        return true;
    }
    bool openLive(const std::string& url, const radio::NetOptions&, const std::string&, uint64_t t, std::string&,
                  const std::atomic<bool>*) override {
        std::lock_guard<std::mutex> lk(m);
        openedUrl = url; token = t; st = St::Playing; ++opens;
        return true;
    }
    void play() override { std::lock_guard<std::mutex> lk(m); if (st == St::Paused) st = St::Playing; }
    void pause() override { std::lock_guard<std::mutex> lk(m); if (st == St::Playing) st = St::Paused; }
    void stop() override { std::lock_guard<std::mutex> lk(m); st = St::Idle; token = 0; }
    void seek(double s) override { std::lock_guard<std::mutex> lk(m); lastSeek = s; pos = s; }
    St state() const override { std::lock_guard<std::mutex> lk(m); return st; }
    uint64_t currentToken() const override { std::lock_guard<std::mutex> lk(m); return st == St::Idle ? 0 : token; }
    double position() const override { std::lock_guard<std::mutex> lk(m); return pos; }
    double duration() const override { std::lock_guard<std::mutex> lk(m); return dur; }
    void dropPrefetched() override { std::lock_guard<std::mutex> lk(m); ++drops; }
    void setVolume(int p, int mu) override { std::lock_guard<std::mutex> lk(m); if (p >= 0) vol = p; if (mu >= 0) mute = mu; ++setVolCalls; }
    bool getVolume(int& p, bool& mu) const override { std::lock_guard<std::mutex> lk(m); p = vol; mu = mute != 0; return true; }
};

static SoapRequest req(const char* type, const char* action, std::vector<std::pair<std::string, std::string>> args) {
    SoapRequest r;
    r.serviceType = type;
    r.action = action;
    r.args = std::move(args);
    return r;
}

// ------------------------------------------------------------------------------------------------ próbaszerver

/**
 * Egyetlen fájlt kiszolgáló HTTP-szerver a megadott címen. Range-et ismer (206); dropFirst: az első válasz a fájl
 * felénél megszakad (a folytatás próbájához). A kapott kérések fejlécei a log-ba kerülnek.
 */
class FileServer {
public:
    std::string body, ctype = "audio/flac";
    bool dropFirst = false, noLength = false;
    std::mutex m;
    std::vector<std::string> log;
    int port = 0;

    bool start(uint32_t ip) {
        s_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(ip);
        a.sin_port = 0;
        if (::bind(s_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(s_, 8) != 0) return false;
#ifdef _WIN32
        int l = sizeof(a);
#else
        socklen_t l = sizeof(a);
#endif
        getsockname(s_, reinterpret_cast<sockaddr*>(&a), &l);
        port = ntohs(a.sin_port);
        run_ = true;
        th_ = std::thread([this] { loop(); });
        return true;
    }
    ~FileServer() {
        run_ = false;
        if (th_.joinable()) th_.join();
        net::closeSocket(s_);
    }

private:
    net::socket_t s_ = net::kInvalidSocket;
    std::atomic<bool> run_{ false };
    std::thread th_;
    int served_ = 0;

    void loop() {
        while (run_) {
            if (!waitReadable(s_, 100)) continue;
            net::socket_t c = ::accept(s_, nullptr, nullptr);
            if (c == net::kInvalidSocket) continue;
            std::string h;
            char buf[2048];
            while (h.find("\r\n\r\n") == std::string::npos && waitReadable(c, 2000)) {
                const long long n = net::recvSome(c, buf, sizeof(buf));
                if (n <= 0) break;
                h.append(buf, static_cast<size_t>(n));
            }
            {
                std::lock_guard<std::mutex> lk(m);
                log.push_back(h);
            }
            size_t from = 0;
            const size_t rp = h.find("Range: bytes=");
            if (rp != std::string::npos) from = static_cast<size_t>(std::strtoull(h.c_str() + rp + 13, nullptr, 10));
            std::string resp;
            if (rp != std::string::npos && from < body.size()) {
                resp = "HTTP/1.1 206 Partial Content\r\nContent-Type: " + ctype + "\r\nContent-Length: " +
                       std::to_string(body.size() - from) + "\r\nContent-Range: bytes " + std::to_string(from) + "-" +
                       std::to_string(body.size() - 1) + "/" + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
            } else {
                from = 0;
                resp = "HTTP/1.1 200 OK\r\nContent-Type: " + ctype + "\r\n" +
                       (noLength ? std::string() : "Content-Length: " + std::to_string(body.size()) + "\r\n") +
                       "Accept-Ranges: bytes\r\nConnection: close\r\n\r\n";
            }
            size_t end = body.size();
            if (dropFirst && served_ == 0) end = body.size() / 2;
            ++served_;
            resp += body.substr(from, end - from);
            size_t off = 0;
            while (off < resp.size()) {
                const long long n = net::sendSome(c, resp.data() + off, resp.size() - off);
                if (n <= 0) break;
                off += static_cast<size_t>(n);
            }
            net::closeSocket(c);
        }
    }
};

/** Egy HTTP-kérés (a válasz teljes szövege; üres: hiba). */
static std::string httpCall(uint32_t ip, int port, const std::string& request) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(ip);
    a.sin_port = htons(static_cast<uint16_t>(port));
    net::socket_t s = net::kInvalidSocket;
    for (int attempt = 0; attempt < 2 && s == net::kInvalidSocket; ++attempt) {
        s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        net::setNonBlocking(s, true);
        ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s, &w);
        timeval tv{ 2, 0 };
        int soErr = 1;
#ifdef _WIN32
        int el = sizeof(soErr);
        const int r = ::select(0, nullptr, &w, nullptr, &tv);
#else
        socklen_t el = sizeof(soErr);
        const int r = ::select(s + 1, nullptr, &w, nullptr, &tv);
#endif
        if (r <= 0 || getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &el) != 0 || soErr != 0) {
            net::closeSocket(s);
            s = net::kInvalidSocket;
        }
    }
    if (s == net::kInvalidSocket) return {};
    net::setNonBlocking(s, false);
    net::sendSome(s, request.data(), request.size());
    std::string out;
    char buf[4096];
    while (waitReadable(s, 3000)) {
        const long long n = net::recvSome(s, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
        const size_t he = out.find("\r\n\r\n");
        if (he != std::string::npos) {
            const size_t cl = out.find("CONTENT-LENGTH: ");
            if (cl != std::string::npos && cl < he) {
                const size_t len = static_cast<size_t>(std::strtoull(out.c_str() + cl + 16, nullptr, 10));
                if (out.size() >= he + 4 + len) break;
            }
        }
    }
    net::closeSocket(s);
    return out;
}

// ------------------------------------------------------------------------------------------------ tesztek

static void testXml() {
    std::printf("XML\n");
    CHECK(xmlEscape("a<b>&\"'") == "a&lt;b&gt;&amp;&quot;&apos;", "escape");
    CHECK(xmlUnescape("&lt;x&gt; &amp;amp; &#65;&#x42; &#x151; &bogus;") == "<x> &amp; AB \xC5\x91 &bogus;", "unescape (numerikus, ismeretlen marad)");
    XmlNode n;
    CHECK(parseXml("<a:r xmlns:a=\"urn:x\"><b x=\"1&amp;2\">t<![CDATA[<raw>&]]></b><!-- m --><c/></a:r>", n) && n.name == "r" &&
              n.ns == "urn:x" && n.child("b") && n.child("b")->text == "t<raw>&" && n.child("b")->attr("x") == "1&2" && n.child("c"),
          "előtag, névtér, CDATA, attribútum, üres elem");
    CHECK(!parseXml("<!DOCTYPE r [<!ENTITY e \"x\">]><r>&e;</r>", n), "DOCTYPE elutasítva");
    CHECK(!parseXml("<a><b></a></b>", n), "rossz egymásba ágyazás");
    std::string deep;
    for (int i = 0; i < 40; ++i) deep += "<d>";
    for (int i = 0; i < 40; ++i) deep += "</d>";
    CHECK(!parseXml(deep, n), "túl mély dokumentum elutasítva");
}

static void testSoap() {
    std::printf("SOAP\n");
    SoapRequest r;
    std::string e;
    const std::string meta = xmlEscape("<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\"><item><dc:title xmlns:dc=\"x\">Á &amp; B</dc:title></item></DIDL-Lite>");
    const std::string body = soapEnv(kAvtType, "SetAVTransportURI",
                                     "<InstanceID>0</InstanceID><CurrentURI>http://192.168.1.2/a.flac?x=1&amp;y=2</CurrentURI><CurrentURIMetaData>" +
                                         meta + "</CurrentURIMetaData>",
                                     "SOAP-ENV");
    CHECK(parseSoapRequest(body, "\"urn:schemas-upnp-org:service:AVTransport:1#SetAVTransportURI\"", r, &e) &&
              r.action == "SetAVTransportURI" && r.serviceType == kAvtType && r.arg("CurrentURI") == "http://192.168.1.2/a.flac?x=1&y=2",
          "boríték más előtaggal, entitás a címben");
    CHECK(contains(r.arg("CurrentURIMetaData"), "<DIDL-Lite") && contains(r.arg("CurrentURIMetaData"), "&amp; B"),
          "a metaadat egyszer oldódik fel (a DIDL saját entitása marad)");
    CHECK(parseSoapRequest(soapEnv(kAvtType, "Play", "<InstanceID>0</InstanceID><Speed>1</Speed>"), "", r) && r.serviceType == kAvtType,
          "fejléc nélkül a névtérből");
    CHECK(parseSoapRequest(soapEnv(kAvtType, "Play", "<InstanceID>0</InstanceID>"), "urn:schemas-upnp-org:service:AVTransport:1#Play", r),
          "fejléc idézőjel nélkül");
    CHECK(!parseSoapRequest(soapEnv(kAvtType, "Play", ""), "\"urn:schemas-upnp-org:service:AVTransport:1#Stop\"", r, &e) && e == "mismatch",
          "eltérő művelet a fejlécben: hiba");
    CHECK(!parseSoapRequest("nem xml", "", r, &e) && e == "xml", "hibás XML");
    CHECK(!parseSoapRequest(std::string(kMaxSoapBody + 1, ' '), "", r, &e) && e == "size", "64 KiB fölött: elutasítva");
    const std::string resp = soapResponse(kAvtType, "GetTransportInfo", { { "CurrentTransportState", "PLAYING" }, { "X", "<&>" } });
    CHECK(contains(resp, "<u:GetTransportInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">") &&
              contains(resp, "<CurrentTransportState>PLAYING</CurrentTransportState>") && contains(resp, "<X>&lt;&amp;&gt;</X>"),
          "válasz (escape-elt érték)");
    const std::string f = soapFault(718);
    CHECK(contains(f, "<faultcode>s:Client</faultcode>") && contains(f, "<faultstring>UPnPError</faultstring>") &&
              contains(f, "<errorCode>718</errorCode>") && contains(f, "Invalid InstanceID"),
          "UPnPError (UDA 1.1 3.2.2)");
    CHECK(std::string(errorText(701)) == "Transition not available" && std::string(errorText(714)) == "Illegal MIME-type" &&
              std::string(errorText(716)) == "Resource not found" && std::string(errorText(710)) == "Seek mode not supported",
          "hibakódok szövege");
}

static void testTime() {
    std::printf("Időformátum\n");
    CHECK(formatTime(0) == "0:00:00" && formatTime(-5) == "0:00:00" && formatTime(3725.9) == "1:02:05" && formatTime(59.999) == "0:00:59",
          "formázás H:MM:SS");
    double v = 0;
    CHECK(parseTime("1:02:05", v) && v == 3725.0, "1:02:05");
    CHECK(parseTime("0:00:10.500", v) && std::abs(v - 10.5) < 1e-9, "tört másodperc");
    CHECK(parseTime("+0:01:00", v) && v == 60.0, "előjel (+)");
    CHECK(parseTime("0:00:01.1/4", v) && std::abs(v - 1.25) < 1e-9, "F0/F1 alak");
    CHECK(parseTime("100:00:00", v) && v == 360000.0, "több jegyű óra");
    CHECK(!parseTime("1:60:00", v) && !parseTime("0:00:61", v) && !parseTime("-0:00:01", v) && !parseTime("00:01", v) &&
              !parseTime("a:00:00", v) && !parseTime("0:0:00", v) && !parseTime("0:00:01.4/2", v) && !parseTime("", v),
          "hibás alakok elutasítva");
}

static void testDidl() {
    std::printf("DIDL-Lite\n");
    const std::string d =
        "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\"><item id=\"1\" parentID=\"0\" restricted=\"1\">"
        "<dc:title>Kék &amp; sárga</dc:title><dc:creator>Zeneszerző</dc:creator><upnp:artist role=\"Performer\">Előadó</upnp:artist>"
        "<upnp:album>Album</upnp:album><upnp:albumArtURI>http://192.168.1.2/art.jpg</upnp:albumArtURI>"
        "<upnp:class>object.item.audioItem.musicTrack</upnp:class>"
        "<res protocolInfo=\"http-get:*:audio/x-flac:DLNA.ORG_OP=01\" duration=\"0:04:05.120\" size=\"31457280\">http://192.168.1.2/a.flac</res>"
        "<res protocolInfo=\"http-get:*:audio/mpeg:*\">http://192.168.1.2/a.mp3</res></item></DIDL-Lite>";
    DidlItem it;
    CHECK(parseDidl(d, it) && it.title == "Kék & sárga" && it.artist == "Előadó" && it.album == "Album" &&
              it.artUri == "http://192.168.1.2/art.jpg" && it.res.size() == 2,
          "cím, előadó (upnp:artist), album, borító, két res");
    CHECK(std::abs(it.res[0].duration - 245.12) < 1e-6 && it.res[0].size == 31457280 && protocolMime(it.res[0].protocolInfo) == "audio/x-flac",
          "res@duration, res@size, protocolInfo MIME");
    CHECK(didlResFor(it, "http://192.168.1.2/a.mp3") == &it.res[1] && didlResFor(it, "http://x/") == &it.res[0], "a címhez tartozó res");
    CHECK(!it.broadcast(), "zeneszám (nem élő)");
    DidlItem r;
    CHECK(parseDidl("<DIDL-Lite><item><dc:title>Rádió</dc:title><dc:creator>Csak szerző</dc:creator><upnp:class>object.item.audioItem.audioBroadcast</upnp:class></item></DIDL-Lite>", r) &&
              r.broadcast() && r.artist == "Csak szerző",
          "audioBroadcast: élő; dc:creator, ha nincs upnp:artist");
    CHECK(!parseDidl("", r) && !parseDidl("<x/>", r) && !parseDidl("<DIDL-Lite></DIDL-Lite>", r), "üres / nem DIDL");
    CHECK(extForMime("audio/flac") == "flac" && extForMime("audio/x-dsf") == "dsf" && extForMime("audio/wav") == "wav" &&
              extForMime("application/x-mpegurl") == "-" && extForMime("video/mp4").empty(),
          "MIME -> kiterjesztés");
    const uint8_t flac[] = { 'f', 'L', 'a', 'C', 0 }, wav[] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E' },
                  dsf[] = { 'D', 'S', 'D', ' ' }, m4a[] = { 0, 0, 0, 0x20, 'f', 't', 'y', 'p' }, mp3[] = { 0xFF, 0xFB, 0x90 };
    CHECK(sniffExt(flac, 5) == "flac" && sniffExt(wav, 12) == "wav" && sniffExt(dsf, 4) == "dsf" && sniffExt(m4a, 8) == "m4a" &&
              sniffExt(mp3, 3) == "mp3",
          "tartalom szerinti felismerés");
    const std::string sink = sinkProtocolInfo();
    CHECK(contains(sink, "http-get:*:audio/flac:*") && contains(sink, "http-get:*:audio/x-dsf:*") && !contains(sink, "video/"),
          "Sink protocolInfo");
}

static void testEvents() {
    std::printf("LastChange és GENA\n");
    const std::string lc = lastChangeXml(kAvtEventNs, { { "TransportState", "PLAYING", "" }, { "CurrentTrackURI", "http://a/?x=1&y=\"2\"", "" } });
    CHECK(lc == "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\"><InstanceID val=\"0\"><TransportState val=\"PLAYING\"/>"
                "<CurrentTrackURI val=\"http://a/?x=1&amp;y=&quot;2&quot;\"/></InstanceID></Event>",
          "AVT LastChange");
    const std::string rc = lastChangeXml(kRcsEventNs, { { "Volume", "30", "Master" } });
    CHECK(contains(rc, "<Volume channel=\"Master\" val=\"30\"/>") && contains(rc, "metadata-1-0/RCS/"), "RCS LastChange (channel)");
    const std::string ps = propertySet({ { "LastChange", lc } });
    XmlNode n;
    CHECK(parseXml(ps, n) && n.name == "propertyset" && n.find("LastChange") && n.find("LastChange")->text == lc,
          "propertyset: a LastChange escape-elve, visszabontható");
    CHECK(parseGenaTimeout("Second-300") == 300 && parseGenaTimeout("Second-5") == 60 && parseGenaTimeout("infinite") == 1800 &&
              parseGenaTimeout("") == 1800 && parseGenaTimeout("Second-99999999") == 1800,
          "TIMEOUT: Second-N [60, 1800]");
}

static void testUrlRule() {
    std::printf("Címszabály\n");
    const Iface via = lan(ipOf("192.168.1.10"), 24);
    radio::RadioUrl u;
    CHECK(checkMediaUrl("http://192.168.1.20:9790/a.flac", via, u).empty(), "magáncím a kérés alhálózatából: igen");
    CHECK(checkMediaUrl("http://192.168.2.20/a.flac", via, u) == "blocked", "más magánhálózat: nem");
    CHECK(checkMediaUrl("http://10.0.0.5/a.flac", via, u) == "blocked", "10/8 (nem ez az alhálózat): nem");
    CHECK(checkMediaUrl("http://127.0.0.1/a.flac", via, u) == "blocked" && checkMediaUrl("http://localhost/a.flac", via, u) == "blocked",
          "loopback: nem");
    CHECK(checkMediaUrl("http://169.254.169.254/latest/meta-data", via, u) == "blocked", "link-local (felhő-metaadat): nem");
    CHECK(checkMediaUrl("http://239.255.255.250:1900/", via, u) == "blocked", "multicast: nem");
    CHECK(checkMediaUrl("http://user:pw@192.168.1.20/a.flac", via, u) == "userinfo", "felhasználónév: nem");
    CHECK(checkMediaUrl("ftp://192.168.1.20/a.flac", via, u) == "scheme" && checkMediaUrl("file:///etc/passwd", via, u) == "scheme",
          "csak http(s)");
    CHECK(checkMediaUrl("https://8.8.8.8/a.flac", via, u).empty() && checkMediaUrl("http://example.com/a.flac", via, u).empty(),
          "nyilvános cím és név (a név a feloldáskor ellenőrződik)");
    CHECK(checkMediaUrl("http://[::1]/a", via, u) == "blocked" && checkMediaUrl("http://[fe80::1]/a", via, u) == "blocked" &&
              checkMediaUrl("http://[::ffff:192.168.1.20]/a", via, u).empty() && checkMediaUrl("http://[::ffff:127.0.0.1]/a", via, u) == "blocked",
          "IPv6: loopback, link-local tiltva; beágyazott IPv4 az IPv4-szabály szerint");
    const uint8_t pub[4] = { 1, 1, 1, 1 }, cg[4] = { 100, 64, 0, 1 }, same[4] = { 192, 168, 1, 99 }, bc[4] = { 255, 255, 255, 255 };
    CHECK(mediaAddrAllowed(4, pub, via) && !mediaAddrAllowed(4, cg, via) && mediaAddrAllowed(4, same, via) && !mediaAddrAllowed(4, bc, via),
          "feloldott címek (nyilvános, szolgáltatói NAT, alhálózat, üzenetszórás)");
    const Iface loopVia = lan(ipOf("127.0.0.1"), 8);
    const uint8_t lo[4] = { 127, 0, 0, 2 };
    CHECK(!mediaAddrAllowed(4, lo, loopVia), "a loopback interfészen át sem");
    CHECK(parseCallback("<http://192.168.1.20:4000/evt>", via, u) && u.port == 4000 && u.target == "/evt", "visszahívás a kérés alhálózatából");
    CHECK(parseCallback("<http://10.1.1.1/x><http://192.168.1.21/y>", via, u) && u.host == "192.168.1.21", "több cím: az első megfelelő");
    CHECK(!parseCallback("<https://192.168.1.20/evt>", via, u) && !parseCallback("<http://127.0.0.1/evt>", via, u) &&
              !parseCallback("<http://pc.local/evt>", via, u) && !parseCallback("http://192.168.1.20/evt", via, u),
          "visszahívás: https, loopback, név, szögletes zárójel nélkül: nem");
}

static void testSsdp() {
    std::printf("SSDP\n");
    const std::string uuid = stableUuid("gep");
    CHECK(uuid == stableUuid("gep") && uuid != stableUuid("masik") && uuid.size() == 36 && uuid[14] == '5', "állandó UUID");
    const auto t = ssdp::targets(uuid, kRendererType, { kAvtType, kRcsType, kCmType });
    CHECK(t.size() == 6 && t[0].nt == "upnp:rootdevice" && t[0].usn == "uuid:" + uuid + "::upnp:rootdevice" && t[1].usn == "uuid:" + uuid,
          "3 + 3 cél (rootdevice, uuid, eszköz, szolgáltatások)");
    const std::string a = ssdp::notifyAlive(t[2], "http://192.168.1.10:49494/description.xml", 7);
    CHECK(a.rfind("NOTIFY * HTTP/1.1\r\n", 0) == 0 && contains(a, "HOST: 239.255.255.250:1900") && contains(a, "NTS: ssdp:alive") &&
              contains(a, "CACHE-CONTROL: max-age=1800") && contains(a, "LOCATION: http://192.168.1.10:49494/description.xml") &&
              contains(a, "NT: " + std::string(kRendererType)) && contains(a, "BOOTID.UPNP.ORG: 7") && a.size() > 4 &&
              a.compare(a.size() - 4, 4, "\r\n\r\n") == 0,
          "ssdp:alive");
    const std::string b = ssdp::notifyByebye(t[0], 7);
    CHECK(contains(b, "NTS: ssdp:byebye") && !contains(b, "LOCATION") && !contains(b, "CACHE-CONTROL"), "ssdp:byebye");
    ssdp::Search q;
    CHECK(ssdp::parseSearch("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ssdp:all\r\n\r\n", q) &&
              q.st == "ssdp:all" && q.mx == 2,
          "M-SEARCH bontása");
    CHECK(!ssdp::parseSearch("M-SEARCH * HTTP/1.1\r\nST: ssdp:all\r\n\r\n", q), "MAN nélkül: nem keresés");
    CHECK(!ssdp::parseSearch("NOTIFY * HTTP/1.1\r\nMAN: \"ssdp:discover\"\r\nST: ssdp:all\r\n\r\n", q), "NOTIFY: nem keresés");
    CHECK(ssdp::matching(t, "ssdp:all").size() == 6 && ssdp::matching(t, kRendererType).size() == 1 &&
              ssdp::matching(t, "urn:schemas-upnp-org:service:AVTransport:1").size() == 1 &&
              ssdp::matching(t, "urn:schemas-upnp-org:service:AVTransport:2").empty() &&
              ssdp::matching(t, "urn:schemas-upnp-org:device:MediaServer:1").empty() && ssdp::matching(t, "uuid:" + uuid).size() == 1,
          "ST-illesztés (verzió, típus, uuid)");
    const std::string r = ssdp::searchResponse(t[0], "http://x/", 1);
    CHECK(r.rfind("HTTP/1.1 200 OK\r\n", 0) == 0 && contains(r, "\r\nEXT:\r\n") && contains(r, "ST: upnp:rootdevice"), "keresési válasz");
    const std::string desc = deviceDescription("Nappali <PC>", uuid);
    XmlNode n;
    CHECK(parseXml(desc, n) && n.find("friendlyName")->text == "Nappali <PC>" && n.find("UDN")->text == "uuid:" + uuid &&
              n.find("deviceType")->text == kRendererType,
          "eszközleírás (jól formált, escape-elt név)");
    CHECK(parseXml(scpdAvTransport(), n) && parseXml(scpdRenderingControl(), n) && parseXml(scpdConnectionManager(), n),
          "a három SCPD jól formált");
    CHECK(contains(scpdAvTransport(), "<name>SetNextAVTransportURI</name>") &&
              contains(scpdAvTransport(), "<stateVariable sendEvents=\"yes\"><name>LastChange</name>"),
          "SCPD: SetNext és evented LastChange");
}

static void testRenderer() {
    std::printf("Állapotgép (hamis lejátszó)\n");
    FakePlayer fp;
    std::mutex em;
    std::vector<std::pair<int, std::string>> events;
    Renderer r(fp);
    r.setEventSink([&](int s, const std::string& x) { std::lock_guard<std::mutex> lk(em); events.push_back({ s, x }); });
    const Iface via = lan(ipOf("192.168.1.10"), 24);
    Renderer::Args out;
    auto avt = [&](const char* a, std::vector<std::pair<std::string, std::string>> args) {
        out.clear();
        return r.handle(req(kAvtType, a, std::move(args)), via, "192.168.1.20", out);
    };
    auto rcs = [&](const char* a, std::vector<std::pair<std::string, std::string>> args) {
        out.clear();
        return r.handle(req(kRcsType, a, std::move(args)), via, "192.168.1.20", out);
    };
    CHECK(avt("GetTransportInfo", { { "InstanceID", "0" } }) == 0 && out[0].second == "NO_MEDIA_PRESENT" && out[1].second == "OK",
          "kezdetben NO_MEDIA_PRESENT");
    CHECK(avt("Play", { { "InstanceID", "0" }, { "Speed", "1" } }) == 701, "Play adathordozó nélkül: 701");
    CHECK(avt("Play", { { "InstanceID", "1" }, { "Speed", "1" } }) == 718, "rossz InstanceID: 718");
    CHECK(avt("Play", {}) == 402, "hiányzó InstanceID: 402");
    CHECK(avt("Record", { { "InstanceID", "0" } }) == 401, "ismeretlen művelet: 401");
    CHECK(avt("SetAVTransportURI", { { "InstanceID", "0" }, { "CurrentURI", "http://127.0.0.1/a.flac" }, { "CurrentURIMetaData", "" } }) == 716,
          "tiltott cím: 716");
    CHECK(avt("SetAVTransportURI", { { "InstanceID", "0" }, { "CurrentURI", "http://192.168.1.20/v.mp4" },
                                     { "CurrentURIMetaData", "<DIDL-Lite><item><res protocolInfo=\"http-get:*:video/mp4:*\">http://192.168.1.20/v.mp4</res></item></DIDL-Lite>" } }) == 714,
          "videó MIME: 714");
    CHECK(avt("Seek", { { "InstanceID", "0" }, { "Unit", "REL_TIME" }, { "Target", "0:00:10" } }) == 701, "Seek adathordozó nélkül: 701");
    CHECK(avt("SetPlayMode", { { "InstanceID", "0" }, { "NewPlayMode", "SHUFFLE" } }) == 712, "SetPlayMode SHUFFLE: 712");
    CHECK(avt("Stop", { { "InstanceID", "0" } }) == 0, "Stop adathordozó nélkül: rendben (nincs teendő)");
    CHECK(avt("GetPositionInfo", { { "InstanceID", "0" } }) == 0 && out[0].second == "0" && out[4].second == "0:00:00", "GetPositionInfo üresen");
    CHECK(avt("GetCurrentTransportActions", { { "InstanceID", "0" } }) == 0 && out[0].second.empty(), "nincs elérhető művelet");
    // hangerő
    CHECK(rcs("GetVolume", { { "InstanceID", "0" }, { "Channel", "Master" } }) == 0 && out[0].second == "50", "GetVolume: a lejátszó hangereje");
    CHECK(rcs("SetVolume", { { "InstanceID", "0" }, { "Channel", "Master" }, { "DesiredVolume", "30" } }) == 0 && fp.vol == 30, "SetVolume 30");
    CHECK(rcs("SetVolume", { { "InstanceID", "0" }, { "Channel", "Master" }, { "DesiredVolume", "101" } }) == 601, "101: 601");
    CHECK(rcs("SetVolume", { { "InstanceID", "0" }, { "Channel", "LF" }, { "DesiredVolume", "10" } }) == 600, "ismeretlen csatorna: 600");
    CHECK(rcs("SetMute", { { "InstanceID", "0" }, { "Channel", "Master" }, { "DesiredMute", "true" } }) == 0 && fp.mute == 1, "SetMute");
    CHECK(rcs("GetVolume", { { "InstanceID", "3" }, { "Channel", "Master" } }) == 702, "RenderingControl rossz InstanceID: 702");
    CHECK(waitFor([&] { std::lock_guard<std::mutex> lk(em); for (auto& e : events) if (e.first == 1 && contains(e.second, "<Volume channel=\"Master\" val=\"30\"/>")) return true; return false; }, 2000),
          "LastChange-esemény a hangerőről");
    r.setFixedVolume(true);
    const int calls = fp.setVolCalls;
    CHECK(rcs("SetVolume", { { "InstanceID", "0" }, { "Channel", "Master" }, { "DesiredVolume", "5" } }) == 0 && fp.setVolCalls == calls && fp.vol == 30,
          "fix hangerő: a SetVolume hatástalan");
    CHECK(rcs("GetVolume", { { "InstanceID", "0" }, { "Channel", "Master" } }) == 0 && out[0].second == "100", "fix hangerő: a vezérlő 100-at lát");
    CHECK(rcs("GetMute", { { "InstanceID", "0" }, { "Channel", "Master" } }) == 0 && out[0].second == "0", "fix hangerő: nincs némítás");
    CHECK(contains(r.fullLastChange(1), "<Volume channel=\"Master\" val=\"100\"/>"), "a teljes RCS LastChange is 100");
    r.setFixedVolume(false);
    // ConnectionManager
    out.clear();
    CHECK(r.handle(req(kCmType, "GetProtocolInfo", {}), via, "x", out) == 0 && contains(out[1].second, "audio/flac"), "GetProtocolInfo");
    CHECK(r.handle(req(kCmType, "GetCurrentConnectionInfo", { { "ConnectionID", "5" } }), via, "x", out) == 706, "ismeretlen kapcsolat: 706");
    // élő folyam (audioBroadcast): letöltés nélkül, rádióként
    const std::string radioDidl = "<DIDL-Lite><item><dc:title>Rádió</dc:title><upnp:class>object.item.audioItem.audioBroadcast</upnp:class>"
                                  "<res protocolInfo=\"http-get:*:audio/mpeg:*\">http://8.8.8.8/live</res></item></DIDL-Lite>";
    CHECK(avt("SetAVTransportURI", { { "InstanceID", "0" }, { "CurrentURI", "http://8.8.8.8/live" }, { "CurrentURIMetaData", radioDidl } }) == 0 &&
              r.transport() == Transport::Stopped,
          "élő folyam beállítva: STOPPED");
    CHECK(avt("Play", { { "InstanceID", "0" }, { "Speed", "2" } }) == 717, "Speed 2: 717");
    CHECK(avt("Play", { { "InstanceID", "0" }, { "Speed", "1" } }) == 0, "Play");
    CHECK(waitFor([&] { return r.transport() == Transport::Playing; }, 2000) && fp.openedUrl == "http://8.8.8.8/live", "élő folyam szól (openLive)");
    CHECK(avt("Seek", { { "InstanceID", "0" }, { "Unit", "REL_TIME" }, { "Target", "0:00:10" } }) == 710, "élő folyamban Seek: 710");
    CHECK(avt("GetCurrentTransportActions", { { "InstanceID", "0" } }) == 0 && out[0].second == "Pause,Stop", "élő folyam: Pause,Stop");
    CHECK(avt("Pause", { { "InstanceID", "0" } }) == 0 && fp.state() == PlayerPort::St::Paused && r.transport() == Transport::Paused, "Pause");
    CHECK(avt("Play", { { "InstanceID", "0" } }) == 0 && fp.state() == PlayerPort::St::Playing, "folytatás");
    {
        std::lock_guard<std::mutex> lk(fp.m);
        fp.token = 7;   // a felület másik számot indított
    }
    CHECK(waitFor([&] { return r.transport() == Transport::Stopped; }, 2000), "ha más szól a lejátszón: STOPPED");
    CHECK(avt("SetAVTransportURI", { { "InstanceID", "0" }, { "CurrentURI", "" } }) == 0 && r.transport() == Transport::NoMedia,
          "üres cím: NO_MEDIA_PRESENT");
    CHECK(waitFor([&] { std::lock_guard<std::mutex> lk(em); for (auto& e : events) if (e.first == 0 && contains(e.second, "TransportState val=\"PLAYING\"")) return true; return false; }, 1000),
          "LastChange-esemény az állapotváltásról");
}

/** Valódi letöltés és lejátszási folyamat (hamis lejátszóval) a gép magánhálózati címén. */
static void testDownload(const Iface& ifc) {
    std::printf("Letöltés és folyamat (helyi próbaszerver: %s)\n", v4Text(ifc.ip).c_str());
    const std::string flac = readFile(kData + "/player_stereo44.flac");
    if (flac.empty()) { CHECK(false, "a tesztfájl hiányzik"); return; }
    FileServer srv;
    srv.body = flac;
    srv.dropFirst = true;
    if (!srv.start(ifc.ip)) { CHECK(false, "a próbaszerver nem indul"); return; }
    const std::string base = "http://" + v4Text(ifc.ip) + ":" + std::to_string(srv.port);

    // HttpTrack közvetlenül: megszakadás után Range-dzsel folytat
    HttpTrack::Config c;
    c.url = base + "/song.flac";
    c.title = "Próba: szám/1";
    c.net.allowAddr = [ifc](int fam, const uint8_t* a) { return mediaAddrAllowed(fam, a, ifc); };
    auto t = HttpTrack::start(c);
    CHECK(t->wait(20000) && t->state() == HttpTrack::State::Ready, "letöltve (%s)", t->error().c_str());
    CHECK(readFile(t->path()) == flac, "a fájl bájtra egyezik");
    CHECK(contains(t->path(), "Próba  szám 1.flac"), "a fájlnév a címből (tiltott karakterek nélkül): %s", t->path().c_str());
    {
        std::lock_guard<std::mutex> lk(srv.m);
        CHECK(srv.log.size() == 2 && contains(srv.log[1], "Range: bytes=" + std::to_string(flac.size() / 2) + "-"),
              "a megszakadás után Range-dzsel folytatta (%zu kérés)", srv.log.size());
        CHECK(!srv.log.empty() && !contains(srv.log[0], "Icy-MetaData"), "véges fájlnál nincs Icy-MetaData");
    }
    const std::string path = t->path();
    t.reset();
    CHECK(!std::ifstream(path).good(), "a megszűnéskor a fájl törlődik");

    // tiltott cím a letöltőben is (a hívó szabálya szerint)
    HttpTrack::Config bad = c;
    bad.net.allowAddr = [](int, const uint8_t*) { return false; };
    auto tb = HttpTrack::start(bad);
    CHECK(tb->wait(5000) && tb->state() == HttpTrack::State::Failed && tb->error() == "blocked", "a címszabály a letöltőben is véd");

    // a renderer folyamata: SetURI, Play, Seek, SetNext (szünetmentes), Stop
    srv.dropFirst = false;
    FakePlayer fp;
    Renderer r(fp);
    Renderer::Args out;
    auto avt = [&](const char* a, std::vector<std::pair<std::string, std::string>> args) {
        out.clear();
        return r.handle(req(kAvtType, a, std::move(args)), ifc, "peer", out);
    };
    const std::string didl = "<DIDL-Lite><item><dc:title>Első</dc:title><upnp:artist>E</upnp:artist><res protocolInfo=\"http-get:*:audio/flac:*\" duration=\"0:00:01.000\">" +
                             base + "/1.flac</res></item></DIDL-Lite>";
    CHECK(avt("SetAVTransportURI", { { "InstanceID", "0" }, { "CurrentURI", base + "/1.flac" }, { "CurrentURIMetaData", didl } }) == 0 &&
              r.transport() == Transport::Stopped,
          "SetAVTransportURI");
    CHECK(avt("GetMediaInfo", { { "InstanceID", "0" } }) == 0 && out[1].second == "0:00:01" && out[2].second == base + "/1.flac" &&
              out[3].second == didl,
          "GetMediaInfo (a DIDL hossza és metaadata)");
    CHECK(avt("Seek", { { "InstanceID", "0" }, { "Unit", "REL_TIME" }, { "Target", "0:00:00.500" } }) == 0, "Seek lejátszás előtt (megjegyzi)");
    CHECK(avt("Seek", { { "InstanceID", "0" }, { "Unit", "X_DLNA_REL_BYTE" }, { "Target", "10" } }) == 710, "ismeretlen mód: 710");
    CHECK(avt("Seek", { { "InstanceID", "0" }, { "Unit", "REL_TIME" }, { "Target", "1:00:00" } }) == 711, "a szám végén túl: 711");
    CHECK(avt("Play", { { "InstanceID", "0" }, { "Speed", "1" } }) == 0, "Play");
    CHECK(waitFor([&] { return r.transport() == Transport::Playing; }, 10000), "PLAYING");
    CHECK(readFile(fp.openedPath) == flac && contains(fp.openedPath, "Első.flac"), "a lejátszó a letöltött fájlt kapta");
    CHECK(fp.lastSeek == 0.5, "a korábbi Seek a megnyitás után érvényesült");
    CHECK(avt("Seek", { { "InstanceID", "0" }, { "Unit", "ABS_TIME" }, { "Target", "0:00:00.250" } }) == 0 && fp.lastSeek == 0.25, "Seek szól közben");
    CHECK(avt("GetPositionInfo", { { "InstanceID", "0" } }) == 0 && out[0].second == "1" && out[3].second == base + "/1.flac",
          "GetPositionInfo");
    const uint64_t firstToken = fp.currentToken();
    CHECK(Renderer::isOwnToken(firstToken), "saját jel a lejátszóban");
    media::Info mi;
    mi.available = true;
    r.patchInfo(mi);
    CHECK(mi.title == "Első" && mi.artist == "E" && mi.app == "UPnP", "„most szól” a DIDL-ből, ha a fájlban nincs címke");
    // következő szám: letöltés, a lejátszó újra kérdez, szünetmentes átlépés
    const int drops0 = fp.drops;
    CHECK(avt("SetNextAVTransportURI", { { "InstanceID", "0" }, { "NextURI", base + "/2.flac" }, { "NextURIMetaData", "" } }) == 0, "SetNextAVTransportURI");
    CHECK(waitFor([&] { std::lock_guard<std::mutex> lk(fp.m); return fp.drops >= drops0 + 2; }, 10000),
          "a kész következő számnál a lejátszó újra kérdez (dropPrefetched)");
    std::string np;
    uint64_t nt = 0;
    CHECK(r.nextFor(firstToken, np, nt) && readFile(np) == flac && Renderer::isOwnToken(nt) && nt != firstToken, "nextFor: a következő fájl és jele");
    CHECK(!r.nextFor(12345, np, nt), "nextFor más jelre: nem");
    {
        std::lock_guard<std::mutex> lk(fp.m);
        fp.token = nt;   // a lejátszó szünetmentesen átlépett
    }
    CHECK(waitFor([&] {
              Renderer::Args o;
              r.handle(req(kAvtType, "GetMediaInfo", { { "InstanceID", "0" } }), ifc, "p", o);
              return o.size() > 4 && o[2].second == base + "/2.flac" && o[4].second.empty();
          }, 3000),
          "az átlépés után az AVTransportURI a következő szám, a NextURI üres");
    CHECK(avt("Stop", { { "InstanceID", "0" } }) == 0 && r.transport() == Transport::Stopped && fp.state() == PlayerPort::St::Idle, "Stop");
    CHECK(avt("Play", { { "InstanceID", "0" } }) == 0 && waitFor([&] { return r.transport() == Transport::Playing; }, 5000),
          "Play a Stop után: a már letöltött szám újra szól");
    {
        std::lock_guard<std::mutex> lk(fp.m);
        fp.st = PlayerPort::St::Ended;
    }
    CHECK(waitFor([&] { return r.transport() == Transport::Stopped; }, 2000), "a szám vége: STOPPED");

    // a teljes szolgáltatás a HTTP-n át: leírás, SOAP, hibaválasz, GENA kezdő esemény
    std::printf("Szolgáltatás (HTTP)\n");
    // Alkalmazásonkénti tűzfal (pl. OpenSnitch) a teszt saját kimenő kapcsolatát eldobhatja (a curl-ét nem): ilyenkor
    // ez a rész kimarad (a CI-ben fut; a gépen a tests/live/upnp_live.py viszi végig a valódi motorral)
    if (httpCall(ifc.ip, srv.port, "GET /probe HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n").empty()) {
        std::printf("  (a teszt nem tud kapcsolódni a saját címére – a gép tűzfala? –, a HTTP-rész kimarad)\n");
        return;
    }
    UpnpServer server(r);
    UpnpServer::Config sc;
    sc.name = "Teszt renderer";
    sc.uuid = stableUuid("teszt");
    sc.port = 49700;
    std::string err;
    CHECK(server.start(sc, err), "a szolgáltatás indul (%s)", err.c_str());
    const int port = server.port();
    std::string resp = httpCall(ifc.ip, port, "GET /description.xml HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(resp.rfind("HTTP/1.1 200 OK", 0) == 0 && contains(resp, "<friendlyName>Teszt renderer</friendlyName>") &&
              contains(resp, "<controlURL>/ctl/avt</controlURL>"),
          "eszközleírás");
    resp = httpCall(ifc.ip, port, "GET /AVTransport.xml HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(contains(resp, "<name>Seek</name>"), "SCPD");
    const std::string body = soapEnv(kAvtType, "GetTransportInfo", "<InstanceID>0</InstanceID>");
    resp = httpCall(ifc.ip, port, "POST /ctl/avt HTTP/1.1\r\nHost: x\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPACTION: \"" +
                                      std::string(kAvtType) + "#GetTransportInfo\"\r\nContent-Length: " + std::to_string(body.size()) +
                                      "\r\nConnection: close\r\n\r\n" + body);
    CHECK(resp.rfind("HTTP/1.1 200 OK", 0) == 0 && contains(resp, "<CurrentTransportState>STOPPED</CurrentTransportState>") && contains(resp, "\r\nEXT:\r\n"),
          "SOAP GetTransportInfo a HTTP-n át");
    const std::string bad1 = soapEnv(kAvtType, "Play", "<InstanceID>9</InstanceID>");
    resp = httpCall(ifc.ip, port, "POST /ctl/avt HTTP/1.1\r\nHost: x\r\nSOAPACTION: \"" + std::string(kAvtType) +
                                      "#Play\"\r\nContent-Length: " + std::to_string(bad1.size()) + "\r\nConnection: close\r\n\r\n" + bad1);
    CHECK(resp.rfind("HTTP/1.1 500", 0) == 0 && contains(resp, "<errorCode>718</errorCode>"), "SOAP-hiba: HTTP 500 + UPnPError");
    resp = httpCall(ifc.ip, port, "POST /ctl/rcs HTTP/1.1\r\nHost: x\r\nContent-Length: 70000\r\nConnection: close\r\n\r\n");
    CHECK(resp.rfind("HTTP/1.1 413", 0) == 0, "64 KiB fölötti törzs: 413");
    // GENA: visszahívás egy helyi figyelőre
    net::socket_t cbs = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in ca{};
    ca.sin_family = AF_INET;
    ca.sin_addr.s_addr = htonl(ifc.ip);
#ifdef _WIN32
    int cl = sizeof(ca);
#else
    socklen_t cl = sizeof(ca);
#endif
    ::bind(cbs, reinterpret_cast<sockaddr*>(&ca), sizeof(ca));
    ::listen(cbs, 4);
    getsockname(cbs, reinterpret_cast<sockaddr*>(&ca), &cl);
    const std::string cbUrl = "http://" + v4Text(ifc.ip) + ":" + std::to_string(ntohs(ca.sin_port)) + "/evt";
    resp = httpCall(ifc.ip, port, "SUBSCRIBE /evt/avt HTTP/1.1\r\nHost: x\r\nCALLBACK: <" + cbUrl +
                                      ">\r\nNT: upnp:event\r\nTIMEOUT: Second-300\r\nConnection: close\r\n\r\n");
    const size_t sp = resp.find("SID: ");
    CHECK(resp.rfind("HTTP/1.1 200 OK", 0) == 0 && sp != std::string::npos && contains(resp, "TIMEOUT: Second-300"), "SUBSCRIBE");
    const std::string sid = sp == std::string::npos ? "" : resp.substr(sp + 5, resp.find("\r\n", sp) - sp - 5);
    std::string note;
    if (waitReadable(cbs, 5000)) {
        net::socket_t c = ::accept(cbs, nullptr, nullptr);
        char buf[8192];
        while (waitReadable(c, 1000)) {
            const long long n = net::recvSome(c, buf, sizeof(buf));
            if (n <= 0) break;
            note.append(buf, static_cast<size_t>(n));
            if (contains(note, "</e:propertyset>")) break;
        }
        const std::string ok = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
        net::sendSome(c, ok.data(), ok.size());
        net::closeSocket(c);
    }
    CHECK(note.rfind("NOTIFY /evt HTTP/1.1", 0) == 0 && contains(note, "SEQ: 0") && contains(note, "SID: " + sid) &&
              contains(note, "NTS: upnp:propchange") && contains(note, "&lt;TransportState val=&quot;STOPPED&quot;/&gt;"),
          "kezdő esemény (SEQ 0, teljes LastChange)");
    resp = httpCall(ifc.ip, port, "SUBSCRIBE /evt/avt HTTP/1.1\r\nHost: x\r\nSID: " + sid + "\r\nTIMEOUT: Second-600\r\nConnection: close\r\n\r\n");
    CHECK(resp.rfind("HTTP/1.1 200 OK", 0) == 0 && contains(resp, "TIMEOUT: Second-600"), "megújítás");
    resp = httpCall(ifc.ip, port, "SUBSCRIBE /evt/avt HTTP/1.1\r\nHost: x\r\nCALLBACK: <http://127.0.0.1:1/x>\r\nNT: upnp:event\r\nConnection: close\r\n\r\n");
    CHECK(resp.rfind("HTTP/1.1 412", 0) == 0, "loopback visszahívás: 412");
    resp = httpCall(ifc.ip, port, "UNSUBSCRIBE /evt/avt HTTP/1.1\r\nHost: x\r\nSID: " + sid + "\r\nConnection: close\r\n\r\n");
    CHECK(resp.rfind("HTTP/1.1 200 OK", 0) == 0 && server.subscriptions() == 0, "UNSUBSCRIBE");
    resp = httpCall(ifc.ip, port, "UNSUBSCRIBE /evt/avt HTTP/1.1\r\nHost: x\r\nSID: " + sid + "\r\nConnection: close\r\n\r\n");
    CHECK(resp.rfind("HTTP/1.1 412", 0) == 0, "ismeretlen SID: 412");
    net::closeSocket(cbs);
    // csak az engedélyezett vezérlő
    server.stop();
    sc.allow = { ipOf("192.0.2.1") };
    CHECK(server.start(sc, err), "újraindítás engedélyezési listával");
    resp = httpCall(ifc.ip, server.port(), "GET /description.xml HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(resp.empty(), "nem engedélyezett vezérlő: a kapcsolat bezárul");
    server.stop();
    resp = httpCall(ifc.ip, port, "GET /description.xml HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(resp.empty(), "leállítás után nem válaszol");
}

int main() {
    net::startup();
    testXml();
    testSoap();
    testTime();
    testDidl();
    testEvents();
    testUrlRule();
    testSsdp();
    testRenderer();
    const auto ifs = privateIfaces();
    if (ifs.empty()) std::printf("(nincs magánhálózati cím: a letöltés és a HTTP-szolgáltatás próbája kimarad)\n");
    else testDownload(ifs.front());
    net::cleanup();
    std::printf(g_fail ? "\n%d HIBA\n" : "\nMINDEN TESZT SIKERES\n", g_fail);
    return g_fail ? 1 : 0;
}
