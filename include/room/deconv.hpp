#pragma once
/**
 * @file deconv.hpp
 * @brief Dekonvolúció, impulzusválasz (IR) kinyerése, polaritás, késleltetés és ablakozás.
 */

#include <algorithm>
#include <cmath>
#ifndef M_PI   // szigorú (-std=c++17) módban a Windows / macOS fordító nem adja
#define M_PI 3.14159265358979323846
#endif
#include <complex>
#include <cstdint>
#include <vector>

namespace aqulify::room {

/** Helyben számolt radix-2 FFT (n kettő hatványa); inverse: inverz transzformáció 1/n skálázással. */
inline void fftRadix2(std::vector<std::complex<double>>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * M_PI / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse) for (auto& x : a) x /= static_cast<double>(n);
}

/**
 * Farina-féle dekonvolúció: a felvétel és az inverz szűrő (generateInverseFilter) lineáris konvolúciója FFT-vel.
 * Az eredmény fő csúcsa a (késleltetés + inverz hossza − 1) helyen van; előtte (negatív időben) a torzítási
 * termékek, amelyeket az ablakozás kizár. A kimenet: az impulzusválasz ablakozott szelete (a csúcs előtt preMs,
 * utána postMs), a csúcs helye a szeletben, és a felvételhez mért érkezési idő (mintában).
 */
struct FarinaResult {
    std::vector<float> ir;        // ablakozott szelet
    size_t peakInIr = 0;          // a fő csúcs helye a szeletben
    double arrivalSamples = 0.0;  // a hang érkezése a felvétel elejéhez képest (mintában)
    float peakValue = 0.0f;       // a csúcs előjeles értéke
    float snrDb = 0.0f;           // a csúcs és a csúcs előtti zajszakasz aránya
    bool ok = false;
};

inline FarinaResult farinaDeconvolve(const std::vector<float>& recorded, const std::vector<float>& inverse,
                                     uint32_t sampleRate, double preMs = 5.0, double postMs = 300.0) {
    FarinaResult r;
    if (recorded.empty() || inverse.empty() || sampleRate == 0) return r;
    const size_t total = recorded.size() + inverse.size() - 1;
    size_t n = 1;
    while (n < total) n <<= 1;
    std::vector<std::complex<double>> A(n), B(n);
    for (size_t i = 0; i < recorded.size(); ++i) A[i] = recorded[i];
    for (size_t i = 0; i < inverse.size(); ++i) B[i] = inverse[i];
    fftRadix2(A, false);
    fftRadix2(B, false);
    for (size_t i = 0; i < n; ++i) A[i] *= B[i];
    fftRadix2(A, true);

    // A fő csúcs a lineáris részben (a torzítási termékek kisebbek és korábbiak)
    const size_t lin0 = inverse.size() - 1;
    size_t peak = lin0;
    double maxAbs = 0.0;
    for (size_t i = lin0; i < total; ++i) {
        const double a = std::abs(A[i].real());
        if (a > maxAbs) { maxAbs = a; peak = i; }
    }
    if (maxAbs <= 0.0) return r;

    const size_t pre = static_cast<size_t>(std::round(preMs * 0.001 * sampleRate));
    const size_t post = static_cast<size_t>(std::round(postMs * 0.001 * sampleRate));
    const size_t begin = peak > pre ? peak - pre : 0;
    const size_t end = std::min(total, peak + post);
    r.ir.resize(end - begin);
    for (size_t i = begin; i < end; ++i) r.ir[i - begin] = static_cast<float>(A[i].real());
    r.peakInIr = peak - begin;
    r.peakValue = static_cast<float>(A[peak].real());
    // Tört minta: parabolaillesztés a csúcs körül (a csatornák közti különbséghez)
    double frac = 0.0;
    if (peak > 0 && peak + 1 < total) {
        const double y0 = std::abs(A[peak - 1].real()), y1 = maxAbs, y2 = std::abs(A[peak + 1].real());
        const double den = y0 - 2.0 * y1 + y2;
        if (std::abs(den) > 1e-30) frac = std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5);
    }
    r.arrivalSamples = static_cast<double>(peak) - static_cast<double>(lin0) + frac;

    // Zaj: a csúcs előtt 10 ms-tól visszafelé 100 ms (a torzítási termékek a sweep végén, ennél jóval korábban vannak)
    const size_t g = static_cast<size_t>(0.010 * sampleRate), w = static_cast<size_t>(0.100 * sampleRate);
    double np = 0.0;
    size_t nc = 0;
    for (size_t i = (peak > g + w ? peak - g - w : lin0); i + g < peak && i < total; ++i) {
        np += A[i].real() * A[i].real();
        ++nc;
    }
    r.snrDb = nc > 0 && np > 0.0 ? static_cast<float>(10.0 * std::log10(maxAbs * maxAbs / (np / nc))) : 90.0f;

    // Ablak: rövid Hann-felfutás a csúcs előtt, Hann-lecsengés a végén (a késői zengés és a zaj kiesik)
    for (size_t i = 0; i < r.peakInIr; ++i) {
        const double t = static_cast<double>(i) / std::max<size_t>(1, r.peakInIr);
        r.ir[i] *= static_cast<float>(0.5 * (1.0 - std::cos(M_PI * t)));
    }
    const size_t tailLen = (r.ir.size() - r.peakInIr) / 3;   // az utolsó harmad lecseng
    for (size_t i = 0; i < tailLen; ++i) {
        const double t = static_cast<double>(i) / std::max<size_t>(1, tailLen);
        r.ir[r.ir.size() - tailLen + i] *= static_cast<float>(0.5 * (1.0 + std::cos(M_PI * t)));
    }
    r.ok = true;
    return r;
}

