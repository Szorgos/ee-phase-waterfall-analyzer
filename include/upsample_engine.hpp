#pragma once
/**
 * @file upsample_engine.hpp
 * @brief Felmintavételező motor a beépített zenelejátszóhoz (libsoxr alapú).
 *        A dekódoló szálon fut, a render szálat nem terheli és nem blokkolja.
 */
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <soxr.h>
#include "audio_options.hpp"

namespace ee_analyzer {

class UpsampleEngine {
public:
    UpsampleEngine() = default;
    ~UpsampleEngine() { reset(); stopWorker(); }

    /**
     * Számítási szálak: 1 = egy szál (egy sztereó libsoxr-példány), 2 = bal és jobb csatorna két mono példányban, a
     * jobb egy állandó segédszálon. A két mód kimenete bitre azonos (a csatornák a libsoxr-ben egymástól függetlenül,
     * ugyanazzal a szűrővel számolódnak). A következő init()-től érvényes. A dekóder-szálon fut, nem a hangszálon.
     */
    void setThreads(int n) { threads_ = (n == 2) ? 2 : 1; }
    int threads() const { return threads_; }

    UpsampleEngine(const UpsampleEngine&) = delete;
    UpsampleEngine& operator=(const UpsampleEngine&) = delete;

    /**
     * Motor inicializálása a megadott mintavételi frekvenciákkal és minőségi paraméterekkel.
     */
    /**
     * Célmintavétel kiválasztása a 3.2 szabályok szerint (lefelé soha, csak támogatott rátára).
     */
    static uint32_t selectTargetRate(uint32_t srcRate,
                                     audioopt::UpsampleRule rule,
                                     const std::vector<uint32_t>& supported,
                                     const std::map<uint32_t, uint32_t>& customMap,
                                     bool bypassUpsample,
                                     bool isDsd,
                                     std::string* reasonOut = nullptr,
                                     uint32_t maxRate = 0) {
        auto setReason = [&](const char* r) { if (reasonOut) *reasonOut = r; };
        if (bypassUpsample) { setReason("bypass"); return srcRate; }
        if (isDsd) { setReason("dsd"); return srcRate; }
        if (srcRate > 192000) { setReason("source_rate"); return srcRate; }
        if (rule == audioopt::UpsampleRule::Off) { setReason("off"); return srcRate; }

        // A felső korlát (a terhelés-tanácsadó állítja) a tudott mintavételek közül kizárja a nagyobbakat
        auto isSupported = [&](uint32_t r) {
            return std::find(supported.begin(), supported.end(), r) != supported.end() && (maxRate == 0 || r <= maxRate);
        };

        if (rule == audioopt::UpsampleRule::Double) {
            const uint32_t target = srcRate * 2;
            if (isSupported(target)) { setReason(""); return target; }
            setReason("dac_unsupported");
            return srcRate;
        }
        if (rule == audioopt::UpsampleRule::PowerOfTwo) {
            uint32_t best = srcRate;
            for (uint32_t mult = 2; mult <= 16; mult *= 2) {
                const uint32_t cand = srcRate * mult;
                if (isSupported(cand) && cand > best) best = cand;
            }
            if (best > srcRate) { setReason(""); return best; }
            setReason("dac_unsupported");
            return srcRate;
        }
        if (rule == audioopt::UpsampleRule::MaxOutput) {
            uint32_t best = srcRate;
            for (uint32_t r : supported) {
                if (isSupported(r) && r > best) best = r;
            }
            if (best > srcRate) { setReason(""); return best; }
            setReason("dac_unsupported");
            return srcRate;
        }
        if (rule == audioopt::UpsampleRule::Custom) {
            auto it = customMap.find(srcRate);
            if (it != customMap.end()) {
                const uint32_t target = it->second;
                if (target > srcRate && isSupported(target)) { setReason(""); return target; }
            }
            setReason("dac_unsupported");
            return srcRate;
        }
        setReason("off");
        return srcRate;
    }

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
        split_ = (threads_ == 2 && channels == 2);
        const unsigned perInstance = split_ ? 1u : channels;
        soxr_ = soxr_create(static_cast<double>(srcRate), static_cast<double>(dstRate), perInstance, &err, &io_spec, &q_spec,
                            &rt_spec);
        if (err == nullptr && soxr_ != nullptr && split_) {
            soxrR_ = soxr_create(static_cast<double>(srcRate), static_cast<double>(dstRate), 1, &err, &io_spec, &q_spec, &rt_spec);
        }
        if (err != nullptr || soxr_ == nullptr || (split_ && soxrR_ == nullptr)) {
            reset();
            return false;
        }
        if (split_) startWorker();
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
        if (soxrR_) {
            soxr_delete(soxrR_);
            soxrR_ = nullptr;
        }
        split_ = false;
        active_ = false;
        srcRate_ = 0;
        dstRate_ = 0;
    }

