#pragma once
/**
 * @file shared_state.hpp
 * @brief Folyamatok közötti közös memória elrendezése:
 *        Aqulify plugin (Equalizer APO alatt az audiodg.exe-ben) <-> Aqulify.exe motor.
 */
#include <atomic>
#include <cstdint>
#include "magic_touch.hpp"
#include "param_block.hpp"
#include "tap_ring.hpp"

namespace ee_analyzer {

constexpr uint32_t kSharedMagic = 0x41455448;   // 'AETH'
constexpr uint32_t kSharedVersion = 9;

// A plugin (szolgáltatásként futó audiodg) a Global\ névtérben hozza létre;
// ha erre nincs joga (pl. DAW-ban vagy az EQ APO szerkesztőben), a Local\ névtérbe esik vissza.
constexpr const wchar_t* kSharedNameGlobal = L"Global\\AqulifyShared_v9";
constexpr const wchar_t* kSharedNameLocal = L"Local\\AqulifyShared_v9";

using SharedTapRing = StereoTapRing<65536>; // ~1,4 s 48 kHz-en

constexpr uint32_t kMaxIrFrames = 262144;   // ~5,5 s 48 kHz-en
constexpr uint32_t kIrNameLen = 128;

/** Konvolver impulzusválasz (motor -> plugin). Seqlock: páratlan seq = írás folyamatban. */
struct IrBlock {
    std::atomic<uint32_t> seq;
    uint32_t sampleRate;
    uint32_t channels;  // 1 vagy 2
    uint32_t frames;    // 0 = nincs IR
    char name[kIrNameLen];
    float data[kMaxIrFrames * 2]; // interleaved, ha channels == 2
};

struct AetherSharedState {
    uint32_t magic;
    uint32_t version;

    // --- plugin -> motor ---
    std::atomic<uint32_t> sampleRate;
    std::atomic<uint32_t> ownerToken;     // melyik plugin-példány írja a tap-et
    std::atomic<uint64_t> ownerTickMs;    // GetTickCount64 az utolsó íráskor
    std::atomic<uint32_t> activeInstances;
    std::atomic<float> gainReductionDb;
    std::atomic<float> bassHarmonicsDb;   // a Bass Enhancer felharmonikus-szintje (a mérőhöz)
    magic::DiagBlock magicDiag;           // a Magic Touch mérései (a lehallgatást író példányé)
    SharedTapRing tap;

    // --- motor -> plugin ---
    ParamBlock params;
    IrBlock ir;
};

static_assert(std::atomic<uint64_t>::is_always_lock_free, "lock-free 64 bites atomik kell");
static_assert(std::atomic<float>::is_always_lock_free, "lock-free float atomik kell");

} // namespace ee_analyzer
