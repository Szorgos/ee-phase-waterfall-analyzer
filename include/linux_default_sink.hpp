#pragma once
/**
 * @file linux_default_sink.hpp
 * @brief Linux: az „Aqulify” virtuális eszköz a rendszer alapértelmezett kimenete (mint Windowson a rendszerhang-híd).
 *
 * PipeWire-független döntés: a motor induláskor és futás közben figyeli az alapértelmezett kimenetet. Ha az nem az
 * aqulify_sink, visszaállítja rá, hogy minden alkalmazás hangja a DSP-láncon (EQ, vízesés) át szóljon. Ha közben a
 * felhasználó a rendszer hangerő-paneljén egy hangkártyát választott (pl. a laptop hangszóróját), az lesz a DSP
 * kimenete – így a választás nem kerüli meg az Aqulify-t, hanem azt mondja meg, hová szóljon.
 */
#include <algorithm>
#include <string>
#include <vector>

#include "pw_reconnect_policy.hpp"

namespace ee_analyzer::defsink {

struct Action {
    bool setDefault = false;   // az aqulify_sink legyen az alapértelmezett kimenet
    std::string dspTarget;     // nem üres: a DSP kimenete erre a hangkártyára kerüljön
    bool remember = false;     // a dspTarget a felhasználó választása (output_sink mentése)
};

/**
 * @param currentDefault  a jelenlegi alapértelmezett kimenet neve (pactl get-default-sink)
 * @param otherSinks      a többi (nem saját) nyelő neve (listPhysicalSinks)
 * @param startup         első ellenőrzés a motor indulásakor
 * @param hasStoredChoice a felhasználó korábban választott kimenetet (output_sink)
 */
inline Action decide(const std::string& currentDefault, const std::vector<std::string>& otherSinks, bool startup,
                     bool hasStoredChoice, const std::string& ownSink = "aqulify_sink") {
    Action a;
    if (currentDefault.empty() || pw_reconnect::isOwnAqulifyNode(currentDefault, ownSink)) return a;
    a.setDefault = true;
    const bool known = std::find(otherSinks.begin(), otherSinks.end(), currentDefault) != otherSinks.end();
    if (!known) return a;
    // Induláskor a korábbi választás nyer (a rendszer közben, motor nélkül, tartalékra állhatott);
    // futás közben a rendszerpanelen választott hangkártya a felhasználó szándéka: ide szóljon a DSP
    if (startup && hasStoredChoice) return a;
    a.dspTarget = currentDefault;
    a.remember = !startup;
    return a;
}

}  // namespace ee_analyzer::defsink
