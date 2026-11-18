#pragma once
/**
 * @file load_advisor.hpp
 * @brief Terhelés-tanácsadó: ha a beállítás tartósan nagy processzorterhelést vagy hangkimaradást okoz, javaslatot ad.
 *
 * A tanácsadó soha nem módosít magától: a felület felajánlja a javasolt lépést, a felhasználó dönt, és minden lépés
 * egy kattintással visszavonható (a felület a lépés előtti beállítást megőrzi).
 *
 * A lépések rögzített sorrendje (az első alkalmazható jön):
 *   1. két szálas számítás (legalább 4 logikai mag esetén; a hang bitre azonos),
 *   2. a felmintavételezés pontossága 28 / 32 → 24 bit,
 *   3. a célmintavétel felső korlátja (768 / 705,6 kHz → 384 kHz; 384 / 352,8 kHz → 192 kHz),
 *   4. a felmintavételezés kikapcsolása.
 * Ha a felmintavételezés ki van kapcsolva, a tanácsadó csak figyelmeztet (a terhelés a DSP-láncból jön).
 */
#include <cstdint>

#include "audio_options.hpp"

namespace ee_analyzer::load {

struct Snapshot {
    double cpuPct = 0.0;          // a motor terhelése a gép teljes kapacitásához (%)
    double sustainedSec = 0.0;    // mióta van a terhelés folyamatosan a küszöb fölött (s)
    uint64_t newUnderruns = 0;    // hangkimaradások a legutóbbi értékelés óta
};

struct Config {
    double thresholdPct = 30.0;   // Raspberry Pi-n 20
    double sustainSec = 20.0;
};

enum class Step { None, TwoThreads, Precision24, LowerTarget, UpsampleOff };

struct Advice {
    bool warn = false;
    Step step = Step::None;
};

inline const char* stepName(Step s) {
    switch (s) {
    case Step::TwoThreads: return "two_threads";
    case Step::Precision24: return "precision24";
    case Step::LowerTarget: return "lower_target";
    case Step::UpsampleOff: return "upsample_off";
    default: return "none";
    }
}

/**
 * currentTarget: a felmintavételezés jelenlegi célmintavétele (ha nincs átalakítás, a forrásé).
 */
inline Advice advise(const Snapshot& s, const Config& c, const audioopt::AudioOptions& o, unsigned cores,
                     uint32_t currentTarget) {
    Advice a;
    a.warn = (s.cpuPct > c.thresholdPct && s.sustainedSec >= c.sustainSec) || s.newUnderruns > 0;
    if (!a.warn || o.upsampleRule == audioopt::UpsampleRule::Off) return a;
    if (cores >= 4 && o.upsampleThreads != 2) a.step = Step::TwoThreads;
    else if (o.upsamplePrecision > 24) a.step = Step::Precision24;
    else if (currentTarget > 192000) a.step = Step::LowerTarget;
    else a.step = Step::UpsampleOff;
    return a;
}

inline audioopt::AudioOptions applyStep(audioopt::AudioOptions o, Step s, uint32_t currentTarget) {
    switch (s) {
    case Step::TwoThreads: o.upsampleThreads = 2; break;
    case Step::Precision24: o.upsamplePrecision = 24; break;
    case Step::LowerTarget:
        if (currentTarget > 384000) o.upsampleMaxRate = 384000;
        else if (currentTarget > 192000) o.upsampleMaxRate = 192000;
        break;
    case Step::UpsampleOff: o.upsampleRule = audioopt::UpsampleRule::Off; break;
    default: break;
    }
    return o;
}

} // namespace ee_analyzer::load
