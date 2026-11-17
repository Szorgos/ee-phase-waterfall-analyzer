#pragma once
/**
 * @file multi_output.hpp
 * @brief Több kimenet (2–4 hangkártya): független zónák vagy aktív keresztváltó. Platformfüggetlen DSP rész.
 *
 * Két üzemmód:
 *   - Független zónák (Zones): minden kimenet a teljes bemenetet kapja, és saját, teljes DSP-lánca van (pl. fejhallgató
 *     AutoEq-profillal, hangfal szobakorrekcióval). A kimenetek egyszerre szólnak.
 *   - Aktív keresztváltó (Crossover): egy közös lánc (az 1. kimenet beállításai), utána Linkwitz–Riley frekvenciaváltó
 *     1–3 vágási ponttal (2–4 sáv). Minden kimenet egy sávot (vagy a teljes jelet) kap.
 *
 * Minden kimenetnek saját erősítése, késleltetése (a hangszórók távolságához) és polaritása van.
 *
 * Linkwitz–Riley szűrők (a sávok összege minden esetben amplitúdóban egyenes, csak a fázis fordul):
 *   - 12 dB/okt (LR2): két elsőrendű Butterworth; a felső ág polaritását a váltó maga fordítja, így az összeg egyenes.
 *   - 24 dB/okt (LR4): két másodrendű Butterworth (Q = 0,7071).
 *   - 48 dB/okt (LR8): két negyedrendű Butterworth (Q = 0,5412 és 1,3066).
 * Több vágási pontnál az alsóbb sávok a felsőbb vágási pontok mindenáteresztő (LP ± HP) szűrőjét is megkapják, így az
 * összes sáv összege fázisban is egyezik.
 *
 * A valós idejű szálon nincs foglalás; a beállítás a lánchoz hasonlóan, a hangszálon lép életbe (applyConfigRT).
 * Az órajel-eltérés (két külön DAC) kezelése a platformrész feladata (Linuxon a PipeWire adaptív újramintavételezése).
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

#include "aether_chain.hpp"
#include "multi_output_config.hpp"
#include "param_block.hpp"

namespace ee_analyzer {
namespace multiout {

constexpr size_t kBlock = 1024;               // a belső feldolgozási darab (a hoszt bármekkora blokkot adhat)
constexpr double kPi = 3.14159265358979323846;

/**
 * Szálak közti átadás seqlockkal (a ParamBlock mintájára): az író nem valós idejű szál, az olvasó a hangszál,
 * amely sosem vár (ha éppen írás folyik, a következő körben olvas). T legyen egyszerűen másolható.
 */
template <class T>
struct SeqBox {
    static_assert(std::is_trivially_copyable<T>::value, "SeqBox: T legyen egyszerűen másolható");
    std::atomic<uint32_t> seq{ 0 };
    T data{};

    void write(const T& d) {
        const uint32_t s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(static_cast<void*>(&data), &d, sizeof(T));
        std::atomic_thread_fence(std::memory_order_release);
        seq.store(s + 2, std::memory_order_release);
    }
    bool tryRead(T& out, uint32_t& outSeq) const {
        const uint32_t s1 = seq.load(std::memory_order_acquire);
        if (s1 & 1u) return false;
        std::memcpy(static_cast<void*>(&out), static_cast<const void*>(&data), sizeof(T));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq.load(std::memory_order_relaxed) != s1) return false;
        outSeq = s1;
        return true;
    }
    uint32_t currentSeq() const { return seq.load(std::memory_order_acquire); }
};

/**
 * Sztereó, egy író – egy olvasó gyűrűpuffer (zármentes) a fő hangszál és a további kimenetek streamjei között.
 * Teli puffernél az író eldobja a többletet; az olvasó a hiányzó részt csenddel tölti (a hívó dönt).
 */
template <uint32_t kFramesPow2>
class StereoSpsc {
    static_assert((kFramesPow2 & (kFramesPow2 - 1)) == 0, "kettőhatvány méret kell");

public:
    uint32_t available() const {
        return static_cast<uint32_t>(w_.load(std::memory_order_acquire) - r_.load(std::memory_order_relaxed));
    }
    uint32_t capacity() const { return kFramesPow2; }

