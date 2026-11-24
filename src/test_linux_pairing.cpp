/**
 * @file test_linux_pairing.cpp
 * @brief Egységtesztek a 6. lépéshez:
 * - Linux fej nélküli (headless) vs. normál párosítási mód eldöntése (HDMI-kijelzős Pi és systemd --user esetek);
 * - localAddresses() / rankLocalAddresses() interfész-szűrés és -rangsorolás (docker0, virbr0, veth*, br-*, 169.254.*);
 * - btlink::RfcommListener::isSupported() platform-jelzés.
 */
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "bt_link.hpp"
#include "linux_headless.hpp"
#include "ws_bridge.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool cond, const std::string& label) {
    if (cond) {
        ++g_pass;
    } else {
        ++g_fail;
        std::cerr << "[HIBA] " << label << "\n";
    }
}

void testHeadlessEvaluation() {
    std::cout << "[1] Linux headless-észlelés és naplóformátum...\n";
    using namespace aq::linux_headless;

    // 1a. Semmi nincs jelen -> valóban fej nélküli Pi
    {
        Env e;
        const auto d = evaluate(e);
        check(d.headless, "kijelző és munkamenet nélkül headless=true");
        const std::string line = formatLogLine(d);
        check(line.find("Párosítás: fej nélküli mód, ok:") != std::string::npos,
              "naplósor formátuma headless módban");
    }

    // 1b. Kifejezett --headless kapcsoló még DISPLAY mellett is headless
    {
        Env e;
        e.forceHeadless = true;
        e.display = ":0";
        const auto d = evaluate(e);
        check(d.headless && d.reason.find("--headless") != std::string::npos,
              "--headless felülírja a DISPLAY-t");
    }

    // 1c. Asztali Linux: DISPLAY beállítva
    {
        Env e;
        e.display = ":0";
        const auto d = evaluate(e);
        check(!d.headless && d.reason.find("DISPLAY") != std::string::npos,
              "DISPLAY -> normál mód");
        check(formatLogLine(d).find("Párosítás: normál mód, ok:") != std::string::npos,
              "naplósor formátuma normál módban");
    }

    // 1d. Asztali Linux: WAYLAND_DISPLAY beállítva
    {
        Env e;
        e.waylandDisplay = "wayland-0";
        const auto d = evaluate(e);
        check(!d.headless && d.reason.find("WAYLAND_DISPLAY") != std::string::npos,
              "WAYLAND_DISPLAY -> normál mód");
    }

    // 1e. Asztali Linux systemd --user: DISPLAY/WAYLAND_DISPLAY üres, de Wayland socket létezik
    {
        Env e;
        e.hasWaylandSocket = true;
        const auto d = evaluate(e);
        check(!d.headless && d.reason.find("Wayland socket") != std::string::npos,
              "Wayland socket -> normál mód (systemd --user asztali gép)");
    }

    // 1f. Asztali Linux systemd --user: X11 socket létezik
    {
        Env e;
        e.hasX11Socket = true;
        const auto d = evaluate(e);
        check(!d.headless && d.reason.find("X11 socket") != std::string::npos,
              "X11 socket -> normál mód");
    }

    // 1g. Asztali munkamenet típusa (wayland / x11)
    {
        Env e;
        e.xdgSessionType = "wayland";
        const auto d = evaluate(e);
        check(!d.headless && d.reason.find("wayland") != std::string::npos,
              "XDG_SESSION_TYPE=wayland -> normál mód");
    }

    // 1h. HDMI-kijelzős Raspberry Pi: DISPLAY üres, de csatlakoztatott DRM kijelző van -> NEM headless!
    {
        Env e;
        e.hasConnectedDrmDisplay = true;
        const auto d = evaluate(e);
        check(!d.headless && d.reason.find("drm") != std::string::npos,
              "HDMI-kijelzős Pi (/sys/class/drm connected) -> normál mód");
    }

    // 1i. DRM könyvtár-olvasás valós ideiglenes mappában
    {
        const auto tmp = std::filesystem::temp_directory_path() / "aq_test_drm_check";
        std::error_code ec;
        std::filesystem::remove_all(tmp, ec);
        std::filesystem::create_directories(tmp / "card0-HDMI-A-1", ec);
        std::filesystem::create_directories(tmp / "card0", ec);
        {
            std::ofstream f(tmp / "card0-HDMI-A-1" / "status");
            f << "disconnected\n";
        }
        check(!detail::hasConnectedDrm(tmp), "DRM disconnected -> false");
        {
            std::ofstream f(tmp / "card0-HDMI-A-1" / "status");
            f << "connected\n";
        }
        check(detail::hasConnectedDrm(tmp), "DRM connected -> true");
        std::filesystem::remove_all(tmp, ec);
    }
}

