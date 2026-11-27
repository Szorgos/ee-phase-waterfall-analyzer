/**
 * @file aether_engine.cpp
 * @brief Aqulify.exe – Windows 11 motor a fázis- és vízesés-analizátorhoz.
 *
 * Jelforrások (automatikus választás):
 *   1. Aqulify EQ APO plugin (közös memória) – a feldolgozott jel pontosan a láncban lévő helyén,
 *      és a GUI-ról állított DSP (preamp, EQ, crossfeed, limiter) a plugin-ban, élőben hat a hangra.
 *   2. WASAPI loopback – az alapértelmezett lejátszóeszköz kimenete (csak analízis, plugin nélkül).
 *
 * A GUI (gui_integrated_system.html) a ws://127.0.0.1:47810 (foglaltság esetén 47811-47814, végül 8080) hídon csatlakozik.
 *
 * Konzolablak nélküli háttéralkalmazás tálcaikonnal (dupla katt: felület, jobb klikk: menü).
 * Kapcsolók: --background (indításkor nem nyitja meg a felületet), --console (naplózás konzolba), --port N
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
// Az llvm-mingw shlwapi.h-jából hiányzik (Windows 8 óta; értéke a Microsoft leírása szerint 0x1000)
#ifndef ASSOCF_IS_PROTOCOL
#define ASSOCF_IS_PROTOCOL static_cast<ASSOCF>(0x00001000)
#endif
#include <iphlpapi.h>
#include <psapi.h>
#include <xmmintrin.h>
#include <pmmintrin.h>

#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <cstring>

#include "ws_bridge.hpp"
#include "pair_cli.hpp"
#include "startup_restore.hpp"
#include "param_block.hpp"
#include "aether_chain.hpp"
#include "shared_state.hpp"
#include "aq_i18n.hpp"
#include "../common/win_shared.hpp"
#include "../common/eqapo_paths.hpp"
#include "dsp_state.hpp"
#include "wasapi_loopback.hpp"
#include "apo_detect.hpp"
#include <endpointvolume.h>
#include "tray.hpp"
#include "media_smtc.hpp"
#include "audio_options.hpp"
#include "log_debounce.hpp"
#include "load_advisor.hpp"
#include "file_player.hpp"
#include "library_bridge.hpp"
#include "radio_bridge.hpp"
#include "upnp_bridge.hpp"
#include "player_output_wasapi.hpp"
#include "player_output_win.hpp"
#include "win_live_bridge.hpp"

namespace ee_analyzer::player {
/** A motor kimenete: WASAPI (megosztott / kizárólagos) vagy ASIO, a haladó hangkimenet beállítása szerint. */
using WinPlayerOutput = BasicWinPlayerOutput<WasapiPlayerOutput>;
}
#include "updater.hpp"
#include "health_monitor.hpp"
#include "report_sender.hpp"
#include "device_format.hpp"

using namespace ee_analyzer;

namespace {
// A „most szól” sáv hangerő-csúszkája: az alapértelmezett lejátszóeszköz hangereje (mint a tálca hangerő-szabályzójáé)
const IID kIID_IAudioEndpointVolume = {0x5CDF2C82, 0x841E, 0x4546, {0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};

template <class F>
bool withEndpointVolume(F&& fn, const std::wstring& deviceId = {}) {
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    {
        win::ComPtr<IMMDeviceEnumerator> en;
        win::ComPtr<IMMDevice> dev;
        win::ComPtr<IAudioEndpointVolume> vol;
        if (SUCCEEDED(CoCreateInstance(win::kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, win::kIID_IMMDeviceEnumerator,
                                       reinterpret_cast<void**>(en.put())))) {
            const HRESULT ghr = deviceId.empty()
                ? en->GetDefaultAudioEndpoint(eRender, eConsole, dev.put())
                : en->GetDevice(deviceId.c_str(), dev.put());
            if (SUCCEEDED(ghr) &&
                SUCCEEDED(dev->Activate(kIID_IAudioEndpointVolume, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(vol.put())))) {
                ok = fn(vol.p);
            }
        }
    }
    if (SUCCEEDED(ci)) CoUninitialize();
    return ok;
}

std::string endpointVolumeJson(const std::wstring& deviceId = {}) {
    float level = 0.0f;
    BOOL muted = FALSE;
    if (!withEndpointVolume([&](IAudioEndpointVolume* v) {
            return SUCCEEDED(v->GetMasterVolumeLevelScalar(&level)) && SUCCEEDED(v->GetMute(&muted));
        }, deviceId))
        return {};
    return ",\"volume\":{\"level\":" + std::to_string(level) + ",\"muted\":" + (muted ? "true" : "false") + "}";
}

/**
 * Van-e az eszköznek hardveres hangereje. Ha nem olvasható, nincs (false): a csúszka ilyenkor a digitális úton
 * szabályoz, és nem hat csendben egy olyan hangerőre, amelyet nem lehet állítani.
 */
bool endpointHardwareVolume(const std::wstring& deviceId = {}) {
    DWORD mask = 0;
    if (!withEndpointVolume([&](IAudioEndpointVolume* v) { return SUCCEEDED(v->QueryHardwareSupport(&mask)); }, deviceId))
        return false;
    return (mask & ENDPOINT_HARDWARE_SUPPORT_VOLUME) != 0;
}

void setEndpointVolume(double level, int mute, const std::wstring& deviceId = {}) {
    withEndpointVolume([&](IAudioEndpointVolume* v) {
        if (level >= 0.0) v->SetMasterVolumeLevelScalar(static_cast<float>(std::min(1.0, level)), nullptr);
        if (mute >= 0) v->SetMute(mute ? TRUE : FALSE, nullptr);
        return true;
    }, deviceId);
}
} // namespace

namespace {

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

void openGui() {
    const std::wstring dir = exeDirectory();
    // Saját ablakos felület (WebView2); ha nincs meg, a böngészős megnyitás a tartalék
    const std::wstring app = dir + L"\\AqulifyGui.exe";
    if (fileExists(app)) {
        const INT_PTR r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", app.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL));
        if (r > 32) {
            std::cout << "[GUI] Felület megnyitva saját ablakban.\n";
            return;
        }
        std::cout << "[GUI] Az AqulifyGui.exe nem indítható (ShellExecute: " << static_cast<long long>(r)
                  << ", GetLastError: " << GetLastError() << ").\n";
    } else {
        std::cout << "[GUI] Az AqulifyGui.exe hiányzik a motor mellől (telepítsd újra; a vírusirtó karanténba tehette).\n";
    }
    for (const std::wstring& candidate : { dir + L"\\gui\\gui_integrated_system.html",
                                           dir + L"\\gui_integrated_system.html",
                                           dir + L"\\..\\gui_integrated_system.html" }) {
        if (fileExists(candidate)) {
            // Saját alkalmazásablak az Edge-dzsel (--app: fülek és címsor nélkül); végső tartalék a böngésző
            std::wstring url = L"file:///" + candidate;
            for (auto& c : url) if (c == L'\\') c = L'/';
            const std::wstring args = L"--app=\"" + url + L"\" --window-size=1480,940";
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"msedge.exe", args.c_str(), nullptr, SW_SHOWNORMAL)) > 32) {
                std::cout << "[GUI] Felület megnyitva saját ablakban (Edge alkalmazásmód).\n";
                return;
            }
            ShellExecuteW(nullptr, L"open", candidate.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            std::cout << "[GUI] Grafikus felület megnyitva az alapértelmezett böngészőben.\n";
            return;
        }
    }
    std::cout << "[GUI] A gui_integrated_system.html nem található az exe mellett (gui\\ mappa).\n";
}


// A GUI ugyanezt a listát próbálja végig (gui_integrated_system.html: ENGINE_PORTS).
// A 8080 csak tartalék: gyakran foglalja más program (fejlesztői szerverek, proxyk).
const int kEnginePorts[] = { 47810, 47811, 47812, 47813, 47814, 8080 };

// Melyik folyamat figyel az adott 127.0.0.1 / 0.0.0.0 TCP porton? (üres, ha nem deríthető ki)
std::wstring portOwner(int port) {
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    std::vector<unsigned char> buf(size + 1024);
    size = static_cast<ULONG>(buf.size());
    if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) return {};
    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buf.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (ntohs(static_cast<u_short>(row.dwLocalPort)) != port) continue;
        std::wstring name = L"PID " + std::to_wstring(row.dwOwningPid);
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, row.dwOwningPid)) {
            wchar_t path[MAX_PATH];
            DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, path, &n)) {
                std::wstring full(path, n);
                const size_t sl = full.find_last_of(L"\\/");
                name = (sl == std::wstring::npos ? full : full.substr(sl + 1)) + L" (" + name + L")";
            }
            CloseHandle(h);
        }
        return name;
    }
    return {};
}

// ---------------------------------------------------------------- EQ profilok, hálózati elérés
// A felület JSON dokumentuma: %APPDATA%\Aqulify\presets.json (felhasználónként, újraindítás után is megmarad);
// a hálózati elérés beállításai és a párosított eszközök: %APPDATA%\Aqulify\remote.json
std::wstring userFilePath(const wchar_t* name, bool createDir) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : win::dataDirectory(createDir);
    dir += L"\\Aqulify";
    if (createDir) CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\" + name;
}

std::wstring presetFilePath(bool createDir) { return userFilePath(L"presets.json", createDir); }

std::string readSmallFile(const std::wstring& path, size_t maxBytes) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string out;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart <= static_cast<LONGLONG>(maxBytes)) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &read, nullptr)) read = 0;
        out.resize(read);
    }
    CloseHandle(f);
    return out;
}

bool writeFileAtomic(const std::wstring& path, const std::string& doc) {
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(f, doc.data(), static_cast<DWORD>(doc.size()), &written, nullptr);
    CloseHandle(f);
    if (!ok || written != doc.size()) return false;
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::string loadPresetFile() {
    // Régi név (AetherAudio, 1.4.8-ig): a profilok egyszeri átvétele
    {
        const std::wstring now = presetFilePath(false);
        std::wstring old = now;
        const size_t k = old.rfind(L"\\Aqulify\\");
        if (k != std::wstring::npos && GetFileAttributesW(now.c_str()) == INVALID_FILE_ATTRIBUTES) {
            old.replace(k, 9, std::wstring(L"\\Aether") + L"Audio\\");
            if (GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES) {
                presetFilePath(true);
                CopyFileW(old.c_str(), now.c_str(), TRUE);
            }
        }
    }
    return readSmallFile(presetFilePath(false), kBridgeMaxPresetBytes);
}

bool savePresetFile(const std::string& doc) { return writeFileAtomic(presetFilePath(true), doc); }

// IR-tár: az IR-profilok impulzusválaszai (%APPDATA%\Aqulify\irs\<azonosító>.bin, az ir.bin formátumában)
std::wstring libraryIrPath(const std::string& id, bool createDir) {
    const std::wstring dir = userFilePath(L"irs", createDir);
    if (createDir) CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\" + std::wstring(id.begin(), id.end()) + L".bin";   // az azonosító csak 0-9 és a-z
}

std::string loadLibraryIr(const std::string& id) {
    return isValidIrId(id) ? readSmallFile(libraryIrPath(id, false), 8u << 20) : std::string();
}

bool saveLibraryIr(const std::string& id, const std::string& bytes) {
    return isValidIrId(id) && !bytes.empty() && writeFileAtomic(libraryIrPath(id, true), bytes);
}

bool removeLibraryIr(const std::string& id) {
    return isValidIrId(id) && DeleteFileW(libraryIrPath(id, false).c_str()) != 0;
}

bool hasArg(int argc, char** argv, const char* a) {
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], a) == 0) return true;
    return false;
}

