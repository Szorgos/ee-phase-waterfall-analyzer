#pragma once
/**
 * @file magic_dsp.hpp
 * @brief A Magic Touch közös építőkövei: biquad szűrők (RBJ Audio EQ Cookbook, ITU-R BS.1770 K-súlyozás),
 *        egyszerű FFT és percentilis-előzmény az elemzéshez.
 */
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace ee_analyzer::magic {

// ------------------------------------------------------------------ szűrők
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;
    inline float process(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0; }
    void set(double nb0, double nb1, double nb2, double na0, double na1, double na2) {
        b0 = static_cast<float>(nb0 / na0); b1 = static_cast<float>(nb1 / na0); b2 = static_cast<float>(nb2 / na0);
        a1 = static_cast<float>(na1 / na0); a2 = static_cast<float>(na2 / na0);
    }
};

/** Duplapontos változat az elemzéshez (hosszú összegzéseknél pontosabb). */
struct BiquadD {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    inline double process(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0; }
    template <class B> void from(const B& f) { b0 = f.b0; b1 = f.b1; b2 = f.b2; a1 = f.a1; a2 = f.a2; }
    void set(double nb0, double nb1, double nb2, double na0, double na1, double na2) {
        b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0; a1 = na1 / na0; a2 = na2 / na0;
    }
};

constexpr double kPi = 3.14159265358979323846;

/** RBJ Audio EQ Cookbook tervezők (B: Biquad vagy BiquadD). */
template <class B> void designLowpass(B& f, double fs, double fc, double q) {
    const double w = 2 * kPi * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
    f.set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}
template <class B> void designHighpass(B& f, double fs, double fc, double q) {
    const double w = 2 * kPi * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
    f.set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}
template <class B> void designBandpass(B& f, double fs, double fc, double q) {   // 0 dB-es csúcs
    const double w = 2 * kPi * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
    f.set(al, 0, -al, 1 + al, -2 * c, 1 - al);
}
template <class B> void designPeaking(B& f, double fs, double fc, double gainDb, double q) {
    const double A = std::pow(10.0, gainDb / 40.0), w = 2 * kPi * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
    f.set(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}
template <class B> void designLowShelf(B& f, double fs, double fc, double gainDb, double slope) {
    const double A = std::pow(10.0, gainDb / 40.0), w = 2 * kPi * fc / fs, c = std::cos(w);
    const double al = std::sin(w) / 2 * std::sqrt((A + 1 / A) * (1 / slope - 1) + 2), sa = 2 * std::sqrt(A) * al;
    f.set(A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa),
          (A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa);
}
template <class B> void designHighShelf(B& f, double fs, double fc, double gainDb, double slope) {
    const double A = std::pow(10.0, gainDb / 40.0), w = 2 * kPi * fc / fs, c = std::cos(w);
    const double al = std::sin(w) / 2 * std::sqrt((A + 1 / A) * (1 / slope - 1) + 2), sa = 2 * std::sqrt(A) * al;
    f.set(A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa),
          (A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa);
}

/** ITU-R BS.1770 K-súlyozás: 1. fokozat (magas polc, fejhatás), tetszőleges mintavételen. */
inline void designKShelf(BiquadD& f, double fs) {
    const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    const double K = std::tan(kPi * f0 / fs), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
    const double a0 = 1.0 + K / Q + K * K;
    f.set(Vh + Vb * K / Q + K * K, 2.0 * (K * K - Vh), Vh - Vb * K / Q + K * K, a0, 2.0 * (K * K - 1.0), 1.0 - K / Q + K * K);
}
/** ITU-R BS.1770 K-súlyozás: 2. fokozat (RLB felüláteresztő). */
inline void designKHighpass(BiquadD& f, double fs) {
    const double f0 = 38.13547087602444, Q = 0.5003270373238773, K = std::tan(kPi * f0 / fs);
    f.set(1.0, -2.0, 1.0, 1.0 + K / Q + K * K, 2.0 * (K * K - 1.0), 1.0 - K / Q + K * K);
}

// ------------------------------------------------------------------ segédek az elemzéshez
/** Egyszerű radix-2 FFT (az elemzés másodpercenként kb. 25-ször fut, nem kritikus). */
class Fft {
public:
    void init(size_t n) {
        n_ = n;
        tw_.resize(n / 2);
        for (size_t k = 0; k < n / 2; ++k) tw_[k] = std::polar(1.0, -2.0 * kPi * static_cast<double>(k) / static_cast<double>(n));
        rev_.resize(n);
        size_t bits = 0;
        while ((size_t{1} << bits) < n) ++bits;
        for (size_t i = 0; i < n; ++i) {
            size_t r = 0;
            for (size_t b = 0; b < bits; ++b) if (i & (size_t{1} << b)) r |= size_t{1} << (bits - 1 - b);
            rev_[i] = r;
        }
    }
    void forward(std::vector<std::complex<double>>& a) const {
        for (size_t i = 0; i < n_; ++i) if (i < rev_[i]) std::swap(a[i], a[rev_[i]]);
        for (size_t len = 2; len <= n_; len <<= 1) {
            const size_t step = n_ / len;
            for (size_t i = 0; i < n_; i += len)
                for (size_t j = 0; j < len / 2; ++j) {
                    const std::complex<double> u = a[i + j], v = a[i + j + len / 2] * tw_[j * step];
                    a[i + j] = u + v;
                    a[i + j + len / 2] = u - v;
                }
        }
    }
    size_t size() const { return n_; }
private:
    size_t n_ = 0;
    std::vector<std::complex<double>> tw_;
    std::vector<size_t> rev_;
};

/** Rögzített méretű előzmény-gyűrű percentilisekhez. */
class History {
public:
    explicit History(size_t cap = 1) { resize(cap); }
    void resize(size_t cap) { v_.assign(cap, 0.0); n_ = 0; pos_ = 0; }
    void clear() { n_ = 0; pos_ = 0; }
    void push(double x) { v_[pos_] = x; pos_ = (pos_ + 1) % v_.size(); n_ = std::min(n_ + 1, v_.size()); }
    size_t size() const { return n_; }
    /** p: 0..1; a tmp a hívó munkapuffere (nincs foglalás a gyakori hívásban). */
    double percentile(double p, std::vector<double>& tmp) const {
        if (n_ == 0) return 0.0;
        tmp.assign(v_.begin(), v_.begin() + static_cast<long>(n_));
        const size_t k = std::min(n_ - 1, static_cast<size_t>(p * static_cast<double>(n_ - 1) + 0.5));
        std::nth_element(tmp.begin(), tmp.begin() + static_cast<long>(k), tmp.end());
        return tmp[k];
    }
    template <class F> void forEach(F f) const { for (size_t i = 0; i < n_; ++i) f(v_[i]); }
private:
    std::vector<double> v_;
    size_t n_ = 0, pos_ = 0;
};

inline double toDb(double p) { return 10.0 * std::log10(std::max(p, 1e-30)); }

} // namespace ee_analyzer::magic
