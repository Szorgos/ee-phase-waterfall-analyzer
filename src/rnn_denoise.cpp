/**
 * @file rnn_denoise.cpp
 * @brief RNNoise burkoló (include/rnn_denoise.hpp).
 */
#include "rnn_denoise.hpp"

#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#ifdef AQ_HAVE_RNNOISE
#include <rnnoise.h>
#endif

namespace ee_analyzer::magic {

#ifdef AQ_HAVE_RNNOISE

bool RnnDenoiser::available() { return rnnoise_get_frame_size() == static_cast<int>(kFrame); }

size_t RnnDenoiser::delay() {
    static size_t d = 0;
    static std::once_flag once;
    std::call_once(once, [] {
        // próbajel: szótagokra bontott, enyhén változó hangmagasságú zönge; a kimenet az 1–3 keretes
        // eltolások közül annál illeszkedik a bemenethez, amelyik az RNNoise késleltetése
        d = kFrame;
        if (!available()) return;
        RnnDenoiser r;
        constexpr size_t kFrames = 200, n = kFrame * kFrames;
        std::vector<float> x(n), y(n);
        for (size_t t = 0; t < n; ++t) {
            const double ts = static_cast<double>(t) / 48000.0;
            const double gate = std::sin(2 * 3.14159265358979 * 3.0 * ts) > 0 ? 0.3 : 0.0;
            x[t] = static_cast<float>(gate * std::sin(2 * 3.14159265358979 * 150.0 * ts + 2.0 * std::sin(2 * 3.14159265358979 * 0.7 * ts)));
        }
        for (size_t f = 0; f + kFrame <= n; f += kFrame) r.processFrame(&x[f], &y[f]);
        double best = -2;
        for (size_t lag = kFrame; lag <= 3 * kFrame; lag += kFrame) {
            double num = 0, a = 0, b = 0;
            for (size_t t = 48000; t < n; ++t) { num += y[t] * x[t - lag]; a += y[t] * y[t]; b += x[t - lag] * x[t - lag]; }
            const double c = num / std::sqrt(a * b + 1e-30);
            if (c > best) { best = c; d = lag; }
        }
    });
    return d;
}

RnnDenoiser::RnnDenoiser() {
    if (available()) st_ = rnnoise_create(nullptr);
}

RnnDenoiser::~RnnDenoiser() {
    if (st_) rnnoise_destroy(static_cast<DenoiseState*>(st_));
}

bool RnnDenoiser::processFrame(const float* in, float* out) {
    if (!st_) {
        std::memcpy(out, in, kFrame * sizeof(float));
        return false;
    }
    // Az RNNoise 16 bites skálán dolgozik
    for (size_t i = 0; i < kFrame; ++i) buf_[i] = in[i] * 32768.0f;
    rnnoise_process_frame(static_cast<DenoiseState*>(st_), buf_, buf_);
    for (size_t i = 0; i < kFrame; ++i) out[i] = buf_[i] * (1.0f / 32768.0f);
    return true;
}

void RnnDenoiser::reset() {
    if (st_) rnnoise_init(static_cast<DenoiseState*>(st_), nullptr);
}

#else

bool RnnDenoiser::available() { return false; }
size_t RnnDenoiser::delay() { return kFrame; }
RnnDenoiser::RnnDenoiser() = default;
RnnDenoiser::~RnnDenoiser() = default;
bool RnnDenoiser::processFrame(const float* in, float* out) {
    std::memcpy(out, in, kFrame * sizeof(float));
    return false;
}
void RnnDenoiser::reset() {}

#endif

} // namespace ee_analyzer::magic
