#pragma once
/**
 * @file spectrum.hpp
 * @brief Valódi FFT-alapú spektrumanalizátor a 3D vízeséshez.
 *
 * Hann-ablakos, radix-2 FFT; a lineáris FFT-binekből 0 Hz – 24 kHz közötti frekvenciasávokat képez, és 0..1
 * közé normált dB-értékeket ad vissza. A sávok a ln(1 + f / kWarpHz) tengelyen egyenközűek: kWarpHz fölött
 * közel logaritmikus, alatta lineáris, így a 0 Hz is a skálán van.
 * A mély sávok (kLowBlendHz alatt) egy decimált jel hosszabb ablakú FFT-jéből jönnek (kb. 3 Hz felbontás a fő
 * FFT 11,7 Hz-e helyett): a basszus szivárgása így nem ér le a legalsó sávokig.
 * Kimenet: L+R közép (M) spektrum, valamint sávonkénti L/R fáziskülönbség.
 */
#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <cstddef>

namespace ee_analyzer {

class SpectrumAnalyzer {
public:
    /** A frekvenciatengely torzítása: a sávhatárok ln(1 + f / kWarpHz) szerint egyenközűek. */
    static constexpr double kWarpHz = 10.0;
    /** A mély sávok elemzése: ez alatt csak a decimált FFT, kLowBlendHz-ig átmenet a fő FFT felé. */
    static constexpr double kLowOnlyHz = 45.0, kLowBlendHz = 75.0;
    /** A decimált jel mintavétele (kb.) és a mély FFT mérete. */
    static constexpr double kLowRateHz = 1500.0;
    static constexpr size_t kLowFft = 512;

    explicit SpectrumAnalyzer(size_t fftSize = 4096, size_t numBands = 160, double maxHz = 24000.0,
                              double floorDb = -90.0, double ceilDb = 0.0)
        : n_(fftSize), bands_(numBands), floorDb_(floorDb), ceilDb_(ceilDb),
          warpSpan_(std::log1p(maxHz / kWarpHz)) {
        history_.assign(n_ * 2, 0.0f);
        window_ = hann(n_);
        lowWindow_ = hann(kLowFft);
        bufM_.resize(n_);
        bufL_.resize(n_);
        bufR_.resize(n_);
        bufLow_.resize(kLowFft);
        lowHistory_.assign(kLowFft, 0.0f);
        levels_.assign(bands_, 0.0f);
        phaseDeg_.assign(bands_, 0.0f);
        twiddle_ = twiddles(n_);
        lowTwiddle_ = twiddles(kLowFft);
        setSampleRate(48000.0);
    }

    size_t fftSize() const { return n_; }
    size_t numBands() const { return bands_; }

    /** A b. sávhatár (0 .. numBands) frekvenciája Hz-ben; a b. sáv [bandEdgeHz(b), bandEdgeHz(b + 1)). */
    double bandEdgeHz(double b) const { return kWarpHz * std::expm1(warpSpan_ * b / static_cast<double>(bands_)); }
    /** A b. sáv közepe (a torzított tengelyen). */
    double bandCenterHz(size_t b) const { return bandEdgeHz(static_cast<double>(b) + 0.5); }
    /** A frekvenciát tartalmazó sáv. */
    size_t bandOf(double hz) const {
        const double b = std::log1p(std::max(0.0, hz) / kWarpHz) / warpSpan_ * static_cast<double>(bands_);
        return std::min(bands_ - 1, static_cast<size_t>(b));
    }

