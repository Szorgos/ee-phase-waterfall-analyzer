#pragma once
/**
 * @file chain_player_dsp.hpp
 * @brief A beépített lejátszó DSP-lánca a kizárólagos (bitpontos) kimenethez, rendszerfüggetlenül (Linux: közvetlen
 *        ALSA-kimenet). Ugyanaz az AetherChain, ugyanazokkal a beállításokkal és IR-rel, mint a rendszer DSP-útja; a
 *        feldolgozott jel a vízesésnek és a mérőknek a saját lehallgatóba (tap) kerül.
 *
 * Hangerő: digitális, a lánc végén (a limiter után, a dither előtt); 1,0-nál és némítás nélkül a kimenet érintetlen,
 * így átlátszó lánccal a kimenet bitpontos.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#endif

#include "aether_chain.hpp"
#include "exclusive_dsp.hpp"
#include "param_block.hpp"
#include "tap_ring.hpp"

namespace ee_analyzer::player {

/** A denormált számok nullázása a feldolgozás idejére (mint a pluginban): x86-on FTZ|DAZ, ARM64-en az FPCR FZ bitje. */
class DenormalGuard {
public:
    DenormalGuard() {
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
        old_ = _mm_getcsr();
        _mm_setcsr(old_ | 0x8040);
#elif defined(__aarch64__)
        uint64_t v;
        __asm__ volatile("mrs %0, fpcr" : "=r"(v));
        old_ = v;
        v |= (1ull << 24);
        __asm__ volatile("msr fpcr, %0" : : "r"(v));
#endif
    }
    ~DenormalGuard() {
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
        _mm_setcsr(static_cast<unsigned int>(old_));
#elif defined(__aarch64__)
        const uint64_t v = old_;
        __asm__ volatile("msr fpcr, %0" : : "r"(v));
#endif
    }
    DenormalGuard(const DenormalGuard&) = delete;
    DenormalGuard& operator=(const DenormalGuard&) = delete;

private:
    uint64_t old_ = 0;
};

class ChainPlayerDsp : public ExclusiveDsp {
public:
    explicit ChainPlayerDsp(const ParamData& initial) {
        chain_.setSampleRate(48000);
        chain_.startWorker();
        params_.write(initial);
    }
    ~ChainPlayerDsp() override { chain_.stopWorker(); }

    void setParams(const ParamData& p) { params_.write(p); }
    void submitIr(IrData ir) { chain_.submitIr(std::move(ir)); }

    /** Digitális hangerő (0..1, hallás szerinti skála) és némítás; level < 0 / mute < 0: nem változik. */
    void setVolume(double level, int mute) {
        if (level >= 0.0) level_ = static_cast<float>(std::clamp(level, 0.0, 1.0));
        if (mute >= 0) muted_ = mute != 0;
        const float l = level_.load();
        // 0 → néma, 1 → 0 dB, közte dB-ben egyenletesen (-50 dB-ig), mint Windowson
        const float g = muted_.load() || l <= 0.0f ? 0.0f : (l >= 1.0f ? 1.0f : std::pow(10.0f, (l - 1.0f) * 50.0f / 20.0f));
        chain_.setOutputGain(g);
    }
    double volumeLevel() const { return level_.load(); }
    bool volumeMuted() const { return muted_.load(); }

    // ---- ExclusiveDsp (a kimenet szálán)
    void prepare(uint32_t rate, uint32_t blockFrames, int ditherBits) override {
        chain_.setSampleRate(rate);
        chain_.reset();
        rate_ = rate;
        bits_ = ditherBits;
        appliedSeq_ = 0;                 // a beállítás újra, az új bitmélységgel
        L_.assign(blockFrames, 0.0f);
        R_.assign(blockFrames, 0.0f);
    }

    void process(float* lr, size_t n) override {
        DenormalGuard guard;
        const uint32_t seq = params_.currentSeq();
        if (seq != appliedSeq_ && (seq & 1u) == 0) {
            ParamData pd;
            uint32_t readSeq = 0;
            if (params_.tryRead(pd, readSeq)) {
                if (bits_) pd.ditherBits = bits_;   // a dither a kimenet tényleges bitmélységére
                chain_.applyParamsRT(pd);
                transparent_.store(!AetherChain::modifiesSignal(pd), std::memory_order_relaxed);
                appliedSeq_ = readSeq;
            }
        }
        if (L_.size() < n) { L_.resize(n); R_.resize(n); }
        for (size_t i = 0; i < n; ++i) { L_[i] = lr[2 * i]; R_[i] = lr[2 * i + 1]; }
        chain_.process(L_.data(), R_.data(), n);
        for (size_t i = 0; i < n; ++i) { lr[2 * i] = L_[i]; lr[2 * i + 1] = R_[i]; }
        tap_.write(L_.data(), R_.data(), static_cast<uint32_t>(n));
        gr_.store(static_cast<float>(chain_.gainReductionDb()), std::memory_order_relaxed);
        bass_.store(static_cast<float>(chain_.bassHarmonicsDb()), std::memory_order_relaxed);
        latency_.store(chain_.eqLatencySamples() + chain_.limiterLatencySamples() +
                           (chain_.params().has(kFlagConvolver) && chain_.convolverActive()
                                ? ee::ZitaInstance::latencyFor(static_cast<uint32_t>(n)) : 0),
                       std::memory_order_relaxed);
        lastData_.store(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count()),
                        std::memory_order_relaxed);
    }

    size_t latencySamples() const override { return latency_.load(std::memory_order_relaxed); }

    // ---- a motornak (bármely szálról)
    StereoTapRing<65536>& tap() { return tap_; }
    uint32_t sampleRate() const { return rate_.load(); }
    double gainReductionDb() const { return gr_.load(); }
    double bassHarmonicsDb() const { return bass_.load(); }
    /** A lánc nem módosítja a jelet (az utolsó alkalmazott beállítás szerint). */
    bool transparent() const { return transparent_.load(); }
    /** A digitális hangerő módosítja-e most a jelet. */
    bool volumeTouches() const { return chain_.outputGain() != 1.0f; }

private:
    AetherChain chain_;
    ParamBlock params_;
    StereoTapRing<65536> tap_;
    std::vector<float> L_, R_;
    uint32_t appliedSeq_ = 0;
    int bits_ = 0;
    std::atomic<uint32_t> rate_{48000};
    std::atomic<float> gr_{0.0f}, bass_{-100.0f};
    std::atomic<bool> transparent_{true};
    std::atomic<size_t> latency_{0};
    std::atomic<uint64_t> lastData_{0};
    std::atomic<float> level_{1.0f};
    std::atomic<bool> muted_{false};
};

} // namespace ee_analyzer::player
