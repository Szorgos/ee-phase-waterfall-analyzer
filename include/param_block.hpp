#pragma once
/**
 * @file param_block.hpp
 * @brief Zármentes paraméterblokk (seqlock) a GUI/híd szál és a valós idejű audio szál között.
 *
 * Kizárólag POD típusokat és címfüggetlen (lock-free) atomikat tartalmaz, ezért
 * folyamatok közötti közös memóriában (Windows: audiodg.exe <-> AetherAudio.exe)
 * is biztonságosan használható.
 */
#include <atomic>
#include <cstdint>
#include <cstring>

namespace ee_analyzer {

constexpr uint32_t kMaxEqBands = 32;

enum ParamFlags : uint32_t {
    kFlagEq        = 1u << 0,
    kFlagCrossfeed = 1u << 1,
    kFlagConvolver = 1u << 2,
    kFlagLimiter   = 1u << 3,
    kFlagBass      = 1u << 4,
    kFlagInitialized = 1u << 31, // a GUI legalább egyszer beállította
};

enum class EqBandType : int32_t {
    Peaking = 0,
    LowShelf = 1,
    HighShelf = 2,
    LowPass = 3,
    HighPass = 4,
};

struct EqBandPod {
    int32_t type = 0;
    float freq = 1000.0f;
    float gainDb = 0.0f;
    float q = 0.7071f;
};

/** A teljes DSP beállítás egyetlen másolható pillanatképe. */
struct ParamData {
    float preampDb = 0.0f;
    uint32_t flags = 0;          // ParamFlags
    int32_t crossfeedPreset = 0; // 0 = Bauer, 1 = Meier, 2 = Studio
    uint32_t numBands = 0;
    EqBandPod bands[kMaxEqBands];

    bool has(uint32_t f) const { return (flags & f) != 0; }
    void set(uint32_t f, bool on) { flags = on ? (flags | f) : (flags & ~f); }
};

static_assert(std::atomic<uint32_t>::is_always_lock_free, "lock-free atomics kellenek");

/**
 * Egyíró / többolvasó seqlock. Az író (GUI híd) sosem várakozik sokáig,
 * az olvasó (audio szál) sosem blokkol: ha éppen írás zajlik, a régi beállítással megy tovább.
 */
struct ParamBlock {
    std::atomic<uint32_t> seq{0};
    ParamData data;

    void write(const ParamData& d) {
        const uint32_t s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_relaxed); // páratlan: írás folyamatban
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(static_cast<void*>(&data), &d, sizeof(ParamData));
        std::atomic_thread_fence(std::memory_order_release);
        seq.store(s + 2, std::memory_order_release); // páros: kész
    }

    /** @return true, ha konzisztens pillanatképet sikerült olvasni */
    bool tryRead(ParamData& out, uint32_t& outSeq) const {
        const uint32_t s1 = seq.load(std::memory_order_acquire);
        if (s1 & 1u) return false;
        std::memcpy(&out, static_cast<const void*>(&data), sizeof(ParamData));
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t s2 = seq.load(std::memory_order_relaxed);
        if (s1 != s2) return false;
        outSeq = s1;
        return true;
    }

    uint32_t currentSeq() const { return seq.load(std::memory_order_acquire); }
};

/** GUI típusnév ("peaking", "lowshelf" ...) -> EqBandType */
inline int32_t eqTypeFromName(const char* name) {
    if (!name) return 0;
    if (std::strcmp(name, "lowshelf") == 0) return static_cast<int32_t>(EqBandType::LowShelf);
    if (std::strcmp(name, "highshelf") == 0) return static_cast<int32_t>(EqBandType::HighShelf);
    if (std::strcmp(name, "lowpass") == 0) return static_cast<int32_t>(EqBandType::LowPass);
    if (std::strcmp(name, "highpass") == 0) return static_cast<int32_t>(EqBandType::HighPass);
    return static_cast<int32_t>(EqBandType::Peaking);
}

inline const char* eqTypeName(int32_t t) {
    switch (static_cast<EqBandType>(t)) {
        case EqBandType::LowShelf: return "lowshelf";
        case EqBandType::HighShelf: return "highshelf";
        case EqBandType::LowPass: return "lowpass";
        case EqBandType::HighPass: return "highpass";
        default: return "peaking";
    }
}

} // namespace ee_analyzer
