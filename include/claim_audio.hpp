#pragma once
/**
 * @file claim_audio.hpp
 * @brief Képernyő nélküli (headless) első beállítás hangos kódfelmondása:
 *        - RAII hangerő-őrző (minden úton – hiba, megszakítás, kivétel – visszaállítja a hangerőt);
 *        - 0–9 számjegy WAV minták összefűzése rövid figyelemfelkeltő hangjelzéssel, nyelvi fallbackkel (angol);
 *        - Az ideiglenes WAV fájl neve soha nem tartalmazza a kódot, és lejátszás után azonnal törlődik.
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace ee_analyzer::claim_audio {

/** Fix, mérsékelt hangerő a kódfelmondás idejére (nem marad meg a beállítás után). */
inline constexpr double kClaimVolumeLevel = 0.45;

/**
 * RAII őrző: elmenti az aktuális hangerőt, beállítja a fix claim-hangerőt,
 * és kilépéskor (normál vég, hiba, megszakítás, kivétel) garantáltan visszaállítja az eredetit.
 */
class ClaimVolumeGuard {
public:
    using GetVolFn = std::function<bool(double& level, int& muted)>;
    using SetVolFn = std::function<void(double level, int muted)>;

    ClaimVolumeGuard(GetVolFn getVol, SetVolFn setVol, std::atomic<bool>* activeFlag = nullptr,
                     double targetLevel = kClaimVolumeLevel)
        : setVol_(std::move(setVol)), activeFlag_(activeFlag) {
        if (activeFlag_) activeFlag_->store(true);
        if (getVol && getVol(savedLevel_, savedMuted_)) {
            hasSaved_ = true;
        }
        if (setVol_) {
            setVol_(targetLevel, 0);
            applied_ = true;
        }
    }

    ~ClaimVolumeGuard() { restore(); }

    ClaimVolumeGuard(const ClaimVolumeGuard&) = delete;
    ClaimVolumeGuard& operator=(const ClaimVolumeGuard&) = delete;

    void restore() noexcept {
        if (restored_) return;
        restored_ = true;
        try {
            if (applied_ && setVol_) {
                if (hasSaved_) {
                    setVol_(savedLevel_, savedMuted_);
                } else {
                    setVol_(1.0, 0);
                }
            }
        } catch (...) {
            // Destruktorból nem dobunk kivételt
        }
        if (activeFlag_) activeFlag_->store(false);
    }

    bool hasSaved() const { return hasSaved_; }
    double savedLevel() const { return savedLevel_; }
    int savedMuted() const { return savedMuted_; }

private:
    SetVolFn setVol_;
    std::atomic<bool>* activeFlag_ = nullptr;
    bool hasSaved_ = false;
    bool applied_ = false;
    bool restored_ = false;
    double savedLevel_ = 1.0;
    int savedMuted_ = 0;
};

/** 2 betűs nyelvkód tisztítása (csak [a-z]{2}, különben "en"). */
inline std::string sanitizeLang(const std::string& raw) {
    std::string out;
    for (char c : raw) {
        if (c == '-' || c == '_') break;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c >= 'a' && c <= 'z') out += c;
        if (out.size() == 2) break;
    }
    return out.size() == 2 ? out : "en";
}

