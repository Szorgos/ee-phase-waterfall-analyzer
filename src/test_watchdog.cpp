// Beépített hibajelentés, 1. szakasz: őrszem (szívverés), helyi állapotfájl és jelentés, tisztítás, előző futás
// rendellenes vége. A 2026-11-19-i beragadás (a híd szála nem jön ki a hang lehúzásából) itt teszt.
//
// Használat: test_watchdog              (minden csoport)
//            test_watchdog --child <kill|abort|clean> <mappa> <napló>   (belső: a [4] gyermekfolyamata)
// Az AQ_CHECK_SEED környezeti változó (ha van) a tisztítás-teszt véletlen neveit és címeit adja.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "audio_options.hpp"
#include "error_report.hpp"
#include "health_monitor.hpp"
#include "mini_json.hpp"
#include "watchdog.hpp"
#include "ws_bridge.hpp"

// A hangeszköz alapformátumának szöveggé alakítója (1b, P3). A befagyasztott tesztcommitban még nincs meg: akkor a
// helyettesítő üres szöveget ad, és a [7] csoport elbukik.
#if __has_include("device_format.hpp")
#include "device_format.hpp"
#else
namespace aq::devfmt {
inline std::string describe(const uint8_t*, size_t) { return {}; }
}
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

using namespace ee_analyzer;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; std::printf("  [HIBA] " __VA_ARGS__); std::printf("\n"); } else { std::printf("  [OK]   " __VA_ARGS__); std::printf("\n"); } std::fflush(stdout); } while (0)

static std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void appendLine(const fs::path& p, const std::string& line) {
    std::ofstream f(p, std::ios::binary | std::ios::app);
    f << line << "\n";
}