/**
 * A beépített lejátszó saját DSP-lánca a kizárólagos módhoz (WASAPI exclusive): ilyenkor az Equalizer APO kimarad,
 * ezért a lánc itt, a lejátszó kimenetének szálán fut (mint macOS-en). A beállítások és az IR ugyanazok, mint amelyeket
 * a motor a pluginnak küld; a feldolgozott jel a vízesésnek és a mérőknek a saját lehallgatóba kerül.
 */
class PlayerDsp : public player::ExclusiveDsp {
public:
    explicit PlayerDsp(const ParamData& initial = transparentParams()) : tap_(std::make_unique<SharedTapRing>()) {
        chain_.setSampleRate(48000);
        chain_.startWorker();
        params_.write(initial);
    }
    ~PlayerDsp() override { chain_.stopWorker(); }

    void setParams(const ParamData& p) { params_.write(p); }
    void submitIr(IrData ir) { chain_.submitIr(std::move(ir)); }
    /** A felhasználó választása: a DAC hardveres hangereje (true) vagy a digitális (false). A következő számtól hat. */
    void setHardwarePref(bool on) { hwPref_ = on; }
    /** Az éppen használt valódi kimeneti végpont azonosítója (üres = rendszer alapértelmezettje; a hídnál a valódi DAC). */
    void setActiveDeviceFn(std::function<std::wstring()> fn) {
        std::lock_guard<std::mutex> lk(devFnMu_);
        activeDeviceFn_ = std::move(fn);
    }

    /**
     * Digitális hangerő (a felület csúszkája, 0..1) és némítás. Csak hardveres hangerő nélküli eszközön hat; ott a lánc
     * végén (a limiter után, a dither előtt) szabályoz. Hardveres hangerőnél a lánc kimenete érintetlen (bitpontos).
     */
    void setVolume(double level, int mute) {
        if (level >= 0.0) level_ = static_cast<float>(std::clamp(level, 0.0, 1.0));
        if (mute >= 0) muted_ = mute != 0;
        applyVolume();
    }
    bool hardwareVolume() const { return hwVolume_.load(); }
    std::string volumeJson() const {
        return ",\"volume\":{\"level\":" + std::to_string(level_.load()) + ",\"muted\":" + (muted_.load() ? "true" : "false") +
               ",\"digital\":true}";
    }
    /** A digitális hangerő módosítja-e most a jelet (nem bitpontos). */
    bool volumeTouches() const { return chain_.outputGain() != 1.0f; }

    // ---- player::ExclusiveDsp (a kimenet szálán)
    void prepare(uint32_t rate, uint32_t blockFrames, int ditherBits) override {
        // a hardveres út csak akkor, ha a felhasználó azt választotta és a valódi céleszköz tényleg tud hangerőt
        std::wstring devId;
        {
            std::lock_guard<std::mutex> lk(devFnMu_);
            if (activeDeviceFn_) devId = activeDeviceFn_();
        }
        hwVolume_ = hwPref_.load() && endpointHardwareVolume(devId);
        applyVolume();
        std::cout << "[Lejátszó] Hangerő kizárólagos módban: "
                  << (hwVolume_ ? "az eszköz hardveres hangereje" : "digitális (a DSP-lánc végén)")
                  << (hwPref_.load() == hwVolume_ ? "" : " (a hardveres hangerő nem érhető el)") << ".\n";
        chain_.setSampleRate(rate);
        chain_.reset();
        rate_ = rate;
        bits_ = ditherBits;
        appliedSeq_ = 0;                 // a beállítás újra, az új bitmélységgel
        L_.assign(blockFrames, 0.0f);
        R_.assign(blockFrames, 0.0f);
    }

    void process(float* lr, size_t n) override {
        const unsigned int oldCsr = _mm_getcsr();
        _mm_setcsr(oldCsr | 0x8040);     // FTZ | DAZ (mint a pluginban)
        const uint32_t seq = params_.currentSeq();
        if (seq != appliedSeq_ && (seq & 1u) == 0) {
            ParamData pd;
            uint32_t readSeq = 0;
            if (params_.tryRead(pd, readSeq)) {
                if (bits_) pd.ditherBits = bits_;   // a dither a kimenet tényleges bitmélységére
                chain_.applyParamsRT(pd);
                transparent_.store(!AetherChain::modifiesSignal(pd), std::memory_order_relaxed);
                appliedSeq_ = readSeq;
            }
        }
        if (L_.size() < n) { L_.resize(n); R_.resize(n); }   // nem várt blokkméret (nem fordulhat elő)
        for (size_t i = 0; i < n; ++i) { L_[i] = lr[2 * i]; R_[i] = lr[2 * i + 1]; }
        chain_.process(L_.data(), R_.data(), n);
        for (size_t i = 0; i < n; ++i) { lr[2 * i] = L_[i]; lr[2 * i + 1] = R_[i]; }
        tap_->write(L_.data(), R_.data(), static_cast<uint32_t>(n));
        gr_.store(static_cast<float>(chain_.gainReductionDb()), std::memory_order_relaxed);
        bass_.store(static_cast<float>(chain_.bassHarmonicsDb()), std::memory_order_relaxed);
        latency_.store(chain_.eqLatencySamples() + chain_.limiterLatencySamples() +
                           (chain_.params().has(kFlagConvolver) && chain_.convolverActive() ? ee::ZitaInstance::latencyFor(static_cast<uint32_t>(n)) : 0),
                       std::memory_order_relaxed);
        lastData_.store(GetTickCount64(), std::memory_order_relaxed);
        _mm_setcsr(oldCsr);
    }

    size_t latencySamples() const override { return latency_.load(std::memory_order_relaxed); }

    // ---- a motornak (bármely szálról)
    const SharedTapRing& tap() const { return *tap_; }
    uint32_t sampleRate() const { return rate_.load(); }
    double gainReductionDb() const { return gr_.load(); }
    double bassHarmonicsDb() const { return bass_.load(); }
    bool transparent() const { return transparent_.load(); }
    uint64_t lastDataMs() const { return lastData_.load(); }
    std::string magicDiagJson() const {
        magic::MagicDiag d;
        return chain_.magicDiag(d) ? magic::diagToJson(d) : std::string();
    }

private:
    void applyVolume() {
        const float l = level_.load();
        // hallás szerinti skála: 0 → néma, 1 → 0 dB, közte dB-ben egyenletesen (-50 dB-ig)
        float g = muted_.load() || l <= 0.0f ? 0.0f : (l >= 1.0f ? 1.0f : std::pow(10.0f, (l - 1.0f) * 50.0f / 20.0f));
        if (hwVolume_.load()) g = 1.0f;
        chain_.setOutputGain(g);
    }

    AetherChain chain_;
    ParamBlock params_;
    std::unique_ptr<SharedTapRing> tap_;
    std::vector<float> L_, R_;
    uint32_t appliedSeq_ = 0;
    int bits_ = 0;
    std::atomic<uint32_t> rate_{48000};
    std::atomic<float> gr_{0.0f}, bass_{-100.0f};
    std::atomic<bool> transparent_{true};
    std::atomic<size_t> latency_{0};
    std::atomic<uint64_t> lastData_{0};
    std::atomic<float> level_{1.0f};
    std::atomic<bool> muted_{false};
    std::atomic<bool> hwVolume_{false}, hwPref_{false};
    std::mutex devFnMu_;
    std::function<std::wstring()> activeDeviceFn_;
};

/** Az EQ APO plugin közös memóriájának figyelése és a jelforrás kiválasztása. */
class SourceManager {
public:
    SourceManager(win::WasapiLoopback& lb, PlayerDsp& dsp, player::WinPlayerOutput& out)
        : loopback_(lb), player_(dsp), playerOut_(out), apoDevice_(loadApoDevice()) {}

    /**
     * A rendszerhang-híd (win_live_bridge.hpp) állapota. Amíg a híd fut, a programok hangja a virtuális kábelen át a
     * motor DSP-láncába (PlayerDsp) jut, és onnan kizárólagos módban a valódi eszközre: a DSP a motorban fut. A kábelen
     * is ott lehet az Equalizer APO a pluginnal; az ilyenkor átengedő beállítást kap, különben a lánc kétszer futna le.
     */
    void setBridgeActiveFn(std::function<bool()> fn) {
        std::lock_guard<std::mutex> lk(mutex_);
        bridgeActiveFn_ = std::move(fn);
    }

    /** A pluginnak szánt beállítás: a híd alatt minden modul ki (bitpontos átengedés), egyébként a felület lánca. */
    static ParamData pluginParamsFor(const ParamData& p, bool bridgeActive) {
        if (!bridgeActive) return p;
        ParamData q = p;
        q.flags &= kFlagInitialized;
        return q;
    }

