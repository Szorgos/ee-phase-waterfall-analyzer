#pragma once
/**
 * @file magic_touch.hpp
 * @brief Magic Touch: automatikus, visszafogott hangszépítő (docs/plans/magic-touch.md, 1–2. ütem).
 *
 * Hangút (valós idejű szál, késleltetés nélkül, csak IIR szűrők és burkolók):
 *
 *   bemenet -> mély-közép vágás (150–300 Hz, csúcsszűrő) -> levegő (high shelf, 10,5 kHz)
 *           -> de-esser (dinamikus csillapítás 5 kHz felett) -> hangerő-kiegyenlítés
 *           -> oldaljel (S): fokozatos mono basszus (két mély polcszűrő) + tágítás 150 Hz felett (magas polc);
 *              a középjel (M) érintetlen, 0 dB-es polcoknál az oldaljel is -> kimenet
 *
 * A hangerő-kiegyenlítés csak a hangszínváltozást egyenlíti ki; a térbeli javítás (pl. az ellenfázisú
 * mély eltávolítása) után nem emeli vissza a szintet.
 *
 * Elemzés (a lánc háttérszálán): rövid idejű hangosság és LRA (ITU-R BS.1770, EBU Tech 3342),
 * csúcstényező, 1/3 oktávos átlagspektrum (mély-közép többlet, élesség, levegő arány), sziszegés,
 * zajpadló (sziszegés), mély sávi sztereó korreláció, szélesség (S/M 150 Hz felett).
 * A döntések szabályrendszerből jönnek (nem egyetlen összegzett számból), lassan változnak,
 * és a kemény korlátokon belül maradnak. A kimenet hangossága a bemenetével egyezik.
 *
 * Az A/B összehasonlításnál az eredeti jel szól (rövid áttűnéssel); mivel a feldolgozott jel
 * hangossága a bemenetével egyezik, a két állapot hangerőre kiegyenlített.
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "tap_ring.hpp"

namespace ee_analyzer::magic {

// ------------------------------------------------------------------ kemény korlátok (100 %-on is)
constexpr float kMaxLowMidCutDb = 3.0f;    // mély-közép vágás
constexpr float kMaxAirDb = 2.0f;          // levegő emelés (a terv: legfeljebb +2–3 dB)
constexpr float kMaxDeessDb = 4.0f;        // de-esser csillapítás
constexpr float kMaxWidthDb = 1.5f;        // oldaljel emelés 150 Hz felett
constexpr float kMaxMakeupDb = 4.0f;       // hangerő-kiegyenlítés
constexpr double kCrossoverHz = 120.0;     // a mély sáv határa a mérésnél (korreláció)
// Fokozatos mono basszus: két egyforma mély polcszűrő az oldaljelen (75 Hz, teljes erővel 2 × −12 dB):
// 50 Hz alatt kb. −20 dB (közel mono), 80 Hz −11 dB, 120 Hz −4 dB, 150 Hz −2 dB, 200 Hz felett < 1 dB
constexpr double kMonoShelfHz = 75.0;
constexpr float kMonoShelfDb = 12.0f;
constexpr double kWidthShelfHz = 200.0;    // a tágítás 150 Hz felett hat

// ------------------------------------------------------------------ szűrők
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;
    inline float process(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0; }
    void set(double nb0, double nb1, double nb2, double na0, double na1, double na2) {
        b0 = static_cast<float>(nb0 / na0); b1 = static_cast<float>(nb1 / na0); b2 = static_cast<float>(nb2 / na0);
        a1 = static_cast<float>(na1 / na0); a2 = static_cast<float>(na2 / na0);
    }
};

/** Duplapontos változat az elemzéshez (hosszú összegzéseknél pontosabb). */
struct BiquadD {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    inline double process(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0; }
    template <class B> void from(const B& f) { b0 = f.b0; b1 = f.b1; b2 = f.b2; a1 = f.a1; a2 = f.a2; }
    void set(double nb0, double nb1, double nb2, double na0, double na1, double na2) {
        b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0; a1 = na1 / na0; a2 = na2 / na0;
    }
};

constexpr double kPi = 3.14159265358979323846;

/** RBJ Audio EQ Cookbook tervezők (B: Biquad vagy BiquadD). */
template <class B> void designLowpass(B& f, double fs, double fc, double q) {
    const double w = 2 * kPi * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
    f.set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}
template <class B> void designHighpass(B& f, double fs, double fc, double q) {
    const double w = 2 * kPi * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
    f.set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}
