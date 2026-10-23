#pragma once
/**
 * @file eqapo_paths.hpp
 * @brief Az Equalizer APO eszközválasztójának (Configurator) helye; a motor és a felület közösen használja.
 *
 * Az Equalizer APO eszközönként működik: csak azokon a lejátszóeszközökön fut (és vele az Aqulify plugin),
 * amelyeket a Configuratorban bekapcsoltak. Új eszköznél (pl. USB hangkártya) ezért ott kell bekapcsolni.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>

namespace ee_analyzer::win {

/** Az Equalizer APO Configurator (régebbi változatban DeviceSelector) teljes útvonala, vagy üres, ha nincs. */
inline std::wstring eqApoConfiguratorPath() {
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\EqualizerAPO", L"InstallPath", RRF_RT_REG_SZ, nullptr, buf, &size) !=
        ERROR_SUCCESS)
        return {};
    std::wstring dir = buf;
    if (dir.empty()) return {};
    if (dir.back() != L'\\') dir += L'\\';
    for (const wchar_t* exe : { L"Configurator.exe", L"DeviceSelector.exe" }) {
        const std::wstring p = dir + exe;
        const DWORD attr = GetFileAttributesW(p.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) return p;
    }
    return {};
}

} // namespace ee_analyzer::win