    /** @return a ténylegesen beírt keretek száma (teli puffernél kevesebb) */
    uint32_t write(const float* l, const float* r, uint32_t n) {
        const uint64_t w = w_.load(std::memory_order_relaxed);
        const uint64_t rd = r_.load(std::memory_order_acquire);
        const uint32_t space = kFramesPow2 - static_cast<uint32_t>(w - rd);
        n = std::min(n, space);
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t k = static_cast<uint32_t>((w + i) & (kFramesPow2 - 1));
            L_[k] = l[i];
            R_[k] = r[i];
        }
        w_.store(w + n, std::memory_order_release);
        return n;
    }
    /** @return a ténylegesen kiolvasott keretek száma */
    uint32_t read(float* l, float* r, uint32_t n) {
        const uint64_t rd = r_.load(std::memory_order_relaxed);
        const uint64_t w = w_.load(std::memory_order_acquire);
        n = std::min(n, static_cast<uint32_t>(w - rd));
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t k = static_cast<uint32_t>((rd + i) & (kFramesPow2 - 1));
            l[i] = L_[k];
            r[i] = R_[k];
        }
        r_.store(rd + n, std::memory_order_release);
        return n;
    }
    /** Az olvasó oldalon: a legrégebbi keretek eldobása (a késleltetés korlátozására). */
    void skip(uint32_t n) {
        const uint64_t rd = r_.load(std::memory_order_relaxed);
        const uint64_t w = w_.load(std::memory_order_acquire);
        r_.store(rd + std::min<uint64_t>(n, w - rd), std::memory_order_release);
    }

private:
    std::array<float, kFramesPow2> L_{}, R_{};
    std::atomic<uint64_t> w_{ 0 }, r_{ 0 };
};

/** Kétszeres pontosságú biquad (DF2T); elsőrendű szakaszhoz b2 = a2 = 0. */
struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    double process(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0.0; }
    void identity() { b0 = 1.0; b1 = b2 = a1 = a2 = 0.0; }

    /** Közös RBJ bilineáris szűrőtervező útonkénti EQ-hoz (Bell / LoShelf / HiShelf). */
    void eqBand(int32_t type, double freq, double gainDb, double q, double sr) {
        if (std::fabs(gainDb) < 1e-9 || sr <= 0.0) {
            identity();
            return;
        }
        const double nyq = std::max(25.0, sr * 0.49);
        const double f0 = std::clamp(freq, 20.0, std::min(20000.0, nyq));
        const double qClamped = std::clamp(q, 0.3, 10.0);
        const double gClamped = std::clamp(gainDb, -18.0, 6.0);
        const double w0 = 2.0 * kPi * f0 / sr;
        const double cw = std::cos(w0);
        const double sw = std::sin(w0);
        const double alpha = sw / (2.0 * qClamped);
        const double A = std::pow(10.0, gClamped / 40.0);
        double nb0 = 1.0, nb1 = 0.0, nb2 = 0.0, a0 = 1.0, na1 = 0.0, na2 = 0.0;
        if (type == static_cast<int32_t>(EqBandType::LoShelf)) {
            const double tsa = 2.0 * std::sqrt(A) * alpha;
            nb0 = A * ((A + 1.0) - (A - 1.0) * cw + tsa);
            nb1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
            nb2 = A * ((A + 1.0) - (A - 1.0) * cw - tsa);
            a0 = (A + 1.0) + (A - 1.0) * cw + tsa;
            na1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw);
            na2 = (A + 1.0) + (A - 1.0) * cw - tsa;
        } else if (type == static_cast<int32_t>(EqBandType::HiShelf)) {
            const double tsa = 2.0 * std::sqrt(A) * alpha;
            nb0 = A * ((A + 1.0) + (A - 1.0) * cw + tsa);
            nb1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
            nb2 = A * ((A + 1.0) + (A - 1.0) * cw - tsa);
            a0 = (A + 1.0) - (A - 1.0) * cw + tsa;
            na1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw);
            na2 = (A + 1.0) - (A - 1.0) * cw - tsa;
        } else {
            nb0 = 1.0 + alpha * A;
            nb1 = -2.0 * cw;
            nb2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A;
            na1 = -2.0 * cw;
            na2 = 1.0 - alpha / A;
        }
        b0 = nb0 / a0;
        b1 = nb1 / a0;
        b2 = nb2 / a0;
        a1 = na1 / a0;
        a2 = na2 / a0;
    }

    /** Komplex átvitel kiértékelése ugyanazokból az együtthatókból. */
    std::complex<double> eval(double f, double sr) const {
        const double w = 2.0 * kPi * f / sr;
        const std::complex<double> zm1 = std::polar(1.0, -w);
        const std::complex<double> zm2 = zm1 * zm1;
        return (b0 + b1 * zm1 + b2 * zm2) / (1.0 + a1 * zm1 + a2 * zm2);
    }

    void firstOrder(bool highpass, double fc, double sr) {
        const double k = std::tan(kPi * fc / sr);
        const double n = 1.0 / (1.0 + k);
        if (highpass) { b0 = n; b1 = -n; } else { b0 = k * n; b1 = k * n; }
        b2 = 0.0;
        a1 = (k - 1.0) * n;
        a2 = 0.0;
    }
    void secondOrder(bool highpass, double fc, double sr, double q) {
        const double w0 = 2.0 * kPi * fc / sr;
        const double cw = std::cos(w0), alpha = std::sin(w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        if (highpass) { b0 = (1.0 + cw) / (2.0 * a0); b1 = -(1.0 + cw) / a0; }
        else { b0 = (1.0 - cw) / (2.0 * a0); b1 = (1.0 - cw) / a0; }
        b2 = b0;
        a1 = -2.0 * cw / a0;
        a2 = (1.0 - alpha) / a0;
    }
};

