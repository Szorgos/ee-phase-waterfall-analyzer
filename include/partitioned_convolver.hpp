#pragma once
/**
 * @file partitioned_convolver.hpp
 * @brief Zéró többletkésleltetésű, egyenletesen partícionált FFT konvolver (sztereó).
 *
 * Felépítés:
 *   - "fej": az impulzusválasz első B mintája közvetlen (időtartománybeli) konvolúcióval -> nincs késleltetés;
 *   - "farok": a többi B hosszú partíció overlap-save FFT-vel (UPOLS), frekvenciatartománybeli késleltetővonallal.
 *     A farok eredménye a következő blokk elején már kész, így a teljes kimenet minta-pontosan
 *     megegyezik a közvetlen konvolúcióval, de a költség jóval kisebb.
 *   - A bal és jobb csatornát egyetlen komplex FFT-be csomagolja (L + iR), így blokkonként
 *     csak egy előre és egy inverz FFT kell.
 *
 * build() foglal memóriát (háttérszálban hívandó), process() valós idejű szálból is hívható.
 */
#include <algorithm>
#include <complex>
#include <cstddef>
#include <vector>
#include "fft.hpp"

namespace ee_analyzer {

class PartitionedConvolver {
public:
    static constexpr size_t B = 256;       // partícióméret (és a közvetlen fej hossza)
    static constexpr size_t N = 2 * B;     // FFT méret
    static constexpr size_t NB = N / 2 + 1; // hasznos frekvenciabinek
    using cpx = std::complex<float>;

    /**
     * @param irL bal (vagy mono) impulzusválasz
     * @param irR jobb impulzusválasz; üres -> a bal csatornáé mindkét csatornára
     */
    bool build(const std::vector<float>& irL, const std::vector<float>& irR) {
        const std::vector<float>& r = irR.empty() ? irL : irR;
        length_ = std::max(irL.size(), r.size());
        if (length_ == 0) return false;
        fft_.init(N);

        // Fej (fordított sorrendben a gyors skaláris szorzathoz)
        headL_.assign(B, 0.0f);
        headR_.assign(B, 0.0f);
        for (size_t i = 0; i < B; ++i) {
            if (i < irL.size()) headL_[B - 1 - i] = irL[i];
            if (i < r.size()) headR_[B - 1 - i] = r[i];
        }

        // Farok partíciók spektrumai
        parts_ = length_ > B ? (length_ - B + B - 1) / B : 0;
        HL_.assign(parts_ * NB, cpx(0, 0));
        HR_.assign(parts_ * NB, cpx(0, 0));
        std::vector<cpx> tmp(N);
        for (size_t k = 0; k < parts_; ++k) {
            const size_t off = (k + 1) * B;
            std::fill(tmp.begin(), tmp.end(), cpx(0, 0));
            for (size_t i = 0; i < B; ++i) {
                const float a = off + i < irL.size() ? irL[off + i] : 0.0f;
                const float b = off + i < r.size() ? r[off + i] : 0.0f;
                tmp[i] = cpx(a, b); // csomagolva: L valós, R képzetes
            }
            fft_.transform(tmp.data(), false);
            unpack(tmp.data(), &HL_[k * NB], &HR_[k * NB]);
        }

        fdlL_.assign(std::max<size_t>(parts_, 1) * NB, cpx(0, 0));
        fdlR_.assign(std::max<size_t>(parts_, 1) * NB, cpx(0, 0));
        work_.assign(N, cpx(0, 0));
        accL_.assign(NB, cpx(0, 0));
        accR_.assign(NB, cpx(0, 0));
        histL_.assign(2 * B, 0.0f);
        histR_.assign(2 * B, 0.0f);
        winL_.assign(N, 0.0f);
        winR_.assign(N, 0.0f);
        tailL_.assign(B, 0.0f);
        tailR_.assign(B, 0.0f);
        reset();
        return true;
    }

    size_t length() const { return length_; }

    void reset() {
        std::fill(fdlL_.begin(), fdlL_.end(), cpx(0, 0));
        std::fill(fdlR_.begin(), fdlR_.end(), cpx(0, 0));
        std::fill(histL_.begin(), histL_.end(), 0.0f);
        std::fill(histR_.begin(), histR_.end(), 0.0f);
        std::fill(winL_.begin(), winL_.end(), 0.0f);
        std::fill(winR_.begin(), winR_.end(), 0.0f);
        std::fill(tailL_.begin(), tailL_.end(), 0.0f);
        std::fill(tailR_.begin(), tailR_.end(), 0.0f);
        pos_ = 0;
        fdlHead_ = 0;
    }

