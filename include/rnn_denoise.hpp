#pragma once
/**
 * @file rnn_denoise.hpp
 * @brief RNNoise (Xiph, BSD-3) egy csatornára: a Magic Touch beszéd zajcsökkentése (docs/plans/magic-touch.md, 5. ütem).
 *
 * 48 kHz, 480 mintás keretek. A kimenet késleltetése változatfüggő (0.1: 480, 0.2: 960 minta, mert az
 * egy keretnyit előre néz): delay() egyszer, próbajellel méri meg.
 * Ha a fordításkor nem volt RNNoise modell (third_party/CMakeLists.txt), available() hamis,
 * és a Magic Touch a saját, sávonkénti zajcsökkentését használja.
 */
#include <cstddef>

namespace ee_analyzer::magic {

class RnnDenoiser {
public:
    static constexpr size_t kFrame = 480;

    /** Beépült-e az RNNoise a fordításkor. */
    static bool available();
    /** Az RNNoise saját késleltetése (minta, a kFrame többszöröse); az első hívás mér (foglal, nem valós idejű). */
    static size_t delay();

    RnnDenoiser();     // foglal: a nem valós idejű szálon kell létrehozni
    ~RnnDenoiser();
    RnnDenoiser(const RnnDenoiser&) = delete;
    RnnDenoiser& operator=(const RnnDenoiser&) = delete;

    /** Egy keret (±1 skála, kFrame minta). false: nincs RNNoise (a kimenet ilyenkor a bemenet). */
    bool processFrame(const float* in, float* out);
    /** Az állapot törlése (foglalás nélkül). */
    void reset();

private:
    void* st_ = nullptr;
    float buf_[kFrame] = {};
};

} // namespace ee_analyzer::magic