/** Linkwitz–Riley alul- vagy felüláteresztő (12 / 24 / 48 dB/okt): legfeljebb 4 biquad sorba kötve. */
class LinkwitzRiley {
public:
    void design(bool highpass, double fc, double sr, int slopeDbPerOct) {
        n_ = 0;
        auto add = [&](auto&& fn) { fn(s_[n_]); s_[n_].reset(); ++n_; };
        switch (slopeDbPerOct) {
            case 12:
                for (int i = 0; i < 2; ++i) add([&](Biquad& b) { b.firstOrder(highpass, fc, sr); });
                break;
            case 48:
                for (int i = 0; i < 2; ++i) {
                    add([&](Biquad& b) { b.secondOrder(highpass, fc, sr, 0.54119610014619698); });
                    add([&](Biquad& b) { b.secondOrder(highpass, fc, sr, 1.30656296487637652); });
                }
                break;
            default:
                for (int i = 0; i < 2; ++i) add([&](Biquad& b) { b.secondOrder(highpass, fc, sr, 0.70710678118654752); });
        }
    }
    double process(double x) {
        for (int i = 0; i < n_; ++i) x = s_[i].process(x);
        return x;
    }
    void reset() { for (int i = 0; i < n_; ++i) s_[i].reset(); }
    std::complex<double> eval(double f, double sr) const {
        std::complex<double> h(1.0, 0.0);
        for (int i = 0; i < n_; ++i) h *= s_[i].eval(f, sr);
        return h;
    }

private:
    std::array<Biquad, 4> s_{};
    int n_ = 0;
};

/** Késleltetővonal mintapontos késleltetéssel (a puffer előre lefoglalva). */
class Delay {
public:
    void allocate(size_t maxSamples) { buf_.assign(maxSamples + 1, 0.0f); pos_ = 0; }
    void setDelay(size_t d) {
        d = std::min(d, buf_.empty() ? size_t(0) : buf_.size() - 1);
        if (d != d_) { d_ = d; std::fill(buf_.begin(), buf_.end(), 0.0f); pos_ = 0; }
    }
    float process(float x) {
        if (d_ == 0) return x;
        buf_[pos_] = x;
        size_t r = pos_ + buf_.size() - d_;
        if (r >= buf_.size()) r -= buf_.size();
        const float y = buf_[r];
        if (++pos_ == buf_.size()) pos_ = 0;
        return y;
    }
    size_t delay() const { return d_; }

private:
    std::vector<float> buf_;
    size_t pos_ = 0, d_ = 0;
};

