/**
 * @file radio_stream.cpp
 * @brief Internetrádió: szállítás (curl / WinHTTP), SSRF-ellenőrzés, ICY, dekóderek folyamból, PCM-puffer,
 *        újracsatlakozás. A leírás: radio_stream.hpp.
 */
#include "radio_stream.hpp"
#include "mini_json.hpp"

#ifdef AQULIFY_VERSION
#define AQULIFY_RADIO_VERSION AQULIFY_VERSION
#else
#define AQULIFY_RADIO_VERSION "2"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include "net_compat.hpp"   // winsock2 a windows.h előtt
#include <windows.h>
#include <winhttp.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include "dr_flac.h"
#include "dr_mp3.h"
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#include "opusfile.h"
#include "neaacdec.h"

namespace ee_analyzer::radio {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxHeadBytes = 16 * 1024;
constexpr size_t kMaxPlaylistBytes = 64 * 1024;
constexpr size_t kRewindKeep = 256 * 1024;   // a kapcsolat első bájtjai visszatekerhetők (a dekóderek felismerése)
constexpr int kMaxHops = 5;

bool isIpLiteral(const RadioUrl& u) {
    if (u.ipv6) return true;
    for (char c : u.host)
        if ((c < '0' || c > '9') && c != '.') return false;
    return true;
}

/**
 * Egy socket-cím megengedett-e: a saját címszabály (policy, ha meg van adva), különben nyilvános, vagy a teszteknél
 * a loopback.
 */
bool sockaddrAllowed(const sockaddr* sa, bool allowLoopback, std::string* text = nullptr,
                     const std::function<bool(int, const uint8_t*)>* policy = nullptr) {
    char buf[INET6_ADDRSTRLEN] = { 0 };
    bool pub = false, loop = false;
    if (sa->sa_family == AF_INET) {
        const auto* s = reinterpret_cast<const sockaddr_in*>(sa);
        uint8_t b[4];
        std::memcpy(b, &s->sin_addr, 4);
        pub = policy && *policy ? (*policy)(4, b) : isPublicV4(b);
        loop = isLoopbackV4(b);
        inet_ntop(AF_INET, const_cast<in_addr*>(&s->sin_addr), buf, sizeof(buf));
    } else if (sa->sa_family == AF_INET6) {
        const auto* s = reinterpret_cast<const sockaddr_in6*>(sa);
        uint8_t b[16];
        std::memcpy(b, &s->sin6_addr, 16);
        pub = policy && *policy ? (*policy)(6, b) : isPublicV6(b);
        loop = isLoopbackV6(b);
        inet_ntop(AF_INET6, const_cast<in6_addr*>(&s->sin6_addr), buf, sizeof(buf));
    } else {
        return false;
    }
    if (text) *text = buf;
    return pub || (allowLoopback && loop);
}

} // namespace

bool resolvePublic(const RadioUrl& u, bool allowLoopback, std::string& ipOut, std::string& err) {
    NetOptions o;
    o.allowLoopback = allowLoopback;
    return resolveAllowed(u, o, ipOut, err);
}

bool resolveAllowed(const RadioUrl& u, const NetOptions& o, std::string& ipOut, std::string& err) {
    const bool allowLoopback = o.allowLoopback;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string port = std::to_string(u.effectivePort());
    if (getaddrinfo(u.host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        err = "dns";
        return false;
    }
    bool any = false, ok = true;
    std::string first;
    for (addrinfo* p = res; p; p = p->ai_next) {
        if (!p->ai_addr || (p->ai_family != AF_INET && p->ai_family != AF_INET6)) continue;
        std::string text;
        any = true;
        if (!sockaddrAllowed(p->ai_addr, allowLoopback, &text, &o.allowAddr)) ok = false;
        if (first.empty()) first = text;
    }
    freeaddrinfo(res);
    if (!any) { err = "dns"; return false; }
    if (!ok) { err = "blocked"; return false; }   // akár egyetlen tiltott cím is elég: a névfeloldás nem megbízható
    ipOut = first;
    return true;
}

const char* stateName(RadioState s) {
    switch (s) {
    case RadioState::Connecting: return "connecting";
    case RadioState::Buffering: return "buffering";
    case RadioState::Playing: return "playing";
    case RadioState::Reconnecting: return "reconnecting";
    case RadioState::Ended: return "ended";
    default: return "error";
    }
}

// =====================================================================================================================
// Szállítás
// =====================================================================================================================

#ifdef _WIN32

namespace {
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::string winhttpError(DWORD e) {
    switch (e) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "dns";
    case ERROR_WINHTTP_TIMEOUT: return "timeout";
    case ERROR_WINHTTP_OPERATION_CANCELLED: return "aborted";
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED: return "tls";
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE: return "http";
    default: return "connect";
    }
}
// A WinHTTP a nevet helyett ezt a címet oldja fel (Windows 10 újabb kiadásai); ha a rendszer nem ismeri, a beállítás
// csendben elmarad, és a válasz utáni címellenőrzés véd (lásd lent).
constexpr DWORD kOptResolutionHostname = 165;
} // namespace

struct HttpStream::Impl {
    std::mutex m;
    HINTERNET ses = nullptr, con = nullptr, req = nullptr;
    bool reqClosed = false;
    bool aborted = false;
    ~Impl() {
        if (req && !reqClosed) WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        if (ses) WinHttpCloseHandle(ses);
    }
};

HttpStream::HttpStream() : impl_(std::make_unique<Impl>()) {}
HttpStream::~HttpStream() = default;

void HttpStream::abort() {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->aborted = true;
    if (impl_->req && !impl_->reqClosed) {
        WinHttpCloseHandle(impl_->req);   // a folyamatban lévő szinkron kérést is megszakítja
        impl_->reqClosed = true;
    }
}

