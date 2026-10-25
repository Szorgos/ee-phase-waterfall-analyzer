#pragma once
/**
 * @file ee_dsp.hpp
 * @brief Az EasyEffects-szel egyező DSP modulok (src/ee_dsp_*.cpp).
 *
 * Mindegyik modul ugyanazt a könyvtárat és ugyanazt a beállítás -> DSP leképezést használja,
 * mint az EasyEffects:
 *   - Equalizer:     LSP Parametric Equalizer x32 (lsp-dsp-units dspu::Equalizer + dspu::Oversampler),
 *                    a para_equalizer plugin update_settings()/process() logikájával
 *   - Limiter:       LSP Limiter Stereo (dspu::Limiter + Oversampler + Delay + Dither), a limiter plugin logikájával
 *   - BassEnhancer:  Calf Bass Enhancer (tap_distortion + RBJ biquadok)
 *   - Crossfeed:     libbs2b
 *   - Convolver:     zita-convolver (FFTW3), IR újramintavételezés SpeexDSP-vel, EasyEffects kernel-előkészítés
 *
 * A process() függvények valós idejű szálból hívhatók (nem foglalnak memóriát, nem várnak zárra).
 * A konvolver példányokat (FFTW tervek, zita szálak) kizárólag a háttérszál hozza létre és szabadítja fel.
 */
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "param_block.hpp"

namespace ee_analyzer::ee {

/** Az LSP DSP könyvtár inicializálása (CPU-specifikus SIMD függvények kiválasztása). Többször is hívható. */
void initDsp();

/** Valós idejű feldolgozási környezet (denormal -> 0, mint az LSP plugin-burkolókban). */
class DspScope {
public:
    DspScope();
    ~DspScope();
    DspScope(const DspScope&) = delete;
    DspScope& operator=(const DspScope&) = delete;
private:
    alignas(16) unsigned char ctx_[64];
};

inline float dbToGain(float db) { return db <= -100.0f ? 0.0f : static_cast<float>(std::pow(10.0, db / 20.0)); }

// ============================================================ Equalizer
class Equalizer {
public:
    Equalizer();
    ~Equalizer();
    Equalizer(const Equalizer&) = delete;
    Equalizer& operator=(const Equalizer&) = delete;

    /** Nem valós idejű. */
    void setSampleRate(uint32_t sr);
    /** Beállítások átvétele (a plugin update_settings() megfelelője; valós idejű szálból is hívható). */
    void update(const ParamData& d);
    /** Helyben végzett sztereó feldolgozás. */
    void process(float* L, float* R, size_t n);
    /** Késleltetés mintában (FIR/FFT/SPM mód és decramp túlmintavételezés). */
    size_t latency() const;
    void reset();

    /**
     * Az EQ eredő átviteli függvénye (a sávok LSP-féle frekvenciamenete, ugyanazzal a decramp
     * mintavételi frekvenciával, mint a feldolgozás). Nem valós idejű.
     */
    static void response(const ParamData& d, uint32_t sampleRate, const float* freqs, size_t count,
                         float* magDb, float* phaseDeg, size_t* latencySamples = nullptr);
    /** A decramp beállításból adódó túlmintavételezés (para_equalizer::calc_decramping). */
    static uint32_t decrampFactor(int32_t decramp, uint32_t sampleRate);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// ============================================================ Limiter
class Limiter {
public:
    Limiter();
    ~Limiter();
    Limiter(const Limiter&) = delete;
    Limiter& operator=(const Limiter&) = delete;

    void setSampleRate(uint32_t sr);
    void update(const ParamData& d);
    void process(float* L, float* R, size_t n);
    size_t latency() const;
    /** A legutóbbi blokk legnagyobb erősítéscsökkentése dB-ben (pozitív szám). */
    float gainReductionDb() const { return grDb_.load(std::memory_order_relaxed); }
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    std::atomic<float> grDb_{0.0f};
};

// ============================================================ Bass Enhancer (Calf)
class BassEnhancer {
public:
    BassEnhancer();
    ~BassEnhancer();
    BassEnhancer(const BassEnhancer&) = delete;
    BassEnhancer& operator=(const BassEnhancer&) = delete;

    void setSampleRate(uint32_t sr);
    void update(const ParamData& d);
    void process(float* L, float* R, size_t n);
    void reset();
    /** A Calf "meter_drive" kimenete (harmonikus szint, dB). */
    float harmonicsDb() const { return harmonicsDb_.load(std::memory_order_relaxed); }

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    std::atomic<float> harmonicsDb_{-100.0f};
};

// ============================================================ Crossfeed (libbs2b)
class Crossfeed {
public:
    Crossfeed();
    ~Crossfeed();
    Crossfeed(const Crossfeed&) = delete;
    Crossfeed& operator=(const Crossfeed&) = delete;

    void setSampleRate(uint32_t sr);
    void update(const ParamData& d);
    void process(float* L, float* R, size_t n);
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// ============================================================ Convolver (zita-convolver)

/** Konvolúciós kernel (sztereó), EasyEffects ConvolverKernelManager::KernelData megfelelője. */
struct ConvKernel {
    uint32_t rate = 0;
    std::vector<float> left, right;
    bool valid() const { return rate > 0 && !left.empty() && left.size() == right.size(); }
};

/**
 * Kernel előkészítése: monó -> sztereó, újramintavételezés SpeexDSP-vel (QUALITY_DESKTOP),
 * sztereó szélesség (ir_width) és autogain, ahogy az EasyEffects teszi. Nem valós idejű.
 */
ConvKernel prepareConvKernel(const std::vector<float>& left, const std::vector<float>& right, uint32_t irRate,
                             uint32_t targetRate, int irWidth, bool autogain);

/**
 * Egy zita-convolver példány a hozzá tartozó blokkméret-illesztéssel (EasyEffects Convolver::process).
 * Létrehozás és törlés csak a háttérszálon.
 */
class ZitaInstance {
public:
    ZitaInstance();
    ~ZitaInstance();
    ZitaInstance(const ZitaInstance&) = delete;
    ZitaInstance& operator=(const ZitaInstance&) = delete;

    /** @param quantum a hoszt blokkmérete (ebből számolja az EasyEffects a zita blokkméretét) */
    bool init(const ConvKernel& k, uint32_t quantum);
    uint32_t quantum() const { return quantum_; }
    /**
     * A kimenet késleltetése a bemenethez képest (minta), hoszt-blokkméret mellett. Kettőhatvány blokknál közvetlen
     * a feldolgozás (0); más blokkméretnél a zita kettőhatvány blokkjai miatt a blokkméret és a blokk maradéka.
     */
    static size_t latencyFor(uint32_t quantum);
    /** Valós idejű: n == quantum() mintát dolgoz fel helyben; false, ha a példány leállt (túlterhelés). */
    bool process(float* L, float* R, size_t n);
    bool failed() const { return failed_.load(std::memory_order_relaxed); }
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    uint32_t quantum_ = 0;
    std::atomic<bool> failed_{false};
};

} // namespace ee_analyzer::ee
