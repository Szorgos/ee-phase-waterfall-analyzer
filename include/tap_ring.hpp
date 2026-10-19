#pragma once
/**
 * @file tap_ring.hpp
 * @brief Felülíró (overwrite) sztereó "lehallgató" körpuffer az analizátorhoz.
 *
 * Az író (audio szál) SOHA nem vár: ha az olvasó lemarad, a legrégebbi adat felülíródik,
 * az olvasó pedig a legfrissebb adatokra ugrik. Fix méretű, POD + atomik, így folyamatok
 * közötti közös memóriában is használható.
 */
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <algorithm>

namespace ee_analyzer {

template <uint32_t kFramesPow2>
struct StereoTapRing {
    static_assert((kFramesPow2 & (kFramesPow2 - 1)) == 0, "2 hatványa kell");
    static constexpr uint32_t kFrames = kFramesPow2;
    static constexpr uint32_t kMask = kFramesPow2 - 1;

    std::atomic<uint64_t> writePos{0}; // összes valaha beírt frame száma
    float samples[kFramesPow2 * 2];     // interleaved L,R

    /** Valós idejű szálból: két külön csatornapuffer beírása. */
    void write(const float* left, const float* right, uint32_t n) {
        uint64_t w = writePos.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t idx = static_cast<uint32_t>((w + i) & kMask) * 2u;
            samples[idx] = left[i];
            samples[idx + 1] = right ? right[i] : left[i];
        }
        writePos.store(w + n, std::memory_order_release);
    }

    /** Interleaved bemenet beírása (pl. WASAPI után). */
    void writeInterleaved(const float* lr, uint32_t n) {
        uint64_t w = writePos.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t idx = static_cast<uint32_t>((w + i) & kMask) * 2u;
            samples[idx] = lr[2 * i];
            samples[idx + 1] = lr[2 * i + 1];
        }
        writePos.store(w + n, std::memory_order_release);
    }
};

/**
 * Olvasó állapot (az olvasó oldalon él, nem a közös memóriában).
 * @return a kiolvasott frame-ek száma (interleaved L,R kerül a dest-be)
 */
template <class Ring>
size_t readTapRing(const Ring& ring, uint64_t& readPos, float* dest, size_t maxFrames) {
    const uint64_t w = ring.writePos.load(std::memory_order_acquire);
    if (w < readPos) readPos = w; // az író újraindult
    uint64_t avail = w - readPos;
    if (avail > Ring::kFrames / 2) { // túl nagy lemaradás: ugrás a friss adatokhoz
        readPos = w - Ring::kFrames / 4;
        avail = Ring::kFrames / 4;
    }
    const size_t n = static_cast<size_t>(std::min<uint64_t>(avail, maxFrames));
    for (size_t i = 0; i < n; ++i) {
        const uint32_t idx = static_cast<uint32_t>((readPos + i) & Ring::kMask) * 2u;
        dest[2 * i] = ring.samples[idx];
        dest[2 * i + 1] = ring.samples[idx + 1];
    }
    // Ha közben az író túlszaladt rajtunk, az adat sérülhetett: eldobjuk.
    const uint64_t w2 = ring.writePos.load(std::memory_order_acquire);
    if (w2 - readPos > Ring::kFrames) {
        readPos = w2;
        return 0;
    }
    readPos += n;
    return n;
}

} // namespace ee_analyzer
