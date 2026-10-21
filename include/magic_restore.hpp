#pragma once
/**
 * @file magic_restore.hpp
 * @brief Magic Touch, Restoration (docs/plans/magic-touch.md, 5. ütem): de-hum, kattanásjavítás, zajcsökkentés.
 *
 * Valós idejű fokozatok:
 *  - Dehum: keskeny vágások a felismert hálózati alapfrekvencián (50/60 Hz) és felharmonikusain (késleltetés nélkül).
 *  - RestoreDelay: kattanásjavítás (2,7 ms előretekintéssel) és RNNoise (beszéd, 48 kHz; a 0.2-es változattal
 *    összesen 32,7 ms, a 0.1-essel 22,7 ms); csak akkor késleltet,
 *    ha ezek közül valamelyik dolgozik, a be- és kikapcsolás rövid le- és felúsztatással történik.
 *  - BandNr: sávonkénti, kíméletes zajcsökkentés (lefelé tágítás 250 Hz felett, öt sávban), késleltetés nélkül:
 *    a sáv a zajpadló közelében legfeljebb a megadott mélységgel halkul (lépcsős magas polcokkal), a zajpadló
 *    fölött 18 dB-lel érintetlen.
 *
 * Elemzés (háttérszál): HumDetector (nagy felbontású átlagspektrum, a halk részekből is), NrMeter (sávonkénti
 * zajpadló minimum-statisztikával és állandóság-vizsgálattal), ClickDetector (a kattanások gyakorisága).
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "magic_dsp.hpp"
#include "rnn_denoise.hpp"

namespace ee_analyzer::magic {

// ------------------------------------------------------------------ korlátok
constexpr int kHumHarmonics = 8;           // alap + 7 felharmonikus
constexpr float kMaxHumCutDb = 24.0f;      // felharmonikusonként (keskeny, 2,5–4,5 Hz széles vágás)
constexpr float kMaxNrDb = 6.0f;           // zajcsökkentés (a terv: legfeljebb 4–6 dB)
constexpr double kClickK = 20.0;           // kattanás: a második differencia a jellemző szintjének 20-szorosa felett
constexpr int kNrBands = 5;                // 250 Hz–1 kHz (csak beszédnél), 1–3, 3–6, 6–10, 10–20 kHz
inline double nrEdge(int b) { static const double e[kNrBands + 1] = { 250, 1000, 3000, 6000, 10000, 20000 }; return e[b]; }

// ------------------------------------------------------------------ kattanás-felismerő
/**
 * Lineáris predikció (16. rendű, 10 ms-onként a legutóbbi ~21 ms-ból) maradékában elszigetelt, rövid
 * (legfeljebb 1 ms) kitörés a jellemző szint (robusztus RMS) sokszorosa felett, utána gyors visszaállás.
 * A hosszabb tranziens (pl. cintányér) nem kattanás, és a szabályos ismétlődés (zöngés beszéd impulzusai) sem.
 */
class ClickDetector {
public:
    static constexpr int kOrder = 16;
    static constexpr size_t kHist = 1024;   // 2 hatvány

    void configure(double fs) {
        maxLen_ = std::max<int64_t>(4, static_cast<int64_t>(0.001 * fs));
        post_ = std::max<int64_t>(4, static_cast<int64_t>(0.00067 * fs));
        cool_ = std::max<int64_t>(4, static_cast<int64_t>(0.002 * fs));
        perMin_ = static_cast<int64_t>(0.0025 * fs);
        perMax_ = static_cast<int64_t>(0.015 * fs);
        update_ = std::max<int64_t>(64, static_cast<int64_t>(0.01 * fs));
        sigCoef_ = 1.0 - std::exp(-1.0 / (0.02 * fs));
        longCoef_ = 1.0 - std::exp(-1.0 / (2.0 * fs));
        win_.resize(kHist);
        for (size_t i = 0; i < kHist; ++i) win_[i] = 0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / static_cast<double>(kHist - 1));
        reset();
    }
    void reset() {
        hist_.fill(0); a_.fill(0); n_ = 0; sig2_ = long2_ = 0; inEvent_ = false; coolUntil_ = 0; last1_ = last2_ = -1000000;
    }
    /** Egy minta (t: abszolút sorszám). true: véget ért egy kattanás, a [start, end] tartományban. */
    bool step(double x, int64_t t, double k, int64_t& start, int64_t& end) {
        // maradék a jelenlegi együtthatókkal (a minta előtti kOrder mintából)
        double pred = 0;
        for (int j = 0; j < kOrder; ++j) pred += a_[static_cast<size_t>(j)] * hist_[(n_ - 1 - static_cast<size_t>(j)) & (kHist - 1)];
        const double e = x - pred;
        hist_[n_ & (kHist - 1)] = x;
        ++n_;
        if (n_ >= kHist && static_cast<int64_t>(n_) % update_ == 0) analyze();
        // jellemző szint: rövid (20 ms) robusztus RMS, de legalább a hosszú (2 s) tizede (a csend utáni hangkezdet nem kattanás)
        const double a = std::fabs(e), sig = std::max(std::sqrt(sig2_), 0.1 * std::sqrt(long2_));
        bool done = false;
        if (!inEvent_) {
            if (n_ > kHist && t >= coolUntil_ && sig > 1e-7 && a > k * sig && a > 1e-4) {
                inEvent_ = true; evStart_ = t; lastHigh_ = t; thr_ = 0.3 * k * sig;
            } else {
                sig2_ += sigCoef_ * (std::min(e * e, 9.0 * sig2_ + 1e-14) - sig2_);   // robusztus: a kiugrás nem húzza fel
                long2_ += longCoef_ * (std::min(e * e, 9.0 * long2_ + 1e-14) - long2_);
            }
        } else {
            if (a > thr_) lastHigh_ = t;
            if (t - evStart_ > maxLen_ + post_) {          // túl hosszú: tranziens, nem kattanás
                inEvent_ = false; coolUntil_ = t + cool_;
            } else if (t - lastHigh_ >= post_) {
                inEvent_ = false; coolUntil_ = t + cool_;
                // szabályos ismétlődés (2,5–15 ms, ±15 %): zöngés impulzus, nem kattanás
                const int64_t d1 = evStart_ - last1_, d0 = last1_ - last2_;
                const bool periodic = (d1 >= perMin_ && d1 <= perMax_) &&
                                      ((d0 >= perMin_ && d0 <= perMax_ && std::llabs(d1 - d0) * 100 <= 15 * d0) || d1 <= perMax_ / 2);
                last2_ = last1_; last1_ = evStart_;
                if (!periodic && lastHigh_ - evStart_ + 1 <= maxLen_) { start = evStart_; end = lastHigh_; done = true; }
            }
        }
        return done;
    }
    /** A javításhoz szükséges legnagyobb visszatekintés (minta): a felismerés ennyivel a kattanás után jelez. */
    int64_t lookback() const { return maxLen_ + post_ + 8; }
    /** A jelenlegi predikciós együtthatók: x̂[t] = Σ a[k]·x[t − 1 − k]. */
    const std::array<double, kOrder>& coeffs() const { return a_; }
    /** A legutóbbi kattanás küszöbe (a maradék abszolút értékére). */
    double threshold() const { return thr_; }

