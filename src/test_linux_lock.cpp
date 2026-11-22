/**
 * Linux egyetlen-példány zár (flock) és két telepítés (script vs. csomag) felismerésének egységtesztje.
 * Ellenőrzi: 2. példány elutasítása + futó PID/exe kiolvasása; SIGKILL összeomlás után a lemezen maradt
 * lockfájl nem akadályozza az újraindulást; két telepítés felismerése mindkét irányban.
 */
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "linux_instance_lock.hpp"

namespace fs = std::filesystem;

static fs::path makeTempDir(const std::string& tag) {
    const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path d = fs::temp_directory_path() / ("aq-test-lock-" + tag + "-" + std::to_string(ts));
    fs::create_directories(d);
    return d;
}

int main() {
    using namespace ee_analyzer::linux_inst;
    int fail = 0;
    auto check = [&](bool cond, const char* what) {
        std::printf("  [%s] %s\n", cond ? "OK" : "HIBA", what);
        if (!cond) ++fail;
    };

    const fs::path dir = makeTempDir("inst");
    const std::string lockPath = (dir / "engine.lock").string();

    std::printf("[1] Egyetlen-példány zár (flock): 2. példány elutasítva, futó PID/exe olvasható\n");
    {
        InstanceLock l1;
        check(l1.acquire(lockPath, "/usr/bin/aqulify-engine", "2.8.3"), "1. példány megszerzi a zárat");

        // Másik folyamat (fork) megpróbálja megszerezni ugyanazt a zárat
        int pfd[2];
        check(::pipe(pfd) == 0, "pipe létrehozva");
        const pid_t child = ::fork();
        if (child == 0) {
            ::close(pfd[0]);
            InstanceLock l2;
            ExistingInfo ex;
            const bool got = l2.acquire(lockPath, "/home/u/.local/bin/aqulify-engine", "2.7.0", &ex);
            const std::string msg = std::string(got ? "GOT" : "BUSY") + "|" + std::to_string(ex.pid) + "|" + ex.exe + "|" + ex.version;
            const ssize_t r = ::write(pfd[1], msg.data(), msg.size());
            (void)r;
            ::close(pfd[1]);
            ::_exit(0);
        }
        ::close(pfd[1]);
        char buf[512]{};
        const ssize_t n = ::read(pfd[0], buf, sizeof(buf) - 1);
        ::close(pfd[0]);
        int st = 0;
        ::waitpid(child, &st, 0);
        const std::string res(buf, n > 0 ? static_cast<size_t>(n) : 0);
        check(res.rfind("BUSY|", 0) == 0, "2. példány nem szerzi meg a zárat");
        check(res.find(std::to_string(static_cast<long>(::getpid()))) != std::string::npos,
              "2. példány kiolvassa a futó PID-et");
        check(res.find("/usr/bin/aqulify-engine|2.8.3") != std::string::npos,
              "2. példány kiolvassa a futó exe útvonalát és verzióját");
    }

    std::printf("[2] Összeomlás (SIGKILL) után beragadt lockfájl nem akadályozza az indulást\n");
    {
        int pfd[2];
        check(::pipe(pfd) == 0, "pipe2 létrehozva");
        const pid_t child = ::fork();
        if (child == 0) {
            ::close(pfd[0]);
            InstanceLock lc;
            const bool got = lc.acquire(lockPath, "/usr/bin/aqulify-engine", "2.8.3");
            const char c = got ? '1' : '0';
            const ssize_t r = ::write(pfd[1], &c, 1);
            (void)r;
            ::close(pfd[1]);
            for (;;) ::pause(); // várja a SIGKILL-t
        }
        ::close(pfd[1]);
        char c = '0';
        const ssize_t r = ::read(pfd[0], &c, 1);
        (void)r;
        ::close(pfd[0]);
        check(c == '1', "gyermek megszerezte a zárat");
        ::kill(child, SIGKILL);
        int st = 0;
        ::waitpid(child, &st, 0);
        check(fs::exists(lockPath), "SIGKILL után a lockfájl a lemezen maradt");

        InstanceLock afterCrash;
        check(afterCrash.acquire(lockPath, "/usr/bin/aqulify-engine", "2.8.3"),
              "SIGKILL után a lemezen maradt lockfájl ellenére az új példány azonnal megszerzi a zárat");
    }

    std::printf("[3] Két telepítés (script vs. csomag) felismerése\n");
    {
        const fs::path fakeHome = dir / "home";
        const fs::path fakeCfg = fakeHome / ".config";
        const fs::path fakeSys = dir / "usr_bin_aqulify";
        fs::create_directories(fakeHome / ".local/bin");
        fs::create_directories(fakeCfg / "systemd/user");

        // Csak script: nincs ütközés
        { std::ofstream f(fakeHome / ".local/bin/aqulify-engine"); f << "x"; }
        auto d0 = detectDualInstall("script", (fakeHome / ".local/bin/aqulify-engine").string(),
                                    fakeHome.string(), fakeCfg.string(), fakeSys.string());
        check(!d0.detected, "csak script telepítés: nincs ütközés");

        // Mindkettő jelen van, script fut
        { std::ofstream f(fakeSys); f << "x"; }
        auto d1 = detectDualInstall("script", (fakeHome / ".local/bin/aqulify-engine").string(),
                                    fakeHome.string(), fakeCfg.string(), fakeSys.string());
        check(d1.detected && d1.runningKind == "script" && d1.otherKind == "pkg" && !d1.removeCmd.empty(),
              "script fut, de /usr/bin csomag is létezik -> dualInstall jelezve");

        // Mindkettő jelen van, COPR csomag fut
        { std::ofstream f(fakeCfg / "systemd/user/aqulify.service"); f << "x"; }
        auto d2 = detectDualInstall("copr", fakeSys.string(), fakeHome.string(), fakeCfg.string(), fakeSys.string());
        check(d2.detected && d2.runningKind == "copr" && d2.otherKind == "script" &&
                  d2.statusJsonField().find("\"dualInstall\"") != std::string::npos,
              "copr csomag fut, de ~/.config/systemd/user/aqulify.service is létezik -> dualInstall JSON");

        // Csak csomag: nincs ütközés
        fs::remove(fakeHome / ".local/bin/aqulify-engine");
        fs::remove(fakeCfg / "systemd/user/aqulify.service");
        auto d3 = detectDualInstall("copr", fakeSys.string(), fakeHome.string(), fakeCfg.string(), fakeSys.string());
        check(!d3.detected, "csak csomag telepítés: nincs ütközés");

        // [4] Régi, zár nélküli motor (port 47810 foglalt vagy dupla PipeWire aqulify_sink)
        auto o0 = detectOtherRunningEngine(false, false, "copr", fakeSys.string());
        check(!o0.detected, "szabad 47810 és nincs dupla sink: nincs riasztás");
        auto o1 = detectOtherRunningEngine(true, false, "copr", fakeSys.string());
        check(o1.detected && o1.reason == "other_running" &&
                  o1.statusJsonField().find("\"reason\":\"other_running\"") != std::string::npos &&
                  !o1.removeCmd.empty(),
              "foglalt 47810-es port -> other_running JSON + removeCmd");
        auto o2 = detectOtherRunningEngine(false, true, "script", "/home/u/.local/bin/aqulify-engine");
        check(o2.detected && o2.reason == "other_running" && o2.otherPath == "aqulify_sink",
              "dupla PipeWire aqulify_sink -> other_running jelzés");
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    std::printf("\n%s (%d hiba)\n", fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", fail);
    return fail ? 1 : 0;
}
