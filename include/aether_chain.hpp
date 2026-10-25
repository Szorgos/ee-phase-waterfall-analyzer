#pragma once
/**
 * @file aether_chain.hpp
 * @brief A teljes Aqulify DSP lánc (Linux PipeWire csomópont és Windows EQ APO plugin közös magja).
 *
 *   Magic Touch -> Preamp -> Equalizer (LSP) -> Bass Enhancer (Calf) -> Crossfeed (bs2b) -> Convolver (zita) -> Limiter (LSP)
 *
 * A Magic Touch (magic_touch.hpp) a felvételt javítja, ezért a lánc elején van; az elemzése a háttérszálon fut.
 * A modulok az EasyEffects-szel azonos könyvtárakat és beállítás-leképezést használnak (ee_dsp.hpp).
 * A konvolver kernelét (újramintavételezés, szélesség, autogain) és a zita példányt (FFTW tervek, szálak)
 * egy háttérszál készíti. Az elkészült példányt a valós idejű szál zármentesen veszi át, egy blokknyi
 * keresztúsztatással; a régit a háttérszál szabadítja fel.
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

#include "param_block.hpp"
#include "ir_data.hpp"
#include "ee_dsp.hpp"
#include "magic_touch.hpp"
#include "mono_bass.hpp"

namespace ee_analyzer {

class AetherChain {
public:
    /** Egy hívásban feldolgozott legnagyobb blokk (a PipeWire legnagyobb kvantuma is ennyi). */
    static constexpr size_t kChunk = 8192;

    AetherChain() {
        ee::initDsp();
        for (auto* v : { &aL_, &aR_, &bL_, &bR_, &dryL_, &dryR_ }) v->assign(kChunk, 0.0f);
        current_ = transparentParams();
        setSampleRateInternal(48000);
    }

    ~AetherChain() {
        stopWorker();
        if (Update* u = irStage_.pending.exchange(nullptr)) { delete u->inst; delete u; }
        if (Update* u = irStage_.retired.exchange(nullptr)) { delete u->old; delete u; }
        delete irStage_.active;
        irStage_.active = nullptr;
    }

    AetherChain(const AetherChain&) = delete;
    AetherChain& operator=(const AetherChain&) = delete;

    // ------------------------------------------------------------ vezérlés (nem valós idejű)

    /** Mintavételi frekvencia (hívás csak leállított feldolgozás mellett). */
    void setSampleRate(double sr) {
        if (sr <= 0) return;
        setSampleRateInternal(static_cast<uint32_t>(std::lround(sr)));
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
                irVersion_.load() == workerSeenIr_.load() && quantum_.load() == workerSeenQuantum_.load()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    // ------------------------------------------------------------ valós idejű szál

    /**
     * Új paraméterek alkalmazása (valós idejű szálból, foglalás nélkül).
     *
     * Modul be/ki kapcsolásakor és késleltetést változtató beállításnál (EQ mód, decramp, limiter
     * túlmintavételezés / előretekintés) a váltás kattanásmentes: rövid leúsztatás, váltás, majd a
     * kimenet az új késleltetés kitelte után úsztatva tér vissza. Egyéb változás azonnal hat.
     */
    void applyParamsRT(const ParamData& in) {
        ParamData d = in;
        sanitizeParams(d);
        workerParams_.write(d);
        if (fade_ == Fade::Out) { pending_ = d; return; }   // a leúsztatás végén ez lép életbe
        if (needsDeclick(current_, d)) {
            pending_ = d;
            // Felúsztatás közben a pillanatnyi szintről indul a leúsztatás (néma várakozásnál azonnal vált)
            if (fade_ == Fade::None) fadePos_ = fadeLen_;
            fade_ = Fade::Out;
            return;
        }
        current_ = d;
        applyParamsInternal(d);
    }

    const ParamData& params() const { return current_; }

    /** Helyben végzett sztereó feldolgozás. */
    void process(float* L, float* R, size_t n) {
        ee::DspScope scope;
        size_t off = 0;
        while (off < n) {
            const size_t m = std::min(kChunk, n - off);
            processChunk(L + off, R + off, m);
            off += m;
        }
    }

    /** Limiter erősítéscsökkentés dB-ben (negatív szám, 0 = nincs csillapítás). */
    double gainReductionDb() const { return current_.has(kFlagLimiter) ? -limiter_.gainReductionDb() : 0.0; }
    /** A Bass Enhancer által keltett felharmonikusok szintje dB-ben (a Calf "meter_drive"; kikapcsolva -100). */
    double bassHarmonicsDb() const { return current_.has(kFlagBass) ? bass_.harmonicsDb() : -100.0; }
    size_t limiterLatencySamples() const { return current_.has(kFlagLimiter) ? limiter_.latency() : 0; }
    size_t eqLatencySamples() const { return current_.has(kFlagEq) ? eq_.latency() : 0; }
    bool convolverActive() const { return irStage_.active != nullptr; }

    /** A Magic Touch mérései és beavatkozásai (bármely szálról). false, ha még nincs adat. */
    bool magicDiag(magic::MagicDiag& out) const { return magic_.diag(out); }

    /** A Magic Touch fut-e (bekapcsolva, nem 0 % intenzitással). */
    static bool magicActive(const ParamData& d) { return d.has(kFlagMagic) && d.magicAmount > 0.0f; }

    void reset() {
        magic_.reset();
        eq_.reset();
        bass_.reset();
        monoBass_.reset();
        crossfeed_.reset();
        limiter_.reset();
        if (irStage_.active) irStage_.active->reset();
    }

private:
    struct Update {
        ee::ZitaInstance* inst = nullptr; // nullptr = a konvolver kikapcsolása
        ee::ZitaInstance* old = nullptr;  // a valós idejű szál ide teszi a lecserélt példányt
    };
    struct Stage {
        std::atomic<Update*> pending{nullptr};
        std::atomic<Update*> retired{nullptr};
        ee::ZitaInstance* active = nullptr; // csak a valós idejű szál használja
    };

    void setSampleRateInternal(uint32_t sr) {
        magic_.setSampleRate(sr);
        eq_.setSampleRate(sr);
        limiter_.setSampleRate(sr);
        bass_.setSampleRate(sr);
        monoBass_.setSampleRate(sr);
        crossfeed_.setSampleRate(sr);
        sampleRate_.store(sr);
        // Mintavétel-váltáskor a lánc úgyis újraindul: a függő váltás azonnal érvényes
        if (fade_ == Fade::Out) current_ = pending_;
        fade_ = Fade::None;
        fadeSamples_ = std::max(1.0f, 0.005f * static_cast<float>(sr));   // 5 ms
        fadeLen_ = static_cast<size_t>(fadeSamples_ + 0.5f);
        fadePos_ = fadeLen_;
        applyParamsInternal(current_);
        preampCur_ = preampTarget_;
        eq_.reset();
        limiter_.reset();
        crossfeed_.reset();
    }

    /** Kell-e kattanásmentes (le-fel úsztatott) váltás a két beállítás között. */
    static bool needsDeclick(const ParamData& a, const ParamData& b) {
        const uint32_t mods = kFlagEq | kFlagBass | kFlagMono | kFlagCrossfeed | kFlagLimiter;
        if ((a.flags & mods) != (b.flags & mods)) return true;
        if (magicActive(a) != magicActive(b)) return true;
        if (b.has(kFlagMono) && a.subsonicHz != b.subsonicHz) return true;
        if (b.has(kFlagEq) && (a.eqMode != b.eqMode || a.eqDecramp != b.eqDecramp)) return true;
        if (b.has(kFlagLimiter) && (a.limOversampling != b.limOversampling || a.limLookaheadMs != b.limLookaheadMs))
            return true;
        return false;
    }

    /** Váltás a függő beállításra (néma pillanatban, a leúsztatás végén). */
    void switchToPending() {
        const ParamData old = current_;
        current_ = pending_;
        applyParamsInternal(current_);
        preampCur_ = preampTarget_;
        // A most bekapcsolt modul ne a régi (elavult) belső állapotából induljon
        eqWasReset_ = current_.has(kFlagEq) &&
                      (!old.has(kFlagEq) || old.eqMode != current_.eqMode || old.eqDecramp != current_.eqDecramp);
        if (eqWasReset_) eq_.reset();
        if (current_.has(kFlagLimiter)) limiter_.reset();
        if (current_.has(kFlagBass) && !old.has(kFlagBass)) bass_.reset();
        if (current_.has(kFlagMono) && !old.has(kFlagMono)) monoBass_.reset();
        if (current_.has(kFlagCrossfeed) && !old.has(kFlagCrossfeed)) crossfeed_.reset();
        if (magicActive(current_) && !magicActive(old)) magic_.reset();
    }

    /**
     * Váltás után ennyi mintáig néma a kimenet: csak az újraindított (üres késleltető vonalú) modulok
     * késleltetése számít. A változatlanul futó EQ (pl. FIR módban, más modul kapcsolásakor) folytatja
     * a jelet, ott nem kell kivárni a késleltetését. Az EQ a pontos értéket a feldolgozás után adja meg.
     */
    size_t chainLatency() const {
        return (eqWasReset_ && current_.has(kFlagEq) ? eq_.latency() : 0) +
               (current_.has(kFlagLimiter) ? limiter_.latency() : 0);
    }

    /**
     * Le-, illetve felúsztatás a kimeneten (a feldolgozott blokk után). Az erősítés fadePos_ / fadeLen_.
     * A leúsztatás a blokk végére igazodik (előtte a szint változatlan), így a váltás pontosan a blokkhatáron
     * történik, és nem marad néma blokkmaradék; csak az újraindított modulok késleltetéséig néma a kimenet.
     */
    void applyFade(float* L, float* R, size_t m) {
        const float inv = 1.0f / static_cast<float>(fadeLen_);
        if (fade_ == Fade::Out) {
            const size_t hold = fadePos_ < m ? m - fadePos_ : 0;   // a hátralévő leúsztatás a blokk végére esik
            const float g0 = static_cast<float>(fadePos_) * inv;
            size_t i = 0;
            for (; i < hold; ++i) { L[i] *= g0; R[i] *= g0; }
            for (; i < m && fadePos_ > 0; ++i) {
                --fadePos_;
                const float g = static_cast<float>(fadePos_) * inv;
                L[i] *= g;
                R[i] *= g;
            }
            if (fadePos_ == 0) {
                for (; i < m; ++i) { L[i] = 0.0f; R[i] = 0.0f; }
                switchToPending();
                fade_ = Fade::In;
                sinceSwitch_ = 0;
            }
            return;
        }
        // Fade::In: az újraindított modulok késleltetésének kiteltéig csend (üres késleltető vonal), utána felúsztatás
        const size_t lat = chainLatency();
        for (size_t i = 0; i < m; ++i, ++sinceSwitch_) {
            if (sinceSwitch_ < lat) { L[i] = 0.0f; R[i] = 0.0f; continue; }
            if (++fadePos_ >= fadeLen_) { fadePos_ = fadeLen_; fade_ = Fade::None; break; }
            const float g = static_cast<float>(fadePos_) * inv;
            L[i] *= g;
            R[i] *= g;
        }
    }

    void applyParamsInternal(const ParamData& d) {
        // Az előerősítés az EQ része (EasyEffects: Equalizer "inputGain"): kikapcsolt EQ-nál nem hat
        preampTarget_ = d.has(kFlagEq) ? ee::dbToGain(d.preampDb) : 1.0f;
        preampStep_ = (preampTarget_ - preampCur_) / fadeSamples_;
        magic_.setControl(d.magicAmount, d.magicCompare != 0, d.has(kFlagCrossfeed), d.magicCharacter, d.magicContent);
        eq_.update(d);
        bass_.update(d);
        monoBass_.update(d.subsonicHz);
        crossfeed_.update(d);
        limiter_.update(d);
        convIn_ = ee::dbToGain(d.convInputGainDb);
        convOut_ = ee::dbToGain(d.convOutputGainDb);
        convDry_ = d.convDryDb <= -100.0f ? 0.0f : ee::dbToGain(d.convDryDb);
        convWet_ = d.convWetDb <= -100.0f ? 0.0f : ee::dbToGain(d.convWetDb);
    }

    /** EasyEffects Convolver::process egy példányra (ready ág). */
    void convolverPath(ee::ZitaInstance* inst, float* L, float* R, size_t m) {
        if (!inst || inst->quantum() != m || inst->failed()) { // nincs kész példány: !ready ág
            for (size_t i = 0; i < m; ++i) { L[i] *= convOut_; R[i] *= convOut_; }
            return;
        }
        if (convIn_ != 1.0f)
            for (size_t i = 0; i < m; ++i) { L[i] *= convIn_; R[i] *= convIn_; }
        std::memcpy(dryL_.data(), L, m * sizeof(float));
        std::memcpy(dryR_.data(), R, m * sizeof(float));
        inst->process(L, R, m); // sikertelen hívásnál a puffer a bemenet marad (EasyEffects-ben is)
        for (size_t i = 0; i < m; ++i) {
            L[i] = (convWet_ * L[i]) + (convDry_ * dryL_[i]);
            R[i] = (convWet_ * R[i]) + (convDry_ * dryR_[i]);
        }
        if (convOut_ != 1.0f)
            for (size_t i = 0; i < m; ++i) { L[i] *= convOut_; R[i] *= convOut_; }
    }

    /** Konvolver fokozat: ha van új példány, egy blokknyi keresztúsztatással cserél. */
    void runConvolver(float* L, float* R, size_t m) {
        Stage& st = irStage_;
        Update* u = nullptr;
        if (st.retired.load(std::memory_order_acquire) == nullptr) {
            u = st.pending.exchange(nullptr, std::memory_order_acq_rel);
        }
        if (!u) {
            if (st.active) convolverPath(st.active, L, R, m);
            return;
        }
        ee::ZitaInstance* oldInst = st.active;
        ee::ZitaInstance* newInst = u->inst;
        std::memcpy(aL_.data(), L, m * sizeof(float));
        std::memcpy(aR_.data(), R, m * sizeof(float));
        std::memcpy(bL_.data(), L, m * sizeof(float));
        std::memcpy(bR_.data(), R, m * sizeof(float));
        if (oldInst) convolverPath(oldInst, aL_.data(), aR_.data(), m);
        if (newInst) convolverPath(newInst, bL_.data(), bR_.data(), m);
        const float inv = 1.0f / static_cast<float>(m);
        for (size_t i = 0; i < m; ++i) {
            const float t = (static_cast<float>(i) + 1.0f) * inv;
            L[i] = aL_[i] + t * (bL_[i] - aL_[i]);
            R[i] = aR_[i] + t * (bR_[i] - aR_[i]);
        }
        st.active = newInst;
        u->old = oldInst;
        st.retired.store(u, std::memory_order_release);
    }

    void processChunk(float* L, float* R, size_t m) {
        quantum_.store(static_cast<uint32_t>(m), std::memory_order_relaxed);
        const ParamData& d = current_;

        if (magicActive(d)) magic_.process(L, R, m);

        if (preampCur_ != preampTarget_) {
            // Előerősítés-változás 5 ms alatt (nincs lépcső / kattanás)
            for (size_t i = 0; i < m; ++i) {
                if (preampCur_ != preampTarget_) {
                    preampCur_ += preampStep_;
                    if ((preampStep_ > 0.0f && preampCur_ >= preampTarget_) || (preampStep_ <= 0.0f && preampCur_ <= preampTarget_))
                        preampCur_ = preampTarget_;
                }
                L[i] *= preampCur_;
                R[i] *= preampCur_;
            }
        } else if (preampCur_ != 1.0f) {
            for (size_t i = 0; i < m; ++i) { L[i] *= preampCur_; R[i] *= preampCur_; }
        }

        if (d.has(kFlagEq)) eq_.process(L, R, m);
        if (d.has(kFlagBass)) bass_.process(L, R, m);
        if (d.has(kFlagMono)) monoBass_.process(L, R, m);
        if (d.has(kFlagCrossfeed)) crossfeed_.process(L, R, m);
        runConvolver(L, R, m);

        if (d.has(kFlagLimiter)) {
            limiter_.process(L, R, m);
        } else {
            // Bit-pontosan érintetlen [-1.0, 1.0] között; csak a túlcsorduló csúcsokat vágjuk,
            // megelőzve az ALSA integer túlcsordulást / négyszögesítést
            for (size_t i = 0; i < m; ++i) {
                if (L[i] > 1.0f) L[i] = 1.0f; else if (L[i] < -1.0f) L[i] = -1.0f;
                if (R[i] > 1.0f) R[i] = 1.0f; else if (R[i] < -1.0f) R[i] = -1.0f;
            }
        }

        if (fade_ != Fade::None) applyFade(L, R, m);
    }

    // ------------------------------------------------------------ háttérszál
    struct IrKey {
        uint64_t version = 0;
        uint32_t sr = 0;
        int32_t width = -1;
        int32_t autogain = -1;
        bool on = false;
        bool operator==(const IrKey& o) const {
            return version == o.version && sr == o.sr && width == o.width && autogain == o.autogain && on == o.on;
        }
    };

    void publish(ee::ZitaInstance* inst) {
        Update* u = new Update{inst, nullptr};
        if (Update* stale = irStage_.pending.exchange(u, std::memory_order_acq_rel)) {
            delete stale->inst; // a valós idejű szál még nem vette át: eldobható
            delete stale;
        }
    }

    void freeRetired() {
        if (Update* r = irStage_.retired.exchange(nullptr, std::memory_order_acq_rel)) {
            delete r->old;
            delete r;
        }
    }

    void workerLoop() {
        IrKey lastIr;
        ee::ConvKernel kernel;
        uint32_t builtQuantum = 0;
        bool published = false;
        const ee::ZitaInstance* publishedInst = nullptr;
        while (workerRunning_) {
            workerIdle_ = false;
            if (tick_) tick_();
            freeRetired();
            magic_.analyze();   // csak az új mintákat dolgozza fel (kikapcsolva nincs új minta)

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
            const uint32_t sr = sampleRate_.load();
            const uint32_t quantum = quantum_.load(std::memory_order_relaxed);

            IrKey want;
            IrData ir;
            uint64_t irVer = 0;
            {
                std::lock_guard<std::mutex> lk(irMutex_);
                irVer = irVersion_.load();
                want.on = d.has(kFlagConvolver) && !ir_.empty();
                if (want.on) {
                    want.version = irVer;
                    want.sr = sr;
                    want.width = d.convIrWidth;
                    want.autogain = d.convAutogain;
                    if (!(want == lastIr)) ir = ir_;
                }
            }
            if (want.on) {
                bool rebuild = false;
                if (!(want == lastIr)) {
                    kernel = ee::prepareConvKernel(ir.left, ir.right, ir.sampleRate, sr, want.width, want.autogain != 0);
                    lastIr = want;
                    rebuild = true;
                }
                // A hoszt blokkmérete változott, vagy a példány túlterhelés miatt leállt: új példány
                if (quantum != 0 && quantum != builtQuantum) rebuild = true;
                if (published && publishedInst && publishedInst->failed()) rebuild = true;
                if (rebuild && kernel.valid() && quantum != 0) {
                    auto* inst = new ee::ZitaInstance();
                    if (inst->init(kernel, quantum)) {
                        publish(inst);
                        publishedInst = inst;
                        published = true;
                        builtQuantum = quantum;
                    } else {
                        delete inst;
                        builtQuantum = quantum; // ugyanarra a méretre nem próbálkozunk újra
                    }
                }
            } else if (published) {
                publish(nullptr);
                published = false;
                publishedInst = nullptr;
                lastIr = IrKey{};
                kernel = ee::ConvKernel{};
                builtQuantum = 0;
            }

            workerSeenSeq_ = haveParams ? seq : seqNow;
            workerSeenIr_ = irVer;
            workerSeenQuantum_ = quantum;
            workerIdle_ = true; // minden ismert változás feldolgozva (az átvétel a valós idejű szálon történik)
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }

    // valós idejű állapot
    magic::MagicTouch magic_;
    ee::Equalizer eq_;
    ee::BassEnhancer bass_;
    mono::MonoBass monoBass_;
    ee::Crossfeed crossfeed_;
    ee::Limiter limiter_;
    ParamData current_;
    ParamData pending_;                 // le-fel úsztatott váltásnál a leúsztatás végén lép életbe
    enum class Fade { None, Out, In };
    Fade fade_ = Fade::None;
    float fadeSamples_ = 240.0f;        // 5 ms (a mintavételi frekvenciából számolva)
    size_t fadeLen_ = 240, fadePos_ = 240;   // úsztatás hossza mintában; pillanatnyi erősítés = fadePos_ / fadeLen_
    size_t sinceSwitch_ = 0;
    bool eqWasReset_ = false;           // a legutóbbi váltásnál újraindult-e az EQ (üres késleltető vonal)
    float preampCur_ = 1.0f, preampTarget_ = 1.0f, preampStep_ = 0.0f;
    float convIn_ = 1.0f, convOut_ = 1.0f, convDry_ = 0.0f, convWet_ = 1.0f;
    std::vector<float> aL_, aR_, bL_, bR_, dryL_, dryR_;
    Stage irStage_;

    // megosztott
    std::atomic<uint32_t> sampleRate_{48000};
    std::atomic<uint32_t> quantum_{0};
    ParamBlock workerParams_;

    mutable std::mutex irMutex_;
    IrData ir_;
    std::atomic<uint64_t> irVersion_{0};

    std::atomic<bool> workerRunning_{false};
    std::atomic<bool> workerIdle_{true};
    std::atomic<uint32_t> workerSeenSeq_{0};
    std::atomic<uint64_t> workerSeenIr_{0};
    std::atomic<uint32_t> workerSeenQuantum_{0};
    std::function<void()> tick_;
    std::thread worker_;
};

} // namespace ee_analyzer
