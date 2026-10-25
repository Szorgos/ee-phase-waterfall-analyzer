/**
 * @file ee_dsp_convolver.cpp
 * @brief Convolver: zita-convolver (FFTW3) az EasyEffects kernel-előkészítésével és blokkillesztésével.
 *
 * EasyEffects megfelelők:
 *   - ConvolverKernelManager::resampleKernel  -> SpeexDSP, SPEEX_RESAMPLER_QUALITY_DESKTOP, csatornánként egy hívás
 *   - ConvolverZita::set_kernel_stereo_width / apply_kernel_autogain / normalizeKernel
 *   - ConvolverZita::init: Convproc::configure(2, 2, méret, blokk, blokk, MAXPART, 0.5), impdata_create(0,0) és (1,1),
 *     start_process(0, SCHED_FIFO), feldolgozás process() hívással (szinkron mód)
 *   - Convolver::setup / process: blokkméret = a hoszt blokkja, ha kettő hatványa; különben az alatta lévő
 *     kettőhatvány (legalább 64), FIFO-val
 * A zita-convolver forrása az Ardour projekt hordozható (Windows-on is futó) változata (third_party/zita-convolver).
 */
#include "ee_dsp.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

#include <pthread.h>
#include <sched.h>

#include <speex/speex_resampler.h>
#include "zita-convolver/zita-convolver.h"

