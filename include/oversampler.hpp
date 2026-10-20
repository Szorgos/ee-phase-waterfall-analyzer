#pragma once
/**
 * @file oversampler.hpp
 * @brief Sztereó túlmintavételező (2x / 4x / 8x) az IIR EQ "decramping"-hez.
 *
 * A bilineáris transzformációval tervezett biquadok amplitúdómenete a Nyquist-frekvencia közelében
 * "összenyomódik" (cramping): pl. egy 16 kHz-es magas polc vagy harang 48 kHz-en torzul.
 * Ha a szűrőt N-szeres mintavételen futtatjuk, a menet a hallható sávban gyakorlatilag az analóg
 * prototípusé lesz. A fel- és lemintavételezés Kaiser-ablakos sinc FIR-rel történik
 * (áteresztősáv ~20 kHz-ig 44,1/48 kHz-en, ~-95 dB zárósáv), polifázisú megvalósításban.
 *
 * Minden memóriát a konstruktor foglal; a process() valós idejű szálból hívható.
 */
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace ee_analyzer {

class Oversampler {
public:
    static constexpr size_t kMaxBlock = 1024; // bemeneti minták / hívás

    Oversampler() {
        for (int i = 0; i < 3; ++i) {
            const int N = 2 << i;
            stages_[i].N = N;
            stages_[i].h = design(N);
            const size_t taps = stages_[i].h.size();
            stages_[i].upHistL.assign(taps / N + 2, 0.0f);
            stages_[i].upHistR.assign(taps / N + 2, 0.0f);
            stages_[i].dnHistL.assign(taps * 2, 0.0f);
            stages_[i].dnHistR.assign(taps * 2, 0.0f);
        }
        bufL_.assign(kMaxBlock * 8, 0.0f);
        bufR_.assign(kMaxBlock * 8, 0.0f);
    }

    /** Késleltetés eredeti mintavételi időben (minta). */
    double latencySamples(int factor) const {
        const Stage* s = stage(factor);
        return s ? static_cast<double>(s->h.size() - 1) / s->N : 0.0;
    }

    void reset() {
        for (auto& s : stages_) {
            std::fill(s.upHistL.begin(), s.upHistL.end(), 0.0f);
            std::fill(s.upHistR.begin(), s.upHistR.end(), 0.0f);
            std::fill(s.dnHistL.begin(), s.dnHistL.end(), 0.0f);
            std::fill(s.dnHistR.begin(), s.dnHistR.end(), 0.0f);
            s.upPos = 0;
            s.dnPos = 0;
        }
    }

    /**
     * Feldolgozás N-szeres mintavételen.
     * @param fn  void(float* L, float* R, size_t n) – az N*n hosszú túlmintavételezett blokkra
     */
    template <class Fn>
    void process(float* L, float* R, size_t n, int factor, Fn&& fn) {
        Stage* s = stage(factor);
        if (!s) { fn(L, R, n); return; }
        size_t off = 0;
        while (off < n) {
            const size_t m = std::min(kMaxBlock, n - off);
            upsample(*s, L + off, R + off, m);
            fn(bufL_.data(), bufR_.data(), m * static_cast<size_t>(s->N));
            downsample(*s, L + off, R + off, m);
            off += m;
        }
    }

private:
    struct Stage {
        int N = 2;
        std::vector<float> h;                 // prototípus aluláteresztő (N-szeres mintavételen)
        std::vector<float> upHistL, upHistR;  // bemeneti history (eredeti mintavétel)
        std::vector<float> dnHistL, dnHistR;  // túlmintavételezett history (kettőzött ring)
        size_t upPos = 0, dnPos = 0;
    };

    Stage* stage(int factor) {
        if (factor >= 8) return &stages_[2];
        if (factor >= 4) return &stages_[1];
        if (factor >= 2) return &stages_[0];
        return nullptr;
    }
    const Stage* stage(int factor) const { return const_cast<Oversampler*>(this)->stage(factor); }

    /** Kaiser-ablakos sinc aluláteresztő a túlmintavételezett rátán; vágás az eredeti Nyquist ~0.92-szöröse. */
    static std::vector<float> design(int N) {
        const double pi = 3.14159265358979323846;
        const size_t taps = static_cast<size_t>(64 * N) | 1u; // páratlan, szimmetrikus
        const double fc = 0.46 / N;                           // normált vágás (1 = mintavétel)
        const double beta = 9.5;
        auto bessel0 = [](double x) {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 40; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; }
            return sum;
        };
        std::vector<float> h(taps);
        const double mid = (taps - 1) / 2.0;
        double sum = 0.0;
        for (size_t i = 0; i < taps; ++i) {
            const double x = i - mid;
            const double sinc = std::abs(x) < 1e-12 ? 2.0 * fc : std::sin(2.0 * pi * fc * x) / (pi * x);
            const double r = x / mid;
            const double w = bessel0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / bessel0(beta);
            h[i] = static_cast<float>(sinc * w);
            sum += h[i];
        }
        for (float& v : h) v = static_cast<float>(v / sum); // egységnyi DC erősítés
        return h;
    }

    /** Polifázisú felmintavételezés (nullák beszúrása + szűrés), N-szeres erősítéssel. */
    void upsample(Stage& s, const float* L, const float* R, size_t m) {
        const size_t taps = s.h.size();
        const size_t hl = s.upHistL.size();
        const int N = s.N;
        for (size_t i = 0; i < m; ++i) {
            s.upPos = (s.upPos + hl - 1) % hl;
            s.upHistL[s.upPos] = L[i];
            s.upHistR[s.upPos] = R[i];
            for (int p = 0; p < N; ++p) {
                float al = 0.0f, ar = 0.0f;
                size_t k = 0;
                for (size_t t = static_cast<size_t>(p); t < taps; t += static_cast<size_t>(N), ++k) {
                    const size_t idx = (s.upPos + k) % hl;
                    al += s.h[t] * s.upHistL[idx];
                    ar += s.h[t] * s.upHistR[idx];
                }
                bufL_[i * N + p] = al * N;
                bufR_[i * N + p] = ar * N;
            }
        }
    }

    /** Szűrés + minden N-edik minta megtartása. */
    void downsample(Stage& s, float* L, float* R, size_t m) {
        const size_t taps = s.h.size();
        const size_t ring = taps;
        const int N = s.N;
        for (size_t i = 0; i < m; ++i) {
            for (int p = 0; p < N; ++p) {
                const float vl = bufL_[i * N + p], vr = bufR_[i * N + p];
                s.dnHistL[s.dnPos] = vl; s.dnHistL[s.dnPos + ring] = vl;
                s.dnHistR[s.dnPos] = vr; s.dnHistR[s.dnPos + ring] = vr;
                s.dnPos = (s.dnPos + 1) % ring;
            }
            // a legutóbbi `taps` minta folytonosan: dnHist[dnPos .. dnPos+taps-1] (legrégebbitől)
            const float* xl = &s.dnHistL[s.dnPos];
            const float* xr = &s.dnHistR[s.dnPos];
            float al = 0.0f, ar = 0.0f;
            for (size_t t = 0; t < taps; ++t) {
                const float c = s.h[taps - 1 - t]; // szimmetrikus, de a sorrend így pontos
                al += c * xl[t];
                ar += c * xr[t];
            }
            L[i] = al;
            R[i] = ar;
        }
    }

    Stage stages_[3];
    std::vector<float> bufL_, bufR_;
};

} // namespace ee_analyzer
