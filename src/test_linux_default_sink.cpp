/**
 * Linux: az „Aqulify” mint alapértelmezett kimenet (linux_default_sink.hpp) egységtesztje.
 * Ellenőrzi: már az Aqulify az alapértelmezett, induláskori átállás (megjegyzett választással és anélkül),
 * futás közbeni kézi hangkártya-választás a rendszerpanelen, ismeretlen és saját nyelő.
 */
#include <cstdio>
#include <string>
#include <vector>

#include "linux_default_sink.hpp"

int main() {
    using namespace ee_analyzer::defsink;
    int fail = 0;
    auto check = [&](bool cond, const char* what) {
        std::printf("  [%s] %s\n", cond ? "OK" : "HIBA", what);
        if (!cond) ++fail;
    };
    const std::string spk = "alsa_output.pci-0000_00_1f.3-platform-skl_hda_dsp_generic.HiFi__Speaker__sink";
    const std::string usb = "alsa_output.usb-Generic_USB_Audio_202109030004-00.analog-stereo";
    const std::vector<std::string> others = { spk, usb };

    std::printf("[1] Már az Aqulify az alapértelmezett\n");
    auto a = decide("aqulify_sink", others, true, false);
    check(!a.setDefault && a.dspTarget.empty(), "nincs teendő");
    a = decide("", others, false, false);
    check(!a.setDefault, "ismeretlen (üres) alapértelmezett: nem nyúlunk hozzá");

    std::printf("[2] Induláskor\n");
    a = decide(spk, others, true, false);
    check(a.setDefault && a.dspTarget == spk && !a.remember, "nincs választás: a DSP a korábbi alapértelmezettre szól, nem mentjük");
    a = decide(spk, others, true, true);
    check(a.setDefault && a.dspTarget.empty(), "megjegyzett választás: az nyer, csak az alapértelmezett áll át");

    std::printf("[3] Futás közben a rendszerpanelen választott hangkártya\n");
    a = decide(usb, others, false, true);
    check(a.setDefault && a.dspTarget == usb && a.remember, "a választott hangkártya lesz a DSP kimenete (mentve), az alapértelmezett vissza");
    a = decide("easyeffects_sink_ismeretlen", others, false, false);
    check(a.setDefault && a.dspTarget.empty(), "nem listázott nyelő: csak az alapértelmezett áll vissza");
    a = decide("aqulify_sink_output", others, false, false);
    check(!a.setDefault, "saját aqulify* csomópont: nincs teendő");

    std::printf("\n%s (%d hiba)\n", fail ? "HIBA" : "Minden teszt rendben", fail);
    return fail ? 1 : 0;
}
