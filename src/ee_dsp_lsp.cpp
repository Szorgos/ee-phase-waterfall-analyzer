/**
 * @file ee_dsp_lsp.cpp
 * @brief Equalizer és Limiter az LSP (Linux Studio Plugins) DSP könyvtáraival.
 *
 * A beállítás -> DSP leképezés és a feldolgozási lépések az LSP pluginok forrásából származnak
 * (azokat tölti be az EasyEffects LV2 pluginként):
 *   - lsp-plugins-para-equalizer 1.0.40, src/main/plug/para_equalizer.cpp
 *     (decode_filter, adjust_gain, filter_has_width, calc_decramping, calc_oversampler_mode,
 *      update_settings, process_channel, process; "x32 stereo", inspect/solo/mute/analyzer nélkül)
 *   - lsp-plugins-limiter 1.0.37, src/main/plug/limiter.cpp
 *     (get_oversampling_mode, get_filtering, get_limiter_mode, get_dithering, update_settings,
 *      update_sample_rate, process; belső sidechain, premix nélkül, ahogy az EasyEffects használja)
 * Az LSP kód LGPL-3.0+ (third_party/lsp), az Aqulify GPL-3.0.
 */
#include "ee_dsp.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include <lsp-plug.in/dsp/dsp.h>
#include <lsp-plug.in/dsp-units/units.h>
#include <lsp-plug.in/dsp-units/filters/Equalizer.h>
#include <lsp-plug.in/dsp-units/util/Oversampler.h>
#include <lsp-plug.in/dsp-units/util/Delay.h>
#include <lsp-plug.in/dsp-units/util/Dither.h>
#include <lsp-plug.in/dsp-units/dynamics/Limiter.h>

using namespace lsp;

