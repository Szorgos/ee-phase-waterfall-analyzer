#pragma once
/**
 * @file live_upsample_bridge.hpp
 * @brief Platformfüggetlen, valós idejű rendszerhang-felmintavételező híd (Linux, Windows, macOS).
 *
 * Felépítés:
 *   - Bemeneti SPSC lock-free gyűrűpuffer (forrás mintavétel, pl. 44,1/48 kHz) — 0 allokáció, 0 lock a hangszálon.
 *   - Háttér munkaszál: UpsampleEngine (libsoxr vagy Aqulify FFT64, választható fázissal) +
 *     finom PI órajel-drift kompenzáció (±500 ppm, szinkron órajeleknél bitpontos 1:1 bypass) +
 *     opcionális DSP-feldolgozó visszahívás a célmintavételen.
 *   - Kimeneti SPSC lock-free gyűrűpuffer (célmintavétel, pl. 88,2/96/192/384 kHz) — 0 allokáció, 0 lock a DAC szálon.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_options.hpp"
#include "upsample_engine.hpp"

namespace ee_analyzer::livebridge {

/** Kisbetűs ASCII-keresés: virtuális hangkábel / loopback eszköz felismerése (Windows / macOS). */
inline bool isVirtualCableName(const std::string& name) {
    std::string low;
    low.reserve(name.size());
    for (unsigned char c : name) low.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? (c - 'A' + 'a') : c));
    return low.find("vb-audio") != std::string::npos ||
           low.find("cable input") != std::string::npos ||
           low.find("cable output") != std::string::npos ||
           low.find("virtual audio cable") != std::string::npos ||
           low.find("voicemeeter") != std::string::npos ||
           low.find("hi-fi cable") != std::string::npos ||
           low.find("blackhole") != std::string::npos ||
           low.find("loopback") != std::string::npos;
}

/**
 * Lock-free egy-író / egy-olvasó (SPSC) sztereó keret-gyűrűpuffer (2-hatvány mérettel).
 * Mind a push(), mind a pop() valós idejű hangszálról hívható: 0 allokáció, 0 mutex.
 */
class StereoSpscRing {
public:
    explicit StereoSpscRing(size_t capacityPow2 = 65536) { init(capacityPow2); }

    void init(size_t capacityPow2) {
        size_t cap = 1024;
        while (cap < capacityPow2) cap <<= 1;
        cap_ = cap;
        mask_ = cap_ - 1;
        buf_.assign(cap_ * 2, 0.0f);
        clear();
    }

    void clear() {
        rd_.store(0, std::memory_order_relaxed);
        wr_.store(0, std::memory_order_relaxed);
    }

    size_t capacity() const { return cap_; }

    size_t available() const {
        const uint64_t w = wr_.load(std::memory_order_acquire);
        const uint64_t r = rd_.load(std::memory_order_acquire);
        return (w >= r) ? static_cast<size_t>(w - r) : 0;
    }

    size_t freeSpace() const {
        const size_t avail = available();
        return (cap_ > avail) ? (cap_ - avail) : 0;
    }

    /** Valós idejű írás (interleaved sztereó float). Visszaadja a ténylegesen beírt keretszámot. */
    size_t push(const float* stereo, size_t frames) {
        if (!stereo || frames == 0 || cap_ == 0) return 0;
        const uint64_t w = wr_.load(std::memory_order_relaxed);
        const uint64_t r = rd_.load(std::memory_order_acquire);
        const size_t free = (w >= r && (w - r) <= cap_) ? static_cast<size_t>(cap_ - (w - r)) : 0;
        const size_t take = std::min(frames, free);
        if (take == 0) return 0;

        const size_t pos = static_cast<size_t>(w & mask_);
        const size_t first = std::min(take, cap_ - pos);
        std::memcpy(&buf_[pos * 2], stereo, first * 2 * sizeof(float));
        if (take > first) {
            std::memcpy(&buf_[0], stereo + first * 2, (take - first) * 2 * sizeof(float));
        }
        wr_.store(w + take, std::memory_order_release);
        return take;
    }

