#pragma once
/**
 * @file aq_i18n.hpp
 * @brief Fordítások a natív részekhez (Windows tálcamenü, üzenetablakok, telepítő; Linux ablak).
 *
 * A szövegek az i18n/native/<nyelv>.json fájlokban vannak; a tools/i18n_build.py ezekből állítja elő
 * az aq_i18n_strings.hpp táblát. A nyelv: a felületen választott (language.txt), különben a rendszeré;
 * nem támogatott nyelvnél angol.
 *
 *   aq_i18n::init();                                     // induláskor
 *   aq_i18n::tr(aq_i18n::Msg::tray_open)                 // UTF-8 szöveg
 *   aq_i18n::fmt(aq_i18n::Msg::tray_source, {{"src", s}}) // {változó} helyettesítéssel
 */
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <sys/stat.h>
#endif

#include "aq_i18n_strings.hpp"

namespace aq_i18n {

/** A nyelvkód indexe a táblában (pl. "hu" -> 0), vagy -1. */
inline int indexOf(const std::string& code) {
    for (int i = 0; i < kLangCount; ++i)
        if (code == kLangCodes[i]) return i;
    return -1;
}

/** "pt_BR.UTF-8", "zh-Hans-CN", "en_US" -> "pt", "zh", "en"; nem támogatott nyelvnél üres. */
inline std::string normalize(const std::string& locale) {
    std::string c;
    for (char ch : locale) {
        if (ch == '_' || ch == '-' || ch == '.' || ch == '@' || ch == ':' || std::isspace(static_cast<unsigned char>(ch))) {
            if (!c.empty()) break;
            continue;
        }
        c += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return indexOf(c) >= 0 ? c : std::string();
}

inline std::atomic<int>& currentIndex() {
    static std::atomic<int> idx{ indexOf("en") };
    return idx;
}

/** Nyelv beállítása kód vagy locale alapján; nem támogatottnál nem változik. */
inline bool set(const std::string& codeOrLocale) {
    const int i = indexOf(normalize(codeOrLocale));
    if (i < 0) return false;
    currentIndex().store(i);
    return true;
}

inline std::string code() { return kLangCodes[currentIndex().load()]; }

inline const char* tr(Msg m) { return kStrings[currentIndex().load()][static_cast<int>(m)]; }

/** Szöveg {név} helyettesítéssel. */
inline std::string fmt(Msg m, std::initializer_list<std::pair<const char*, std::string>> vars) {
    std::string s = tr(m);
    for (const auto& [name, value] : vars) {
        const std::string key = std::string("{") + name + "}";
        for (size_t p = s.find(key); p != std::string::npos; p = s.find(key, p + value.size()))
            s.replace(p, key.size(), value);
    }
    return s;
}

#ifdef _WIN32
/** A Windows megjelenítési nyelve (Beállítások → Idő és nyelv). */
inline std::string systemLanguage() {
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_HUNGARIAN: return "hu";
        case LANG_GERMAN: return "de";
        case LANG_SPANISH: return "es";
        case LANG_FRENCH: return "fr";
        case LANG_PORTUGUESE: return "pt";
        case LANG_ITALIAN: return "it";
        case LANG_POLISH: return "pl";
        case LANG_RUSSIAN: return "ru";
        case LANG_UKRAINIAN: return "uk";
        case LANG_CZECH: return "cs";
        case LANG_DUTCH: return "nl";
        case LANG_TURKISH: return "tr";
        case LANG_CHINESE: return "zh";
        case LANG_JAPANESE: return "ja";
        case LANG_KOREAN: return "ko";
        default: return "en";
    }
}

/** %LOCALAPPDATA%\Aqulify\language.txt */
inline std::wstring settingsFile(bool createDir = false) {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p) {
        dir = std::wstring(p) + L"\\Aqulify";
        CoTaskMemFree(p);
    } else {
        return {};
    }
    if (createDir) CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\language.txt";
}

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
inline std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
inline std::wstring trw(Msg m) { return widen(tr(m)); }
inline std::wstring fmtw(Msg m, std::initializer_list<std::pair<const char*, std::string>> vars) { return widen(fmt(m, vars)); }
#else
/** A munkamenet nyelve (LANGUAGE, LC_ALL, LC_MESSAGES, LANG). */
inline std::string systemLanguage() {
    for (const char* var : { "LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG" }) {
        const char* v = std::getenv(var);
        if (!v || !*v) continue;
        const std::string n = normalize(v);
        if (!n.empty()) return n;
        if (std::string(v) != "C" && std::string(v) != "POSIX") return "en";
    }
    return "en";
}

/** $XDG_CONFIG_HOME/aqulify/language.txt (alapból ~/.config/aqulify/language.txt) */
inline std::string settingsFile(bool createDir = false) {
    std::string dir;
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) dir = x;
    else if (const char* h = std::getenv("HOME"); h && *h) dir = std::string(h) + "/.config";
    else return {};
    if (createDir) mkdir(dir.c_str(), 0755);
    dir += "/aqulify";
    if (createDir) mkdir(dir.c_str(), 0755);
    return dir + "/language.txt";
}
#endif

/** A felhasználó által választott nyelv (language.txt), vagy üres, ha még nem választott. */
inline std::string savedLanguage() {
    const auto path = settingsFile();
    if (path.empty()) return {};
    std::ifstream f{ std::filesystem::path(path) };
    std::string line;
    if (!f || !std::getline(f, line)) return {};
    return normalize(line);
}

/** A választott nyelv mentése (a motor, a felület ablaka és a telepítő is ezt olvassa). */
inline bool saveLanguage(const std::string& code) {
    const std::string n = normalize(code);
    if (n.empty()) return false;
    const auto path = settingsFile(true);
    if (path.empty()) return false;
    std::ofstream f(std::filesystem::path(path), std::ios::trunc);
    if (!f) return false;
    f << n << "\n";
    return static_cast<bool>(f);
}

/** Induláskor: a választott nyelv, ha nincs, a rendszeré. */
inline void init() {
    const std::string saved = savedLanguage();
    if (saved.empty() || !set(saved)) set(systemLanguage());
}

}  // namespace aq_i18n