namespace ee_analyzer::ee {

// ============================================================ közös
void initDsp() {
    static std::once_flag once;
    std::call_once(once, []() { dsp::init(); });
}

static_assert(sizeof(dsp::context_t) <= 64, "dsp::context_t méret");

DspScope::DspScope() { dsp::start(reinterpret_cast<dsp::context_t*>(ctx_)); }
DspScope::~DspScope() { dsp::finish(reinterpret_cast<dsp::context_t*>(ctx_)); }

// ============================================================ Equalizer (para_equalizer)
namespace {

constexpr size_t EQ_BUFFER_SIZE = 0x400U;
constexpr size_t EQ_RANK = 12;
constexpr size_t EQ_SMOOTH_STEP = 32;
constexpr size_t EQ_FILTERS = kMaxEqBands;

// meta::para_equalizer_metadata
enum { EQF_OFF, EQF_BELL, EQF_HIPASS, EQF_HISHELF, EQF_LOPASS, EQF_LOSHELF, EQF_NOTCH, EQF_RESONANCE, EQF_ALLPASS,
       EQF_BANDPASS, EQF_LADDERPASS, EQF_LADDERREJ };
enum { EFM_RLC_BT, EFM_RLC_MT, EFM_BWC_BT, EFM_BWC_MT, EFM_LRX_BT, EFM_LRX_MT, EFM_APO_DR };

void decode_filter(uint32_t* ftype, uint32_t* slope, size_t mode) {
#define EQS(k, t, ks) case EFM_##k: *ftype = dspu::t; *slope = ks * *slope; return;
#define EQDFL default: *ftype = dspu::FLT_NONE; *slope = 1; return;
    switch (*ftype) {
        case EQF_BELL:
            switch (mode) {
                EQS(RLC_BT, FLT_BT_RLC_BELL, 1) EQS(RLC_MT, FLT_MT_RLC_BELL, 1) EQS(BWC_BT, FLT_BT_BWC_BELL, 1)
                EQS(BWC_MT, FLT_MT_BWC_BELL, 1) EQS(LRX_BT, FLT_BT_LRX_BELL, 1) EQS(LRX_MT, FLT_MT_LRX_BELL, 1)
                EQS(APO_DR, FLT_DR_APO_PEAKING, 1) EQDFL
            }
            break;
        case EQF_HIPASS:
            switch (mode) {
                EQS(RLC_BT, FLT_BT_RLC_HIPASS, 2) EQS(RLC_MT, FLT_MT_RLC_HIPASS, 2) EQS(BWC_BT, FLT_BT_BWC_HIPASS, 2)
                EQS(BWC_MT, FLT_MT_BWC_HIPASS, 2) EQS(LRX_BT, FLT_BT_LRX_HIPASS, 1) EQS(LRX_MT, FLT_MT_LRX_HIPASS, 1)
                EQS(APO_DR, FLT_DR_APO_HIPASS, 1) EQDFL
            }
            break;
        case EQF_HISHELF:
            switch (mode) {
                EQS(RLC_BT, FLT_BT_RLC_HISHELF, 1) EQS(RLC_MT, FLT_MT_RLC_HISHELF, 1) EQS(BWC_BT, FLT_BT_BWC_HISHELF, 1)
                EQS(BWC_MT, FLT_MT_BWC_HISHELF, 1) EQS(LRX_BT, FLT_BT_LRX_HISHELF, 1) EQS(LRX_MT, FLT_MT_LRX_HISHELF, 1)
                EQS(APO_DR, FLT_DR_APO_HISHELF, 1) EQDFL
            }
            break;
        case EQF_LOPASS:
            switch (mode) {
                EQS(RLC_BT, FLT_BT_RLC_LOPASS, 2) EQS(RLC_MT, FLT_MT_RLC_LOPASS, 2) EQS(BWC_BT, FLT_BT_BWC_LOPASS, 2)
                EQS(BWC_MT, FLT_MT_BWC_LOPASS, 2) EQS(LRX_BT, FLT_BT_LRX_LOPASS, 1) EQS(LRX_MT, FLT_MT_LRX_LOPASS, 1)
                EQS(APO_DR, FLT_DR_APO_LOPASS, 1) EQDFL
            }
            break;
        case EQF_LOSHELF:
            switch (mode) {
                EQS(RLC_BT, FLT_BT_RLC_LOSHELF, 1) EQS(RLC_MT, FLT_MT_RLC_LOSHELF, 1) EQS(BWC_BT, FLT_BT_BWC_LOSHELF, 1)
                EQS(BWC_MT, FLT_MT_BWC_LOSHELF, 1) EQS(LRX_BT, FLT_BT_LRX_LOSHELF, 1) EQS(LRX_MT, FLT_MT_LRX_LOSHELF, 1)
                EQS(APO_DR, FLT_DR_APO_LOSHELF, 1) EQDFL
            }
            break;
        case EQF_NOTCH:
            switch (mode) {
                EQS(RLC_BT, FLT_BT_RLC_NOTCH, 1) EQS(RLC_MT, FLT_MT_RLC_NOTCH, 1) EQS(BWC_BT, FLT_BT_RLC_NOTCH, 1)
                EQS(BWC_MT, FLT_MT_RLC_NOTCH, 1) EQS(LRX_BT, FLT_BT_RLC_NOTCH, 1) EQS(LRX_MT, FLT_MT_RLC_NOTCH, 1)
                EQS(APO_DR, FLT_DR_APO_NOTCH, 1) EQDFL
            }
            break;
        case EQF_ALLPASS:
            switch (mode) {
                EQS(RLC_BT, FLT_BT_RLC_ALLPASS, 1) EQS(RLC_MT, FLT_BT_RLC_ALLPASS, 1) EQS(BWC_BT, FLT_BT_BWC_ALLPASS, 2)
                EQS(BWC_MT, FLT_BT_BWC_ALLPASS, 2) EQS(LRX_BT, FLT_BT_LRX_ALLPASS, 1) EQS(LRX_MT, FLT_BT_LRX_ALLPASS, 1)
                EQS(APO_DR, FLT_DR_APO_ALLPASS, 1) EQDFL
            }
            break;
        case EQF_RESONANCE:
            switch (mode) {
                EQS(RLC_BT, FLT_BT_RLC_RESONANCE, 1) EQS(RLC_MT, FLT_MT_RLC_RESONANCE, 1)
                EQS(BWC_BT, FLT_BT_RLC_RESONANCE, 1) EQS(BWC_MT, FLT_MT_RLC_RESONANCE, 1)
                EQS(LRX_BT, FLT_BT_RLC_RESONANCE, 1) EQS(LRX_MT, FLT_MT_RLC_RESONANCE, 1)
                EQS(APO_DR, FLT_DR_APO_PEAKING, 1) EQDFL
            }
            break;
        case EQF_BANDPASS:
            switch (mode) {
                EQS(RLC_BT, FLT_BT_RLC_BANDPASS, 1) EQS(RLC_MT, FLT_MT_RLC_BANDPASS, 1)
                EQS(BWC_BT, FLT_BT_BWC_BANDPASS, 1) EQS(BWC_MT, FLT_MT_BWC_BANDPASS, 1)
                EQS(LRX_BT, FLT_BT_LRX_BANDPASS, 1) EQS(LRX_MT, FLT_MT_LRX_BANDPASS, 1)
                EQS(APO_DR, FLT_DR_APO_BANDPASS, 1) EQDFL
            }
            break;
        case EQF_LADDERPASS:
            switch (mode) {
                EQS(RLC_BT, FLT_BT_RLC_LADDERPASS, 1) EQS(RLC_MT, FLT_MT_RLC_LADDERPASS, 1)
                EQS(BWC_BT, FLT_BT_BWC_LADDERPASS, 1) EQS(BWC_MT, FLT_MT_BWC_LADDERPASS, 1)
                EQS(LRX_BT, FLT_BT_LRX_LADDERPASS, 1) EQS(LRX_MT, FLT_MT_LRX_LADDERPASS, 1)
                EQS(APO_DR, FLT_DR_APO_LADDERPASS, 1) EQDFL
            }
            break;
        case EQF_LADDERREJ:
            switch (mode) {
                EQS(RLC_BT, FLT_BT_RLC_LADDERREJ, 1) EQS(RLC_MT, FLT_MT_RLC_LADDERREJ, 1)
                EQS(BWC_BT, FLT_BT_BWC_LADDERREJ, 1) EQS(BWC_MT, FLT_MT_BWC_LADDERREJ, 1)
                EQS(LRX_BT, FLT_BT_LRX_LADDERREJ, 1) EQS(LRX_MT, FLT_MT_LRX_LADDERREJ, 1)
                EQS(APO_DR, FLT_DR_APO_LADDERREJ, 1) EQDFL
            }
            break;
        case EQF_OFF:
        default:
            *ftype = dspu::FLT_NONE;
            *slope = 1;
            return;
    }
#undef EQDFL
#undef EQS
}

bool adjust_gain(size_t filter_type) {
    switch (filter_type) {
        case dspu::FLT_NONE:
        case dspu::FLT_BT_RLC_LOPASS: case dspu::FLT_MT_RLC_LOPASS:
        case dspu::FLT_BT_RLC_HIPASS: case dspu::FLT_MT_RLC_HIPASS:
        case dspu::FLT_BT_RLC_NOTCH: case dspu::FLT_MT_RLC_NOTCH:
        case dspu::FLT_BT_RLC_ALLPASS: case dspu::FLT_MT_RLC_ALLPASS:
        case dspu::FLT_BT_RLC_ALLPASS2: case dspu::FLT_MT_RLC_ALLPASS2:
        case dspu::FLT_BT_RLC_BANDPASS: case dspu::FLT_MT_RLC_BANDPASS:
        case dspu::FLT_BT_BWC_LOPASS: case dspu::FLT_MT_BWC_LOPASS:
        case dspu::FLT_BT_BWC_HIPASS: case dspu::FLT_MT_BWC_HIPASS:
        case dspu::FLT_BT_BWC_ALLPASS: case dspu::FLT_MT_BWC_ALLPASS:
        case dspu::FLT_BT_BWC_BANDPASS: case dspu::FLT_MT_BWC_BANDPASS:
        case dspu::FLT_BT_LRX_LOPASS: case dspu::FLT_MT_LRX_LOPASS:
        case dspu::FLT_BT_LRX_HIPASS: case dspu::FLT_MT_LRX_HIPASS:
        case dspu::FLT_BT_LRX_ALLPASS: case dspu::FLT_MT_LRX_ALLPASS:
        case dspu::FLT_BT_LRX_BANDPASS: case dspu::FLT_MT_LRX_BANDPASS:
        // Disable gain adjust for several APO filters, too
        case dspu::FLT_DR_APO_NOTCH:
        case dspu::FLT_DR_APO_LOPASS:
        case dspu::FLT_DR_APO_HIPASS:
        case dspu::FLT_DR_APO_ALLPASS:
        case dspu::FLT_DR_APO_ALLPASS2:
        case dspu::FLT_DR_APO_BANDPASS:
            return false;
        default:
            break;
    }
    return true;
}

bool filter_has_width(size_t type) {
    switch (type) {
        case dspu::FLT_BT_RLC_BANDPASS: case dspu::FLT_MT_RLC_BANDPASS:
        case dspu::FLT_BT_BWC_BANDPASS: case dspu::FLT_MT_BWC_BANDPASS:
        case dspu::FLT_BT_LRX_BANDPASS: case dspu::FLT_MT_LRX_BANDPASS:
        case dspu::FLT_BT_RLC_LADDERPASS: case dspu::FLT_MT_RLC_LADDERPASS:
        case dspu::FLT_BT_RLC_LADDERREJ: case dspu::FLT_MT_RLC_LADDERREJ:
        case dspu::FLT_BT_BWC_LADDERPASS: case dspu::FLT_MT_BWC_LADDERPASS:
        case dspu::FLT_BT_BWC_LADDERREJ: case dspu::FLT_MT_BWC_LADDERREJ:
        case dspu::FLT_BT_LRX_LADDERPASS: case dspu::FLT_MT_LRX_LADDERPASS:
        case dspu::FLT_BT_LRX_LADDERREJ: case dspu::FLT_MT_LRX_LADDERREJ:
        case dspu::FLT_DR_APO_LADDERPASS: case dspu::FLT_DR_APO_LADDERREJ:
            return true;
    }
    return false;
}

dspu::equalizer_mode_t get_eq_mode(int32_t mode) {
    switch (mode) {
        case 0: return dspu::EQM_IIR;
        case 1: return dspu::EQM_FIR;
        case 2: return dspu::EQM_FFT;
        case 3: return dspu::EQM_SPM;
        default: break;
    }
    return dspu::EQM_BYPASS;
}

dspu::over_mode_t calc_oversampler_mode(dspu::equalizer_mode_t eq_mode, size_t decramp) {
    if ((eq_mode != dspu::EQM_IIR) && (eq_mode != dspu::EQM_FIR))
        return dspu::over_mode_t::OM_NONE;
    switch (decramp) {
        case 2: return dspu::over_mode_t::OM_LANCZOS_2X16BIT;
        case 3: return dspu::over_mode_t::OM_LANCZOS_3X16BIT;
        case 4: return dspu::over_mode_t::OM_LANCZOS_4X16BIT;
        case 6: return dspu::over_mode_t::OM_LANCZOS_6X16BIT;
        case 8: return dspu::over_mode_t::OM_LANCZOS_8X16BIT;
        default: break;
    }
    return dspu::over_mode_t::OM_NONE;
}

/**
 * Az EQ késleltetése az eredeti mintavételen. A dspu::Equalizer a túlmintavételezett tartományban fut, így a
 * get_latency() ottani mintákban adja meg (FIR módban x4-nél 6144 belső minta = 1536 eredeti). A para_equalizer
 * plugin ezt osztás nélkül adja össze az oversampler késleltetésével; a mért valódi késleltetés az osztott érték
 * (T06 jelentés). IIR-nél 0, FFT / SPM módban nincs túlmintavételezés (1×), ott nem változik semmi.
 */
size_t eqLatencyBase(size_t eqLatency, size_t oversampling) {
    return oversampling > 1 ? eqLatency / oversampling : eqLatency;
}

/** Egy sáv LSP szűrőparaméterei (para_equalizer::update_settings, solo/mute/inspect nélkül). */
void band_params(const ParamData& d, size_t j, dspu::filter_params_t* fp) {
    if (j >= std::min<size_t>(d.numBands, EQ_FILTERS)) {
        fp->nType = dspu::FLT_NONE;
        fp->nSlope = 1;
        fp->fFreq = fp->fFreq2 = 1000.0f;
        fp->fGain = 1.0f;
        fp->fQuality = 0.0f;
        return;
    }
    const EqBandPod& b = d.bands[j];
    fp->nType = static_cast<uint32_t>(b.type);
    fp->nSlope = static_cast<uint32_t>(b.slope) + 1;
    decode_filter(&fp->nType, &fp->nSlope, static_cast<size_t>(b.mode));

    const float pitch = 1.0f; // dspu::semitones_to_frequency_shift(0)
    if (filter_has_width(fp->nType)) {
        const float center = b.freq * pitch;
        const float k = powf(2, (b.width * 0.5f));
        fp->fFreq = center / k;
        fp->fFreq2 = center * k;
    } else {
        fp->fFreq = b.freq * pitch;
        fp->fFreq2 = fp->fFreq;
    }
    fp->fGain = (adjust_gain(fp->nType)) ? dspu::db_to_gain(b.gainDb) : 1.0f;
    fp->fQuality = b.q;
}

} // namespace

uint32_t Equalizer::decrampFactor(int32_t decramp, uint32_t sampleRate) {
    // para_equalizer::calc_decramping
    if (sampleRate == 0) return 1;
    const float kf = 44100.0f / static_cast<float>(sampleRate);
    uint32_t exp = 1;
    switch (decramp) {
        case 0: exp = 1; break;
        case 1: exp = 2; break;
        case 2: exp = 3; break;
        case 3: exp = 4; break;
        case 4: exp = 6; break;
        case 5: exp = 8; break;
        default: exp = 1; break;
    }
    exp = static_cast<uint32_t>(ceilf(exp * kf));
    switch (exp) {
        case 0:
        case 1: return 1;
        case 2: case 3: case 4: case 6: case 8: return exp;
        case 5: case 7: return exp + 1;
        default: break;
    }
    return 8;
}

struct Equalizer::Impl {
    struct Filter {
        dspu::filter_params_t sFP{};
        dspu::filter_params_t sOldFP{};
    };
    struct Channel {
        dspu::Equalizer sEqualizer;
        dspu::Oversampler sOversampler;
        Filter vFilters[EQ_FILTERS];
        float vOutBuffer[EQ_BUFFER_SIZE];
    };
    Channel ch[2];
    float vBuffer[EQ_BUFFER_SIZE];
    uint32_t fSampleRate = 0;
    uint32_t nDecramp = 1;
    bool bSmoothMode = false;
    size_t latency = 0;

