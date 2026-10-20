/**
 * @file setup.cpp
 * @brief Aqulify-Setup.exe – egyfájlos Windows telepítő és eltávolító.
 *
 * Telepítés:   Aqulify-Setup.exe            (varázsló)
 *              Aqulify-Setup.exe /S [/D=mappa] [/NOAPO] [/DOWNLOADAPO]   (csendes telepítés)
 * Eltávolítás: Uninstall.exe /uninstall [/S]     (a "Programok és szolgáltatások" ezt hívja)
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "install_ops.hpp"

#include <commctrl.h>
#include <shellapi.h>

#include <atomic>
#include <memory>
#include <thread>

using namespace aether_setup;

namespace {

// ------------------------------------------------------------------ beállítások
struct Options {
    std::wstring installDir;
    bool integrateApo = true;
    bool downloadApo = true;
    bool autostart = true;
    bool desktopIcon = true;
    bool silent = false;
};

struct Result {
    bool ok = false;
    bool apoIntegrated = false;
    bool apoInstalledNow = false;
    std::wstring message;
};

int g_dpi = 96;
int S(int v) { return MulDiv(v, g_dpi, 96); }

std::wstring defaultInstallDir() {
    const std::wstring prev = regReadString(HKEY_LOCAL_MACHINE, kAppKey, L"InstallDir");
    if (!prev.empty()) return prev;
    return joinPath(knownFolder(FOLDERID_ProgramFiles), L"Aqulify");
}

// ------------------------------------------------------------------ telepítés (háttérszál)
class Installer {
public:
    Logger log;
    std::function<void(int)> progress;     // 0..100
    std::function<void(const std::wstring&)> phase;
    bool cancel = false;

    Result run(const Options& opt) {
        Result res;
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const std::vector<PayloadFile> files = payloadIndex();
        if (files.empty()) {
            res.message = L"A telepítő sérült: nem találhatók a beágyazott fájlok.";
            CoUninitialize();
            return res;
        }
        const std::wstring exe = joinPath(opt.installDir, L"Aqulify.exe");

        phase(L"Futó Aqulify bezárása...");
        closeRunningEngine(exe);
        removeLegacyAetherAudio(log);   // régi név (AetherAudio) telepítésének átvétele
        progress(5);

        // 1. Fájlok
        phase(L"Fájlok másolása...");
        if (!createDirs(opt.installDir)) {
            res.message = L"A telepítési mappa nem hozható létre:\n" + opt.installDir;
            CoUninitialize();
            return res;
        }
        const void* dllData = nullptr;
        size_t dllSize = 0;
        int done = 0;
        for (const auto& f : files) {
            const void* data = nullptr;
            size_t size = 0;
            if (!payloadData(f.resName, data, size)) continue;
            if (f.relPath == L"plugin\\Aqulify.dll") { dllData = data; dllSize = size; }
            const std::wstring dst = joinPath(opt.installDir, f.relPath);
            createDirs(parentDir(dst));
            if (!replaceFile(dst, data, size, log)) {
                res.message = L"Nem sikerült írni:\n" + dst;
                CoUninitialize();
                return res;
            }
            log(L"  " + f.relPath);
            progress(5 + 35 * (++done) / static_cast<int>(files.size()));
        }
        // Eltávolító = a telepítő másolata
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        const std::wstring uninst = joinPath(opt.installDir, L"Uninstall.exe");
        if (_wcsicmp(self, uninst.c_str()) != 0) {
            if (!CopyFileW(self, uninst.c_str(), FALSE)) {
                std::vector<char> bytes;
                if (readFileBytes(self, bytes)) replaceFile(uninst, bytes.data(), bytes.size(), log);
            }
        }
        prepareDataDir();
        progress(45);

        // 2. Equalizer APO
        EqApoInfo apo = detectEqApo();
        if (opt.integrateApo && !apo.installed && opt.downloadApo) {
            phase(L"Equalizer APO letöltése...");
            const std::wstring tmp = joinPath(knownFolder(FOLDERID_ProgramData), L"Aqulify\\EqualizerAPO-setup.exe");
            bool got = false;
            for (const wchar_t* url : kEqApoDownloadUrls) {
                log(std::wstring(L"Letöltés: ") + url);
                if (downloadFile(url, tmp, [&](uint64_t a, uint64_t t) { if (t) progress(45 + static_cast<int>(20 * a / t)); }, &cancel) &&
                    looksLikeExe(tmp)) {
                    got = true;
                    break;
                }
                log(L"  sikertelen, következő forrás...");
            }
            if (got) {
                phase(L"Equalizer APO telepítése (külön ablakban)...");
                log(L"Az Equalizer APO telepítője elindult. A végén jelöld be azt a hangeszközt, amin zenét hallgatsz!");
                runAndWait(tmp, L"");
                DeleteFileW(tmp.c_str());
                apo = detectEqApo();
                res.apoInstalledNow = apo.installed;
                log(apo.installed ? L"Equalizer APO telepítve." : L"Az Equalizer APO telepítése megszakadt vagy sikertelen.");
            } else {
                log(L"Az Equalizer APO automatikus letöltése nem sikerült. Később kézzel telepítheted: " + std::wstring(kEqApoPage));
            }
        }
        progress(70);

        if (opt.integrateApo && apo.installed) {
            phase(L"Beillesztés az Equalizer APO láncba...");
            if (dllData) res.apoIntegrated = integrateEqApo(apo, dllData, dllSize, log);
            regWriteDword(HKEY_LOCAL_MACHINE, kAppKey, L"EqApoIntegrated", res.apoIntegrated ? 1 : 0);
        } else if (opt.integrateApo) {
            log(L"Equalizer APO nélkül az Aqulify csak analizál (WASAPI loopback). Az EQ APO telepítése után futtasd újra a telepítőt.");
        }
        progress(80);

        // 3. Parancsikonok, automatikus indítás
        phase(L"Parancsikonok és beállítások...");
        const std::wstring menuDir = joinPath(knownFolder(FOLDERID_CommonPrograms), L"Aqulify");
        createDirs(menuDir);
        createShortcut(joinPath(menuDir, L"Aqulify.lnk"), exe, L"", L"Aqulify – fázis és vízesés analizátor, DSP", opt.installDir);
        createShortcut(joinPath(menuDir, L"Aqulify eltávolítása.lnk"), uninst, L"/uninstall", L"Aqulify eltávolítása", opt.installDir);
        const std::wstring desk = joinPath(knownFolder(FOLDERID_PublicDesktop), L"Aqulify.lnk");
        if (opt.desktopIcon) createShortcut(desk, exe, L"", L"Aqulify", opt.installDir);
        else DeleteFileW(desk.c_str());

        regDeleteValue(HKEY_LOCAL_MACHINE, kRunKey, L"Aqulify");
        if (opt.autostart) regWriteString(HKEY_CURRENT_USER, kRunKey, L"Aqulify", L"\"" + exe + L"\" --background");
        else regDeleteValue(HKEY_CURRENT_USER, kRunKey, L"Aqulify");
        progress(90);

        // 4. Programok és szolgáltatások
        regWriteString(HKEY_LOCAL_MACHINE, kAppKey, L"InstallDir", opt.installDir);
        regWriteString(HKEY_LOCAL_MACHINE, kAppKey, L"Version", kVersion);
        regWriteString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayName", L"Aqulify");
        regWriteString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayVersion", kVersion);
        regWriteString(HKEY_LOCAL_MACHINE, kUninstallKey, L"Publisher", kPublisher);
        regWriteString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayIcon", exe + L",0");
        regWriteString(HKEY_LOCAL_MACHINE, kUninstallKey, L"InstallLocation", opt.installDir);
        regWriteString(HKEY_LOCAL_MACHINE, kUninstallKey, L"UninstallString", L"\"" + uninst + L"\" /uninstall");
        regWriteString(HKEY_LOCAL_MACHINE, kUninstallKey, L"QuietUninstallString", L"\"" + uninst + L"\" /uninstall /S");
        regWriteDword(HKEY_LOCAL_MACHINE, kUninstallKey, L"NoModify", 1);
        regWriteDword(HKEY_LOCAL_MACHINE, kUninstallKey, L"NoRepair", 1);
        regWriteDword(HKEY_LOCAL_MACHINE, kUninstallKey, L"EstimatedSize", 12 * 1024);
        progress(100);
        phase(L"Kész.");
        res.ok = true;
        CoUninitialize();
        return res;
    }
};

// ------------------------------------------------------------------ eltávolítás
int runUninstall(bool silent) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring installDir = regReadString(HKEY_LOCAL_MACHINE, kAppKey, L"InstallDir");
    if (installDir.empty()) installDir = parentDir(self);

    // Ha a telepítési mappából futunk, ideiglenes másolatból folytatjuk, hogy a mappa törölhető legyen
    const std::wstring cmd = GetCommandLineW();
    if (cmd.find(L"/fromtemp") == std::wstring::npos) {
        wchar_t tmpDir[MAX_PATH];
        GetTempPathW(MAX_PATH, tmpDir);
        const std::wstring tmp = joinPath(tmpDir, L"Aqulify-uninstall-" + std::to_wstring(GetCurrentProcessId()) + L".exe");
        if (CopyFileW(self, tmp.c_str(), FALSE)) {
            MoveFileExW(tmp.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            const std::wstring args = std::wstring(L"/uninstall /fromtemp") + (silent ? L" /S" : L"");
            SHELLEXECUTEINFOW sei{};
            sei.cbSize = sizeof(sei);
            sei.lpFile = tmp.c_str();
            sei.lpParameters = args.c_str();
            sei.nShow = SW_SHOWNORMAL;
            if (ShellExecuteExW(&sei)) return 0;
        }
    }

    if (!silent &&
        MessageBoxW(nullptr, L"Biztosan eltávolítod az Aqulify-t?\n\nAz Equalizer APO megmarad, csak az Aqulify "
                             L"plugin kerül ki a láncából.",
                    L"Aqulify eltávolítása", MB_ICONQUESTION | MB_YESNO) != IDYES) {
        return 1;
    }
    const bool removeData =
        !silent && MessageBoxW(nullptr, L"Törlődjenek a mentett beállítások is (EQ sávok, betöltött impulzusválasz)?",
                               L"Aqulify eltávolítása", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) == IDYES;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Logger log = [](const std::wstring&) {};
    closeRunningEngine(joinPath(installDir, L"Aqulify.exe"));
    removeEqApoIntegration(log);

    DeleteFileW(joinPath(knownFolder(FOLDERID_PublicDesktop), L"Aqulify.lnk").c_str());
    deleteTree(joinPath(knownFolder(FOLDERID_CommonPrograms), L"Aqulify"));
    regDeleteValue(HKEY_CURRENT_USER, kRunKey, L"Aqulify");
    regDeleteValue(HKEY_LOCAL_MACHINE, kRunKey, L"Aqulify");
    regDeleteTree(HKEY_LOCAL_MACHINE, kUninstallKey);
    regDeleteTree(HKEY_LOCAL_MACHINE, kAppKey);
    if (dirExists(installDir) && fileExists(joinPath(installDir, L"Aqulify.exe"))) deleteTree(installDir);
    if (removeData) {
        deleteTree(joinPath(knownFolder(FOLDERID_ProgramData), L"Aqulify"));
        deleteTree(joinPath(knownFolder(FOLDERID_LocalAppData), L"Aqulify"));
    }
    CoUninitialize();
    if (!silent) MessageBoxW(nullptr, L"Az Aqulify eltávolítása kész.", L"Aqulify", MB_ICONINFORMATION | MB_OK);
    return 0;
}

// ------------------------------------------------------------------ varázsló ablak
enum Page { kWelcome, kOptions, kProgress, kFinish };
enum Ctl : int {
    IDC_BACK = 1001, IDC_NEXT, IDC_CANCEL, IDC_BROWSE, IDC_DIR, IDC_CHK_APO, IDC_CHK_DLAPO, IDC_CHK_AUTO,
    IDC_CHK_DESK, IDC_CHK_LAUNCH, IDC_CHK_REBOOT, IDC_PROGRESS, IDC_LOG, IDC_PHASE
};
constexpr UINT WM_INST_LOG = WM_APP + 10;
constexpr UINT WM_INST_PROGRESS = WM_APP + 11;
constexpr UINT WM_INST_PHASE = WM_APP + 12;
constexpr UINT WM_INST_DONE = WM_APP + 13;

class Wizard {
public:
    int run(HINSTANCE inst) {
        inst_ = inst;
        opt_.installDir = defaultInstallDir();
        apo_ = detectEqApo();

        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        ncm.lfMessageFont.lfHeight = -S(13);
        font_ = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW big = ncm.lfMessageFont;
        big.lfHeight = -S(20);
        big.lfWeight = FW_SEMIBOLD;
        fontBig_ = CreateFontIndirectW(&big);
        LOGFONTW title = ncm.lfMessageFont;
        title.lfHeight = -S(24);
        title.lfWeight = FW_BOLD;
        fontTitle_ = CreateFontIndirectW(&title);

        icon_ = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, S(48), S(48), LR_DEFAULTCOLOR));
        HICON smallIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                         GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &Wizard::wndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = L"AqulifySetupWnd";
        wc.hIcon = icon_;
        wc.hIconSm = smallIcon;
        RegisterClassExW(&wc);

        RECT r{ 0, 0, S(620), S(450) };
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRect(&r, style, FALSE);
        const int w = r.right - r.left, h = r.bottom - r.top;
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"Aqulify telepítése", style,
                                (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                                nullptr, nullptr, inst, this);
        if (!hwnd_) return 1;
        createControls();
        showPage(kWelcome);
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        UpdateWindow(hwnd_);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (worker_.joinable()) worker_.join();
        return result_.ok ? 0 : 1;
    }

private:
    HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, std::vector<HWND>* page,
             HFONT f = nullptr) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | style, S(x), S(y), S(w), S(h), hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f ? f : font_), TRUE);
        if (page) page->push_back(c);
        return c;
    }

    void createControls() {
        // Közös gombok
        btnBack_ = add(L"BUTTON", L"< Vissza", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 300, 404, 96, 30, IDC_BACK, nullptr);
        btnNext_ = add(L"BUTTON", L"Tovább >", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 402, 404, 96, 30, IDC_NEXT, nullptr);
        btnCancel_ = add(L"BUTTON", L"Mégse", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 510, 404, 96, 30, IDC_CANCEL, nullptr);

        // 1. Üdvözlés
        add(L"STATIC", L"Üdvözöl az Aqulify telepítője", SS_LEFT, 28, 104, 560, 32, 0, &pWelcome_, fontBig_);
        add(L"STATIC",
            L"Az Aqulify valós idejű 3D akusztikai vízesést, fázis- és csoportkésleltetés-analizátort, "
            L"valamint audiofil DSP láncot ad a Windows hangrendszeréhez (paraméteres és FIR EQ, crossfeed, "
            L"partícionált konvolver, bass enhancer, limiter).\n\n"
            L"A telepítő mindent elvégez: bemásolja a programot, beilleszti a plugint az Equalizer APO láncába, "
            L"létrehozza a parancsikonokat, és rendes eltávolítót ad a Programok és szolgáltatások listájához.",
            SS_LEFT, 28, 144, 560, 130, 0, &pWelcome_);
        lblApoStatus_ = add(L"STATIC", L"", SS_LEFT, 28, 290, 560, 60, 0, &pWelcome_);

        // 2. Beállítások
        add(L"STATIC", L"Telepítési mappa:", SS_LEFT, 28, 104, 300, 20, 0, &pOptions_);
        edDir_ = add(L"EDIT", opt_.installDir.c_str(), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 28, 126, 460, 26, IDC_DIR, &pOptions_);
        add(L"BUTTON", L"Tallózás...", WS_TABSTOP | BS_PUSHBUTTON, 496, 125, 96, 28, IDC_BROWSE, &pOptions_);
        chkApo_ = add(L"BUTTON", L"Beillesztés az Equalizer APO láncba (élő DSP + pontos mérés a lánc végén)",
                      WS_TABSTOP | BS_AUTOCHECKBOX, 28, 170, 570, 24, IDC_CHK_APO, &pOptions_);
        chkDlApo_ = add(L"BUTTON", L"Az Equalizer APO letöltése és telepítése (sourceforge.net, ~12 MB, ingyenes)",
                        WS_TABSTOP | BS_AUTOCHECKBOX, 48, 196, 550, 24, IDC_CHK_DLAPO, &pOptions_);
        chkAuto_ = add(L"BUTTON", L"Indítás a Windows-zal (háttérben, tálcaikonnal)", WS_TABSTOP | BS_AUTOCHECKBOX, 28, 230,
                       570, 24, IDC_CHK_AUTO, &pOptions_);
        chkDesk_ = add(L"BUTTON", L"Parancsikon az Asztalon", WS_TABSTOP | BS_AUTOCHECKBOX, 28, 258, 570, 24, IDC_CHK_DESK, &pOptions_);
        lblOptNote_ = add(L"STATIC", L"", SS_LEFT, 28, 296, 570, 90, 0, &pOptions_);
        SendMessageW(chkApo_, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(chkDlApo_, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(chkAuto_, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(chkDesk_, BM_SETCHECK, BST_CHECKED, 0);

        // 3. Folyamat
        lblPhase_ = add(L"STATIC", L"Előkészítés...", SS_LEFT, 28, 104, 570, 22, IDC_PHASE, &pProgress_);
        prog_ = add(PROGRESS_CLASSW, L"", 0, 28, 132, 564, 20, IDC_PROGRESS, &pProgress_);
        SendMessageW(prog_, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        lstLog_ = add(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOSEL, 28, 164, 564, 222,
                      IDC_LOG, &pProgress_);
        SendMessageW(lstLog_, LB_SETHORIZONTALEXTENT, S(1600), 0);

        // 4. Befejezés
        lblFinTitle_ = add(L"STATIC", L"", SS_LEFT, 28, 104, 560, 32, 0, &pFinish_, fontBig_);
        lblFinText_ = add(L"STATIC", L"", SS_LEFT, 28, 144, 560, 170, 0, &pFinish_);
        chkLaunch_ = add(L"BUTTON", L"Aqulify indítása most", WS_TABSTOP | BS_AUTOCHECKBOX, 28, 330, 560, 24, IDC_CHK_LAUNCH, &pFinish_);
        chkReboot_ = add(L"BUTTON", L"A számítógép újraindítása most (az Equalizer APO aktiválásához szükséges)",
                         WS_TABSTOP | BS_AUTOCHECKBOX, 28, 358, 560, 24, IDC_CHK_REBOOT, &pFinish_);
        SendMessageW(chkLaunch_, BM_SETCHECK, BST_CHECKED, 0);
        updateApoTexts();
    }

    void updateApoTexts() {
        std::wstring st;
        if (apo_.installed) {
            st = L"✔ Equalizer APO telepítve: " + apo_.installPath +
                 L"\nAz Aqulify a lánc végére kerül, így pontosan azt méri, ami a DAC-ra megy.";
        } else {
            st = L"⚠ Az Equalizer APO nincs telepítve. A telepítő letöltheti és elindíthatja a telepítőjét; "
                 L"nélküle az Aqulify csak analizál (a DSP beállítások nem hatnak a hangra).";
        }
        SetWindowTextW(lblApoStatus_, st.c_str());
        ShowWindow(chkDlApo_, (!apo_.installed && page_ == kOptions) ? SW_SHOW : SW_HIDE);
        std::wstring note = apo_.installed
            ? L"A beillesztés egyetlen sort ad az EQ APO config.txt-jéhez (Include: Aqulify.txt), a saját "
              L"szűrőid érintetlenek maradnak. Az eltávolító ezt visszaveszi."
            : L"Az Equalizer APO telepítője külön ablakban indul: a végén jelöld be azt a hangeszközt, amin zenét "
              L"hallgatsz, majd indítsd újra a gépet, amikor kéri.";
        SetWindowTextW(lblOptNote_, note.c_str());
    }

    void showPage(Page p) {
        page_ = p;
        for (auto* v : { &pWelcome_, &pOptions_, &pProgress_, &pFinish_ })
            for (HWND c : *v) ShowWindow(c, SW_HIDE);
        std::vector<HWND>& cur = p == kWelcome ? pWelcome_ : p == kOptions ? pOptions_ : p == kProgress ? pProgress_ : pFinish_;
        for (HWND c : cur) ShowWindow(c, SW_SHOW);
        if (p == kOptions) updateApoTexts();
        if (p == kFinish) ShowWindow(chkReboot_, result_.apoInstalledNow ? SW_SHOW : SW_HIDE);
        if (p == kFinish) ShowWindow(chkLaunch_, result_.ok ? SW_SHOW : SW_HIDE);

        EnableWindow(btnBack_, p == kOptions);
        ShowWindow(btnBack_, p == kFinish || p == kProgress ? SW_HIDE : SW_SHOW);
        SetWindowTextW(btnNext_, p == kOptions ? L"Telepítés" : p == kFinish ? L"Befejezés" : L"Tovább >");
        SetWindowPos(btnNext_, nullptr, S(p == kFinish ? 510 : 402), S(404), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        EnableWindow(btnNext_, p != kProgress);
        ShowWindow(btnCancel_, p == kFinish ? SW_HIDE : SW_SHOW);
        EnableWindow(btnCancel_, p != kProgress);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void browse() {
        IFileOpenDialog* dlg = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                    reinterpret_cast<void**>(&dlg)))) return;
        DWORD fl = 0;
        dlg->GetOptions(&fl);
        dlg->SetOptions(fl | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(L"Telepítési mappa kiválasztása");
        if (SUCCEEDED(dlg->Show(hwnd_))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    std::wstring p = path;
                    CoTaskMemFree(path);
                    if (p.find(L"Aqulify") == std::wstring::npos) p = joinPath(p, L"Aqulify");
                    SetWindowTextW(edDir_, p.c_str());
                }
                item->Release();
            }
        }
        dlg->Release();
    }

    void startInstall() {
        wchar_t buf[MAX_PATH * 2];
        GetWindowTextW(edDir_, buf, MAX_PATH * 2);
        opt_.installDir = buf;
        while (!opt_.installDir.empty() && (opt_.installDir.back() == L'\\' || opt_.installDir.back() == L' ')) opt_.installDir.pop_back();
        if (opt_.installDir.size() < 4) {
            MessageBoxW(hwnd_, L"Adj meg egy érvényes telepítési mappát.", L"Aqulify", MB_ICONWARNING);
            return;
        }
        opt_.integrateApo = SendMessageW(chkApo_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        opt_.downloadApo = SendMessageW(chkDlApo_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        opt_.autostart = SendMessageW(chkAuto_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        opt_.desktopIcon = SendMessageW(chkDesk_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        showPage(kProgress);

        HWND h = hwnd_;
        worker_ = std::thread([this, h] {
            Installer ins;
            ins.log = [h](const std::wstring& s) { PostMessageW(h, WM_INST_LOG, 0, reinterpret_cast<LPARAM>(new std::wstring(s))); };
            ins.progress = [h](int p) { PostMessageW(h, WM_INST_PROGRESS, static_cast<WPARAM>(p), 0); };
            ins.phase = [h](const std::wstring& s) { PostMessageW(h, WM_INST_PHASE, 0, reinterpret_cast<LPARAM>(new std::wstring(s))); };
            auto* r = new Result(ins.run(opt_));
            PostMessageW(h, WM_INST_DONE, 0, reinterpret_cast<LPARAM>(r));
        });
    }

    void finish() {
        if (result_.ok && IsWindowVisible(chkReboot_) && SendMessageW(chkReboot_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            DestroyWindow(hwnd_);
            HANDLE tok;
            if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
                TOKEN_PRIVILEGES tp{};
                LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
                tp.PrivilegeCount = 1;
                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
                CloseHandle(tok);
            }
            ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_INSTALLATION | SHTDN_REASON_FLAG_PLANNED);
            return;
        }
        if (result_.ok && SendMessageW(chkLaunch_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            launchUnelevated(joinPath(opt_.installDir, L"Aqulify.exe"), L"");
        }
        DestroyWindow(hwnd_);
    }

    void paintBanner(HDC dc) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        const int bh = S(84);
        // Átmenetes sáv (mélykék -> lila), mint az ikon háttere
        for (int x = 0; x < rc.right; x += 2) {
            const double t = static_cast<double>(x) / rc.right;
            HBRUSH b = CreateSolidBrush(RGB(14 + static_cast<int>(44 * t), 22 + static_cast<int>(12 * t), 40 + static_cast<int>(52 * t)));
            RECT r{ x, 0, x + 2, bh };
            FillRect(dc, &r, b);
            DeleteObject(b);
        }
        if (icon_) DrawIconEx(dc, S(20), (bh - S(48)) / 2, icon_, S(48), S(48), 0, nullptr, DI_NORMAL);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        HGDIOBJ old = SelectObject(dc, fontTitle_);
        RECT tr{ S(84), S(14), rc.right - S(20), S(46) };
        DrawTextW(dc, L"Aqulify", -1, &tr, DT_LEFT | DT_SINGLELINE);
        SelectObject(dc, font_);
        SetTextColor(dc, RGB(120, 220, 255));
        const wchar_t* sub = page_ == kWelcome ? L"Fázis, spektrum és akusztikai vízesés – Windows 11"
                           : page_ == kOptions ? L"Beállítások"
                           : page_ == kProgress ? L"Telepítés folyamatban..."
                           : L"Befejezés";
        RECT sr{ S(86), S(48), rc.right - S(20), S(72) };
        DrawTextW(dc, sub, -1, &sr, DT_LEFT | DT_SINGLELINE);
        SetTextColor(dc, RGB(150, 150, 160));
        RECT vr{ rc.right - S(200), S(56), rc.right - S(16), S(78) };
        std::wstring v = std::wstring(L"v") + kVersion;
        DrawTextW(dc, v.c_str(), -1, &vr, DT_RIGHT | DT_SINGLELINE);
        SelectObject(dc, old);
        // elválasztó a gombsor felett
        HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DLIGHT));
        HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, 0, S(392), nullptr);
        LineTo(dc, rc.right, S(392));
        SelectObject(dc, op);
        DeleteObject(pen);
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd_, &ps);
                paintBanner(dc);
                EndPaint(hwnd_, &ps);
                return 0;
            }
            case WM_CTLCOLORSTATIC: {
                HDC dc = reinterpret_cast<HDC>(wp);
                SetBkColor(dc, GetSysColor(COLOR_WINDOW));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            }
            case WM_COMMAND:
                switch (LOWORD(wp)) {
                    case IDC_NEXT:
                        if (page_ == kWelcome) showPage(kOptions);
                        else if (page_ == kOptions) startInstall();
                        else if (page_ == kFinish) finish();
                        return 0;
                    case IDC_BACK:
                        if (page_ == kOptions) showPage(kWelcome);
                        return 0;
                    case IDC_CANCEL:
                        if (page_ != kProgress &&
                            MessageBoxW(hwnd_, L"Megszakítod a telepítést?", L"Aqulify", MB_ICONQUESTION | MB_YESNO) == IDYES)
                            DestroyWindow(hwnd_);
                        return 0;
                    case IDC_BROWSE: browse(); return 0;
                    case IDC_CHK_APO:
                        EnableWindow(chkDlApo_, SendMessageW(chkApo_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                        return 0;
                }
                break;
            case WM_INST_LOG: {
                std::unique_ptr<std::wstring> s(reinterpret_cast<std::wstring*>(lp));
                const LRESULT i = SendMessageW(lstLog_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s->c_str()));
                SendMessageW(lstLog_, LB_SETTOPINDEX, static_cast<WPARAM>(i), 0);
                return 0;
            }
            case WM_INST_PHASE: {
                std::unique_ptr<std::wstring> s(reinterpret_cast<std::wstring*>(lp));
                SetWindowTextW(lblPhase_, s->c_str());
                return 0;
            }
            case WM_INST_PROGRESS:
                SendMessageW(prog_, PBM_SETPOS, wp, 0);
                return 0;
            case WM_INST_DONE: {
                std::unique_ptr<Result> r(reinterpret_cast<Result*>(lp));
                result_ = *r;
                if (worker_.joinable()) worker_.join();
                if (result_.ok) {
                    SetWindowTextW(lblFinTitle_, L"Az Aqulify telepítése kész");
                    std::wstring t;
                    if (result_.apoIntegrated) {
                        t = L"A plugin a helyére került az Equalizer APO láncában, a vízesés és a DSP élőben működik.\n\n";
                    } else {
                        t = L"Az Aqulify analizátor módban fut (WASAPI loopback). Az élő DSP-hez telepítsd az "
                            L"Equalizer APO-t, majd futtasd újra ezt a telepítőt.\n\n";
                    }
                    if (result_.apoInstalledNow)
                        t += L"Az Equalizer APO most települt: a hangeszközön csak újraindítás után lép életbe.\n\n";
                    t += L"Az Aqulify a tálcán (értesítési területen) fut: dupla kattintás nyitja a felületet, "
                         L"jobb kattintás a menüt. A Start menüből is elérhető.";
                    SetWindowTextW(lblFinText_, t.c_str());
                    if (result_.apoInstalledNow) {
                        SendMessageW(chkReboot_, BM_SETCHECK, BST_CHECKED, 0);
                        SendMessageW(chkLaunch_, BM_SETCHECK, BST_UNCHECKED, 0);
                    }
                } else {
                    SetWindowTextW(lblFinTitle_, L"A telepítés nem sikerült");
                    SetWindowTextW(lblFinText_, (result_.message + L"\n\nA részletek a telepítési naplóban láthatók.").c_str());
                }
                showPage(kFinish);
                return 0;
            }
            case WM_CLOSE:
                if (page_ == kProgress) return 0;
                if (page_ == kFinish ||
                    MessageBoxW(hwnd_, L"Megszakítod a telepítést?", L"Aqulify", MB_ICONQUESTION | MB_YESNO) == IDYES)
                    DestroyWindow(hwnd_);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            static_cast<Wizard*>(cs->lpCreateParams)->hwnd_ = h;
        }
        auto* self = reinterpret_cast<Wizard*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        return self ? self->handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
    }

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr, fontBig_ = nullptr, fontTitle_ = nullptr;
    HICON icon_ = nullptr;
    Page page_ = kWelcome;
    Options opt_;
    EqApoInfo apo_;
    Result result_;
    std::thread worker_;
    std::vector<HWND> pWelcome_, pOptions_, pProgress_, pFinish_;
    HWND btnBack_{}, btnNext_{}, btnCancel_{}, edDir_{}, chkApo_{}, chkDlApo_{}, chkAuto_{}, chkDesk_{}, lblOptNote_{};
    HWND lblApoStatus_{}, lblPhase_{}, prog_{}, lstLog_{}, lblFinTitle_{}, lblFinText_{}, chkLaunch_{}, chkReboot_{};
};

bool hasSwitch(const std::wstring& cmd, const wchar_t* sw) {
    std::wstring low = cmd, s = sw;
    for (auto& c : low) c = static_cast<wchar_t>(towlower(c));
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return low.find(s) != std::wstring::npos;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();
    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const std::wstring cmd = GetCommandLineW();
    const bool silent = hasSwitch(cmd, L" /s");
    int rc = 0;
    if (hasSwitch(cmd, L"/uninstall")) {
        rc = runUninstall(silent);
    } else if (silent) {
        Options opt;
        opt.installDir = defaultInstallDir();
        const size_t d = cmd.find(L"/D=");
        if (d != std::wstring::npos) opt.installDir = cmd.substr(d + 3);
        opt.integrateApo = !hasSwitch(cmd, L"/noapo");
        opt.downloadApo = hasSwitch(cmd, L"/downloadapo");
        opt.desktopIcon = false;
        opt.silent = true;
        Installer ins;
        ins.log = [](const std::wstring&) {};
        ins.progress = [](int) {};
        ins.phase = [](const std::wstring&) {};
        rc = ins.run(opt).ok ? 0 : 1;
    } else {
        // Egyszerre csak egy telepítő fusson
        HANDLE m = CreateMutexW(nullptr, TRUE, L"Global\\AqulifySetup");
        if (m && GetLastError() == ERROR_ALREADY_EXISTS) {
            MessageBoxW(nullptr, L"Az Aqulify telepítője már fut.", L"Aqulify", MB_ICONINFORMATION);
            return 1;
        }
        Wizard w;
        rc = w.run(inst);
        if (m) CloseHandle(m);
    }
    CoUninitialize();
    return rc;
}