    /** Rendszeresen hívandó (pl. 250 ms-onként) a fő szálból. */
    void poll(const ParamData& currentParams) {
        std::lock_guard<std::mutex> lk(mutex_);
        lastParams_ = currentParams;   // a motor mostani lánca (a pushParams is frissíti)
        // A rendszerhang-híd indulása / leállása: a forrás és a plugin beállítása ehhez igazodik
        const bool bridge = bridgeActiveFn_ && bridgeActiveFn_() && !playerOut_.exclusiveActive();
        if (bridge != bridgeActive_) {
            bridgeActive_ = bridge;
            std::cout << (bridge ? "[Forrás] Rendszerhang-híd: a DSP a motorban fut (a kábelen a plugin átenged).\n"
                                 : "[Forrás] A rendszerhang-híd leállt: a DSP ismét az Equalizer APO pluginban fut.\n");
            if (mapping_.state()) mapping_.state()->params.write(pluginParamsFor(lastParams_, bridgeActive_));
        }
        if (!mapping_.state()) {
            if (mapping_.open()) {
                std::cout << "[Plugin] Aqulify EQ APO plugin közös memória megtalálva ("
                          << (mapping_.isGlobal() ? "Global" : "Local") << ").\n";
                // A GUI-ban beállított, mentett lánc átadása a pluginnak
                // (a régi, leállás előtti lánc nem maradhat élesen; a híd alatt átengedés)
                mapping_.state()->params.write(pluginParamsFor(lastParams_, bridgeActive_));
                {
                    std::lock_guard<std::mutex> ik(irMutex_);
                    if (!ir_.empty()) win::writeIrBlock(mapping_.state()->ir, ir_);
                }
                pluginReadPos_ = mapping_.state()->tap.writePos.load();
            }
        }
        // A lejátszó kizárólagos módban: a hang nem megy át a pluginon, a mérés a lejátszó saját láncából jön
        const bool excl = playerOut_.exclusiveActive();
        if (excl != playerActive_) {
            playerActive_ = excl;
            std::cout << (excl ? "[Forrás] Beépített lejátszó, kizárólagos mód (a DSP a motorban fut).\n"
                               : "[Forrás] A kizárólagos mód véget ért.\n");
            playerReadPos_ = player_.tap().writePos.load();
            lastPlayerData_ = GetTickCount64();
            if (!excl) {
                if (pluginActive_ && mapping_.state()) pluginReadPos_ = mapping_.state()->tap.writePos.load();
                loopbackReadPos_ = loopback_.ring().writePos.load();
            }
        }
        refreshApoOnDeviceLocked();
        // A plugin csak akkor a forrás, ha a mostani eszköz hangját dolgozza fel: ha ezen az eszközön nincs Equalizer
        // APO (pl. új USB DAC), a plugin egy másik eszközön fut, a zene feldolgozás nélkül szól. Ilyenkor a vízesés a
        // loopbackből kap jelet, és a felület megmondja, mit kell bekapcsolni.
        const bool rawPluginAlive = pluginAliveLocked();
        const bool alive = pluginServesDevice(rawPluginAlive, apoOnDevice_);
        if (alive != pluginActive_) {
            pluginActive_ = alive;
            if (alive) pluginReadPos_ = mapping_.state()->tap.writePos.load();
            else loopbackReadPos_ = loopback_.ring().writePos.load();
            playback_.reset();   // a lejátszás mérése a plugin leállásától indul
        }
        // A [Forrás] naplósor hiszterézissel (2,5 s) íródik, hogy a pillanatnyi audiodg/plugin újrapéldányosítás
        // ne szemetelje tele a naplót (a tényleges jelforrás-váltás fent azonnal megtörténik).
        const int srcKind = alive ? kSrcApo : (rawPluginAlive ? kSrcLoopbackNoApo : kSrcLoopback);
        int loggedKind = 0;
        // a híd alatt a forrás a motor (fent naplózva); a kábel plugin / loopback váltakozása nem érdekes
        if (sourceLog_.update(srcKind, GetTickCount64(), loggedKind) && !bridgeActive_) {
            if (loggedKind == kSrcLoopbackNoApo)
                std::cout << "[Forrás] A mostani eszközön nincs bekapcsolva az Equalizer APO: WASAPI loopback (csak "
                             "analízis); a plugin egy másik eszközön fut.\n";
            else if (loggedKind == kSrcApo)
                std::cout << "[Forrás] EQ APO plugin jel aktív (DSP élőben vezérelhető).\n";
            else
                std::cout << "[Forrás] WASAPI loopback (csak analízis; a DSP-hez az EQ APO plugin kell).\n";
        }
        if (!loopback_.active()) playback_.reset();   // nincs lehallgatás (nincs eszköz): nincs lejátszás
        // Az eszköz, amelyen a plugin fut (az alapértelmezett lejátszóeszköz): ha később egy másik eszközre
        // vált a hang (pl. bedugott USB hangkártya), a felület meg tudja mondani, hol működött eddig
        if (pluginActive_ && apoOnDevice_ == ApoOnDevice::Yes) {   // csak biztosan feldolgozott eszközt jegyzünk meg
            const std::string dev = loopback_.deviceName();
            if (!dev.empty() && dev != apoDevice_) {
                apoDevice_ = dev;
                saveApoDevice(dev);
            }
        }
    }

    size_t pull(float* lr, size_t maxFrames) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (playerActive_) {
            const size_t n = readTapRing(player_.tap(), playerReadPos_, lr, maxFrames);
            const uint64_t now = GetTickCount64();
            if (n > 0) {
                lastPlayerData_ = now;
                return n;
            }
            // szünetben (vagy DoP módban, ahol a DSP kimarad) nincs feldolgozás: csend, hogy a vízesés és a mérők lecsengjenek
            if (now - player_.lastDataMs() > 100 && now - lastPlayerData_ > 20) {
                const uint64_t ms = std::min<uint64_t>(now - lastPlayerData_, 100);
                const uint32_t sr = playerOut_.dopActive() ? playerOut_.currentRate() : player_.sampleRate();
                const size_t frames = std::min<size_t>(maxFrames, static_cast<size_t>(ms * (sr ? sr : 48000) / 1000));
                std::fill(lr, lr + frames * 2, 0.0f);
                lastPlayerData_ = now;
                return frames;
            }
            return 0;
        }
        if (pluginActive_ && mapping_.state()) {
            const AetherSharedState* st = mapping_.state();
            const size_t n = readTapRing(st->tap, pluginReadPos_, lr, maxFrames);
            const uint64_t now = GetTickCount64();
            if (n > 0) {
                lastPluginData_ = now;
                return n;
            }
            // Az EQ APO szünetben nem hívja a plugint: csendet adunk, hogy a vízesés lecsengjen
            const uint64_t tick = st->ownerTickMs.load();
            if (now - tick > 100 && now - lastPluginData_ > 20) {
                const uint64_t ms = std::min<uint64_t>(now - lastPluginData_, 100);
                const size_t frames = std::min<size_t>(maxFrames, static_cast<size_t>(ms * st->sampleRate.load() / 1000));
                std::fill(lr, lr + frames * 2, 0.0f);
                lastPluginData_ = now;
                return frames;
            }
            return 0;
        }
        const size_t n = readTapRing(loopback_.ring(), loopbackReadPos_, lr, maxFrames);
        playback_.push(lr, n, loopback_.sampleRate());   // szól-e a zene, miközben a plugin nem fut
        return n;
    }

    double sampleRate() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (playerActive_) return playerOut_.dopActive() ? playerOut_.currentRate() : player_.sampleRate();
        if (pluginActive_ && mapping_.state()) return mapping_.state()->sampleRate.load();
        return loopback_.sampleRate();
    }

    /** A Bass Enhancer felharmonikus-szintje (dB) a pluginból; plugin nélkül nincs feldolgozás (-100). */
    double bassHarmonicsDb() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (playerActive_) return playerOut_.dopActive() ? -100.0 : player_.bassHarmonicsDb();
        if (bridgeActive_) return player_.bassHarmonicsDb();
        if (pluginActive_ && mapping_.state()) return mapping_.state()->bassHarmonicsDb.load();
        return -100.0;
    }

    /** Új konvolver IR a GUI-ból: tárolás, mentés a pluginnak, és átadás a közös memórián. */
    void setIr(IrData&& ir) {
        {
            std::lock_guard<std::mutex> ik(irMutex_);
            ir_ = std::move(ir);
            if (!win::saveIrFile(ir_)) std::cout << "[IR] Figyelem: az ir.bin nem menthető a ProgramData mappába.\n";
            player_.submitIr(ir_);
        }
        std::lock_guard<std::mutex> lk(mutex_);
        if (mapping_.state()) {
            std::lock_guard<std::mutex> ik(irMutex_);
            if (ir_.empty()) {
                // "Nincs IR" jelzése: üres blokk új sorszámmal
                IrData none;
                win::writeIrBlock(mapping_.state()->ir, none);
            } else {
                win::writeIrBlock(mapping_.state()->ir, ir_);
            }
        }
    }

    IrData irCopy() {
        std::lock_guard<std::mutex> ik(irMutex_);
        return ir_;
    }

    void loadStoredIr() {
        IrData ir;
        if (win::loadIrFile(ir)) {
            std::cout << "[IR] Mentett impulzusválasz: " << ir.name << " (" << ir.frames() << " minta)\n";
            std::lock_guard<std::mutex> ik(irMutex_);
            ir_ = std::move(ir);
            player_.submitIr(ir_);
        }
    }

    void pushParams(const ParamData& p) {
        std::lock_guard<std::mutex> lk(mutex_);
        lastParams_ = p;
        player_.setParams(p);
        if (mapping_.state()) mapping_.state()->params.write(pluginParamsFor(p, bridgeActive_));
    }

    /** Rövid, emberi leírás a tálcaikonhoz. */
    std::wstring label() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (playerActive_) {
            wchar_t buf[96];
            if (playerOut_.asioActive()) {
                swprintf(buf, 96, playerOut_.dopActive() ? L"ASIO DSD (%.1f kHz)" : L"ASIO (%.1f kHz)",
                         playerOut_.currentRate() / 1000.0);
            } else if (playerOut_.dopActive()) {
                swprintf(buf, 96, L"WASAPI exclusive DoP (%.1f kHz)", playerOut_.currentRate() / 1000.0);
            } else {
                swprintf(buf, 96, L"WASAPI exclusive (%.1f kHz)", player_.sampleRate() / 1000.0);
            }
            return buf;
        }
        if (bridgeActive_) {
            wchar_t buf[96];
            swprintf(buf, 96, L"Rendszerhang-híd (%.1f kHz)", player_.sampleRate() / 1000.0);
            return buf;
        }
        if (pluginActive_ && mapping_.state()) {
            wchar_t buf[96];
            swprintf(buf, 96, L"EQ APO plugin (%.1f kHz)", mapping_.state()->sampleRate.load() / 1000.0);
            return buf;
        }
        return L"WASAPI loopback – " + win::utf8ToWide(loopback_.deviceName());
    }

    std::string statusFields() {
        std::lock_guard<std::mutex> lk(mutex_);
        std::string s = "\"platform\":\"windows\"";
        char buf[160];
        if (playerActive_) {
            const bool dop = playerOut_.dopActive();
            const uint32_t sr = dop ? playerOut_.currentRate() : player_.sampleRate();
            const bool asio = playerOut_.asioActive();
            std::snprintf(buf, sizeof(buf), ",\"source\":\"%s\",\"dspActive\":%s,\"sr\":%u,\"gr\":%.2f",
                          asio ? (dop ? "ASIO (DSD)" : "ASIO") : dop ? "WASAPI exclusive (DoP)" : "WASAPI exclusive",
                          dop ? "false" : "true", sr, dop ? 0.0 : player_.gainReductionDb());
            s += buf;
            s += ",\"dspState\":\"active\"";
        } else if (bridgeActive_) {
            // Rendszerhang-híd: a lánc a motorban fut (a kábelen a plugin átenged), a mintavétel a valódi eszközé
            std::snprintf(buf, sizeof(buf), ",\"source\":\"%s\",\"dspActive\":true,\"sr\":%u,\"gr\":%.2f",
                          "System audio bridge", player_.sampleRate(), player_.gainReductionDb());
            s += buf;
            s += ",\"dspState\":\"active\"";
        } else if (pluginActive_ && mapping_.state()) {
            std::snprintf(buf, sizeof(buf),
                          ",\"source\":\"EQ APO plugin\",\"dspActive\":true,\"sr\":%u,\"instances\":%u,\"gr\":%.2f",
                          mapping_.state()->sampleRate.load(), mapping_.state()->activeInstances.load(),
                          static_cast<double>(mapping_.state()->gainReductionDb.load()));
            s += buf;
            s += ",\"dspState\":\"active\"";
        } else {
            std::snprintf(buf, sizeof(buf), ",\"source\":\"WASAPI loopback\",\"dspActive\":false,\"sr\":%u",
                          loopback_.sampleRate());
            s += buf;
            const std::string dev = loopback_.deviceName();
            s += ",\"device\":\"" + jsonEscape(dev) + "\"";
            // A telepítő beillesztette a plugint, de az nem jelentkezik: a mostani eszközön nincs bekapcsolva az
            // Equalizer APO (pl. új USB hangkártya), vagy frissítés után a régi plugin fut még
            if (pluginIntegrated()) {
                // szünet (nem hiba), hiba, másik eszköz vagy nincs eszköz (dsp_state.hpp)
                DspStateInput in;
                in.pluginAlive = pluginAliveLocked();
                in.deviceOk = loopback_.active();
                in.device = dev;
                in.apoDevice = apoDevice_;
                in.playingMs = playback_.playingMs();
                in.apoOnDevice = apoOnDevice_;
                // Ha ezen az eszközön nincs Equalizer APO, a „hol működik” név egy olyan eszköz legyen, amelyen van
                // (a korábban mentett név lehet éppen ez az eszköz, ha a plugin közben egy másikon futott)
                if (apoOnDevice_ == ApoOnDevice::No && (in.apoDevice.empty() || in.apoDevice == dev)) in.apoDevice = apoDeviceHint_;
                s += ",\"dspState\":\"" + std::string(dspStateName(classifyDspState(in))) + "\"";
                s += ",\"pluginExpected\":true";
                if (!in.apoDevice.empty() && in.apoDevice != dev) s += ",\"apoDevice\":\"" + jsonEscape(in.apoDevice) + "\"";
                if (!win::eqApoConfiguratorPath().empty()) s += ",\"apoConfigurator\":true";
            }
        }
        return s;
    }

    /** A telepítő beillesztette-e a plugint az Equalizer APO-ba (HKLM\Software\Aqulify, EqApoIntegrated). */
    static bool pluginIntegrated() {
        DWORD v = 0, size = sizeof(v);
        return RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Aqulify", L"EqApoIntegrated", RRF_RT_REG_DWORD, nullptr, &v,
                            &size) == ERROR_SUCCESS && v != 0;
    }

    /** A Magic Touch mérései (csak a pluginból: WASAPI loopbacknél nincs DSP). Üres, ha nincs adat. */
    std::string magicDiagJson() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (playerActive_) return playerOut_.dopActive() ? std::string{} : player_.magicDiagJson();
        if (bridgeActive_) return player_.magicDiagJson();
        if (!pluginActive_ || !mapping_.state()) return {};
        magic::MagicDiag d;
        if (!mapping_.state()->magicDiag.read(d)) return {};
        return magic::diagToJson(d);
    }