    Impl() {
        initDsp();
        for (Channel& c : ch) {
            c.sOversampler.init();
            c.sOversampler.set_filtering(false);
            c.sEqualizer.init(EQ_FILTERS, EQ_RANK);
            c.sEqualizer.set_smooth(true);
            for (Filter& f : c.vFilters) {
                f.sOldFP.nType = dspu::FLT_NONE; f.sOldFP.fFreq = 0.0f; f.sOldFP.fFreq2 = 0.0f;
                f.sOldFP.fGain = 1.0f; f.sOldFP.nSlope = 0; f.sOldFP.fQuality = 0.0f;
                f.sFP = f.sOldFP;
            }
        }
        std::memset(vBuffer, 0, sizeof(vBuffer));
    }

    void update(const ParamData& d) {
        if (fSampleRate == 0) return;
        const dspu::equalizer_mode_t eq_mode = get_eq_mode(d.eqMode);
        bool mode_changed = false;
        bool decramp_changed = false;
        bSmoothMode = false;
        const uint32_t decramp = Equalizer::decrampFactor(d.eqDecramp, fSampleRate);
        const dspu::over_mode_t over_mode = calc_oversampler_mode(eq_mode, decramp);
        if (decramp != nDecramp) {
            nDecramp = decramp;
            decramp_changed = true;
        }
        for (Channel& c : ch) {
            c.sOversampler.set_sample_rate(fSampleRate);
            c.sOversampler.set_mode(over_mode);
            if (c.sOversampler.modified()) {
                c.sOversampler.update_settings();
                decramp_changed = true;
            }
            if (c.sEqualizer.mode() != eq_mode) {
                c.sEqualizer.set_mode(eq_mode);
                mode_changed = true;
            }
            c.sEqualizer.set_sample_rate(fSampleRate * nDecramp);
            c.sEqualizer.set_actual_sample_rate(fSampleRate);

            for (size_t j = 0; j < EQ_FILTERS; ++j) {
                Filter& f = c.vFilters[j];
                f.sOldFP = f.sFP;
                dspu::filter_params_t* fp = &f.sFP;
                dspu::filter_params_t* op = &f.sOldFP;
                band_params(d, j, fp);
                c.sEqualizer.limit_params(j, fp);
                const bool type_changed = (fp->nType != op->nType) || (fp->nSlope != op->nSlope);
                const bool param_changed = (fp->fGain != op->fGain) || (fp->fFreq != op->fFreq) ||
                                           (fp->fFreq2 != op->fFreq2) || (fp->fQuality != op->fQuality);
                if (type_changed || param_changed || decramp_changed) {
                    c.sEqualizer.set_params(j, fp);
                    if (type_changed) mode_changed = true;
                    if (param_changed) bSmoothMode = true;
                }
            }
        }
        // Do not enable smooth mode if significant changes have been applied
        if (mode_changed || (eq_mode != dspu::EQM_IIR))
            bSmoothMode = false;

        size_t lat = 0;
        for (Channel& c : ch) {
            const size_t ovs_lat = c.sOversampler.latency();
            // Módváltáskor az új FIR/FFT kernel közvetlenül lép életbe (get_latency() itt építi fel).
            // Simítással az LSP az első blokkban még a régi kernellel számolna és abból úsztatna át, de SPM után
            // ott nem FIR kernel van (hanem SPM magnitúdó) -> kattanás. A váltást a lánc úgyis le-fel úsztatja.
            if (mode_changed) c.sEqualizer.set_smooth(false);
            const size_t eq_lat = c.sEqualizer.get_latency();
            if (mode_changed) c.sEqualizer.set_smooth(true);
            lat = std::max(lat, ovs_lat + eqLatencyBase(eq_lat, c.sOversampler.get_oversampling()));
        }
        latency = lat;
    }

