#pragma once
/**
 * @file bass_enhancer.hpp
 * @brief Pszichoakusztikai mélyhang-kiemelő ("hiányzó alaphang" hatás) membránkímélő mélyvágással.
 *
 * Működés csatornánként:
 *   1. A keresztváltási frekvencia alatti sáv leválasztása (4. rendű aluláteresztő).
 *   2. Burkoló-normalizált Csebisev-polinomokkal tiszta 2. / 3. felharmonikusok előállítása
 *      (T2 = 2u^2 - 1, T3 = 4u^3 - 3u), majd visszaskálázás a burkolóval -> a felharmonikusok
 *      szintje arányos marad a mély tartalommal, nincs "zúgás" csendben.
 *   3. A felharmonikusok sávszűrése (keresztváltás ... ~5x keresztváltás), és hozzákeverés.
 *   4. Mélyvágás (4. rendű felüláteresztő) a kimeneten: a kis membrán nem kapja meg a hasznavehetetlen
 *      szubbasszust, a fül viszont a felharmonikusokból "kiegészíti" az alaphangot.
 */
#include <algorithm>
#include <cmath>
#include "biquad_analyzer.hpp"
#include "dsp_pipeline.hpp"

namespace ee_analyzer {

class BassEnhancer {
public:
    enum class Harmonics : int { Both = 0, Second = 1, Third = 2 };

    bool enabled = false;
    double sampleRate = 48000.0;
    double crossoverHz = 80.0;
    double harmonicsDb = 3.0;   // 0..12 dB "dúsítás"
    Harmonics type = Harmonics::Both;
    double subcutHz = 30.0;

    void update() {
        const double sr = sampleRate;
        const double xo = std::clamp(crossoverHz, 20.0, sr * 0.2);
        lp_ = BiquadFilter(FilterType::LowPass, xo, 0.7071, 0.0, sr).calculateCoefficients();
        hpH_ = BiquadFilter(FilterType::HighPass, xo, 0.7071, 0.0, sr).calculateCoefficients();
        lpH_ = BiquadFilter(FilterType::LowPass, std::min(xo * 5.0, sr * 0.45), 0.7071, 0.0, sr).calculateCoefficients();
        sub_ = BiquadFilter(FilterType::HighPass, std::clamp(subcutHz, 10.0, xo), 0.7071, 0.0, sr).calculateCoefficients();
        amount_ = static_cast<float>(std::pow(10.0, std::clamp(harmonicsDb, 0.0, 18.0) / 20.0) - 1.0);
        envRelease_ = static_cast<float>(std::exp(-1.0 / (0.030 * sr))); // 30 ms lecsengés
    }

    void reset() {
        for (auto& c : ch_) c = Channel{};
    }

    void process(float* L, float* R, size_t n) {
        if (!enabled) return;
        processChannel(ch_[0], L, n);
        processChannel(ch_[1], R, n);
    }

private:
    struct Channel {
        BiquadState lp1, lp2, hp1, hp2, lpH, sub1, sub2;
        float env = 0.0f;
    };

    void processChannel(Channel& c, float* x, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            const float in = x[i];
            const float low = c.lp2.process(c.lp1.process(in, lp_), lp_);
            const float a = std::abs(low);
            c.env = a > c.env ? a : a + envRelease_ * (c.env - a);
            const float u = std::clamp(low / (c.env + 1e-9f), -1.0f, 1.0f);
            const float t2 = 2.0f * u * u - 1.0f;
            const float t3 = (4.0f * u * u - 3.0f) * u;
            float h;
            switch (type) {
                case Harmonics::Second: h = t2; break;
                case Harmonics::Third: h = t3; break;
                default: h = 0.6f * t2 + 0.4f * t3; break;
            }
            h *= c.env;
            h = c.hp2.process(c.hp1.process(h, hpH_), hpH_);
            h = c.lpH.process(h, lpH_);
            float y = in + amount_ * h;
            y = c.sub2.process(c.sub1.process(y, sub_), sub_);
            x[i] = y;
        }
    }

    BiquadCoefficients lp_, hpH_, lpH_, sub_;
    float amount_ = 0.41f;
    float envRelease_ = 0.999f;
    Channel ch_[2];
};

} // namespace ee_analyzer