private:
    /** A mostani lejátszóeszközön van-e Equalizer APO (eszközváltáskor és 5 s-onként újraolvasva). */
    void refreshApoOnDeviceLocked() {
        const std::wstring id = loopback_.deviceId();
        const uint64_t now = GetTickCount64();
        if (id == apoCheckedId_ && now - apoCheckedAt_ < 5000) return;
        apoCheckedId_ = id;
        apoCheckedAt_ = now;
        const ApoOnDevice v = id.empty() ? ApoOnDevice::Unknown : win::apoOnRenderDevice(id);
        if (v != apoOnDevice_) {
            apoOnDevice_ = v;
            if (v == ApoOnDevice::No) {
                apoDeviceHint_ = win::firstActiveApoDeviceName();
                std::cout << "[APO] A(z) „" << loopback_.deviceName() << "” eszközön nincs bekapcsolva az Equalizer APO"
                          << (apoDeviceHint_.empty() ? std::string(".") : "; bekapcsolva itt: „" + apoDeviceHint_ + "”.")
                          << "\n";
            }
        }
    }

    bool pluginAliveLocked() const {
        const AetherSharedState* st = mapping_.state();
        if (!st) return false;
        // A plugin akkor "jelen van", ha van élő példánya. Szünetben (nincs lejátszás) az EQ APO
        // nem hívja a feldolgozást, ezért a friss írás hiánya önmagában nem jelenti a plugin eltűnését;
        // 10 perc teljes csend után (vagy összeomlott audiodg esetén) viszont visszaváltunk loopbackre.
        const uint64_t tick = st->ownerTickMs.load();
        const uint64_t now = GetTickCount64();
        return st->activeInstances.load() > 0 && tick != 0 && now >= tick && now - tick < 600000;
    }

    /** Az eszköz neve, amelyen a plugin legutóbb futott (HKCU\Software\Aqulify, ApoDevice; UTF-8). */
    static std::string loadApoDevice() {
        wchar_t buf[512] = {};
        DWORD size = sizeof(buf);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Aqulify", L"ApoDevice", RRF_RT_REG_SZ, nullptr, buf, &size) !=
            ERROR_SUCCESS)
            return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1) return {};
        std::string out(static_cast<size_t>(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), n, nullptr, nullptr);
        return out;
    }
    static void saveApoDevice(const std::string& name) {
        const std::wstring w = win::utf8ToWide(name);
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Aqulify", L"ApoDevice", REG_SZ, w.c_str(),
                        static_cast<DWORD>((w.size() + 1) * sizeof(wchar_t)));
    }

    static std::string jsonEscape(const std::string& in) {
        std::string out;
        for (char c : in) {
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
            else out += c;
        }
        return out;
    }

    win::WasapiLoopback& loopback_;
    PlayerDsp& player_;
    player::WinPlayerOutput& playerOut_;
    win::SharedMapping mapping_;
    std::mutex mutex_;
    bool playerActive_ = false;
    uint64_t playerReadPos_ = 0;
    uint64_t lastPlayerData_ = 0;
    std::function<bool()> bridgeActiveFn_;
    bool bridgeActive_ = false;      // a rendszerhang-híd fut: a DSP a motorban (PlayerDsp), a plugin átenged
    ParamData lastParams_{};         // a felület lánca (a híd leállásakor ez megy vissza a pluginnak)
    bool pluginActive_ = false;
    uint64_t pluginReadPos_ = 0;
    uint64_t loopbackReadPos_ = 0;
    uint64_t lastPluginData_ = 0;
    std::string apoDevice_;
    ApoOnDevice apoOnDevice_ = ApoOnDevice::Unknown;
    std::wstring apoCheckedId_;
    uint64_t apoCheckedAt_ = 0;
    std::string apoDeviceHint_;
    PlaybackDetector playback_;
    enum SourceLogKind { kSrcApo = 1, kSrcLoopbackNoApo = 2, kSrcLoopback = 3 };
    aq::log::StableStateLogger sourceLog_{ 2500 };
    std::mutex irMutex_;
    IrData ir_;
};

} // namespace

