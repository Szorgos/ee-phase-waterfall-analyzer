/**
 * @file ee_dsp_loudness.cpp
 * @brief Loudness Compensator (ISO 226 egyenlő hangosságú görbék) az LSP könyvtárral.
 *
 * Az EasyEffects Loudness moduljával megegyező motor és beállítás-leképezés:
 *   - Motor: Linux Studio Plugins (lsp-plugins-loud-comp 1.0.36, loud_comp)
 *   - Szabványok: ISO 226:2003, ISO 226:2023, Fletcher-Munson, Robinson-Dadson
 *   - Üzemmódok:
 *       0: IIR mód (Zero Latency) - Linkwitz-Riley sávok (Fastest: 16, Low: 24, Normal: 32, High: 48, Best: 64 szűrő), 0 minta késleltetés
 *       1..7: FFT mód (Linear Phase) - SpectralProcessor (256, 512, 1024, 2048, 4096, 8192, 16384 méret)
 *   - Bemeneti és kimeneti erősítés (-36 .. +36 dB)
 *   - Hard-clipping védelem dinamikus küszöbbel
 *
 * A beállítás-feldolgozás és a szűrőkonfigurálás az LSP loud_comp forrásából származik
 * (src/main/plug/loud_comp.cpp 437-713 és 815-912. sorok), a plugin-keretrendszer nélkül,
 * a meglévő src/ee_dsp_lsp.cpp mintájára.
 *
 * A process() függvény valós idejű szálból hívható (nem foglal memóriát, nem vár zárra).
 * Az LSP kód LGPL-3.0-or-later (third_party/lsp/lsp-plugins-loud-comp), az Aqulify GPL-3.0.
 */
#include "ee_dsp.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#include <lsp-plug.in/dsp/dsp.h>
#include <lsp-plug.in/dsp-units/units.h>
#include <lsp-plug.in/dsp-units/filters/Equalizer.h>
#include <lsp-plug.in/dsp-units/util/SpectralProcessor.h>
#include <lsp-plug.in/stdlib/math.h>

#include <generated/iso226/defs.h>
#include <generated/iso226/iso226-2003.h>
#include <generated/iso226/iso226-2023.h>
#include <generated/iso226/fletcher_munson.h>
#include <generated/iso226/robinson_dadson.h>

using namespace lsp;

namespace ee_analyzer::ee {

namespace {

// Átvéve: lsp-plugins-loud-comp 1.0.36, include/private/meta/loud_comp.h 32-62. sorok
namespace meta {
    static constexpr float PHONS_MIN     = -83.0f;
    static constexpr float PHONS_MAX     = 7.0f;
    static constexpr float HCRANGE_MIN   = 0.0f;
    static constexpr float HCRANGE_MAX   = 24.0f;
    static constexpr float HCRANGE_DFL   = 6.0f;
    static constexpr float FREQ_MIN      = 10.0f;
    static constexpr float FREQ_MAX      = 24000.0f;
    static constexpr size_t FFT_RANK_MIN = 8;
    static constexpr size_t FFT_RANK_MAX = 14;
    static constexpr size_t FILTER_BANDS = 64;

    // LSP loud_comp IIR közelítési fokozatok (0: Fastest, 1: Low, 2: Normal, 3: High, 4: Best)
    static constexpr size_t APPROX_FILTERS[5] = { 16, 24, 32, 48, 64 };
    static constexpr int APPROX_SLOPE[5]       = { 1, 2, 3, 4, 5 };
}

constexpr size_t EQ_SMOOTH_STEP = 32;
constexpr size_t NUM_CURVES = 4;
constexpr float CURVE_APPROX = 0.0005f * M_LN10;
constexpr size_t LOUD_FILTER_BANDS = meta::FILTER_BANDS; // 64 sáv
constexpr size_t MAX_FFT_RANK = meta::FFT_RANK_MAX;     // 14 -> 16384 minta

static const freq_curve_t* const s_freqCurves[NUM_CURVES] = {
    &iso226_2003_curve,     // 1: ISO 226:2003
    &fletcher_munson_curve, // 2: Fletcher-Munson
    &robinson_dadson_curve, // 3: Robinson-Dadson
    &iso226_2023_curve      // 4: ISO 226:2023
};

inline const freq_curve_t* getCurve(int32_t stdIdx) {
    if (stdIdx >= 1 && stdIdx <= 4) {
        return s_freqCurves[stdIdx - 1];
    }
    return nullptr; // 0: Flat
}

inline size_t modeToRank(int32_t mode) {
    switch (mode) {
        case 1: return 8;  // 256
        case 2: return 9;  // 512
        case 3: return 10; // 1024
        case 4: return 11; // 2048
        case 5: return 12; // 4096 (EasyEffects alapérték)
        case 6: return 13; // 8192
        case 7: return 14; // 16384
        default: return 12;
    }
}

} // namespace

struct Loudness::Impl {
    uint32_t sampleRate_ = 48000;
    dspu::Equalizer equalizer_[2];
    dspu::SpectralProcessor proc_[2];

