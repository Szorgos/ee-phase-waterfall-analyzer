#pragma once
/**
 * @file linux_store.hpp
 * @brief A DSP beállítások és a konvolver IR mentése Linuxon ($XDG_CONFIG_HOME/aqulify vagy ~/.config/aqulify)
 *        és macOS-en (~/Library/Application Support/Aqulify).
 *
 * Fájlformátum azonos a Windowsos változatéval (params.bin: 'AAEP' v3, ir.bin: 'AIRF' v1), így a beállítások
 * a két rendszer között átmásolhatók.
 */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "ir_data.hpp"
#include "param_block.hpp"
#include "shared_state.hpp"

namespace ee_analyzer::linux_store {

constexpr uint32_t kParamMagic = 0x50454141; // 'AAEP'
constexpr uint32_t kParamVersion = kParamLayoutVersion;
constexpr uint32_t kIrMagic = 0x46524941;    // 'AIRF'

inline std::string configDir(bool create) {
#ifdef __APPLE__
    std::string mac;
    if (const char* h = std::getenv("HOME"); h && *h) mac = std::string(h) + "/Library/Application Support";
    else mac = ".";
    if (create) mkdir(mac.c_str(), 0755);
    mac += "/Aqulify";
    if (create) mkdir(mac.c_str(), 0755);
    return mac;
#else
    std::string dir;
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) dir = x;
    else if (const char* h = std::getenv("HOME"); h && *h) dir = std::string(h) + "/.config";
    else dir = ".";
    if (create) mkdir(dir.c_str(), 0755);
    // Régi név (AetherAudio, 1.4.x-ig): a beállítások, profilok és az IR egyszeri átköltöztetése
    static bool migrated = false;
    if (!migrated) {
        migrated = true;
        const std::string legacy = dir + "/" + "aether" "audio";
        struct stat st{};
        if (stat((dir + "/aqulify").c_str(), &st) != 0 && stat(legacy.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            std::rename(legacy.c_str(), (dir + "/aqulify").c_str());
    }
    dir += "/aqulify";
    if (create) mkdir(dir.c_str(), 0755);
    return dir;
#endif
}

inline bool writeAtomic(const std::string& path, const std::vector<char>& bytes) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!f) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

inline bool saveParams(const ParamData& d) {
    std::vector<char> b(12 + sizeof(ParamData));
    const uint32_t hdr[3] = { kParamMagic, kParamVersion, static_cast<uint32_t>(sizeof(ParamData)) };
    std::memcpy(b.data(), hdr, 12);
    std::memcpy(b.data() + 12, &d, sizeof(ParamData));
    return writeAtomic(configDir(true) + "/params.bin", b);
}

inline bool loadParams(ParamData& out) {
    std::ifstream f(configDir(false) + "/params.bin", std::ios::binary);
    if (!f) return false;
    std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 12) return false;
    uint32_t hdr[3];
    std::memcpy(hdr, b.data(), 12);
    if (hdr[0] != kParamMagic) return false;
    return loadParamBytes(b.data() + 12, b.size() - 12, hdr[1], hdr[2], out);
}

inline bool saveIr(const IrData& ir) {
    const std::string path = configDir(true) + "/ir.bin";
    if (ir.empty()) { std::remove(path.c_str()); return true; }
    const uint32_t frames = static_cast<uint32_t>(std::min<size_t>(ir.frames(), kMaxIrFrames));
    const uint32_t hdr[6] = { kIrMagic, 1u, ir.sampleRate, ir.channels(), frames, static_cast<uint32_t>(ir.name.size()) };
    std::vector<char> b(sizeof(hdr) + ir.name.size() + frames * 4u * ir.channels());
    char* p = b.data();
    std::memcpy(p, hdr, sizeof(hdr)); p += sizeof(hdr);
    std::memcpy(p, ir.name.data(), ir.name.size()); p += ir.name.size();
    std::memcpy(p, ir.left.data(), frames * 4u); p += frames * 4u;
    if (!ir.right.empty()) std::memcpy(p, ir.right.data(), frames * 4u);
    return writeAtomic(path, b);
}

inline bool loadIr(IrData& out) {
    std::ifstream f(configDir(false) + "/ir.bin", std::ios::binary);
    if (!f) return false;
    uint32_t hdr[6] = {};
    if (!f.read(reinterpret_cast<char*>(hdr), sizeof(hdr))) return false;
    if (hdr[0] != kIrMagic || hdr[1] != 1u || (hdr[3] != 1 && hdr[3] != 2) || hdr[4] == 0 || hdr[4] > kMaxIrFrames ||
        hdr[5] >= 4096)
        return false;
    IrData ir;
    ir.sampleRate = hdr[2];
    ir.name.resize(hdr[5]);
    ir.left.resize(hdr[4]);
    if (!f.read(ir.name.data(), hdr[5])) return false;
    if (!f.read(reinterpret_cast<char*>(ir.left.data()), hdr[4] * 4u)) return false;
    if (hdr[3] == 2) {
        ir.right.resize(hdr[4]);
        if (!f.read(reinterpret_cast<char*>(ir.right.data()), hdr[4] * 4u)) return false;
    }
    out = std::move(ir);
    return true;
}

/** IR-tár: az IR-profilok impulzusválaszai (~/.config/aqulify/irs/<azonosító>.bin, az ir.bin formátumában). */
inline std::string libraryIrPath(const std::string& id, bool create) {
    std::string dir = configDir(create) + "/irs";
    if (create) mkdir(dir.c_str(), 0755);
    return dir + "/" + id + ".bin";
}

