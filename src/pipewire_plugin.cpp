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

#include "audio_options.hpp"
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

// ---------------------------------------------------------------- PipeWire-beállítások (settings metadata)
// A mintavétel (clock.allowed-rates, clock.force-rate) futás közben állítható; a motor leálláskor visszaállítja az
// eredetit. Csak számot és [ ]-t írunk be (nincs felhasználói szöveg a parancssorban).

std::string pwSetting(const std::string& key) {
    const std::string txt = runCapture("pw-metadata -n settings 0");
    const std::string tag = "key:'" + key + "' value:'";
    const size_t p = txt.find(tag);
    if (p == std::string::npos) return {};
    const size_t b = p + tag.size(), e = txt.find('\'', b);
    return e == std::string::npos ? std::string() : txt.substr(b, e - b);
}

bool isSafeSettingValue(const std::string& v) {
    return !v.empty() && std::all_of(v.begin(), v.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == ' ' || c == '[' || c == ']'; });
}

void pwSetSetting(const std::string& key, const std::string& value) {
    if (!isSafeSettingValue(value)) return;
    const std::string cmd = "pw-metadata -n settings 0 " + key + " '" + value + "' >/dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0) std::cerr << "[PipeWire] A beállítás nem írható: " << key << "\n";
}

// ---------------------------------------------------------------- hangerő (a kimeneti hangkártyáé)
// A „most szól” sáv csúszkája annak a hangkártyának a hangerejét állítja, amelyre az Aqulify szól (ha van hardveres
// hangereje, azt; a DSP-lánc ettől érintetlen marad). Az érték a pactl százaléka (mint a rendszer hangerő-szabályzójáé).

bool sinkVolume(const std::string& sink, double& level, bool& muted) {
    if (sink.empty() || !isSafeSinkName(sink)) return false;
    const std::string v = runCapture("pactl get-sink-volume '" + sink + "'");   // "Volume: front-left: 65536 / 100% / ..."
    const size_t pct = v.find('%');
    if (pct == std::string::npos) return false;
    size_t b = pct;
    while (b > 0 && std::isdigit(static_cast<unsigned char>(v[b - 1]))) --b;
    if (b == pct) return false;
    level = std::atoi(v.substr(b, pct - b).c_str()) / 100.0;
    muted = runCapture("pactl get-sink-mute '" + sink + "'").find("yes") != std::string::npos;
    return true;
}

void setSinkVolume(const std::string& sink, double level, int mute) {
    if (sink.empty() || !isSafeSinkName(sink)) return;
    if (level >= 0.0) {
        const int pct = static_cast<int>(std::lround(std::clamp(level, 0.0, 1.0) * 100.0));
        const std::string cmd = "LC_ALL=C pactl set-sink-volume '" + sink + "' " + std::to_string(pct) + "% >/dev/null 2>&1";
        if (std::system(cmd.c_str()) != 0) std::cerr << "[PipeWire] A hangerő nem állítható: " << sink << "\n";
    }
    if (mute >= 0) {
        const std::string cmd = "LC_ALL=C pactl set-sink-mute '" + sink + "' " + (mute ? "1" : "0") + " >/dev/null 2>&1";
        if (std::system(cmd.c_str()) != 0) std::cerr << "[PipeWire] A némítás nem állítható: " << sink << "\n";
    }
}

/** A lánc érintetlenül engedi-e át a jelet (bitpontos): nincs bekapcsolt modul, a Magic Touch sem fut. */
bool chainTransparent(const ParamData& d) { return !AetherChain::modifiesSignal(d); }

} // namespace

