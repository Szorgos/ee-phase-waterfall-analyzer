#pragma once
/**
 * @file health_monitor.hpp
 * @brief A motor őrszála és helyi állapotfájlja (beépített hibajelentés, 1. szakasz: H1, R2, H3 alapja).
 *
 * - Őrszál: 100 ms-onként ellenőrzi a szívveréseket (watchdog.hpp). Ha egy szál a küszöbnél (alap 5 s) tovább nem
 *   jelez, a motor naplójába ír (szál, mióta, utolsó címke), és azonnal frissíti az állapotfájlt. A megfigyelt szálak
 *   zárait nem veszi fel; a motorból csak a neki átadott (setSection) másolatokat olvassa.
 * - Állapotfájl: <diag>/state.json, 2 s-onként és változáskor (átmeneti fájl + átnevezés): verzió, platform,
 *   csomagtípus, szívverések, beragadások, CPU / memória, a motor által átadott részek (lejátszási út, audio_opts,
 *   hangkártyák).
 * - „Rendben leállt” jel: a <diag>/running fájl induláskor létrejön, rendes leálláskor (stopClean) törlődik. Ha
 *   induláskor megvan, az előző futás rendellenesen ért véget: az előző állapotból, az összeomlás-jelből és a napló
 *   utolsó ~200 sorából helyi jelentés készül (<diag>/previous-run.json és .txt). Küldés nincs (3. szakasz).
 * - Kikapcsolt beállításnál (errorReports = false) a jelentés csak helyben él, és a következő rendes indításkor
 *   törlődik; bekapcsolva a küldésig (3. szakasz) megmarad.
 * - Az őrszem csak figyel és jelent: nem öl szálat, nem indít újra semmit.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "crash_handler.hpp"
#include "error_report.hpp"
#include "mini_json.hpp"
#include "watchdog.hpp"

#ifdef _WIN32
#include <psapi.h>
#else
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#include <streambuf>
#include <iostream>
#endif

namespace aq::health {

namespace fs = std::filesystem;

#ifndef _WIN32
/**
 * Linux / macOS: a motor szabványos kimenetének (std::cout, std::cerr) tükre a <diag>/engine.log-ba, soronként
 * (szálanként külön sorpufferrel). A napló tetején minden indulás egy kLogBanner sorral kezdődik.
 * Kikapcsolt beállításnál indításkor csak az előző futás marad meg (a régebbiek törlődnek); bekapcsolva 1 MB-onként
 * forog (engine.log.1).
 */
class LogTee : public std::streambuf {
public:
    static bool install(const fs::path& path, bool keepOnlyLastRun, size_t maxBytes = 1024 * 1024) {
        File& f = file();
        std::lock_guard<std::mutex> lk(f.m);
        if (f.fd.load() >= 0) return true;
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (keepOnlyLastRun) {
            std::string data = report::detail::readAll(path, 4 * 1024 * 1024);
            const size_t b = data.rfind(report::kLogBanner);
            if (b != std::string::npos) {
                const size_t ls = data.rfind('\n', b);
                data = data.substr(ls == std::string::npos ? 0 : ls + 1);
            } else {
                data.clear();
            }
            std::ofstream o(path, std::ios::binary | std::ios::trunc);
            o << data;
            fs::remove(path.string() + ".1", ec);
        }
        f.path = path;
        f.maxBytes = maxBytes;
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (fd < 0) return false;
        f.size = static_cast<size_t>(fs::file_size(path, ec));
        f.fd.store(fd);
        // szándékosan soha nem szűnik meg: a cout / cerr a program legvégén (a statikus objektumok után) is írhat
        static LogTee* out = new LogTee(0, std::cout.rdbuf());
        static LogTee* err = new LogTee(1, std::cerr.rdbuf());
        std::cout.rdbuf(out);
        std::cerr.rdbuf(err);
        char ts[64];
        const std::time_t t = std::time(nullptr);
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        const std::string banner = std::string("\n") + report::kLogBanner + ": " + ts + " =====\n";
        writeLocked(f, banner.data(), banner.size());
        return true;
    }

    static bool active() { return file().fd.load() >= 0; }

