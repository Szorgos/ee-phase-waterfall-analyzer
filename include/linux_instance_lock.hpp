#pragma once
/**
 * @file linux_instance_lock.hpp
 * @brief Egyetlen-példány zár (flock) és párhuzamos Linux-telepítés (script vs. csomag) felismerése.
 *
 * A zár fájlleírón alapuló kernel-szintű flock(LOCK_EX | LOCK_NB): rendes kilépéskor és összeomláskor
 * (SIGKILL, SIGSEGV) is azonnal felszabadul, így egy lemezen maradt lockfájl soha nem akadályozza az indulást.
 */
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>

namespace ee_analyzer::linux_inst {

struct ExistingInfo {
    long pid = 0;
    std::string exe;
    std::string version;
};

/** Folyamatszintű exkluzív zár (flock). A destruktor bezárja a leírót, de a fájlt nem törli (nincs versenyhelyzet). */
class InstanceLock {
public:
    InstanceLock() = default;
    ~InstanceLock() { release(); }
    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;

    /**
     * Megpróbálja megszerezni a zárat.
     * - true: a zár a miénk, a lockfájlba beírtuk a saját PID/exe/verzió adatainkat.
     * - false: egy másik élő folyamat tartja a zárat (outExisting kitöltve a lockfájlból, ha olvasható).
     */
    bool acquire(const std::string& lockPath, const std::string& selfExe, const std::string& selfVersion,
                 ExistingInfo* outExisting = nullptr) {
        release();
        std::error_code ec;
        const std::filesystem::path p(lockPath);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
        const int fd = ::open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (fd < 0) return true; // ha a mappa nem írható, ne akadályozzuk az indulást
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                if (outExisting) *outExisting = readInfo(fd);
                ::close(fd);
                return false;
            }
            // Egyéb (pl. flock-ot nem támogató) fájlrendszer: engedjük tovább
            ::close(fd);
            return true;
        }
        fd_ = fd;
        const std::string body = "pid=" + std::to_string(static_cast<long>(::getpid())) +
                                 "\nexe=" + selfExe + "\nversion=" + selfVersion + "\n";
        if (::ftruncate(fd_, 0) == 0 && ::lseek(fd_, 0, SEEK_SET) == 0) {
            const ssize_t r = ::write(fd_, body.data(), body.size());
            (void)r;
        }
        return true;
    }

    void release() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool held() const { return fd_ >= 0; }

private:
    static ExistingInfo readInfo(int fd) {
        ExistingInfo info;
        if (::lseek(fd, 0, SEEK_SET) != 0) return info;
        char buf[1024];
        const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) return info;
        buf[n] = '\0';
        std::istringstream in(buf);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("pid=", 0) == 0) info.pid = std::strtol(line.c_str() + 4, nullptr, 10);
            else if (line.rfind("exe=", 0) == 0) info.exe = line.substr(4);
            else if (line.rfind("version=", 0) == 0) info.version = line.substr(8);
        }
        return info;
    }

    int fd_ = -1;
};

struct DualInstallInfo {
    bool detected = false;
    std::string reason;      // "dual_install" vagy "other_running" (régi, zár nélküli motor)
    std::string runningKind;
    std::string runningPath;
    std::string otherKind;
    std::string otherPath;
    std::string removeCmd;

    static std::string esc(const std::string& s) {
        std::string o;
        o.reserve(s.size() + 8);
        for (char c : s) {
            if (c == '\\' || c == '"') { o += '\\'; o += c; }
            else if (c == '\n') o += "\\n";
            else if (static_cast<unsigned char>(c) >= 0x20) o += c;
        }
        return o;
    }

    /** Üres, ha nincs két telepítés; különben vesszővel kezdődő JSON mező a status üzenethez. */
    std::string statusJsonField() const {
        if (!detected) return {};
        return ",\"dualInstall\":{\"reason\":\"" + esc(reason.empty() ? "dual_install" : reason) +
               "\",\"running\":\"" + esc(runningKind) +
               "\",\"runningPath\":\"" + esc(runningPath) +
               "\",\"other\":\"" + esc(otherKind) +
               "\",\"otherPath\":\"" + esc(otherPath) +
               "\",\"removeCmd\":\"" + esc(removeCmd) + "\"}";
    }
};

/**
 * Két párhuzamos Linux-telepítés (script ~/.local + ~/.config/systemd/user vs. csomag /usr/bin) felismerése.
 * A felhasználói fájlokhoz nem nyúl; csak visszaadja, melyik fut, melyik a másik, és mi a pontos eltávolítási parancs.
 */
inline DualInstallInfo detectDualInstall(const std::string& runningKind, const std::string& selfExe,
                                         const std::string& homeDir, const std::string& configBaseDir,
                                         const std::string& systemBin = "/usr/bin/aqulify-engine") {
    DualInstallInfo info;
    auto exists = [](const std::string& p) {
        std::error_code ec;
        return !p.empty() && std::filesystem::exists(p, ec);
    };
    const std::string userBin = homeDir.empty() ? std::string() : (homeDir + "/.local/bin/aqulify-engine");
    const std::string userUnit = configBaseDir.empty() ? std::string() : (configBaseDir + "/systemd/user/aqulify.service");
    const std::string kRemoveScriptCmd =
        "rm -f ~/.config/systemd/user/aqulify.service ~/.local/bin/aqulify-engine ~/.local/bin/aqulify-gui && "
        "systemctl --user daemon-reload && systemctl --user restart aqulify.service";

    if (runningKind == "script") {
        if (exists(systemBin)) {
            info.detected = true;
            info.reason = "dual_install";
            info.runningKind = "script";
            info.runningPath = selfExe;
            info.otherKind = "pkg";
            info.otherPath = systemBin;
            info.removeCmd = kRemoveScriptCmd;
        }
    } else {
        // Csomagból (copr / deb / pacman / pkg) fut: ellenőrizzük a régi scriptes maradékot
        const bool hasUserBin = exists(userBin);
        const bool hasUserUnit = exists(userUnit);
        if (hasUserBin || hasUserUnit) {
            info.detected = true;
            info.reason = "dual_install";
            info.runningKind = runningKind;
            info.runningPath = selfExe;
            info.otherKind = "script";
            info.otherPath = hasUserUnit ? userUnit : userBin;
            info.removeCmd = kRemoveScriptCmd;
        }
    }
    return info;
}

/**
 * Régi, zár (flock) nélküli motor (2.7.0–2.8.3) felismerése: ha az alapértelmezett 47810-es port foglalt volt
 * (a motor 47811-re lépett), vagy a PipeWire-gráfban már van egy másik aqulify_sink csomópont.
 */
inline DualInstallInfo detectOtherRunningEngine(bool defaultPortBusy, bool duplicatePipeWireSink,
                                                const std::string& runningKind, const std::string& selfExe) {
    DualInstallInfo info;
    if (!defaultPortBusy && !duplicatePipeWireSink) return info;
    info.detected = true;
    info.reason = "other_running";
    info.runningKind = runningKind;
    info.runningPath = selfExe;
    info.otherKind = "running";
    info.otherPath = defaultPortBusy ? "port 47810" : "aqulify_sink";
    info.removeCmd =
        "systemctl --user stop aqulify.service; pkill -x aqulify-engine; "
        "rm -f ~/.config/systemd/user/aqulify.service ~/.local/bin/aqulify-engine ~/.local/bin/aqulify-gui && "
        "systemctl --user daemon-reload && systemctl --user restart aqulify.service";
    return info;
}

} // namespace ee_analyzer::linux_inst
