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

} // namespace

int main() {
    testPlanMprisMethod();
    testFormatMprisCommandLog();
    std::cout << "\nEredmény: " << g_pass << " OK, " << g_fail << " HIBA.\n";
    return g_fail == 0 ? 0 : 1;
}