namespace ee_analyzer::ee {

namespace {

/** Az FFTW tervező nem szálbiztos: tervek létrehozása / törlése csak e zár alatt (EasyEffects: util::fftw_lock). */
std::mutex& fftwLock() {
    static std::mutex m;
    return m;
}

std::vector<float> resampleChannel(const std::vector<float>& in, uint32_t inRate, uint32_t outRate) {
    // EasyEffects Resampler: egyetlen speex_resampler_process_float hívás, a kimenet ceil(n * arány) hosszú
    int err = 0;
    SpeexResamplerState* st = speex_resampler_init(1, inRate, outRate, SPEEX_RESAMPLER_QUALITY_DESKTOP, &err);
    if (!st || err != RESAMPLER_ERR_SUCCESS) {
        if (st) speex_resampler_destroy(st);
        return in;
    }
    const double ratio = static_cast<double>(outRate) / static_cast<double>(inRate);
    const size_t expected = static_cast<size_t>(std::ceil(in.size() * ratio));
    std::vector<float> out(expected);
    spx_uint32_t inLen = static_cast<spx_uint32_t>(in.size());
    spx_uint32_t outLen = static_cast<spx_uint32_t>(expected);
    speex_resampler_process_float(st, 0, in.data(), &inLen, out.data(), &outLen);
    out.resize(outLen);
    speex_resampler_destroy(st);
    return out;
}

/** Ellenőrzi, hogy SCHED_FIFO szál indítható-e (különben a zita szálai nem indulnának el, és a start_process várna). */
int convolverSchedPolicy() {
#if defined(_WIN32)
    return SCHED_OTHER;
#else
    static const int policy = []() {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        sched_param parm{};
        parm.sched_priority = sched_get_priority_min(SCHED_FIFO);
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        pthread_attr_setschedparam(&attr, &parm);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_t t;
        const int rc = pthread_create(&t, &attr, [](void*) -> void* { return nullptr; }, nullptr);
        pthread_attr_destroy(&attr);
        if (rc == 0) {
            pthread_join(t, nullptr);
            return SCHED_FIFO;
        }
        return SCHED_OTHER;
    }();
    return policy;
#endif
}

/** Rögzített kapacitású FIFO (valós idejű szálban nem foglal). */
struct Fifo {
    std::vector<float> buf;
    size_t head = 0, size = 0;
    void init(size_t cap) { buf.assign(cap, 0.0f); head = size = 0; }
    void clear() { head = size = 0; }
    void push(const float* p, size_t n) {
        for (size_t i = 0; i < n && size < buf.size(); ++i) { buf[(head + size) % buf.size()] = p[i]; ++size; }
    }
    void pop(float* p, size_t n) {
        for (size_t i = 0; i < n && size > 0; ++i) { p[i] = buf[head]; head = (head + 1) % buf.size(); --size; }
    }
};

} // namespace

ConvKernel prepareConvKernel(const std::vector<float>& left, const std::vector<float>& right, uint32_t irRate,
                             uint32_t targetRate, int irWidth, bool autogain) {
    ConvKernel k;
    if (left.empty() || irRate == 0) return k;
    // Monó IR: mindkét csatornára ugyanaz (EasyEffects: channels == 1 -> 2)
    k.rate = irRate;
    k.left = left;
    k.right = right.empty() ? left : right;
    if (k.right.size() != k.left.size()) k.right.resize(k.left.size(), 0.0f);

    // resampleKernel
    if (targetRate != 0 && k.rate != targetRate) {
        k.left = resampleChannel(k.left, k.rate, targetRate);
        k.right = resampleChannel(k.right, k.rate, targetRate);
        k.rate = targetRate;
        const size_t n = std::min(k.left.size(), k.right.size());
        k.left.resize(n);
        k.right.resize(n);
    }
    if (!k.valid()) return k;

    // set_kernel_stereo_width (Mid-Side; https://github.com/tomszilagyi/ir.lv2)
    {
        // osztás (és nem · 0,01F): 100 %-nál pontosan 1, így FMA-s gépen (pl. aarch64) is pontosan érintetlen
        const float w = static_cast<float>(irWidth) / 100.0F;
        const float x = (1.0F - w) / (1.0F + w);
        for (size_t i = 0; i < k.left.size(); i++) {
            const float LL = k.left[i];
            const float RR = k.right[i];
            k.left[i] = LL + (x * RR);
            k.right[i] = RR + (x * LL);
        }
    }

    if (autogain) {
        // normalizeKernel
        float peak = 0.0f;
        for (float v : k.left) peak = std::max(peak, std::fabs(v));
        for (float v : k.right) peak = std::max(peak, std::fabs(v));
        if (peak > 1e-6) {
            for (float& v : k.left) v /= peak;
            for (float& v : k.right) v /= peak;
        }
        // apply_kernel_autogain
        float power_LL = 0.0F, power_RR = 0.0F;
        for (size_t i = 0; i < k.left.size(); i++) {
            power_LL += k.left[i] * k.left[i];
            power_RR += k.right[i] * k.right[i];
        }
        const float power = std::max(power_LL, power_RR);
        const float gain = std::min(1.0F, 1.0F / std::sqrt(power));
        for (size_t i = 0; i < k.left.size(); i++) {
            k.left[i] *= gain;
            k.right[i] *= gain;
        }
    }
    return k;
}

struct ZitaInstance::Impl {
    ArdourZita::Convproc* conv = nullptr;
    uint32_t blocksize = 0;
    bool direct = true;          // n_samples_is_power_of_2 (és a blokk egyezik)
    Fifo inL, inR, outL, outR;
    std::vector<float> dataL, dataR;

    ~Impl() { destroy(); }

