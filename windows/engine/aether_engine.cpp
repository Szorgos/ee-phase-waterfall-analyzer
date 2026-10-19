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

using namespace ee_analyzer;

namespace {

std::atomic<bool> g_running{true};

BOOL WINAPI consoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT ||
        type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

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

    void pushParams(const ParamData& p) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (mapping_.state()) mapping_.state()->params.write(p);
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
};

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    std::cout << "========================================================\n";
    std::cout << " AetherAudio • Fázis & Akusztikai Vízesés – Windows motor\n";
    std::cout << "========================================================\n\n";

    if (hasArg(argc, argv, "--help") || hasArg(argc, argv, "-h")) {
        std::cout << "Használat: AetherAudio.exe [--no-gui] [--port N]\n"
                     "  --no-gui   ne nyissa meg automatikusan a böngészős felületet\n"
                     "  --port N   WebSocket port (alapértelmezés: 8080; a GUI a 8080-at használja)\n";
        return 0;
    }
    int port = 8080;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0) port = std::atoi(argv[i + 1]);
    }
    const bool noGui = hasArg(argc, argv, "--no-gui");

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

    WebSocketBridge bridge(port);
    if (!bridge.start(cb)) {
        std::cout << "[Hiba] A WebSocket híd nem indult el. Valószínűleg már fut egy AetherAudio példány.\n";
        if (!noGui) openGui();
        loopback->stop();
        Sleep(3000);
        return 1;
    }

    if (!noGui) openGui();
    std::cout << "\nFut. Bezárás: Ctrl+C vagy az ablak bezárása.\n\n";

    while (g_running) {
        Sleep(250);
        ParamData snapshot;
        bool save = false;
        {
            std::lock_guard<std::mutex> lk(paramMutex);
            snapshot = current;
            if (dirty && std::chrono::steady_clock::now() - lastChange > std::chrono::milliseconds(600)) {
                dirty = false;
                save = true;
            }
        }
        sources.poll(snapshot);
        if (save && !win::saveParamFile(snapshot)) {
            std::cout << "[Beállítások] Figyelem: a ProgramData\\AetherAudio\\params.bin nem írható.\n";
        }
    }

    std::cout << "[Info] Leállítás...\n";
    bridge.stop();
    loopback->stop();
    {
        std::lock_guard<std::mutex> lk(paramMutex);
        if (dirty) win::saveParamFile(current);
    }
    return 0;
}
