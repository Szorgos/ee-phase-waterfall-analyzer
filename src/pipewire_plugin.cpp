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
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "audio_options.hpp"
#include "file_player.hpp"
#include "health_monitor.hpp"
#include "report_sender.hpp"
#include "library_bridge.hpp"
#include "radio_bridge.hpp"
#include "upnp_bridge.hpp"
#include "linux_headless.hpp"
#include "linux_default_sink.hpp"
#include "linux_store.hpp"
#include "linux_instance_lock.hpp"
#include "live_upsample.hpp"
#include "load_advisor.hpp"
#include "media_mpris.hpp"
#include "pair_cli.hpp"
#if defined(AQ_HAVE_ALSA)
#include "chain_player_dsp.hpp"
#include "player_output_alsa.hpp"
#include "player_output_select.hpp"
#endif
#include "pipewire_sink_node.hpp"
#include "player_output_pw.hpp"
#include "self_path.hpp"
#include "startup_restore.hpp"
#if defined(AQ_HAVE_SYSTEMD)
#include "linux_tray.hpp"
#endif
#include <unistd.h>
#include <spawn.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fstream>

extern char** environ;
#include "update_logic.hpp"
#include "ws_bridge.hpp"
#include "visual_source.hpp"

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
        else if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
            out += buf;
        }
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
        if (pw_reconnect::isOwnAqulifyNode(s.name) || pw_reconnect::isDummyOrNullSink(s.name) || !isSafeSinkName(s.name))
            continue; // a saját virtuális nyelőnk és a dummy/auto_null nem valódi fizikai cél
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

/**
 * A nyelők felhasználónak szóló neve (PipeWire node.description, pl. „Built-in Audio Analog Stereo”) név szerint.
 * Csak megjelenítésre: parancsba soha nem kerül (oda a listPhysicalSinks ellenőrzött neve megy).
 */
std::vector<std::pair<std::string, std::string>> sinkDescriptions() {
    std::vector<std::pair<std::string, std::string>> v;
    const std::string txt = runCapture("pactl list sinks");
    std::string curName;
    size_t pos = 0;
    while (pos < txt.size()) {
        size_t eol = txt.find('\n', pos);
        if (eol == std::string::npos) eol = txt.size();
        std::string line = txt.substr(pos, eol - pos);
        pos = eol + 1;
        const size_t f = line.find_first_not_of(" \t");
        if (f == std::string::npos) continue;
        line = line.substr(f);
        if (line.rfind("Sink #", 0) == 0) curName.clear();
        else if (line.rfind("Name: ", 0) == 0) curName = line.substr(6);
        else if (line.rfind("Description: ", 0) == 0 && !curName.empty()) {
            std::string d = line.substr(13);
            while (!d.empty() && (d.back() == ' ' || d.back() == '\t' || d.back() == '\r')) d.pop_back();
            if (!d.empty() && d.size() <= 200) v.emplace_back(curName, d);
            curName.clear();
        }
    }
    return v;
}

/**
 * A hangkártya fajtája nyelvfüggetlen kulcsként ("speaker", "usb", "hdmi", "bluetooth", "analog" vagy üres);
 * a feliratot a felület állítja össze a választott nyelven (gui_i18n.js: sink.kind.*).
 */
std::string sinkKind(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.find("speaker") != std::string::npos) return "speaker";
    if (lower.find("usb") != std::string::npos) return "usb";
    if (lower.find("hdmi") != std::string::npos) return "hdmi";
    if (lower.find("bluez") != std::string::npos) return "bluetooth";
    if (lower.find("analog") != std::string::npos) return "analog";
    return "";
}

std::string getOutputSinksJson(std::string* outCurrentSink = nullptr) {
    std::string siId, curSinkId;
    findOwnSinkInput(siId, curSinkId);
    std::string currentSinkName;
    std::string json = "[";
    bool first = true;
    const auto descs = sinkDescriptions();
    for (const auto& s : listPhysicalSinks()) {
        const bool active = !curSinkId.empty() && curSinkId == s.id;
        if (active) currentSinkName = s.name;
        std::string desc;
        for (const auto& d : descs)
            if (d.first == s.name) { desc = d.second; break; }
        if (!first) json += ",";
        first = false;
        // kind + desc: a felület ezekből építi a feliratot a választott nyelven; a label nyelvfüggetlen tartalék
        json += "{\"id\":\"" + jsonEscape(s.id) + "\",\"name\":\"" + jsonEscape(s.name) + "\",\"kind\":\"" +
                sinkKind(s.name) + "\",\"desc\":\"" + jsonEscape(desc) + "\",\"label\":\"" +
                jsonEscape(desc.empty() ? s.name : desc) + "\",\"active\":" + (active ? "true" : "false") + "}";
    }
    json += "]";
    if (outCurrentSink) *outCurrentSink = currentSinkName;
    return json;
}

/**
 * A DSP kimenetét a megadott fizikai nyelőre irányítja. Csak létező nyelőnévre (vagy régi
 * "speaker"/"usb"/"hdmi" kulcsszóra) visz át; ismeretlen szövegből soha nem lesz parancs.
 * pin: akkor is átviszi, ha már ott szól – így a stream célja rögzül (target.object), és nem követi az
 * alapértelmezett kimenetet (amikor az Aqulify lesz az alapértelmezett, a WirePlumber máshová kötné).
 */
bool applyOutputSink(const std::string& targetSink, bool remember, bool pin = false) {
    if (targetSink.empty()) return false;

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

    std::string sinkInputId, curSinkId;
    if (!findOwnSinkInput(sinkInputId, curSinkId)) {
        // A lejátszó stream épp hiba / újracsatlakozás alatt állhat: a célt akkor is beállítjuk és elindítjuk az újrakötést
        if (g_node) g_node->setPreferredOutput(resolved);
        if (remember) linux_store::saveOutputSink(resolved);
        return true;
    }

    if (resolvedId == curSinkId) {                    // már ott szól
        // pin: a move-sink-input ugyanarra a nyelőre nem rögzít célt, ezért a metaadatba írjuk (a sink-input
        // száma a PipeWire-csomópont azonosítója; a név ellenőrzött)
        if (pin) {
            const int rc = std::system(("pw-metadata " + sinkInputId + " target.object '" + resolved + "' >/dev/null 2>&1").c_str());
            (void)rc;
        }
        if (g_node) {
            g_node->noteCurrentOutputTarget(resolved);
            g_node->setPreferredOutput(resolved);
        }
        if (remember) linux_store::saveOutputSink(resolved);
        return true;
    }
    const std::string cmd = "LC_ALL=C pactl move-sink-input " + sinkInputId + " '" + resolved + "' >/dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0) return false;
    std::cout << "[PipeWire] Kimeneti eszköz: " << resolved << " (sink-input #" << sinkInputId << ")\n";
    if (g_node) {
        g_node->noteCurrentOutputTarget(resolved);
        g_node->setPreferredOutput(resolved);
    }
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

// ---------------------------------------------------------------- beépített hibajelentés (helyi állapot)

/** A hangkártyák gyári típusneve és formátumai (/proc/asound: cards és az USB-s kártyák stream0 fájlja). */
std::string asoundSummary() {
    auto readCapped = [](const std::string& path, size_t maxBytes) {
        std::ifstream f(path, std::ios::binary);
        std::string s;
        if (!f) return s;
        s.resize(maxBytes);
        f.read(&s[0], static_cast<std::streamsize>(maxBytes));
        s.resize(static_cast<size_t>(f.gcount()));
        return s;
    };
    std::string rawCards = readCapped("/proc/asound/cards", 4096);
    // Az ALSA SOF meghajtók (sof-hda-dsp, sof-soundwire stb.) a 2. sorba (longname) a gép teljes DMI-láncát
    // (gyártó-típus-sorozatszám-alaplap) írják: ezt nem tesszük bele a diagnosztikai állapotba.
    std::string out;
    out.reserve(rawCards.size());
    bool redactNext = false;
    size_t pos = 0;
    while (pos < rawCards.size()) {
        const size_t nl = rawCards.find('\n', pos);
        const size_t end = (nl == std::string::npos) ? rawCards.size() : nl;
        const std::string line = rawCards.substr(pos, end - pos);
        if (redactNext && !line.empty() && (line[0] == ' ' || line[0] == '\t')) {
            out += "                      <dmi>";
            redactNext = false;
        } else {
            out += line;
            redactNext = (line.find("]: sof-") != std::string::npos || line.find("]: SOF") != std::string::npos);
        }
        if (nl != std::string::npos) { out += '\n'; pos = nl + 1; }
        else break;
    }
    if (DIR* d = opendir("/proc/asound")) {
        std::vector<std::string> cards;
        while (const dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.rfind("card", 0) == 0 && n.size() > 4 && std::isdigit(static_cast<unsigned char>(n[4]))) cards.push_back(n);
        }
        closedir(d);
        std::sort(cards.begin(), cards.end());
        for (const auto& c : cards) {
            const std::string s = readCapped("/proc/asound/" + c + "/stream0", 3072);
            if (!s.empty()) out += "\n--- " + c + "/stream0 ---\n" + s;
        }
    }
    return out;
}

/** A rendszer: a kernel, a gép architektúrája, az asztali környezet (a gép neve NEM). */
std::string systemSectionJson() {
    struct utsname u{};
    std::string os, machine;
    if (uname(&u) == 0) { os = std::string(u.sysname) + " " + u.release; machine = u.machine; }
    std::string distro;
    {
        std::ifstream f("/etc/os-release");
        std::string line;
        while (std::getline(f, line))
            if (line.rfind("PRETTY_NAME=", 0) == 0) { distro = line.substr(12); distro.erase(std::remove(distro.begin(), distro.end(), '"'), distro.end()); }
    }
    const char* de = std::getenv("XDG_CURRENT_DESKTOP");
    const char* st = std::getenv("XDG_SESSION_TYPE");
    return "{\"os\":\"" + jsonEscape(os) + "\",\"machine\":\"" + jsonEscape(machine) + "\",\"distro\":\"" + jsonEscape(distro) +
           "\",\"desktop\":\"" + jsonEscape(de ? de : "") + "\",\"session\":\"" + jsonEscape(st ? st : "") + "\"}";
}

} // namespace

