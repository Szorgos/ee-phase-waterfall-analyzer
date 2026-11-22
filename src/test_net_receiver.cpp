/**
 * Hálózati vevő (net_receiver.hpp) egységtesztje, Linux: a név tisztítása, a kimeneti modul választása, a
 * beállítófájl, a felügyelő életciklusa hamis gyermekfolyamattal (indítás, összeomlás utáni újraindítás, leállítás,
 * SIGTERM-et figyelmen kívül hagyó gyerek, gyors hibák → „hiba”, a motor halálakor a gyerek is leáll), végül a
 * Receiver egy hamis shairport-sync szkripttel (be / ki, névváltás, hiányzó program), valamint az audio.json mezői.
 */
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "audio_options.hpp"
#include "net_receiver.hpp"

namespace fs = std::filesystem;
using namespace aq::netrecv;
using namespace std::chrono_literals;

static int g_fail = 0;
static void check(bool cond, const std::string& what) {
    std::printf("  [%s] %s\n", cond ? "OK" : "HIBA", what.c_str());
    if (!cond) ++g_fail;
}

static fs::path makeTempDir() {
    const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path d = fs::temp_directory_path() / ("aq-test-netrecv-" + std::to_string(getpid()) + "-" + std::to_string(ts));
    fs::create_directories(d);
    return d;
}

