#pragma once
/**
 * @file net_receiver.hpp
 * @brief Hálózati vevő (AirPlay-kompatibilis lejátszás telefonról / számítógépről), Linux és Raspberry Pi.
 *
 * A vevő a rendszer `shairport-sync` programja (MIT licenc, külön csomag), amelyet a motor gyermekfolyamatként indít
 * és felügyel. A hang a PipeWire „Aqulify” eszközére (aqulify_sink) megy, így a motor élő útja ugyanúgy EQ-zza, mint
 * bármely más program hangját: a motorban nincs új hangút. A beállítás (be / ki, kijelzett név) az audio.json része.
 *
 * Részek:
 *  - tiszta függvények (tesztelhetők, platformfüggetlenek): név tisztítása, a kimeneti modul választása a
 *    `shairport-sync -h` kimenetéből, a beállítófájl szövege;
 *  - Supervisor (csak POSIX): egy gyermekfolyamat indítása, a kimenete soronként a naplóba, összeomláskor
 *    újraindítás növekvő várakozással, leállításkor SIGTERM, majd SIGKILL. Linuxon a gyerek a motor halálakor is
 *    leáll (PR_SET_PDEATHSIG), így összeomlás után nem marad gazdátlan vevő;
 *  - Receiver: a fentiek összerakása (program keresése, beállítófájl írása, indítás / leállítás / újraindítás).
 *
 * Őszinte korlát: a hálózati lejátszás kb. CD-minőségű (44,1 kHz / 16 bit, veszteségmentes), nem bitpontos és nem hi-res.
 */
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
extern char** environ;
#endif

namespace aq::netrecv {

/** A kijelzett név legnagyobb hossza bájtban (a shairport-sync legfeljebb 50 karaktert fogad el). */
inline constexpr size_t kMaxNameBytes = 50;
/** Az alapértelmezett kijelzett név (üres beállításnál). */
inline constexpr const char* kDefaultName = "Aqulify";
/**
 * Saját portok: egy esetleg futó rendszerszintű shairport-sync szolgáltatás (5000 / 7000, UDP 6001–6010) mellett is
 * elindul. A telefon a portot az mDNS-hirdetésből tudja, ezért nem kell a szabványos.
 */
inline constexpr int kRtspPort = 7110;
inline constexpr int kUdpPortBase = 6110;

/**
 * A kijelzett név tisztítása: vezérlőkarakter, idézőjel, visszaper, `$` (a beállítófájl ${VÁLTOZÓ} cseréje) és `%`
 * (a program saját helyettesítései) nélkül, a széleken szóköz nélkül, legfeljebb kMaxNameBytes bájt (UTF-8 határon
 * vágva). Üres eredmény: üres (a hívó az alapnevet használja).
 */
inline std::string sanitizeName(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\' || c == '$' || c == '%') continue;
        out.push_back(static_cast<char>(c));
    }
    const size_t b = out.find_first_not_of(' ');
    if (b == std::string::npos) return {};
    out = out.substr(b, out.find_last_not_of(' ') - b + 1);
    if (out.size() > kMaxNameBytes) {
        size_t cut = kMaxNameBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;   // UTF-8 folytató bájt
        out.resize(cut);
        while (!out.empty() && out.back() == ' ') out.pop_back();
    }
    return out;
}

/** A használt név: a tisztított beállítás, üresen az alapnév. */
inline std::string effectiveName(const std::string& configured) {
    const std::string s = sanitizeName(configured);
    return s.empty() ? std::string(kDefaultName) : s;
}

/**
 * A kimeneti modul választása a `shairport-sync -h` szövegéből ("Available audio backends:" utáni sorok, pl.
 * "    pipewire (default)"). Sorrend: PipeWire (4.3-tól "pipewire"), PulseAudio ("pulseaudio", a 3.x-ben "pa"; a
 * PipeWire saját PulseAudio-kiszolgálóján át), ALSA. Ha egyik sincs a listában (vagy nincs lista), üres: a program
 * a saját alapértelmezését használja (a környezeti változók akkor is az Aqulify eszközre irányítják).
 */
inline std::string pickBackend(const std::string& helpText) {
    std::vector<std::string> names;
    std::istringstream in(helpText);
    std::string line;
    bool inList = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find("Available audio backends") != std::string::npos) { inList = true; continue; }
        if (!inList) continue;
        const size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) break;            // üres sor: vége a listának
        if (b == 0) break;                            // behúzás nélküli sor: már más szakasz
        std::string name;
        for (size_t i = b; i < line.size() && (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '_'); ++i)
            name.push_back(line[i]);
        if (!name.empty()) names.push_back(name);
    }
    for (const char* want : { "pipewire", "pw", "pulseaudio", "pa", "alsa" })
        if (std::find(names.begin(), names.end(), want) != names.end()) return want;
    return {};
}

