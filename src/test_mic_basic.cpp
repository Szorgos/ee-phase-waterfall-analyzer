/**
 * @file test_mic_basic.cpp
 * @brief A mikrofonfelvevő alaptesztje hangeszköz nélküli gépen is (CI: Windows, macOS).
 *
 * Ellenőrzi: a platform felvevője létrejön; az eszközlista lekérdezése nem omlik össze; hibás paraméter és nem
 * létező eszköz elutasítva, érthető hibakóddal; ha van bemeneti eszköz, rövid felvétel indul és leáll. A valódi
 * jelutat Linuxon a test_mic_live (elszigetelt PipeWire, visszacsatolt hurok) ellenőrzi.
 */
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "room/mic_capture.hpp"

using namespace aqulify::room;

static int g_fail = 0;
#define CHECK(c, ...) do { const bool ok_ = static_cast<bool>(c); std::printf("  [%s] ", ok_ ? "OK" : "HIBA"); std::printf(__VA_ARGS__); std::printf("\n"); if (!ok_) ++g_fail; } while (0)

int main() {
    std::printf("[1] Mikrofonfelvevő alapviselkedés\n");
    auto cap = createMicCapture();
    CHECK(cap != nullptr, "a platform felvevője létrejön");
    if (!cap) { std::printf("\nSIKERTELEN (%d hiba)\n", g_fail); return 1; }

    const auto devs = cap->listInputs(true);
    std::printf("     eszközök: %zu\n", devs.size());
    for (const auto& d : devs)
        std::printf("     - %s%s%s\n", d.name.c_str(), d.isDefault ? " [alapértelmezett]" : "", d.isMonitor ? " [monitor]" : "");
    CHECK(true, "az eszközlista lekérdezése nem omlik össze");

    std::string err;
    CHECK(!cap->start("", 1000, 1.0, err) && err == "format", "érvénytelen mintavétel elutasítva (%s)", err.c_str());
    err.clear();
    CHECK(!cap->start("", 48000, 0.0, err) && err == "format", "nulla hossz elutasítva (%s)", err.c_str());
    err.clear();
    CHECK(!cap->start("nincs-ilyen-eszkoz", 48000, 1.0, err) && !err.empty(), "nem létező eszköz elutasítva (%s)", err.c_str());
    CHECK(!cap->running() && cap->stop().empty(), "hiba után nem fut, és üres a felvétel");

    bool hasInput = false;
    for (const auto& d : devs) hasInput |= !d.isMonitor;
    if (hasInput) {
        err.clear();
        const bool started = cap->start("", 48000, 2.0, err);
        if (started) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            const auto rec = cap->stop();
            CHECK(!rec.empty(), "rövid felvétel az alapértelmezett bemenetről: %zu minta", rec.size());
        } else {
            std::printf("     (a bemenet nem indult: %s – pl. tiltott mikrofon-hozzáférés; nem hiba)\n", err.c_str());
        }
    } else {
        std::printf("     (nincs bemeneti eszköz ezen a gépen: a felvétel itt nem próbálható)\n");
    }

    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
