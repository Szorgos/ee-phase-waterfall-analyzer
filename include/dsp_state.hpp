#pragma once
/**
 * @file dsp_state.hpp
 * @brief Miért nem fut a hangfeldolgozás (Windows: az Equalizer APO plugin)? Szünet, hiba, másik eszköz, vagy nincs eszköz.
 *
 * Az Equalizer APO néhány perc csend után leállítja a hangfolyamot, és vele bezárja a plugint is: ilyenkor a motor
 * ugyanazt látja („nincs élő plugin”), mint egy valódi hibánál. A különbséget a lejátszóeszköz és a hallható jel
 * adja: szünetben nincs hang, hibánál szól a zene, de a plugin nem indul el. Platformfüggetlen, tesztelhető logika.
 */
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace ee_analyzer {

/** A hangfeldolgozás állapota a felület számára. */
enum class DspState {
    Active,       ///< a plugin fut
    Idle,         ///< szünet: nincs lejátszás, a plugin ezért állt le (nem hiba)
    NotLoaded,    ///< szól a zene, de a plugin nem fut (pl. frissítés után a régi maradt betöltve)
    OtherDevice,  ///< a hang másik eszközön szól, mint ahol a plugin eddig futott (pl. új USB hangkártya)
    NoDevice,     ///< nincs elérhető lejátszóeszköz (pl. kihúzott DAC)
};

inline const char* dspStateName(DspState s) {
    switch (s) {
        case DspState::Active: return "active";
        case DspState::Idle: return "idle";
        case DspState::NotLoaded: return "notLoaded";
        case DspState::OtherDevice: return "otherDevice";
        case DspState::NoDevice: return "noDevice";
    }
    return "idle";
}

struct DspStateInput {
    bool pluginAlive = false;     ///< van élő plugin-példány
    bool deviceOk = true;         ///< a lejátszóeszköz elérhető (a lehallgatás fut)
    std::string device;           ///< a mostani lejátszóeszköz neve
    std::string apoDevice;        ///< az eszköz, amelyen a plugin legutóbb futott (üres: még sosem)
    double playingMs = 0.0;       ///< mióta szól folyamatosan hallható jel (PlaybackDetector)
};

/** Ennyi ideig szóló zene után számít hibának, ha a plugin nem indult el (indításkor a plugin ennél jóval gyorsabb). */
constexpr double kDspNotLoadedGraceMs = 3000.0;

inline DspState classifyDspState(const DspStateInput& in) {
    if (in.pluginAlive) return DspState::Active;
    if (!in.deviceOk) return DspState::NoDevice;
    if (!in.apoDevice.empty() && in.device != in.apoDevice) return DspState::OtherDevice;
    if (in.playingMs >= kDspNotLoadedGraceMs) return DspState::NotLoaded;
    return DspState::Idle;
}

/**
 * Szól-e hallható jel, és mióta folyamatosan. A számok közti rövid csend (legfeljebb kGapMs) nem szakítja meg a
 * lejátszást; a küszöb alatti jel (zajpadló, digitális csend) nem számít lejátszásnak.
 */
class PlaybackDetector {
public:
    static constexpr float kThreshold = 3.2e-4f;   // kb. −70 dBFS csúcs
    static constexpr double kGapMs = 1500.0;

    /** Interleaved sztereó minták a lejátszóeszköz kimenetéről. */
    void push(const float* lr, size_t frames, double sampleRate) {
        if (frames == 0 || !(sampleRate > 0.0)) return;
        float peak = 0.0f;
        for (size_t i = 0; i < frames * 2; ++i) peak = std::max(peak, std::fabs(lr[i]));
        advance(static_cast<double>(frames) * 1000.0 / sampleRate, peak >= kThreshold);
    }

    /** Idő telik adat nélkül (pl. a lehallgatás nem ad mintát): csendnek számít. */
    void idle(double ms) { advance(ms, false); }

    void reset() { playingMs_ = 0.0; silentMs_ = kGapMs; }

    double playingMs() const { return playingMs_; }

private:
    void advance(double ms, bool loud) {
        if (loud) {
            silentMs_ = 0.0;
            playingMs_ += ms;
        } else {
            silentMs_ += ms;
            if (silentMs_ > kGapMs) playingMs_ = 0.0;
            else if (playingMs_ > 0.0) playingMs_ += ms;   // rövid szünet a lejátszáson belül
        }
    }

    double playingMs_ = 0.0;
    double silentMs_ = kGapMs;
};

} // namespace ee_analyzer