    void process_channel(Channel& c, const float* in, size_t start, size_t samples, size_t total_samples) {
        const size_t oversampling = c.sOversampler.get_oversampling();
        const size_t ovs_count = EQ_BUFFER_SIZE / oversampling;

        if (bSmoothMode) {
            dspu::filter_params_t fp;
            const float den = 1.0f / total_samples;
            for (size_t offset = 0; offset < samples;) {
                const size_t count = std::min(samples - offset, EQ_SMOOTH_STEP);
                const float k = float(start + offset) * den;
                for (size_t j = 0; j < EQ_FILTERS; ++j) {
                    Filter& f = c.vFilters[j];
                    fp.nType = f.sFP.nType;
                    fp.fFreq = f.sOldFP.fFreq * expf(logf(f.sFP.fFreq / f.sOldFP.fFreq) * k);
                    fp.fFreq2 = f.sOldFP.fFreq2 * expf(logf(f.sFP.fFreq2 / f.sOldFP.fFreq2) * k);
                    fp.nSlope = f.sFP.nSlope;
                    fp.fGain = f.sOldFP.fGain * expf(logf(f.sFP.fGain / f.sOldFP.fGain) * k);
                    fp.fQuality = f.sOldFP.fQuality + (f.sFP.fQuality - f.sOldFP.fQuality) * k;
                    c.sEqualizer.set_params(j, &fp);
                }
                if (oversampling > 1) {
                    for (size_t j = 0; j < count;) {
                        const size_t to_do = std::min(count - j, ovs_count);
                        c.sOversampler.upsample(vBuffer, &in[offset + j], to_do);
                        c.sEqualizer.process(vBuffer, vBuffer, to_do * oversampling);
                        c.sOversampler.downsample(&c.vOutBuffer[offset + j], vBuffer, to_do);
                        j += to_do;
                    }
                } else {
                    c.sEqualizer.process(&c.vOutBuffer[offset], &in[offset], count);
                }
                offset += count;
            }
        } else {
            if (oversampling > 1) {
                for (size_t j = 0; j < samples;) {
                    const size_t to_do = std::min(samples - j, ovs_count);
                    c.sOversampler.upsample(vBuffer, &in[j], to_do);
                    c.sEqualizer.process(vBuffer, vBuffer, to_do * oversampling);
                    c.sOversampler.downsample(&c.vOutBuffer[j], vBuffer, to_do);
                    j += to_do;
                }
            } else {
                c.sEqualizer.process(c.vOutBuffer, in, samples);
            }
        }
    }

