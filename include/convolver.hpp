#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

namespace ee_analyzer {

/**
 * @brief FIR Convolver modul akusztikai és fejhallgató-impulzusválaszokhoz (IR)
 * Támogatja a közvetlen időtartománybeli konvolúciót és késleltetésmentes szűrést.
 */
class FirConvolver {
public:
    bool enabled = true;
    double sampleRate = 48000.0;
    float dryWet = 1.0f; // 1.0 = 100% szűrt (Wet), 0.0 = tiszta (Dry)

    // Impulzusválasz mintaegyütthatók
    std::vector<float> impulseResponse;

    // Késleltető vonal (gyűrűpuffer) a bal és jobb csatornához
    std::vector<float> ringBufferL;
    std::vector<float> ringBufferR;
    size_t writeIndex = 0;

    FirConvolver(double sr = 48000.0) : sampleRate(sr) {}

    /**
     * @brief Impulzusválasz betöltése és normalizálása
     */
    void loadImpulseResponse(const std::vector<float>& ir) {
        impulseResponse = ir;
        if (impulseResponse.empty()) return;

        // Csúcsérték normalizálás
        float maxVal = 0.0f;
        for (float v : impulseResponse) {
            maxVal = std::max(maxVal, std::abs(v));
        }
        if (maxVal > 1e-6f) {
            for (float& v : impulseResponse) {
                v /= maxVal;
            }
        }

        // Puffer méretének igazítása az IR hosszához
        const size_t irLen = impulseResponse.size();
        ringBufferL.assign(irLen, 0.0f);
        ringBufferR.assign(irLen, 0.0f);
        writeIndex = 0;
    }

    /**
     * @brief Egy minta konvolúciós feldolgozása
     */
    void process(float& inOutL, float& inOutR) {
        if (!enabled || impulseResponse.empty()) return;

        const size_t irLen = impulseResponse.size();

        // Minták beírása a gyűrűpufferbe
        ringBufferL[writeIndex] = inOutL;
        ringBufferR[writeIndex] = inOutR;

        // Közvetlen FIR konvolúció összege
        float sumL = 0.0f;
        float sumR = 0.0f;
        size_t readIndex = writeIndex;

        for (size_t i = 0; i < irLen; ++i) {
            const float coeff = impulseResponse[i];
            sumL += ringBufferL[readIndex] * coeff;
            sumR += ringBufferR[readIndex] * coeff;

            if (readIndex == 0) {
                readIndex = irLen - 1;
            } else {
                --readIndex;
            }
        }

        // Gyűrűpuffer mutató léptetése
        writeIndex = (writeIndex + 1) % irLen;

        // Wet / Dry keverés
        inOutL = (1.0f - dryWet) * inOutL + dryWet * sumL;
        inOutR = (1.0f - dryWet) * inOutR + dryWet * sumR;
    }

    /**
     * @brief Puffer feldolgozása
     */
    void processBlock(float* leftChannel, float* rightChannel, size_t numSamples) {
        if (!enabled || impulseResponse.empty()) return;
        for (size_t i = 0; i < numSamples; ++i) {
            process(leftChannel[i], rightChannel[i]);
        }
    }

    void reset() {
        std::fill(ringBufferL.begin(), ringBufferL.end(), 0.0f);
        std::fill(ringBufferR.begin(), ringBufferR.end(), 0.0f);
        writeIndex = 0;
    }
};

} // namespace ee_analyzer