    void destroy() {
        std::lock_guard<std::mutex> lk(fftwLock());
        if (conv) {
            conv->stop_process();
            while (!conv->check_stop()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            conv->cleanup();
            delete conv;
            conv = nullptr;
        }
    }

    bool zitaProcess(float* L, float* R) {
        if (!conv || conv->state() != ArdourZita::Convproc::ST_PROC) return false;
        std::memcpy(conv->inpdata(0), L, blocksize * sizeof(float));
        std::memcpy(conv->inpdata(1), R, blocksize * sizeof(float));
        if (conv->process() != 0 && conv->state() != ArdourZita::Convproc::ST_PROC) return false;
        std::memcpy(L, conv->outdata(0), blocksize * sizeof(float));
        std::memcpy(R, conv->outdata(1), blocksize * sizeof(float));
        return true;
    }
};

ZitaInstance::ZitaInstance() : p_(std::make_unique<Impl>()) {}
ZitaInstance::~ZitaInstance() = default;

bool ZitaInstance::init(const ConvKernel& k, uint32_t quantum) {
    if (!k.valid() || quantum == 0) return false;
    quantum_ = quantum;

    // Convolver::setup
    uint32_t blocksize = quantum;
    const bool pow2 = (quantum & (quantum - 1U)) == 0U;
    if (!pow2) {
        while ((blocksize & (blocksize - 1)) != 0 && blocksize > 2) blocksize--;
    }
    blocksize = std::max<uint32_t>(blocksize, 64); // zita does not work with less than 64
    p_->blocksize = blocksize;
    p_->direct = pow2 && blocksize == quantum;
    const size_t cap = 2 * (static_cast<size_t>(quantum) + blocksize) + 16;
    p_->inL.init(cap); p_->inR.init(cap); p_->outL.init(cap); p_->outR.init(cap);
    p_->dataL.assign(blocksize, 0.0f);
    p_->dataR.assign(blocksize, 0.0f);

    // ConvolverZita::init
    std::lock_guard<std::mutex> lk(fftwLock());
    auto* conv = new ArdourZita::Convproc();
    conv->set_options(0);
    const float density = 0.5F;
    const uint32_t size = static_cast<uint32_t>(k.left.size());
    if (conv->configure(2, 2, size, blocksize, blocksize, ArdourZita::Convproc::MAXPART, density) != 0 ||
        conv->impdata_create(0, 0, 1, const_cast<float*>(k.left.data()), 0, static_cast<int>(size)) != 0 ||
        conv->impdata_create(1, 1, 1, const_cast<float*>(k.right.data()), 0, static_cast<int>(size)) != 0) {
        conv->cleanup();
        delete conv;
        return false;
    }
    if (conv->start_process(0, convolverSchedPolicy()) != 0) {
        conv->cleanup();
        delete conv;
        return false;
    }
    p_->conv = conv;
    return true;
}

size_t ZitaInstance::latencyFor(uint32_t quantum) {
    // A init() blokkméret-illesztésével egyezik: a FIFO a kettőhatvány blokkokat dolgozza fel, a maradék késlelteti
    if (quantum == 0 || (quantum & (quantum - 1U)) == 0U) return 0;
    uint32_t blocksize = quantum;
    while ((blocksize & (blocksize - 1)) != 0 && blocksize > 2) blocksize--;
    blocksize = std::max<uint32_t>(blocksize, 64);
    return quantum % blocksize;
}

bool ZitaInstance::process(float* L, float* R, size_t n) {
    if (failed_.load(std::memory_order_relaxed) || n != quantum_) return false;
    Impl& p = *p_;
    if (p.direct) {
        if (!p.zitaProcess(L, R)) { failed_.store(true); return false; }
        return true;
    }
    // Nem kettőhatvány blokkméret: FIFO (Convolver::process)
    p.inL.push(L, n);
    p.inR.push(R, n);
    while (p.inL.size >= p.blocksize) {
        p.inL.pop(p.dataL.data(), p.blocksize);
        p.inR.pop(p.dataR.data(), p.blocksize);
        if (!p.zitaProcess(p.dataL.data(), p.dataR.data())) { failed_.store(true); return false; }
        p.outL.push(p.dataL.data(), p.blocksize);
        p.outR.push(p.dataR.data(), p.blocksize);
    }
    if (p.outL.size >= n) {
        p.outL.pop(L, n);
        p.outR.pop(R, n);
    } else {
        const size_t offset = n - p.outL.size;
        std::fill_n(L, offset, 0.0F);
        std::fill_n(R, offset, 0.0F);
        const size_t have = p.outL.size;
        p.outL.pop(L + offset, have);
        p.outR.pop(R + offset, have);
    }
    return true;
}

void ZitaInstance::reset() {
    p_->inL.clear(); p_->inR.clear(); p_->outL.clear(); p_->outR.clear();
}

} // namespace ee_analyzer::ee
