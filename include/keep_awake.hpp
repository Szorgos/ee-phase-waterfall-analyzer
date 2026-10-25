#pragma once
/**
 * @file keep_awake.hpp
 * @brief Ébren tartás a burn-in alatt (Windows): a rendszer ne aludjon el, a kijelző elalhat.
 *
 * A Windows-os felület (windows/gui) ezt a hívást kéri a burn-in indításakor és leállításakor. Linuxon a
 * systemd-inhibit zár szolgál erre (src/aether_gui_linux.cpp), macOS-en nincs ébren tartás.
 */
#ifdef _WIN32
#include <windows.h>
#endif

namespace ee_analyzer {
namespace keepawake {

/** true: a rendszer ne aludjon el (ES_SYSTEM_REQUIRED); false: visszaáll az alapértelmezett viselkedés.
 *  @return a Windows hívás sikere (nem Windows-on mindig false) */
inline bool set(bool on) {
#ifdef _WIN32
    return SetThreadExecutionState(on ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED) : ES_CONTINUOUS) != 0;
#else
    (void)on;
    return false;
#endif
}

} // namespace keepawake
} // namespace ee_analyzer
