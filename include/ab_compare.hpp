#pragma once
/**
 * @file ab_compare.hpp
 * @brief A/B összehasonlítás egy modullal (EQ, Konvolver, Bass Enhancer vagy Mono): a feldolgozott és az eredeti jel
 *        hangerőre kiegyenlítve.
 *
 * A modul mindig fut, az eredeti jel a modul bemenetéből jön, a modul késleltetésével (így a két ág időben illeszkedik).
 * A szintkülönbséget a két ág futó RMS-aránya adja, így az eredeti ugyanakkora hangerőn szól, mint a feldolgozott.
 * Az átkapcsolás 20 ms alatt simul, nincs kattanás.
 */
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace ee_analyzer {
namespace abcmp {

constexpr size_t kRing = 32768;           // 2 hatványa; 192 kHz-en is elég a 128 ms-os FIR-késleltetéshez
constexpr double kRampMs = 20.0;          // az A/B áttűnés hossza
constexpr double kLevelMs = 300.0;        // a szintkiegyenlítés időállandója

class AbCompare {
public:
    void setSampleRate(double sr, size_t maxBlock) {
        sr_ = sr > 0.0 ? sr : 48000.0;
        rampStep_ = 1.0 / (kRampMs * 0.001 * sr_);
        levelCoef_ = 1.0 / (kLevelMs * 0.001 * sr_);
        ringL_.assign(kRing, 0.0f);
        ringR_.assign(kRing, 0.0f);
        dryL_.assign(maxBlock, 0.0f);
        dryR_.assign(maxBlock, 0.0f);
        reset();
    }

    /** Új modulállapot: a késleltető vonal és a szintmérés üres. Az áttűnés állapota marad. */
    void reset() {
        std::fill(ringL_.begin(), ringL_.end(), 0.0f);
        std::fill(ringR_.begin(), ringR_.end(), 0.0f);
        wpos_ = 0;
        ew_ = ed_ = 0.0;
        gain_ = 1.0;
    }

    /**
     * A modul feldolgozása egy blokkra. A `stage` a bemeneten (L, R) helyben futtatja a modult. compare = true: az eredeti
     * (a `delay` mintával késleltetett, szintre igazított) jel szól; különben a feldolgozott.
     */
    template <class Stage>
    void process(float* L, float* R, size_t n, size_t delay, bool compare, Stage&& stage) {
        if (n == 0) return;
        delay = std::min(delay, kRing - n);
        // Eredeti jel a késleltető vonalba, majd a késleltetett másolat
        for (size_t i = 0; i < n; ++i) {
            ringL_[(wpos_ + i) & (kRing - 1)] = L[i];
            ringR_[(wpos_ + i) & (kRing - 1)] = R[i];
        }
        for (size_t i = 0; i < n; ++i) {
            const size_t idx = (wpos_ + kRing + i - delay) & (kRing - 1);
            dryL_[i] = ringL_[idx];
            dryR_[i] = ringR_[idx];
        }
        wpos_ = (wpos_ + n) & (kRing - 1);

        stage(L, R, n);

        // Futó szintmérés: a feldolgozott és az eredeti átlagos négyzetes teljesítménye
        for (size_t i = 0; i < n; ++i) {
            const double pw = 0.5 * (static_cast<double>(L[i]) * L[i] + static_cast<double>(R[i]) * R[i]);
            const double pd = 0.5 * (static_cast<double>(dryL_[i]) * dryL_[i] + static_cast<double>(dryR_[i]) * dryR_[i]);
            ew_ += levelCoef_ * (pw - ew_);
            ed_ += levelCoef_ * (pd - ed_);
        }
        gain_ = std::sqrt((ew_ + 1e-12) / (ed_ + 1e-12));

        const double target = compare ? 1.0 : 0.0;
        if (mix_ == 0.0 && target == 0.0) return;   // az eredeti ág nem szól: a kimenet a feldolgozott jel
        const double g = gain_;
        for (size_t i = 0; i < n; ++i) {
            if (mix_ != target) mix_ = mix_ < target ? std::min(target, mix_ + rampStep_) : std::max(target, mix_ - rampStep_);
            L[i] = static_cast<float>((1.0 - mix_) * L[i] + mix_ * g * dryL_[i]);
            R[i] = static_cast<float>((1.0 - mix_) * R[i] + mix_ * g * dryR_[i]);
        }
    }

private:
    double sr_ = 48000.0;
    double rampStep_ = 0.0, levelCoef_ = 0.0;
    std::vector<float> ringL_, ringR_, dryL_, dryR_;
    size_t wpos_ = 0;
    double ew_ = 0.0, ed_ = 0.0, gain_ = 1.0;
    double mix_ = 0.0;
};

} // namespace abcmp
} // namespace ee_analyzer