/**
 * A több kimenetes motor. A bemenet sztereó; minden kimenet sztereó (a keresztváltó csatornánként külön fut).
 * Alapállapot (zónák, minden kimenet alap lánccal, 0 dB, 0 ms): minden kimenet bit-pontosan a bemenet.
 */
class MultiOutputEngine {
public:
    MultiOutputEngine() {
        for (auto& c : chains_) c = std::make_unique<AetherChain>();
        tmpL_.assign(kBlock, 0.0f);
        tmpR_.assign(kBlock, 0.0f);
        setSampleRate(48000.0);
        MultiConfig c;
        applyConfigRT(c);
    }

    void setSampleRate(double sr) {
        sr_ = sr > 0 ? sr : 48000.0;
        for (auto& c : chains_) c->setSampleRate(sr_);
        const size_t maxD = static_cast<size_t>(std::ceil(kMaxDelayMs * 0.001 * sr_));
        for (auto& o : outs_) for (auto& d : o.delay) d.allocate(maxD);
        hasProcessed_ = false;
        designFilters();
        designAllWayEq(false);
    }
    double sampleRate() const { return sr_; }

    /**
     * Külső fő lánc az 1. kimenetnek (a motor meglévő lánca: a felület, a konvolver, a mérők ezt kezelik).
     * Független zónában az 1. kimenet ezen fut, keresztváltóban ez a közös lánc. A beállításait a hívó adja
     * (applyConfigRT ilyenkor az 1. kimenet láncát nem állítja). nullptr: a saját belső lánc.
     */
    void setExternalMainChain(AetherChain* c) { external_ = c; }

    /** A zónák láncának háttérszálai (FIR-módok, konvolver); a motor indítja és állítja le. */
    void startWorkers() { for (auto& c : chains_) c->startWorker(); }
    void stopWorkers() { for (auto& c : chains_) c->stopWorker(); }

    /** A k-adik kimenet saját lánca (pl. impulzusválasz egy zónának). */
    AetherChain& chain(int k) { return *chains_[std::clamp(k, 0, kMaxOutputs - 1)]; }

    /** A hangszálon hívandó, a beállítás pillanatképével (a hívó gondoskodik a szálak közti átadásról). */
    void applyConfigRT(const MultiConfig& in) {
        MultiConfig c = in;
        sanitize(c);
        const MultiConfig prev = cfg_;
        const bool modeChanged = c.mode != prev.mode;
        const bool xoverChanged = modeChanged || c.numCrossovers != prev.numCrossovers ||
                                  c.slopeDbPerOct != cfg_.slopeDbPerOct || c.crossoverHz != cfg_.crossoverHz || !designed_;
        cfg_ = c;
        const int chainsUsed = cfg_.mode == Mode::Zones ? cfg_.numOutputs : 1;
        for (int k = external_ ? 1 : 0; k < chainsUsed; ++k) chains_[k]->applyParamsRT(cfg_.outputs[k].chain);
        for (int k = 0; k < kMaxOutputs; ++k) {
            const auto& o = cfg_.outputs[k];
            outs_[k].gain = static_cast<float>(std::pow(10.0, o.gainDb / 20.0)) * (o.invert ? -1.0f : 1.0f);
            const size_t d = static_cast<size_t>(std::lround(o.delayMs * 0.001 * sr_));
            for (auto& dl : outs_[k].delay) dl.setDelay(d);
            if (!wayDesigned_ || modeChanged || wayEqChanged(prev.outputs[k].way, o.way)) {
                designWayEq(k, hasProcessed_);
            }
        }
        wayDesigned_ = true;
        if (xoverChanged) designFilters();
    }
    const MultiConfig& config() const { return cfg_; }

    /** A kimenet késleltetése mintában (a beállított késleltetés; a lánc saját késleltetése nélkül). */
    size_t outputDelaySamples(int k) const { return outs_[k].delay[0].delay(); }

