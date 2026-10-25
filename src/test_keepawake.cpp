/**
 * Ébren tartás (Windows): a keep_awake::set() a hívó szál végrehajtási állapotát állítja.
 * Az állapot a SetThreadExecutionState visszatérési értékéből olvasható vissza (az előző állapotot adja).
 * Nem Windows-on a teszt kihagyva fut, és sikeres.
 */
#include <cstdio>

#include "keep_awake.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

int main() {
#ifndef _WIN32
    std::printf("test_keepawake: kihagyva (nem Windows)\n");
    return 0;
#else
    int fail = 0;
    auto check = [&](bool cond, const char* what) {
        std::printf("  [%s] %s\n", cond ? "OK" : "HIBA", what);
        if (!cond) ++fail;
    };

    const bool okOn = ee_analyzer::keepawake::set(true);
    // Az előző állapotot olvassuk vissza úgy, hogy közben a bekapcsolt állapot marad
    const EXECUTION_STATE afterOn = SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    check(okOn && (afterOn & ES_SYSTEM_REQUIRED) != 0, "bekapcsolva: a szál kéri, hogy a rendszer ne aludjon (ES_SYSTEM_REQUIRED)");

    const bool okOff = ee_analyzer::keepawake::set(false);
    const EXECUTION_STATE afterOff = SetThreadExecutionState(ES_CONTINUOUS);
    check(okOff && (afterOff & ES_SYSTEM_REQUIRED) == 0, "kikapcsolva: a kérés visszavonva");

    std::printf("\n%s (%d hiba)\n", fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", fail);
    return fail ? 1 : 0;
#endif
}