    /** Közvetlen sor a naplóba (az őrszál: a tükör zárja és a cout nélkül; a szabványos hibakimenetre is). */
    static void writeRaw(const std::string& line) {
        const int fd = file().fd.load();
        if (fd >= 0) { const ssize_t r = ::write(fd, line.data(), line.size()); (void)r; }
        const ssize_t r2 = ::write(2, line.data(), line.size());
        (void)r2;
    }

protected:
    int overflow(int ch) override {
        if (ch == traits_type::eof()) return traits_type::not_eof(ch);
        std::string& l = line();
        l += static_cast<char>(ch);
        if (ch == '\n') emit(l);
        return ch;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::string& l = line();
        for (std::streamsize i = 0; i < n; ++i) {
            l += s[i];
            if (s[i] == '\n') emit(l);
        }
        return n;
    }
    int sync() override { return orig_ ? orig_->pubsync() : 0; }

private:
    struct File {
        std::mutex m;
        std::atomic<int> fd{ -1 };
        fs::path path;
        size_t size = 0;
        size_t maxBytes = 1024 * 1024;
    };
    static File& file() { static File* f = new File(); return *f; }

    LogTee(int idx, std::streambuf* orig) : idx_(idx), orig_(orig) {}
    std::string& line() {
        thread_local std::string lines[2];
        return lines[idx_];
    }
    void emit(std::string& l) {
        File& f = file();
        std::lock_guard<std::mutex> lk(f.m);
        if (orig_) orig_->sputn(l.data(), static_cast<std::streamsize>(l.size()));
        writeLocked(f, l.data(), l.size());
        l.clear();
    }
    static void writeLocked(File& f, const char* d, size_t n) {
        int fd = f.fd.load();
        if (fd < 0) return;
        const ssize_t r = ::write(fd, d, n);
        (void)r;
        f.size += n;
        if (f.size > f.maxBytes) {   // forgatás: engine.log → engine.log.1
            std::error_code ec;
            fs::rename(f.path, f.path.string() + ".1", ec);
            const int nfd = ::open(f.path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
            if (nfd >= 0) {
                f.fd.store(nfd);
                ::close(fd);
                f.size = 0;
            }
        }
    }

    int idx_;
    std::streambuf* orig_;
};
#endif

struct Config {
    fs::path dir;                    // a diagnosztikai mappa (létrejön)
    fs::path logPath;                // a motor naplója (a jelentés utolsó sorai ebből)
    std::string version, platform, package;
    double thresholdSec = 5.0;       // ennyi szívverés nélküli idő után jelez
    int writeIntervalMs = 2000;      // az állapotfájl rendszeres írása
    int checkIntervalMs = 100;       // az őrszál ellenőrzési üteme
    bool enabled = false;            // a felhasználó beállítása (errorReports); alapból ki
    bool crashHandlers = false;      // összeomlás-kezelők (a motorban igen, a tesztekben csak a gyermekfolyamatban)
    aq::wd::Registry* registry = nullptr;   // null: a folyamat közös regisztere
    std::vector<std::string> scrubNames;    // a felhasználó adta nevek (párosított eszközök) a tisztításhoz
};

class Monitor {
public:
    Monitor() = default;
    ~Monitor() { stop(); }
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    bool start(const Config& c) {
        if (thread_.joinable()) return false;
        cfg_ = c;
        reg_ = c.registry ? c.registry : &aq::wd::Registry::global();
        wd_ = std::make_unique<aq::wd::Watchdog>(*reg_, c.thresholdSec);
        enabled_ = c.enabled;
        std::error_code ec;
        fs::create_directories(cfg_.dir, ec);
        if (!fs::is_directory(cfg_.dir, ec)) return false;
        openLog();

        // Az előző futás: hiányzó „rendben leállt” jel (a running fájl megmaradt) → helyi jelentés
        prevAbnormal_ = fs::exists(cfg_.dir / "running", ec);
        const fs::path repJson = previousReportPath(), repTxt = cfg_.dir / "previous-run.txt";
        if (prevAbnormal_) {
            report::ScrubContext ctx = report::systemScrubContext();
            ctx.names.insert(ctx.names.end(), cfg_.scrubNames.begin(), cfg_.scrubNames.end());
            const report::Report r = report::build(cfg_.dir, cfg_.logPath, ctx, "previous_abnormal", "", 200, true);
            writeAtomic(repJson, r.json);
            writeAtomic(repTxt, r.preview);
            log("[Hibajelentés] Az előző futás nem állt le rendesen; helyi jelentés készült (diag/previous-run.json).\n");
        } else if (!enabled_.load()) {
            fs::remove(repJson, ec);   // kikapcsolt beállítás: a jelentés nem marad meg tartósan
            fs::remove(repTxt, ec);
        }
        fs::remove(cfg_.dir / "crash.txt", ec);
        fs::remove(cfg_.dir / "state.json", ec);
        writeAtomic(cfg_.dir / "running", std::to_string(pidNow()) + "\n");
        if (cfg_.crashHandlers) aq::crash::install(cfg_.dir / "crash.txt", reg_);

        startedNs_ = aq::wd::nowNs();
        startedUnix_ = static_cast<long long>(std::time(nullptr));
        lastCpuS_ = cpuSeconds();
        lastCpuNs_ = startedNs_;
        quit_ = false;
        writeState(false);
        thread_ = std::thread(&Monitor::loop, this);
        return true;
    }