template <class B> void designPeaking(B& f, double fs, double fc, double gainDb, double q) {
    const double A = std::pow(10.0, gainDb / 40.0), w = 2 * kPi * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
    f.set(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}
template <class B> void designLowShelf(B& f, double fs, double fc, double gainDb, double slope) {
    const double A = std::pow(10.0, gainDb / 40.0), w = 2 * kPi * fc / fs, c = std::cos(w);
    const double al = std::sin(w) / 2 * std::sqrt((A + 1 / A) * (1 / slope - 1) + 2), sa = 2 * std::sqrt(A) * al;
    f.set(A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa),
          (A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa);
}
template <class B> void designHighShelf(B& f, double fs, double fc, double gainDb, double slope) {
    const double A = std::pow(10.0, gainDb / 40.0), w = 2 * kPi * fc / fs, c = std::cos(w);
    const double al = std::sin(w) / 2 * std::sqrt((A + 1 / A) * (1 / slope - 1) + 2), sa = 2 * std::sqrt(A) * al;
    f.set(A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa),
          (A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa);
}

/** ITU-R BS.1770 K-súlyozás: 1. fokozat (magas polc, fejhatás), tetszőleges mintavételen. */
inline void designKShelf(BiquadD& f, double fs) {
    const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    const double K = std::tan(kPi * f0 / fs), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
    const double a0 = 1.0 + K / Q + K * K;
    f.set(Vh + Vb * K / Q + K * K, 2.0 * (K * K - Vh), Vh - Vb * K / Q + K * K, a0, 2.0 * (K * K - 1.0), 1.0 - K / Q + K * K);
}
/** ITU-R BS.1770 K-súlyozás: 2. fokozat (RLB felüláteresztő). */
inline void designKHighpass(BiquadD& f, double fs) {
    const double f0 = 38.13547087602444, Q = 0.5003270373238773, K = std::tan(kPi * f0 / fs);
    f.set(1.0, -2.0, 1.0, 1.0 + K / Q + K * K, 2.0 * (K * K - 1.0), 1.0 - K / Q + K * K);
}

// ------------------------------------------------------------------ diagnosztika (felületnek)
struct MagicDiag {
    uint32_t valid = 0;          // 1, ha van elég adat a döntésekhez
    uint32_t silent = 1;         // 1, ha most csend van (a döntések állnak)
    // mérések
    float lufsShort = -70;       // rövid idejű hangosság (3 s), LUFS
    float lra = 0;               // hangossági tartomány, LU
    float crestDb = 0;           // csúcstényező (3 s)
    float lowMidExcessDb = 0;    // 150–300 Hz többlet a spektrum trendvonalához képest
    float lowMidFreqHz = 200;    // a többlet közepe
    float harshness = 0;         // 0..1 (2–5 kHz)
    float sibilance = 0;         // 0..1
    float airRatioDb = 0;        // L(10–16 kHz) − L(2–8 kHz)
    float hiss = 0;              // 0..1 (állandó magas zajpadló)
    float lfCorr = 1;            // mély sáv (< 120 Hz) bal-jobb korrelációja
    float sideMidDb = -100;      // S/M energiaarány 150 Hz felett
    // beavatkozások (pillanatnyi értékek)
    float lowMidCutDb = 0;
    float airGainDb = 0;
    float deessDepthDb = 0;      // a de-esser legnagyobb megengedett csillapítása
    float deessNowDb = 0;        // a de-esser pillanatnyi csillapítása (az utolsó időszak legnagyobbja)
    float monoBass = 0;          // 0..1
    float widthGainDb = 0;
    float makeupDb = 0;
};

/** Seqlock a diagnosztikához (POD; folyamatok közötti közös memóriában is használható). */
struct DiagBlock {
    std::atomic<uint32_t> seq{0};
    MagicDiag data;
    void write(const MagicDiag& d) {
        const uint32_t s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(static_cast<void*>(&data), &d, sizeof(MagicDiag));
        std::atomic_thread_fence(std::memory_order_release);
        seq.store(s + 2, std::memory_order_release);
    }
    bool read(MagicDiag& out) const {
        for (int tries = 0; tries < 4; ++tries) {
            const uint32_t s1 = seq.load(std::memory_order_acquire);
            if (s1 & 1u) continue;
            std::memcpy(&out, static_cast<const void*>(&data), sizeof(MagicDiag));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) == s1) return s1 != 0;
        }
        return false;
    }
};

/** A diagnosztika JSON töredékként (a motor állapotüzenetébe: "magic":{...}). */
inline std::string diagToJson(const MagicDiag& d) {
    char b[640];
    std::snprintf(b, sizeof(b),
                  "{\"valid\":%u,\"silent\":%u,\"lufs\":%.1f,\"lra\":%.1f,\"crest\":%.1f,\"lowMid\":%.2f,\"lowMidHz\":%.0f,"
                  "\"harsh\":%.2f,\"sib\":%.2f,\"air\":%.1f,\"hiss\":%.2f,\"lfCorr\":%.2f,\"sideMid\":%.1f,"
                  "\"cut\":%.2f,\"airGain\":%.2f,\"deess\":%.2f,\"deessNow\":%.2f,\"mono\":%.2f,\"width\":%.2f,\"makeup\":%.2f}",
                  d.valid, d.silent, d.lufsShort, d.lra, d.crestDb, d.lowMidExcessDb, d.lowMidFreqHz, d.harshness, d.sibilance,
                  d.airRatioDb, d.hiss, d.lfCorr, d.sideMidDb, d.lowMidCutDb, d.airGainDb, d.deessDepthDb, d.deessNowDb,
                  d.monoBass, d.widthGainDb, d.makeupDb);
    return b;
}

// ------------------------------------------------------------------ segédek az elemzéshez
/** Egyszerű radix-2 FFT (az elemzés másodpercenként kb. 25-ször fut, nem kritikus). */
class Fft {
public:
    void init(size_t n) {
        n_ = n;
        tw_.resize(n / 2);
        for (size_t k = 0; k < n / 2; ++k) tw_[k] = std::polar(1.0, -2.0 * kPi * static_cast<double>(k) / static_cast<double>(n));
        rev_.resize(n);
        size_t bits = 0;
        while ((size_t{1} << bits) < n) ++bits;
        for (size_t i = 0; i < n; ++i) {
            size_t r = 0;
            for (size_t b = 0; b < bits; ++b) if (i & (size_t{1} << b)) r |= size_t{1} << (bits - 1 - b);
            rev_[i] = r;
        }
    }
    void forward(std::vector<std::complex<double>>& a) const {
        for (size_t i = 0; i < n_; ++i) if (i < rev_[i]) std::swap(a[i], a[rev_[i]]);
        for (size_t len = 2; len <= n_; len <<= 1) {
            const size_t step = n_ / len;
            for (size_t i = 0; i < n_; i += len)
                for (size_t j = 0; j < len / 2; ++j) {
                    const std::complex<double> u = a[i + j], v = a[i + j + len / 2] * tw_[j * step];
                    a[i + j] = u + v;
                    a[i + j + len / 2] = u - v;
                }
        }
    }
    size_t size() const { return n_; }
private:
    size_t n_ = 0;
    std::vector<std::complex<double>> tw_;
    std::vector<size_t> rev_;
};

/** Rögzített méretű előzmény-gyűrű percentilisekhez. */
class History {
public:
    explicit History(size_t cap = 1) { resize(cap); }
    void resize(size_t cap) { v_.assign(cap, 0.0); n_ = 0; pos_ = 0; }
    void clear() { n_ = 0; pos_ = 0; }
    void push(double x) { v_[pos_] = x; pos_ = (pos_ + 1) % v_.size(); n_ = std::min(n_ + 1, v_.size()); }
    size_t size() const { return n_; }
    /** p: 0..1; a tmp a hívó munkapuffere (nincs foglalás a gyakori hívásban). */
    double percentile(double p, std::vector<double>& tmp) const {
        if (n_ == 0) return 0.0;
        tmp.assign(v_.begin(), v_.begin() + static_cast<long>(n_));
        const size_t k = std::min(n_ - 1, static_cast<size_t>(p * static_cast<double>(n_ - 1) + 0.5));
        std::nth_element(tmp.begin(), tmp.begin() + static_cast<long>(k), tmp.end());
        return tmp[k];
    }
    template <class F> void forEach(F f) const { for (size_t i = 0; i < n_; ++i) f(v_[i]); }
private:
    std::vector<double> v_;
    size_t n_ = 0, pos_ = 0;
};

