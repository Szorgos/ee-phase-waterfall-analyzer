#pragma once
/**
 * @file win_download.hpp
 * @brief HTTPS letöltés WinHTTP-vel és a letöltött telepítő gyors ellenőrzése. Közös a telepítővel (Equalizer APO
 *        letöltése) és a motorral (egykattintásos frissítés, windows/engine/updater.hpp).
 */
#include <windows.h>
#include <winhttp.h>

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace ee_analyzer::win {

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

} // namespace ee_analyzer::win