#if defined(AQ_HAVE_SYSTEMD)
/** A felület megnyitása a tálcáról: az aqulify-gui a motor mappájából (vagy a PATH-ból), a háttérben indul. */
void openGuiFromTray() {
    std::string gui = "aqulify-gui";
    const std::string self = selfExePath();
    const size_t slash = self.rfind('/');
    if (slash != std::string::npos) {
        const std::string candidate = self.substr(0, slash) + "/aqulify-gui";
        if (access(candidate.c_str(), X_OK) == 0) gui = candidate;
    }
    std::thread([gui] { std::system(("\"" + gui + "\" >/dev/null 2>&1 &").c_str()); }).detach();
}
#endif
int main(int argc, char* argv[]) {
    // Párosítás kijelző nélkül (pl. SSH-n): a már futó motortól kér kódot, és kilép (pair_cli.hpp)
    if (hasArg(argc, argv, "--pair")) {
        std::vector<int> pairPorts(std::begin(kEnginePorts), std::end(kEnginePorts));
        for (int i = 1; i + 1 < argc; ++i)
            if (std::strcmp(argv[i], "--port") == 0) pairPorts = { std::atoi(argv[i + 1]) };
        const bool admin = hasArg(argc, argv, "--admin");
        return paircli::run(pairPorts, "Indítsd el: systemctl --user start aqulify", stdout, admin);
    }
    PwDeinitGuard pwDeinit;   // utoljára szűnik meg: a PipeWire lezárása minden más objektum után
    std::cout << "========================================================\n";
    std::cout << " Aqulify • Fázis & Akusztikai Vízesés – Linux motor (PipeWire)\n";
    std::cout << "========================================================\n";

    if (hasArg(argc, argv, "--help") || hasArg(argc, argv, "-h")) {
        std::cout << "Használat: aqulify-engine [opciók]\n"
                     "  --port N         csak ezt a WebSocket portot próbálja (alapból: 47810-47814, majd 8080)\n"
                     "  --listen lan     hálózati távvezérlés bekapcsolása (47820-as port és mDNS hirdető)\n"
                     "  --headless       fej nélküli (kijelző nélküli) párosítási mód kényszerítése\n"
                     "  --pair           párosító kód a telefonhoz a már futó motortól (kijelző nélkül, pl. SSH-n)\n"
                     "A motor egy \"Aqulify\" nevű kimeneti hangeszközt hoz létre, és alapértelmezett kimenetnek állítja,\n"
                     "így minden hang a DSP láncon át szól. A rendszer hangerő-paneljén választott hangkártya lesz a DSP kimenete.\n";
        return 0;
    }
    // Beépített hibajelentés: a diagnosztikai mappa és a napló tükre (a jelentés utolsó sorai ebből). A beállítás
    // (errorReports, audio.json) az indításkori visszaállítástól független: kikapcsolva csak az előző futás naplója marad.
    const std::string diagDir = linux_store::configDir(true) + "/diag";
    bool errorReportsSaved = false;
    {
        json::Value v;
        const std::string doc = linux_store::loadAudioOptions();
        if (!doc.empty() && json::parse(doc, v)) errorReportsSaved = audioopt::fromJson(v).errorReports;
    }
    aq::health::LogTee::install(diagDir + "/engine.log", !errorReportsSaved);
    aq::health::Monitor health;
    aq::report::Service reportService;
    // Nyelv (a hangbeállításokban látszó kimenet neve): a felületen választott, különben a munkameneté
    aq_i18n::init();

    // Egyetlen-példány zár (flock): még a PipeWire virtuális eszköz (aqulify_sink) létrehozása előtt!
    // Kilépéskor és összeomláskor is automatikusan felszabadul; eltérő XDG_CONFIG_HOME esetén nem ütközik.
    linux_inst::InstanceLock instLock;
    {
        linux_inst::ExistingInfo ex;
        const std::string lockPath = linux_store::configDir(true) + "/engine.lock";
        if (!instLock.acquire(lockPath, selfExePath(), AQULIFY_VERSION, &ex)) {
            std::cerr << "[Info] Az Aqulify motor már fut (PID " << ex.pid
                      << (ex.exe.empty() ? "" : ", " + ex.exe)
                      << (ex.version.empty() ? "" : ", v" + ex.version)
                      << "); ez a példány (" << selfExePath() << ") nem indul el másodszor.\n";
            if (!ex.exe.empty() && ex.exe != selfExePath()) {
                std::cerr << "[Figyelem] Két eltérő Aqulify-telepítés van a gépen. A régi scriptes telepítés eltávolításához:\n"
                          << "  rm -f ~/.config/systemd/user/aqulify.service ~/.local/bin/aqulify-engine ~/.local/bin/aqulify-gui && "
                          << "systemctl --user daemon-reload && systemctl --user restart aqulify.service\n";
            }
            return 0;
        }
    }

    std::vector<int> ports(std::begin(kEnginePorts), std::end(kEnginePorts));
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--port") == 0) ports = { std::atoi(argv[i + 1]) };

    const bool forceHeadless = hasArg(argc, argv, "--headless");
    bool listenLan = hasArg(argc, argv, "--remote");
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--listen") == 0 && i + 1 < argc && std::strcmp(argv[i + 1], "lan") == 0) {
            listenLan = true;
        }
    }

    std::mutex paramMutex;
    // Indításkori visszaállítás (startup_restore.hpp): a mentett részeket az restore.json szerint állítjuk vissza.
    // Kérdezés (mód „ask”) esetén a felület válaszáig az alapállapot marad, a mentett adat a fájlokban van.
    std::mutex restoreMutex;
    startup_restore::Pref restorePref = startup_restore::parsePref(linux_store::loadRestorePref());
    ParamData savedChain = transparentParams();
    const bool hasChain = linux_store::loadParams(savedChain);
    IrData savedIr;
    const bool hasIr = linux_store::loadIr(savedIr);
    ParamData current = transparentParams();   // az élő lánc (alap, amíg nincs visszaállítás)
    bool dirty = false;
    auto lastChange = std::chrono::steady_clock::now();

    AetherSinkNode node(48000);
    g_node = &node;
#if defined(AQ_HAVE_SYSTEMD)
    LinuxTray tray;   // tálcaikon: a motor az ablak nélkül is fut, a menü: Megnyitás / Kilépés
#endif
    node.params.write(current);

    pwDeinit.armed = true;
    if (!node.start(argc, argv)) {
        std::cerr << "[Hiba] A PipeWire nem érhető el. Fut a pipewire (és a wireplumber) szolgáltatás?\n";
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#if defined(AQ_HAVE_SYSTEMD)
    if (tray.start({ std::string(aq_i18n::tr(aq_i18n::Msg::tray_open)), std::string(aq_i18n::tr(aq_i18n::Msg::tray_exit)) }, { openGuiFromTray, [] { if (g_node) g_node->quit(); } }))
        std::cout << "[Tálca] A tálcaikon exportálva; a tálcagazda csatlakozását figyeli.\n";
    else
        std::cout << "[Tálca] Nincs session-busz: a motor tálcaikon nélkül fut.\n";
#endif
    std::cout << "[PipeWire] \"Aqulify\" kimeneti eszköz létrehozva (aqulify_sink).\n";

    // Kezdeti kimenet: csak ha a felhasználó korábban választott. Különben a rendszer (WirePlumber)
    // alapértelmezett eszközén marad – nem kényszerítjük pl. a laptop hangszóróra.
    const std::string initialSink = linux_store::loadOutputSink();
    if (!initialSink.empty()) {
        node.setPreferredOutput(initialSink);
        std::thread([initialSink]() {
            for (int i = 0; i < 25; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (applyOutputSink(initialSink, false)) break;
            }
        }).detach();
    }

    // Diagnosztikai hangerő-gyorsítótár (szálbiztos atomikus mezők: a s_healthTick nem indít popen-t)
    std::atomic<int> cachedVolPct{-1};
    std::atomic<int> cachedVolMuted{-1};   // -1: ismeretlen, 0: nincs némítva, 1: némítva

    // Az „Aqulify” a rendszer alapértelmezett kimenete (linux_default_sink.hpp): induláskor átáll rá, és ha a
    // felhasználó a rendszer hangerő-paneljén hangkártyát választ, az lesz a DSP kimenete, az alapértelmezett pedig
    // visszaáll az Aqulify-ra – így a hang (és a vízesés, az EQ) nem kerüli meg a DSP-láncot.
    std::mutex defMu;
    std::condition_variable defCv;
    bool defQuit = false;
    std::thread defaultThread([&, hasStoredChoice = !initialSink.empty()] {
        bool startup = true, warned = false;
        int volTick = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(defMu);
                defCv.wait_for(lk, std::chrono::milliseconds(startup ? 300 : 1000), [&] { return defQuit; });
                if (defQuit) return;
            }
            std::string cur = runCapture("pactl get-default-sink");
            while (!cur.empty() && (cur.back() == '\n' || cur.back() == '\r' || cur.back() == ' ')) cur.pop_back();
            if (cur == "aqulify_sink") {
                startup = false;
                warned = false;
                // Ha az aqulify_sink maradt az alapértelmezett (pl. crash után), ellenőrizzük, hogy a kimeneti
                // stream valódi fizikai hangkártyára van-e kötve (ne ragadjon error-ban vagy auto_null-on)
                if (!node.externalExclusiveActive_.load(std::memory_order_acquire)) {
                    const int st = node.playbackState.load(std::memory_order_relaxed);
                    const std::string curTarget = node.outputTarget();
                    const bool needTarget = (st == PW_STREAM_STATE_ERROR || st == PW_STREAM_STATE_UNCONNECTED ||
                                             curTarget.empty() || pw_reconnect::isDummyOrNullSink(curTarget));
                    if (needTarget) {
                        std::unordered_set<std::string> physSet;
                        for (const auto& s : listPhysicalSinks()) physSet.insert(s.name);
                        const std::string best = pw_reconnect::resolveTargetOrFallback(linux_store::loadOutputSink(), physSet);
                        if (!best.empty()) {
                            node.setPreferredOutput(best);
                            if (node.playbackStreaming.load(std::memory_order_acquire)) {
                                applyOutputSink(best, false, true);
                            } else {
                                node.requestReconnectIfNeeded();
                            }
                        }
                    } else if (cachedVolPct.load(std::memory_order_relaxed) < 0 || (++volTick % 10 == 0)) {
                        double lv = 0.0;
                        bool mu = false;
                        if (sinkVolume(curTarget, lv, mu)) {
                            cachedVolPct.store(static_cast<int>(std::lround(lv * 100.0)), std::memory_order_relaxed);
                            cachedVolMuted.store(mu ? 1 : 0, std::memory_order_relaxed);
                        }
                    }
                }
                continue;
            }
            if (runCapture("pactl list short sinks").find("\taqulify_sink\t") == std::string::npos) continue;   // még nem látszik
            std::vector<std::string> others;
            for (const auto& s : listPhysicalSinks()) others.push_back(s.name);
            const defsink::Action a = defsink::decide(cur, others, startup, hasStoredChoice);
            // A DSP kimenete rögzített célt kap (a választott vagy a megjegyzett hangkártya). Előbb az alapértelmezett
            // áll át: a WirePlumber ekkor a „követő” streameket újrakötné, a saját kimenetünket máshová is
            const std::string pinTarget = !a.dspTarget.empty() ? a.dspTarget : a.setDefault ? linux_store::loadOutputSink() : "";
            if (!pinTarget.empty()) node.setPreferredOutput(pinTarget);
            if (a.setDefault) {
                if (std::system("LC_ALL=C pactl set-default-sink aqulify_sink >/dev/null 2>&1") == 0) {
                    std::cout << "[PipeWire] Alapértelmezett kimenet: Aqulify (előtte: " << cur << ")\n";
                    warned = false;
                } else if (!warned) {
                    std::cerr << "[PipeWire] Az Aqulify nem állítható be alapértelmezett kimenetnek (pactl set-default-sink).\n";
                    warned = true;
                }
            }
            if (!pinTarget.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));   // a WirePlumber újrakötése után
                for (int i = 0; i < 10 && !applyOutputSink(pinTarget, a.remember, true); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            startup = false;
        }
    });
    struct DefaultStopper {
        std::function<void()> f;
        ~DefaultStopper() { if (f) f(); }
    } defaultStopper{ [&] {
        { std::lock_guard<std::mutex> lk(defMu); defQuit = true; }
        defCv.notify_all();
        if (defaultThread.joinable()) defaultThread.join();
    } };

    uint64_t readPos = 0;