private:
    /** Autokorreláció (Hann ablak, enyhe zajpadló) és Levinson–Durbin; sávszélesség-tágítás 0,98. */
    void analyze() {
        std::array<double, kOrder + 1> r{};
        const size_t base = n_ - kHist;
        for (size_t i = 0; i < kHist; ++i) w_[i] = hist_[(base + i) & (kHist - 1)] * win_[i];
        for (int lag = 0; lag <= kOrder; ++lag) {
            double acc = 0;
            for (size_t i = static_cast<size_t>(lag); i < kHist; ++i) acc += w_[i] * w_[i - static_cast<size_t>(lag)];
            r[static_cast<size_t>(lag)] = acc;
        }
        if (r[0] <= 1e-12) { a_.fill(0); return; }
        r[0] *= 1.0001;
        std::array<double, kOrder> c{}, tmp{};
        double err = r[0];
        for (int i = 0; i < kOrder; ++i) {
            double acc = r[static_cast<size_t>(i + 1)];
            for (int j = 0; j < i; ++j) acc -= c[static_cast<size_t>(j)] * r[static_cast<size_t>(i - j)];
            const double ki = acc / err;
            tmp = c;
            c[static_cast<size_t>(i)] = ki;
            for (int j = 0; j < i; ++j) c[static_cast<size_t>(j)] = tmp[static_cast<size_t>(j)] - ki * tmp[static_cast<size_t>(i - 1 - j)];
            err *= (1.0 - ki * ki);
            if (err <= 1e-14) break;
        }
        double g = 0.98;
        for (int j = 0; j < kOrder; ++j) { a_[static_cast<size_t>(j)] = c[static_cast<size_t>(j)] * g; g *= 0.98; }
    }

    std::array<double, kHist> hist_{}, w_{};
    std::vector<double> win_;
    std::array<double, kOrder> a_{};
    size_t n_ = 0;
    double sig2_ = 0, long2_ = 0, thr_ = 0, sigCoef_ = 0.001, longCoef_ = 1e-5;
    bool inEvent_ = false;
    int64_t evStart_ = 0, lastHigh_ = 0, coolUntil_ = 0, maxLen_ = 48, post_ = 32, cool_ = 96, perMin_ = 120, perMax_ = 720,
            update_ = 480, last1_ = -1000000, last2_ = -1000000;
};

