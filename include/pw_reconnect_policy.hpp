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
#include <vector>

namespace ee_analyzer::pw_reconnect {

inline constexpr int kInitialBackoffMs = 1000;
inline constexpr int kMaxBackoffMs = 5000;
inline constexpr int kNewSinkDelayMs = 150;

inline std::string toLowerAscii(const std::string& s) {
    std::string low;
    low.reserve(s.size());
    for (unsigned char c : s) low.push_back(static_cast<char>(std::tolower(c)));
    return low;
}

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
 * PipeWire/PulseAudio néma/dummy nyelő-e (pl. auto_null, dummy output).
 * Ezek nem valódi hangkártyák: ha a fizikai DAC átmenetileg eltűnik (pl. ALSA foglalás miatt),
 * nem szabad véglegesen ezekre rögzülni vagy újracsatlakozási okként kezelni őket.
 */
inline bool isDummyOrNullSink(const std::string& name) {
    if (name.empty()) return false;
    const std::string low = toLowerAscii(name);
    return low == "auto_null" || low == "null" ||
           low.find("auto_null") != std::string::npos ||
           low.find("dummy") != std::string::npos ||
           low.find("null_sink") != std::string::npos;
}

/** Külső PipeWire Audio/Sink jelölt névvel és munkamenet-prioritással (priority.session). */
struct SinkCandidate {
    std::string name;
    int priority = 0;
};

/**
 * Determinisztikus tartalék-választás valódi külső nyelők közül (nem saját aqulify*, nem auto_null/dummy).
 * Sorrend: magasabb priority.session előbb (mint a WirePlumbernél), egyenlőnél USB előnyben, végül alfabetikusan.
 */
inline std::string selectFallbackSinkByPriority(const std::vector<SinkCandidate>& candidates,
                                                const std::string& sinkName = "aqulify_sink") {
    std::vector<SinkCandidate> valid;
    valid.reserve(candidates.size());
    for (const auto& c : candidates) {
        if (c.name.empty() || isOwnAqulifyNode(c.name, sinkName) || isDummyOrNullSink(c.name)) continue;
        valid.push_back(c);
    }
    if (valid.empty()) return {};
    std::sort(valid.begin(), valid.end(), [](const SinkCandidate& a, const SinkCandidate& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        const bool aUsb = toLowerAscii(a.name).find("usb") != std::string::npos;
        const bool bUsb = toLowerAscii(b.name).find("usb") != std::string::npos;
        if (aUsb != bUsb) return aUsb;
        return a.name < b.name;
    });
    return valid.front().name;
}

/**
 * Determinisztikus tartalék-választás névhalmazból (prioritás nélkül: USB előnyben, majd alfabetikusan).
 */
inline std::string selectFallbackSink(const std::unordered_set<std::string>& knownExternalSinks,
                                      const std::string& sinkName = "aqulify_sink") {
    std::vector<SinkCandidate> candidates;
    candidates.reserve(knownExternalSinks.size());
    for (const auto& s : knownExternalSinks) candidates.push_back({ s, 0 });
    return selectFallbackSinkByPriority(candidates, sinkName);
}

/**
 * A lejátszó stream céleszközének kiválasztása:
 * - ha a felhasználó választott eszközt, az nem saját aqulify* és nem dummy csomópont, és elérhető a gráfban,
 *   akkor azt célozzuk (PW_KEY_TARGET_OBJECT);
 * - különben üres szöveget adunk vissza (a meglévő kompatibilitási hívások számára).
 */
inline std::string resolveTarget(const std::string& preferredTarget,
                                 const std::unordered_set<std::string>& knownExternalSinks,
                                 const std::string& sinkName = "aqulify_sink") {
    if (preferredTarget.empty() || isOwnAqulifyNode(preferredTarget, sinkName) || isDummyOrNullSink(preferredTarget))
        return {};
    if (knownExternalSinks.find(preferredTarget) != knownExternalSinks.end()) return preferredTarget;
    return {};
}

/**
 * Céleszköz-választás automatikus fizikai tartalékkal (priority.session sorrenddel).
 */
inline std::string resolveTargetOrFallbackByPriority(const std::string& preferredTarget,
                                                     const std::vector<SinkCandidate>& candidates,
                                                     const std::string& sinkName = "aqulify_sink") {
    if (!preferredTarget.empty() && !isOwnAqulifyNode(preferredTarget, sinkName) && !isDummyOrNullSink(preferredTarget)) {
        for (const auto& c : candidates) {
            if (c.name == preferredTarget) return preferredTarget;
        }
    }
    return selectFallbackSinkByPriority(candidates, sinkName);
}

/**
 * Céleszköz-választás automatikus fizikai tartalékkal (névhalmazból).
 */
inline std::string resolveTargetOrFallback(const std::string& preferredTarget,
                                           const std::unordered_set<std::string>& knownExternalSinks,
                                           const std::string& sinkName = "aqulify_sink") {
    const std::string exact = resolveTarget(preferredTarget, knownExternalSinks, sinkName);
    if (!exact.empty()) return exact;
    return selectFallbackSink(knownExternalSinks, sinkName);
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

    /** Új valódi külső (nem saját aqulify*, nem dummy/auto_null) Audio/Sink csomópont jelent meg a regiszterben. */
    Action onNewExternalSink(const std::string& nodeName, const std::string& sinkName = "aqulify_sink") {
        Action a;
        if (!pending_ || isOwnAqulifyNode(nodeName, sinkName) || isDummyOrNullSink(nodeName)) return a;
        backoffMs_ = kInitialBackoffMs;
        a.schedule = true;
        a.delayMs = kNewSinkDelayMs;
        return a;
    }

    /** Azonnali újracsatlakozás kérése (pl. rögzítetlen/dummy cél cseréje valódi DAC megjelenésekor vagy ALSA után). */
    Action requestImmediate(int delayMs = kNewSinkDelayMs) {
        Action a;
        pending_ = true;
        backoffMs_ = kInitialBackoffMs;
        a.schedule = true;
        a.delayMs = std::max(1, delayMs);
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
