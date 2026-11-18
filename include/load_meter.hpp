#pragma once
/**
 * @file load_meter.hpp
 * @brief A motor saját processzorterhelésének mérése és szálrögzítés (dedikált mag).
 *
 * ProcessLoadMeter::sample(): a folyamat összes szálának CPU-ideje az előző mintavétel óta, a gép teljes
 * kapacitásához mérve (100 % = minden logikai mag teljesen terhelt). Linux / macOS: getrusage, Windows:
 * GetProcessTimes. Nem a hangszálon hívandó.
 *
 * pinCurrentThread(core): a hívó szál rögzítése egy logikai magra (Linux: sched_setaffinity, Windows:
 * SetThreadAffinityMask). macOS-en nem lehetséges (false).
 */
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN   // a winsock2.h később is behúzható (a régi winsock.h nem jön vele)
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/time.h>
#endif
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace ee_analyzer::load {

inline unsigned logicalCores() {
    const unsigned n = std::thread::hardware_concurrency();
    return n > 0 ? n : 1;
}

/** A folyamat eddigi CPU-ideje (felhasználói + rendszer), másodpercben. */
inline double processCpuSeconds() {
#if defined(_WIN32)
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0.0;
    auto toSec = [](const FILETIME& f) {
        return static_cast<double>((static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7;
    };
    return toSec(k) + toSec(u);
#else
    rusage r{};
    if (getrusage(RUSAGE_SELF, &r) != 0) return 0.0;
    auto tv = [](const timeval& t) { return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_usec) * 1e-6; };
    return tv(r.ru_utime) + tv(r.ru_stime);
#endif
}

class ProcessLoadMeter {
public:
    ProcessLoadMeter() : lastCpu_(processCpuSeconds()), lastWall_(std::chrono::steady_clock::now()) {}

    /** Terhelés az előző hívás óta, százalékban (0..100, a gép teljes kapacitásához). */
    double sample() {
        const double cpu = processCpuSeconds();
        const auto now = std::chrono::steady_clock::now();
        const double wall = std::chrono::duration<double>(now - lastWall_).count();
        const double pct = wall > 1e-3 ? 100.0 * (cpu - lastCpu_) / (wall * logicalCores()) : 0.0;
        lastCpu_ = cpu;
        lastWall_ = now;
        return std::clamp(pct, 0.0, 100.0);
    }

private:
    double lastCpu_;
    std::chrono::steady_clock::time_point lastWall_;
};

/** Raspberry Pi-n fut-e (Linux: /proc/device-tree/model). */
inline bool isRaspberryPi() {
#if defined(__linux__)
    FILE* f = std::fopen("/proc/device-tree/model", "rb");
    if (!f) return false;
    char buf[128] = {};
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    return std::string(buf, n).find("Raspberry Pi") != std::string::npos;
#else
    return false;
#endif
}

/** Elérhető-e a szálrögzítés ezen a platformon. */
inline bool affinitySupported() {
#if defined(__linux__) || defined(_WIN32)
    return true;
#else
    return false;
#endif
}

/** A hívó szál rögzítése egy logikai magra (core < 0: a rögzítés feloldása, minden mag). */
inline bool pinCurrentThread(int core) {
    const unsigned n = logicalCores();
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (core < 0) {
        for (unsigned i = 0; i < n && i < CPU_SETSIZE; ++i) CPU_SET(i, &set);
    } else {
        if (static_cast<unsigned>(core) >= n) return false;
        CPU_SET(core, &set);
    }
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#elif defined(_WIN32)
    DWORD_PTR mask = 0;
    if (core < 0) {
        DWORD_PTR proc = 0, sys = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys)) return false;
        mask = proc;
    } else {
        if (static_cast<unsigned>(core) >= n || core >= static_cast<int>(sizeof(DWORD_PTR) * 8)) return false;
        mask = static_cast<DWORD_PTR>(1) << core;
    }
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#else
    (void)core;
    (void)n;
    return false;
#endif
}

/** A hívó szál rögzítése visszaolvasva: ha pontosan egy magra van rögzítve, annak száma; különben -1. */
inline int currentThreadAffinitySingle() {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (pthread_getaffinity_np(pthread_self(), sizeof(set), &set) != 0) return -1;
    if (CPU_COUNT(&set) != 1) return -1;
    for (int i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &set)) return i;
    return -1;
#elif defined(_WIN32)
    // A jelenlegi maszk lekérdezése: átállítjuk a folyamat maszkjára, a visszakapott régi értéket visszaírjuk
    DWORD_PTR proc = 0, sys = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys)) return -1;
    const DWORD_PTR old = SetThreadAffinityMask(GetCurrentThread(), proc);
    if (old == 0) return -1;
    SetThreadAffinityMask(GetCurrentThread(), old);
    if (old == 0 || (old & (old - 1)) != 0) return -1;
    int idx = 0;
    for (DWORD_PTR m = old; (m & 1) == 0; m >>= 1) ++idx;
    return idx;
#else
    return -1;
#endif
}

} // namespace ee_analyzer::load
