#pragma once
/**
 * @file mono_bass.hpp
 * @brief Mono és mélyvágás. A modul a láncban a Bass Enhancer után, a Crossfeed előtt fut.
 *
 * Két szűrő, mindkettő 24 dB/oktáves Linkwitz–Riley 4. rendű (két egymás utáni Butterworth 2. rendű szakasz):
 *   - Mélyvágás (10 / 20 / 30 Hz): a bal és a jobb csatornán, a teljes jelen.
 *   - Mono-keverés (0–100 %, a határfrekvencia alatt; 40–250 Hz, alapérték 100 Hz): az oldalsó jel (S = (L−R)/2) mély
 *     részét keveri a középsőbe: S_out = (1−a)·S + a·HP(S). A középső jel (M = (L+R)/2) érintetlen marad, így a fázisa
 *     sem változik. Alsó határ nincs: a legmélyebb sávot a mélyvágás kezeli, a mono-keverésnek ott a legkisebb a hatása.
 *
 * Kikapcsolt modulnál (a láncban nem hívjuk) és 0 % keverésnél, vágás nélkül a kimenet bit-pontosan a bemenet.
 */
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "param_block.hpp"

namespace ee_analyzer {
namespace mono {

constexpr double kPi = 3.14159265358979323846;
constexpr double kButterworthQ = 0.70710678118654752440;   // 1 / sqrt(2)
constexpr double kMonoHz = 100.0;                          // a mono-keverés felső határának alapértéke
constexpr double kMixRampMs = 20.0;                        // a keverés változása ennyi idő alatt simul

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

class MonoBass {
public:
    void setSampleRate(uint32_t sr) {
        sr_ = sr > 0 ? static_cast<double>(sr) : 48000.0;
        side_.design(monoHz_, sr_);
        side_.reset();
        aStep_ = 1.0 / (kMixRampMs * 0.001 * sr_);
        const int32_t hz = subsonicHz();
        subHz_ = -1;   // a jelenlegi fc-t új mintavételi frekvencián tervezzük újra
        setSubsonic(hz);
    }

    /** A valós idejű szálon, a beállítás életbe lépésekor. */
    void update(const ParamData& d) {
        aTarget_ = std::clamp(static_cast<double>(d.monoAmount) / 100.0, 0.0, 1.0);
        setSubsonic(d.subsonicHz);
        // Új határfrekvencia: csak az együtthatók cserélődnek, az állapot marad (a csúszka húzása közben sincs kattanás)
        const double hz = std::clamp(static_cast<double>(d.monoFreqHz), 40.0, 250.0);
        if (hz != monoHz_) { monoHz_ = hz; side_.design(monoHz_, sr_); }
    }

    double monoHz() const { return monoHz_; }

    /** Csak bekapcsolt modulnál hívandó. A mono-szűrő állapota a modul teljes ideje alatt fut, így a keverés bármikor pontos. */
    void process(float* L, float* R, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            double l = L[i], r = R[i];
            if (subHz_ > 0) {
                l = subL_.process(l);
                r = subR_.process(r);
            }
            const double s = 0.5 * (l - r);
            const double sLow = side_.process(s);   // az oldalsó jel a határfrekvencia fölötti része (HP), a keveréshez
            if (aCur_ != aTarget_) aCur_ += std::clamp(aTarget_ - aCur_, -aStep_, aStep_);
            if (aCur_ > 0.0) {
                const double m = 0.5 * (l + r);
                const double sOut = s + aCur_ * (sLow - s);
                l = m + sOut;
                r = m - sOut;
            }
            L[i] = static_cast<float>(l);
            R[i] = static_cast<float>(r);
        }
    }

    void reset() {
        subL_.reset();
        subR_.reset();
        side_.reset();
    }

    int32_t subsonicHz() const { return subHz_ < 0 ? 0 : subHz_; }

private:
    void setSubsonic(int32_t hz) {
        if (hz == subHz_) return;
        subHz_ = hz;
        if (subHz_ > 0) {
            subL_.design(static_cast<double>(subHz_), sr_);
            subR_.design(static_cast<double>(subHz_), sr_);
        }
        subL_.reset();
        subR_.reset();
    }

    double sr_ = 48000.0;
    double monoHz_ = kMonoHz;
    int32_t subHz_ = 0;
    HighPass4 subL_, subR_;
    HighPass4 side_;
    double aTarget_ = 0.0, aCur_ = 0.0, aStep_ = 0.0;
};

} // namespace mono
} // namespace ee_analyzer
