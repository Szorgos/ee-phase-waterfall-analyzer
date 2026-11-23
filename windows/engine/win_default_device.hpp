#pragma once
/**
 * @file win_default_device.hpp
 * @brief A Windows alapértelmezett lejátszóeszközének lekérdezése és átállítása (a rendszerhang-hídhoz).
 *
 * A „Minden hang” felmintavételezésnél a programok a virtuális kábelre szólnak, az Aqulify onnan viszi a hangot
 * a valódi DAC-ra. Ha a híd leáll (kikapcsolás, kilépés), az alapértelmezett eszközt vissza kell állítani a DAC-ra,
 * különben a hang a kábelben elvész. A Windows ehhez nem ad hivatalos felületet; az IPolicyConfig COM-felületet
 * használjuk (Windows 7 óta változatlan, a hangeszköz-váltó programok is ezt hívják). Ha nem érhető el, a hívás
 * egyszerűen false-szal tér vissza, és minden marad a régiben.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <cstdio>
#include <string>

#include "wasapi_loopback.hpp"

namespace ee_analyzer::win {

namespace policy_detail {

// CLSID_PolicyConfigClient és IID_IPolicyConfig (Windows 7+)
static const CLSID kCLSID_PolicyConfigClient = {0x870AF99C, 0x171D, 0x4F9E, {0xAF, 0x0D, 0xE6, 0x3D, 0xF4, 0x0C, 0x2B, 0xC9}};
static const IID kIID_IPolicyConfig = {0xF8679F50, 0x850A, 0x41CF, {0x9C, 0x72, 0x43, 0x0F, 0x29, 0x02, 0x90, 0xC8}};

/** Csak a vtábla sorrendje számít: a SetDefaultEndpoint a 11. saját metódus (az IUnknown után). */
struct IPolicyConfigAq : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR deviceId, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

struct ComScope {
    bool uninit = false;
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        uninit = (hr == S_OK || hr == S_FALSE);
    }
    ~ComScope() { if (uninit) CoUninitialize(); }
};

} // namespace policy_detail

/** A Windows mostani alapértelmezett lejátszóeszközének azonosítója (üres, ha nincs). */
inline std::wstring getDefaultRenderDeviceId() {
    policy_detail::ComScope com;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator,
                                reinterpret_cast<void**>(en.put()))))
        return {};
    ComPtr<IMMDevice> dev;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.put())) || !dev) return {};
    LPWSTR id = nullptr;
    std::wstring out;
    if (SUCCEEDED(dev->GetId(&id)) && id) {
        out = id;
        CoTaskMemFree(id);
    }
    return out;
}

/** Az alapértelmezett lejátszóeszköz átállítása mindhárom szerepre (konzol, multimédia, kommunikáció). */
inline bool setDefaultRenderDeviceId(const std::wstring& id) {
    if (id.empty()) return false;
    policy_detail::ComScope com;
    ComPtr<policy_detail::IPolicyConfigAq> pc;
    if (FAILED(CoCreateInstance(policy_detail::kCLSID_PolicyConfigClient, nullptr, CLSCTX_ALL,
                                policy_detail::kIID_IPolicyConfig, reinterpret_cast<void**>(pc.put()))) || !pc)
        return false;
    bool ok = true;
    for (ERole r : {eConsole, eMultimedia, eCommunications})
        ok = SUCCEEDED(pc->SetDefaultEndpoint(id.c_str(), r)) && ok;
    return ok;
}

/**
 * A rendszerhang-híd alapértelmezett-eszköz döntései (tiszta logika, platformfüggetlenül tesztelhető).
 *
 * engaged: a híd átvette a rendszer kimenetét (a programok a kábelre szólnak, a visszaállítandó eszköz ismert).
 */
struct DefaultDeviceState {
    bool engaged = false;
    std::wstring restoreId;   // a valódi eszköz, amelyre kikapcsoláskor visszaállunk
    std::wstring cableId;     // a kábel, amelyre átállítottuk
};

/** Teendő egy figyelési körben. */
struct DefaultDeviceAction {
    bool setDefault = false;      // állítsuk át az alapértelmezett eszközt erre: target
    std::wstring target;
    bool stateChanged = false;    // a megőrzendő állapot változott (fájlba írás)
};

/**
 * Bekapcsolt híd: az alapértelmezett eszköz legyen a kábel; az előző valódi eszközt megjegyezzük.
 * Ha a felhasználó közben másik valódi eszközt választott, az lesz a visszaállítási cél (és a híd kimenete),
 * a rendszer kimenete pedig visszaáll a kábelre (különben a hang feldolgozás nélkül szólna).
 */
inline DefaultDeviceAction planEngage(DefaultDeviceState& st, const std::wstring& currentDefault,
                                      bool currentIsVirtual, const std::wstring& cableId,
                                      const std::wstring& fallbackOutputId) {
    DefaultDeviceAction a;
    if (!currentDefault.empty() && !currentIsVirtual && currentDefault != cableId) {
        if (st.restoreId != currentDefault) { st.restoreId = currentDefault; a.stateChanged = true; }
    } else if (st.restoreId.empty() && !fallbackOutputId.empty()) {
        st.restoreId = fallbackOutputId;   // a felhasználó kézzel állította a kábelre: a híd kimenete a visszaállítási cél
        a.stateChanged = true;
    }
    if (!st.engaged || st.cableId != cableId) { st.engaged = true; st.cableId = cableId; a.stateChanged = true; }
    if (currentDefault != cableId) { a.setDefault = true; a.target = cableId; }
    return a;
}

/** Kikapcsolt híd: ha a rendszer még a kábelre szól, visszaállítjuk a megjegyzett valódi eszközre. */
inline DefaultDeviceAction planRelease(DefaultDeviceState& st, const std::wstring& currentDefault,
                                       bool currentIsVirtual) {
    DefaultDeviceAction a;
    if (!st.engaged) return a;
    if (!st.restoreId.empty() && currentIsVirtual && currentDefault != st.restoreId) {
        a.setDefault = true;
        a.target = st.restoreId;
    }
    st = DefaultDeviceState{};
    a.stateChanged = true;
    return a;
}

/** Megőrzött állapot (összeomlás vagy kilőtt folyamat után a következő indulás visszaállíthatja). */
inline bool saveDefaultDeviceState(const std::wstring& path, const DefaultDeviceState& st) {
    if (path.empty()) return false;
    if (!st.engaged) return DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const std::string r = WasapiLoopback::wideToUtf8(st.restoreId.c_str());
    const std::string c = WasapiLoopback::wideToUtf8(st.cableId.c_str());
    std::fprintf(f, "%s\n%s\n", r.c_str(), c.c_str());
    std::fclose(f);
    return true;
}

inline DefaultDeviceState loadDefaultDeviceState(const std::wstring& path) {
    DefaultDeviceState st;
    if (path.empty()) return st;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return st;
    char buf[2048];
    std::string lines[2];
    for (auto& l : lines) {
        if (!std::fgets(buf, sizeof(buf), f)) break;
        l = buf;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
    }
    std::fclose(f);
    st.restoreId = WasapiLoopback::utf8ToWide(lines[0]);
    st.cableId = WasapiLoopback::utf8ToWide(lines[1]);
    st.engaged = !st.restoreId.empty() && !st.cableId.empty();
    return st;
}

} // namespace ee_analyzer::win