    /**
     * A bemenet mintavétele (a mély elemzés decimálásához). Változáskor a mély elemzés újraindul;
     * a compute() is beállítja, de a push() előtt kell, ha a mintavétel megváltozhat.
     */
    void setSampleRate(double sr) {
        if (!(sr > 0.0) || std::abs(sr - sampleRate_) < 0.5) return;
        sampleRate_ = sr;
        decim_ = std::max<size_t>(1, static_cast<size_t>(std::lround(sr / kLowRateHz)));
        lowRate_ = sr / static_cast<double>(decim_);
        // 8-adrendű Butterworth aluláteresztő (négy biquad) a decimálás előtt: a mély sávokba visszatükröződő
        // részek a -90 dB-es alsó határ alatt maradnak
        const double fc = std::min(200.0, 0.3 * lowRate_);
        const double pi = 3.14159265358979323846;
        const double w0 = 2.0 * pi * fc / sr, cw = std::cos(w0), sw = std::sin(w0);
        for (size_t k = 0; k < 4; ++k) {
            const double q = 1.0 / (2.0 * std::cos(pi * (2.0 * k + 1.0) / 16.0));
            const double alpha = sw / (2.0 * q), a0 = 1.0 + alpha;
            Biquad& s = lowpass_[k];
            s.b0 = (1.0 - cw) / 2.0 / a0;
            s.b1 = (1.0 - cw) / a0;
            s.b2 = s.b0;
            s.a1 = -2.0 * cw / a0;
            s.a2 = (1.0 - alpha) / a0;
            s.z1 = s.z2 = 0.0;
        }
        decimCount_ = 0;
        std::fill(lowHistory_.begin(), lowHistory_.end(), 0.0f);
    }