bool HttpStream::open(const RadioUrl& u, const NetOptions& o, HttpHead& head, std::string& err) {
    std::string ip;
    if (!resolveAllowed(u, o, ip, err)) return false;
    Impl& I = *impl_;
    HINTERNET req = nullptr;
    {
        std::lock_guard<std::mutex> lk(I.m);
        if (I.aborted) { err = "aborted"; return false; }
        I.ses = WinHttpOpen(wide(o.userAgent).c_str(), WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!I.ses) { err = "connect"; return false; }
        WinHttpSetTimeouts(I.ses, o.connectTimeoutS * 1000, o.connectTimeoutS * 1000, o.headTimeoutS * 1000,
                           o.stallTimeoutS * 1000);
        I.con = WinHttpConnect(I.ses, wide(u.host).c_str(), u.effectivePort(), 0);
        if (!I.con) { err = winhttpError(GetLastError()); return false; }
        I.req = WinHttpOpenRequest(I.con, L"GET", wide(u.target).c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, u.https ? WINHTTP_FLAG_SECURE : 0);
        if (!I.req) { err = winhttpError(GetLastError()); return false; }
        req = I.req;
    }
    DWORD pol = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &pol, sizeof(pol));
    DWORD feat = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &feat, sizeof(feat));
    if (!isIpLiteral(u)) {
        std::wstring wip = wide(ip);
        WinHttpSetOption(req, kOptResolutionHostname, wip.data(), static_cast<DWORD>(wip.size()));
    }
    std::string hdrText = o.icyMetaData ? "Icy-MetaData: 1\r\n" : "";
    for (const auto& h : o.extraHeaders) hdrText += h + "\r\n";
    const std::wstring hdrs = wide(hdrText);
    if (!WinHttpSendRequest(req, hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(),
                            hdrs.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        const DWORD e = GetLastError();
        std::lock_guard<std::mutex> lk(I.m);
        err = I.aborted ? "aborted" : winhttpError(e);
        return false;
    }
#ifdef WINHTTP_OPTION_CONNECTION_INFO
    {
        // A ténylegesen használt cím: ha a névfeloldás közben megváltozott és tiltott címre mutat, a törzs nem olvasódik
        WINHTTP_CONNECTION_INFO ci{};
        ci.cbSize = sizeof(ci);
        DWORD len = sizeof(ci);
        if (!WinHttpQueryOption(req, WINHTTP_OPTION_CONNECTION_INFO, &ci, &len) ||
            !sockaddrAllowed(reinterpret_cast<const sockaddr*>(&ci.RemoteAddress), o.allowLoopback, nullptr, &o.allowAddr)) {
            err = "blocked";
            return false;
        }
    }
#endif
    DWORD sz = 0;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &sz,
                        WINHTTP_NO_HEADER_INDEX);
    if (sz == 0 || sz > kMaxHeadBytes * sizeof(wchar_t)) { err = "http"; return false; }
    std::wstring raw(sz / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &sz,
                             WINHTTP_NO_HEADER_INDEX)) {
        err = "http";
        return false;
    }
    raw.resize(sz / sizeof(wchar_t));
    if (!parseHttpHead(narrow(raw), head)) { err = "http"; return false; }
    return true;
}

size_t HttpStream::read(uint8_t* buf, size_t n) {
    HINTERNET req;
    {
        std::lock_guard<std::mutex> lk(impl_->m);
        if (impl_->aborted || impl_->reqClosed || !impl_->req) return 0;
        req = impl_->req;
    }
    DWORD got = 0;
    if (!WinHttpReadData(req, buf, static_cast<DWORD>(std::min<size_t>(n, 1u << 20)), &got)) return 0;
    return got;
}

#else // ------------------------------------------------------------------ Linux / Pi / macOS: rendszer curl

namespace {
std::string curlError(int code) {
    switch (code) {
    case 6: return "dns";
    case 7: return "connect";
    case 28: return "timeout";
    case 35: case 51: case 53: case 54: case 58: case 59: case 60: case 77: case 80: case 83: case 90: case 91: return "tls";
    case 1: case 3: return "url";
    default: return "connect";
    }
}
} // namespace

struct HttpStream::Impl {
    std::mutex m;
    pid_t pid = -1;
    bool aborted = false;
    int fd = -1;
    int stallMs = 20000;
    std::vector<uint8_t> pending;
    size_t pendOff = 0;

    /** A curl kilépési kódja (a folyamat begyűjtése; utána a pid nem használható). */
    int reap(bool force) {
        pid_t p;
        {
            std::lock_guard<std::mutex> lk(m);
            p = pid;
        }
        if (p <= 0) return -1;
        int st = 0;
        bool done = false;
        if (force) {
            ::kill(p, SIGTERM);
            for (int i = 0; i < 100 && !done; ++i) {
                const pid_t r = ::waitpid(p, &st, WNOHANG);
                if (r == p || r < 0) done = true;
                else ::usleep(10000);
            }
            if (!done) {
                ::kill(p, SIGKILL);
                ::waitpid(p, &st, 0);
            }
        } else {
            while (::waitpid(p, &st, 0) < 0 && errno == EINTR) {}
        }
        std::lock_guard<std::mutex> lk(m);
        pid = -1;   // a begyűjtött pid-et soha többé nem jelezzük (újrahasznosulhat)
        return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    }
    ~Impl() {
        if (fd >= 0) ::close(fd);
        reap(true);
    }
};

HttpStream::HttpStream() : impl_(std::make_unique<Impl>()) {}
HttpStream::~HttpStream() = default;

void HttpStream::abort() {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->aborted = true;
    if (impl_->pid > 0) ::kill(impl_->pid, SIGTERM);   // a csővezeték bezárul, a read azonnal visszatér
}

bool HttpStream::open(const RadioUrl& u, const NetOptions& o, HttpHead& head, std::string& err) {
    Impl& I = *impl_;
    I.stallMs = std::max(1, o.stallTimeoutS) * 1000;
    std::string ip;
    if (!resolveAllowed(u, o, ip, err)) return false;
    // Argumentumtömb, héj nélkül; -q: a felhasználó .curlrc-je nem számít; proxy nincs (különben a proxy oldaná fel a
    // nevet, és a címellenőrzés hatástalan lenne); átirányítást nem követ (a hívó ellenőrzi és követi). --http0.9: a
    // régi „ICY 200 OK” válaszsort a curl HTTP/0.9-nek veszi; így a sor a kimenetbe kerül, és a fejlécet mi bontjuk
    std::vector<std::string> args = { "curl", "-q", "-sS", "-i", "-N", "--http0.9", "--proto", "=http,https", "--noproxy", "*",
                                      "--max-redirs", "0", "--connect-timeout", std::to_string(o.connectTimeoutS),
                                      "-y", std::to_string(std::max(1, o.stallTimeoutS)), "-Y", "1",
                                      "-A", o.userAgent };
    if (o.icyMetaData) { args.push_back("-H"); args.push_back("Icy-MetaData: 1"); }
    for (const auto& h : o.extraHeaders) { args.push_back("-H"); args.push_back(h); }
    if (!isIpLiteral(u)) {
        // a már ellenőrzött címre kapcsolódik: a név nem oldható fel újra (DNS-újrakötés ellen)
        const std::string ipPart = ip.find(':') != std::string::npos ? "[" + ip + "]" : ip;
        args.push_back("--resolve");
        args.push_back(u.host + ":" + std::to_string(u.effectivePort()) + ":" + ipPart);
    }
    args.push_back(u.str());
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    int fds[2];
    if (::pipe(fds) != 0) { err = "connect"; return false; }
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);   // a gyermekben a dup2 után az 1-es leíró megmarad, a többi bezárul
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    int rc;
    {
        std::lock_guard<std::mutex> lk(I.m);
        if (I.aborted) rc = -1;
        else {
            pid_t p = -1;
            rc = posix_spawnp(&p, "curl", &fa, nullptr, argv.data(), environ);
            if (rc == 0) I.pid = p;
        }
    }
    posix_spawn_file_actions_destroy(&fa);
    ::close(fds[1]);
    if (rc != 0) {
        ::close(fds[0]);
        err = rc == -1 ? "aborted" : "nocurl";
        return false;
    }
    I.fd = fds[0];

    // A fejléc (curl -i: a válasz fejléce, üres sor, utána a törzs)
    std::string hdr;
    const auto deadline = Clock::now() + std::chrono::seconds(std::max(1, o.headTimeoutS));
    size_t end = std::string::npos, skip = 0;
    char buf[4096];
    for (;;) {
        if ((end = hdr.find("\r\n\r\n")) != std::string::npos) { skip = 4; break; }
        if ((end = hdr.find("\n\n")) != std::string::npos) { skip = 2; break; }
        if (hdr.size() > kMaxHeadBytes) { err = "http"; return false; }
        {
            std::lock_guard<std::mutex> lk(I.m);
            if (I.aborted) { err = "aborted"; return false; }
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) { err = "timeout"; return false; }
        pollfd pf{ I.fd, POLLIN, 0 };
        const int pr = ::poll(&pf, 1, static_cast<int>(std::min<long long>(left, 200)));
        if (pr < 0 && errno != EINTR) { err = "connect"; return false; }
        if (pr <= 0) continue;
        const ssize_t n = ::read(I.fd, buf, sizeof(buf));
        if (n <= 0) {
            // a curl kilépett válasz nélkül: a kilépési kódja mondja meg, miért
            ::close(I.fd);
            I.fd = -1;
            const int code = I.reap(false);
            bool ab;
            {
                std::lock_guard<std::mutex> lk(I.m);
                ab = I.aborted;
            }
            err = ab ? "aborted" : curlError(code);
            return false;
        }
        hdr.append(buf, static_cast<size_t>(n));
    }
    if (!parseHttpHead(hdr.substr(0, end), head)) { err = "http"; return false; }
    I.pending.assign(hdr.begin() + static_cast<std::ptrdiff_t>(end + skip), hdr.end());
    I.pendOff = 0;
    return true;
}