    void process(float* L, float* R, size_t samples) {
        float* io[2] = { L, R };
        for (size_t offset = 0; offset < samples;) {
            const size_t to_process = std::min(samples - offset, EQ_BUFFER_SIZE);
            for (size_t i = 0; i < 2; ++i) {
                process_channel(ch[i], io[i] + offset, offset, to_process, samples);
                std::memcpy(io[i] + offset, ch[i].vOutBuffer, to_process * sizeof(float));
            }
            offset += to_process;
        }
        // Reset smooth mode: apply actual settings of equalizer at the end
        if (bSmoothMode) {
            for (Channel& c : ch)
                for (size_t j = 0; j < EQ_FILTERS; ++j) c.sEqualizer.set_params(j, &c.vFilters[j].sFP);
            bSmoothMode = false;
        }
    }
};

Equalizer::Equalizer() : p_(std::make_unique<Impl>()) {}
Equalizer::~Equalizer() = default;

void Equalizer::setSampleRate(uint32_t sr) {
    p_->fSampleRate = sr;
    for (auto& c : p_->ch) c.sOversampler.set_sample_rate(sr);
}
void Equalizer::update(const ParamData& d) { p_->update(d); }
void Equalizer::process(float* L, float* R, size_t n) {
    p_->process(L, R, n);
    // A FIR/FFT/SPM átkonfigurálás a feldolgozáskor történik; a késleltetés utána érvényes
    size_t lat = 0;
    for (auto& c : p_->ch)
        lat = std::max(lat, c.sOversampler.latency() + eqLatencyBase(c.sEqualizer.get_latency(), c.sOversampler.get_oversampling()));
    p_->latency = lat;
}
size_t Equalizer::latency() const { return p_->latency; }
void Equalizer::reset() {
    for (auto& c : p_->ch) c.sEqualizer.reset();
}

void Equalizer::response(const ParamData& d, uint32_t sampleRate, const float* freqs, size_t count, float* magDb,
                         float* phaseDeg, size_t* latencySamples) {
    initDsp();
    dspu::Equalizer eq;
    eq.init(EQ_FILTERS, 0);
    const uint32_t decramp = decrampFactor(d.eqDecramp, sampleRate);
    eq.set_mode(dspu::EQM_IIR);
    eq.set_sample_rate(sampleRate * decramp);
    eq.set_actual_sample_rate(sampleRate);
    for (size_t j = 0; j < EQ_FILTERS; ++j) {
        dspu::filter_params_t fp{};
        band_params(d, j, &fp);
        eq.limit_params(j, &fp);
        eq.set_params(j, &fp);
    }
    std::vector<float> re(count), im(count);
    eq.freq_chart(re.data(), im.data(), freqs, count);
    for (size_t i = 0; i < count; ++i) {
        const float m = std::hypot(re[i], im[i]);
        magDb[i] = 20.0f * std::log10(std::max(m, 1e-6f));
        phaseDeg[i] = std::atan2(im[i], re[i]) * 57.29577951f;
    }
    if (latencySamples) {
        // A feldolgozás késleltetése ugyanígy beállított példányon (EQ mód + decramp túlmintavételezés)
        Equalizer probe;
        probe.setSampleRate(sampleRate);
        probe.update(d);
        float l[16] = {}, r[16] = {};
        probe.process(l, r, 16);
        *latencySamples = probe.latency();
    }
}

// ============================================================ Limiter (limiter_stereo)
namespace {

constexpr size_t LIMIT_BUFSIZE = 8192;
constexpr size_t LIMIT_BUFMULTIPLE = 16;
constexpr size_t MAX_SAMPLE_RATE = 384000;      // lsp-plugin-fw
constexpr size_t OVERSAMPLING_MAX = 8;          // meta::limiter_metadata
constexpr float LOOKAHEAD_MAX = 20.0f;

enum { OVS_NONE,
       OVS_HALF_2X16BIT, OVS_HALF_2X24BIT, OVS_HALF_3X16BIT, OVS_HALF_3X24BIT, OVS_HALF_4X16BIT, OVS_HALF_4X24BIT,
       OVS_HALF_6X16BIT, OVS_HALF_6X24BIT, OVS_HALF_8X16BIT, OVS_HALF_8X24BIT,
       OVS_FULL_2X16BIT, OVS_FULL_2X24BIT, OVS_FULL_3X16BIT, OVS_FULL_3X24BIT, OVS_FULL_4X16BIT, OVS_FULL_4X24BIT,
       OVS_FULL_6X16BIT, OVS_FULL_6X24BIT, OVS_FULL_8X16BIT, OVS_FULL_8X24BIT,
       OVS_TRUE_PEAK_16BIT, OVS_TRUE_PEAK_24BIT };

struct true_peak_mode_t {
    uint32_t frequency;
    dspu::over_mode_t modes[2];
};
const true_peak_mode_t true_peak_modes[] = {
    { 0,      { dspu::OM_LANCZOS_8X16BIT, dspu::OM_LANCZOS_8X24BIT } },
    { 22050,  { dspu::OM_LANCZOS_8X16BIT, dspu::OM_LANCZOS_8X24BIT } },
    { 29400,  { dspu::OM_LANCZOS_6X16BIT, dspu::OM_LANCZOS_6X24BIT } },
    { 44100,  { dspu::OM_LANCZOS_4X16BIT, dspu::OM_LANCZOS_4X24BIT } },
    { 58800,  { dspu::OM_LANCZOS_3X16BIT, dspu::OM_LANCZOS_3X24BIT } },
    { 88200,  { dspu::OM_LANCZOS_2X16BIT, dspu::OM_LANCZOS_2X24BIT } },
    { 176400, { dspu::OM_NONE, dspu::OM_NONE } },
};

dspu::over_mode_t get_oversampling_mode(size_t mode, uint32_t srate) {
#define L_KEY(x) case OVS_HALF_##x: case OVS_FULL_##x: return dspu::OM_LANCZOS_##x;
    switch (mode) {
        L_KEY(2X16BIT) L_KEY(2X24BIT) L_KEY(3X16BIT) L_KEY(3X24BIT) L_KEY(4X16BIT)
        L_KEY(4X24BIT) L_KEY(6X16BIT) L_KEY(6X24BIT) L_KEY(8X16BIT) L_KEY(8X24BIT)
        case OVS_TRUE_PEAK_16BIT:
        case OVS_TRUE_PEAK_24BIT: {
            const size_t index = (mode == OVS_TRUE_PEAK_24BIT) ? 1 : 0;
            size_t best = 0;
            for (size_t i = 1; i < sizeof(true_peak_modes) / sizeof(true_peak_mode_t); ++i)
                if (true_peak_modes[i].frequency <= srate) best = i;
            return true_peak_modes[best].modes[index];
        }
        case OVS_NONE:
        default:
            return dspu::OM_NONE;
    }
#undef L_KEY
}

bool get_filtering(size_t mode) { return (mode >= OVS_FULL_2X16BIT) && (mode <= OVS_FULL_8X24BIT); }

dspu::limiter_mode_t get_limiter_mode(size_t mode) {
    switch (mode) {
        case 0: return dspu::LM_HERM_THIN;
        case 1: return dspu::LM_HERM_WIDE;
        case 2: return dspu::LM_HERM_TAIL;
        case 3: return dspu::LM_HERM_DUCK;
        case 4: return dspu::LM_EXP_THIN;
        case 5: return dspu::LM_EXP_WIDE;
        case 6: return dspu::LM_EXP_TAIL;
        case 7: return dspu::LM_EXP_DUCK;
        case 8: return dspu::LM_LINE_THIN;
        case 9: return dspu::LM_LINE_WIDE;
        case 10: return dspu::LM_LINE_TAIL;
        case 11: return dspu::LM_LINE_DUCK;
        default: break;
    }
    return dspu::LM_HERM_THIN;
}

size_t get_dithering(size_t mode) {
    static const size_t bits[] = { 0, 7, 8, 11, 12, 15, 16, 23, 24 };
    return mode < sizeof(bits) / sizeof(bits[0]) ? bits[mode] : 0;
}

} // namespace

struct Limiter::Impl {
    struct Channel {
        dspu::Oversampler sOver;
        dspu::Oversampler sScOver;
        dspu::Limiter sLimit;
        dspu::Delay sDataDelay;
        float vDataBuf[LIMIT_BUFSIZE];
        float vScBuf[LIMIT_BUFSIZE];
        float vGainBuf[LIMIT_BUFSIZE];
        float vOutBuf[LIMIT_BUFSIZE];
    };
    Channel ch[2];
    dspu::Dither sDither;
    uint32_t fSampleRate = 0;
    float fOutGain = 1.0f;
    float fStereoLink = 1.0f;
    float inGain = 1.0f, outGain = 1.0f; // EasyEffects bemeneti / kimeneti erősítés
    size_t latency = 0;