// ------------------------------------------------------------------ de-hum (valós idejű)
class Dehum {
public:
    void setSampleRate(double fs) { fs_ = fs; cur_.fill(0); applied_.fill(0); f0_ = appliedF0_ = 0; active_ = false; reset(); }
    void reset() { for (auto& h : f_) for (auto& c : h) c.reset(); }
    /** A cél (f0 Hz, felharmonikusonkénti vágás dB-ben, ≥ 0) felé simítás alblokkonként; k: simítási együttható. */
    void step(float f0, const float* depth, float k) {
        bool any = false, redesign = std::fabs(f0 - appliedF0_) > 0.02f;
        f0_ = f0;
        for (int h = 0; h < kHumHarmonics; ++h) {
            cur_[h] += k * (depth[h] - cur_[h]);
            if (cur_[h] < 1e-3f && depth[h] == 0.0f) cur_[h] = 0.0f;
            if (std::fabs(cur_[h] - applied_[h]) > 0.05f || (cur_[h] == 0.0f && applied_[h] != 0.0f)) redesign = true;
            any = any || cur_[h] > 0.0f;
        }
        if (any && !active_) reset();
        active_ = any && f0_ > 0.0f;
        if (active_ && redesign) design();
    }
    bool active() const { return active_; }
    void process(float* l, float* r, size_t m) {
        if (!active_) return;
        for (int h = 0; h < kHumHarmonics; ++h) {
            if (applied_[h] <= 0.0f) continue;
            BiquadD& a = f_[h][0];
            BiquadD& b = f_[h][1];
            for (size_t i = 0; i < m; ++i) {
                l[i] = static_cast<float>(a.process(l[i]));
                r[i] = static_cast<float>(b.process(r[i]));
            }
        }
    }
    float maxCut() const { float m = 0; for (float v : applied_) m = std::max(m, v); return m; }
private:
    void design() {
        appliedF0_ = f0_;
        for (int h = 0; h < kHumHarmonics; ++h) {
            applied_[h] = cur_[h];
            const double f = static_cast<double>(f0_) * (h + 1);
            if (applied_[h] <= 0.0f || f >= 0.45 * fs_) { applied_[h] = 0.0f; continue; }
            const double bw = 2.5 + 0.25 * h;   // Hz
            for (auto& c : f_[h]) designPeaking(c, fs_, f, -applied_[h], f / bw);
        }
    }
    double fs_ = 48000;
    BiquadD f_[kHumHarmonics][2];
    std::array<float, kHumHarmonics> cur_{}, applied_{};
    float f0_ = 0, appliedF0_ = 0;
    bool active_ = false;
};

// ------------------------------------------------------------------ sávonkénti zajcsökkentés (valós idejű)
class BandNr {
public:
    void setSampleRate(double fs) {
        fs_ = fs;
        for (int b = 0; b < kNrBands; ++b) {
            const double lo = nrEdge(b), hi = std::min(nrEdge(b + 1), 0.45 * fs);
            designHighpass(scHp_[b], fs, lo, std::sqrt(0.5));
            designLowpass(scLp_[b], fs, hi, std::sqrt(0.5));
        }
        // a teljesítmény-burkoló közel szimmetrikus (5 / 20 ms): a zaj pillanatnyi csúcsai ne nyissák ki a kaput
        att_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.005 * fs)));
        rel_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.020 * fs)));
        open_ = static_cast<float>(1.0 - std::exp(-32.0 / (0.003 * fs)));
        close_ = static_cast<float>(1.0 - std::exp(-32.0 / (0.050 * fs)));
        reset();
    }
    void reset() {
        for (int b = 0; b < kNrBands; ++b) { scHp_[b].reset(); scLp_[b].reset(); for (auto& c : stage_[b]) c.reset(); }
        env_.fill(0); gain_.fill(0); applied_.fill(0); active_ = false;
    }
    /** depth: sávonkénti legnagyobb csillapítás (dB, ≥ 0), floor: zajpadló (dBFS, teljesítmény). */
    void process(float* l, float* r, size_t m, const float* depth, const float* floorDb, float& maxNowDb) {
        bool want = false;
        for (int b = 0; b < kNrBands; ++b) want = want || depth[b] > 0.01f || gain_[b] < -0.001f;
        if (!want) { if (active_) reset(); return; }
        if (!active_) { reset(); active_ = true; }
        // oldallánc (mono) és a sávok burkolója
        for (size_t i = 0; i < m; ++i) {
            const float x = 0.5f * (l[i] + r[i]);
            for (int b = 0; b < kNrBands; ++b) {
                if (depth[b] <= 0.01f && gain_[b] >= -0.001f) continue;
                const float y = scLp_[b].process(scHp_[b].process(x)), p = y * y;
                env_[b] += (p > env_[b] ? att_ : rel_) * (p - env_[b]);
            }
        }
        // sávonkénti erősítés: a zajpadló + 6 dB alatt (a zaj ingadozása) a teljes mélység, + 18 dB felett érintetlen
        for (int b = 0; b < kNrBands; ++b) {
            float g = 0.0f;
            if (depth[b] > 0.01f) {
                const float e = 10.0f * std::log10(env_[b] + 1e-20f);
                g = -depth[b] * std::clamp((floorDb[b] + 18.0f - e) / 12.0f, 0.0f, 1.0f);
            }
            gain_[b] += (g > gain_[b] ? open_ : close_) * (g - gain_[b]);
            if (gain_[b] > -1e-4f && g == 0.0f) gain_[b] = 0.0f;
            maxNowDb = std::max(maxNowDb, -gain_[b]);
        }
        // lépcsős magas polcok: a k. polc a k. sáv alsó határán a (G_k − G_{k−1}) különbséggel (0 dB-nél nem fut)
        float prev = 0.0f;
        for (int b = 0; b < kNrBands; ++b) {
            const float d = gain_[b] - prev;
            prev = gain_[b];
            if (d == 0.0f) { if (applied_[b] != 0.0f) { applied_[b] = 0.0f; stage_[b][0].reset(); stage_[b][1].reset(); } continue; }
            if (std::fabs(d - applied_[b]) > 0.02f) {
                if (applied_[b] == 0.0f) { stage_[b][0].reset(); stage_[b][1].reset(); }
                applied_[b] = d;
                for (auto& c : stage_[b]) designHighShelf(c, fs_, nrEdge(b), d, 1.0);
            }
            for (size_t i = 0; i < m; ++i) {
                l[i] = stage_[b][0].process(l[i]);
                r[i] = stage_[b][1].process(r[i]);
            }
        }
    }
