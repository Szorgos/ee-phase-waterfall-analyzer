#pragma once
/**
 * @file upnp_ssdp.hpp
 * @brief UPnP-renderer: SSDP-felfedezés (hirdetés és keresésre válasz) a helyi hálózaton, függőség nélkül.
 *
 * Forrás: UPnP Device Architecture 1.1 (2008), 1. fejezet (Discovery):
 *  - 1.2.2: NOTIFY * HTTP/1.1, NTS: ssdp:alive – HOST 239.255.255.250:1900, CACHE-CONTROL max-age, LOCATION, NT,
 *    SERVER, USN, BOOTID.UPNP.ORG, CONFIGID.UPNP.ORG. Gyökéreszköznél 3 + szolgáltatásonként 1 hirdetés
 *    (upnp:rootdevice, uuid:…, az eszköz típusa, a szolgáltatások típusai). Az ismétlés véletlen időközönként,
 *    a max-age felénél korábban.
 *  - 1.2.3: ssdp:byebye leálláskor (LOCATION és CACHE-CONTROL nélkül).
 *  - 1.3.2 / 1.3.3: M-SEARCH (MAN: "ssdp:discover", MX, ST) és a válasz (HTTP/1.1 200 OK, EXT, ST, USN …) a kérdezőnek
 *    unicastban, 0..MX (legfeljebb 5) másodperc véletlen késleltetéssel. Az ST lehet ssdp:all, upnp:rootdevice,
 *    uuid:…, az eszköz vagy egy szolgáltatás típusa.
 *
 * Felépítés az mdns_announcer.hpp mintájára (net_compat.hpp fölött). A csoporthoz minden magánhálózati (RFC 1918)
 * interfészen csatlakozik, és csak ezeken hirdet; a LOCATION mindig annak az interfésznek a címe, amelynek az
 * alhálózatából a kérdés jött. Más hálózatból (nyilvános cím, VPN-alagút, loopback) jövő keresésre nem válaszol.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "net_compat.hpp"
#include "upnp_soap.hpp"

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/select.h>
#endif

#ifndef AQULIFY_VERSION
#define AQULIFY_VERSION "0.0.0"
#endif

namespace ee_analyzer::upnp {

/** A gép működő, magánhálózati IPv4-interfészei (cím + maszk); loopback, link-local és nyilvános cím nélkül. */
inline std::vector<Iface> privateIfaces() {
    std::vector<Iface> out;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        r = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
    }
    if (r != NO_ERROR) return out;
    for (auto* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            if (sin->sin_family != AF_INET) continue;
            Iface f;
            f.ip = ntohl(sin->sin_addr.s_addr);
            const unsigned bits = std::min<unsigned>(u->OnLinkPrefixLength, 32);
            f.mask = bits == 0 ? 0u : ~0u << (32 - bits);
            f.name = a->AdapterName ? a->AdapterName : "";
            if (isPrivateV4(f.ip) && f.mask != 0) out.push_back(f);
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !a->ifa_netmask) continue;
        if (!(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
        Iface f;
        f.ip = ntohl(reinterpret_cast<const sockaddr_in*>(a->ifa_addr)->sin_addr.s_addr);
        f.mask = ntohl(reinterpret_cast<const sockaddr_in*>(a->ifa_netmask)->sin_addr.s_addr);
        f.name = a->ifa_name ? a->ifa_name : "";
        if (isPrivateV4(f.ip) && f.mask != 0) out.push_back(f);
    }
    freeifaddrs(list);
#endif
    return out;
}

/** Az az interfész, amelynek alhálózatában a cím van; nullptr: egyik sem. */
inline const Iface* ifaceFor(const std::vector<Iface>& ifs, uint32_t addr) {
    for (const auto& f : ifs)
        if (f.contains(addr)) return &f;
    return nullptr;
}

/** Várakozás olvasható adatra (ms); igaz, ha van. */
inline bool waitReadable(net::socket_t s, int ms) {
    fd_set r;
    FD_ZERO(&r);
    FD_SET(s, &r);
    timeval tv{ ms / 1000, (ms % 1000) * 1000 };
#ifdef _WIN32
    return ::select(0, &r, nullptr, nullptr, &tv) > 0;
#else
    return ::select(s + 1, &r, nullptr, nullptr, &tv) > 0;
#endif
}

