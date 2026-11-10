/**
 * @file firewall.hpp
 * @brief A telefonos távirányító tűzfal-engedélyei (Windows Defender tűzfal, INetFwPolicy2).
 *
 * A telepítő (rendszergazdaként) két bejövő engedélyt vesz fel, csak az Aqulify.exe-re és csak a magán- és tartományi
 * hálózatra (nyilvános Wi-Fi-re nem):
 *  - TCP 47820–47824: a telefon WebSocket-kapcsolata (ws_bridge.hpp: kRemotePortFirst, kRemotePortCount),
 *  - UDP 5353: a gépkeresés (mDNS, mdns_announcer.hpp).
 * Enélkül a Windows csak az első indításkor kérdez, és csak arra a hálózattípusra ad engedélyt, amelyiken a gép épp
 * van; ha az nyilvános volt, otthon (magánhálózaton) a telefon nem éri el a gépet. Az eltávolító a szabályokat törli.
 * A felhasználó által korábban kifejezetten tiltott (Block) szabályokhoz nem nyúl.
 */
#pragma once

#include <windows.h>
#include <oleauto.h>
#include <netfw.h>

#include <string>

namespace aether_setup {

namespace fw_detail {
// A CLSID-k és IID-k itt, hogy ne kelljen külön könyvtárat linkelni (a netfw.h a uuid könyvtárra hivatkozna)
const GUID kClsidPolicy2 = { 0xE2B3C97F, 0x6AE1, 0x41AC, { 0x81, 0x7A, 0xF6, 0xF9, 0x21, 0x66, 0xD7, 0xDD } };
const GUID kIidPolicy2 = { 0x98325047, 0xC671, 0x4174, { 0x8D, 0x81, 0xDE, 0xFC, 0xD3, 0xF0, 0x31, 0x86 } };
const GUID kClsidRule = { 0x2C5BC43E, 0x3369, 0x4C33, { 0xAB, 0x0C, 0xBE, 0x94, 0x69, 0x67, 0x7A, 0xF4 } };
const GUID kIidRule = { 0xAF230D27, 0xBABA, 0x4E42, { 0xAC, 0xED, 0xF5, 0x24, 0xF2, 0x2C, 0xFC, 0xE2 } };

constexpr const wchar_t* kRuleTcp = L"Aqulify Remote (TCP 47820-47824)";
constexpr const wchar_t* kRuleMdns = L"Aqulify Remote (mDNS, UDP 5353)";
constexpr const wchar_t* kGroup = L"Aqulify";

/** BSTR, ami a hatókör végén felszabadul. */
struct Bstr {
    BSTR s;
    explicit Bstr(const wchar_t* v) : s(SysAllocString(v)) {}
    ~Bstr() { SysFreeString(s); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
};

inline INetFwPolicy2* policy() {
    INetFwPolicy2* p = nullptr;
    if (FAILED(CoCreateInstance(kClsidPolicy2, nullptr, CLSCTX_INPROC_SERVER, kIidPolicy2, reinterpret_cast<void**>(&p))))
        return nullptr;
    return p;
}

/** Az összes ilyen nevű szabály törlése (a Remove név szerint egyet töröl, ezért addig ismételjük, amíg van). */
inline void removeByName(INetFwRules* rules, const wchar_t* name) {
    for (int guard = 0; guard < 16; ++guard) {
        INetFwRule* r = nullptr;
        Bstr n(name);
        if (FAILED(rules->Item(n.s, &r)) || !r) break;
        r->Release();
        if (FAILED(rules->Remove(n.s))) break;
    }
}

inline bool addRule(INetFwRules* rules, const wchar_t* name, const std::wstring& exe, LONG protocol, const wchar_t* ports,
                    const wchar_t* description) {
    INetFwRule* r = nullptr;
    if (FAILED(CoCreateInstance(kClsidRule, nullptr, CLSCTX_INPROC_SERVER, kIidRule, reinterpret_cast<void**>(&r))) || !r)
        return false;
    Bstr n(name), app(exe.c_str()), port(ports), group(kGroup), desc(description);
    bool ok = SUCCEEDED(r->put_Name(n.s)) && SUCCEEDED(r->put_Description(desc.s)) &&
              SUCCEEDED(r->put_ApplicationName(app.s)) && SUCCEEDED(r->put_Protocol(protocol)) &&
              SUCCEEDED(r->put_LocalPorts(port.s)) && SUCCEEDED(r->put_Direction(NET_FW_RULE_DIR_IN)) &&
              SUCCEEDED(r->put_Action(NET_FW_ACTION_ALLOW)) && SUCCEEDED(r->put_Grouping(group.s)) &&
              SUCCEEDED(r->put_Profiles(NET_FW_PROFILE2_PRIVATE | NET_FW_PROFILE2_DOMAIN)) &&
              SUCCEEDED(r->put_Enabled(VARIANT_TRUE));
    ok = ok && SUCCEEDED(rules->Add(r));
    r->Release();
    return ok;
}
} // namespace fw_detail

/**
 * A távirányító tűzfal-engedélyeinek felvétele (a korábbi saját szabályok helyére). A hívó szálon a COM-nak
 * inicializálva kell lennie. Visszatérés: mindkét szabály felkerült-e.
 */
inline bool addRemoteFirewallRules(const std::wstring& exe) {
    INetFwPolicy2* p = fw_detail::policy();
    if (!p) return false;
    INetFwRules* rules = nullptr;
    bool ok = false;
    if (SUCCEEDED(p->get_Rules(&rules)) && rules) {
        fw_detail::removeByName(rules, fw_detail::kRuleTcp);
        fw_detail::removeByName(rules, fw_detail::kRuleMdns);
        const bool tcp = fw_detail::addRule(rules, fw_detail::kRuleTcp, exe, NET_FW_IP_PROTOCOL_TCP, L"47820-47824",
                                            L"Aqulify: connection of the Aqulify Remote phone app (private and domain networks only)");
        const bool udp = fw_detail::addRule(rules, fw_detail::kRuleMdns, exe, NET_FW_IP_PROTOCOL_UDP, L"5353",
                                            L"Aqulify: the phone app finds this computer on the local network (mDNS)");
        ok = tcp && udp;
        rules->Release();
    }
    p->Release();
    return ok;
}

/** Az eltávolító: a saját szabályok törlése (a COM-nak inicializálva kell lennie). */
inline void removeRemoteFirewallRules() {
    INetFwPolicy2* p = fw_detail::policy();
    if (!p) return;
    INetFwRules* rules = nullptr;
    if (SUCCEEDED(p->get_Rules(&rules)) && rules) {
        fw_detail::removeByName(rules, fw_detail::kRuleTcp);
        fw_detail::removeByName(rules, fw_detail::kRuleMdns);
        rules->Release();
    }
    p->Release();
}

} // namespace aether_setup