private:
    double fs_ = 48000;
    Biquad scHp_[kNrBands], scLp_[kNrBands], stage_[kNrBands][2];
    std::array<float, kNrBands> env_{}, gain_{}, applied_{};
    float att_ = 0.007f, rel_ = 0.0003f, open_ = 0.3f, close_ = 0.01f;
    bool active_ = false;
};

// ------------------------------------------------------------------ kattanásjavítás és RNNoise (késleltetéssel)
class RestoreDelay {
public:
    static constexpr size_t kRing = 4096;   // 2 hatvány, > a teljes késleltetés (legfeljebb ≈ 2050) + egy RNNoise keret
    static constexpr size_t kMask = kRing - 1;

    RestoreDelay() {
        for (int c = 0; c < 2; ++c) rnn_[c] = std::make_unique<RnnDenoiser>();
        (void)RnnDenoiser::delay();   // az egyszeri mérés itt (nem a valós idejű szálon) fut le
        rawRing_.assign(2 * kRing, 0.0f);
        wkRing_.assign(2 * kRing, 0.0f);
        rnnRing_.assign(2 * kRing, 0.0f);
        mat_.assign(static_cast<size_t>(kMaxGap) * kMaxGap, 0.0);
        vec_.assign(static_cast<size_t>(kMaxGap) + ClickDetector::kOrder + 1, 0.0);
        rhs_.assign(static_cast<size_t>(kMaxGap), 0.0);
    }

    void setSampleRate(uint32_t fs) {
        fs_ = fs;
        for (auto& d : clk_) d.configure(fs);
        dc_ = static_cast<int64_t>(std::max<double>(16.0, std::ceil(0.0027 * fs)));   // a kattanásjavítás előretekintése
        rnnUsable_ = fs == 48000 && RnnDenoiser::available();
        rnnDelay_ = rnnUsable_ ? static_cast<int64_t>(RnnDenoiser::delay()) : 0;
        dtRnn_ = dc_ + (rnnUsable_ ? static_cast<int64_t>(RnnDenoiser::kFrame) + rnnDelay_ : 0);
        fadeStep_ = 1.0f / std::max(1.0f, 0.005f * static_cast<float>(fs));   // 5 ms
        wCoef_ = 1.0f / std::max(1.0f, 0.3f * static_cast<float>(fs));        // a keverés 0,3 s alatt áll be
        reset();
    }
    void reset() {
        std::fill(rawRing_.begin(), rawRing_.end(), 0.0f);
        std::fill(wkRing_.begin(), wkRing_.end(), 0.0f);
        std::fill(rnnRing_.begin(), rnnRing_.end(), 0.0f);
        for (auto& d : clk_) d.reset();
        mode_ = 0; fade_ = 1.0f; switching_ = false; w_ = 0.0f; rnnWarm_ = 0; frameFill_ = 0;
    }

    bool rnnUsable() const { return rnnUsable_; }
    /** Mód: 0 nincs, 1 kattanásjavítás, 2 RNNoise (és kattanásjavítás). A mód késleltetése (minta). */
    int64_t latency(int mode) const { return mode == 2 && rnnUsable_ ? dtRnn_ : mode >= 1 ? dc_ : 0; }
    int mode() const { return mode_; }