inline double toDb(double p) { return 10.0 * std::log10(std::max(p, 1e-30)); }

// ------------------------------------------------------------------ Magic Touch
class MagicTouch {
public:
    static constexpr size_t kMaxBlock = 8192;   // egy process() hívás legnagyobb mérete
    static constexpr size_t kSub = 32;          // paraméter-simítás lépésköze (minta)

    MagicTouch() {
        dryL_.assign(kMaxBlock, 0.0f);
        dryR_.assign(kMaxBlock, 0.0f);
        setSampleRate(48000);
    }
    MagicTouch(const MagicTouch&) = delete;
    MagicTouch& operator=(const MagicTouch&) = delete;

    /** Mintavételi frekvencia (a valós idejű feldolgozás áll). Az elemzés a saját szálán követi. */
    void setSampleRate(uint32_t sr) {
        if (sr < 8000) return;
        fs_ = sr;
        fsShared_.store(sr, std::memory_order_release);
        const double fs = sr;
        for (int c = 0; c < 2; ++c) designHighpass(deessHp_[c], fs, std::min(5000.0, 0.4 * fs), std::sqrt(0.5));
        designHighpass(scHp_, fs, std::min(4500.0, 0.4 * fs), std::sqrt(0.5));
        airEnabled_ = fs >= 32000;
        subCoef_ = static_cast<float>(1.0 - std::exp(-static_cast<double>(kSub) / (0.05 * fs)));     // 50 ms simítás
        scCoef_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.003 * fs)));                            // 3 ms energia
        grAtt_ = static_cast<float>(1.0 - std::exp(-static_cast<double>(kSub) / (0.002 * fs)));      // 2 ms
        grRel_ = static_cast<float>(1.0 - std::exp(-static_cast<double>(kSub) / (0.080 * fs)));      // 80 ms
        cmpStep_ = 1.0f / std::max(1.0f, 0.03f * static_cast<float>(fs));                            // 30 ms áttűnés
        cur_ = Smoothed{};
        coefDirty_ = true;
        updateCoefs();
        reset();
    }

    /** Vezérlés (a lánc paraméterváltásakor, valós idejű szálról is hívható). */
    void setControl(float amountPct, bool compare, bool crossfeedOn) {
        amount_.store(std::clamp(amountPct, 0.0f, 100.0f) / 100.0f, std::memory_order_relaxed);
        compare_ = compare;
        crossfeed_.store(crossfeedOn, std::memory_order_relaxed);
    }

    /** Belső állapot törlése (bekapcsoláskor, hogy ne régi állapotból induljon). */
    void reset() {
        for (int c = 0; c < 2; ++c) { lowMid_[c].reset(); air_[c].reset(); deessHp_[c].reset(); sMono_[c].reset(); }
        sWide_.reset();
        scHp_.reset();
        eSc_ = eBb_ = 0.0f;
        gr_ = 0.0f;
        cmpMix_ = compare_ ? 1.0f : 0.0f;
    }

    // ------------------------------------------------------------ valós idejű szál
    void process(float* L, float* R, size_t n) {
        n = std::min(n, kMaxBlock);
        pre_.write(L, R, static_cast<uint32_t>(n));
        std::memcpy(dryL_.data(), L, n * sizeof(float));
        std::memcpy(dryR_.data(), R, n * sizeof(float));

        float grMax = 0.0f;
        for (size_t off = 0; off < n; off += kSub) {
            const size_t m = std::min(kSub, n - off);
            stepParams();
            float* l = L + off;
            float* r = R + off;
            // 1. mély-közép vágás és 2. levegő (0 dB-nél nem futnak: a jel érintetlen)
            if (lowMidActive_) {
                for (size_t i = 0; i < m; ++i) {
                    l[i] = lowMid_[0].process(l[i]);
                    r[i] = lowMid_[1].process(r[i]);
                }
            }
            if (airActive_) {
                for (size_t i = 0; i < m; ++i) {
                    l[i] = air_[0].process(l[i]);
                    r[i] = air_[1].process(r[i]);
                }
            }
            // 3. de-esser: oldallánc = 4,5 kHz feletti energia a teljes sávhoz képest; csillapítás 5 kHz felett
            for (size_t i = 0; i < m; ++i) {
                const float mono = 0.5f * (l[i] + r[i]);
                const float sc = scHp_.process(mono);
                eSc_ += scCoef_ * (sc * sc - eSc_);
                eBb_ += scCoef_ * (mono * mono - eBb_);
            }
            float grTarget = 0.0f;
            if (cur_.deessDepth > 0.01f && eBb_ > 1e-9f) {
                const float ratioDb = 10.0f * std::log10((eSc_ + 1e-12f) / (eBb_ + 1e-12f));
                grTarget = std::clamp((ratioDb - cur_.deessThresh) * 0.5f, 0.0f, cur_.deessDepth);
            }
            gr_ += (grTarget > gr_ ? grAtt_ : grRel_) * (grTarget - gr_);
            grMax = std::max(grMax, gr_);
            const float g1 = std::pow(10.0f, -gr_ / 20.0f) - 1.0f;   // y = x + (g − 1)·HP(x)
            for (size_t i = 0; i < m; ++i) {
                const float hl = deessHp_[0].process(l[i]);
                const float hr = deessHp_[1].process(r[i]);
                l[i] += g1 * hl;
                r[i] += g1 * hr;
            }
        }
        post_.write(L, R, static_cast<uint32_t>(n));   // a hangerő-kiegyenlítés előtti jel (az elemzésnek)
        deessNow_.store(grMax, std::memory_order_relaxed);

        // 4. hangerő-kiegyenlítés
        const float mkTarget = std::pow(10.0f, tgt_.makeupDb.load(std::memory_order_relaxed) / 20.0f);
        if (mk_ != 1.0f || mkTarget != 1.0f) {
            for (size_t i = 0; i < n; ++i) {
                mk_ += 0.0005f * (mkTarget - mk_);
                L[i] *= mk_;
                R[i] *= mk_;
            }
        }
        // 5. Oldaljel (S): fokozatos mono basszus és tágítás. A középjel (M) nem változik, és 0 dB-es
        //    polcoknál az oldaljel sem: a rendben lévő felvétel fázisa és csúcsai érintetlenek maradnak
        if (msActive_) {
            for (size_t i = 0; i < n; ++i) {
                const float mid = 0.5f * (L[i] + R[i]);
                const float side = sWide_.process(sMono_[1].process(sMono_[0].process(0.5f * (L[i] - R[i]))));
                L[i] = mid + side;
                R[i] = mid - side;
            }
        }
        // 6. A/B áttűnés
        const float cmpTarget = compare_ ? 1.0f : 0.0f;
        if (cmpMix_ != 0.0f || cmpTarget != 0.0f) {
            for (size_t i = 0; i < n; ++i) {
                if (cmpMix_ != cmpTarget) cmpMix_ = cmpMix_ < cmpTarget ? std::min(cmpTarget, cmpMix_ + cmpStep_)
                                                                        : std::max(cmpTarget, cmpMix_ - cmpStep_);
                L[i] += cmpMix_ * (dryL_[i] - L[i]);
                R[i] += cmpMix_ * (dryR_[i] - R[i]);
            }
        }
    }

    // ------------------------------------------------------------ elemzés (háttérszál)
    /** A háttérszálon, rendszeresen hívandó (pl. 10–20 ms-onként); az új mintákat dolgozza fel. */
    void analyze() {
        const uint32_t sr = fsShared_.load(std::memory_order_acquire);
        if (sr != aFs_) configureAnalysis(sr);
        const size_t cap = aPre_.size() / 2;
        for (;;) {
            // Csak azokat a mintákat, amelyeknek a feldolgozott párja is megvan (a process() végén íródik)
            const uint64_t wPost = post_.writePos.load(std::memory_order_acquire);
            if (wPost < preRead_) preRead_ = wPost;   // újraindult az író
            if (wPost == preRead_) break;
            const size_t want = static_cast<size_t>(std::min<uint64_t>(wPost - preRead_, cap));
            const size_t got = readTapRing(pre_, preRead_, aPre_.data(), want);
            if (got == 0) break;
            uint64_t pr = preRead_ - got;   // a bemenettel azonos pozíció
            const size_t gotPost = readTapRing(post_, pr, aPost_.data(), got);
            consume(aPre_.data(), gotPost == got ? aPost_.data() : nullptr, got);
            if (got < cap) break;
        }
    }

    /** A legutóbbi diagnosztika (bármely szálról). */
    bool diag(MagicDiag& out) const { return diag_.read(out); }