    /** Valós idejű olvasás (interleaved sztereó float). Visszaadja a ténylegesen kiolvasott keretszámot. */
    size_t pop(float* stereo, size_t frames) {
        if (!stereo || frames == 0 || cap_ == 0) return 0;
        const uint64_t r = rd_.load(std::memory_order_relaxed);
        const uint64_t w = wr_.load(std::memory_order_acquire);
        const size_t avail = (w >= r) ? static_cast<size_t>(w - r) : 0;
        const size_t take = std::min(frames, avail);
        if (take == 0) return 0;

        const size_t pos = static_cast<size_t>(r & mask_);
        const size_t first = std::min(take, cap_ - pos);
        std::memcpy(stereo, &buf_[pos * 2], first * 2 * sizeof(float));
        if (take > first) {
            std::memcpy(stereo + first * 2, &buf_[0], (take - first) * 2 * sizeof(float));
        }
        rd_.store(r + take, std::memory_order_release);
        return take;
    }

private:
    size_t cap_ = 0;
    size_t mask_ = 0;
    std::vector<float> buf_;
    alignas(64) std::atomic<uint64_t> rd_{0};
    alignas(64) std::atomic<uint64_t> wr_{0};
};

/**
 * Finom órajel-drift kompenzátor a célmintavételen (4-pontos köbös Hermite-interpoláció).
 * Két külön hardveres órajel (pl. virtuális kábel és USB DAC) ±500 ppm eltérését simítja ki
 * kattanás nélkül; 0 drift esetén (< 0,1 ppm) pontos 1:1 átengedés (0 számítási hiba).
 */
class MicroDriftCompensator {
public:
    void reset() {
        hist_.clear();
        phase_ = 0.0;
        seeded_ = false;
    }

    /**
     * `step = 1.0 + driftRatio` (driftRatio > 0: kissé gyorsabb bemenet-fogyasztás a puffer csökkentéséhez;
     * driftRatio < 0: kissé több kimeneti minta a puffer feltöltéséhez).
     */
    void process(const float* inStereo, size_t inFrames, double driftRatio, std::vector<float>& outStereo) {
        outStereo.clear();
        if (!inStereo || inFrames == 0) return;

        const double dr = std::clamp(driftRatio, -0.0005, 0.0005);  // ±500 ppm korlát
        // Ha a két órajel gyakorlatilag szinkronban van és nincs fáziseltolás, 1:1 bitpontos átengedés
        if (std::abs(dr) < 1e-7 && (!seeded_ || std::abs(phase_) < 1e-9)) {
            outStereo.assign(inStereo, inStereo + inFrames * 2);
            // Az utolsó 4 keretet megőrizzük, hogy ha később drift indulna, zökkenőmentesen válthasson
            const size_t keep = std::min<size_t>(inFrames, 4);
            hist_.assign(inStereo + (inFrames - keep) * 2, inStereo + inFrames * 2);
            phase_ = 0.0;
            seeded_ = (keep >= 4);
            return;
        }

        if (!seeded_) {
            hist_.assign(4 * 2, 0.0f);  // 4 keret előzmény a Hermite-ablakhoz
            phase_ = 1.0;
            seeded_ = true;
        }

        const size_t oldFrames = hist_.size() / 2;
        hist_.resize((oldFrames + inFrames) * 2);
        std::memcpy(hist_.data() + oldFrames * 2, inStereo, inFrames * 2 * sizeof(float));
        const size_t totalFrames = hist_.size() / 2;

        const double step = 1.0 + dr;
        outStereo.reserve((inFrames + 4) * 2);

        // Hermite-ablak: idx - 1, idx, idx + 1, idx + 2 mind a tartományon belül maradjon
        while (phase_ + 2.0 < static_cast<double>(totalFrames)) {
            const size_t idx = static_cast<size_t>(phase_);
            if (idx < 1) { phase_ = 1.0; continue; }
            const float t = static_cast<float>(phase_ - static_cast<double>(idx));
            const float* xm1 = &hist_[(idx - 1) * 2];
            const float* x0  = &hist_[idx * 2];
            const float* x1  = &hist_[(idx + 1) * 2];
            const float* x2  = &hist_[(idx + 2) * 2];
            outStereo.push_back(hermite4(xm1[0], x0[0], x1[0], x2[0], t));
            outStereo.push_back(hermite4(xm1[1], x0[1], x1[1], x2[1], t));
            phase_ += step;
        }

        const size_t consumed = (phase_ >= 1.0) ? (static_cast<size_t>(phase_) - 1) : 0;
        if (consumed > 0 && consumed <= totalFrames) {
            const size_t rem = totalFrames - consumed;
            std::memmove(hist_.data(), hist_.data() + consumed * 2, rem * 2 * sizeof(float));
            hist_.resize(rem * 2);
            phase_ -= static_cast<double>(consumed);
        }
    }

private:
    static inline float hermite4(float y0, float y1, float y2, float y3, float t) {
        const float c0 = y1;
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + c0;
    }

    std::vector<float> hist_;
    double phase_ = 0.0;
    bool seeded_ = false;
};