    /**
     * Egy út teljes komplex átvitele (keresztváltó + út EQ + szint + polaritás + késleltetés).
     * Ugyanazokat az együtthatókat használja, mint a valós idejű feldolgozás.
     */
    std::vector<std::complex<double>> wayResponse(int k, const std::vector<double>& freqs) const {
        std::vector<std::complex<double>> out(freqs.size(), { 0.0, 0.0 });
        if (k < 0 || k >= cfg_.numOutputs || !cfg_.outputs[k].enabled) return out;
        const OutFilters& f = filt_[k];
        const double gain = outs_[k].gain;
        const double d = static_cast<double>(outs_[k].delay[0].delay());
        const bool xoverWayOn = (cfg_.mode == Mode::Crossover) && curWayOn_[k];
        const int wn = curWayN_[k];
        for (size_t i = 0; i < freqs.size(); ++i) {
            const double freq = freqs[i];
            std::complex<double> h(gain, 0.0);
            if (cfg_.mode == Mode::Crossover && f.stages > 0) {
                for (int j = 0; j < f.stages; ++j) {
                    switch (f.kind[j]) {
                        case 1:
                        case 2: h *= f.main[j][0].eval(freq, sr_); break;
                        case 3: h *= (f.apLp[j][0].eval(freq, sr_) + apSign_ * f.apHp[j][0].eval(freq, sr_)); break;
                        default: break;
                    }
                }
                h *= f.sign;
            }
            if (xoverWayOn) {
                for (int b = 0; b < wn; ++b) h *= curWay_[k][0][b].eval(freq, sr_);
            }
            if (d > 0.0) h *= std::polar(1.0, -2.0 * kPi * freq * d / sr_);
            out[i] = h;
        }
        return out;
    }

    /** Az engedélyezett utak komplex összege. */
    std::vector<std::complex<double>> sumResponse(const std::vector<double>& freqs) const {
        std::vector<std::complex<double>> sum(freqs.size(), { 0.0, 0.0 });
        for (int k = 0; k < cfg_.numOutputs; ++k) {
            if (!cfg_.outputs[k].enabled) continue;
            const auto w = wayResponse(k, freqs);
            for (size_t i = 0; i < freqs.size(); ++i) sum[i] += w[i];
        }
        return sum;
    }

    /** Adott konfigurációból és mintavételi frekvenciából számolt átvitel (a híd szálhoz, adatverseny nélkül). */
    static std::vector<std::complex<double>> computeWayResponse(const MultiConfig& raw, double sr, int k,
                                                                 const std::vector<double>& freqs) {
        MultiConfig c = raw;
        sanitize(c);
        const double useSr = sr > 0.0 ? sr : 48000.0;
        std::vector<std::complex<double>> out(freqs.size(), { 0.0, 0.0 });
        if (k < 0 || k >= c.numOutputs || !c.outputs[k].enabled) return out;
        const auto& o = c.outputs[k];
        const double gain = std::pow(10.0, o.gainDb / 20.0) * (o.invert ? -1.0 : 1.0);
        const double d = static_cast<double>(std::lround(o.delayMs * 0.001 * useSr));
        std::array<LinkwitzRiley, kMaxCrossovers> xoMain{}, xoApLp{}, xoApHp{};
        std::array<int, kMaxCrossovers> xoKind{};
        int stages = 0;
        double xoSign = 1.0;
        const double apSign = (c.slopeDbPerOct == 12) ? -1.0 : 1.0;
        if (c.mode == Mode::Crossover && o.band >= 0) {
            stages = c.numCrossovers;
            for (int j = 0; j < stages; ++j) {
                const double fc = c.crossoverHz[j];
                int kind = (o.band == stages || j < o.band) ? 2 : (j == o.band ? 1 : 3);
                xoKind[j] = kind;
                if (kind == 1) xoMain[j].design(false, fc, useSr, c.slopeDbPerOct);
                else if (kind == 2) {
                    xoMain[j].design(true, fc, useSr, c.slopeDbPerOct);
                    if (c.slopeDbPerOct == 12) xoSign = -xoSign;
                } else {
                    xoApLp[j].design(false, fc, useSr, c.slopeDbPerOct);
                    xoApHp[j].design(true, fc, useSr, c.slopeDbPerOct);
                }
            }
        }
        std::array<Biquad, kMaxWayBands> wayEq{};
        const int wn = (c.mode == Mode::Crossover && o.way.on) ? std::clamp(o.way.numBands, 0, kMaxWayBands) : 0;
        for (int b = 0; b < wn; ++b) {
            wayEq[b].eqBand(o.way.bands[b].type, o.way.bands[b].freq, o.way.bands[b].gainDb, o.way.bands[b].q, useSr);
        }
        for (size_t i = 0; i < freqs.size(); ++i) {
            const double freq = freqs[i];
            std::complex<double> h(gain, 0.0);
            for (int j = 0; j < stages; ++j) {
                if (xoKind[j] == 1 || xoKind[j] == 2) h *= xoMain[j].eval(freq, useSr);
                else if (xoKind[j] == 3) h *= (xoApLp[j].eval(freq, useSr) + apSign * xoApHp[j].eval(freq, useSr));
            }
            h *= xoSign;
            for (int b = 0; b < wn; ++b) h *= wayEq[b].eval(freq, useSr);
            if (d > 0.0) h *= std::polar(1.0, -2.0 * kPi * freq * d / useSr);
            out[i] = h;
        }
        return out;
    }

