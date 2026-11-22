// Az internetrádió tesztje: a tiszta segédek (cím, címosztályozás, ICY, formátum, lista, fejléc), és egy helyi
// próbaszerverrel (127.0.0.1, a teszt maga indítja) a valódi szállítás (Linux / macOS: curl, Windows: WinHTTP):
// ICY metaadat, átirányítás, lejátszási lista, megszakadás és újracsatlakozás, elakadás, tiltott címek (átirányítás
// után is), nem támogatott formátumok, megszakítás, a FilePlayer és a vezérlő / híd üzenetei.
// A hangfájlok: tests/data/player_stereo44.{mp3,ogg,flac}, player_stereo48.opus.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "file_player.hpp"
#include "net_compat.hpp"
#include "radio_bridge.hpp"
#include "radio_net.hpp"
#include "radio_stream.hpp"

using namespace ee_analyzer;
using namespace ee_analyzer::radio;
using Clock = std::chrono::steady_clock;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; std::printf("  [HIBA] " __VA_ARGS__); } else { std::printf("  [OK]   " __VA_ARGS__); } std::printf("\n"); std::fflush(stdout); } while (0)

static const std::string kData = std::getenv("AQ_TEST_DATA_DIR") ? std::getenv("AQ_TEST_DATA_DIR") : AQ_TEST_DATA_DIR;

static std::vector<uint8_t> readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::mutex searchM;     // a próbaszerver utolsó katalógus-kérése
static std::string lastSearch;

static double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// ------------------------------------------------------------------------------------------------ próbaszerver

/** ICY-folyam: metaint bájtonként egy metaadat-blokk (meta(i): az i. blokk szövege; üres: 0 hosszú blokk). */
static std::vector<uint8_t> icyStream(const std::vector<uint8_t>& audio, size_t metaint, const std::function<std::string(int)>& meta) {
    std::vector<uint8_t> out;
    int block = 0;
    for (size_t off = 0; off < audio.size(); off += metaint) {
        const size_t n = std::min(metaint, audio.size() - off);
        out.insert(out.end(), audio.begin() + static_cast<std::ptrdiff_t>(off), audio.begin() + static_cast<std::ptrdiff_t>(off + n));
        if (n < metaint) break;
        std::string m = meta(block++);
        while (m.size() % 16) m += '\0';
        out.push_back(static_cast<uint8_t>(m.size() / 16));
        out.insert(out.end(), m.begin(), m.end());
    }
    return out;
}

// ---- Ogg FLAC (láncolt rádióadás, mint a valódi FLAC-adóknál: számonként új logikai folyam új sorozatszámmal)

static uint32_t oggCrc(const uint8_t* p, size_t n) {
    static uint32_t tab[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t r = i << 24;
            for (int k = 0; k < 8; ++k) r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : (r << 1);
            tab[i] = r;
        }
        init = true;
    }
    uint32_t crc = 0;
    for (size_t i = 0; i < n; ++i) crc = (crc << 8) ^ tab[((crc >> 24) ^ p[i]) & 0xFF];
    return crc;
}

static void oggPage(std::vector<uint8_t>& out, const std::vector<uint8_t>& body, uint8_t flags, uint32_t serial, uint32_t seq, uint64_t granule) {
    std::vector<uint8_t> pg = { 'O', 'g', 'g', 'S', 0, flags };
    for (int i = 0; i < 8; ++i) pg.push_back(static_cast<uint8_t>(granule >> (8 * i)));
    for (int i = 0; i < 4; ++i) pg.push_back(static_cast<uint8_t>(serial >> (8 * i)));
    for (int i = 0; i < 4; ++i) pg.push_back(static_cast<uint8_t>(seq >> (8 * i)));
    for (int i = 0; i < 4; ++i) pg.push_back(0);
    std::vector<uint8_t> segs;
    size_t left = body.size();
    while (left >= 255) { segs.push_back(255); left -= 255; }
    segs.push_back(static_cast<uint8_t>(left));
    pg.push_back(static_cast<uint8_t>(segs.size()));
    pg.insert(pg.end(), segs.begin(), segs.end());
    pg.insert(pg.end(), body.begin(), body.end());
    const uint32_t crc = oggCrc(pg.data(), pg.size());
    for (int i = 0; i < 4; ++i) pg[22 + i] = static_cast<uint8_t>(crc >> (8 * i));
    out.insert(out.end(), pg.begin(), pg.end());
}

/** Natív FLAC fájlból egy Ogg FLAC lánc (fejléc-csomag, Vorbis-megjegyzés TITLE / ARTIST, a keretek lapokban). */
static std::vector<uint8_t> oggFlacChain(const std::vector<uint8_t>& flac, uint32_t serial, const std::string& artist, const std::string& title) {
    std::vector<uint8_t> out;
    if (flac.size() < 42 || std::memcmp(flac.data(), "fLaC", 4) != 0) return out;
    size_t p = 4;
    std::vector<uint8_t> streaminfo;
    for (;;) {
        const bool last = (flac[p] & 0x80) != 0;
        const int type = flac[p] & 0x7F;
        const size_t len = (size_t(flac[p + 1]) << 16) | (size_t(flac[p + 2]) << 8) | flac[p + 3];
        if (type == 0) streaminfo.assign(flac.begin() + p + 4, flac.begin() + p + 4 + len);
        p += 4 + len;
        if (last) break;
    }
    std::vector<uint8_t> head = { 0x7F, 'F', 'L', 'A', 'C', 1, 0, 0, 1, 'f', 'L', 'a', 'C', 0x00, 0, 0, 34 };
    head.insert(head.end(), streaminfo.begin(), streaminfo.end());
    uint32_t seq = 0;
    oggPage(out, head, 0x02, serial, seq++, 0);
    auto le32 = [](std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); };
    std::vector<uint8_t> vc;
    const std::string vendor = "teszt", c1 = "TITLE=" + title, c2 = "ARTIST=" + artist;
    le32(vc, static_cast<uint32_t>(vendor.size())); vc.insert(vc.end(), vendor.begin(), vendor.end());
    le32(vc, 2);
    le32(vc, static_cast<uint32_t>(c1.size())); vc.insert(vc.end(), c1.begin(), c1.end());
    le32(vc, static_cast<uint32_t>(c2.size())); vc.insert(vc.end(), c2.begin(), c2.end());
    std::vector<uint8_t> vcBlock = { 0x84, static_cast<uint8_t>(vc.size() >> 16), static_cast<uint8_t>(vc.size() >> 8), static_cast<uint8_t>(vc.size()) };
    vcBlock.insert(vcBlock.end(), vc.begin(), vc.end());
    oggPage(out, vcBlock, 0, serial, seq++, 0);
    for (size_t off = p; off < flac.size(); off += 4000) {
        const size_t n = std::min<size_t>(4000, flac.size() - off);
        std::vector<uint8_t> body(flac.begin() + off, flac.begin() + off + n);
        oggPage(out, body, off + n >= flac.size() ? 0x04 : 0, serial, seq++, 0);
    }
    return out;
}

