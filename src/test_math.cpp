#include <iostream>
#include <iomanip>
#include <vector>
#include "biquad_analyzer.hpp"

using namespace ee_analyzer;

int main() {
    std::cout << "====================================================\n";
    std::cout << " EasyEffects Analitikus Biquad & Fáziskalkulátor Teszt\n";
    std::cout << "====================================================\n\n";

    FilterChain chain;
    chain.sampleRate = 48000.0;

    // Szimulálunk egy tipikus EasyEffects EQ beállítást:
    // 1. Basszus emelés (Sub-bass peak): 60 Hz, Q = 1.414, +6 dB
    chain.addFilter(BiquadFilter(FilterType::PeakingEQ, 60.0, 1.414, 6.0, 48000.0));

    // 2. Szobamódus kivágás (Room mode notch): 120 Hz, Q = 5.0, -9 dB
    chain.addFilter(BiquadFilter(FilterType::PeakingEQ, 120.0, 5.0, -9.0, 48000.0));

    // 3. Magas kiemelés (Air shelf): 10 000 Hz, Q = 0.707, +3 dB
    chain.addFilter(BiquadFilter(FilterType::HighShelf, 10000.0, 0.7071, 3.0, 48000.0));

    std::cout << "Aktív szűrők a láncban:\n";
    std::cout << " 1. Peaking EQ:    60 Hz,  Q=1.41, +6.0 dB\n";
    std::cout << " 2. Peaking EQ:   120 Hz,  Q=5.00, -9.0 dB (éles szobavágás)\n";
    std::cout << " 3. High Shelf: 10000 Hz,  Q=0.71, +3.0 dB\n\n";

    std::cout << std::setw(10) << "Freq (Hz)"
              << std::setw(14) << "Gain (dB)"
              << std::setw(14) << "Fázis (°)"
              << std::setw(18) << "Késleltetés (ms)" << "\n";
    std::cout << "--------------------------------------------------------\n";

    const std::vector<double> testFreqs = {
        20.0, 40.0, 60.0, 80.0, 110.0, 120.0, 130.0, 200.0, 500.0, 1000.0, 5000.0, 10000.0, 20000.0
    };

    for (double f : testFreqs) {
        double mag = chain.magnitudeDbAt(f);
        double phase = chain.phaseDegreesAt(f);
        double delay = chain.groupDelayMsAt(f);

        std::cout << std::fixed << std::setprecision(1)
                  << std::setw(10) << f
                  << std::setprecision(2)
                  << std::setw(14) << mag
                  << std::setw(14) << phase
                  << std::setprecision(3)
                  << std::setw(18) << delay << "\n";
    }

    std::cout << "\n[OK] Zárt képletű csoportkésleltetés-számítás sikeresen lefutott!\n";
    return 0;
}
