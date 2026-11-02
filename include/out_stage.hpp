#pragma once
/**
 * @file out_stage.hpp
 * @brief Kimeneti fokozat (a lánc végén): csatornánkénti késleltetés, szint és polaritás (a hangszórók távolságának
 *        és szintjének kiegyenlítése, ha a hallgató nem középen ül), valamint dither a hangkártya bitmélységére.
 *
 * - Késleltetés: egész mintányi (48 kHz-en 1 minta ≈ 7 mm), legfeljebb 20 ms csatornánként.
 * - Szint: 5 ms alatt úszik az új értékre (nincs lépcső).
 * - Dither: TPDF (két egyenletes zaj különbsége, ±1 LSB), vagy zajformált (TPDF + elsőrendű hibavisszacsatolás: a
 *   kvantálási zaj a magas frekvenciák felé tolódik, a hallás legérzékenyebb sávjában halkabb). A kimenet a megadott
 *   bitmélység rácsára kerül, így a PipeWire / Core Audio egész számmá alakítása már nem kerekít tovább.
 *   A dither csak akkor fut, ha a lánc módosítja a jelet (process(..., modified)); érintetlen jelnél a kimenet bitpontos.
 */
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "param_block.hpp"

namespace ee_analyzer::outstage {

class OutStage {
public:
    static constexpr float kMaxDelayMs = 20.0f;

    void setSampleRate(uint32_t sr) {
        sr_ = sr > 0 ? sr : 48000;
        const size_t need = static_cast<size_t>(std::ceil(kMaxDelayMs * 1e-3 * 192000.0)) + 1;   // a legnagyobb mintavételhez
        for (auto& b : buf_) if (b.size() < need) b.assign(need, 0.0f);
        rampLen_ = std::max(1.0f, 0.005f * static_cast<float>(sr_));
        update(params_);
        for (int c = 0; c < 2; ++c) gain_[c] = target_[c];
        reset();
    }

    void reset() {
        for (auto& b : buf_) std::fill(b.begin(), b.end(), 0.0f);
        pos_ = 0;
        err_[0] = err_[1] = 0.0f;
    }

    /** Új beállítás (valós idejű szál, foglalás nélkül). A késleltetés változása a lánc úsztatott váltásán át jön. */
    void update(const ParamData& d) {
        params_ = d;
        for (int c = 0; c < 2; ++c) {
            delay_[c] = static_cast<size_t>(std::lround(std::clamp(d.chDelayMs[c], 0.0f, kMaxDelayMs) * 1e-3f * static_cast<float>(sr_)));
            float g = std::pow(10.0f, std::clamp(d.chGainDb[c], -24.0f, 6.0f) / 20.0f);
            if (d.chInvert[c]) g = -g;
            target_[c] = g;
            step_[c] = (target_[c] - gain_[c]) / rampLen_;
        }
        lsb_ = d.ditherBits == 16 ? 1.0f / 32768.0f : 1.0f / 8388608.0f;
    }

    /** A csatornák beállítása semleges-e (nincs késleltetés, 0 dB, nincs fordítás): ilyenkor a jel érintetlen. */
    static bool neutral(const ParamData& d) {
        return d.chDelayMs[0] <= 0.0f && d.chDelayMs[1] <= 0.0f && d.chGainDb[0] == 0.0f && d.chGainDb[1] == 0.0f &&
               d.chInvert[0] == 0 && d.chInvert[1] == 0;
    }

    /** A csatornakiegyenlítés (késleltetés, szint, polaritás). */
    void processChannels(float* L, float* R, size_t n) {
        float* ch[2] = { L, R };
        const size_t len = buf_[0].size();
        if (delay_[0] || delay_[1]) {
            for (size_t i = 0; i < n; ++i) {
                for (int c = 0; c < 2; ++c) {
                    buf_[c][pos_] = ch[c][i];
                    const size_t rd = (pos_ + len - delay_[c]) % len;
                    ch[c][i] = buf_[c][rd];
                }
                pos_ = (pos_ + 1) % len;
            }
        }
        for (int c = 0; c < 2; ++c) {
            float* x = ch[c];
            if (gain_[c] != target_[c]) {
                for (size_t i = 0; i < n; ++i) {
                    if (gain_[c] != target_[c]) {
                        gain_[c] += step_[c];
                        if ((step_[c] > 0.0f && gain_[c] >= target_[c]) || (step_[c] <= 0.0f && gain_[c] <= target_[c])) gain_[c] = target_[c];
                    }
                    x[i] *= gain_[c];
                }
            } else if (gain_[c] != 1.0f) {
                for (size_t i = 0; i < n; ++i) x[i] *= gain_[c];
            }
        }
    }

    /** Dither és kvantálás a beállított bitmélységre (a lánc legvégén). */
    void processDither(float* L, float* R, size_t n) {
        const int mode = params_.ditherMode;
        if (mode == 0) return;
        float* ch[2] = { L, R };
        const float lsb = lsb_, inv = 1.0f / lsb_;
        const float hi = 1.0f - lsb;
        for (int c = 0; c < 2; ++c) {
            float* x = ch[c];
            float e = err_[c];
            for (size_t i = 0; i < n; ++i) {
                const float tpdf = (uniform() - uniform()) * lsb;   // háromszög eloszlás, ±1 LSB
                const float v = mode == 2 ? x[i] - e : x[i];
                float y = std::nearbyint((v + tpdf) * inv) * lsb;
                y = std::clamp(y, -1.0f, hi);
                if (mode == 2) e = y - v;
                x[i] = y;
            }
            err_[c] = mode == 2 ? std::clamp(e, -4.0f * lsb, 4.0f * lsb) : 0.0f;
        }
    }

private:
    /** Egyenletes eloszlású szám [0, 1) között (xorshift32; valós idejű szálon is biztonságos). */
    float uniform() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return static_cast<float>(rng_ >> 8) * (1.0f / 16777216.0f);
    }

    ParamData params_ = transparentParams();
    uint32_t sr_ = 48000;
    std::vector<float> buf_[2];
    size_t pos_ = 0;
    size_t delay_[2] = { 0, 0 };
    float gain_[2] = { 1.0f, 1.0f }, target_[2] = { 1.0f, 1.0f }, step_[2] = { 0.0f, 0.0f };
    float rampLen_ = 240.0f;
    float lsb_ = 1.0f / 8388608.0f;
    float err_[2] = { 0.0f, 0.0f };
    uint32_t rng_ = 0x2545F491u;
};

} // namespace ee_analyzer::outstage
