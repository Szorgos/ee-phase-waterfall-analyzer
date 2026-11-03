#pragma once
/**
 * @file pcm_convert.hpp
 * @brief Lebegőpontos minták átalakítása egész PCM-re (kis-endián) a hangkártya formátumára: 16 bit, 24 bit (3 bájt),
 *        24 bit 32 bites keretben (a felső 24 biten), 32 bit. A Windows-lejátszó kizárólagos módja használja
 *        (player_output_wasapi.hpp); rendszerfüggetlen, hogy a tesztek Linuxon is ellenőrizzék.
 *
 * Kerekítés a legközelebbi egészre; a tartományon kívüli érték a legnagyobb / legkisebb kódra kerül (1.0 → max).
 * A visszatérési érték jelzi, hogy minden minta pontosan a rácsra esett-e (bitpontos átalakítás).
 */
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ee_analyzer::pcm {

/**
 * bits: a minta mérete a pufferben (16, 24 vagy 32); valid: ebből érvényes (16, 24 vagy 32; valid <= bits).
 * false, ha valamelyik minta nem esett pontosan a rácsra (kerekítés vagy vágás történt).
 */
inline bool floatToInt(const float* in, uint8_t* out, size_t samples, int bits, int valid) {
    const double scale = std::ldexp(1.0, valid - 1);
    const long long hi = (1LL << (valid - 1)) - 1, lo = -(1LL << (valid - 1));
    bool exact = true;
    for (size_t i = 0; i < samples; ++i) {
        const double v = static_cast<double>(in[i]) * scale;
        long long r = std::llround(v);
        if (r > hi) r = hi;
        else if (r < lo) r = lo;
        if (static_cast<double>(r) != v) exact = false;
        if (bits == 32) {
            const int32_t s = static_cast<int32_t>(r * (1LL << (32 - valid)));   // 24 érvényes bit: a felső bitekre
            std::memcpy(out + i * 4, &s, 4);
        } else if (bits == 24) {
            const int32_t s = static_cast<int32_t>(r);
            out[i * 3] = static_cast<uint8_t>(s & 0xFF);
            out[i * 3 + 1] = static_cast<uint8_t>((s >> 8) & 0xFF);
            out[i * 3 + 2] = static_cast<uint8_t>((s >> 16) & 0xFF);
        } else {
            const int16_t s = static_cast<int16_t>(r);
            std::memcpy(out + i * 2, &s, 2);
        }
    }
    return exact;
}

} // namespace ee_analyzer::pcm
