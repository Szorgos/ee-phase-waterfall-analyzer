/**
 * PipeWire lejátszó stream újracsatlakozási politika (pw_reconnect_policy.hpp) egységtesztje.
 * Ellenőrzi: exponenciális backoff (1 -> 2 -> 4 -> 5 s), első hiba + sikeres visszatérés egyszeri naplózása,
 * saját aqulify* csomópontok kizárása, felhasználó által választott cél előnyben részesítése és fallback.
 */
#include <cstdio>
#include <string>
#include <unordered_set>

#include "pw_reconnect_policy.hpp"

int main() {
    using namespace ee_analyzer::pw_reconnect;
    int fail = 0;
    auto check = [&](bool cond, const char* what) {
        std::printf("  [%s] %s\n", cond ? "OK" : "HIBA", what);
        if (!cond) ++fail;
    };

    std::printf("[1] Saját aqulify* csomópontok kizárása és célválasztás\n");
    check(isOwnAqulifyNode("aqulify_sink"), "aqulify_sink saját csomópont");
    check(isOwnAqulifyNode("aqulify_sink_output"), "aqulify_sink_output saját csomópont");
    check(isOwnAqulifyNode("Aqulify_custom"), "Aqulify_custom (nagybetűs) saját csomópont");
    check(!isOwnAqulifyNode("alsa_output.usb-XMOS_HIFI_DSD-00.analog-stereo"), "USB DAC nem saját csomópont");
    check(!isOwnAqulifyNode("test_dac"), "virtuális DAC nem saját csomópont");

    const std::unordered_set<std::string> sinks = {
        "alsa_output.usb-XMOS_HIFI_DSD-00.analog-stereo",
        "alsa_output.pci-0000_00_1f.3.analog-stereo"
    };
    check(resolveTarget("alsa_output.usb-XMOS_HIFI_DSD-00.analog-stereo", sinks) ==
              "alsa_output.usb-XMOS_HIFI_DSD-00.analog-stereo",
          "választott kimenet megvan -> azt célozza");
    check(resolveTarget("alsa_output.usb-HIANYZO_DAC.analog-stereo", sinks).empty(),
          "választott kimenet nincs meg -> üres cél (alapértelmezett fallback)");
    check(resolveTarget("aqulify_sink", { "aqulify_sink", "test_dac" }).empty(),
          "saját aqulify_sink akkor sem lehet cél, ha szerepel a listában");

    std::printf("[2] Backoff, naplózaj-védelem és külső Audio/Sink megjelenése\n");
    State st;
    const auto e1 = st.onError();
    check(e1.schedule && e1.logError && e1.delayMs == 1000, "1. hiba: ütemez 1000 ms, naplózza");
    const auto e2 = st.onError();
    check(e2.schedule && !e2.logError && e2.delayMs == 2000, "2. hiba: ütemez 2000 ms, nincs újabb hiba-napló");
    const auto e3 = st.onAttemptFailed();
    check(e3.schedule && !e3.logError && e3.delayMs == 4000, "3. sikertelen próba: 4000 ms, nincs napló");
    const auto e4 = st.onAttemptFailed();
    check(e4.schedule && !e4.logError && e4.delayMs == 5000, "4. sikertelen próba: 5000 ms (felső korlát)");
    const auto e5 = st.onAttemptFailed();
    check(e5.schedule && !e5.logError && e5.delayMs == 5000, "5. sikertelen próba: marad 5000 ms");

    const auto ownEv = st.onNewExternalSink("aqulify_sink");
    check(!ownEv.schedule, "saját aqulify_sink megjelenése nem indít újracsatlakozást");
    const auto extEv = st.onNewExternalSink("test_dac");
    check(extEv.schedule && extEv.delayMs == kNewSinkDelayMs && st.backoffMs() == 1000,
          "külső sink megjelenése azonnal (150 ms) ütemez és alapra állítja a backoffot");

    check(st.onStreaming(), "sikeres STREAMING hiba után -> naplózza az újracsatlakozást");
    check(!st.onStreaming(), "második STREAMING hiba nélkül -> nem naplóz újra");
    check(!st.pending() && !st.errorLogged() && st.backoffMs() == 1000, "visszatérés után az állapot alaphelyzetben");

    // 2. kiesési ciklus: újabb hiba ismét naplózza az 1. hibát és a visszatérést (köztes próba nélkül)
    const auto c2e1 = st.onError();
    check(c2e1.schedule && c2e1.logError && c2e1.delayMs == 1000, "2. kiesés 1. hibája: ismét naplóz, 1000 ms");
    const auto c2e2 = st.onError();
    check(c2e2.schedule && !c2e2.logError && c2e2.delayMs == 2000, "2. kiesés 2. hibája: nem naplóz újra");
    check(st.onStreaming(), "2. kiesés utáni STREAMING: ismét naplózza az újracsatlakozást");

    std::printf("[3] Dummy/null nyelő kiszűrése és fizikai tartalék-választás (resolveTargetOrFallback)\n");
    check(isDummyOrNullSink("auto_null"), "auto_null dummy nyelőként felismerve");
    check(isDummyOrNullSink("alsa_output.dummy"), "dummy nyelő felismerve");
    check(!isDummyOrNullSink("alsa_output.usb-BurrBrown_USB_AUDIO_DAC-00.analog-stereo"), "USB DAC nem dummy");
    check(!isDummyOrNullSink("test_dac"), "virtuális teszt DAC nem dummy");

    const std::unordered_set<std::string> mixed = {
        "auto_null",
        "aqulify_sink",
        "alsa_output.pci-0000_00_1f.3.analog-stereo",
        "alsa_output.usb-BurrBrown_USB_AUDIO_DAC-00.analog-stereo"
    };
    check(resolveTargetOrFallback("", mixed) == "alsa_output.usb-BurrBrown_USB_AUDIO_DAC-00.analog-stereo",
          "üres választásnál a valódi USB DAC-ot választja (nem auto_null-t és nem aqulify_sink-et)");
    check(resolveTargetOrFallback("", { "auto_null", "alsa_output.pci-0000_00_1f.3.analog-stereo" }) ==
              "alsa_output.pci-0000_00_1f.3.analog-stereo",
          "USB nélkül a valódi PCI kártyát választja (nem auto_null-t)");
    check(resolveTargetOrFallback("", { "auto_null", "aqulify_sink" }).empty(),
          "csak saját és dummy nyelő esetén üres cél");
    check(resolveTargetOrFallback("alsa_output.usb-HIANYZO.analog-stereo", { "auto_null", "alsa_output.pci-0.analog-stereo" }) ==
              "alsa_output.pci-0.analog-stereo",
          "hiányzó választott DAC esetén fizikai tartalékra esik vissza");
    check(resolveTargetOrFallback("aqulify_sink", { "aqulify_sink", "test_dac" }) == "test_dac",
          "saját aqulify_sink megadásakor is valódi külső nyelőt választ");

    State st3;
    st3.onError();
    check(!st3.onNewExternalSink("auto_null").schedule, "auto_null megjelenése nem indít újracsatlakozást");
    check(st3.onNewExternalSink("alsa_output.usb-BurrBrown-00.analog-stereo").schedule,
          "valódi USB DAC megjelenése azonnal újracsatlakozást indít");
    State st4;
    const auto imm = st4.requestImmediate();
    check(imm.schedule && imm.delayMs == kNewSinkDelayMs && st4.pending(),
          "requestImmediate() beállítja a pending állapotot és gyors időzítőt kér");

    const std::vector<SinkCandidate> prioCands = {
        { "aq_test_speaker", 500 },
        { "dac_b", 1000 },
        { "dac_a", 2000 },
        { "auto_null", 9999 }
    };
    check(resolveTargetOrFallbackByPriority("", prioCands) == "dac_a",
          "priority.session sorrend: dac_a (2000) megelőzi az alfabetikusan előrébb álló aq_test_speaker-t (500) és az auto_null-t");
    check(resolveTargetOrFallbackByPriority("dac_b", prioCands) == "dac_b",
          "explicit választott kimenet (dac_b) megelőzi a magasabb prioritású tartalékot");

    std::printf("\n%s (%d hiba)\n", fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", fail);
    return fail ? 1 : 0;
}
