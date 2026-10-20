#pragma once
/**
 * @file install_ops.hpp
 * @brief Az Aqulify telepítő műveletei (felhasználói felülettől független rész).
 *
 *  - beágyazott fájlok kicsomagolása
 *  - Equalizer APO felismerése, letöltése, a plugin bemásolása és beillesztése a config.txt-be (Include:)
 *  - parancsikonok, automatikus indítás, "Programok és szolgáltatások" bejegyzés
 *  - mindezek visszavonása eltávolításkor
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shldisp.h>
#include <exdisp.h>
#include <shlguid.h>
#include <winhttp.h>
#include <tlhelp32.h>
#include <cwctype>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace aether_setup {

constexpr const wchar_t* kAppName = L"Aqulify";
#ifndef AQULIFY_VERSION
#define AQULIFY_VERSION "0.0.0"
#endif
#define AQ_WIDEN2(x) L##x
#define AQ_WIDEN(x) AQ_WIDEN2(x)
constexpr const wchar_t* kVersion = AQ_WIDEN(AQULIFY_VERSION);
constexpr const wchar_t* kPublisher = L"Aqulify (Sass Gergő)";
constexpr const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Aqulify";
constexpr const wchar_t* kAppKey = L"Software\\Aqulify";
constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kEngineWindowClass = L"AqulifyEngineWnd";
constexpr const wchar_t* kEqApoKey = L"SOFTWARE\\EqualizerAPO";
constexpr const wchar_t* kIncludeLine = L"Include: Aqulify.txt";
constexpr const wchar_t* kIncludeComment = L"# Aqulify (a telepítő adta hozzá, eltávolításkor törlődik)";
constexpr const wchar_t* kEqApoDownloadUrls[] = {
    L"https://sourceforge.net/projects/equalizerapo/files/latest/download",
    L"https://sourceforge.net/projects/equalizerapo/files/1.4.2/EqualizerAPO-x64-1.4.2.exe/download",
};
constexpr const wchar_t* kEqApoPage = L"https://sourceforge.net/projects/equalizerapo/";

using Logger = std::function<void(const std::wstring&)>;

// ------------------------------------------------------------------ általános segédek

inline std::wstring joinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

inline std::wstring parentDir(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s);
}

inline bool fileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool dirExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool createDirs(const std::wstring& p) {
    return SHCreateDirectoryExW(nullptr, p.c_str(), nullptr) == ERROR_SUCCESS || dirExists(p);
}

inline std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
    if (p) CoTaskMemFree(p);
    return out;
}

inline std::wstring regReadString(HKEY root, const wchar_t* key, const wchar_t* value, REGSAM view = KEY_WOW64_64KEY) {
    HKEY k;
    if (RegOpenKeyExW(root, key, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return {};
    wchar_t buf[2048];
    DWORD size = sizeof(buf), type = 0;
    std::wstring out;
    if (RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        out.assign(buf, size / sizeof(wchar_t));
        while (!out.empty() && out.back() == L'\0') out.pop_back();
    }
    RegCloseKey(k);
    return out;
}

inline bool regWriteString(HKEY root, const wchar_t* key, const wchar_t* name, const std::wstring& v) {
    HKEY k;
    if (RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    const LONG r = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                                  static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

inline bool regWriteDword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD v) {
    HKEY k;
    if (RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    const LONG r = RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

inline void regDeleteValue(HKEY root, const wchar_t* key, const wchar_t* name) {
    HKEY k;
    if (RegOpenKeyExW(root, key, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, name);
        RegCloseKey(k);
    }
}

inline void regDeleteTree(HKEY root, const wchar_t* key) {
    RegDeleteTreeW(root, key);
    RegDeleteKeyExW(root, key, KEY_WOW64_64KEY, 0);
}

inline bool readFileBytes(const std::wstring& path, std::vector<char>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(f, &sz);
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD r = 0;
    const bool ok = out.empty() || (ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &r, nullptr) && r == out.size());
    CloseHandle(f);
    return ok;
}

inline bool writeFileBytes(const std::wstring& path, const void* data, size_t len) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = len == 0 || (WriteFile(f, data, static_cast<DWORD>(len), &w, nullptr) && w == len);
    CloseHandle(f);
    return ok;
}

/**
 * Fájl cseréje akkor is, ha egy futó folyamat (pl. audiodg.exe) betöltve tartja:
 * a zárolt fájl átnevezhető, így a régi példány újraindításig "félreáll", az új a helyére kerül.
 */
