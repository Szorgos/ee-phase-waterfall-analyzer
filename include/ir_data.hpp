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

} // namespace ee_analyzer