/** Egyszerű 16 bites PCM WAV olvasása (mono, 16 kHz / 22.05 kHz / 44.1 kHz / 48 kHz). */
inline bool readPcm16MonoWav(const std::string& path, std::vector<int16_t>& samples, uint32_t& sampleRate) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 44) return false;
    if (std::memcmp(data.data(), "RIFF", 4) != 0 || std::memcmp(data.data() + 8, "WAVE", 4) != 0) return false;

    auto u16 = [&](size_t off) -> uint16_t {
        return static_cast<uint16_t>(data[off] | (static_cast<uint16_t>(data[off + 1]) << 8));
    };
    auto u32 = [&](size_t off) -> uint32_t {
        return static_cast<uint32_t>(data[off]) | (static_cast<uint32_t>(data[off + 1]) << 8) |
               (static_cast<uint32_t>(data[off + 2]) << 16) | (static_cast<uint32_t>(data[off + 3]) << 24);
    };

    uint16_t audioFmt = 0, channels = 0, bitsPerSample = 0;
    uint32_t sr = 0;
    size_t pos = 12;
    size_t dataOff = 0, dataLen = 0;
    while (pos + 8 <= data.size()) {
        const uint32_t chunkSize = u32(pos + 4);
        if (pos + 8 + chunkSize > data.size()) break;
        if (std::memcmp(data.data() + pos, "fmt ", 4) == 0 && chunkSize >= 16) {
            audioFmt = u16(pos + 8);
            channels = u16(pos + 10);
            sr = u32(pos + 12);
            bitsPerSample = u16(pos + 22);
        } else if (std::memcmp(data.data() + pos, "data", 4) == 0) {
            dataOff = pos + 8;
            dataLen = chunkSize;
        }
        pos += 8 + chunkSize + (chunkSize & 1u);
    }
    if (audioFmt != 1 || bitsPerSample != 16 || (channels != 1 && channels != 2) || sr < 8000 || sr > 192000 ||
        dataLen < 2) {
        return false;
    }
    sampleRate = sr;
    const size_t frames = dataLen / (2u * channels);
    samples.clear();
    samples.reserve(frames);
    for (size_t i = 0; i < frames; ++i) {
        const size_t off = dataOff + i * 2u * channels;
        const int16_t s0 = static_cast<int16_t>(u16(off));
        if (channels == 1) {
            samples.push_back(s0);
        } else {
            const int16_t s1 = static_cast<int16_t>(u16(off + 2));
            samples.push_back(static_cast<int16_t>((static_cast<int32_t>(s0) + static_cast<int32_t>(s1)) / 2));
        }
    }
    return true;
}

/** Egyszerű 16 bites PCM mono WAV írása (0600 jogosultsággal POSIX-on). */
inline bool writePcm16MonoWav(const std::string& path, const std::vector<int16_t>& samples, uint32_t sampleRate) {
    const uint32_t dataSize = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    const uint32_t riffSize = 36 + dataSize;
    const uint32_t byteRate = sampleRate * 2;
    uint8_t hdr[44];
    auto put4 = [&](size_t off, const char* s) { std::memcpy(hdr + off, s, 4); };
    auto putU16 = [&](size_t off, uint16_t v) {
        hdr[off] = static_cast<uint8_t>(v & 0xFF);
        hdr[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    };
    auto putU32 = [&](size_t off, uint32_t v) {
        hdr[off] = static_cast<uint8_t>(v & 0xFF);
        hdr[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        hdr[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        hdr[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    };
    put4(0, "RIFF");
    putU32(4, riffSize);
    put4(8, "WAVE");
    put4(12, "fmt ");
    putU32(16, 16);
    putU16(20, 1);  // PCM
    putU16(22, 1);  // mono
    putU32(24, sampleRate);
    putU32(28, byteRate);
    putU16(32, 2);  // block align
    putU16(34, 16); // bits per sample
    put4(36, "data");
    putU32(40, dataSize);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    if (!samples.empty()) {
        out.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(dataSize));
    }
    out.flush();
    if (!out.good()) return false;
#ifndef _WIN32
    chmod(path.c_str(), 0600);
#endif
    return true;
}

/** Rövid, barátságos két hangból álló figyelemfelkeltő hangjelzés (C5 -> G5, ~0,4 s). */
inline void appendChime(std::vector<int16_t>& out, uint32_t sr) {
    constexpr double kPi = 3.14159265358979323846;
    const double freqs[2] = { 523.25, 783.99 };
    const size_t toneN = static_cast<size_t>(sr * 0.16);
    const size_t gapN = static_cast<size_t>(sr * 0.04);
    for (int t = 0; t < 2; ++t) {
        for (size_t i = 0; i < toneN; ++i) {
            const double sec = static_cast<double>(i) / static_cast<double>(sr);
            const double env = std::min(1.0, static_cast<double>(i) / (sr * 0.015)) *
                               std::exp(-sec * 6.0);
            const double val = 0.45 * env * std::sin(2.0 * kPi * freqs[t] * sec);
            out.push_back(static_cast<int16_t>(std::clamp(val, -1.0, 1.0) * 32000.0));
        }
        out.insert(out.end(), gapN, 0);
    }
}

/** Megkeresi a számjegy-hangminták mappáját a megadott nyelvre (ha nincs meg mind a 10, angolra esik vissza). */
inline std::string resolveDigitDir(const std::vector<std::string>& searchRoots, const std::string& lang) {
    const std::string clean = sanitizeLang(lang);
    auto hasAllDigits = [](const std::filesystem::path& dir) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec) || ec) return false;
        for (int d = 0; d <= 9; ++d) {
            if (!std::filesystem::is_regular_file(dir / (std::to_string(d) + ".wav"), ec) || ec) return false;
        }
        return true;
    };
    for (const auto& root : searchRoots) {
        if (root.empty()) continue;
        std::filesystem::path p = std::filesystem::path(root) / clean;
        if (hasAllDigits(p)) return p.string();
    }
    if (clean != "en") {
        for (const auto& root : searchRoots) {
            if (root.empty()) continue;
            std::filesystem::path p = std::filesystem::path(root) / "en";
            if (hasAllDigits(p)) return p.string();
        }
    }
    return {};
}