inline bool replaceFile(const std::wstring& dst, const void* data, size_t len, Logger log) {
    if (writeFileBytes(dst, data, len)) return true;
    if (!fileExists(dst)) return false;
    const std::wstring aside = dst + L".old-" + std::to_wstring(GetTickCount64());
    if (!MoveFileExW(dst.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
    MoveFileExW(aside.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    if (log) log(L"A régi fájl használatban volt, félretéve (újraindításkor törlődik): " + aside);
    return writeFileBytes(dst, data, len);
}

/** Fájl törlése; ha zárolt, újraindításkori törlésre ütemezi. */
inline void deleteFileOrSchedule(const std::wstring& p) {
    if (!fileExists(p)) return;
    if (DeleteFileW(p.c_str())) return;
    const std::wstring aside = p + L".del-" + std::to_wstring(GetTickCount64());
    if (MoveFileExW(p.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(aside.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    } else {
        MoveFileExW(p.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
}

inline void deleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(joinPath(dir, L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring p = joinPath(dir, name);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) deleteTree(p);
            else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                deleteFileOrSchedule(p);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!RemoveDirectoryW(dir.c_str())) MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

// ------------------------------------------------------------------ beágyazott fájlok

struct PayloadFile {
    std::wstring resName;
    std::wstring relPath;
};

inline std::vector<PayloadFile> payloadIndex() {
    std::vector<PayloadFile> out;
    HRSRC r = FindResourceW(nullptr, L"PAYLOAD_INDEX", MAKEINTRESOURCEW(10)); // RT_RCDATA
    if (!r) return out;
    const char* data = static_cast<const char*>(LockResource(LoadResource(nullptr, r)));
    const DWORD size = SizeofResource(nullptr, r);
    std::string text(data, size);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t bar = line.find('|');
        if (bar == std::string::npos) continue;
        auto widen = [](const std::string& s) {
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
            return w;
        };
        out.push_back({ widen(line.substr(0, bar)), widen(line.substr(bar + 1)) });
    }
    return out;
}

inline bool payloadData(const std::wstring& resName, const void*& data, size_t& size) {
    HRSRC r = FindResourceW(nullptr, resName.c_str(), MAKEINTRESOURCEW(10));
    if (!r) return false;
    data = LockResource(LoadResource(nullptr, r));
    size = SizeofResource(nullptr, r);
    return data != nullptr;
}

// ------------------------------------------------------------------ Equalizer APO

struct EqApoInfo {
    bool installed = false;
    std::wstring installPath;
    std::wstring configPath;
};

inline EqApoInfo detectEqApo() {
    EqApoInfo info;
    info.installPath = regReadString(HKEY_LOCAL_MACHINE, kEqApoKey, L"InstallPath");
    info.configPath = regReadString(HKEY_LOCAL_MACHINE, kEqApoKey, L"ConfigPath");
    if (info.installPath.empty()) {
        const std::wstring pf = knownFolder(FOLDERID_ProgramFiles);
        if (fileExists(joinPath(pf, L"EqualizerAPO\\Editor.exe"))) info.installPath = joinPath(pf, L"EqualizerAPO");
    }
    if (!info.installPath.empty() && info.configPath.empty()) info.configPath = joinPath(info.installPath, L"config");
    info.installed = !info.installPath.empty() && dirExists(info.installPath);
    return info;
}

/** HTTPS letöltés WinHTTP-vel (átirányítások követése). */
inline bool downloadFile(const std::wstring& url, const std::wstring& dest, std::function<void(uint64_t, uint64_t)> progress,
                         const bool* cancel) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;

    HINTERNET ses = WinHttpOpen(L"Wget/1.21.4 (Aqulify-Setup)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) ses = WinHttpOpen(L"Wget/1.21.4 (Aqulify-Setup)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    bool ok = false;
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &len, nullptr);
        uint64_t total = 0;
        wchar_t cl[64];
        DWORD clLen = sizeof(cl);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, cl, &clLen, nullptr)) total = _wcstoui64(cl, nullptr, 10);
        if (status == 200) {
            HANDLE f = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                std::vector<char> buf(64 * 1024);
                uint64_t got = 0;
                ok = true;
                for (;;) {
                    if (cancel && *cancel) { ok = false; break; }
                    DWORD n = 0;
                    if (!WinHttpReadData(req, buf.data(), static_cast<DWORD>(buf.size()), &n)) { ok = false; break; }
                    if (n == 0) break;
                    DWORD w = 0;
                    if (!WriteFile(f, buf.data(), n, &w, nullptr) || w != n) { ok = false; break; }
                    got += n;
                    if (progress) progress(got, total);
                }
                CloseHandle(f);
                if (!ok) DeleteFileW(dest.c_str());
            }
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

/** Ellenőrzi, hogy a letöltött fájl Windows futtatható-e (és nem egy HTML oldal). */
inline bool looksLikeExe(const std::wstring& p) {
    std::vector<char> head;
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char b[2] = {};
    DWORD r = 0;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    ReadFile(f, b, 2, &r, nullptr);
    CloseHandle(f);
    return r == 2 && b[0] == 'M' && b[1] == 'Z' && sz.QuadPart > 2 * 1024 * 1024;
}

/** Program futtatása és megvárása. */
inline bool runAndWait(const std::wstring& exe, const std::wstring& args, DWORD* exitCode = nullptr) {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return false;
    WaitForSingleObject(sei.hProcess, INFINITE);
    if (exitCode) GetExitCodeProcess(sei.hProcess, exitCode);
    CloseHandle(sei.hProcess);
    return true;
}

/** config.txt kódolás-megtartó olvasása (UTF-8 BOM-mal/nélkül, UTF-16LE). */
struct TextFile {
    enum class Enc { Utf8, Utf8Bom, Utf16 } enc = Enc::Utf8;
    std::wstring text;
};

inline bool readTextFile(const std::wstring& path, TextFile& tf) {
    std::vector<char> b;
    if (!readFileBytes(path, b)) return false;
    if (b.size() >= 2 && static_cast<unsigned char>(b[0]) == 0xFF && static_cast<unsigned char>(b[1]) == 0xFE) {
        tf.enc = TextFile::Enc::Utf16;
        tf.text.assign(reinterpret_cast<const wchar_t*>(b.data() + 2), (b.size() - 2) / 2);
        return true;
    }
    size_t off = 0;
    tf.enc = TextFile::Enc::Utf8;
    if (b.size() >= 3 && static_cast<unsigned char>(b[0]) == 0xEF && static_cast<unsigned char>(b[1]) == 0xBB &&
        static_cast<unsigned char>(b[2]) == 0xBF) {
        tf.enc = TextFile::Enc::Utf8Bom;
        off = 3;
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, b.data() + off, static_cast<int>(b.size() - off), nullptr, 0);
    tf.text.assign(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, b.data() + off, static_cast<int>(b.size() - off), tf.text.data(), n);
    return true;
}

inline bool writeTextFile(const std::wstring& path, const TextFile& tf) {
    std::vector<char> out;
    if (tf.enc == TextFile::Enc::Utf16) {
        out.push_back(static_cast<char>(0xFF));
        out.push_back(static_cast<char>(0xFE));
        const char* p = reinterpret_cast<const char*>(tf.text.data());
        out.insert(out.end(), p, p + tf.text.size() * 2);
    } else {
        if (tf.enc == TextFile::Enc::Utf8Bom) { out.push_back(static_cast<char>(0xEF)); out.push_back(static_cast<char>(0xBB)); out.push_back(static_cast<char>(0xBF)); }
        const int n = WideCharToMultiByte(CP_UTF8, 0, tf.text.data(), static_cast<int>(tf.text.size()), nullptr, 0, nullptr, nullptr);
        const size_t base = out.size();
        out.resize(base + static_cast<size_t>(n));
        WideCharToMultiByte(CP_UTF8, 0, tf.text.data(), static_cast<int>(tf.text.size()), out.data() + base, n, nullptr, nullptr);
    }
    const std::wstring tmp = path + L".aether-tmp";
    if (!writeFileBytes(tmp, out.data(), out.size())) return false;
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

/** Igaz, ha a szövegben van pontosan ilyen (szóközök/CR nélkül vett) sor. */
inline bool hasLine(const std::wstring& text, const std::wstring& wanted) {
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = text.substr(pos, eol - pos);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' || line.back() == L'\t')) line.pop_back();
        size_t a = line.find_first_not_of(L" \t\xFEFF");
        if (a != std::wstring::npos && line.compare(a, std::wstring::npos, wanted) == 0) return true;
        pos = eol + 1;
    }
    return false;
}

/** A plugin beillesztése: Aqulify.txt + "Include:" sor a config.txt végére (idempotens). */
inline bool integrateEqApo(const EqApoInfo& apo, const void* dll, size_t dllSize, Logger log) {
    const std::wstring pluginDir = joinPath(apo.installPath, L"VSTPlugins\\Aqulify");
    if (!createDirs(pluginDir)) { log(L"HIBA: nem hozható létre: " + pluginDir); return false; }
    const std::wstring dllPath = joinPath(pluginDir, L"Aqulify.dll");
    if (!replaceFile(dllPath, dll, dllSize, log)) { log(L"HIBA: a plugin nem másolható ide: " + dllPath); return false; }
    log(L"Plugin bemásolva: " + dllPath);

    if (!createDirs(apo.configPath)) { log(L"HIBA: az EQ APO config mappa nem érhető el: " + apo.configPath); return false; }
    const std::string inc =
        "# Aqulify DSP + analizátor csatlakozási pont\r\n"
        "# Ezt a fájlt az Aqulify telepítője kezeli; a beállításokat az Aqulify felületén módosítsd.\r\n"
        "# Bypass 1 = a DSP lánc kikerülése (az analizátor továbbra is kap jelet); Remote 0 = a felület nem vezérli.\r\n"
        "VSTPlugin: Library \"Aqulify\\Aqulify.dll\" Bypass 0 Remote 1\r\n";
    const std::wstring incPath = joinPath(apo.configPath, L"Aqulify.txt");
    if (!writeFileBytes(incPath, inc.data(), inc.size())) { log(L"HIBA: nem írható: " + incPath); return false; }

    const std::wstring cfgPath = joinPath(apo.configPath, L"config.txt");
    TextFile tf;
    if (!readTextFile(cfgPath, tf)) { tf.enc = TextFile::Enc::Utf8; tf.text.clear(); }
    const std::wstring backup = cfgPath + L".aqulify-backup";
    if (fileExists(cfgPath) && !fileExists(backup)) CopyFileW(cfgPath.c_str(), backup.c_str(), TRUE);

    // Korábban kézzel (Configuration Editorral) felvett Aqulify plugin sorok kikapcsolása,
    // különben a lánc kétszer futna
    {
        std::wstring out;
        size_t pos = 0;
        int disabled = 0;
        while (pos < tf.text.size()) {
            size_t eol = tf.text.find(L'\n', pos);
            const size_t end = eol == std::wstring::npos ? tf.text.size() : eol + 1;
            std::wstring line = tf.text.substr(pos, end - pos);
            std::wstring low = line;
            for (auto& c : low) c = static_cast<wchar_t>(towlower(c));
            const size_t first = low.find_first_not_of(L" \t");
            if (first != std::wstring::npos && low.compare(first, 10, L"vstplugin:") == 0 &&
                low.find(L"aqulify.dll") != std::wstring::npos) {
                line = L"# (Aqulify telepítő: kikapcsolva, az Aqulify.txt beillesztés helyettesíti) " + line;
                ++disabled;
            }
            out += line;
            pos = end;
        }
        if (disabled > 0) {
            tf.text = out;
            log(L"Korábban kézzel felvett Aqulify plugin sor kikapcsolva (" + std::to_wstring(disabled) + L" db).");
        }
    }

    if (hasLine(tf.text, kIncludeLine)) {
        log(L"Az EQ APO config.txt már tartalmazza az Aqulify sort.");
        return writeTextFile(cfgPath, tf);
    }
    if (!tf.text.empty() && tf.text.back() != L'\n') tf.text += L"\r\n";
    tf.text += std::wstring(kIncludeComment) + L"\r\n" + kIncludeLine + L"\r\n";
    if (!writeTextFile(cfgPath, tf)) { log(L"HIBA: a config.txt nem írható: " + cfgPath); return false; }
    log(L"Az Aqulify a lánc végére került (config.txt → Include: Aqulify.txt).");
    return true;
}

/** Visszavonás: Include sor + Aqulify.txt + plugin DLL. */
inline void removeEqApoIntegration(Logger log) {
    const EqApoInfo apo = detectEqApo();
    if (!apo.installed) return;
    const std::wstring cfgPath = joinPath(apo.configPath, L"config.txt");
    TextFile tf;
    if (readTextFile(cfgPath, tf)) {
        std::wstring out;
        size_t pos = 0;
        bool changed = false;
        while (pos <= tf.text.size()) {
            size_t eol = tf.text.find(L'\n', pos);
            const bool last = eol == std::wstring::npos;
            if (last) eol = tf.text.size();
            std::wstring line = tf.text.substr(pos, eol - pos);
            std::wstring trimmed = line;
            while (!trimmed.empty() && (trimmed.back() == L'\r' || trimmed.back() == L' ')) trimmed.pop_back();
            if (trimmed == kIncludeLine || trimmed == kIncludeComment) {
                changed = true;
            } else {
                out += line;
                if (!last) out += L'\n';
            }
            if (last) break;
            pos = eol + 1;
        }
        if (changed) {
            tf.text = out;
            if (writeTextFile(cfgPath, tf)) log(L"Az Aqulify sor törölve az EQ APO config.txt-ből.");
        }
    }
    deleteFileOrSchedule(joinPath(apo.configPath, L"Aqulify.txt"));
    // Az EQ APO a config változását figyeli és elengedi a plugint; adunk neki egy kis időt
    const std::wstring dll = joinPath(apo.installPath, L"VSTPlugins\\Aqulify\\Aqulify.dll");
    for (int i = 0; i < 20 && fileExists(dll); ++i) {
        if (DeleteFileW(dll.c_str())) break;
        Sleep(250);
    }
    deleteTree(joinPath(apo.installPath, L"VSTPlugins\\Aqulify"));
}


// ------------------------------------------------------------------ régi név (AetherAudio, 1.4.8-ig)
/**
 * A korábbi "AetherAudio" telepítés eltávolítása frissítéskor: futó motor/felület, EQ APO plugin sora
 * (különben két plugin dolgozna egymás után), parancsikonok, automatikus indítás, programmappa,
 * eltávolító bejegyzés. A ProgramData beállításai (params.bin, ir.bin) átkerülnek az Aqulify mappájába.
 */
inline void removeLegacyAetherAudio(Logger log) {
    const std::wstring oldName = std::wstring(L"Aether") + L"Audio";
    const std::wstring oldUninstKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + oldName;
    std::wstring oldDir = regReadString(HKEY_LOCAL_MACHINE, oldUninstKey.c_str(), L"InstallLocation");
    if (oldDir.empty()) oldDir = joinPath(knownFolder(FOLDERID_ProgramFiles), oldName);
    const std::wstring oldData = joinPath(knownFolder(FOLDERID_ProgramData), oldName);
    const EqApoInfo apo = detectEqApo();
    const bool present = fileExists(joinPath(oldDir, oldName + L".exe")) || dirExists(oldData) ||
                         (apo.installed && fileExists(joinPath(apo.configPath, oldName + L".txt")));
    if (!present) return;
    log(L"Korábbi " + oldName + L" telepítés található: eltávolítás, a beállítások átvétele...");

    // 1. Futó régi motor és felület
    const std::wstring oldWndClass = oldName + L"EngineWnd";
    for (int round = 0; round < 3; ++round) {
        HWND w = nullptr;
        while ((w = FindWindowExW(nullptr, w, oldWndClass.c_str(), nullptr)) != nullptr) PostMessageW(w, WM_CLOSE, 0, 0);
        for (int i = 0; i < 20 && FindWindowW(oldWndClass.c_str(), nullptr); ++i) Sleep(100);
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            if (_wcsicmp(pe.szExeFile, (oldName + L".exe").c_str()) != 0 && _wcsicmp(pe.szExeFile, (oldName + L"Gui.exe").c_str()) != 0) continue;
            if (HANDLE pr = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID)) {
                TerminateProcess(pr, 0);
                WaitForSingleObject(pr, 3000);
                CloseHandle(pr);
            }
        }
        CloseHandle(snap);
    }

    // 2. EQ APO: régi Include sor, include fájl és plugin DLL
    if (apo.installed) {
        const std::wstring cfgPath = joinPath(apo.configPath, L"config.txt");
        const std::wstring oldInclude = L"Include: " + oldName + L".txt";
        TextFile tf;
        if (readTextFile(cfgPath, tf)) {
            std::wstring out;
            size_t pos = 0;
            bool changed = false;
            while (pos <= tf.text.size()) {
                size_t eol = tf.text.find(L'\n', pos);
                const bool last = eol == std::wstring::npos;
                if (last) eol = tf.text.size();
                std::wstring line = tf.text.substr(pos, eol - pos);
                std::wstring trimmed = line;
                while (!trimmed.empty() && (trimmed.back() == L'\r' || trimmed.back() == L' ')) trimmed.pop_back();
                if (trimmed == oldInclude || (trimmed.rfind(L"# " + oldName + L" (a telepítő adta hozzá", 0) == 0)) {
                    changed = true;
                } else {
                    out += line;
                    if (!last) out += L'\n';
                }
                if (last) break;
                pos = eol + 1;
            }
            if (changed) {
                tf.text = out;
                if (writeTextFile(cfgPath, tf)) log(L"A régi " + oldName + L" sor törölve az EQ APO config.txt-ből.");
            }
        }
        deleteFileOrSchedule(joinPath(apo.configPath, oldName + L".txt"));
        const std::wstring oldDll = joinPath(apo.installPath, L"VSTPlugins\\" + oldName + L"\\" + oldName + L".dll");
        for (int i = 0; i < 20 && fileExists(oldDll); ++i) {
            if (DeleteFileW(oldDll.c_str())) break;
            Sleep(250);
        }
        deleteTree(joinPath(apo.installPath, L"VSTPlugins\\" + oldName));
    }

    // 3. Beállítások átvétele (ha az Aqulify-nak még nincs sajátja)
    const std::wstring newData = joinPath(knownFolder(FOLDERID_ProgramData), L"Aqulify");
    createDirs(newData);
    for (const wchar_t* f : { L"params.bin", L"ir.bin" }) {
        const std::wstring src = joinPath(oldData, f), dst = joinPath(newData, f);
        if (fileExists(src) && !fileExists(dst) && CopyFileW(src.c_str(), dst.c_str(), TRUE))
            log(std::wstring(L"Beállítás átvéve: ") + f);
    }

    // 4. Parancsikonok, automatikus indítás, programmappa, eltávolító bejegyzés
    deleteTree(joinPath(knownFolder(FOLDERID_CommonPrograms), oldName));
    DeleteFileW(joinPath(knownFolder(FOLDERID_PublicDesktop), oldName + L".lnk").c_str());
    regDeleteValue(HKEY_CURRENT_USER, kRunKey, oldName.c_str());
    regDeleteValue(HKEY_LOCAL_MACHINE, kRunKey, oldName.c_str());
    if (dirExists(oldDir) && fileExists(joinPath(oldDir, oldName + L".exe"))) deleteTree(oldDir);
    regDeleteTree(HKEY_LOCAL_MACHINE, oldUninstKey.c_str());
    regDeleteTree(HKEY_LOCAL_MACHINE, (L"Software\\" + oldName).c_str());
    deleteTree(oldData);
    log(L"A korábbi " + oldName + L" eltávolítva; az Aqulify veszi át a helyét.");
}

// ------------------------------------------------------------------ rendszer integráció

/** ProgramData\Aqulify: a motor (felhasználó) írja, az audiodg (LOCAL SERVICE) olvassa/naplóz. */
inline void prepareDataDir() {
    const std::wstring dir = joinPath(knownFolder(FOLDERID_ProgramData), L"Aqulify");
    createDirs(dir);
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;IU)(A;OICI;FA;;;LS)(A;OICI;0x1301bf;;;BU)(A;OICI;FR;;;WD)",
            SDDL_REVISION_1, &sd, nullptr)) {
        SetFileSecurityW(dir.c_str(), DACL_SECURITY_INFORMATION, sd);
        LocalFree(sd);
    }
}