/**
 * A beállítófájl (libconfig) szövege. A PipeWire-, PulseAudio- és ALSA-szakasz mind benne van (a program csak a
 * választott modulét olvassa), mindegyik az Aqulify eszközre mutat. A nevet a parancssor adja (-a), nem ez a fájl.
 */
inline std::string buildConfig(const std::string& backend, const std::string& sink, int port = kRtspPort,
                               int udpBase = kUdpPortBase) {
    std::string s;
    s += "// Az Aqulify motor írja minden indításkor; a kézi módosítás elvész.\n";
    s += "general = {\n";
    if (!backend.empty()) s += "  output_backend = \"" + backend + "\";\n";
    s += "  port = " + std::to_string(port) + ";\n";
    s += "  udp_port_base = " + std::to_string(udpBase) + ";\n";
    s += "  udp_port_range = 10;\n";
    // a felhasználó munkamenetében fut: a rendszerbuszon nem kaphat nevet
    s += "  dbus_service_bus = \"session\";\n";
    s += "  mpris_service_bus = \"session\";\n";
    s += "};\n";
    s += "pipewire = {\n  sink_target = \"" + sink + "\";\n  node_name = \"aqulify_netrecv\";\n"
         "  application_name = \"Aqulify network receiver\";\n};\n";
    s += "pulseaudio = {\n  sink = \"" + sink + "\";\n  application_name = \"Aqulify network receiver\";\n};\n";
    s += "pa = {\n  sink = \"" + sink + "\";\n  application_name = \"Aqulify network receiver\";\n};\n";
    s += "alsa = {\n  output_device = \"default\";\n};\n";
    return s;
}

/** A gyermekfolyamat parancssora: program, beállítófájl, név. */
inline std::vector<std::string> buildArgv(const std::string& program, const std::string& configPath,
                                          const std::string& name) {
    return { program, "-c", configPath, "-a", effectiveName(name) };
}

/** A gyerek környezete: a PipeWire- és PulseAudio-ügyfelek alapból az Aqulify eszközre szóljanak. */
inline std::vector<std::string> buildEnv(const std::string& sink) {
    return { "PIPEWIRE_NODE=" + sink, "PULSE_SINK=" + sink };
}

enum class State { Off, Missing, Starting, Running, Restarting, Failed };

inline const char* stateName(State s) {
    switch (s) {
    case State::Missing: return "missing";
    case State::Starting: return "starting";
    case State::Running: return "running";
    case State::Restarting: return "restarting";
    case State::Failed: return "failed";
    default: return "off";
    }
}

#if !defined(_WIN32)

/** Program keresése: abszolút / relatív útvonal közvetlenül, különben a PATH-ban. Üres: nincs. */
inline std::string findProgram(const std::string& prog) {
    if (prog.empty()) return {};
    if (prog.find('/') != std::string::npos) return access(prog.c_str(), X_OK) == 0 ? prog : std::string();
    const char* path = std::getenv("PATH");
    const std::string p = path && *path ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= p.size()) {
        const size_t end = p.find(':', start);
        const std::string dir = p.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            const std::string full = dir + "/" + prog;
            struct stat st{};
            if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(full.c_str(), X_OK) == 0) return full;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

/** Egy program rövid futtatása, a kimenete (stdout + stderr) legfeljebb maxBytes bájtig, legfeljebb timeoutMs ideig. */
inline std::string captureOutput(const std::vector<std::string>& argv, int timeoutMs = 3000, size_t maxBytes = 65536) {
    if (argv.empty()) return {};
    int fds[2];
    if (pipe(fds) != 0) return {};
    std::vector<char*> av;
    for (const auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return {}; }
    if (pid == 0) {
        const int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        execv(av[0], av.data());
        _exit(127);
    }
    close(fds[1]);
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    char buf[4096];
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) break;
        pollfd pf{ fds[0], POLLIN, 0 };
        const int r = poll(&pf, 1, static_cast<int>(left));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        const ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n <= 0) break;
        if (out.size() < maxBytes) out.append(buf, static_cast<size_t>(std::min<ssize_t>(n, static_cast<ssize_t>(maxBytes - out.size()))));
    }
    close(fds[0]);
    if (waitpid(pid, nullptr, WNOHANG) == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
    }
    return out;
}

