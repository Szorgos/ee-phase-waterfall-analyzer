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
#include "src_fft64.hpp"

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
     * Célmintavétel kiválasztása a 3.2 szabályok szerint, csak támogatott rátára.
     * Felfelé a szabály dönt (192 kHz-es forrásig). allowDown (a lejátszóban: downsampleEngine ≥ 0): ha a kimenet a
     * forrás mintavételét nem tudja (vagy a felső korlát fölött van), pl. 192 kHz-es fájl 96 kHz-es DAC-on, lefelé
     * alakít a választott motorral – a felfelé szabálytól függetlenül; az egyéni táblázat lefelé is megadhat célt.
     * allowDown nélkül lefelé soha (ilyenkor a rendszer keverője alakít).
     */
    static uint32_t selectTargetRate(uint32_t srcRate,
                                     audioopt::UpsampleRule rule,
                                     const std::vector<uint32_t>& supported,
                                     const std::map<uint32_t, uint32_t>& customMap,
                                     bool bypassUpsample,
                                     bool isDsd,
                                     std::string* reasonOut = nullptr,
                                     uint32_t maxRate = 0,
                                     bool allowDown = false) {
        auto setReason = [&](const char* r) { if (reasonOut) *reasonOut = r; };
        if (bypassUpsample) { setReason("bypass"); return srcRate; }
        if (isDsd) { setReason("dsd"); return srcRate; }
        if (rule == audioopt::UpsampleRule::Off && !allowDown) { setReason("off"); return srcRate; }

        // A felső korlát (a terhelés-tanácsadó állítja) a tudott mintavételek közül kizárja a nagyobbakat
        auto isSupported = [&](uint32_t r) {
            return std::find(supported.begin(), supported.end(), r) != supported.end() && (maxRate == 0 || r <= maxRate);
        };

        uint32_t up = srcRate;
        if (srcRate <= 192000) {
            if (rule == audioopt::UpsampleRule::Double) {
                if (isSupported(srcRate * 2)) up = srcRate * 2;
            } else if (rule == audioopt::UpsampleRule::PowerOfTwo) {
                for (uint32_t mult = 2; mult <= 16; mult *= 2)
                    if (isSupported(srcRate * mult)) up = srcRate * mult;
            } else if (rule == audioopt::UpsampleRule::MaxOutput) {
                for (uint32_t r : supported)
                    if (isSupported(r) && r > up) up = r;
            } else if (rule == audioopt::UpsampleRule::Custom) {
                auto it = customMap.find(srcRate);
                if (it != customMap.end() && it->second > srcRate && isSupported(it->second)) up = it->second;
            }
        }
        if (up > srcRate) { setReason(""); return up; }

        if (allowDown) {
            // Egyéni táblázat: lefelé választott cél (pl. 192 → 96 kHz)
            if (rule == audioopt::UpsampleRule::Custom) {
                auto it = customMap.find(srcRate);
                if (it != customMap.end() && it->second < srcRate && isSupported(it->second)) { setReason(""); return it->second; }
            }
            if (!isSupported(srcRate)) {
                const uint32_t down = selectDownRate(srcRate, rule, supported, isSupported);
                if (down > 0) { setReason(""); return down; }
            }
        }
        setReason(rule == audioopt::UpsampleRule::Off ? "off" : srcRate > 192000 ? "source_rate" : "dac_unsupported");
        return srcRate;
    }

    /**
     * Lefelé alakítás célja, ha a kimenet a forrást nem tudja (0: nincs kisebb támogatott ráta). „Kimenet maximuma”:
     * a legnagyobb tudott ráta; a többi szabály (és a kikapcsolt felfelé alakítás) az egész arányú (azonos családú, pl. 176,4 → 88,2 kHz) rátát
     * választja előbb, és csak ha ilyen nincs, a legnagyobb kisebbet.
     */
    template <class Pred>
    static uint32_t selectDownRate(uint32_t srcRate, audioopt::UpsampleRule rule, const std::vector<uint32_t>& supported,
                                   Pred isSupported) {
        uint32_t best = 0, family = 0;
        for (uint32_t r : supported) {
            if (r >= srcRate || !isSupported(r)) continue;
            best = std::max(best, r);
            if (srcRate % r == 0) family = std::max(family, r);
        }
        if (rule == audioopt::UpsampleRule::MaxOutput) return best;
        return family > 0 ? family : best;
    }

    bool init(uint32_t srcRate, uint32_t dstRate, uint32_t channels = 2,
              int precision = 28, double phase = 25.0,
              double passbandEnd = 0.95, double stopbandBegin = 1.00,
              float headroomDb = -1.0f, int engineType = 0) {
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

        if (engineType == static_cast<int>(audioopt::UpsampleEngineType::Fft64) ||
            engineType == static_cast<int>(audioopt::UpsampleEngineType::FftDD)) {
            const bool isDD = (engineType == static_cast<int>(audioopt::UpsampleEngineType::FftDD));
            const double beta = isDD ? 35.2 : 34.5;
            if (fftStream_.init(srcRate, dstRate, channels, passbandEnd, beta, phase, isDD)) {
                useFft64_ = true;
                active_ = true;
                return true;
            }
            // Nem racionális / túl nagy aránynál biztonságos visszalépés libsoxr-re
        }

        soxr_quality_spec_t q_spec = soxr_quality_spec(SOXR_VHQ, 0);
        q_spec.precision = static_cast<double>(precision);
        q_spec.phase_response = std::clamp(phase, 0.0, 50.0);
        q_spec.passband_end = passbandEnd;
        // Felmintavételezésnél (io_ratio < 1) a libsoxr megköveteli: stopband_begin - 1 <= 1 - passband_end
        // ("imaging greater than rolloff"), különben soxr_create() hibával visszatér.
        const double maxSoxrStopband = (dstRate > srcRate) ? std::max(1.0, 2.0 - passbandEnd) : 1.50;
        q_spec.stopband_begin = std::clamp(stopbandBegin, passbandEnd + 0.003, maxSoxrStopband);

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
                    opts.upsampleHeadroomDb,
                    opts.upsampleEngine);
    }

    /**
     * „Szűrő a DAC rátáján”: a DAC a forrásnál nagyobb mintavételt nem tud, de a felhasználó a választott szűrő
     * karakterét kéri. Két lépés: (1) srcRate → 2·srcRate a felhasználó saját beállításaival (motor, fázis, áteresztő
     * sáv, pontosság, fejtér) – pontosan, amit az initWithOptions egy rendes duplázásnál csinálna; (2) 2·srcRate →
     * srcRate egy rögzített, átlátszó lépéssel (lásd kFilterOnlyDown*). A kimenet mintavétele srcRate marad.
     */
    bool initFilterOnly(uint32_t srcRate, uint32_t channels, const audioopt::AudioOptions& opts) {
        if (srcRate == 0 || !initWithOptions(srcRate, srcRate * 2, channels, opts) || !active_) {
            reset();
            return false;
        }
        // A második lépés: libsoxr, lineáris fázis, 28 bites pontosság (a 32 bites lebegőpontos minták 24 bites
        // mantisszájánál nagyobb), az áteresztő sáv vége a DAC Nyquist-frekvenciájának 99 %-a (44,1 kHz-nél
        // 21,83 kHz), a zárósáv kezdete pontosan a Nyquist-frekvencia (1,0: a Nyquist fölötti rész nem tükröződik
        // vissza), fejtér nélkül. Indok: a hangkaraktert az első (a felhasználó) szűrője adja; ez a lépés csak azt vágja
        // le, ami a DAC Nyquist-frekvenciája fölött van; a 99 % a felhasználói szűrők szokásos áteresztő sávja (95 %)
        // fölött van. Szűkebb átmenetnél / nagyobb pontosságnál a libsoxr sokkal hosszabb blokkokkal dolgozik; mérve
        // (88,2 -> 44,1 kHz, 4096-os adagok, átlagos soxr_delay): 28 bit / 99 %: kb. 93 ms; 32 bit / 99 %: kb. 185 ms;
        // 32 bit / 99,5 %: kb. 390 ms. A lejátszó a késleltetést kiegyenlíti, de a hosszabb blokk fölösleges terhelés.
        soxr_quality_spec_t q = soxr_quality_spec(SOXR_VHQ, 0);   // az első lépés libsoxr-ágával azonos alap
        q.precision = kFilterOnlyDownPrecision;
        q.phase_response = 50.0;
        q.passband_end = kFilterOnlyDownPassband;
        q.stopband_begin = 1.0;
        soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
        soxr_runtime_spec_t rt = soxr_runtime_spec(1);
        soxr_error_t err = nullptr;
        down_ = soxr_create(static_cast<double>(srcRate) * 2.0, static_cast<double>(srcRate), channels, &err, &io, &q, &rt);
        if (err != nullptr || down_ == nullptr) {
            reset();
            return false;
        }
        downDrained_ = false;
        return true;
    }
    static constexpr double kFilterOnlyDownPrecision = 28.0;   // bit
    static constexpr double kFilterOnlyDownPassband = 0.99;    // a kimeneti Nyquist-frekvencia arányában

    void reset() {
        if (soxr_) {
            soxr_delete(soxr_);
            soxr_ = nullptr;
        }
        if (soxrR_) {
            soxr_delete(soxrR_);
            soxrR_ = nullptr;
        }
        if (down_) {
            soxr_delete(down_);
            down_ = nullptr;
        }
        downDrained_ = false;
        fftStream_.reset();
        useFft64_ = false;
        split_ = false;
        active_ = false;
        srcRate_ = 0;
        dstRate_ = 0;
    }

    void clear() {
        if (useFft64_) fftStream_.clear();
        if (soxr_) soxr_clear(soxr_);
        if (soxrR_) soxr_clear(soxrR_);
        if (down_) soxr_clear(down_);
        downDrained_ = false;
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
        if (!down_) return processMain(in, inFrames, out, outCapacityFrames, outFrames, true);
        // Szűrő a DAC rátáján: az első lépés (felhasználói szűrő) a köztes pufferbe, onnan a második lépés a kimenetre
        if (in && inFrames > 0) downDrained_ = false;
        const size_t midCap = inFrames * 2 + 4096;
        if (mid_.size() < midCap * channels_) mid_.resize(midCap * channels_);
        size_t midFrames = 0;
        bool ok = processMain(in, inFrames, mid_.data(), midCap, midFrames, false);
        ok = runDown(mid_.data(), midFrames, out, outCapacityFrames, outFrames) && ok;
        return ok;
    }

    /**
     * Belső szűrő kiürítése (a forrásfolyam végén).
     */
    bool flush(float* out, size_t outCapacityFrames, size_t& outFrames) {
        if (!down_) return flushMain(out, outCapacityFrames, outFrames, true);
        outFrames = 0;
        if (!active_) return true;
        bool ok = true;
        // Előbb az első lépés maradékát visszük át a második lépésen; amíg az ad mintát, a hívó újra kér
        while (!downDrained_) {
            const size_t midCap = outCapacityFrames * 2 + 4096;
            if (mid_.size() < midCap * channels_) mid_.resize(midCap * channels_);
            size_t midFrames = 0;
            ok = flushMain(mid_.data(), midCap, midFrames, false) && ok;
            if (midFrames == 0) { downDrained_ = true; break; }
            ok = runDown(mid_.data(), midFrames, out, outCapacityFrames, outFrames) && ok;
            if (outFrames > 0) return ok;
        }
        // Utána a második lépés saját maradéka
        size_t odone = 0;
        const soxr_error_t err = soxr_process(down_, nullptr, 0, nullptr, out, outCapacityFrames, &odone);
        outFrames = odone;
        finishOutput(out, odone);
        return ok && err == nullptr;
    }

    /**
     * Késleltetés kimeneti keretekben (a belső szűrő állapota szerint). Szűrő a DAC rátáján: a két lépésé együtt
     * (az első lépésé a köztes mintavételről a kimenetire átszámolva).
     */
    double delayFrames() const {
        double d = mainDelayFrames();
        if (down_) d = d * static_cast<double>(srcRate_) / static_cast<double>(dstRate_) + soxr_delay(down_);
        return d;
    }

    bool isActive() const { return active_; }
    bool isFft64() const { return useFft64_; }   // a saját FFT-ág (64 vagy 106 bites)
    /** Szűrő a DAC rátáján (két lépés, a kimenet mintavétele = srcRate). */
    bool isFilterOnly() const { return down_ != nullptr; }
    /** A ténylegesen futó motor: 0 = libsoxr, 1 = Aqulify FFT64, 2 = Aqulify FFT-DD (audioopt::UpsampleEngineType). */
    int engineType() const { return useFft64_ ? (fftStream_.isDD() ? 2 : 1) : 0; }
    uint32_t srcRate() const { return srcRate_; }
    /** A felhasználói szűrő kimeneti mintavétele (szűrő a DAC rátáján: a köztes 2·srcRate). */
    uint32_t dstRate() const { return dstRate_; }
    uint32_t channels() const { return channels_; }
    float headroomDb() const { return headroomDb_; }
    float headroomGain() const { return headroomGain_; }
    /** Túlvezérelt kimeneti minták (szűrő a DAC rátáján: a második lépés után, a DAC-ra menő jelben). */
    uint64_t clippedSamples() const { return clippedSamples_; }
    /** A kimenetre adott keretek (szűrő a DAC rátáján: a második lépés után). */
    uint64_t processedFrames() const { return processedFrames_; }
    void resetClippedSamples() { clippedSamples_ = 0; }