void testRankLocalAddresses() {
    std::cout << "[2] localAddresses() / rankLocalAddresses() szűrés és sorrend...\n";
    using ee_analyzer::bridge_detail::InterfaceAddr;
    using ee_analyzer::bridge_detail::rankLocalAddresses;

    const std::vector<InterfaceAddr> raw = {
        { "lo", "127.0.0.1" },
        { "docker0", "172.17.0.1" },
        { "virbr0", "192.168.122.1" },
        { "veth3a9b1c", "172.18.0.2" },
        { "br-abcd1234", "172.19.0.1" },
        { "enp0s31f6", "169.254.12.34" },
        { "wlp2s0", "192.168.1.42" },
        { "enp3s0", "10.0.0.15" },
        { "wlp2s0", "192.168.1.42" },   // duplikátum
    };

    const auto out = rankLocalAddresses(raw);
    check(out.size() == 6, "127.*, 169.254.* és duplikátum kiszűrve (6 cím marad)");
    if (out.size() >= 6) {
        check(out[0] == "192.168.1.42", "wlp2s0 valódi Wi-Fi IP az 1. helyen");
        check(out[1] == "10.0.0.15", "enp3s0 valódi Ethernet IP a 2. helyen");
        // A virtuális interfészek (docker0, virbr0, veth*, br-*) a fizikaiak mögött vannak
        check(out[2] == "172.17.0.1", "docker0 hátrasorolva");
        check(out[3] == "192.168.122.1", "virbr0 hátrasorolva (még 192.168.* ellenére is!)");
        check(out[4] == "172.18.0.2", "veth* hátrasorolva");
        check(out[5] == "172.19.0.1", "br-* hátrasorolva");
    }
    for (const auto& ip : out) {
        check(ip.rfind("169.254.", 0) != 0, "169.254.* nem szerepel az eredményben");
        check(ip.rfind("127.", 0) != 0, "127.* nem szerepel az eredményben");
    }
}

void testBluetoothSupportedFlag() {
    std::cout << "[3] Bluetooth platform-támogatás jelző...\n";
#if defined(_WIN32) || defined(__linux__)
    check(ee_analyzer::btlink::RfcommListener::isSupported(), "Windowson és Linuxon (BlueZ) a Bluetooth RFCOMM támogatott");
#else
    check(!ee_analyzer::btlink::RfcommListener::isSupported(), "macOS-en a Bluetooth RFCOMM nem támogatott");
#endif
}

void testLingerWarning() {
    std::cout << "[4] Bejelentkezés nélküli futás (lingering) figyelmeztetés...\n";
    using namespace aq::linux_headless;
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "aq_linger_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    Env headlessEnv;
    const auto headless = evaluate(headlessEnv);
    Env deskEnv;
    deskEnv.display = ":0";
    const auto desktop = evaluate(deskEnv);

    // nincs linger-fájl: fej nélküli módban figyelmeztet, a felhasználónév NINCS a sorban (PII)
    const std::string w = lingerWarning(headless, "probauser", dir.string());
    check(!w.empty(), "fej nélküli mód lingering nélkül: figyelmeztetés");
    check(w.find("loginctl enable-linger $USER") != std::string::npos, "a figyelmeztetés megadja a parancsot");
    check(w.find("probauser") == std::string::npos, "a felhasználónév nem kerül a naplóba");
    // asztali gépen nincs figyelmeztetés (bejelentkezett munkamenet tartja életben)
    check(lingerWarning(desktop, "probauser", dir.string()).empty(), "normál módban nincs figyelmeztetés");
    // a linger-fájl megvan: nincs figyelmeztetés
    std::ofstream(dir / "probauser").put('\n');
    check(lingerEnabled("probauser", dir.string()), "a linger-fájl alapján bekapcsolt");
    check(lingerWarning(headless, "probauser", dir.string()).empty(), "bekapcsolt lingeringnél nincs figyelmeztetés");
    // ismeretlen felhasználó: nem figyelmeztet tévesen
    check(lingerWarning(headless, "", dir.string()).empty(), "ismeretlen felhasználónál nincs figyelmeztetés");
    check(!currentUserName().empty(), "a futó folyamat felhasználóneve kiolvasható");
    fs::remove_all(dir);
}

} // namespace

int main() {
    testHeadlessEvaluation();
    testRankLocalAddresses();
    testBluetoothSupportedFlag();
    testLingerWarning();
    std::cout << "\nEredmény: " << g_pass << " OK, " << g_fail << " HIBA.\n";
    return g_fail == 0 ? 0 : 1;
}
