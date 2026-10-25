#pragma once
/**
 * @file mono_bass.hpp
 * @brief Mélyvágás (10 / 20 / 30 Hz) felüláteresztővel. A modul a láncban a Bass Enhancer után, a Crossfeed előtt fut.
 *
 * A szűrő 24 dB/oktáves Linkwitz–Riley 4. rendű: két egymás utáni Butterworth 2. rendű szakasz. Ezt a bal és a jobb
 * csatornán, a teljes jelen alkalmazzuk. A modul kikapcsolt állapotban (a láncban) nem fut, így bit-pontosan átlátszó.
 */
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "param_block.hpp"

namespace ee_analyzer {
namespace mono {

constexpr double kPi = 3.14159265358979323846;
constexpr double kButterworthQ = 0.70710678118654752440;   // 1 / sqrt(2)

/** Másodrendű felüláteresztő (RBJ-kialakítás), kétszeres pontosságú állapottal. */
class HighPass2 {
public:
    void design(double fc, double sr) {
        const double w0 = 2.0 * kPi * fc / sr;
        const double cw = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * kButterworthQ);
        const double a0 = 1.0 + alpha;
        b0_ = (1.0 + cw) / (2.0 * a0);
        b1_ = -(1.0 + cw) / a0;
        a1_ = -2.0 * cw / a0;
        a2_ = (1.0 - alpha) / a0;
    }

    double process(double x) {
        const double y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b0_ * x - a2_ * y;
        return y;
    }

    void reset() { z1_ = z2_ = 0.0; }

private:
    double b0_ = 0.0, b1_ = 0.0, a1_ = 0.0, a2_ = 0.0;
    double z1_ = 0.0, z2_ = 0.0;
};

/** 24 dB/oktáves (Linkwitz–Riley 4.) felüláteresztő: két azonos Butterworth-szakasz sorba kötve. */
class HighPass4 {
public:
    void design(double fc, double sr) { a_.design(fc, sr); b_.design(fc, sr); }
    double process(double x) { return b_.process(a_.process(x)); }
    void reset() { a_.reset(); b_.reset(); }

private:
    HighPass2 a_, b_;
};

/** A mélyvágás modul. A fc-t (10 / 20 / 30 Hz, 0 = ki) a paraméterekből kapja; a szűrőt csak változáskor tervezi újra. */
class MonoBass {
public:
    void setSampleRate(uint32_t sr) {
        sr_ = sr > 0 ? static_cast<double>(sr) : 48000.0;
        const int32_t hz = subsonicHz();
        subHz_ = -1;   // a jelenlegi fc-t új mintavételi frekvencián tervezzük újra
        update(hz);
    }

    /** A valós idejű szálon, a beállítás életbe lépésekor. */
    void update(int32_t subsonicHz) {
        if (subsonicHz == subHz_) return;
        subHz_ = subsonicHz;
        if (subHz_ > 0) {
            subL_.design(static_cast<double>(subHz_), sr_);
            subR_.design(static_cast<double>(subHz_), sr_);
        }
        reset();
    }

    /** Csak bekapcsolt modulnál hívandó; a fc = 0 (ki) ilyenkor sincs hatással. */
    void process(float* L, float* R, size_t n) {
        if (subHz_ <= 0) return;
        for (size_t i = 0; i < n; ++i) {
            L[i] = static_cast<float>(subL_.process(L[i]));
            R[i] = static_cast<float>(subR_.process(R[i]));
        }
    }

    void reset() {
        subL_.reset();
        subR_.reset();
    }

    int32_t subsonicHz() const { return subHz_ < 0 ? 0 : subHz_; }

private:
    double sr_ = 48000.0;
    int32_t subHz_ = 0;
    HighPass4 subL_, subR_;
};

} // namespace mono
} // namespace ee_analyzer
