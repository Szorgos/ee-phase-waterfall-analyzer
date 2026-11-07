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
#include <iphlpapi.h>
#include <psapi.h>
#include <xmmintrin.h>
#include <pmmintrin.h>

#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>

#include "ws_bridge.hpp"
#include "startup_restore.hpp"
#include "param_block.hpp"
#include "aether_chain.hpp"
#include "shared_state.hpp"
#include "aq_i18n.hpp"
#include "../common/win_shared.hpp"
#include "../common/eqapo_paths.hpp"
#include "dsp_state.hpp"
#include "wasapi_loopback.hpp"
#include <endpointvolume.h>
#include "tray.hpp"
#include "media_smtc.hpp"
#include "audio_options.hpp"
#include "file_player.hpp"
#include "library_bridge.hpp"
#include "player_output_wasapi.hpp"
#include "updater.hpp"

using namespace ee_analyzer;

namespace {
// A „most szól” sáv hangerő-csúszkája: az alapértelmezett lejátszóeszköz hangereje (mint a tálca hangerő-szabályzójáé)
const IID kIID_IAudioEndpointVolume = {0x5CDF2C82, 0x841E, 0x4546, {0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};

template <class F>
bool withEndpointVolume(F&& fn) {
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    {
        win::ComPtr<IMMDeviceEnumerator> en;
        win::ComPtr<IMMDevice> dev;
        win::ComPtr<IAudioEndpointVolume> vol;
        if (SUCCEEDED(CoCreateInstance(win::kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, win::kIID_IMMDeviceEnumerator,
                                       reinterpret_cast<void**>(en.put()))) &&
            SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.put())) &&
            SUCCEEDED(dev->Activate(kIID_IAudioEndpointVolume, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(vol.put())))) {
            ok = fn(vol.p);
        }
    }
    if (SUCCEEDED(ci)) CoUninitialize();
    return ok;
}

std::string endpointVolumeJson() {
    float level = 0.0f;
    BOOL muted = FALSE;
    if (!withEndpointVolume([&](IAudioEndpointVolume* v) {
            return SUCCEEDED(v->GetMasterVolumeLevelScalar(&level)) && SUCCEEDED(v->GetMute(&muted));
        }))
        return {};
    return ",\"volume\":{\"level\":" + std::to_string(level) + ",\"muted\":" + (muted ? "true" : "false") + "}";
}

/**
 * Van-e az eszköznek hardveres hangereje. Ha nem olvasható, nincs (false): a csúszka ilyenkor a digitális úton
 * szabályoz, és nem hat csendben egy olyan hangerőre, amelyet nem lehet állítani.
 */
bool endpointHardwareVolume() {
    DWORD mask = 0;
    if (!withEndpointVolume([&](IAudioEndpointVolume* v) { return SUCCEEDED(v->QueryHardwareSupport(&mask)); })) return false;
    return (mask & ENDPOINT_HARDWARE_SUPPORT_VOLUME) != 0;
}

void setEndpointVolume(double level, int mute) {
    withEndpointVolume([&](IAudioEndpointVolume* v) {
        if (level >= 0.0) v->SetMasterVolumeLevelScalar(static_cast<float>(std::min(1.0, level)), nullptr);
        if (mute >= 0) v->SetMute(mute ? TRUE : FALSE, nullptr);
        return true;
    });
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
    PlayerDsp() : tap_(std::make_unique<SharedTapRing>()) {
        chain_.setSampleRate(48000);
        chain_.startWorker();
    }
    ~PlayerDsp() override { chain_.stopWorker(); }

    void setParams(const ParamData& p) { params_.write(p); }
    void submitIr(IrData ir) { chain_.submitIr(std::move(ir)); }
    /** A felhasználó választása: a DAC hardveres hangereje (true) vagy a digitális (false). A következő számtól hat. */
    void setHardwarePref(bool on) { hwPref_ = on; }

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
        // a hardveres út csak akkor, ha a felhasználó azt választotta és az eszköz tényleg tud hangerőt
        hwVolume_ = hwPref_.load() && endpointHardwareVolume();
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
};

/** Az EQ APO plugin közös memóriájának figyelése és a jelforrás kiválasztása. */
class SourceManager {
public:
    SourceManager(win::WasapiLoopback& lb, PlayerDsp& dsp, player::WasapiPlayerOutput& out)
        : loopback_(lb), player_(dsp), playerOut_(out), apoDevice_(loadApoDevice()) {}

