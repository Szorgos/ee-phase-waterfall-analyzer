#pragma once
/**
 * @file multi_output_config.hpp
 * @brief A több kimenet beállításai (a DSP-lánc nélkül is használható: híd, platformrész, tesztek).
 * A feldolgozás: multi_output.hpp.
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "param_block.hpp"

namespace ee_analyzer {
namespace multiout {

constexpr int kMaxOutputs = 4;
constexpr int kMaxCrossovers = kMaxOutputs - 1;
constexpr double kMaxDelayMs = 50.0;          // legfeljebb ~17 m távolságkülönbség

enum class Mode : int32_t { Zones = 0, Crossover = 1 };

/** Egy kimenet beállításai. */
struct OutputConfig {
    bool enabled = true;
    int32_t band = -1;          // keresztváltóban: 0 = legalsó sáv … (vágási pontok száma) = legfelső; -1 = teljes jel
    float gainDb = 0.0f;        // -24 … +12 dB
    float delayMs = 0.0f;       // 0 … 50 ms
    bool invert = false;        // polaritásfordítás (pl. fordítva bekötött hangszóró)
    ParamData chain = transparentParams();   // független zónában a kimenet saját lánca; keresztváltóban csak az 1.-é él
};

struct MultiConfig {
    bool enabled = false;                            // a motor használja: kikapcsolva csak az 1. kimenet szól (a régi út)
    Mode mode = Mode::Zones;
    int32_t numOutputs = 2;                          // 2–4
    int32_t numCrossovers = 1;                       // keresztváltóban 1–3 vágási pont (2–4 sáv)
    std::array<float, kMaxCrossovers> crossoverHz{ { 80.0f, 2500.0f, 8000.0f } };
    int32_t slopeDbPerOct = 24;                      // 12 / 24 / 48
    std::array<OutputConfig, kMaxOutputs> outputs{};
};

/** Érvényes tartományba igazítás (a felületről vagy fájlból érkező beállításra). */
inline void sanitize(MultiConfig& c) {
    c.mode = c.mode == Mode::Crossover ? Mode::Crossover : Mode::Zones;
    c.numOutputs = std::clamp(c.numOutputs, 2, kMaxOutputs);
    c.numCrossovers = std::clamp(c.numCrossovers, 1, kMaxCrossovers);
    if (c.slopeDbPerOct != 12 && c.slopeDbPerOct != 48) c.slopeDbPerOct = 24;
    for (auto& f : c.crossoverHz) if (!std::isfinite(f)) f = 1000.0f;
    for (auto& f : c.crossoverHz) f = std::clamp(f, 20.0f, 20000.0f);
    std::sort(c.crossoverHz.begin(), c.crossoverHz.begin() + c.numCrossovers);
    for (auto& o : c.outputs) {
        if (!std::isfinite(o.gainDb)) o.gainDb = 0.0f;
        if (!std::isfinite(o.delayMs)) o.delayMs = 0.0f;
        o.gainDb = std::clamp(o.gainDb, -24.0f, 12.0f);
        o.delayMs = std::clamp(o.delayMs, 0.0f, static_cast<float>(kMaxDelayMs));
        o.band = std::clamp(o.band, -1, c.numCrossovers);
        sanitizeParams(o.chain);
    }
}

} // namespace multiout
} // namespace ee_analyzer