    // Előre lefoglalt pufferek (valós idejű szálban nincs allokáció)
    std::vector<float> freqApplyBuf_; // (2 << MAX_FFT_RANK) float
    std::vector<float> tmpBuf_;       // 2048 float
    float* vFreqApply_ = nullptr;
    float* vTmpBuf_ = nullptr;

    float vGains_[LOUD_FILTER_BANDS];
    float vOldGains_[LOUD_FILTER_BANDS];
    float freqs_[LOUD_FILTER_BANDS];

    size_t nFilters_ = meta::APPROX_FILTERS[2]; // Alapértelmezett: 32 szűrő (Normal)
    int32_t currentMode_ = 5;  // 0: IIR, 1..7: FFT (alapérték: 5 = FFT 4096)
    size_t currentRank_ = 12;  // FFT rank
    int32_t currentStd_ = 1;   // Görbeszabvány (alapérték: 1 = ISO 226:2003)
    int32_t currentApprox_ = 2; // IIR fokozat (alapérték: 2 = Normal, 32 szűrő)
    float currentVolume_ = -999.0f; // Hangerőszint
    bool smooth_ = false;

    float inGain_ = 1.0f;
    float outGain_ = 1.0f;
    float curveMaxGain_ = 1.0f;
    bool hclip_ = false;
    float hclipRangeDb_ = 6.0f;
    float hclipLvl_ = 1.0f;
    std::atomic<bool> clipping_{false};
    size_t latency_ = 0;

    Impl() {
        initDsp();

        freqApplyBuf_.assign(2 << MAX_FFT_RANK, 1.0f);
        tmpBuf_.assign(2048, 0.0f);
        vFreqApply_ = freqApplyBuf_.data();
        vTmpBuf_ = tmpBuf_.data();

        dsp::fill(vGains_, 1.0f, LOUD_FILTER_BANDS);
        dsp::fill(vOldGains_, 1.0f, LOUD_FILTER_BANDS);
        dsp::fill(freqs_, 1000.0f, LOUD_FILTER_BANDS);

        for (int ch = 0; ch < 2; ++ch) {
            equalizer_[ch].init(LOUD_FILTER_BANDS, 0);
            equalizer_[ch].set_mode(dspu::EQM_IIR);
            equalizer_[ch].set_sample_rate(sampleRate_);

            proc_[ch].init(MAX_FFT_RANK);
            proc_[ch].bind(processSpectrumCb, this, &proc_[ch]);
            proc_[ch].set_phase(0.5f * ch);
        }

        updateCurves(0.0f, currentStd_, currentMode_, currentApprox_, true);
    }

    ~Impl() {
        for (int ch = 0; ch < 2; ++ch) {
            proc_[ch].unbind();
            proc_[ch].destroy();
            equalizer_[ch].destroy();
        }
    }

    void setSampleRate(uint32_t sr) {
        sr = sr > 0 ? sr : 48000;
        if (sampleRate_ == sr) return;
        sampleRate_ = sr;
        for (int ch = 0; ch < 2; ++ch) {
            equalizer_[ch].set_sample_rate(sampleRate_);
        }
        const float vol = (currentVolume_ < -500.0f) ? 0.0f : currentVolume_;
        currentVolume_ = -999.0f;
        updateCurves(vol, currentStd_, currentMode_, currentApprox_, true);
    }