private:
    /** A második lépés (2·srcRate → srcRate): a bemenetet mindig teljesen átveszi (idone nélkül a libsoxr nem
     *  korlátozza a kimeneti hely szerint); ami a kimenetre nem fér, a libsoxr-ben marad a következő hívásig. */
    bool runDown(const float* mid, size_t midFrames, float* out, size_t outCap, size_t& outFrames) {
        size_t odone = 0;
        // A bemenet mutatója üres adagnál sem nullptr: az a libsoxr-ben kiürítést (a folyam végét) jelentené
        const soxr_error_t err = soxr_process(down_, mid, midFrames, nullptr, out, outCap, &odone);
        outFrames = odone;
        finishOutput(out, odone);
        return err == nullptr;
    }

    /** A kimenetre kerülő minták számlálása (szűrő a DAC rátáján: a fejteret az első lépés már alkalmazta). */
    void finishOutput(const float* out, size_t frames) {
        processedFrames_ += frames;
        const size_t samples = frames * channels_;
        for (size_t i = 0; i < samples; ++i)
            if (out[i] > 1.0f || out[i] < -1.0f) ++clippedSamples_;
    }

    double mainDelayFrames() const {
        if (active_ && useFft64_) return fftStream_.delayFrames();
        if (active_ && soxr_) {
            return soxr_delay(soxr_);
        }
        return 0.0;
    }

    /** final: ez a kimenet a DAC-ra megy (keretszám és túlvezérlés számlálása); különben a köztes jel. */
    bool processMain(const float* in, size_t inFrames, float* out, size_t outCapacityFrames, size_t& outFrames, bool final) {
        if (!active_ || (!useFft64_ && soxr_ == nullptr)) {
            size_t toCopy = std::min(inFrames, outCapacityFrames);
            if (in && out && toCopy > 0) {
                std::memcpy(out, in, toCopy * channels_ * sizeof(float));
            }
            outFrames = toCopy;
            return true;
        }

        if (useFft64_) {
            size_t odone = 0;
            const bool ok = fftStream_.process(in, inFrames, out, outCapacityFrames, odone);
            outFrames = odone;
            if (final) processedFrames_ += odone;
            applyHeadroom(out, odone * channels_, final);
            return ok;
        }

        size_t odone = 0;
        soxr_error_t err = split_ ? processSplit(in, inFrames, out, outCapacityFrames, odone)
                                  : runOne(soxr_, in, inFrames, out, outCapacityFrames, odone);
        outFrames = odone;
        if (final) processedFrames_ += odone;
        applyHeadroom(out, odone * channels_, final);
        return (err == nullptr);
    }

    bool flushMain(float* out, size_t outCapacityFrames, size_t& outFrames, bool final) {
        if (!active_ || (!useFft64_ && soxr_ == nullptr)) {
            outFrames = 0;
            return true;
        }
        if (useFft64_) {
            size_t odone = 0;
            const bool ok = fftStream_.flush(out, outCapacityFrames, odone);
            outFrames = odone;
            if (final) processedFrames_ += odone;
            applyHeadroom(out, odone * channels_, final);
            return ok;
        }
        size_t odone = 0;
        soxr_error_t err = split_ ? processSplit(nullptr, 0, out, outCapacityFrames, odone)
                                  : runOne(soxr_, nullptr, 0, out, outCapacityFrames, odone);
        outFrames = odone;
        if (final) processedFrames_ += odone;
        applyHeadroom(out, odone * channels_, final);
        return (err == nullptr);
    }

    static soxr_error_t runOne(soxr_t s, const float* in, size_t inFrames, float* out, size_t outCap, size_t& odone) {
        size_t idone = 0;
        odone = 0;
        return soxr_process(s, in, inFrames, &idone, out, outCap, &odone);
    }

    /** countClips: a túlvezérelt minták számlálása (csak a DAC-ra menő jelben). */
    void applyHeadroom(float* out, size_t samples, bool countClips) {
        for (size_t i = 0; i < samples; ++i) {
            const float v = out[i] * headroomGain_;
            if (countClips && (v > 1.0f || v < -1.0f)) ++clippedSamples_;
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
    soxr_t down_ = nullptr;          // szűrő a DAC rátáján: a második (2·srcRate → srcRate) lépés
    bool downDrained_ = false;       // kiürítéskor az első lépés már mindent átadott a másodiknak
    std::vector<float> mid_;         // a két lépés közötti (2·srcRate) minták
    srcconv::fft64::StreamResampler fftStream_;
    bool useFft64_ = false;
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
