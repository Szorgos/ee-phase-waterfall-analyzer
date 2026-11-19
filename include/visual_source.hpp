#pragma once
/**
 * @file visual_source.hpp
 * @brief Honnan kapja a híd (vízesés, spektrum, mérők) a hangot a Linux / Pi motorban.
 *
 * Három eset:
 * - közvetlen DSD-kimenet fut (DoP vagy natív DSD a kizárólagos ALSA-kimeneten): vizualizálható PCM nincs, a DSP-lánc
 *   és a mérők kimaradnak;
 * - a beépített lejátszó kizárólagos (közvetlen ALSA) PCM-kimenete: a lejátszó saját DSP-láncának lehallgatópontja;
 * - egyébként a motor (PipeWire „Aqulify” eszköz) vizualizációs lehallgatópontja.
 */
#include <cstddef>

namespace ee_analyzer {

enum class VisualSource { DsdDirect, PlayerExclusive, Engine };

inline VisualSource visualSourceFor(bool dsdDirectActive, bool playerExclusive) {
    if (dsdDirectActive) return VisualSource::DsdDirect;
    if (playerExclusive) return VisualSource::PlayerExclusive;
    return VisualSource::Engine;
}

/**
 * A híd `pullAudio` visszahívásának törzse. `readExclusive` és `readEngine`: (float* lr, size_t maxFrames) -> size_t.
 *
 * Közvetlen DSD-kimenetnél **0 keretet** ad (nincs mit mutatni, mint szünetben). 2.2.0–2.6.1 között itt minden
 * hívásra `maxFrames` keret csend ment, ezért a híd hanglehúzó ciklusa soha nem ért véget: a felület parancsai
 * elakadtak, és a motor nem állt le (hibajelentés 2026-11-19, Fedora, FiiO BTR7).
 */
template <class ReadExclusive, class ReadEngine>
size_t pullVisualAudio(bool dsdDirectActive, bool playerExclusive, float* lr, size_t maxFrames, ReadExclusive&& readExclusive,
                       ReadEngine&& readEngine) {
    switch (visualSourceFor(dsdDirectActive, playerExclusive)) {
        case VisualSource::DsdDirect:
            return 0;
        case VisualSource::PlayerExclusive:
            return readExclusive(lr, maxFrames);
        case VisualSource::Engine:
        default:
            return readEngine(lr, maxFrames);
    }
}

}  // namespace ee_analyzer
