#pragma once
/**
 * @file fir_design.hpp
 * @brief FIR tervezés (háttérszálban): EQ görbe -> lineáris / köztes / minimálfázisú FIR,
 *        impulzusválasz előkészítése (újramintavételezés, minimálfázisúvá alakítás, normalizálás).
 */
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>
#include "fft.hpp"
#include "biquad_analyzer.hpp"

namespace ee_analyzer {

enum class FirPhase { Linear = 1, Intermediate = 2, Minimum = 3 };

namespace fir_detail {

constexpr double kPi = 3.14159265358979323846;

/** Teljes (Hermitikus) amplitúdóspektrum -> minimálfázisú komplex spektrum (cepstrum módszer). */
inline std::vector<std::complex<double>> minimumPhaseSpectrum(const std::vector<double>& mag) {
    const size_t P = mag.size();
    Fft<double> fft(P);
    std::vector<std::complex<double>> c(P);
    for (size_t k = 0; k < P; ++k) c[k] = std::log(std::max(mag[k], 1e-9));
    fft.transform(c.data(), true); // valós cepstrum
    // "Hajtogatás": kauzális rész kétszerezése
    for (size_t n = 1; n < P / 2; ++n) c[n] *= 2.0;
    for (size_t n = P / 2 + 1; n < P; ++n) c[n] = 0.0;
    c[0] = c[0].real();
    c[P / 2] = c[P / 2].real();
    fft.transform(c.data(), false); // log H_min (valós rész = ln|H|, képzetes = kicsomagolt fázis)
    return c;
}

/** Hermitikus spektrum (komplex) -> valós impulzusválasz. */
inline std::vector<double> realIfft(std::vector<std::complex<double>> spec) {
    Fft<double> fft(spec.size());
    fft.transform(spec.data(), true);
    std::vector<double> out(spec.size());
    for (size_t i = 0; i < spec.size(); ++i) out[i] = spec[i].real();
    return out;
}

} // namespace fir_detail

/** Javasolt FIR hossz a mintavételi frekvenciához (~170 ms körüli felbontás). */
inline size_t recommendedEqFirTaps(double sr) {
    if (sr <= 50000.0) return 8192;
    if (sr <= 100000.0) return 16384;
    return 32768;
}

/**
 * Az EQ szűrőlánc amplitúdómenetéből FIR-t tervez.
 * @param latency [ki] a szűrő csoportkésleltetése mintában (lineáris: taps/2, köztes: ~5%, minimál: 0)
 */
inline std::vector<float> designEqFir(const std::vector<BiquadFilter>& filters, double sr, FirPhase phase,
                                      size_t taps, size_t* latency = nullptr) {
    using namespace fir_detail;
    (void)sr; // a szűrők már a megadott mintavételi frekvenciára vannak hangolva
    taps = nextPow2(std::max<size_t>(taps, 512));
    const size_t P = taps * 4; // sűrűbb frekvencia-mintavétel a cepstrum aliasing ellen
    std::vector<BiquadCoefficients> co;
    co.reserve(filters.size());
    for (const auto& f : filters) co.push_back(f.calculateCoefficients());

    // Amplitúdómenet a P pontos rácson (Hermitikus: |H(k)| = |H(P-k)|)
    std::vector<double> mag(P, 1.0);
    for (size_t k = 0; k <= P / 2; ++k) {
        const double w = 2.0 * kPi * static_cast<double>(k) / static_cast<double>(P);
        const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
        std::complex<double> h(1.0, 0.0);
        for (const auto& c : co) h *= (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
        mag[k] = std::abs(h);
        if (k > 0 && k < P / 2) mag[P - k] = mag[k];
    }

    std::vector<double> h;
    size_t lat = 0;
    if (phase == FirPhase::Linear) {
        // Nullfázisú spektrum -> középre tolt, szimmetrikus FIR
        std::vector<std::complex<double>> spec(P);
        lat = taps / 2;
        for (size_t k = 0; k < P; ++k) {
            const double w = 2.0 * kPi * static_cast<double>(k) / static_cast<double>(P);
            spec[k] = std::polar(mag[k], -w * static_cast<double>(lat));
        }
        h = realIfft(std::move(spec));
        h.resize(taps);
        for (size_t i = 0; i < taps; ++i) { // Hann ablak
            h[i] *= 0.5 - 0.5 * std::cos(2.0 * kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(taps));
        }
    } else {
        std::vector<std::complex<double>> logMin = minimumPhaseSpectrum(mag);
        const double beta = (phase == FirPhase::Minimum) ? 1.0 : 0.5;
        lat = (phase == FirPhase::Minimum) ? 0 : static_cast<size_t>(0.05 * static_cast<double>(taps));
        std::vector<std::complex<double>> spec(P);
        for (size_t k = 0; k < P; ++k) {
            // A kicsomagolt fázist a [0, P/2] tartományból számoljuk, és tükrözzük (Hermitikus szimmetria)
            const size_t kk = k <= P / 2 ? k : P - k;
            const double sign = k <= P / 2 ? 1.0 : -1.0;
            const double w = 2.0 * kPi * static_cast<double>(kk) / static_cast<double>(P);
            const double ph = beta * logMin[kk].imag() - w * static_cast<double>(lat);
            spec[k] = std::polar(mag[k], sign * ph);
        }
        h = realIfft(std::move(spec));
        h.resize(taps);
        // Végek simítása: rövid felfutás a köztes fázis előcsengése előtt, lecsengő farok a végén
        const size_t fadeOut = taps / 8;
        for (size_t i = 0; i < fadeOut; ++i) {
            const double g = 0.5 + 0.5 * std::cos(kPi * (static_cast<double>(i) + 1.0) / static_cast<double>(fadeOut));
            h[taps - fadeOut + i] *= g;
        }
        if (lat > 1) {
            for (size_t i = 0; i < lat / 2; ++i) {
                const double g = 0.5 - 0.5 * std::cos(kPi * static_cast<double>(i) / static_cast<double>(lat / 2));
                h[i] *= g;
            }
        }
    }
    if (latency) *latency = lat;
    std::vector<float> out(taps);
    for (size_t i = 0; i < taps; ++i) out[i] = static_cast<float>(h[i]);
    return out;
}

/** Sávkorlátozott (Kaiser-ablakos sinc) újramintavételezés impulzusválaszokhoz. */
inline std::vector<float> resampleIr(const std::vector<float>& in, double srIn, double srOut) {
    using namespace fir_detail;
    if (in.empty() || srIn <= 0 || srOut <= 0 || std::abs(srIn - srOut) < 0.5) return in;
    const double ratio = srOut / srIn;
    const size_t outLen = static_cast<size_t>(std::ceil(static_cast<double>(in.size()) * ratio));
    const double cutoff = std::min(1.0, ratio) * 0.95;
    const int half = 32;
    const double beta = 8.0;
    auto bessel0 = [](double x) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 30; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; }
        return sum;
    };
    const double i0b = bessel0(beta);
    constexpr size_t kTable = 4096; // Kaiser ablak táblázat |r| szerint
    std::vector<double> winTab(kTable + 1);
    for (size_t i = 0; i <= kTable; ++i) {
        const double r = static_cast<double>(i) / kTable;
        winTab[i] = bessel0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
    }
    std::vector<float> out(outLen, 0.0f);
    for (size_t n = 0; n < outLen; ++n) {
        const double t = static_cast<double>(n) / ratio;
        const long c = static_cast<long>(std::floor(t));
        const double span = half / cutoff;
        double acc = 0.0;
        for (long i = c - static_cast<long>(span); i <= c + static_cast<long>(span) + 1; ++i) {
            if (i < 0 || i >= static_cast<long>(in.size())) continue;
            const double x = t - static_cast<double>(i);
            const double r = x / span;
            if (std::abs(r) >= 1.0) continue;
            const double sinc = (std::abs(x) < 1e-12) ? 1.0 : std::sin(kPi * cutoff * x) / (kPi * cutoff * x);
            const double win = winTab[static_cast<size_t>(std::abs(r) * kTable)];
            acc += in[static_cast<size_t>(i)] * cutoff * sinc * win;
        }
        out[n] = static_cast<float>(acc);
    }
    return out;
}