    static std::vector<std::complex<double>> computeSumResponse(const MultiConfig& raw, double sr,
                                                                 const std::vector<double>& freqs) {
        MultiConfig c = raw;
        sanitize(c);
        std::vector<std::complex<double>> sum(freqs.size(), { 0.0, 0.0 });
        for (int k = 0; k < c.numOutputs; ++k) {
            if (!c.outputs[k].enabled) continue;
            const auto w = computeWayResponse(c, sr, k, freqs);
            for (size_t i = 0; i < freqs.size(); ++i) sum[i] += w[i];
        }
        return sum;
    }

    /**
     * Feldolgozás: inL / inR bemenet (n minta), outL[k] / outR[k] a k-adik kimenet pufferei (numOutputs darab).
     * A bemenet nem változik.
     */
    void process(const float* inL, const float* inR, float* const* outL, float* const* outR, size_t n) {
        for (size_t off = 0; off < n; off += kBlock) {
            const size_t m = std::min(kBlock, n - off);
            float* oL[kMaxOutputs];
            float* oR[kMaxOutputs];
            for (int k = 0; k < cfg_.numOutputs; ++k) { oL[k] = outL[k] + off; oR[k] = outR[k] + off; }
            processBlock(inL + off, inR + off, oL, oR, m);
        }
    }

private:
    struct Out {
        float gain = 1.0f;
        std::array<Delay, 2> delay{};
    };

    /**
     * A keresztváltó fa: a j-edik vágási pontnál a jel alsó (LP) és felső (HP) részre válik. A b-edik sáv:
     *   b = 0:            LP(f0), majd minden további vágási pont mindenáteresztője (LP(fj) ± HP(fj)), j ≥ 1
     *   0 < b < N:        HP(f0) … HP(f(b−1)), LP(fb), majd a fölötte lévő pontok mindenáteresztői
     *   b = N:            HP(f0) … HP(f(N−1))
     * Kimenetenként külön szűrőállapot (a kimenetek sávja tetszőleges, akár azonos is lehet).
     */
    struct OutFilters {
        // fokozatonként (vágási pont szerint) csatornánként: a fő ág (LP vagy HP), és a mindenáteresztő két fele
        std::array<std::array<LinkwitzRiley, 2>, kMaxCrossovers> main{};
        std::array<std::array<LinkwitzRiley, 2>, kMaxCrossovers> apLp{};
        std::array<std::array<LinkwitzRiley, 2>, kMaxCrossovers> apHp{};
        // fokozatonként: 0 = átengedés, 1 = LP, 2 = HP, 3 = mindenáteresztő
        std::array<int, kMaxCrossovers> kind{};
        int stages = 0;
        double sign = 1.0;   // LR2: a felső ágak polaritásfordítása (páratlan számú HP-fokozatnál −1)
    };

