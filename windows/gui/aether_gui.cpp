/**
 * @file aether_gui.cpp
 * @brief AetherAudio grafikus felület saját ablakban (WebView2).
 *
 * A gui_integrated_system.html-t egy beágyazott WebView2 (Chromium) vezérlőben jeleníti meg,
 * böngészőfülek és címsor nélkül. A felület ugyanúgy a motor WebSocket-hídján (127.0.0.1) kommunikál.
 * A fájlokat a "https://aether.local/" virtuális címről szolgálja ki (biztonságos eredet,
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

#include <string>

namespace {

constexpr const wchar_t* kTitle = L"AetherAudio";
constexpr const char* kTitleUtf8 = "AetherAudio";
constexpr const wchar_t* kVirtualHost = L"aether.local";
constexpr const char* kStartUrl = "https://aether.local/gui_integrated_system.html";

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

/** Tartalék: a felület megnyitása az alapértelmezett böngészőben. */
int openInBrowser(const std::wstring& guiFolder) {
    if (guiFolder.empty()) {
        MessageBoxW(nullptr, L"A felület fájljai (gui\\gui_integrated_system.html) nem találhatók.\n"
                             L"Telepítsd újra az AetherAudio-t.", kTitle, MB_ICONERROR | MB_OK);
        return 1;
    }
    const std::wstring html = guiFolder + L"\\gui_integrated_system.html";
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

/** A gui mappa kiszolgálása a https://aether.local/ virtuális címen. */
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

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\AetherAudioGui");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        activateExistingWindow();
        CloseHandle(single);
        return 0;
    }

    const std::wstring guiFolder = findGuiFolder();
    if (guiFolder.empty()) return openInBrowser(guiFolder);

    int rc = 0;
    try {
        webview::webview w(false, nullptr);
        w.set_title(kTitleUtf8);
        w.set_size(1480, 940, WEBVIEW_HINT_NONE);
        w.set_size(1000, 640, WEBVIEW_HINT_MIN);

        auto win = w.window();
        if (win.ok()) styleWindow(static_cast<HWND>(win.value()));

        if (mapVirtualHost(w, guiFolder)) {
            w.navigate(kStartUrl);
        } else {
            // Régi WebView2 futtatókörnyezet: helyi fájlként töltjük be
            std::wstring url = L"file:///" + guiFolder + L"/gui_integrated_system.html";
            for (auto& c : url) if (c == L'\\') c = L'/';
            w.navigate(toUtf8(url));
        }
        w.run();
    } catch (const std::exception&) {
        // Nincs (vagy nem indítható) WebView2 futtatókörnyezet: böngészős tartalék
        rc = openInBrowser(guiFolder);
    }

    if (single) { ReleaseMutex(single); CloseHandle(single); }
    return rc;
}
