#pragma once
/**
 * @file linux_headless.hpp
 * @brief Linux fej nélküli (headless) mód eldöntése a párosításhoz.
 *
 * Szabály:
 * - Kifejezett --headless kapcsolóval mindig fej nélküli mód.
 * - Ha van aktív grafikus kijelző vagy asztali munkamenet (DISPLAY, WAYLAND_DISPLAY,
 *   XDG_CURRENT_DESKTOP, XDG_SESSION_TYPE=wayland|x11, Wayland/X11 socket, vagy
 *   csatlakoztatott DRM/HDMI/eDP kijelző), akkor normál (asztali / HDMI-kijelzős Pi) mód.
 * - Csak akkor fej nélküli (claimable), ha egyik sincs jelen.
 */
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace aq::linux_headless {

struct Env {
    bool forceHeadless = false;
    std::string display;
    std::string waylandDisplay;
    std::string xdgCurrentDesktop;
    std::string xdgSessionType;
    bool hasWaylandSocket = false;
    bool hasX11Socket = false;
    bool hasConnectedDrmDisplay = false;
};

struct Decision {
    bool headless = false;
    std::string reason;
};

/** Tiszta, determinisztikus döntés (egységtesztelhető, nem nyúl a rendszerhez). */
inline Decision evaluate(const Env& env) {
    if (env.forceHeadless) {
        return { true, "--headless kapcsoló" };
    }
    if (!env.display.empty()) {
        return { false, "DISPLAY beállítva" };
    }
    if (!env.waylandDisplay.empty()) {
        return { false, "WAYLAND_DISPLAY beállítva" };
    }
    if (!env.xdgCurrentDesktop.empty()) {
        return { false, "XDG_CURRENT_DESKTOP beállítva" };
    }
    if (env.xdgSessionType == "wayland" || env.xdgSessionType == "x11") {
        return { false, "asztali munkamenet (" + env.xdgSessionType + ")" };
    }
    if (env.hasWaylandSocket) {
        return { false, "Wayland socket elérhető" };
    }
    if (env.hasX11Socket) {
        return { false, "X11 socket elérhető" };
    }
    if (env.hasConnectedDrmDisplay) {
        return { false, "csatlakoztatott kijelző (/sys/class/drm)" };
    }
    return { true, "nincs aktív grafikus kijelző vagy munkamenet" };
}

namespace detail {

inline std::string envStr(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

inline bool dirHasPrefix(const std::filesystem::path& dir, const std::string& prefix) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) return false;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) == 0 && name.size() > prefix.size()) return true;
    }
    return false;
}

/** Van-e csatlakoztatott (HDMI/DP/eDP/LVDS) kijelző a /sys/class/drm alatt. */
inline bool hasConnectedDrm(const std::filesystem::path& drmRoot = "/sys/class/drm") {
    std::error_code ec;
    if (!std::filesystem::exists(drmRoot, ec) || !std::filesystem::is_directory(drmRoot, ec)) return false;
    for (const auto& entry : std::filesystem::directory_iterator(drmRoot, ec)) {
        if (ec) break;
        const std::string name = entry.path().filename().string();
        // Csak a csatlakozók: pl. card0-HDMI-A-1, card1-eDP-1 (a puszta card0 nem csatlakozó)
        if (name.rfind("card", 0) != 0 || name.find('-') == std::string::npos) continue;
        std::ifstream f(entry.path() / "status");
        std::string status;
        if (std::getline(f, status)) {
            while (!status.empty() && (status.back() == '\r' || status.back() == '\n' || status.back() == ' '))
                status.pop_back();
            if (status == "connected") return true;
        }
    }
    return false;
}

} // namespace detail

/** A futó Linux rendszer vizsgálata. */
inline Decision detect(bool forceHeadless = false) {
    Env e;
    e.forceHeadless = forceHeadless;
    e.display = detail::envStr("DISPLAY");
    e.waylandDisplay = detail::envStr("WAYLAND_DISPLAY");
    e.xdgCurrentDesktop = detail::envStr("XDG_CURRENT_DESKTOP");
    e.xdgSessionType = detail::envStr("XDG_SESSION_TYPE");
    const std::string runtimeDir = detail::envStr("XDG_RUNTIME_DIR");
    if (!runtimeDir.empty()) {
        e.hasWaylandSocket = detail::dirHasPrefix(runtimeDir, "wayland-");
    }
    e.hasX11Socket = detail::dirHasPrefix("/tmp/.X11-unix", "X");
    e.hasConnectedDrmDisplay = detail::hasConnectedDrm();
    return evaluate(e);
}

/** Egysoros, PII-mentes naplóüzenet formázása. */
inline std::string formatLogLine(const Decision& d) {
    return std::string("[Párosítás] Párosítás: ") +
           (d.headless ? "fej nélküli mód" : "normál mód") +
           ", ok: " + d.reason;
}

} // namespace aq::linux_headless
