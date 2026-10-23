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

#include <atomic>
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
#include "param_block.hpp"
#include "aq_i18n.hpp"
#include "../common/win_shared.hpp"
#include "../common/eqapo_paths.hpp"
#include "dsp_state.hpp"
#include "wasapi_loopback.hpp"
#include "tray.hpp"

using namespace ee_analyzer;

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

// ---------------------------------------------------------------- EQ profilok
// A felület JSON dokumentuma: %APPDATA%\Aqulify\presets.json (felhasználónként, újraindítás után is megmarad)
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

bool hasArg(int argc, char** argv, const char* a) {
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], a) == 0) return true;
    return false;
}

/** Az EQ APO plugin közös memóriájának figyelése és a jelforrás kiválasztása. */
class SourceManager {
public:
    explicit SourceManager(win::WasapiLoopback& lb) : loopback_(lb), apoDevice_(loadApoDevice()) {}

    /** Rendszeresen hívandó (pl. 250 ms-onként) a fő szálból. */
    void poll(const ParamData& currentParams) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!mapping_.state()) {
            if (mapping_.open()) {
                std::cout << "[Plugin] Aqulify EQ APO plugin közös memória megtalálva ("
                          << (mapping_.isGlobal() ? "Global" : "Local") << ").\n";
                // A GUI-ban beállított, mentett lánc átadása a pluginnak
                if (currentParams.has(kFlagInitialized)) mapping_.state()->params.write(currentParams);
                {
                    std::lock_guard<std::mutex> ik(irMutex_);
                    if (!ir_.empty()) win::writeIrBlock(mapping_.state()->ir, ir_);
                }
                pluginReadPos_ = mapping_.state()->tap.writePos.load();
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
        if (pluginActive_ && mapping_.state()) return mapping_.state()->sampleRate.load();
        return loopback_.sampleRate();
    }

    /** A Bass Enhancer felharmonikus-szintje (dB) a pluginból; plugin nélkül nincs feldolgozás (-100). */
    double bassHarmonicsDb() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (pluginActive_ && mapping_.state()) return mapping_.state()->bassHarmonicsDb.load();
        return -100.0;
    }

    /** Új konvolver IR a GUI-ból: tárolás, mentés a pluginnak, és átadás a közös memórián. */
    void setIr(IrData&& ir) {
        {
            std::lock_guard<std::mutex> ik(irMutex_);
            ir_ = std::move(ir);
            if (!win::saveIrFile(ir_)) std::cout << "[IR] Figyelem: az ir.bin nem menthető a ProgramData mappába.\n";
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
        }
    }

    void pushParams(const ParamData& p) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (mapping_.state()) mapping_.state()->params.write(p);
    }

    /** Rövid, emberi leírás a tálcaikonhoz. */
    std::wstring label() {
        std::lock_guard<std::mutex> lk(mutex_);
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
        if (pluginActive_ && mapping_.state()) {
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
    win::SharedMapping mapping_;
    std::mutex mutex_;
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

    // Mentett beállítások (a GUI utolsó állapota)
    std::mutex paramMutex;
    ParamData current = transparentParams();
    if (win::loadParamFile(current)) {
        std::cout << "[Beállítások] Utolsó DSP lánc betöltve (" << current.numBands << " EQ sáv).\n";
    }
    bool dirty = false;
    auto lastChange = std::chrono::steady_clock::now();

    auto loopback = std::make_unique<win::WasapiLoopback>();
    loopback->start();

    SourceManager sources(*loopback);
    sources.loadStoredIr();
    sources.poll(current);

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
    cb.statusFields = [&]() { return sources.statusFields(); };
    cb.setIr = [&](IrData&& ir) { sources.setIr(std::move(ir)); };
    cb.getIr = [&]() { return sources.irCopy(); };
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
        if (save && !win::saveParamFile(snapshot)) {
            std::cout << "[Beállítások] Figyelem: a ProgramData\\Aqulify\\params.bin nem írható.\n";
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
    if (!tray.create(GetModuleHandleW(nullptr))) {
        std::cout << "[Hiba] A tálcaikon nem hozható létre.\n";
    }

    if (!background) openGui();
    std::cout << "Fut a háttérben (tálcaikon).\n";

    tray.run();

    std::cout << "[Info] Leállítás...\n";
    bridge->stop();
    loopback->stop();
    saveIfDue(true);
    if (single) { ReleaseMutex(single); CloseHandle(single); }
    CoUninitialize();
    return 0;
}