int main(int argc, char** argv) {
    // Párosítás kijelző nélkül: a már futó motortól kér kódot, és kilép (pair_cli.hpp). A program ablakos
    // alkalmazás, ezért a kimenet a hívó parancssor konzoljára megy (ha nincs ilyen, saját konzol nyílik).
    if (hasArg(argc, argv, "--pair")) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        SetConsoleOutputCP(CP_UTF8);
        std::FILE* out = std::freopen("CONOUT$", "w", stdout);
        std::vector<int> pairPorts(std::begin(kEnginePorts), std::end(kEnginePorts));
        for (int i = 1; i + 1 < argc; ++i)
            if (std::strcmp(argv[i], "--port") == 0) pairPorts = { std::atoi(argv[i + 1]) };
        const bool admin = hasArg(argc, argv, "--admin");
        return paircli::run(pairPorts, "Indítsd el az Aqulify-t.", out ? out : stdout, admin);
    }
    const bool console = hasArg(argc, argv, "--console");
    win::LogSink log(console);

    std::cout << "========================================================\n";
    std::cout << " Aqulify • Fázis & Akusztikai Vízesés – Windows motor\n";
    std::cout << "========================================================\n\n";

    if (hasArg(argc, argv, "--help") || hasArg(argc, argv, "-h")) {
        std::cout << "Használat: Aqulify.exe [--background] [--console] [--port N] [--pair]\n"
                     "  --background  indításkor ne nyissa meg a felületet (automatikus indításhoz)\n"
                     "  --console     naplózás konzolablakba a napló fájl helyett\n"
                     "  --port N      csak ezt a WebSocket portot próbálja (alapból: 47810-47814, majd 8080)\n"
                     "  --pair        párosító kód a telefonhoz a már futó motortól (konzolba írva)\n";
        return 0;
    }
    std::vector<int> ports(std::begin(kEnginePorts), std::end(kEnginePorts));
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0) ports = { std::atoi(argv[i + 1]) };
    }
    const bool background = hasArg(argc, argv, "--background") || hasArg(argc, argv, "--no-gui");

    // Egy példány felhasználónként: a második indítás csak a felületet nyitja meg
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\AqulifyEngine");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cout << "[Info] Már fut egy példány; csak a felület nyílik meg.\n";
        if (!background) openGui();
        CloseHandle(single);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // Nyelv (tálcamenü, üzenetek): a felületen választott, különben a Windows nyelve
    aq_i18n::init();

    // Indításkori visszaállítás (startup_restore.hpp): az utolsó működő lánc és az IR (a Windows-on több kimenet
    // és hangkimenet-beállítás nincs). Mód „ask”: a felület kérdez, addig az alapállapot marad.
    std::mutex paramMutex;
    std::mutex restoreMutex;
    startup_restore::Pref restorePref = startup_restore::parsePref(win::loadRestorePrefText());
    ParamData savedChain = transparentParams();
    const bool hasChain = win::loadRestoreParamFile(savedChain);
    const bool chainNow = hasChain && restorePref.mode == startup_restore::Mode::Always && restorePref.parts.chain;
    ParamData current = transparentParams();
    if (chainNow) current = savedChain;
    bool dirty = false;
    auto lastChange = std::chrono::steady_clock::now();

    auto loopback = std::make_unique<win::WasapiLoopback>();
    loopback->start();

    // A beépített lejátszó kimenete és a kizárólagos módú saját lánca (a lánc a lejátszónál tovább él);
    // a visszaállított láncot azonnal megkapja, hogy az első GUI-módosítás előtt is egyezzen a pluginnal
    PlayerDsp playerDsp(current);
    auto playerOutOwned = std::make_unique<player::WinPlayerOutput>();
    player::WinPlayerOutput& playerOut = *playerOutOwned;
    playerOut.setExclusiveDsp(&playerDsp);

    SourceManager sources(*loopback, playerDsp, playerOut);
    if (chainNow) {
        sources.loadStoredIr();
        // a plugin a params.bin-ből olvas: a visszaállított láncot oda is kiírjuk
        if (!win::saveParamFile(current)) std::cout << "[Beállítások] Figyelem: a params.bin nem írható.\n";
        std::cout << "[Beállítások] A mentett lánc visszaállítva.\n";
    }
    sources.poll(current);
    // Mód „ask”, és van mentett lánc: a felület kérdez (a válaszig a lánc alapállapotban marad)
    std::atomic<bool> restorePending{ restorePref.mode == startup_restore::Mode::Ask && hasChain };
    if (restorePending) std::cout << "[Beállítások] Indításkor a felület megkérdezi, mit állítson vissza.\n";
    // A mentett lánc és IR élesítése (a felület kérésére)
    auto applyChain = [&] {
        ParamData p;
        {
            std::lock_guard<std::mutex> lk(paramMutex);
            if (!hasChain) return;
            current = savedChain;
            p = current;
        }
        sources.loadStoredIr();
        sources.pushParams(p);
        if (!win::saveParamFile(p)) std::cout << "[Beállítások] Figyelem: a params.bin nem írható.\n";
        std::cout << "[Beállítások] A mentett lánc visszaállítva.\n";
    };

    BridgeCallbacks cb;
    cb.pullAudio = [&](float* lr, size_t maxFrames) { return sources.pull(lr, maxFrames); };
    cb.sampleRate = [&]() { return sources.sampleRate(); };
    cb.bassHarmonics = [&]() { return sources.bassHarmonicsDb(); };
    cb.getParams = [&]() { std::lock_guard<std::mutex> lk(paramMutex); return current; };
    cb.setParams = [&](const ParamData& p) {
        {
            std::lock_guard<std::mutex> lk(paramMutex);
            current = p;
            dirty = true;
            lastChange = std::chrono::steady_clock::now();
        }
        sources.pushParams(p);
    };
    cb.restoreStatus = [&]() {
        std::lock_guard<std::mutex> lk(restoreMutex);
        return startup_restore::statusMessage(restorePending.load(), restorePref, hasChain, false, false);
    };
    cb.restoreRequest = [&](const std::string& type, const json::Value& msg) {
        std::lock_guard<std::mutex> lk(restoreMutex);
        if (type == "restore_choice") {
            const bool restore = msg.get("choice") && msg.get("choice")->stringOr("") == "restore";
            const startup_restore::Parts want = restore
                ? startup_restore::partsFromJson(msg.get("parts"), startup_restore::Parts{})
                : startup_restore::Parts{ false, false, false };
            if (restorePending.exchange(false) && want.chain) applyChain();
            const json::Value* rem = msg.get("remember");
            if (rem && rem->type == json::Value::Type::Bool && rem->b) {
                restorePref.parts = want;
                restorePref.mode = startup_restore::anyPart(want) ? startup_restore::Mode::Always : startup_restore::Mode::Never;
                if (!win::saveRestorePrefText(startup_restore::serializePref(restorePref)))
                    std::cout << "[Beállítások] Figyelem: a restore.json nem írható.\n";
            }
        } else if (type == "restore_mode") {
            if (const json::Value* m = msg.get("mode"); m && m->type == json::Value::Type::String)
                restorePref.mode = startup_restore::modeFromName(m->str);
            restorePref.parts = startup_restore::partsFromJson(msg.get("parts"), restorePref.parts);
            if (!win::saveRestorePrefText(startup_restore::serializePref(restorePref)))
                std::cout << "[Beállítások] Figyelem: a restore.json nem írható.\n";
        }
    };
    // (a statusFields és a setVolume a lejátszó és a rendszerhang-híd után, lent)
    cb.setIr = [&](IrData&& ir) { sources.setIr(std::move(ir)); };
    cb.getIr = [&]() { return sources.irCopy(); };
    cb.loadLibraryIr = loadLibraryIr;
    cb.saveLibraryIr = saveLibraryIr;
    cb.removeLibraryIr = removeLibraryIr;
    std::mutex presetMutex;
    std::string presetDoc = loadPresetFile();
    cb.getPresets = [&]() { std::lock_guard<std::mutex> lk(presetMutex); return presetDoc; };
    cb.setPresets = [&](const std::string& doc) {
        std::lock_guard<std::mutex> lk(presetMutex);
        presetDoc = doc;
        if (!savePresetFile(doc)) std::cout << "[Profilok] Figyelem: a presets.json nem írható.\n";
    };
    cb.magicDiagJson = [&]() {
        {
            std::lock_guard<std::mutex> lk(paramMutex);
            if (!(current.has(kFlagMagic) && current.magicAmount > 0.0f)) return std::string();
        }
        return sources.magicDiagJson();
    };
    // A gépen szóló lejátszó (a Windows médiavezérlése): „most szól” és vezérlés a felületről és a távirányítóról
    media::SmtcSession mediaSession;
    mediaSession.start();
    // A beépített lejátszó (2.x): az alapértelmezett lejátszóeszközre játszik (WASAPI), így a hang az Equalizer APO-ban
    // futó Aqulify DSP-láncon át szól. Amíg van betöltött szám, a „most szól” és a vezérlés ezt mutatja; utána ismét
    // a gépen futó lejátszót (SMTC).
    // Kizárólagos módban (Hangkimenet, haladó) a lejátszó egyedül használja az eszközt: az APO kimarad, a lánc a motorban fut.
    player::FilePlayer filePlayer(std::move(playerOutOwned));
    // Windows rendszerhang-felmintavételező híd (virtuális kábel -> saját motor -> WASAPI Exclusive / ASIO)
    win::WinLiveBridge winLiveBridge;
    uint64_t liveBridgeReadPos = 0;
    {
        auto basePull = cb.pullAudio;
        auto baseRate = cb.sampleRate;
        cb.pullAudio = [&, basePull](float* lr, size_t maxFrames) -> size_t {
            if (!filePlayer.hasTrack() && winLiveBridge.isActive()) {
                return readTapRing(playerDsp.tap(), liveBridgeReadPos, lr, maxFrames);
            }
            return basePull(lr, maxFrames);
        };
        cb.sampleRate = [&, baseRate]() -> double {
            if (!filePlayer.hasTrack() && winLiveBridge.isActive()) {
                const uint32_t r = winLiveBridge.actualRate();
                if (r > 0) return static_cast<double>(r);
            }
            return baseRate();
        };
    }
    // A forrásválasztó tudja, mikor fut a híd: a DSP ilyenkor a motorban, a kábelen a plugin átenged (nincs kétszeres lánc)
    sources.setBridgeActiveFn([&]() { return !filePlayer.hasTrack() && winLiveBridge.dspInEngine(); });
    // A híd kizárólagos kimeneténél a Windows alapértelmezettje a virtuális kábel: a hangerő a valódi céleszközre menjen
    playerDsp.setActiveDeviceFn([&]() { return !filePlayer.hasTrack() ? winLiveBridge.currentOutputId() : std::wstring{}; });
    cb.setVolume = [&](double level, int mute) {
        const bool bridgeExcl = !filePlayer.hasTrack() && winLiveBridge.dspInEngine();
        const bool excl = playerOut.exclusiveActive() || bridgeExcl;
        if (excl && !playerDsp.hardwareVolume()) playerDsp.setVolume(level, mute);
        else setEndpointVolume(level, mute, bridgeExcl ? winLiveBridge.currentOutputId() : std::wstring{});
    };
    // Zenetár (2.x): a mentett zenei mappák (%APPDATA%\Aqulify\library.json), különben a Zene mappa
    library::MusicLibrary musicLibrary;
    // A lejátszási napló (3c2, %APPDATA%\Aqulify\stats.json): a sor előtt, mert a sor mérőszála a megszűnéséig hívhatja
    player::PlayStats playStats([](const std::string& doc) {
        if (!writeFileAtomic(userFilePath(L"stats.json", true), doc)) std::cout << "[Lejátszó] Figyelem: a stats.json nem írható.\n";
    });
    playStats.load(readSmallFile(userFilePath(L"stats.json", false), 64u << 20));
    // Telefonos zeneátvitel gyorsítótára (4b)
    player::PhoneCache phoneCache(player::toUtf8Path(std::filesystem::path(userFilePath(L"phone-cache", true))));
    // Lejátszási sor (3b): a zenetár azonosítóival; a queue.json-ból visszaáll, de magától nem indul el
    player::PlayQueue playQueue(
        filePlayer, [&](const std::string& id, std::string& path) -> bool {
            // A beépített lejátszó sorból nyíló száma elsőbbséget élvez a rendszerhang-híddal szemben
            winLiveBridge.suspendForPlayer();
            bool ok = false;
            if (id.rfind("p_", 0) == 0) {
                if (phoneCache.resolve(id, path)) ok = true;
                else {
                    if (cb.broadcast) {
                        cb.broadcast("{\"type\":\"phone_track_demand\",\"id\":\"" + media::jsonEscape(id) + "\"}");
                    }
                    ok = phoneCache.waitForTrack(id, std::chrono::milliseconds(2500), path);
                }
            } else {
                ok = musicLibrary.trackPath(id, path);
            }
            if (!ok && !filePlayer.hasTrack()) winLiveBridge.resumeAfterPlayer();
            return ok;
        },
        [](const std::string& doc) {
            if (!writeFileAtomic(userFilePath(L"queue.json", true), doc)) std::cout << "[Lejátszó] Figyelem: a queue.json nem írható.\n";
        });
    playQueue.restore(readSmallFile(userFilePath(L"queue.json", false), 16u << 20));
    // Lejátszási listák (3c): a motor tárolja (%APPDATA%\Aqulify\playlists.json)
    player::PlaylistStore playlists([](const std::string& doc) {
        if (!writeFileAtomic(userFilePath(L"playlists.json", true), doc)) std::cout << "[Lejátszó] Figyelem: a playlists.json nem írható.\n";
    });
    playlists.load(readSmallFile(userFilePath(L"playlists.json", false), 64u << 20));
    // Okos listák (3c2): az alaplisták az első induláskor, a felület nyelvén; a napló a sor mérőjéből
    playQueue.setPlayedCallback([&](const std::string& id) { playStats.record(id); });
    playlists.seedOnce(library::defaultSmartLists(aq_i18n::code() == "hu"));
    cb.mediaJson = [&]() { return filePlayer.hasTrack() ? media::toJson(playQueue.mediaInfo()) : mediaSession.json(); };
    cb.mediaCommand = [&](const std::string& action, double value) {
        if (filePlayer.hasTrack()) {
            playQueue.command(action, value);
            if (action == "stop" || !filePlayer.hasTrack()) winLiveBridge.resumeAfterPlayer();
        } else {
            mediaSession.command(action == "stop" ? "pause" : action, value);   // más lejátszót csak szüneteltetünk
        }
    };
    cb.mediaArt = [&](const std::string& id, std::string& mime, std::string& bytes, std::string& url) {
        if (filePlayer.art(id, mime, bytes)) return true;
        return mediaSession.art(id, mime, bytes, url);
    };
    cb.playerOpen = [&](const std::string& path, std::string& error) {
        winLiveBridge.suspendForPlayer();
        if (!filePlayer.open(path, &error)) {
            winLiveBridge.resumeAfterPlayer();
            std::cout << "[Lejátszó] " << path << ": " << error << "\n";
            return false;
        }
        std::cout << "[Lejátszó] Szól: " << path << "\n";
        return true;
    };
    cb.playerOpenBypass = [&](const std::string& path, std::string& error) {
        winLiveBridge.suspendForPlayer();
        if (!filePlayer.open(path, &error, 0, /*bypassUpsample=*/true)) {
            winLiveBridge.resumeAfterPlayer();
            std::cout << "[Lejátszó/Mérés] " << path << ": " << error << "\n";
            return false;
        }
        return true;
    };
    cb.playerStop = [&] {
        filePlayer.stop();
        winLiveBridge.resumeAfterPlayer();
    };
    // Szobamérés: mikrofonfelvétel WASAPI-val (room/mic_capture_wasapi.hpp)
    cb.makeMicCapture = [] { return aqulify::room::createMicCapture(); };
    // Egykattintásos frissítés (a felület „Új verzió érhető el” ablakából): letöltés, ellenőrzés, a telepítő indítása
    win::Updater updater;
    cb.updateInstall = [&](const std::string& version, std::string& error) { return updater.start(version, error); };
    // Média-hivatkozás (URL) megnyitása a felhasználó alapértelmezett böngészőjében. Külön, rövid életű szálon: a
    // böngésző indítása eltarthat egy ideig, és ne állítsa meg a WebSocket-hidat; a ShellExecute előtt a COM-ot
    // inicializálni kell (Microsoft-leírás). A cím már ellenőrzött (isAllowedMediaUrl).
    cb.openUrl = [](const std::string& u) -> bool {
        const int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, nullptr, 0);
        if (n <= 1) return false;
        std::wstring wurl(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, wurl.data(), n);
        wurl.resize(static_cast<size_t>(n - 1));   // a záró nulla nélkül
        try {
            std::thread([wurl] {
                const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                const auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
                if (r <= 32) std::cerr << "[Hivatkozás] A megnyitás nem sikerült (" << r << ").\n";
                if (SUCCEEDED(hr)) CoUninitialize();
            }).detach();
        } catch (...) {
            return false;   // a szál nem indult el
        }
        return true;
    };
    // Van-e alkalmazás a protokollhoz (pl. "tidal"): a Windows társítása szerint. A hagyományos alkalmazásnak
    // indítóparancsa van (EXECUTABLE / COMMAND), az áruházas (Microsoft Store) alkalmazásnak nincs, de alkalmazás-
    // azonosítója van (ASSOCSTR_APPID = 21; az llvm-mingw fejlécéből hiányzik). Élő próbán a Store-os TIDAL csak
    // az APPID-re válaszolt; nem regisztrált protokollnál mindhárom hibát ad (akkor a webes lejátszó nyílik meg).
    cb.hasUrlHandler = [](const std::string& scheme) -> bool {
        const std::wstring ws(scheme.begin(), scheme.end());   // csak ASCII protokollnév jön (pl. "tidal")
        for (ASSOCSTR what : { ASSOCSTR_EXECUTABLE, ASSOCSTR_COMMAND, static_cast<ASSOCSTR>(21) }) {
            wchar_t buf[MAX_PATH * 2];
            DWORD n = static_cast<DWORD>(std::size(buf));
            if (AssocQueryStringW(ASSOCF_IS_PROTOCOL, what, ws.c_str(), nullptr, buf, &n) == S_OK && buf[0] != L'\0')
                return true;
        }
        return false;
    };
    cb.statusFields = [&]() {
        std::string s = sources.statusFields() + updater.statusJson();
        const bool bridgeExcl = !filePlayer.hasTrack() && winLiveBridge.dspInEngine();
        const bool excl = playerOut.exclusiveActive();
        const bool dop = playerOut.dopActive() || filePlayer.isDopActive();
        const bool digitalVolume = (excl || bridgeExcl) && !dop && !playerDsp.hardwareVolume();
        s += digitalVolume ? playerDsp.volumeJson()
                           : endpointVolumeJson(bridgeExcl ? winLiveBridge.currentOutputId() : std::wstring{});
        {
            const bool uActive = filePlayer.isUpsampleActive();
            const uint32_t uSrc = filePlayer.upsampleSrcRate();
            const uint32_t uTgt = filePlayer.upsampleTargetRate();
            const uint32_t uActual = excl ? playerOut.actualRate() : 0;
            const bool uResampleWarn = uActive && (!excl || (uActual > 0 && uTgt > 0 && uActual != uTgt));
            const std::string uReason = filePlayer.upsampleReason();
            char ubuf[384];
            std::snprintf(ubuf, sizeof(ubuf),
                          ",\"upsample\":{\"active\":%s,\"srcRate\":%u,\"targetRate\":%u,\"actualDacRate\":%u,"
                          "\"resampleWarning\":%s,\"reason\":\"%s\",\"clippedSamples\":%llu,"
                          "\"processedFrames\":%llu,\"delayMs\":%.2f}",
                          uActive ? "true" : "false", uSrc, uTgt, uActual,
                          uResampleWarn ? "true" : "false", uReason.c_str(),
                          static_cast<unsigned long long>(filePlayer.upsampleClippedSamples()),
                          static_cast<unsigned long long>(filePlayer.upsampleProcessedFrames()),
                          filePlayer.upsampleDelayMs());
            s += ubuf;
            s += winLiveBridge.statusJson();
        }
        if (!filePlayer.hasTrack()) return s;
        // A jelút (Hangkimenet, haladó): megosztott módban a Windows hangmotorja keveri a jelet (nem bitpontos);
        // kizárólagos módban bitpontos, ha sem a lánc, sem a ReplayGain, sem a hangerő, sem a formátum nem változtat rajta
        const uint32_t rate = filePlayer.streamRate();
        const float rg = filePlayer.currentGainDb();
        const double dspMs = excl && !dop && rate ? 1000.0 * static_cast<double>(playerDsp.latencySamples()) / rate : 0.0;
        const bool transparent = dop || (excl && playerDsp.transparent() && !playerDsp.volumeTouches() && rg == 0.0f &&
                                         playerOut.quantExact());
        std::string out = playerOut.statusJson(dspMs);
        const std::string dopFb = filePlayer.dopFallbackReason();
        char pb[384];
        std::snprintf(pb, sizeof(pb),
                      ",\"dop\":{\"want\":%s,\"active\":%s,\"dsdRate\":%u,\"fallback\":\"%s\"},"
                      "\"dspMs\":%.2f,\"transparent\":%s,\"mixer\":%s,\"keepAwake\":false,\"playerRate\":%u,"
                      "\"playerGain\":%.2f,\"digitalVolume\":%s,\"dspTransparent\":%s,\"volTouch\":%s",
                      filePlayer.dsdDop() ? "true" : "false", dop ? "true" : "false",
                      filePlayer.dopDsdRate(), dopFb.c_str(),
                      dspMs, transparent ? "true" : "false", excl ? "false" : "true", rate, rg,
                      digitalVolume ? "true" : "false", (dop || playerDsp.transparent()) ? "true" : "false",
                      digitalVolume && playerDsp.volumeTouches() ? "true" : "false");
        out += pb;
        return s + ",\"path\":{" + out + "}";
    };
    {
        musicLibrary.setCacheStorage([] { return readSmallFile(userFilePath(L"library-cache.json", false), 64 * 1024 * 1024); },
                                     [](const std::string& doc) {
                                         if (!writeFileAtomic(userFilePath(L"library-cache.json", true), doc))
                                             std::cout << "[Zenetár] Figyelem: a library-cache.json nem írható.\n";
                                     });
        std::vector<std::string> roots = library::parseRootsDoc(readSmallFile(userFilePath(L"library.json", false), 256 * 1024));
        if (roots.empty()) roots.push_back(library::MusicLibrary::defaultMusicDir());
        musicLibrary.setRoots(roots);
    }
    library::attachLibrary(cb, musicLibrary, playQueue, playlists, playStats, [](const std::string& doc) {
        if (!writeFileAtomic(userFilePath(L"library.json", true), doc)) std::cout << "[Zenetár] Figyelem: a library.json nem írható.\n";
    }, &phoneCache);
    // Internetrádió: a felhasználó állomáslistája (%APPDATA%\Aqulify\radio.json); a lejátszás a beépített lejátszón
    radio::StationStore radioStations([](const std::string& doc) {
        if (!writeFileAtomic(userFilePath(L"radio.json", true), doc)) std::cout << "[Rádió] Figyelem: a radio.json nem írható.\n";
    });
    radioStations.load(readSmallFile(userFilePath(L"radio.json", false), 1u << 20));
    radio::RadioController radioCtl(filePlayer, radioStations);
    radio::attachRadio(cb, radioCtl, radioStations);
    // Helyi zene / rádió / UPnP indítása előtt minden éppen szóló külső lejátszót (SMTC) szüneteltetünk,
    // és megszakítjuk az esetlegesen még csatlakozó internetrádiót, így nem szólhat két zene egymásra.
    filePlayer.setBeforePlayCallback([&]() {
        radioCtl.cancelPendingIfExternal();
        mediaSession.pauseAllPlaying();
    });
    // UPnP / DLNA lejátszó (upnp_bridge.hpp): a telefonos vezérlők (BubbleUPnP, mconnect, foobar2000, …) a beépített
    // lejátszón át, a teljes láncon (kizárólagos WASAPI / ASIO, DoP / natív DSD). Alapból ki (audio.json).
    upnp::UpnpService upnpService(filePlayer, playQueue, cb.setVolume, cb.getVolume);
    cb.mediaJson = [&]() {
        if (!filePlayer.hasTrack()) return mediaSession.json();
        media::Info m = playQueue.mediaInfo();
        upnpService.patchInfo(m);
        return media::toJson(m);
    };
    cb.loadRemote = [] { return readSmallFile(userFilePath(L"remote.json", false), 256 * 1024); };
    cb.saveRemote = [](const std::string& doc) { return writeFileAtomic(userFilePath(L"remote.json", true), doc); };
    cb.loadTargets = [] { return readSmallFile(userFilePath(L"remote-targets.json", false), 256 * 1024); };
    cb.saveTargets = [](const std::string& doc) { return writeFileAtomic(userFilePath(L"remote-targets.json", true), doc); };
    // Ablak bezárása „Bezárás” választással: ugyanaz, mint a tálcamenü „Kilépés” pontja (rendes leállás)
    static std::atomic<win::TrayApp*> s_tray{ nullptr };
    static std::atomic<aq::health::Monitor*> s_health{ nullptr };
    // Konzol-vezérlő jelek (Ctrl+C, konzolablak bezárása, kijelentkezés, leállítás): külön rendszerszálon fut,
    // ezért a szálbiztos markClean()-t azonnal hívja, majd zárja a tálcaablakot.
    struct ConsoleCtrl {
        static BOOL WINAPI handle(DWORD ctrlType) {
            switch (ctrlType) {
                case CTRL_C_EVENT:
                case CTRL_BREAK_EVENT:
                case CTRL_CLOSE_EVENT:
                case CTRL_LOGOFF_EVENT:
                case CTRL_SHUTDOWN_EVENT:
                    if (auto* h = s_health.load()) h->markClean();
                    if (win::TrayApp* t = s_tray.load()) t->quit();
                    return TRUE;
                default:
                    return FALSE;
            }
        }
    };
    cb.quitEngine = [] {
        if (auto* h = s_health.load()) h->markClean();
        if (win::TrayApp* t = s_tray.load()) t->quit();
    };

    // Haladó hangkimenet Windowson: csak a beépített lejátszó beállításai (ReplayGain, előbetöltés, kizárólagos mód).
    // A pufferméretet és a mintavételt a Windows hangmotorja határozza meg (az Equalizer APO abban fut); kizárólagos
    // módban a fájl mintavétele szól.
    std::mutex audioMutex;
    audioopt::AudioOptions audioOpts;
    bool dsdDop = false;
    aq::health::Monitor health;   // beépített hibajelentés: őrszál és helyi állapotfájl (a híd indulása után indul)
    aq::report::Service reportService;
    auto playerOnly = [](audioopt::AudioOptions o) {
        o.bufferFrames = 0;
        o.keepAwake = false;
        o.rateMode = audioopt::RateMode::System;
        o.resampleQuality = -1;
        return o;
    };
    auto optsToJsonWithDop = [](const audioopt::AudioOptions& o, bool dop) {
        std::string j = audioopt::toJson(o);
        if (!j.empty() && j.back() == '}') {
            j.pop_back();
            j += std::string(",\"dsdDop\":") + (dop ? "true}" : "false}");
        }
        return j;
    };
    {
        const std::string doc = readSmallFile(userFilePath(L"audio.json", false), 64 * 1024);
        json::Value v;
        if (!doc.empty() && json::parse(doc, v)) {
            audioOpts = playerOnly(audioopt::fromJson(v));
            if (const json::Value* d = v.get("dsdDop"); d && d->type == json::Value::Type::Bool) dsdDop = d->b;
        }
        filePlayer.setReplayGain(audioOpts.replayGain, audioOpts.rgPreampDb, audioOpts.rgPreventClip);
        filePlayer.setPreload(audioOpts.preload);
        filePlayer.setUpsampleOptions(audioOpts);
        winLiveBridge.setOptions(audioOpts);
        upnpService.apply(audioOpts.upnp, audioOpts.upnpName, audioOpts.upnpFixedVolume, audioOpts.upnpAllow);
        // Ha az előző futás a virtuális kábelen hagyta a Windows hangkimenetét, innen tudja, mire állítsa vissza
        winLiveBridge.setStateFile(userFilePath(L"system_audio_device.txt", true));
        winLiveBridge.start(&playerDsp, [&]() { return filePlayer.hasTrack(); });
        playerOut.setExclusive(audioOpts.exclusive);
        playerOut.setAsio(audioOpts.asio, audioOpts.asioDriver);
        // natív DSD: kizárólagos WASAPI-n (DoP) vagy ASIO-n (natív DSD, ha a meghajtó tudja; különben DoP)
        playerOut.setWantDop(audioOpts.exclusive && dsdDop);
        filePlayer.setDsdDop((audioOpts.exclusive || audioOpts.asio) && dsdDop);
        // ASIO-n a Windows végponti hangereje nem hat a hangfolyamra: ott mindig digitális a hangerő
        playerDsp.setHardwarePref(audioOpts.exclHwVolume && !audioOpts.asio);
        if (audioOpts.asio) std::cout << "[Lejátszó] ASIO-kimenet kérve (" << (audioOpts.asioDriver.empty() ? "az első meghajtó" : audioOpts.asioDriver) << ").\n";
    }
    cb.audioOptionsJson = [&]() -> std::string {
        std::lock_guard<std::mutex> lk(audioMutex);
        // a telepített ASIO-meghajtók (csak olvasás a registryből; üres lista, ha nincs)
        std::string drivers = "[";
        for (const auto& d : player::AsioPlayerOutput::listDrivers()) {
            if (drivers.size() > 1) drivers += ",";
            drivers += "{\"id\":\"" + audioopt::jsonStringBody(d.clsid) + "\",\"name\":\"" +
                       audioopt::jsonStringBody(d.description) + "\"}";
        }
        drivers += "]";
        return "{\"opts\":" + optsToJsonWithDop(audioOpts, dsdDop) +
               ",\"caps\":{\"buffer\":false,\"keepAwake\":false,\"rate\":false,\"resample\":false,\"player\":true,\"upsample\":true,"
               "\"systemUpsample\":true,\"virtualCables\":" + win::WinLiveBridge::virtualCablesJson() +
               ",\"exclusive\":true,\"dop\":true,\"asio\":true,\"asioDrivers\":" + drivers + ",\"upnp\":true}" +
               ",\"upnpState\":" + upnpService.statusJson() + "}";
    };
    cb.setAudioOptions = [&](const json::Value& v) {
        std::lock_guard<std::mutex> lk(audioMutex);
        const audioopt::AudioOptions o = playerOnly(audioopt::fromJson(v, audioOpts));
        bool wantDop = dsdDop;
        if (const json::Value* d = v.get("dsdDop"); d && d->type == json::Value::Type::Bool) wantDop = d->b;
        if (o == audioOpts && wantDop == dsdDop) return;
        audioOpts = o;
        dsdDop = wantDop;
        health.setEnabled(o.errorReports);
        reportService.setEnabled(o.errorReports);
        filePlayer.setReplayGain(o.replayGain, o.rgPreampDb, o.rgPreventClip);
        filePlayer.setPreload(o.preload);
        filePlayer.setUpsampleOptions(o);
        winLiveBridge.setOptions(o);
        upnpService.apply(o.upnp, o.upnpName, o.upnpFixedVolume, o.upnpAllow);
        playerDsp.setHardwarePref(o.exclHwVolume && !o.asio);
        if (o.exclusive != playerOut.wantExclusive()) {
            playerOut.setExclusive(o.exclusive);
            std::cout << "[Lejátszó] Kizárólagos mód: " << (o.exclusive ? "be" : "ki") << ".\n";
        }
        // ASIO be / ki vagy másik meghajtó: a szóló szám a mostani helyről újranyílik az új kimeneten
        const bool asioChanged = o.asio != playerOut.wantAsio() || (o.asio && o.asioDriver != playerOut.asioDriverWanted());
        if (asioChanged) {
            playerOut.setAsio(o.asio, o.asioDriver);
            std::cout << "[Lejátszó] ASIO-kimenet: " << (o.asio ? "be" : "ki") << ".\n";
        }
        playerOut.setWantDop(o.exclusive && dsdDop);
        const bool effDop = (o.exclusive || o.asio) && dsdDop;
        if (effDop != filePlayer.dsdDop()) {
            filePlayer.setDsdDop(effDop);   // DSD-fájlnál maga nyitja újra
            std::cout << "[Lejátszó] DSD natívan: " << (effDop ? "be" : "ki") << ".\n";
        }
        // (ha egyszerre a DSD-beállítás is változott és DSD szól, ez egy második, rövid újranyitás – ritka eset)
        if (asioChanged) filePlayer.reopenOutput();
        writeFileAtomic(userFilePath(L"audio.json", true), optsToJsonWithDop(o, dsdDop));
    };
    // Terhelés-tanácsadó: a motor processzorterhelése és a lejátszó hangkimaradásai az állapot végére
    load::Tracker loadTracker(load::defaultThresholdPct());
    {
        auto base = cb.statusFields;
        cb.statusFields = [&, base]() {
            std::string s = base ? base() : std::string();
            audioopt::AudioOptions curOpts;
            { std::lock_guard<std::mutex> lk(audioMutex); curOpts = audioOpts; }
            const uint32_t tgt = filePlayer.isUpsampleActive() ? filePlayer.upsampleTargetRate() : filePlayer.upsampleSrcRate();
            return s + loadTracker.json(curOpts, tgt, filePlayer.underruns(), filePlayer.streamRate() > audioopt::kMaxDspRate,
                                        filePlayer.upsampleThreadsInUse(), filePlayer.dedicatedCoreApplied());
        };
    }
    cb.getLanguage = [] { return aq_i18n::savedLanguage(); };
    cb.setLanguage = [](const std::string& l) {
        if (aq_i18n::set(l) && !aq_i18n::saveLanguage(l)) std::cout << "[Nyelv] Figyelem: a language.txt nem írható.\n";
    };
    // A legújabb kiadás ellenőrzése (6 óránként, WinHTTP-vel; AQULIFY_NO_UPDATE_CHECK=1: kikapcsolva). A híd előtt
    // jön létre, így utána szűnik meg (a híd a statusJson-t hívja).
    vercheck::Checker versionChecker;
    if (!std::getenv("AQULIFY_NO_UPDATE_CHECK")) {
        versionChecker.start([](const std::string& url) -> std::string {
            const std::wstring tmp = userFilePath(L"latest-versions.json", true);
            const std::wstring wurl(url.begin(), url.end());   // a cím rögzített, csak ASCII
            std::string text;
            if (win::downloadFile(wurl, tmp, nullptr, nullptr)) text = readSmallFile(tmp, vercheck::kMaxBytes);
            DeleteFileW(tmp.c_str());
            return text;
        }, AQULIFY_VERSION);
        cb.versionStatus = [&versionChecker] { return versionChecker.statusJson(); };
    }
    cb.errorReportStatus = [&reportService] { return reportService.statusJson(); };
    cb.errorReportPreview = [&reportService](const std::string& kind, const std::string& symptom, const std::string& note,
                                             std::string& preview, size_t& bytes, bool& configured) {
        reportService.buildPreview(kind, symptom, note, preview, bytes, configured);
    };
    cb.errorReportSend = [&reportService](const std::string& kind, const std::string& symptom, const std::string& note,
                                          std::function<void(bool, long long, const std::string&)> onDone) {
        reportService.sendAsync(kind, symptom, note, [onDone = std::move(onDone)](const aq::report::SendResult& r) {
            if (onDone) onDone(r.ok, r.id, r.error);
        });
    };
    cb.errorReportSave = [&reportService](const std::string& kind, const std::string& symptom, const std::string& note,
                                          std::string& filename, std::string& err) {
        return reportService.saveReport(kind, symptom, note, filename, err);
    };
    cb.errorReportDismissPrevious = [&reportService] { reportService.dismissPrevious(); };
    cb.errorLogRead = [&reportService](int lines, int& outLines) { return reportService.readLog(lines, outLines); };

    std::unique_ptr<WebSocketBridge> bridge;
    for (int p : ports) {
        auto b = std::make_unique<WebSocketBridge>(p);
        if (b->start(cb)) { bridge = std::move(b); break; }
        std::cout << "[Info] A " << p << " port foglalt, a következőt próbálom.\n";
    }
    if (!bridge) {
        std::string list;
        for (size_t i = 0; i < ports.size(); ++i) list += (i ? ", " : "") + std::to_string(ports[i]);
        std::wstring msg = aq_i18n::fmtw(aq_i18n::Msg::engine_portsBusy, { { "ports", list } });
        const std::wstring owner = portOwner(ports.front());
        if (!owner.empty()) {
            msg += L"\n\n" + aq_i18n::fmtw(aq_i18n::Msg::engine_portOwner, { { "port", std::to_string(ports.front()) },
                                                                               { "owner", aq_i18n::narrow(owner) } });
        }
        msg += L"\n\n" + aq_i18n::trw(aq_i18n::Msg::engine_portsHint);
        std::cout << "[Hiba] A WebSocket híd nem indult el (minden port foglalt).\n";
        MessageBoxW(nullptr, msg.c_str(), L"Aqulify", MB_ICONWARNING | MB_OK);
        loopback->stop();
        return 1;
    }

    auto saveIfDue = [&](bool force) {
        ParamData snapshot;
        bool save = false;
        {
            std::lock_guard<std::mutex> lk(paramMutex);
            snapshot = current;
            if (dirty && (force || std::chrono::steady_clock::now() - lastChange > std::chrono::milliseconds(600))) {
                dirty = false;
                save = true;
            }
        }
        sources.poll(snapshot);
        // ASIO: a meghajtó resetet kért (pl. pufferméret- vagy mintavétel-váltás a saját paneljén), vagy a hangfolyam
        // megszakadt: a szóló szám a mostani helyről újranyílik (ASIO-n, ha megy; különben WASAPI-n)
        if (playerOut.asioNeedsReopen()) {
            std::cout << "[Lejátszó] Az ASIO-hangfolyam leállt (" << playerOut.asio().lastError() << "); újranyitás.\n";
            playerOut.acknowledgeAsioStop();
            filePlayer.reopenOutput();
        }
        if (save) {
            if (!win::saveParamFile(snapshot)) std::cout << "[Beállítások] Figyelem: a ProgramData\\Aqulify\\params.bin nem írható.\n";
            if (!win::saveRestoreParamFile(snapshot)) std::cout << "[Beállítások] Figyelem: a last_params.bin nem írható.\n";
        }
    };

    // Beépített hibajelentés: őrszál, állapotfájl (%LOCALAPPDATA%\Aqulify\diag), az előző futás rendellenes végének
    // helyi jelentése. Küldés nincs (3. szakasz).
    {
        aq::health::Config hc;
        hc.dir = win::localAppDir() / L"diag";
        hc.logPath = log.path();
        hc.version = AQULIFY_VERSION;
        hc.platform = "windows";
        hc.package = "setup";
        { std::lock_guard<std::mutex> lk(audioMutex); hc.enabled = audioOpts.errorReports; }
        hc.crashHandlers = true;
        if (const char* s = std::getenv("AQULIFY_WATCHDOG_SECONDS"); s && std::atof(s) >= 1.0) hc.thresholdSec = std::atof(s);
        for (const wchar_t* f : { L"remote.json", L"remote-targets.json" })
            for (const auto& n : aq::report::namesFromJson(readSmallFile(userFilePath(f, false), 256 * 1024))) hc.scrubNames.push_back(n);
        if (!health.start(hc)) std::cout << "[Hibajelentés] A diagnosztikai mappa nem írható.\n";
        s_health.store(&health);
        SetConsoleCtrlHandler(&ConsoleCtrl::handle, TRUE);
        reportService.start(hc.dir, hc.logPath, hc.scrubNames, hc.enabled);
        reportService.setNamesProvider([] {
            std::vector<std::string> names;
            for (const wchar_t* f : { L"remote.json", L"remote-targets.json" })
                for (const auto& n : aq::report::namesFromJson(readSmallFile(userFilePath(f, false), 256 * 1024))) names.push_back(n);
            return names;
        });
    }
    int healthTicks = 0;
    auto healthTick = [&] {
        // a tálca időzítője 250 ms-onként hív: 2 s-onként a motor állapota, 30 s-onként a hangeszközök
        const int n = healthTicks++;
        if (n % 120 == 0) {
            std::string cards;
            for (const auto& d : win::listRenderDevices()) {
                if (!d.active) continue;
                // csak a meghajtó (gyári) neve és a végpont alapformátuma; a felhasználó által átnevezhető végpontnév
                // nem kerül bele
                const std::wstring props = std::wstring(win::apo_detail::renderBase()) + d.key + L"\\Properties";
                const std::wstring iface = win::apo_detail::regString(HKEY_LOCAL_MACHINE, props, L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6");
                const std::string fmt = aq::devfmt::readRegistry(HKEY_LOCAL_MACHINE, props);
                cards += "render: " + (iface.empty() ? std::string("?") : aq_i18n::narrow(iface)) +
                         "; format: " + (fmt.empty() ? std::string("?") : fmt) +
                         (d.apo == ApoOnDevice::Yes ? " (Equalizer APO: yes)" : d.apo == ApoOnDevice::No ? " (Equalizer APO: no)" : "") + "\n";
            }
            for (const auto& d : player::AsioPlayerOutput::listDrivers()) cards += "asio driver: " + d.description + "\n";
            health.setSoundCards(cards);
        }
        if (n % 8 != 0) return;
        audioopt::AudioOptions o;
        bool dop = false;
        { std::lock_guard<std::mutex> lk(audioMutex); o = audioOpts; dop = dsdDop; }
        const bool excl = playerOut.exclusiveActive();
        const bool dopOn = playerOut.dopActive() || filePlayer.isDopActive();
        std::string path;
        if (!filePlayer.hasTrack()) path = "Windows audio (Equalizer APO path)";
        else if (dopOn) path = std::string(o.asio ? "ASIO" : "exclusive WASAPI") + ", DSD" + std::to_string(filePlayer.dopDsdRate() / 44100);
        else path = "PCM " + std::to_string(filePlayer.streamRate()) + " Hz, " + (excl ? (o.asio ? "ASIO" : "exclusive WASAPI") : "shared WASAPI");
        char buf[384];
        std::snprintf(buf, sizeof(buf), "{\"path\":\"%s\",\"rate\":%u,\"player\":%s,\"player_underruns\":%llu,\"upsample\":%s,"
                      "\"dsd_dop_setting\":%s,\"audio_opts\":",
                      audioopt::jsonStringBody(path).c_str(), filePlayer.streamRate(), filePlayer.hasTrack() ? "true" : "false",
                      static_cast<unsigned long long>(filePlayer.underruns()), filePlayer.isUpsampleActive() ? "true" : "false",
                      dop ? "true" : "false");
        health.setSection("engine", std::string(buf) + audioopt::toJson(o) + "}");
    };
    healthTick();

    win::TrayApp::Callbacks tcb;
    tcb.openGui = [] { openGui(); };
    tcb.openLog = [&] {
        if (!log.path().empty()) ShellExecuteW(nullptr, L"open", log.path().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    tcb.onTimer = [&] { saveIfDue(false); healthTick(); };
    tcb.statusText = [&] { return sources.label(); };
    // Tálca „Kilépés”, WM_CLOSE (telepítő / frissítő / GUI / taskkill) vagy WM_DESTROY: a rendben-leállt jel még
    // az ablak eltűnése előtt lemezre kerül.
    tcb.onQuit = [&] { health.markClean(); };
    // Windows rendszerleállítás, újraindítás vagy kijelentkezés (WM_ENDSESSION): a Windows a visszatérés után
    // kilőheti a folyamatot, ezért a rendes-leállás jel, a lánc mentése és a params.bin alapra állítása szinkron fut.
    tcb.onEndSession = [&] {
        std::cout << "[Info] Munkamenet vége (WM_ENDSESSION), leállítás...\n";
        health.markClean();
        saveIfDue(true);
        {
            std::lock_guard<std::mutex> lk(paramMutex);
            current = transparentParams();
            dirty = false;
        }
        sources.pushParams(current);
        win::saveParamFile(current);
    };
    win::TrayApp tray(tcb);
    s_tray.store(&tray);
    if (!tray.create(GetModuleHandleW(nullptr))) {
        std::cout << "[Hiba] A tálcaikon nem hozható létre.\n";
    }

    if (!background) openGui();
    std::cout << "Fut a háttérben (tálcaikon).\n";

    tray.run();
    s_tray.store(nullptr);

    std::cout << "[Info] Leállítás...\n";
    health.markClean(); // a „rendben leállt” jel már most: a telepítő a leállás közben is kilőhet (frissítés)
    SetConsoleCtrlHandler(&ConsoleCtrl::handle, FALSE);
    s_health.store(nullptr);
    bridge->stop();
    updater.stop();
    winLiveBridge.stop();
    filePlayer.setBeforePlayCallback(nullptr);
    filePlayer.stop();
    mediaSession.stop();
    loopback->stop();
    // Az utolsó lánc a params.bin-be és a last_params.bin-be kerül (a visszaállításhoz). Utána a params.bin alapra
    // vált: a plugin a program bezárása után is abból olvas, és nem szabad tovább szűrnie a hangot.
    saveIfDue(true);
    {
        std::lock_guard<std::mutex> lk(paramMutex);
        current = transparentParams();
        dirty = false;
    }
    sources.pushParams(current);
    if (!win::saveParamFile(current)) std::cout << "[Beállítások] Figyelem: a params.bin nem állítható alapra.\n";
    reportService.stop(); // nem vár hálózati időkorlátra
    health.stopClean();   // a „rendben leállt” jel: a következő indítás nem készít rendellenes-vég jelentést
    if (single) { ReleaseMutex(single); CloseHandle(single); }
    CoUninitialize();
    return 0;
}