size_t HttpStream::read(uint8_t* buf, size_t n) {
    Impl& I = *impl_;
    if (I.pendOff < I.pending.size()) {
        const size_t k = std::min(n, I.pending.size() - I.pendOff);
        std::memcpy(buf, I.pending.data() + I.pendOff, k);
        I.pendOff += k;
        return k;
    }
    if (I.fd < 0) return 0;
    int waited = 0;
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(I.m);
            if (I.aborted) return 0;
        }
        pollfd pf{ I.fd, POLLIN, 0 };
        const int pr = ::poll(&pf, 1, 200);
        if (pr > 0) {
            const ssize_t r = ::read(I.fd, buf, n);
            return r > 0 ? static_cast<size_t>(r) : 0;
        }
        if (pr == 0) {
            waited += 200;
            if (waited >= I.stallMs) return 0;   // adat nélkül túl sokáig: a kapcsolat halottnak számít
        } else if (errno != EINTR) {
            return 0;
        }
    }
}

#endif

// =====================================================================================================================
// A megosztott állapot és a dekóderek
// =====================================================================================================================

struct RadioDecoder::Core {
    Config cfg;
    mutable std::mutex m;
    std::condition_variable cv;
    bool quit = false, done = false, closed = false;
    HttpStream* conn = nullptr;            // az éppen nyitott kapcsolat (a megszakításhoz), m alatt
    bool formatKnown = false;
    uint32_t rate = 0, ch = 0;
    std::vector<float> fifo;
    size_t capFrames = 0, preFrames = 0;
    uint64_t wr = 0, rd = 0;
    bool buffering = true;
    bool reconnecting = false;
    bool terminal = false;                 // végleges vége (hiba vagy leállítás): a puffer kifut, utána read() = 0
    bool failed = false;
    std::string error, codec, icyName;
    int bitrate = 0;
    uint64_t reconnects = 0, dropped = 0;
    std::deque<std::pair<uint64_t, std::string>> titles;   // (a PCM írási helye, cím): a hallott résznél lép életbe
    std::string title;

    void addTitle(const std::string& t) {
        std::lock_guard<std::mutex> lk(m);
        if (!titles.empty() && titles.back().second == t) return;
        if (titles.empty() && t == title) return;
        titles.emplace_back(wr, t);
        while (titles.size() > 16) titles.pop_front();
    }
    void push(const float* p, size_t n) {
        std::lock_guard<std::mutex> lk(m);
        if (!capFrames || !ch) return;
        if (n > capFrames) { p += (n - capFrames) * ch; n = capFrames; }
        const size_t used = static_cast<size_t>(wr - rd);
        if (used + n > capFrames) {   // túlcsordulás (szünet, órajel-eltérés): a legrégebbi rész kiesik, élő marad
            const size_t drop = used + n - capFrames;
            rd += drop;
            dropped += drop;
        }
        for (size_t i = 0; i < n; ++i) {
            const size_t idx = static_cast<size_t>((wr + i) % capFrames) * ch;
            std::memcpy(&fifo[idx], p + i * ch, ch * sizeof(float));
        }
        wr += n;
        if (buffering && wr - rd >= preFrames) buffering = false;
        cv.notify_all();
    }
};

namespace {

using Core = RadioDecoder::Core;

/** A kapcsolat bájtjai az ICY-metaadat nélkül; az első kRewindKeep bájt visszatekerhető. Csak a dolgozó szál használja. */
class Reader {
public:
    Reader(Core& c, HttpStream& h, size_t metaint) : c_(c), h_(h), icy_(metaint) {}
    size_t read(uint8_t* dst, size_t n) {
        size_t got = 0;
        while (got < n) {
            // Ogg-láncok (szétvágás mellett): a dekóder csak a saját láncának lapjait kapja; az új lánc kezdetén „vége”
            uint64_t end = base_ + buf_.size();
            if (track_) end = std::min(end, nextPage_);   // a még nem elemzett lapfejléc után nem adunk ki bájtot
            if (split_) {
                while (!bos_.empty() && bos_.front() <= chainStart_) bos_.pop_front();
                if (!bos_.empty()) {
                    if (pos_ >= bos_.front()) { chainBreak_ = true; break; }
                    end = std::min(end, bos_.front());
                }
            }
            if (pos_ < end) {
                const size_t off = static_cast<size_t>(pos_ - base_);
                const size_t k = static_cast<size_t>(std::min<uint64_t>(n - got, end - pos_));
                std::memcpy(dst + got, buf_.data() + off, k);
                pos_ += k;
                got += k;
                continue;
            }
            if (!fill()) break;
        }
        // a cím attól a hangbájttól érvényes, ahol a metaadat-blokk állt: a dekóder most ért oda (így a PCM-puffer
        // írási helye kb. a hallható váltás helye)
        while (!titles_.empty() && titles_.front().first <= pos_) {
            c_.addTitle(titles_.front().second);
            titles_.pop_front();
        }
        trim();
        return got;
    }
    size_t peek(uint8_t* dst, size_t n) {
        const uint64_t save = pos_;
        const size_t k = read(dst, n);
        pos_ = save;
        return k;
    }
    bool seekTo(uint64_t p) {
        if (p < base_) return false;
        while (p > base_ + buf_.size())
            if (!fill()) return false;
        pos_ = p;
        return true;
    }
    bool skip(long long off) {
        if (off < 0 && static_cast<uint64_t>(-off) > pos_) return false;
        return seekTo(static_cast<uint64_t>(static_cast<long long>(pos_) + off));
    }
    uint64_t tell() const { return pos_; }
    bool atEnd() const { return eof_ && pos_ >= base_ + buf_.size(); }

