#pragma once
/**
 * @file media_mpris_policy.hpp
 * @brief Platformfüggetlen döntési és naplózási segédfüggvények az MPRIS lejátszásvezérléshez.
 */
#include <string>

namespace ee_analyzer::media {

struct MprisCommandPlan {
    std::string primaryMethod;
    std::string fallbackMethod;
};

/**
 * Melyik MPRIS D-Bus metódust hívjuk (és mi a tartalék, ha a hívás hibával tér vissza).
 * - "toggle": PlayPause (tartalék: Pause/Play az aktuális playing állapot alapján)
 * - "play":   Play (tartalék: PlayPause)
 * - "pause":  Pause (tartalék: PlayPause)
 */
inline MprisCommandPlan planMprisMethod(const std::string& action, bool currentlyPlaying) {
    if (action == "toggle") {
        return { "PlayPause", currentlyPlaying ? "Pause" : "Play" };
    }
    if (action == "play") {
        return { "Play", "PlayPause" };
    }
    if (action == "pause") {
        return { "Pause", "PlayPause" };
    }
    if (action == "next") {
        return { "Next", "" };
    }
    if (action == "previous") {
        return { "Previous", "" };
    }
    return {};
}

/**
 * Rövid, PII-mentes diagnosztikai naplósor külső lejátszó parancsához.
 * Szándékosan NEM tartalmaz számcímet, előadót vagy albumnevet.
 */
inline std::string formatMprisCommandLog(const std::string& app,
                                         const std::string& action,
                                         const std::string& beforeStatus,
                                         const std::string& afterStatus) {
    return "[Média] " + (app.empty() ? std::string("MPRIS") : app) + ": " + action +
           " → PlaybackStatus előtte/utána: " +
           (beforeStatus.empty() ? std::string("?") : beforeStatus) + " / " +
           (afterStatus.empty() ? std::string("?") : afterStatus);
}

} // namespace ee_analyzer::media