/** A felügyelet időzítései (a tesztek rövidebbre veszik). */
struct SupervisorTiming {
    int backoffStartMs = 1000;   // az első újraindítás előtti várakozás; hibánként kétszereződik
    int backoffMaxMs = 30000;    // a leghosszabb várakozás
    int runningAfterMs = 1500;   // ennyi futás után számít „fut” állapotúnak
    int stableAfterMs = 60000;   // ennyi futás után a hibaszámláló nullázódik
    int failLimit = 5;           // ennyi gyors hiba után „hiba” állapot (az újrapróba ritkán folytatódik)
    int termWaitMs = 2000;       // SIGTERM után ennyit vár a SIGKILL előtt
    int pollMs = 100;
};

/**
 * Egy gyermekfolyamat felügyelete egy saját szálon. start(): (újra)indítja a megadott parancssorral; stop(): leállítja
 * és megvárja. A gyerek kimenete soronként a lineHandler-hez kerül (a motor naplója), az utolsó sor lekérdezhető.
 */
class Supervisor {
public:
    using LineFn = std::function<void(const std::string&)>;
    explicit Supervisor(SupervisorTiming t = {}) : t_(t) {}
    ~Supervisor() { stop(); }
    Supervisor(const Supervisor&) = delete;
    Supervisor& operator=(const Supervisor&) = delete;

    void setLineHandler(LineFn fn) {
        std::lock_guard<std::mutex> lk(mu_);
        onLine_ = std::move(fn);
    }

    /** Indítás (ha már fut, előbb leáll). env: "KULCS=érték" bejegyzések, a motor környezetét felülírják. */
    void start(std::vector<std::string> argv, std::vector<std::string> env = {}) {
        stop();
        std::lock_guard<std::mutex> lk(mu_);
        argv_ = std::move(argv);
        env_ = std::move(env);
        quit_ = false;
        failures_ = 0;
        restarts_ = 0;
        lastLine_.clear();
        state_ = State::Starting;
        thread_ = std::thread([this] { loop(); });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            quit_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        std::lock_guard<std::mutex> lk(mu_);
        state_ = State::Off;
        pid_ = -1;
    }

    State state() const { std::lock_guard<std::mutex> lk(mu_); return state_; }
    int restarts() const { std::lock_guard<std::mutex> lk(mu_); return restarts_; }
    pid_t pid() const { std::lock_guard<std::mutex> lk(mu_); return pid_; }
    std::string lastLine() const { std::lock_guard<std::mutex> lk(mu_); return lastLine_; }

private:
    // A gyerek indítása (fork + exec; a fork és az exec között csak aszinkron-jelbiztos hívások). -1: hiba.
    pid_t spawn(int& outFd) {
        std::vector<std::string> envStore;
        for (char** e = environ; e && *e; ++e) {
            const std::string kv = *e;
            const std::string key = kv.substr(0, kv.find('='));
            bool overridden = false;
            for (const auto& o : env_)
                if (o.compare(0, key.size() + 1, key + "=") == 0) { overridden = true; break; }
            if (!overridden) envStore.push_back(kv);
        }
        for (const auto& o : env_) envStore.push_back(o);
        std::vector<char*> av, ev;
        for (auto& a : argv_) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        for (auto& e : envStore) ev.push_back(const_cast<char*>(e.c_str()));
        ev.push_back(nullptr);
        if (av.size() < 2) return -1;

        int fds[2];
#if defined(__linux__)
        if (pipe2(fds, O_CLOEXEC) != 0) return -1;   // más szál fork-ja se örökölje (a dup2 a 1/2-n törli)
#else
        if (pipe(fds) != 0) return -1;
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#endif
        long maxFd = sysconf(_SC_OPEN_MAX);
        if (maxFd < 0 || maxFd > 65536) maxFd = 65536;
        const pid_t parent = getpid();
        const pid_t pid = fork();
        if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
        if (pid == 0) {
            setpgid(0, 0);   // saját folyamatcsoport: a leállítás a gyerek gyerekeit is eléri
#if defined(__linux__)
            prctl(PR_SET_PDEATHSIG, SIGTERM);   // a motor (a felügyelő szál) halálakor a vevő is leáll
            if (getppid() != parent) _exit(0);
#else
            (void)parent;
#endif
            const int nul = open("/dev/null", O_RDONLY);
            if (nul >= 0) dup2(nul, 0);
            dup2(fds[1], 1);
            dup2(fds[1], 2);
            for (long fd = 3; fd < maxFd; ++fd) close(static_cast<int>(fd));   // a motor foglalatai ne öröklődjenek
            signal(SIGPIPE, SIG_DFL);
            execve(av[0], av.data(), ev.data());
            _exit(127);
        }
        close(fds[1]);
        fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
        outFd = fds[0];
        return pid;
    }