    static void processSpectrumCb(void* object, void* /*subject*/, float* buf, size_t rank) {
        auto* self = static_cast<Impl*>(object);
        self->processSpectrum(buf, rank);
    }

    void processSpectrum(float* buf, size_t rank) {
        const size_t count = 2 << rank;
        dsp::mul2(buf, vFreqApply_, count);
    }

    void updateCurves(float volume, int32_t stdIdx, int32_t mode, int32_t approx, bool force) {
        const size_t rank = modeToRank(mode);
        const bool modeChanged = (mode != currentMode_);
        const bool stdChanged = (stdIdx != currentStd_);
        const bool approxChanged = (approx != currentApprox_);
        const bool volChanged = (std::abs(volume - currentVolume_) > 1e-4f);

        if (!force && !modeChanged && !stdChanged && !approxChanged && !volChanged) return;

        if (modeChanged) {
            for (int ch = 0; ch < 2; ++ch) {
                proc_[ch].reset();
                equalizer_[ch].reset();
            }
        }

        const freq_curve_t* c = getCurve(stdIdx);

        if (mode == 0) {
            // IIR mód
            smooth_ = !force && !modeChanged && !stdChanged && !approxChanged;
            currentMode_ = mode;
            currentStd_ = stdIdx;
            currentApprox_ = approx;
            currentVolume_ = volume;
            latency_ = 0;

            updateIirCurve(c, volume, approx);
        } else {
            // FFT mód
            smooth_ = false;
            currentMode_ = mode;
            currentRank_ = rank;
            currentStd_ = stdIdx;
            currentApprox_ = approx;
            currentVolume_ = volume;
            latency_ = 1 << rank;

            for (int ch = 0; ch < 2; ++ch) {
                proc_[ch].set_rank(rank);
            }

            updateFftCurve(c, volume, rank);
        }
    }

    void updateIirCurve(const freq_curve_t* c, float volume, int32_t approx) {
        approx = std::clamp(approx, 0, 4);
        nFilters_ = meta::APPROX_FILTERS[approx];
        const int slope = meta::APPROX_SLOPE[approx];
        if (c != nullptr) {
            const float rfmin = 1.0f / c->fmin;
            const float df = std::log(c->fmax * rfmin);
            const float kf = df / (nFilters_ - 1);

            for (size_t i = 0; i < nFilters_; ++i) {
                freqs_[i] = c->fmin * std::exp((static_cast<float>(i) + 0.5f) * kf);
            }

            const float vol = std::clamp(volume - meta::PHONS_MIN, c->amin, c->amax) - c->amin;
            const float range = c->amax - c->amin;
            const float step = range / (c->curves - 1);
            ssize_t nc = static_cast<ssize_t>(vol / step);
            if (nc >= static_cast<ssize_t>(c->curves - 1)) nc = c->curves - 2;
            const float k2 = CURVE_APPROX * (vol / step - static_cast<float>(nc));
            const float k1 = CURVE_APPROX - k2;

            const int16_t* a = c->data[nc];
            const int16_t* b = c->data[nc + 1];
            const float kdf = (c->hdots - 1) / df;

            for (size_t i = 0; i < nFilters_; ++i) {
                const float f = c->fmin * std::exp(static_cast<float>(i) * kf);
                ssize_t idx = 0;
                if (f <= c->fmin) idx = 0;
                else if (f >= c->fmax) idx = c->hdots - 1;
                else idx = static_cast<ssize_t>(std::log(f * rfmin) * kdf);

                vGains_[i] = std::exp(a[idx] * k1 + b[idx] * k2);
            }
        } else {
            // Flat mód (c == nullptr): tiszta frekvenciafüggetlen szint
            const float df = std::log(meta::FREQ_MAX / meta::FREQ_MIN);
            const float kf = df / (nFilters_ - 1);
            for (size_t i = 0; i < nFilters_; ++i) {
                freqs_[i] = meta::FREQ_MIN * std::exp((static_cast<float>(i) + 0.5f) * kf);
            }
            const float vol = dspu::db_to_gain(volume);
            dsp::fill(vGains_, vol, nFilters_);
        }

        if (!smooth_) {
            dsp::copy(vOldGains_, vGains_, nFilters_);
        }

        curveMaxGain_ = dsp::max(vGains_, nFilters_);

        // Equalizer szűrők beállítása (Slope = meta::APPROX_SLOPE[approx])
        dspu::filter_params_t fp;
        fp.nType = dspu::FLT_NONE;
        fp.nSlope = slope;
        fp.fFreq = 0.0f;
        fp.fFreq2 = 0.0f;
        fp.fGain = GAIN_AMP_0_DB;
        fp.fQuality = 0.0f;

        for (size_t i = 0; i < LOUD_FILTER_BANDS; ++i) {
            if (i >= nFilters_) {
                fp.nType = dspu::FLT_NONE;
            } else if (i == 0) {
                fp.nType = dspu::FLT_BT_LRX_LOSHELF;
                fp.fFreq = freqs_[0];
                fp.fFreq2 = freqs_[i];
            } else if (i < nFilters_ - 1) {
                fp.nType = dspu::FLT_BT_LRX_LADDERPASS;
                fp.fFreq = freqs_[i - 1];
                fp.fFreq2 = freqs_[i];
            } else {
                fp.nType = dspu::FLT_BT_LRX_HISHELF;
                fp.fFreq = freqs_[i - 1];
                fp.fFreq2 = fp.fFreq;
            }
            fp.fGain = vGains_[i];

            equalizer_[0].set_params(i, &fp);
            equalizer_[1].set_params(i, &fp);
        }
    }