/**
 * Összeállítja a hangjelzést + 4 számjegyet egyetlen mono PCM16 WAV fájlba.
 * A kimeneti útvonalat a hívó adja meg (véletlen fájlnév, kód nélkül!).
 * @return A hang hossza milliszekundumban (0, ha nem sikerült).
 */
inline int buildCodeWav(const std::string& code, const std::string& lang,
                        const std::vector<std::string>& searchRoots, const std::string& outWavPath) {
    if (code.size() != 4) return 0;
    for (char c : code) {
        if (c < '0' || c > '9') return 0;
    }
    const std::string digitDir = resolveDigitDir(searchRoots, lang);
    if (digitDir.empty()) return 0;

    uint32_t sr = 0;
    std::vector<std::vector<int16_t>> digitPcm(4);
    for (size_t i = 0; i < 4; ++i) {
        uint32_t dsr = 0;
        const std::string p = (std::filesystem::path(digitDir) / (std::string(1, code[i]) + ".wav")).string();
        if (!readPcm16MonoWav(p, digitPcm[i], dsr)) return 0;
        if (sr == 0) sr = dsr;
        else if (dsr != sr) return 0;
    }

    std::vector<int16_t> out;
    out.reserve(sr * 5);
    out.insert(out.end(), static_cast<size_t>(sr * 0.15), 0);
    appendChime(out, sr);
    out.insert(out.end(), static_cast<size_t>(sr * 0.25), 0);
    for (size_t i = 0; i < 4; ++i) {
        out.insert(out.end(), digitPcm[i].begin(), digitPcm[i].end());
        out.insert(out.end(), static_cast<size_t>(sr * 0.35), 0);
    }

    if (!writePcm16MonoWav(outWavPath, out, sr)) return 0;
    return static_cast<int>((out.size() * 1000ull) / sr);
}

/** Alapértelmezett keresési helyek a telepített számjegy-hangmintákhoz. */
inline std::vector<std::string> defaultAudioSearchRoots(const std::string& exeDir = "") {
    std::vector<std::string> roots;
    if (const char* env = std::getenv("AQULIFY_CLAIM_AUDIO_DIR"); env && env[0] != '\0') {
        roots.emplace_back(env);
    }
    if (!exeDir.empty()) {
        roots.push_back((std::filesystem::path(exeDir) / "claim_audio").string());
        roots.push_back((std::filesystem::path(exeDir) / ".." / "share" / "aqulify" / "claim_audio").string());
    }
#if defined(__linux__)
    roots.emplace_back("/usr/share/aqulify/claim_audio");
    roots.emplace_back("/usr/local/share/aqulify/claim_audio");
#endif
    return roots;
}

} // namespace ee_analyzer::claim_audio
