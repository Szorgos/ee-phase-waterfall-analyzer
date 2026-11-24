#pragma once
/**
 * @file crash_handler.hpp
 * @brief Összeomláskor egy rövid, helyi jel (crash.txt) a következő indítás jelentéséhez (beépített hibajelentés, R2).
 *
 * - Linux / macOS: jelzéskezelő (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT) és std::terminate.
 * - Windows: SetUnhandledExceptionFilter, a CRT SIGABRT-jelzése és std::terminate.
 * - A kezelőben CSAK aszinkron-jelzésbiztos művelet van: előre megnyitott fájlleíró, write / WriteFile, kézi
 *   számkiírás; nincs foglalás, printf, iostream, zár. (A tools/check_signal_safe.py ezt a jelölt részt ellenőrzi.)
 * - Memóriakép nincs. A kezelő után a folyamat a rendszer alapértelmezése szerint áll le.
 */
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>

#include "watchdog.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#endif

namespace aq::crash {

namespace detail {

#ifdef _WIN32
inline std::atomic<HANDLE>& fileHandle() { static std::atomic<HANDLE> h{ INVALID_HANDLE_VALUE }; return h; }
#else
inline std::atomic<int>& fileFd() { static std::atomic<int> fd{ -1 }; return fd; }
alignas(16) inline char g_altStack[64 * 1024];
#endif
inline std::atomic<aq::wd::Registry*>& registry() { static std::atomic<aq::wd::Registry*> r{ nullptr }; return r; }

// AQ_SIGNAL_SAFE_BEGIN – innentől a jelölt végéig csak aszinkron-jelzésbiztos művelet (tools/check_signal_safe.py)
inline void putRaw(const char* s, size_t n) noexcept {
#ifdef _WIN32
    const HANDLE h = fileHandle().load();
    DWORD w = 0;
    if (h != INVALID_HANDLE_VALUE) WriteFile(h, s, static_cast<DWORD>(n), &w, nullptr);
    const HANDLE e = GetStdHandle(STD_ERROR_HANDLE);
    if (e != nullptr && e != INVALID_HANDLE_VALUE) WriteFile(e, s, static_cast<DWORD>(n), &w, nullptr);
#else
    const int fd = fileFd().load();
    if (fd >= 0) { const ssize_t r = ::write(fd, s, n); (void)r; }
    const ssize_t r2 = ::write(2, s, n);
    (void)r2;
#endif
}

inline void putStr(const char* s) noexcept {
    size_t n = 0;
    while (s[n]) ++n;
    putRaw(s, n);
}

inline void putNum(unsigned long long v) noexcept {
    char b[24];
    int i = 23;
    b[i] = 0;
    do { b[--i] = static_cast<char>('0' + v % 10); v /= 10; } while (v && i > 0);
    putStr(b + i);
}

inline void putHex(unsigned long long v) noexcept {
    char b[24];
    int i = 23;
    b[i] = 0;
    do { const unsigned d = static_cast<unsigned>(v & 0xF); b[--i] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; } while (v && i > 2);
    b[--i] = 'x';
    b[--i] = '0';
    putStr(b + i);
}

inline const char* sigName(int sig) noexcept {
    switch (sig) {
        case SIGABRT: return "SIGABRT";
        case SIGSEGV: return "SIGSEGV";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
#ifndef _WIN32
        case SIGBUS: return "SIGBUS";
#endif
        default: return "?";
    }
}

inline int64_t monotonicNs() noexcept {
#ifdef _WIN32
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return static_cast<int64_t>(static_cast<double>(c.QuadPart) * 1e9 / static_cast<double>(f.QuadPart));
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
#endif
}

/** A szálak utolsó címkéje (a regiszter atomikus mezőiből, másolás és foglalás nélkül). */
inline void dumpThreads() noexcept {
    aq::wd::Registry* reg = registry().load();
    if (!reg) return;
    const int64_t now = monotonicNs();
    const aq::wd::Slot* slots = reg->slots();
    for (int i = 0; i < aq::wd::kMaxSlots; ++i) {
        const aq::wd::Slot& s = slots[i];
        if (!s.used.load(std::memory_order_acquire)) continue;
        char name[aq::wd::kNameLen];
        for (size_t k = 0; k < aq::wd::kNameLen; ++k) name[k] = s.name[k];
        name[aq::wd::kNameLen - 1] = 0;
        const char* label = s.label.load(std::memory_order_relaxed);
        const int64_t last = s.lastNs.load(std::memory_order_relaxed);
        putStr("thread=");
        putStr(name);
        putStr(" label=");
        putStr(label ? label : "");
        putStr(" age_ms=");
        if (last <= 0) putStr("-1");
        else putNum(now > last ? static_cast<unsigned long long>((now - last) / 1000000) : 0ULL);
        putStr("\n");
    }
}

inline void onSignal(int sig) noexcept {
    putStr("AQ_CRASH signal=");
    putNum(static_cast<unsigned long long>(sig));
    putStr(" (");
    putStr(sigName(sig));
    putStr(")\n");
    dumpThreads();
#ifndef _WIN32
    // SA_RESETHAND: a kezelő már az alapértelmezett; az újra kiváltott jelzés a kezelő után leállítja a folyamatot
    raise(sig);
#endif
}

inline void onTerminate() noexcept {
    putStr("AQ_CRASH terminate (uncaught exception or std::terminate)\n");
    std::abort();
}

#ifdef _WIN32
inline LONG WINAPI onException(EXCEPTION_POINTERS* ep) noexcept {
    putStr("AQ_CRASH exception=");
    putHex(ep && ep->ExceptionRecord ? static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionCode) : 0ULL);
    putStr(" address=");
    putHex(ep && ep->ExceptionRecord ? reinterpret_cast<unsigned long long>(ep->ExceptionRecord->ExceptionAddress) : 0ULL);
    putStr("\n");
    dumpThreads();
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif
// AQ_SIGNAL_SAFE_END

} // namespace detail

/**
 * A kezelők telepítése; a jel a crashFile-ba kerül (a fájl most nyílik meg, üresen). Folyamatonként egyszer
 * értelmes (a motor indításakor); a tesztek gyermekfolyamatban hívják.
 */
inline bool install(const std::filesystem::path& crashFile, aq::wd::Registry* reg = nullptr) {
    detail::registry().store(reg ? reg : &aq::wd::Registry::global());
#ifdef _WIN32
    const HANDLE h = CreateFileW(crashFile.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const HANDLE old = detail::fileHandle().exchange(h);
    if (old != INVALID_HANDLE_VALUE) CloseHandle(old);
    SetUnhandledExceptionFilter(&detail::onException);
    std::signal(SIGABRT, [](int sig) { detail::onSignal(sig); });
#else
    const int fd = ::open(crashFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const int old = detail::fileFd().exchange(fd);
    if (old >= 0) ::close(old);
    stack_t ss{};
    ss.ss_sp = detail::g_altStack;
    ss.ss_size = sizeof(detail::g_altStack);
    sigaltstack(&ss, nullptr);   // verem-túlcsordulás a fő szálon: a kezelő a saját vermén fut
    struct sigaction sa{};
    sa.sa_handler = [](int sig) { detail::onSignal(sig); };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND | SA_ONSTACK;
    for (int sig : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT }) sigaction(sig, &sa, nullptr);
#endif
    std::set_terminate(&detail::onTerminate);
    return true;
}

/** Rendes leálláskor: a jelfájl lezárása (egy későbbi jelzés már nem ír bele). */
inline void disarm() {
#ifdef _WIN32
    const HANDLE h = detail::fileHandle().exchange(INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
#else
    const int fd = detail::fileFd().exchange(-1);
    if (fd >= 0) ::close(fd);
#endif
}

} // namespace aq::crash