    /** A motor részadata (érvényes JSON-objektum; más szöveg karakterláncként kerül be). Bármely nem valós idejű szálról. */
    void setSection(const std::string& name, const std::string& json) {
        ee_analyzer::json::Value v;
        const bool ok = ee_analyzer::json::parse(json, v) && v.type == ee_analyzer::json::Value::Type::Object;
        std::lock_guard<std::mutex> lk(secMu_);
        sections_[name] = Section{ ok ? json : "\"" + report::detail::jsonEsc(json) + "\"", aq::wd::nowNs() };
        dirty_ = true;
    }

    /** A hangkártyák gyári típusneve és formátumai (szöveg). */
    void setSoundCards(const std::string& text) {
        std::lock_guard<std::mutex> lk(secMu_);
        soundCards_ = text.size() > 16384 ? text.substr(0, 16384) : text;
        dirty_ = true;
    }

    void setEnabled(bool on) {
        enabled_ = on;
        dirty_ = true;
    }
    bool enabled() const { return enabled_.load(); }

    /** Rendes leállás: utolsó állapot clean_exit = true-val, a „fut” jel törlése. */
    void stopClean() {
        if (!thread_.joinable()) return;
        stop();
        writeState(true);
        aq::crash::disarm();
        std::error_code ec;
        fs::remove(cfg_.dir / "running", ec);
        closeLog();
    }

