/**
 * @file test_mpris_control.cpp
 * @brief Egységtesztek a 7. lépéshez: MPRIS lejátszásvezérlés metódus-választás, fallback és PII-mentes napló.
 */
#include <iostream>
#include <string>

#include "media_mpris_policy.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool cond, const std::string& label) {
    if (cond) {
        ++g_pass;
    } else {
        ++g_fail;
        std::cerr << "[HIBA] " << label << "\n";
    }
}

void testPlanMprisMethod() {
    std::cout << "[1] planMprisMethod() metódus- és tartalék-választás...\n";
    using ee_analyzer::media::planMprisMethod;

    {
        const auto p = planMprisMethod("toggle", true);
        check(p.primaryMethod == "PlayPause" && p.fallbackMethod == "Pause",
              "toggle (playing=true) -> PlayPause, fallback Pause");
    }
    {
        const auto p = planMprisMethod("toggle", false);
        check(p.primaryMethod == "PlayPause" && p.fallbackMethod == "Play",
              "toggle (playing=false) -> PlayPause, fallback Play");
    }
    {
        const auto p = planMprisMethod("play", false);
        check(p.primaryMethod == "Play" && p.fallbackMethod == "PlayPause",
              "play -> Play, fallback PlayPause");
    }
    {
        const auto p = planMprisMethod("pause", true);
        check(p.primaryMethod == "Pause" && p.fallbackMethod == "PlayPause",
              "pause -> Pause, fallback PlayPause");
    }
    {
        const auto p = planMprisMethod("next", true);
        check(p.primaryMethod == "Next" && p.fallbackMethod.empty(),
              "next -> Next, nincs fallback");
    }
    {
        const auto p = planMprisMethod("previous", true);
        check(p.primaryMethod == "Previous" && p.fallbackMethod.empty(),
              "previous -> Previous, nincs fallback");
    }
    {
        const auto p = planMprisMethod("unknown", false);
        check(p.primaryMethod.empty() && p.fallbackMethod.empty(),
              "ismeretlen parancs -> üres terv");
    }
}

void testFormatMprisCommandLog() {
    std::cout << "[2] formatMprisCommandLog() formátum és PII-mentesség...\n";
    using ee_analyzer::media::formatMprisCommandLog;

    {
        const std::string line = formatMprisCommandLog("TIDAL", "toggle", "Playing", "Paused");
        check(line == "[Média] TIDAL: toggle → PlaybackStatus előtte/utána: Playing / Paused",
              "normál naplósor formátuma pontos");
    }
    {
        const std::string line = formatMprisCommandLog("", "pause", "", "");
        check(line == "[Média] MPRIS: pause → PlaybackStatus előtte/utána: ? / ?",
              "üres mezőknél alapértelmezett helyettesítés");
    }
}

void testAutoPausePolicy() {
    std::cout << "[3] selectPlayingMprisTargets() és PII-mentes auto-pause napló...\n";
    using ee_analyzer::media::MprisPlayerEntry;
    using ee_analyzer::media::formatMprisAutoPauseLog;
    using ee_analyzer::media::isOwnAqulifyMprisName;
    using ee_analyzer::media::selectPlayingMprisTargets;

    check(isOwnAqulifyMprisName("org.mpris.MediaPlayer2.aqulify"), "saját aqulify busznév kiszűrve");
    check(isOwnAqulifyMprisName("org.mpris.MediaPlayer2.Aqulify.instance123"), "nagybetűs Aqulify busznév is kiszűrve");
    check(!isOwnAqulifyMprisName("org.mpris.MediaPlayer2.sone"), "külső lejátszó busznév nem saját");

    const std::vector<MprisPlayerEntry> input = {
        { "org.mpris.MediaPlayer2.sone", "Playing" },
        { "org.mpris.MediaPlayer2.chromium.instance42", "Paused" },
        { "org.mpris.MediaPlayer2.vlc", "Stopped" },
        { "org.mpris.MediaPlayer2.aqulify", "Playing" },
        { "org.mpris.MediaPlayer2.sone", "Playing" },
        { "org.mpris.MediaPlayer2.firefox.instance99", "Playing" },
    };
    const auto targets = selectPlayingMprisTargets(input);
    check(targets.size() == 2 &&
          targets[0].busName == "org.mpris.MediaPlayer2.sone" &&
          targets[1].busName == "org.mpris.MediaPlayer2.firefox.instance99",
          "csak az egyedi, külső, Playing állapotú lejátszók kerülnek kiválasztásra (Paused/Stopped/saját kihagyva)");

    const std::string log1 = formatMprisAutoPauseLog(1, 0);
    check(log1 == "[Média] Külső lejátszók szüneteltetése a helyi lejátszás előtt: 1 szüneteltetve",
          "auto-pause napló sikeres szüneteltetésnél pontos és PII-mentes");
    const std::string log2 = formatMprisAutoPauseLog(1, 1);
    check(log2 == "[Média] Külső lejátszók szüneteltetése a helyi lejátszás előtt: 1 szüneteltetve, 1 még nem jelzett szünetet",
          "auto-pause napló részleges szüneteltetésnél jelzi a még futók számát");
}

} // namespace

int main() {
    testPlanMprisMethod();
    testFormatMprisCommandLog();
    testAutoPausePolicy();
    std::cout << "\nEredmény: " << g_pass << " OK, " << g_fail << " HIBA.\n";
    return g_fail == 0 ? 0 : 1;
}