    Impl() {
        initDsp();
        const float lk_latency = int(dspu::samples_to_millis(MAX_SAMPLE_RATE, OVERSAMPLING_MAX)) + LOOKAHEAD_MAX + 1.0f;
        for (Channel& c : ch) {
            c.sOver.init();
            c.sScOver.init();
            c.sLimit.init(MAX_SAMPLE_RATE * OVERSAMPLING_MAX, lk_latency);
            c.sDataDelay.init(dspu::millis_to_samples(MAX_SAMPLE_RATE * OVERSAMPLING_MAX, lk_latency) + LIMIT_BUFSIZE);
            std::memset(c.vDataBuf, 0, sizeof(c.vDataBuf));
            std::memset(c.vScBuf, 0, sizeof(c.vScBuf));
            std::memset(c.vGainBuf, 0, sizeof(c.vGainBuf));
            std::memset(c.vOutBuf, 0, sizeof(c.vOutBuf));
        }
        sDither.init();
    }

    void update_sample_rate(uint32_t sr) {
        fSampleRate = sr;
        for (Channel& c : ch) {
            c.sOver.set_sample_rate(sr);
            c.sScOver.set_sample_rate(sr);
            const size_t real_sample_rate = c.sOver.get_oversampling() * sr;
            c.sLimit.set_mode(dspu::LM_HERM_THIN);
            c.sLimit.set_sample_rate(real_sample_rate);
        }
    }

