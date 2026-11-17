/**
 * @file test_room.cpp
 * @brief Saját szobamérő és korrekciós mag tesztjei szintetikus szobával.
 */

#include "room/room_correction.hpp"
#include "room/measurement_manager.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            std::printf("HIBA: " __VA_ARGS__); \
            std::printf(" (%s:%d)\n", __FILE__, __LINE__); \
            std::exit(1); \
        } \
    } while (0)

using namespace aqulify::room;

static void testSweepAndInverse() {
    std::printf("[1] Sweep és inverz szűrő generálás (Farina-módszer)\n");
    const uint32_t sr = 48000;
    const double dur = 1.0; // 1 másodperc
    const double f1 = 20.0, f2 = 20000.0;

    auto sweep = generateLogSweep(f1, f2, dur, sr, 0.2f, 10.0);
    auto inv = generateInverseFilter(f1, f2, dur, sr, 0.2f, 10.0);

    CHECK(!sweep.empty(), "Sweep nem üres");
    CHECK(!inv.empty(), "Inverz szűrő nem üres");
    CHECK(sweep.size() == inv.size(), "Sweep és inverz hossza egyezik (%zu vs %zu)", sweep.size(), inv.size());

    // Cirkuláris / direkt konvolúció ellenőrzése közvetlen hurokban
    // s(t) * inv(t) egy Dirac impulzust kell adjon
    double dotPeak = 0.0;
    for (size_t i = 0; i < sweep.size(); ++i) {
        dotPeak += static_cast<double>(sweep[i]) * inv[i];
    }
    CHECK(std::abs(dotPeak - 1.0) < 0.05, "Sweep és inverz Dirac csúcs ~1.0 (mért: %.4f)", dotPeak);
    std::printf("   -> Dirac rekonstrukció csúcsérték: %.4f (tűrés: 0.05)\n", dotPeak);
}

static void testSyntheticRoomDeconvolution() {
    std::printf("[2] Szintetikus szoba dekonvolúciója, késleltetés és polaritás\n");
    const uint32_t sr = 48000;
    const double dur = 1.0;
    const double f1 = 20.0, f2 = 20000.0;

    auto sweep = generateLogSweep(f1, f2, dur, sr, 0.2f, 10.0);

    SyntheticRoom room;
    room.delayMs = 12.5f;           // 12.5 ms késleltetés
    room.invertPolarity = true;      // Fordított polaritás
    room.noiseLevel = 0.0001f;       // Kis alapzaj

    auto recorded = room.simulateResponse(sweep, sr);
    CHECK(recorded.size() > sweep.size(), "Felvétel hossza megfelelő");

    auto result = deconvolve(recorded, sweep, sr);
    CHECK(!result.ir.empty(), "Dekonvolválási eredmény nem üres");

    // Késleltetés ellenőrzése: elvárt 12.5 ms, megengedett tűrés 0.1 ms
    const float delayDiff = std::abs(result.delayMs - 12.5f);
    CHECK(delayDiff < 0.1f, "Késleltetés felismerve: mért %.2f ms, elvárt 12.5 ms (eltérés: %.3f ms)",
          result.delayMs, delayDiff);

    // Polaritás ellenőrzése
    CHECK(result.invertedPolarity, "Fordított polaritás helyesen felismerve");
    CHECK(result.peakValue < 0.0f, "Csúcs értéke negatív: %.4f", result.peakValue);

    std::printf("   -> Mért késleltetés: %.2f ms (tűrés: 0.1 ms)\n", result.delayMs);
    std::printf("   -> Polaritás fordítás: felismerve (csúcs: %.4f)\n", result.peakValue);
    std::printf("   -> Jel-zaj viszony (SNR): %.1f dB\n", result.snrDb);
}