    void updateFftCurve(const freq_curve_t* c, float volume, size_t rank) {
        const size_t fft_size = 1 << rank;
        const size_t fft_csize = (fft_size >> 1) + 1;

        if (c != nullptr) {
            const float vol = std::clamp(volume - meta::PHONS_MIN, c->amin, c->amax) - c->amin;
            const float range = c->amax - c->amin;
            const float step = range / (c->curves - 1);
            ssize_t nc = static_cast<ssize_t>(vol / step);
            if (nc >= static_cast<ssize_t>(c->curves - 1)) nc = c->curves - 2;
            const float k2 = CURVE_APPROX * (vol / step - static_cast<float>(nc));
            const float k1 = CURVE_APPROX - k2;

            const int16_t* a = c->data[nc];
            const int16_t* b = c->data[nc + 1];
            for (size_t i = 0; i < c->hdots; ++i) {
                vTmpBuf_[i] = a[i] * k1 + b[i] * k2;
            }
            dsp::exp1(vTmpBuf_, c->hdots);

            float* v = vFreqApply_;
            const float rfmin = 1.0f / c->fmin;
            const float kf = static_cast<float>(sampleRate_) / static_cast<float>(fft_size);
            const float kdf = (c->hdots - 1) / std::log(c->fmax * rfmin);

            for (size_t i = 0; i < fft_csize; ++i, v += 2) {
                const float f = kf * static_cast<float>(i);
                ssize_t idx = 0;
                if (f <= c->fmin) idx = 0;
                else if (f >= c->fmax) idx = c->hdots - 1;
                else idx = static_cast<ssize_t>(std::log(f * rfmin) * kdf);

                const float gain = vTmpBuf_[idx];
                v[0] = gain;
                v[1] = gain;
            }

            dsp::reverse2(&vFreqApply_[fft_size + 2], &vFreqApply_[2], fft_size - 2);
            curveMaxGain_ = dsp::max(vFreqApply_, fft_csize);
        } else {
            // Flat mód (c == nullptr)
            const float vol = dspu::db_to_gain(volume);
            dsp::fill(vFreqApply_, vol, fft_size * 2);
            curveMaxGain_ = vol;
        }
    }

    void update(const ParamData& d) {
        inGain_ = dbToGain(d.loudInputDb);
        outGain_ = dbToGain(d.loudOutputDb);
        hclip_ = (d.loudHclip != 0);
        hclipRangeDb_ = d.loudHclipRangeDb;

        updateCurves(d.loudVolumeDb, d.loudStd, d.loudMode, d.loudApprox, false);

        if (hclip_) {
            const float range = dbToGain(hclipRangeDb_);
            if (currentMode_ == 0) {
                hclipLvl_ = range * curveMaxGain_;
            } else {
                float minVal = 0.0f, maxVal = 0.0f;
                dsp::abs_minmax(vFreqApply_, 2 << currentRank_, &minVal, &maxVal);
                hclipLvl_ = range * std::sqrt(minVal * maxVal);
            }
        } else {
            hclipLvl_ = 1.0f;
        }
    }

