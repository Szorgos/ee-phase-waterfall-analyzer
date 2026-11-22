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

    std::printf("\n%s (%d hiba)\n", fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", fail);
    return fail ? 1 : 0;
}
