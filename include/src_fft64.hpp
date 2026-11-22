#pragma once
/**
 * @file src_fft64.hpp
 * @brief Egylépcsős, 64 bites (double) és 106 bites (double-double) Overlap-Save FFT + Kaiser-sinc
 *        mintavétel-átalakító fájlátalakításhoz és a beépített valós idejű lejátszóhoz.
 *
 * Külső függőség nélkül működik minden platformon (Linux, Windows, macOS, arm64):
 *  - Saját Stockham vegyes-radix (2, 3, 4, 5, 7) + Bluestein komplex/valós FFT (double és double-double);
 *  - Nagy pontosságú Kaiser-ablakolt sinc szűrő (lineáris, köztes és minimum fázissal is);
 *  - Egzakt racionális L/M blokkméretezés: nulla többlépcsős polifázisos tükörkép.
 */
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace ee_analyzer::srcconv::fft64 {

inline constexpr long double kPiL = 3.14159265358979323846264338327950288L;

// ============================================================================
// Double-Double (106 bites / ~31 decimális jegyű) lebegőpontos aritmetika
// ============================================================================

inline void quickTwoSum(double a, double b, double& s, double& e) {
    s = a + b;
    e = b - (s - a);
}

inline void twoSum(double a, double b, double& s, double& e) {
    s = a + b;
    const double bb = s - a;
    e = (a - (s - bb)) + (b - bb);
}

inline void twoProd(double a, double b, double& p, double& e) {
    p = a * b;
#if defined(__FMA__) || defined(__aarch64__) || defined(_M_ARM64)
    e = std::fma(a, b, -p);
#else
    constexpr double kSplit = 134217729.0;  // 2^27 + 1 (Veltkamp-Dekker)
    const double ca = kSplit * a;
    const double ah = ca - (ca - a);
    const double al = a - ah;
    const double cb = kSplit * b;
    const double bh = cb - (cb - b);
    const double bl = b - bh;
    e = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
#endif
}

struct DD {
    double hi = 0.0;
    double lo = 0.0;

    constexpr DD() = default;
    constexpr explicit DD(double h) : hi(h), lo(0.0) {}
    constexpr DD(double h, double l) : hi(h), lo(l) {}
};

inline DD operator-(const DD& a) { return {-a.hi, -a.lo}; }

inline DD operator+(const DD& a, const DD& b) {
    double s1, s2, t1, t2;
    twoSum(a.hi, b.hi, s1, s2);
    twoSum(a.lo, b.lo, t1, t2);
    s2 += t1;
    quickTwoSum(s1, s2, s1, s2);
    s2 += t2;
    quickTwoSum(s1, s2, s1, s2);
    return {s1, s2};
}

inline DD operator-(const DD& a, const DD& b) { return a + (-b); }

inline DD operator*(const DD& a, const DD& b) {
    double p1, p2;
    twoProd(a.hi, b.hi, p1, p2);
    p2 += a.hi * b.lo + a.lo * b.hi;
    quickTwoSum(p1, p2, p1, p2);
    return {p1, p2};
}

inline DD operator*(const DD& a, double b) {
    double p1, p2;
    twoProd(a.hi, b, p1, p2);
    p2 += a.lo * b;
    quickTwoSum(p1, p2, p1, p2);
    return {p1, p2};
}

inline DD operator/(const DD& a, const DD& b) {
    const double q1 = a.hi / b.hi;
    DD r = a - b * q1;
    const double q2 = r.hi / b.hi;
    r = r - b * q2;
    const double q3 = r.hi / b.hi;
    double s, e;
    quickTwoSum(q1, q2, s, e);
    return DD(s, e) + DD(q3);
}

inline DD& operator+=(DD& a, const DD& b) { a = a + b; return a; }
inline DD& operator-=(DD& a, const DD& b) { a = a - b; return a; }
inline DD& operator*=(DD& a, const DD& b) { a = a * b; return a; }

inline constexpr DD kDdPi{3.141592653589793115997963468544185161590576171875,
                          1.22464679914735320717376402945839660462569212467758e-16};
inline constexpr DD kDdPiHalf{1.5707963267948965579989817342720925807952880859375,
                              6.1232339957367660358688201472919830231284606233879e-17};

inline DD ddSqrt(const DD& x) {
    if (x.hi <= 0.0) return DD(0.0);
    DD y(std::sqrt(x.hi));
    y = (y + x / y) * 0.5;
    y = (y + x / y) * 0.5;
    return y;
}

/** 106 bites pontosságú sin/cos (negyed-redukció + 18 tagú Taylor-sor, hiba < 1e-34). */
inline void ddSinCos(const DD& x, DD& sOut, DD& cOut) {
    const DD qd = x / kDdPiHalf;
    const int64_t k = static_cast<int64_t>(std::llround(qd.hi));
    const DD r = x - kDdPiHalf * static_cast<double>(k);
    const DD r2 = r * r;

    DD s = r;
    DD termS = r;
    DD c(1.0);
    DD termC(1.0);
    for (int m = 1; m <= 18; ++m) {
        const double d1 = static_cast<double>(2 * m - 1);
        const double d2 = static_cast<double>(2 * m);
        const double d3 = static_cast<double>(2 * m + 1);
        termC = -termC * r2 / DD(d1 * d2);
        c += termC;
        termS = -termS * r2 / DD(d2 * d3);
        s += termS;
    }

    switch (k & 3) {
        case 0: sOut = s; cOut = c; break;
        case 1: sOut = c; cOut = -s; break;
        case 2: sOut = -s; cOut = -c; break;
        default: sOut = -c; cOut = s; break;
    }
}

/** 106 bites pontosságú módosított I0 Bessel-függvény (Kaiser-ablakhoz). */
inline DD ddBesselI0(const DD& x) {
    const DD half = x * 0.5;
    const DD y = half * half;
    DD sum(1.0);
    DD term(1.0);
    for (int k = 1; k <= 220; ++k) {
        const DD dk(static_cast<double>(k));
        term = term * y / (dk * dk);
        sum += term;
        if (std::abs(term.hi) <= std::abs(sum.hi) * 1e-34) break;
    }
    return sum;
}

// ============================================================================
// Valós-típus segédek (double és DD egységes kezelése)
// ============================================================================

template <typename Real>
inline Real realFromDouble(double v);
template <>
inline double realFromDouble<double>(double v) { return v; }
template <>
inline DD realFromDouble<DD>(double v) { return DD(v); }

template <typename Real>
inline double realToDouble(const Real& v);
template <>
inline double realToDouble<double>(const double& v) { return v; }
template <>
inline double realToDouble<DD>(const DD& v) { return v.hi; }

template <typename Real>
inline Real realPi();
template <>
inline double realPi<double>() { return static_cast<double>(kPiL); }
template <>
inline DD realPi<DD>() { return kDdPi; }

template <typename Real>
inline void realSinCosRatio(int64_t num, uint64_t den, Real& s, Real& c);
template <>
inline void realSinCosRatio<double>(int64_t num, uint64_t den, double& s, double& c) {
    const long double ang = static_cast<long double>(num) * kPiL / static_cast<long double>(den);
    s = static_cast<double>(std::sin(ang));
    c = static_cast<double>(std::cos(ang));
}
template <>
inline void realSinCosRatio<DD>(int64_t num, uint64_t den, DD& s, DD& c) {
    const DD ang = (kDdPi * static_cast<double>(num)) / DD(static_cast<double>(den));
    ddSinCos(ang, s, c);
}

// ============================================================================
// Komplex típus és Stockham vegyes-radix + Bluestein FFT sablon (double / DD)
// ============================================================================

template <typename Real>
struct ComplexT {
    Real r = realFromDouble<Real>(0.0);
    Real i = realFromDouble<Real>(0.0);
};

template <typename Real>
inline ComplexT<Real> operator+(const ComplexT<Real>& a, const ComplexT<Real>& b) {
    return {a.r + b.r, a.i + b.i};
}
template <typename Real>
inline ComplexT<Real> operator-(const ComplexT<Real>& a, const ComplexT<Real>& b) {
    return {a.r - b.r, a.i - b.i};
}
template <typename Real>
inline ComplexT<Real> operator*(const ComplexT<Real>& a, const ComplexT<Real>& b) {
    return {a.r * b.r - a.i * b.i, a.r * b.i + a.i * b.r};
}

using C64 = ComplexT<double>;
using CDD = ComplexT<DD>;

/**
 * Komplex FFT terv (Stockham vegyes-radix: 4, 2, 3, 5, 7; más prímtényező esetén Bluestein).
 * double (64 bit) és DD (106 bit) pontossággal is példányosítható.
 */
template <typename Real>
class ComplexFftT {
public:
    using C = ComplexT<Real>;

    ComplexFftT() = default;
    explicit ComplexFftT(size_t n) { init(n); }

    size_t size() const { return n_; }

    void init(size_t n) {
        n_ = n;
        radices_.clear();
        twiddlesFwd_.clear();
        twiddlesInv_.clear();
        useBluestein_ = false;
        if (n <= 1) return;

        // Radix-3, 5, 7 forgatókonstansok pontos (Real) előállítása
        Real dummy;
        realSinCosRatio<Real>(1, 3, sin60_, dummy);  // sin(pi/3) = sqrt(3)/2
        realSinCosRatio<Real>(2, 5, s5_1_, c5_1_);
        realSinCosRatio<Real>(4, 5, s5_2_, c5_2_);
        realSinCosRatio<Real>(2, 7, s7_1_, c7_1_);
        realSinCosRatio<Real>(4, 7, s7_2_, c7_2_);
        realSinCosRatio<Real>(6, 7, s7_3_, c7_3_);

        size_t rem = n;
        while (rem % 4 == 0) { radices_.push_back(4); rem /= 4; }
        while (rem % 2 == 0) { radices_.push_back(2); rem /= 2; }
        while (rem % 3 == 0) { radices_.push_back(3); rem /= 3; }
        while (rem % 5 == 0) { radices_.push_back(5); rem /= 5; }
        while (rem % 7 == 0) { radices_.push_back(7); rem /= 7; }

        if (rem == 1) {
            size_t s = 1;
            twiddlesFwd_.resize(radices_.size());
            twiddlesInv_.resize(radices_.size());
            for (size_t idx = 0; idx < radices_.size(); ++idx) {
                const size_t r = radices_[idx];
                const size_t rs = r * s;
                auto& tf = twiddlesFwd_[idx];
                auto& ti = twiddlesInv_[idx];
                tf.resize(s * (r - 1));
                ti.resize(s * (r - 1));
                for (size_t j = 0; j < s; ++j) {
                    for (size_t p = 1; p < r; ++p) {
                        const uint64_t pj = (static_cast<uint64_t>(p) * j) % rs;
                        Real sn, cs;
                        realSinCosRatio<Real>(-static_cast<int64_t>(2 * pj), static_cast<uint64_t>(rs), sn, cs);
                        tf[j * (r - 1) + (p - 1)] = {cs, sn};
                        ti[j * (r - 1) + (p - 1)] = {cs, -sn};
                    }
                }
                s = rs;
            }
            scratch_.resize(n_);
        } else {
            useBluestein_ = true;
            radices_.clear();
            size_t m = 1;
            while (m < 2 * n_ - 1) m <<= 1;
            bluesteinM_ = m;
            chirpFwd_.resize(n_);
            chirpInv_.resize(n_);
            for (size_t k = 0; k < n_; ++k) {
                const uint64_t k2 = (static_cast<uint64_t>(k) * k) % (2ull * n_);
                Real sn, cs;
                realSinCosRatio<Real>(-static_cast<int64_t>(k2), static_cast<uint64_t>(n_), sn, cs);
                chirpFwd_[k] = {cs, sn};
                chirpInv_[k] = {cs, -sn};
            }
            bluesteinFft_.reset(new ComplexFftT<Real>(m));
            const C zero{realFromDouble<Real>(0.0), realFromDouble<Real>(0.0)};
            const C one{realFromDouble<Real>(1.0), realFromDouble<Real>(0.0)};
            bluesteinBFwd_.assign(m, zero);
            bluesteinBInv_.assign(m, zero);
            bluesteinBFwd_[0] = one;
            bluesteinBInv_[0] = one;
            for (size_t k = 1; k < n_; ++k) {
                bluesteinBFwd_[k] = chirpInv_[k];
                bluesteinBFwd_[m - k] = chirpInv_[k];
                bluesteinBInv_[k] = chirpFwd_[k];
                bluesteinBInv_[m - k] = chirpFwd_[k];
            }
            bluesteinFft_->transform(bluesteinBFwd_.data(), false);
            bluesteinFft_->transform(bluesteinBInv_.data(), false);
            bluesteinBuf_.resize(m);
        }
    }

    /** Helyben végzett komplex DFT (inverse=false: előre, inverse=true: vissza, 1/N skálázással). */
    void transform(C* data, bool inverse) const {
        if (n_ <= 1) return;
        if (useBluestein_) {
            transformBluestein(data, inverse);
            return;
        }

        const Real half = realFromDouble<Real>(0.5);
        const Real s3 = inverse ? sin60_ : -sin60_;
        const Real s5_1 = inverse ? s5_1_ : -s5_1_;
        const Real s5_2 = inverse ? s5_2_ : -s5_2_;
        const Real s7_1 = inverse ? s7_1_ : -s7_1_;
        const Real s7_2 = inverse ? s7_2_ : -s7_2_;
        const Real s7_3 = inverse ? s7_3_ : -s7_3_;

        C* src = data;
        C* dst = scratch_.data();
        size_t s = 1;
        const auto& twTable = inverse ? twiddlesInv_ : twiddlesFwd_;

        for (size_t idx = 0; idx < radices_.size(); ++idx) {
            const size_t r = radices_[idx];
            const size_t q = n_ / (r * s);
            const C* tw = twTable[idx].data();

            if (r == 2) {
                for (size_t k = 0; k < q; ++k) {
                    const C* in0 = src + k * s;
                    const C* in1 = src + (q + k) * s;
                    C* out0 = dst + (k * 2) * s;
                    C* out1 = dst + (k * 2 + 1) * s;
                    for (size_t j = 0; j < s; ++j) {
                        const C a0 = in0[j];
                        const C a1 = in1[j] * tw[j];
                        out0[j] = a0 + a1;
                        out1[j] = a0 - a1;
                    }
                }
            } else if (r == 4) {
                for (size_t k = 0; k < q; ++k) {
                    const C* in0 = src + k * s;
                    const C* in1 = src + (q + k) * s;
                    const C* in2 = src + (2 * q + k) * s;
                    const C* in3 = src + (3 * q + k) * s;
                    C* out0 = dst + (k * 4) * s;
                    C* out1 = dst + (k * 4 + 1) * s;
                    C* out2 = dst + (k * 4 + 2) * s;
                    C* out3 = dst + (k * 4 + 3) * s;
                    for (size_t j = 0; j < s; ++j) {
                        const C* w = tw + j * 3;
                        const C a0 = in0[j];
                        const C a1 = in1[j] * w[0];
                        const C a2 = in2[j] * w[1];
                        const C a3 = in3[j] * w[2];
                        const C t0 = a0 + a2;
                        const C t1 = a0 - a2;
                        const C t2 = a1 + a3;
                        const C t3 = a1 - a3;
                        out0[j] = t0 + t2;
                        out2[j] = t0 - t2;
                        if (inverse) {
                            out1[j] = {t1.r - t3.i, t1.i + t3.r};
                            out3[j] = {t1.r + t3.i, t1.i - t3.r};
                        } else {
                            out1[j] = {t1.r + t3.i, t1.i - t3.r};
                            out3[j] = {t1.r - t3.i, t1.i + t3.r};
                        }
                    }
                }
            } else if (r == 3) {
                for (size_t k = 0; k < q; ++k) {
                    const C* in0 = src + k * s;
                    const C* in1 = src + (q + k) * s;
                    const C* in2 = src + (2 * q + k) * s;
                    C* out0 = dst + (k * 3) * s;
                    C* out1 = dst + (k * 3 + 1) * s;
                    C* out2 = dst + (k * 3 + 2) * s;
                    for (size_t j = 0; j < s; ++j) {
                        const C* w = tw + j * 2;
                        const C a0 = in0[j];
                        const C a1 = in1[j] * w[0];
                        const C a2 = in2[j] * w[1];
                        const C t1 = a1 + a2;
                        const C t2 = a1 - a2;
                        const C m1 = {a0.r - half * t1.r, a0.i - half * t1.i};
                        const C m2 = {-s3 * t2.i, s3 * t2.r};
                        out0[j] = a0 + t1;
                        out1[j] = m1 + m2;
                        out2[j] = m1 - m2;
                    }
                }
            } else if (r == 5) {
                for (size_t k = 0; k < q; ++k) {
                    const C* in0 = src + k * s;
                    const C* in1 = src + (q + k) * s;
                    const C* in2 = src + (2 * q + k) * s;
                    const C* in3 = src + (3 * q + k) * s;
                    const C* in4 = src + (4 * q + k) * s;
                    C* out0 = dst + (k * 5) * s;
                    C* out1 = dst + (k * 5 + 1) * s;
                    C* out2 = dst + (k * 5 + 2) * s;
                    C* out3 = dst + (k * 5 + 3) * s;
                    C* out4 = dst + (k * 5 + 4) * s;
                    for (size_t j = 0; j < s; ++j) {
                        const C* w = tw + j * 4;
                        const C a0 = in0[j];
                        const C a1 = in1[j] * w[0];
                        const C a2 = in2[j] * w[1];
                        const C a3 = in3[j] * w[2];
                        const C a4 = in4[j] * w[3];
                        const C s1 = a1 + a4, d1 = a1 - a4;
                        const C s2 = a2 + a3, d2 = a2 - a3;
                        out0[j] = a0 + s1 + s2;
                        const C r1 = {a0.r + c5_1_ * s1.r + c5_2_ * s2.r, a0.i + c5_1_ * s1.i + c5_2_ * s2.i};
                        const C r2 = {a0.r + c5_2_ * s1.r + c5_1_ * s2.r, a0.i + c5_2_ * s1.i + c5_1_ * s2.i};
                        const C i1 = {-(s5_1 * d1.i + s5_2 * d2.i), s5_1 * d1.r + s5_2 * d2.r};
                        const C i2 = {-(s5_2 * d1.i - s5_1 * d2.i), s5_2 * d1.r - s5_1 * d2.r};
                        out1[j] = r1 + i1;
                        out4[j] = r1 - i1;
                        out2[j] = r2 + i2;
                        out3[j] = r2 - i2;
                    }
                }
            } else if (r == 7) {
                for (size_t k = 0; k < q; ++k) {
                    const C* in0 = src + k * s;
                    const C* in1 = src + (q + k) * s;
                    const C* in2 = src + (2 * q + k) * s;
                    const C* in3 = src + (3 * q + k) * s;
                    const C* in4 = src + (4 * q + k) * s;
                    const C* in5 = src + (5 * q + k) * s;
                    const C* in6 = src + (6 * q + k) * s;
                    C* out0 = dst + (k * 7) * s;
                    C* out1 = dst + (k * 7 + 1) * s;
                    C* out2 = dst + (k * 7 + 2) * s;
                    C* out3 = dst + (k * 7 + 3) * s;
                    C* out4 = dst + (k * 7 + 4) * s;
                    C* out5 = dst + (k * 7 + 5) * s;
                    C* out6 = dst + (k * 7 + 6) * s;
                    for (size_t j = 0; j < s; ++j) {
                        const C* w = tw + j * 6;
                        const C a0 = in0[j];
                        const C a1 = in1[j] * w[0];
                        const C a2 = in2[j] * w[1];
                        const C a3 = in3[j] * w[2];
                        const C a4 = in4[j] * w[3];
                        const C a5 = in5[j] * w[4];
                        const C a6 = in6[j] * w[5];
                        const C s1 = a1 + a6, d1 = a1 - a6;
                        const C s2 = a2 + a5, d2 = a2 - a5;
                        const C s3 = a3 + a4, d3 = a3 - a4;
                        out0[j] = a0 + s1 + s2 + s3;
                        const C r1 = {a0.r + c7_1_ * s1.r + c7_2_ * s2.r + c7_3_ * s3.r,
                                      a0.i + c7_1_ * s1.i + c7_2_ * s2.i + c7_3_ * s3.i};
                        const C r2 = {a0.r + c7_2_ * s1.r + c7_3_ * s2.r + c7_1_ * s3.r,
                                      a0.i + c7_2_ * s1.i + c7_3_ * s2.i + c7_1_ * s3.i};
                        const C r3 = {a0.r + c7_3_ * s1.r + c7_1_ * s2.r + c7_2_ * s3.r,
                                      a0.i + c7_3_ * s1.i + c7_1_ * s2.i + c7_2_ * s3.i};
                        const C i1 = {-(s7_1 * d1.i + s7_2 * d2.i + s7_3 * d3.i), s7_1 * d1.r + s7_2 * d2.r + s7_3 * d3.r};
                        const C i2 = {-(s7_2 * d1.i - s7_3 * d2.i - s7_1 * d3.i), s7_2 * d1.r - s7_3 * d2.r - s7_1 * d3.r};
                        const C i3 = {-(s7_3 * d1.i - s7_1 * d2.i + s7_2 * d3.i), s7_3 * d1.r - s7_1 * d2.r + s7_2 * d3.r};
                        out1[j] = r1 + i1;
                        out6[j] = r1 - i1;
                        out2[j] = r2 + i2;
                        out5[j] = r2 - i2;
                        out3[j] = r3 + i3;
                        out4[j] = r3 - i3;
                    }
                }
            }
            std::swap(src, dst);
            s *= r;
        }

        if (src != data) {
            std::copy(src, src + n_, data);
        }
        if (inverse) {
            const Real invN = realFromDouble<Real>(1.0) / realFromDouble<Real>(static_cast<double>(n_));
            for (size_t i = 0; i < n_; ++i) {
                data[i].r = data[i].r * invN;
                data[i].i = data[i].i * invN;
            }
        }
    }

private:
    void transformBluestein(C* data, bool inverse) const {
        const auto& chirp = inverse ? chirpInv_ : chirpFwd_;
        const auto& bSpec = inverse ? bluesteinBInv_ : bluesteinBFwd_;
        const C zero{realFromDouble<Real>(0.0), realFromDouble<Real>(0.0)};
        std::fill(bluesteinBuf_.begin(), bluesteinBuf_.end(), zero);
        for (size_t k = 0; k < n_; ++k) bluesteinBuf_[k] = data[k] * chirp[k];
        bluesteinFft_->transform(bluesteinBuf_.data(), false);
        for (size_t k = 0; k < bluesteinM_; ++k) bluesteinBuf_[k] = bluesteinBuf_[k] * bSpec[k];
        bluesteinFft_->transform(bluesteinBuf_.data(), true);
        const Real scale = inverse ? (realFromDouble<Real>(1.0) / realFromDouble<Real>(static_cast<double>(n_)))
                                   : realFromDouble<Real>(1.0);
        for (size_t k = 0; k < n_; ++k) {
            const C v = bluesteinBuf_[k] * chirp[k];
            data[k] = {v.r * scale, v.i * scale};
        }
    }

    size_t n_ = 0;
    Real sin60_ = realFromDouble<Real>(0.0);
    Real c5_1_ = realFromDouble<Real>(0.0), c5_2_ = realFromDouble<Real>(0.0);
    Real s5_1_ = realFromDouble<Real>(0.0), s5_2_ = realFromDouble<Real>(0.0);
    Real c7_1_ = realFromDouble<Real>(0.0), c7_2_ = realFromDouble<Real>(0.0), c7_3_ = realFromDouble<Real>(0.0);
    Real s7_1_ = realFromDouble<Real>(0.0), s7_2_ = realFromDouble<Real>(0.0), s7_3_ = realFromDouble<Real>(0.0);
    std::vector<size_t> radices_;
    std::vector<std::vector<C>> twiddlesFwd_;
    std::vector<std::vector<C>> twiddlesInv_;
    mutable std::vector<C> scratch_;

    bool useBluestein_ = false;
    size_t bluesteinM_ = 0;
    std::unique_ptr<ComplexFftT<Real>> bluesteinFft_;
    std::vector<C> chirpFwd_;
    std::vector<C> chirpInv_;
    std::vector<C> bluesteinBFwd_;
    std::vector<C> bluesteinBInv_;
    mutable std::vector<C> bluesteinBuf_;
};

/**
 * Valós-komplex FFT páros N méretre (N/2-es komplex trükkel felezi a műveletszámot).
 */
template <typename Real>
class RealFftT {
public:
    using C = ComplexT<Real>;

    RealFftT() = default;
    explicit RealFftT(size_t n) { init(n); }

    size_t size() const { return n_; }

    void init(size_t n) {
        n_ = n;
        half_ = n / 2;
        halfFft_.init(half_);
        twFwd_.resize(half_);
        twInv_.resize(half_);
        for (size_t k = 0; k < half_; ++k) {
            Real sn, cs;
            realSinCosRatio<Real>(-static_cast<int64_t>(2 * k), static_cast<uint64_t>(n_), sn, cs);
            twFwd_[k] = {cs, sn};
            twInv_[k] = {cs, -sn};
        }
        buf_.resize(half_);
    }

    /** N valós minta -> N/2+1 komplex spektrumpont. */
    void forward(const Real* in, C* out) const {
        if (n_ == 0) return;
        const Real half = realFromDouble<Real>(0.5);
        const Real zero = realFromDouble<Real>(0.0);
        for (size_t i = 0; i < half_; ++i) buf_[i] = {in[2 * i], in[2 * i + 1]};
        halfFft_.transform(buf_.data(), false);
        const C z0 = buf_[0];
        out[0] = {z0.r + z0.i, zero};
        out[half_] = {z0.r - z0.i, zero};
        for (size_t k = 1; k < half_; ++k) {
            const C zk = buf_[k];
            const C zmk = buf_[half_ - k];
            const C even = {(zk.r + zmk.r) * half, (zk.i - zmk.i) * half};
            const C odd = {(zk.i + zmk.i) * half, (zmk.r - zk.r) * half};
            out[k] = even + twFwd_[k] * odd;
        }
    }

    /** N/2+1 komplex spektrumpont -> N valós minta (1/N skálázással). */
    void inverse(const C* in, Real* out) const {
        if (n_ == 0) return;
        const Real half = realFromDouble<Real>(0.5);
        const C x0 = in[0];
        const C xh = in[half_];
        buf_[0] = {(x0.r + xh.r) * half, (x0.r - xh.r) * half};
        for (size_t k = 1; k < half_; ++k) {
            const C xk = in[k];
            const C xmk = in[half_ - k];
            const C even = {(xk.r + xmk.r) * half, (xk.i - xmk.i) * half};
            const C diff = {(xk.r - xmk.r) * half, (xk.i + xmk.i) * half};
            const C w = twInv_[k];
            const C odd = diff * w;
            buf_[k] = {even.r - odd.i, even.i + odd.r};
        }
        halfFft_.transform(buf_.data(), true);
        for (size_t i = 0; i < half_; ++i) {
            out[2 * i] = buf_[i].r;
            out[2 * i + 1] = buf_[i].i;
        }
    }

private:
    size_t n_ = 0;
    size_t half_ = 0;
    ComplexFftT<Real> halfFft_;
    std::vector<C> twFwd_;
    std::vector<C> twInv_;
    mutable std::vector<C> buf_;
};

using ComplexFft = ComplexFftT<double>;
using ComplexFftDD = ComplexFftT<DD>;
using RealFft = RealFftT<double>;
using RealFftDD = RealFftT<DD>;

/** Módosított I0 Bessel-függvény long double pontossággal (Kaiser-ablakhoz). */
inline long double besselI0L(long double x) {
    const long double half = x * 0.5L;
    const long double y = half * half;
    long double sum = 1.0L;
    long double term = 1.0L;
    for (int k = 1; k <= 120; ++k) {
        term *= y / (static_cast<long double>(k) * static_cast<long double>(k));
        sum += term;
        if (term <= sum * 1e-22L) break;
    }
    return sum;
}

/**
 * Zéró-késleltetésű, egzakt lineáris fázisú Kaiser-sinc szűrő spektruma (double, H = nIn / 4).
 */
inline std::vector<double> designKaiserSincSpectrum(size_t nIn, uint32_t srcRate, uint32_t dstRate,
                                                    double passbandFrac, double beta, const RealFft& fftIn) {
    const size_t H = nIn / 4;
    const long double nyq = static_cast<long double>(std::min(srcRate, dstRate)) * 0.5L;
    const long double pb = std::clamp(static_cast<long double>(passbandFrac), 0.5L, 0.9990L);
    const long double fc = nyq * 0.5L * (pb + 1.0L);
    const long double fcNorm = fc / static_cast<long double>(srcRate);
    const long double betaL = static_cast<long double>(beta);
    const long double invI0 = 1.0L / besselI0L(betaL);

    std::vector<long double> hw(2 * H + 1, 0.0L);
    long double dc = 0.0L;
    for (size_t idx = 0; idx <= 2 * H; ++idx) {
        const int64_t n = static_cast<int64_t>(idx) - static_cast<int64_t>(H);
        const long double u = static_cast<long double>(n) / static_cast<long double>(H);
        const long double rad = std::max(0.0L, 1.0L - u * u);
        const long double w = besselI0L(betaL * std::sqrt(rad)) * invI0;
        long double h = 0.0L;
        if (n == 0) {
            h = 2.0L * fcNorm;
        } else {
            const long double arg = 2.0L * kPiL * fcNorm * static_cast<long double>(n);
            h = std::sin(arg) / (kPiL * static_cast<long double>(n));
        }
        hw[idx] = h * w;
        dc += hw[idx];
    }

    std::vector<double> hBuf(nIn, 0.0);
    for (size_t k = 0; k <= H; ++k) hBuf[k] = static_cast<double>(hw[H + k] / dc);
    for (size_t k = 1; k <= H; ++k) hBuf[nIn - k] = static_cast<double>(hw[H - k] / dc);

    std::vector<C64> spec(nIn / 2 + 1);
    fftIn.forward(hBuf.data(), spec.data());
    std::vector<double> hReal(nIn / 2 + 1);
    for (size_t k = 0; k < hReal.size(); ++k) hReal[k] = spec[k].r;
    return hReal;
}

/**
 * 106 bites (double-double) pontosságú, zéró-késleltetésű Kaiser-sinc szűrő spektruma (H <= nIn / 4).
 */
inline std::vector<DD> designKaiserSincSpectrumDD(size_t nIn, uint32_t srcRate, uint32_t dstRate,
                                                  double passbandFrac, double beta, const RealFftDD& fftIn,
                                                  size_t hTaps = 0) {
    const size_t maxH = nIn / 4;
    const size_t H = (hTaps > 0 && hTaps < maxH) ? hTaps : maxH;
    const DD nyq = DD(static_cast<double>(std::min(srcRate, dstRate))) * 0.5;
    const DD pb(std::clamp(passbandFrac, 0.5, 0.9990));
    const DD fc = nyq * ((pb + DD(1.0)) * 0.5);
    const DD fcNorm = fc / DD(static_cast<double>(srcRate));
    const DD betaD(beta);
    const DD invI0 = DD(1.0) / ddBesselI0(betaD);
    const DD invH = DD(1.0) / DD(static_cast<double>(H));

    // Szimmetrikus: csak n = 0..H-ra kell kiszámítani (fele idő!)
    std::vector<DD> hwHalf(H + 1);
    hwHalf[0] = fcNorm * 2.0;  // w(0) = 1.0
    DD dc = hwHalf[0];
    for (size_t k = 1; k <= H; ++k) {
        const DD dk(static_cast<double>(k));
        const DD u = dk * invH;
        const DD rad = DD(1.0) - u * u;
        const DD w = (rad.hi > 0.0) ? (ddBesselI0(betaD * ddSqrt(rad)) * invI0) : invI0;
        const DD arg = (kDdPi * 2.0) * fcNorm * dk;
        DD sn, cs;
        ddSinCos(arg, sn, cs);
        const DD h = sn / (kDdPi * dk);
        hwHalf[k] = h * w;
        dc += hwHalf[k] * 2.0;
    }

    const DD invDc = DD(1.0) / dc;
    std::vector<DD> hBuf(nIn, DD(0.0));
    hBuf[0] = hwHalf[0] * invDc;
    for (size_t k = 1; k <= H; ++k) {
        const DD v = hwHalf[k] * invDc;
        hBuf[k] = v;
        hBuf[nIn - k] = v;
    }

    std::vector<CDD> spec(nIn / 2 + 1);
    fftIn.forward(hBuf.data(), spec.data());
    std::vector<DD> hReal(nIn / 2 + 1);
    for (size_t k = 0; k < hReal.size(); ++k) hReal[k] = spec[k].r;
    return hReal;
}

/** Egzakt racionális Overlap-Save blokkméretek számítása (4-gyel osztható nIn és nOut). */
inline bool computeBlockSizes(uint32_t srcRate, uint32_t dstRate, size_t& nIn, size_t& nOut) {
    nIn = 0;
    nOut = 0;
    if (srcRate == 0 || dstRate == 0 || srcRate == dstRate) return false;
    const uint32_t g = std::gcd(srcRate, dstRate);
    const size_t M = srcRate / g;
    const size_t L = dstRate / g;
    const size_t mx = std::max(M, L);
    if (mx == 0 || mx > 65536) return false;
    size_t B = 4;  // garantálja, hogy nIn és nOut is 4-gyel osztható (H = n/4 egzakt egész!)
    while (mx * B < 65536) B <<= 1;
    nIn = M * B;
    nOut = L * B;
    return true;
}

/**
 * Nagy felbontású (106 bites DD) Overlap-Save blokkméretek:
 * 96 -> 44.1 kHz-re B = 192 (N_in = 61440, N_out = 28224, H_blk = 15360),
 * tisztán 7-sima Stockham radixokkal és rövid időtartományú csengéssel.
 */
inline bool computeBlockSizesDD(uint32_t srcRate, uint32_t dstRate, size_t& nIn, size_t& nOut) {
    if (!computeBlockSizes(srcRate, dstRate, nIn, nOut)) return false;
    const uint32_t g = std::gcd(srcRate, dstRate);
    const size_t M = srcRate / g;
    const size_t L = dstRate / g;
    size_t B = nIn / M;
    const size_t mx = std::max(M, L);
    if ((B % 16 == 0) && (mx * (B * 3 / 4) >= 57600)) {
        B = (B * 3) / 4;
        nIn = M * B;
        nOut = L * B;
    }
    return true;
}

/**
 * Választható fázisú (0=minimum, 25=köztes, 50=lineáris) Kaiser-sinc spektrum tervezése
 * valós cepstrumos Hilbert-transzformációval és egzakt zéró-késleltetésű Overlap-Save ablakolással.
 */
inline std::vector<C64> designPhasedKaiserSincSpectrum(size_t nIn, uint32_t srcRate, uint32_t dstRate,
                                                       double passbandFrac, double beta, double phase,
                                                       size_t leadIn, const RealFft& fftIn) {
    const std::vector<double> hLin = designKaiserSincSpectrum(nIn, srcRate, dstRate, passbandFrac, beta, fftIn);
    const size_t nBins = hLin.size();
    std::vector<C64> out(nBins);

    const double ph = std::clamp(phase, 0.0, 50.0);
    if (ph >= 49.9) {
        for (size_t k = 0; k < nBins; ++k) out[k] = {hLin[k], 0.0};
        return out;
    }

    // Sima padlójú log-amplitúdó (-260 dB = 1e-13) -> valós cepstrum -> kauzális minimum-fázis
    const double alpha = 1.0 - ph / 50.0;
    std::vector<C64> logM(nBins);
    std::vector<double> mag(nBins);
    for (size_t k = 0; k < nBins; ++k) {
        const double m = std::sqrt(hLin[k] * hLin[k] + 1e-26);
        mag[k] = m;
        logM[k] = {std::log(m), 0.0};
    }

    std::vector<double> cep(nIn, 0.0);
    fftIn.inverse(logM.data(), cep.data());

    // Kauzális cepstrum ablak: c_min[0]=c[0], c_min[1..N/2-1]=2*c, c_min[N/2]=c[N/2], többi 0
    for (size_t n = 1; n < nIn / 2; ++n) cep[n] *= 2.0;
    for (size_t n = nIn / 2 + 1; n < nIn; ++n) cep[n] = 0.0;

    std::vector<C64> Z(nBins);
    fftIn.forward(cep.data(), Z.data());

    std::vector<C64> hPhSpec(nBins);
    for (size_t k = 0; k < nBins; ++k) {
        const double ang = alpha * Z[k].i;
        hPhSpec[k] = {mag[k] * std::cos(ang), mag[k] * std::sin(ang)};
    }
    if ((nIn & 1u) == 0u) hPhSpec[nIn / 2].i = 0.0;

    std::vector<double> hPh(nIn, 0.0);
    fftIn.inverse(hPhSpec.data(), hPh.data());

    // Szigorú [-lPre .. +lPost] időtartományú ablakolás (külső 20%-on sima Hann-lecsengéssel),
    // így garantált a 0 cirkuláris áthallás Overlap-Save-ben.
    const size_t totalSpan = nIn / 2;  // 2 * H_in
    const size_t lPost = std::min(leadIn, totalSpan);
    const size_t lPre = totalSpan - lPost;

    std::vector<double> hWin(nIn, 0.0);
    for (size_t i = 0; i <= lPost; ++i) hWin[i] = hPh[i];
    if (lPost >= 4) {
        const size_t tLen = std::max<size_t>(1, lPost / 5);
        for (size_t i = 0; i < tLen; ++i) {
            const double ramp = 0.5 * (1.0 + std::cos(static_cast<double>(kPiL) * static_cast<double>(i) / static_cast<double>(tLen)));
            hWin[lPost + 1 - tLen + i] *= ramp;
        }
    }
    if (lPre > 0) {
        for (size_t i = 0; i < lPre; ++i) hWin[nIn - lPre + i] = hPh[nIn - lPre + i];
        if (lPre >= 4) {
            const size_t tLen = std::max<size_t>(1, lPre / 5);
            for (size_t i = 0; i < tLen; ++i) {
                const double ramp = 0.5 * (1.0 - std::cos(static_cast<double>(kPiL) * static_cast<double>(i) / static_cast<double>(tLen)));
                hWin[nIn - lPre + i] *= ramp;
            }
        }
    }

    double dc = 0.0;
    for (double v : hWin) dc += v;
    if (std::abs(dc) > 1e-12) {
        const double invDc = 1.0 / dc;
        for (double& v : hWin) v *= invDc;
    }

    fftIn.forward(hWin.data(), out.data());
    return out;
}

/**
 * Folyamatos (streaming) 64 bites Overlap-Save FFT + Kaiser-sinc átmintavételező a valós idejű lejátszóhoz.
 * Támogatja a választható fázist (0 = minimum, 25 = köztes, 50 = lineáris), szünetmentesen (clear() nélkül)
 * fut tovább a számok között, és a végén flush()-sal pontosan kiüríthető.
 */
class StreamResampler {
public:
    StreamResampler() = default;

    bool init(uint32_t srcRate, uint32_t dstRate, uint32_t channels, double passbandFrac = 0.9970,
              double beta = 34.5, double phase = 50.0) {
        reset();
        if (srcRate == 0 || dstRate == 0 || srcRate == dstRate || channels == 0 || channels > 8) return false;
        if (!computeBlockSizes(srcRate, dstRate, nIn_, nOut_)) return false;

        srcRate_ = srcRate;
        dstRate_ = dstRate;
        ch_ = channels;
        const uint32_t g = std::gcd(srcRate_, dstRate_);
        const size_t M = srcRate_ / g;
        const size_t L = dstRate_ / g;
        const size_t B = nIn_ / M;

        hIn_ = nIn_ / 4;
        stepIn_ = nIn_ / 2;
        hOut_ = nOut_ / 4;
        stepOut_ = nOut_ / 2;

        // Egzakt egész blokkeltolás a választott fázishoz: leadIn/M == leadOut/L -> zéró késleltetés!
        const double ph = std::clamp(phase, 0.0, 50.0);
        const double alpha = 1.0 - ph / 50.0;
        const size_t leadBlocks = std::clamp<size_t>(
            static_cast<size_t>(std::llround((1.0 + alpha) * static_cast<double>(B) * 0.25)),
            B / 4, B / 2);
        leadIn_ = leadBlocks * M;
        leadOut_ = leadBlocks * L;

        fftIn_.init(nIn_);
        fftOut_.init(nOut_);
        const double pb = std::clamp(passbandFrac, 0.5, 0.997);
        const double bClamped = std::clamp(beta, 4.0, 45.0);
        // Felfelé mintavételezésnél (dstRate > srcRate) a nagyobb (kimeneti) rácson tervezzük a fázis-szűrőt,
        // így a minimum- és köztes fázis a kimeneti mintavételen is szigorúan kauzális és tükörkép-mentes!
        const auto hSpec = (dstRate_ > srcRate_)
                               ? designPhasedKaiserSincSpectrum(nOut_, dstRate_, srcRate_, pb, bClamped, ph, leadOut_, fftOut_)
                               : designPhasedKaiserSincSpectrum(nIn_, srcRate_, dstRate_, pb, bClamped, ph, leadIn_, fftIn_);

        nBinsOut_ = nOut_ / 2 + 1;
        nBinsCopy_ = std::min(nIn_ / 2 + 1, nBinsOut_);
        const double scale = static_cast<double>(nOut_) / static_cast<double>(nIn_);
        hScaled_.resize(nBinsCopy_);
        for (size_t k = 0; k < nBinsCopy_; ++k) hScaled_[k] = {hSpec[k].r * scale, hSpec[k].i * scale};

        X_.resize(nIn_ / 2 + 1);
        Y_.resize(nBinsOut_);
        yBlk_.resize(nOut_);
        chOutBuf_.assign(ch_, std::vector<double>(stepOut_));
        inHist_.resize(ch_);
        for (auto& h : inHist_) {
            h.reserve(nIn_ + 8192);
            h.assign(leadIn_, 0.0);
        }
        outQ_.clear();
        outReadPos_ = 0;
        totalInFrames_ = 0;
        emittedOutFrames_ = 0;
        flushed_ = false;
        active_ = true;
        return true;
    }

    void reset() {
        active_ = false;
        flushed_ = false;
        srcRate_ = dstRate_ = ch_ = 0;
        nIn_ = nOut_ = hIn_ = stepIn_ = hOut_ = stepOut_ = leadIn_ = leadOut_ = 0;
        inHist_.clear();
        outQ_.clear();
        outReadPos_ = 0;
        totalInFrames_ = 0;
        emittedOutFrames_ = 0;
    }

    void clear() {
        if (!active_) return;
        for (auto& h : inHist_) h.assign(leadIn_, 0.0);
        outQ_.clear();
        outReadPos_ = 0;
        totalInFrames_ = 0;
        emittedOutFrames_ = 0;
        flushed_ = false;
    }

    bool isActive() const { return active_; }

    bool process(const float* in, size_t inFrames, float* out, size_t outCap, size_t& outFrames) {
        outFrames = 0;
        if (!active_) return false;
        if (in && inFrames > 0) {
            if (flushed_) clear();
            for (uint32_t c = 0; c < ch_; ++c) {
                auto& hist = inHist_[c];
                const size_t oldSz = hist.size();
                hist.resize(oldSz + inFrames);
                for (size_t i = 0; i < inFrames; ++i) hist[oldSz + i] = static_cast<double>(in[i * ch_ + c]);
            }
            totalInFrames_ += inFrames;
            while (inHist_[0].size() >= nIn_) {
                runBlock();
            }
        }
        popOutput(out, outCap, outFrames);
        return true;
    }

    bool flush(float* out, size_t outCap, size_t& outFrames) {
        outFrames = 0;
        if (!active_) return true;
        if (!flushed_) {
            const uint64_t targetTotal = static_cast<uint64_t>(std::llround(
                static_cast<double>(totalInFrames_) * static_cast<double>(dstRate_) / static_cast<double>(srcRate_)));
            uint64_t produced = emittedOutFrames_ + ((outQ_.size() - outReadPos_) / ch_);
            while (produced < targetTotal) {
                if (inHist_[0].size() < nIn_) {
                    for (uint32_t c = 0; c < ch_; ++c) inHist_[c].resize(nIn_, 0.0);
                }
                runBlock();
                produced += stepOut_;
            }
            if (produced > targetTotal) {
                const size_t excess = static_cast<size_t>(produced - targetTotal) * ch_;
                if (excess <= outQ_.size() - outReadPos_) {
                    outQ_.resize(outQ_.size() - excess);
                }
            }
            flushed_ = true;
        }
        popOutput(out, outCap, outFrames);
        return true;
    }

    double delayFrames() const {
        if (!active_) return 0.0;
        if (flushed_) return static_cast<double>((outQ_.size() - outReadPos_) / ch_);
        const double expTotal = static_cast<double>(totalInFrames_) * static_cast<double>(dstRate_) / static_cast<double>(srcRate_);
        return std::max(0.0, expTotal - static_cast<double>(emittedOutFrames_));
    }

private:
    void runBlock() {
        for (uint32_t c = 0; c < ch_; ++c) {
            fftIn_.forward(inHist_[c].data(), X_.data());
            std::fill(Y_.begin(), Y_.end(), C64{0.0, 0.0});
            for (size_t k = 0; k < nBinsCopy_; ++k) {
                Y_[k] = X_[k] * hScaled_[k];
            }
            if ((nOut_ & 1u) == 0u) Y_[nOut_ / 2].i = 0.0;
            fftOut_.inverse(Y_.data(), yBlk_.data());
            auto& dst = chOutBuf_[c];
            for (size_t j = 0; j < stepOut_; ++j) dst[j] = yBlk_[leadOut_ + j];
            auto& h = inHist_[c];
            const size_t rem = h.size() - stepIn_;
            std::copy(h.begin() + static_cast<std::ptrdiff_t>(stepIn_), h.end(), h.begin());
            h.resize(rem);
        }
        const size_t oldSz = outQ_.size();
        outQ_.resize(oldSz + stepOut_ * ch_);
        for (size_t j = 0; j < stepOut_; ++j) {
            for (uint32_t c = 0; c < ch_; ++c) {
                outQ_[oldSz + j * ch_ + c] = static_cast<float>(chOutBuf_[c][j]);
            }
        }
    }

    void popOutput(float* out, size_t outCap, size_t& outFrames) {
        const size_t avail = (outQ_.size() - outReadPos_) / ch_;
        const size_t take = std::min(outCap, avail);
        if (take > 0 && out) {
            std::copy(outQ_.begin() + static_cast<std::ptrdiff_t>(outReadPos_),
                      outQ_.begin() + static_cast<std::ptrdiff_t>(outReadPos_ + take * ch_), out);
            outReadPos_ += take * ch_;
            emittedOutFrames_ += take;
            if (outReadPos_ == outQ_.size()) {
                outQ_.clear();
                outReadPos_ = 0;
            } else if (outReadPos_ > 65536 * ch_) {
                const size_t rem = outQ_.size() - outReadPos_;
                std::copy(outQ_.begin() + static_cast<std::ptrdiff_t>(outReadPos_), outQ_.end(), outQ_.begin());
                outQ_.resize(rem);
                outReadPos_ = 0;
            }
        }
        outFrames = take;
    }

    bool active_ = false;
    bool flushed_ = false;
    uint32_t srcRate_ = 0, dstRate_ = 0, ch_ = 0;
    size_t nIn_ = 0, nOut_ = 0, hIn_ = 0, stepIn_ = 0, hOut_ = 0, stepOut_ = 0;
    size_t leadIn_ = 0, leadOut_ = 0;
    size_t nBinsOut_ = 0, nBinsCopy_ = 0;

    RealFft fftIn_, fftOut_;
    std::vector<C64> hScaled_;
    std::vector<C64> X_, Y_;
    std::vector<double> yBlk_;
    std::vector<std::vector<double>> inHist_;
    std::vector<std::vector<double>> chOutBuf_;
    std::vector<float> outQ_;
    size_t outReadPos_ = 0;
    uint64_t totalInFrames_ = 0;
    uint64_t emittedOutFrames_ = 0;
};

}  // namespace ee_analyzer::srcconv::fft64
