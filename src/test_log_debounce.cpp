/**
 * @file test_log_debounce.cpp
 * @brief Egységtesztek a 8. lépéshez: StableStateLogger hiszterézis / debounce naplózás.
 */
#include <iostream>
#include <string>

#include "log_debounce.hpp"

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

void testStableStateLogger() {
    std::cout << "[1] StableStateLogger: kezdeti napló, rövid villanás elnyomása, tartós váltás...\n";
    aq::log::StableStateLogger lg(2500);
    int out = -1;

    // 1. Első állapot azonnal naplózódik
    check(lg.update(1, 1000, out) && out == 1, "első állapot azonnal naplózódik (1)");
    check(!lg.update(1, 1500, out), "változatlan állapot nem naplózódik");

    // 2. Rövid 1 -> 2 -> 1 villanás (1000 ms < 2500 ms): 0 naplósor
    check(!lg.update(2, 2000, out), "váltás kezdete (2) még nem naplózódik");
    check(!lg.update(2, 3000, out), "1000 ms után még nem naplózódik");
    check(!lg.update(1, 3100, out), "visszatérés 1-be törli a függőben lévő váltást");
    check(!lg.update(1, 7000, out), "hosszú idő után sem naplózódik újra 1");

    // 3. Tartós váltás 1 -> 3 (>= 2500 ms)
    check(!lg.update(3, 8000, out), "váltás kezdete (3) még nem naplózódik");
    check(!lg.update(3, 10499, out), "2499 ms-nél még nem naplózódik");
    check(lg.update(3, 10500, out) && out == 3, "2500 ms-nél naplózódik (3)");
    check(!lg.update(3, 15000, out), "utána ugyanaz nem naplózódik újra");

    // 4. Köztes állapotváltás újraindítja az időzítőt (3 -> 2 -> 1)
    check(!lg.update(2, 20000, out), "3 -> 2 indul");
    check(!lg.update(1, 22000, out), "2 -> 1 újraindítja a 2500 ms ablakot");
    check(!lg.update(1, 24000, out), "2000 ms-nél még nem naplózódik (1)");
    check(lg.update(1, 24500, out) && out == 1, "2500 ms-nél naplózódik (1)");
}

} // namespace

int main() {
    testStableStateLogger();
    std::cout << "\nEredmény: " << g_pass << " OK, " << g_fail << " HIBA.\n";
    return g_fail == 0 ? 0 : 1;
}