namespace ssdp {

constexpr const char* kGroup = "239.255.255.250";
constexpr uint16_t kPort = 1900;
constexpr int kMaxAge = 1800;

inline std::string serverHeader() {
#ifdef _WIN32
    const char* os = "Windows/10";
#elif defined(__APPLE__)
    const char* os = "macOS/12";
#else
    const char* os = "Linux/5";
#endif
    return std::string(os) + " UPnP/1.1 Aqulify/" AQULIFY_VERSION;
}

/** Egy hirdetett cél: NT (illetve ST) és a hozzá tartozó USN. */
struct Target {
    std::string nt, usn;
};

/** A gyökéreszköz összes célja (UDA 1.1, 1.2.2): rootdevice, uuid, eszköztípus, a szolgáltatások típusai. */
inline std::vector<Target> targets(const std::string& uuid, const std::string& deviceType,
                                   const std::vector<std::string>& serviceTypes) {
    const std::string u = "uuid:" + uuid;
    std::vector<Target> t = { { "upnp:rootdevice", u + "::upnp:rootdevice" }, { u, u }, { deviceType, u + "::" + deviceType } };
    for (const auto& s : serviceTypes) t.push_back({ s, u + "::" + s });
    return t;
}

inline std::string notifyAlive(const Target& t, const std::string& location, uint32_t bootId) {
    return "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nCACHE-CONTROL: max-age=" + std::to_string(kMaxAge) +
           "\r\nLOCATION: " + location + "\r\nNT: " + t.nt + "\r\nNTS: ssdp:alive\r\nSERVER: " + serverHeader() +
           "\r\nUSN: " + t.usn + "\r\nBOOTID.UPNP.ORG: " + std::to_string(bootId) + "\r\nCONFIGID.UPNP.ORG: 1\r\n\r\n";
}

inline std::string notifyByebye(const Target& t, uint32_t bootId) {
    return "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nNT: " + t.nt + "\r\nNTS: ssdp:byebye\r\nUSN: " + t.usn +
           "\r\nBOOTID.UPNP.ORG: " + std::to_string(bootId) + "\r\nCONFIGID.UPNP.ORG: 1\r\n\r\n";
}

/** RFC 1123 dátum (a válasz DATE fejléce). */
inline std::string httpDate() {
    const std::time_t now = std::time(nullptr);
    std::tm g{};
#ifdef _WIN32
    gmtime_s(&g, &now);
#else
    gmtime_r(&now, &g);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &g);
    return buf;
}

inline std::string searchResponse(const Target& t, const std::string& location, uint32_t bootId) {
    return "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=" + std::to_string(kMaxAge) + "\r\nDATE: " + httpDate() +
           "\r\nEXT:\r\nLOCATION: " + location + "\r\nSERVER: " + serverHeader() + "\r\nST: " + t.nt + "\r\nUSN: " + t.usn +
           "\r\nBOOTID.UPNP.ORG: " + std::to_string(bootId) + "\r\nCONFIGID.UPNP.ORG: 1\r\n\r\n";
}

struct Search {
    std::string st;
    int mx = -1;   // -1: nincs megadva (unicast keresés: azonnali válasz)
};

/** M-SEARCH bontása (UDA 1.1, 1.3.2): az első sor, MAN: "ssdp:discover", ST, MX. false: nem keresés. */
inline bool parseSearch(const std::string& pkt, Search& out) {
    if (pkt.rfind("M-SEARCH * HTTP/1.", 0) != 0) return false;
    Search s;
    bool man = false;
    size_t pos = pkt.find('\n');
    while (pos != std::string::npos && pos + 1 < pkt.size()) {
        size_t eol = pkt.find('\n', pos + 1);
        std::string line = pkt.substr(pos + 1, (eol == std::string::npos ? pkt.size() : eol) - pos - 1);
        pos = eol;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        const size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = line.substr(0, c);
        for (char& ch : k) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        const std::string v = trim(line.substr(c + 1));
        if (k == "MAN") man = v == "\"ssdp:discover\"" || v == "ssdp:discover";
        else if (k == "ST") s.st = v;
        else if (k == "MX") {
            int m = 0;
            bool ok = !v.empty() && v.size() <= 4;
            for (char ch : v) {
                if (ch < '0' || ch > '9') { ok = false; break; }
                m = m * 10 + (ch - '0');
            }
            if (ok) s.mx = m;
        }
    }
    if (!man || s.st.empty()) return false;
    out = s;
    return true;
}

