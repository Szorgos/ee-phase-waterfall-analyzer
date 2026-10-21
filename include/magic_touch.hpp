#pragma once
/**
 * @file magic_touch.hpp
 * @brief Magic Touch: automatikus, visszafogott hangszépítő (docs/plans/magic-touch.md, 1–4. ütem).
 *
 * Hangút (valós idejű szál, késleltetés nélkül, csak IIR szűrők és burkolók):
 *
 *   bemenet -> beszéd: mélyvágás 80 Hz alatt -> mély-közép vágás (150–300 Hz, csúcsszűrő)
 *           -> Meleg karakter: mély polc (150 Hz) -> beszéd: jelenlét (2,2 kHz) -> levegő (high shelf, 10,5 kHz)
 *           -> de-esser (dinamikus csillapítás 5 kHz felett)
 *           -> exciter (páros felharmonikusok: 3–9 kHz -> 6 kHz felett; Meleg: 120–600 Hz -> 200 Hz felett)
 *           -> mikrodinamika (tranziens-kiemelés, a halk részek finom emelése; a csúcsot nem viszi −1 dBFS fölé)
 *           -> hangerő-kiegyenlítés (beszédnél a szintkiegyenlítéssel együtt)
 *           -> oldaljel (S): fokozatos mono basszus (két mély polcszűrő) + tágítás 150 Hz felett (magas polc);
 *              a középjel (M) érintetlen, 0 dB-es polcoknál az oldaljel is -> kimenet
 *
 * A hangerő-kiegyenlítés csak a hangszínváltozást egyenlíti ki; a térbeli javítás (pl. az ellenfázisú
 * mély eltávolítása) után nem emeli vissza a szintet.
 *
 * Elemzés (a lánc háttérszálán): rövid idejű hangosság és LRA (ITU-R BS.1770, EBU Tech 3342),
 * csúcstényező, 1/3 oktávos átlagspektrum (mély-közép többlet, élesség, levegő arány, sávszélesség),
 * sziszegés, zajpadló (sziszegés), mély sávi sztereó korreláció, szélesség (S/M 150 Hz felett),
 * tartalom (beszéd: halk keretek aránya, 4 Hz körüli moduláció, kevés mély; archív: letört sávszélesség).
 * A döntések szabályrendszerből jönnek (nem egyetlen összegzett számból), lassan változnak,
 * és a kemény korlátokon belül maradnak. A Karakter és a felismert tartalom a szabályok súlyait
 * állítja. A kimenet hangossága a bemenetével egyezik (beszédnél a hosszú távú átlaga).
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
constexpr float kMaxAirDb = 2.5f;          // levegő emelés (a terv: legfeljebb +2–3 dB; a Fényes karakternél)
constexpr float kMaxDeessDb = 4.0f;        // de-esser csillapítás
constexpr float kMaxWidthDb = 1.5f;        // oldaljel emelés 150 Hz felett
constexpr float kMaxMakeupDb = 4.0f;       // hangerő-kiegyenlítés
constexpr float kMaxTransientDb = 1.2f;    // tranziens-kiemelés (+15 %: 20·log10(1,15) ≈ 1,2 dB)
constexpr float kMaxUpwardDb = 2.0f;       // a halk részek emelése
constexpr float kMaxExciterMix = 0.12f;    // exciter keverés (a terv: legfeljebb 10–15 %)
constexpr float kMaxWarmthMix = 0.08f;     // meleg felharmonikusok keverése (Meleg karakter)
constexpr float kMaxBodyDb = 2.0f;         // Meleg karakter: mély polc 150 Hz
constexpr float kMaxPresenceDb = 1.5f;     // beszéd: jelenlét 2,2 kHz (élességnél nincs)
constexpr float kMaxLevelDb = 6.0f;        // beszéd szintkiegyenlítés
constexpr float kPeakCeiling = 0.891f;     // −1 dBFS: a mikrodinamika a csúcsot ennél nem viszi feljebb
constexpr double kCrossoverHz = 120.0;     // a mély sáv határa a mérésnél (korreláció)
// Fokozatos mono basszus: két egyforma mély polcszűrő az oldaljelen (75 Hz, teljes erővel 2 × −12 dB):
// 50 Hz alatt kb. −20 dB (közel mono), 80 Hz −11 dB, 120 Hz −4 dB, 150 Hz −2 dB, 200 Hz felett < 1 dB
constexpr double kMonoShelfHz = 75.0;
constexpr float kMonoShelfDb = 12.0f;
constexpr double kWidthShelfHz = 200.0;    // a tágítás 150 Hz felett hat

/** Karakter (a felhasználó ízlése; a Harman-vizsgálatban sem volt egyetlen görbe mindenkinek jó). */
enum Character : int32_t { kCharNatural = 0, kCharWarm = 1, kCharBright = 2, kCharSpacious = 3 };
/** Tartalom választó (0: automatikus felismerés). */
enum ContentSel : int32_t { kContentAuto = 0, kContentMusic = 1, kContentSpeech = 2, kContentArchive = 3 };
/** A használt profil (a diagnosztikában is). */
enum Profile : uint32_t { kProfMusic = 0, kProfMusicDense = 1, kProfMusicDynamic = 2, kProfSpeech = 3, kProfArchive = 4 };