    /** Interleaved sztereó minták betolása a csúszó ablakba. */
    void push(const float* lr, size_t frames) {
        // Mély elemzés: minden minta a szűrőn át, minden decim_. a decimált pufferbe
        for (size_t i = 0; i < frames; ++i) {
            double y = 0.5 * (static_cast<double>(lr[2 * i]) + static_cast<double>(lr[2 * i + 1]));
            for (Biquad& s : lowpass_) y = s.process(y);
            if (++decimCount_ >= decim_) {
                decimCount_ = 0;
                lowHistory_[lowPos_] = static_cast<float>(y);
                lowPos_ = (lowPos_ + 1) % kLowFft;
            }
        }
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
        setSampleRate(sampleRate);
        for (size_t i = 0; i < n_; ++i) {
            const float w = window_[i];
            const float l = history_[2 * i];
            const float r = history_[2 * i + 1];
            bufM_[i] = { 0.5f * (l + r) * w, 0.0f };
            bufL_[i] = { l * w, 0.0f };
            bufR_[i] = { r * w, 0.0f };
        }
        fft(bufM_, twiddle_);
        fft(bufL_, twiddle_);
        fft(bufR_, twiddle_);

        // Mély FFT: a decimált puffer időrendben, az egyenáramú összetevő nélkül (az ablakkal súlyozott átlagot
        // vonjuk le: így a 0. bin pontosan nulla, a sima átlag az ablakon belüli töredék periódustól függene)
        double sum = 0.0, wsum = 0.0;
        for (size_t i = 0; i < kLowFft; ++i) {
            sum += static_cast<double>(lowWindow_[i]) * lowHistory_[(lowPos_ + i) % kLowFft];
            wsum += lowWindow_[i];
        }
        const double mean = sum / wsum;
        for (size_t i = 0; i < kLowFft; ++i) {
            const float v = static_cast<float>(lowHistory_[(lowPos_ + i) % kLowFft] - mean);
            bufLow_[i] = { v * lowWindow_[i], 0.0f };
        }
        fft(bufLow_, lowTwiddle_);

        // Hann ablak koherens erősítése 0.5 -> teljes kivezérlésű szinusz csúcsa n/4
        const double norm = 4.0 / static_cast<double>(n_);
        const double lowNorm = 4.0 / static_cast<double>(kLowFft);
        const double binHz = sampleRate / static_cast<double>(n_);
        const double lowBinHz = lowRate_ / static_cast<double>(kLowFft);
        // A sávhatárok minden mintavételi frekvencián azonosak (a felület skálája fix 0 Hz – 24 kHz);
        // a Nyquist-frekvencia fölötti sávok (pl. 44,1 kHz-nél 22,05 kHz felett) üresek maradnak.
        const double nyquist = sampleRate * 0.5;
        const double pi = 3.14159265358979323846;

        for (size_t b = 0; b < bands_; ++b) {
            const double f0 = bandEdgeHz(static_cast<double>(b));
            const double f1 = bandEdgeHz(static_cast<double>(b) + 1.0);
            if (f0 >= nyquist) {
                levels_[b] = 0.0f;
                phaseDeg_[b] = 0.0f;
                continue;
            }
            std::complex<double> cross(0.0, 0.0);
            const double peak = bandPeak(bufM_, f0 / binHz, f1 / binHz, 1.0, &cross);
            double db = 20.0 * std::log10(std::max(1e-12, peak * norm));
            const double fc = 0.5 * (f0 + f1);
            if (fc < kLowBlendHz) {
                const double lowPeak = bandPeak(bufLow_, f0 / lowBinHz, f1 / lowBinHz, 0.0, nullptr);
                const double lowDb = 20.0 * std::log10(std::max(1e-12, lowPeak * lowNorm));
                const double w = std::clamp((kLowBlendHz - fc) / (kLowBlendHz - kLowOnlyHz), 0.0, 1.0);
                // A két felbontás közül a kisebb érték: a hosszú ablak időbeli elkenődése (egy elhalt basszushang
                // még 340 ms-ig látszana, a hangindítás szétterülne 0 Hz-ig) a rövid ablakban nincs meg, a rövid
                // ablak frekvencia-szivárgása pedig a hosszúban
                db = w * std::min(lowDb, db) + (1.0 - w) * db;
            }
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
    struct Biquad {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0;
        double process(double x) {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    /**
     * Egy sáv csúcsértéke a [k0, k1] bin-tartományban; a binnél keskenyebb sáv a középfrekvencián lineárisan
     * interpolál. minBin: a legkisebb használható bin (a fő FFT-ben 1, mert ott az egyenáram nincs levonva).
     * cross: ha nem null, az L/R keresztspektrum összege (fáziskülönbséghez, a fő FFT-ből).
     */
    double bandPeak(const std::vector<std::complex<float>>& buf, double k0, double k1, double minBin,
                    std::complex<double>* cross) const {
        const double maxBin = static_cast<double>(buf.size() / 2 - 1);
        double peak = 0.0;
        if (k1 - k0 < 1.0) {
            const double kc = std::clamp(0.5 * (k0 + k1), minBin, maxBin - 1.0);
            const size_t ki = static_cast<size_t>(kc);
            const double t = kc - static_cast<double>(ki);
            const double m0 = std::abs(buf[ki]), m1 = std::abs(buf[ki + 1]);
            peak = m0 + (m1 - m0) * t;
            if (cross) *cross = std::complex<double>(bufL_[ki]) * std::conj(std::complex<double>(bufR_[ki]));
        } else {
            const size_t a = static_cast<size_t>(std::clamp(std::ceil(k0), minBin, maxBin));
            const size_t e = static_cast<size_t>(std::clamp(std::floor(k1), minBin, maxBin));
            for (size_t k = a; k <= e; ++k) {
                peak = std::max(peak, static_cast<double>(std::abs(buf[k])));
                if (cross) *cross += std::complex<double>(bufL_[k]) * std::conj(std::complex<double>(bufR_[k]));
            }
        }
        return peak;
    }

    static std::vector<float> hann(size_t n) {
        std::vector<float> w(n);
        const double pi = 3.14159265358979323846;
        for (size_t i = 0; i < n; ++i) w[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * pi * i / (n - 1)));
        return w;
    }

    static std::vector<std::complex<float>> twiddles(size_t n) {
        std::vector<std::complex<float>> tw(n / 2);
        const double pi = 3.14159265358979323846;
        for (size_t k = 0; k < n / 2; ++k) tw[k] = std::polar(1.0f, static_cast<float>(-2.0 * pi * k / n));
        return tw;
    }

    static void fft(std::vector<std::complex<float>>& a, const std::vector<std::complex<float>>& tw) {
        const size_t n = a.size();
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
                    const std::complex<float> v = a[i + k + len / 2] * tw[k * step];
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                }
            }
        }
    }

    size_t n_, bands_;
    double floorDb_, ceilDb_, warpSpan_;
    double sampleRate_ = 0.0, lowRate_ = kLowRateHz;
    size_t decim_ = 1, decimCount_ = 0, lowPos_ = 0;
    Biquad lowpass_[4];
    std::vector<float> history_, window_, lowWindow_, lowHistory_, levels_, phaseDeg_;
    std::vector<std::complex<float>> bufM_, bufL_, bufR_, bufLow_, twiddle_, lowTwiddle_;
};

} // namespace ee_analyzer
