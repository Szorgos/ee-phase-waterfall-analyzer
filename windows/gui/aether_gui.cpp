/**
 * @file aether_gui.cpp
 * @brief Aqulify grafikus felület saját ablakban (WebView2).
 *
 * A gui_integrated_system.html-t egy beágyazott WebView2 (Chromium) vezérlőben jeleníti meg,
 * böngészőfülek és címsor nélkül. A felület ugyanúgy a motor WebSocket-hídján (127.0.0.1) kommunikál.
 * A fájlokat a "https://aqulify.local/" virtuális címről szolgálja ki (biztonságos eredet,
 * a localStorage és a fájlválasztó rendesen működik).
 *
 * Ha a gépen nincs WebView2 futtatókörnyezet, a felület az alapértelmezett böngészőben nyílik meg.
 * Egyszerre egy ablak: a második indítás az előzőt hozza előtérbe.
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>

#define WEBVIEW_MSWEBVIEW2_BUILTIN_IMPL 1
#include "webview/webview.h"

#include <shlobj.h>

#include <cstdio>
#include <string>

namespace {

/** Hibanapló a felülethez: %LOCALAPPDATA%\Aqulify\gui.log (miért nem indult a saját ablak). */
void guiLog(const std::string& msg) {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) || !p) return;
    std::wstring dir = std::wstring(p) + L"\\Aqulify";
    CoTaskMemFree(p);
    CreateDirectoryW(dir.c_str(), nullptr);
    FILE* f = _wfopen((dir + L"\\gui.log").c_str(), L"a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, msg.c_str());
    std::fclose(f);
}

constexpr const wchar_t* kTitle = L"Aqulify";
constexpr const char* kTitleUtf8 = "Aqulify";
constexpr const wchar_t* kVirtualHost = L"aqulify.local";
constexpr const char* kStartUrl = "https://aqulify.local/gui_integrated_system.html";

std::wstring exeDirectory() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : p.substr(0, slash);
}

bool fileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/** A gui_integrated_system.html-t tartalmazó mappa (telepítve: <exe>\gui). */
std::wstring findGuiFolder() {
    const std::wstring dir = exeDirectory();
    for (const std::wstring& d : { dir + L"\\gui", dir, dir + L"\\.." }) {
        if (fileExists(d + L"\\gui_integrated_system.html")) return d;
    }
    return {};
}

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

/**
 * Tartalék WebView2 nélkül: a felület saját alkalmazásablakban az Edge-dzsel (--app: nincsenek fülek és
 * címsor), és csak ha az sem indul, az alapértelmezett böngészőben.
 */
int openInBrowser(const std::wstring& guiFolder) {
    if (guiFolder.empty()) {
        MessageBoxW(nullptr, L"A felület fájljai (gui\\gui_integrated_system.html) nem találhatók.\n"
                             L"Telepítsd újra az Aqulify-t.", kTitle, MB_ICONERROR | MB_OK);
        return 1;
    }
    const std::wstring html = guiFolder + L"\\gui_integrated_system.html";
    std::wstring url = L"file:///" + html;
    for (auto& c : url) if (c == L'\\') c = L'/';
    const std::wstring args = L"--app=\"" + url + L"\" --window-size=1480,940";
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"msedge.exe", args.c_str(), nullptr, SW_SHOWNORMAL)) > 32)
        return 0;
    ShellExecuteW(nullptr, L"open", html.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return 0;
}

/** Ha már nyitva van egy ablakunk, előtérbe hozzuk. */
bool activateExistingWindow() {
    HWND w = nullptr;
    while ((w = FindWindowExW(nullptr, w, nullptr, kTitle)) != nullptr) {
        wchar_t cls[64];
        GetClassNameW(w, cls, 64);
        if (_wcsicmp(cls, L"webview") != 0) continue; // csak a saját (webview) ablakunk számít
        if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
        else ShowWindow(w, SW_SHOW);
        SetForegroundWindow(w);
        return true;
    }
    return false;
}

void styleWindow(HWND hwnd) {
    // Ikon a saját erőforrásból (1-es ICON)
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (HICON big = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                    GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0)))
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    if (HICON small = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0)))
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
    // Sötét címsor (a felület sötét témájú); régebbi Windowson egyszerűen hatástalan
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
}

