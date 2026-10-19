#pragma once
#include <cmath>
#include <algorithm>
#include <vector>

namespace ee_analyzer {

/**
 * @brief Stúdió minőségű Limiter & Soft-Clipper modul
 * Megvédi a DAC-ot a digitális túlvezérléstől (0 dBFS inter-sample peak klippelés ellen),
 * és selymes analóg-jellegű telítést nyújt magas kivezérlés esetén.
 */
class StudioLimiter {
public:
    bool enabled = true;
    double sampleRate = 48000.0;
    double thresholdDb = -0.2;  // dBFS (Küszöbérték)
    double ceilingDb = -0.1;    // dBFS (Abszolút csúcsérték korlát)
    double releaseMs = 50.0;    // ms (Felengedési idő)

    // Dinamika burkológörbe követő állapotok
    double envelope = 0.0;
    double releaseCoeff = 0.0;
    double currentGainReductionDb = 0.0;

    StudioLimiter(double sr = 48000.0) : sampleRate(sr) {
        updateParameters();
    }

    /**
     * @brief Paraméterek és burkológörbe-együtthatók újraszámítása
     */
    void updateParameters() {
        if (sampleRate <= 0.0 || releaseMs <= 0.0) return;
        // Exponenciális felengedési együttható
        releaseCoeff = std::exp(-1000.0 / (releaseMs * sampleRate));
    }

    /**
     * @brief Sima szaturáció (Soft-Clipper analóg karakterisztika)
     * tanh-alapú kerekítés az éles digitális szögletesség megszüntetésére
     */
    inline float softClip(float sample) const {
        if (std::abs(sample) < 0.8f) {
            return sample;
        }
        // Lágy átmenet 0.8 és 1.0 között tanh függvénnyel
        return std::tanh(sample);
    }

    /**
     * @brief Egy sztereó minta limiteres feldolgozása
     */
    void process(float& inOutL, float& inOutR) {
        if (!enabled) return;

        // Csúcsérték detektálás a két csatorna maximumából
        const double peakL = std::abs(static_cast<double>(inOutL));
        const double peakR = std::abs(static_cast<double>(inOutR));
        const double maxPeak = std::max(peakL, peakR);

        // Küszöbérték lineáris skálán
        const double thresholdLinear = std::pow(10.0, thresholdDb / 20.0);
        const double ceilingLinear = std::pow(10.0, ceilingDb / 20.0);

        // Burkológörbe frissítése (azonnali felfutás / attack = 0, exponenciális felengedés)
        if (maxPeak > envelope) {
            envelope = maxPeak;
        } else {
            envelope = maxPeak + releaseCoeff * (envelope - maxPeak);
        }

        // Erősítéscsökkentés (Gain Reduction) számítása
        double gain = 1.0;
        if (envelope > thresholdLinear && envelope > 1e-9) {
            gain = thresholdLinear / envelope;
            currentGainReductionDb = 20.0 * std::log10(gain);
        } else {
            currentGainReductionDb = 0.0;
        }

        // Skálázott és védett kimeneti minta
        float outL = static_cast<float>(inOutL * gain);
        float outR = static_cast<float>(inOutR * gain);

        // Végső védelmi vonal: Soft-Clipper a mennyezet (Ceiling) magasságában
        const float maxCeil = static_cast<float>(ceilingLinear);
        outL = std::clamp(softClip(outL), -maxCeil, maxCeil);
        outR = std::clamp(softClip(outR), -maxCeil, maxCeil);

        inOutL = outL;
        inOutR = outR;
    }

    /**
     * @brief Puffer feldolgozása
     */
    void processBlock(float* leftChannel, float* rightChannel, size_t numSamples) {
        if (!enabled) return;
        for (size_t i = 0; i < numSamples; ++i) {
            process(leftChannel[i], rightChannel[i]);
        }
    }

    /**
     * @brief Pillanatnyi erősítéscsökkentés lekérdezése dB-ben (UI kijelzőkhöz)
     */
    double getGainReductionDb() const {
        return currentGainReductionDb;
    }

    void reset() {
        envelope = 0.0;
        currentGainReductionDb = 0.0;
    }
};

} // namespace ee_analyzer