    /**
     * Ogg-lapok követése a folyam elejétől: az új logikai láncok (BOS-lap a lánc adatlapjai után) helye. split: a dekóder
     * a lánc végén „fájlvéget” lát (chainBreak), és a hívó új dekódert indít a következő láncra (nextChain). A beépített
     * FLAC-dekóder ugyanis a más sorozatszámú lapokat csendben átugorja (a rádió a szám végén elnémulna), a Vorbis pedig
     * nem kezel láncot; az Opus (opusfile) maga kezeli, ott nem kell szétvágni.
     */
    void trackOgg(bool split) {
        track_ = true;
        split_ = split;
        nextPage_ = pos_;      // pos_ mindig lap elején áll (a folyam, illetve egy lánc kezdete)
        chainStart_ = pos_;
        inBosGroup_ = true;
        scanPages();
    }
    bool chainBreak() const { return chainBreak_; }
    void nextChain() {
        chainStart_ = pos_;
        chainBreak_ = false;
    }
    /** Ugrás a mostani lánc (illetve a kapcsolat) elejéhez képest (a dekóderek SEEK_SET-je). */
    bool seekRel(uint64_t off) { return seekTo(chainStart_ + off); }

private:
    void scanPages() {
        while (track_) {
            const uint64_t end = base_ + buf_.size();
            if (nextPage_ < base_ || nextPage_ + 27 > end) break;
            const size_t i = static_cast<size_t>(nextPage_ - base_);
            if (std::memcmp(&buf_[i], "OggS", 4) != 0) { track_ = split_ = false; break; }   // szinkronvesztés: nem követjük
            const size_t ns = buf_[i + 26];
            if (nextPage_ + 27 + ns > end) break;
            size_t body = 0;
            for (size_t k = 0; k < ns; ++k) body += buf_[i + 27 + k];
            const bool bos = (buf_[i + 5] & 0x02) != 0;
            // a lánc eleji BOS-csoport (több logikai folyam is kezdődhet egyszerre) egy lánc; új lánc: adatlap utáni BOS
            if (bos && !inBosGroup_ && bos_.size() < 64) bos_.push_back(nextPage_);
            inBosGroup_ = bos;
            nextPage_ += 27 + ns + body;
        }
    }
    bool fill() {
        if (eof_) return false;
        for (;;) {
            uint8_t tmp[16384];
            const size_t n = h_.read(tmp, sizeof(tmp));
            if (n == 0) { eof_ = true; return false; }
            audio_.clear();
            metas_.clear();
            metaAt_.clear();
            icy_.feed(tmp, n, audio_, metas_, &metaAt_);
            const uint64_t end = base_ + buf_.size();
            for (size_t i = 0; i < metas_.size(); ++i) {
                const std::string t = icyStreamTitle(metas_[i]);
                if (!t.empty() && titles_.size() < 64) titles_.emplace_back(end + metaAt_[i], t);
            }
            if (audio_.empty()) continue;
            buf_.insert(buf_.end(), audio_.begin(), audio_.end());
            scanPages();
            return true;
        }
    }
    void trim() {
        // a kezdő rész (felismerés, visszatekerés) után a már olvasott bájtok eldobhatók
        if (pos_ <= kRewindKeep) return;
        const uint64_t keepFrom = pos_ - std::min<uint64_t>(pos_, 64 * 1024);
        if (keepFrom > base_ + 256 * 1024) {
            const size_t drop = static_cast<size_t>(keepFrom - base_);
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(drop));
            base_ += drop;
        }
    }
    Core& c_;
    HttpStream& h_;
    IcyDemux icy_;
    std::vector<uint8_t> buf_, audio_;
    std::vector<std::string> metas_;
    std::vector<size_t> metaAt_;
    std::deque<std::pair<uint64_t, std::string>> titles_;   // (hangbájt helye, cím)
    bool track_ = false, split_ = false, chainBreak_ = false, inBosGroup_ = true;
    uint64_t nextPage_ = 0, chainStart_ = 0;
    std::deque<uint64_t> bos_;                              // az új láncok kezdete (bájthely)
    uint64_t base_ = 0, pos_ = 0;
    bool eof_ = false;
};

struct Codec {
    virtual ~Codec() = default;
    uint32_t rate = 0, ch = 0;
    const char* name = "";
    bool formatChanged = false;
    /** Legfeljebb frames keret; 0: a kapcsolat vége (vagy dekódolhatatlan). */
    virtual size_t decode(float* out, size_t frames) = 0;
};

/** Vorbis-megjegyzésekből a „most szól” szöveg (Ogg Vorbis / Opus folyamnál a szám váltásakor új lánc jön). */
std::string commentTitle(const player::TrackTags& t) {
    if (t.title.empty()) return {};
    return cleanText(t.artist.empty() ? t.title : t.artist + " - " + t.title);
}

class Mp3Codec final : public Codec {
public:
    ~Mp3Codec() override { if (ok_) drmp3_uninit(&mp3_); }
    bool init(Reader& r) {
        ok_ = drmp3_init(&mp3_, &Mp3Codec::onRead, &Mp3Codec::onSeek, nullptr, nullptr, &r, nullptr);
        if (!ok_ || mp3_.channels == 0 || mp3_.sampleRate == 0) return false;
        // élő folyam: egy esetleges Xing / Info fejléc (pl. a forrás egy fájllal indult) hossza nem érvényes, különben a
        // dekóder annyi keret után megállna
        mp3_.totalPCMFrameCount = DRMP3_UINT64_MAX;
        mp3_.paddingInPCMFrames = 0;
        rate = mp3_.sampleRate;
        ch = mp3_.channels;
        name = "mp3";
        return true;
    }
    size_t decode(float* out, size_t frames) override {
        return static_cast<size_t>(drmp3_read_pcm_frames_f32(&mp3_, frames, out));
    }

private:
    static size_t onRead(void* u, void* out, size_t n) { return static_cast<Reader*>(u)->read(static_cast<uint8_t*>(out), n); }
    static drmp3_bool32 onSeek(void* u, int off, drmp3_seek_origin o) {
        auto* r = static_cast<Reader*>(u);
        if (o == DRMP3_SEEK_SET) return off >= 0 && r->seekRel(static_cast<uint64_t>(off));
        if (o == DRMP3_SEEK_CUR) return r->skip(off);
        return DRMP3_FALSE;   // a folyam vége nem érhető el
    }
    drmp3 mp3_{};
    bool ok_ = false;
};

class FlacCodec final : public Codec {
public:
    explicit FlacCodec(Core& c) : c_(c) {}
    ~FlacCodec() override { if (f_) drflac_close(f_); }
    bool init(Reader& r) {
        r_ = &r;
        // natív és Ogg FLAC; a Vorbis-megjegyzés (Ogg-láncban számonként új) a „most szól”
        f_ = drflac_open_with_metadata(&FlacCodec::onRead, &FlacCodec::onSeek, nullptr, &FlacCodec::onMeta, this, nullptr);
        if (!f_ || f_->channels == 0 || f_->sampleRate == 0) return false;
        if (!title_.empty()) c_.addTitle(title_);
        rate = f_->sampleRate;
        ch = f_->channels;
        name = "flac";
        return true;
    }
    size_t decode(float* out, size_t frames) override {
        return static_cast<size_t>(drflac_read_pcm_frames_f32(f_, frames, out));
    }

private:
    static size_t onRead(void* u, void* out, size_t n) {
        return static_cast<FlacCodec*>(u)->r_->read(static_cast<uint8_t*>(out), n);
    }
    static drflac_bool32 onSeek(void* u, int off, drflac_seek_origin o) {
        Reader* r = static_cast<FlacCodec*>(u)->r_;
        if (o == DRFLAC_SEEK_SET) return off >= 0 && r->seekRel(static_cast<uint64_t>(off));
        if (o == DRFLAC_SEEK_CUR) return r->skip(off);
        return DRFLAC_FALSE;
    }
    static void onMeta(void* u, drflac_metadata* m) {
        auto* self = static_cast<FlacCodec*>(u);
        if (m->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;
        drflac_vorbis_comment_iterator it;
        drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount, m->data.vorbis_comment.pComments);
        drflac_uint32 len = 0;
        player::TrackTags tags;
        while (const char* cm = drflac_next_vorbis_comment(&it, &len)) player::detail::applyVorbisComment(cm, len, tags);
        self->title_ = commentTitle(tags);
    }
    Core& c_;
    Reader* r_ = nullptr;
    drflac* f_ = nullptr;
    std::string title_;
};

