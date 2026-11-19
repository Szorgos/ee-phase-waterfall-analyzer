#pragma once
/**
 * @file device_format.hpp
 * @brief Egy hangeszköz alapformátuma szövegként (beépített hibajelentés: az állapotfájl hangkártya-része).
 *
 * - describe(): WAVEFORMATEX / WAVEFORMATEXTENSIBLE bájtsor → „48000 Hz, 24 bit (32-bit container), 2 ch, PCM”.
 *   A bájtsor elején lehet a Windows regisztrációs adatbázisának 8 bájtos fejléce (VT_BLOB = 0x41, majd egy dword).
 *   Értelmezhetetlen vagy csonka bemenetre üres szöveg. Platformfüggetlen (a mezők little-endian sorrendben).
 * - readRegistry() (csak Windows): a végpont Properties kulcsának PKEY_AudioEngine_DeviceFormat értéke
 *   ({f19f064d-082c-4e27-bc73-6882a1bb8e4c},0) COM nélkül, csak olvasva. A felhasználó által átnevezhető
 *   végpontnevet nem olvassa.
 */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace aq::devfmt {

namespace detail {
inline uint32_t le16(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8); }
inline uint32_t le32(const uint8_t* p) { return le16(p) | (le16(p + 2) << 16); }
}  // namespace detail

inline std::string describe(const uint8_t* data, size_t n) {
    using detail::le16;
    using detail::le32;
    if (!data) return {};
    // a regisztrációs adatbázis fejléce: VT_BLOB (0x41) és egy dword, utána a WAVEFORMATEX(TENSIBLE)
    if (n >= 8 && le32(data) == 0x41) {
        data += 8;
        n -= 8;
    }
    if (n < 16) return {};   // a wBitsPerSample mezőig
    const uint32_t tag = le16(data), channels = le16(data + 2), rate = le32(data + 4), container = le16(data + 14);
    uint32_t bits = container;
    std::string kind;
    if (tag == 0xFFFE) {   // WAVE_FORMAT_EXTENSIBLE: cbSize ≥ 22, a valódi bitmélység és az altípus (GUID)
        if (n < 40 || le16(data + 16) < 22) return {};
        static const uint8_t kBaseGuidTail[12] = { 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 };
        const uint8_t* sub = data + 24;
        bool baseGuid = true;
        for (int i = 0; i < 12; ++i) baseGuid = baseGuid && sub[4 + i] == kBaseGuidTail[i];
        const uint32_t subTag = le32(sub);
        const uint32_t valid = le16(data + 18);
        if (valid != 0) bits = valid;
        if (baseGuid && subTag == 1) kind = "PCM";
        else if (baseGuid && subTag == 3) kind = "float";
        else {
            char b[32];
            std::snprintf(b, sizeof(b), "format 0x%04x", baseGuid ? subTag & 0xFFFFu : 0xFFFEu);
            kind = b;
        }
    } else if (tag == 1) {
        kind = "PCM";
    } else if (tag == 3) {
        kind = "float";
    } else {
        char b[32];
        std::snprintf(b, sizeof(b), "format 0x%04x", tag);
        kind = b;
    }
    if (channels == 0 || channels > 64 || rate < 1000 || rate > 10000000 || container == 0 || container > 64 ||
        bits > container)
        return {};
    char out[128];
    if (bits != container)
        std::snprintf(out, sizeof(out), "%u Hz, %u bit (%u-bit container), %u ch, %s", rate, bits, container, channels, kind.c_str());
    else
        std::snprintf(out, sizeof(out), "%u Hz, %u bit, %u ch, %s", rate, bits, channels, kind.c_str());
    return out;
}

#ifdef _WIN32
/** A végpont alapformátuma: propertiesKey pl. SOFTWARE\…\MMDevices\Audio\Render\{…}\Properties. Hiány esetén üres. */
inline std::string readRegistry(HKEY root, const std::wstring& propertiesKey) {
    uint8_t buf[256];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, propertiesKey.c_str(), L"{f19f064d-082c-4e27-bc73-6882a1bb8e4c},0", RRF_RT_REG_BINARY, nullptr, buf,
                     &size) != ERROR_SUCCESS)
        return {};
    return describe(buf, size);
}
#endif

}  // namespace aq::devfmt