    void processIirSmooth(float* L, float* R, size_t samples) {
        dspu::filter_params_t fp;
        const float den = 1.0f / static_cast<float>(samples);

        for (size_t offset = 0; offset < samples; ) {
            const size_t count = std::min(samples - offset, EQ_SMOOTH_STEP);
            const float k = static_cast<float>(offset) * den;

            for (size_t j = 0; j < nFilters_; ++j) {
                equalizer_[0].get_params(j, &fp);
                fp.fGain = vOldGains_[j] * std::exp(std::log(vGains_[j] / vOldGains_[j]) * k);
                equalizer_[0].set_params(j, &fp);
                equalizer_[1].set_params(j, &fp);
            }

            equalizer_[0].process(L + offset, L + offset, count);
            equalizer_[1].process(R + offset, R + offset, count);
            offset += count;
        }

        dsp::copy(vOldGains_, vGains_, nFilters_);
        for (size_t j = 0; j < nFilters_; ++j) {
            equalizer_[0].get_params(j, &fp);
            fp.fGain = vGains_[j];
            equalizer_[0].set_params(j, &fp);
            equalizer_[1].set_params(j, &fp);
        }
    }

    void process(float* L, float* R, size_t n) {
        DspScope scope;
        if (n == 0) return;

        // 1. Bemeneti erősítés
        if (inGain_ != 1.0f) {
            dsp::mul_k2(L, inGain_, n);
            dsp::mul_k2(R, inGain_, n);
        }

        // 2. Loudness kompenzáció (IIR vagy FFT)
        if (currentMode_ == 0) {
            if (smooth_) {
                processIirSmooth(L, R, n);
                smooth_ = false;
            } else {
                equalizer_[0].process(L, L, n);
                equalizer_[1].process(R, R, n);
            }
        } else {
            proc_[0].process(L, L, n);
            proc_[1].process(R, R, n);
        }

        // 3. Hard-clipping védelem (az EasyEffects szerint a plugin kimenetén, a kimeneti erősítés ELŐTT)
        if (hclip_) {
            const float lvl = hclipLvl_;
            const float maxL = dsp::abs_max(L, n);
            const float maxR = dsp::abs_max(R, n);
            if (maxL > lvl || maxR > lvl) {
                clipping_.store(true, std::memory_order_relaxed);
                if (maxL > lvl) dsp::limit1(L, -lvl, lvl, n);
                if (maxR > lvl) dsp::limit1(R, -lvl, lvl, n);
            }
        }

        // 4. Kimeneti erősítés
        if (outGain_ != 1.0f) {
            dsp::mul_k2(L, outGain_, n);
            dsp::mul_k2(R, outGain_, n);
        }
    }

    void clearClipping() {
        clipping_.store(false, std::memory_order_relaxed);
    }

    void reset() {
        for (int ch = 0; ch < 2; ++ch) {
            equalizer_[ch].reset();
            proc_[ch].reset();
        }
        clipping_.store(false, std::memory_order_relaxed);
    }
};

Loudness::Loudness() : p_(std::make_unique<Impl>()) {}
Loudness::~Loudness() = default;

void Loudness::setSampleRate(uint32_t sr) {
    p_->setSampleRate(sr);
}

void Loudness::update(const ParamData& d) {
    p_->update(d);
}

void Loudness::process(float* L, float* R, size_t n) {
    p_->process(L, R, n);
}

size_t Loudness::latency() const {
    return p_->latency_;
}

void Loudness::reset() {
    p_->reset();
}

float Loudness::outputGain() const {
    return p_->curveMaxGain_;
}

bool Loudness::isClipping() const {
    return p_->clipping_.load(std::memory_order_relaxed);
}

void Loudness::clearClipping() {
    p_->clearClipping();
}

} // namespace ee_analyzer::ee
