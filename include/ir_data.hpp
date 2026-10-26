#pragma once
/**
 * @file ir_data.hpp
 * @brief Konvolver impulzusválasz tároló + segédfüggvények (előnézet, bináris üzenet formátum).
 */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ee_analyzer {

struct IrData {
    uint32_t sampleRate = 0;
    std::string name;
    std::vector<float> left;   // mono IR esetén csak ez
    std::vector<float> right;  // üres = mono

    bool empty() const { return left.empty(); }
    uint32_t channels() const { return left.empty() ? 0u : (right.empty() ? 1u : 2u); }
    size_t frames() const { return left.size(); }

    static IrData fromInterleaved(const float* data, uint32_t frames, uint32_t channels, uint32_t sr, std::string nm) {
        IrData ir;
        ir.sampleRate = sr;
        ir.name = std::move(nm);
        if (!data || frames == 0 || channels == 0) return ir;
        ir.left.resize(frames);
        if (channels >= 2) ir.right.resize(frames);
        for (uint32_t i = 0; i < frames; ++i) {
            ir.left[i] = data[static_cast<size_t>(i) * channels];
            if (channels >= 2) ir.right[i] = data[static_cast<size_t>(i) * channels + 1];
        }
        return ir;
    }

    /** Burkológörbe-előnézet (abszolút csúcs dB-ben, -60..0 -> 0..1) a GUI-nak. */
    std::vector<float> preview(size_t points = 400) const {
        std::vector<float> out;
        if (empty()) return out;
        const size_t n = frames();
        points = std::min(points, n);
        out.resize(points, 0.0f);
        float peak = 1e-9f;
        for (size_t i = 0; i < n; ++i) {
            peak = std::max(peak, std::abs(left[i]));
            if (!right.empty()) peak = std::max(peak, std::abs(right[i]));
        }
        for (size_t p = 0; p < points; ++p) {
            const size_t a = p * n / points, b = std::max(a + 1, (p + 1) * n / points);
            float m = 0.0f;
            for (size_t i = a; i < b && i < n; ++i) {
                m = std::max(m, std::abs(left[i]));
                if (!right.empty()) m = std::max(m, std::abs(right[i]));
            }
            const float db = 20.0f * std::log10(std::max(m / peak, 1e-6f));
            out[p] = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
        }
        return out;
    }
};

/**
 * Bináris WebSocket üzenet a GUI-tól (kis-endián):
 *   "AEIR" | u32 verzió(1) | u32 sr | u32 csatornák | u32 frame-ek | u32 névhossz | név (UTF-8) | float32 interleaved minták
 */
inline bool parseIrMessage(const std::string& msg, IrData& out, uint32_t maxFrames) {
    auto rd = [&](size_t off) {
        uint32_t v;
        std::memcpy(&v, msg.data() + off, 4);
        return v;
    };
    if (msg.size() < 24 || std::memcmp(msg.data(), "AEIR", 4) != 0 || rd(4) != 1) return false;
    const uint32_t sr = rd(8), ch = rd(12), frames = rd(16), nameLen = rd(20);
    if (sr < 8000 || sr > 768000 || ch < 1 || ch > 8 || frames == 0 || nameLen > 1024) return false;
    const size_t dataOff = 24 + nameLen;
    if (msg.size() < dataOff + static_cast<size_t>(frames) * ch * 4) return false;
    const uint32_t useFrames = std::min(frames, maxFrames);
    std::vector<float> inter(static_cast<size_t>(useFrames) * ch);
    // csak az első 2 csatornát használjuk
    std::memcpy(inter.data(), msg.data() + dataOff, inter.size() * 4);
    std::vector<float> st(static_cast<size_t>(useFrames) * std::min<uint32_t>(ch, 2));
    const uint32_t outCh = std::min<uint32_t>(ch, 2);
    for (uint32_t i = 0; i < useFrames; ++i)
        for (uint32_t c = 0; c < outCh; ++c) st[static_cast<size_t>(i) * outCh + c] = inter[static_cast<size_t>(i) * ch + c];
    out = IrData::fromInterleaved(st.data(), useFrames, outCh, sr, msg.substr(24, nameLen));
    // Hibás (NaN/Inf) minták kiszűrése
    for (auto* v : { &out.left, &out.right })
        for (float& x : *v) if (!std::isfinite(x)) x = 0.0f;
    return true;
}

// ---------------------------------------------------------------- IR-tár (IR-profilok)

/** IR-tár azonosító: 1–32 karakter, csak 0-9 és a-z (fájlnévként is biztonságos). */
inline bool isValidIrId(const std::string& id) {
    return !id.empty() && id.size() <= 32 &&
           std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'); });
}

constexpr uint32_t kIrFileMagicAirf = 0x46524941; // 'AIRF' – az ir.bin formátuma (Linux, macOS, Windows)

/** Az ir.bin formátuma bájtokként: 'AIRF' | 1 | sr | csatornák | frame-ek | névhossz | név | bal | jobb. */
inline std::string encodeIrFile(const IrData& ir) {
    if (ir.empty()) return {};
    const uint32_t hdr[6] = { kIrFileMagicAirf, 1u, ir.sampleRate, ir.channels(), static_cast<uint32_t>(ir.frames()),
                              static_cast<uint32_t>(ir.name.size()) };
    std::string b(sizeof(hdr) + ir.name.size() + ir.frames() * 4u * ir.channels(), '\0');
    char* p = b.data();
    std::memcpy(p, hdr, sizeof(hdr)); p += sizeof(hdr);
    std::memcpy(p, ir.name.data(), ir.name.size()); p += ir.name.size();
    std::memcpy(p, ir.left.data(), ir.frames() * 4u); p += ir.frames() * 4u;
    if (!ir.right.empty()) std::memcpy(p, ir.right.data(), ir.frames() * 4u);
    return b;
}

inline bool decodeIrFile(const std::string& b, IrData& out, uint32_t maxFrames) {
    uint32_t hdr[6] = {};
    if (b.size() < sizeof(hdr)) return false;
    std::memcpy(hdr, b.data(), sizeof(hdr));
    if (hdr[0] != kIrFileMagicAirf || hdr[1] != 1u || (hdr[3] != 1 && hdr[3] != 2) || hdr[4] == 0 ||
        hdr[4] > maxFrames || hdr[5] >= 4096 || hdr[2] < 8000 || hdr[2] > 768000)
        return false;
    const size_t frames = hdr[4], need = sizeof(hdr) + hdr[5] + frames * 4u * hdr[3];
    if (b.size() < need) return false;
    IrData ir;
    ir.sampleRate = hdr[2];
    const char* p = b.data() + sizeof(hdr);
    ir.name.assign(p, hdr[5]); p += hdr[5];
    ir.left.resize(frames);
    std::memcpy(ir.left.data(), p, frames * 4u); p += frames * 4u;
    if (hdr[3] == 2) {
        ir.right.resize(frames);
        std::memcpy(ir.right.data(), p, frames * 4u);
    }
    for (auto* v : { &ir.left, &ir.right })
        for (float& x : *v) if (!std::isfinite(x)) x = 0.0f;
    out = std::move(ir);
    return true;
}

/**
 * IR mentése a tárba (bináris WebSocket üzenet a GUI-tól, kis-endián):
 *   "AEIS" | u32 verzió(1) | u32 azonosítóhossz | azonosító | teljes "AEIR" üzenet
 */
inline bool parseIrStoreMessage(const std::string& msg, std::string& id, IrData& out, uint32_t maxFrames) {
    if (msg.size() < 12 || std::memcmp(msg.data(), "AEIS", 4) != 0) return false;
    uint32_t ver, idLen;
    std::memcpy(&ver, msg.data() + 4, 4);
    std::memcpy(&idLen, msg.data() + 8, 4);
    if (ver != 1 || idLen == 0 || idLen > 32 || msg.size() < 12 + static_cast<size_t>(idLen)) return false;
    id = msg.substr(12, idLen);
    return isValidIrId(id) && parseIrMessage(msg.substr(12 + idLen), out, maxFrames);
}

} // namespace ee_analyzer