    void update(const ParamData& d) {
        if (fSampleRate == 0) return;
        const size_t ovs_mode = static_cast<size_t>(d.limOversampling);
        const dspu::over_mode_t mode = get_oversampling_mode(ovs_mode, fSampleRate);
        const bool filtering = get_filtering(ovs_mode);
        const size_t dither = get_dithering(static_cast<size_t>(d.limDither));
        const float thresh = dspu::db_to_gain(d.limThresholdDb);      // EasyEffects: BIND_LV2_PORT_DB("th")
        const float lk_ahead = d.limLookaheadMs;
        const float attack = d.limAttackMs;
        const float release = d.limReleaseMs;
        const float knee_level = dspu::db_to_gain(d.limAlrKneeDb);    // "knee" (dB -> lineáris)
        const float knee_smooth = dspu::db_to_gain(d.limAlrKneeSmoothDb);
        const bool alr_on = d.limAlr != 0;
        const float alr_attack = d.limAlrAttackMs;
        const float alr_release = d.limAlrReleaseMs;
        fStereoLink = d.limStereoLink * 0.01f;
        const bool boost = d.limGainBoost != 0;
        fOutGain = 1.0f;                                              // a plugin "g_out" portja (EasyEffects: 1)
        if (boost) fOutGain /= thresh;
        const dspu::limiter_mode_t op_mode = get_limiter_mode(static_cast<size_t>(d.limMode));
        inGain = dbToGain(d.limInputGainDb);
        outGain = dbToGain(d.limOutputGainDb);

        sDither.set_bits(dither);
        for (Channel& c : ch) {
            c.sOver.set_mode(mode);
            c.sOver.set_filtering(filtering);
            if (c.sOver.modified()) c.sOver.update_settings();
            c.sScOver.set_mode(mode);
            c.sScOver.set_filtering(false);
            if (c.sScOver.modified()) c.sScOver.update_settings();

            const size_t real_sample_rate = c.sOver.get_oversampling() * fSampleRate;
            // Update lookahead because oversampling adds extra latency
            const float lk_ahead_ch = lk_ahead + dspu::samples_to_millis(fSampleRate, c.sScOver.latency());
            // Cleanup the data delay if limiter's sample rate is going to change
            if (c.sLimit.sample_rate() != real_sample_rate) c.sDataDelay.clear();

            c.sLimit.set_mode(op_mode);
            c.sLimit.set_sample_rate(real_sample_rate);
            c.sLimit.set_lookahead(lk_ahead_ch);
            c.sLimit.set_threshold(thresh, !boost);
            c.sLimit.set_attack(attack);
            c.sLimit.set_release(release);
            c.sLimit.set_knee(knee_level);
            c.sLimit.set_alr_knee(knee_smooth);
            c.sLimit.set_alr(alr_on);
            c.sLimit.set_alr_attack(alr_attack);
            c.sLimit.set_alr_release(alr_release);
            c.sLimit.update_settings();

            c.sDataDelay.set_delay(c.sLimit.get_latency());
        }
        // sync_latency()
        const Channel& c0 = ch[0];
        latency = c0.sLimit.get_latency() / c0.sScOver.get_oversampling() + c0.sScOver.latency();
    }

