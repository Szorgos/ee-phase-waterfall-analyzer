/**
 * @file pipewire_plugin.cpp
 * @brief AetherAudio motor Linuxra (PipeWire).
 *
 * Létrehoz egy "AetherAudio" virtuális kimeneti eszközt; ami ide szól, az a DSP láncon át a valódi
 * hangkártyára megy. A felület a WebSocket hídon (127.0.0.1:47810–47814, végső tartalék: 8080) csatlakozik.
 * A beállítások és a konvolver IR a ~/.config/aetheraudio mappába mentődnek.
 */
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

std::string jsonEscape(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

std::string getOutputSinksJson(std::string* outCurrentSink = nullptr) {
    std::string currentSinkId;
    std::string sinkInputId;
    {
        FILE* fp = popen("pactl list sink-inputs 2>/dev/null | awk '/^([0-9]+\\. nyel|Sink Input #)/{id=$1; gsub(/[^0-9]/,\"\",id)} /node\\.name = \"aetheraudio_sink_output\"/{print id; exit}'", "r");
        if (fp) {
            char buf[64];
            if (fgets(buf, sizeof(buf), fp)) {
                char* p = buf;
                while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
                char* end = p;
                while (*end && *end >= '0' && *end <= '9') ++end;
                sinkInputId = std::string(p, end - p);
            }
            pclose(fp);
        }
    }
    if (!sinkInputId.empty()) {
        FILE* fp = popen("pactl list short sink-inputs 2>/dev/null", "r");
        if (fp) {
            char line[256];
            while (fgets(line, sizeof(line), fp)) {
                char siId[32] = {0};
                char sId[32] = {0};
                if (sscanf(line, "%31s\t%31s", siId, sId) == 2) {
                    if (siId == sinkInputId) {
                        currentSinkId = sId;
                        break;
                    }
                }
            }
            pclose(fp);
        }
    }

    std::string currentSinkName;
    std::string json = "[";
    {
        FILE* fp = popen("pactl list short sinks 2>/dev/null", "r");
        if (fp) {
            char line[512];
            bool first = true;
            while (fgets(line, sizeof(line), fp)) {
                char sId[32] = {0};
                char sName[256] = {0};
                if (sscanf(line, "%31s\t%255s", sId, sName) >= 2) {
                    std::string name = sName;
                    if (name == "aetheraudio_sink") continue; // virtuális nyelő, kihagyjuk

                    bool active = (!currentSinkId.empty() && currentSinkId == sId);
                    if (active) currentSinkName = name;

                    std::string lower = name;
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                    std::string label;
                    if (lower.find("speaker") != std::string::npos) {
                        label = "🔊 Laptop Hangszóró";
                    } else if (lower.find("usb") != std::string::npos || lower.find("dac") != std::string::npos) {
                        label = "🎧 USB Audio (Dokkoló / DAC)";
                    } else if (lower.find("hdmi3") != std::string::npos) {
                        label = "🖥️ HDMI 3 Kimenet";
                    } else if (lower.find("hdmi2") != std::string::npos) {
                        label = "🖥️ HDMI 2 Kimenet";
                    } else if (lower.find("hdmi") != std::string::npos) {
                        label = "🖥️ HDMI Kimenet";
                    } else {
                        label = "🔈 " + name;
                    }

                    if (!first) json += ",";
                    first = false;
                    json += "{\"id\":" + std::string(sId) + ",\"name\":\"" + jsonEscape(name) + "\",\"label\":\"" + jsonEscape(label) + "\",\"active\":" + (active ? "true" : "false") + "}";
                }
            }
            pclose(fp);
        }
    }
    json += "]";
    if (outCurrentSink) *outCurrentSink = currentSinkName;
    return json;
}

bool applyOutputSink(const std::string& targetSink) {
    if (targetSink.empty()) return false;
    
    std::string sinkInputId;
    {
        FILE* fp = popen("pactl list sink-inputs 2>/dev/null | awk '/^([0-9]+\\. nyel|Sink Input #)/{id=$1; gsub(/[^0-9]/,\"\",id)} /node\\.name = \"aetheraudio_sink_output\"/{print id; exit}'", "r");
        if (fp) {
            char buf[64];
            if (fgets(buf, sizeof(buf), fp)) {
                char* p = buf;
                while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
                char* end = p;
                while (*end && *end >= '0' && *end <= '9') ++end;
                sinkInputId = std::string(p, end - p);
            }
            pclose(fp);
        }
    }
    if (sinkInputId.empty()) {
        return false;
    }

    std::string resolvedSink = targetSink;
    {
        FILE* fp = popen("pactl list short sinks 2>/dev/null", "r");
        if (fp) {
            char line[512];
            std::string lowerTarget = targetSink;
            std::transform(lowerTarget.begin(), lowerTarget.end(), lowerTarget.begin(), ::tolower);
            std::string bestMatch;
            while (fgets(line, sizeof(line), fp)) {
                char sId[32] = {0};
                char sName[256] = {0};
                if (sscanf(line, "%31s\t%255s", sId, sName) >= 2) {
                    std::string name = sName;
                    if (name == "aetheraudio_sink") continue;
                    if (name == targetSink || sId == targetSink) {
                        bestMatch = name;
                        break;
                    }
                    std::string lowerName = name;
                    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                    if (lowerTarget.find("speaker") != std::string::npos && lowerName.find("speaker") != std::string::npos) {
                        bestMatch = name;
                    } else if ((lowerTarget.find("usb") != std::string::npos || lowerTarget.find("dac") != std::string::npos) &&
                               lowerName.find("usb") != std::string::npos) {
                        bestMatch = name;
                    } else if (lowerTarget.find("hdmi") != std::string::npos && lowerName.find("hdmi") != std::string::npos) {
                        if (bestMatch.empty()) bestMatch = name;
                    }
                }
            }
            pclose(fp);
            if (!bestMatch.empty()) resolvedSink = bestMatch;
        }
    }

    std::string cmd = "pactl move-sink-input " + sinkInputId + " \"" + resolvedSink + "\" 2>/dev/null";
    int ret = std::system(cmd.c_str());
    if (ret == 0) {
        std::cout << "[PipeWire] Kimeneti eszköz átirányítva: " << resolvedSink << " (sink-input #" << sinkInputId << ")\n";
        linux_store::saveOutputSink(resolvedSink);
        return true;
    }
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

    // Kezdeti kimenet beállítása (mentett vagy alapértelmezett Speaker)
    std::string initialSink = linux_store::loadOutputSink();
    if (initialSink.empty()) {
        initialSink = "speaker";
    }

    // Háttérszál, amely a PipeWire playback stream regisztrációja után beállítja a kimenetet
    std::thread([initialSink]() {
        for (int i = 0; i < 25; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (applyOutputSink(initialSink)) break;
        }
    }).detach();

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
    cb.gainReduction = [&]() { return node.chain.gainReductionDb(); };

    std::string cachedSinksJson = "[]";
    std::string cachedCurrentSink = "";
    auto lastSinkQuery = std::chrono::steady_clock::time_point{};

    auto updateSinksCache = [&]() {
        cachedSinksJson = getOutputSinksJson(&cachedCurrentSink);
        lastSinkQuery = std::chrono::steady_clock::now();
    };

    cb.setOutputSink = [&](const std::string& target) {
        applyOutputSink(target);
        updateSinksCache();
    };

    cb.statusFields = [&]() {
        auto now = std::chrono::steady_clock::now();
        if (now - lastSinkQuery > std::chrono::seconds(2)) {
            updateSinksCache();
        }
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "\"platform\":\"linux\",\"source\":\"PipeWire – AetherAudio\",\"dspActive\":%s,\"sr\":%u,\"gr\":%.2f",
                      node.playbackStreaming.load() ? "true" : "false", node.sampleRate.load(),
                      node.chain.gainReductionDb());
        std::string s = buf;
        s += ",\"currentSink\":\"" + jsonEscape(cachedCurrentSink) + "\"";
        s += ",\"outputSinks\":" + cachedSinksJson;
        return s;
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
