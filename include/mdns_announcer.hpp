#pragma once
/**
 * @file mdns_announcer.hpp
 * @brief Aqulify 2.x mDNS / Zeroconf hálózati szolgáltatás-hirdető (RFC 6762).
 *
 * Függőségektől mentes, cross-platform UDP multicast hirdető (_aqulify._tcp.local).
 * Amikor az Aqulify hálózati elérése be van kapcsolva a PC-n, ez az osztály
 * periodikusan és lekérdezésre (query) is meghirdeti a gép nevét, platformját,
 * verzióját és a WebSocket portot a helyi hálózaton (224.0.0.251:5353).
 * Leállításkor RFC 6762 szerinti búcsúcsomagot (Goodbye packet, TTL=0) küld.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "net_compat.hpp"

namespace ee_analyzer {

class MdnsAnnouncer {
public:
    MdnsAnnouncer() = default;
    ~MdnsAnnouncer() { stop(); }

    MdnsAnnouncer(const MdnsAnnouncer&) = delete;
    MdnsAnnouncer& operator=(const MdnsAnnouncer&) = delete;

    /**
     * @brief mDNS hirdetés indítása a megadott porttal és gépnévvel.
     * @param servicePort A távoli WebSocket elérés portja (pl. 47820)
     * @param customName Egyedi megjelenítendő név (üres: a gép neve)
     */
    bool start(int servicePort, const std::string& customName = "") {
        if (running_) stop();

        servicePort_ = servicePort;
        hostname_ = defaultHostname();
        instanceName_ = customName.empty() ? ("Aqulify (" + hostname_ + ")") : customName;
        running_ = true;

        worker_ = std::thread(&MdnsAnnouncer::run, this);
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;

        // Búcsúcsomag (Goodbye packet, TTL=0): a kliensek azonnal törlik a gyorsítótárból
        sendAnnouncement(0);

        if (worker_.joinable()) worker_.join();
    }

    bool isRunning() const { return running_.load(); }
    int servicePort() const { return servicePort_; }
    const std::string& instanceName() const { return instanceName_; }

    /**
     * @brief RFC 6762 kompatibilis DNS válaszcsomag összeállítása (teszteléshez is).
     * @param ttl Érvényességi idő másodpercben (0 = búcsú / törlés)
     */
    std::vector<uint8_t> buildPacket(uint32_t ttl = 120) const {
        std::vector<uint8_t> pkt;
        pkt.reserve(512);

        // DNS Fejléc (Header)
        // ID: 0x0000, Flags: 0x8400 (Response, Authoritative Answer)
        pkt.push_back(0x00); pkt.push_back(0x00);
        pkt.push_back(0x84); pkt.push_back(0x00);
        // QDCOUNT = 0
        pkt.push_back(0x00); pkt.push_back(0x00);
        // ANCOUNT = 3 (PTR + SRV + TXT rekordok)
        pkt.push_back(0x00); pkt.push_back(0x03);
        // NSCOUNT = 0, ARCOUNT = 0
        pkt.push_back(0x00); pkt.push_back(0x00);
        pkt.push_back(0x00); pkt.push_back(0x00);

        // 1. Rekord: PTR (_aqulify._tcp.local -> <InstanceName>._aqulify._tcp.local)
        // PTR osztott válasz, nem kap cache-flush bitet (osztály = 0x0001, IN)
        appendDnsName(pkt, "_aqulify._tcp.local");
        appendU16(pkt, 0x000C); // TYPE: PTR (12)
        appendU16(pkt, 0x0001); // CLASS: IN (1)
        appendU32(pkt, ttl);    // TTL

        std::string fullInstance = instanceName_ + "._aqulify._tcp.local";
        std::vector<uint8_t> ptrData;
        appendDnsName(ptrData, fullInstance);
        appendU16(pkt, static_cast<uint16_t>(ptrData.size()));
        pkt.insert(pkt.end(), ptrData.begin(), ptrData.end());

        // 2. Rekord: SRV (<InstanceName>._aqulify._tcp.local -> Target: <Hostname>.local, Port)
        // Egyedi rekord: cache-flush bit 0x8000 bekapcsolva (CLASS = 0x8001)
        appendDnsName(pkt, fullInstance);
        appendU16(pkt, 0x0021); // TYPE: SRV (33)
        appendU16(pkt, 0x8001); // CLASS: IN | Flush (0x8001)
        appendU32(pkt, ttl);    // TTL

        std::vector<uint8_t> srvData;
        appendU16(srvData, 0); // Priority: 0
        appendU16(srvData, 0); // Weight: 0
        appendU16(srvData, static_cast<uint16_t>(servicePort_)); // Port
        appendDnsName(srvData, hostname_ + ".local");
        appendU16(pkt, static_cast<uint16_t>(srvData.size()));
        pkt.insert(pkt.end(), srvData.begin(), srvData.end());

        // 3. Rekord: TXT (<InstanceName>._aqulify._tcp.local -> metaadatok)
        appendDnsName(pkt, fullInstance);
        appendU16(pkt, 0x0010); // TYPE: TXT (16)
        appendU16(pkt, 0x8001); // CLASS: IN | Flush (0x8001)
        appendU32(pkt, ttl);    // TTL

        std::vector<std::string> txtEntries = {
            "name=" + instanceName_,
            "port=" + std::to_string(servicePort_),
            "proto=2",
            "ver=2.0.0",
            "caps=library,queue,playlists,phone_track",
#ifdef _WIN32
            "platform=windows"
#elif defined(__APPLE__)
            "platform=macos"
#else
            "platform=linux"
#endif
        };
        std::vector<uint8_t> txtData;
        for (const auto& entry : txtEntries) {
            if (entry.empty() || entry.size() > 255) continue;
            txtData.push_back(static_cast<uint8_t>(entry.size()));
            txtData.insert(txtData.end(), entry.begin(), entry.end());
        }
        appendU16(pkt, static_cast<uint16_t>(txtData.size()));
        pkt.insert(pkt.end(), txtData.begin(), txtData.end());

        return pkt;
    }

    static void appendDnsName(std::vector<uint8_t>& buf, const std::string& name) {
        size_t start = 0;
        while (start < name.size()) {
            size_t dot = name.find('.', start);
            if (dot == std::string::npos) dot = name.size();
            uint8_t len = static_cast<uint8_t>(std::min<size_t>(dot - start, 63));
            buf.push_back(len);
            for (size_t i = 0; i < len; ++i) buf.push_back(static_cast<uint8_t>(name[start + i]));
            start = dot + 1;
        }
        buf.push_back(0x00); // Végzáró null
    }