    float process(float* L, float* R, size_t samples) {
        float* io[2] = { L, R };
        if (inGain != 1.0f)
            for (float* p : io) for (size_t i = 0; i < samples; ++i) p[i] *= inGain;

        const size_t times = ch[0].sOver.get_oversampling();
        const size_t buf_size = (LIMIT_BUFSIZE / times) & (~(LIMIT_BUFMULTIPLE - 1));
        float minGain = 1.0f;
        size_t done = 0;
        for (size_t nsamples = samples; nsamples > 0;) {
            const size_t to_do = std::min(buf_size, nsamples);
            const size_t to_doxn = to_do * times;
            for (size_t i = 0; i < 2; ++i) {
                Channel& c = ch[i];
                c.sOver.upsample(c.vDataBuf, io[i] + done, to_do);
                // Internal sidechain (fPreamp == 1)
                dsp::copy(c.vScBuf, c.vDataBuf, to_doxn);
                c.sLimit.process(c.vGainBuf, c.vScBuf, to_doxn);
                c.sDataDelay.process(c.vDataBuf, c.vDataBuf, to_doxn);
            }
            // Perform stereo linking
            {
                float* cl = ch[0].vGainBuf;
                float* cr = ch[1].vGainBuf;
                for (size_t i = 0; i < to_doxn; ++i) {
                    const float gl = cl[i];
                    const float gr = cr[i];
                    if (gl < gr) cr[i] = gr + (gl - gr) * fStereoLink;
                    else cl[i] = gl + (gr - gl) * fStereoLink;
                }
            }
            for (size_t i = 0; i < 2; ++i) {
                Channel& c = ch[i];
                dsp::fmmul_k3(c.vDataBuf, c.vGainBuf, fOutGain, to_doxn);
                minGain = std::min(minGain, dsp::min(c.vGainBuf, to_doxn));
                c.sOver.downsample(c.vOutBuf, c.vDataBuf, to_do);
                sDither.process(c.vOutBuf, c.vOutBuf, to_do);
                std::memcpy(io[i] + done, c.vOutBuf, to_do * sizeof(float));
            }
            done += to_do;
            nsamples -= to_do;
        }
        if (outGain != 1.0f)
            for (float* p : io) for (size_t i = 0; i < samples; ++i) p[i] *= outGain;
        return minGain;
    }
};

Limiter::Limiter() : p_(std::make_unique<Impl>()) {}
Limiter::~Limiter() = default;
void Limiter::setSampleRate(uint32_t sr) { p_->update_sample_rate(sr); }
void Limiter::update(const ParamData& d) { p_->update(d); }
void Limiter::process(float* L, float* R, size_t n) {
    const float g = p_->process(L, R, n);
    grDb_.store(g < 1.0f ? -20.0f * std::log10(std::max(g, 1e-6f)) : 0.0f, std::memory_order_relaxed);
}
size_t Limiter::latency() const { return p_->latency; }
void Limiter::reset() {
    for (auto& c : p_->ch) c.sDataDelay.clear();
}

} // namespace ee_analyzer::ee
