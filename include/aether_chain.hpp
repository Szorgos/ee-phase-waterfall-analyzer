#pragma once
/**
 * @file aether_chain.hpp
 * @brief A teljes AetherAudio DSP lánc (Linux PipeWire csomópont és Windows EQ APO plugin közös magja).
 *
 *   Preamp -> EQ (IIR vagy FIR: lineáris / köztes / minimálfázis) -> Bass Enhancer -> Crossfeed
 *          -> Convolver (partícionált FFT, IR) -> Limiter
 *
 * A drága műveleteket (FIR tervezés, IR újramintavételezés, minimálfázisúvá alakítás, partíciók FFT-je)
 * egy háttérszál végzi. Az elkészült konvolvert a valós idejű szál zármentesen veszi át, egy blokknyi
 * keresztúsztatással (nincs kattanás), a régit pedig a háttérszál szabadítja fel.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "dsp_pipeline.hpp"
#include "param_apply.hpp"
#include "bass_enhancer.hpp"
#include "true_peak_limiter.hpp"
#include "oversampler.hpp"
#include "partitioned_convolver.hpp"
#include "fir_design.hpp"
#include "ir_data.hpp"

namespace ee_analyzer {

class AetherChain {
public:
    static constexpr size_t kChunk = 1024;

    AetherChain() {
        preparePipeline(pipe_);
        for (auto* v : { &aL_, &aR_, &bL_, &bR_ }) v->assign(kChunk, 0.0f);
        limiter_.prepare(48000.0);
        current_ = transparentParams();
        applyParamsInternal(current_);
    }

    ~AetherChain() {
        stopWorker();
        for (Stage* st : { &eqStage_, &irStage_ }) {
            if (Update* u = st->pending.exchange(nullptr)) { delete u->conv; delete u; }
            if (Update* u = st->retired.exchange(nullptr)) { delete u->old; delete u; }
            delete st->active;
            st->active = nullptr;
        }
    }

    AetherChain(const AetherChain&) = delete;
    AetherChain& operator=(const AetherChain&) = delete;

    // ------------------------------------------------------------ vezérlés (nem valós idejű)

    /** Mintavételi frekvencia (hívás csak leállított feldolgozás mellett). */
    void setSampleRate(double sr) {
        if (sr <= 0) return;
        setPipelineSampleRate(pipe_, sr);
        limiter_.prepare(sr);
        oversampler_.reset();
        bass_.sampleRate = sr;
        bass_.update();
        bass_.reset();
        applyParamsInternal(current_);
        sampleRate_.store(sr);
    }

    double sampleRate() const { return sampleRate_.load(); }

    /** Új impulzusválasz a konvolvernek (bármely szálról, nem valós idejű). */
    void submitIr(IrData ir) {
        std::lock_guard<std::mutex> lk(irMutex_);
        ir_ = std::move(ir);
        irVersion_.fetch_add(1);
    }

    IrData irCopy() const {
        std::lock_guard<std::mutex> lk(irMutex_);
        return ir_;
    }

    /** Háttérszál indítása. A tick (opcionális) minden körben lefut (pl. közös memória figyelése). */
    void startWorker(std::function<void()> tick = {}) {
        if (workerRunning_.exchange(true)) return;
        tick_ = std::move(tick);
        worker_ = std::thread(&AetherChain::workerLoop, this);
    }

    void stopWorker() {
        if (!workerRunning_.exchange(false)) return;
        if (worker_.joinable()) worker_.join();
    }

    /** Teszteknek: megvárja, amíg a háttérszál minden változást feldolgozott. */
    bool waitIdle(int timeoutMs = 5000) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < until) {
            if (workerIdle_.load() && workerParams_.currentSeq() == workerSeenSeq_.load() &&
                irVersion_.load() == workerSeenIr_.load()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    // ------------------------------------------------------------ valós idejű szál

    /** Új paraméterek alkalmazása (valós idejű szálból, foglalás nélkül). */
    void applyParamsRT(const ParamData& d) {
        current_ = d;
        applyParamsInternal(d);
        workerParams_.write(d);
    }

    const ParamData& params() const { return current_; }

    /** Helyben végzett sztereó feldolgozás. */
    void process(float* L, float* R, size_t n) {
        size_t off = 0;
        while (off < n) {
            const size_t m = std::min(kChunk, n - off);
            processChunk(L + off, R + off, m);
            off += m;
        }
    }

    double gainReductionDb() const { return limiter_.enabled ? limiter_.gainReductionDb() : 0.0; }
    size_t limiterLatencySamples() const { return limiter_.enabled ? limiter_.latencySamples() : 0; }
    bool firEqActive() const { return eqStage_.active != nullptr; }
    bool convolverActive() const { return irStage_.active != nullptr; }
    size_t eqLatencySamples() const { return eqLatency_.load(); }

    void reset() {
        pipe_.reset();
        limiter_.reset();
        oversampler_.reset();
        bass_.reset();
        if (eqStage_.active) eqStage_.active->reset();
        if (irStage_.active) irStage_.active->reset();
    }

private:
    struct Update {
        PartitionedConvolver* conv = nullptr; // nullptr = a fokozat kikapcsolása
        PartitionedConvolver* old = nullptr;  // a valós idejű szál ide teszi a lecserélt példányt
    };
    struct Stage {
        std::atomic<Update*> pending{nullptr};
        std::atomic<Update*> retired{nullptr};
        PartitionedConvolver* active = nullptr; // csak a valós idejű szál használja
    };

    void applyParamsInternal(const ParamData& d) {
        // Az IIR sávokat FIR módban is betöltjük: amíg a FIR elkészül, az IIR szól helyette
        ParamData iir = d;
        iir.eqMode = static_cast<int32_t>(EqMode::Iir);
        applyParams(pipe_, iir);

        // Decramp: az IIR biquadok N-szeres mintavételre tervezve, N-szeres rátán futnak
        decramp_ = d.eqDecramp >= 8 ? 8 : d.eqDecramp >= 4 ? 4 : d.eqDecramp >= 2 ? 2 : 1;
        if (decramp_ > 1) {
            for (auto& f : pipe_.eqChain.filters) f.sampleRate = pipe_.sampleRate * decramp_;
            pipe_.updateEqCoefficients();
        }
        if (decramp_ != lastDecramp_) {
            oversampler_.reset();
            for (size_t f = 0; f < pipe_.eqStatesL.size(); ++f) { pipe_.eqStatesL[f].reset(); pipe_.eqStatesR[f].reset(); }
            lastDecramp_ = decramp_;
        }

        // True peak limiter (a régi mintaérték-limiter helyett)
        pipe_.limiter.enabled = false;
        const bool limOn = d.has(kFlagLimiter);
        if (limOn && !limiter_.enabled) limiter_.reset();
        limiter_.enabled = limOn;
        limiter_.configure(d.limiterCeilingDb, d.limiterReleaseMs, d.limiterLookaheadMs, d.limiterOversample);

        bass_.enabled = d.has(kFlagBass);
        bass_.crossoverHz = d.bassCrossoverHz;
        bass_.harmonicsDb = d.bassHarmonicsDb;
        bass_.subcutHz = d.bassSubcutHz;
        bass_.type = static_cast<BassEnhancer::Harmonics>(std::clamp(d.bassType, 0, 2));
        bass_.update();
        convGain_ = static_cast<float>(std::pow(10.0, std::clamp(static_cast<double>(d.convGainDb), -40.0, 20.0) / 20.0));
    }

    /** Fokozat: ha van új konvolver, egy blokknyi keresztúsztatással cserél. */
    template <class NullPath, class WetPath>
    void runStage(Stage& st, float* L, float* R, size_t m, NullPath&& nullPath, WetPath&& wetPath) {
        Update* u = nullptr;
        if (st.retired.load(std::memory_order_acquire) == nullptr) {
            u = st.pending.exchange(nullptr, std::memory_order_acq_rel);
        }
        if (!u) {
            if (st.active) wetPath(st.active, L, R, m);
            else nullPath(L, R, m);
            return;
        }
        PartitionedConvolver* oldConv = st.active;
        PartitionedConvolver* newConv = u->conv;
        std::memcpy(aL_.data(), L, m * sizeof(float));
        std::memcpy(aR_.data(), R, m * sizeof(float));
        std::memcpy(bL_.data(), L, m * sizeof(float));
        std::memcpy(bR_.data(), R, m * sizeof(float));
        if (oldConv) wetPath(oldConv, aL_.data(), aR_.data(), m);
        else nullPath(aL_.data(), aR_.data(), m);
        if (newConv) wetPath(newConv, bL_.data(), bR_.data(), m);
        else nullPath(bL_.data(), bR_.data(), m);
        const float inv = 1.0f / static_cast<float>(m);
        for (size_t i = 0; i < m; ++i) {
            const float t = (static_cast<float>(i) + 1.0f) * inv;
            L[i] = aL_[i] + t * (bL_[i] - aL_[i]);
            R[i] = aR_[i] + t * (bR_[i] - aR_[i]);
        }
        st.active = newConv;
        u->old = oldConv;
        st.retired.store(u, std::memory_order_release);
    }

    void processChunk(float* L, float* R, size_t m) {
        pipe_.processPreamp(L, R, m);

        // EQ: FIR konvolver, vagy (ha nincs aktív FIR) az IIR lánc
        const bool wasFir = eqStage_.active != nullptr;
        runStage(eqStage_, L, R, m,
                 [this, wasFir](float* l, float* r, size_t k) {
                     if (wasFir) { for (size_t f = 0; f < pipe_.eqStatesL.size(); ++f) { pipe_.eqStatesL[f].reset(); pipe_.eqStatesR[f].reset(); } }
                     if (decramp_ > 1 && !pipe_.eqCoeffs.empty()) {
                         oversampler_.process(l, r, k, decramp_, [this](float* L2, float* R2, size_t n2) { pipe_.processIirEq(L2, R2, n2); });
                     } else {
                         pipe_.processIirEq(l, r, k);
                     }
                 },
                 [](PartitionedConvolver* c, float* l, float* r, size_t k) { c->process(l, r, k); });

        bass_.process(L, R, m);

        if (pipe_.crossfeed.enabled) pipe_.crossfeed.processBlock(L, R, m);

        const float g = convGain_;
        runStage(irStage_, L, R, m,
                 [](float*, float*, size_t) {},
                 [g](PartitionedConvolver* c, float* l, float* r, size_t k) {
                     c->process(l, r, k);
                     for (size_t i = 0; i < k; ++i) { l[i] *= g; r[i] *= g; }
                 });

        limiter_.process(L, R, m);
    }

    // ------------------------------------------------------------ háttérszál
    struct EqKey {
        int32_t mode = -1;
        int32_t decramp = 1;
        uint32_t numBands = 0;
        EqBandPod bands[kMaxEqBands]{};
        double sr = 0;
        bool operator==(const EqKey& o) const {
            return mode == o.mode && decramp == o.decramp && numBands == o.numBands && sr == o.sr &&
                   std::memcmp(bands, o.bands, sizeof(EqBandPod) * numBands) == 0;
        }
    };
    struct IrKey {
        uint64_t version = 0;
        int32_t phase = -1;
        int32_t maxTaps = 0;
        double sr = 0;
        bool on = false;
        bool operator==(const IrKey& o) const {
            return version == o.version && phase == o.phase && maxTaps == o.maxTaps && sr == o.sr && on == o.on;
        }
    };

    void publish(Stage& st, PartitionedConvolver* conv) {
        Update* u = new Update{conv, nullptr};
        if (Update* stale = st.pending.exchange(u, std::memory_order_acq_rel)) {
            delete stale->conv; // a valós idejű szál még nem vette át: eldobható
            delete stale;
        }
    }

    void freeRetired(Stage& st) {
        if (Update* r = st.retired.exchange(nullptr, std::memory_order_acq_rel)) {
            delete r->old;
            delete r;
        }
    }

    void workerLoop() {
        EqKey lastEq;           // mode = -1: még semmi
        IrKey lastIr;
        bool eqPublishedOn = false, irPublishedOn = false;
        while (workerRunning_) {
            workerIdle_ = false;
            if (tick_) tick_();
            freeRetired(eqStage_);
            freeRetired(irStage_);

            ParamData d;
            uint32_t seq = 0;
            const uint32_t seqNow = workerParams_.currentSeq();
            bool haveParams = workerParams_.tryRead(d, seq);
            if (!haveParams) {
                if (seqNow != 0) { // éppen írás zajlik: a következő körben újra
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                d = transparentParams(); // még senki nem állított semmit
            }
            const double sr = sampleRate_.load();

            // --- FIR EQ ---
            EqKey want;
            const bool firOn = d.has(kFlagEq) && d.eqMode != static_cast<int32_t>(EqMode::Iir) && d.numBands > 0;
            if (firOn) {
                want.mode = d.eqMode;
                want.decramp = d.eqDecramp >= 8 ? 8 : d.eqDecramp >= 4 ? 4 : d.eqDecramp >= 2 ? 2 : 1;
                want.numBands = std::min(d.numBands, kMaxEqBands);
                std::memcpy(want.bands, d.bands, sizeof(EqBandPod) * want.numBands);
                want.sr = sr;
            }
            if (firOn && !(want == lastEq)) {
                std::vector<BiquadFilter> filters;
                for (uint32_t i = 0; i < want.numBands; ++i) {
                    const EqBandPod& b = want.bands[i];
                    // Decramp: a cél-amplitúdómenet az N-szeres rátára tervezett (közel analóg) szűrőké
                    filters.emplace_back(toFilterType(b.type), std::clamp(static_cast<double>(b.freq), 10.0, sr * 0.49),
                                         std::clamp(static_cast<double>(b.q), 0.05, 40.0),
                                         std::clamp(static_cast<double>(b.gainDb), -30.0, 30.0), sr * want.decramp);
                }
                size_t lat = 0;
                std::vector<float> h = designEqFir(filters, sr, static_cast<FirPhase>(want.mode), recommendedEqFirTaps(sr), &lat,
                                                   want.decramp);
                auto* conv = new PartitionedConvolver();
                conv->build(h, {});
                publish(eqStage_, conv);
                eqLatency_ = lat;
                lastEq = want;
                eqPublishedOn = true;
            } else if (!firOn && eqPublishedOn) {
                publish(eqStage_, nullptr);
                eqPublishedOn = false;
                eqLatency_ = 0;
                lastEq = EqKey{};
            }

            // --- Convolver IR ---
            IrKey wantIr;
            IrData ir;
            uint64_t irVer = 0;
            {
                std::lock_guard<std::mutex> lk(irMutex_);
                irVer = irVersion_.load();
                wantIr.on = d.has(kFlagConvolver) && !ir_.empty();
                if (wantIr.on) {
                    wantIr.version = irVer;
                    wantIr.phase = d.convPhaseMode;
                    wantIr.maxTaps = d.convMaxTaps;
                    wantIr.sr = sr;
                    if (!(wantIr == lastIr)) ir = ir_;
                }
            }
            if (wantIr.on && !(wantIr == lastIr) && !ir.empty()) {
                std::vector<float> l = resampleIr(ir.left, ir.sampleRate, sr);
                std::vector<float> r = ir.right.empty() ? std::vector<float>{} : resampleIr(ir.right, ir.sampleRate, sr);
                const size_t maxTaps = static_cast<size_t>(std::clamp(d.convMaxTaps, 256, 262144));
                if (l.size() > maxTaps) l.resize(maxTaps);
                if (r.size() > maxTaps) r.resize(maxTaps);
                if (d.convPhaseMode == 0) {
                    l = toMinimumPhase(l);
                    if (!r.empty()) r = toMinimumPhase(r);
                }
                normalizeIrPeakResponse(l, r);
                auto* conv = new PartitionedConvolver();
                conv->build(l, r);
                publish(irStage_, conv);
                lastIr = wantIr;
                irPublishedOn = true;
            } else if (!wantIr.on && irPublishedOn) {
                publish(irStage_, nullptr);
                irPublishedOn = false;
                lastIr = IrKey{};
            }

            workerSeenSeq_ = haveParams ? seq : seqNow;
            workerSeenIr_ = irVer;
            workerIdle_ = true; // minden ismert változás feldolgozva (az átvétel a valós idejű szálon történik)
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }

    // valós idejű állapot
    DspPipeline pipe_;
    BassEnhancer bass_;
    TruePeakLimiter limiter_;
    Oversampler oversampler_;
    int decramp_ = 1, lastDecramp_ = 1;
    ParamData current_;
    float convGain_ = 1.0f;
    std::vector<float> aL_, aR_, bL_, bR_;
    Stage eqStage_, irStage_;

    // megosztott
    std::atomic<double> sampleRate_{48000.0};
    ParamBlock workerParams_;
    std::atomic<size_t> eqLatency_{0};

    mutable std::mutex irMutex_;
    IrData ir_;
    std::atomic<uint64_t> irVersion_{0};

    std::atomic<bool> workerRunning_{false};
    std::atomic<bool> workerIdle_{true};
    std::atomic<uint32_t> workerSeenSeq_{0};
    std::atomic<uint64_t> workerSeenIr_{0};
    std::function<void()> tick_;
    std::thread worker_;
};

} // namespace ee_analyzer