    /**
     * L/R: a de-hum utáni jel (helyben a kimenet), dryL/dryR: a nyers bemenet (bekapcsolva a késleltetett nyers jelre
     * cserélődik, hogy az A/B összehasonlítás egy időben legyen). want: a kívánt mód (0, 1, 2; lásd latency()).
     * rnnW: az RNNoise súlya (0..1), rnnAtt: a zaj legnagyobb csillapítása lineárisan (pl. 0,5 = −6 dB).
     * Módváltáskor rövid le- és felúsztatás (a késleltetés változik).
     */
    void process(float* L, float* R, float* dryL, float* dryR, size_t n, int want, bool declick, double clickK,
                 float rnnW, float rnnAtt, uint32_t& clicksFixed) {
        if (want == 2 && !rnnUsable_) want = declick ? 1 : 0;
        if (want == 0 && mode_ == 0 && !switching_ && fade_ == 1.0f) {
            // nincs mit tenni: csak az előzmény (a bekapcsoláskor innen olvas, így nincs csendes rés)
            for (size_t i = 0; i < n; ++i, ++t_) {
                const size_t wi = static_cast<size_t>(t_) & kMask;
                rawRing_[wi] = dryL[i]; rawRing_[kRing + wi] = dryR[i];
                wkRing_[wi] = L[i]; wkRing_[kRing + wi] = R[i];
            }
            return;
        }
        const bool rnnRun = mode_ == 2 && rnnW > 0.0f;
        if (!rnnRun) rnnWarm_ = 0;
        float* io[2] = { L, R };
        float* dry[2] = { dryL, dryR };
        for (size_t i = 0; i < n; ++i, ++t_) {
            const size_t wi = static_cast<size_t>(t_) & kMask;
            rawRing_[wi] = dryL[i]; rawRing_[kRing + wi] = dryR[i];
            wkRing_[wi] = L[i]; wkRing_[kRing + wi] = R[i];
            // állapotváltás: lehalkítás, váltás, felhangosítás
            if (want != mode_ && !switching_) switching_ = true;
            if (switching_) {
                fade_ -= fadeStep_;
                if (fade_ <= 0.0f) {
                    fade_ = 0.0f; switching_ = false; mode_ = want;
                    for (auto& d : clk_) d.reset();
                    frameFill_ = 0; rnnWarm_ = 0; w_ = 0.0f;
                }
            } else if (fade_ < 1.0f) {
                fade_ = std::min(1.0f, fade_ + fadeStep_);
            }
            if (mode_ == 0) {
                L[i] *= fade_; R[i] *= fade_;
                continue;
            }
            // kattanásjavítás a gyűrűben (a kimenet dc_ mintával később olvas, a javított rész még nem ment ki)
            if (declick && mode_ >= 1) {
                for (int c = 0; c < 2; ++c) {
                    int64_t a = 0, b = 0;
                    if (clk_[c].step(wkRing_[c * kRing + wi], t_, clickK, a, b) && t_ - a < dc_ - 4) {
                        repair(c, a, b);
                        ++clicksFixed;
                    }
                }
            }
            // RNNoise: 480 mintás keretek a végleges (javított) jelből; a kimenet a bemeneti sorszámra igazítva
            const int64_t fin = t_ - dc_;
            if (rnnRun) {
                for (int c = 0; c < 2; ++c) frame_[c][frameFill_] = wkRing_[c * kRing + (static_cast<size_t>(fin) & kMask)];
                if (++frameFill_ == RnnDenoiser::kFrame) {
                    frameFill_ = 0;
                    const int64_t s0 = fin - static_cast<int64_t>(RnnDenoiser::kFrame) + 1 - rnnDelay_;
                    for (int c = 0; c < 2; ++c) {
                        rnn_[c]->processFrame(frame_[c], out_[c]);
                        for (size_t j = 0; j < RnnDenoiser::kFrame; ++j)
                            rnnRing_[c * kRing + (static_cast<size_t>(s0 + static_cast<int64_t>(j)) & kMask)] = out_[c][j];
                    }
                    if (rnnWarm_ < 4) ++rnnWarm_;
                }
            }
            const float wT = rnnRun && rnnWarm_ >= 4 ? rnnW : 0.0f;
            w_ += std::clamp(wT - w_, -wCoef_, wCoef_);
            const int64_t u = t_ - (mode_ == 2 ? dtRnn_ : dc_);
            const size_t ui = static_cast<size_t>(u) & kMask;
            for (int c = 0; c < 2; ++c) {
                float y = wkRing_[c * kRing + ui];
                if (w_ > 0.0f) {
                    const float rv = rnnRing_[c * kRing + ui];
                    y += w_ * (1.0f - rnnAtt) * (rv - y);   // y = d + w·(1 − a)·(rnn − d): a zaj legfeljebb a-szorosára halkul
                }
                io[c][i] = y * fade_;
                dry[c][i] = rawRing_[c * kRing + ui];
            }
        }
    }

private:
    /**
     * LSAR interpoláció (Vaseghi, Godsill): a kattanás mintáit úgy tölti ki, hogy a lineáris predikció maradéka a
     * hézagon és utána (P mintán) a legkisebb legyen; a P × P-s sávos normálegyenlet Cholesky-bontással.
     */
    void repair(int c, int64_t a, int64_t b) {
        float* w = wkRing_.data() + static_cast<size_t>(c) * kRing;
        const auto at = [&](int64_t t) -> float& { return w[static_cast<size_t>(t) & kMask]; };
        constexpr int P = ClickDetector::kOrder;
        const auto& ar = clk_[c].coeffs();
        // A sérült minták: az előre irányú maradék a kattanás után még P mintán át nagy, a hátra irányú előtte;
        // a kettő metszete a valóban sérült rész (a stacionárius AR-modell hátra irányú együtthatói ugyanazok)
        {
            const double thr = clk_[c].threshold();
            int64_t last = a;
            for (int64_t t = a; t <= b; ++t) {
                double pred = 0;
                for (int k = 0; k < P; ++k) pred += ar[static_cast<size_t>(k)] * at(t + 1 + k);
                if (std::fabs(at(t) - pred) > thr) last = t;
            }
            b = last + 1;
            a -= 1;
        }
        const int L = static_cast<int>(b - a + 1);
        if (L <= 0 || L > kMaxGap) return;
        double cc[P + 1], rc[P + 1];
        cc[0] = 1.0;
        for (int k = 0; k < P; ++k) cc[k + 1] = -ar[static_cast<size_t>(k)];
        for (int m = 0; m <= P; ++m) { double acc = 0; for (int k = 0; k + m <= P; ++k) acc += cc[k] * cc[k + m]; rc[m] = acc; }
        double* M = mat_.data();
        for (int i = 0; i < L; ++i)
            for (int j = 0; j < L; ++j) M[i * L + j] = std::abs(i - j) <= P ? rc[std::abs(i - j)] : 0.0;
        for (int i = 0; i < L; ++i) M[i * L + i] += 1e-9 * rc[0] + 1e-15;
        // az ismert minták hozzájárulása a maradékhoz (t = a .. b + P)
        double* st = vec_.data();
        for (int64_t t = a; t <= b + P; ++t) {
            double acc = 0;
            for (int k = 0; k <= P; ++k) { const int64_t q = t - k; if (q < a || q > b) acc += cc[k] * at(q); }
            st[t - a] = acc;
        }
        double* rhs = rhs_.data();
        for (int i = 0; i < L; ++i) {
            double acc = 0;
            for (int k = 0; k <= P; ++k) acc += cc[k] * st[i + k];
            rhs[i] = -acc;
        }
        // Cholesky: M = G·Gᵀ, majd előre és vissza helyettesítés
        for (int j = 0; j < L; ++j) {
            double d = M[j * L + j];
            for (int k = 0; k < j; ++k) d -= M[j * L + k] * M[j * L + k];
            if (d <= 0) return;   // nem javítható: marad az eredeti
            d = std::sqrt(d);
            M[j * L + j] = d;
            for (int i = j + 1; i < L; ++i) {
                double v = M[i * L + j];
                for (int k = 0; k < j; ++k) v -= M[i * L + k] * M[j * L + k];
                M[i * L + j] = v / d;
            }
        }
        for (int i = 0; i < L; ++i) { double v = rhs[i]; for (int k = 0; k < i; ++k) v -= M[i * L + k] * rhs[k]; rhs[i] = v / M[i * L + i]; }
        for (int i = L - 1; i >= 0; --i) { double v = rhs[i]; for (int k = i + 1; k < L; ++k) v -= M[k * L + i] * rhs[k]; rhs[i] = v / M[i * L + i]; }
        for (int i = 0; i < L; ++i) at(a + i) = static_cast<float>(rhs[i]);
    }