struct DeconvResult {
    std::vector<float> ir;          // Kinyert impulzusválasz
    size_t peakIndex = 0;           // A fő impulzus mintapozíciója
    float peakValue = 0.0f;         // A csúcs értéke (előjellel)
    float delayMs = 0.0f;           // Mért késleltetés milliszekundumban
    bool invertedPolarity = false;  // Fordított polaritás észlelve
    float snrDb = 0.0f;             // Becsült jel-zaj viszony dB-ben
};

/**
 * Szoba felvételének dekonvolválása a referencia sweep jellel illesztett szűrővel (Matched filter).
 * Kiszámítja az impulzusválaszt, a mintaszintű késleltetést, a polaritást és az SNR-t.
 */
inline DeconvResult deconvolve(const std::vector<float>& recorded,
                               const std::vector<float>& referenceSweep,
                               uint32_t sampleRate) {
    DeconvResult res;
    if (recorded.empty() || referenceSweep.empty() || sampleRate == 0) return res;

    const size_t recLen = recorded.size();
    const size_t refLen = referenceSweep.size();
    if (recLen < refLen) return res;
    const size_t irLen = recLen - refLen + 1;
    res.ir.assign(irLen, 0.0f);

    double refEnergy = 0.0;
    for (float v : referenceSweep) refEnergy += static_cast<double>(v) * v;
    if (refEnergy < 1e-12) return res;
    const double invEnergy = 1.0 / refEnergy;

    // Keresztkorreláció / illesztett szűrő (Matched filter)
    for (size_t i = 0; i < irLen; ++i) {
        double sum = 0.0;
        for (size_t k = 0; k < refLen; ++k) {
            sum += static_cast<double>(recorded[i + k]) * static_cast<double>(referenceSweep[k]);
        }
        res.ir[i] = static_cast<float>(sum * invEnergy);
    }

    // Csúcskeresés
    float maxAbs = 0.0f;
    for (size_t i = 0; i < irLen; ++i) {
        const float a = std::abs(res.ir[i]);
        if (a > maxAbs) {
            maxAbs = a;
            res.peakIndex = i;
            res.peakValue = res.ir[i];
        }
    }

    res.delayMs = static_cast<float>(res.peakIndex) / sampleRate * 1000.0f;
    res.invertedPolarity = (res.peakValue < 0.0f);

    // Zajszint becslése a csúcs előtti és utáni szakaszból
    double noisePower = 0.0;
    size_t noiseSamples = 0;
    const size_t guard = static_cast<size_t>(sampleRate * 0.005); // 5 ms
    for (size_t i = 0; i < irLen; ++i) {
        if (i + guard < res.peakIndex || i > res.peakIndex + guard * 10) {
            noisePower += static_cast<double>(res.ir[i]) * res.ir[i];
            noiseSamples++;
        }
    }
    if (noiseSamples > 0 && noisePower > 1e-12) {
        const double avgNoise = noisePower / noiseSamples;
        res.snrDb = static_cast<float>(10.0 * std::log10(std::max(1e-12, (maxAbs * maxAbs) / avgNoise)));
    } else {
        res.snrDb = 90.0f;
    }

    return res;
}

/**
 * Időablakozás alkalmazása az impulzusválaszra:
 * A közvetlen hangot és a korai visszaverődéseket izolálja, csökkenti a szoba távoli zengését.
 */
inline std::vector<float> applyWindow(const std::vector<float>& ir, size_t peakIdx,
                                      uint32_t sampleRate, double leftMs = 3.0,
                                      double rightMs = 150.0) {
    if (ir.empty() || sampleRate == 0) return {};
    std::vector<float> windowed = ir;
    const size_t n = ir.size();

    const size_t leftSamples = static_cast<size_t>(std::round(leftMs * 0.001 * sampleRate));
    const size_t rightSamples = static_cast<size_t>(std::round(rightMs * 0.001 * sampleRate));

    // Bal oldal nullázása és Hann fade-in
    const size_t startFade = (peakIdx > leftSamples) ? (peakIdx - leftSamples) : 0;
    for (size_t i = 0; i < startFade; ++i) windowed[i] = 0.0f;
    for (size_t i = startFade; i < peakIdx; ++i) {
        const double t = static_cast<double>(i - startFade) / (peakIdx - startFade);
        const double w = 0.5 * (1.0 - std::cos(M_PI * t));
        windowed[i] *= static_cast<float>(w);
    }

    // Jobb oldal Hann fade-out és utána nullázás
    const size_t endFade = std::min(n, peakIdx + rightSamples);
    for (size_t i = peakIdx; i < endFade; ++i) {
        const double t = static_cast<double>(i - peakIdx) / (endFade - peakIdx);
        const double w = 0.5 * (1.0 + std::cos(M_PI * t));
        windowed[i] *= static_cast<float>(w);
    }
    for (size_t i = endFade; i < n; ++i) windowed[i] = 0.0f;

    return windowed;
}

} // namespace aqulify::room
