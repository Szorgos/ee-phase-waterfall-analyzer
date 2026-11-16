#pragma once
/**
 * @file mdns_browser.hpp
 * @brief Aqulify mDNS szolgáltatás-felderítő (RFC 6762).
 *
 * PTR-lekérdezések küldése (_aqulify._tcp.local, _smb._tcp.local, _nfs._tcp.local)
 * és válaszcsomagok (PTR/SRV/TXT/A) értelmezése.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "net_compat.hpp"
#include "dns_packet.hpp"

namespace ee_analyzer {

struct DiscoveredService {
    std::string kind; // "engine" | "smb" | "nfs"
    std::string name;
    std::string host;
    std::vector<std::string> addrs;
    int port = 0;
    std::map<std::string, std::string> txt;
};

struct DiscoveryResult {
    std::vector<DiscoveredService> engines;
    std::vector<DiscoveredService> shares;
};

class MdnsBrowser {
public:
    MdnsBrowser() = default;

    /** Ellenőrzi, hogy a megadott IPv4 cím privát (RFC 1918), link-local vagy loopback-e. */
    static bool isPrivateOrLinkLocal(const std::string& ipStr) {
        in_addr addr{};
        if (inet_pton(AF_INET, ipStr.c_str(), &addr) != 1) return false;
        const uint32_t ip = ntohl(addr.s_addr);
        const uint8_t b0 = static_cast<uint8_t>((ip >> 24) & 0xFF);
        const uint8_t b1 = static_cast<uint8_t>((ip >> 16) & 0xFF);
        if (b0 == 10) return true;                                // 10.0.0.0/8
        if (b0 == 172 && (b1 >= 16 && b1 <= 31)) return true;     // 172.16.0.0/12
        if (b0 == 192 && b1 == 168) return true;                  // 192.168.0.0/16
        if (b0 == 169 && b1 == 254) return true;                  // 169.254.0.0/16 (link-local)
        if (b0 == 127) return true;                                // 127.0.0.0/8 (loopback / elszigetelt tesztek)
        return false;
    }

    /** Címek rendezése: a megbízhatóbb otthoni alhálózatok elöl. */
    static void sortAddresses(std::vector<std::string>& addrs) {
        std::sort(addrs.begin(), addrs.end(), [](const std::string& a, const std::string& b) {
            auto rank = [](const std::string& s) -> int {
                if (s.rfind("192.168.", 0) == 0) return 0;
                if (s.rfind("10.", 0) == 0) return 1;
                if (s.rfind("172.", 0) == 0) return 2;
                if (s.rfind("127.", 0) == 0) return 3;
                return 4;
            };
            return rank(a) < rank(b);
        });
    }

    /** PTR lekérdező csomag építése a megadott szolgáltatástípushoz. */
    static std::vector<uint8_t> buildQuery(const std::string& serviceType) {
        std::vector<uint8_t> pkt;
        pkt.reserve(64);
        // Header: ID=0, Flags=0x0000, QDCOUNT=1, ANCOUNT=0, NSCOUNT=0, ARCOUNT=0
        dns_packet::appendU16(pkt, 0x0000);
        dns_packet::appendU16(pkt, 0x0000);
        dns_packet::appendU16(pkt, 0x0001);
        dns_packet::appendU16(pkt, 0x0000);
        dns_packet::appendU16(pkt, 0x0000);
        dns_packet::appendU16(pkt, 0x0000);

        dns_packet::appendDnsName(pkt, serviceType);
        dns_packet::appendU16(pkt, 0x000C); // TYPE: PTR (12)
        dns_packet::appendU16(pkt, 0x0001); // CLASS: IN (1)
        return pkt;
    }

    /** Beérkező DNS válaszcsomag biztonságos feldolgozása. */
    static std::vector<DiscoveredService> parsePacket(const uint8_t* p, size_t size) {
        std::vector<DiscoveredService> results;
        if (!p || size < 12) return results;

        const uint16_t flags = static_cast<uint16_t>((p[2] << 8) | p[3]);
        // Csak válaszcsomagot fogadunk (QR bit = 1)
        if ((flags & 0x8000) == 0) return results;

        const uint16_t qdcount = static_cast<uint16_t>((p[4] << 8) | p[5]);
        const uint16_t ancount = static_cast<uint16_t>((p[6] << 8) | p[7]);
        const uint16_t nscount = static_cast<uint16_t>((p[8] << 8) | p[9]);
        const uint16_t arcount = static_cast<uint16_t>((p[10] << 8) | p[11]);
        const size_t totalRecords = std::min<size_t>(static_cast<size_t>(ancount) + nscount + arcount, size / 10 + 1);

        size_t off = 12;
        // Kérdések átlépése
        for (uint16_t i = 0; i < qdcount; ++i) {
            if (off >= size) return results;
            dns_packet::readDnsName(p, size, off);
            if (off + 4 > size) return results;
            off += 4; // qtype + qclass
        }

        struct DnsParsedRecord {
            std::string name;
            uint16_t type = 0;
            size_t rdataOff = 0;
            size_t rdlen = 0;
        };
        std::vector<DnsParsedRecord> records;
        records.reserve(totalRecords);

        for (size_t i = 0; i < totalRecords; ++i) {
            if (off >= size) break;
            std::string rname = dns_packet::readDnsName(p, size, off);
            if (rname.empty() || off + 10 > size) break;
            const uint16_t rtype = static_cast<uint16_t>((p[off] << 8) | p[off + 1]);
            const size_t rdlen = (static_cast<size_t>(p[off + 8]) << 8) | p[off + 9];
            off += 10;
            if (off + rdlen > size) break;
            records.push_back({rname, rtype, off, rdlen});
            off += rdlen;
        }

        // 1. PTR rekordok és instance nevek feltérképezése
        struct InstanceContext {
            std::string kind;
            std::string instanceName;
            std::string host;
            int port = 0;
            std::map<std::string, std::string> txt;
            std::vector<std::string> addrs;
        };
        std::map<std::string, InstanceContext> instances;

        auto kindFromType = [](const std::string& s) -> std::string {
            if (s.find("_aqulify._tcp") != std::string::npos) return "engine";
            if (s.find("_smb._tcp") != std::string::npos) return "smb";
            if (s.find("_nfs._tcp") != std::string::npos) return "nfs";
            return "";
        };

        for (const auto& r : records) {
            if (r.type == 0x000C) { // PTR
                std::string k = kindFromType(r.name);
                if (k.empty()) continue;
                size_t roff = r.rdataOff;
                std::string targetInstance = dns_packet::readDnsName(p, size, roff);
                if (!targetInstance.empty()) {
                    auto& ctx = instances[targetInstance];
                    ctx.kind = k;
                    ctx.instanceName = targetInstance;
                }
            }
        }

        // Ha a csomagban nincs PTR (pl. csak SRV/TXT/A érkezett), de a névben benne van a szolgáltatás
        for (const auto& r : records) {
            if (r.type == 0x0021 || r.type == 0x0010) { // SRV vagy TXT
                std::string k = kindFromType(r.name);
                if (!k.empty() && instances.find(r.name) == instances.end()) {
                    auto& ctx = instances[r.name];
                    ctx.kind = k;
                    ctx.instanceName = r.name;
                }
            }
        }

        // 2. SRV rekordok feldolgozása (Port, Target Host)
        for (const auto& r : records) {
            if (r.type == 0x0021) { // SRV
                auto it = instances.find(r.name);
                if (it != instances.end() && r.rdlen >= 6) {
                    uint16_t port = static_cast<uint16_t>((p[r.rdataOff + 4] << 8) | p[r.rdataOff + 5]);
                    size_t roff = r.rdataOff + 6;
                    std::string targetHost = dns_packet::readDnsName(p, size, roff);
                    it->second.port = port;
                    it->second.host = targetHost;
                }
            }
        }

        // 3. TXT rekordok feldolgozása
        for (const auto& r : records) {
            if (r.type == 0x0010) { // TXT
                auto it = instances.find(r.name);
                if (it != instances.end()) {
                    it->second.txt = dns_packet::parseTxtRecord(p + r.rdataOff, r.rdlen);
                    if (it->second.port == 0 && it->second.txt.count("port")) {
                        try { it->second.port = std::stoi(it->second.txt["port"]); } catch (...) {}
                    }
                    if (it->second.txt.count("addr")) {
                        const std::string& addrsStr = it->second.txt["addr"];
                        size_t pos = 0;
                        while (pos < addrsStr.size()) {
                            size_t comma = addrsStr.find(',', pos);
                            if (comma == std::string::npos) comma = addrsStr.size();
                            std::string ip = addrsStr.substr(pos, comma - pos);
                            if (isPrivateOrLinkLocal(ip)) {
                                if (std::find(it->second.addrs.begin(), it->second.addrs.end(), ip) == it->second.addrs.end()) {
                                    it->second.addrs.push_back(ip);
                                }
                            }
                            pos = comma + 1;
                        }
                    }
                }
            }
        }

        // 4. A-rekordok feldolgozása (IPv4)
        for (const auto& r : records) {
            if (r.type == 0x0001 && r.rdlen == 4) { // A
                char ipBuf[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, p + r.rdataOff, ipBuf, sizeof(ipBuf));
                std::string ipStr(ipBuf);
                if (isPrivateOrLinkLocal(ipStr)) {
                    for (auto& pair : instances) {
                        auto& ctx = pair.second;
                        if (!ctx.host.empty() && (ctx.host == r.name || ctx.host.rfind(r.name, 0) == 0 || r.name.rfind(ctx.host, 0) == 0)) {
                            if (std::find(ctx.addrs.begin(), ctx.addrs.end(), ipStr) == ctx.addrs.end()) {
                                ctx.addrs.push_back(ipStr);
                            }
                        }
                    }
                }
            }
        }

        // 5. Eredmények összeállítása
        for (auto& pair : instances) {
            auto& ctx = pair.second;
            DiscoveredService svc;
            svc.kind = ctx.kind;
            if (ctx.txt.count("name") && !ctx.txt["name"].empty()) {
                svc.name = ctx.txt["name"];
            } else {
                size_t dot = ctx.instanceName.find('.');
                svc.name = (dot != std::string::npos) ? ctx.instanceName.substr(0, dot) : ctx.instanceName;
            }
            svc.host = ctx.host;
            svc.port = ctx.port;
            if (svc.port == 0) {
                if (svc.kind == "engine") svc.port = 47820;
                else if (svc.kind == "smb") svc.port = 445;
                else if (svc.kind == "nfs") svc.port = 2049;
            }
            svc.addrs = ctx.addrs;
            sortAddresses(svc.addrs);
            svc.txt = ctx.txt;
            results.push_back(std::move(svc));
        }

        return results;
    }

    /** Élő mDNS böngészés hálózaton keresztül. */
    static DiscoveryResult browse(std::chrono::milliseconds timeout, const std::string& myMachineId = "", int myPort = 0) {
        DiscoveryResult result;
        net::socket_t sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == net::kInvalidSocket) return result;

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
        localAddr.sin_port = htons(dns_packet::kMdnsPort);
        localAddr.sin_addr.s_addr = htonl(INADDR_ANY);

        bool bound = (::bind(sock, reinterpret_cast<const sockaddr*>(&localAddr), sizeof(localAddr)) == 0);
        if (!bound) {
            localAddr.sin_port = 0;
            ::bind(sock, reinterpret_cast<const sockaddr*>(&localAddr), sizeof(localAddr));
        }

        ip_mreq mreq{};
        inet_pton(AF_INET, dns_packet::kMulticastGroup, &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));

        net::setNonBlocking(sock, true);

        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(dns_packet::kMdnsPort);
        inet_pton(AF_INET, dns_packet::kMulticastGroup, &dest.sin_addr);

        std::vector<std::string> queryServices = {
            "_aqulify._tcp.local",
            "_smb._tcp.local",
            "_nfs._tcp.local"
        };

        auto sendQueries = [&]() {
            for (const auto& svc : queryServices) {
                auto qPkt = buildQuery(svc);
                ::sendto(sock, reinterpret_cast<const char*>(qPkt.data()), static_cast<int>(qPkt.size()), 0,
                         reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
            }
        };

        sendQueries();

        std::map<std::string, DiscoveredService> discovered;
        auto start = std::chrono::steady_clock::now();
        auto lastBurst = start;
        uint8_t inBuf[2048];

        while (std::chrono::steady_clock::now() - start < timeout) {
            auto now = std::chrono::steady_clock::now();
            if (now - lastBurst >= std::chrono::seconds(1)) {
                sendQueries();
                lastBurst = now;
            }

            sockaddr_in srcAddr{};
#ifdef _WIN32
            int srcLen = sizeof(srcAddr);
#else
            socklen_t srcLen = sizeof(srcAddr);
#endif
            int n = ::recvfrom(sock, reinterpret_cast<char*>(inBuf), sizeof(inBuf), 0,
                               reinterpret_cast<sockaddr*>(&srcAddr), &srcLen);
            if (n > 12) {
                auto list = parsePacket(inBuf, static_cast<size_t>(n));
                for (auto& s : list) {
                    char srcIp[INET_ADDRSTRLEN] = {0};
                    inet_ntop(AF_INET, &srcAddr.sin_addr, srcIp, sizeof(srcIp));
                    std::string srcIpStr(srcIp);
                    if (isPrivateOrLinkLocal(srcIpStr)) {
                        if (std::find(s.addrs.begin(), s.addrs.end(), srcIpStr) == s.addrs.end()) {
                            s.addrs.push_back(srcIpStr);
                            sortAddresses(s.addrs);
                        }
                    }
                    std::string key = s.kind + ":" + s.name;
                    auto it = discovered.find(key);
                    if (it == discovered.end()) {
                        discovered[key] = std::move(s);
                    } else {
                        if (it->second.port == 0 && s.port != 0) it->second.port = s.port;
                        if (it->second.host.empty() && !s.host.empty()) it->second.host = s.host;
                        for (const auto& ip : s.addrs) {
                            if (std::find(it->second.addrs.begin(), it->second.addrs.end(), ip) == it->second.addrs.end()) {
                                it->second.addrs.push_back(ip);
                            }
                        }
                        sortAddresses(it->second.addrs);
                        for (const auto& kv : s.txt) {
                            if (it->second.txt.find(kv.first) == it->second.txt.end()) {
                                it->second.txt[kv.first] = kv.second;
                            }
                        }
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        net::closeSocket(sock);

        for (auto& pair : discovered) {
            auto& s = pair.second;
            if (s.kind == "engine") {
                // Saját gép kihagyása (machineId vagy saját port/loopback alapján)
                if (!myMachineId.empty() && s.txt.count("id") && s.txt["id"] == myMachineId) continue;
                if (myPort > 0 && s.port == myPort) {
                    bool selfIp = false;
                    for (const auto& ip : s.addrs) {
                        if (ip == "127.0.0.1" || ip == "localhost") { selfIp = true; break; }
                    }
                    if (selfIp && !myMachineId.empty()) continue;
                }
                result.engines.push_back(std::move(s));
            } else if (s.kind == "smb" || s.kind == "nfs") {
                result.shares.push_back(std::move(s));
            }
        }

        return result;
    }
};

} // namespace ee_analyzer