private:
    std::atomic<bool> running_{false};
    int servicePort_{47820};
    std::string hostname_;
    std::string instanceName_;
    std::thread worker_;

    static constexpr const char* kMulticastGroup = "224.0.0.251";
    static constexpr uint16_t kMdnsPort = 5353;

    static void appendU16(std::vector<uint8_t>& buf, uint16_t val) {
        buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(val & 0xFF));
    }

    static void appendU32(std::vector<uint8_t>& buf, uint32_t val) {
        buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
        buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(val & 0xFF));
    }

    static std::string defaultHostname() {
        char buf[256] = {0};
#ifdef _WIN32
        DWORD sz = sizeof(buf);
        if (GetComputerNameA(buf, &sz) && sz > 0) return std::string(buf);
#else
        if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0] != '\0') return std::string(buf);
#endif
        return "Aqulify-PC";
    }

    void sendAnnouncement(uint32_t ttl) {
        net::socket_t sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == net::kInvalidSocket) return;

        unsigned char loop = 1;
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof(loop));
        unsigned char hops = 255;
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&hops), sizeof(hops));

        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(kMdnsPort);
        inet_pton(AF_INET, kMulticastGroup, &dest.sin_addr);

        std::vector<uint8_t> pkt = buildPacket(ttl);
        ::sendto(sock, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0,
                 reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));

        net::closeSocket(sock);
    }

    void run() {
        net::socket_t sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == net::kInvalidSocket) return;

        int reuse = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#ifdef SO_REUSEPORT
        setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#endif

        unsigned char loop = 1;
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof(loop));
        unsigned char hops = 255;
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&hops), sizeof(hops));

        sockaddr_in localAddr{};
        localAddr.sin_family = AF_INET;
        localAddr.sin_port = htons(kMdnsPort);
        localAddr.sin_addr.s_addr = htonl(INADDR_ANY);

        bool bound = (::bind(sock, reinterpret_cast<const sockaddr*>(&localAddr), sizeof(localAddr)) == 0);

        if (bound) {
            ip_mreq mreq{};
            inet_pton(AF_INET, kMulticastGroup, &mreq.imr_multiaddr);
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));
        }

        net::setNonBlocking(sock, true);

        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(kMdnsPort);
        inet_pton(AF_INET, kMulticastGroup, &dest.sin_addr);

        // 1. Kezdeti bejelentés küldése (initial burst: 2 csomag, 1s különbséggel)
        std::vector<uint8_t> annPkt = buildPacket(120);
        ::sendto(sock, reinterpret_cast<const char*>(annPkt.data()), static_cast<int>(annPkt.size()), 0,
                 reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));

        auto lastAnnounce = std::chrono::steady_clock::now();
        int burstCount = 1;

        uint8_t inBuf[2048];
        while (running_.load()) {
            // Lekérdezések figyelése
            if (bound) {
                sockaddr_in srcAddr{};
#ifdef _WIN32
                int srcLen = sizeof(srcAddr);
#else
                socklen_t srcLen = sizeof(srcAddr);
#endif
                int n = ::recvfrom(sock, reinterpret_cast<char*>(inBuf), sizeof(inBuf), 0,
                                   reinterpret_cast<sockaddr*>(&srcAddr), &srcLen);
                if (n > 12) {
                    // Ha query (QR bit == 0) és szerepel benne az "_aqulify"
                    bool isQuery = ((inBuf[2] & 0x80) == 0);
                    if (isQuery && matchesQuery(inBuf, n, "_aqulify")) {
                        ::sendto(sock, reinterpret_cast<const char*>(annPkt.data()), static_cast<int>(annPkt.size()), 0,
                                 reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (!running_.load()) break;

            auto now = std::chrono::steady_clock::now();
            if (burstCount < 2 && now - lastAnnounce >= std::chrono::seconds(1)) {
                ::sendto(sock, reinterpret_cast<const char*>(annPkt.data()), static_cast<int>(annPkt.size()), 0,
                         reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
                lastAnnounce = now;
                burstCount++;
            } else if (now - lastAnnounce >= std::chrono::seconds(15)) {
                ::sendto(sock, reinterpret_cast<const char*>(annPkt.data()), static_cast<int>(annPkt.size()), 0,
                         reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
                lastAnnounce = now;
            }
        }

        net::closeSocket(sock);
    }

    static bool matchesQuery(const uint8_t* buf, int len, const char* needle) {
        if (!needle || len <= 12) return false;
        size_t nlen = std::strlen(needle);
        for (int i = 12; i + static_cast<int>(nlen) <= len; ++i) {
            if (std::memcmp(buf + i, needle, nlen) == 0) return true;
        }
        return false;
    }
};

} // namespace ee_analyzer
