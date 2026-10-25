#pragma once
/**
 * @file autogain.hpp
 * @brief Autogain: automatikus szintkiegyenlítés az EBU R128 rövid távú hangosságára (libebur128, MIT).
 *
 * A modul a láncban a konvolver után, a limiter előtt fut. A bemenet 3 s-os rövid távú hangosságát méri
 * (ITU-R BS.1770 K-súlyozás, EBU R128), és a kimenetet a célszintre igazítja, legfeljebb ±maxGain dB-lel.
 * A gain legfeljebb 1 dB/s sebességgel változik (nincs pumpálás). Csend (70 LUFS alatt) és a bekapcsolás
 * utáni első 3 s alatt a gain tartja az értékét: csendet és a régi, már nem releváns mérést nem erősít.
 *
 * Kikapcsolt modulnál (a láncban nem hívjuk) és 0 dB-es gainnél a kimenet bit-pontosan a bemenet.
 */
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ebur128.h"
#include "param_block.hpp"

namespace ee_analyzer {
namespace agc {

constexpr double kGateLufs = -70.0;        // csend-küszöb: e alatt a gain nem változik
constexpr double kWindowSec = 3.0;         // a libebur128 rövid távú mérési ablaka (EBU R128)
constexpr double kMaxRateDbPerSec = 1.0;   // a gain legnagyobb változási sebessége
constexpr size_t kMaxBlock = 8192;         // egy belső blokk (a lánc kChunk értékével azonos)

inline double dbToLin(double db) { return std::pow(10.0, db / 20.0); }

class Autogain {
public:
    Autogain() : buf_(2 * kMaxBlock, 0.0f) {}
    ~Autogain() { destroyMeter(); }

    Autogain(const Autogain&) = delete;
    Autogain& operator=(const Autogain&) = delete;

    /** Mintavételi frekvencia (nem valós idejű; a lánc hívja a feldolgozás leállítása mellett). */
    void setSampleRate(uint32_t sr) {
        destroyMeter();
        sr_ = sr > 0 ? sr : 48000;
        meter_ = ebur128_init(2, static_cast<unsigned long>(sr_), EBUR128_MODE_S);
        reset();
    }

    /** A valós idejű szálon, a beállítás életbe lépésekor. */
    void update(const ParamData& d) {
        target_ = static_cast<double>(d.agcTargetLufs);
        maxDb_ = std::max(0.0, static_cast<double>(d.agcMaxGainDb));
    }

    /**
     * Újraindításkor (bekapcsoláskor): a gain 0 dB-ről indul, és a mérés csak akkor szól bele, ha a
     * bekapcsolás óta legalább egy teljes mérési ablak (3 s) befutott, így a régi tartalom nem számít.
     * A libebur128 állapotát nem építjük újra (foglalás nélkül, a valós idejű szálon is biztonságos).
     */
    void reset() {
        gainDb_ = 0.0;
        appliedDb_ = 0.0;
        fed_ = 0;
        pending_ = 0;
    }

    /** Csak bekapcsolt modulnál hívandó. A mérés a bemeneten fut, az erősítés a kimenetre kerül. */
    void process(float* L, float* R, size_t n) {
        if (!meter_) return;
        for (size_t off = 0; off < n; off += kMaxBlock) {
            const size_t m = std::min(kMaxBlock, n - off);
            processBlock(L + off, R + off, m);
        }
    }

    /** Az aktuális erősítés dB-ben (diagnosztika, pozitív = erősítés). */
    double gainDb() const { return gainDb_; }

private:
    void processBlock(float* L, float* R, size_t m) {
        for (size_t i = 0; i < m; ++i) {
            buf_[2 * i] = L[i];
            buf_[2 * i + 1] = R[i];
        }
        ebur128_add_frames_float(meter_, buf_.data(), m);
        const double startDb = appliedDb_;
        if (fed_ < windowFrames()) {
            // Az ablak még nem telt meg: a mérés nem szól bele, a számlálót a döntésig nullán tartjuk
            fed_ += m;
            pending_ = 0;
        } else {
            pending_ += m;
            if (pending_ >= measureFrames()) {
                decide(static_cast<double>(pending_) / sr_);
                pending_ = 0;
            }
        }
        appliedDb_ = gainDb_;
        if (startDb == 0.0 && appliedDb_ == 0.0) return;   // 0 dB: bit-pontosan átenged
        const double lin0 = dbToLin(startDb);
        const double lin1 = dbToLin(appliedDb_);
        const double inv = 1.0 / static_cast<double>(m);
        for (size_t i = 0; i < m; ++i) {
            const double g = lin0 + (lin1 - lin0) * (static_cast<double>(i) + 1.0) * inv;
            L[i] = static_cast<float>(L[i] * g);
            R[i] = static_cast<float>(R[i] * g);
        }
    }

    /** Új gain a mért rövid távú hangosság alapján, a sebességkorláttal (elapsedSec másodperc telt el). */
    void decide(double elapsedSec) {
        double loud = 0.0;
        if (ebur128_loudness_shortterm(meter_, &loud) != EBUR128_SUCCESS) return;
        if (!(loud > kGateLufs)) return;   // csend (-inf) vagy hibás érték: a gain tartja az értékét
        const double want = std::clamp(target_ - loud, -maxDb_, maxDb_);
        const double step = kMaxRateDbPerSec * elapsedSec;
        gainDb_ += std::clamp(want - gainDb_, -step, step);
    }

    size_t windowFrames() const { return static_cast<size_t>(kWindowSec * sr_); }
    size_t measureFrames() const { return std::max<size_t>(1, static_cast<size_t>(0.1 * sr_)); }

    void destroyMeter() {
        if (meter_) ebur128_destroy(&meter_);
        meter_ = nullptr;
    }

    ebur128_state* meter_ = nullptr;
    uint32_t sr_ = 48000;
    double target_ = -18.0, maxDb_ = 6.0;
    double gainDb_ = 0.0, appliedDb_ = 0.0;
    size_t fed_ = 0, pending_ = 0;
    std::vector<float> buf_;
};

} // namespace agc
} // namespace ee_analyzer
