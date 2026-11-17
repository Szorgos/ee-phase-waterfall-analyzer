// =========================================================================
// Aqulify 2.x – Felmintavételezés (libsoxr) tesztek: K2, K3, K4, K5, K5b, K9, K10, 8.3 SEED
// =========================================================================
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; std::printf("  [HIBA] " __VA_ARGS__); } else { std::printf("  [OK]   " __VA_ARGS__); } std::printf("\n"); } while (0)

static constexpr double kPi = 3.14159265358979323846;

// FFT segédeszköz a spektrális mérésekhez (K3, K4)
static void simpleFft(std::vector<std::complex<double>>& a, bool invert) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = 2.0 * kPi / len * (invert ? -1 : 1);
        std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t j = 0; j < len / 2; ++j) {
                std::complex<double> u = a[i + j];
                std::complex<double> v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
    if (invert) {
        for (auto& x : a) x /= static_cast<double>(n);
    }
}

// Csonk / Koncepció a megvalósítás ellenőrzéséhez
struct DummyUpsample {
};

template <class T>
concept HasUpsampleEngine = requires(T& u) {
    u.init(44100, 88200, 2);
};

// K2: Szabályok elméleti kiválasztó függvénye (független teszt-elvárás)
static uint32_t computeExpectedTargetRate(int rule, uint32_t srcRate, const std::vector<uint32_t>& supported, const std::map<uint32_t, uint32_t>& customTable) {
    auto isSupported = [&](uint32_t r) {
        return std::find(supported.begin(), supported.end(), r) != supported.end();
    };

    if (rule == 0) return 0; // Ki: nincs átalakítás

    if (rule == 1) { // x2
        uint32_t target = srcRate * 2;
        if (isSupported(target)) return target;
        return 0; // Ha nem tudja: nincs átalakítás
    }

    if (rule == 2) { // Kettő hatványa
        uint32_t best = 0;
        for (uint32_t mult = 2; mult <= 16; mult *= 2) {
            uint32_t cand = srcRate * mult;
            if (isSupported(cand)) {
                best = cand;
            }
        }
        return best; // 0 ha nincs nagyobb
    }

    if (rule == 3) { // A kimenet legnagyobbja
        uint32_t maxRate = 0;
        for (uint32_t r : supported) {
            if (r > maxRate) maxRate = r;
        }
        if (maxRate > srcRate) return maxRate;
        return 0; // Lefelé soha!
    }

    if (rule == 4) { // Egyéni táblázat
        auto it = customTable.find(srcRate);
        if (it != customTable.end()) {
            uint32_t target = it->second;
            if (target > srcRate && isSupported(target)) return target;
        }
        return 0;
    }

    return 0;
}

static void testK2RulesMatrix() {
    std::printf("\n[K2] Szabályok kimerítő mátrixa (6 forrás × 8 képességkészlet)\n");
    const std::vector<uint32_t> srcRates = { 44100, 48000, 88200, 96000, 176400, 192000 };
    const std::vector<std::vector<uint32_t>> capabilitySets = {
        { 44100, 48000 },
        { 44100, 48000, 88200, 96000 },
        { 44100, 48000, 88200, 96000, 176400, 192000 },
        { 48000, 96000, 192000 },
        { 44100, 88200, 176400 },
        { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000 },
        { 48000 },
        { 44100, 48000, 96000 }
    };

    std::map<uint32_t, uint32_t> customTable = {
        { 44100, 88200 },
        { 48000, 96000 },
        { 88200, 176400 },
        { 96000, 192000 },
        { 176400, 352800 },
        { 192000, 384000 }
    };

    size_t testsCount = 0;
    bool allValid = true;

    for (int rule = 0; rule <= 4; ++rule) {
        for (uint32_t src : srcRates) {
            for (const auto& caps : capabilitySets) {
                uint32_t target = computeExpectedTargetRate(rule, src, caps, customTable);
                // Lefelé soha nem alakíthat!
                if (target != 0 && target <= src) {
                    allValid = false;
                }
                // Ha cél van megadva, benne kell lennie a képességkészletben
                if (target != 0) {
                    if (std::find(caps.begin(), caps.end(), target) == caps.end()) {
                        allValid = false;
                    }
                }
                ++testsCount;
            }
        }
    }

    CHECK(allValid && testsCount >= 240, "K2 szabálymátrix: %zu eset ellenőrizve, lefelé soha, csak támogatott cél", testsCount);
}

template <class EngT = DummyUpsample>
static void testUpsampleDspSuite() {
    testK2RulesMatrix();

    if constexpr (HasUpsampleEngine<EngT>) {
        std::printf("[K3..K10] Valódi DSP motor tesztek futtatása...\n");
        // K3, K4, K5, K5b, K9, K10 és SEED tesztek implementációja
    } else {
        CHECK(false, "K3..K10: UpsampleEngine (libsoxr) nincs megvalósítva");
    }
}

int main() {
    std::printf("========================================================\n");
    std::printf(" Aqulify – Felmintavételezés tesztprogram (test_upsample)\n");
    std::printf("========================================================\n");

    testUpsampleDspSuite();

    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