/** Impulzusválasz minimálfázisúvá alakítása (az amplitúdómenet megtartásával). */
inline std::vector<float> toMinimumPhase(const std::vector<float>& ir) {
    using namespace fir_detail;
    if (ir.empty()) return ir;
    const size_t P = nextPow2(ir.size()) * 4;
    Fft<double> fft(P);
    std::vector<std::complex<double>> s(P, 0.0);
    for (size_t i = 0; i < ir.size(); ++i) s[i] = ir[i];
    fft.transform(s.data(), false);
    std::vector<double> mag(P);
    for (size_t k = 0; k < P; ++k) mag[k] = std::abs(s[k]);
    std::vector<std::complex<double>> logMin = minimumPhaseSpectrum(mag);
    for (size_t k = 0; k < P; ++k) s[k] = std::exp(logMin[k]);
    fft.transform(s.data(), true);
    std::vector<float> out(ir.size());
    const size_t fade = std::max<size_t>(ir.size() / 16, 1);
    for (size_t i = 0; i < ir.size(); ++i) {
        double v = s[i].real();
        if (i >= ir.size() - fade) v *= 0.5 + 0.5 * std::cos(kPi * static_cast<double>(i - (ir.size() - fade) + 1) / static_cast<double>(fade));
        out[i] = static_cast<float>(v);
    }
    return out;
}

/** A sztereó IR közös skálázása úgy, hogy az amplitúdómenet csúcsa 0 dB legyen (nincs túlvezérlés). */
inline float normalizeIrPeakResponse(std::vector<float>& l, std::vector<float>& r) {
    const size_t len = std::max(l.size(), r.size());
    if (len == 0) return 1.0f;
    const size_t P = nextPow2(len) * 2;
    Fft<double> fft(P);
    double peak = 0.0;
    for (const std::vector<float>* ch : { &l, &r }) {
        if (ch->empty()) continue;
        std::vector<std::complex<double>> s(P, 0.0);
        for (size_t i = 0; i < ch->size(); ++i) s[i] = (*ch)[i];
        fft.transform(s.data(), false);
        for (size_t k = 0; k <= P / 2; ++k) peak = std::max(peak, std::abs(s[k]));
    }
    if (peak < 1e-9) return 1.0f;
    const float g = static_cast<float>(1.0 / peak);
    for (float& v : l) v *= g;
    for (float& v : r) v *= g;
    return g;
}

} // namespace ee_analyzer