inline bool createShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& args,
                           const std::wstring& desc, const std::wstring& workDir) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&sl))))
        return false;
    sl->SetPath(target.c_str());
    sl->SetArguments(args.c_str());
    sl->SetDescription(desc.c_str());
    sl->SetWorkingDirectory(workDir.c_str());
    sl->SetIconLocation(target.c_str(), 0);
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pf)))) {
        ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
        pf->Release();
    }
    sl->Release();
    return ok;
}

/** A futó motor(ok) és felület(ek) bezárása (tálcaablak WM_CLOSE, végső esetben a folyamat leállítása). */
inline void closeRunningEngine(const std::wstring& exePath) {
    for (int round = 0; round < 3; ++round) {
        HWND w = nullptr;
        while ((w = FindWindowExW(nullptr, w, kEngineWindowClass, nullptr)) != nullptr) PostMessageW(w, WM_CLOSE, 0, 0);
        for (int i = 0; i < 20 && FindWindowW(kEngineWindowClass, nullptr); ++i) Sleep(100);
    }
    // Más munkamenetben (vagy beragadt) futó példány: folyamat név + útvonal alapján
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        const bool isEngine = _wcsicmp(pe.szExeFile, L"Aqulify.exe") == 0;
        const bool isGui = _wcsicmp(pe.szExeFile, L"AqulifyGui.exe") == 0;
        if (!isEngine && !isGui) continue;
        HANDLE p = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pe.th32ProcessID);
        if (!p) continue;
        wchar_t img[MAX_PATH];
        DWORD n = MAX_PATH;
        // A felület (AqulifyGui.exe) ugyanabban a mappában van, mint a motor
        std::wstring want = exePath;
        if (isGui && !want.empty()) {
            const size_t sl = want.find_last_of(L"\\/");
            want = (sl == std::wstring::npos ? std::wstring() : want.substr(0, sl + 1)) + L"AqulifyGui.exe";
        }
        if (QueryFullProcessImageNameW(p, 0, img, &n) && (want.empty() || _wcsicmp(img, want.c_str()) == 0)) {
            TerminateProcess(p, 0);
            WaitForSingleObject(p, 3000);
        }
        CloseHandle(p);
    }
    CloseHandle(snap);
}

