#pragma once
/**
 * @file fft.hpp
 * @brief Egyszerű, függőségmentes radix-2 komplex FFT előre számolt forgatótényezőkkel.
 *        A transform() valós idejű szálból is hívható (nem foglal memóriát).
 */
#include <complex>
#include <vector>
#include <cmath>
#include <cstddef>
#include <utility>

namespace ee_analyzer {

template <class T>
class Fft {
public:
    using cpx = std::complex<T>;

    explicit Fft(size_t n = 0) { if (n) init(n); }

    void init(size_t n) {
        n_ = n;
        const double pi = 3.14159265358979323846;
        tw_.resize(n / 2);
        for (size_t k = 0; k < n / 2; ++k) {
            tw_[k] = cpx(static_cast<T>(std::cos(-2.0 * pi * k / n)), static_cast<T>(std::sin(-2.0 * pi * k / n)));
        }
        rev_.resize(n);
        size_t bits = 0;
        while ((size_t(1) << bits) < n) ++bits;
        for (size_t i = 0; i < n; ++i) {
            size_t r = 0;
            for (size_t b = 0; b < bits; ++b) if (i & (size_t(1) << b)) r |= size_t(1) << (bits - 1 - b);
            rev_[i] = r;
        }
    }

    size_t size() const { return n_; }

    /** Helyben végzett transzformáció. inverse=true esetén 1/N skálázással. */
    void transform(cpx* a, bool inverse) const {
        const size_t n = n_;
        for (size_t i = 0; i < n; ++i) {
            const size_t j = rev_[i];
            if (i < j) std::swap(a[i], a[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1) {
            const size_t half = len >> 1;
            const size_t step = n / len;
            for (size_t i = 0; i < n; i += len) {
                for (size_t k = 0; k < half; ++k) {
                    cpx w = tw_[k * step];
                    if (inverse) w = std::conj(w);
                    const cpx u = a[i + k];
                    const cpx v = a[i + k + half] * w;
                    a[i + k] = u + v;
                    a[i + k + half] = u - v;
                }
            }
        }
        if (inverse) {
            const T s = T(1) / static_cast<T>(n);
            for (size_t i = 0; i < n; ++i) a[i] *= s;
        }
    }

private:
    size_t n_ = 0;
    std::vector<cpx> tw_;
    std::vector<size_t> rev_;
};

/** Következő 2-hatvány. */
inline size_t nextPow2(size_t v) {
    size_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

} // namespace ee_analyzer