/**
 * A keresésre illő célok. Típusnál a kért verzió nem lehet nagyobb a miénknél (UDA 1.1, 1.3.2: a kisebb verzióra
 * is válaszolni kell, a saját típusunkkal; nálunk minden típus az 1. verzió).
 */
inline std::vector<Target> matching(const std::vector<Target>& all, const std::string& st) {
    if (st == "ssdp:all") return all;
    std::vector<Target> out;
    for (const auto& t : all) {
        if (t.nt == st) { out.push_back(t); continue; }
        // urn:…:typ:N kérés, ha N <= a mi verziónk
        const size_t c1 = st.rfind(':'), c2 = t.nt.rfind(':');
        if (c1 == std::string::npos || c2 == std::string::npos || st.rfind("urn:", 0) != 0) continue;
        if (st.substr(0, c1) != t.nt.substr(0, c2)) continue;
        const int want = std::atoi(st.c_str() + c1 + 1), have = std::atoi(t.nt.c_str() + c2 + 1);
        if (want >= 1 && want <= have) out.push_back(t);
    }
    return out;
}

} // namespace ssdp

/**
 * Az SSDP-hirdető: saját szálon figyel a 239.255.255.250:1900 csoportban, válaszol a keresésekre és ismétli a
 * hirdetést. A LOCATION a `location(ip)` függvény eredménye (pl. http://ip:port/description.xml).
 */
class SsdpAnnouncer {
public:
    using Location = std::function<std::string(const std::string& ip)>;

    SsdpAnnouncer() = default;
    ~SsdpAnnouncer() { stop(); }
    SsdpAnnouncer(const SsdpAnnouncer&) = delete;
    SsdpAnnouncer& operator=(const SsdpAnnouncer&) = delete;

    void start(std::vector<ssdp::Target> targets, Location location) {
        stop();
        targets_ = std::move(targets);
        location_ = std::move(location);
        bootId_ = static_cast<uint32_t>(std::time(nullptr) & 0x7FFFFFFF);
        running_ = true;
        worker_ = std::thread(&SsdpAnnouncer::run, this);
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (worker_.joinable()) worker_.join();
    }

    bool isRunning() const { return running_.load(); }
    /** Igaz, ha a csoporthoz legalább egy interfészen csatlakozott (az állapotkijelzéshez). */
    bool joined() const { return joined_.load(); }

private:
    struct Pending {
        std::chrono::steady_clock::time_point due;
        sockaddr_in to{};
        std::vector<std::string> pkts;
    };

    std::atomic<bool> running_{false};
    std::atomic<bool> joined_{false};
    std::vector<ssdp::Target> targets_;
    Location location_;
    uint32_t bootId_ = 1;
    std::thread worker_;
    std::mt19937 rng_{ std::random_device{}() };

    static sockaddr_in groupAddr() {
        sockaddr_in d{};
        d.sin_family = AF_INET;
        d.sin_port = htons(ssdp::kPort);
        inet_pton(AF_INET, ssdp::kGroup, &d.sin_addr);
        return d;
    }

    void sendTo(net::socket_t s, const std::string& pkt, const sockaddr_in& to) {
        ::sendto(s, pkt.data(), static_cast<int>(pkt.size()), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }

    /** Minden célt kiküld minden interfészen (alive vagy byebye). */
    void announce(net::socket_t s, const std::vector<Iface>& ifs, bool alive) {
        const sockaddr_in g = groupAddr();
        for (const auto& f : ifs) {
            in_addr ia{};
            ia.s_addr = htonl(f.ip);
            setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&ia), sizeof(ia));
            const std::string loc = location_ ? location_(v4Text(f.ip)) : std::string();
            for (const auto& t : targets_) sendTo(s, alive ? ssdp::notifyAlive(t, loc, bootId_) : ssdp::notifyByebye(t, bootId_), g);
        }
    }

    void joinAll(net::socket_t s, const std::vector<Iface>& ifs, bool join) {
        int n = 0;
        for (const auto& f : ifs) {
            ip_mreq m{};
            inet_pton(AF_INET, ssdp::kGroup, &m.imr_multiaddr);
            m.imr_interface.s_addr = htonl(f.ip);
            if (setsockopt(s, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, reinterpret_cast<const char*>(&m),
                           sizeof(m)) == 0)
                ++n;
        }
        if (join) joined_ = n > 0;
    }

    static bool sameIfaces(const std::vector<Iface>& a, const std::vector<Iface>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].ip != b[i].ip || a[i].mask != b[i].mask) return false;
        return true;
    }

    void run() {
        using Clock = std::chrono::steady_clock;
        net::socket_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == net::kInvalidSocket) { running_ = false; return; }
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef SO_REUSEPORT
        setsockopt(s, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&one), sizeof(one));
