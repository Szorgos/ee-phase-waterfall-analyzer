#pragma once
/**
 * @file media_mpris_policy.hpp
 * @brief Platformfüggetlen döntési és naplózási segédfüggvények az MPRIS lejátszásvezérléshez.
 */
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace ee_analyzer::media {

struct MprisCommandPlan {
    std::string primaryMethod;
    std::string fallbackMethod;
};

/** Egy MPRIS lejátszó rövid leírása a szüneteltetési döntéshez. */
struct MprisPlayerEntry {
    std::string busName;
    std::string status;   // pl. "Playing", "Paused", "Stopped"
};

/** Saját Aqulify MPRIS busznév felismerése (önmagunkat soha ne szüneteltessük). */
inline bool isOwnAqulifyMprisName(const std::string& busName) {
    std::string low;
    low.reserve(busName.size());
    for (unsigned char c : busName) low.push_back(static_cast<char>(std::tolower(c)));
    return low.find("aqulify") != std::string::npos;
}

/**
 * Azon külső MPRIS lejátszók kiválasztása, amelyek ténylegesen „Playing” állapotban vannak.
 * A Paused / Stopped és a saját Aqulify példányok kimaradnak, a duplikált busznevek szűrve vannak.
 */
inline std::vector<MprisPlayerEntry> selectPlayingMprisTargets(const std::vector<MprisPlayerEntry>& players) {
    std::vector<MprisPlayerEntry> out;
    for (const auto& p : players) {
        if (p.busName.empty() || p.status != "Playing" || isOwnAqulifyMprisName(p.busName)) continue;
        const bool seen = std::any_of(out.begin(), out.end(),
                                      [&](const MprisPlayerEntry& x) { return x.busName == p.busName; });
        if (!seen) out.push_back(p);
    }
    return out;
}

/**
 * Rövid, PII-mentes naplósor a beépített lejátszó indítása előtti automatikus külső szüneteltetéshez.
 * Nem tartalmaz számcímet, előadót, albumot vagy márkanév-listát.
 */
inline std::string formatMprisAutoPauseLog(size_t pausedCount, size_t stillPlayingCount) {
    std::string s = "[Média] Külső lejátszók szüneteltetése a helyi lejátszás előtt: " +
                    std::to_string(pausedCount) + " szüneteltetve";
    if (stillPlayingCount > 0) {
        s += ", " + std::to_string(stillPlayingCount) + " még nem jelzett szünetet";
    }
    return s;
}

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