    /** Az őrszál leállítása a „rendben leállt” jel nélkül. */
    void stop() {
        {
            std::lock_guard<std::mutex> lk(cvMu_);
            quit_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    bool previousRunAbnormal() const { return prevAbnormal_; }
    fs::path previousReportPath() const { return cfg_.dir / "previous-run.json"; }
    fs::path statePath() const { return cfg_.dir / "state.json"; }
    const fs::path& dir() const { return cfg_.dir; }

private:
    struct Section {
        std::string json;
        int64_t updatedNs = 0;
    };
    struct Stall {
        std::string thread, label;
        int64_t sinceNs = 0;
        long long atUnix = 0;
        double seconds = 0.0;
        bool ongoing = true;
    };

    void loop() {
        int64_t lastWrite = aq::wd::nowNs();
        std::unique_lock<std::mutex> lk(cvMu_);
        while (!quit_) {
            cv_.wait_for(lk, std::chrono::milliseconds(cfg_.checkIntervalMs), [this] { return quit_; });
            if (quit_) break;
            lk.unlock();
            const int64_t now = aq::wd::nowNs();
            for (const aq::wd::Event& e : wd_->check(now)) {
                char buf[160];
                if (!e.recovered) {
                    Stall s;
                    s.thread = e.thread;
                    s.label = e.label;
                    s.sinceNs = now - static_cast<int64_t>(e.seconds * 1e9);
                    s.atUnix = static_cast<long long>(std::time(nullptr));
                    s.seconds = e.seconds;
                    if (stalls_.size() >= 50) stalls_.erase(stalls_.begin());
                    stalls_.push_back(s);
                    std::snprintf(buf, sizeof(buf), "[Őrszem] A(z) \"%s\" szál %.1f s óta nem jelez (utolsó: %s).\n",
                                  e.thread.c_str(), e.seconds, e.label.c_str());
                } else {
                    for (auto it = stalls_.rbegin(); it != stalls_.rend(); ++it)
                        if (it->ongoing && it->thread == e.thread) { it->ongoing = false; it->seconds = e.seconds; break; }
                    std::snprintf(buf, sizeof(buf), "[Őrszem] A(z) \"%s\" szál ismét jelez (%.1f s kiesés után).\n",
                                  e.thread.c_str(), e.seconds);
                }
                log(buf);
                dirty_ = true;
            }
            if (dirty_.exchange(false) || now - lastWrite >= static_cast<int64_t>(cfg_.writeIntervalMs) * 1000000) {
                writeState(false);
                lastWrite = now;
            }
            lk.lock();
        }
    }

    void writeState(bool cleanExit) {
        const int64_t now = aq::wd::nowNs();
        // CPU (az előző írás óta) és memória
        const double cpu = cpuSeconds();
        const double dt = static_cast<double>(now - lastCpuNs_) / 1e9;
        const double pct = dt > 0.05 ? 100.0 * (cpu - lastCpuS_) / dt : lastPct_;
        if (dt > 0.05) { lastCpuS_ = cpu; lastCpuNs_ = now; lastPct_ = pct; }
        char num[256];
        std::string j = "{\"format\":1,\"version\":\"" + esc(cfg_.version) + "\",\"platform\":\"" + esc(cfg_.platform) +
                        "\",\"package\":\"" + esc(cfg_.package) + "\"";
        std::snprintf(num, sizeof(num),
                      ",\"pid\":%lld,\"started_unix\":%lld,\"updated_unix\":%lld,\"uptime_s\":%.1f,\"threshold_s\":%.1f",
                      static_cast<long long>(pidNow()), startedUnix_, static_cast<long long>(std::time(nullptr)),
                      static_cast<double>(now - startedNs_) / 1e9, wd_->threshold());
        j += num;
        j += std::string(",\"clean_exit\":") + (cleanExit ? "true" : "false");
        j += std::string(",\"reports_enabled\":") + (enabled_.load() ? "true" : "false");
        j += ",\"heartbeats\":" + wd_->heartbeatsJson(now);
        j += ",\"stalls\":[";
        for (size_t i = 0; i < stalls_.size(); ++i) {
            const Stall& s = stalls_[i];
            const double secs = s.ongoing ? static_cast<double>(now - s.sinceNs) / 1e9 : s.seconds;
            std::snprintf(num, sizeof(num), "\",\"seconds\":%.1f,\"at_unix\":%lld,\"ongoing\":%s}", secs, s.atUnix,
                          s.ongoing ? "true" : "false");
            j += std::string(i ? "," : "") + "{\"thread\":\"" + esc(s.thread) + "\",\"label\":\"" + esc(s.label) + num;
        }
        j += "]";
        std::snprintf(num, sizeof(num), ",\"process\":{\"cpu_pct\":%.1f,\"cpu_s\":%.1f,\"rss_kb\":%lld}", pct, cpu,
                      rssKb());
        j += num;
        {
            std::lock_guard<std::mutex> lk(secMu_);
            j += ",\"sections\":{";
            bool first = true;
            for (const auto& [name, s] : sections_) {
                j += std::string(first ? "" : ",") + "\"" + esc(name) + "\":" + s.json;
                first = false;
            }
            j += "},\"section_age_s\":{";
            first = true;
            for (const auto& [name, s] : sections_) {
                std::snprintf(num, sizeof(num), "%.1f", static_cast<double>(now - s.updatedNs) / 1e9);
                j += std::string(first ? "" : ",") + "\"" + esc(name) + "\":" + num;
                first = false;
            }
            j += "},\"sound_cards\":\"" + report::detail::jsonEsc(soundCards_) + "\"";
        }
        j += "}";
        writeAtomic(statePath(), j);
    }

    static std::string esc(const std::string& s) { return report::detail::jsonEsc(s); }

    static bool writeAtomic(const fs::path& p, const std::string& data) {
        const fs::path tmp = p.string() + ".tmp";
        {
            std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
            if (!o) return false;
            o << data;
            if (!o) return false;
        }
        std::error_code ec;
        fs::rename(tmp, p, ec);
        return !ec;
    }

    static long long pidNow() {
#ifdef _WIN32
        return static_cast<long long>(GetCurrentProcessId());
#else
        return static_cast<long long>(getpid());
#endif
    }

    static double cpuSeconds() {
#ifdef _WIN32
        FILETIME c, e, k, u;
        if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0.0;
        auto toS = [](const FILETIME& f) { return (static_cast<double>(f.dwHighDateTime) * 4294967296.0 + f.dwLowDateTime) / 1e7; };
        return toS(k) + toS(u);
#else
        struct rusage ru{};
        if (getrusage(RUSAGE_SELF, &ru) != 0) return 0.0;
        return static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) +
               static_cast<double>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
#endif
    }

    /** A pillanatnyi memóriahasználat (Linux: rezidens; Windows: munkakészlet; macOS: csúcsérték). */
    static long long rssKb() {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS pmc{};
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return static_cast<long long>(pmc.WorkingSetSize / 1024);
        return -1;
#elif defined(__linux__)
        std::ifstream f("/proc/self/statm");
        long long size = 0, res = 0;
        if (f >> size >> res) return res * (sysconf(_SC_PAGESIZE) / 1024);
        return -1;
#else
        struct rusage ru{};
        if (getrusage(RUSAGE_SELF, &ru) == 0) return static_cast<long long>(ru.ru_maxrss / 1024);
        return -1;
#endif
    }

    void openLog() {
#ifdef _WIN32
        if (!cfg_.logPath.empty())
            logHandle_ = CreateFileW(cfg_.logPath.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
        if (!cfg_.logPath.empty() && !LogTee::active())
            logFd_ = ::open(cfg_.logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
#endif
    }
    void closeLog() {
#ifdef _WIN32
        if (logHandle_ != INVALID_HANDLE_VALUE) CloseHandle(logHandle_);
        logHandle_ = INVALID_HANDLE_VALUE;
#else
        if (logFd_ >= 0) ::close(logFd_);
        logFd_ = -1;
#endif
    }
    /** Közvetlen naplósor (a cout és a motor zárai nélkül). */
    void log(const std::string& line) {
#ifdef _WIN32
        DWORD w = 0;
        if (logHandle_ != INVALID_HANDLE_VALUE) WriteFile(logHandle_, line.data(), static_cast<DWORD>(line.size()), &w, nullptr);
#else
        if (LogTee::active()) { LogTee::writeRaw(line); return; }
        if (logFd_ >= 0) { const ssize_t r = ::write(logFd_, line.data(), line.size()); (void)r; }
        const ssize_t r2 = ::write(2, line.data(), line.size());
        (void)r2;
#endif
    }

    Config cfg_;
    aq::wd::Registry* reg_ = nullptr;
    std::unique_ptr<aq::wd::Watchdog> wd_;
    std::atomic<bool> enabled_{ false };
    std::atomic<bool> dirty_{ false };
    bool prevAbnormal_ = false;
    int64_t startedNs_ = 0;
    long long startedUnix_ = 0;
    double lastCpuS_ = 0.0, lastPct_ = 0.0;
    int64_t lastCpuNs_ = 0;
    std::vector<Stall> stalls_;
    std::mutex secMu_;
    std::map<std::string, Section> sections_;
    std::string soundCards_;
    std::mutex cvMu_;
    std::condition_variable cv_;
    bool quit_ = false;
    std::thread thread_;
#ifdef _WIN32
    HANDLE logHandle_ = INVALID_HANDLE_VALUE;
#else
    int logFd_ = -1;
#endif
};

} // namespace aq::health
