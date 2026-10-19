/**
 * @file aether_engine.cpp
 * @brief AetherAudio.exe – Windows 11 motor a fázis- és vízesés-analizátorhoz.
 *
 * Jelforrások (automatikus választás):
 *   1. AetherAudio EQ APO plugin (közös memória) – a feldolgozott jel pontosan a láncban lévő helyén,
 *      és a GUI-ról állított DSP (preamp, EQ, crossfeed, limiter) a plugin-ban, élőben hat a hangra.
 *   2. WASAPI loopback – az alapértelmezett lejátszóeszköz kimenete (csak analízis, plugin nélkül).
 *
 * A GUI (gui_integrated_system.html) a ws://127.0.0.1:8080 hídon csatlakozik, ugyanúgy, mint Linuxon.
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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <algorithm>
#include <cstring>

#include "ws_bridge.hpp"
#include "param_apply.hpp"
#include "../common/win_shared.hpp"
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
    for (const std::wstring& candidate : { dir + L"\\gui\\gui_integrated_system.html",
                                           dir + L"\\gui_integrated_system.html",
                                           dir + L"\\..\\gui_integrated_system.html" }) {
        if (fileExists(candidate)) {
            ShellExecuteW(nullptr, L"open", candidate.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            std::cout << "[GUI] Grafikus felület megnyitva az alapértelmezett böngészőben.\n";
            return;
        }
    }
    std::cout << "[GUI] A gui_integrated_system.html nem található az exe mellett (gui\\ mappa).\n";
}

bool hasArg(int argc, char** argv, const char* a) {
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], a) == 0) return true;
    return false;
}

/** Az EQ APO plugin közös memóriájának figyelése és a jelforrás kiválasztása. */
class SourceManager {
public:
    explicit SourceManager(win::WasapiLoopback& lb) : loopback_(lb) {}

    /** Rendszeresen hívandó (pl. 250 ms-onként) a fő szálból. */
    void poll(const ParamData& currentParams) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!mapping_.state()) {
            if (mapping_.open()) {
                std::cout << "[Plugin] AetherAudio EQ APO plugin közös memória megtalálva ("
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
        return readTapRing(loopback_.ring(), loopbackReadPos_, lr, maxFrames);
    }

    double sampleRate() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (pluginActive_ && mapping_.state()) return mapping_.state()->sampleRate.load();
        return loopback_.sampleRate();
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
        } else {
            std::snprintf(buf, sizeof(buf), ",\"source\":\"WASAPI loopback\",\"dspActive\":false,\"sr\":%u",
                          loopback_.sampleRate());
            s += buf;
            s += ",\"device\":\"" + jsonEscape(loopback_.deviceName()) + "\"";
        }
        return s;
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
    std::mutex irMutex_;
    IrData ir_;
};

} // namespace

int main(int argc, char** argv) {
    const bool console = hasArg(argc, argv, "--console");
    win::LogSink log(console);

    std::cout << "========================================================\n";
    std::cout << " AetherAudio • Fázis & Akusztikai Vízesés – Windows motor\n";
    std::cout << "========================================================\n\n";

    if (hasArg(argc, argv, "--help") || hasArg(argc, argv, "-h")) {
        std::cout << "Használat: AetherAudio.exe [--background] [--console] [--port N]\n"
                     "  --background  indításkor ne nyissa meg a felületet (automatikus indításhoz)\n"
                     "  --console     naplózás konzolablakba a napló fájl helyett\n"
                     "  --port N      WebSocket port (alapértelmezés: 8080; a GUI a 8080-at használja)\n";
        return 0;
    }
    int port = 8080;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0) port = std::atoi(argv[i + 1]);
    }
    const bool background = hasArg(argc, argv, "--background") || hasArg(argc, argv, "--no-gui");

    // Egy példány felhasználónként: a második indítás csak a felületet nyitja meg
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\AetherAudioEngine");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cout << "[Info] Már fut egy példány; csak a felület nyílik meg.\n";
        if (!background) openGui();
        CloseHandle(single);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

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

    WebSocketBridge bridge(port);
    if (!bridge.start(cb)) {
        std::cout << "[Hiba] A WebSocket híd nem indult el (a " << port << " port foglalt).\n";
        MessageBoxW(nullptr,
                    L"Az AetherAudio nem tudott elindulni, mert a 8080-as helyi portot egy másik program használja.\n\n"
                    L"Zárd be azt a programot, majd indítsd újra az AetherAudio-t.",
                    L"AetherAudio", MB_ICONWARNING | MB_OK);
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
            std::cout << "[Beállítások] Figyelem: a ProgramData\\AetherAudio\\params.bin nem írható.\n";
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
    bridge.stop();
    loopback->stop();
    saveIfDue(true);
    if (single) { ReleaseMutex(single); CloseHandle(single); }
    CoUninitialize();
    return 0;
}
