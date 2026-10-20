/**
 * @file ee_dsp_misc.cpp
 * @brief Bass Enhancer (Calf) és Crossfeed (libbs2b), az EasyEffects beállítás-leképezésével.
 *
 * EasyEffects (src/bass_enhancer.cpp, src/crossfeed.cpp, src/plugin_base.cpp):
 *   - minden modul: bemeneti erősítés -> plugin -> kimeneti erősítés
 *   - Bass Enhancer: "amount" dB -> lineáris, "drive" = harmonics, "freq" = scope, "floor", "blend",
 *     "floor_active", "listen"; a Calf saját level_in / level_out portja 1
 *   - Crossfeed: bs2b fcut (Hz) és feed (dB * 10)
 */
#include "ee_dsp.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "calf_bass_enhancer.h"
extern "C" {
#include "bs2b.h"
}

namespace ee_analyzer::ee {

// ============================================================ Bass Enhancer (Calf)
struct BassEnhancer::Impl {
    calf_plugins::calf_bass_enhancer calf;
    float inGain = 1.0f, outGain = 1.0f;
    uint32_t sr = 0;
};

BassEnhancer::BassEnhancer() : p_(std::make_unique<Impl>()) {}
BassEnhancer::~BassEnhancer() = default;

void BassEnhancer::setSampleRate(uint32_t sr) {
    if (sr == p_->sr) return;
    // Az LV2 hoszt új mintavételnél új példányt hoz létre (instantiate -> set_sample_rate -> activate)
    p_->calf = calf_plugins::calf_bass_enhancer{};
    p_->sr = sr;
    p_->calf.set_sample_rate(sr);
    p_->calf.activate();
}

void BassEnhancer::update(const ParamData& d) {
    auto& c = p_->calf.p;
    c.amount = static_cast<float>(std::pow(10.0, d.bassAmountDb / 20.0)); // BIND_LV2_PORT_DB("amount", ..., false)
    c.drive = d.bassHarmonics;
    c.freq = d.bassScopeHz;
    c.floor = d.bassFloorHz;
    c.blend = d.bassBlend;
    c.floor_active = d.bassFloorActive ? 1.0f : 0.0f;
    c.listen = d.bassListen ? 1.0f : 0.0f;
    p_->calf.params_changed();
    p_->inGain = dbToGain(d.bassInputGainDb);
    p_->outGain = dbToGain(d.bassOutputGainDb);
}

void BassEnhancer::process(float* L, float* R, size_t n) {
    if (p_->sr == 0) return;
    if (p_->inGain != 1.0f)
        for (size_t i = 0; i < n; ++i) { L[i] *= p_->inGain; R[i] *= p_->inGain; }
    p_->calf.process(L, R, L, R, static_cast<uint32_t>(n));
    if (p_->outGain != 1.0f)
        for (size_t i = 0; i < n; ++i) { L[i] *= p_->outGain; R[i] *= p_->outGain; }
    const float h = p_->calf.harmonics_level();
    harmonicsDb_.store(h > 1e-5f ? 20.0f * std::log10(h) : -100.0f, std::memory_order_relaxed);
}

void BassEnhancer::reset() { p_->calf.reset(); }

// ============================================================ Crossfeed (libbs2b)
struct Crossfeed::Impl {
    t_bs2bdp bs2b = nullptr;
    std::vector<float> data;
    int fcut = 0, feed = 0;
    Impl() {
        bs2b = bs2b_open();
        data.resize(2 * 8192);
    }
    ~Impl() {
        if (bs2b) bs2b_close(bs2b);
    }
};

Crossfeed::Crossfeed() : p_(std::make_unique<Impl>()) {}
Crossfeed::~Crossfeed() = default;

void Crossfeed::setSampleRate(uint32_t sr) {
    if (p_->bs2b && static_cast<uint32_t>(bs2b_get_srate(p_->bs2b)) != sr) bs2b_set_srate(p_->bs2b, sr);
}

void Crossfeed::update(const ParamData& d) {
    if (!p_->bs2b) return;
    // bs2b: fcut Hz-ben, feed tized dB-ben. (Az EasyEffects 8.x a feed-et egész dB-re vágja, és az fcut
    // változását tévesen a feed-re küldi; itt a könyvtár mindkét értéket a beállított pontossággal kapja,
    // így a 4,5 dB-es Bauer és a 9,5 dB-es Jan Meier szint pontosan érvényesül.)
    const int fcut = static_cast<int>(std::lround(d.crossfeedFcut));
    const int feed = static_cast<int>(std::lround(d.crossfeedFeedDb * 10.0f));
    if (fcut != p_->fcut || feed != p_->feed) {
        bs2b_set_level(p_->bs2b, static_cast<uint32_t>(fcut) | (static_cast<uint32_t>(feed) << 16));
        p_->fcut = fcut;
        p_->feed = feed;
    }
}

void Crossfeed::process(float* L, float* R, size_t n) {
    if (!p_->bs2b) return;
    float* data = p_->data.data();
    size_t done = 0;
    while (done < n) {
        const size_t m = std::min<size_t>(n - done, p_->data.size() / 2);
        for (size_t i = 0; i < m; ++i) { data[2 * i] = L[done + i]; data[2 * i + 1] = R[done + i]; }
        bs2b_cross_feed_f(p_->bs2b, data, static_cast<int>(m));
        for (size_t i = 0; i < m; ++i) { L[done + i] = data[2 * i]; R[done + i] = data[2 * i + 1]; }
        done += m;
    }
}

void Crossfeed::reset() {
    if (p_->bs2b) bs2b_clear(p_->bs2b);
}

} // namespace ee_analyzer::ee