class OpusCodec final : public Codec {
public:
    explicit OpusCodec(Core& c) : c_(c) {}
    ~OpusCodec() override { if (of_) op_free(of_); }
    bool init(Reader& r) {
        OpusFileCallbacks cb{ &OpusCodec::onRead, nullptr, nullptr, nullptr };   // nem tekerhető folyam
        int e = 0;
        of_ = op_open_callbacks(&r, &cb, nullptr, 0, &e);
        if (!of_) return false;
        const int n = op_channel_count(of_, -1);
        if (n <= 0) return false;
        ch = static_cast<uint32_t>(n);
        rate = 48000;
        name = "opus";
        link_ = op_current_link(of_);
        emitTags(link_);
        return true;
    }
    size_t decode(float* out, size_t frames) override {
        size_t got = 0;
        int holes = 0;
        while (got < frames) {
            int li = -1;
            const int ret = op_read_float(of_, out + got * ch, static_cast<int>((frames - got) * ch), &li);
            if (ret == OP_HOLE && ++holes < 64) continue;
            if (ret <= 0) break;
            if (op_channel_count(of_, li) != static_cast<int>(ch)) { formatChanged = true; break; }
            if (li != link_) { link_ = li; emitTags(li); }
            got += static_cast<size_t>(ret);
        }
        return got;
    }

private:
    static int onRead(void* s, unsigned char* p, int n) {
        return n <= 0 ? 0 : static_cast<int>(static_cast<Reader*>(s)->read(p, static_cast<size_t>(n)));
    }
    void emitTags(int li) {
        const OpusTags* t = op_tags(of_, li);
        if (!t) return;
        player::TrackTags tags;
        for (int i = 0; i < t->comments; ++i)
            if (t->user_comments[i] && t->comment_lengths[i] > 0)
                player::detail::applyVorbisComment(t->user_comments[i], static_cast<size_t>(t->comment_lengths[i]), tags);
        const std::string s = commentTitle(tags);
        if (!s.empty()) c_.addTitle(s);
    }
    Core& c_;
    OggOpusFile* of_ = nullptr;
    int link_ = -1;
};

class VorbisCodec final : public Codec {
public:
    explicit VorbisCodec(Core& c) : c_(c) {}
    ~VorbisCodec() override { if (v_) stb_vorbis_close(v_); }
    bool init(Reader& r) {
        r_ = &r;
        if (!openHere()) return false;
        const stb_vorbis_info info = stb_vorbis_get_info(v_);
        rate = info.sample_rate;
        ch = static_cast<uint32_t>(info.channels);
        name = "vorbis";
        return rate > 0 && ch > 0;
    }
    size_t decode(float* out, size_t frames) override {
        size_t got = 0;
        int idle = 0;
        while (got < frames) {
            if (leftOff_ < left_.size()) {
                const size_t k = std::min((left_.size() - leftOff_) / ch, frames - got);
                std::memcpy(out + got * ch, left_.data() + leftOff_, k * ch * sizeof(float));
                leftOff_ += k * ch;
                got += k;
                continue;
            }
            if (!v_) break;
            int chans = 0, samples = 0;
            float** pcm = nullptr;
            const int used = buf_.empty() ? 0
                : stb_vorbis_decode_frame_pushdata(v_, buf_.data(), static_cast<int>(buf_.size()), &chans, &pcm, &samples);
            const int e = stb_vorbis_get_error(v_);
            if (used == 0 && samples == 0) {
                if (e != 0 && e != VORBIS_need_more_data) { if (!restartChain()) break; continue; }
                if (buf_.size() > 512 * 1024 || !more()) break;
                continue;
            }
            buf_.erase(buf_.begin(), buf_.begin() + used);
            if (samples > 0) {
                idle = 0;
                if (chans != static_cast<int>(ch)) { formatChanged = true; break; }
                left_.resize(static_cast<size_t>(samples) * ch);
                for (int i = 0; i < samples; ++i)
                    for (uint32_t c = 0; c < ch; ++c) left_[static_cast<size_t>(i) * ch + c] = pcm[c][i];
                leftOff_ = 0;
            } else if ((e != 0 && e != VORBIS_need_more_data) || ++idle > 256) {
                // új logikai folyam (a szerver a számváltáskor új láncot kezd) vagy tartós hiba: újranyitás a fejlécnél
                if (!restartChain()) break;
                idle = 0;
            }
        }
        return got;
    }

private:
    bool more() {
        uint8_t tmp[8192];
        const size_t n = r_->read(tmp, sizeof(tmp));
        if (n == 0) return false;
        buf_.insert(buf_.end(), tmp, tmp + n);
        return true;
    }
    bool openHere() {
        for (;;) {
            int used = 0, err = 0;
            if (!buf_.empty()) {
                v_ = stb_vorbis_open_pushdata(buf_.data(), static_cast<int>(buf_.size()), &used, &err, nullptr);
                if (v_) {
                    buf_.erase(buf_.begin(), buf_.begin() + used);
                    const stb_vorbis_comment cm = stb_vorbis_get_comment(v_);
                    player::TrackTags tags;
                    for (int i = 0; i < cm.comment_list_length; ++i)
                        if (cm.comment_list[i])
                            player::detail::applyVorbisComment(cm.comment_list[i], std::strlen(cm.comment_list[i]), tags);
                    const std::string s = commentTitle(tags);
                    if (!s.empty()) c_.addTitle(s);
                    return true;
                }
                if (err != VORBIS_need_more_data) return false;
            }
            if (buf_.size() > kRewindKeep || !more()) return false;
        }
    }
    /** A következő „kezdő” Ogg-lap (BOS jelző) keresése, és új dekóder onnan. */
    bool restartChain() {
        if (v_) { stb_vorbis_close(v_); v_ = nullptr; }
        left_.clear();
        leftOff_ = 0;
        for (int guard = 0; guard < 64; ++guard) {
            for (size_t i = 0; i + 6 <= buf_.size(); ++i) {
                if (std::memcmp(&buf_[i], "OggS", 4) == 0 && (buf_[i + 5] & 0x02)) {
                    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(i));
                    if (!openHere()) return false;
                    const stb_vorbis_info info = stb_vorbis_get_info(v_);
                    if (info.sample_rate != rate || static_cast<uint32_t>(info.channels) != ch) {
                        formatChanged = true;
                        return false;
                    }
                    return true;
                }
            }
            if (buf_.size() > 6) buf_.erase(buf_.begin(), buf_.end() - 5);
            if (!more()) return false;
        }
        return false;
    }
    Core& c_;
    Reader* r_ = nullptr;
    stb_vorbis* v_ = nullptr;
    std::vector<uint8_t> buf_;
    std::vector<float> left_;
    size_t leftOff_ = 0;
};

