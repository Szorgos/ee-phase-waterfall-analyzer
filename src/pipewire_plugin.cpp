/**
 * @file pipewire_plugin.cpp
 * @brief Aqulify motor Linuxra (PipeWire).
 *
 * Létrehoz egy "Aqulify" virtuális kimeneti eszközt; ami ide szól, az a DSP láncon át a valódi
 * hangkártyára megy. A felület a WebSocket hídon (127.0.0.1:47810–47814, végső tartalék: 8080) csatlakozik.
 * A beállítások és a konvolver IR a ~/.config/aqulify mappába mentődnek.
 */
#include <algorithm>
#include <cctype>
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

#include "file_player.hpp"
#include "library_bridge.hpp"
#include "linux_store.hpp"
#include "media_mpris.hpp"
#include "pipewire_sink_node.hpp"
#include "player_output_pw.hpp"
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

// ---------------------------------------------------------------- fizikai kimenet (pactl)
// A pactl kimenete nyelvfüggő ("Sink Input #12" / "12. nyelő-bemenet"), ezért mindig LC_ALL=C-vel hívjuk.

std::string runCapture(const std::string& cmd) {
    std::string out;
    FILE* fp = popen(("LC_ALL=C " + cmd + " 2>/dev/null").c_str(), "r");
    if (!fp) return out;
    char buf[1024];
    while (fgets(buf, sizeof(buf), fp)) out += buf;
    pclose(fp);
    return out;
}

/** Csak PipeWire/PulseAudio nyelőnevekben előforduló karakterek (shell-injekció ellen). */
bool isSafeSinkName(const std::string& n) {
    if (n.empty() || n.size() > 200) return false;
    for (char c : n)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-' || c == ':' || c == '+' || c == '@'))
            return false;
    return true;
}

struct SinkInfo { std::string id, name; };

std::vector<SinkInfo> listPhysicalSinks() {
    std::vector<SinkInfo> v;
    const std::string txt = runCapture("pactl list short sinks");
    size_t pos = 0;
    while (pos < txt.size()) {
        size_t eol = txt.find('\n', pos);
        if (eol == std::string::npos) eol = txt.size();
        const std::string line = txt.substr(pos, eol - pos);
        pos = eol + 1;
        const size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) t2 = line.size();
        SinkInfo s{ line.substr(0, t1), line.substr(t1 + 1, t2 - t1 - 1) };
        if (s.name == "aqulify_sink" || !isSafeSinkName(s.name)) continue; // a saját virtuális nyelőnk nem cél
        v.push_back(std::move(s));
    }
    return v;
}

/** A motor lejátszó streamjének (aqulify_sink_output) sink-input azonosítója és a nyelő, amelyre most szól. */
bool findOwnSinkInput(std::string& sinkInputId, std::string& sinkId) {
    sinkInputId.clear();
    sinkId.clear();
    const std::string txt = runCapture("pactl list sink-inputs");
    std::string curId, curSink;
    size_t pos = 0;
    while (pos < txt.size()) {
        size_t eol = txt.find('\n', pos);
        if (eol == std::string::npos) eol = txt.size();
        std::string line = txt.substr(pos, eol - pos);
        pos = eol + 1;
        const size_t f = line.find_first_not_of(" \t");
        if (f == std::string::npos) continue;
        line = line.substr(f);
        if (line.rfind("Sink Input #", 0) == 0) { curId = line.substr(12); curSink.clear(); }
        else if (line.rfind("Sink: ", 0) == 0) curSink = line.substr(6);
        else if (line.find("node.name = \"aqulify_sink_output\"") != std::string::npos && !curId.empty()) {
            sinkInputId = curId;
            sinkId = curSink;
            return std::all_of(sinkInputId.begin(), sinkInputId.end(), [](char c) { return c >= '0' && c <= '9'; });
        }
    }
    return false;
}

