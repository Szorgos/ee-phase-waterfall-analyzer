#pragma once
/**
 * @file true_peak_limiter.hpp
 * @brief Előretekintő (lookahead) true peak limiter, túlmintavételezett csúcsdetektorral.
 *
 * Felépítés:
 *  1. Csúcsdetektor: minden mintaközt a Lanczos (a = 3) interpolációval 2x / 4x / 8x felbontásban
 *     megvizsgál, így a D/A átalakítás utáni mintaközi csúcsokat (ISP) is látja (1x = mintaérték-csúcs).
 *  2. Szükséges erősítés: g = min(1, plafon / csúcs).
 *  3. Csúcstartás (L+1 minta) + mozgóátlag (L+1 minta): a hangolás garantáltan elkészül, mire
 *     a késleltetett hang a csúcshoz ér, és a beavatkozás sima (nincs "kattanás").
 *  4. Felengedés exponenciális, a megadott időállandóval.
 *  5. Biztonsági vágás a plafonnál (a Lanczos becslés maradék hibájára).
 *
 * Késleltetés: lookahead + 3 minta (a detektor interpolációs ablaka). Memóriát csak a prepare() foglal.
 */
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace ee_analyzer {

class TruePeakLimiter {
public:
    bool enabled = false;

    /** Nem valós idejű: a maximális lookaheadhez (ms) szükséges pufferek lefoglalása. */
    void prepare(double sampleRate, double maxLookaheadMs = 10.0) {
        sr_ = sampleRate;
        cap_ = static_cast<size_t>(std::ceil(maxLookaheadMs * 1e-3 * sampleRate)) + 8;
        delayL_.assign(cap_ + 8, 0.0f);
        delayR_.assign(cap_ + 8, 0.0f);
        dqIdx_.assign(cap_ + 8, 0);
        dqVal_.assign(cap_ + 8, 1.0f);
        avgBuf_.assign(cap_ + 8, 1.0f);
        configure(ceilingDb_, releaseMs_, lookaheadMs_, oversample_);
        reset();
    }

    /**
     * Paraméterek (valós idejű szálból is hívható, foglalás nélkül).
     * @param oversample 1, 2, 4 vagy 8
     */
    void configure(double ceilingDb, double releaseMs, double lookaheadMs, int oversample) {
        ceilingDb_ = std::clamp(ceilingDb, -24.0, 0.0);
        ceiling_ = static_cast<float>(std::pow(10.0, ceilingDb_ / 20.0));
        releaseMs_ = std::clamp(releaseMs, 1.0, 2000.0);
        relCoeff_ = static_cast<float>(std::exp(-1.0 / (releaseMs_ * 1e-3 * sr_)));
        const int os = oversample >= 8 ? 8 : oversample >= 4 ? 4 : oversample >= 2 ? 2 : 1;
        if (os != oversample_) {
            oversample_ = os;
            buildInterpolator();
        }
        const double la = std::clamp(lookaheadMs, 0.0, 10.0);
        size_t L = static_cast<size_t>(std::lround(la * 1e-3 * sr_));
        L = std::clamp<size_t>(L, 1, cap_ > 8 ? cap_ - 8 : 1);
        if (L != L_ || la != lookaheadMs_) {
            lookaheadMs_ = la;
            if (L != L_) {
                L_ = L;
                reset();
            }
        }
    }

    void reset() {
        std::fill(delayL_.begin(), delayL_.end(), 0.0f);
        std::fill(delayR_.begin(), delayR_.end(), 0.0f);
        std::fill(avgBuf_.begin(), avgBuf_.end(), 1.0f);
        for (float& v : histL_) v = 0.0f;
        for (float& v : histR_) v = 0.0f;
        dqHead_ = dqTail_ = 0;
        avgSum_ = static_cast<double>(L_ + 1);
        avgPos_ = 0;
        wpos_ = 0;
        sampleIdx_ = 0;
        gain_ = 1.0f;
        minGain_ = 1.0f;
        lastMinGain_ = 1.0f;
        meterGr_ = 1.0f;
    }

    size_t latencySamples() const { return L_ + 3; }

    /** A legutóbbi blokk legnagyobb erősítéscsökkentése dB-ben (negatív vagy 0). */
    double gainReductionDb() const { return 20.0 * std::log10(std::max(lastMinGain_, 1e-6f)); }

    void process(float* L, float* R, size_t n) {
        if (!enabled) return;
        const size_t W = L_ + 1;           // csúcstartás és átlagolás hossza
        const size_t D = L_ + 3;           // teljes késleltetés
        const size_t ring = delayL_.size();
        const float inv = 1.0f / static_cast<float>(W);
        float blockMin = 1.0f;
        for (size_t i = 0; i < n; ++i) {
            const float xl = L[i], xr = R[i];

            // 1. interpolációs ablak: hist[0..5] = x[m-2 .. m+3], m = aktuális - 3
            for (int k = 0; k < 5; ++k) { histL_[k] = histL_[k + 1]; histR_[k] = histR_[k + 1]; }
            histL_[5] = xl;
            histR_[5] = xr;
            float peak = std::max(std::abs(histL_[2]), std::abs(histR_[2]));
            for (int ph = 0; ph < oversample_ - 1; ++ph) {
                const float* c = coeff_[ph];
                float il = 0.0f, ir = 0.0f;
                for (int k = 0; k < 6; ++k) { il += c[k] * histL_[k]; ir += c[k] * histR_[k]; }
                peak = std::max(peak, std::max(std::abs(il), std::abs(ir)));
            }
            const float g = peak > ceiling_ ? ceiling_ / peak : 1.0f;

            // 2. csúcstartás: csúszóablakos minimum (monoton sor) W hosszon
            const size_t cap = dqIdx_.size();
            while (dqTail_ != dqHead_ && dqVal_[(dqTail_ + cap - 1) % cap] >= g) dqTail_ = (dqTail_ + cap - 1) % cap;
            dqIdx_[dqTail_] = sampleIdx_;
            dqVal_[dqTail_] = g;
            dqTail_ = (dqTail_ + 1) % cap;
            while (dqIdx_[dqHead_] + W <= sampleIdx_) dqHead_ = (dqHead_ + 1) % cap;
            const float held = dqVal_[dqHead_];

            // 3. mozgóátlag W hosszon
            avgSum_ += held - avgBuf_[avgPos_];
            avgBuf_[avgPos_] = held;
            avgPos_ = (avgPos_ + 1) % W;
            const float target = std::min(1.0f, static_cast<float>(avgSum_) * inv);

            // 4. azonnali beavatkozás, exponenciális felengedés
            gain_ = target < gain_ ? target : target + relCoeff_ * (gain_ - target);
            blockMin = std::min(blockMin, gain_);

            // 5. késleltetett jel * erősítés, biztonsági vágás
            delayL_[wpos_] = xl;
            delayR_[wpos_] = xr;
            const size_t rpos = (wpos_ + ring - D) % ring;
            wpos_ = (wpos_ + 1) % ring;
            L[i] = std::clamp(delayL_[rpos] * gain_, -ceiling_, ceiling_);
            R[i] = std::clamp(delayR_[rpos] * gain_, -ceiling_, ceiling_);
            ++sampleIdx_;
        }
        lastMinGain_ = blockMin;
        // a mozgóátlag lebegőpontos sodródásának megakadályozása
        if ((sampleIdx_ & 0xFFFF) < n) {
            double s = 0.0;
            for (size_t k = 0; k < W; ++k) s += avgBuf_[k];
            avgSum_ = s;
        }
    }

private:
    void buildInterpolator() {
        const double pi = 3.14159265358979323846;
        auto sinc = [pi](double x) { return std::abs(x) < 1e-12 ? 1.0 : std::sin(pi * x) / (pi * x); };
        for (int ph = 0; ph < oversample_ - 1; ++ph) {
            const double t = static_cast<double>(ph + 1) / oversample_; // pozíció x[m] és x[m+1] között
            double sum = 0.0;
            double c[6];
            for (int k = 0; k < 6; ++k) {
                const double z = t - static_cast<double>(k - 2); // a minta helye: m + (k-2)
                c[k] = std::abs(z) < 3.0 ? sinc(z) * sinc(z / 3.0) : 0.0;
                sum += c[k];
            }
            for (int k = 0; k < 6; ++k) coeff_[ph][k] = static_cast<float>(c[k] / sum);
        }
    }

    double sr_ = 48000.0;
    double ceilingDb_ = -1.0, releaseMs_ = 50.0, lookaheadMs_ = 5.0;
    int oversample_ = 0; // 0 = még nincs interpolátor (az első configure() építi)
    float ceiling_ = 0.891f, relCoeff_ = 0.999f;
    size_t cap_ = 0, L_ = 240;
    float coeff_[7][6] = {};
    float histL_[6] = {}, histR_[6] = {};
    std::vector<float> delayL_, delayR_, dqVal_, avgBuf_;
    std::vector<size_t> dqIdx_;
    size_t dqHead_ = 0, dqTail_ = 0, avgPos_ = 0, wpos_ = 0, sampleIdx_ = 0;
    double avgSum_ = 0.0;
    float gain_ = 1.0f, minGain_ = 1.0f, lastMinGain_ = 1.0f, meterGr_ = 1.0f;
};

} // namespace ee_analyzer
