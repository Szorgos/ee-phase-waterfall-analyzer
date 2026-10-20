/**
 * @file pipewire_plugin.cpp
 * @brief AetherAudio motor Linuxra (PipeWire).
 *
 * Létrehoz egy "AetherAudio" virtuális kimeneti eszközt; ami ide szól, az a DSP láncon át a valódi
 * hangkártyára megy. A felület a WebSocket hídon (127.0.0.1:47810–47814, végső tartalék: 8080) csatlakozik.
 * A beállítások és a konvolver IR a ~/.config/aetheraudio mappába mentődnek.
 */
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "linux_store.hpp"
#include "pipewire_sink_node.hpp"
#include "ws_bridge.hpp"

using namespace ee_analyzer;

namespace {

// A GUI (gui_integrated_system.html: ENGINE_PORTS) ugyanezt a listát próbálja végig
const int kEnginePorts[] = { 47810, 47811, 47812, 47813, 47814, 8080 };

AetherSinkNode* g_node = nullptr;

void onSignal(int) {
    if (g_node) g_node->quit(); // pw_main_loop_quit szignálbiztos (eventfd)
}

bool hasArg(int argc, char** argv, const char* a) {
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], a) == 0) return true;
    return false;
}

struct Saver {
    std::mutex* mutex = nullptr;
    ParamData* current = nullptr;
    bool* dirty = nullptr;
    std::chrono::steady_clock::time_point* lastChange = nullptr;
};

void saveTick(void* data, uint64_t) {
    auto* s = static_cast<Saver*>(data);
    ParamData snapshot;
    {
        std::lock_guard<std::mutex> lk(*s->mutex);
        if (!*s->dirty || std::chrono::steady_clock::now() - *s->lastChange < std::chrono::milliseconds(600)) return;
        *s->dirty = false;
        snapshot = *s->current;
    }
    if (!linux_store::saveParams(snapshot))
        std::cerr << "[Beállítások] Figyelem: a " << linux_store::configDir(false) << "/params.bin nem írható.\n";
}

} // namespace

int main(int argc, char* argv[]) {
    std::cout << "========================================================\n";
    std::cout << " AetherAudio • Fázis & Akusztikai Vízesés – Linux motor (PipeWire)\n";
    std::cout << "========================================================\n";

    if (hasArg(argc, argv, "--help") || hasArg(argc, argv, "-h")) {
        std::cout << "Használat: aetheraudio-engine [--port N]\n"
                     "  --port N   csak ezt a WebSocket portot próbálja (alapból: 47810-47814, majd 8080)\n"
                     "A motor egy \"AetherAudio\" nevű kimeneti hangeszközt hoz létre. Válaszd ki a hangbeállításokban\n"
                     "(vagy: pactl set-default-sink aetheraudio_sink), és minden hang a DSP láncon át szól.\n";
        return 0;
    }
    std::vector<int> ports(std::begin(kEnginePorts), std::end(kEnginePorts));
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--port") == 0) ports = { std::atoi(argv[i + 1]) };

    // Utolsó beállítások; első indításkor átlátszó (minden modul kikapcsolva), a felület adja át a sajátját
    std::mutex paramMutex;
    ParamData current = transparentParams();
    if (linux_store::loadParams(current))
        std::cout << "[Beállítások] Utolsó DSP lánc betöltve (" << current.numBands << " EQ sáv).\n";
    bool dirty = false;
    auto lastChange = std::chrono::steady_clock::now();

    AetherSinkNode node(48000);
    g_node = &node;
    node.params.write(current);
    {
        IrData ir;
        if (linux_store::loadIr(ir)) {
            std::cout << "[Konvolver] Mentett impulzusválasz betöltve: " << ir.name << "\n";
            node.chain.submitIr(std::move(ir));
        }
    }

    if (!node.start(argc, argv)) {
        std::cerr << "[Hiba] A PipeWire nem érhető el. Fut a pipewire (és a wireplumber) szolgáltatás?\n";
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::cout << "[PipeWire] \"AetherAudio\" kimeneti eszköz létrehozva (aetheraudio_sink).\n";

    uint64_t readPos = 0;
    BridgeCallbacks cb;
    cb.pullAudio = [&](float* lr, size_t maxFrames) { return readTapRing(node.visualizerTap, readPos, lr, maxFrames); };
    cb.sampleRate = [&]() { return static_cast<double>(node.sampleRate.load()); };
    cb.getParams = [&]() { std::lock_guard<std::mutex> lk(paramMutex); return current; };
    cb.setParams = [&](const ParamData& p) {
        {
            std::lock_guard<std::mutex> lk(paramMutex);
            current = p;
            dirty = true;
            lastChange = std::chrono::steady_clock::now();
        }
        node.params.write(p);
    };
    cb.statusFields = [&]() {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "\"platform\":\"linux\",\"source\":\"PipeWire – AetherAudio\",\"dspActive\":%s,\"sr\":%u,\"gr\":%.2f",
                      node.playbackStreaming.load() ? "true" : "false", node.sampleRate.load(),
                      node.chain.gainReductionDb());
        return std::string(buf);
    };
    cb.setIr = [&](IrData&& ir) {
        if (!linux_store::saveIr(ir)) std::cerr << "[Konvolver] Figyelem: az IR nem menthető.\n";
        node.chain.submitIr(std::move(ir));
    };
    cb.getIr = [&]() { return node.chain.irCopy(); };

    std::unique_ptr<WebSocketBridge> bridge;
    for (int p : ports) {
        auto b = std::make_unique<WebSocketBridge>(p);
        if (b->start(cb)) { bridge = std::move(b); break; }
        std::cout << "[Info] A " << p << " port foglalt, a következőt próbálom.\n";
    }
    if (!bridge) {
        std::cerr << "[Hiba] Minden kapcsolódási port foglalt; a felület nem tud csatlakozni.\n"
                     "       Ellenőrizd: ss -ltnp | grep -E '4781|8080'\n";
        node.shutdown();
        return 1;
    }

    Saver saver{ &paramMutex, &current, &dirty, &lastChange };
    node.addTimer(saveTick, &saver, 1000);

    std::cout << "Fut. Leállítás: Ctrl+C (vagy systemctl --user stop aetheraudio).\n";
    node.run();

    std::cout << "[Info] Leállítás...\n";
    bridge->stop();
    {
        std::lock_guard<std::mutex> lk(paramMutex);
        if (dirty) linux_store::saveParams(current);
    }
    const bool crashedDaemon = node.coreError.load();
    node.shutdown();
    return crashedDaemon ? 2 : 0;
}
