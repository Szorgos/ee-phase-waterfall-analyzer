#include <iostream>
#include <csignal>
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

    // Alapértelmezett audiofil profil betöltése a csomópontba
    node.pipeline.preampGainDb = -2.0;
    node.pipeline.crossfeed.setPreset(CrossfeedPreset::BauerDefault);
    node.pipeline.limiter.thresholdDb = -0.2;
    node.pipeline.limiter.ceilingDb = -0.1;
    node.pipeline.updateEqCoefficients();

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

    WebSocketBridge bridge(8080);
    g_bridge = &bridge;
    bridge.start(&node.pipeline, &node.visualizerRingBuffer);

    std::cout << "[4] Sikeresen csatlakozva! A csomópont aktívan szűri a hangot.\n";
    std::cout << "Nyomj Ctrl+C-t a leállításhoz.\n";

    node.run();
    bridge.stop();
    return 0;
}
