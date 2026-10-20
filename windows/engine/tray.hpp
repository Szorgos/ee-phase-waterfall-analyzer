#pragma once
/**
 * @file tray.hpp
 * @brief Tálcaikon (értesítési terület) és naplózás az Aqulify motorhoz: konzolablak nélküli háttérfutás.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

namespace ee_analyzer::win {

constexpr const wchar_t* kEngineWindowClass = L"AqulifyEngineWnd"; // a telepítő is ezt keresi
constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValue = L"Aqulify";

inline std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

inline std::wstring exePath() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf, n);
}

/** %LOCALAPPDATA%\Aqulify */
inline std::filesystem::path localAppDir() {
    PWSTR p = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p) {
        dir = std::filesystem::path(p) / L"Aqulify";
        CoTaskMemFree(p);
    } else {
        dir = std::filesystem::temp_directory_path() / L"Aqulify";
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

/** Kimenet átirányítása: --console esetén konzolablak, egyébként napló fájl. */
class LogSink {
public:
    explicit LogSink(bool console) {
        if (console) {
            if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
            SetConsoleOutputCP(CP_UTF8);
            FILE* f = nullptr;
            freopen_s(&f, "CONOUT$", "w", stdout);
            freopen_s(&f, "CONOUT$", "w", stderr);
            std::cout.clear();
            std::cerr.clear();
            return;
        }
        path_ = localAppDir() / L"engine.log";
        std::error_code ec;
        if (std::filesystem::exists(path_, ec) && std::filesystem::file_size(path_, ec) > 2 * 1024 * 1024) {
            std::filesystem::rename(path_, localAppDir() / L"engine.old.log", ec);
        }
        file_ = std::make_unique<std::ofstream>(path_, std::ios::app);
        if (file_->is_open()) {
            *file_ << std::unitbuf;
            oldOut_ = std::cout.rdbuf(file_->rdbuf());
            oldErr_ = std::cerr.rdbuf(file_->rdbuf());
            std::cout << std::unitbuf; // minden sor azonnal a fájlba (összeomlás / leállítás esetén se vesszen el)
            SYSTEMTIME t;
            GetLocalTime(&t);
            char ts[64];
            std::snprintf(ts, sizeof(ts), "%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
            std::cout << "\n===== Aqulify motor indul: " << ts << " =====\n";
        }
    }
    ~LogSink() {
        if (oldOut_) std::cout.rdbuf(oldOut_);
        if (oldErr_) std::cerr.rdbuf(oldErr_);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    std::unique_ptr<std::ofstream> file_;
    std::streambuf* oldOut_ = nullptr;
    std::streambuf* oldErr_ = nullptr;
};

/** Automatikus indítás (HKCU\...\Run) */
inline bool autostartEnabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    const bool on = RegQueryValueExW(k, kRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

inline bool setAutostart(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    LONG r;
    if (on) {
        const std::wstring cmd = L"\"" + exePath() + L"\" --background";
        r = RegSetValueExW(k, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                           static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        r = RegDeleteValueW(k, kRunValue);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

/**
 * Tálcaikon rejtett üzenetablakkal. A fő szál üzenethurkát futtatja; a periodikus munka (forrásfigyelés,
 * mentés) időzítőről hívódik.
 */
class TrayApp {
public:
    struct Callbacks {
        std::function<void()> openGui;
        std::function<void()> openLog;
        std::function<void()> onTimer;     // ~250 ms
        std::function<std::wstring()> statusText; // tooltip + menü első sora
    };

    explicit TrayApp(Callbacks cb) : cb_(std::move(cb)) {}

    bool create(HINSTANCE inst) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &TrayApp::wndProc;
        wc.hInstance = inst;
        wc.lpszClassName = kEngineWindowClass;
        RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(0, kEngineWindowClass, L"Aqulify", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, inst, this);
        if (!hwnd_) return false;
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        icon_ = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                              GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        if (!icon_) icon_ = LoadIconW(nullptr, IDI_APPLICATION);
        addIcon();
        SetTimer(hwnd_, 1, 250, nullptr);
        return true;
    }

    /** Üzenethurok; WM_QUIT-ig fut. */
    int run() {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        removeIcon();
        return 0;
    }

    void quit() { if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0); }

    void balloon(const std::wstring& title, const std::wstring& text) {
        NOTIFYICONDATAW nid = baseData();
        nid.uFlags = NIF_INFO;
        wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
        nid.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

private:
    static constexpr UINT kTrayMsg = WM_APP + 1;
    enum : UINT { kCmdOpen = 100, kCmdAutostart, kCmdLog, kCmdExit };

    NOTIFYICONDATAW baseData() const {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = 1;
        return nid;
    }

    void addIcon() {
        NOTIFYICONDATAW nid = baseData();
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = kTrayMsg;
        nid.hIcon = icon_;
        wcsncpy_s(nid.szTip, L"Aqulify", _TRUNCATE);
        Shell_NotifyIconW(NIM_ADD, &nid);
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }

    void removeIcon() {
        NOTIFYICONDATAW nid = baseData();
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }

    void updateTip() {
        if (!cb_.statusText) return;
        const std::wstring st = cb_.statusText();
        if (st == lastTip_) return;
        lastTip_ = st;
        NOTIFYICONDATAW nid = baseData();
        nid.uFlags = NIF_TIP;
        const std::wstring tip = L"Aqulify – " + st;
        wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void showMenu() {
        HMENU m = CreatePopupMenu();
        const std::wstring st = cb_.statusText ? cb_.statusText() : L"";
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, (L"Forrás: " + st).c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, kCmdOpen, L"Felület megnyitása");
        AppendMenuW(m, MF_STRING | (autostartEnabled() ? MF_CHECKED : MF_UNCHECKED), kCmdAutostart, L"Indítás a Windows-zal");
        AppendMenuW(m, MF_STRING, kCmdLog, L"Napló megnyitása");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, kCmdExit, L"Kilépés");
        SetMenuDefaultItem(m, kCmdOpen, FALSE);
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd_);
        TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        DestroyMenu(m);
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == taskbarCreated_ && taskbarCreated_ != 0) { addIcon(); return 0; }
        switch (msg) {
            case kTrayMsg:
                switch (LOWORD(lp)) {
                    case WM_LBUTTONDBLCLK:
                    case NIN_SELECT:
                    case NIN_KEYSELECT:
                        if (cb_.openGui) cb_.openGui();
                        return 0;
                    case WM_CONTEXTMENU:
                    case WM_RBUTTONUP:
                        showMenu();
                        return 0;
                }
                return 0;
            case WM_COMMAND:
                switch (LOWORD(wp)) {
                    case kCmdOpen: if (cb_.openGui) cb_.openGui(); break;
                    case kCmdAutostart: setAutostart(!autostartEnabled()); break;
                    case kCmdLog: if (cb_.openLog) cb_.openLog(); break;
                    case kCmdExit: DestroyWindow(hwnd_); break;
                }
                return 0;
            case WM_TIMER:
                if (cb_.onTimer) cb_.onTimer();
                updateTip();
                return 0;
            case WM_CLOSE:          // a telepítő / eltávolító is ezt küldi
            case WM_QUERYENDSESSION:
                if (msg == WM_CLOSE) DestroyWindow(hwnd_);
                return TRUE;
            case WM_DESTROY:
                KillTimer(hwnd_, 1);
                removeIcon();
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            static_cast<TrayApp*>(cs->lpCreateParams)->hwnd_ = h;
        }
        auto* self = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        return self ? self->handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
    }

    Callbacks cb_;
    HWND hwnd_ = nullptr;
    HICON icon_ = nullptr;
    UINT taskbarCreated_ = 0;
    std::wstring lastTip_;
};

} // namespace ee_analyzer::win