static std::string readFile(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static pid_t readPid(const fs::path& p) {
    const std::string s = readFile(p);
    return s.empty() ? -1 : static_cast<pid_t>(std::atol(s.c_str()));
}

/** Él-e a folyamat (a zombi halottnak számít). */
static bool alive(pid_t pid) {
    if (pid <= 0 || kill(pid, 0) != 0) return false;
    std::ifstream st("/proc/" + std::to_string(pid) + "/stat");
    std::string all;
    std::getline(st, all);
    const size_t rp = all.rfind(')');
    return !(rp != std::string::npos && rp + 2 < all.size() && all[rp + 2] == 'Z');
}

template <class F>
static bool waitFor(F cond, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return cond();
}

static SupervisorTiming fastTiming() {
    SupervisorTiming t;
    t.backoffStartMs = 100;
    t.backoffMaxMs = 400;
    t.runningAfterMs = 200;
    t.stableAfterMs = 5000;
    t.failLimit = 3;
    t.termWaitMs = 500;
    t.pollMs = 20;
    return t;
}

static void writeScript(const fs::path& p, const std::string& body) {
    std::ofstream f(p);
    f << "#!/bin/sh\n" << body;
    f.close();
    chmod(p.c_str(), 0755);
}

int main() {
    const fs::path dir = makeTempDir();

    std::printf("[1] Név tisztítása\n");
    check(sanitizeName("  Nappali  ") == "Nappali", "szélső szóközök levágva");
    check(sanitizeName("a\"b\\c$d%e\nf") == "abcdef", "idézőjel, visszaper, $, %, vezérlőkarakter kiszűrve");
    check(sanitizeName("") .empty() && effectiveName("  ") == "Aqulify", "üres név: alapnév");
    {
        std::string longName(49, 'x');
        longName += "é";   // 2 bájt: az 50. bájt határán
        const std::string s = sanitizeName(longName);
        check(s.size() == 49 && s == std::string(49, 'x'), "50 bájtra vágva, UTF-8 karakter közepén nem vág");
        check(sanitizeName("Hálószoba") == "Hálószoba", "ékezetes név megmarad");
    }

    std::printf("[2] Kimeneti modul választása a -h szövegéből\n");
    const std::string help4 = "Usage: shairport-sync [options...]\n\nAvailable audio backends:\n    alsa (default)\n"
                              "    pipewire\n    pulseaudio\n    pipe\n    stdout\n\nSettings and options for the audio backend \"alsa\":\n"
                              "    -d output-device    set the output device, default is \"default\".\n";
    check(pickBackend(help4) == "pipewire", "4.x: PipeWire az első választás");
    check(pickBackend("Available audio backends:\n    alsa (default)\n    pa\n    pipe\n\n") == "pa", "3.x: PulseAudio (\"pa\")");
    check(pickBackend("Available audio backends:\n    alsa (default)\n    stdout\n") == "alsa", "csak ALSA");
    check(pickBackend("Usage: valami\n    pipewire\n").empty(), "lista nélkül: üres (a program alapértelmezése)");
    check(pickBackend("Available audio backends:\n    pipe\n\n    -d pipewire is mentioned later\n").empty(),
          "a lista utáni szöveg nem számít");

    std::printf("[3] Beállítófájl és parancssor\n");
    {
        const std::string c = buildConfig("pipewire", "aqulify_sink");
        check(c.find("output_backend = \"pipewire\";") != std::string::npos, "output_backend beírva");
        check(c.find("sink_target = \"aqulify_sink\";") != std::string::npos, "PipeWire: az Aqulify eszköz a cél");
        check(c.find("sink = \"aqulify_sink\";") != std::string::npos, "PulseAudio: az Aqulify eszköz a cél");
        check(c.find("port = 7110;") != std::string::npos && c.find("udp_port_base = 6110;") != std::string::npos,
              "saját portok (nem ütközik rendszerszintű vevővel)");
        check(buildConfig("", "aqulify_sink").find("output_backend") == std::string::npos, "ismeretlen modul: nincs output_backend");
        const auto av = buildArgv("/usr/bin/shairport-sync", "/tmp/x.conf", "Nap\"pali");
        check(av.size() == 5 && av[1] == "-c" && av[2] == "/tmp/x.conf" && av[3] == "-a" && av[4] == "Nappali",
              "parancssor: -c <fájl> -a <tisztított név>");
        const auto env = buildEnv("aqulify_sink");
        check(env.size() == 2 && env[0] == "PIPEWIRE_NODE=aqulify_sink" && env[1] == "PULSE_SINK=aqulify_sink",
              "környezet: PIPEWIRE_NODE és PULSE_SINK");
    }

    std::printf("[4] Felügyelő: indítás, összeomlás utáni újraindítás, leállítás\n");
    {
        const fs::path pidFile = dir / "pid4";
        Supervisor s(fastTiming());
        std::string lines;
        std::mutex lm;
        s.setLineHandler([&](const std::string& l) { std::lock_guard<std::mutex> lk(lm); lines += l + "\n"; });
        s.start({ "/bin/sh", "-c", "echo $$ > \"$PIDF\"; echo \"hello $AQ_T\" >&2; exec sleep 30" },
                { "PIDF=" + pidFile.string(), "AQ_T=env-ok" });
        check(waitFor([&] { return readPid(pidFile) > 0; }, 3000ms), "a gyerek elindult");
        const pid_t p1 = readPid(pidFile);
        check(waitFor([&] { return s.state() == State::Running; }, 2000ms), "állapot: fut");
        check(waitFor([&] { std::lock_guard<std::mutex> lk(lm); return lines.find("hello env-ok") != std::string::npos; }, 2000ms),
              "a gyerek kimenete soronként a naplóba, a környezeti változó átjut");
        check(s.pid() == p1, "a felügyelő a gyerek PID-jét tudja");
        fs::remove(pidFile);
        kill(p1, SIGKILL);   // „összeomlás”
        check(waitFor([&] { return readPid(pidFile) > 0 && readPid(pidFile) != p1; }, 3000ms), "összeomlás után újraindult");
        const pid_t p2 = readPid(pidFile);
        check(!alive(p1) && alive(p2), "a régi folyamat nincs meg, az új fut");
        check(s.restarts() == 1, "egy újraindítás számolva");
        {
            std::lock_guard<std::mutex> lk(lm);
            check(lines.find("(exited: signal 9)") != std::string::npos, "a kilépés oka a naplóban");
        }
        const auto t0 = std::chrono::steady_clock::now();
        s.stop();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        check(!alive(p2), "leállítás után a gyerek nem fut");
        check(ms < 1500, "a leállítás gyors (" + std::to_string(ms) + " ms)");
        check(s.state() == State::Off && s.pid() == -1, "állapot: ki");
        std::this_thread::sleep_for(300ms);
        check(!fs::exists(pidFile) || readPid(pidFile) == p2, "leállítás után nem indul újra");
    }

    std::printf("[5] SIGTERM-et figyelmen kívül hagyó gyerek: SIGKILL a türelmi idő után\n");
    {
        const fs::path pidFile = dir / "pid5";
        Supervisor s(fastTiming());
        s.start({ "/bin/sh", "-c", "trap '' TERM; echo $$ > \"$PIDF\"; while :; do sleep 1; done" }, { "PIDF=" + pidFile.string() });
        check(waitFor([&] { return readPid(pidFile) > 0; }, 3000ms), "elindult");
        const pid_t p = readPid(pidFile);
        std::this_thread::sleep_for(100ms);
        const auto t0 = std::chrono::steady_clock::now();
        s.stop();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        check(!alive(p), "a makacs gyerek sem marad meg");
        check(ms >= 400 && ms < 2500, "a SIGKILL a türelmi idő után jön (" + std::to_string(ms) + " ms)");
    }

    std::printf("[6] Gyors hibák: növekvő várakozás, majd „hiba” állapot (az újrapróba folytatódik)\n");
    {
        const fs::path countFile = dir / "count6";
        Supervisor s(fastTiming());
        s.start({ "/bin/sh", "-c", "echo x >> \"$CF\"; echo 'fatal: bind failed' >&2; exit 3" }, { "CF=" + countFile.string() });
        check(waitFor([&] { return s.state() == State::Failed; }, 3000ms), "3 gyors hiba után: hiba");
        check(s.lastLine() == "fatal: bind failed", "az utolsó hibaüzenet lekérdezhető");
        const auto lines = [&] { std::ifstream f(countFile); int n = 0; std::string l; while (std::getline(f, l)) ++n; return n; };
        const int n1 = lines();
        std::this_thread::sleep_for(900ms);
        const int n2 = lines();
        check(n2 > n1 && n2 - n1 <= 4, "a legnagyobb várakozással tovább próbál (" + std::to_string(n2 - n1) + " indítás 0,9 s alatt)");
        s.stop();
        check(s.state() == State::Off, "leállítva");
    }

    std::printf("[7] Nem létező program: a felügyelő nem omlik össze\n");
    {
        Supervisor s(fastTiming());
        s.start({ (dir / "nincs-ilyen").string() });
        check(waitFor([&] { return s.state() == State::Failed; }, 3000ms), "végül hiba állapot");
        s.stop();
    }

    std::printf("[8] A motor halálakor a vevő is leáll (PR_SET_PDEATHSIG)\n");
    {
        const fs::path pidFile = dir / "pid8";
        const pid_t engine = fork();
        if (engine == 0) {
            Supervisor s(fastTiming());
            s.start({ "/bin/sh", "-c", "echo $$ > \"$PIDF\"; exec sleep 30" }, { "PIDF=" + pidFile.string() });
            for (int i = 0; i < 150 && readPid(pidFile) <= 0; ++i) std::this_thread::sleep_for(20ms);
            std::this_thread::sleep_for(100ms);
            _exit(0);   // „összeomlás”: nincs stop(), nincs destruktor
        }
        int st = 0;
        waitpid(engine, &st, 0);
        const pid_t child = readPid(pidFile);
        check(child > 0, "a vevő elindult a motor alatt");
        check(waitFor([&] { return !alive(child); }, 3000ms), "a motor kilépése után a vevő nem marad gazdátlanul");
    }

    std::printf("[9] Receiver hamis shairport-sync szkripttel\n");
    {
        const fs::path fake = dir / "shairport-sync";
        const fs::path argsFile = dir / "args9";
        const fs::path envFile = dir / "env9";
        writeScript(fake,
                    "if [ \"$1\" = \"-h\" ]; then printf 'Usage\\n\\nAvailable audio backends:\\n    alsa (default)\\n    pipewire\\n\\n'; exit 0; fi\n"
                    "echo \"$@\" > \"" + argsFile.string() + "\"\n"
                    "echo \"$PIPEWIRE_NODE $PULSE_SINK\" > \"" + envFile.string() + "\"\n"
                    "exec sleep 30\n");
        Receiver::Config rc;
        rc.program = fake.string();
        rc.configPath = (dir / "netrecv.conf").string();
        rc.timing = fastTiming();
        Receiver r(rc);
        check(r.state() == State::Off, "alapból ki");
        r.apply(true, "Nappali");
        check(waitFor([&] { return fs::exists(argsFile) && !readFile(argsFile).empty(); }, 3000ms), "bekapcsolva elindult");
        const std::string args = readFile(argsFile);
        check(args.find("-c " + rc.configPath) != std::string::npos && args.find("-a Nappali") != std::string::npos,
              "parancssor: a beállítófájl és a név");
        check(waitFor([&] { return readFile(envFile).find("aqulify_sink aqulify_sink") != std::string::npos; }, 2000ms),
              "a gyerek környezete az Aqulify eszközre mutat");
        const std::string conf = readFile(rc.configPath);
        check(r.backend() == "pipewire" && conf.find("output_backend = \"pipewire\";") != std::string::npos,
              "a -h szerinti modul a beállítófájlban");
        check(waitFor([&] { return r.state() == State::Running; }, 2000ms), "állapot: fut");
        check(r.statusJson().find("\"state\":\"running\"") != std::string::npos &&
              r.statusJson().find("\"name\":\"Nappali\"") != std::string::npos, "állapot JSON");
        const pid_t p1 = r.pid();
        r.apply(true, "Nappali");
        check(r.pid() == p1, "ugyanaz a beállítás: nem indul újra");
        fs::remove(argsFile);
        r.apply(true, "Konyha");
        check(waitFor([&] { return readFile(argsFile).find("-a Konyha") != std::string::npos; }, 3000ms), "névváltás: újraindul az új névvel");
        check(!alive(p1), "a régi példány leállt");
        const pid_t p2 = r.pid();
        r.apply(false, "Konyha");
        check(r.state() == State::Off && !alive(p2), "kikapcsolva leáll");
        fs::remove(argsFile);
        r.requestApply(true, "Aszinkron");
        check(waitFor([&] { return readFile(argsFile).find("-a Aszinkron") != std::string::npos; }, 3000ms),
              "requestApply: a saját szálán indul");
        r.requestApply(false, "Aszinkron");
        check(waitFor([&] { return r.state() == State::Off && r.pid() == -1; }, 3000ms), "requestApply: kikapcsol");

        Receiver::Config mc = rc;
        mc.program = (dir / "nincs-telepitve").string();
        Receiver m(mc);
        m.apply(true, "");
        check(m.state() == State::Missing && m.statusJson().find("\"state\":\"missing\"") != std::string::npos,
              "nincs telepítve: „missing” állapot");
        check(m.statusJson().find("\"name\":\"Aqulify\"") != std::string::npos, "üres név: alapnév");
    }

    std::printf("[10] audio.json mezők: netRecv, netRecvName\n");
    {
        using namespace ee_analyzer;
        audioopt::AudioOptions o;
        check(!o.netRecv && o.netRecvName.empty(), "alapból ki, üres név");
        o.netRecv = true;
        o.netRecvName = "Nappali \"Pi\"";
        const std::string j = audioopt::toJson(o);
        json::Value v;
        check(json::parse(j, v), "a JSON értelmezhető");
        const audioopt::AudioOptions back = audioopt::fromJson(v);
        check(back.netRecv && back.netRecvName == "Nappali Pi", "visszaolvasva (a név tisztítva)");
        check(back == audioopt::fromJson(v), "összehasonlítás");
        audioopt::AudioOptions d;
        check(!(d == back), "a netRecv különbséget az összehasonlítás látja");
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    std::printf("%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT RENDBEN", g_fail);
    return g_fail ? 1 : 0;
}