    void designFilters() {
        designed_ = true;
        const int N = cfg_.numCrossovers;
        const int slope = cfg_.slopeDbPerOct;
        const double apSign = slope == 12 ? -1.0 : 1.0;   // LR2-nél a mindenáteresztő LP − HP
        apSign_ = apSign;
        for (int k = 0; k < kMaxOutputs; ++k) {
            OutFilters& f = filt_[k];
            f.stages = 0;
            f.sign = 1.0;
            const int b = cfg_.outputs[k].band;
            if (cfg_.mode != Mode::Crossover || b < 0) continue;
            f.stages = N;
            for (int j = 0; j < N; ++j) {
                const double fc = cfg_.crossoverHz[j];
                int kind;
                if (j < b) kind = 2;            // e pont fölött van: HP
                else if (j == b) kind = 1;      // ennél a pontnál vált alulra: LP
                else kind = 3;                  // fölötte lévő pont: mindenáteresztő
                if (b == N && j < N) kind = 2;
                f.kind[j] = kind;
                for (int ch = 0; ch < 2; ++ch) {
                    if (kind == 1) f.main[j][ch].design(false, fc, sr_, slope);
                    else if (kind == 2) f.main[j][ch].design(true, fc, sr_, slope);
                    else {
                        f.apLp[j][ch].design(false, fc, sr_, slope);
                        f.apHp[j][ch].design(true, fc, sr_, slope);
                    }
                }
                if (kind == 2 && slope == 12) f.sign = -f.sign;
            }
        }
    }

    AetherChain& mainOr(int k) { return (k == 0 && external_) ? *external_ : *chains_[k]; }

    double filterSample(OutFilters& f, int ch, double x) {
        for (int j = 0; j < f.stages; ++j) {
            switch (f.kind[j]) {
                case 1:
                case 2: x = f.main[j][ch].process(x); break;
                case 3: x = f.apLp[j][ch].process(x) + apSign_ * f.apHp[j][ch].process(x); break;
                default: break;
            }
        }
        return x * f.sign;
    }

    void processBlock(const float* inL, const float* inR, float** oL, float** oR, size_t m) {
        const int nOut = cfg_.numOutputs;
        if (cfg_.mode == Mode::Zones) {
            for (int k = 0; k < nOut; ++k) {
                std::copy(inL, inL + m, oL[k]);
                std::copy(inR, inR + m, oR[k]);
                mainOr(k).process(oL[k], oR[k], m);
            }
        } else {
            std::copy(inL, inL + m, tmpL_.data());
            std::copy(inR, inR + m, tmpR_.data());
            mainOr(0).process(tmpL_.data(), tmpR_.data(), m);
            for (int k = 0; k < nOut; ++k) {
                OutFilters& f = filt_[k];
                if (f.stages == 0) {
                    for (size_t i = 0; i < m; ++i) {
                        oL[k][i] = curWayOn_[k] || fadePos_[k][0] > 0 ? static_cast<float>(processWaySample(k, 0, tmpL_[i])) : tmpL_[i];
                        oR[k][i] = curWayOn_[k] || fadePos_[k][1] > 0 ? static_cast<float>(processWaySample(k, 1, tmpR_[i])) : tmpR_[i];
                    }
                    continue;
                }
                for (size_t i = 0; i < m; ++i) {
                    oL[k][i] = static_cast<float>(processWaySample(k, 0, filterSample(f, 0, tmpL_[i])));
                    oR[k][i] = static_cast<float>(processWaySample(k, 1, filterSample(f, 1, tmpR_[i])));
                }
            }
        }
        for (int k = 0; k < nOut; ++k) {
            Out& o = outs_[k];
            const bool on = cfg_.outputs[k].enabled;
            const float g = on ? o.gain : 0.0f;
            for (size_t i = 0; i < m; ++i) {
                const float l = o.delay[0].process(oL[k][i]);
                const float r = o.delay[1].process(oR[k][i]);
                oL[k][i] = g == 1.0f ? l : l * g;
                oR[k][i] = g == 1.0f ? r : r * g;
            }
        }
        if (m > 0) hasProcessed_ = true;
    }

