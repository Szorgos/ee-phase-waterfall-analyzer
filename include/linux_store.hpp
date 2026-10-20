#pragma once
/**
 * @file linux_store.hpp
 * @brief A DSP beállítások és a konvolver IR mentése Linuxon ($XDG_CONFIG_HOME/aetheraudio vagy ~/.config/aetheraudio).
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
constexpr uint32_t kParamVersion = 3;
constexpr uint32_t kIrMagic = 0x46524941;    // 'AIRF'

inline std::string configDir(bool create) {
    std::string dir;
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) dir = x;
    else if (const char* h = std::getenv("HOME"); h && *h) dir = std::string(h) + "/.config";
    else dir = ".";
    if (create) mkdir(dir.c_str(), 0755);
    dir += "/aetheraudio";
    if (create) mkdir(dir.c_str(), 0755);
    return dir;
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
    if (hdr[0] != kParamMagic || hdr[1] < 2 || hdr[1] > kParamVersion || hdr[2] > sizeof(ParamData) ||
        b.size() < 12 + hdr[2] || hdr[2] < offsetof(ParamData, eqMode))
        return false;
    ParamData d{};
    std::memcpy(&d, b.data() + 12, hdr[2]);
    if (d.numBands > kMaxEqBands) d.numBands = kMaxEqBands;
    out = d;
    return true;
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

/** EQ profilok: a felület JSON dokumentuma (~/.config/aetheraudio/presets.json). */
inline bool savePresets(const std::string& doc) {
    return writeAtomic(configDir(true) + "/presets.json", std::vector<char>(doc.begin(), doc.end()));
}

inline std::string loadPresets() {
    std::ifstream f(configDir(false) + "/presets.json", std::ios::binary);
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

} // namespace ee_analyzer::linux_store