    static constexpr int kMaxGap = 96;   // 2 ms 48 kHz-en

    uint32_t fs_ = 48000;
    std::unique_ptr<RnnDenoiser> rnn_[2];
    std::vector<float> rawRing_, wkRing_, rnnRing_;
    std::vector<double> mat_, vec_, rhs_;   // az LSAR munkaterülete (előre foglalva)
    ClickDetector clk_[2];
    float frame_[2][RnnDenoiser::kFrame] = {}, out_[2][RnnDenoiser::kFrame] = {};
    size_t frameFill_ = 0;
    int64_t t_ = 0, dc_ = 128, dtRnn_ = 128, rnnDelay_ = 0;
    int mode_ = 0;
    bool rnnUsable_ = false, switching_ = false;
    float fade_ = 1.0f, fadeStep_ = 0.004f, w_ = 0.0f, wCoef_ = 0.0001f;
    int rnnWarm_ = 0;
};

// ------------------------------------------------------------------ elemzés: zümmögés
/**
 * Nagy felbontású (≈ 1,5 Hz) átlagspektrum a halk keretekből (a kb. 100 s-os előzmény alsó negyede); zümmögés,
 * ha az 50 vagy 60 Hz-es sor legalább három tagja (vagy kettő nagyon erősen) pontosan az alap egész számú
 * többszörösén kiemelkedik a környezetéből, és ez legalább hat egymást követő halk kereten át megmarad.
 */
class HumDetector {
public:
    struct Result { float f0 = 0; std::array<float, kHumHarmonics> promDb{}; };

    void configure(double fs) {
        fs_ = fs;
        n_ = 1;
        while (static_cast<double>(n_) < 0.6 * fs) n_ <<= 1;   // 48 kHz: 32768 (1,46 Hz)
        fft_.init(n_);
        buf_.assign(n_, 0.0); work_.assign(n_, {}); win_.resize(n_);
        for (size_t i = 0; i < n_; ++i) win_[i] = 0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / static_cast<double>(n_));
        const size_t bins = std::min<size_t>(n_ / 2, static_cast<size_t>(560.0 * static_cast<double>(n_) / fs) + 2);   // 8 × 60 Hz + tartalék
        quiet_.assign(bins, 0.0);
        fill_ = 0; frames_ = quietFrames_ = 0; lev_.resize(300); stable_ = miss_ = 0; lastF0_ = 0;
        res_ = Result{};
    }
    void restart() {
        std::fill(quiet_.begin(), quiet_.end(), 0.0);
        frames_ = quietFrames_ = 0; stable_ = miss_ = 0; lastF0_ = 0; res_ = Result{}; lev_.clear();
    }
    void push(double x) {
        buf_[fill_++] = x;
        if (fill_ == n_) { frame(); std::memmove(buf_.data(), buf_.data() + n_ / 2, n_ / 2 * sizeof(double)); fill_ = n_ / 2; }
    }
    const Result& result() const { return res_; }