class FakeServer {
public:
    using Handler = std::function<void(net::socket_t s, const std::string& path, FakeServer& srv)>;
    explicit FakeServer(Handler h) : handler_(std::move(h)) {
        ls_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(ls_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        ::bind(ls_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ::listen(ls_, 16);
        socklen_t len = sizeof(a);
        ::getsockname(ls_, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        th_ = std::thread([this] { acceptLoop(); });
    }
    ~FakeServer() {
        stop = true;
        if (th_.joinable()) th_.join();
        std::vector<std::thread> ws;
        {
            std::lock_guard<std::mutex> lk(m_);
            ws.swap(workers_);
        }
        for (auto& w : ws) w.join();
        net::closeSocket(ls_);
    }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port) + path; }
    int hits(const std::string& path) {
        std::lock_guard<std::mutex> lk(m_);
        return hits_[path];
    }
    int totalHits() {
        std::lock_guard<std::mutex> lk(m_);
        int n = 0;
        for (auto& kv : hits_) n += kv.second;
        return n;
    }
    /** Mindent elküld; false: a kliens bontott (vagy a szerver leáll). */
    bool sendAll(net::socket_t s, const void* p, size_t n) {
        const char* c = static_cast<const char*>(p);
        while (n > 0) {
            if (stop) return false;
#ifdef MSG_NOSIGNAL
            const int flags = MSG_NOSIGNAL;
#else
            const int flags = 0;
#endif
            const int k = static_cast<int>(::send(s, c, static_cast<int>(std::min<size_t>(n, 16384)), flags));
            if (k <= 0) return false;
            c += k;
            n -= static_cast<size_t>(k);
        }
        return true;
    }
    bool sendStr(net::socket_t s, const std::string& t) { return sendAll(s, t.data(), t.size()); }
    /** Vár (a leállítást figyelve). */
    void idle(int ms) {
        for (int i = 0; i < ms / 10 && !stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::atomic<bool> stop{ false };
    uint16_t port = 0;

private:
    void acceptLoop() {
        while (!stop) {
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(ls_, &rs);
            timeval tv{ 0, 100000 };
            if (::select(static_cast<int>(ls_) + 1, &rs, nullptr, nullptr, &tv) <= 0) continue;
            net::socket_t c = ::accept(ls_, nullptr, nullptr);
            if (c == net::kInvalidSocket) continue;
            std::lock_guard<std::mutex> lk(m_);
            workers_.emplace_back([this, c] { serve(c); });
        }
    }
    void serve(net::socket_t c) {
        std::string req;
        char buf[2048];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
            const int n = static_cast<int>(::recv(c, buf, sizeof(buf), 0));
            if (n <= 0) break;
            req.append(buf, static_cast<size_t>(n));
        }
        std::string path;
        const size_t sp = req.find(' ');
        if (sp != std::string::npos) path = req.substr(sp + 1, req.find(' ', sp + 1) - sp - 1);
        {
            std::lock_guard<std::mutex> lk(m_);
            hits_[path]++;
            if (req.find("Icy-MetaData: 1") != std::string::npos || req.find("icy-metadata: 1") != std::string::npos) icyAsked = true;
        }
        handler_(c, path, *this);
        net::closeSocket(c);
    }

public:
    std::atomic<bool> icyAsked{ false };

private:
    Handler handler_;
    net::socket_t ls_ = net::kInvalidSocket;
    std::thread th_;
    std::mutex m_;
    std::vector<std::thread> workers_;
    std::map<std::string, int> hits_;
};

// ------------------------------------------------------------------------------------------------ segédek

static NetOptions testNet() {
    NetOptions o;
    o.allowLoopback = true;
    o.connectTimeoutS = 3;
    o.headTimeoutS = 4;
    o.stallTimeoutS = 2;
    return o;
}

static RadioDecoder::Config testCfg(const std::string& url) {
    RadioDecoder::Config c;
    c.url = url;
    c.station = "Próba Rádió";
    c.net = testNet();
    c.prebufferSec = 0.3;
    c.backoffBaseMs = 50;
    return c;
}

/** Olvasás a dekóderből (a lejátszó módjára, nem blokkol) legfeljebb ms ideig; a nem csendes keretek száma. */
static size_t pump(RadioDecoder& d, int ms, const std::function<bool()>& until = nullptr) {
    std::vector<float> buf(1024 * d.channels());
    size_t loud = 0;
    const auto t0 = Clock::now();
    while (msSince(t0) < ms) {
        const size_t n = d.read(buf.data(), 1024);
        if (n == 0) break;
        for (size_t i = 0; i < n; ++i) {
            float e = 0;
            for (uint32_t c = 0; c < d.channels(); ++c) e += std::fabs(buf[i * d.channels() + c]);
            if (e > 1e-3f) ++loud;
        }
        if (until && until()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return loud;
}

/** Próba-kimenet a FilePlayerhez: a render egy saját szálon, valós idő körül fut. */
class PullOutput : public player::PlayerOutput {
public:
    ~PullOutput() override { close(); }
    bool open(uint32_t sr, RenderFn r, std::string*) override {
        close();
        std::lock_guard<std::mutex> l(m_);
        rate = sr;
        render_ = std::move(r);
        run_ = true;
        th_ = std::thread([this] {
            std::vector<float> buf(2 * 512);
            while (run_) {
                {
                    std::lock_guard<std::mutex> l2(m_);
                    if (active_ && render_) {
                        render_(buf.data(), 512);
                        for (float v : buf) if (std::fabs(v) > 1e-3f) { ++loud; break; }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        return true;
    }
    void close() override {
        run_ = false;
        if (th_.joinable()) th_.join();
        std::lock_guard<std::mutex> l(m_);
        render_ = nullptr;
        active_ = false;
    }
    void setActive(bool a) override { std::lock_guard<std::mutex> l(m_); active_ = a; }
    void lock() override { m_.lock(); }
    void unlock() override { m_.unlock(); }
    std::atomic<uint32_t> rate{ 0 };
    std::atomic<int> loud{ 0 };

private:
    std::mutex m_;
    RenderFn render_;
    bool active_ = false;
    std::atomic<bool> run_{ false };
    std::thread th_;
};

// ------------------------------------------------------------------------------------------------ tesztek

static void testPure() {
    std::printf("\n[1] Tiszta segédek\n");
    RadioUrl u;
    std::string e;
    CHECK(parseRadioUrl("http://Stream.Example.com:8000/live.mp3?x=1#frag", u, &e) && !u.https && u.host == "stream.example.com" &&
          u.port == 8000 && u.target == "/live.mp3?x=1", "http cím felbontása (kisbetűs hoszt, port, # nélkül)");
    CHECK(parseRadioUrl("https://radio.example.org", u) && u.https && u.target == "/" && u.effectivePort() == 443, "https, üres út -> /");
    CHECK(parseRadioUrl("http://[2001:db8::1]:8080/a", u) && u.ipv6 && u.host == "2001:db8::1" && u.port == 8080, "IPv6-cím zárójelben");
    CHECK(!parseRadioUrl("ftp://x.example/a", u, &e) && e == "scheme", "más séma tiltva");
    CHECK(!parseRadioUrl("file:///etc/passwd", u), "file: tiltva");
    CHECK(!parseRadioUrl("http://user:pw@example.com/", u, &e) && e == "userinfo", "felhasználónév tiltva");
    CHECK(!parseRadioUrl("http://example.com:0/", u) && !parseRadioUrl("http://example.com:70000/", u) &&
          !parseRadioUrl("http://example.com:8a/", u), "hibás port");
    CHECK(!parseRadioUrl("http://exa mple.com/", u) && !parseRadioUrl("http://example.com/\"x", u) && !parseRadioUrl("http:///x", u),
          "szóköz, idézőjel, üres hoszt");
    CHECK(!parseRadioUrl("http://ex_ample.com/", u) && !parseRadioUrl("http://a..b/", u), "hibás hosztnév");
    CHECK(!parseRadioUrl(std::string("http://e.com/") + std::string(3000, 'a'), u), "túl hosszú cím");
    CHECK(!parseRadioUrl("http://e.com/a%00b", u), "kódolt null bájt");

    RadioUrl base, r;
    parseRadioUrl("http://a.example:8000/dir/list.pls?q=1", base);
    CHECK(resolveLocation(base, "/stream", r) && r.str() == "http://a.example:8000/stream", "Location: abszolút út");
    CHECK(resolveLocation(base, "live.mp3", r) && r.str() == "http://a.example:8000/dir/live.mp3", "Location: relatív");
    CHECK(resolveLocation(base, "//b.example/x", r) && r.str() == "http://b.example/x", "Location: séma nélküli");
    CHECK(resolveLocation(base, "https://c.example/y", r) && r.https && r.host == "c.example", "Location: teljes cím");
    CHECK(!resolveLocation(base, "ftp://c.example/y", r) && !resolveLocation(base, "", r), "Location: tiltott séma / üres");

    auto v4 = [](int a, int b, int c, int d) { const uint8_t x[4] = { uint8_t(a), uint8_t(b), uint8_t(c), uint8_t(d) }; return isPublicV4(x); };
    CHECK(v4(8, 8, 8, 8) && v4(1, 1, 1, 1) && v4(93, 184, 216, 34) && v4(172, 32, 0, 1) && v4(100, 128, 0, 1), "nyilvános IPv4");
    CHECK(!v4(127, 0, 0, 1) && !v4(127, 9, 9, 9) && !v4(10, 1, 2, 3) && !v4(172, 16, 0, 1) && !v4(172, 31, 255, 255) &&
          !v4(192, 168, 1, 1) && !v4(169, 254, 169, 254) && !v4(100, 64, 0, 1) && !v4(0, 0, 0, 0) && !v4(224, 0, 0, 251) &&
          !v4(255, 255, 255, 255) && !v4(198, 18, 0, 1) && !v4(192, 0, 2, 1) && !v4(240, 0, 0, 1),
          "tiltott IPv4: loopback, magán, link-local, CGNAT, 0/8, multicast, broadcast, fenntartott, dokumentáció");
    uint8_t g[16] = { 0x26, 0x06, 0x47, 0x00 };   // 2606:4700::
    g[15] = 1;
    CHECK(isPublicV6(g), "nyilvános IPv6 (2000::/3)");
    uint8_t lo[16] = {}; lo[15] = 1;
    uint8_t ula[16] = { 0xFD, 0x12 };
    uint8_t ll[16] = { 0xFE, 0x80 };
    uint8_t mc[16] = { 0xFF, 0x02 };
    uint8_t doc6[16] = { 0x20, 0x01, 0x0D, 0xB8 };
    uint8_t mapped[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 10, 0, 0, 1 };
    uint8_t mappedPub[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 8, 8, 8, 8 };
    uint8_t sixto4[16] = { 0x20, 0x02, 192, 168, 0, 1 };
    uint8_t nat64[16] = { 0, 0x64, 0xFF, 0x9B, 0, 0, 0, 0, 0, 0, 0, 0, 127, 0, 0, 1 };
    uint8_t any6[16] = {};
    const uint8_t lo4[4] = { 127, 0, 0, 1 };
    CHECK(!isPublicV6(lo) && !isPublicV6(ula) && !isPublicV6(ll) && !isPublicV6(mc) && !isPublicV6(doc6) && !isPublicV6(any6),
          "tiltott IPv6: ::1, fc00::/7, fe80::/10, ff00::/8, dokumentáció, ::");
    CHECK(!isPublicV6(mapped) && isPublicV6(mappedPub) && !isPublicV6(sixto4) && !isPublicV6(nat64),
          "IPv6-ba ágyazott IPv4 (::ffff:, 2002::, 64:ff9b::) a beágyazott cím szerint");
    CHECK(isLoopbackV6(lo) && isLoopbackV4(lo4), "loopback felismerése");

    // ICY: darabokban és egyben ugyanazt adja
    std::vector<uint8_t> audio(10000);
    for (size_t i = 0; i < audio.size(); ++i) audio[i] = static_cast<uint8_t>(i * 7);
    const auto stream = icyStream(audio, 1000, [](int i) { return i == 2 ? std::string("StreamTitle='Előadó - Cím';StreamUrl='';") : std::string(); });
    std::vector<uint8_t> a1, a2;
    std::vector<std::string> m1, m2;
    IcyDemux d1(1000), d2(1000);
    d1.feed(stream.data(), stream.size(), a1, m1);
    for (uint8_t b : stream) d2.feed(&b, 1, a2, m2);
    CHECK(a1 == audio && a2 == audio, "ICY: a hang bájtjai változatlanok (egyben és bájtonként)");
    CHECK(m1.size() == 1 && m2.size() == 1 && icyStreamTitle(m1[0]) == "Előadó - Cím", "ICY: StreamTitle kinyerése");
    CHECK(icyStreamTitle("StreamTitle='It''s ok';") == "It''s ok" && icyStreamTitle("Foo=1;").empty(), "ICY: aposztróf, hiányzó cím");
    CHECK(icyStreamTitle(std::string("StreamTitle='Caf\xE9';")) == "Caf\xC3\xA9", "ICY: Latin-1 -> UTF-8");
    CHECK(cleanText(std::string("a\x01" "b\nc")) == "a b c" && cleanText(std::string(400, 'x')).size() == 256, "szöveg tisztítása, hosszkorlát");
    std::string at, ti;
    splitArtistTitle("Előadó - Cím - Remix", at, ti);
    CHECK(at == "Előadó" && ti == "Cím - Remix", "előadó / cím szétbontása");
    splitArtistTitle("Csak cím", at, ti);
    CHECK(at.empty() && ti == "Csak cím", "cím előadó nélkül");

    auto sniff = [](const std::string& ct, const std::string& b) {
        return sniffFormat(ct, reinterpret_cast<const uint8_t*>(b.data()), b.size());
    };
    CHECK(sniff("audio/mpeg", "ID3\x04") == StreamFormat::Mp3 && sniff("", "\xFF\xFB\x90\x00") == StreamFormat::Mp3, "MP3 felismerése");
    CHECK(sniff("audio/aacp", "\xFF\xF1\x50\x80") == StreamFormat::Aac && sniff("audio/mpeg", "\xFF\xF9\x50\x80") == StreamFormat::Aac,
          "AAC (ADTS) felismerése, a Content-Type ellenére is");
    CHECK(sniff("", "OggS\x00\x02") == StreamFormat::Ogg && sniff("", "fLaC") == StreamFormat::Flac, "Ogg és FLAC");
    CHECK(sniff("audio/x-scpls", "[playlist]\nFile1=http://a/") == StreamFormat::Playlist &&
          sniff("text/plain", "#EXTM3U\nhttp://a/b") == StreamFormat::Playlist, "PLS / M3U");
    CHECK(sniff("application/vnd.apple.mpegurl", "#EXTM3U\n#EXT-X-VERSION:3\n") == StreamFormat::Hls, "HLS");
    CHECK(sniff("text/html", "<!DOCTYPE html>") == StreamFormat::Html && sniff("application/octet-stream", "\x01\x02") == StreamFormat::Unknown,
          "HTML és ismeretlen");
    const auto pls = playlistUrls("[playlist]\r\nNumberOfEntries=2\r\nFile1=http://a.example/1\r\nTitle1=X\r\nFile2=https://b.example/2\r\nVersion=2\r\n");
    CHECK(pls.size() == 2 && pls[0] == "http://a.example/1" && pls[1] == "https://b.example/2", "PLS címei");
    const auto m3u = playlistUrls("#EXTM3U\n#EXTINF:-1,Adó\nhttp://c.example/live\n\nftp://x/y\n");
    CHECK(m3u.size() == 1 && m3u[0] == "http://c.example/live", "M3U címei (csak http/https)");

    HttpHead h;
    CHECK(parseHttpHead("HTTP/1.1 302 Found\r\nLocation: /x \r\nContent-Type: audio/mpeg\r\n\r\n", h) && h.status == 302 &&
          h.get("location") == "/x" && h.get("content-type") == "audio/mpeg", "HTTP/1.1 fejléc");
    CHECK(parseHttpHead("ICY 200 OK\r\nicy-metaint:8192\r\n", h) && h.status == 200 && h.get("icy-metaint") == "8192", "ICY 200 OK");
    CHECK(parseHttpHead("HTTP/2 200\r\ncontent-type: audio/ogg\r\n", h) && h.status == 200, "HTTP/2");
    CHECK(!parseHttpHead("<html>", h) && !parseHttpHead("HTTP/1.1 2x0 OK", h) && !parseHttpHead("", h), "hibás fejléc");
}

static void testResolve() {
    std::printf("\n[2] Névfeloldás és címellenőrzés (kapcsolódás nélkül)\n");
    RadioUrl u;
    std::string ip, e;
    parseRadioUrl("http://127.0.0.1/", u);
    CHECK(!resolvePublic(u, false, ip, e) && e == "blocked", "127.0.0.1 tiltott");
    CHECK(resolvePublic(u, true, ip, e) && ip == "127.0.0.1", "127.0.0.1 a teszt-beállítással engedélyezett");
    parseRadioUrl("http://[::1]/", u);
    CHECK(!resolvePublic(u, false, ip, e) && e == "blocked", "[::1] tiltott");
    parseRadioUrl("http://localhost/", u);
    CHECK(!resolvePublic(u, false, ip, e) && e == "blocked", "localhost (név) tiltott");
    for (const char* bad : { "http://10.1.2.3/", "http://192.168.0.1/", "http://169.254.169.254/latest/meta-data/",
                             "http://0.0.0.0/", "http://[fe80::1]/", "http://[::ffff:10.0.0.1]/", "http://100.64.1.1/" }) {
        parseRadioUrl(bad, u);
        CHECK(!resolvePublic(u, true, ip, e) && e == "blocked", "%s tiltott (a loopback-engedély mellett is)", bad);
    }
}

static void testNetwork() {
    std::printf("\n[3] Próbaszerverrel\n");
    const auto mp3 = readFile(kData + "/player_stereo44.mp3");
    const auto ogg = readFile(kData + "/player_stereo44.ogg");
    const auto opus = readFile(kData + "/player_stereo48.opus");
    const auto flac = readFile(kData + "/player_stereo44.flac");
    const auto aac = readFile(kData + "/player_stereo44.aac");   // saját szinusz (tools/make_aac_test_data.cpp)
    CHECK(!mp3.empty() && !ogg.empty() && !opus.empty() && !flac.empty(), "tesztfájlok (%zu, %zu, %zu, %zu bájt)", mp3.size(), ogg.size(), opus.size(), flac.size());
    std::vector<uint8_t> mp3x3;
    for (int i = 0; i < 3; ++i) mp3x3.insert(mp3x3.end(), mp3.begin(), mp3.end());
    const auto icyBody = icyStream(mp3x3, 4096, [](int i) {
        if (i == 0) return std::string("StreamTitle='Előadó - Első szám';");
        if (i == 4) return std::string("StreamTitle='Másik - Második';");
        return std::string();
    });

    FakeServer srv([&](net::socket_t s, const std::string& path, FakeServer& sv) {
        const std::string self = "http://127.0.0.1:" + std::to_string(sv.port);
        if (path == "/mp3" || path == "/icystatus") {
            sv.sendStr(s, std::string(path == "/icystatus" ? "ICY 200 OK\r\n" : "HTTP/1.0 200 OK\r\n") +
                              "Content-Type: audio/mpeg\r\nicy-metaint: 4096\r\nicy-name: Szerver Neve\r\nicy-br: 128\r\n\r\n");
            sv.sendAll(s, icyBody.data(), icyBody.size());   // utána bont: a kliens újracsatlakozik
        } else if (path == "/live") {
            sv.sendStr(s, "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\n\r\n");
            while (!sv.stop && sv.sendAll(s, mp3.data(), mp3.size())) sv.idle(250);
        } else if (path == "/redir") {
            sv.sendStr(s, "HTTP/1.1 302 Found\r\nLocation: /mp3\r\nContent-Length: 0\r\n\r\n");
        } else if (path == "/redir-abs") {
            sv.sendStr(s, "HTTP/1.1 301 Moved\r\nLocation: " + self + "/mp3\r\nContent-Length: 0\r\n\r\n");
        } else if (path == "/redir-private") {
            sv.sendStr(s, "HTTP/1.1 302 Found\r\nLocation: http://10.11.12.13:8000/stream\r\nContent-Length: 0\r\n\r\n");
        } else if (path == "/redir-meta") {
            sv.sendStr(s, "HTTP/1.1 307 Temporary\r\nLocation: http://169.254.169.254/latest/meta-data/\r\nContent-Length: 0\r\n\r\n");
        } else if (path == "/redir-loop") {
            sv.sendStr(s, "HTTP/1.1 302 Found\r\nLocation: /redir-loop\r\nContent-Length: 0\r\n\r\n");
        } else if (path == "/pls") {
            sv.sendStr(s, "HTTP/1.0 200 OK\r\nContent-Type: audio/x-scpls\r\n\r\n[playlist]\nNumberOfEntries=1\nFile1=" + self + "/mp3\nTitle1=X\n");
        } else if (path == "/pls-private") {
            sv.sendStr(s, "HTTP/1.0 200 OK\r\nContent-Type: audio/x-scpls\r\n\r\n[playlist]\nFile1=http://192.168.1.1/\n");
        } else if (path == "/hls") {
            sv.sendStr(s, "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\n\r\n#EXTM3U\n#EXT-X-VERSION:3\n#EXTINF:10,\nseg1.ts\n");
        } else if (path == "/aac") {
            sv.sendStr(s, "HTTP/1.1 200 OK\r\nContent-Type: audio/aac\r\n\r\n");
            sv.sendAll(s, aac.data(), aac.size());
        } else if (path == "/aacjunk") {
            sv.sendStr(s, "HTTP/1.1 200 OK\r\nContent-Type: audio/aacp\r\n\r\n");
            const unsigned char adts[] = { 0xFF, 0xF1, 0x50, 0x80, 0x02, 0x1F, 0xFC, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
            for (int i = 0; i < 64; ++i) sv.sendAll(s, adts, sizeof(adts));
        } else if (path == "/html") {
            sv.sendStr(s, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<!DOCTYPE html><html><body>Hello</body></html>");
        } else if (path == "/404") {
            sv.sendStr(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
        } else if (path == "/bighead") {
            sv.sendStr(s, "HTTP/1.1 200 OK\r\nX-Pad: " + std::string(40000, 'a') + "\r\n\r\n");
        } else if (path == "/ogg" || path == "/opus" || path == "/flac") {
            const auto& body = path == "/ogg" ? ogg : path == "/opus" ? opus : flac;
            sv.sendStr(s, std::string("HTTP/1.1 200 OK\r\nContent-Type: ") + (path == "/flac" ? "audio/flac" : "audio/ogg") + "\r\n\r\n");
            sv.sendAll(s, body.data(), body.size());
        } else if (path == "/oggflac" || path == "/vorbis2") {
            // két lánc egymás után (mint a számváltás egy valódi adásban), utána bont
            std::vector<uint8_t> body;
            if (path == "/oggflac") {
                body = oggFlacChain(flac, 0x1111, "Egy", "Első");
                const auto second = oggFlacChain(flac, 0x2222, "Kettő", "Második");
                body.insert(body.end(), second.begin(), second.end());
            } else {
                body = ogg;
                body.insert(body.end(), ogg.begin(), ogg.end());
            }
            sv.sendStr(s, "HTTP/1.1 200 OK\r\nContent-Type: audio/ogg\r\n\r\n");
            sv.sendAll(s, body.data(), body.size());
        } else if (path == "/stall") {
            sv.sendStr(s, "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\n\r\n");
            sv.sendAll(s, mp3.data(), mp3.size());
            sv.idle(8000);   // elakad: a kliens a stallTimeout után bont és újracsatlakozik
        } else if (path == "/silent") {
            sv.idle(8000);   // fejlécet sem küld
        } else if (path.rfind("/json/stations/search?", 0) == 0) {
            // a katalógus próbaválasza: egy jó, egy tiltott sémájú, egy HLS-es, egy név nélküli és egy https-es tétel
            {
                std::lock_guard<std::mutex> lk(searchM);
                lastSearch = path;
            }
            const std::string body =
                "[{\"name\":\"Kék Duna Rádió\",\"url\":\"http://x.example/a\",\"url_resolved\":\"http://x.example/a.mp3\",\"codec\":\"MP3\","
                "\"bitrate\":128,\"countrycode\":\"HU\",\"tags\":\"pop,hírek\",\"hls\":0},"
                "{\"name\":\"Rossz\",\"url_resolved\":\"javascript:alert(1)\",\"codec\":\"MP3\"},"
                "{\"name\":\"Szegmentált\",\"url_resolved\":\"https://h.example/live.m3u8\",\"codec\":\"AAC\",\"hls\":1},"
                "{\"name\":\"\",\"url_resolved\":\"http://n.example/\"},"
                "{\"name\":\"Második \\u0001adó\",\"url\":\"https://y.example/s\",\"codec\":\"AAC+\",\"bitrate\":64,\"countrycode\":\"AT\"}]";
            sv.sendStr(s, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                              "\r\n\r\n" + body);
        } else {
            sv.sendStr(s, "HTTP/1.1 500 Error\r\n\r\n");
        }
    });

    std::string err;
    {
        auto t0 = Clock::now();
        auto d = RadioDecoder::open(testCfg(srv.url("/mp3")), err);
        CHECK(d != nullptr, "ICY-s MP3 folyam megnyílik (%s, %.0f ms)", err.c_str(), msSince(t0));
        if (d) {
            CHECK(d->sampleRate() == 44100 && d->channels() == 2, "formátum: %u Hz, %u csatorna", d->sampleRate(), d->channels());
            CHECK(srv.icyAsked.load(), "a kérés kéri az ICY metaadatot (Icy-MetaData: 1)");
            std::string t, a, al;
            const size_t loud = pump(*d, 6000, [&] { d->liveInfo(t, a, al); return srv.hits("/mp3") >= 2 && t == "Második"; });
            d->liveInfo(t, a, al);
            const RadioStatus st = d->status();
            CHECK(loud > 5000, "szól (nem csendes keretek: %zu)", loud);
            CHECK(st.codec == "mp3" && st.bitrate == 128 && st.icyName == "Szerver Neve", "állapot: %s, %d kbit/s, „%s”", st.codec.c_str(), st.bitrate, st.icyName.c_str());
            CHECK(t == "Második" && a == "Másik" && al == "Próba Rádió", "most szól (a hallott helyen): „%s” – „%s”, album „%s”", a.c_str(), t.c_str(), al.c_str());
            CHECK(st.reconnects >= 1 && srv.hits("/mp3") >= 2, "a szerver bontása után újracsatlakozik (%llu, %d kérés)",
                  static_cast<unsigned long long>(st.reconnects), srv.hits("/mp3"));
        }
    }
    {
        auto d = RadioDecoder::open(testCfg(srv.url("/redir")), err);
        CHECK(d != nullptr && srv.hits("/redir") == 1, "átirányítás (relatív Location) követése");
        auto d2 = RadioDecoder::open(testCfg(srv.url("/redir-abs")), err);
        CHECK(d2 != nullptr, "átirányítás (teljes cím)");
    }
    {
        const int before = srv.totalHits();
        auto t0 = Clock::now();
        auto d = RadioDecoder::open(testCfg(srv.url("/redir-private")), err);
        CHECK(!d && err == "blocked" && msSince(t0) < 3000, "átirányítás magánhálózatra: tiltva (%s, %.0f ms)", err.c_str(), msSince(t0));
        d = RadioDecoder::open(testCfg(srv.url("/redir-meta")), err);
        CHECK(!d && err == "blocked", "átirányítás a felhő-metaadat címre (169.254.169.254): tiltva (%s)", err.c_str());
        d = RadioDecoder::open(testCfg(srv.url("/pls-private")), err);
        CHECK(!d && err == "blocked", "lejátszási lista magánhálózati címmel: tiltva (%s)", err.c_str());
        CHECK(srv.totalHits() - before == 3, "a tiltott lépések után nincs további kérés");
        d = RadioDecoder::open(testCfg(srv.url("/redir-loop")), err);
        CHECK(!d && err == "redirect" && srv.hits("/redir-loop") <= 6, "átirányítási hurok: legfeljebb 5 lépés (%s, %d)", err.c_str(), srv.hits("/redir-loop"));
    }
    {
        RadioDecoder::Config c = testCfg(srv.url("/mp3"));
        c.net.allowLoopback = false;
        const int before = srv.totalHits();
        auto d = RadioDecoder::open(c, err);
        CHECK(!d && err == "blocked" && srv.totalHits() == before, "a helyi gép (127.0.0.1) a teszt-beállítás nélkül tiltott, kapcsolódás sincs");
        c.url = "http://10.20.30.40:8000/";
        c.net.allowLoopback = true;
        auto t0 = Clock::now();
        d = RadioDecoder::open(c, err);
        CHECK(!d && err == "blocked" && msSince(t0) < 1000, "magánhálózati cím közvetlenül: azonnal tiltva (%.0f ms)", msSince(t0));
    }
    {
        auto d = RadioDecoder::open(testCfg(srv.url("/pls")), err);
        CHECK(d && d->status().codec == "mp3", "PLS lejátszási lista követése (%s)", err.c_str());
    }
    {
        struct { const char* path; const char* want; } bad[] = { { "/hls", "hls" }, { "/aacjunk", "decode" }, { "/html", "html" },
                                                                   { "/404", "http" }, { "/bighead", "http" } };
        for (auto& b : bad) {
            auto d = RadioDecoder::open(testCfg(srv.url(b.path)), err);
            CHECK(!d && err == b.want, "%s: hiba „%s” (kapott: „%s”)", b.path, b.want, err.c_str());
        }
    }
    {
        struct { const char* path; const char* codec; uint32_t rate; } ok[] = { { "/ogg", "vorbis", 44100 }, { "/opus", "opus", 48000 },
                                                                                 { "/flac", "flac", 44100 }, { "/aac", "aac", 44100 } };
        for (auto& o : ok) {
            auto d = RadioDecoder::open(testCfg(srv.url(o.path)), err);
            CHECK(d && d->status().codec == o.codec && d->sampleRate() == o.rate && d->channels() == 2, "%s: %s, %u Hz (%s)", o.path, o.codec, o.rate, err.c_str());
            if (d) {
                size_t loud = 0;
                const auto t0 = Clock::now();
                while (msSince(t0) < 4000 && (loud < 30000 || d->status().reconnects < 1)) loud += pump(*d, 200);
                CHECK(loud > 10000, "%s: szól (%zu keret), újracsatlakozás: %llu", o.path, loud, static_cast<unsigned long long>(d->status().reconnects));
            }
        }
    }
    {
        // AAC: a dekódolt jel a kódolt szinusz (bal 440 Hz, jobb 880 Hz), nem csak „valami szól”
        CHECK(aac.size() > 10000, "AAC tesztfájl (%zu bájt)", aac.size());
        auto d = RadioDecoder::open(testCfg(srv.url("/aac")), err);
        if (d) {
            std::vector<float> x, buf(1024 * 2);
            const auto t0 = Clock::now();
            bool started = false;
            while (x.size() < 16384 * 2 && msSince(t0) < 3000) {
                const size_t n = d->read(buf.data(), 1024);
                if (n == 0) break;
                for (size_t i = 0; i < n && x.size() < 16384 * 2; ++i) {
                    if (!started && std::fabs(buf[2 * i]) < 0.05f) continue;   // kódoló-előtét és csend
                    started = true;
                    x.push_back(buf[2 * i]);
                    x.push_back(buf[2 * i + 1]);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            auto amp = [&](size_t c, double f) {
                double re = 0, im = 0;
                const size_t n = x.size() / 2;
                for (size_t i = 0; i < n; ++i) {
                    const double w = 0.5 - 0.5 * std::cos(2 * 3.14159265358979 * i / (n - 1));
                    re += w * x[2 * i + c] * std::cos(2 * 3.14159265358979 * f * i / 44100.0);
                    im -= w * x[2 * i + c] * std::sin(2 * 3.14159265358979 * f * i / 44100.0);
                }
                return 2.0 * std::sqrt(re * re + im * im) / (n * 0.5);
            };
            const double l440 = amp(0, 440), l880 = amp(0, 880), r880 = amp(1, 880), r440 = amp(1, 440);
            CHECK(x.size() == 16384 * 2 && l440 > 0.4 && r880 > 0.4 && l880 < 0.05 && r440 < 0.05,
                  "AAC: bal 440 Hz %.3f (880: %.3f), jobb 880 Hz %.3f (440: %.3f)", l440, l880, r880, r440);
        } else {
            CHECK(false, "AAC megnyitása (%s)", err.c_str());
        }
    }
    {
        // láncolt Ogg: a második lánc (új sorozatszám, új cím) is szól, ugyanazon a kapcsolaton, újracsatlakozás nélkül
        for (const char* path : { "/oggflac", "/vorbis2" }) {
            RadioDecoder::Config cfg = testCfg(srv.url(path));
            cfg.backoffBaseMs = 5000;   // a mérés alatt ne jöjjön új kapcsolat
            cfg.maxBufferSec = 30.0;    // az egész adás elférjen
            auto d = RadioDecoder::open(cfg, err);
            CHECK(d != nullptr, "%s: megnyílik (%s)", path, err.c_str());
            if (!d) continue;
            std::vector<std::string> seen;
            size_t loud = 0;
            const auto t0 = Clock::now();
            while (msSince(t0) < 2500) {
                loud += pump(*d, 100);
                std::string ti, ar, al;
                d->liveInfo(ti, ar, al);
                if (seen.empty() || seen.back() != ti) seen.push_back(ti);
            }
            const RadioStatus st = d->status();
            CHECK(loud > 80000 && srv.hits(path) == 1, "%s: mindkét lánc szól egy kapcsolaton (%zu hangos keret, %d kérés)", path, loud, srv.hits(path));
            if (std::string(path) == "/oggflac") {
                const bool both = std::find(seen.begin(), seen.end(), "Első") != seen.end() && seen.back() == "Második";
                CHECK(st.codec == "flac" && both, "%s: a „most szól” a láncváltásnál vált (utolsó: „%s”)", path, seen.empty() ? "" : seen.back().c_str());
            }
        }
    }
#ifndef _WIN32
    {
        auto d = RadioDecoder::open(testCfg(srv.url("/icystatus")), err);
        CHECK(d != nullptr, "régi „ICY 200 OK” válaszsor (%s)", err.c_str());
    }
#endif
    {
        auto d = RadioDecoder::open(testCfg(srv.url("/stall")), err);
        CHECK(d != nullptr, "elakadó folyam megnyílik (%s)", err.c_str());
        if (d) {
            pump(*d, 8000, [&] { return srv.hits("/stall") >= 2; });
            CHECK(d->status().reconnects >= 1 && srv.hits("/stall") >= 2, "adat nélkül 2 s után bont és újracsatlakozik (%d kérés)", srv.hits("/stall"));
        }
    }
    {
        std::atomic<bool> cancel{ false };
        std::thread canceller([&] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); cancel = true; });
        auto t0 = Clock::now();
        auto d = RadioDecoder::open(testCfg(srv.url("/silent")), err, &cancel);
        const double ms = msSince(t0);
        canceller.join();
        CHECK(!d && err == "aborted" && ms < 1500, "kapcsolódás közbeni megszakítás (%s, %.0f ms)", err.c_str(), ms);
    }
    {
        auto d = RadioDecoder::open(testCfg(srv.url("/live")), err);
        CHECK(d != nullptr, "élő folyam (%s)", err.c_str());
        auto t0 = Clock::now();
        d.reset();
        CHECK(msSince(t0) < 1000, "a dekóder megszűnése a szól folyamnál gyors (%.0f ms)", msSince(t0));
    }

    // FilePlayer: a folyam a lejátszón át szól, élő „most szól” adatokkal
    {
        auto outp = std::make_unique<PullOutput>();
        PullOutput* out = outp.get();
        player::FilePlayer fp(std::move(outp));
        auto d = RadioDecoder::open(testCfg(srv.url("/mp3")), err);
        CHECK(d != nullptr, "FilePlayer: folyam megnyitva");
        if (d) {
            std::string perr;
            CHECK(fp.openStream(std::move(d), "Próba Rádió", &perr) && fp.state() == player::FilePlayer::State::Playing, "FilePlayer: szól (%s)", perr.c_str());
            for (int i = 0; i < 300 && out->loud.load() < 20; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            CHECK(out->loud.load() >= 20 && out->rate.load() == 44100, "FilePlayer: a kimenetre hang jut (%d blokk)", out->loud.load());
            media::Info info;
            for (int i = 0; i < 300; ++i) {
                info = fp.info();
                if (info.title == "Első szám" || info.title == "Második") break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            CHECK(info.live && info.album == "Próba Rádió" && !info.canSeek && info.duration == 0.0 &&
                  (info.title == "Első szám" || info.title == "Második"), "FilePlayer: élő adatok (cím „%s”, album „%s”)", info.title.c_str(), info.album.c_str());
            CHECK(media::toJson(info).find("\"live\":true") != std::string::npos, "media JSON: live mező");
            fp.stop();
            CHECK(fp.state() == player::FilePlayer::State::Idle, "FilePlayer: leállítás");
        }
    }

    // Vezérlő, állomáslista, híd
    {
        std::string saved;
        StationStore store([&](const std::string& doc) { saved = doc; });
        std::mutex bm;
        std::vector<std::string> broadcasts;
        BridgeCallbacks cb;   // a vezérlő előtt: a kereső szála a vezérlő megszűnéséig hívhatja
        cb.broadcast = [&](const std::string& j) { std::lock_guard<std::mutex> lk(bm); broadcasts.push_back(j); };
        player::FilePlayer fp(std::make_unique<PullOutput>());
        RadioController ctl(fp, store, testNet());
        ctl.setCatalogBase(srv.url(""));
        attachRadio(cb, ctl, store);
        auto req = [&](const std::string& j, bool remote) {
            json::Value v;
            json::parse(j, v);
            const json::Value* t = v.get("type");
            return cb.radioRequest(t ? t->stringOr("") : "", v, remote);
        };
        CHECK(req("{\"type\":\"radio_add\",\"name\":\"X\",\"url\":\"http://a.example/\"}", true).find("\"remote\"") != std::string::npos,
              "távoli (nem admin) kliens nem szerkesztheti a listát");
        CHECK(req("{\"type\":\"radio_add\",\"name\":\"X\",\"url\":\"ftp://a.example/\"}", false).find("\"url\"") != std::string::npos &&
              req("{\"type\":\"radio_add\",\"name\":\"  \",\"url\":\"http://a.example/\"}", false).find("\"name\"") != std::string::npos,
              "hibás cím / üres név elutasítva");
        const std::string added = req("{\"type\":\"radio_add\",\"name\":\"Próba Rádió\",\"url\":\"" + srv.url("/live") + "\"}", false);
        CHECK(added.find("\"type\":\"radio\"") != std::string::npos && added.find("Próba Rádió") != std::string::npos &&
              saved.find("Próba Rádió") != std::string::npos, "állomás felvétele és mentése");
        const auto all = store.all();
        CHECK(all.size() == 1, "egy állomás a listában");
        if (!all.empty()) {
            const std::string id = all[0].id;
            CHECK(req("{\"type\":\"radio_play\",\"id\":\"" + id + "\"}", true).find("\"ok\":true") != std::string::npos,
                  "lejátszás indítása a listából (távirányítóról is)");
            bool playing = false;
            for (int i = 0; i < 600 && !playing; ++i) {
                playing = cb.radioStatus().find("\"state\":\"playing\"") != std::string::npos;
                if (!playing) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            CHECK(playing && fp.state() == player::FilePlayer::State::Playing, "állapot: playing (%s)", cb.radioStatus().c_str());
            CHECK(cb.radioStatus().find("\"station\":\"" + id + "\"") != std::string::npos, "állapot: az állomás azonosítója");
            CHECK(req("{\"type\":\"radio_stop\"}", true).find("\"ok\":true") != std::string::npos &&
                  fp.state() == player::FilePlayer::State::Idle && cb.radioStatus().find("\"state\":\"idle\"") != std::string::npos,
                  "leállítás");
            CHECK(req("{\"type\":\"radio_play\",\"id\":\"nincs\"}", true).find("\"unknown\"") != std::string::npos, "ismeretlen állomás");
            CHECK(req("{\"type\":\"radio_update\",\"id\":\"" + id + "\",\"name\":\"Új név\"}", false).find("Új név") != std::string::npos, "átnevezés");
            StationStore reloaded(nullptr);
            reloaded.load(saved);
            Station s;
            CHECK(reloaded.get(id, s) && s.name == "Új név" && s.url == srv.url("/live"), "a mentett lista visszatölthető");
            CHECK(req("{\"type\":\"radio_remove\",\"id\":\"" + id + "\"}", false).find("\"items\":[]") != std::string::npos, "törlés");
        }
        // Állomáskereső: alapból ki; csak helyben / adminnal kapcsolható; a keresés aszinkron, a válasz broadcast
        CHECK(!store.catalog() && cb.radioStatus().find("\"catalog\":false") != std::string::npos, "a kereső alapból ki van kapcsolva");
        CHECK(req("{\"type\":\"radio_search\",\"q\":\"duna\"}", true).find("\"catalogOff\"") != std::string::npos &&
              srv.hits("/json/stations/search?name=duna&limit=50&hidebroken=true&order=clickcount&reverse=true") == 0,
              "kikapcsolt kereső: nincs kérés a katalógushoz");
        CHECK(req("{\"type\":\"radio_catalog\",\"on\":true}", true).find("\"remote\"") != std::string::npos && !store.catalog(),
              "távoli (nem admin) kliens nem kapcsolhatja be");
        CHECK(req("{\"type\":\"radio_catalog\",\"on\":true}", false).find("\"catalog\":true") != std::string::npos && store.catalog() &&
              saved.find("\"catalog\":true") != std::string::npos, "bekapcsolás és mentés");
        const std::string started = req("{\"type\":\"radio_search\",\"q\":\"Kék Duna\",\"req\":\"k1\"}", true);
        CHECK(started.find("\"state\":\"searching\"") != std::string::npos && started.find("\"req\":\"k1\"") != std::string::npos,
              "keresés indul (azonnali válasz)");
        std::string result;
        for (int i = 0; i < 500 && result.empty(); ++i) {
            {
                std::lock_guard<std::mutex> lk(bm);
                for (const auto& b : broadcasts) if (b.find("\"type\":\"radio_search\"") != std::string::npos) result = b;
            }
            if (result.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::string sp;
        {
            std::lock_guard<std::mutex> lk(searchM);
            sp = lastSearch;
        }
        CHECK(sp.find("name=K%C3%A9k%20Duna&") != std::string::npos, "a keresőszöveg URL-kódolva megy (%s)", sp.c_str());
        json::Value rv;
        const bool parsed = json::parse(result, rv);
        const json::Value* items = parsed ? rv.get("items") : nullptr;
        CHECK(items && items->type == json::Value::Type::Array && items->arr.size() == 2, "találatok: a hibás, a HLS-es és a név nélküli kimarad (%s)", result.c_str());
        if (items && items->arr.size() == 2) {
            const json::Value& a = items->arr[0];
            CHECK(a.get("url")->stringOr("") == "http://x.example/a.mp3" && a.get("codec")->stringOr("") == "MP3" &&
                  a.get("bitrate")->numberOr(0) == 128 && a.get("country")->stringOr("") == "HU" && a.get("name")->stringOr("") == "Kék Duna Rádió",
                  "találat: a feloldott cím, formátum, bitráta, ország");
            CHECK(items->arr[1].get("url")->stringOr("") == "https://y.example/s" && items->arr[1].get("name")->stringOr("") == "Második  adó",
                  "url_resolved nélkül az url; a név tisztítva");
        }
        CHECK(req("{\"type\":\"radio_catalog\",\"on\":false}", false).find("\"catalog\":false") != std::string::npos && !store.catalog(),
              "kikapcsolás");
        {
            std::vector<CatalogItem> out;
            std::string e;
            CHECK(!catalogSearch("   ", testNet(), out, e, nullptr, srv.url("")) && e == "query", "üres keresőszöveg");
            NetOptions strict = testNet();
            strict.allowLoopback = false;
            CHECK(!catalogSearch("duna", strict, out, e, nullptr, srv.url("")) && e == "blocked", "a katalóguskérés is SSRF-ellenőrzött");
        }

        StationStore junk(nullptr);
        junk.load("{\"stations\":[{\"id\":\"a1\",\"name\":\"Jó\",\"url\":\"http://a.example/\"},{\"id\":\"b2\",\"name\":\"Rossz\",\"url\":\"javascript:alert(1)\"},"
                  "{\"id\":\"../x\",\"name\":\"Rossz id\",\"url\":\"http://a.example/\"},{\"id\":\"a1\",\"name\":\"Dupla\",\"url\":\"http://b.example/\"}]}");
        CHECK(junk.count() == 1, "hibás mentett tételek kimaradnak (%zu)", junk.count());
        junk.load("nem json");
        CHECK(junk.count() == 0, "sérült dokumentum: üres lista");
    }
}

/**
 * Élő próba valódi állomással (csak kézzel, a CI-ben nem fut): AQ_RADIO_LIVE="cím1 cím2 …", AQ_RADIO_LIVE_SEC (alapból 25).
 * Állomásonként kiírja az állapotot, a formátumot, a „most szól” szöveget és az újracsatlakozásokat.
 */
static int liveProbe(const char* urls) {
    const char* secEnv = std::getenv("AQ_RADIO_LIVE_SEC");
    const int sec = secEnv ? std::max(5, std::atoi(secEnv)) : 25;
    std::string all = urls;
    size_t pos = 0;
    while (pos < all.size()) {
        size_t sp = all.find(' ', pos);
        if (sp == std::string::npos) sp = all.size();
        const std::string url = all.substr(pos, sp - pos);
        pos = sp + 1;
        if (url.empty()) continue;
        std::printf("\n== %s\n", url.c_str());
        RadioDecoder::Config c;
        c.url = url;
        c.station = "Élő próba";
        std::string err;
        const auto t0 = Clock::now();
        auto d = RadioDecoder::open(c, err);
        if (!d) { std::printf("  [HIBA] nem nyílt meg: %s (%.0f ms)\n", err.c_str(), msSince(t0)); ++g_fail; continue; }
        std::printf("  megnyílt %.0f ms alatt: %u Hz, %u csatorna\n", msSince(t0), d->sampleRate(), d->channels());
        size_t loud = 0, total = 0;
        std::vector<float> buf(1024 * d->channels());
        const auto t1 = Clock::now();
        double nextPrint = 0;
        uint64_t in20 = 0;
        double t20 = 0;
        // valós idejű olvasás (mint a lejátszó): 1024 keret, utána annyi várakozás, amennyi ideig szólna
        while (msSince(t1) < sec * 1000.0) {
            const size_t n = d->read(buf.data(), 1024);
            if (n == 0) { std::printf("  a folyam véget ért\n"); break; }
            total += n;
            for (size_t i = 0; i < n; ++i) if (std::fabs(buf[i * d->channels()]) > 1e-4f) ++loud;
            if (msSince(t1) >= nextPrint) {
                const RadioStatus st = d->status();
                std::string ti, ar, al;
                d->liveInfo(ti, ar, al);
                std::printf("  %5.1f s  %-12s %s %d kbit/s  újracsatl.: %llu  eldobva: %llu  szól: %s – %s\n", msSince(t1) / 1000.0,
                            stateName(st.state), st.codec.c_str(), st.bitrate, static_cast<unsigned long long>(st.reconnects),
                            static_cast<unsigned long long>(st.droppedFrames), ar.c_str(), ti.c_str());
                if (!in20 && msSince(t1) >= 20000) { in20 = st.framesIn; t20 = msSince(t1); }
                if (in20 && msSince(t1) > t20 + 1000)
                    std::printf("           a szerver üteme 20 s óta: %.1f keret/s (névleges %u)\n",
                                (st.framesIn - in20) * 1000.0 / (msSince(t1) - t20), d->sampleRate());
                nextPrint += 5000;
            }
            // abszolút időzítés (mint egy DAC): annyi keretet olvasunk, amennyi az eltelt idő alatt szólna
            while (static_cast<double>(total) > msSince(t1) / 1000.0 * d->sampleRate() && msSince(t1) < sec * 1000.0)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const RadioStatus st = d->status();
        std::printf("  összesen %zu keret, ebből hangos %.0f%%, újracsatlakozás %llu, utolsó hiba: %s\n", total,
                    total ? 100.0 * loud / total : 0.0, static_cast<unsigned long long>(st.reconnects), st.error.c_str());
        if (total == 0 || loud * 2 < total) ++g_fail;
    }
    return g_fail ? 1 : 0;
}

int main() {
    net::startup();
    if (const char* q = std::getenv("AQ_RADIO_SEARCH")) {   // kézi próba a valódi katalógussal (a CI-ben nem fut)
        std::vector<CatalogItem> out;
        std::string e;
        const auto t0 = Clock::now();
        const bool ok = catalogSearch(q, NetOptions{}, out, e);
        std::printf("katalógus: %s, %zu találat, %.0f ms %s\n", ok ? "ok" : "HIBA", out.size(), msSince(t0), e.c_str());
        for (size_t i = 0; i < out.size() && i < 8; ++i)
            std::printf("  %s | %s %d | %s | %s\n", out[i].name.c_str(), out[i].codec.c_str(), out[i].bitrate, out[i].country.c_str(), out[i].url.c_str());
        net::cleanup();
        return ok ? 0 : 1;
    }
    if (const char* live = std::getenv("AQ_RADIO_LIVE")) {
        const int rc = liveProbe(live);
        net::cleanup();
        return rc;
    }
    std::printf("Internetrádió teszt\n");
    testPure();
    testResolve();
    testNetwork();
    std::printf("\n%s (%d hiba)\n", g_fail ? "HIBA" : "MINDEN RENDBEN", g_fail);
    net::cleanup();
    return g_fail ? 1 : 0;
}