static void testSmoothingAndAutoEq() {
    std::printf("[3] Simítás és Auto-EQ célgörbe illesztés és gain korlátok\n");

    // Szintetikus frekvenciamenet generálása (1/24 oktáv sűrűséggel 20 Hz .. 20 kHz)
    std::vector<FreqPoint> rawResponse;
    for (int i = 0; i < 240; ++i) {
        const double f = 20.0 * std::pow(20000.0 / 20.0, static_cast<double>(i) / 239.0);
        // Szoba módus: 45 Hz körül +6 dB csúcs (Q=5), 120 Hz körül -6 dB kioltás
        double mag = 0.0;
        const double w45 = f / 45.0;
        mag += 6.0 / (1.0 + 25.0 * (w45 - 1.0 / w45) * (w45 - 1.0 / w45)); // +6 dB peak
        const double w120 = f / 120.0;
        mag -= 6.0 / (1.0 + 9.0 * (w120 - 1.0 / w120) * (w120 - 1.0 / w120)); // -6 dB dip

        FreqPoint pt;
        pt.freq = static_cast<float>(f);
        pt.magDb = static_cast<float>(mag);
        rawResponse.push_back(pt);
    }

    // 1/6 oktáv simítás
    auto smoothed = smoothSpectrum(rawResponse, 6);
    CHECK(smoothed.size() == rawResponse.size(), "Simított pontok száma egyezik");

    // Auto-EQ számítása max +3 dB emelési korláttal
    auto eqRes = calculateAutoEq(smoothed, {}, 10, 3.0f, -24.0f);
    CHECK(!eqRes.bands.empty(), "Auto-EQ talált korrekciós sávokat (db: %zu)", eqRes.bands.size());

    // Ellenőrzés 1: A 45 Hz-es kiemelést visszavágja (negatív gain ~ -6 dB)
    bool found45Mode = false;
    for (const auto& b : eqRes.bands) {
        if (std::abs(b.freq - 45.0f) < 8.0f && b.gainDb < -3.0f) {
            found45Mode = true;
            std::printf("   -> 45 Hz-es szobamódus korrekciója: %.1f Hz, gain: %.2f dB, Q: %.2f\n",
                        b.freq, b.gainDb, b.q);
            break;
        }
    }
    CHECK(found45Mode, "45 Hz-es szobamódus vágása sikeres");

    // Ellenőrzés 2: A 120 Hz-es kioltást NEM kompenzálja túl: maxBoostDb (+3 dB) betartva!
    for (const auto& b : eqRes.bands) {
        CHECK(b.gainDb <= 3.01f, "Maximális emelési korlát betartva (mért sáv gain: %.2f dB > 3.0 dB)", b.gainDb);
    }

    CHECK(eqRes.rmsErrorDb < 1.5f, "Auto-EQ RMS reziduális hiba kicsi: %.2f dB (tűrés: 1.5 dB)", eqRes.rmsErrorDb);
    std::printf("   -> Auto-EQ sávok száma: %zu, RMS maradék hiba: %.2f dB\n", eqRes.bands.size(), eqRes.rmsErrorDb);

    // Többpozíciós energiamérés (Power Averaging) tesztje
    std::vector<std::vector<FreqPoint>> posList = { rawResponse, smoothed };
    auto avgPos = powerAveragePositions(posList);
    CHECK(avgPos.size() == rawResponse.size(), "Többpontos átlagolás pontszáma egyezik");
    std::printf("   -> Többpontos energiamérések átlagolása: sikeres (%zu pozíció)\n", posList.size());
}