#endif
        unsigned char ttl = 2;   // UDA 1.1, 1.1.2: a TTL alapértéke 2
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
        unsigned char loop = 1;  // a gépen futó vezérlő (pl. upplay) is lássa
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof(loop));
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(ssdp::kPort);
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        const bool bound = ::bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0;

        std::vector<Iface> ifs = privateIfaces();
        if (bound) joinAll(s, ifs, true);
        // Indításkor háromszor (az UDP elveszhet), utána véletlen időközönként a max-age felénél korábban
        announce(s, ifs, true);
        int burst = 1;
        auto last = Clock::now();
        auto lastIfCheck = last;
        auto nextPeriod = [this] {
            std::uniform_int_distribution<int> d(ssdp::kMaxAge / 4, ssdp::kMaxAge / 2 - 30);
            return std::chrono::seconds(d(rng_));
        };
        auto period = nextPeriod();
        std::vector<Pending> pending;
        char buf[2048];
        while (running_.load()) {
            if (bound && waitReadable(s, 100)) {
                sockaddr_in from{};
#ifdef _WIN32
                int fl = sizeof(from);
#else
                socklen_t fl = sizeof(from);
#endif
                const int n = ::recvfrom(s, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fl);
                ssdp::Search q;
                if (n > 0 && ssdp::parseSearch(std::string(buf, static_cast<size_t>(n)), q)) {
                    const Iface* f = ifaceFor(ifs, ntohl(from.sin_addr.s_addr));
                    const auto hits = ssdp::matching(targets_, q.st);
                    if (f && !hits.empty() && pending.size() < 64) {
                        Pending p;
                        p.to = from;
                        const std::string loc = location_ ? location_(v4Text(f->ip)) : std::string();
                        for (const auto& t : hits) p.pkts.push_back(ssdp::searchResponse(t, loc, bootId_));
                        const int mx = q.mx < 0 ? 0 : std::min(q.mx, 5);
                        std::uniform_int_distribution<int> d(0, mx * 1000);
                        p.due = Clock::now() + std::chrono::milliseconds(mx > 0 ? d(rng_) : 0);
                        pending.push_back(std::move(p));
                    }
                }
            } else if (!bound) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            const auto now = Clock::now();
            for (size_t i = 0; i < pending.size();) {
                if (pending[i].due <= now) {
                    for (const auto& pk : pending[i].pkts) sendTo(s, pk, pending[i].to);
                    pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
            if (burst < 3 && now - last >= std::chrono::milliseconds(800)) {
                announce(s, ifs, true);
                ++burst;
                last = now;
            } else if (now - last >= period) {
                announce(s, ifs, true);
                last = now;
                period = nextPeriod();
            }
            // Hálózatváltás (pl. új DHCP-cím): a csoporttagság és a hirdetés az új interfészekkel
            if (now - lastIfCheck >= std::chrono::seconds(30)) {
                lastIfCheck = now;
                std::vector<Iface> fresh = privateIfaces();
                if (!sameIfaces(fresh, ifs)) {
                    if (bound) {
                        joinAll(s, ifs, false);
                        joinAll(s, fresh, true);
                    }
                    ifs = std::move(fresh);
                    announce(s, ifs, true);
                    burst = 1;
                    last = now;
                }
            }
        }
        announce(s, ifs, false);   // ssdp:byebye
        if (bound) joinAll(s, ifs, false);
        net::closeSocket(s);
    }
};

} // namespace ee_analyzer::upnp