/** A gui mappa kiszolgálása a https://aqulify.local/ virtuális címen. */
bool mapVirtualHost(webview::webview& w, const std::wstring& folder) {
    auto res = w.browser_controller();
    if (!res.ok()) return false;
    auto* controller = static_cast<ICoreWebView2Controller*>(res.value());
    ICoreWebView2* core = nullptr;
    if (FAILED(controller->get_CoreWebView2(&core)) || !core) return false;
    bool ok = false;
    ICoreWebView2_3* core3 = nullptr;
    if (SUCCEEDED(core->QueryInterface(IID_ICoreWebView2_3, reinterpret_cast<void**>(&core3))) && core3) {
        ok = SUCCEEDED(core3->SetVirtualHostNameToFolderMapping(kVirtualHost, folder.c_str(),
                                                                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW));
        core3->Release();
    }
    // A böngésző saját gyorsbillentyűi (Ctrl+P nyomtatás, F5 stb.) maradnak; a jobb klikkes menü is
    core->Release();
    return ok;
}


/** JS-ből hívható külső link (pl. támogatás): csak engedélyezett https címeket nyit meg a rendszer böngészőjében. */
std::string allowedExternalUrl(const std::string& req) {
    // req: JSON tömb, pl. ["https://revolut.me/gergoqhg6"]
    const size_t a = req.find('"');
    const size_t b = a == std::string::npos ? std::string::npos : req.find('"', a + 1);
    if (b == std::string::npos) return {};
    const std::string url = req.substr(a + 1, b - a - 1);
    for (const char* ok : { "https://revolut.me/", "https://github.com/Szorgos/" })
        if (url.rfind(ok, 0) == 0 && url.find_first_of("'\\ \"<>`$;&|") == std::string::npos) return url;
    return {};
}

} // namespace

/** A felület WebView2 ablakban; kivételt dob, ha a WebView2 nem indítható. */
void runWindow(const std::wstring& guiFolder) {
    webview::webview w(false, nullptr);
    w.set_title(kTitleUtf8);
    w.set_size(1480, 940, WEBVIEW_HINT_NONE);
    w.set_size(1000, 640, WEBVIEW_HINT_MIN);

    auto win = w.window();
    if (win.ok()) styleWindow(static_cast<HWND>(win.value()));

    w.bind("aetherOpenUrl", [](const std::string& req) -> std::string {
        const std::string url = allowedExternalUrl(req);
        if (!url.empty()) {
            const int n = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
            std::wstring wurl(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wurl.data(), n);
            ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return "null";
    });
    if (mapVirtualHost(w, guiFolder)) {
        w.navigate(kStartUrl);
    } else {
        // Régi WebView2 futtatókörnyezet: helyi fájlként töltjük be
        std::wstring url = L"file:///" + guiFolder + L"/gui_integrated_system.html";
        for (auto& c : url) if (c == L'\\') c = L'/';
        w.navigate(toUtf8(url));
    }
    w.run();
}

/**
 * A WebView2 adatmappája: %LOCALAPPDATA%\Aqulify\<név>. A WEBVIEW2_USER_DATA_FOLDER felülírja a
 * webview könyvtár alapértelmezését (%APPDATA%\AqulifyGui.exe), amely egy korábbi, rendszergazdaként
 * futott példány után nem írható lehet, és ilyenkor a WebView2 nem indul.
 */
void useWebViewDataFolder(const wchar_t* name, bool reset) {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) || !p) return;
    std::wstring dir = std::wstring(p) + L"\\Aqulify";
    CoTaskMemFree(p);
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\";
    dir += name;
    if (reset) {
        std::wstring from = dir;
        from.push_back(L'\0'); // SHFileOperation: dupla nullával lezárt lista
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_NO_UI;
        SHFileOperationW(&op);
    }
    SetEnvironmentVariableW(L"WEBVIEW2_USER_DATA_FOLDER", dir.c_str());
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\AqulifyGui");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        activateExistingWindow();
        CloseHandle(single);
        return 0;
    }

    const std::wstring guiFolder = findGuiFolder();
    if (guiFolder.empty()) return openInBrowser(guiFolder);

    int rc = 0;
    bool shown = false;
    for (int attempt = 0; attempt < 2 && !shown; ++attempt) {
        // 2. kísérlet: friss, üres adatmappa (sérült vagy nem írható profil esetére)
        useWebViewDataFolder(attempt == 0 ? L"WebView2" : L"WebView2-uj", attempt > 0);
        try {
            runWindow(guiFolder);
            shown = true;
        } catch (const std::exception& e) {
            guiLog(std::string("A WebView2 ablak nem indult (") + std::to_string(attempt + 1) + ". kísérlet): " + e.what());
        }
    }
    // Nincs (vagy nem indítható) WebView2 futtatókörnyezet: Edge alkalmazásablak / böngészős tartalék
    if (!shown) rc = openInBrowser(guiFolder);

    if (single) { ReleaseMutex(single); CloseHandle(single); }
    return rc;
}