static void testMicCalibration() {
    std::printf("[4] Mikrofon-kalibráció beolvasása és motoroldali levonása (10-mikrofon-kalibracio.txt)\n");
#ifdef AQ_TEST_DATA_DIR
    const std::string calPath = std::string(AQ_TEST_DATA_DIR) + "/import/10-mikrofon-kalibracio.txt";
#else
    const std::string calPath = "tests/data/import/10-mikrofon-kalibracio.txt";
#endif
    std::ifstream in(calPath);
    CHECK(in.is_open(), "Kalibrációs mintafájl megnyitható: %s", calPath.c_str());

    MeasureConfig cfg;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '"' || line[0] == '#' || line[0] == '*') continue;
        std::istringstream iss(line);
        float f = 0.0f, db = 0.0f;
        if (iss >> f >> db) {
            cfg.micCal.push_back({ f, db });
        }
    }

    CHECK(cfg.micCal.size() == 30, "Kalibrációs pontok száma 30 (31 sorból az idézőjeles fejléc kimaradt; kapott: %zu)", cfg.micCal.size());
    CHECK(std::abs(cfg.micCal.front().freq - 20.0f) < 0.01f && std::abs(cfg.micCal.front().magDb - 0.80f) < 0.01f,
          "Első pont: 20.0 Hz +0.80 dB");
    CHECK(std::abs(cfg.micCal.back().freq - 16255.0f) < 0.1f && std::abs(cfg.micCal.back().magDb - (-1.20f)) < 0.01f,
          "Utolsó pont: 16255.0 Hz -1.20 dB");

    // Ismert mért görbe: 0 dB sík átvitel + egy tesztpont köztes frekvencián logaritmikus interpolációra
    // 25.2 Hz (+0.80 dB) és 31.7 Hz (0.00 dB) mértani közepe: sqrt(25.2 * 31.7) ≈ 28.2638 Hz -> elvárt cal = +0.40 dB -> levonva: -0.40 dB
    const float midFreq = std::sqrt(25.2f * 31.7f);
    std::vector<FreqPoint> measured = {
        { 20.0f, 0.0f },
        { 25.2f, 0.0f },
        { midFreq, 0.0f },
        { 1000.0f, 5.0f },
        { 16255.0f, 0.0f },
        { 20000.0f, 0.0f }
    };

    applyMicCalibration(measured, cfg.micCal);

    CHECK(std::abs(measured[0].magDb - (-0.80f)) < 0.01f, "20 Hz: +0.80 dB levonva -> -0.80 dB (kapott: %.3f)", measured[0].magDb);
    CHECK(std::abs(measured[1].magDb - (-0.80f)) < 0.01f, "25.2 Hz: +0.80 dB levonva -> -0.80 dB (kapott: %.3f)", measured[1].magDb);
    CHECK(std::abs(measured[2].magDb - (-0.40f)) < 0.02f, "Log-interpolált %.1f Hz: +0.40 dB levonva -> -0.40 dB (kapott: %.3f)", midFreq, measured[2].magDb);
    CHECK(std::abs(measured[3].magDb - 5.0f) < 0.01f, "1000 Hz (0.00 dB cal): változatlan 5.00 dB (kapott: %.3f)", measured[3].magDb);
    CHECK(std::abs(measured[4].magDb - 1.20f) < 0.01f, "16255 Hz: -1.20 dB levonva -> +1.20 dB (kapott: %.3f)", measured[4].magDb);
    CHECK(std::abs(measured[5].magDb - 1.20f) < 0.01f, "20000 Hz (utolsó kalibrációs pont tartása): +1.20 dB (kapott: %.3f)", measured[5].magDb);
    std::printf("   -> Motoroldali kalibráció-levonás ellenőrizve (20 Hz: %.2f dB, 16.25 kHz: %+.2f dB)\n",
                measured[0].magDb, measured[4].magDb);
}

int main() {
    std::printf("==========================================\n");
    std::printf("Aqulify Szobamérő Mag Tesztek (include/room/)\n");
    std::printf("==========================================\n");

    testSweepAndInverse();
    testSyntheticRoomDeconvolution();
    testSmoothingAndAutoEq();
    testMicCalibration();

    std::printf("\n==========================================\n");
    std::printf("SIKER: Minden szobamérő mag teszt sikeres!\n");
    std::printf("==========================================\n");
    return 0;
}
