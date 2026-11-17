#pragma once
/**
 * @file upsample_engine.hpp
 * @brief Felmintavételező motor a beépített zenelejátszóhoz (libsoxr alapú).
 *        A dekódoló szálon fut, a render szálat nem terheli és nem blokkolja.
 */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <soxr.h>
#include "audio_options.hpp"

namespace ee_analyzer {

class UpsampleEngine {
public:
    UpsampleEngine() = default;
    ~UpsampleEngine() { reset(); }

    UpsampleEngine(const UpsampleEngine&) = delete;
    UpsampleEngine& operator=(const UpsampleEngine&) = delete;

    /**
     * Motor inicializálása a megadott mintavételi frekvenciákkal és minőségi paraméterekkel.
     */
    bool init(uint32_t srcRate, uint32_t dstRate, uint32_t channels = 2,
              int precision = 28, double phase = 25.0,
              double passbandEnd = 0.95, double stopbandBegin = 1.00,
              float headroomDb = -1.0f) {
        reset();
        srcRate_ = srcRate;
        dstRate_ = dstRate;
        channels_ = channels;
        headroomDb_ = headroomDb;
        headroomGain_ = std::pow(10.0f, headroomDb / 20.0f);

        if (srcRate == 0 || dstRate == 0 || srcRate == dstRate) {
            active_ = false;
            return true;
        }

        soxr_quality_spec_t q_spec = soxr_quality_spec(SOXR_VHQ, 0);
        q_spec.precision = static_cast<double>(precision);
        q_spec.phase_response = std::clamp(phase, 0.0, 50.0);
        q_spec.passband_end = passbandEnd;
        q_spec.stopband_begin = stopbandBegin;

        soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
        soxr_runtime_spec_t rt_spec = soxr_runtime_spec(1);

        soxr_error_t err = nullptr;
        soxr_ = soxr_create(static_cast<double>(srcRate),
                            static_cast<double>(dstRate),
                            channels,
                            &err,
                            &io_spec,
                            &q_spec,
                            &rt_spec);
        if (err != nullptr || soxr_ == nullptr) {
            active_ = false;
            return false;
        }
        active_ = true;
        return true;
    }

    bool initWithOptions(uint32_t srcRate, uint32_t dstRate, uint32_t channels,
                         const audioopt::AudioOptions& opts) {
        return init(srcRate, dstRate, channels,
                    opts.upsamplePrecision,
                    opts.upsamplePhase,
                    opts.upsamplePassbandEnd,
                    opts.upsampleStopbandBegin,
                    opts.upsampleHeadroomDb);
    }

    void reset() {
        if (soxr_) {
            soxr_delete(soxr_);
            soxr_ = nullptr;
        }
        active_ = false;
        srcRate_ = 0;
        dstRate_ = 0;
    }

    void clear() {
        if (soxr_) {
            soxr_clear(soxr_);
        }
    }

    /**
     * Hangminták átalakítása (sztereó vagy többcsatornás interleaved float).
     * in: bemeneti minták (inFrames * channels)
     * inFrames: bemeneti keretek száma
     * out: kimeneti puffer
     * outCapacityFrames: kimeneti puffer kapacitása keretben
     * outFrames: ténylegesen előállított kimeneti keretek
     */
    bool process(const float* in, size_t inFrames, float* out, size_t outCapacityFrames, size_t& outFrames) {
        if (!active_ || soxr_ == nullptr) {
            size_t toCopy = std::min(inFrames, outCapacityFrames);
            if (in && out && toCopy > 0) {
                std::memcpy(out, in, toCopy * channels_ * sizeof(float));
            }
            outFrames = toCopy;
            return true;
        }

        size_t idone = 0;
        size_t odone = 0;
        soxr_error_t err = soxr_process(soxr_, in, inFrames, &idone, out, outCapacityFrames, &odone);
        outFrames = odone;

        // Fejtér alkalmazása és túlvezérlés számlálása
        const size_t totalSamples = odone * channels_;
        for (size_t i = 0; i < totalSamples; ++i) {
            float v = out[i] * headroomGain_;
            if (v > 1.0f || v < -1.0f) {
                ++clippedSamples_;
            }
            out[i] = v;
        }

        return (err == nullptr);
    }

    /**
     * Belső szűrő kiürítése (a forrásfolyam végén).
     */
    bool flush(float* out, size_t outCapacityFrames, size_t& outFrames) {
        if (!active_ || soxr_ == nullptr) {
            outFrames = 0;
            return true;
        }
        size_t idone = 0, odone = 0;
        soxr_error_t err = soxr_process(soxr_, nullptr, 0, &idone, out, outCapacityFrames, &odone);
        outFrames = odone;

        const size_t totalSamples = odone * channels_;
        for (size_t i = 0; i < totalSamples; ++i) {
            float v = out[i] * headroomGain_;
            if (v > 1.0f || v < -1.0f) {
                ++clippedSamples_;
            }
            out[i] = v;
        }

        return (err == nullptr);
    }

    /**
     * Késleltetés kimeneti keretekben (a libsoxr belső szűrőjének állapota szerint).
     */
    double delayFrames() const {
        if (active_ && soxr_) {
            return soxr_delay(soxr_);
        }
        return 0.0;
    }

    bool isActive() const { return active_; }
    uint32_t srcRate() const { return srcRate_; }
    uint32_t dstRate() const { return dstRate_; }
    uint32_t channels() const { return channels_; }
    float headroomDb() const { return headroomDb_; }
    float headroomGain() const { return headroomGain_; }
    uint64_t clippedSamples() const { return clippedSamples_; }
    void resetClippedSamples() { clippedSamples_ = 0; }

private:
    soxr_t soxr_ = nullptr;
    bool active_ = false;
    uint32_t srcRate_ = 0;
    uint32_t dstRate_ = 0;
    uint32_t channels_ = 2;
    float headroomDb_ = -1.0f;
    float headroomGain_ = 1.0f;
    uint64_t clippedSamples_ = 0;
};

} // namespace ee_analyzer