#if defined(AQ_HAVE_ALSA)
    // A beépített lejátszó közvetlen ALSA-kimenete (kizárólagos mód): ilyenkor a DSP a lejátszó saját láncában fut,
    // és a mérők / a vízesés ebből kapják a jelet (a PipeWire „Aqulify” eszközén nem szól semmi)
    std::unique_ptr<player::ChainPlayerDsp> playerDsp;
    player::SelectablePlayerOutput* playerSel = nullptr;
    player::AlsaPlayerOutput* alsaRaw = nullptr;
    player::FilePlayer* playerFilePlayer = nullptr;
    uint64_t playerReadPos = 0;
    auto playerExclusive = [&]() { return playerSel && playerSel->exclusiveActive(); };
#else
    auto playerExclusive = []() { return false; };
#endif
    BridgeCallbacks cb;
    cb.pullAudio = [&](float* lr, size_t maxFrames) {
#if defined(AQ_TEST_HOOKS)
        // Csak tesztfordításban (AQ_TEST_HOOKS): AQ_TEST_STALL_PULL_AFTER=<s> után a híd szála itt beragad (élő próba)
        static const double stallAfter = [] { const char* e = std::getenv("AQ_TEST_STALL_PULL_AFTER"); return e ? std::atof(e) : -1.0; }();
        static const auto t0 = std::chrono::steady_clock::now();
        if (stallAfter >= 0.0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > stallAfter) {
            std::cout << "[Teszt] A híd szála szándékosan beragad (AQ_TEST_STALL_PULL_AFTER).\n";
            for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
        }
#endif
        auto readEngine = [&](float* d, size_t n) { return readTapRing(node.visualizerTap, readPos, d, n); };
#if defined(AQ_HAVE_ALSA)
        const bool dsdDirect = playerFilePlayer && playerFilePlayer->isDopActive();
        const bool excl = !dsdDirect && playerExclusive();
        return pullVisualAudio(dsdDirect, excl, lr, maxFrames,
                               [&](float* d, size_t n) { return readTapRing(playerDsp->tap(), playerReadPos, d, n); }, readEngine);
#else
        return pullVisualAudio(false, false, lr, maxFrames, [](float*, size_t) -> size_t { return 0; }, readEngine);
#endif
    };
    cb.sampleRate = [&]() {
#if defined(AQ_HAVE_ALSA)
        if (playerFilePlayer && playerFilePlayer->isDopActive()) return static_cast<double>(playerFilePlayer->streamRate());
        if (playerExclusive()) return static_cast<double>(playerDsp->sampleRate());
#endif
        return static_cast<double>(node.sampleRate.load());
    };
    cb.getParams = [&]() { std::lock_guard<std::mutex> lk(paramMutex); return current; };
    cb.setParams = [&](const ParamData& p) {
        {
            std::lock_guard<std::mutex> lk(paramMutex);
            current = p;
            dirty = true;
            lastChange = std::chrono::steady_clock::now();
        }
        node.params.write(p);
#if defined(AQ_HAVE_ALSA)
        if (playerDsp) playerDsp->setParams(p);
#endif
    };
    cb.gainReduction = [&]() {
#if defined(AQ_HAVE_ALSA)
        if (playerFilePlayer && playerFilePlayer->isDopActive()) return 0.0;
        if (playerExclusive()) return playerDsp->gainReductionDb();
#endif
        return node.chain.gainReductionDb();
    };
    cb.bassHarmonics = [&]() {
#if defined(AQ_HAVE_ALSA)
        if (playerFilePlayer && playerFilePlayer->isDopActive()) return -100.0;
#endif
        return node.chain.bassHarmonicsDb();
    };

    std::string cachedSinksJson = "[]";
    std::string cachedCurrentSink = "";
    auto lastSinkQuery = std::chrono::steady_clock::time_point{};

    std::string cachedVolumeJson;   // "level/muted" a kimeneti hangkártyáé (üres: nem ismert)
    auto updateSinksCache = [&]() {
        cachedSinksJson = getOutputSinksJson(&cachedCurrentSink);
        double lv = 0.0;
        bool mu = false;
        if (sinkVolume(cachedCurrentSink, lv, mu)) {
            cachedVolPct.store(static_cast<int>(std::lround(lv * 100.0)), std::memory_order_relaxed);
            cachedVolMuted.store(mu ? 1 : 0, std::memory_order_relaxed);
            cachedVolumeJson = ",\"volume\":{\"level\":" + std::to_string(lv) + ",\"muted\":" + (mu ? "true" : "false") + "}";
        } else {
            cachedVolPct.store(-1, std::memory_order_relaxed);
            cachedVolMuted.store(-1, std::memory_order_relaxed);
            cachedVolumeJson.clear();
        }
        lastSinkQuery = std::chrono::steady_clock::now();
    };

    cb.setOutputSink = [&](const std::string& target) {
        if (!applyOutputSink(target, true))
            std::cerr << "[PipeWire] A kért kimenet nem található vagy nem állítható be.\n";
        updateSinksCache();
    };

    // A telepítés módja (csomag, COPR, install.sh): a felület ebből tudja, milyen paranccsal frissíthető a program
    const char* installKind = update::linuxInstallName(update::detectLinuxInstall(selfExePath(), update::realFs()));
    std::cout << "[Frissítés] A telepítés módja: " << installKind << "\n";
    const char* homeEnv = std::getenv("HOME");
    const char* xdgCfg = std::getenv("XDG_CONFIG_HOME");
    const std::string homeStr = homeEnv ? homeEnv : "";
    const std::string cfgBase = (xdgCfg && *xdgCfg) ? std::string(xdgCfg) : (homeStr.empty() ? std::string() : (homeStr + "/.config"));
    const std::string selfExe = selfExePath();
    const linux_inst::DualInstallInfo dualInstall = linux_inst::detectDualInstall(installKind, selfExe, homeStr, cfgBase);
    if (dualInstall.detected) {
        std::cout << "[Figyelem] Két Aqulify-telepítés van a gépen (fut: " << dualInstall.runningKind
                  << " [" << dualInstall.runningPath << "], másik: " << dualInstall.otherKind
                  << " [" << dualInstall.otherPath << "]). A régi scriptes telepítés eltávolítása:\n  "
                  << dualInstall.removeCmd << "\n";
    }
    std::atomic<bool> defaultPortBusy{false};
    const std::string installJsonBase = std::string(",\"install\":\"") + installKind + "\"";
    cb.statusFields = [&]() {
        auto now = std::chrono::steady_clock::now();
        if (now - lastSinkQuery > std::chrono::seconds(2)) {
            updateSinksCache();
        }
        char buf[256];
#if defined(AQ_HAVE_ALSA)
        if (playerFilePlayer && playerFilePlayer->isDopActive()) {
            std::snprintf(buf, sizeof(buf),
                          "\"platform\":\"linux\",\"source\":\"ALSA exclusive (DoP)\",\"dspActive\":false,\"sr\":%u,\"gr\":0.00",
                          playerFilePlayer->streamRate());
        } else if (playerExclusive()) {
            std::snprintf(buf, sizeof(buf),
                          "\"platform\":\"linux\",\"source\":\"ALSA (kizárólagos)\",\"dspActive\":true,\"sr\":%u,\"gr\":%.2f",
                          playerDsp->sampleRate(), playerDsp->gainReductionDb());
        } else
#else
        if (false) {} else
#endif
        {
            std::snprintf(buf, sizeof(buf),
                          "\"platform\":\"linux\",\"source\":\"PipeWire – Aqulify\",\"dspActive\":%s,\"sr\":%u,\"gr\":%.2f",
                          node.playbackStreaming.load() ? "true" : "false", node.sampleRate.load(),
                          node.chain.gainReductionDb());
        }
        std::string s = buf;
        s += ",\"currentSink\":\"" + jsonEscape(cachedCurrentSink) + "\"";
        s += ",\"outputSinks\":" + cachedSinksJson;
#if defined(AQ_HAVE_ALSA)
        if (playerFilePlayer && playerFilePlayer->isDopActive())
            s += ",\"volume\":{\"level\":1.0,\"muted\":false,\"digital\":false,\"dop\":true}";
        else if (playerExclusive())   // kizárólagos módban a lejátszó digitális hangereje (a PipeWire-eszközé itt nem hat)
            s += ",\"volume\":{\"level\":" + std::to_string(playerDsp->volumeLevel()) + ",\"muted\":" +
                 (playerDsp->volumeMuted() ? "true" : "false") + ",\"digital\":true}";
        else
#endif
            s += cachedVolumeJson;
        s += installJsonBase;
        if (dualInstall.detected) {
            s += dualInstall.statusJsonField();
        } else if (defaultPortBusy.load(std::memory_order_relaxed) || node.hasDuplicateAqulifySink()) {
            s += linux_inst::detectOtherRunningEngine(defaultPortBusy.load(std::memory_order_relaxed),
                                                      node.hasDuplicateAqulifySink(), installKind, selfExe).statusJsonField();
        }
#if defined(AQ_HAVE_SYSTEMD)
        s += std::string(",\"tray\":") + (tray.registered() ? "true" : "false");
#endif
        return s;
    };
    cb.setVolume = [&](double level, int mute) {
#if defined(AQ_HAVE_ALSA)
        if (playerExclusive()) { playerDsp->setVolume(level, mute); return; }
#endif
        setSinkVolume(cachedCurrentSink, level, mute);
        lastSinkQuery = std::chrono::steady_clock::time_point{};   // a következő állapotüzenet már az új értéket mutatja
    };
    std::atomic<bool> claimVolumeActive{ false };
    cb.getVolume = [&](double& level, int& muted) -> bool {
        bool mu = false;
        if (sinkVolume(cachedCurrentSink, level, mu)) {
            muted = mu ? 1 : 0;
            return true;
        }
        return false;
    };
    cb.setIr = [&](IrData&& ir) {
        if (!linux_store::saveIr(ir)) std::cerr << "[Konvolver] Figyelem: az IR nem menthető.\n";
#if defined(AQ_HAVE_ALSA)
        if (playerDsp) playerDsp->submitIr(IrData(ir));
#endif
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
    player::PwPlayerOutput* pwPlayerRaw = nullptr;
#if defined(AQ_HAVE_ALSA)
    // Kizárólagos módban (beállítás: exclusive) közvetlenül az ALSA-eszközre, a lejátszó saját DSP-láncán át
    {
        std::lock_guard<std::mutex> lk(paramMutex);
        playerDsp = std::make_unique<player::ChainPlayerDsp>(current);
    }
    auto alsaOut = std::make_unique<player::AlsaPlayerOutput>();
    alsaOut->setExclusiveDsp(playerDsp.get());
    alsaRaw = alsaOut.get();   // a jelút állapotához (natív DSD és formátuma)
    auto pwOut = std::make_unique<player::PwPlayerOutput>("aqulify_sink");
    pwPlayerRaw = pwOut.get();
    auto selOut = std::make_unique<player::SelectablePlayerOutput>(std::move(pwOut), std::move(alsaOut));
    selOut->setExclusiveChangedCallback([&node](bool active) {
        node.setExternalExclusiveActive(active);
    });
    selOut->setBeforeFallbackToSharedHook([&node]() {
        // Működő közvetlen ALSA lezárása után a WirePlumbernek idő kell a DAC visszavételéhez:
        // kérjük az újracsatlakozást, és legfeljebb ~1,2 s-ig megvárjuk, amíg a fizikai kimenet újra kész.
        node.requestReconnectIfNeeded();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
        while (std::chrono::steady_clock::now() < deadline && !node.hasReadyPhysicalOutput()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
    });
    playerSel = selOut.get();
    player::FilePlayer filePlayer(std::move(selOut));
    playerFilePlayer = &filePlayer;
#else
    auto pwOut = std::make_unique<player::PwPlayerOutput>("aqulify_sink");
    pwPlayerRaw = pwOut.get();
    player::FilePlayer filePlayer(std::move(pwOut));
#endif
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
        else mediaSession.command(action == "stop" ? "pause" : action, value);   // más lejátszót csak szüneteltetünk
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
    cb.playerOpenBypass = [&](const std::string& path, std::string& error) {
        if (!filePlayer.open(path, &error, 0, /*bypassUpsample=*/true)) {
            std::cerr << "[Lejátszó/Mérés] " << path << ": " << error << "\n";
            return false;
        }
        std::cout << "[Lejátszó/Mérés] Mérőjel (felmintavételezés megkerülve): " << path << "\n";
        return true;
    };
    cb.playerStop = [&] { filePlayer.stop(); };
    cb.speakClaimCode = [&](const std::string& code, const std::string& lang) -> bool {
        // RAII hangerő-őrző: hiba, megszakítás vagy kivétel esetén is visszaállítja az eredeti hangerőt
        claim_audio::ClaimVolumeGuard volGuard(cb.getVolume, cb.setVolume, &claimVolumeActive);
        const std::string tmpWav = "/tmp/aq-claim-" + bridge_detail::randomHex(8) + ".wav";
        struct TmpCleanup {
            std::string p;
            ~TmpCleanup() { if (!p.empty()) std::remove(p.c_str()); }
        } cleanup{ tmpWav };

        const std::string exePath = selfExePath();
        const std::string exeDir = exePath.empty() ? std::string() : std::filesystem::path(exePath).parent_path().string();
        const std::vector<std::string> roots = claim_audio::defaultAudioSearchRoots(exeDir);
        const int durMs = claim_audio::buildCodeWav(code, lang, roots, tmpWav);
        if (durMs <= 0) {
            std::cerr << "[Headless] A számjegy-hangminták nem találhatók.\n";
            return false;
        }
        std::string err;
        if (!filePlayer.open(tmpWav, &err, 0, /*bypassUpsample=*/true)) {
            std::cerr << "[Headless] A beállítási kód hangja nem játszható le: " << err << "\n";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(durMs + 300));
        filePlayer.stop();
        return true;
    };
    // Szobamérés: mikrofonfelvétel PipeWire-rel (room/mic_capture_pipewire.hpp)
    cb.makeMicCapture = [] { return aqulify::room::createMicCapture(); };
    // Média-hivatkozás (URL) megnyitása: csak grafikus munkamenetben (DISPLAY / WAYLAND_DISPLAY)
    const char* disp = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    if ((disp && *disp) || (wayland && *wayland)) {
        cb.openUrl = [](const std::string& u) -> bool {
            pid_t pid = -1;
            char* const argv[] = {
                const_cast<char*>("xdg-open"),
                const_cast<char*>(u.c_str()),
                nullptr
            };
            if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, argv, environ) != 0) {
                return false;
            }
            std::thread([pid]() {
                waitpid(pid, nullptr, 0);
            }).detach();
            return true;
        };
        // Van-e alkalmazás a protokollhoz (pl. "tidal" → a SONE vagy más TIDAL-kliens, "spotify" → Spotify): az
        // xdg-mime alapértelmezett kezelője (a Flatpak- és Snap-alkalmazások exportált .desktop fájljaival is)
        cb.hasUrlHandler = [](const std::string& scheme) -> bool {
            // A protokollnév a parancssorba kerül: csak kisbetű, legfeljebb 20 karakter
            if (scheme.empty() || scheme.size() > 20) return false;
            for (char ch : scheme)
                if (ch < 'a' || ch > 'z') return false;
            const std::string cmd = "xdg-mime query default x-scheme-handler/" + scheme + " 2>/dev/null";
            FILE* f = popen(cmd.c_str(), "r");
            if (!f) return false;
            char buf[256] = {};
            const bool got = std::fgets(buf, sizeof(buf), f) != nullptr;
            pclose(f);
            return got && std::strstr(buf, ".desktop") != nullptr;
        };
    }
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
    // Internetrádió: a felhasználó állomáslistája (radio.json); a lejátszás a beépített lejátszón, a DSP-láncon át
    radio::StationStore radioStations([](const std::string& doc) {
        if (!linux_store::saveRadio(doc)) std::cerr << "[Rádió] Figyelem: a radio.json nem írható.\n";
    });
    radioStations.load(linux_store::loadRadio());
    radio::RadioController radioCtl(filePlayer, radioStations);
    radio::attachRadio(cb, radioCtl, radioStations);
    cb.loadRemote = [] { return linux_store::loadRemote(); };
    cb.saveRemote = [](const std::string& doc) { return linux_store::saveRemote(doc); };
    cb.loadTargets = [] { return linux_store::loadRemoteTargets(); };
    cb.saveTargets = [](const std::string& doc) { return linux_store::saveRemoteTargets(doc); };
    // Ablak bezárása „Bezárás” választással: rendes leállás (0-s kilépési kód, a systemd nem indítja újra)
    cb.quitEngine = [] { if (g_node) g_node->quit(); };

    // Több kimenet (2–4 hangkártya). Induláskor ki van kapcsolva (alap állapot: csak a fő kimenet szól, a hangzás
    // nem változik); a felület a csatlakozáskor adja át. A 2–4. kimenet céleszköze csak létező fizikai nyelő lehet.
    // A beállítás és a zónák lánca / IR-je megmarad (multiout.bin, zoneN_ir.bin), de induláskor ki van kapcsolva.
    std::mutex multiMutex;
    multiout::MultiConfig multiState;             // az élő állapot (alap, amíg nincs visszaállítás)
    std::array<std::string, 3> multiTargets{};
    // A mentett több kimenet és a zónák IR-je: a visszaállítás (multi rész) élesíti
    multiout::MultiConfig savedMulti;
    std::array<std::string, 3> savedTargets{};
    const bool hasMulti = linux_store::loadMultiOut(savedMulti, savedTargets);
    std::array<IrData, multiout::kMaxOutputs> savedZoneIr{};
    if (hasMulti) {
        for (int k = 1; k < multiout::kMaxOutputs; ++k)
            linux_store::loadIrFile("zone" + std::to_string(k + 1) + "_ir.bin", savedZoneIr[k]);
    }
    static std::atomic<bool> s_multiDirty{ false };
    auto pushMulti = [&] { node.setMultiOutput(multiState, multiTargets); s_multiDirty = true; };
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
        linux_store::saveMultiOut(multiState, multiTargets);
        s_multiDirty = false;
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
                 ",\"invert\":" + (o.invert ? "true" : "false") + ",\"enabled\":" + (o.enabled ? "true" : "false") +
                 ",\"way\":" + multiout::wayEqToJson(o.way) + "}";
        }
        const double curSr = std::max(1.0, static_cast<double>(node.sampleRate.load()));
        s += "],\"zones\":" + node.multiStatusJson() + ",\"response\":" + multiout::multiResponseJson(c, curSr) + ",\"sinks\":" + cachedSinksJson + "}";
        return s;
    };
    cb.getMultiOut = [&]() -> multiout::MultiConfig {
        std::lock_guard<std::mutex> lk(multiMutex);
        return multiState;
    };
    cb.setWayEq = [&](int output, const multiout::WayEq& w) {
        std::lock_guard<std::mutex> lk(multiMutex);
        if (output < 0 || output >= multiState.numOutputs || output >= multiout::kMaxOutputs) return;
        multiState.outputs[output].way = w;
        multiout::sanitize(multiState);
        pushMulti();
        linux_store::saveMultiOut(multiState, multiTargets);
        s_multiDirty = false;
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
    audioopt::AudioOptions audioOpts;             // az élő beállítás (alap, amíg nincs visszaállítás)
    audioopt::AudioOptions savedAudio;            // a mentett (audio.json), a visszaállítás (audio rész) élesíti
    bool dsdDop = false;
    bool savedDsdDop = false;
    const bool hasAudio = [&] {
        const std::string doc = linux_store::loadAudioOptions();
        json::Value v;
        if (doc.empty() || !json::parse(doc, v)) return false;
        savedAudio = audioopt::fromJson(v);
        if (const json::Value* dopVal = v.get("dsdDop"); dopVal && dopVal->type == json::Value::Type::Bool) {
            savedDsdDop = dopVal->b;
        }
        return true;
    }();
    audioOpts.errorReports = savedAudio.errorReports;           // hozzájárulás: a visszaállítás módjától függetlenül él
    audioOpts.errorReportsAsked = savedAudio.errorReportsAsked; // első indításkori kérdés állapota
    audioOpts.netRecv = savedAudio.netRecv;                     // hálózati vevő: szolgáltatás, mindig visszaáll
    audioOpts.netRecvName = savedAudio.netRecvName;
    // Hálózati vevő (net_receiver.hpp): a rendszer shairport-sync programja gyermekfolyamatként, a hangja az
    // aqulify_sink eszközre megy (az élő út EQ-zza). Alapból ki; a beállítás az audio.json része.
    aq::netrecv::Receiver::Config netRecvCfg;
    netRecvCfg.configPath = linux_store::configDir(true) + "/netrecv.conf";
    aq::netrecv::Receiver netReceiver(netRecvCfg);
    netReceiver.setLineHandler([](const std::string& line) { std::cout << "[Hálózati vevő] " << line << std::endl; });
    // UPnP / DLNA lejátszó (upnp_bridge.hpp): a telefonos vezérlők (BubbleUPnP, mconnect, …) a beépített lejátszón
    // át, a teljes láncon (bitpontos / hi-res, DSD). Alapból ki; a beállítás az audio.json része, mindig visszaáll.
    audioOpts.upnp = savedAudio.upnp;
    audioOpts.upnpName = savedAudio.upnpName;
    audioOpts.upnpFixedVolume = savedAudio.upnpFixedVolume;
    audioOpts.upnpAllow = savedAudio.upnpAllow;
    upnp::UpnpService upnpService(filePlayer, playQueue, cb.setVolume, cb.getVolume);
    cb.mediaJson = [&]() {
        if (!filePlayer.hasTrack()) return mediaSession.json();
        media::Info m = playQueue.mediaInfo();
        upnpService.patchInfo(m);
        return media::toJson(m);
    };
    auto optsToJsonWithDop = [](const audioopt::AudioOptions& o, bool dop) {
        std::string j = audioopt::toJson(o);
        if (!j.empty() && j.back() == '}') {
            j.pop_back();
            j += std::string(",\"dsdDop\":") + (dop ? "true}" : "false}");
        }
        return j;
    };
    const std::string origAllowedRates = pwSetting("clock.allowed-rates");
    const std::string origForceRate = pwSetting("clock.force-rate");
    int cachedAutoQuantum = [] {
        const int q = std::atoi(pwSetting("clock.quantum").c_str());
        return q > 0 ? q : 1024;
    }();
    int lastAppliedBufFrames = -1;
    bool ratesTouched = false;
    std::atomic<bool> playerStreamOpenFlag{false};
    auto playerStreamOpen = [&]() { return playerStreamOpenFlag.load(); };
    // Felmintavételezés az élő úton (más programból, pl. MPD-ből az Aqulify eszközre szóló hang; live_upsample.hpp):
    // a gráf mintavétele a célra emelkedik (clock.force-rate), a forrást a PipeWire saját átalakítója hozza fel.
    std::mutex liveMu;
    std::condition_variable liveCv;
    bool liveWake = false, liveQuit = false;          // liveMu alatt
    uint32_t liveForced = 0;                          // az általunk kért force-rate (0: nincs); liveMu alatt
    uint32_t liveOwnTarget = 0;                       // saját motoros rendszerhang-felmintavételezés célja (0: ki); liveMu alatt
    liveups::Decision liveState;                      // a felületnek; liveMu alatt
    bool liveDacKnown = false;                        // liveMu alatt
    // A kényszer feloldása: forrás-követésnél 0, rendszer-mintavételnél az eredeti érték. liveMu alatt hívandó.
    auto liveReleaseLocked = [&](audioopt::RateMode rm) {
        if (liveForced == 0) return;
        pwSetSetting("clock.force-rate", rm == audioopt::RateMode::System && !origForceRate.empty() ? origForceRate : "0");
        std::cout << "[Felmintavételezés] Élő út: a gráf mintavétele újra szabad.\n";
        liveForced = 0;
    };
    auto applyAudio = [&](const audioopt::AudioOptions& o) {
        audioopt::resampleQuality().store(o.resampleQuality);
        netReceiver.requestApply(o.netRecv, o.netRecvName);
        upnpService.apply(o.upnp, o.upnpName, o.upnpFixedVolume, o.upnpAllow);
        filePlayer.setReplayGain(o.replayGain, o.rgPreampDb, o.rgPreventClip);
        filePlayer.setPreload(o.preload);
        filePlayer.setUpsampleOptions(o);
#if defined(AQ_HAVE_ALSA)
        filePlayer.setDsdDop(o.exclusive && dsdDop);
        if (playerSel && playerSel->wantExclusive() != o.exclusive) {
            playerSel->setExclusive(o.exclusive);   // a következő számtól
            std::cout << "[Lejátszó] Közvetlen ALSA-kimenet (kizárólagos mód): " << (o.exclusive ? "be" : "ki")
                      << " (a következő számtól).\n";
        }
#else
        filePlayer.setDsdDop(false);
#endif
        // Csak pufferméret-váltáskor kérdezzük le frissen a clock.quantum-ot (csúszkahúzáskor ne indítson popen-t!)
        if (o.bufferFrames != lastAppliedBufFrames) {
            if (o.bufferFrames == 0 && lastAppliedBufFrames > 0) {
                const int q = std::atoi(pwSetting("clock.quantum").c_str());
                if (q > 0) cachedAutoQuantum = q;
            }
            lastAppliedBufFrames = o.bufferFrames;
        }
        const bool pauseForPlayer = o.rateMode == audioopt::RateMode::Follow && playerStreamOpen();
        node.setAudioOptions(o.bufferFrames, o.keepAwake && !pauseForPlayer, cachedAutoQuantum);
        node.setRateFollow(o.rateMode == audioopt::RateMode::Follow);
        {
            // Az alábbi ágak a force-rate-et felülírják (forrás-követés, fix, vagy visszaállítás): az élő út kényszere
            // így megszűnik; a figyelő szál az új beállítás szerint azonnal újra dönt
            std::lock_guard<std::mutex> lk(liveMu);
            if (o.rateMode != audioopt::RateMode::System || ratesTouched) liveForced = 0;
            liveWake = true;
        }
        liveCv.notify_all();
        if (o.rateMode == audioopt::RateMode::Follow) {
            pwSetSetting("clock.force-rate", "0");
            // A forrás (vagy a felmintavételezés célja) szerint vált a gráf; a hangkártya által nem tudott értéket a
            // PipeWire nem választja. 384 kHz fölött a DSP-lánc bitpontosan átenged (AetherChain).
            pwSetSetting("clock.allowed-rates", "[ 44100 48000 88200 96000 176400 192000 352800 384000 705600 768000 ]");
            ratesTouched = true;
        } else if (o.rateMode == audioopt::RateMode::Fixed) {
            pwSetSetting("clock.force-rate", std::to_string(o.fixedRate));
            ratesTouched = true;
        } else if (ratesTouched) {
            pwSetSetting("clock.force-rate", origForceRate.empty() ? "0" : origForceRate);
            if (!origAllowedRates.empty()) pwSetSetting("clock.allowed-rates", origAllowedRates);
            else runCapture("pw-metadata -n settings 0 -d clock.allowed-rates");
            ratesTouched = false;
        }
    };
    applyAudio(audioOpts);

    // Indításkori visszaállítás: a mentett részek alkalmazása (mód „always”: azonnal; „ask”: a felület kérdezi)
    auto applyRestoreParts = [&](const startup_restore::Parts& want) {
        if (want.chain) {
            if (hasChain) {
                std::lock_guard<std::mutex> lk(paramMutex);
                current = savedChain;
                node.params.write(current);
#if defined(AQ_HAVE_ALSA)
                if (playerDsp) playerDsp->setParams(current);
#endif
            }
            if (hasIr) {
                std::cout << "[Konvolver] Mentett impulzusválasz visszaállítva: " << savedIr.name << "\n";
                node.chain.submitIr(IrData(savedIr));
#if defined(AQ_HAVE_ALSA)
                if (playerDsp) playerDsp->submitIr(IrData(savedIr));
#endif
            }
        }
        if (want.multi && hasMulti) {
            std::lock_guard<std::mutex> lk(multiMutex);
            multiState = savedMulti;
            multiTargets = savedTargets;
            for (int k = 1; k < multiout::kMaxOutputs; ++k)
                if (!savedZoneIr[k].empty()) node.zoneChain(k).submitIr(IrData(savedZoneIr[k]));
            pushMulti();
        }
        if (want.audio && hasAudio) {
            std::lock_guard<std::mutex> lk(audioMutex);
            audioOpts = savedAudio;
            dsdDop = savedDsdDop;
            applyAudio(audioOpts);
        }
    };
    if (restorePref.mode == startup_restore::Mode::Always) applyRestoreParts(restorePref.parts);
    std::atomic<bool> restorePending{ restorePref.mode == startup_restore::Mode::Ask &&
                                      (hasChain || hasIr || hasMulti || hasAudio) };
    if (restorePending) std::cout << "[Beállítások] Indításkor a felület megkérdezi, mit állítson vissza.\n";
    cb.restoreStatus = [&]() {
        std::lock_guard<std::mutex> lk(restoreMutex);
        return startup_restore::statusMessage(restorePending.load(), restorePref, hasChain || hasIr, hasMulti, hasAudio);
    };
    cb.restoreRequest = [&](const std::string& type, const json::Value& msg) {
        std::lock_guard<std::mutex> lk(restoreMutex);
        if (type == "restore_choice") {
            // choice: "restore" (a kijelölt részek) vagy "default" (semmi); remember: legközelebb ne kérdezzen
            const bool restore = msg.get("choice") && msg.get("choice")->stringOr("") == "restore";
            const startup_restore::Parts want = restore
                ? startup_restore::partsFromJson(msg.get("parts"), startup_restore::Parts{})
                : startup_restore::Parts{ false, false, false };
            if (restorePending.exchange(false)) applyRestoreParts(want);
            const json::Value* rem = msg.get("remember");
            if (rem && rem->type == json::Value::Type::Bool && rem->b) {
                restorePref.parts = want;
                restorePref.mode = startup_restore::anyPart(want) ? startup_restore::Mode::Always : startup_restore::Mode::Never;
                if (!linux_store::saveRestorePref(startup_restore::serializePref(restorePref)))
                    std::cerr << "[Beállítások] Figyelem: a restore.json nem írható.\n";
            }
        } else if (type == "restore_mode") {
            // a Kimenet menü beállítása: mode („ask” / „always” / „never”) és a részek
            if (const json::Value* m = msg.get("mode"); m && m->type == json::Value::Type::String)
                restorePref.mode = startup_restore::modeFromName(m->str);
            restorePref.parts = startup_restore::partsFromJson(msg.get("parts"), restorePref.parts);
            if (!linux_store::saveRestorePref(startup_restore::serializePref(restorePref)))
                std::cerr << "[Beállítások] Figyelem: a restore.json nem írható.\n";
        }
    };

    // „A forrást követi” + ébren tartás: a folyamatosan szóló csendes hangfolyam mellett a PipeWire nem vált
    // mintavételt. Amíg a beépített lejátszó hangfolyama nyitva van, a hangkártya úgyis szól, ezért az ébren tartás
    // addig szünetel; a hangfolyam bezárása után 1,5 s-mal visszakapcsol (ha közben új szám nyílt, nem). Külső
    // program új hangfolyamakor az ébren tartás szintén rövid időre szünetel (AetherSinkNode::onRegistryGlobal).
    static std::atomic<uint64_t> s_playerTicket{0};
    static std::atomic<bool> s_engineQuit{false};
    std::atomic<bool>& s_playerOpen = playerStreamOpenFlag;
    auto parseAllowedRates = [](const std::string& raw) -> std::vector<uint32_t> {
        std::vector<uint32_t> out;
        uint32_t cur = 0;
        bool inNum = false;
        for (char ch : raw) {
            if (ch >= '0' && ch <= '9') {
                cur = cur * 10u + static_cast<uint32_t>(ch - '0');
                inNum = true;
            } else if (inNum) {
                if (cur >= 8000 && cur <= 768000) out.push_back(cur);
                cur = 0;
                inNum = false;
            }
        }
        if (inNum && cur >= 8000 && cur <= 768000) out.push_back(cur);
        return out;
    };
    auto detectCurrentDacRates = [&]() -> std::vector<uint32_t> {
        std::string siId, outSinkId;
        findOwnSinkInput(siId, outSinkId);
        if (outSinkId.empty()) {
            const std::string t = node.outputTarget();
            if (!t.empty()) outSinkId = liveups::sinkIdByName(runCapture("pactl list short sinks"), t);
        }
        if (outSinkId.empty()) return {};
        const int card = liveups::alsaCardOfSink(runCapture("pactl list sinks"), outSinkId);
        if (card < 0) return {};
        std::ifstream f("/proc/asound/card" + std::to_string(card) + "/stream0");
        if (!f) return {};
        const std::string txt((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return liveups::parseStream0PlaybackRates(txt);
    };
    player::pwAllowedRatesHook() = [&, parseAllowedRates, detectCurrentDacRates]() -> std::vector<uint32_t> {
        audioopt::RateMode rm = audioopt::RateMode::System;
        uint32_t fixedRate = 0;
        { std::lock_guard<std::mutex> lk(audioMutex); rm = audioOpts.rateMode; fixedRate = static_cast<uint32_t>(std::max(0, audioOpts.fixedRate)); }
        // Rögzített mintavétel: a gráf csak ezen szól, a lejátszó ide alakít (fel- vagy lefelé) a választott motorral
        if (rm == audioopt::RateMode::Fixed && fixedRate > 0) return { fixedRate };
        // Rendszer és „forrást követi”: ha az USB DAC valós hardveres rátái kiolvashatók (/proc/asound/cardN/stream0),
        // azokat vesszük alapul (és metsszük a PipeWire allowed-rates listájával), így pl. 48 kHz-es DAC-ra nem kér 96 kHz-et
        const auto pwRates = parseAllowedRates(pwSetting("clock.allowed-rates"));
        const auto dacRates = detectCurrentDacRates();
        if (!dacRates.empty()) {
            if (!pwRates.empty()) {
                std::vector<uint32_t> common;
                for (uint32_t r : dacRates)
                    if (std::find(pwRates.begin(), pwRates.end(), r) != pwRates.end()) common.push_back(r);
                if (!common.empty()) return common;
            }
            return dacRates;
        }
        if (!pwRates.empty()) return pwRates;
        return { 44100, 48000, 88200, 96000, 176400, 192000 };
    };
    player::pwActualRateHook() = [&]() -> uint32_t {
        return node.actualGraphRate();
    };
    player::pwBeforeOpenHook() = [&](uint32_t) {
        s_playerTicket.fetch_add(1);
        s_playerOpen = true;
        {
            // A beépített lejátszó a saját (libsoxr / FFT64) útján dönt a mintavételről: az élő út kényszere megszűnik
            audioopt::RateMode rm = audioopt::RateMode::System;
            { std::lock_guard<std::mutex> lk(audioMutex); rm = audioOpts.rateMode; }
            std::lock_guard<std::mutex> lk(liveMu);
            liveReleaseLocked(rm);
            if (liveOwnTarget != 0) {
                node.setLiveUpsample(false, 0, 0, audioopt::AudioOptions{});
                liveOwnTarget = 0;
            }
        }
        bool paused = false;
        {
            std::lock_guard<std::mutex> lk(audioMutex);
            if (audioOpts.rateMode == audioopt::RateMode::Follow && audioOpts.keepAwake) {
                node.setAudioOptions(audioOpts.bufferFrames, false, cachedAutoQuantum);
                paused = true;
            }
        }
        if (paused) std::this_thread::sleep_for(std::chrono::milliseconds(300));   // az ébren tartó hangfolyam megszűnik
    };
    player::pwAfterCloseHook() = [&]() {
        const uint64_t ticket = s_playerTicket.fetch_add(1) + 1;
        std::thread([&, ticket] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            if (s_playerTicket.load() != ticket || s_engineQuit.load()) return;   // közben új szám nyílt, vagy leállás
            s_playerOpen = false;
            std::lock_guard<std::mutex> lk(audioMutex);
            if (!audioOpts.keepAwake) return;
            node.setAudioOptions(audioOpts.bufferFrames, true, cachedAutoQuantum);
        }).detach();
    };
    // Az élő út figyelője: másodpercenként megnézi, milyen mintavételű külső hangfolyam szól az Aqulify eszközre, és
    // bekapcsolt felmintavételezésnél a gráfot a célra állítja. Csak akkor kérdez (pactl), ha van mit eldönteni.
    std::thread liveThread([&] {
        std::string ownSinkId, dacSinkId;
        std::vector<uint32_t> dacRates;
        int idleTicks = 0;
        const std::vector<uint32_t> fallback{ 44100, 48000, 88200, 96000, 176400, 192000 };
        for (;;) {
            audioopt::AudioOptions o;
            { std::lock_guard<std::mutex> lk(audioMutex); o = audioOpts; }
            const bool relevant = o.upsampleRule != audioopt::UpsampleRule::Off && o.rateMode != audioopt::RateMode::Fixed;
            {
                std::unique_lock<std::mutex> lk(liveMu);
                liveCv.wait_for(lk, std::chrono::milliseconds(relevant ? 1000 : 5000), [&] { return liveWake || liveQuit; });
                if (liveQuit) return;
                liveWake = false;
            }
            { std::lock_guard<std::mutex> lk(audioMutex); o = audioOpts; }
            uint32_t src = 0;
            const bool playerOpen = s_playerOpen.load();
            if (o.upsampleRule != audioopt::UpsampleRule::Off && o.rateMode != audioopt::RateMode::Fixed && !playerOpen) {
                if (ownSinkId.empty()) ownSinkId = liveups::sinkIdByName(runCapture("pactl list short sinks"), "aqulify_sink");
                const auto inputs = liveups::parseSinkInputs(runCapture("pactl list sink-inputs"));
                src = liveups::liveSourceRate(inputs, ownSinkId);
                std::string outSink;
                for (const auto& s : inputs)
                    if (s.nodeName == "aqulify_sink_output") outSink = s.sinkId;
                if (src > 0 && outSink != dacSinkId) {   // a DAC (a mi kimenetünk célja) tudott mintavételei
                    dacSinkId = outSink;
                    dacRates.clear();
                    const int card = liveups::alsaCardOfSink(runCapture("pactl list sinks"), outSink);
                    if (card >= 0) {
                        std::ifstream f("/proc/asound/card" + std::to_string(card) + "/stream0");
                        const std::string txt((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                        dacRates = liveups::parseStream0PlaybackRates(txt);
                    }
                }
            }
            const liveups::Decision d = liveups::decide(src, o, dacRates, fallback, playerOpen);
            std::lock_guard<std::mutex> lk(liveMu);
            if (liveQuit) return;
            liveState = d;
            liveDacKnown = !dacRates.empty();
            // Szám közti szünetnél (nincs forrás) nem engedjük el rögtön: 3 s után
            idleTicks = (d.reason == "no_source") ? idleTicks + 1 : 0;
            if (o.upsampleSystemAudio) {
                // Saját motoros mód (libsoxr / Aqulify FFT64): a capture marad a forrás natív mintavételén,
                // a playback stream külön órajelen, a cél mintavételen megy a DAC-ra.
                if (liveForced != 0) liveReleaseLocked(o.rateMode);
                if (d.forceRate > 0 && d.srcRate > 0 && d.forceRate > d.srcRate) {
                    node.setLiveUpsample(true, d.srcRate, d.forceRate, o);
                    liveOwnTarget = d.forceRate;
                } else if (liveOwnTarget != 0 && (d.reason != "no_source" || idleTicks >= 3)) {
                    node.setLiveUpsample(false, 0, 0, o);
                    liveOwnTarget = 0;
                }
            } else {
                if (liveOwnTarget != 0) {
                    node.setLiveUpsample(false, 0, 0, o);
                    liveOwnTarget = 0;
                }
                if (d.forceRate > 0) {
                    if (d.forceRate != liveForced) {
                        pwSetSetting("clock.force-rate", std::to_string(d.forceRate));
                        std::cout << "[Felmintavételezés] Élő út: " << d.srcRate << " Hz -> " << d.forceRate
                                  << " Hz (a PipeWire átalakítójával; a DAC mintavételei: "
                                  << (dacRates.empty() ? std::string("ismeretlen") : std::to_string(dacRates.size()) + " db") << ").\n";
                        liveForced = d.forceRate;
                    }
                } else if (liveForced != 0 && (d.reason != "no_source" || idleTicks >= 3)) {
                    liveReleaseLocked(o.rateMode);
                }
            }
        }
    });
    auto stopLiveThread = [&] {
        {
            std::lock_guard<std::mutex> lk(liveMu);
            liveQuit = true;
        }
        liveCv.notify_all();
        if (liveThread.joinable()) liveThread.join();
        audioopt::RateMode rm = audioopt::RateMode::System;
        { std::lock_guard<std::mutex> lk(audioMutex); rm = audioOpts.rateMode; }
        std::lock_guard<std::mutex> lk(liveMu);
        liveReleaseLocked(rm);
        if (liveOwnTarget != 0) {
            node.setLiveUpsample(false, 0, 0, audioopt::AudioOptions{});
            liveOwnTarget = 0;
        }
    };
    // Korai kilépésnél (pl. foglalt portok) is le kell állnia, különben a std::thread megszűnése leállítja a programot
    struct LiveStopper {
        std::function<void()> f;
        ~LiveStopper() { if (f) f(); }
    } liveStopper{ stopLiveThread };
    cb.audioOptionsJson = [&]() -> std::string {
        std::lock_guard<std::mutex> lk(audioMutex);
#if defined(AQ_HAVE_ALSA)
        const char* exclCap = ",\"exclusive\":true,\"exclusiveKind\":\"alsa\",\"dop\":true";   // közvetlen ALSA-kimenet (hardveres hangerő nincs, DoP támogatott)
#else
        const char* exclCap = "";
#endif
        // nincs telepítve: minden lekérdezéskor újra keresi (a felhasználó közben telepíthette)
        if (audioOpts.netRecv && netReceiver.state() == aq::netrecv::State::Missing)
            netReceiver.requestApply(true, audioOpts.netRecvName);
        return "{\"opts\":" + optsToJsonWithDop(audioOpts, dsdDop) +
               ",\"caps\":{\"buffer\":true,\"keepAwake\":true,\"rate\":true,\"resample\":true,\"player\":true,\"upsample\":true,\"systemUpsample\":true" +
               exclCap + ",\"netRecv\":true,\"upnp\":true},\"netRecvState\":" + netReceiver.statusJson() +
               ",\"upnpState\":" + upnpService.statusJson() + "}";
    };
    cb.setAudioOptions = [&](const json::Value& v) {
        std::lock_guard<std::mutex> lk(audioMutex);
        const audioopt::AudioOptions o = audioopt::fromJson(v, audioOpts);
        bool wantDop = dsdDop;
        if (const json::Value* dopVal = v.get("dsdDop"); dopVal && dopVal->type == json::Value::Type::Bool) {
            wantDop = dopVal->b;
        }
        if (o == audioOpts && wantDop == dsdDop) return;
        audioOpts = o;
        dsdDop = wantDop;
        health.setEnabled(o.errorReports);
        reportService.setEnabled(o.errorReports);
        applyAudio(o);
        const std::string savedJson = optsToJsonWithDop(o, dsdDop);
        if (!linux_store::saveAudioOptions(savedJson)) std::cerr << "[Hangkimenet] Figyelem: az audio.json nem írható.\n";
        std::cout << "[Hangkimenet] " << savedJson << "\n";
    };
    // Terhelés-tanácsadó (a motor processzorterhelése, hangkimaradások; Pi-n szigorúbb küszöb)
    load::Tracker loadTracker(load::defaultThresholdPct());
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
            bool dopActive = false;
            bool dopWant = false;
            bool dsdNative = false;
            uint32_t dopDsdRate = 0;
            std::string dopFb, dsdFormat;
#if defined(AQ_HAVE_ALSA)
            dopActive = filePlayer.isDopActive();
            dopWant = filePlayer.dsdDop();
            dopDsdRate = filePlayer.dopDsdRate();
            dopFb = filePlayer.dopFallbackReason();
            if (dopActive && alsaRaw && alsaRaw->isNativeDsd()) {   // natív DSD (DSD_U32 / U16 / U8), nem DoP
                dsdNative = true;
                dsdFormat = alsaRaw->nativeDsdFormat();
            }
#endif
            const bool transparent = dopActive || (chainTransparent(p) && !crossover);
            const bool dspTransparent = dopActive || chainTransparent(p);
            char buf[720];
            std::snprintf(buf, sizeof(buf),
                          ",\"path\":{\"quantum\":%u,\"bufReq\":%d,\"bufMs\":%.2f,\"eqLat\":%zu,\"limLat\":%zu,\"convLat\":%zu,"
                          "\"dspMs\":%.2f,\"totalMs\":%.1f,\"load\":%.1f,\"peak\":%.1f,\"overruns\":%u,\"transparent\":%s,"
                          "\"dspTransparent\":%s,\"keepAwake\":%s,\"playerRate\":%u,\"playerGain\":%.2f,"
                          "\"dop\":{\"want\":%s,\"active\":%s,\"dsdRate\":%u,\"fallback\":\"%s\",\"native\":%s,\"format\":\"%s\"}}",
                          q, reqBuf, bufMs, eqLat, limLat, convLat, dspMs, 2.0 * bufMs + dspMs, 100.0 * node.dspLoad(),
                          100.0 * node.takeDspPeak(), node.overruns(), transparent ? "true" : "false",
                          dspTransparent ? "true" : "false",
                          keep && node.keepAwakeActive() ? "true" : "false", filePlayer.streamRate(),
                          filePlayer.hasTrack() ? filePlayer.currentGainDb() : 0.0f,
                          dopWant ? "true" : "false", dopActive ? "true" : "false",
                          dopDsdRate, dopFb.c_str(), dsdNative ? "true" : "false", dsdFormat.c_str());
            const bool uActive = filePlayer.isUpsampleActive();
            const uint32_t uSrc = filePlayer.upsampleSrcRate();
            const uint32_t uTgt = filePlayer.upsampleTargetRate();
            uint32_t uActual = node.actualGraphRate();
#if defined(AQ_HAVE_ALSA)
            if (playerSel && playerSel->exclusiveActive()) {
                uActual = playerSel->actualRate();
            }
#endif
            const bool uResampleWarn = uActive && uActual > 0 && uTgt > 0 && uActual != uTgt;
            const std::string uReason = filePlayer.upsampleReason();
            const uint64_t uClipped = filePlayer.upsampleClippedSamples();
            const uint64_t uProcessed = filePlayer.upsampleProcessedFrames();
            const double uDelayMs = filePlayer.upsampleDelayMs();
            char ubuf[384];
            std::snprintf(ubuf, sizeof(ubuf),
                          ",\"upsample\":{\"active\":%s,\"srcRate\":%u,\"targetRate\":%u,\"actualDacRate\":%u,"
                          "\"resampleWarning\":%s,\"reason\":\"%s\",\"clippedSamples\":%llu,"
                          "\"processedFrames\":%llu,\"delayMs\":%.2f}",
                          uActive ? "true" : "false", uSrc, uTgt, uActual,
                          uResampleWarn ? "true" : "false", uReason.c_str(),
                          static_cast<unsigned long long>(uClipped),
                          static_cast<unsigned long long>(uProcessed), uDelayMs);
            // Élő út (más programból az Aqulify eszközre szóló hang): saját motor (libsoxr / Aqulify FFT64) vagy PipeWire
            char lbuf[384];
            {
                std::lock_guard<std::mutex> lk(liveMu);
                const bool ownOn = node.liveUpsampleActive() && liveOwnTarget > 0;
                const auto bst = ownOn ? node.liveUpsampleStatus() : livebridge::BridgeStatus{};
                const char* conv = ownOn
                    ? audioopt::upsampleConverterCode(bst.engineType)
                    : "pipewire";
                const uint32_t tgtRate = ownOn ? liveOwnTarget : liveForced;
                std::snprintf(lbuf, sizeof(lbuf),
                              ",\"liveUpsample\":{\"active\":%s,\"ownEngine\":%s,\"engine\":%d,"
                              "\"srcRate\":%u,\"targetRate\":%u,\"actualDacRate\":%u,"
                              "\"latencyMs\":%.1f,\"driftPpm\":%.1f,\"underruns\":%llu,"
                              "\"reason\":\"%s\",\"dacRatesKnown\":%s,\"converter\":\"%s\"}",
                              (ownOn || liveForced > 0) ? "true" : "false",
                              ownOn ? "true" : "false",
                              ownOn ? bst.engineType : 0,
                              ownOn ? bst.srcRate : liveState.srcRate,
                              tgtRate, node.actualGraphRate(),
                              ownOn ? bst.latencyMs : 0.0,
                              ownOn ? bst.driftPpm : 0.0,
                              static_cast<unsigned long long>(ownOn ? bst.underruns : 0),
                              jsonEscape(liveState.reason).c_str(), liveDacKnown ? "true" : "false", conv);
            }
            audioopt::AudioOptions curOpts;
            { std::lock_guard<std::mutex> lk(audioMutex); curOpts = audioOpts; }
            const bool dspBypassed = node.chain.dspBypassedForRate() || filePlayer.streamRate() > audioopt::kMaxDspRate;
            return s + buf + ubuf + lbuf +
                   loadTracker.json(curOpts, uActive ? uTgt : uSrc, filePlayer.underruns(), dspBypassed,
                                    filePlayer.upsampleThreadsInUse(), filePlayer.dedicatedCoreApplied());
        };
    }
    cb.getLanguage = [] { return aq_i18n::savedLanguage(); };
    cb.setLanguage = [](const std::string& l) {
        if (aq_i18n::set(l) && !aq_i18n::saveLanguage(l)) std::cerr << "[Nyelv] Figyelem: a language.txt nem írható.\n";
    };
    // A legújabb kiadás ellenőrzése (6 óránként, a rendszer curl programjával; AQULIFY_NO_UPDATE_CHECK=1: kikapcsolva).
    // A híd előtt jön létre, így utána szűnik meg (a híd a statusJson-t hívja).
    vercheck::Checker versionChecker;
    if (!std::getenv("AQULIFY_NO_UPDATE_CHECK")) {
        versionChecker.start(vercheck::fetchWithCurl, AQULIFY_VERSION);
        cb.versionStatus = [&versionChecker] { return versionChecker.statusJson(); };
    }
    cb.errorReportStatus = [&reportService] { return reportService.statusJson(); };
    cb.errorReportPreview = [&reportService](const std::string& kind, const std::string& symptom, const std::string& note,
                                             std::string& preview, size_t& bytes, bool& configured) {
        reportService.buildPreview(kind, symptom, note, preview, bytes, configured);
    };
    cb.errorReportSend = [&reportService](const std::string& kind, const std::string& symptom, const std::string& note,
                                          std::function<void(bool, long long, const std::string&)> onDone) {
        reportService.sendAsync(kind, symptom, note, [onDone = std::move(onDone)](const aq::report::SendResult& r) {
            if (onDone) onDone(r.ok, r.id, r.error);
        });
    };
    cb.errorReportSave = [&reportService](const std::string& kind, const std::string& symptom, const std::string& note,
                                          std::string& filename, std::string& err) {
        return reportService.saveReport(kind, symptom, note, filename, err);
    };
    cb.errorReportDismissPrevious = [&reportService] { reportService.dismissPrevious(); };
    cb.errorLogRead = [&reportService](int lines, int& outLines) { return reportService.readLog(lines, outLines); };

    // Boot-partíciós aqulify-reset ellenőrzése még a híd indulása előtt
    {
        const auto resetRes = headless::applyBootReset(headless::defaultBootDirs(),
                                                       linux_store::configDir(true) + "/remote.json");
        if (resetRes.triggered) {
            std::cout << "[Headless] aqulify-reset észlelve (" << resetRes.flagPath
                      << "): párosítások törölve=" << (resetRes.configCleared ? "igen" : "nem")
                      << ", jelzőfájl törölve=" << (resetRes.flagRemoved ? "igen" : "nem") << ".\n";
            if (!resetRes.error.empty()) std::cerr << "[Headless] Figyelem: " << resetRes.error << "\n";
        }
    }

    const auto headlessDec = aq::linux_headless::detect(forceHeadless);
    std::cout << aq::linux_headless::formatLogLine(headlessDec) << "\n";
    if (const std::string w = aq::linux_headless::lingerWarning(headlessDec, aq::linux_headless::currentUserName());
        !w.empty())
        std::cout << w << "\n";

    std::unique_ptr<WebSocketBridge> bridge;
    for (int p : ports) {
        auto b = std::make_unique<WebSocketBridge>(p);
        b->setHeadlessMode(headlessDec.headless);
        if (b->start(cb)) {
            if (ports.size() > 1 && p != kEnginePorts[0]) defaultPortBusy.store(true, std::memory_order_relaxed);
            bridge = std::move(b);
            break;
        }
        std::cout << "[Info] A " << p << " port foglalt, a következőt próbálom.\n";
    }
    if (!bridge) {
        std::cerr << "[Hiba] Minden kapcsolódási port foglalt; a felület nem tud csatlakozni.\n"
                     "       Ellenőrizd: ss -ltnp | grep -E '4781|8080'\n";
        node.shutdown();
        return 1;
    }

    if (listenLan) {
        bridge->setRemoteEnabled(true, /*openPairCode=*/!headlessDec.headless);
        std::cout << "[Hálózat] Távvezérlés parancssorból bekapcsolva (--listen lan).\n";
    } else if (headlessDec.headless && !bridge->remoteEnabled() && linux_store::loadRemote().empty()) {
        // Képernyő nélküli (headless) első indítás: automatikusan gazdátlan (claimable) módban indul
        bridge->setRemoteEnabled(true, /*openPairCode=*/false);
        std::cout << "[Headless] Képernyő nélküli mód: gazdátlan beállítás aktív (mDNS claimable=1).\n";
    }

    Saver saver{ &paramMutex, &current, &dirty, &lastChange };
    node.addTimer(saveTick, &saver, 1000);

    // Beépített hibajelentés: őrszál, állapotfájl, az előző futás rendellenes végének helyi jelentése (küldés nincs)
    {
        aq::health::Config hc;
        hc.dir = diagDir;
        hc.logPath = diagDir + "/engine.log";
        hc.version = AQULIFY_VERSION;
        hc.platform = "linux";
        hc.package = installKind;
        { std::lock_guard<std::mutex> lk(audioMutex); hc.enabled = audioOpts.errorReports; }
        hc.crashHandlers = true;
        if (const char* s = std::getenv("AQULIFY_WATCHDOG_SECONDS"); s && std::atof(s) >= 1.0) hc.thresholdSec = std::atof(s);
        for (const auto& n : aq::report::namesFromJson(linux_store::loadRemote())) hc.scrubNames.push_back(n);
        for (const auto& n : aq::report::namesFromJson(linux_store::loadRemoteTargets())) hc.scrubNames.push_back(n);
        if (!health.start(hc)) std::cerr << "[Hibajelentés] A diagnosztikai mappa nem írható: " << diagDir << "\n";
        reportService.start(hc.dir, hc.logPath, hc.scrubNames, hc.enabled);
        reportService.setNamesProvider([] {
            std::vector<std::string> names;
            for (const auto& n : aq::report::namesFromJson(linux_store::loadRemote())) names.push_back(n);
            for (const auto& n : aq::report::namesFromJson(linux_store::loadRemoteTargets())) names.push_back(n);
            return names;
        });
    }
    health.setSection("system", systemSectionJson());
    // A motor állapota az állapotfájlba (2 s-onként, a PipeWire fő szálán; az őrszál csak ezt a másolatot olvassa)
    static std::function<void()> s_healthTick;
    int healthTicks = 0;
    s_healthTick = [&] {
        if (healthTicks++ % 15 == 0) health.setSoundCards(asoundSummary());
        audioopt::AudioOptions o;
        bool dop = false;
        { std::lock_guard<std::mutex> lk(audioMutex); o = audioOpts; dop = dsdDop; }
        std::string path;
        uint32_t rate = node.sampleRate.load();
#if defined(AQ_HAVE_ALSA)
        if (filePlayer.isDopActive()) {
            const uint32_t dsd = filePlayer.dopDsdRate();
            const bool native = alsaRaw && alsaRaw->isNativeDsd();
            path = std::string(native ? "native DSD" : "DoP") + ", DSD" + std::to_string(dsd / 44100) + ", exclusive ALSA" +
                   (native ? " (" + alsaRaw->nativeDsdFormat() + ")" : std::string());
            rate = filePlayer.streamRate();
        } else if (playerExclusive()) {
            rate = playerDsp->sampleRate();
            path = "PCM " + std::to_string(rate) + " Hz, exclusive ALSA";
        } else
#endif
        {
            path = filePlayer.hasTrack() ? "PCM " + std::to_string(filePlayer.streamRate()) + " Hz, player via PipeWire"
                                         : "PipeWire (system sound through the Aqulify device)";
        }
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "{\"path\":\"%s\",\"rate\":%u,\"player\":%s,\"dsp_streaming\":%s,\"dsp_load_pct\":%.1f,\"overruns\":%u,"
                      "\"player_underruns\":%llu,\"upsample\":%s,\"dsd_dop_setting\":%s,\"audio_opts\":",
                      jsonEscape(path).c_str(), rate, filePlayer.hasTrack() ? "true" : "false",
                      node.playbackStreaming.load() ? "true" : "false", 100.0 * node.dspLoad(), node.overruns(),
                      static_cast<unsigned long long>(filePlayer.underruns()), filePlayer.isUpsampleActive() ? "true" : "false",
                      dop ? "true" : "false");
        // „Nincs hang” diagnosztika: hová megy a kimenet (üres: a PipeWire alapértelmezettje), a kimeneti stream
        // állapota, mikor jött utoljára nem néma rendszerhang (-1: az indulás óta nem), és a kimenet hangereje/némítása
        const std::string target = node.outputTarget();
        const int volPct = cachedVolPct.load(std::memory_order_relaxed);
        const int volMut = cachedVolMuted.load(std::memory_order_relaxed);
        const std::string diag = "\"output_target\":\"" + jsonEscape(target.empty() ? std::string("(default)") : target) +
                                 "\",\"output_state\":\"" + node.playbackStateName() +
                                 "\",\"input_signal_age_ms\":" + std::to_string(node.inputSignalAgeMs()) +
                                 ",\"output_volume_pct\":" + std::to_string(volPct) +
                                 ",\"output_muted\":" + (volMut < 0 ? "null" : (volMut ? "true" : "false")) +
                                 (pwPlayerRaw ? (std::string(",\"player_pw_state\":\"") + pwPlayerRaw->streamStateName() + "\"") : std::string()) + ",";
        std::string head(buf);
        head.insert(1, diag);   // a nyitó { után
        health.setSection("engine", head + audioopt::toJson(o) + "}");
    };
    s_healthTick();
    node.addTimer([](void*, uint64_t) { if (s_healthTick) s_healthTick(); }, nullptr, 2000);

    std::cout << "Fut. Leállítás: Ctrl+C (vagy systemctl --user stop aqulify).\n";
    node.run();

    std::cout << "[Info] Leállítás...\n";
    bridge->stop();
    s_engineQuit = true;
    player::pwBeforeOpenHook() = nullptr;
    player::pwAfterCloseHook() = nullptr;
    player::pwAllowedRatesHook() = nullptr;
    player::pwActualRateHook() = nullptr;
    filePlayer.stop();
    mediaSession.stop();
    {
        // Az utolsó beállítás marad a params.bin-ben: a következő indításkor visszaáll
        std::lock_guard<std::mutex> lk(paramMutex);
        if (dirty) linux_store::saveParams(current);
    }
    stopLiveThread();   // az élő út kényszere megszűnik, mielőtt az eredeti beállítás visszaáll
    {
        // A PipeWire mintavételi beállítása visszaáll (a program bezárása után a rendszer a régi)
        audioopt::AudioOptions off = audioOpts;
        off.rateMode = audioopt::RateMode::System;
        off.netRecv = false;
        off.upnp = false;
        applyAudio(off);
    }
#if defined(AQ_HAVE_SYSTEMD)
    tray.stop();
#endif
    const bool crashedDaemon = node.coreError.load();
    netReceiver.apply(false, "");   // a vevő az Aqulify eszköz megszűnése előtt álljon le
    node.shutdown();
    s_healthTick = nullptr;
    reportService.stop(); // nem vár hálózati időkorlátra
    health.stopClean();   // a „rendben leállt” jel: a következő indítás nem készít rendellenes-vég jelentést
    return crashedDaemon ? 2 : 0;
}
