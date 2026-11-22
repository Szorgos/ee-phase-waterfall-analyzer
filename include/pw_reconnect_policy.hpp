#pragma once
/**
 * @file pw_reconnect_policy.hpp
 * @brief PipeWire lejátszó stream („aqulify_sink_output”) automatikus újracsatlakozási politikája.
 *
 * PipeWire-független állapotgép és célválasztás: a stream hibája („no target node available”, USB DAC
 * kihúzása / késői megjelenése) esetén exponenciálisan növekvő, korlátozott várakozással (1 → 2 → 4 → 5 s)
 * ütemezi az újranyitást, a naplóban csak az első hibát és a sikeres visszatérést jelzi, a saját
 * aqulify* csomópontokat soha nem célozza, és a felhasználó által választott kimenetet előnyben részesíti.
 */
#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>

namespace ee_analyzer::pw_reconnect {

inline constexpr int kInitialBackoffMs = 1000;
inline constexpr int kMaxBackoffMs = 5000;
inline constexpr int kNewSinkDelayMs = 150;

/** Saját Aqulify csomópont-e (visszacsatolási hurok elkerülése: soha nem lehet kimeneti cél). */
inline bool isOwnAqulifyNode(const std::string& name, const std::string& sinkName = "aqulify_sink") {
    if (name.empty()) return false;
    if (!sinkName.empty() && name == sinkName) return true;
    if (name.size() >= 7) {
        const char* pfx = "aqulify";
        bool match = true;
        for (size_t i = 0; i < 7; ++i) {
            if (std::tolower(static_cast<unsigned char>(name[i])) != pfx[i]) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

/**
 * A lejátszó stream céleszközének kiválasztása:
 * - ha a felhasználó választott eszközt, az nem saját aqulify* csomópont, és jelenleg elérhető a gráfban,
 *   akkor azt célozzuk (PW_KEY_TARGET_OBJECT);
 * - különben üres szöveget adunk vissza (a WirePlumber az alapértelmezett, nem saját kimenetre köt).
 */
inline std::string resolveTarget(const std::string& preferredTarget,
                                 const std::unordered_set<std::string>& knownExternalSinks,
                                 const std::string& sinkName = "aqulify_sink") {
    if (preferredTarget.empty() || isOwnAqulifyNode(preferredTarget, sinkName)) return {};
    if (knownExternalSinks.find(preferredTarget) != knownExternalSinks.end()) return preferredTarget;
    return {};
}

/** Állapotgép a lejátszó stream hiba utáni újracsatlakozásához és napló-zajcsökkentéséhez. */
class State {
public:
    struct Action {
        bool schedule = false;
        bool logError = false;
        int delayMs = 0;
    };

    /** A playback stream PW_STREAM_STATE_ERROR állapotba lépett. */
    Action onError() {
        Action a;
        a.schedule = true;
        a.logError = !errorLogged_;
        errorLogged_ = true;
        pending_ = true;
        a.delayMs = backoffMs_;
        backoffMs_ = std::min(kMaxBackoffMs, backoffMs_ * 2);
        return a;
    }

    /** Egy újracsatlakozási kísérlet közvetlen connect() hibával tért vissza (köztes próba: nincs napló). */
    Action onAttemptFailed() {
        Action a;
        a.schedule = true;
        a.logError = false;
        pending_ = true;
        a.delayMs = backoffMs_;
        backoffMs_ = std::min(kMaxBackoffMs, backoffMs_ * 2);
        return a;
    }

    /** Új külső (nem saját aqulify*) Audio/Sink csomópont jelent meg a regiszterben. */
    Action onNewExternalSink(const std::string& nodeName, const std::string& sinkName = "aqulify_sink") {
        Action a;
        if (!pending_ || isOwnAqulifyNode(nodeName, sinkName)) return a;
        backoffMs_ = kInitialBackoffMs;
        a.schedule = true;
        a.delayMs = kNewSinkDelayMs;
        return a;
    }

    /** A playback stream sikeresen elindult (PW_STREAM_STATE_STREAMING). Igaz, ha hiba után tért vissza. */
    bool onStreaming() {
        const bool recovered = errorLogged_;
        errorLogged_ = false;
        pending_ = false;
        backoffMs_ = kInitialBackoffMs;
        return recovered;
    }

    void reset() {
        pending_ = false;
        errorLogged_ = false;
        backoffMs_ = kInitialBackoffMs;
    }

    bool pending() const { return pending_; }
    bool errorLogged() const { return errorLogged_; }
    int backoffMs() const { return backoffMs_; }

private:
    bool pending_ = false;
    bool errorLogged_ = false;
    int backoffMs_ = kInitialBackoffMs;
};

} // namespace ee_analyzer::pw_reconnect
