#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include <string>

namespace ee_analyzer {

/**
 * @brief Fejhallgatós Crossfeed preset típusok
 * (Természetes sztereó hangszóró-hatás szimulálása fejhallgatón)
 */
enum class CrossfeedPreset {
    BauerDefault,   // Bauer bs2b iparági standard (700 Hz, 4.5 dB áthallás)
    MeierCorda,     // Jan Meier Corda audiofil profil (650 Hz, 6.0 dB)
    StudioNearfield // Közeltéri stúdiómonitor profil (800 Hz, 3.5 dB)
};

/**
 * @brief Crossfeed DSP processzor
 * Kétcsatornás sztereó áthallás és frekvenciafüggő késleltetés modellezése.
 */
class CrossfeedProcessor {
public:
    bool enabled = true;
    double sampleRate = 48000.0;
    double cutoffFreq = 700.0;  // Hz (aluláteresztő vágási frekvencia az áthallásra)
    double feedLevelDb = 4.5;   // dB (áthallás mértéke)
    double delayMs = 0.0;       // ms (ITD: az áthalló jel fej körüli futásideje, pl. 0.29 ms)

    // Belső szűrő állapotok (egy-pólusú IIR állapotváltozók a bal és jobb csatornához)
    double filterStateL = 0.0;
    double filterStateR = 0.0;

    // ITD késleltetővonal (prepareDelay() foglalja, a feldolgozás nem foglal)
    std::vector<float> delayL, delayR;
    size_t delayPos = 0;
    float delaySamples = 0.0f;

    // Kiszámított szűrőegyütthatók
    double b0 = 1.0, a1 = 0.0;
    double directGain = 1.0;
    double crossGain = 0.0;

    CrossfeedProcessor(double sr = 48000.0) : sampleRate(sr) {
        setPreset(CrossfeedPreset::BauerDefault);
    }

    /**
     * @brief Előre definiált profil beállítása
     */
    void setPreset(CrossfeedPreset preset) {
        switch (preset) {
            case CrossfeedPreset::BauerDefault:
                cutoffFreq = 700.0;
                feedLevelDb = 4.5;
                break;
            case CrossfeedPreset::MeierCorda:
                cutoffFreq = 650.0;
                feedLevelDb = 6.0;
                break;
            case CrossfeedPreset::StudioNearfield:
                cutoffFreq = 800.0;
                feedLevelDb = 3.5;
                break;
        }
        updateCoefficients();
    }

    /** Nem valós idejű: késleltetővonal lefoglalása (max. 2 ms). */
    void prepareDelay() {
        const size_t n = static_cast<size_t>(std::ceil(0.002 * sampleRate)) + 4;
        delayL.assign(n, 0.0f);
        delayR.assign(n, 0.0f);
        delayPos = 0;
        updateCoefficients();
    }

    /**
     * @brief Szűrőegyütthatók frissítése a mintavételi frekvencia és paraméterek alapján
     */
    void updateCoefficients() {
        if (sampleRate <= 0.0) return;
        if (!delayL.empty()) {
            const double maxD = static_cast<double>(delayL.size() - 3);
            delaySamples = static_cast<float>(std::clamp(delayMs * 1e-3 * sampleRate, 0.0, maxD));
        } else {
            delaySamples = 0.0f;
        }

        // Normált vágási körfrekvencia
        const double pi = 3.14159265358979323846;
        const double omega = 2.0 * pi * cutoffFreq / sampleRate;

        // Egyszerű 1-pólusú aluláteresztő tag az áthalló komponenshez: y[n] = (1 - alpha)*x[n] + alpha*y[n-1]
        const double alpha = std::exp(-omega);
        b0 = 1.0 - alpha;
        a1 = alpha;

        // Lineáris áthallási szint és kompenzáló direkt szint
        crossGain = std::pow(10.0, -feedLevelDb / 20.0);
        directGain = 1.0 - (crossGain * 0.3); // Kismértékű szintkiegyenlítés a monó felhalmozódás ellen
    }

    /**
     * @brief Egy sztereó minta valós idejű feldolgozása
     * @param inOutL Bal csatorna minta (-1.0 ... +1.0)
     * @param inOutR Jobb csatorna minta (-1.0 ... +1.0)
     */
    void process(float& inOutL, float& inOutR) {
        if (!enabled) return;

        // Aluláteresztett áthallási komponensek
        filterStateL = b0 * inOutL + a1 * filterStateL;
        filterStateR = b0 * inOutR + a1 * filterStateR;

        // ITD: az áthalló jel tört késleltetése (lineáris interpoláció; a jel már aluláteresztett)
        double crossFromR = filterStateR, crossFromL = filterStateL;
        if (!delayL.empty() && delaySamples > 0.0f) {
            const size_t n = delayL.size();
            delayL[delayPos] = static_cast<float>(filterStateL);
            delayR[delayPos] = static_cast<float>(filterStateR);
            const size_t di = static_cast<size_t>(delaySamples);
            const float frac = delaySamples - static_cast<float>(di);
            const size_t i0 = (delayPos + n - di) % n, i1 = (delayPos + n - di - 1) % n;
            crossFromL = delayL[i0] + frac * (delayL[i1] - delayL[i0]);
            crossFromR = delayR[i0] + frac * (delayR[i1] - delayR[i0]);
            delayPos = (delayPos + 1) % n;
        }

        // Keresztcsatornás keverés:
        // Új bal = direkt bal + szűrt (késleltetett) jobb áthallás
        // Új jobb = direkt jobb + szűrt (késleltetett) bal áthallás
        const float newL = static_cast<float>(directGain * inOutL + crossGain * crossFromR);
        const float newR = static_cast<float>(directGain * inOutR + crossGain * crossFromL);

        inOutL = newL;
        inOutR = newR;
    }

    /**
     * @brief Puffer feldolgozása (sztereó blokk)
     */
    void processBlock(float* leftChannel, float* rightChannel, size_t numSamples) {
        if (!enabled) return;
        for (size_t i = 0; i < numSamples; ++i) {
            process(leftChannel[i], rightChannel[i]);
        }
    }

    /**
     * @brief Állapotváltozók törlése (pl. hangmegállítás vagy számváltás esetén)
     */
    void reset() {
        filterStateL = 0.0;
        filterStateR = 0.0;
        std::fill(delayL.begin(), delayL.end(), 0.0f);
        std::fill(delayR.begin(), delayR.end(), 0.0f);
    }
};

} // namespace ee_analyzer
