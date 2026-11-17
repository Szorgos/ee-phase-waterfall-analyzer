#pragma once
/**
 * @file room_correction.hpp
 * @brief Saját szobamérő és akusztikai korrekciós mag C++-ban (tiszta szobás implementáció).
 */

#include "sweep.hpp"
#include "deconv.hpp"
#include "smoothing.hpp"
#include "auto_eq.hpp"

namespace aqulify::room {

/**
 * Szintetikus szobaszimulátor teszteléshez és ellenőrzéshez:
 * Ismert paraméterekkel rendelkező virtuális akusztikai teret valósít meg:
 * - Késleltetés (távolság a hangszórótól)
 * - Fő rezonanciák (room modes: pl. 45 Hz-en és 120 Hz-en peaking)
 * - Magasfrekvenciás elnyelés (Highshelf)
 * - Polaritás megfordítása
 * - Háttérzaj
 */
struct SyntheticRoom {
    float delayMs = 15.0f;
    bool invertPolarity = false;
    float noiseLevel = 0.001f; // -60 dBFS
    std::vector<AutoEqBand> acousticFilters = {
        { 1, 45.0f, 6.0f, 5.0f },    // +6 dB módus 45 Hz-en
        { 1, 120.0f, -5.0f, 3.0f },  // -5 dB kioltás 120 Hz-en
        { 3, 8000.0f, -3.0f, 0.71f } // -3 dB magaselnyelés
    };

    /**
     * Szoba átvitele egy adott sweep jelre.
     */
    std::vector<float> simulateResponse(const std::vector<float>& inputSweep, uint32_t sampleRate) const {
        if (inputSweep.empty() || sampleRate == 0) return {};
        const size_t delaySamples = static_cast<size_t>(std::round(delayMs * 0.001 * sampleRate));
        const size_t outLen = inputSweep.size() + delaySamples + sampleRate; // +1 mp lecsengés
        std::vector<float> output(outLen, 0.0f);

        // Késleltetés és polaritás
        const float sign = invertPolarity ? -1.0f : 1.0f;
        for (size_t i = 0; i < inputSweep.size(); ++i) {
            output[i + delaySamples] = sign * inputSweep[i];
        }

        // Akusztikai szűrők alkalmazása másodfokú IIR formájában
        for (const auto& f : acousticFilters) {
            applyBiquadFilter(output, f, sampleRate);
        }

        // Statikus zaj hozzáadása
        if (noiseLevel > 0.0f) {
            uint32_t seed = 12345;
            for (auto& v : output) {
                seed = seed * 1664525u + 1013904223u;
                const float r = (static_cast<float>(seed & 0xFFFF) / 32768.0f - 1.0f) * noiseLevel;
                v += r;
            }
        }

        return output;
    }

private:
    static void applyBiquadFilter(std::vector<float>& data, const AutoEqBand& band, uint32_t sr) {
        if (band.type == 0 || band.gainDb == 0.0f) return;
        const double w0 = 2.0 * M_PI * band.freq / sr;
        const double alpha = std::sin(w0) / (2.0 * std::max(0.1f, band.q));
        const double A = std::pow(10.0, band.gainDb / 40.0);
        const double cosw0 = std::cos(w0);

        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;
        if (band.type == 1) { // Peaking EQ
            b0 = 1.0 + alpha * A;
            b1 = -2.0 * cosw0;
            b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A;
            a1 = -2.0 * cosw0;
            a2 = 1.0 - alpha / A;
        } else if (band.type == 3) { // Highshelf
            const double sqrtA = std::sqrt(A);
            b0 = A * ((A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0);
            b2 = A * ((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha);
            a0 = (A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
            a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosw0);
            a2 = (A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha;
        }

        // Normalizálás a0-val
        b0 /= a0; b1 /= a0; b2 /= a0;
        a1 /= a0; a2 /= a0;

        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
        for (auto& s : data) {
            const double in = s;
            const double out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = in;
            y2 = y1; y1 = out;
            s = static_cast<float>(out);
        }
    }
};

} // namespace aqulify::room
