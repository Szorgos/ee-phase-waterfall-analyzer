#pragma once
/**
 * @file pw_live_upsample_policy.hpp
 * @brief PipeWire saját motoros rendszerhang-felmintavételezés ráta-invariáns őrzője és biztonsági politikája.
 *
 * PipeWire-független, determinisztikus szabályok és állapotgép (#51, #52):
 *  - A szétválasztott (saját motoros) mód kizárólag érvényes, nem saját aqulify*, nem dummy/auto_null fizikai
 *    kimeneti céllal indulhat (visszacsatolási hurok kizárása).
 *  - A lejátszási sebesség- és hangmagasság-invariáns:
 *        (playbackRate / captureRate) * (bridgeSrc / bridgeDst) == 1.0
 *    csak akkor teljesül, ha captureRate == bridgeSrc és playbackRate == bridgeDst.
 *    Ha a PipeWire-gráf (vagy egy csak 48 kHz-et tudó hardver, pl. Intel sof-hda-dsp) miatt a captureRate,
 *    a playbackRate vagy a hardveres driver-órajel (driverClockRate) eltér a kért pártól, az őrző tisztán
 *    visszalép közvetlen módba és blokkolja az ismételt hibás próbálkozást (ping-pong védelem), amíg a
 *    kimeneti eszköz, a mintavételi mód vagy a kért rátapár nem változik.
 */
#include <cstdint>
#include <string>

#include "pw_reconnect_policy.hpp"

namespace ee_analyzer::pw_live_ups {

inline constexpr int kGuardCheckIntervalMs = 200;
inline constexpr int kSettleGraceMs = 1500;       // indulás utáni türelmi ablak a driver-órajel beállásáig
inline constexpr int kMismatchConfirmMs = 350;    // stream formátum-eltérés megerősítése (callback-sorrend ellen)

/** Valódi külső fizikai kimeneti eszköz-e (üres, saját aqulify* vagy dummy/auto_null elutasítva). */
inline bool hasValidPhysicalTarget(const std::string& target, const std::string& sinkName = "aqulify_sink") {
    return !target.empty() &&
           !pw_reconnect::isOwnAqulifyNode(target, sinkName) &&
           !pw_reconnect::isDummyOrNullSink(target);
}

/**
 * Valós idejű ráta-invariáns: a híd csak akkor dolgozhat, ha a capture stream tényleges rátája megegyezik
 * a híd bemeneti rátájával, és a playback stream tényleges rátája megegyezik a híd kimeneti rátájával.
 */
inline bool ratesMatchBridge(uint32_t captureRate, uint32_t playbackRate, uint32_t bridgeSrc, uint32_t bridgeDst) {
    return bridgeSrc > 0 && bridgeDst > bridgeSrc &&
           captureRate == bridgeSrc && playbackRate == bridgeDst;
}

/** Tényleges lejátszási sebesség- és hangmagasság-arány (ideálisan pontosan 1.0). */
inline double effectiveSpeedRatio(uint32_t captureRate, uint32_t playbackRate, uint32_t bridgeSrc, uint32_t bridgeDst) {
    if (captureRate == 0 || playbackRate == 0 || bridgeSrc == 0 || bridgeDst == 0) return 1.0;
    return (static_cast<double>(playbackRate) / static_cast<double>(captureRate)) *
           (static_cast<double>(bridgeSrc) / static_cast<double>(bridgeDst));
}

/** Ping-pong elleni kulcs: az őrző az adott (kimenet, forrás->cél, rateMode) négyesre lépett vissza. */
struct FallbackKey {
    std::string target;
    uint32_t srcRate = 0;
    uint32_t dstRate = 0;
    int rateMode = 0;

    bool valid() const { return srcRate > 0 && dstRate > 0; }
    bool operator==(const FallbackKey& o) const {
        return target == o.target && srcRate == o.srcRate && dstRate == o.dstRate && rateMode == o.rateMode;
    }
    bool operator!=(const FallbackKey& o) const { return !(*this == o); }
};

enum class GuardVerdict {
    Ok,                       // a capture, playback és hardver-órajel összhangban van a híddal
    WaitSettle,               // még a beállási vagy megerősítési ablakon belül vagyunk
    FallbackCaptureMismatch,  // a capture stream rátája eltér a híd srcRate-jétől (pl. 96 kHz 48 helyett -> 0,5x lenne)
    FallbackPlaybackMismatch, // a playback stream formátum-rátája eltér a híd dstRate-jétől
    FallbackDacRateMismatch   // a fizikai DAC / driver tényleges órajele nem váltott dstRate-re (pl. 48 kHz-es kártya)
};

/** A felület (gui_integrated_system.html) és a diagnosztika által értelmezett nyelvfüggetlen ok-kód. */
inline const char* verdictReasonCode(GuardVerdict v) {
    switch (v) {
        case GuardVerdict::FallbackCaptureMismatch:
        case GuardVerdict::FallbackPlaybackMismatch:
            return "rate_mismatch";
        case GuardVerdict::FallbackDacRateMismatch:
            return "dac_unsupported";
        default:
            return "";
    }
}

/** Magyar naplómagyarázat a visszalépés műszaki okáról. */
inline const char* verdictDetailHu(GuardVerdict v) {
    switch (v) {
        case GuardVerdict::FallbackCaptureMismatch:
            return "a bemeneti stream (aqulify_sink) nem maradt a forrás mintavételén";
        case GuardVerdict::FallbackPlaybackMismatch:
            return "a kimeneti stream nem a célmintavételen nyílt meg";
        case GuardVerdict::FallbackDacRateMismatch:
            return "a hangkártya tényleges órajele nem váltott a célmintavételre";
        default:
            return "";
    }
}

/**
 * Ráta-invariáns őrző állapotgép (PipeWire fő hurok szálán használható; időbélyeggel determinisztikusan tesztelhető).
 */
class RateInvariantGuard {
public:
    void resetSession(int64_t nowMs, uint32_t bridgeSrc, uint32_t bridgeDst) {
        active_ = (bridgeSrc > 0 && bridgeDst > bridgeSrc);
        bridgeSrc_ = bridgeSrc;
        bridgeDst_ = bridgeDst;
        startedMs_ = nowMs;
        mismatchSinceMs_ = 0;
        lastVerdict_ = GuardVerdict::Ok;
    }

