#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <sstream>
#include <iomanip>

namespace ee_analyzer {

/**
 * @brief Önálló, szabványos SHA-1 implementáció (RFC 3174) külső függőségek nélkül
 */
class Sha1 {
public:
    static std::vector<uint8_t> hash(const std::string& input) {
        uint32_t h0 = 0x67452301;
        uint32_t h1 = 0xEFCDAB89;
        uint32_t h2 = 0x98BADCFE;
        uint32_t h3 = 0x10325476;
        uint32_t h4 = 0xC3D2E1F0;

        std::vector<uint8_t> msg(input.begin(), input.end());
        uint64_t bitLen = msg.size() * 8;

        msg.push_back(0x80);
        while ((msg.size() % 64) != 56) {
            msg.push_back(0x00);
        }

        for (int i = 7; i >= 0; --i) {
            msg.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xFF));
        }

        auto rol = [](uint32_t value, uint32_t bits) -> uint32_t {
            return (value << bits) | (value >> (32 - bits));
        };

        for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
            uint32_t w[80];
            for (size_t i = 0; i < 16; ++i) {
                w[i] = (static_cast<uint32_t>(msg[chunk + i * 4]) << 24) |
                       (static_cast<uint32_t>(msg[chunk + i * 4 + 1]) << 16) |
                       (static_cast<uint32_t>(msg[chunk + i * 4 + 2]) << 8) |
                       (static_cast<uint32_t>(msg[chunk + i * 4 + 3]));
            }
            for (size_t i = 16; i < 80; ++i) {
                w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            }

            uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;

            for (size_t i = 0; i < 80; ++i) {
                uint32_t f = 0, k = 0;
                if (i < 20) {
                    f = (b & c) | ((~b) & d);
                    k = 0x5A827999;
                } else if (i < 40) {
                    f = b ^ c ^ d;
                    k = 0x6ED9EBA1;
                } else if (i < 60) {
                    f = (b & c) | (b & d) | (c & d);
                    k = 0x8F1BBCDC;
                } else {
                    f = b ^ c ^ d;
                    k = 0xCA62C1D6;
                }

                uint32_t temp = rol(a, 5) + f + e + k + w[i];
                e = d;
                d = c;
                c = rol(b, 30);
                b = a;
                a = temp;
            }

            h0 += a;
            h1 += b;
            h2 += c;
            h3 += d;
            h4 += e;
        }

        std::vector<uint8_t> digest(20);
        for (int i = 0; i < 4; ++i) {
            digest[i] = static_cast<uint8_t>((h0 >> ((3 - i) * 8)) & 0xFF);
            digest[i + 4] = static_cast<uint8_t>((h1 >> ((3 - i) * 8)) & 0xFF);
            digest[i + 8] = static_cast<uint8_t>((h2 >> ((3 - i) * 8)) & 0xFF);
            digest[i + 12] = static_cast<uint8_t>((h3 >> ((3 - i) * 8)) & 0xFF);
            digest[i + 16] = static_cast<uint8_t>((h4 >> ((3 - i) * 8)) & 0xFF);
        }
        return digest;
    }
};

/**
 * @brief Base64 kódoló a WebSocket Sec-WebSocket-Accept válaszhoz
 */
class Base64 {
public:
    static std::string encode(const std::vector<uint8_t>& data) {
        static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        int val = 0, valb = -6;
        for (uint8_t c : data) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) {
                out.push_back(table[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        if (valb > -6) out.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
        while (out.size() % 4) out.push_back('=');
        return out;
    }
};

} // namespace ee_analyzer