/**
 * Program indítása NEM emelt jogosultsággal egy emelt telepítőből (az Asztal shell-jén keresztül),
 * hogy a motor és a böngésző a felhasználó normál jogaival fusson.
 */
inline bool launchUnelevated(const std::wstring& exe, const std::wstring& args) {
    bool ok = false;
    IShellWindows* sw = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_IShellWindows,
                                   reinterpret_cast<void**>(&sw)))) {
        VARIANT loc, empty;
        VariantInit(&loc);
        VariantInit(&empty);
        long hwnd = 0;
        IDispatch* disp = nullptr;
        if (sw->FindWindowSW(&loc, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp) == S_OK && disp) {
            IServiceProvider* sp = nullptr;
            if (SUCCEEDED(disp->QueryInterface(IID_IServiceProvider, reinterpret_cast<void**>(&sp)))) {
                IShellBrowser* sb = nullptr;
                if (SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, reinterpret_cast<void**>(&sb)))) {
                    IShellView* sv = nullptr;
                    if (SUCCEEDED(sb->QueryActiveShellView(&sv))) {
                        IDispatch* bg = nullptr;
                        if (SUCCEEDED(sv->GetItemObject(SVGIO_BACKGROUND, IID_IDispatch, reinterpret_cast<void**>(&bg)))) {
                            IShellFolderViewDual* fv = nullptr;
                            if (SUCCEEDED(bg->QueryInterface(IID_IShellFolderViewDual, reinterpret_cast<void**>(&fv)))) {
                                IDispatch* app = nullptr;
                                if (SUCCEEDED(fv->get_Application(&app))) {
                                    IShellDispatch2* sd = nullptr;
                                    if (SUCCEEDED(app->QueryInterface(IID_IShellDispatch2, reinterpret_cast<void**>(&sd)))) {
                                        BSTR file = SysAllocString(exe.c_str());
                                        VARIANT vArgs, vDir, vOp, vShow;
                                        VariantInit(&vArgs); VariantInit(&vDir); VariantInit(&vOp); VariantInit(&vShow);
                                        vArgs.vt = VT_BSTR; vArgs.bstrVal = SysAllocString(args.c_str());
                                        vDir.vt = VT_BSTR; vDir.bstrVal = SysAllocString(parentDir(exe).c_str());
                                        vShow.vt = VT_I4; vShow.lVal = SW_SHOWNORMAL;
                                        ok = SUCCEEDED(sd->ShellExecute(file, vArgs, vDir, vOp, vShow));
                                        SysFreeString(file);
                                        VariantClear(&vArgs);
                                        VariantClear(&vDir);
                                        sd->Release();
                                    }
                                    app->Release();
                                }
                                fv->Release();
                            }
                            bg->Release();
                        }
                        sv->Release();
                    }
                    sb->Release();
                }
                sp->Release();
            }
            disp->Release();
        }
        sw->Release();
    }
    if (!ok) {
        ok = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), args.c_str(), parentDir(exe).c_str(),
                                                    SW_SHOWNORMAL)) > 32;
    }
    return ok;
}

} // namespace aether_setup