    void emitLine(std::string line) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) return;
        if (line.size() > 300) line.resize(300);
        LineFn fn;
        {
            std::lock_guard<std::mutex> lk(mu_);
            lastLine_ = line;
            fn = onLine_;
        }
        if (fn) fn(line);
    }

    void drain(int fd, std::string& partial) {
        char buf[2048];
        for (;;) {
            const ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            partial.append(buf, static_cast<size_t>(n));
            size_t nl;
            while ((nl = partial.find('\n')) != std::string::npos) {
                emitLine(partial.substr(0, nl));
                partial.erase(0, nl + 1);
            }
            if (partial.size() > 4096) { emitLine(partial); partial.clear(); }
        }
    }

    // Leállítás: SIGTERM a csoportnak, termWaitMs után SIGKILL; a gyereket megvárjuk (nincs zombi).
    void terminate(pid_t pid) {
        kill(-pid, SIGTERM);
        kill(pid, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(t_.termWaitMs);
        while (std::chrono::steady_clock::now() < deadline) {
            const pid_t r = waitpid(pid, nullptr, WNOHANG);
            if (r == pid || (r < 0 && errno == ECHILD)) { kill(-pid, SIGKILL); return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
    }

    void loop() {
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (quit_) return;
            }
            int fd = -1;
            const pid_t pid = spawn(fd);
            const auto started = std::chrono::steady_clock::now();
            bool exited = pid < 0;
            int status = 0;
            if (pid > 0) {
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    pid_ = pid;
                    if (state_ != State::Failed) state_ = State::Starting;
                }
                std::string partial;
                for (;;) {
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        if (quit_) break;
                    }
                    pollfd pf{ fd, POLLIN, 0 };
                    if (poll(&pf, 1, t_.pollMs) > 0) drain(fd, partial);
                    const pid_t r = waitpid(pid, &status, WNOHANG);
                    if (r == pid || (r < 0 && errno == ECHILD)) {
                        drain(fd, partial);
                        exited = true;
                        break;
                    }
                    const auto up = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
                    std::lock_guard<std::mutex> lk(mu_);
                    if (up >= t_.runningAfterMs && state_ != State::Running &&
                        (state_ != State::Failed || up >= t_.stableAfterMs))
                        state_ = State::Running;
                    if (up >= t_.stableAfterMs) failures_ = 0;
                }
                if (!partial.empty()) emitLine(partial);
                if (!exited) {   // leállítás kérve
                    terminate(pid);
                    close(fd);
                    return;
                }
                close(fd);
            }
            // A gyerek kilépett (vagy el sem indult): újraindítás növekvő várakozással
            const auto up = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            int waitMs;
            {
                std::lock_guard<std::mutex> lk(mu_);
                pid_ = -1;
                if (quit_) return;
                if (up >= t_.stableAfterMs) failures_ = 0;
                ++failures_;
                ++restarts_;
                state_ = failures_ >= t_.failLimit ? State::Failed : State::Restarting;
                long long w = t_.backoffStartMs;
                for (int i = 1; i < failures_ && w < t_.backoffMaxMs; ++i) w *= 2;
                waitMs = static_cast<int>(std::min<long long>(w, t_.backoffMaxMs));
            }
            if (WIFEXITED(status) || WIFSIGNALED(status) || pid < 0) {
                char msg[96];
                if (pid < 0) std::snprintf(msg, sizeof msg, "(spawn failed)");
                else if (WIFSIGNALED(status)) std::snprintf(msg, sizeof msg, "(exited: signal %d)", WTERMSIG(status));
                else std::snprintf(msg, sizeof msg, "(exited: code %d)", WEXITSTATUS(status));
                LineFn fn;
                { std::lock_guard<std::mutex> lk(mu_); fn = onLine_; }
                if (fn) fn(msg);
            }
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(waitMs), [this] { return quit_; });
            if (quit_) return;
        }
    }

    SupervisorTiming t_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::thread thread_;
    std::vector<std::string> argv_, env_;
    bool quit_ = false;
    State state_ = State::Off;
    int failures_ = 0;
    int restarts_ = 0;
    pid_t pid_ = -1;
    std::string lastLine_;
    LineFn onLine_;
};

/**
 * A hálózati vevő: apply(enabled, name) a beállítás szerint indítja / leállítja / újraindítja (névváltáskor) a
 * shairport-sync-et. A programot a PATH-ban keresi (a tesztek a program útvonalát adják meg).
 */