int main(int argc, char* argv[]) {
    std::cout << "========================================================\n";
    std::cout << " Aqulify • Fázis & Akusztikai Vízesés – Linux motor (PipeWire)\n";
    std::cout << "========================================================\n";

    if (hasArg(argc, argv, "--help") || hasArg(argc, argv, "-h")) {
        std::cout << "Használat: aqulify-engine [opciók]\n"
                     "  --port N         csak ezt a WebSocket portot próbálja (alapból: 47810-47814, majd 8080)\n"
                     "  --listen lan     hálózati távvezérlés bekapcsolása (47820-as port és mDNS hirdető)\n"
                     "A motor egy \"Aqulify\" nevű kimeneti hangeszközt hoz létre. Válaszd ki a hangbeállításokban\n"
                     "(vagy: pactl set-default-sink aqulify_sink), és minden hang a DSP láncon át szól.\n";
        return 0;
    }
    // Nyelv (a hangbeállításokban látszó kimenet neve): a felületen választott, különben a munkameneté
    aq_i18n::init();
    std::vector<int> ports(std::begin(kEnginePorts), std::end(kEnginePorts));
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--port") == 0) ports = { std::atoi(argv[i + 1]) };

    bool listenLan = hasArg(argc, argv, "--remote");
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--listen") == 0 && i + 1 < argc && std::strcmp(argv[i + 1], "lan") == 0) {
            listenLan = true;
        }
    }

    // Indításkor mindig alap (minden modul ki): az utolsó lánc nem áll vissza automatikusan,
    // a felület a saját állapotát a csatlakozáskor adja át
    std::mutex paramMutex;
    ParamData current = transparentParams();
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

    std::string cachedVolumeJson;   // "level/muted" a kimeneti hangkártyáé (üres: nem ismert)
    auto updateSinksCache = [&]() {
        cachedSinksJson = getOutputSinksJson(&cachedCurrentSink);
        double lv = 0.0;
        bool mu = false;
        cachedVolumeJson = sinkVolume(cachedCurrentSink, lv, mu)
                               ? ",\"volume\":{\"level\":" + std::to_string(lv) + ",\"muted\":" + (mu ? "true" : "false") + "}"
                               : std::string();
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
        s += cachedVolumeJson;
        return s;
    };
    cb.setVolume = [&](double level, int mute) {
        setSinkVolume(cachedCurrentSink, level, mute);
        lastSinkQuery = std::chrono::steady_clock::time_point{};   // a következő állapotüzenet már az új értéket mutatja
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
    // Ablak bezárása „Bezárás” választással: rendes leállás (0-s kilépési kód, a systemd nem indítja újra)
    cb.quitEngine = [] { if (g_node) g_node->quit(); };

    // Több kimenet (2–4 hangkártya). Induláskor ki van kapcsolva (alap állapot: csak a fő kimenet szól, a hangzás
    // nem változik); a felület a csatlakozáskor adja át. A 2–4. kimenet céleszköze csak létező fizikai nyelő lehet.
    // A beállítás és a zónák lánca / IR-je megmarad (multiout.bin, zoneN_ir.bin), de induláskor ki van kapcsolva.
    std::mutex multiMutex;
    multiout::MultiConfig multiState;
    std::array<std::string, 3> multiTargets{};
    if (linux_store::loadMultiOut(multiState, multiTargets)) {
        multiState.enabled = false;
        std::cout << "[Több kimenet] Mentett beállítás betöltve (" << multiState.numOutputs << " kimenet, kikapcsolva).\n";
    }
    for (int k = 1; k < multiout::kMaxOutputs; ++k) {
        IrData ir;
        if (linux_store::loadIrFile("zone" + std::to_string(k + 1) + "_ir.bin", ir)) node.zoneChain(k).submitIr(std::move(ir));
    }
    static std::atomic<bool> s_multiDirty{ false };
    auto pushMulti = [&] { node.setMultiOutput(multiState, multiTargets); s_multiDirty = true; };
    pushMulti();
    struct MultiSaver { std::mutex* m; multiout::MultiConfig* c; std::array<std::string, 3>* t; };
    static MultiSaver s_multiSaver{ &multiMutex, &multiState, &multiTargets };
    node.addTimer([](void* data, uint64_t) {
        if (!s_multiDirty.exchange(false)) return;
        auto* ms = static_cast<MultiSaver*>(data);
        std::lock_guard<std::mutex> lk(*ms->m);
        if (!linux_store::saveMultiOut(*ms->c, *ms->t)) std::cerr << "[Több kimenet] Figyelem: a multiout.bin nem írható.\n";
    }, &s_multiSaver, 1000);
    cb.setMultiOut = [&](const multiout::MultiConfig& in, const std::array<std::string, 3>& sinks) {
        const std::vector<SinkInfo> phys = listPhysicalSinks();
        std::lock_guard<std::mutex> lk(multiMutex);
        multiout::MultiConfig c = in;
        for (int k = 1; k < multiout::kMaxOutputs; ++k) c.outputs[k].chain = multiState.outputs[k].chain;   // a zónák lánca marad
        multiout::sanitize(c);
        for (int k = 0; k < 3; ++k) {
            const std::string& t = sinks[k];
            const bool known = !t.empty() && isSafeSinkName(t) &&
                               std::any_of(phys.begin(), phys.end(), [&](const SinkInfo& si) { return si.name == t; });
            if (!t.empty() && !known) std::cerr << "[Több kimenet] Ismeretlen céleszköz, kihagyva: " << t << "\n";
            multiTargets[k] = known ? t : std::string();
        }
        multiState = c;
        pushMulti();
        std::cout << "[Több kimenet] " << (c.enabled ? "bekapcsolva" : "kikapcsolva") << ", " << c.numOutputs << " kimenet, "
                  << (c.mode == multiout::Mode::Crossover ? "keresztváltó" : "független zónák") << "\n";
    };
    cb.multiOutStatus = [&]() -> std::string {
        std::lock_guard<std::mutex> lk(multiMutex);
        const auto& c = multiState;
        std::string s = "{\"enabled\":" + std::string(c.enabled ? "true" : "false") + ",\"mode\":\"" +
                        (c.mode == multiout::Mode::Crossover ? "crossover" : "zones") + "\",\"slope\":" +
                        std::to_string(c.slopeDbPerOct) + ",\"crossovers\":[";
        for (int j = 0; j < c.numCrossovers; ++j) s += (j ? "," : "") + std::to_string(c.crossoverHz[j]);
        s += "],\"outputs\":[";
        for (int k = 0; k < c.numOutputs; ++k) {
            const auto& o = c.outputs[k];
            if (k) s += ",";
            s += "{\"sink\":\"" + jsonEscape(k == 0 ? std::string() : multiTargets[k - 1]) + "\",\"band\":" + std::to_string(o.band) +
                 ",\"gain\":" + std::to_string(o.gainDb) + ",\"delay\":" + std::to_string(o.delayMs) +
                 ",\"invert\":" + (o.invert ? "true" : "false") + ",\"enabled\":" + (o.enabled ? "true" : "false") + "}";
        }
        s += "],\"zones\":" + node.multiStatusJson() + ",\"sinks\":" + cachedSinksJson + "}";
        return s;
    };
    cb.getZoneParams = [&](int zone) -> ParamData {
        std::lock_guard<std::mutex> lk(multiMutex);
        return multiState.outputs[std::clamp(zone, 0, multiout::kMaxOutputs - 1)].chain;
    };
    cb.setZoneIr = [&](int zone, IrData&& ir) {
        if (zone < 1 || zone >= multiout::kMaxOutputs) return;
        std::cout << "[Több kimenet] " << (zone + 1) << ". kimenet impulzusválasza: " << (ir.empty() ? "nincs" : ir.name) << "\n";
        if (!linux_store::saveIrFile("zone" + std::to_string(zone + 1) + "_ir.bin", ir))
            std::cerr << "[Több kimenet] Figyelem: a zóna IR-je nem menthető.\n";
        node.zoneChain(zone).submitIr(std::move(ir));
    };
    cb.getZoneIr = [&](int zone) -> IrData {
        return (zone < 1 || zone >= multiout::kMaxOutputs) ? IrData{} : node.zoneChain(zone).irCopy();
    };
    cb.setZoneParams = [&](int zone, const ParamData& p) {
        std::lock_guard<std::mutex> lk(multiMutex);
        multiState.outputs[std::clamp(zone, 1, multiout::kMaxOutputs - 1)].chain = p;
        pushMulti();
    };
    // Haladó hangkimenet: pufferméret, ébren tartás, mintavétel, újramintavételezés (audio.json; megmarad).
    // A PipeWire mintavételi beállítását a motor leálláskor visszaállítja az eredetire.
    std::mutex audioMutex;
    audioopt::AudioOptions audioOpts;
    {
        const std::string doc = linux_store::loadAudioOptions();
        json::Value v;
        if (!doc.empty() && json::parse(doc, v)) audioOpts = audioopt::fromJson(v);
    }
    const std::string origAllowedRates = pwSetting("clock.allowed-rates");
    const std::string origForceRate = pwSetting("clock.force-rate");
    bool ratesTouched = false;
    auto applyAudio = [&](const audioopt::AudioOptions& o) {
        audioopt::resampleQuality().store(o.resampleQuality);
        filePlayer.setReplayGain(o.replayGain, o.rgPreampDb, o.rgPreventClip);
        filePlayer.setPreload(o.preload);
        const int autoQ = std::atoi(pwSetting("clock.quantum").c_str());
        node.setAudioOptions(o.bufferFrames, o.keepAwake, autoQ > 0 ? autoQ : 1024);
        if (o.rateMode == audioopt::RateMode::Follow) {
            pwSetSetting("clock.force-rate", "0");
            pwSetSetting("clock.allowed-rates", "[ 44100 48000 88200 96000 176400 192000 ]");
            ratesTouched = true;
        } else if (o.rateMode == audioopt::RateMode::Fixed) {
            pwSetSetting("clock.force-rate", std::to_string(o.fixedRate));
            ratesTouched = true;
        } else if (ratesTouched) {
            pwSetSetting("clock.force-rate", origForceRate.empty() ? "0" : origForceRate);
            if (!origAllowedRates.empty()) pwSetSetting("clock.allowed-rates", origAllowedRates);
            ratesTouched = false;
        }
    };
    applyAudio(audioOpts);
    cb.audioOptionsJson = [&]() -> std::string {
        std::lock_guard<std::mutex> lk(audioMutex);
        return "{\"opts\":" + audioopt::toJson(audioOpts) +
               ",\"caps\":{\"buffer\":true,\"keepAwake\":true,\"rate\":true,\"resample\":true,\"player\":true}}";
    };
    cb.setAudioOptions = [&](const json::Value& v) {
        std::lock_guard<std::mutex> lk(audioMutex);
        const audioopt::AudioOptions o = audioopt::fromJson(v, audioOpts);
        if (o == audioOpts) return;
        audioOpts = o;
        applyAudio(o);
        if (!linux_store::saveAudioOptions(audioopt::toJson(o))) std::cerr << "[Hangkimenet] Figyelem: az audio.json nem írható.\n";
        std::cout << "[Hangkimenet] " << audioopt::toJson(o) << "\n";
    };
    // Jelút és késleltetés a felület állapotsorába: quantum, a DSP saját késleltetése, terhelés, bitpontosság
    {
        auto base = cb.statusFields;
        cb.statusFields = [&, base]() {
            std::string s = base ? base() : std::string();
            const double sr = std::max<uint32_t>(1, node.sampleRate.load());
            const uint32_t q = node.actualQuantum();
            ParamData p;
            { std::lock_guard<std::mutex> lk(paramMutex); p = current; }
            bool crossover = false;
            { std::lock_guard<std::mutex> lk(multiMutex); crossover = multiState.enabled && multiState.mode == multiout::Mode::Crossover; }
            const size_t eqLat = p.has(kFlagEq) ? node.chain.eqLatencySamples() : 0;
            const size_t limLat = p.has(kFlagLimiter) ? node.chain.limiterLatencySamples() : 0;
            const size_t convLat = p.has(kFlagConvolver) && node.chain.convolverActive() && q ? ee::ZitaInstance::latencyFor(q) : 0;
            const double dspMs = 1000.0 * static_cast<double>(eqLat + limLat + convLat) / sr;
            const double bufMs = 1000.0 * q / sr;
            int reqBuf = 0;
            bool keep = false;
            { std::lock_guard<std::mutex> lk(audioMutex); reqBuf = audioOpts.bufferFrames; keep = audioOpts.keepAwake; }
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                          ",\"path\":{\"quantum\":%u,\"bufReq\":%d,\"bufMs\":%.2f,\"eqLat\":%zu,\"limLat\":%zu,\"convLat\":%zu,"
                          "\"dspMs\":%.2f,\"totalMs\":%.1f,\"load\":%.1f,\"peak\":%.1f,\"overruns\":%u,\"transparent\":%s,"
                          "\"keepAwake\":%s,\"playerRate\":%u,\"playerGain\":%.2f}",
                          q, reqBuf, bufMs, eqLat, limLat, convLat, dspMs, 2.0 * bufMs + dspMs, 100.0 * node.dspLoad(),
                          100.0 * node.takeDspPeak(), node.overruns(), chainTransparent(p) && !crossover ? "true" : "false",
                          keep && node.keepAwakeActive() ? "true" : "false", filePlayer.streamRate(),
                          filePlayer.hasTrack() ? filePlayer.currentGainDb() : 0.0f);
            return s + buf;
        };
    }
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

    if (listenLan) {
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
        // Leálláskor alap: minden modul ki, így a program bezárása után nem változik a hangzás
        std::lock_guard<std::mutex> lk(paramMutex);
        current = transparentParams();
        node.params.write(current);
        linux_store::saveParams(current);
    }
    {
        // A PipeWire mintavételi beállítása visszaáll (a program bezárása után a rendszer a régi)
        audioopt::AudioOptions off = audioOpts;
        off.rateMode = audioopt::RateMode::System;
        applyAudio(off);
    }
    const bool crashedDaemon = node.coreError.load();
    node.shutdown();
    return crashedDaemon ? 2 : 0;
}