    static bool wayEqChanged(const WayEq& a, const WayEq& b) {
        if (a.on != b.on || a.numBands != b.numBands) return true;
        const int n = std::clamp(a.numBands, 0, kMaxWayBands);
        for (int i = 0; i < n; ++i) {
            if (a.bands[i].type != b.bands[i].type || a.bands[i].freq != b.bands[i].freq ||
                a.bands[i].gainDb != b.bands[i].gainDb || a.bands[i].q != b.bands[i].q) {
                return true;
            }
        }
        return false;
    }

    void designWayEq(int k, bool allowFade) {
        const auto& w = cfg_.outputs[k].way;
        const bool active = (cfg_.mode == Mode::Crossover) && w.on && w.numBands > 0;
        const int count = active ? std::clamp(w.numBands, 0, kMaxWayBands) : 0;
        const bool needFade = allowFade && (curWayOn_[k] || active);
        if (needFade) {
            prevWay_[k] = curWay_[k];
            prevWayN_[k] = curWayN_[k];
            prevWayOn_[k] = curWayOn_[k];
            const uint32_t fadeN = static_cast<uint32_t>(std::max(1L, std::lround(0.015 * sr_)));
            fadeTotal_[k] = fadeN;
            fadePos_[k][0] = fadeN;
            fadePos_[k][1] = fadeN;
        } else {
            fadeTotal_[k] = 0;
            fadePos_[k][0] = fadePos_[k][1] = 0;
            prevWayOn_[k] = false;
            prevWayN_[k] = 0;
        }
        curWayOn_[k] = active;
        curWayN_[k] = count;
        for (int ch = 0; ch < 2; ++ch) {
            for (int b = 0; b < count; ++b) {
                curWay_[k][ch][b].eqBand(w.bands[b].type, w.bands[b].freq, w.bands[b].gainDb, w.bands[b].q, sr_);
                curWay_[k][ch][b].reset();
            }
        }
    }

    void designAllWayEq(bool allowFade) {
        for (int k = 0; k < kMaxOutputs; ++k) designWayEq(k, allowFade);
        wayDesigned_ = true;
    }

    double processWaySample(int k, int ch, double x) {
        if (cfg_.mode != Mode::Crossover) return x;
        if (fadePos_[k][ch] == 0) {
            if (!curWayOn_[k]) return x;
            double y = x;
            const int n = curWayN_[k];
            for (int b = 0; b < n; ++b) y = curWay_[k][ch][b].process(y);
            return y;
        }
        double yOld = x;
        if (prevWayOn_[k]) {
            const int pn = prevWayN_[k];
            for (int b = 0; b < pn; ++b) yOld = prevWay_[k][ch][b].process(yOld);
        }
        double yNew = x;
        if (curWayOn_[k]) {
            const int cn = curWayN_[k];
            for (int b = 0; b < cn; ++b) yNew = curWay_[k][ch][b].process(yNew);
        }
        const uint32_t total = std::max<uint32_t>(1u, fadeTotal_[k]);
        const uint32_t rem = fadePos_[k][ch]--;
        const double a = static_cast<double>(total - rem + 1) / static_cast<double>(total);
        return yOld + a * (yNew - yOld);
    }

    double sr_ = 48000.0;
    MultiConfig cfg_{};
    bool designed_ = false;
    AetherChain* external_ = nullptr;
    double apSign_ = 1.0;
    std::array<std::unique_ptr<AetherChain>, kMaxOutputs> chains_{};
    std::array<OutFilters, kMaxOutputs> filt_{};
    std::array<Out, kMaxOutputs> outs_{};
    std::vector<float> tmpL_, tmpR_;
    bool hasProcessed_ = false;
    bool wayDesigned_ = false;
    std::array<std::array<std::array<Biquad, kMaxWayBands>, 2>, kMaxOutputs> curWay_{}, prevWay_{};
    std::array<int, kMaxOutputs> curWayN_{}, prevWayN_{};
    std::array<bool, kMaxOutputs> curWayOn_{}, prevWayOn_{};
    std::array<uint32_t, kMaxOutputs> fadeTotal_{};
    std::array<std::array<uint32_t, 2>, kMaxOutputs> fadePos_{};
};

} // namespace multiout
} // namespace ee_analyzer