static fs::path makeTempDir(const std::string& tag) {
    std::mt19937_64 rng(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path p = fs::temp_directory_path() / ("aq_wd_" + tag + "_" + std::to_string(rng() % 1000000000ULL));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

static bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

static double secondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

/** Az állapotfájl „stalls” listájában szerepel-e a szál (a fájlból, a motortól függetlenül). */
static bool stateHasStall(const fs::path& dir, const std::string& thread, std::string* labelOut = nullptr) {
    const std::string doc = readFile(dir / "state.json");
    json::Value v;
    if (doc.empty() || !json::parse(doc, v)) return false;
    const json::Value* st = v.get("stalls");
    if (!st || st->type != json::Value::Type::Array) return false;
    for (const auto& s : st->arr) {
        const json::Value* t = s.get("thread");
        if (t && t->type == json::Value::Type::String && t->str == thread) {
            if (labelOut) {
                const json::Value* l = s.get("label");
                *labelOut = (l && l->type == json::Value::Type::String) ? l->str : std::string();
            }
            return true;
        }
    }
    return false;
}

static std::vector<std::string> stallThreads(const fs::path& dir) {
    std::vector<std::string> out;
    const std::string doc = readFile(dir / "state.json");
    json::Value v;
    if (doc.empty() || !json::parse(doc, v)) return out;
    const json::Value* st = v.get("stalls");
    if (!st || st->type != json::Value::Type::Array) return out;
    for (const auto& s : st->arr)
        if (const json::Value* t = s.get("thread"); t && t->type == json::Value::Type::String) out.push_back(t->str);
    return out;
}

// ------------------------------------------------------------------------------------------------ [1]
static void testWatchdogBasic() {
    std::printf("[1] Őrszem: a megálló szál jelez (küszöb + 1 s-on belül), a többi nem\n");
    const fs::path dir = makeTempDir("basic");
    const fs::path log = dir / "engine.log";
    aq::wd::Registry reg;
    std::atomic<bool> notExpected{ false };
    std::atomic<bool> stop{ false };

    aq::health::Config c;
    c.dir = dir.string();
    c.logPath = log.string();
    c.version = "0.0.0-test";
    c.platform = "test";
    c.package = "test";
    c.thresholdSec = 1.0;
    c.writeIntervalMs = 200;
    c.registry = &reg;
    aq::health::Monitor mon;
    CHECK(mon.start(c), "az őrszem elindul (saját regiszter, küszöb 1 s)");

    std::chrono::steady_clock::time_point lastBeat{};
    std::atomic<bool> stopped{ false };
    std::thread tStop([&] {
        aq::wd::Heartbeat hb("t-stop", nullptr, &reg);
        for (int i = 0; i < 20; ++i) { hb.beat("teszt: dolgozik"); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        hb.beat("teszt: megallas");
        lastBeat = std::chrono::steady_clock::now();
        stopped = true;
        while (!stop) std::this_thread::sleep_for(std::chrono::milliseconds(10));   // nem ad több szívverést
    });
    std::thread tOk([&] {
        aq::wd::Heartbeat hb("t-ok", nullptr, &reg);
        while (!stop) { hb.beat("teszt: rendben"); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    });
    std::thread tIdle([&] {
        // regisztrált, de most nem várunk tőle szívverést (pl. a kimenet, amíg nem szól): nem jelezhet
        aq::wd::Heartbeat hb("t-idle", &notExpected, &reg);
        hb.beat("teszt: nem kell");
        while (!stop) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });
    while (!stopped) std::this_thread::sleep_for(std::chrono::milliseconds(5));

    double detected = -1.0;
    std::string label;
    while (secondsSince(lastBeat) < 4.0) {
        if (stateHasStall(dir, "t-stop", &label)) { detected = secondsSince(lastBeat); break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::printf("K_METRIC:{\"test\":\"test_watchdog\",\"group\":1,\"detect_s\":%.3f}\n", detected);
    CHECK(detected >= 0.9 && detected <= 2.0, "a megálló szál %.2f s után jelzett (elvárt: 1,0–2,0 s)", detected);
    CHECK(label == "teszt: megallas", "az állapotfájlban az utolsó címke: \"%s\"", label.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const std::vector<std::string> th = stallThreads(dir);
    CHECK(std::count(th.begin(), th.end(), "t-stop") == 1, "a megálló szál egyszer szerepel a listában");
    CHECK(std::count(th.begin(), th.end(), "t-ok") == 0, "a rendesen jelző szál nem jelez");
    CHECK(std::count(th.begin(), th.end(), "t-idle") == 0, "a most nem várt szál nem jelez");
    const std::string logText = readFile(log);
    CHECK(contains(logText, "t-stop") && contains(logText, "teszt: megallas"), "a naplóban: a szál neve és az utolsó címke");

    const aq::report::Report rep = aq::report::build(dir.string(), log.string(), aq::report::ScrubContext{}, "manual", "freeze");
    CHECK(contains(rep.json, "t-stop") && contains(rep.json, "teszt: megallas"), "a jelentés (JSON) tartalmazza a szál nevét és címkéjét");
    CHECK(contains(rep.preview, "t-stop") && contains(rep.preview, "teszt: megallas"), "az előnézet is");
    json::Value rv;
    CHECK(json::parse(rep.json, rv), "a jelentés érvényes JSON");

    // állapotfájl: a kötelező mezők
    json::Value sv;
    const bool parsed = json::parse(readFile(dir / "state.json"), sv);
    CHECK(parsed, "az állapotfájl érvényes JSON");
    if (parsed) {
        CHECK(sv.get("version") && sv.get("platform") && sv.get("package"), "verzió, platform, csomagtípus");
        const json::Value* hb = sv.get("heartbeats");
        CHECK(hb && hb->type == json::Value::Type::Array && hb->arr.size() >= 3, "szívverések listája (3 szál)");
        const json::Value* pr = sv.get("process");
        CHECK(pr && pr->get("cpu_pct") && pr->get("rss_kb"), "CPU- és memóriahasználat");
        const json::Value* ce = sv.get("clean_exit");
        CHECK(ce && ce->type == json::Value::Type::Bool && !ce->b, "futás közben clean_exit = false");
    }
    stop = true;
    tStop.join();
    tOk.join();
    tIdle.join();
    mon.stopClean();
    json::Value sv2;
    CHECK(json::parse(readFile(dir / "state.json"), sv2) && sv2.get("clean_exit") && sv2.get("clean_exit")->b,
          "rendes leállás után clean_exit = true");
    CHECK(!fs::exists(dir / "running"), "rendes leállás után a „fut” jel törölve");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ------------------------------------------------------------------------------------------------ [2]
static void testBridgeStall() {
    std::printf("[2] A 2026-11-19-i hiba mint teszt: a híd szála nem jön ki a hang lehúzásából\n");
    const fs::path dir = makeTempDir("bridge");
    const fs::path log = dir / "engine.log";
    aq::health::Config c;
    c.dir = dir.string();
    c.logPath = log.string();
    c.version = "0.0.0-test";
    c.platform = "test";
    c.package = "test";
    // alapértelmezett küszöb (5 s), a motor globális regisztere (a híd ide jelez)
    aq::health::Monitor mon;
    CHECK(mon.start(c), "az őrszem elindul (alapértelmezett küszöb, globális regiszter)");
    mon.setSection("engine", "{\"path\":\"natív DSD, DSD256, kizárólagos ALSA\",\"device\":\"FiiO BTR7\"}");

    // A hamis hangforrás: élesítés után örökké adatot ad, a hívásból nem tér vissza (a híd ciklusa nem lép ki).
    // A feloldás csak a teszt végén, hogy a híd rendben leállítható legyen.
    std::atomic<bool> armed{ false }, release{ false };
    std::atomic<uint64_t> produced{ 0 };
    BridgeCallbacks cb;
    cb.pullAudio = [&](float* lr, size_t maxFrames) -> size_t {
        if (!armed) return 0;
        while (!release) {
            std::fill_n(lr, maxFrames * 2, 0.0f);
            produced += maxFrames;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return 0;
    };
    cb.sampleRate = [] { return 352800.0; };
    cb.getParams = [] { return transparentParams(); };
    cb.setParams = [](const ParamData&) {};
    WebSocketBridge bridge(47898);
    const bool started = bridge.start(cb);
    CHECK(started, "a híd elindul (127.0.0.1:47898)");
    if (!started) { mon.stopClean(); return; }
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    CHECK(!stateHasStall(dir, "bridge"), "élesítés előtt a híd nem jelez");

    const auto t0 = std::chrono::steady_clock::now();
    armed = true;
    double detected = -1.0;
    std::string label;
    while (secondsSince(t0) < 12.0) {
        if (stateHasStall(dir, "bridge", &label)) { detected = secondsSince(t0); break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::printf("K_METRIC:{\"test\":\"test_watchdog\",\"group\":2,\"detect_s\":%.3f}\n", detected);
    CHECK(detected >= 4.5 && detected <= 10.0, "az őrszem %.2f s után jelezte a hidat (elvárt: 5–10 s)", detected);
    CHECK(label == "bridge: pullAudio", "az utolsó címke: \"%s\" (elvárt: bridge: pullAudio)", label.c_str());
    CHECK(produced.load() > 0, "a hamis forrás közben folyamatosan adatot adott");

    // A jelentés a beragadt híd mellett, fájlból készül
    const aq::report::Report rep = aq::report::build(dir.string(), log.string(), aq::report::ScrubContext{}, "manual", "freeze");
    CHECK(contains(rep.json, "\"bridge\"") && contains(rep.json, "bridge: pullAudio"), "a jelentésben a szál és a címke");
    CHECK(contains(rep.json, "DSD256") && contains(rep.json, "FiiO BTR7"), "a jelentésben a lejátszási út és a DAC típusa");
    CHECK(contains(rep.json, "\"symptom\":\"freeze\""), "a jelentésben a tünet");
    CHECK(contains(rep.preview, "bridge: pullAudio") && contains(rep.preview, "DSD256"), "az előnézetben is");
    CHECK(contains(readFile(log), "bridge: pullAudio"), "a motor naplójában is");
    const std::vector<std::string> th = stallThreads(dir);
    CHECK(th.size() == 1 && th[0] == "bridge", "csak a híd jelez (%zu bejegyzés)", th.size());

    release = true;
    const auto ts = std::chrono::steady_clock::now();
    bridge.stop();
    CHECK(secondsSince(ts) < 3.0, "feloldás után a híd leáll");
    mon.stopClean();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ------------------------------------------------------------------------------------------------ [3]
static std::string randomWord(std::mt19937& rng, int minLen, int maxLen) {
    std::uniform_int_distribution<int> len(minLen, maxLen), ch(0, 25);
    std::string s;
    const int n = len(rng);
    for (int i = 0; i < n; ++i) s += static_cast<char>('a' + ch(rng));
    return s;
}

static void testScrub(uint32_t seed) {
    std::printf("[3] Tisztítás: felhasználónév, gépnév, otthoni mappa, IPv4/IPv6, MAC, eszköznév (seed %u)\n", seed);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> b(1, 254), hx(0, 255);
    const std::string user = "u" + randomWord(rng, 6, 9);
    std::string host = "PC-" + randomWord(rng, 5, 8);
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    const std::string phone = "Telefon " + randomWord(rng, 4, 7);
    const std::string ip4a = "192.168." + std::to_string(b(rng)) + "." + std::to_string(b(rng));
    const std::string ip4b = "10." + std::to_string(b(rng)) + "." + std::to_string(b(rng)) + "." + std::to_string(b(rng));
    char mac[32], ip6a[64];
    std::snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", hx(rng), hx(rng), hx(rng), hx(rng), hx(rng), hx(rng));
    std::snprintf(ip6a, sizeof(ip6a), "fe80::%x:%xff:fe%02x:%x", 0x1000 + hx(rng), hx(rng), hx(rng), 0x100 + hx(rng));
    const std::string ip6b = "2001:db8:85a3::8a2e:370:" + std::to_string(1000 + hx(rng));
    std::string macDash = mac;
    std::replace(macDash.begin(), macDash.end(), ':', '-');
    std::transform(macDash.begin(), macDash.end(), macDash.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    std::string macBluez = macDash;
    std::replace(macBluez.begin(), macBluez.end(), '-', '_');

    aq::report::ScrubContext ctx;
    ctx.users = { user };
    ctx.hosts = { host };
    ctx.homes = { "/home/" + user, "C:\\Users\\" + user };
    ctx.names = { phone };

    const std::string text =
        "[Lejátszó] Szól: /home/" + user + "/Zene/Album (2020)/01 dal.flac\n"
        "[Zenetár] mappa: ~/Zene/klasszikus\n"
        "[Windows] C:\\Users\\" + user + "\\Music\\a.flac megnyitva\n"
        "[Hálózat] kliens " + ip4a + " és " + ip4b + ":47820, helyi 127.0.0.1\n"
        "[Hálózat] IPv6 " + ip6a + " és [" + ip6b + "]:47820\n"
        "[Bluetooth] eszköz " + mac + " / " + macDash + ", nyelő bluez_output." + macBluez + ".1\n"
        "[Párosítás] párosítva: " + phone + "\n"
        "[mDNS] Aqulify on " + host + ".local, felhasználó: " + user + "\n"
        "12:34:56 DAC: FiiO BTR7, DSD256, natív DSD; verzió 2.6.1; bridge: pullAudio\n";
    const std::string out = aq::report::scrub(text, ctx);
    std::printf("%s", out.c_str());
    CHECK(!contains(out, user), "a felhasználónév nincs a kimenetben");
    std::string lowerOut = out, lowerHost = host;
    std::transform(lowerOut.begin(), lowerOut.end(), lowerOut.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::transform(lowerHost.begin(), lowerHost.end(), lowerHost.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    CHECK(!contains(lowerOut, lowerHost), "a gépnév nincs a kimenetben (kis- és nagybetűtől függetlenül)");
    CHECK(!contains(out, "/home/") && !contains(out, "C:\\Users") && !contains(out, "~/Zene") && !contains(out, "01 dal.flac"),
          "az otthoni mappa és a fájlútvonalak nincsenek a kimenetben");
    CHECK(contains(out, "<path>"), "az útvonalak helyén <path>");
    CHECK(!contains(out, ip4a) && !contains(out, ip4b), "az IPv4-címek nincsenek a kimenetben");
    CHECK(!contains(out, ip6a) && !contains(out, ip6b) && !contains(out, "fe80:") && !contains(out, "2001:db8"),
          "az IPv6-címek nincsenek a kimenetben");
    std::string macUpper = mac;
    std::transform(macUpper.begin(), macUpper.end(), macUpper.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    CHECK(!contains(out, mac) && !contains(out, macDash) && !contains(out, macBluez) && !contains(out, macUpper),
          "a MAC-címek (: - _ elválasztással) nincsenek a kimenetben");
    CHECK(!contains(out, phone), "a felhasználó által adott eszköznév nincs a kimenetben");
    CHECK(contains(out, "FiiO BTR7") && contains(out, "DSD256") && contains(out, "bridge: pullAudio"),
          "a DAC gyári típusneve, a formátum és a szál címkéje megmarad");
    CHECK(contains(out, "12:34:56") && contains(out, "2.6.1") && contains(out, "127.0.0.1"),
          "az időbélyeg, a verziószám és a helyi visszacsatolási cím megmarad");

    // A tisztítás az előállított jelentés szövegén fut: a naplósorban és az állapotban lévő adat is eltűnik
    const fs::path dir = makeTempDir("scrub");
    const fs::path log = dir / "engine.log";
    appendLine(log, "===== Aqulify motor indul: 2026-11-19 10:00:00 =====");
    appendLine(log, "[Lejátszó] Szól: /home/" + user + "/Zene/a.dsf");
    appendLine(log, "[Hálózat] kliens " + ip4a + " (" + mac + ")");
    {
        std::ofstream f(dir / "state.json", std::ios::binary);
        f << "{\"format\":1,\"version\":\"2.6.1\",\"platform\":\"linux\",\"package\":\"deb\",\"clean_exit\":false,"
             "\"heartbeats\":[],\"stalls\":[],\"process\":{\"cpu_pct\":1.0,\"rss_kb\":1000},"
             "\"sections\":{\"engine\":{\"path\":\"natív DSD, DSD256\",\"host\":\"" + host + "\",\"ip\":\"" + ip6b +
             "\",\"lib\":\"/home/" + user + "/Zene\",\"win\":\"C:\\\\Users\\\\" + user + "\\\\Music\",\"device\":\"FiiO BTR7\"}}}";
    }
    const aq::report::Report rep = aq::report::build(dir.string(), log.string(), ctx, "manual", "noSound");
    for (const std::string* s : { &rep.json, &rep.preview }) {
        CHECK(!contains(*s, user) && !contains(*s, ip4a) && !contains(*s, ip6b) && !contains(*s, mac) && !contains(*s, host),
              "jelentés (%s): nincs benne név, cím, útvonal", s == &rep.json ? "JSON" : "előnézet");
        CHECK(contains(*s, "FiiO BTR7") && contains(*s, "DSD256"), "jelentés (%s): a DAC típusa és a formátum megmarad",
              s == &rep.json ? "JSON" : "előnézet");
    }
    json::Value rv;
    CHECK(json::parse(rep.json, rv), "a tisztított jelentés érvényes JSON");

    // A valódi felhasználónév, gépnév és otthoni mappa (a rendszerből) is eltűnik
    const aq::report::ScrubContext sys = aq::report::systemScrubContext();
    std::string real;
    for (const auto& u : sys.users) real += "user " + u + " ";
    for (const auto& h : sys.hosts) real += "host " + h + " ";
    for (const auto& h : sys.homes) real += "home " + h + "/x.txt ";
    const std::string realOut = aq::report::scrub(real, sys);
    bool anyLeft = false;
    for (const auto& u : sys.users) if (u.size() >= 3 && contains(realOut, u)) anyLeft = true;
    for (const auto& h : sys.hosts) if (h.size() >= 3 && contains(realOut, h)) anyLeft = true;
    for (const auto& h : sys.homes) if (contains(realOut, h)) anyLeft = true;
    CHECK(!sys.users.empty() && !sys.homes.empty(), "a rendszer felhasználóneve és otthoni mappája ismert (%zu / %zu)",
          sys.users.size(), sys.homes.size());
    CHECK(!anyLeft, "a valódi felhasználónév, gépnév és otthoni mappa nincs a kimenetben");

    // A párosított eszközök neve a remote.json-ból
    const std::vector<std::string> names = aq::report::namesFromJson(
        "{\"enabled\":true,\"devices\":[{\"id\":\"1\",\"name\":\"" + phone + "\",\"hash\":\"x\"},{\"id\":\"2\",\"name\":\"Tablet\"}]}");
    CHECK(std::find(names.begin(), names.end(), phone) != names.end() && names.size() == 2, "a párosított eszközök nevei (%zu)", names.size());
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ------------------------------------------------------------------------------------------------ [4]
static int runChild(const std::string& mode, const std::string& dir, const std::string& log) {
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    appendLine(log, "===== Aqulify motor indul: gyermek =====");
    aq::health::Config c;
    c.dir = dir;
    c.logPath = log;
    c.version = "0.0.0-test";
    c.platform = "test";
    c.package = "test";
    c.crashHandlers = (mode == "abort");
    aq::health::Monitor mon;
    if (!mon.start(c)) return 3;
    mon.setSection("engine", "{\"path\":\"PCM 44100\"}");
    appendLine(log, "[Gyermek] utolsó sor a leállás előtt (" + mode + ")");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (mode == "kill") {
#ifdef _WIN32
        TerminateProcess(GetCurrentProcess(), 137);
#else
        kill(getpid(), SIGKILL);
#endif
    } else if (mode == "abort") {
        std::abort();
    } else if (mode == "clean") {
        mon.stopClean();
        return 0;
    }
    return 4;
}

static int spawnChild(const std::string& self, const std::string& mode, const fs::path& dir, const fs::path& log) {
    std::string cmd = "\"" + self + "\" --child " + mode + " \"" + dir.string() + "\" \"" + log.string() + "\"";
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";   // cmd /c: a teljes parancssor idézőjelben
#else
    cmd += " >/dev/null 2>&1";
#endif
    return std::system(cmd.c_str());
}

static void startNext(const fs::path& dir, const fs::path& log, bool enabled, bool& abnormal, std::string& report) {
    appendLine(log, "===== Aqulify motor indul: következő =====");
    appendLine(log, "[Következő] ez már az új futás sora");
    aq::health::Config c;
    c.dir = dir.string();
    c.logPath = log.string();
    c.version = "0.0.0-test";
    c.platform = "test";
    c.package = "test";
    c.enabled = enabled;
    aq::health::Monitor mon;
    mon.start(c);
    abnormal = mon.previousRunAbnormal();
    report = readFile(mon.previousReportPath());
    mon.stopClean();
}

static void testAbnormalEnd(const std::string& self) {
    std::printf("[4] Előző futás rendellenes vége: SIGKILL / abort() után jelentés, rendes leállás után nem\n");
    const fs::path dir = makeTempDir("abnormal");
    const fs::path log = dir / "engine.log";
    bool abnormal = false;
    std::string rep;

    spawnChild(self, "kill", dir, log);
    CHECK(fs::exists(dir / "running"), "a megölt gyermek után a „fut” jel megmaradt");
    startNext(dir, log, false, abnormal, rep);
    CHECK(abnormal, "a következő indítás rendellenes előző futást lát (kill)");
    CHECK(contains(rep, "previous_abnormal") && contains(rep, "utolsó sor a leállás előtt (kill)"),
          "a jelentésben: rendellenes vég és a napló utolsó sora");
    CHECK(!contains(rep, "ez már az új futás sora"), "a jelentésben nincs az új futás naplója");
    CHECK(contains(rep, "PCM 44100"), "a jelentésben az előző futás állapota");
    CHECK(fs::exists(dir / "previous-run.txt"), "olvasható előnézet is készült (previous-run.txt)");

    // Kikapcsolt beállítás: a következő rendes indításkor törlődik
    startNext(dir, log, false, abnormal, rep);
    CHECK(!abnormal, "rendes leállás után a következő indítás nem lát rendellenes futást");
    CHECK(!fs::exists(dir / "previous-run.json") && !fs::exists(dir / "previous-run.txt"),
          "kikapcsolt beállításnál a jelentés a következő rendes indításkor törlődik");

    spawnChild(self, "abort", dir, log);
    startNext(dir, log, true, abnormal, rep);
    CHECK(abnormal, "a következő indítás rendellenes előző futást lát (abort)");
    CHECK(contains(rep, "AQ_CRASH") && contains(rep, "SIGABRT"), "a jelentésben az összeomlás jele (SIGABRT, a kezelő írta)");
    CHECK(contains(rep, "utolsó sor a leállás előtt (abort)"), "és a napló utolsó sora");
    // Bekapcsolt beállítás: a jelentés megmarad (a küldés a 3. szakasz)
    startNext(dir, log, true, abnormal, rep);
    CHECK(!abnormal && fs::exists(dir / "previous-run.json"), "bekapcsolt beállításnál a jelentés megmarad a küldésig");

    const int rc = spawnChild(self, "clean", dir, log);
    CHECK(rc == 0, "a gyermek rendben leállt (kód: %d)", rc);
    CHECK(!fs::exists(dir / "running"), "rendes leállás után nincs „fut” jel");
    startNext(dir, log, false, abnormal, rep);
    CHECK(!abnormal, "rendes leállás után nincs „előző rendellenes futás” jelentés");
    CHECK(!fs::exists(dir / "previous-run.json"), "és a régi jelentés (kikapcsolt beállítás) törlődött");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ------------------------------------------------------------------------------------------------ [5]
static void testSetting() {
    std::printf("[5] A beállítás (errorReports) alapból ki, JSON-ban oda-vissza\n");
    audioopt::AudioOptions o;
    CHECK(!o.errorReports, "alapértéke: ki");
    const std::string j = audioopt::toJson(o);
    CHECK(contains(j, "\"errorReports\":false"), "a JSON-ban: \"errorReports\":false");
    json::Value v;
    json::parse("{\"errorReports\":true}", v);
    const audioopt::AudioOptions on = audioopt::fromJson(v, o);
    CHECK(on.errorReports && !(on == o), "bekapcsolva beolvasható, és az összehasonlítás látja");
    json::Value bad;
    json::parse("{\"errorReports\":\"igen\"}", bad);
    CHECK(!audioopt::fromJson(bad, o).errorReports, "nem logikai érték: marad ki");
}

// ------------------------------------------------------------------------------------------------ [6]
static void testScrubKernelVersion() {
    std::printf("[6] Tisztítás: a négyrészes kernelverzió nem IP-cím, a valódi címek továbbra is eltűnnek\n");
    const aq::report::ScrubContext ctx;
    const std::string text =
        "[Rendszer] Linux 6.6.87.2-microsoft-standard-WSL2 x86_64\n"
        "[Hálózat] kliens 192.168.1.23 csatlakozott\n"
        "[Hálózat] távoli cél 10.0.0.5:47820\n"
        "[Hálózat] utolsó kliens: 192.168.1.23.\n"
        "[Hálózat] a 192.168.1.23-hoz csatlakozik\n"
        "[Híd] felület: ws://127.0.0.1:47899\n";
    const std::string out = aq::report::scrub(text, ctx);
    std::printf("%s", out.c_str());
    CHECK(contains(out, "6.6.87.2-microsoft-standard-WSL2"), "a kernelverzió (6.6.87.2-microsoft-standard-WSL2) megmarad");
    CHECK(!contains(out, "192.168.1.23"), "a 192.168.1.23 eltűnik (mondat közben, mondatvégi ponttal és -hoz raggal is)");
    CHECK(!contains(out, "10.0.0.5"), "a 10.0.0.5:47820 címe eltűnik");
    CHECK(contains(out, "<ip>:47820") && contains(out, "<ip>.") && contains(out, "<ip>-hoz"),
          "a helyükön <ip> (a port, a mondatvégi pont és a rag megmarad)");
    CHECK(contains(out, "ws://127.0.0.1:47899"), "a helyi visszacsatolási cím (ws://127.0.0.1:47899) megmarad");
    const std::string remote = aq::report::scrub("[Híd] távoli: ws://192.168.1.23:47820/ws\n", ctx);
    CHECK(!contains(remote, "192.168.1.23"), "nem helyi ws:// cím eltűnik (%s)", remote.c_str());
}

// ------------------------------------------------------------------------------------------------ [7]
static std::vector<uint8_t> hexBytes(const char* hex) {
    std::vector<uint8_t> out;
    std::istringstream in(hex);
    std::string tok;
    while (in >> tok) out.push_back(static_cast<uint8_t>(std::strtoul(tok.c_str(), nullptr, 16)));
    return out;
}

static void testDeviceFormat() {
    std::printf("[7] A hangeszköz alapformátuma (WAVEFORMATEX / WAVEFORMATEXTENSIBLE bájtsor) szövegként\n");
    struct Case { const char* name; const char* hex; const char* want; };
    // Az első négy minta a Windows regisztrációs adatbázisából (MMDevices\Audio\Render\{…}\Properties,
    // {f19f064d-082c-4e27-bc73-6882a1bb8e4c},0): 8 bájtos fejléc (VT_BLOB) + WAVEFORMATEXTENSIBLE.
    const Case cases[] = {
        { "48 kHz, 24 bit 32 bites tárolóban, sztereó, PCM (fejléccel)",
          "41 00 00 00 01 00 00 00 fe ff 02 00 80 bb 00 00 00 dc 05 00 08 00 20 00 16 00 18 00 03 00 00 00 "
          "01 00 00 00 00 00 10 00 80 00 00 aa 00 38 9b 71",
          "48000 Hz, 24 bit (32-bit container), 2 ch, PCM" },
        { "44,1 kHz, 16 bit, sztereó, PCM (fejléccel)",
          "41 00 00 00 01 00 00 00 fe ff 02 00 44 ac 00 00 10 b1 02 00 04 00 10 00 16 00 10 00 03 00 00 00 "
          "01 00 00 00 00 00 10 00 80 00 00 aa 00 38 9b 71",
          "44100 Hz, 16 bit, 2 ch, PCM" },
        { "48 kHz, 24 bit 24 bites tárolóban (fejléccel)",
          "41 00 00 00 01 00 00 00 fe ff 02 00 80 bb 00 00 00 65 04 00 06 00 18 00 16 00 18 00 03 00 00 00 "
          "01 00 00 00 00 00 10 00 80 00 00 aa 00 38 9b 71",
          "48000 Hz, 24 bit, 2 ch, PCM" },
        { "8 kHz, 16 bit, monó (fejléccel)",
          "41 00 00 00 01 00 00 00 fe ff 01 00 40 1f 00 00 80 3e 00 00 02 00 10 00 16 00 10 00 04 00 00 00 "
          "01 00 00 00 00 00 10 00 80 00 00 aa 00 38 9b 71",
          "8000 Hz, 16 bit, 1 ch, PCM" },
        { "192 kHz, 32 bit lebegőpontos, WAVEFORMATEXTENSIBLE fejléc nélkül",
          "fe ff 02 00 00 ee 02 00 00 70 17 00 08 00 20 00 16 00 20 00 03 00 00 00 "
          "03 00 00 00 00 00 10 00 80 00 00 aa 00 38 9b 71",
          "192000 Hz, 32 bit, 2 ch, float" },
        { "96 kHz, 32 bit lebegőpontos, egyszerű WAVEFORMATEX (18 bájt)",
          "03 00 02 00 00 77 01 00 00 b8 0b 00 08 00 20 00 00 00",
          "96000 Hz, 32 bit, 2 ch, float" },
        { "csonka bájtsor (a fejléc után 12 bájt)",
          "41 00 00 00 01 00 00 00 fe ff 02 00 80 bb 00 00 00 dc 05 00",
          "" },
        { "csonka WAVEFORMATEXTENSIBLE (a kiterjesztés hiányzik)",
          "41 00 00 00 01 00 00 00 fe ff 02 00 80 bb 00 00 00 dc 05 00 08 00 20 00 16 00 18 00",
          "" },
        { "0 csatorna: érvénytelen",
          "01 00 00 00 80 bb 00 00 00 00 00 00 04 00 10 00 00 00",
          "" },
    };
    for (const Case& c : cases) {
        const std::vector<uint8_t> b = hexBytes(c.hex);
        const std::string got = aq::devfmt::describe(b.data(), b.size());
        CHECK(got == c.want, "%s: \"%s\" (elvárt: \"%s\")", c.name, got.c_str(), c.want);
    }
    CHECK(aq::devfmt::describe(nullptr, 0).empty(), "üres bemenet: üres szöveg");
}

int main(int argc, char** argv) {
    if (argc >= 5 && std::strcmp(argv[1], "--child") == 0) return runChild(argv[2], argv[3], argv[4]);
    uint32_t seed = static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    if (const char* s = std::getenv("AQ_CHECK_SEED"); s && *s) seed = static_cast<uint32_t>(std::strtoul(s, nullptr, 10));
    std::printf("test_watchdog (AQ_CHECK_SEED=%u)\n", seed);
    testWatchdogBasic();
    testBridgeStall();
    testScrub(seed);
    testAbnormalEnd(argv[0]);
    testSetting();
    testScrubKernelVersion();
    testDeviceFormat();
    std::printf("\n%s (%d hiba)\n", g_fail == 0 ? "MINDEN TESZT SIKERES" : "SIKERTELEN", g_fail);
    return g_fail == 0 ? 0 : 1;
}
