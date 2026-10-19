#pragma once
/**
 * @file shared_state.hpp
 * @brief Folyamatok közötti közös memória elrendezése:
 *        AetherAudio plugin (Equalizer APO alatt az audiodg.exe-ben) <-> AetherAudio.exe motor.
 */
#include <atomic>
#include <cstdint>
#include "param_block.hpp"
#include "tap_ring.hpp"

namespace ee_analyzer {

constexpr uint32_t kSharedMagic = 0x41455448;   // 'AETH'
constexpr uint32_t kSharedVersion = 1;

// A plugin (szolgáltatásként futó audiodg) a Global\ névtérben hozza létre;
// ha erre nincs joga (pl. DAW-ban vagy az EQ APO szerkesztőben), a Local\ névtérbe esik vissza.
constexpr const wchar_t* kSharedNameGlobal = L"Global\\AetherAudioShared_v1";
constexpr const wchar_t* kSharedNameLocal = L"Local\\AetherAudioShared_v1";

using SharedTapRing = StereoTapRing<65536>; // ~1,4 s 48 kHz-en

struct AetherSharedState {
    uint32_t magic;
    uint32_t version;

    // --- plugin -> motor ---
    std::atomic<uint32_t> sampleRate;
    std::atomic<uint32_t> ownerToken;     // melyik plugin-példány írja a tap-et
    std::atomic<uint64_t> ownerTickMs;    // GetTickCount64 az utolsó íráskor
    std::atomic<uint32_t> activeInstances;
    std::atomic<float> gainReductionDb;
    SharedTapRing tap;

    // --- motor -> plugin ---
    ParamBlock params;
};

static_assert(std::atomic<uint64_t>::is_always_lock_free, "lock-free 64 bites atomik kell");
static_assert(std::atomic<float>::is_always_lock_free, "lock-free float atomik kell");

} // namespace ee_analyzer