inline bool saveLibraryIr(const std::string& id, const std::string& bytes) {
    if (!isValidIrId(id) || bytes.empty()) return false;
    return writeAtomic(libraryIrPath(id, true), std::vector<char>(bytes.begin(), bytes.end()));
}

inline std::string loadLibraryIr(const std::string& id) {
    if (!isValidIrId(id)) return "";
    std::ifstream f(libraryIrPath(id, false), std::ios::binary);
    if (!f) return "";
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

inline bool removeLibraryIr(const std::string& id) {
    return isValidIrId(id) && std::remove(libraryIrPath(id, false).c_str()) == 0;
}

/** EQ profilok: a felület JSON dokumentuma (~/.config/aqulify/presets.json). */
inline bool savePresets(const std::string& doc) {
    return writeAtomic(configDir(true) + "/presets.json", std::vector<char>(doc.begin(), doc.end()));
}

inline std::string loadPresets() {
    std::ifstream f(configDir(false) + "/presets.json", std::ios::binary);
    if (!f) return "";
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

/** A zenetár mappái (2.x): {"roots":[…]} a library.json-ban. */
inline bool saveLibrary(const std::string& doc) {
    return writeAtomic(configDir(true) + "/library.json", std::vector<char>(doc.begin(), doc.end()));
}

inline std::string loadLibrary() {
    std::ifstream f(configDir(false) + "/library.json", std::ios::binary);
    if (!f) return "";
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

/** A zenetár gyorsítótára (2.x): a lemezbejárás eredménye a library-cache.json-ban. */
inline std::string loadLibraryCache() {
    std::ifstream f(configDir(false) + "/library-cache.json", std::ios::binary);
    if (!f) return "";
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

inline bool saveLibraryCache(const std::string& doc) {
    return writeAtomic(configDir(true) + "/library-cache.json", std::vector<char>(doc.begin(), doc.end()));
}

/** A lejátszási sor (2.x): a tételek és a sorszám a queue.json-ban. */
inline bool saveQueue(const std::string& doc) {
    return writeAtomic(configDir(true) + "/queue.json", std::vector<char>(doc.begin(), doc.end()));
}

inline std::string loadQueue() {
    std::ifstream f(configDir(false) + "/queue.json", std::ios::binary);
    if (!f) return "";
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

/** A lejátszási listák (2.x): a playlists.json-ban. */
inline bool savePlaylists(const std::string& doc) {
    return writeAtomic(configDir(true) + "/playlists.json", std::vector<char>(doc.begin(), doc.end()));
}

inline std::string loadPlaylists() {
    std::ifstream f(configDir(false) + "/playlists.json", std::ios::binary);
    if (!f) return "";
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

/** A lejátszási napló (2.x, okos listák): a stats.json-ban. */
inline bool saveStats(const std::string& doc) {
    return writeAtomic(configDir(true) + "/stats.json", std::vector<char>(doc.begin(), doc.end()));
}

inline std::string loadStats() {
    std::ifstream f(configDir(false) + "/stats.json", std::ios::binary);
    if (!f) return "";
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

/** A hálózati elérés beállításai és a párosított eszközök (remote.json, csak a felhasználó olvashatja). */
inline bool saveRemote(const std::string& doc) {
    const std::string path = configDir(true) + "/remote.json";
    if (!writeAtomic(path, std::vector<char>(doc.begin(), doc.end()))) return false;
    chmod(path.c_str(), 0600);
    return true;
}

inline std::string loadRemote() {
    std::ifstream f(configDir(false) + "/remote.json", std::ios::binary);
    if (!f) return "";
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

inline bool saveOutputSink(const std::string& sinkName) {
    const std::string path = configDir(true) + "/output_sink.txt";
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << sinkName << "\n";
    return true;
}

inline std::string loadOutputSink() {
    const std::string path = configDir(false) + "/output_sink.txt";
    std::ifstream f(path);
    if (!f) return "";
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

/** Headless mód (Raspberry Pi): az aktuális 6-jegyű párosító PIN kód (pair.pin). */
inline bool savePairPin(const std::string& pin) {
    const std::string path = configDir(true) + "/pair.pin";
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << pin << "\n";
#ifndef _WIN32
    chmod(path.c_str(), 0600);
#endif
    return true;
}

inline std::string loadPairPin() {
    const std::string path = configDir(false) + "/pair.pin";
    std::ifstream f(path);
    if (!f) return "";
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

/** A grafikus felület fájljainak alapértelmezett mappája (HTML, JS, CSS). */
inline std::string defaultGuiDir() {
    std::vector<std::string> candidates;
#ifndef _WIN32
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x)
        candidates.push_back(std::string(x) + "/aqulify");
    else if (const char* h = std::getenv("HOME"); h && *h)
        candidates.push_back(std::string(h) + "/.local/share/aqulify");
    candidates.push_back("/usr/share/aqulify");
    candidates.push_back("/usr/local/share/aqulify");
#endif
    candidates.push_back(".");
    candidates.push_back("..");

    for (const auto& c : candidates) {
        struct stat st{};
        const std::string testFile = c + "/gui_integrated_system.html";
        if (stat(testFile.c_str(), &st) == 0 && S_ISREG(st.st_mode))
            return c;
    }
    return "";
}

} // namespace ee_analyzer::linux_store