std::string sinkLabel(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.find("speaker") != std::string::npos) return "🔊 Hangszóró (" + name + ")";
    if (lower.find("usb") != std::string::npos) return "🎧 USB (" + name + ")";
    if (lower.find("hdmi") != std::string::npos) return "🖥️ HDMI (" + name + ")";
    if (lower.find("bluez") != std::string::npos) return "📶 Bluetooth (" + name + ")";
    if (lower.find("analog") != std::string::npos) return "🔈 Analóg kimenet (" + name + ")";
    return "🔈 " + name;
}

std::string getOutputSinksJson(std::string* outCurrentSink = nullptr) {
    std::string siId, curSinkId;
    findOwnSinkInput(siId, curSinkId);
    std::string currentSinkName;
    std::string json = "[";
    bool first = true;
    for (const auto& s : listPhysicalSinks()) {
        const bool active = !curSinkId.empty() && curSinkId == s.id;
        if (active) currentSinkName = s.name;
        if (!first) json += ",";
        first = false;
        json += "{\"id\":\"" + jsonEscape(s.id) + "\",\"name\":\"" + jsonEscape(s.name) + "\",\"label\":\"" +
                jsonEscape(sinkLabel(s.name)) + "\",\"active\":" + (active ? "true" : "false") + "}";
    }
    json += "]";
    if (outCurrentSink) *outCurrentSink = currentSinkName;
    return json;
}

/**
 * A DSP kimenetét a megadott fizikai nyelőre irányítja. Csak létező nyelőnévre (vagy régi
 * "speaker"/"usb"/"hdmi" kulcsszóra) visz át; ismeretlen szövegből soha nem lesz parancs.
 */