    /** Helyben végzett sztereó konvolúció. Valós idejű szálból hívható. */
    void process(float* L, float* R, size_t n) {
        if (length_ == 0) return;
        for (size_t s = 0; s < n; ++s) {
            const float xl = L[s];
            const float xr = R[s];

            // Közvetlen fej: kettőzött history puffer -> folytonos ablak
            const size_t wp = hp_;
            histL_[wp] = xl; histL_[wp + B] = xl;
            histR_[wp] = xr; histR_[wp + B] = xr;
            const float* wl = &histL_[wp + 1];
            const float* wr = &histR_[wp + 1];
            float yl = 0.0f, yr = 0.0f;
            for (size_t j = 0; j < B; ++j) {
                yl += headL_[j] * wl[j];
                yr += headR_[j] * wr[j];
            }
            hp_ = (hp_ + 1) & (B - 1);

            // Az aktuális blokk mintája az FFT ablak második felébe
            winL_[B + pos_] = xl;
            winR_[B + pos_] = xr;

            L[s] = yl + tailL_[pos_];
            R[s] = yr + tailR_[pos_];

            if (++pos_ == B) {
                pos_ = 0;
                blockDone();
            }
        }
    }

private:
    /** Z = FFT(l + i r) -> külön L és R spektrum (0..N/2). */
    static void unpack(const cpx* Z, cpx* outL, cpx* outR) {
        for (size_t k = 0; k < NB; ++k) {
            const cpx a = Z[k];
            const cpx b = std::conj(Z[(N - k) & (N - 1)]);
            outL[k] = (a + b) * 0.5f;
            outR[k] = (a - b) * cpx(0.0f, -0.5f);
        }
    }

    void blockDone() {
        if (parts_ > 0) {
            // 1. Az utolsó 2B bemeneti minta spektruma a késleltetővonal elejére
            for (size_t i = 0; i < N; ++i) work_[i] = cpx(winL_[i], winR_[i]);
            fft_.transform(work_.data(), false);
            fdlHead_ = (fdlHead_ + parts_ - 1) % parts_;
            unpack(work_.data(), &fdlL_[fdlHead_ * NB], &fdlR_[fdlHead_ * NB]);

            // 2. Y = sum_k X_{m-k} * H_{k+1}
            std::fill(accL_.begin(), accL_.end(), cpx(0, 0));
            std::fill(accR_.begin(), accR_.end(), cpx(0, 0));
            for (size_t k = 0; k < parts_; ++k) {
                const size_t slot = (fdlHead_ + k) % parts_;
                const cpx* xl = &fdlL_[slot * NB];
                const cpx* xr = &fdlR_[slot * NB];
                const cpx* hl = &HL_[k * NB];
                const cpx* hr = &HR_[k * NB];
                for (size_t b = 0; b < NB; ++b) {
                    accL_[b] += xl[b] * hl[b];
                    accR_[b] += xr[b] * hr[b];
                }
            }

            // 3. Visszacsomagolás (YL + i YR) és egyetlen inverz FFT
            for (size_t k = 0; k < NB; ++k) work_[k] = accL_[k] + cpx(0.0f, 1.0f) * accR_[k];
            for (size_t k = NB; k < N; ++k) {
                work_[k] = std::conj(accL_[N - k]) + cpx(0.0f, 1.0f) * std::conj(accR_[N - k]);
            }
            fft_.transform(work_.data(), true);
            for (size_t i = 0; i < B; ++i) {
                tailL_[i] = work_[B + i].real();
                tailR_[i] = work_[B + i].imag();
            }
        }
        // Ablak léptetése: az aktuális blokk lesz az "előző"
        std::copy(winL_.begin() + B, winL_.end(), winL_.begin());
        std::copy(winR_.begin() + B, winR_.end(), winR_.begin());
    }

    Fft<float> fft_;
    size_t length_ = 0;
    size_t parts_ = 0;
    std::vector<float> headL_, headR_;
    std::vector<cpx> HL_, HR_, fdlL_, fdlR_, work_, accL_, accR_;
    std::vector<float> histL_, histR_, winL_, winR_, tailL_, tailR_;
    size_t pos_ = 0, hp_ = 0, fdlHead_ = 0;
};

} // namespace ee_analyzer