    void stopSession() {
        active_ = false;
        bridgeSrc_ = 0;
        bridgeDst_ = 0;
        startedMs_ = 0;
        mismatchSinceMs_ = 0;
    }

    void blockKey(const FallbackKey& key, const std::string& reason, GuardVerdict verdict) {
        blockedKey_ = key;
        hasBlocked_ = key.valid();
        lastReason_ = reason;
        lastVerdict_ = verdict;
    }

    void setTransientReason(const std::string& reason) {
        lastReason_ = reason;
    }

    void clearBlocked() {
        hasBlocked_ = false;
        blockedKey_ = FallbackKey{};
        lastReason_.clear();
        lastVerdict_ = GuardVerdict::Ok;
    }

    /** Kimenetváltáskor (pl. laptop hangszóró -> USB DAC) törli az előző eszközre rögzített tiltást. */
    void onTargetChanged(const std::string& newTarget) {
        if (hasBlocked_ && blockedKey_.target != newTarget) clearBlocked();
    }

    /** Mintavételi mód váltáskor (pl. system -> follow) törli az előző módhoz tartozó tiltást. */
    void onRateModeChanged(int newRateMode) {
        if (hasBlocked_ && blockedKey_.rateMode != newRateMode) clearBlocked();
    }

    bool isBlocked(const FallbackKey& key) const {
        return hasBlocked_ && key.valid() && blockedKey_ == key;
    }

    const std::string& lastReason() const { return lastReason_; }
    GuardVerdict lastVerdict() const { return lastVerdict_; }
    bool active() const { return active_; }
    uint32_t bridgeSrc() const { return bridgeSrc_; }
    uint32_t bridgeDst() const { return bridgeDst_; }

    /**
     * Az aktív szétválasztott munkamenet értékelése.
     * - captureRate: a capture stream (aqulify_sink) SPA_PARAM_Format mintavétele (0: még ismeretlen)
     * - playbackRate: a playback stream (aqulify_sink_output) SPA_PARAM_Format mintavétele (0: még ismeretlen)
     * - driverClockRate: a playback stream pw_stream_get_time_n valódi driver-órajele (0: még nem érkezett)
     * - playbackStreaming: PW_STREAM_STATE_STREAMING állapotban van-e a playback stream
     */
    GuardVerdict evaluate(int64_t nowMs,
                          uint32_t captureRate,
                          uint32_t playbackRate,
                          uint32_t driverClockRate,
                          bool playbackStreaming) {
        if (!active_) return GuardVerdict::Ok;

        const bool streamRatesOk = (captureRate == bridgeSrc_) && (playbackRate == bridgeDst_);
        const bool dacClockOk = (driverClockRate == 0 || driverClockRate == bridgeDst_);

        if (streamRatesOk && dacClockOk) {
            mismatchSinceMs_ = 0;
            return GuardVerdict::Ok;
        }

        // Ha a kimeneti stream már fut és mindkét stream-formátum ismert, de eltér a hídétól (pl. capture 96 kHz-re
        // váltott 48 kHz helyett -> #52 0,5x lassulás), rövid megerősítés után azonnal visszalépünk.
        if (playbackStreaming && captureRate > 0 && playbackRate > 0 && !streamRatesOk) {
            if (mismatchSinceMs_ == 0) mismatchSinceMs_ = nowMs;
            if (nowMs - mismatchSinceMs_ >= kMismatchConfirmMs) {
                return (captureRate != bridgeSrc_)
                           ? GuardVerdict::FallbackCaptureMismatch
                           : GuardVerdict::FallbackPlaybackMismatch;
            }
            return GuardVerdict::WaitSettle;
        }

        // Induláskori beállási ablak (pl. a playback még csatlakozik, vagy a driver-órajel épp vált)
        if (nowMs - startedMs_ < kSettleGraceMs) {
            return GuardVerdict::WaitSettle;
        }

        if (captureRate != bridgeSrc_) return GuardVerdict::FallbackCaptureMismatch;
        if (playbackRate != bridgeDst_) return GuardVerdict::FallbackPlaybackMismatch;
        if (driverClockRate > 0 && driverClockRate != bridgeDst_) return GuardVerdict::FallbackDacRateMismatch;
        return GuardVerdict::Ok;
    }

private:
    bool active_ = false;
    uint32_t bridgeSrc_ = 0;
    uint32_t bridgeDst_ = 0;
    int64_t startedMs_ = 0;
    int64_t mismatchSinceMs_ = 0;
    bool hasBlocked_ = false;
    FallbackKey blockedKey_{};
    std::string lastReason_;
    GuardVerdict lastVerdict_ = GuardVerdict::Ok;
};

} // namespace ee_analyzer::pw_live_ups
