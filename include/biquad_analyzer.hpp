#pragma once
#include <vector>
#include <cmath>
#include <complex>

namespace ee_analyzer {

/**
 * @brief Szűrőtípusok (Robert Bristow-Johnson Audio EQ Cookbook alapján)
 */
enum class FilterType {
    PeakingEQ,
    LowShelf,
    HighShelf,
    LowPass,
    HighPass,
    BandPass,
    Notch,
    AllPass
};

/**
 * @brief Digitális Biquad (másodfokú IIR) szűrő átviteli függvénye:
 *        H(z) = (b0 + b1*z^-1 + b2*z^-2) / (a0 + a1*z^-1 + a2*z^-2)
 */
struct BiquadCoefficients {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a0 = 1.0, a1 = 0.0, a2 = 0.0;
};

class BiquadFilter {
public:
    FilterType type = FilterType::PeakingEQ;
    double frequency = 1000.0; // Hz
    double qFactor = 0.7071;    // Q
    double gainDb = 0.0;       // dB (csak Peaking és Shelf esetén)
    double sampleRate = 48000.0; // Hz

    BiquadFilter() = default;
    BiquadFilter(FilterType t, double f, double q, double g = 0.0, double sr = 48000.0)
        : type(t), frequency(f), qFactor(q), gainDb(g), sampleRate(sr) {}

    /**
     * @brief RBJ Cookbook alapján kiszámolja az IIR együtthatókat
     */
    BiquadCoefficients calculateCoefficients() const {
        BiquadCoefficients coeff;
        const double pi = 3.14159265358979323846;
        const double w0 = 2.0 * pi * frequency / sampleRate;
        const double cos_w0 = std::cos(w0);
        const double sin_w0 = std::sin(w0);
        const double alpha = sin_w0 / (2.0 * qFactor);
        const double A = std::pow(10.0, gainDb / 40.0); // sqrt(10^(gainDb/20))

        switch (type) {
            case FilterType::PeakingEQ: {
                coeff.b0 = 1.0 + alpha * A;
                coeff.b1 = -2.0 * cos_w0;
                coeff.b2 = 1.0 - alpha * A;
                coeff.a0 = 1.0 + alpha / A;
                coeff.a1 = -2.0 * cos_w0;
                coeff.a2 = 1.0 - alpha / A;
                break;
            }
            case FilterType::LowPass: {
                coeff.b0 = (1.0 - cos_w0) / 2.0;
                coeff.b1 = 1.0 - cos_w0;
                coeff.b2 = (1.0 - cos_w0) / 2.0;
                coeff.a0 = 1.0 + alpha;
                coeff.a1 = -2.0 * cos_w0;
                coeff.a2 = 1.0 - alpha;
                break;
            }
            case FilterType::HighPass: {
                coeff.b0 = (1.0 + cos_w0) / 2.0;
                coeff.b1 = -(1.0 + cos_w0);
                coeff.b2 = (1.0 + cos_w0) / 2.0;
                coeff.a0 = 1.0 + alpha;
                coeff.a1 = -2.0 * cos_w0;
                coeff.a2 = 1.0 - alpha;
                break;
            }
            case FilterType::LowShelf: {
                const double two_sqrtA_alpha = 2.0 * std::sqrt(A) * alpha;
                coeff.b0 = A * ((A + 1.0) - (A - 1.0) * cos_w0 + two_sqrtA_alpha);
                coeff.b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cos_w0);
                coeff.b2 = A * ((A + 1.0) - (A - 1.0) * cos_w0 - two_sqrtA_alpha);
                coeff.a0 = (A + 1.0) + (A - 1.0) * cos_w0 + two_sqrtA_alpha;
                coeff.a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cos_w0);
                coeff.a2 = (A + 1.0) + (A - 1.0) * cos_w0 - two_sqrtA_alpha;
                break;
            }
            case FilterType::HighShelf: {
                const double two_sqrtA_alpha = 2.0 * std::sqrt(A) * alpha;
                coeff.b0 = A * ((A + 1.0) + (A - 1.0) * cos_w0 + two_sqrtA_alpha);
                coeff.b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cos_w0);
                coeff.b2 = A * ((A + 1.0) + (A - 1.0) * cos_w0 - two_sqrtA_alpha);
                coeff.a0 = (A + 1.0) - (A - 1.0) * cos_w0 + two_sqrtA_alpha;
                coeff.a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cos_w0);
                coeff.a2 = (A + 1.0) - (A - 1.0) * cos_w0 - two_sqrtA_alpha;
                break;
            }
            case FilterType::AllPass: {
                coeff.b0 = 1.0 - alpha;
                coeff.b1 = -2.0 * cos_w0;
                coeff.b2 = 1.0 + alpha;
                coeff.a0 = 1.0 + alpha;
                coeff.a1 = -2.0 * cos_w0;
                coeff.a2 = 1.0 - alpha;
                break;
            }
            default:
                break;
        }

        // a0-val normalizálunk
        if (coeff.a0 != 1.0 && coeff.a0 != 0.0) {
            coeff.b0 /= coeff.a0;
            coeff.b1 /= coeff.a0;
            coeff.b2 /= coeff.a0;
            coeff.a1 /= coeff.a0;
            coeff.a2 /= coeff.a0;
            coeff.a0 = 1.0;
        }
        return coeff;
    }

    /**
     * @brief Kiszámítja az átviteli függvényt H(e^jw) adott f frekvencián
     */
    std::complex<double> evaluateTransferFunction(double targetFreq) const {
        const double pi = 3.14159265358979323846;
        const double w = 2.0 * pi * targetFreq / sampleRate;
        const auto c = calculateCoefficients();

        const std::complex<double> z1(std::cos(-w), std::sin(-w));
        const std::complex<double> z2(std::cos(-2.0 * w), std::sin(-2.0 * w));

        const std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
        const std::complex<double> den = 1.0 + c.a1 * z1 + c.a2 * z2;

        return num / den;
    }

    /**
     * @brief Kiszámítja a fázisszöget radiánban [-pi, pi]
     */
    double phaseAt(double targetFreq) const {
        return std::arg(evaluateTransferFunction(targetFreq));
    }

    /**
     * @brief Zárt képletű csoportkésleltetés másodpercben: tau_g = -d(phi)/dw
     *        Numerikus deriválással (kis delta w-vel) garantált stabilitással
     */
    double groupDelaySecondsAt(double targetFreq) const {
        const double deltaF = 0.5; // 0.5 Hz differencia
        const double f1 = std::max(1.0, targetFreq - deltaF);
        const double f2 = targetFreq + deltaF;

        const double p1 = phaseAt(f1);
        double p2 = phaseAt(f2);

        // Fázis átfordulás (phase unwrap) korrekció
        const double pi = 3.14159265358979323846;
        while (p2 - p1 > pi) p2 -= 2.0 * pi;
        while (p2 - p1 < -pi) p2 += 2.0 * pi;

        const double deltaW = 2.0 * pi * (f2 - f1);
        const double dPhi = p2 - p1;

        return -dPhi / deltaW; // Késleltetés másodpercben
    }
};

