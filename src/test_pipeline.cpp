#include <iostream>
#include <iomanip>
#include <vector>
#include <cmath>
#include "dsp_pipeline.hpp"

using namespace ee_analyzer;

int main() {
    std::cout << "========================================================\n";
    std::cout << " EasyEffects C++ DSP Pipeline Integrációs Teszt\n";
    std::cout << " (Preamp -> EQ -> Crossfeed -> Convolver -> Limiter)\n";
    std::cout << "========================================================\n\n";

    DspPipeline pipeline(48000.0);
    pipeline.preampGainDb = -3.0; // -3 dB védelmi headroom

    // 1. Parametrikus EQ beállítása
    pipeline.eqChain.addFilter(BiquadFilter(FilterType::LowShelf, 105.0, 0.71, 5.5, 48000.0));
    pipeline.eqChain.addFilter(BiquadFilter(FilterType::PeakingEQ, 3500.0, 2.0, -4.0, 48000.0));
    pipeline.eqChain.addFilter(BiquadFilter(FilterType::HighShelf, 10000.0, 0.71, 2.0, 48000.0));
    pipeline.updateEqCoefficients();

    std::cout << "[1] EQ sávok betöltve (" << pipeline.eqChain.filters.size() << " sáv)\n";

    // 2. Crossfeed beállítása (Bauer bs2b profil)
    pipeline.crossfeed.setPreset(CrossfeedPreset::BauerDefault);
    std::cout << "[2] Crossfeed aktív (Bauer bs2b: 700 Hz, 4.5 dB)\n";

    // 3. Limiter beállítása (-0.2 dB küszöb)
    pipeline.limiter.thresholdDb = -0.2;
    pipeline.limiter.ceilingDb = -0.1;
    pipeline.limiter.updateParameters();
    std::cout << "[3] Studio Limiter aktív (Küszöb: -0.2 dBFS, Plafon: -0.1 dBFS)\n\n";

    // 4. Sztereó tesztjel generálása (1 kHz-es szinusz csak a bal csatornán)
    const size_t blockSize = 256;
    std::vector<float> bufferL(blockSize);
    std::vector<float> bufferR(blockSize, 0.0f); // Jobb oldal csend

    const double pi = 3.14159265358979323846;
    for (size_t i = 0; i < blockSize; ++i) {
        // Erős, +6 dB-es túlvezérelt szinusz (1.414 csúcs)
        bufferL[i] = static_cast<float>(1.414 * std::sin(2.0 * pi * 1000.0 * i / 48000.0));
    }

    std::cout << "Bemeneti bal csúcs: " << 1.414f << " (Digitális túlvezérlés!)\n";
    std::cout << "Bemeneti jobb csúcs: 0.000 (Tiszta csend)\n\n";

    // 5. Feldolgozás a DSP láncon
    pipeline.processBlock(bufferL.data(), bufferR.data(), blockSize);

    // 6. Eredmények ellenőrzése
    float maxOutL = 0.0f;
    float maxOutR = 0.0f;
    for (size_t i = 0; i < blockSize; ++i) {
        maxOutL = std::max(maxOutL, std::abs(bufferL[i]));
        maxOutR = std::max(maxOutR, std::abs(bufferR[i]));
    }

    const float ceilingLinear = static_cast<float>(std::pow(10.0, pipeline.limiter.ceilingDb / 20.0));

    std::cout << "Kimeneti bal csúcs:  " << std::fixed << std::setprecision(4) << maxOutL
              << " (Limitálva a plafon alá: " << ceilingLinear << ")\n";
    std::cout << "Kimeneti jobb csúcs: " << std::fixed << std::setprecision(4) << maxOutR
              << " (Megjelent a természetes Crossfeed áthallás!)\n";
    std::cout << "Limiter csillapítás: " << std::setprecision(2) << pipeline.limiter.getGainReductionDb() << " dB\n\n";

    if (maxOutL <= ceilingLinear + 1e-4f && maxOutR > 0.01f) {
        std::cout << ">> SIKER: A C++ DSP lánc hibátlanul működik és megvédi a kimenetet!\n";
        return 0;
    } else {
        std::cerr << ">> HIBA a határértékek ellenőrzésekor!\n";
        return 1;
    }
}
