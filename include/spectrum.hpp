#pragma once
/**
 * @file spectrum.hpp
 * @brief Valódi FFT-alapú spektrumanalizátor a 3D vízeséshez.
 *
 * Hann-ablakos, radix-2 FFT; a lineáris FFT-binekből logaritmikus (10 Hz - 24 kHz)
 * frekvenciasávokat képez, és 0..1 közé normált dB-értékeket ad vissza.
 * Kimenet: L+R közép (M) spektrum, valamint sávonkénti L/R fáziskülönbség és koherencia-jellegű korreláció.
 */
#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <cstddef>

namespace ee_analyzer {

class SpectrumAnalyzer {
public:
    explicit SpectrumAnalyzer(size_t fftSize = 4096, size_t numBands = 160,
                              double minHz = 10.0, double maxHz = 24000.0,
                              double floorDb = -90.0, double ceilDb = 0.0)
        : n_(fftSize), bands_(numBands), minHz_(minHz), maxHz_(maxHz),
          floorDb_(floorDb), ceilDb_(ceilDb) {
        history_.assign(n_ * 2, 0.0f);
        window_.resize(n_);
        const double pi = 3.14159265358979323846;
        for (size_t i = 0; i < n_; ++i) {
            window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * pi * i / (n_ - 1)));
        }
        bufM_.resize(n_);
        bufL_.resize(n_);
        bufR_.resize(n_);
        levels_.assign(bands_, 0.0f);
        phaseDeg_.assign(bands_, 0.0f);
        buildTwiddles();
    }

    size_t fftSize() const { return n_; }
    size_t numBands() const { return bands_; }

    /** Interleaved sztereó minták betolása a csúszó ablakba. */
    void push(const float* lr, size_t frames) {
        if (frames >= n_) {
            lr += (frames - n_) * 2;
            frames = n_;
        }
        const size_t keep = (n_ - frames) * 2;
        std::move(history_.begin() + frames * 2, history_.end(), history_.begin());
        std::copy(lr, lr + frames * 2, history_.begin() + keep);
    }

    /**
     * Spektrum kiszámítása az aktuális ablakra.
     * @param sampleRate a jel mintavételi frekvenciája
     */
    void compute(double sampleRate) {
        for (size_t i = 0; i < n_; ++i) {
            const float w = window_[i];
            const float l = history_[2 * i];
            const float r = history_[2 * i + 1];
            bufM_[i] = { 0.5f * (l + r) * w, 0.0f };
            bufL_[i] = { l * w, 0.0f };
            bufR_[i] = { r * w, 0.0f };
        }
        fft(bufM_);
        fft(bufL_);
        fft(bufR_);

        // Hann ablak koherens erősítése 0.5 -> teljes kivezérlésű szinusz csúcsa n/4
        const double norm = 4.0 / static_cast<double>(n_);
        const double binHz = sampleRate / static_cast<double>(n_);
        const double maxBin = static_cast<double>(n_ / 2 - 1);
        // A sávhatárok minden mintavételi frekvencián azonosak (a felület skálája fix 10 Hz – 24 kHz);
        // a Nyquist-frekvencia fölötti sávok (pl. 44,1 kHz-nél 22,05 kHz felett) üresek maradnak.
        const double logMin = std::log10(minHz_), logMax = std::log10(maxHz_);
        const double nyquist = sampleRate * 0.5;
        const double pi = 3.14159265358979323846;

        for (size_t b = 0; b < bands_; ++b) {
            const double f0 = std::pow(10.0, logMin + (logMax - logMin) * b / bands_);
            const double f1 = std::pow(10.0, logMin + (logMax - logMin) * (b + 1) / bands_);
            if (f0 >= nyquist) {
                levels_[b] = 0.0f;
                phaseDeg_[b] = 0.0f;
                continue;
            }
            double k0 = f0 / binHz, k1 = f1 / binHz;

            double peak = 0.0;
            std::complex<double> cross(0.0, 0.0);
            if (k1 - k0 < 1.0) {
                // Keskenyebb sáv, mint egy FFT bin: lineáris interpoláció a középfrekvencián
                const double kc = std::clamp(0.5 * (k0 + k1), 1.0, maxBin);
                const size_t ki = static_cast<size_t>(kc);
                const double t = kc - ki;
                const double m0 = std::abs(bufM_[ki]), m1 = std::abs(bufM_[ki + 1]);
                peak = m0 + (m1 - m0) * t;
                cross = std::complex<double>(bufL_[ki]) * std::conj(std::complex<double>(bufR_[ki]));
            } else {
                const size_t a = static_cast<size_t>(std::clamp(std::ceil(k0), 1.0, maxBin));
                const size_t e = static_cast<size_t>(std::clamp(std::floor(k1), 1.0, maxBin));
                for (size_t k = a; k <= e; ++k) {
                    peak = std::max(peak, static_cast<double>(std::abs(bufM_[k])));
                    cross += std::complex<double>(bufL_[k]) * std::conj(std::complex<double>(bufR_[k]));
                }
            }
            const double db = 20.0 * std::log10(std::max(1e-12, peak * norm));
            levels_[b] = static_cast<float>(std::clamp((db - floorDb_) / (ceilDb_ - floorDb_), 0.0, 1.0));
            phaseDeg_[b] = (std::abs(cross) > 1e-12) ? static_cast<float>(std::arg(cross) * 180.0 / pi) : 0.0f;
        }
    }

    /** Sávonkénti szint 0..1 (floorDb..ceilDb leképezés). */
    const std::vector<float>& levels() const { return levels_; }
    /** Sávonkénti L-R fáziskülönbség fokban (-180..180). */
    const std::vector<float>& phaseDegrees() const { return phaseDeg_; }

    /** Pillanatnyi csúcsszint dBFS-ben a csúszó pufferből (-90 .. +12 dBFS). */
    float peakDb() const {
        float mx = 0.0f;
        for (size_t i = 0; i < n_ * 2; ++i) {
            const float a = std::abs(history_[i]);
            if (a > mx) mx = a;
        }
        return mx > 1e-5f ? static_cast<float>(20.0 * std::log10(mx)) : -90.0f;
    }

private:
    void buildTwiddles() {
        twiddle_.resize(n_ / 2);
        const double pi = 3.14159265358979323846;
        for (size_t k = 0; k < n_ / 2; ++k) {
            twiddle_[k] = std::polar(1.0f, static_cast<float>(-2.0 * pi * k / n_));
        }
    }

    void fft(std::vector<std::complex<float>>& a) const {
        const size_t n = n_;
        for (size_t i = 1, j = 0; i < n; ++i) {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1) {
            const size_t step = n / len;
            for (size_t i = 0; i < n; i += len) {
                for (size_t k = 0; k < len / 2; ++k) {
                    const std::complex<float> u = a[i + k];
                    const std::complex<float> v = a[i + k + len / 2] * twiddle_[k * step];
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                }
            }
        }
    }

    size_t n_, bands_;
    double minHz_, maxHz_, floorDb_, ceilDb_;
    std::vector<float> history_, window_, levels_, phaseDeg_;
    std::vector<std::complex<float>> bufM_, bufL_, bufR_, twiddle_;
};

} // namespace ee_analyzer