private:
    void frame() {
        double e = 0;
        for (size_t i = 0; i < n_; ++i) { work_[i] = { buf_[i] * win_[i], 0.0 }; e += buf_[i] * buf_[i]; }
        const double lev = toDb(e / static_cast<double>(n_));
        if (lev < -100.0) return;   // digitális csend
        fft_.forward(work_);
        lev_.push(lev);
        // halk keret: a kb. 100 s-os előzmény alsó negyedében (a hangos szakaszban nem frissül, így a zümmögés
        // „ujjlenyomata” megmarad akkor is, ha a zene elfedi)
        const double q = lev_.size() >= 8 ? lev_.percentile(0.25, tmp_) : -1e9;
        ++frames_;
        const bool quietNow = lev <= q + 0.5;
        if (quietNow) {
            const double aq = quietFrames_ < 8 ? 1.0 / static_cast<double>(quietFrames_ + 1) : 0.15;
            for (size_t k = 0; k < quiet_.size(); ++k) quiet_[k] += aq * (std::norm(work_[k]) - quiet_[k]);
            ++quietFrames_;
        }
        // Döntés csak új halk keret után, a halk keretek spektrumából (a zümmögés ott is ott van, a zene kevésbé
        // fedi el); a hangos szakaszban nincs új adat, így a „stabil” számláló sem nő
        if (!quietNow || quietFrames_ < 6) return;
        const std::vector<double>& s = quiet_;
        Result best;
        int bestCount = 0;
        float bestMax = 0;
        for (double base : { 50.0, 60.0 }) {
            Result r;
            double fsum = 0, wsum = 0;
            int count = 0;
            float mx = 0;
            for (int h = 0; h < kHumHarmonics; ++h) {
                const double f = base * (h + 1);
                double pf = 0, pr = 0;
                if (!peak(s, f, 0.012 * f + 1.5, pf, pr)) continue;
                // a hálózati frekvencia pontos (50 / 60 ± 0,2 Hz): a zenei hang (pl. G1 = 49,0 Hz) nem fér bele
                if (std::fabs(pf / (h + 1) - base) > 0.35) continue;
                r.promDb[h] = static_cast<float>(pr);
                if (pr >= 8.0) { ++count; fsum += pf / (h + 1) * pr; wsum += pr; }
                mx = std::max(mx, static_cast<float>(pr));
            }
            r.f0 = wsum > 0 ? static_cast<float>(fsum / wsum) : 0.0f;
            // a felharmonikusoknak az alap pontos többszörösén kell lenniük
            if (r.f0 > 0)
                for (int h = 0; h < kHumHarmonics; ++h) {
                    double pf = 0, pr = 0;
                    if (r.promDb[h] > 0 && peak(s, static_cast<double>(r.f0) * (h + 1), 1.5, pf, pr) &&
                        std::fabs(pf - static_cast<double>(r.f0) * (h + 1)) > 0.25 + 0.05 * h)
                        r.promDb[h] = 0;
                }
            // legalább három tagja kiemelkedik, vagy kettő nagyon erősen (a zene hangjai ritkán esnek pontosan a sorra)
            if (((count >= 3 && mx >= 12.0f) || (count >= 2 && mx >= 20.0f)) &&
                (count > bestCount || (count == bestCount && mx > bestMax))) {
                best = r; bestCount = count; bestMax = mx;
            }
        }
        // állandóság: legalább 6 egymást követő keret (≈ 2 s) ugyanazon a frekvencián; elengedés 10 hiányzó keret után
        // A már felismert zümmögés csak a valóban halk részek alapján változik (a hangos szakaszban a zene elfedi,
        // de attól még ott van): ilyenkor a mérés áll
        const bool informative = res_.f0 <= 0 || (quietNow && lev <= refLev_ + 6.0);
        if (best.f0 > 0) {
            if (std::fabs(best.f0 - lastF0_) < 0.2f) ++stable_;
            else stable_ = 1;
            lastF0_ = best.f0;
            miss_ = 0;
            if (stable_ >= 6 && informative) {
                // a kiemelkedés csúcsát lassan (keretenként 0,3 dB) engedi el; az alapfrekvencia lassan követ
                const bool same = res_.f0 > 0 && std::fabs(best.f0 - res_.f0) < 1.0f;
                for (int h = 0; h < kHumHarmonics; ++h)
                    res_.promDb[h] = same ? std::max(best.promDb[h], res_.promDb[h] - 0.3f) : best.promDb[h];
                res_.f0 = same ? res_.f0 + 0.2f * (best.f0 - res_.f0) : best.f0;
                refLev_ = same ? refLev_ + 0.2 * (lev - refLev_) : lev;
            }
        } else if (informative && ++miss_ >= 10) {
            stable_ = 0; lastF0_ = 0; res_ = Result{};
        }
    }
    /** Csúcs a [f − tol, f + tol] tartományban (parabolikus finomítás), kiemelkedése a ±6–30 Hz környezet mediánjához. */
    bool peak(const std::vector<double>& s, double f, double tol, double& pf, double& promDb) {
        const double binHz = fs_ / static_cast<double>(n_);
        const long lo = std::max(2L, static_cast<long>((f - tol) / binHz)), hi = static_cast<long>((f + tol) / binHz) + 1;
        if (hi + 2 >= static_cast<long>(s.size())) return false;
        long kb = lo;
        for (long k = lo; k <= hi; ++k) if (s[static_cast<size_t>(k)] > s[static_cast<size_t>(kb)]) kb = k;
        const double ym = toDb(s[static_cast<size_t>(kb - 1)]), y0 = toDb(s[static_cast<size_t>(kb)]), yp = toDb(s[static_cast<size_t>(kb + 1)]);
        const double den = ym - 2 * y0 + yp, d = den < 0 ? std::clamp(0.5 * (ym - yp) / den, -0.5, 0.5) : 0.0;
        pf = (static_cast<double>(kb) + d) * binHz;
        nb_.clear();
        const long w0 = static_cast<long>(6.0 / binHz), w1 = static_cast<long>(30.0 / binHz);
        for (long k = kb - w1; k <= kb + w1; ++k) {
            if (std::labs(k - kb) < w0 || k < 1 || k >= static_cast<long>(s.size())) continue;
            nb_.push_back(s[static_cast<size_t>(k)]);
        }
        if (nb_.size() < 4) return false;
        std::nth_element(nb_.begin(), nb_.begin() + static_cast<long>(nb_.size() / 2), nb_.end());
        promDb = y0 - toDb(nb_[nb_.size() / 2]);
        return true;
    }

    double fs_ = 48000;
    size_t n_ = 32768, fill_ = 0, frames_ = 0, quietFrames_ = 0;
    Fft fft_;
    std::vector<double> buf_, win_, quiet_, nb_, tmp_;
    std::vector<std::complex<double>> work_;
    History lev_{300};
    int stable_ = 0, miss_ = 0;
    float lastF0_ = 0;
    double refLev_ = -200;   // a halk keretek szintje, amikor a zümmögést felismerte
    Result res_;
};

