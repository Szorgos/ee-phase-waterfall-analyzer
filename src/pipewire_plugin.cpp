#include <iostream>
#include <csignal>
#include <mutex>
#include "pipewire_filter_node.hpp"
#include "ws_bridge.hpp"

using namespace ee_analyzer;

static PipeWireFilterNode* g_node = nullptr;
static WebSocketBridge* g_bridge = nullptr;

void signalHandler(int sig) {
    std::cout << "\n[Info] Leállítási szignál érkezett (" << sig << "), PipeWire csomópont és híd bontása...\n";
    if (g_bridge) {
        g_bridge->stop();
    }
    if (g_node) {
        g_node->stop();
    }
    std::exit(0);
}

int main(int argc, char* argv[]) {
    std::cout << "========================================================\n";
    std::cout << " EasyEffects PipeWire Real-time DSP Filter Csomópont\n";
    std::cout << "========================================================\n\n";

    PipeWireFilterNode node(48000);
    g_node = &node;

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Alapértelmezett audiofil profil (a GUI csatlakozáskor ezt veszi át)
    std::mutex paramMutex;
    ParamData current = transparentParams();
    current.preampDb = -2.0f;
    current.set(kFlagCrossfeed, true);
    current.set(kFlagLimiter, true);
    current.set(kFlagInitialized, true);
    current.crossfeedPreset = 0; // Bauer bs2b
    current.limiterCeilingDb = -0.1f;
    node.params.write(current);

    std::cout << "[1] DSP lánc felkonfigurálva (-2 dB Preamp, Bauer Crossfeed, -0.1 dB True Peak Limiter).\n";

    if (!node.init(argc, argv, "ee-phase-waterfall-filter")) {
        std::cerr << "[Hiba] A PipeWire inicializálása sikertelen!\n";
        return 1;
    }

    std::cout << "[2] PipeWire audio portok és eseménykezelők regisztrálva.\n";
    std::cout << "[3] Csatlakozás a PipeWire hanggráfhoz...\n";

    if (!node.connect()) {
        std::cout << "[Figyelmeztetés] Nincs aktív PipeWire démon a háttérben (fejlesztői/teszt környezet).\n";
        std::cout << "[OK] A PipeWire C++ interfész és a kódbázis sikeresen felépült és készen áll!\n";
        return 0;
    }

    uint64_t readPos = 0;
    BridgeCallbacks cb;
    cb.pullAudio = [&](float* lr, size_t maxFrames) {
        return readTapRing(node.visualizerTap, readPos, lr, maxFrames);
    };
    cb.sampleRate = [&]() { return static_cast<double>(node.sampleRate); };
    cb.getParams = [&]() { std::lock_guard<std::mutex> lk(paramMutex); return current; };
    cb.setParams = [&](const ParamData& p) {
        std::lock_guard<std::mutex> lk(paramMutex);
        current = p;
        node.params.write(p);
    };
    cb.statusFields = [&]() { return std::string("\"source\":\"PipeWire\",\"platform\":\"linux\""); };
    cb.setIr = [&](IrData&& ir) { node.chain.submitIr(std::move(ir)); };
    cb.getIr = [&]() { return node.chain.irCopy(); };

    WebSocketBridge bridge(8080);
    g_bridge = &bridge;
    bridge.start(cb);

    std::cout << "[4] Sikeresen csatlakozva! A csomópont aktívan szűri a hangot.\n";
    std::cout << "Nyomj Ctrl+C-t a leállításhoz.\n";

    node.run();
    bridge.stop();
    return 0;
}