    /** Rendszeresen hívandó (pl. 250 ms-onként) a fő szálból. */
    void poll(const ParamData& currentParams) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!mapping_.state()) {
            if (mapping_.open()) {
                std::cout << "[Plugin] Aqulify EQ APO plugin közös memória megtalálva ("
                          << (mapping_.isGlobal() ? "Global" : "Local") << ").\n";
                // A GUI-ban beállított, mentett lánc átadása a pluginnak
                mapping_.state()->params.write(currentParams);   // a régi, leállás előtti lánc nem maradhat élesen
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
        const bool alive = pluginAliveLocked();
        if (alive != pluginActive_) {
            pluginActive_ = alive;
            std::cout << (alive ? "[Forrás] EQ APO plugin jel aktív (DSP élőben vezérelhető).\n"
                                : "[Forrás] WASAPI loopback (csak analízis; a DSP-hez az EQ APO plugin kell).\n");
            if (alive) pluginReadPos_ = mapping_.state()->tap.writePos.load();
            else loopbackReadPos_ = loopback_.ring().writePos.load();
            playback_.reset();   // a lejátszás mérése a plugin leállásától indul
        }
        if (!loopback_.active()) playback_.reset();   // nincs lehallgatás (nincs eszköz): nincs lejátszás
        // Az eszköz, amelyen a plugin fut (az alapértelmezett lejátszóeszköz): ha később egy másik eszközre
        // vált a hang (pl. bedugott USB hangkártya), a felület meg tudja mondani, hol működött eddig
        if (pluginActive_) {
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
            // szünetben nincs feldolgozás: csend, hogy a vízesés lecsengjen
            if (now - player_.lastDataMs() > 100 && now - lastPlayerData_ > 20) {
                const uint64_t ms = std::min<uint64_t>(now - lastPlayerData_, 100);
                const size_t frames = std::min<size_t>(maxFrames, static_cast<size_t>(ms * player_.sampleRate() / 1000));
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
        if (playerActive_) return player_.sampleRate();
        if (pluginActive_ && mapping_.state()) return mapping_.state()->sampleRate.load();
        return loopback_.sampleRate();
    }

    /** A Bass Enhancer felharmonikus-szintje (dB) a pluginból; plugin nélkül nincs feldolgozás (-100). */
    double bassHarmonicsDb() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (playerActive_) return player_.bassHarmonicsDb();
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
        player_.setParams(p);
        if (mapping_.state()) mapping_.state()->params.write(p);
    }

    /** Rövid, emberi leírás a tálcaikonhoz. */
    std::wstring label() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (playerActive_) {
            wchar_t buf[96];
            swprintf(buf, 96, L"WASAPI exclusive (%.1f kHz)", player_.sampleRate() / 1000.0);
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
            std::snprintf(buf, sizeof(buf), ",\"source\":\"WASAPI exclusive\",\"dspActive\":true,\"sr\":%u,\"gr\":%.2f",
                          player_.sampleRate(), player_.gainReductionDb());
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
                in.deviceOk = loopback_.active();
                in.device = dev;
                in.apoDevice = apoDevice_;
                in.playingMs = playback_.playingMs();
                s += ",\"dspState\":\"" + std::string(dspStateName(classifyDspState(in))) + "\"";
                s += ",\"pluginExpected\":true";
                if (!apoDevice_.empty() && apoDevice_ != dev) s += ",\"apoDevice\":\"" + jsonEscape(apoDevice_) + "\"";
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
        if (playerActive_) return player_.magicDiagJson();
        if (!pluginActive_ || !mapping_.state()) return {};
        magic::MagicDiag d;
        if (!mapping_.state()->magicDiag.read(d)) return {};
        return magic::diagToJson(d);
    }

private:
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
    player::WasapiPlayerOutput& playerOut_;
    win::SharedMapping mapping_;
    std::mutex mutex_;
    bool playerActive_ = false;
    uint64_t playerReadPos_ = 0;
    uint64_t lastPlayerData_ = 0;
    bool pluginActive_ = false;
    uint64_t pluginReadPos_ = 0;
    uint64_t loopbackReadPos_ = 0;
    uint64_t lastPluginData_ = 0;
    std::string apoDevice_;
    PlaybackDetector playback_;
    std::mutex irMutex_;
    IrData ir_;
};

} // namespace

