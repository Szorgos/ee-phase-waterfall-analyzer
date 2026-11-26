#pragma once
/**
 * @file pw_keepalive_policy.hpp
 * @brief PipeWire hangkártya ébren tartásának (keepAlive) szüneteltetési és naplózási politikája (#46, #53).
 *
 * PipeWire-független, determinisztikus szabályok:
 *  - A felhasználói szándék (userWanted) elválik a belső szüneteltetéstől (lejátszó, aktív rendszerhang-
 *    felmintavételezés, forrás-követés alatti külső hangfolyam). A „Hangkártya ébren tartása: be/ki” naplósor
 *    kizárólag a felhasználói beállítás tényleges változásakor jelenik meg.
 *  - Az aktív külső forrás állapotát NEM növelő/csökkentő számláló adja, hanem minden döntéskor a PipeWire
 *    regiszter aktuális Stream/Output/Audio csomópontjaiból számoljuk újra, így nem ragadhat be.
 *  - Biztonsági felső határ (kIdleSuppressCapMs): ha egy külső stream a regiszterben marad (pl. szüneteltetett
 *    böngészőfül), de tényleges hang nincs, legkésőbb néhány másodperc múlva a keepAlive visszakapcsol.
 */
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

#include "pw_reconnect_policy.hpp"

namespace ee_analyzer::pw_keepalive {

inline constexpr int64_t kExternalPauseGraceMs = 1500;  // új / megszűnő külső stream melletti rövid átmeneti ablak
inline constexpr int64_t kIdleSuppressCapMs = 4000;     // biztonsági felső határ: hang nélküli szünet legkésőbb ennyi után feloldódik

/** A PipeWire regiszterben jelenleg élő Stream/Output/Audio csomópont leírása. */
struct RegistryStreamNode {
    uint32_t id = 0;
    std::string nodeName;
    std::string appName;
};

/** Külső (nem saját Aqulify) Stream/Output/Audio csomópont-e. */
inline bool isExternalAudioOutputStream(const std::string& nodeName, const std::string& appName) {
    if (pw_reconnect::isOwnAqulifyNode(nodeName)) return false;
    if (appName == "Aqulify") return false;
    return true;
}

/**
 * Minden döntéskor a regiszter aktuális Stream/Output/Audio csomópontjaiból számolja újra,
 * hogy van-e külső hangfolyam (nincs növelő/csökkentő számláló -> nem ragadhat be).
 */
inline bool hasExternalAudioOutputStream(const std::unordered_map<uint32_t, RegistryStreamNode>& streams) {
    for (const auto& [id, node] : streams) {
        (void)id;
        if (isExternalAudioOutputStream(node.nodeName, node.appName)) return true;
    }
    return false;
}

struct KeepAliveInputs {
    int64_t nowMs = 0;
    bool userWanted = false;           // a felhasználó keepAwake beállítása
    bool rateFollow = false;           // rateMode == Follow
    bool playerSuppressed = false;     // a beépített lejátszó nyitva van (vagy zárás utáni 1,5 s ablakban)
    bool liveUpsampleActive = false;   // saját motoros szétválasztott felmintavételezés fut (a playback tartja ébren a DAC-ot)
    bool hasExternalStream = false;    // frissen újraszámolva a regiszter Stream/Output/Audio csomópontjaiból
    bool externalAudioActive = false;  // ténylegesen fut-e hang a láncban a keepAlive nélkül
    bool liveRateForced = false;       // PipeWire-átalakítós élő felmintavételezés (clock.force-rate aktív)
};

struct KeepAliveDecision {
    bool shouldRun = false;      // futnia kell-e a csendes aqulify_sink_keepalive streamnek
    bool logUserChange = false;  // csak felhasználói be/ki váltáskor true
    bool userEnabled = false;    // a naplózandó felhasználói állapot (be / ki)
};

class KeepAlivePolicy {
public:
    KeepAliveDecision evaluate(const KeepAliveInputs& in) {
        KeepAliveDecision out{};
        if (in.userWanted != lastUserWanted_) {
            lastUserWanted_ = in.userWanted;
            out.logUserChange = true;
            out.userEnabled = in.userWanted;
        }
        if (!in.userWanted) {
            extHoldUntilMs_ = 0;
            idleExtSinceMs_ = 0;
            wasExternal_ = false;
            wasActiveExternal_ = false;
            out.shouldRun = false;
            return out;
        }

        // Aktív saját motoros felmintavételezés, beépített lejátszó vagy PipeWire-átalakítós élő kényszer alatt
        // a hangkártya úgyis szól: csendesen szünetel a keepAlive (nincs felesleges nullakeverés / 48<->96 rángatás)
        if (in.liveUpsampleActive || in.playerSuppressed || (in.rateFollow && in.liveRateForced)) {
            idleExtSinceMs_ = 0;
            wasExternal_ = in.hasExternalStream;
            wasActiveExternal_ = in.hasExternalStream;
            out.shouldRun = false;
            return out;
        }

        if (in.rateFollow) {
            if (in.hasExternalStream) {
                if (!wasExternal_) {
                    extHoldUntilMs_ = std::max(extHoldUntilMs_, in.nowMs + kExternalPauseGraceMs);
                    idleExtSinceMs_ = 0;
                }
                wasExternal_ = true;
                if (in.externalAudioActive) {
                    idleExtSinceMs_ = 0;
                    wasActiveExternal_ = true;
                    out.shouldRun = false;
                    return out;
                }
                // Van külső stream a regiszterben, de jelenleg nincs tényleges hang (pl. induló vagy PAUSED fül)
                if (in.nowMs < extHoldUntilMs_) {
                    out.shouldRun = false;
                    return out;
                }
                if (idleExtSinceMs_ == 0) {
                    idleExtSinceMs_ = (extHoldUntilMs_ > 0 && in.nowMs >= extHoldUntilMs_) ? extHoldUntilMs_ : in.nowMs;
                }
                if (in.nowMs - idleExtSinceMs_ < kIdleSuppressCapMs) {
                    out.shouldRun = false;
                    return out;
                }
                // Biztonsági felső határ lejárt: hang nélküli külső stream miatt nem maradhat kikapcsolva a keepAlive
                wasActiveExternal_ = false;
                out.shouldRun = true;
                return out;
            }

            // Nincs külső stream a regiszterben: aktív lejátszás vége után rövid lecsengés, majd biztos visszakapcsolás
            if (wasExternal_ && (wasActiveExternal_ || in.nowMs < extHoldUntilMs_)) {
                extHoldUntilMs_ = std::max(extHoldUntilMs_, in.nowMs + kExternalPauseGraceMs);
            }
            wasExternal_ = false;
            wasActiveExternal_ = false;
            idleExtSinceMs_ = 0;
            out.shouldRun = (in.nowMs >= extHoldUntilMs_);
            return out;
        }

        // Nem forrás-követő mód (system / fixed): a keepAlive szabadon futhat
        wasExternal_ = in.hasExternalStream;
        wasActiveExternal_ = false;
        idleExtSinceMs_ = 0;
        extHoldUntilMs_ = 0;
        out.shouldRun = true;
        return out;
    }

private:
    bool lastUserWanted_ = false;
    bool wasExternal_ = false;
    bool wasActiveExternal_ = false;
    int64_t extHoldUntilMs_ = 0;
    int64_t idleExtSinceMs_ = 0;
};

} // namespace ee_analyzer::pw_keepalive