/**
 * AAC (ADTS-keretek, AAC-LC, HE-AAC / SBR, HE-AACv2 / PS) a FAAD2 könyvtárral (third_party/faad2, GPL-2.0-or-later).
 * A mintavételt és a csatornaszámot az első dekódolt keret adja (SBR-nél a dupla, PS-nél sztereó); 5.1-et a FAAD2
 * sztereóra keveri.
 */
class AacCodec final : public Codec {
public:
    ~AacCodec() override { if (h_) NeAACDecClose(h_); }
    bool init(Reader& r) {
        r_ = &r;
        h_ = NeAACDecOpen();
        if (!h_) return false;
        NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(h_);
        cfg->outputFormat = FAAD_FMT_FLOAT;
        cfg->downMatrix = 1;
        NeAACDecSetConfiguration(h_, cfg);
        if (!fillTo(kWant) || !syncAdts()) return false;
        unsigned long sr = 0;
        unsigned char chn = 0;
        const long used = NeAACDecInit(h_, buf_.data(), static_cast<unsigned long>(buf_.size()), &sr, &chn);
        if (used < 0) return false;
        buf_.erase(buf_.begin(), buf_.begin() + used);
        // az első hangot adó keret dönti el a formátumot
        for (int i = 0; i < 64 && left_.empty(); ++i)
            if (!decodeFrame(true)) return false;
        name = "aac";
        return rate > 0 && ch > 0 && !left_.empty();
    }
    size_t decode(float* out, size_t frames) override {
        size_t got = 0;
        int guard = 0;
        while (got < frames) {
            if (leftOff_ < left_.size()) {
                const size_t k = std::min((left_.size() - leftOff_) / ch, frames - got);
                std::memcpy(out + got * ch, left_.data() + leftOff_, k * ch * sizeof(float));
                leftOff_ += k * ch;
                got += k;
                continue;
            }
            if (formatChanged || ++guard > 256 || !decodeFrame(false)) break;
        }
        return got;
    }

private:
    static constexpr size_t kWant = 16 * 1024;   // legalább két teljes ADTS-keret (egy keret legfeljebb 8191 bájt)
    bool fillTo(size_t n) {
        uint8_t tmp[8192];
        while (buf_.size() < n) {
            const size_t k = r_->read(tmp, sizeof(tmp));
            if (k == 0) return !buf_.empty();
            buf_.insert(buf_.end(), tmp, tmp + k);
        }
        return true;
    }
    /** Az első ADTS-szinkronig eldobja a bájtokat (12 bit 0xFFF, layer 00). */
    bool syncAdts() {
        for (int guard = 0; guard < 64; ++guard) {
            for (size_t i = 0; i + 1 < buf_.size(); ++i) {
                if (buf_[i] == 0xFF && (buf_[i + 1] & 0xF6) == 0xF0) {
                    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(i));
                    return fillTo(kWant);
                }
            }
            if (!buf_.empty()) buf_.erase(buf_.begin(), buf_.end() - 1);
            if (!fillTo(kWant) || buf_.size() < 2) return false;
        }
        return false;
    }
    /** Egy keret dekódolása a left_-be; false: a kapcsolat vége vagy tartós hiba. */
    bool decodeFrame(bool first) {
        for (int errors = 0; errors < 64;) {
            fillTo(kWant);
            if (buf_.size() < 8) return false;
            NeAACDecFrameInfo fi{};
            void* pcm = NeAACDecDecode(h_, &fi, buf_.data(), static_cast<unsigned long>(buf_.size()));
            if (fi.error || !pcm) {
                // sérült keret: egy bájt eldobása és újraszinkronizálás a következő ADTS-fejlécre
                ++errors;
                buf_.erase(buf_.begin());
                if (!syncAdts()) return false;
                continue;
            }
            if (fi.bytesconsumed == 0) return false;
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(fi.bytesconsumed, buf_.size())));
            if (fi.samples == 0 || fi.channels == 0) continue;
            if (first && rate == 0) {
                rate = static_cast<uint32_t>(fi.samplerate);
                ch = fi.channels;
            } else if (fi.samplerate != rate || fi.channels != ch) {
                formatChanged = true;
                return false;
            }
            const float* s = static_cast<const float*>(pcm);
            left_.assign(s, s + fi.samples);
            leftOff_ = 0;
            return true;
        }
        return false;
    }
    Reader* r_ = nullptr;
    NeAACDecHandle h_ = nullptr;
    std::vector<uint8_t> buf_;
    std::vector<float> left_;
    size_t leftOff_ = 0;
};

std::unique_ptr<Codec> makeCodec(StreamFormat f, Reader& r, Core& c) {
    if (f == StreamFormat::Aac) {
        auto d = std::make_unique<AacCodec>();
        return d->init(r) ? std::move(d) : nullptr;
    }
    if (f == StreamFormat::Mp3) {
        auto d = std::make_unique<Mp3Codec>();
        return d->init(r) ? std::move(d) : nullptr;
    }
    if (f == StreamFormat::Flac) {
        auto d = std::make_unique<FlacCodec>(c);
        return d->init(r) ? std::move(d) : nullptr;
    }
    if (f == StreamFormat::Ogg) {
        // az első lap tartalma dönt: Opus, Vorbis vagy FLAC
        uint8_t pg[512];
        const size_t n = r.peek(pg, sizeof(pg));
        if (n < 28) return nullptr;
        const size_t payload = 27 + pg[26];
        auto at = [&](const char* sig, size_t len) { return payload + len <= n && std::memcmp(pg + payload, sig, len) == 0; };
        if (at("OpusHead", 8)) {   // az opusfile maga kezeli a láncokat
            auto d = std::make_unique<OpusCodec>(c);
            return d->init(r) ? std::move(d) : nullptr;
        }
        if (at("\x01vorbis", 7)) {
            r.trackOgg(true);
            auto d = std::make_unique<VorbisCodec>(c);
            return d->init(r) ? std::move(d) : nullptr;
        }
        if (at("\x7f" "FLAC", 5)) {
            r.trackOgg(true);
            auto d = std::make_unique<FlacCodec>(c);
            return d->init(r) ? std::move(d) : nullptr;
        }
    }
    return nullptr;
}

/** Az éppen nyitott kapcsolat bejegyzése (a megszakításhoz); a kapcsolat előtt szűnik meg. */
struct ConnReg {
    Core& c;
    ConnReg(Core& core, HttpStream* h) : c(core) { std::lock_guard<std::mutex> lk(c.m); c.conn = h; }
    ~ConnReg() { std::lock_guard<std::mutex> lk(c.m); c.conn = nullptr; }
};

/**
 * Egy kapcsolat: átirányítások és lejátszási lista követése (minden lépés újra ellenőrizve), a formátum felismerése,
 * dekódolás a kapcsolat végéig. fatal: nincs értelme újrapróbálni (tiltott cím, nem támogatott formátum, …).
 */
