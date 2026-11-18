#pragma once
/**
 * @file apo_detect.hpp
 * @brief Be van-e kapcsolva az Equalizer APO egy Windows-lejátszóeszközön (csak olvas a regisztrációs adatbázisból).
 *
 * Az Equalizer APO eszközönként települ: az eszköz `FxProperties` kulcsába a saját APO-osztályainak azonosítóit írja.
 * Ha a hang olyan eszközön szól, amelyen nincs bekapcsolva (pl. új USB hangkártya, más USB-port), az Aqulify DSP-je
 * ott nem hat, és a plugin csak egy másik eszközön fut. A felismerés nem égeti be az azonosítókat: az eszköz
 * `FxProperties` értékei közül azt keresi, amelynek az osztályneve (HKLM\SOFTWARE\Classes\CLSID\{…}) „EqualizerAPO”
 * kezdetű. Ha a kulcs nem olvasható, az eredmény „ismeretlen”: ilyenkor nincs figyelmeztetés (téves riasztás helyett).
 */
#include <string>
#include <vector>

#include "dsp_state.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace ee_analyzer::win {

/**
 * A WASAPI-eszközazonosítóból (pl. "{0.0.0.00000000}.{56869ca8-…}") a regisztrációs kulcs neve ("{56869ca8-…}").
 * Üres, ha az azonosító nem ilyen alakú.
 */
inline std::wstring endpointKeyFromId(const std::wstring& id) {
    const size_t dot = id.rfind(L"}.{");
    if (dot == std::wstring::npos) return {};
    std::wstring key = id.substr(dot + 2);
    if (key.size() != 38 || key.front() != L'{' || key.back() != L'}') return {};
    return key;
}

/** Az osztálynév az Equalizer APO egyik APO-osztálya-e ("EqualizerAPO Pre-Mix Class", "EqualizerAPO Post-Mix Class" …). */
inline bool isEqApoClassName(const std::wstring& name) {
    static const std::wstring kPrefix = L"EqualizerAPO";
    return name.size() >= kPrefix.size() && name.compare(0, kPrefix.size(), kPrefix) == 0;
}

#ifdef _WIN32
namespace apo_detail {
inline const wchar_t* renderBase() { return L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\"; }

inline std::wstring regString(HKEY root, const std::wstring& sub, const wchar_t* value) {
    wchar_t buf[512] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, sub.c_str(), value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

/** Az eszköz FxProperties kulcsában van-e Equalizer APO osztály. Unknown, ha a kulcs nem olvasható. */
inline ApoOnDevice fxHasEqApo(const std::wstring& endpointKey) {
    HKEY fx = nullptr;
    const std::wstring path = std::wstring(renderBase()) + endpointKey + L"\\FxProperties";
    const LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &fx);
    if (rc == ERROR_FILE_NOT_FOUND) return ApoOnDevice::No;   // nincs effektus-beállítás: Equalizer APO sincs rajta
    if (rc != ERROR_SUCCESS) return ApoOnDevice::Unknown;
    ApoOnDevice out = ApoOnDevice::No;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD nameLen = 256, type = 0;
        wchar_t data[128] = {};
        DWORD dataLen = sizeof(data);
        const LONG e = RegEnumValueW(fx, i, name, &nameLen, nullptr, &type, reinterpret_cast<BYTE*>(data), &dataLen);
        if (e == ERROR_NO_MORE_ITEMS) break;
        if (e != ERROR_SUCCESS || type != REG_SZ) continue;
        const std::wstring clsid(data);
        if (clsid.size() != 38 || clsid.front() != L'{') continue;
        const std::wstring cls = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\" + clsid, nullptr);
        if (isEqApoClassName(cls)) { out = ApoOnDevice::Yes; break; }
    }
    RegCloseKey(fx);
    return out;
}
}  // namespace apo_detail

/** Be van-e kapcsolva az Equalizer APO a WASAPI-azonosítóval megadott lejátszóeszközön. */
inline ApoOnDevice apoOnRenderDevice(const std::wstring& deviceId) {
    const std::wstring key = endpointKeyFromId(deviceId);
    if (key.empty()) return ApoOnDevice::Unknown;
    return apo_detail::fxHasEqApo(key);
}

struct RenderDeviceInfo {
    std::wstring key;      // regisztrációs kulcs ("{…}")
    std::string name;      // pl. "Hangszórók (Realtek(R) Audio)" – a Windows hangbeállításaiban látható név (UTF-8)
    bool active = false;   // csatlakoztatva és engedélyezve
    ApoOnDevice apo = ApoOnDevice::Unknown;
};

/** Az összes lejátszóeszköz a regisztrációs adatbázisból (név, állapot, Equalizer APO). */
inline std::vector<RenderDeviceInfo> listRenderDevices() {
    std::vector<RenderDeviceInfo> out;
    HKEY base = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, apo_detail::renderBase(), 0, KEY_READ | KEY_WOW64_64KEY, &base) != ERROR_SUCCESS)
        return out;
    for (DWORD i = 0;; ++i) {
        wchar_t sub[64];
        DWORD subLen = 64;
        if (RegEnumKeyExW(base, i, sub, &subLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        RenderDeviceInfo d;
        d.key = sub;
        const std::wstring devPath = std::wstring(apo_detail::renderBase()) + d.key;
        DWORD state = 0, size = sizeof(state);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, devPath.c_str(), L"DeviceState", RRF_RT_REG_DWORD, nullptr, &state, &size) ==
            ERROR_SUCCESS)
            d.active = (state == 1);   // DEVICE_STATE_ACTIVE
        const std::wstring props = devPath + L"\\Properties";
        // PKEY_Device_DeviceDesc ("Hangszórók") és PKEY_DeviceInterface_FriendlyName ("Realtek(R) Audio")
        const std::wstring desc = apo_detail::regString(HKEY_LOCAL_MACHINE, props, L"{a45c254e-df1c-4efd-8020-67d146a850e0},2");
        const std::wstring iface = apo_detail::regString(HKEY_LOCAL_MACHINE, props, L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6");
        const std::wstring wname = iface.empty() ? desc : desc + L" (" + iface + L")";
        const int n = WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            d.name.assign(static_cast<size_t>(n - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1, d.name.data(), n, nullptr, nullptr);
        }
        d.apo = apo_detail::fxHasEqApo(d.key);
        out.push_back(std::move(d));
    }
    RegCloseKey(base);
    return out;
}

/** Az első csatlakoztatott lejátszóeszköz neve, amelyen be van kapcsolva az Equalizer APO (üres, ha nincs ilyen). */
inline std::string firstActiveApoDeviceName() {
    for (const auto& d : listRenderDevices())
        if (d.active && d.apo == ApoOnDevice::Yes && !d.name.empty()) return d.name;
    return {};
}
#endif

}  // namespace ee_analyzer::win