/**
 * @brief Teljes kaszkádolt EQ szűrősorozat kezelője (mint az EasyEffects EQ modulja)
 */
class FilterChain {
public:
    std::vector<BiquadFilter> filters;
    double sampleRate = 48000.0;

    void addFilter(const BiquadFilter& f) {
        filters.push_back(f);
    }

    void clear() {
        filters.clear();
    }

    /**
     * @brief A teljes lánc eredő amplitúdóátvitele dB-ben
     */
    double magnitudeDbAt(double freq) const {
        std::complex<double> totalH(1.0, 0.0);
        for (const auto& f : filters) {
            totalH *= f.evaluateTransferFunction(freq);
        }
        return 20.0 * std::log10(std::max(1e-6, std::abs(totalH)));
    }

    /**
     * @brief A teljes lánc eredő fázistolása fokban (-180° és +180° között)
     */
    double phaseDegreesAt(double freq) const {
        std::complex<double> totalH(1.0, 0.0);
        for (const auto& f : filters) {
            totalH *= f.evaluateTransferFunction(freq);
        }
        const double rad = std::arg(totalH);
        return rad * (180.0 / 3.14159265358979323846);
    }

    /**
     * @brief A teljes lánc eredő csoportkésleltetése milliszekundumban (ms)
     */
    double groupDelayMsAt(double freq) const {
        double totalSeconds = 0.0;
        for (const auto& f : filters) {
            totalSeconds += f.groupDelaySecondsAt(freq);
        }
        return totalSeconds * 1000.0;
    }
};

} // namespace ee_analyzer