int main(int argc, char** argv) {
    const bool console = hasArg(argc, argv, "--console");
    win::LogSink log(console);

    std::cout << "========================================================\n";
    std::cout << " Aqulify • Fázis & Akusztikai Vízesés – Windows motor\n";
    std::cout << "========================================================\n\n";

    if (hasArg(argc, argv, "--help") || hasArg(argc, argv, "-h")) {
        std::cout << "Használat: Aqulify.exe [--background] [--console] [--port N]\n"
                     "  --background  indításkor ne nyissa meg a felületet (automatikus indításhoz)\n"
                     "  --console     naplózás konzolablakba a napló fájl helyett\n"
                     "  --port N      csak ezt a WebSocket portot próbálja (alapból: 47810-47814, majd 8080)\n";
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

    // A beépített lejátszó kimenete és a kizárólagos módú saját lánca (a lánc a lejátszónál tovább él)
    PlayerDsp playerDsp;
    auto playerOutOwned = std::make_unique<player::WasapiPlayerOutput>();
    player::WasapiPlayerOutput& playerOut = *playerOutOwned;
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
    // (a statusFields a lejátszó után, lent)
    cb.setVolume = [&](double level, int mute) {
        // kizárólagos módban, hardveres hangerő nélküli eszközön a csúszka a lánc digitális hangereje
        if (playerOut.exclusiveActive() && !playerDsp.hardwareVolume()) playerDsp.setVolume(level, mute);
        else setEndpointVolume(level, mute);
    };
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
            if (id.rfind("p_", 0) == 0) {
                if (phoneCache.resolve(id, path)) return true;
                if (cb.broadcast) {
                    cb.broadcast("{\"type\":\"phone_track_demand\",\"id\":\"" + media::jsonEscape(id) + "\"}");
                }
                return phoneCache.waitForTrack(id, std::chrono::milliseconds(2500), path);
            }
            return musicLibrary.trackPath(id, path);
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
        if (filePlayer.hasTrack()) playQueue.command(action, value);
        else mediaSession.command(action, value);
    };
    cb.mediaArt = [&](const std::string& id, std::string& mime, std::string& bytes, std::string& url) {
        if (filePlayer.art(id, mime, bytes)) return true;
        return mediaSession.art(id, mime, bytes, url);
    };
    cb.playerOpen = [&](const std::string& path, std::string& error) {
        if (!filePlayer.open(path, &error)) {
            std::cout << "[Lejátszó] " << path << ": " << error << "\n";
            return false;
        }
        std::cout << "[Lejátszó] Szól: " << path << "\n";
        return true;
    };
    cb.playerStop = [&] { filePlayer.stop(); };
    // Egykattintásos frissítés (a felület „Új verzió érhető el” ablakából): letöltés, ellenőrzés, a telepítő indítása
    win::Updater updater;
    cb.updateInstall = [&](const std::string& version, std::string& error) { return updater.start(version, error); };
    cb.statusFields = [&]() {
        std::string s = sources.statusFields() + updater.statusJson();
        const bool digitalVolume = playerOut.exclusiveActive() && !playerDsp.hardwareVolume();
        s += digitalVolume ? playerDsp.volumeJson() : endpointVolumeJson();
        if (!filePlayer.hasTrack()) return s;
        // A jelút (Hangkimenet, haladó): megosztott módban a Windows hangmotorja keveri a jelet (nem bitpontos);
        // kizárólagos módban bitpontos, ha sem a lánc, sem a ReplayGain, sem a hangerő, sem a formátum nem változtat rajta
        const bool excl = playerOut.exclusiveActive();
        const uint32_t rate = filePlayer.streamRate();
        const float rg = filePlayer.currentGainDb();
        const double dspMs = excl && rate ? 1000.0 * static_cast<double>(playerDsp.latencySamples()) / rate : 0.0;
        const bool transparent = excl && playerDsp.transparent() && !playerDsp.volumeTouches() && rg == 0.0f &&
                                 playerOut.quantExact();
        std::string out = playerOut.statusJson(dspMs);
        char pb[256];
        std::snprintf(pb, sizeof(pb), ",\"dspMs\":%.2f,\"transparent\":%s,\"mixer\":%s,\"keepAwake\":false,\"playerRate\":%u,"
                      "\"playerGain\":%.2f,\"digitalVolume\":%s,\"dspTransparent\":%s,\"volTouch\":%s",
                      dspMs, transparent ? "true" : "false", excl ? "false" : "true", rate, rg,
                      digitalVolume ? "true" : "false", playerDsp.transparent() ? "true" : "false",
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
    cb.loadRemote = [] { return readSmallFile(userFilePath(L"remote.json", false), 256 * 1024); };
    cb.saveRemote = [](const std::string& doc) { return writeFileAtomic(userFilePath(L"remote.json", true), doc); };
    // Ablak bezárása „Bezárás” választással: ugyanaz, mint a tálcamenü „Kilépés” pontja (rendes leállás)
    static std::atomic<win::TrayApp*> s_tray{ nullptr };
    cb.quitEngine = [] { if (win::TrayApp* t = s_tray.load()) t->quit(); };

    // Haladó hangkimenet Windowson: csak a beépített lejátszó beállításai (ReplayGain, előbetöltés, kizárólagos mód).
    // A pufferméretet és a mintavételt a Windows hangmotorja határozza meg (az Equalizer APO abban fut); kizárólagos
    // módban a fájl mintavétele szól.
    std::mutex audioMutex;
    audioopt::AudioOptions audioOpts;
    auto playerOnly = [](audioopt::AudioOptions o) {
        o.bufferFrames = 0;
        o.keepAwake = false;
        o.rateMode = audioopt::RateMode::System;
        o.resampleQuality = -1;
        return o;
    };
    {
        const std::string doc = readSmallFile(userFilePath(L"audio.json", false), 64 * 1024);
        json::Value v;
        if (!doc.empty() && json::parse(doc, v)) audioOpts = playerOnly(audioopt::fromJson(v));
        filePlayer.setReplayGain(audioOpts.replayGain, audioOpts.rgPreampDb, audioOpts.rgPreventClip);
        filePlayer.setPreload(audioOpts.preload);
        playerOut.setExclusive(audioOpts.exclusive);
        playerDsp.setHardwarePref(audioOpts.exclHwVolume);
    }
    cb.audioOptionsJson = [&]() -> std::string {
        std::lock_guard<std::mutex> lk(audioMutex);
        return "{\"opts\":" + audioopt::toJson(audioOpts) +
               ",\"caps\":{\"buffer\":false,\"keepAwake\":false,\"rate\":false,\"resample\":false,\"player\":true,"
               "\"exclusive\":true}}";
    };
    cb.setAudioOptions = [&](const json::Value& v) {
        std::lock_guard<std::mutex> lk(audioMutex);
        const audioopt::AudioOptions o = playerOnly(audioopt::fromJson(v, audioOpts));
        if (o == audioOpts) return;
        audioOpts = o;
        filePlayer.setReplayGain(o.replayGain, o.rgPreampDb, o.rgPreventClip);
        filePlayer.setPreload(o.preload);
        playerDsp.setHardwarePref(o.exclHwVolume);
        if (o.exclusive != playerOut.wantExclusive()) {
            playerOut.setExclusive(o.exclusive);
            std::cout << "[Lejátszó] Kizárólagos mód: " << (o.exclusive ? "be" : "ki") << ".\n";
        }
        writeFileAtomic(userFilePath(L"audio.json", true), audioopt::toJson(o));
    };
    cb.getLanguage = [] { return aq_i18n::savedLanguage(); };
    cb.setLanguage = [](const std::string& l) {
        if (aq_i18n::set(l) && !aq_i18n::saveLanguage(l)) std::cout << "[Nyelv] Figyelem: a language.txt nem írható.\n";
    };

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
        if (save) {
            if (!win::saveParamFile(snapshot)) std::cout << "[Beállítások] Figyelem: a ProgramData\\Aqulify\\params.bin nem írható.\n";
            if (!win::saveRestoreParamFile(snapshot)) std::cout << "[Beállítások] Figyelem: a last_params.bin nem írható.\n";
        }
    };

    win::TrayApp::Callbacks tcb;
    tcb.openGui = [] { openGui(); };
    tcb.openLog = [&] {
        if (!log.path().empty()) ShellExecuteW(nullptr, L"open", log.path().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    tcb.onTimer = [&] { saveIfDue(false); };
    tcb.statusText = [&] { return sources.label(); };
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
    bridge->stop();
    updater.stop();
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
    if (single) { ReleaseMutex(single); CloseHandle(single); }
    CoUninitialize();
    return 0;
}