void session(Core& c, std::string& err, bool& fatal, uint64_t& framesOut) {
    RadioUrl u;
    if (!parseRadioUrl(c.cfg.url, u, nullptr)) { err = "url"; fatal = true; return; }
    for (int hop = 0;; ++hop) {
        if (hop > kMaxHops) { err = "redirect"; fatal = true; return; }
        {
            std::lock_guard<std::mutex> lk(c.m);
            if (c.quit) { err = "aborted"; return; }
        }
        HttpStream hs;
        ConnReg reg(c, &hs);
        HttpHead head;
        if (!hs.open(u, c.cfg.net, head, err)) {
            fatal = err == "blocked" || err == "url";
            return;
        }
        const int st = head.status;
        if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) {
            RadioUrl next;
            if (!resolveLocation(u, head.get("location"), next, nullptr)) { err = "redirect"; fatal = true; return; }
            u = next;
            continue;
        }
        if (st != 200) { err = "http"; fatal = st == 400 || st == 401 || st == 403 || st == 404 || st == 410; return; }
        size_t metaint = 0;
        const std::string mi = head.get("icy-metaint");
        if (!mi.empty()) {
            const long v = std::strtol(mi.c_str(), nullptr, 10);
            if (v > 0 && v <= (1L << 20)) metaint = static_cast<size_t>(v);
        }
        {
            std::lock_guard<std::mutex> lk(c.m);
            const std::string name = cleanText(head.get("icy-name"), 128);
            if (!name.empty()) c.icyName = name;
            const long br = std::strtol(head.get("icy-br").c_str(), nullptr, 10);
            c.bitrate = br > 0 && br < 100000 ? static_cast<int>(br) : 0;
        }
        Reader rd(c, hs, metaint);
        uint8_t pk[512];
        const size_t pn = rd.peek(pk, sizeof(pk));
        if (pn == 0) { err = "empty"; return; }
        const StreamFormat f = sniffFormat(head.get("content-type"), pk, pn);
        if (f == StreamFormat::Playlist) {
            std::string body;
            uint8_t tmp[4096];
            while (body.size() <= kMaxPlaylistBytes) {
                const size_t n = rd.read(tmp, sizeof(tmp));
                if (n == 0) break;
                body.append(reinterpret_cast<const char*>(tmp), n);
            }
            if (body.size() > kMaxPlaylistBytes) { err = "playlist"; fatal = true; return; }
            const auto urls = playlistUrls(body);
            RadioUrl next;
            if (urls.empty() || !resolveLocation(u, urls[0], next, nullptr)) { err = "playlist"; fatal = true; return; }
            u = next;
            continue;
        }
        if (f != StreamFormat::Mp3 && f != StreamFormat::Ogg && f != StreamFormat::Flac && f != StreamFormat::Aac) {
            err = f == StreamFormat::Unknown ? "format" : formatName(f);
            fatal = true;
            return;
        }
        std::unique_ptr<Codec> codec = makeCodec(f, rd, c);
        if (!codec) { err = "decode"; return; }
        {
            std::lock_guard<std::mutex> lk(c.m);
            if (!c.formatKnown) {
                c.rate = codec->rate;
                c.ch = codec->ch;
                c.capFrames = static_cast<size_t>(c.cfg.maxBufferSec * c.rate);
                c.preFrames = std::min(c.capFrames / 2, static_cast<size_t>(c.cfg.prebufferSec * c.rate));
                c.fifo.assign(c.capFrames * c.ch, 0.0f);
                c.formatKnown = true;
            } else if (c.rate != codec->rate || c.ch != codec->ch) {
                err = "formatChange";   // a lejátszó új hangfolyamot nyit (a vezérlő újraindítja)
                fatal = true;
                return;
            }
            c.codec = codec->name;
            c.reconnecting = false;
        }
        std::vector<float> tmp(1024 * static_cast<size_t>(codec->ch));
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(c.m);
                if (c.quit) break;
            }
            const size_t n = codec->decode(tmp.data(), 1024);
            if (n == 0 && rd.chainBreak() && !codec->formatChanged) {
                // új Ogg-lánc (a szerver a szám váltásakor új logikai folyamot kezd): új dekóder ugyanazon a kapcsolaton
                rd.nextChain();
                const uint32_t rate = codec->rate, ch = codec->ch;
                codec.reset();
                codec = makeCodec(f, rd, c);
                if (!codec) { err = "decode"; return; }
                if (codec->rate != rate || codec->ch != ch) { err = "formatChange"; fatal = true; return; }
                continue;
            }
            if (n == 0) break;
            c.push(tmp.data(), n);
            framesOut += n;
        }
        if (codec->formatChanged) { err = "formatChange"; fatal = true; }
        else err = "eof";
        return;
    }
}

void runWorker(std::shared_ptr<Core> cp) {
    Core& c = *cp;
    int failures = 0;
    for (;;) {
        std::string err;
        bool fatal = false;
        uint64_t frames = 0;
        session(c, err, fatal, frames);
        std::unique_lock<std::mutex> lk(c.m);
        if (c.quit) break;
        if (fatal || !c.formatKnown) {   // az első kapcsolat hibája végleges (a felhasználó azonnal lássa)
            c.failed = true;
            c.terminal = true;
            c.error = err;
            break;
        }
        failures = (c.rate && frames / c.rate >= 10) ? 0 : failures + 1;
        if (failures > c.cfg.maxFailures) {
            c.failed = true;
            c.terminal = true;
            c.error = err;
            break;
        }
        c.reconnecting = true;
        c.reconnects++;
        c.error = err;
        const int ms = std::min(30000, c.cfg.backoffBaseMs << std::clamp(failures - 1, 0, 5));   // 1, 2, 4 … 30 s
        c.cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return c.quit; });
        if (c.quit) break;
    }
    std::lock_guard<std::mutex> lk(c.m);
    c.terminal = true;
    c.done = true;
    c.cv.notify_all();
}

} // namespace

// =====================================================================================================================
// RadioDecoder
// =====================================================================================================================

RadioDecoder::RadioDecoder(std::shared_ptr<Core> c) : core_(std::move(c)) {
    thread_ = std::thread(runWorker, core_);
}

RadioDecoder::~RadioDecoder() {
    bool finished;
    {
        std::unique_lock<std::mutex> lk(core_->m);
        core_->quit = true;
        core_->closed = true;
        if (core_->conn) core_->conn->abort();
        core_->cv.notify_all();
        // a lejátszó zárja alatt is futhat: rövid várakozás, utána a szál a megosztott állapottal magától fejeződik be
        finished = core_->cv.wait_for(lk, std::chrono::milliseconds(500), [&] { return core_->done; });
    }
    if (thread_.joinable()) {
        if (finished) thread_.join();
        else thread_.detach();   // pl. lassú névfeloldás: a szál csak a saját (megosztott) állapotát éri el
    }
}

