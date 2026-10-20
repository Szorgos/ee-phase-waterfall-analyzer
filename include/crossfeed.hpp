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

    // Kiszámított szűrőegyütthatók (Bauer bs2b: aluláteresztő áthallás + felül-polcos direkt ág)
    double a0Lo = 0.0, b1Lo = 0.0;            // áthalló ág: 1-pólusú aluláteresztő
    double a0Hi = 1.0, a1Hi = 0.0, b1Hi = 0.0; // direkt ág: 1. rendű felül-polc
    double gain = 1.0;                        // szintkiegyenlítés: középre kevert (mono) jelnél ~0 dB
    // régi mezőnevek (tesztek, felület): a mélyfrekvenciás áthallás és a direkt szint
    double directGain = 1.0;
    double crossGain = 0.0;

    // Belső állapot
    double hiL = 0.0, hiR = 0.0, inL1 = 0.0, inR1 = 0.0;

    CrossfeedProcessor(double sr = 48000.0) : sampleRate(sr) {
        setPreset(CrossfeedPreset::BauerDefault);
    }

    void setPreset(CrossfeedPreset preset) {
        switch (preset) {
            case CrossfeedPreset::BauerDefault:    cutoffFreq = 700.0; feedLevelDb = 4.5; break;
            case CrossfeedPreset::MeierCorda:      cutoffFreq = 650.0; feedLevelDb = 6.0; break;
            case CrossfeedPreset::StudioNearfield: cutoffFreq = 800.0; feedLevelDb = 3.5; break;
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
     * Együtthatók a libbs2b képletei szerint. A feedLevelDb a mély- és magasfrekvenciás áthallás
     * különbsége; a teljes erősítés úgy van skálázva, hogy a középre kevert (mono) jel szintje
     * ne nőjön (korábban a mély hangok +3 dB-t kaptak, ami túlvezérléshez/limiter-munkához vezetett).
     */
    void updateCoefficients() {
        if (sampleRate <= 0.0) return;
        const double pi = 3.14159265358979323846;
        const double feed = std::clamp(feedLevelDb, 1.0, 15.0);
        const double fcLo = std::clamp(cutoffFreq, 200.0, 2000.0);
        const double gbLo = feed * -5.0 / 6.0 - 3.0;
        const double gbHi = feed / 6.0 - 3.0;
        const double gLo = std::pow(10.0, gbLo / 20.0);
        const double gHi = 1.0 - std::pow(10.0, gbHi / 20.0);
        const double fcHi = fcLo * std::pow(2.0, (gbLo - 20.0 * std::log10(gHi)) / 12.0);

        double x = std::exp(-2.0 * pi * fcLo / sampleRate);
        b1Lo = x;
        a0Lo = gLo * (1.0 - x);
        x = std::exp(-2.0 * pi * fcHi / sampleRate);
        b1Hi = x;
        a0Hi = 1.0 - gHi * (1.0 - x);
        a1Hi = -x;
        gain = 1.0 / (1.0 - gHi + gLo);
        directGain = gain * (1.0 - gHi);   // tájékoztató: mély hangok direkt szintje
        crossGain = gain * gLo;            // tájékoztató: mély hangok áthallási szintje

        // ITD: az aluláteresztő szűrő saját futásideje (~1/(2·pi·fc)) már késleltet; csak a különbséget adjuk hozzá
        if (!delayL.empty()) {
            const double lpDelayMs = 1000.0 / (2.0 * pi * fcLo);
            const double extra = std::max(0.0, delayMs - lpDelayMs) * 1e-3 * sampleRate;
            const double maxD = static_cast<double>(delayL.size() - 3);
            delaySamples = static_cast<float>(std::clamp(extra, 0.0, maxD));
        } else {
            delaySamples = 0.0f;
        }
    }

    void process(float& inOutL, float& inOutR) {
        if (!enabled) return;
        const double l = inOutL, r = inOutR;
        // áthalló ág (aluláteresztett)
        filterStateL = a0Lo * l + b1Lo * filterStateL;
        filterStateR = a0Lo * r + b1Lo * filterStateR;
        // direkt ág (felül-polc)
        hiL = a0Hi * l + a1Hi * inL1 + b1Hi * hiL;
        hiR = a0Hi * r + a1Hi * inR1 + b1Hi * hiR;
        inL1 = l; inR1 = r;

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
        inOutL = static_cast<float>((hiL + crossFromR) * gain);
        inOutR = static_cast<float>((hiR + crossFromL) * gain);
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
        hiL = hiR = inL1 = inR1 = 0.0;
        std::fill(delayL.begin(), delayL.end(), 0.0f);
        std::fill(delayR.begin(), delayR.end(), 0.0f);
    }
};

} // namespace ee_analyzer