class Receiver {
public:
    struct Config {
        std::string program = "shairport-sync";
        std::string configPath;              // a beállítófájl helye (pl. ~/.config/aqulify/netrecv.conf)
        std::string sink = "aqulify_sink";
        SupervisorTiming timing{};
    };

    explicit Receiver(Config c) : c_(std::move(c)), sup_(c_.timing) {}
    ~Receiver() {
        {
            std::lock_guard<std::mutex> lk(reqMu_);
            reqQuit_ = true;
        }
        reqCv_.notify_all();
        if (worker_.joinable()) worker_.join();
        sup_.stop();
    }

    void setLineHandler(Supervisor::LineFn fn) { sup_.setLineHandler(std::move(fn)); }

    /**
     * Mint az apply(), de egy saját szálon (a hívó nem vár a leállításra / a -h lekérdezésére; a motor a beállítás
     * zárolása alatt hívja). Több gyors kérésből csak az utolsó számít.
     */
    void requestApply(bool enabled, const std::string& name) {
        std::lock_guard<std::mutex> lk(reqMu_);
        reqEnabled_ = enabled;
        reqName_ = name;
        reqPending_ = true;
        if (!worker_.joinable()) {
            worker_ = std::thread([this] {
                std::unique_lock<std::mutex> l(reqMu_);
                for (;;) {
                    reqCv_.wait(l, [this] { return reqQuit_ || reqPending_; });
                    if (reqQuit_) return;
                    const bool en = reqEnabled_;
                    const std::string nm = reqName_;
                    reqPending_ = false;
                    l.unlock();
                    apply(en, nm);
                    l.lock();
                }
            });
        }
        reqCv_.notify_all();
    }

    /** A beállítás alkalmazása. Ugyanarra a beállításra (és futó vevőre) nem csinál semmit. */
    void apply(bool enabled, const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        const std::string eff = effectiveName(name);
        if (!enabled) {
            sup_.stop();
            enabled_ = false;
            missing_ = false;
            return;
        }
        if (enabled_ && eff == name_ && !missing_) return;
        sup_.stop();
        enabled_ = true;
        name_ = eff;
        const std::string prog = findProgram(c_.program);
        missing_ = prog.empty();
        if (missing_) return;
        if (backendFor_ != prog) {   // a kimeneti modul a programhoz tartozik: egyszer kérdezzük le
            backend_ = pickBackend(captureOutput({ prog, "-h" }));
            backendFor_ = prog;
        }
        std::ofstream f(c_.configPath, std::ios::trunc);
        f << buildConfig(backend_, c_.sink);
        f.close();
        sup_.start(buildArgv(prog, c_.configPath, eff), buildEnv(c_.sink));
    }

    State state() const {
        std::lock_guard<std::mutex> lk(mu_);
        if (!enabled_) return State::Off;
        if (missing_) return State::Missing;
        return sup_.state();
    }
    std::string backend() const { std::lock_guard<std::mutex> lk(mu_); return backend_; }
    std::string lastLine() const { return sup_.lastLine(); }
    int restarts() const { return sup_.restarts(); }
    pid_t pid() const { return sup_.pid(); }

    /** Állapot a felületnek: {"state":"running","name":"Aqulify","backend":"pipewire","detail":"..."}. */
    std::string statusJson() const {
        auto esc = [](const std::string& s) {
            std::string o;
            for (unsigned char ch : s) {
                if (ch == '"' || ch == '\\') { o.push_back('\\'); o.push_back(static_cast<char>(ch)); }
                else if (ch >= 0x20) o.push_back(static_cast<char>(ch));
            }
            return o;
        };
        const State st = state();
        std::string name, be;
        { std::lock_guard<std::mutex> lk(mu_); name = name_; be = backend_; }
        const std::string detail = (st == State::Restarting || st == State::Failed) ? sup_.lastLine() : std::string();
        return std::string("{\"state\":\"") + stateName(st) + "\",\"name\":\"" + esc(name) + "\",\"backend\":\"" +
               esc(be) + "\",\"detail\":\"" + esc(detail) + "\"}";
    }

private:
    Config c_;
    Supervisor sup_;
    mutable std::mutex mu_;
    bool enabled_ = false;
    bool missing_ = false;
    std::string name_;
    std::string backend_, backendFor_;
    std::mutex reqMu_;
    std::condition_variable reqCv_;
    std::thread worker_;
    bool reqQuit_ = false, reqPending_ = false, reqEnabled_ = false;
    std::string reqName_;
};

#endif // !_WIN32

} // namespace aq::netrecv
