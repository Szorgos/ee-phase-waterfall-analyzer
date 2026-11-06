#pragma once
/**
 * @file update_logic.hpp
 * @brief A frissítés rendszerfüggetlen része: a verziószám ellenőrzése és összevetése, a kiadás letöltési címei (rögzített
 *        tároló: csak a saját GitHub-kiadásból), és a kiadás .sha256 fájljának értelmezése. A Windows-frissítő
 *        (windows/engine/updater.hpp) és a macOS-frissítő (macos/app/mac_updater.hpp) használja; a tesztek (test_core)
 *        Linuxon is ellenőrzik.
 */
#include <cctype>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

namespace ee_analyzer::update {

inline constexpr const char* kRepo = "Szorgos/ee-phase-waterfall-analyzer";

/** Érvényes kiadási verzió: X.Y.Z, részenként 1–4 számjegy (más karakter nincs benne). */
inline bool validVersion(const std::string& v) {
    int dots = 0, digits = 0;
    for (char c : v) {
        if (c >= '0' && c <= '9') {
            if (++digits > 4) return false;
        } else if (c == '.') {
            if (digits == 0) return false;
            ++dots;
            digits = 0;
        } else {
            return false;
        }
    }
    return dots == 2 && digits > 0;
}

/** Az X.Y.Z verzió idx. része (0, 1 vagy 2). */
inline int versionPart(const std::string& v, int idx) {
    int i = 0, n = 0;
    for (char c : v) {
        if (c == '.') {
            if (i == idx) return n;
            ++i;
            n = 0;
        } else {
            n = n * 10 + (c - '0');
        }
    }
    return i == idx ? n : 0;
}

/** Negatív, ha a < b; nulla, ha egyenlők (érvényes verziókra). */
inline int compareVersions(const std::string& a, const std::string& b) {
    for (int i = 0; i < 3; ++i) {
        const int d = versionPart(a, i) - versionPart(b, i);
        if (d) return d;
    }
    return 0;
}

/** A Windows-telepítő címe a kiadásban (csak érvényes verzióval hívható). */
inline std::string installerUrl(const std::string& v) {
    return std::string("https://github.com/") + kRepo + "/releases/download/v" + v + "/Aqulify-Setup-" + v + ".exe";
}

inline std::string checksumUrl(const std::string& v) { return installerUrl(v) + ".sha256"; }

/** A macOS-lemezkép (DMG) címe a kiadásban (csak érvényes verzióval hívható). */
inline std::string dmgUrl(const std::string& v) {
    return std::string("https://github.com/") + kRepo + "/releases/download/v" + v + "/Aqulify-" + v + "-macOS.dmg";
}

inline std::string dmgChecksumUrl(const std::string& v) { return dmgUrl(v) + ".sha256"; }

/** Hogyan került a gépre a Linux-változat: ezen múlik, milyen paranccsal frissíthető. */
enum class LinuxInstall { Copr, Rpm, Deb, Arch, Script, Unknown };

inline const char* linuxInstallName(LinuxInstall k) {
    switch (k) {
    case LinuxInstall::Copr: return "copr";
    case LinuxInstall::Rpm: return "rpm";
    case LinuxInstall::Deb: return "deb";
    case LinuxInstall::Arch: return "arch";
    case LinuxInstall::Script: return "script";
    default: return "unknown";
    }
}

/** Fájlrendszer-hozzáférés a felismeréshez (a tesztek hamisat adnak). list: a mappa elemeinek neve. */
struct FsProbe {
    std::function<bool(const std::string&)> exists;
    std::function<std::vector<std::string>(const std::string&)> list;
};

inline FsProbe realFs() {
    FsProbe fs;
    fs.exists = [](const std::string& p) {
        std::error_code ec;
        return std::filesystem::exists(p, ec);
    };
    fs.list = [](const std::string& dir) {
        std::vector<std::string> out;
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator();
             it.increment(ec))
            out.push_back(it->path().filename().string());
        return out;
    };
    return fs;
}

/**
 * A futó motor útvonala és a csomagkezelők saját nyilvántartása alapján. /usr/... (de nem /usr/local) → csomag: a dpkg,
 * a pacman, illetve az rpm nyilvántartása dönt; rpm-nél a COPR-t a tárolófájlja jelzi (az /etc/yum.repos.d mappában egy
 * „aqulify”-t tartalmazó .repo fájl; a „dnf copr enable nlali/aqulify” hozza létre). Minden más (pl. ~/.local) → az
 * install.sh-val telepítve.
 */
inline LinuxInstall detectLinuxInstall(const std::string& exe, const FsProbe& fs) {
    if (exe.empty()) return LinuxInstall::Unknown;
    const bool packaged = exe.rfind("/usr/", 0) == 0 && exe.rfind("/usr/local/", 0) != 0;
    if (!packaged) return LinuxInstall::Script;
    if (fs.exists("/var/lib/dpkg/info/aqulify.list") || fs.exists("/var/lib/dpkg/info/aqulify:amd64.list"))
        return LinuxInstall::Deb;
    for (const std::string& n : fs.list("/var/lib/pacman/local"))
        if (n.rfind("aqulify-", 0) == 0) return LinuxInstall::Arch;
    if (fs.exists("/var/lib/rpm") || fs.exists("/usr/lib/sysimage/rpm")) {
        for (const std::string& n : fs.list("/etc/yum.repos.d"))
            if (n.find("aqulify") != std::string::npos && n.size() > 5 && n.compare(n.size() - 5, 5, ".repo") == 0)
                return LinuxInstall::Copr;
        return LinuxInstall::Rpm;
    }
    return LinuxInstall::Unknown;
}

/**
 * A .sha256 fájl első szava (a sha256sum formája: "<kivonat>  <fájlnév>"), kisbetűsítve; üres, ha nem 64 hexadecimális
 * jegy. Az esetleges UTF-8 BOM-ot átugorja.
 */
inline std::string parseChecksum(const std::string& text) {
    size_t i = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    std::string h;
    while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) {
        h += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
        ++i;
    }
    if (h.size() != 64) return {};
    for (char c : h)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return {};
    return h;
}

} // namespace ee_analyzer::update
