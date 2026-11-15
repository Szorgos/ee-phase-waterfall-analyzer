#pragma once
/**
 * @file exclusive_dsp.hpp
 * @brief A beépített lejátszó kizárólagos (bitpontos) kimenetének DSP-je: Windowson a WASAPI exclusive, Linuxon a
 *        közvetlen ALSA-kimenet hívja a saját szálán, mert ilyenkor a rendszer DSP-útja (Equalizer APO, illetve a
 *        PipeWire „Aqulify” eszköze) kimarad.
 */
#include <cstddef>
#include <cstdint>

namespace ee_analyzer::player {

/** A kizárólagos mód DSP-je (a motor adja): a kimenet szála hívja. */
class ExclusiveDsp {
public:
    virtual ~ExclusiveDsp() = default;
    /**
     * Új kizárólagos hangfolyam, a feldolgozás előtt: mintavétel, blokkméret (keret), és a kimenet bitmélysége a
     * ditherhez (16 vagy 24; 0: lebegőpontos vagy 32 bites kimenet, a beállított érték marad).
     */
    virtual void prepare(uint32_t rate, uint32_t blockFrames, int ditherBits) = 0;
    /** Helyben, sztereó (váltakozó L, R) keretek; a blokk mindig a prepare blockFrames értéke. */
    virtual void process(float* stereo, size_t frames) = 0;
    /** A DSP késleltetése mintában (a jelút kijelzéséhez; bármely szálról). */
    virtual size_t latencySamples() const { return 0; }
};

} // namespace ee_analyzer::player