bool applyOutputSink(const std::string& targetSink, bool remember) {
    if (targetSink.empty()) return false;
    std::string sinkInputId, curSinkId;
    if (!findOwnSinkInput(sinkInputId, curSinkId)) return false;

    std::string lowerTarget = targetSink;
    std::transform(lowerTarget.begin(), lowerTarget.end(), lowerTarget.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string resolved, resolvedId;
    for (const auto& s : listPhysicalSinks()) {
        if (s.name == targetSink || s.id == targetSink) { resolved = s.name; resolvedId = s.id; break; }
        std::string ln = s.name;
        std::transform(ln.begin(), ln.end(), ln.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool kw = (lowerTarget == "speaker" || lowerTarget == "usb" || lowerTarget == "hdmi") && ln.find(lowerTarget) != std::string::npos;
        if (kw && resolved.empty()) { resolved = s.name; resolvedId = s.id; }
    }
    if (resolved.empty() || !isSafeSinkName(resolved)) return false;
    if (resolvedId == curSinkId) {                    // már ott szól
        if (remember) linux_store::saveOutputSink(resolved);
        return true;
    }
    const std::string cmd = "LC_ALL=C pactl move-sink-input " + sinkInputId + " '" + resolved + "' >/dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0) return false;
    std::cout << "[PipeWire] Kimeneti eszköz: " << resolved << " (sink-input #" << sinkInputId << ")\n";
    if (remember) linux_store::saveOutputSink(resolved);
    return true;
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
    std::cout << " Aqulify • Fázis & Akusztikai Vízesés – Linux motor (PipeWire)\n";
    std::cout << "========================================================\n";

    if (hasArg(argc, argv, "--help") || hasArg(argc, argv, "-h")) {
        std::cout << "Használat: aqulify-engine [opciók]\n"
                     "  --port N         csak ezt a WebSocket portot próbálja (alapból: 47810-47814, majd 8080)\n"
                     "  --listen lan     hálózati távvezérlés bekapcsolása (47820-as port és mDNS hirdető)\n"
                     "  --headless       kijelző nélküli szerver üzemmód (auto-LAN, PIN és setup párosítás)\n"
                     "  --web-root DIR   a beépített HTTP szerver felületi mappája\n"
                     "A motor egy \"Aqulify\" nevű kimeneti hangeszközt hoz létre. Válaszd ki a hangbeállításokban\n"
                     "(vagy: pactl set-default-sink aqulify_sink), és minden hang a DSP láncon át szól.\n";
        return 0;
    }
    // Nyelv (a hangbeállításokban látszó kimenet neve): a felületen választott, különben a munkameneté
    aq_i18n::init();
    std::vector<int> ports(std::begin(kEnginePorts), std::end(kEnginePorts));
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--port") == 0) ports = { std::atoi(argv[i + 1]) };

    const bool headless = hasArg(argc, argv, "--headless");
    bool listenLan = hasArg(argc, argv, "--remote");
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--listen") == 0 && i + 1 < argc && std::strcmp(argv[i + 1], "lan") == 0) {
            listenLan = true;
        }
    }
    std::string webRoot;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--web-root") == 0) {
            webRoot = argv[i + 1];
        }
    }
    if (webRoot.empty()) {
        webRoot = linux_store::defaultGuiDir();
    }

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
    std::cout << "[PipeWire] \"Aqulify\" kimeneti eszköz létrehozva (aqulify_sink).\n";

    // Kezdeti kimenet: csak ha a felhasználó korábban választott. Különben a rendszer (WirePlumber)
    // alapértelmezett eszközén marad – nem kényszerítjük pl. a laptop hangszóróra.
    const std::string initialSink = linux_store::loadOutputSink();
    if (!initialSink.empty()) {
        std::thread([initialSink]() {
            for (int i = 0; i < 25; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (applyOutputSink(initialSink, false)) break;
            }
        }).detach();
    }

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
    cb.bassHarmonics = [&]() { return node.chain.bassHarmonicsDb(); };

    std::string cachedSinksJson = "[]";
    std::string cachedCurrentSink = "";
    auto lastSinkQuery = std::chrono::steady_clock::time_point{};

    auto updateSinksCache = [&]() {
        cachedSinksJson = getOutputSinksJson(&cachedCurrentSink);
        lastSinkQuery = std::chrono::steady_clock::now();
    };

    cb.setOutputSink = [&](const std::string& target) {
        if (!applyOutputSink(target, true))
            std::cerr << "[PipeWire] A kért kimenet nem található vagy nem állítható be.\n";
        updateSinksCache();
    };

    cb.statusFields = [&]() {
        auto now = std::chrono::steady_clock::now();
        if (now - lastSinkQuery > std::chrono::seconds(2)) {
            updateSinksCache();
        }
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "\"platform\":\"linux\",\"source\":\"PipeWire – Aqulify\",\"dspActive\":%s,\"sr\":%u,\"gr\":%.2f",
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
    cb.loadLibraryIr = linux_store::loadLibraryIr;
    cb.saveLibraryIr = linux_store::saveLibraryIr;
    cb.removeLibraryIr = linux_store::removeLibraryIr;
    std::mutex presetMutex;
    std::string presetDoc = linux_store::loadPresets();
    cb.getPresets = [&]() { std::lock_guard<std::mutex> lk(presetMutex); return presetDoc; };
    cb.setPresets = [&](const std::string& doc) {
        std::lock_guard<std::mutex> lk(presetMutex);
        presetDoc = doc;
        if (!linux_store::savePresets(doc)) std::cerr << "[Profilok] Figyelem: a presets.json nem írható.\n";
    };
    cb.magicDiagJson = [&]() {
        {
            std::lock_guard<std::mutex> lk(paramMutex);
            if (!(current.has(kFlagMagic) && current.magicAmount > 0.0f)) return std::string();
        }
        magic::MagicDiag d;
        return node.chain.magicDiag(d) ? magic::diagToJson(d) : std::string();
    };
    // A gépen szóló lejátszó (MPRIS): „most szól” és vezérlés a felületről és a távirányítóról
    media::MprisSession mediaSession;
    mediaSession.start();
    // A beépített lejátszó (2.x): az „Aqulify” eszközre játszik, így a hang a DSP-láncon át szól. Amíg van betöltött
    // szám, a „most szól” és a vezérlés ezt mutatja; utána ismét a gépen futó lejátszót (MPRIS).
    player::FilePlayer filePlayer(std::make_unique<player::PwPlayerOutput>("aqulify_sink"));
    // Zenetár (2.x): a mentett zenei mappák, különben a Zene mappa; a bejárás a háttérben indul
    library::MusicLibrary musicLibrary;
    // A lejátszási napló (3c2, stats.json): a sor előtt, mert a sor mérőszála a megszűnéséig hívhatja
    player::PlayStats playStats([](const std::string& doc) {
        if (!linux_store::saveStats(doc)) std::cerr << "[Lejátszó] Figyelem: a stats.json nem írható.\n";
    });
    playStats.load(linux_store::loadStats());
    // Telefonos zeneátvitel gyorsítótára (4b)
    player::PhoneCache phoneCache(linux_store::configDir(true) + "/phone-cache");
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
            if (!linux_store::saveQueue(doc)) std::cerr << "[Lejátszó] Figyelem: a queue.json nem írható.\n";
        });
    playQueue.restore(linux_store::loadQueue());
    // Lejátszási listák (3c): a motor tárolja (playlists.json)
    player::PlaylistStore playlists([](const std::string& doc) {
        if (!linux_store::savePlaylists(doc)) std::cerr << "[Lejátszó] Figyelem: a playlists.json nem írható.\n";
    });
    playlists.load(linux_store::loadPlaylists());
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
            std::cerr << "[Lejátszó] " << path << ": " << error << "\n";
            return false;
        }
        std::cout << "[Lejátszó] Szól: " << path << "\n";
        return true;
    };
    cb.playerStop = [&] { filePlayer.stop(); };
    {
        musicLibrary.setCacheStorage([] { return linux_store::loadLibraryCache(); },
                                     [](const std::string& doc) {
                                         if (!linux_store::saveLibraryCache(doc))
                                             std::cerr << "[Zenetár] Figyelem: a library-cache.json nem írható.\n";
                                     });
        std::vector<std::string> roots = library::parseRootsDoc(linux_store::loadLibrary());
        if (roots.empty()) roots.push_back(library::MusicLibrary::defaultMusicDir());
        musicLibrary.setRoots(roots);
    }
    library::attachLibrary(cb, musicLibrary, playQueue, playlists, playStats, [](const std::string& doc) {
        if (!linux_store::saveLibrary(doc)) std::cerr << "[Zenetár] Figyelem: a library.json nem írható.\n";
    }, &phoneCache);
    cb.loadRemote = [] { return linux_store::loadRemote(); };
    cb.saveRemote = [](const std::string& doc) { return linux_store::saveRemote(doc); };
    cb.savePairPin = linux_store::savePairPin;
    cb.getLanguage = [] { return aq_i18n::savedLanguage(); };
    cb.setLanguage = [](const std::string& l) {
        if (aq_i18n::set(l) && !aq_i18n::saveLanguage(l)) std::cerr << "[Nyelv] Figyelem: a language.txt nem írható.\n";
    };

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

    if (!webRoot.empty()) {
        bridge->setWebRootDir(webRoot);
        std::cout << "[HTTP] Beépített felület mappája: " << webRoot << "\n";
    }
    if (headless) {
        bridge->setHeadless(true);
        std::cout << "[Headless] Kijelző nélküli szerver üzemmód aktív (Pi / headless).\n";
    } else if (listenLan) {
        bridge->setRemoteEnabled(true);
        std::cout << "[Hálózat] Távvezérlés parancssorból bekapcsolva (--listen lan).\n";
    }

    Saver saver{ &paramMutex, &current, &dirty, &lastChange };
    node.addTimer(saveTick, &saver, 1000);

    std::cout << "Fut. Leállítás: Ctrl+C (vagy systemctl --user stop aqulify).\n";
    node.run();

    std::cout << "[Info] Leállítás...\n";
    bridge->stop();
    filePlayer.stop();
    mediaSession.stop();
    {
        std::lock_guard<std::mutex> lk(paramMutex);
        if (dirty) linux_store::saveParams(current);
    }
    const bool crashedDaemon = node.coreError.load();
    node.shutdown();
    return crashedDaemon ? 2 : 0;
}
