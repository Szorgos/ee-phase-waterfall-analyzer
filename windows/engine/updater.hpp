#pragma once
/**
 * @file updater.hpp
 * @brief Egykattintásos frissítés Windowson. A felület kérésére (csak a helyi felületről, a hálózati távirányítóról nem)
 *        letölti a kiadás telepítőjét a saját GitHub-kiadásból (rögzített cím: update_logic.hpp), egyezteti a kiadás
 *        .sha256 ellenőrzőösszegével, majd csendes módban elindítja (/S /launch). A telepítő rendszergazdai engedélyt
 *        kér (UAC), leállítja a motort és a felületet, telepít, és újraindítja az Aqulify-t.
 *
 * Az állapotot (fázis, letöltött bájtok, hiba) a motor az állapotüzenet "update" mezőjében küldi (statusJson); az a
 * híd saját szálán épül, így a frissítő szála nem küld üzenetet közvetlenül. Egyszerre egy frissítés fut.
 */
#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sha256.hpp"
#include "update_logic.hpp"
#include "../common/win_download.hpp"

namespace ee_analyzer::win {

class Updater {
public:
    ~Updater() { stop(); }

    /** Frissítés a megadott verzióra; false: érvénytelen verzió (error = "version") vagy már fut ("busy"). */
    bool start(const std::string& version, std::string& error) {
        if (!update::validVersion(version)) {
            error = "version";
            return false;
        }
        std::lock_guard<std::mutex> lk(m_);
        if (busy_) {
            error = "busy";
            return false;
        }
        if (thread_.joinable()) thread_.join();   // az előző, már befejezett futás
        busy_ = true;
        cancel_ = false;
        version_ = version;
        phase_ = "download";
        error_.clear();
        got_ = total_ = 0;
        thread_ = std::thread([this, version] { run(version); });
        return true;
    }

    /** A motor kilépésekor: a letöltés megszakad, a szál véget ér. */
    void stop() {
        cancel_ = true;
        if (thread_.joinable()) thread_.join();
    }

    /** Az állapotüzenet "update" mezője (vesszővel kezdve); üres, ha ebben a futásban nem volt frissítés. */
    std::string statusJson() const {
        std::lock_guard<std::mutex> lk(m_);
        if (phase_.empty()) return {};
        return ",\"update\":{\"version\":\"" + version_ + "\",\"phase\":\"" + phase_ + "\",\"got\":" + std::to_string(got_) +
               ",\"total\":" + std::to_string(total_) + ",\"error\":\"" + error_ + "\"}";
    }

private:
    void set(const char* phase, uint64_t got, uint64_t total) {
        std::lock_guard<std::mutex> lk(m_);
        phase_ = phase;
        got_ = got;
        total_ = total;
    }

    static std::wstring tempDir() {
        wchar_t buf[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, buf);
        std::wstring d = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring(L".\\");
        d += L"Aqulify-update";
        CreateDirectoryW(d.c_str(), nullptr);
        return d;
    }

    static std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }   // csak ASCII (cím, verzió)

    static std::string readSmall(const std::wstring& path) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return {};
        char buf[4096];
        DWORD r = 0;
        const BOOL ok = ReadFile(f, buf, sizeof(buf), &r, nullptr);
        CloseHandle(f);
        return ok ? std::string(buf, r) : std::string();
    }

    static std::string fileSha256(const std::wstring& path) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f == INVALID_HANDLE_VALUE) return {};
        Sha256 h;
        std::vector<char> buf(64 * 1024);
        DWORD r = 0;
        bool ok = true;
        for (;;) {
            if (!ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &r, nullptr)) { ok = false; break; }
            if (r == 0) break;
            h.update(buf.data(), r);
        }
        CloseHandle(f);
        return ok ? h.hexDigest() : std::string();
    }

    /** A letöltés, az ellenőrzés és az indítás; üres: sikerült, különben a hiba kódja. */
    std::string work(const std::string& v, const std::wstring& exe, const std::wstring& sum) {
        if (!downloadFile(widen(update::checksumUrl(v)), sum, nullptr, &cancel_)) return cancel_ ? "cancelled" : "download";
        const std::string want = update::parseChecksum(readSmall(sum));
        if (want.empty()) return "checksum";
        ULONGLONG last = 0;
        const bool got = downloadFile(widen(update::installerUrl(v)), exe, [&](uint64_t done, uint64_t total) {
            const ULONGLONG now = GetTickCount64();
            if (now - last >= 200) {
                last = now;
                set("download", done, total);
            }
        }, &cancel_);
        if (!got) return cancel_ ? "cancelled" : "download";
        set("verify", 0, 0);
        if (!looksLikeExe(exe) || fileSha256(exe) != want) return "checksum";
        set("launch", 0, 0);
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOASYNC;
        sei.lpVerb = L"open";            // a telepítő maga kér rendszergazdai jogot (requireAdministrator)
        sei.lpFile = exe.c_str();
        sei.lpParameters = L"/S /launch";
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei)) return GetLastError() == ERROR_CANCELLED ? "uac" : "launch";
        return {};
    }

    void run(const std::string& v) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const std::wstring exe = tempDir() + L"\\Aqulify-Setup-" + widen(v) + L".exe";
        const std::wstring sum = exe + L".sha256";
        DeleteFileW(exe.c_str());
        DeleteFileW(sum.c_str());
        std::cout << "[Frissítés] " << v << ": letöltés innen: " << update::installerUrl(v) << "\n";
        const std::string err = work(v, exe, sum);
        DeleteFileW(sum.c_str());
        if (err.empty()) {
            std::cout << "[Frissítés] A telepítő elindult; a motor a telepítés idejére leáll, utána újraindul.\n";
        } else {
            DeleteFileW(exe.c_str());
            std::cout << "[Frissítés] Nem sikerült: " << err << ".\n";
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            phase_ = err.empty() ? "installing" : "error";
            error_ = err;
            busy_ = false;
        }
        CoUninitialize();
    }

    mutable std::mutex m_;
    std::thread thread_;
    bool busy_ = false;
    std::atomic<bool> cancel_{false};   // a letöltés megszakítása (stop, másik szálról); a downloadFile csak olvassa
    std::string version_, phase_, error_;
    uint64_t got_ = 0, total_ = 0;
};

} // namespace ee_analyzer::win