private:
    struct Smoothed {
        float mono = 0, widthDb = 0, lowMidDb = 0, lowMidHz = 200, airDb = 0, deessDepth = 0, deessThresh = 0;
    };
    struct Targets {
        std::atomic<float> mono{0}, widthDb{0}, lowMidDb{0}, lowMidHz{200}, airDb{0}, deessDepth{0}, deessThresh{-6},
                           makeupDb{0};
    };

    void stepParams() {
        const float k = subCoef_;
        const float lmDb = tgt_.lowMidDb.load(std::memory_order_relaxed), lmHz = tgt_.lowMidHz.load(std::memory_order_relaxed);
        const float aDb = tgt_.airDb.load(std::memory_order_relaxed);
        cur_.mono += k * (tgt_.mono.load(std::memory_order_relaxed) - cur_.mono);
        cur_.widthDb += k * (tgt_.widthDb.load(std::memory_order_relaxed) - cur_.widthDb);
        cur_.lowMidDb += k * (lmDb - cur_.lowMidDb);
        cur_.lowMidHz += k * (lmHz - cur_.lowMidHz);
        cur_.airDb += k * (aDb - cur_.airDb);
        cur_.deessDepth += k * (tgt_.deessDepth.load(std::memory_order_relaxed) - cur_.deessDepth);
        cur_.deessThresh = tgt_.deessThresh.load(std::memory_order_relaxed);
        // Új együtthatók, ha a simított érték érezhetően eltér a legutóbb beállítottól
        const Smoothed& a = applied_;
        if (coefDirty_ || std::fabs(cur_.lowMidDb - a.lowMidDb) > 0.01f || std::fabs(cur_.lowMidHz - a.lowMidHz) > 0.5f ||
            std::fabs(cur_.airDb - a.airDb) > 0.01f || std::fabs(cur_.mono - a.mono) > 0.001f ||
            std::fabs(cur_.widthDb - a.widthDb) > 0.01f) {
            updateCoefs();
        }
    }

    void updateCoefs() {
        coefDirty_ = false;
        applied_ = cur_;
        for (int c = 0; c < 2; ++c) {
            designPeaking(lowMid_[c], fs_, cur_.lowMidHz, cur_.lowMidDb, 1.0);
            if (airEnabled_) designHighShelf(air_[c], fs_, 10500.0, cur_.airDb, 0.9);
            designLowShelf(sMono_[c], fs_, kMonoShelfHz, -kMonoShelfDb * cur_.mono, 1.0);
        }
        designHighShelf(sWide_, fs_, kWidthShelfHz, cur_.widthDb, 0.9);
        // A fokozatok csak akkor futnak, ha van mit tenniük: különben a jel bitre érintetlen
        // (bekapcsoláskor 0 dB közeli erősítéssel, üres állapotból indulnak, így nincs ugrás)
        const bool ms = cur_.mono > 0.0005f || cur_.widthDb > 0.005f;
        if (ms && !msActive_) { sMono_[0].reset(); sMono_[1].reset(); sWide_.reset(); }
        msActive_ = ms;
        const bool lm = std::fabs(cur_.lowMidDb) > 0.005f;
        if (lm && !lowMidActive_) { lowMid_[0].reset(); lowMid_[1].reset(); }
        lowMidActive_ = lm;
        const bool air = airEnabled_ && cur_.airDb > 0.005f;
        if (air && !airActive_) { air_[0].reset(); air_[1].reset(); }
        airActive_ = air;
    }

    // --- elemzés ---
    static constexpr int kBands = 31;   // 1/3 oktáv, 20 Hz .. 20 kHz
    static double bandCenter(int b) { return 1000.0 * std::pow(2.0, (b - 17) / 3.0); }

    void configureAnalysis(uint32_t sr) {
        aFs_ = sr;
        const double fs = sr;
        designKShelf(kPreA_[0], fs); designKHighpass(kPreB_[0], fs);
        kPreA_[1] = kPreA_[0]; kPreB_[1] = kPreB_[0];
        kPostA_[0] = kPreA_[0]; kPostA_[1] = kPreA_[0]; kPostB_[0] = kPreB_[0]; kPostB_[1] = kPreB_[0];
        for (int c = 0; c < 2; ++c) for (int k = 0; k < 2; ++k) designLowpass(lfLp_[c][k], fs, kCrossoverHz, std::sqrt(0.5));
        designHighpass(whHp_[0], fs, 150.0, std::sqrt(0.5));
        whHp_[1] = whHp_[0];
        designHighpass(hfHp_[0], fs, 8000.0, std::sqrt(0.5)); hfHp_[1] = hfHp_[0];
        designLowpass(hfLp_[0], fs, std::min(16000.0, 0.45 * fs), std::sqrt(0.5)); hfLp_[1] = hfLp_[0];
        designHighpass(sibHp_, fs, std::min(4500.0, 0.4 * fs), std::sqrt(0.5));
        for (auto* f : { &kPreA_[0], &kPreA_[1], &kPreB_[0], &kPreB_[1], &kPostA_[0], &kPostA_[1], &kPostB_[0], &kPostB_[1],
                         &whHp_[0], &whHp_[1], &hfHp_[0], &hfHp_[1], &hfLp_[0], &hfLp_[1], &sibHp_ }) f->reset();
        for (auto& c : lfLp_) for (auto& f : c) f.reset();

        blockLen_ = std::max<size_t>(1, sr / 10);        // 100 ms
        hfFrameLen_ = std::max<size_t>(1, sr / 50);      // 20 ms
        sibFrameLen_ = std::max<size_t>(1, sr / 100);    // 10 ms
        fftN_ = sr > 50000 ? 8192 : 4096;
        fft_.init(fftN_);
        fftBuf_.assign(fftN_, 0.0);
        fftWork_.assign(fftN_, {});
        win_.resize(fftN_);
        for (size_t i = 0; i < fftN_; ++i) win_[i] = 0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / static_cast<double>(fftN_));
        fftFill_ = 0;
        fftHop_ = fftN_ / 2;
        // 1/3 oktávos sávok határai FFT bin-ekben (üres sáv a legközelebbi bin-t kapja)
        const double binHz = fs / static_cast<double>(fftN_);
        for (int b = 0; b < kBands; ++b) {
            const double c = bandCenter(b), lo = c / std::pow(2.0, 1.0 / 6.0), hi = c * std::pow(2.0, 1.0 / 6.0);
            size_t b0 = static_cast<size_t>(std::ceil(lo / binHz)), b1 = static_cast<size_t>(std::floor(hi / binHz));
            b0 = std::clamp<size_t>(b0, 1, fftN_ / 2);
            b1 = std::clamp<size_t>(b1, 1, fftN_ / 2);
            if (b1 < b0) b1 = b0;
            bandLo_[b] = b0; bandHi_[b] = b1;
            bandOk_[b] = hi < 0.5 * fs;
        }
        specAvg_.fill(0.0);
        specFrames_ = 0;
        blocks_.clear(); stHist_.resize(600); hfHist_.resize(400); bbHist_.resize(400); sibHist_.resize(800);
        blkPos_ = hfPos_ = sibPos_ = 0;
        blk_ = Block{};
        hfAcc_ = bbAcc_ = sibAcc_ = sibBb_ = 0;
        lfLL_ = lfRR_ = lfLR_ = 0; whM_ = whS_ = 0; lfValid_ = whValid_ = false;
        activeBlocks_ = 0; blockCount_ = 0; silentBlocks_ = 0;
        dg_ = MagicDiag{};
        st_ = Decision{};
        preRead_ = post_.writePos.load();
    }

    struct Block {
        double kPre = 0, kPost = 0, raw = 0, peak = 0, lfL = 0, lfR = 0, lfLR = 0, mHi = 0, sHi = 0;
        size_t n = 0;
    };

    void consume(const float* pre, const float* post, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            const double l = pre[2 * i], r = pre[2 * i + 1];
            // hangosság (K-súlyozás) a bemeneten és a feldolgozott jelen
            const double kl = kPreB_[0].process(kPreA_[0].process(l)), kr = kPreB_[1].process(kPreA_[1].process(r));
            blk_.kPre += kl * kl + kr * kr;
            if (post) {
                const double pl = post[2 * i], pr = post[2 * i + 1];
                const double ql = kPostB_[0].process(kPostA_[0].process(pl)), qr = kPostB_[1].process(kPostA_[1].process(pr));
                blk_.kPost += ql * ql + qr * qr;
            }
            blk_.raw += 0.5 * (l * l + r * r);
            blk_.peak = std::max(blk_.peak, std::max(std::fabs(l), std::fabs(r)));
            // mély sáv korreláció
            const double ll = lfLp_[0][1].process(lfLp_[0][0].process(l)), lr = lfLp_[1][1].process(lfLp_[1][0].process(r));
            blk_.lfL += ll * ll; blk_.lfR += lr * lr; blk_.lfLR += ll * lr;
            // szélesség 150 Hz felett
            const double mh = whHp_[0].process(0.5 * (l + r)), sh = whHp_[1].process(0.5 * (l - r));
            blk_.mHi += mh * mh; blk_.sHi += sh * sh;
            const double mono = 0.5 * (l + r);
            // zajpadló: 8–16 kHz és teljes sáv, 20 ms-os keretek
            const double hf = hfLp_[1].process(hfLp_[0].process(hfHp_[1].process(hfHp_[0].process(mono))));
            hfAcc_ += hf * hf; bbAcc_ += mono * mono;
            if (++hfPos_ >= hfFrameLen_) {
                const double eb = bbAcc_ / static_cast<double>(hfFrameLen_), eh = hfAcc_ / static_cast<double>(hfFrameLen_);
                if (toDb(eb) > -60.0) { hfHist_.push(toDb(eh)); bbHist_.push(toDb(eb)); }
                hfAcc_ = bbAcc_ = 0; hfPos_ = 0;
            }
            // sziszegés: 4,5 kHz feletti energia aránya 10 ms-os keretekben
            const double sb = sibHp_.process(mono);
            sibAcc_ += sb * sb; sibBb_ += mono * mono;
            if (++sibPos_ >= sibFrameLen_) {
                const double eb = sibBb_ / static_cast<double>(sibFrameLen_);
                if (toDb(eb) > -55.0) sibHist_.push(toDb(sibAcc_ / static_cast<double>(sibFrameLen_)) - toDb(eb));
                sibAcc_ = sibBb_ = 0; sibPos_ = 0;
            }
            // spektrum (mono, Hann ablak, 50 % átfedés)
            fftBuf_[fftFill_++] = mono;
            if (fftFill_ == fftN_) {
                spectrumFrame();
                std::memmove(fftBuf_.data(), fftBuf_.data() + fftHop_, (fftN_ - fftHop_) * sizeof(double));
                fftFill_ = fftN_ - fftHop_;
            }
            if (++blk_.n >= blockLen_) {
                endBlock(post != nullptr);
                blk_ = Block{};
            }
        }
    }

    void spectrumFrame() {
        double e = 0;
        for (size_t i = 0; i < fftN_; ++i) { fftWork_[i] = { fftBuf_[i] * win_[i], 0.0 }; e += fftBuf_[i] * fftBuf_[i]; }
        if (toDb(e / static_cast<double>(fftN_)) < -65.0) return;   // csend: nem számít az átlagba
        fft_.forward(fftWork_);
        // A hangszín egy felvételen belül állandó: hosszú átlag (τ ≈ 20 s), hogy a refrén és a vers között ne
        // ingadozzon a beavatkozás; új számnál (csend után) gyors indulás
        const double alpha = static_cast<double>(fftHop_) / (20.0 * static_cast<double>(aFs_));
        const double a = specFrames_ < 20 ? 1.0 / static_cast<double>(specFrames_ + 1) : alpha;
        for (int b = 0; b < kBands; ++b) {
            double p = 0;
            for (size_t k = bandLo_[b]; k <= bandHi_[b]; ++k) p += std::norm(fftWork_[k]);
            specAvg_[b] += a * (p - specAvg_[b]);
        }
        ++specFrames_;
    }

    struct Decision {
        float lowMid = 0, lowMidHz = 200, air = 0, deess = 0, deessThr = -6, mono = 0, width = 0, makeup = 0;
    };

    void endBlock(bool havePost) {
        ++blockCount_;
        const double n = static_cast<double>(std::max<size_t>(1, blk_.n));
        blocks_.push_back(blk_);
        if (blocks_.size() > 30) blocks_.erase(blocks_.begin());   // 3 s
        // rövid idejű hangosság (3 s) a bemeneten és a feldolgozott jelen
        double kPre = 0, kPost = 0, raw = 0, peak = 0, cnt = 0;
        for (const Block& b : blocks_) { kPre += b.kPre; kPost += b.kPost; raw += b.raw; peak = std::max(peak, b.peak); cnt += static_cast<double>(b.n); }
        const double lPre = -0.691 + toDb(kPre / cnt), lPost = -0.691 + toDb(kPost / cnt);
        stHist_.push(lPre);
        dg_.lufsShort = static_cast<float>(lPre);
        dg_.crestDb = static_cast<float>(20.0 * std::log10(std::max(peak, 1e-9) / std::sqrt(std::max(raw / cnt, 1e-18))));
        // mély sáv korreláció és szélesség (lassú átlag, csak ha van jel)
        const double ab = 0.02;   // ~5 s
        if ((blk_.lfL + blk_.lfR) / n > 1e-7) {
            if (!lfValid_) { lfLL_ = blk_.lfL; lfRR_ = blk_.lfR; lfLR_ = blk_.lfLR; lfValid_ = true; }
            lfLL_ += ab * (blk_.lfL - lfLL_); lfRR_ += ab * (blk_.lfR - lfRR_); lfLR_ += ab * (blk_.lfLR - lfLR_);
        }
        if (blk_.mHi / n > 1e-8) {
            if (!whValid_) { whM_ = blk_.mHi; whS_ = blk_.sHi; whValid_ = true; }
            whM_ += ab * (blk_.mHi - whM_); whS_ += ab * (blk_.sHi - whS_);
        }
        const bool silent = lPre < -55.0;
        dg_.silent = silent ? 1u : 0u;
        // Új szám (legalább 1,5 s csend után): a mérések újraindulnak, a beavatkozás onnan lassan igazodik
        if (silent) {
            ++silentBlocks_;
        } else {
            if (silentBlocks_ >= 15) restartMeasurements();
            silentBlocks_ = 0;
            ++activeBlocks_;
        }
        if (blockCount_ % 10 == 0) updateSlowStats();

        const float I = amount_.load(std::memory_order_relaxed);
        const bool ready = activeBlocks_ >= 30 && specFrames_ >= 20;
        dg_.valid = ready ? 1u : 0u;
        if (ready) measureSpectrum();
        if (ready && !silent) decide(I);
        // hangerő-kiegyenlítés: a feldolgozott jel (kiegyenlítés előtt) hangossága a bemenetéhez
        if (havePost && !silent && lPost > -70.0) {
            const float want = std::clamp(static_cast<float>(lPre - lPost), -kMaxMakeupDb, kMaxMakeupDb);
            st_.makeup = slew(st_.makeup, want, 0.05f);   // 0,5 dB/s
        }
        publish();
    }

    void restartMeasurements() {
        specAvg_.fill(0.0);
        specFrames_ = 0;
        hfHist_.clear(); bbHist_.clear(); sibHist_.clear(); stHist_.clear();
        lfValid_ = whValid_ = false;
        activeBlocks_ = 0;
    }

    /** Ritkább statisztikák (másodpercenként): LRA, sziszegés, zajpadló. */
    void updateSlowStats() {
        // LRA (EBU Tech 3342): abszolút kapu −70 LUFS, relatív kapu −20 LU, P95 − P10
        std::vector<double>& t = tmp_;
        t.clear();
        double sum = 0; size_t k = 0;
        stHist_.forEach([&](double v) { if (v > -70.0) { sum += std::pow(10.0, v / 10.0); ++k; } });
        if (k >= 10) {
            const double rel = toDb(sum / static_cast<double>(k)) - 20.0;
            stHist_.forEach([&](double v) { if (v > -70.0 && v > rel) t.push_back(v); });
            if (t.size() >= 10) {
                std::sort(t.begin(), t.end());
                const auto pct = [&](double p) { return t[std::min(t.size() - 1, static_cast<size_t>(p * static_cast<double>(t.size() - 1) + 0.5))]; };
                dg_.lra = static_cast<float>(pct(0.95) - pct(0.10));
            }
        }
        // sziszegés: a 4,5 kHz feletti arány kiugrásai a medián fölött
        if (sibHist_.size() >= 100) {
            const double med = sibHist_.percentile(0.5, tmp_);
            size_t over = 0;
            sibHist_.forEach([&](double v) { if (v > med + 10.0) ++over; });
            const double frac = static_cast<double>(over) / static_cast<double>(sibHist_.size());
            sibMedian_ = med;
            sibP95_ = sibHist_.percentile(0.95, tmp_);
            dg_.sibilance = static_cast<float>(std::clamp((frac - 0.08) / 0.2, 0.0, 1.0));
        }
        // zajpadló: a 8–16 kHz sáv dinamikája a teljes sávéhoz képest (állandó sziszegésnél kicsi)
        if (hfHist_.size() >= 100) {
            const double dynH = hfHist_.percentile(0.9, tmp_) - hfHist_.percentile(0.1, tmp_);
            const double dynB = bbHist_.percentile(0.9, tmp_) - bbHist_.percentile(0.1, tmp_);
            dg_.hiss = static_cast<float>(std::clamp((dynB - dynH - 2.0) / 10.0, 0.0, 1.0));
        }
    }

    /** Az átlagspektrumból: mély-közép többlet, élesség, levegő arány. */
    void measureSpectrum() {
        std::array<double, kBands> db{};
        for (int b = 0; b < kBands; ++b) db[b] = toDb(specAvg_[b]);
        // trendvonal (legkisebb négyzetek, log2 frekvencia) 95 Hz és 5,2 kHz között
        double sx = 0, sy = 0, sxx = 0, sxy = 0, cnt = 0;
        for (int b = 0; b < kBands; ++b) {
            const double c = bandCenter(b);
            if (c < 95.0 || c > 5200.0 || !bandOk_[b]) continue;
            const double x = std::log2(c);
            sx += x; sy += db[b]; sxx += x * x; sxy += x * db[b]; ++cnt;
        }
        if (cnt < 5) return;
        const double slope = (cnt * sxy - sx * sy) / std::max(1e-12, cnt * sxx - sx * sx), icpt = (sy - slope * sx) / cnt;
        const auto res = [&](int b) { return db[b] - (icpt + slope * std::log2(bandCenter(b))); };
        // mély-közép (160, 200, 250 Hz sávok)
        double lm = 0, best = -1e9; int bestB = 10;
        for (int b = 9; b <= 11; ++b) { const double r = res(b); lm += r; if (r > best) { best = r; bestB = b; } }
        dg_.lowMidExcessDb = static_cast<float>(lm / 3.0);
        dg_.lowMidFreqHz = static_cast<float>(std::clamp(bandCenter(bestB), 150.0, 300.0));
        // élesség (2,5–5 kHz): a trendvonal feletti rész és a helyi kiemelkedés (görbület)
        double hr = 0;
        for (int b = 21; b <= 24; ++b) hr += res(b);
        hr /= 4.0;
        double curv = -1e9;
        const auto sm3 = [&](int b) { return (db[b - 1] + db[b] + db[b + 1]) / 3.0; };
        for (int b = 21; b <= 24; ++b) curv = std::max(curv, sm3(b) - 0.5 * (sm3(b - 3) + sm3(b + 3)));
        dg_.harshness = static_cast<float>(std::clamp((std::max(hr + 1.0, 0.0) + std::max(curv - 2.0, 0.0)) / 4.0, 0.0, 1.0));
        // levegő arány: 10–16 kHz a 2–8 kHz-hez képest
        double hi = 0, mid = 0;
        for (int b = 27; b <= 29; ++b) if (bandOk_[b]) hi += specAvg_[b];
        for (int b = 20; b <= 26; ++b) mid += specAvg_[b];
        dg_.airRatioDb = static_cast<float>(toDb(hi) - toDb(mid));
    }

    static float slew(float cur, float want, float step) {
        return cur + std::clamp(want - cur, -step, step);
    }

    /** Szabályrendszer (100 ms-onként): minden modul a saját mérése alapján, korlátok között. */
    void decide(float I) {
        // mély-közép többlet: csak a trendvonal fölötti rész, 1 dB felett
        const float lowMid = -std::clamp((dg_.lowMidExcessDb - 1.0f) * 1.0f, 0.0f, kMaxLowMidCutDb) * I;
        st_.lowMid = slew(st_.lowMid, lowMid, 0.05f);   // 0,5 dB/s
        st_.lowMidHz = slew(st_.lowMidHz, dg_.lowMidFreqHz, 5.0f);
        // levegő: ha hiányzik, de nem zajos, nem éles, nem sziszegő és nem sávkorlátos a felvétel
        const bool bandLimited = dg_.airRatioDb < -35.0f;
        float air = bandLimited ? 0.0f : std::clamp((-8.0f - dg_.airRatioDb) * 0.3f, 0.0f, kMaxAirDb);
        air *= (1.0f - dg_.hiss) * (1.0f - 0.7f * dg_.harshness) * (1.0f - 0.5f * dg_.sibilance) * I;
        st_.air = slew(st_.air, air, 0.05f);
        // de-esser: csak a felvételre jellemzőnél jóval erősebb kitöréseket fogja vissza (a medián fölött
        // legalább 12 dB-lel és a 95. percentilis fölött), hogy a cintányérokat és a lábcint ne tompítsa
        const float deess = std::clamp(1.5f + 2.5f * dg_.sibilance + 0.5f * st_.air, 0.0f, kMaxDeessDb) * I;
        st_.deess = slew(st_.deess, deess, 0.2f);
        st_.deessThr = static_cast<float>(std::max(sibMedian_ + 12.0, sibP95_ + 3.0) - 2.0 * dg_.harshness);
        // fokozatos mono basszus: csak fázisproblémás (alacsony vagy negatív korrelációjú) mélynél
        dg_.lfCorr = lfValid_ ? static_cast<float>(lfLR_ / std::sqrt(std::max(lfLL_ * lfRR_, 1e-30))) : 1.0f;
        const float mono = lfValid_ ? std::clamp((0.4f - dg_.lfCorr) / 0.6f, 0.0f, 1.0f) * I : 0.0f;
        st_.mono = slew(st_.mono, mono, 0.02f);
        // tágítás 150 Hz felett: csak keskeny (de nem mono) keverésnél, és ha a Crossfeed nincs bekapcsolva
        dg_.sideMidDb = whValid_ ? static_cast<float>(toDb(whS_) - toDb(whM_)) : -100.0f;
        const bool cf = crossfeed_.load(std::memory_order_relaxed);
        const float width = (cf || dg_.sideMidDb < -40.0f || dg_.sideMidDb > -14.0f)
                                ? 0.0f : std::clamp((-14.0f - dg_.sideMidDb) * 0.15f, 0.0f, kMaxWidthDb) * I;
        st_.width = slew(st_.width, width, 0.05f);
    }

    void publish() {
        tgt_.lowMidDb.store(st_.lowMid, std::memory_order_relaxed);
        tgt_.lowMidHz.store(st_.lowMidHz, std::memory_order_relaxed);
        tgt_.airDb.store(st_.air, std::memory_order_relaxed);
        tgt_.deessDepth.store(st_.deess, std::memory_order_relaxed);
        tgt_.deessThresh.store(st_.deessThr, std::memory_order_relaxed);
        tgt_.mono.store(st_.mono, std::memory_order_relaxed);
        tgt_.widthDb.store(st_.width, std::memory_order_relaxed);
        tgt_.makeupDb.store(st_.makeup, std::memory_order_relaxed);
        dg_.lowMidCutDb = st_.lowMid;
        dg_.airGainDb = st_.air;
        dg_.deessDepthDb = st_.deess;
        dg_.deessNowDb = deessNow_.load(std::memory_order_relaxed);
        dg_.monoBass = st_.mono;
        dg_.widthGainDb = st_.width;
        dg_.makeupDb = st_.makeup;
        diag_.write(dg_);
    }

    // valós idejű állapot
    uint32_t fs_ = 48000;
    Biquad sMono_[2], sWide_;             // oldaljel: két mély polc (mono basszus), magas polc (tágítás)
    Biquad lowMid_[2], air_[2], deessHp_[2], scHp_;
    bool airEnabled_ = true, coefDirty_ = true, compare_ = false, msActive_ = false, lowMidActive_ = false, airActive_ = false;
    float subCoef_ = 0.1f, scCoef_ = 0.01f, grAtt_ = 0.5f, grRel_ = 0.01f, cmpStep_ = 0.001f;
    float eSc_ = 0, eBb_ = 0, gr_ = 0, mk_ = 1.0f, cmpMix_ = 0.0f;
    Smoothed cur_, applied_;
    std::vector<float> dryL_, dryR_;

    // szálak között
    std::atomic<uint32_t> fsShared_{0};
    std::atomic<float> amount_{0.5f};
    std::atomic<bool> crossfeed_{false};
    std::atomic<float> deessNow_{0.0f};
    Targets tgt_;
    // A gyűrűk a kupacon (egyenként 512 KB): a lánc vermen is létrehozható marad (pl. tesztekben, Windowson 1 MB verem)
    using Ring = StereoTapRing<65536>;
    std::unique_ptr<Ring> preRing_ = std::make_unique<Ring>(), postRing_ = std::make_unique<Ring>();
    Ring& pre_ = *preRing_;
    Ring& post_ = *postRing_;
    DiagBlock diag_;

    // elemzés (csak a háttérszál)
    uint32_t aFs_ = 0;
    uint64_t preRead_ = 0;
    std::vector<float> aPre_ = std::vector<float>(2 * 8192), aPost_ = std::vector<float>(2 * 8192);
    BiquadD kPreA_[2], kPreB_[2], kPostA_[2], kPostB_[2], lfLp_[2][2], whHp_[2], hfHp_[2], hfLp_[2], sibHp_;
    size_t blockLen_ = 4800, hfFrameLen_ = 960, sibFrameLen_ = 480, hfPos_ = 0, sibPos_ = 0, blkPos_ = 0;
    double hfAcc_ = 0, bbAcc_ = 0, sibAcc_ = 0, sibBb_ = 0, sibMedian_ = -20.0, sibP95_ = -10.0;
    Fft fft_;
    size_t fftN_ = 4096, fftFill_ = 0, fftHop_ = 2048;
    std::vector<double> fftBuf_, win_, tmp_;
    std::vector<std::complex<double>> fftWork_;
    std::array<size_t, kBands> bandLo_{}, bandHi_{};
    std::array<bool, kBands> bandOk_{};
    std::array<double, kBands> specAvg_{};
    size_t specFrames_ = 0;
    Block blk_;
    std::vector<Block> blocks_;
    History stHist_{600}, hfHist_{400}, bbHist_{400}, sibHist_{800};
    double lfLL_ = 0, lfRR_ = 0, lfLR_ = 0, whM_ = 0, whS_ = 0;
    bool lfValid_ = false, whValid_ = false;
    size_t activeBlocks_ = 0, blockCount_ = 0, silentBlocks_ = 0;
    MagicDiag dg_;
    Decision st_;
};

} // namespace ee_analyzer::magic