// ------------------------------------------------------------------ elemzés: sávonkénti zajpadló
/**
 * A BandNr sávjainak 20 ms-os szintjei (15 s előzmény). Zaj, ha a halk keretek szintje állandó (a 2. és a
 * 10. percentilis közel van egymáshoz), a zajpadló időben sem vándorol (a halk zenei rész igen), és a jel
 * jóval a zajpadló fölé megy (90. percentilis).
 */
class NrMeter {
public:
    void configure(double fs) {
        for (int b = 0; b < kNrBands; ++b) {
            designHighpass(hp_[b], fs, nrEdge(b), std::sqrt(0.5));
            designLowpass(lp_[b], fs, std::min(nrEdge(b + 1), 0.45 * fs), std::sqrt(0.5));
            hp_[b].reset(); lp_[b].reset();
            hist_[b].resize(750);
        }
        frameLen_ = std::max<size_t>(1, static_cast<size_t>(0.02 * fs));
        pos_ = 0; acc_.fill(0); floor_.fill(-120); conf_.fill(0);
    }
    void restart() { for (auto& h : hist_) h.clear(); conf_.fill(0); fhN_ = fhPos_ = 0; }
    void push(double x) {
        for (int b = 0; b < kNrBands; ++b) { const double y = lp_[b].process(hp_[b].process(x)); acc_[b] += y * y; }
        if (++pos_ >= frameLen_) {
            for (int b = 0; b < kNrBands; ++b) {
                const double l = toDb(acc_[b] / static_cast<double>(frameLen_));
                if (l > -130.0) hist_[b].push(l);   // a digitális csend nem zajpadló
                acc_[b] = 0;
            }
            pos_ = 0;
        }
    }
    /** Másodpercenként: zajpadló és megbízhatóság (0..1) sávonként. */
    void update() {
        for (int b = 0; b < kNrBands; ++b) {
            History& h = hist_[b];
            if (h.size() < 250) { conf_[b] = 0; continue; }   // legalább 5 s
            const double p2 = h.percentile(0.02, tmp_), p10 = h.percentile(0.10, tmp_), p90 = h.percentile(0.90, tmp_);
            floor_[b] = static_cast<float>(p10);
            spread_[b] = static_cast<float>(p10 - p2);
            range_[b] = static_cast<float>(p90 - p10);
            // a zajpadló vándorlása az utolsó 8 mérés (≈ 8 s) alatt
            fh_[b][fhPos_ % 8] = floor_[b];
            float lo = 1e9f, hi = -1e9f;
            const size_t cnt = std::min<size_t>(fhN_ + 1, 8);
            for (size_t i = 0; i < cnt; ++i) { lo = std::min(lo, fh_[b][i]); hi = std::max(hi, fh_[b][i]); }
            drift_[b] = cnt >= 4 ? hi - lo : 99.0f;
            conf_[b] = p10 > -100.0 ? 1.0f : 0.0f;
        }
        ++fhPos_;
        fhN_ = std::min<size_t>(fhN_ + 1, 8);
    }
    float floorDb(int b) const { return floor_[b]; }
    /**
     * Megbízhatóság (0..1): állandó zajpadló (a halk keretek szórása kicsi), és a jel legalább minRange dB-lel fölötte
     * (zenénél nagyobb kell, hogy a sűrű, végig hangos keverést ne nézze zajnak; beszédnél a szünetek a zajt mutatják).
     */
    float conf(int b, float minRange) const {
        return conf_[b] * std::clamp((3.5f - spread_[b]) / 1.5f, 0.0f, 1.0f) * std::clamp((range_[b] - minRange) / 8.0f, 0.0f, 1.0f) *
               std::clamp((3.0f - drift_[b]) / 1.5f, 0.0f, 1.0f);
    }
    float drift(int b) const { return drift_[b]; }
    float spread(int b) const { return spread_[b]; }
    float range(int b) const { return range_[b]; }
private:
    BiquadD hp_[kNrBands], lp_[kNrBands];
    History hist_[kNrBands];
    std::array<double, kNrBands> acc_{};
    std::array<float, kNrBands> floor_{}, conf_{}, spread_{}, range_{}, drift_{};
    float fh_[kNrBands][8] = {};
    size_t fhN_ = 0, fhPos_ = 0;
    std::vector<double> tmp_;
    size_t frameLen_ = 960, pos_ = 0;
};

} // namespace ee_analyzer::magic
