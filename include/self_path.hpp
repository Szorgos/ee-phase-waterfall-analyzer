#pragma once
/**
 * @file self_path.hpp
 * @brief A futó program saját elérési útja (diagnosztika: a felület kiírja, ha a motor más verziójú).
 *
 * Linuxon például egyszerre lehet fent a saját telepítős (~/.local/bin) és a csomagos (/usr/bin) motor.
 * A ~/.config/systemd/user alatti szolgáltatás felülírja a csomagét, ezért ilyenkor a régi motor indul (T28).
 * Más platformon üres szöveget ad.
 */
#include <string>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace ee_analyzer {

inline std::string selfExePath() {
#if defined(__linux__)
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) return std::string(buf, static_cast<size_t>(n));
#endif
    return {};
}

} // namespace ee_analyzer
