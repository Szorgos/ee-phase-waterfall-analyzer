#pragma once
/**
 * @file dns_packet.hpp
 * @brief Közös mDNS és DNS csomag-író és olvasó segédfüggvények (RFC 1035 és RFC 6762).
 */

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace ee_analyzer::dns_packet {

constexpr const char* kMulticastGroup = "224.0.0.251";
constexpr uint16_t kMdnsPort = 5353;

/** 16 bites érték hozzáfűzése hálózati bájtsorrendben (big-endian). */
inline void appendU16(std::vector<uint8_t>& buf, uint16_t val) {
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

/** 32 bites érték hozzáfűzése hálózati bájtsorrendben (big-endian). */
inline void appendU32(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

/** DNS-név hozzáfűzése pontokkal elválasztott címkék alakjában, végzáró nullával. */
inline void appendDnsName(std::vector<uint8_t>& buf, const std::string& name) {
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

/** Névtömörítés: mutató a csomagban korábban szereplő névre (2 bájt, 0xC000 | eltolás). */
inline void appendNamePointer(std::vector<uint8_t>& buf, size_t offset) {
    buf.push_back(static_cast<uint8_t>(0xC0 | ((offset >> 8) & 0x3F)));
    buf.push_back(static_cast<uint8_t>(offset & 0xFF));
}

/** DNS-név biztonságos olvasása a csomagból tömörítési mutatókkal; hiba esetén üres. */
inline std::string readDnsName(const uint8_t* p, size_t size, size_t& off) {
    if (!p || off >= size) return "";
    std::string out;
    size_t pos = off;
    bool jumped = false;
    for (int guard = 0; guard < 64 && pos < size; ++guard) {
        const uint8_t len = p[pos];
        if (len == 0) {
            if (!jumped) off = pos + 1;
            return out;
        }
        if ((len & 0xC0) == 0xC0) {
            if (pos + 1 >= size) return "";
            if (!jumped) off = pos + 2;
            pos = (static_cast<size_t>(len & 0x3F) << 8) | p[pos + 1];
            jumped = true;
            continue;
        }
        if (pos + 1 + len > size) return "";
        if (!out.empty()) out += '.';
        out.append(reinterpret_cast<const char*>(&p[pos + 1]), len);
        pos += 1 + len;
    }
    return "";
}

/** TXT rekord Pascal-sztringjeinek kulcs=érték párjait kinyerő segédfüggvény. */
inline std::map<std::string, std::string> parseTxtRecord(const uint8_t* rdata, size_t rdlen) {
    std::map<std::string, std::string> out;
    if (!rdata || rdlen == 0) return out;
    size_t pos = 0;
    while (pos < rdlen) {
        uint8_t len = rdata[pos++];
        if (pos + len > rdlen) break;
        std::string entry(reinterpret_cast<const char*>(rdata + pos), len);
        pos += len;
        size_t eq = entry.find('=');
        if (eq != std::string::npos) {
            std::string k = entry.substr(0, eq);
            std::string v = entry.substr(eq + 1);
            out[k] = v;
        } else if (!entry.empty()) {
            out[entry] = "";
        }
    }
    return out;
}

} // namespace ee_analyzer::dns_packet