    void clear() {
        if (soxr_) soxr_clear(soxr_);
        if (soxrR_) soxr_clear(soxrR_);
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

        size_t odone = 0;
        soxr_error_t err = split_ ? processSplit(in, inFrames, out, outCapacityFrames, odone)
                                  : runOne(soxr_, in, inFrames, out, outCapacityFrames, odone);
        outFrames = odone;
        processedFrames_ += odone;
        applyHeadroom(out, odone * channels_);
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
        size_t odone = 0;
        soxr_error_t err = split_ ? processSplit(nullptr, 0, out, outCapacityFrames, odone)
                                  : runOne(soxr_, nullptr, 0, out, outCapacityFrames, odone);
        outFrames = odone;
        processedFrames_ += odone;
        applyHeadroom(out, odone * channels_);
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
    uint64_t processedFrames() const { return processedFrames_; }
    void resetClippedSamples() { clippedSamples_ = 0; }

private:
    static soxr_error_t runOne(soxr_t s, const float* in, size_t inFrames, float* out, size_t outCap, size_t& odone) {
        size_t idone = 0;
        odone = 0;
        return soxr_process(s, in, inFrames, &idone, out, outCap, &odone);
    }

    void applyHeadroom(float* out, size_t samples) {
        for (size_t i = 0; i < samples; ++i) {
            const float v = out[i] * headroomGain_;
            if (v > 1.0f || v < -1.0f) ++clippedSamples_;
            out[i] = v;
        }
    }

    /** Két szál: a bal csatorna ezen a szálon, a jobb a segédszálon; a végén visszafésülve. in == nullptr: kiürítés. */
    soxr_error_t processSplit(const float* in, size_t inFrames, float* out, size_t outCap, size_t& odone) {
        if (in) {
            if (inL_.size() < inFrames) { inL_.resize(inFrames); inR_.resize(inFrames); }
            for (size_t i = 0; i < inFrames; ++i) { inL_[i] = in[2 * i]; inR_[i] = in[2 * i + 1]; }
        }
        if (outL_.size() < outCap) { outL_.resize(outCap); outR_.resize(outCap); }
        {
            std::lock_guard<std::mutex> lk(jobMu_);
            jobIn_ = in ? inR_.data() : nullptr;
            jobInFrames_ = in ? inFrames : 0;
            jobOutCap_ = outCap;
            jobDone_ = false;
            jobPending_ = true;
        }
        jobCv_.notify_one();
        size_t doneL = 0;
        const soxr_error_t eL = runOne(soxr_, in ? inL_.data() : nullptr, in ? inFrames : 0, outL_.data(), outCap, doneL);
        {
            std::unique_lock<std::mutex> lk(jobMu_);
            jobCv_.wait(lk, [&] { return jobDone_; });
        }
        const size_t n = std::min(doneL, jobOutFrames_);
        for (size_t i = 0; i < n; ++i) { out[2 * i] = outL_[i]; out[2 * i + 1] = outR_[i]; }
        odone = n;
        return eL != nullptr ? eL : jobErr_;
    }

    void startWorker() {
        if (worker_.joinable()) return;
        quit_ = false;
        worker_ = std::thread([this] {
            std::unique_lock<std::mutex> lk(jobMu_);
            for (;;) {
                jobCv_.wait(lk, [&] { return jobPending_ || quit_; });
                if (quit_) return;
                jobPending_ = false;
                const float* in = jobIn_;
                const size_t inFrames = jobInFrames_, cap = jobOutCap_;
                lk.unlock();
                size_t done = 0;
                const soxr_error_t e = runOne(soxrR_, in, inFrames, outR_.data(), cap, done);
                lk.lock();
                jobOutFrames_ = done;
                jobErr_ = e;
                jobDone_ = true;
                jobCv_.notify_all();
            }
        });
    }

    void stopWorker() {
        if (!worker_.joinable()) return;
        {
            std::lock_guard<std::mutex> lk(jobMu_);
            quit_ = true;
        }
        jobCv_.notify_all();
        worker_.join();
    }

    int threads_ = 1;
    bool split_ = false;
    soxr_t soxrR_ = nullptr;
    std::vector<float> inL_, inR_, outL_, outR_;
    std::thread worker_;
    std::mutex jobMu_;
    std::condition_variable jobCv_;
    bool jobPending_ = false, jobDone_ = false, quit_ = false;
    const float* jobIn_ = nullptr;
    size_t jobInFrames_ = 0, jobOutCap_ = 0, jobOutFrames_ = 0;
    soxr_error_t jobErr_ = nullptr;

    soxr_t soxr_ = nullptr;
    bool active_ = false;
    uint32_t srcRate_ = 0;
    uint32_t dstRate_ = 0;
    uint32_t channels_ = 2;
    float headroomDb_ = -1.0f;
    float headroomGain_ = 1.0f;
    uint64_t clippedSamples_ = 0;
    uint64_t processedFrames_ = 0;
};

} // namespace ee_analyzer