/** Élő rendszerhang-felmintavételezés állapotpillanatképe a GUI és a diagnosztika számára. */
struct BridgeStatus {
    bool active = false;
    bool prefilled = false;
    uint32_t srcRate = 0;
    uint32_t dstRate = 0;
    int engineType = 0;       // 0 = libsoxr, 1 = Aqulify FFT64, 2 = Aqulify FFT-DD
    double latencyMs = 0.0;
    double driftPpm = 0.0;
    uint64_t underruns = 0;
    uint64_t overruns = 0;
    uint64_t clipped = 0;
    std::string reason;
};

/**
 * Platformfüggetlen élő rendszerhang-felmintavételező híd.
 *
 * Használat:
 *   1. configure(srcRate, dstRate, opts, dspCallback)
 *   2. start() (vagy szinkron teszthez stepOnce() közvetlenül, háttérszál nélkül)
 *   3. Capture RT szál: pushInput(stereo, frames)
 *   4. Playback RT szál: pullOutput(stereo, frames)
 *   5. stop()
 */
class LiveUpsampleBridge {
public:
    using DspCallback = std::function<void(float* stereo, size_t frames, uint32_t rate)>;

    LiveUpsampleBridge() : inRing_(65536), outRing_(262144) {}
    ~LiveUpsampleBridge() { stop(); }

    LiveUpsampleBridge(const LiveUpsampleBridge&) = delete;
    LiveUpsampleBridge& operator=(const LiveUpsampleBridge&) = delete;

    /**
     * Híd beállítása új mintavételpárra és szűrőopciókra. Leállítja az esetleges futó munkaszálat.
     * Ha srcRate == dstRate, átengedő módban (UpsampleEngine nélkül, csak opcionális DSP + drift) fut.
     */
    bool configure(uint32_t srcRate, uint32_t dstRate, const audioopt::AudioOptions& optsIn,
                   DspCallback dspCb = nullptr, bool enableDriftControl = true) {
        stop();
        if (srcRate < 8000 || srcRate > 768000 || dstRate < 8000 || dstRate > 768000) return false;

        audioopt::AudioOptions opts = optsIn;
        audioopt::sanitize(opts);

        srcRate_ = srcRate;
        dstRate_ = dstRate;
        opts_ = opts;
        dspCb_ = std::move(dspCb);
        driftEnabled_ = enableDriftControl;

        inRing_.clear();
        outRing_.clear();
        drift_.reset();
        errFiltered_ = 0.0;
        errIntegral_ = 0.0;
        driftLocked_ = false;
        driftTargetFrames_ = 0.0;
        driftRatio_.store(0.0, std::memory_order_relaxed);
        underruns_.store(0, std::memory_order_relaxed);
        overruns_.store(0, std::memory_order_relaxed);
        prefilled_.store(false, std::memory_order_relaxed);

        upsampler_.reset();
        const int thr = (opts.upsampleThreads == 2) ? 2 : 1;
        upsampler_.setThreads(thr);
        if (srcRate != dstRate) {
            if (!upsampler_.initWithOptions(srcRate, dstRate, 2, opts)) {
                configured_ = false;
                return false;
            }
        }

        // Cél puffer-kitöltöttség a kimeneti gyűrűpufferben:
        // libsoxr: ~90 ms (elnyeli a 28/32 bites VHQ blokk-lökéseit is), FFT64: elég nagy az Overlap-Save blokkhoz.
        const bool isFft = upsampler_.isFft64();
        size_t target = isFft ? static_cast<size_t>(dstRate) * 240 / 1000
                              : static_cast<size_t>(dstRate) * 90 / 1000;
        if (isFft) {
            size_t nIn = 0, nOut = 0;
            if (srcconv::fft64::computeBlockSizes(srcRate, dstRate, nIn, nOut)) {
                target = std::max(target, nOut / 2 + 8192);
            }
        } else {
            target = std::max<size_t>(target, 8192);
        }
        targetFillFrames_ = std::clamp<size_t>(target, 4096, outRing_.capacity() / 2);
        prefillFrames_ = targetFillFrames_;

        const size_t ratioCeil = static_cast<size_t>(std::ceil(static_cast<double>(dstRate) / static_cast<double>(srcRate))) + 1;
        inStage_.assign(kWorkChunk * 2, 0.0f);
        upStage_.assign(std::max<size_t>(kWorkChunk * ratioCeil + 4096, isFft ? 65536u : 16384u) * 2, 0.0f);
        driftStage_.reserve(upStage_.size());

        configured_ = true;
        return true;
    }