std::unique_ptr<RadioDecoder> RadioDecoder::open(const Config& cfgIn, std::string& err, const std::atomic<bool>* cancel) {
    RadioUrl u;
    if (!parseRadioUrl(cfgIn.url, u, nullptr)) { err = "url"; return nullptr; }
    auto core = std::make_shared<Core>();
    core->cfg = cfgIn;
    core->cfg.prebufferSec = std::clamp(core->cfg.prebufferSec, 0.1, 8.0);
    core->cfg.maxBufferSec = std::clamp(core->cfg.maxBufferSec, core->cfg.prebufferSec * 2, 30.0);
    core->cfg.maxFailures = std::clamp(core->cfg.maxFailures, 0, 1000);
    core->cfg.backoffBaseMs = std::clamp(core->cfg.backoffBaseMs, 10, 30000);
    std::unique_ptr<RadioDecoder> d(new RadioDecoder(core));
    const auto deadline = Clock::now() + std::chrono::seconds(core->cfg.net.headTimeoutS * 2 + 10) +
                          std::chrono::milliseconds(static_cast<int>(core->cfg.prebufferSec * 1000));
    {
        std::unique_lock<std::mutex> lk(core->m);
        for (;;) {
            if (core->formatKnown && !core->buffering) return d;
            if (core->terminal) { err = core->error.empty() ? "failed" : core->error; break; }
            if (cancel && cancel->load()) { err = "aborted"; break; }
            if (Clock::now() > deadline) { err = "timeout"; break; }
            core->cv.wait_for(lk, std::chrono::milliseconds(100));
        }
    }
    d.reset();
    return nullptr;
}

uint32_t RadioDecoder::sampleRate() const {
    std::lock_guard<std::mutex> lk(core_->m);
    return core_->rate;
}

uint32_t RadioDecoder::channels() const {
    std::lock_guard<std::mutex> lk(core_->m);
    return core_->ch;
}

size_t RadioDecoder::read(float* out, size_t frames) {
    Core& c = *core_;
    std::lock_guard<std::mutex> lk(c.m);
    const uint32_t ch = c.ch;
    if (!c.formatKnown || ch == 0 || frames == 0) return 0;
    if (c.buffering && !c.terminal) {
        std::memset(out, 0, frames * ch * sizeof(float));   // újrapufferelés: csend, a lejátszó nem áll le
        return frames;
    }
    const size_t n = std::min(frames, static_cast<size_t>(c.wr - c.rd));
    for (size_t i = 0; i < n; ++i) {
        const size_t idx = static_cast<size_t>((c.rd + i) % c.capFrames) * ch;
        std::memcpy(out + i * ch, &c.fifo[idx], ch * sizeof(float));
    }
    c.rd += n;
    while (!c.titles.empty() && c.titles.front().first <= c.rd) {
        c.title = c.titles.front().second;
        c.titles.pop_front();
    }
    if (n < frames) {
        if (c.terminal) return n;   // a végleges vég után a maradék, majd 0 (a lejátszó leáll)
        c.buffering = true;
        std::memset(out + n * ch, 0, (frames - n) * ch * sizeof(float));
        return frames;
    }
    return n;
}

void RadioDecoder::liveInfo(std::string& title, std::string& artist, std::string& album) const {
    std::lock_guard<std::mutex> lk(core_->m);
    album = core_->cfg.station.empty() ? core_->icyName : core_->cfg.station;
    if (core_->title.empty()) {
        title = album;
        artist.clear();
    } else {
        splitArtistTitle(core_->title, artist, title);
    }
}

RadioStatus RadioDecoder::statusOf(const Core& c) {
    std::lock_guard<std::mutex> lk(c.m);
    RadioStatus s;
    if (c.failed) s.state = RadioState::Failed;
    else if (c.terminal) s.state = RadioState::Ended;
    else if (c.reconnecting) s.state = RadioState::Reconnecting;
    else if (!c.formatKnown) s.state = RadioState::Connecting;
    else if (c.buffering) s.state = RadioState::Buffering;
    else s.state = RadioState::Playing;
    s.error = c.error;
    s.codec = c.codec;
    s.bitrate = c.bitrate;
    s.reconnects = c.reconnects;
    s.droppedFrames = c.dropped;
    s.framesIn = c.wr;
    s.icyName = c.icyName;
    s.streamTitle = c.title;
    return s;
}

bool RadioDecoder::closed(const Core& c) {
    std::lock_guard<std::mutex> lk(c.m);
    return c.closed;
}

RadioStatus RadioDecoder::status() const { return statusOf(*core_); }

// =====================================================================================================================
// Állomáskereső (közösségi katalógus)
// =====================================================================================================================

namespace {
constexpr size_t kMaxCatalogBytes = 1u << 20;

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}
} // namespace

bool catalogSearch(const std::string& query, const NetOptions& net, std::vector<CatalogItem>& out, std::string& err,
                   const std::function<void(HttpStream*)>& reg, const std::string& baseUrl) {
    out.clear();
    const std::string q = cleanText(query, 100);
    if (q.empty()) { err = "query"; return false; }
    const std::string url = (baseUrl.empty() ? std::string(kCatalogBase) : baseUrl) + "/json/stations/search?name=" +
                            urlEncode(q) + "&limit=50&hidebroken=true&order=clickcount&reverse=true";
    RadioUrl u;
    if (!parseRadioUrl(url, u, nullptr)) { err = "url"; return false; }
    NetOptions o = net;
    o.userAgent = std::string("Aqulify/") + AQULIFY_RADIO_VERSION;   // a katalógus kéri a „beszélő” azonosítót
    for (int attempt = 0; attempt < 2; ++attempt) {   // a DNS másik szervert adhat a második próbára
        std::string body;
        HttpHead head;
        std::string e;
        bool ok;
        {
            HttpStream hs;
            if (reg) reg(&hs);
            ok = hs.open(u, o, head, e);
            if (ok && head.status == 200) {
                uint8_t buf[16384];
                while (body.size() <= kMaxCatalogBytes) {
                    const size_t n = hs.read(buf, sizeof(buf));
                    if (n == 0) break;
                    body.append(reinterpret_cast<const char*>(buf), n);
                }
            }
            if (reg) reg(nullptr);
        }
        if (!ok) {
            err = e;
            if (e == "aborted" || e == "blocked") return false;
            continue;
        }
        if (head.status != 200) { err = "http"; continue; }
        if (body.size() > kMaxCatalogBytes) { err = "size"; return false; }
        json::Value v;
        if (!json::parse(body, v) || v.type != json::Value::Type::Array) { err = "json"; continue; }
        for (const auto& e2 : v.arr) {
            if (out.size() >= 50) break;
            auto s = [&](const char* k) { const json::Value* x = e2.get(k); return x ? x->stringOr("") : std::string(); };
            CatalogItem it;
            it.name = cleanText(s("name"), 120);
            std::string link = s("url_resolved");
            if (link.empty()) link = s("url");
            while (!link.empty() && (link.back() == ' ' || link.back() == '\r' || link.back() == '\n')) link.pop_back();
            RadioUrl ru;
            if (it.name.empty() || !parseRadioUrl(link, ru, nullptr)) continue;
            if (const json::Value* h = e2.get("hls"); h && h->numberOr(0) != 0) continue;   // szegmentált adás: nem játszható
            it.url = link;
            it.codec = cleanText(s("codec"), 16);
            it.country = cleanText(s("countrycode"), 4);
            it.tags = cleanText(s("tags"), 80);
            const json::Value* br = e2.get("bitrate");
            const double b = br ? br->numberOr(0) : 0;
            it.bitrate = b > 0 && b < 100000 ? static_cast<int>(b) : 0;
            out.push_back(std::move(it));
        }
        err.clear();
        return true;
    }
    return false;
}

} // namespace ee_analyzer::radio
