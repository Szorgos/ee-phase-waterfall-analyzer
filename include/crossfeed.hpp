#pragma once
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

    // Belső szűrő állapotok (egy-pólusú IIR állapotváltozók a bal és jobb csatornához)
    double filterStateL = 0.0;
    double filterStateR = 0.0;

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

    /**
     * @brief Szűrőegyütthatók frissítése a mintavételi frekvencia és paraméterek alapján
     */
    void updateCoefficients() {
        if (sampleRate <= 0.0) return;

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

        // Keresztcsatornás keverés:
        // Új bal = direkt bal + szűrt jobb áthallás
        // Új jobb = direkt jobb + szűrt bal áthallás
        const float newL = static_cast<float>(directGain * inOutL + crossGain * filterStateR);
        const float newR = static_cast<float>(directGain * inOutR + crossGain * filterStateL);

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
    }
};

} // namespace ee_analyzer