    /** Háttér munkaszál indítása. */
    bool start() {
        if (!configured_) return false;
        if (running_.exchange(true)) return true;
        worker_ = std::thread(&LiveUpsampleBridge::workerLoop, this);
        return true;
    }

    /** Háttér munkaszál tiszta leállítása. */
    void stop() {
        if (!running_.exchange(false)) return;
        {
            std::lock_guard<std::mutex> lk(wakeMu_);
            wakeFlag_ = true;
        }
        wakeCv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    bool isConfigured() const { return configured_; }
    bool isRunning() const { return running_.load(std::memory_order_acquire); }
    uint32_t srcRate() const { return srcRate_; }
    uint32_t dstRate() const { return dstRate_; }
    size_t targetFillFrames() const { return targetFillFrames_; }
    size_t outAvailableFrames() const { return outRing_.available(); }
    size_t inAvailableFrames() const { return inRing_.available(); }

    /**
     * Valós idejű bemeneti hívás (capture callback): 0 allokáció, 0 blokkoló lock.
     * Ha a gyűrűpuffer megtelt, a többletet eldobja és növeli az overrun számlálót.
     */
    size_t pushInput(const float* stereo, size_t frames) {
        if (!configured_ || !stereo || frames == 0) return 0;
        const size_t pushed = inRing_.push(stereo, frames);
        if (pushed < frames) {
            overruns_.fetch_add(frames - pushed, std::memory_order_relaxed);
        }
        if (pushed > 0 && running_.load(std::memory_order_relaxed)) {
            if (wakeMu_.try_lock()) {
                wakeFlag_ = true;
                wakeMu_.unlock();
                wakeCv_.notify_one();
            }
        }
        return pushed;
    }

    /**
     * Valós idejű kimeneti hívás (DAC render callback): 0 allokáció, 0 lock.
     * Induláskor megvárja a prefillFrames_ feltöltődést (addig csendet ad, nem számol underrunt).
     */
    size_t pullOutput(float* stereo, size_t frames) {
        if (!stereo || frames == 0) return 0;
        if (!configured_) {
            std::memset(stereo, 0, frames * 2 * sizeof(float));
            return 0;
        }
        if (!prefilled_.load(std::memory_order_acquire)) {
            if (outRing_.available() >= prefillFrames_) {
                prefilled_.store(true, std::memory_order_release);
            } else {
                std::memset(stereo, 0, frames * 2 * sizeof(float));
                return 0;
            }
        }
        const size_t got = outRing_.pop(stereo, frames);
        if (got < frames) {
            std::memset(stereo + got * 2, 0, (frames - got) * 2 * sizeof(float));
            underruns_.fetch_add(frames - got, std::memory_order_relaxed);
        }
        return got;
    }

    /**
     * Egy munkalépés szinkron futtatása (a háttérszál és a determinisztikus egységtesztek is ezt hívják).
     * Visszaadja, hogy dolgozott-e fel bemeneti kereteket.
     */
    bool stepOnce() {
        if (!configured_) return false;
        const size_t availIn = inRing_.available();
        if (availIn == 0) return false;
        const size_t minOutFree = upsampler_.isFft64() ? 65536u : 8192u;
        if (outRing_.freeSpace() < minOutFree) return false;

        const size_t take = std::min(availIn, kWorkChunk);
        const size_t got = inRing_.pop(inStage_.data(), take);
        if (got == 0) return false;

        const float* prodPtr = inStage_.data();
        size_t prodFrames = got;

        if (srcRate_ != dstRate_) {
            size_t outFrames = 0;
            if (!upsampler_.process(inStage_.data(), got, upStage_.data(), upStage_.size() / 2, outFrames)) {
                return true;
            }
            prodPtr = upStage_.data();
            prodFrames = outFrames;
        }

        if (prodFrames == 0) return true;

        // PI órajel-drift szabályzó: a teljes csővezeték-kitöltöttség (bemeneti gyűrű + szűrőkésleltetés + kimeneti gyűrű)
        // teljesen mentes a libsoxr/FFT64 blokklökések fűrészfog-ingadozásától — kizárólag a valódi órajel-driftet látja!
        double dr = 0.0;
        if (driftEnabled_ && prefilled_.load(std::memory_order_acquire)) {
            const double ratio = static_cast<double>(dstRate_) / static_cast<double>(srcRate_);
            const double pipeFill = static_cast<double>(inRing_.available()) * ratio +
                                    ((srcRate_ != dstRate_) ? upsampler_.delayFrames() : 0.0) +
                                    static_cast<double>(outRing_.available()) +
                                    static_cast<double>(prodFrames);
            if (!driftLocked_) {
                driftTargetFrames_ = pipeFill;
                driftLocked_ = true;
            }
            const double errSec = (pipeFill - driftTargetFrames_) / static_cast<double>(dstRate_);
            const double dt = static_cast<double>(prodFrames) / static_cast<double>(dstRate_);
            const double alpha = std::clamp(dt / 0.4, 0.005, 0.35);
            errFiltered_ += alpha * (errSec - errFiltered_);
            errIntegral_ = std::clamp(errIntegral_ + errFiltered_ * dt, -0.05, 0.05);
            // Kritikusan csillapított PI (omega_n = 0,20 rad/s, zeta = 1,0): ~10 s beállás, zéró túllövés
            dr = std::clamp(0.40 * errFiltered_ + 0.04 * errIntegral_, -0.0005, 0.0005);
            driftRatio_.store(dr, std::memory_order_relaxed);
        } else {
            driftRatio_.store(0.0, std::memory_order_relaxed);
        }

        drift_.process(prodPtr, prodFrames, dr, driftStage_);
        size_t finalFrames = driftStage_.size() / 2;
        if (finalFrames == 0) return true;

        if (dspCb_) {
            dspCb_(driftStage_.data(), finalFrames, dstRate_);
        }

        // Ha a kimeneti gyűrűpufferben átmenetileg kevés hely van, a lehető legtöbbet írjuk be
        size_t written = outRing_.push(driftStage_.data(), finalFrames);
        if (written < finalFrames) {
            overruns_.fetch_add(finalFrames - written, std::memory_order_relaxed);
        }
        return true;
    }

    /** Aktuális állapot-pillanatkép lekérése (bármely szálról biztonságos). */
    BridgeStatus status() const {
        BridgeStatus st;
        st.active = configured_ && (running_.load(std::memory_order_acquire) || prefilled_.load(std::memory_order_acquire));
        st.prefilled = prefilled_.load(std::memory_order_acquire);
        st.srcRate = srcRate_;
        st.dstRate = dstRate_;
        st.engineType = upsampler_.engineType();   // a 106 bites FFT-DD is külön (eddig FFT64-nek látszott)
        if (srcRate_ > 0 && dstRate_ > 0) {
            const double inMs = 1000.0 * static_cast<double>(inRing_.available()) / static_cast<double>(srcRate_);
            const double outMs = 1000.0 * static_cast<double>(outRing_.available()) / static_cast<double>(dstRate_);
            const double filtMs = (srcRate_ != dstRate_)
                                      ? (1000.0 * upsampler_.delayFrames() / static_cast<double>(dstRate_))
                                      : 0.0;
            st.latencyMs = inMs + outMs + filtMs;
        }
        st.driftPpm = driftRatio_.load(std::memory_order_relaxed) * 1e6;
        st.underruns = underruns_.load(std::memory_order_relaxed);
        st.overruns = overruns_.load(std::memory_order_relaxed);
        st.clipped = upsampler_.clippedSamples();
        return st;
    }

private:
    static constexpr size_t kWorkChunk = 1024;

    void workerLoop() {
        while (running_.load(std::memory_order_acquire)) {
            bool didWork = false;
            while (running_.load(std::memory_order_acquire) && stepOnce()) {
                didWork = true;
            }
            if (!didWork) {
                std::unique_lock<std::mutex> lk(wakeMu_);
                wakeCv_.wait_for(lk, std::chrono::milliseconds(4), [&] {
                    return wakeFlag_ || !running_.load(std::memory_order_acquire);
                });
                wakeFlag_ = false;
            }
        }
    }

    StereoSpscRing inRing_;
    StereoSpscRing outRing_;
    UpsampleEngine upsampler_;
    MicroDriftCompensator drift_;
    DspCallback dspCb_;

    bool configured_ = false;
    bool driftEnabled_ = true;
    uint32_t srcRate_ = 0;
    uint32_t dstRate_ = 0;
    audioopt::AudioOptions opts_;
    size_t targetFillFrames_ = 4096;
    size_t prefillFrames_ = 4096;

    double errFiltered_ = 0.0;
    double errIntegral_ = 0.0;
    bool driftLocked_ = false;
    double driftTargetFrames_ = 0.0;
    std::atomic<double> driftRatio_{0.0};
    std::atomic<bool> prefilled_{false};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> overruns_{0};

    std::vector<float> inStage_;
    std::vector<float> upStage_;
    std::vector<float> driftStage_;

    std::atomic<bool> running_{false};
    std::mutex wakeMu_;
    std::condition_variable wakeCv_;
    bool wakeFlag_ = false;
    std::thread worker_;
};

}  // namespace ee_analyzer::livebridge