/** A szabályok súlyai tartalmi profilonként (docs/plans/magic-touch.md, „Tartalmi profilok”). */
struct ProfileWeights { float lowMid, air, exciter, transient, upward, width, warmth; bool speech; };
inline const ProfileWeights& profileWeights(uint32_t p) {
    static const ProfileWeights w[5] = {
        //  mély-k. levegő exciter tranz. emelés tágítás meleg  beszéd
        { 1.0f, 1.0f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, false },   // zene
        { 1.0f, 1.0f, 0.3f, 0.6f, 1.0f, 1.0f, 0.5f, false },   // túltömörített zene: nincs erős exciter, visszafogott tranziens
        { 0.5f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, false },   // akusztikus / élő: minimális beavatkozás
        { 1.0f, 0.3f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, true  },   // beszéd: érthetőség, stabil szint
        { 1.0f, 0.5f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, false },   // archív: sávszélesség és élet, zajkiemelés nélkül
    };
    return w[std::min<uint32_t>(p, 4)];
}

/** A Karakter hatása a szabályokra (minden a kemény korlátokon belül). */
struct CharacterWeights { float airMax, airThr, airTilt, exciterBias, warmth, bodyDb, lowMid, widthThr, widthSlope; };
inline const CharacterWeights& characterWeights(int32_t c) {
    static const CharacterWeights w[4] = {
        // levegő: max, küszöb, dőlés; exciter, meleg, mély polc, mély-k., tágítás: küszöb, meredekség
        { 2.0f,  -8.0f,  0.0f, 0.0f, 0.0f, 0.0f,       1.0f, -14.0f, 0.15f },   // Természetes
        { 1.0f, -10.0f, -1.0f, 0.0f, 1.0f, kMaxBodyDb, 0.6f, -14.0f, 0.15f },   // Meleg
        { 2.5f,  -2.0f,  0.0f, 0.4f, 0.0f, 0.0f,       1.0f, -14.0f, 0.15f },   // Fényes
        { 2.0f,  -8.0f,  0.0f, 0.0f, 0.0f, 0.0f,       1.0f,  -8.0f, 0.25f },   // Tágas
    };
    return w[std::clamp(c, 0, 3)];
}

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
    // 3–4. ütem
    uint32_t content = kProfMusic; // a használt profil (Profile)
    uint32_t contentAuto = 1;      // 1: automatikus felismerés
    float speechScore = 0;         // 0..1 (beszéd-jelleg)
    float bandwidthHz = 20000;     // a spektrum letörése (archív felvétel jele)
    float transientDb = 0;         // a tranziens-kiemelés legnagyobb megengedett értéke
    float transientNowDb = 0;      // pillanatnyi (az utolsó időszak legnagyobbja)
    float upwardDb = 0;            // a halk részek emelésének legnagyobb megengedett értéke
    float upwardNowDb = 0;
    float exciterMix = 0;          // 0..1
    float warmthMix = 0;           // 0..1
    float bodyDb = 0;              // Meleg karakter: mély polc
    float presenceDb = 0;          // beszéd: jelenlét
    float lowCut = 0;              // beszéd: mélyvágás (0..1)
    float levelDb = 0;             // beszéd szintkiegyenlítés
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
    char b[1024];
    std::snprintf(b, sizeof(b),
                  "{\"valid\":%u,\"silent\":%u,\"lufs\":%.1f,\"lra\":%.1f,\"crest\":%.1f,\"lowMid\":%.2f,\"lowMidHz\":%.0f,"
                  "\"harsh\":%.2f,\"sib\":%.2f,\"air\":%.1f,\"hiss\":%.2f,\"lfCorr\":%.2f,\"sideMid\":%.1f,"
                  "\"cut\":%.2f,\"airGain\":%.2f,\"deess\":%.2f,\"deessNow\":%.2f,\"mono\":%.2f,\"width\":%.2f,\"makeup\":%.2f,"
                  "\"content\":%u,\"contentAuto\":%u,\"speech\":%.2f,\"bw\":%.0f,\"trans\":%.2f,\"transNow\":%.2f,"
                  "\"up\":%.2f,\"upNow\":%.2f,\"exciter\":%.3f,\"warmth\":%.3f,\"body\":%.2f,\"presence\":%.2f,"
                  "\"lowCut\":%.2f,\"level\":%.2f}",
                  d.valid, d.silent, d.lufsShort, d.lra, d.crestDb, d.lowMidExcessDb, d.lowMidFreqHz, d.harshness, d.sibilance,
                  d.airRatioDb, d.hiss, d.lfCorr, d.sideMidDb, d.lowMidCutDb, d.airGainDb, d.deessDepthDb, d.deessNowDb,
                  d.monoBass, d.widthGainDb, d.makeupDb, d.content, d.contentAuto, d.speechScore, d.bandwidthHz,
                  d.transientDb, d.transientNowDb, d.upwardDb, d.upwardNowDb, d.exciterMix, d.warmthMix, d.bodyDb,
                  d.presenceDb, d.lowCut, d.levelDb);
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
        for (int c = 0; c < 2; ++c) {
            designHighpass(lowCut_[c], fs, 80.0, std::sqrt(0.5));
            // exciter: a 3–9 kHz sáv páros felharmonikusai 6 kHz felett (a 3–5 kHz-et nem emeli)
            designHighpass(exHp_[c], fs, std::min(3000.0, 0.2 * fs), std::sqrt(0.5));
            designLowpass(exLp_[c], fs, std::min(9000.0, 0.2 * fs), std::sqrt(0.5));
            designHighpass(exPost_[c], fs, std::min(6000.0, 0.3 * fs), std::sqrt(0.5));
            // meleg felharmonikusok: a 120–600 Hz sávból, 200 Hz felett
            designHighpass(wmHp_[c], fs, 120.0, std::sqrt(0.5));
            designLowpass(wmLp_[c], fs, 600.0, std::sqrt(0.5));
            designHighpass(wmPost_[c], fs, 200.0, std::sqrt(0.5));
        }
        exEnabled_ = fs >= 32000;
        exCoef_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.005 * fs)));                             // 5 ms
        wmCoef_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.020 * fs)));                             // 20 ms
        tfCoef_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.002 * fs)));                             // 2 ms
        tsCoef_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.040 * fs)));                             // 40 ms
        upAtt_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.005 * fs)));                              // 5 ms
        upRel_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.150 * fs)));                              // 150 ms
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
    void setControl(float amountPct, bool compare, bool crossfeedOn, int32_t character = kCharNatural,
                    int32_t content = kContentAuto) {
        amount_.store(std::clamp(amountPct, 0.0f, 100.0f) / 100.0f, std::memory_order_relaxed);
        compare_ = compare;
        crossfeed_.store(crossfeedOn, std::memory_order_relaxed);
        character_.store(std::clamp(character, 0, 3), std::memory_order_relaxed);
        content_.store(std::clamp(content, 0, 3), std::memory_order_relaxed);
    }

    /** Belső állapot törlése (bekapcsoláskor, hogy ne régi állapotból induljon). */
    void reset() {
        for (int c = 0; c < 2; ++c) {
            lowMid_[c].reset(); air_[c].reset(); deessHp_[c].reset(); sMono_[c].reset();
            lowCut_[c].reset(); body_[c].reset(); pres_[c].reset();
            exHp_[c].reset(); exLp_[c].reset(); exPost_[c].reset(); wmHp_[c].reset(); wmLp_[c].reset(); wmPost_[c].reset();
            exEnv_[c] = wmEnv_[c] = 0.0f;
        }
        sWide_.reset();
        scHp_.reset();
        eSc_ = eBb_ = 0.0f;
        gr_ = 0.0f;
        tFast_ = tSlow_ = upEnv_ = 0.0f;
        microG_ = 1.0f;
        cmpMix_ = compare_ ? 1.0f : 0.0f;
    }

    // ------------------------------------------------------------ valós idejű szál
    void process(float* L, float* R, size_t n) {
        n = std::min(n, kMaxBlock);
        pre_.write(L, R, static_cast<uint32_t>(n));
        std::memcpy(dryL_.data(), L, n * sizeof(float));
        std::memcpy(dryR_.data(), R, n * sizeof(float));

        float grMax = 0.0f, tMax = 0.0f, uMax = 0.0f;
        for (size_t off = 0; off < n; off += kSub) {
            const size_t m = std::min(kSub, n - off);
            stepParams();
            float* l = L + off;
            float* r = R + off;
            // 0. beszéd: mélyvágás 80 Hz alatt (áttűnéssel be és ki)
            if (lowCutActive_) {
                const float w = cur_.lowCut;
                for (size_t i = 0; i < m; ++i) {
                    l[i] += w * (lowCut_[0].process(l[i]) - l[i]);
                    r[i] += w * (lowCut_[1].process(r[i]) - r[i]);
                }
            }
            // 1. mély-közép vágás, Meleg: mély polc, beszéd: jelenlét, 2. levegő (0 dB-nél nem futnak: a jel érintetlen)
            if (lowMidActive_) {
                for (size_t i = 0; i < m; ++i) {
                    l[i] = lowMid_[0].process(l[i]);
                    r[i] = lowMid_[1].process(r[i]);
                }
            }
            if (bodyActive_) {
                for (size_t i = 0; i < m; ++i) {
                    l[i] = body_[0].process(l[i]);
                    r[i] = body_[1].process(r[i]);
                }
            }
            if (presActive_) {
                for (size_t i = 0; i < m; ++i) {
                    l[i] = pres_[0].process(l[i]);
                    r[i] = pres_[1].process(r[i]);
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
            // 4. exciter: páros felharmonikusok (h = sáv² / sáv RMS: a szinttel arányos, mint egy lágy telítés
            //    második felharmonikusa), csak a felharmonikusok sávjában keverve
            if (exActive_) exciter(l, r, m, exHp_, exLp_, exPost_, exEnv_, exCoef_, cur_.exMix);
            if (warmActive_) exciter(l, r, m, wmHp_, wmLp_, wmPost_, wmEnv_, wmCoef_, cur_.warmMix);
            // 5. mikrodinamika
            if (microActive_) microDynamics(l, r, m, tMax, uMax);
        }
        post_.write(L, R, static_cast<uint32_t>(n));   // a hangerő-kiegyenlítés előtti jel (az elemzésnek)
        // pillanatnyi értékek: a legnagyobb a legutóbbi közlés óta (az elemzés nullázza)
        storeMax(deessNow_, grMax);
        storeMax(transNow_, tMax);
        storeMax(upNow_, uMax);

        // 6. hangerő-kiegyenlítés (beszédnél a szintkiegyenlítéssel együtt)
        const float mkTarget = std::pow(10.0f, (tgt_.makeupDb.load(std::memory_order_relaxed) +
                                                tgt_.levelDb.load(std::memory_order_relaxed)) / 20.0f);
        if (mk_ != 1.0f || mkTarget != 1.0f) {
            for (size_t i = 0; i < n; ++i) {
                mk_ += 0.0005f * (mkTarget - mk_);
                L[i] *= mk_;
                R[i] *= mk_;
            }
        }
        // 7. Oldaljel (S): fokozatos mono basszus és tágítás. A középjel (M) nem változik, és 0 dB-es
        //    polcoknál az oldaljel sem: a rendben lévő felvétel fázisa és csúcsai érintetlenek maradnak
        if (msActive_) {
            for (size_t i = 0; i < n; ++i) {
                const float mid = 0.5f * (L[i] + R[i]);
                const float side = sWide_.process(sMono_[1].process(sMono_[0].process(0.5f * (L[i] - R[i]))));
                L[i] = mid + side;
                R[i] = mid - side;
            }
        }
        // 8. A/B áttűnés
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
        float lowCut = 0, bodyDb = 0, presDb = 0, exMix = 0, warmMix = 0, transDb = 0, upDb = 0, refDb = -20;
    };
    struct Targets {
        std::atomic<float> mono{0}, widthDb{0}, lowMidDb{0}, lowMidHz{200}, airDb{0}, deessDepth{0}, deessThresh{-6},
                           makeupDb{0};
        std::atomic<float> lowCut{0}, bodyDb{0}, presDb{0}, exMix{0}, warmMix{0}, transDb{0}, upDb{0}, refDb{-20}, levelDb{0};
    };

    static void storeMax(std::atomic<float>& a, float v) {
        if (v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed);
    }

    /** Páros felharmonikus-keltő: sáv (HP+LP) -> sáv² / RMS -> HP (az egyenszint és a különbségi hangok nélkül). */
    static void exciter(float* l, float* r, size_t m, Biquad* hp, Biquad* lp, Biquad* post, float* env, float coef, float mix) {
        float* ch[2] = { l, r };
        for (int c = 0; c < 2; ++c) {
            float* x = ch[c];
            float e = env[c];
            for (size_t i = 0; i < m; ++i) {
                const float b = lp[c].process(hp[c].process(x[i]));
                e += coef * (b * b - e);
                const float h = post[c].process(b * b / std::sqrt(e + 1e-12f));
                x[i] += mix * h;
            }
            env[c] = e;
        }
    }

    /**
     * Mikrodinamika (sztereó csatolt, a 32 mintás alblokkon belül lineárisan változó erősítés):
     * - tranziens-kiemelés: a gyors (2 ms) és a lassú (40 ms) teljesítmény-burkoló aránya (3 dB felett);
     * - a halk részek emelése: a 3 s-os szinthez képest 18 dB-lel halkabb részeken, a csend és a zajpadló kivételével.
     * A csúcs nem kerül −1 dBFS fölé (ha a bemenetben sem volt ott), így nincs klippelés.
     */
    void microDynamics(float* l, float* r, size_t m, float& tMax, float& uMax) {
        float tDb = 0.0f, uDb = 0.0f;
        if (cur_.transDb > 0.0f && tSlow_ > 1e-12f) {
            const float ratioDb = 10.0f * std::log10((tFast_ + 1e-12f) / tSlow_);
            tDb = std::clamp((ratioDb - 3.0f) * 0.4f, 0.0f, cur_.transDb);
        }
        if (cur_.upDb > 0.0f && upEnv_ > 1e-12f) {
            const float e = 10.0f * std::log10(upEnv_);
            uDb = std::clamp((cur_.refDb - 18.0f - e) * 0.25f, 0.0f, cur_.upDb) *
                  std::clamp((e - (cur_.refDb - 50.0f)) / 5.0f, 0.0f, 1.0f);
        }
        tMax = std::max(tMax, tDb);
        uMax = std::max(uMax, uDb);
        const float g0 = microG_, g1 = std::pow(10.0f, (tDb + uDb) / 20.0f), step = (g1 - g0) / static_cast<float>(m);
        for (size_t i = 0; i < m; ++i) {
            const float p = 0.5f * (l[i] * l[i] + r[i] * r[i]);
            tFast_ += tfCoef_ * (p - tFast_);
            tSlow_ += tsCoef_ * (p - tSlow_);
            upEnv_ += (p > upEnv_ ? upAtt_ : upRel_) * (p - upEnv_);
            float g = g0 + step * static_cast<float>(i + 1);
            const float pk = std::max(std::fabs(l[i]), std::fabs(r[i]));
            if (g > 1.0f && pk * g > kPeakCeiling) g = std::max(1.0f, kPeakCeiling / pk);
            l[i] *= g;
            r[i] *= g;
        }
        microG_ = g1;
    }

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
        cur_.lowCut += k * (tgt_.lowCut.load(std::memory_order_relaxed) - cur_.lowCut);
        cur_.bodyDb += k * (tgt_.bodyDb.load(std::memory_order_relaxed) - cur_.bodyDb);
        cur_.presDb += k * (tgt_.presDb.load(std::memory_order_relaxed) - cur_.presDb);
        cur_.exMix += k * (tgt_.exMix.load(std::memory_order_relaxed) - cur_.exMix);
        cur_.warmMix += k * (tgt_.warmMix.load(std::memory_order_relaxed) - cur_.warmMix);
        cur_.transDb += k * (tgt_.transDb.load(std::memory_order_relaxed) - cur_.transDb);
        cur_.upDb += k * (tgt_.upDb.load(std::memory_order_relaxed) - cur_.upDb);
        cur_.refDb += k * (tgt_.refDb.load(std::memory_order_relaxed) - cur_.refDb);
        // A fokozatok, amelyekhez nem kell új szűrő-együttható: csak akkor futnak, ha van mit tenniük
        const bool lc = cur_.lowCut > 0.0005f;
        if (lc && !lowCutActive_) { lowCut_[0].reset(); lowCut_[1].reset(); }
        lowCutActive_ = lc;
        const bool ex = exEnabled_ && cur_.exMix > 1e-4f;
        if (ex && !exActive_) for (int c = 0; c < 2; ++c) { exHp_[c].reset(); exLp_[c].reset(); exPost_[c].reset(); exEnv_[c] = 0; }
        exActive_ = ex;
        const bool wm = cur_.warmMix > 1e-4f;
        if (wm && !warmActive_) for (int c = 0; c < 2; ++c) { wmHp_[c].reset(); wmLp_[c].reset(); wmPost_[c].reset(); wmEnv_[c] = 0; }
        warmActive_ = wm;
        const bool md = cur_.transDb > 0.005f || cur_.upDb > 0.005f || microG_ != 1.0f;
        if (md && !microActive_) { tFast_ = tSlow_ = upEnv_ = 0.0f; microG_ = 1.0f; }
        microActive_ = md;
        // Új együtthatók, ha a simított érték érezhetően eltér a legutóbb beállítottól
        const Smoothed& a = applied_;
        if (coefDirty_ || std::fabs(cur_.lowMidDb - a.lowMidDb) > 0.01f || std::fabs(cur_.lowMidHz - a.lowMidHz) > 0.5f ||
            std::fabs(cur_.airDb - a.airDb) > 0.01f || std::fabs(cur_.mono - a.mono) > 0.001f ||
            std::fabs(cur_.widthDb - a.widthDb) > 0.01f || std::fabs(cur_.bodyDb - a.bodyDb) > 0.01f ||
            std::fabs(cur_.presDb - a.presDb) > 0.01f) {
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
            designLowShelf(body_[c], fs_, 150.0, cur_.bodyDb, 0.8);
            designPeaking(pres_[c], fs_, 2200.0, cur_.presDb, 1.2);
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
        const bool air = airEnabled_ && std::fabs(cur_.airDb) > 0.005f;
        if (air && !airActive_) { air_[0].reset(); air_[1].reset(); }
        airActive_ = air;
        const bool body = cur_.bodyDb > 0.005f;
        if (body && !bodyActive_) { body_[0].reset(); body_[1].reset(); }
        bodyActive_ = body;
        const bool pres = cur_.presDb > 0.005f;
        if (pres && !presActive_) { pres_[0].reset(); pres_[1].reset(); }
        presActive_ = pres;
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
        specFast_.fill(0.0);
        specFrames_ = 0;
        envN_ = envPos_ = 0;
        crestAvg_ = 0; crestN_ = 0;
        lvlRefP_ = 0; lvlRefValid_ = false;
        speechAvg_ = 0; group_ = kGroupMusic; pendingGroup_ = kGroupMusic; pendingSecs_ = 0; profile_ = kProfMusic;
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
                // a tartalomfelismeréshez: a 10 ms-os keretek szintje (burkoló, 100 Hz)
                envRing_[envPos_] = static_cast<float>(eb);
                envPos_ = (envPos_ + 1) % kEnvLen;
                envN_ = std::min(envN_ + 1, kEnvLen);
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
        // a tartalomfelismeréshez rövidebb átlag (τ ≈ 4 s)
        const double af = std::max(a, static_cast<double>(fftHop_) / (4.0 * static_cast<double>(aFs_)));
        for (int b = 0; b < kBands; ++b) {
            double p = 0;
            for (size_t k = bandLo_[b]; k <= bandHi_[b]; ++k) p += std::norm(fftWork_[k]);
            specAvg_[b] += a * (p - specAvg_[b]);
            specFast_[b] += af * (p - specFast_[b]);
        }
        ++specFrames_;
    }

    struct Decision {
        float lowMid = 0, lowMidHz = 200, air = 0, deess = 0, deessThr = -6, mono = 0, width = 0, makeup = 0;
        float lowCut = 0, body = 0, presence = 0, exciter = 0, warmth = 0, transient = 0, upward = 0, level = 0, refDb = -20;
    };
    enum Group : uint32_t { kGroupMusic = 0, kGroupSpeech = 1, kGroupArchive = 2 };

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
        st_.refDb = static_cast<float>(toDb(raw / cnt));   // a mikrodinamika viszonyítási szintje (3 s, súlyozatlan)
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
        level(I, n, silent);
        publish();
    }

    /**
     * Beszéd szintkiegyenlítés (EBU R128 szerinti mérés): a 3 s-os hangosság a felvétel hosszú távú (τ ≈ 50 s,
     * kapuzott) átlagához igazodik, legfeljebb ±6 dB-lel és 1 dB/s sebességgel. A hosszú távú hangosság így
     * nem változik (nem hangosít); szünetben a beállítás áll.
     */
    void level(float I, double n, bool silent) {
        const bool speech = profileWeights(profile_).speech;
        const double lBlk = -0.691 + toDb(blk_.kPre / n);
        if (speech && !silent && lBlk > -50.0) {
            const double refDb = lvlRefValid_ ? toDb(lvlRefP_) : lBlk;
            if (lBlk > refDb - 20.0) {   // relatív kapu: a halk részek nem húzzák le az átlagot
                const double p = std::pow(10.0, lBlk / 10.0);
                if (!lvlRefValid_) { lvlRefP_ = p; lvlRefValid_ = true; }
                lvlRefP_ += 0.002 * (p - lvlRefP_);
            }
        }
        float want = 0.0f;
        if (speech && lvlRefValid_) {
            const double lShort = blocks_.empty() ? -70.0 : static_cast<double>(dg_.lufsShort);
            if (silent || lShort < -50.0) want = st_.level;   // szünet: áll
            else want = std::clamp(static_cast<float>(toDb(lvlRefP_) - lShort), -kMaxLevelDb, kMaxLevelDb) * I;
        }
        st_.level = slew(st_.level, want, 0.1f);   // 1 dB/s
    }

    void restartMeasurements() {
        specAvg_.fill(0.0);
        specFast_.fill(0.0);
        envN_ = envPos_ = 0;
        crestN_ = 0;
        pendingSecs_ = 0;   // a profil marad, amíg az új szám nem egyértelmű
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
        // csúcstényező: lassú átlag (≈ 10 s) a döntésekhez, csak jel közben
        if (!dg_.silent) {
            crestAvg_ = crestN_ == 0 ? dg_.crestDb : crestAvg_ + 0.1f * (dg_.crestDb - crestAvg_);
            ++crestN_;
        }
        classifyContent();
    }

    /**
     * Beszéd-jelleg (0..1) a 10 ms-os burkolóból és a rövid átlagspektrumból:
     * - a halk keretek aránya (1 s-os szakaszonként az átlag felénél halkabb keretek; beszédben a szünetek miatt sok),
     * - a burkoló 4 Hz körüli (2,5–6,5 Hz) modulációja (a szótagok üteme),
     * - kevés mély (40–80 Hz) a 200 Hz–2 kHz sávhoz képest.
     */
    float speechFeatures() {
        if (envN_ < 300) return -1.0f;   // legalább 3 s
        const auto at = [&](size_t back) { return envRing_[(envPos_ + kEnvLen - 1 - back) % kEnvLen]; };
        // halk keretek aránya (az utolsó legfeljebb 5 s, 1 s-os szakaszok, a csendes szakaszok nélkül)
        double lowFrac = 0; int segs = 0;
        for (size_t s0 = 0; s0 + 100 <= std::min<size_t>(envN_, 500); s0 += 100) {
            double mean = 0;
            for (size_t i = 0; i < 100; ++i) mean += std::sqrt(static_cast<double>(at(s0 + i)));
            mean /= 100.0;
            if (20.0 * std::log10(mean + 1e-12) < -50.0) continue;
            int low = 0;
            for (size_t i = 0; i < 100; ++i) if (std::sqrt(static_cast<double>(at(s0 + i))) < 0.5 * mean) ++low;
            lowFrac += low / 100.0; ++segs;
        }
        if (segs == 0) return -1.0f;
        lowFrac /= segs;
        // a burkoló modulációs spektruma (az utolsó 2,56 s, Hann ablak)
        constexpr size_t N = 256;
        const size_t nn = std::min<size_t>(N, envN_);
        double mean = 0;
        for (size_t i = 0; i < nn; ++i) mean += std::sqrt(static_cast<double>(at(i)));
        mean /= static_cast<double>(nn);
        double e4 = 0, eAll = 0;
        for (size_t k = 1; k <= 30; ++k) {   // 0,39 .. 11,7 Hz
            std::complex<double> acc{};
            for (size_t i = 0; i < nn; ++i) {
                const double w = 0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / static_cast<double>(nn - 1));
                const double v = (std::sqrt(static_cast<double>(at(nn - 1 - i))) - mean) * w;
                acc += v * std::polar(1.0, -2 * kPi * static_cast<double>(k * i) / static_cast<double>(nn));
            }
            const double f = static_cast<double>(k) * 100.0 / static_cast<double>(nn), p = std::norm(acc);
            if (f >= 0.5) eAll += p;
            if (f >= 2.5 && f <= 6.5) e4 += p;
        }
        const double mod4 = eAll > 0 ? e4 / eAll : 0.0;
        // mély (40–80 Hz) a 200 Hz – 2 kHz sávokhoz képest (sávonkénti átlag)
        double lf = 0, mid = 0;
        for (int b = 3; b <= 6; ++b) lf += specFast_[b];
        for (int b = 10; b <= 20; ++b) mid += specFast_[b];
        const double lfRel = toDb(lf / 4.0) - toDb(mid / 11.0);
        lowFrac_ = static_cast<float>(lowFrac); mod4_ = static_cast<float>(mod4); lfRel_ = static_cast<float>(lfRel);
        const double s1 = std::clamp((lowFrac - 0.12) / 0.2, 0.0, 1.0);
        const double s2 = std::clamp((mod4 - 0.2) / 0.25, 0.0, 1.0);
        const double s3 = std::clamp((-lfRel - 12.0) / 12.0, 0.0, 1.0);
        return static_cast<float>(0.4 * s1 + 0.3 * s2 + 0.3 * s3);
    }

    /**
     * Tartalom: beszéd / zene / archív felvétel. Automatikus módban a profil csak akkor vált, ha az új tartalom
     * 8 másodpercig egyértelmű; a zenei alprofil (túltömörített, akusztikus / élő) a csúcstényezőből és az LRA-ból jön.
     */
    void classifyContent() {
        const float sc = speechFeatures();
        if (sc >= 0.0f) speechAvg_ += 0.3f * (sc - speechAvg_);
        dg_.speechScore = speechAvg_;
        const int32_t sel = content_.load(std::memory_order_relaxed);
        dg_.contentAuto = sel == kContentAuto ? 1u : 0u;
        if (sel == kContentMusic) { group_ = kGroupMusic; pendingSecs_ = 0; }
        else if (sel == kContentSpeech) { group_ = kGroupSpeech; pendingSecs_ = 0; }
        else if (sel == kContentArchive) { group_ = kGroupArchive; pendingSecs_ = 0; }
        else if (sc >= 0.0f && !dg_.silent && dg_.valid) {
            // archív jel: 14,5 kHz alatt letört spektrum, vagy közel mono, zajos felvétel
            const bool archive = dg_.bandwidthHz < 14500.0f || (dg_.sideMidDb < -35.0f && dg_.hiss > 0.5f);
            int cand = -1;
            if (speechAvg_ > 0.6f) cand = kGroupSpeech;
            else if (speechAvg_ < 0.4f) cand = archive ? kGroupArchive : kGroupMusic;
            if (cand >= 0) {
                if (static_cast<uint32_t>(cand) == pendingGroup_) ++pendingSecs_;
                else { pendingGroup_ = static_cast<uint32_t>(cand); pendingSecs_ = 1; }
                if (pendingSecs_ >= 8) group_ = pendingGroup_;
            } else {
                pendingSecs_ = 0;
            }
        }
        if (group_ == kGroupSpeech) profile_ = kProfSpeech;
        else if (group_ == kGroupArchive) profile_ = kProfArchive;
        else {
            // zenei alprofil (hiszterézissel); az LRA-hoz legalább 10 s előzmény kell
            const bool lraOk = stHist_.size() >= 100 && crestN_ >= 5;
            const float c = crestAvg_, lra = dg_.lra;
            uint32_t p = profile_ <= kProfMusicDynamic ? profile_ : kProfMusic;
            if (!lraOk) { /* új szám: az előző alprofil marad, amíg nincs elég előzmény */ }
            else if (p == kProfMusicDense) { if (c > 11.0f || lra > 6.0f) p = kProfMusic; }
            else if (p == kProfMusicDynamic) { if (lra < 10.0f && (c < 14.0f || lra < 5.0f)) p = kProfMusic; }
            else if (c < 10.0f && lra < 5.0f) p = kProfMusicDense;
            else if ((c > 15.0f && lra > 6.0f) || lra > 12.0f) p = kProfMusicDynamic;
            profile_ = p;
        }
        dg_.content = profile_;
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
        // sávszélesség: az első meredek letörés 5 kHz felett (≥ 12 dB egy harmad oktávon, a jel alá 30 dB-lel)
        double ref = 0;
        for (int b = 17; b <= 23; ++b) ref += db[b];
        ref /= 7.0;
        int top = 0;
        for (int b = 24; b < kBands && bandOk_[b]; ++b) top = b;
        int cut = top;
        for (int b = 24; b < top; ++b)
            if (db[b] - db[b + 1] >= 12.0 && db[b + 1] < ref - 30.0 && db[b] > ref - 45.0) { cut = b; break; }
        dg_.bandwidthHz = static_cast<float>(top == 0 ? 0.0 : bandCenter(cut) * std::pow(2.0, 1.0 / 6.0));
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
        // (sávkorlátos felvételnél a letörés előtti lejtő miatt a görbület nem mérvadó: csak a trendvonal feletti rész számít)
        if (cut > 28)
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
        const ProfileWeights& pw = profileWeights(profile_);
        const CharacterWeights& cw = characterWeights(character_.load(std::memory_order_relaxed));
        // mély-közép többlet: csak a trendvonal fölötti rész, 1 dB felett
        const float lowMid = -std::clamp((dg_.lowMidExcessDb - 1.0f) * 1.0f, 0.0f, kMaxLowMidCutDb) * pw.lowMid * cw.lowMid * I;
        st_.lowMid = slew(st_.lowMid, lowMid, 0.05f);   // 0,5 dB/s
        st_.lowMidHz = slew(st_.lowMidHz, dg_.lowMidFreqHz, 5.0f);
        // levegő: ha hiányzik, de nem zajos, nem éles, nem sziszegő és nem sávkorlátos a felvétel
        const bool bandLimited = dg_.airRatioDb < -35.0f;
        float air = bandLimited ? 0.0f : std::clamp((cw.airThr - dg_.airRatioDb) * 0.3f, 0.0f, std::min(cw.airMax, kMaxAirDb));
        air *= (1.0f - dg_.hiss) * (1.0f - 0.7f * dg_.harshness) * (1.0f - 0.5f * dg_.sibilance) * pw.air * I;
        air += cw.airTilt * I;   // Meleg: a magas enyhe visszafogása
        st_.air = slew(st_.air, air, 0.05f);
        // exciter: ahol a magas hiányzik (tompa vagy sávkorlátos felvétel), a 3–9 kHz sáv felharmonikusaival;
        // zajnál, élességnél és sziszegésnél vissza (a susogást és az érdességet ne erősítse)
        const float dull = bandLimited ? 1.0f : std::clamp((-10.0f - dg_.airRatioDb) / 10.0f, 0.0f, 1.0f);
        const float ex = kMaxExciterMix * std::clamp(dull * pw.exciter + cw.exciterBias * (pw.exciter > 0.0f ? 1.0f : 0.0f), 0.0f, 1.0f) *
                         (1.0f - dg_.hiss) * (1.0f - dg_.harshness) * (1.0f - 0.7f * dg_.sibilance) * I;
        st_.exciter = slew(st_.exciter, ex, 0.005f);   // 5 %/s
        // Meleg karakter: mély-közép felharmonikusok és mély polc, de nem a már dobozos felvételre
        const float boxy = std::clamp(dg_.lowMidExcessDb / 3.0f, 0.0f, 1.0f);
        const float warm = kMaxWarmthMix * cw.warmth * pw.warmth * (1.0f - boxy) * (1.0f - 0.5f * dg_.harshness) * I;
        st_.warmth = slew(st_.warmth, warm, 0.005f);
        st_.body = slew(st_.body, cw.bodyDb * (1.0f - boxy) * (pw.speech ? 0.0f : 1.0f) * I, 0.05f);
        // mikrodinamika: lapos dinamikánál (alacsony csúcstényező, alacsony LRA) finom tranziens-kiemelés és a halk
        // részek emelése; további kompresszió és limitálás nincs
        const bool lraOk = stHist_.size() >= 100 && crestN_ >= 5;
        const float lowCrest = lraOk ? std::clamp((12.0f - crestAvg_) / 4.0f, 0.0f, 1.0f) : 0.0f;
        const float lowLra = lraOk ? std::clamp((6.0f - dg_.lra) / 4.0f, 0.0f, 1.0f) : 0.0f;
        st_.transient = slew(st_.transient, kMaxTransientDb * lowCrest * (0.5f + 0.5f * lowLra) * pw.transient *
                                                (1.0f - 0.5f * dg_.harshness) * I, 0.05f);
        st_.upward = slew(st_.upward, kMaxUpwardDb * lowLra * (1.0f - dg_.hiss) * pw.upward * I, 0.05f);
        // beszéd: mélyvágás 80 Hz alatt, enyhe jelenlét 2,2 kHz-en, ha nincs élesség és sziszegés
        st_.lowCut = slew(st_.lowCut, pw.speech ? std::min(1.0f, 2.0f * I) : 0.0f, 0.05f);
        const float pres = pw.speech ? kMaxPresenceDb * std::clamp(1.0f - dg_.harshness / 0.3f, 0.0f, 1.0f) *
                                           std::clamp(1.0f - dg_.sibilance / 0.3f, 0.0f, 1.0f) * (1.0f - 0.5f * dg_.hiss) * I
                                     : 0.0f;
        st_.presence = slew(st_.presence, pres, 0.05f);
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
        const float width = (cf || dg_.sideMidDb < -40.0f || dg_.sideMidDb > cw.widthThr)
                                ? 0.0f : std::clamp((cw.widthThr - dg_.sideMidDb) * cw.widthSlope, 0.0f, kMaxWidthDb) * pw.width * I;
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
        tgt_.lowCut.store(st_.lowCut, std::memory_order_relaxed);
        tgt_.bodyDb.store(st_.body, std::memory_order_relaxed);
        tgt_.presDb.store(st_.presence, std::memory_order_relaxed);
        tgt_.exMix.store(st_.exciter, std::memory_order_relaxed);
        tgt_.warmMix.store(st_.warmth, std::memory_order_relaxed);
        tgt_.transDb.store(st_.transient, std::memory_order_relaxed);
        tgt_.upDb.store(st_.upward, std::memory_order_relaxed);
        tgt_.refDb.store(st_.refDb, std::memory_order_relaxed);
        tgt_.levelDb.store(st_.level, std::memory_order_relaxed);
        dg_.lowCut = st_.lowCut;
        dg_.bodyDb = st_.body;
        dg_.presenceDb = st_.presence;
        dg_.exciterMix = st_.exciter;
        dg_.warmthMix = st_.warmth;
        dg_.transientDb = st_.transient;
        dg_.transientNowDb = transNow_.exchange(0.0f, std::memory_order_relaxed);
        dg_.upwardDb = st_.upward;
        dg_.upwardNowDb = upNow_.exchange(0.0f, std::memory_order_relaxed);
        dg_.levelDb = st_.level;
        dg_.lowMidCutDb = st_.lowMid;
        dg_.airGainDb = st_.air;
        dg_.deessDepthDb = st_.deess;
        dg_.deessNowDb = deessNow_.exchange(0.0f, std::memory_order_relaxed);
        dg_.monoBass = st_.mono;
        dg_.widthGainDb = st_.width;
        dg_.makeupDb = st_.makeup;
        diag_.write(dg_);
    }

    // valós idejű állapot
    uint32_t fs_ = 48000;
    Biquad sMono_[2], sWide_;             // oldaljel: két mély polc (mono basszus), magas polc (tágítás)
    Biquad lowMid_[2], air_[2], deessHp_[2], scHp_;
    Biquad lowCut_[2], body_[2], pres_[2];
    Biquad exHp_[2], exLp_[2], exPost_[2], wmHp_[2], wmLp_[2], wmPost_[2];
    bool airEnabled_ = true, coefDirty_ = true, compare_ = false, msActive_ = false, lowMidActive_ = false, airActive_ = false;
    bool exEnabled_ = true, lowCutActive_ = false, bodyActive_ = false, presActive_ = false, exActive_ = false,
         warmActive_ = false, microActive_ = false;
    float subCoef_ = 0.1f, scCoef_ = 0.01f, grAtt_ = 0.5f, grRel_ = 0.01f, cmpStep_ = 0.001f;
    float exCoef_ = 0.004f, wmCoef_ = 0.001f, tfCoef_ = 0.01f, tsCoef_ = 0.0005f, upAtt_ = 0.004f, upRel_ = 0.0001f;
    float eSc_ = 0, eBb_ = 0, gr_ = 0, mk_ = 1.0f, cmpMix_ = 0.0f;
    float exEnv_[2] = { 0, 0 }, wmEnv_[2] = { 0, 0 }, tFast_ = 0, tSlow_ = 0, upEnv_ = 0, microG_ = 1.0f;
    Smoothed cur_, applied_;
    std::vector<float> dryL_, dryR_;

    // szálak között
    std::atomic<uint32_t> fsShared_{0};
    std::atomic<float> amount_{0.5f};
    std::atomic<bool> crossfeed_{false};
    std::atomic<int32_t> character_{kCharNatural}, content_{kContentAuto};
    std::atomic<float> deessNow_{0.0f}, transNow_{0.0f}, upNow_{0.0f};
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
    std::array<double, kBands> specAvg_{}, specFast_{};
    size_t specFrames_ = 0;
    static constexpr size_t kEnvLen = 512;      // 5,12 s a 10 ms-os keretekből
    std::array<float, kEnvLen> envRing_{};
    size_t envN_ = 0, envPos_ = 0;
    float crestAvg_ = 0, speechAvg_ = 0, lowFrac_ = 0, mod4_ = 0, lfRel_ = 0;
    size_t crestN_ = 0;
    double lvlRefP_ = 0;
    bool lvlRefValid_ = false;
    uint32_t group_ = kGroupMusic, pendingGroup_ = kGroupMusic, pendingSecs_ = 0, profile_ = kProfMusic;
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
