/**
 * @file test_room.cpp
 * @brief Saját szobamérő és korrekciós mag tesztjei szintetikus szobával.
 */

#include "room/room_correction.hpp"
#include "room/measurement_manager.hpp"
#include "room/measurement_session.hpp"
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

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

    // A sweep és az inverz valódi (lineáris) konvolúciója: csúcs az N−1. mintán, értéke 1, és az
    // átvitele a sweep tartományában sík (nem dől) – a korábbi teszt csak skaláris szorzatot nézett.
    const size_t N = sweep.size();
    size_t n = 1;
    while (n < 2 * N) n <<= 1;
    std::vector<std::complex<double>> A(n), B(n);
    for (size_t i = 0; i < N; ++i) { A[i] = sweep[i]; B[i] = inv[i]; }
    fftRadix2(A, false);
    fftRadix2(B, false);
    std::vector<std::complex<double>> H(n);
    for (size_t i = 0; i < n; ++i) H[i] = A[i] * B[i];
    // a frekvenciaátvitel (|S·I|) 100 Hz, 1 kHz és 10 kHz-en (a sávszélektől távol)
    auto magAt = [&](double f) { return 20.0 * std::log10(std::abs(H[static_cast<size_t>(std::round(f * n / sr))])); };
    const double m100 = magAt(100.0), m1k = magAt(1000.0), m10k = magAt(10000.0);
    std::printf("   -> átvitel: 100 Hz %+.2f, 1 kHz %+.2f, 10 kHz %+.2f dB\n", m100, m1k, m10k);
    CHECK(std::fabs(m100 - m1k) < 0.5 && std::fabs(m10k - m1k) < 0.5, "a sweep · inverz átvitele sík (±0,5 dB)");
    fftRadix2(H, true);
    size_t pk = 0;
    for (size_t i = 0; i < n; ++i) if (std::abs(H[i].real()) > std::abs(H[pk].real())) pk = i;
    CHECK(pk == N - 1, "a csúcs az N−1. mintán (kapott: %zu, várt: %zu)", pk, N - 1);
    const double dotPeak = H[pk].real();   // az inverz a sweep amplitúdóját is kiegyenlíti
    CHECK(std::abs(dotPeak - 1.0) < 0.05, "Dirac csúcs ~1.0 (mért: %.4f)", dotPeak);
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

/** A simított görbe értéke a legközelebbi ponton. */
static float respAt(const std::vector<FreqPoint>& r, float f) {
    float best = 1e9f, v = 0.0f;
    for (const auto& p : r) {
        const float d = std::fabs(std::log(p.freq / f));
        if (d < best) { best = d; v = p.magDb; }
    }
    return v;
}

static void testProcessMeasurementEndToEnd() {
    std::printf("[5] Teljes feldolgozás (processMeasurement): sweep -> szintetikus szoba -> görbe és javaslat\n");
    const uint32_t sr = 48000;
    MeasureConfig cfg;
    cfg.levelDb = -20.0f;
    cfg.durationSec = 3.0f;
    cfg.smoothingFraction = 12;
    cfg.targetType = "flat";
    const float amp = std::pow(10.0f, cfg.levelDb / 20.0f);
    auto sweep = generateLogSweep(cfg.startFreq, cfg.endFreq, cfg.durationSec, sr, amp);
    // a felvétel elején 0,6 s csend (mint a valódi mérésnél), utána a szoba válasza
    std::vector<float> lead(static_cast<size_t>(0.6 * sr), 0.0f);
    std::vector<float> in = lead;
    in.insert(in.end(), sweep.begin(), sweep.end());
    SyntheticRoom room;              // 45 Hz +6 dB, 120 Hz −5 dB, 8 kHz-től −3 dB polc, 15 ms
    room.noiseLevel = 0.0002f;
    auto rec = room.simulateResponse(in, sr);
    cfg.noiseRms = 0.0002f / std::sqrt(3.0f);

    const auto t0 = std::chrono::steady_clock::now();
    MeasurementManager mgr;
    MeasureResult r = mgr.processMeasurement(cfg, sr, rec);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r.success, "a feldolgozás sikeres (%s: %s)", r.errorCode.c_str(), r.errorMessage.c_str());
    std::printf("   -> feldolgozási idő: %.2f s; SNR: %.1f dB; érkezés: %.2f ms\n", secs, r.snrDb, r.measuredDelayMs);
    CHECK(secs < 5.0, "a feldolgozás gyors (FFT): %.2f s", secs);
    CHECK(std::fabs(r.measuredDelayMs - 615.0f) < 0.1f, "érkezés 600 + 15 ms (mért: %.3f ms)", r.measuredDelayMs);
    CHECK(r.snrDb > 40.0f, "jel-zaj viszony > 40 dB (%.1f)", r.snrDb);
    const float v45 = respAt(r.smoothedResponse, 45.0f), v120 = respAt(r.smoothedResponse, 120.0f);
    const float v1k = respAt(r.smoothedResponse, 1000.0f), v12k = respAt(r.smoothedResponse, 12000.0f);
    const float v500 = respAt(r.smoothedResponse, 500.0f);
    std::printf("   -> görbe: 45 Hz %+.2f, 120 Hz %+.2f, 500 Hz %+.2f, 1 kHz %+.2f, 12 kHz %+.2f dB\n", v45, v120, v500, v1k, v12k);
    CHECK(std::fabs(v1k) < 0.5f && std::fabs(v500) < 0.5f, "a középtartomány sík (1 kHz %+.2f, 500 Hz %+.2f dB)", v1k, v500);
    CHECK(v45 > 4.0f && v45 < 7.0f, "45 Hz-es módus látszik: %+.2f dB (szoba: +6, simítva)", v45);
    CHECK(v120 < -3.0f && v120 > -6.0f, "120 Hz-es kioltás látszik: %+.2f dB (szoba: −5, simítva)", v120);
    CHECK(v12k < -2.3f && v12k > -3.7f, "8 kHz-es polc: 12 kHz-en %+.2f dB (szoba: −3)", v12k);
    bool cut45 = false;
    for (const auto& b : r.correctionBands) if (std::fabs(b.freq - 45.0f) < 10.0f && b.gainDb < -3.0f) cut45 = true;
    CHECK(cut45, "a javaslat a 45 Hz-es módust vágja");
    CHECK(r.humanSummaryHu.find("Javaslat") != std::string::npos, "az összefoglaló javaslatként fogalmaz");
    CHECK(r.humanSummaryHu.find("ms") == std::string::npos, "az összefoglaló nem állít késleltetés-korrekciót");

    // Fordított polaritás és zajos szoba
    SyntheticRoom inv = room;
    inv.invertPolarity = true;
    MeasureResult ri = mgr.processMeasurement(cfg, sr, inv.simulateResponse(in, sr));
    CHECK(ri.success && ri.invertedPolarity, "fordított polaritás felismerve");
    SyntheticRoom noisy = room;
    noisy.noiseLevel = 0.2f;
    MeasureConfig cn = cfg;
    cn.noiseRms = 0.2f / std::sqrt(3.0f);
    MeasureResult rn = mgr.processMeasurement(cn, sr, noisy.simulateResponse(in, sr));
    CHECK(!rn.success && rn.errorCode == "NOISY_ROOM", "zajos szoba: érthető hiba (%s)", rn.errorCode.c_str());
}

// ---- A mérésvezénylő (MeasurementSession) álmikrofonnal és állejátszóval ----

struct FakeWorld {
    std::mutex mu;
    std::chrono::steady_clock::time_point t0;
    bool recording = false;
    double playAtFrames = -1.0;
    std::vector<float> playedL, playedR;
    SyntheticRoom roomL, roomR;
    int bypassOn = 0, bypassOff = 0;
    bool lastBypass = false;
    std::vector<std::string> posts;
    bool failMic = false;

    // Valósághű álmikrofon hibamódok (kapcsolhatók)
    bool startClick = false;                     // 0 dBFS-es tüske az első 50 ms-ban
    float dcOffset = 0.0f;                       // egyenfeszültség-eltolás
    float usbLatencyMs = 10.0f;                  // USB-késés (alap: 10 ms)
    double clockDriftPpm = 0.0;                  // órajel-eltérés ppm-ben (pl. +100)
    bool enableAgc = false;                      // automatikus erősítés (0,5 s időállandó)
    float noiseGateThresh = 0.0f;                // zajkapu küszöb (0 = ki)
    std::vector<AutoEqBand> micResponse;         // a mikrofon saját frekvenciamenete
};

static bool readStereoFloatWav(const std::string& path, std::vector<float>& L, std::vector<float>& R) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 44) return false;
    const float* d = reinterpret_cast<const float*>(b.data() + 44);
    const size_t n = (b.size() - 44) / 8;
    L.resize(n); R.resize(n);
    for (size_t i = 0; i < n; ++i) { L[i] = d[2 * i]; R[i] = d[2 * i + 1]; }
    return true;
}

class FakeMic : public MicCapture {
public:
    explicit FakeMic(FakeWorld& w) : w_(w) {}
    std::vector<InputDevice> listInputs(bool) override { return { { "mic1", "Teszt mikrofon", true, false } }; }
    bool start(const std::string&, uint32_t, double, std::string& error) override {
        if (w_.failMic) { error = "permission"; return false; }
        std::lock_guard<std::mutex> lk(w_.mu);
        w_.t0 = std::chrono::steady_clock::now();
        w_.recording = true;
        w_.playAtFrames = -1.0;
        return true;
    }
    std::vector<float> stop() override {
        std::lock_guard<std::mutex> lk(w_.mu);
        w_.recording = false;
        const size_t n = framesNow();
        std::vector<float> out(n, 0.0f);
        if (w_.playAtFrames >= 0.0) {
            // a szoba válasza a lejátszás kezdetétől, usbLatencyMs rendszerkéséssel; bal + jobb összeadódik
            auto yl = w_.roomL.simulateResponse(w_.playedL, 48000);
            auto yr = w_.roomR.simulateResponse(w_.playedR, 48000);
            const size_t lat = static_cast<size_t>(std::round(w_.usbLatencyMs * 48.0));
            const size_t off = static_cast<size_t>(w_.playAtFrames) + lat;
            for (size_t i = 0; i + off < n && i < yl.size(); ++i) out[i + off] += yl[i] + (i < yr.size() ? yr[i] : 0.0f);
        }
        // Ismert mikrofon-frekvenciamenet (ha meg van adva)
        if (!w_.micResponse.empty()) {
            SyntheticRoom micFilt;
            micFilt.delayMs = 0.0f;
            micFilt.noiseLevel = 0.0f;
            micFilt.acousticFilters = w_.micResponse;
            auto filtered = micFilt.simulateResponse(out, 48000);
            if (filtered.size() >= n) out.assign(filtered.begin(), filtered.begin() + static_cast<std::ptrdiff_t>(n));
        }
        // Órajel-eltérés (4 pontos köbös Catmull-Rom újramintavételezéssel, hogy ne torzítsa a magasakat)
        if (std::fabs(w_.clockDriftPpm) > 0.01 && out.size() > 4) {
            const double step = 1.0 + w_.clockDriftPpm * 1e-6;
            std::vector<float> drifted(out.size(), 0.0f);
            for (size_t i = 0; i < out.size(); ++i) {
                const double pos = i * step;
                const size_t idx = static_cast<size_t>(pos);
                if (idx >= 1 && idx + 2 < out.size()) {
                    const double t = pos - idx;
                    const double y0 = out[idx - 1], y1 = out[idx], y2 = out[idx + 1], y3 = out[idx + 2];
                    const double c0 = y1;
                    const double c1 = 0.5 * (y2 - y0);
                    const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
                    const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
                    drifted[i] = static_cast<float>(((c3 * t + c2) * t + c1) * t + c0);
                }
            }
            out.swap(drifted);
        }
        // Alapzaj
        uint32_t seed = 777;
        for (auto& v : out) { seed = seed * 1664525u + 1013904223u; v += (static_cast<float>(seed & 0xFFFF) / 32768.0f - 1.0f) * 0.0001f; }
        // Automatikus erősítés (AGC): 0,5 s időállandóval 0,15 RMS célszintre húzza a jelet
        if (w_.enableAgc) {
            double env = 0.05;
            const double alpha = 1.0 / (0.5 * 48000.0);
            for (auto& v : out) {
                env += alpha * (std::fabs(v) - env);
                const double gain = std::clamp(0.15 / std::max(0.005, env), 0.2, 6.0);
                v = static_cast<float>(std::clamp(v * gain, -0.95, 0.95));
            }
        }
        // Zajkapu
        if (w_.noiseGateThresh > 0.0f) {
            for (auto& v : out) if (std::fabs(v) < w_.noiseGateThresh) v = 0.0f;
        }
        // Induláskori kattanás (0 dBFS tüske 20 ms-nál, az első 50 ms-ban)
        if (w_.startClick && out.size() > 1000) {
            for (size_t i = 960; i < 968 && i < out.size(); ++i) out[i] = 1.0f;
        }
        // DC-eltolás
        if (w_.dcOffset != 0.0f) {
            for (auto& v : out) v += w_.dcOffset;
        }
        return out;
    }
    bool running() const override { return w_.recording; }
    float takePeak() override {
        std::lock_guard<std::mutex> lk(w_.mu);
        if (w_.playAtFrames < 0.0 || w_.playedL.empty()) return 0.001f;
        const size_t nowF = framesNow();
        const size_t lat = static_cast<size_t>(std::round(w_.usbLatencyMs * 48.0));
        const size_t playStart = static_cast<size_t>(w_.playAtFrames) + lat;
        if (nowF <= playStart) return 0.001f;
        const size_t pos = nowF - playStart;
        const size_t win = 4800; // 100 ms ablak
        const size_t lo = pos > win ? pos - win : 0;
        const size_t hi = std::min(pos, w_.playedL.size());
        float pk = 0.0f;
        for (size_t i = lo; i < hi; ++i) {
            const float s = w_.playedL[i] + (i < w_.playedR.size() ? w_.playedR[i] : 0.0f);
            pk = std::max(pk, std::fabs(s));
        }
        if (pk < 1e-5f) return 0.001f;
        if (w_.enableAgc) return 0.18f; // AGC állandó szintre szabályoz (nem követi a -6 dB lépést)
        return pk;
    }
    size_t recordedFrames() const override { return framesNow(); }
    size_t framesNow() const {
        return static_cast<size_t>(std::chrono::duration<double>(std::chrono::steady_clock::now() - w_.t0).count() * 48000.0);
    }
private:
    FakeWorld& w_;
};

static SessionDeps fakeDeps(FakeWorld& w) {
    SessionDeps d;
    d.bypassChain = [&w](bool on) { std::lock_guard<std::mutex> lk(w.mu); (on ? w.bypassOn : w.bypassOff)++; w.lastBypass = on; };
    d.playFile = [&w](const std::string& path, std::string& err) {
        std::vector<float> L, R;
        if (!readStereoFloatWav(path, L, R)) { err = "olvasási hiba"; return false; }
        std::lock_guard<std::mutex> lk(w.mu);
        w.playedL = std::move(L);
        w.playedR = std::move(R);
        w.playAtFrames = std::chrono::duration<double>(std::chrono::steady_clock::now() - w.t0).count() * 48000.0;
        return true;
    };
    d.stopPlayback = [] {};
    d.pauseMedia = [] {};
    d.makeMic = [&w]() -> std::unique_ptr<MicCapture> { return std::make_unique<FakeMic>(w); };
    d.post = [&w](const std::string& s) { std::lock_guard<std::mutex> lk(w.mu); w.posts.push_back(s); };
    return d;
}

static std::string waitFor(FakeWorld& w, MeasurementSession& s, const char* type, double maxSec) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(maxSec * 1000));
    const std::string key = std::string("\"type\":\"") + type + "\"";
    while (std::chrono::steady_clock::now() < until) {
        {
            std::lock_guard<std::mutex> lk(w.mu);
            for (const auto& p : w.posts) if (p.find(key) != std::string::npos) return p;
        }
        if (!s.busy()) {
            std::lock_guard<std::mutex> lk(w.mu);
            for (const auto& p : w.posts) if (p.find(key) != std::string::npos) return p;
            return std::string();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::string();
}

static double jsonNum(const std::string& j, const std::string& key) {
    const auto p = j.find("\"" + key + "\":");
    return p == std::string::npos ? NAN : std::atof(j.c_str() + p + key.size() + 3);
}

static std::vector<double> jsonArr(const std::string& j, const std::string& key) {
    std::vector<double> v;
    auto p = j.find("\"" + key + "\":[");
    if (p == std::string::npos) return v;
    p += key.size() + 4;
    while (p < j.size() && j[p] != ']') {
        char* e = nullptr;
        v.push_back(std::strtod(j.c_str() + p, &e));
        if (e == j.c_str() + p) break;
        p = static_cast<size_t>(e - j.c_str());
        if (p < j.size() && j[p] == ',') ++p;
    }
    return v;
}

static void testMeasurementSession() {
    std::printf("[6] Mérésvezénylő: lejátszás + felvétel + lánc kikapcsolása / visszaállítása (álmikrofonnal)\n");
    FakeWorld w;
    w.roomL.delayMs = 3.0f;
    w.roomR.delayMs = 4.0f;           // a jobb hangszóró 1 ms-mal (≈ 34 cm) távolabb
    w.roomR.invertPolarity = true;    // fordítva bekötött jobb hangszóró
    w.roomL.noiseLevel = w.roomR.noiseLevel = 0.0f;
    MeasurementSession s(fakeDeps(w));

    const std::string list = s.listInputsJson();
    CHECK(list.find("\"id\":\"mic1\"") != std::string::npos && list.find("\"available\":true") != std::string::npos,
          "eszközlista: %s", list.c_str());

    MeasureConfig cfg;
    cfg.durationSec = 1.0f;
    cfg.channel = "left";
    cfg.targetType = "flat";
    std::string err;
    CHECK(s.startMeasurement(cfg, "mic1", err), "a bal csatorna mérése elindul");
    std::string err2;
    CHECK(!s.startMeasurement(cfg, "mic1", err2) && err2 == "BUSY", "második indítás futás közben: BUSY");
    std::string res = waitFor(w, s, "measurement_result", 15.0);
    CHECK(!res.empty(), "bal: eredmény érkezett");
    {
        std::lock_guard<std::mutex> lk(w.mu);
        CHECK(w.bypassOn == 1 && w.bypassOff == 1 && !w.lastBypass, "a lánc mérési módba került, majd visszaállt (%d/%d)", w.bypassOn, w.bypassOff);
        CHECK(!w.playedL.empty() && !w.playedR.empty(), "a mérőjel a lejátszón szólt");
        float pkR = 0.0f;
        for (float v : w.playedR) pkR = std::max(pkR, std::fabs(v));
        CHECK(pkR == 0.0f, "bal mérésnél a jobb csatorna néma");
        bool progress = false;
        for (const auto& p : w.posts) if (p.find("measurement_progress") != std::string::npos && p.find("level_db") != std::string::npos) progress = true;
        CHECK(progress, "haladás + mikrofonszint üzenetek");
        w.posts.clear();
    }
    CHECK(jsonNum(res, "delay_ms") == 0.0, "egyetlen mérés nem ad késleltetés-korrekciót");
    CHECK(res.find("\"freqs\":[") != std::string::npos && res.find("\"bands\":[") != std::string::npos, "eredmény: görbe és sávok");

    CHECK(res.find("lr_delay_ms") == std::string::npos, "egy csatorna mérése nem ad bal–jobb összevetést");

    // Mindkét csatorna egy felvételben: bal, szünet, jobb – a különbség mintapontos
    cfg.channel = "both";
    CHECK(s.startMeasurement(cfg, "mic1", err), "a két csatorna mérése elindul");
    res = waitFor(w, s, "measurement_result", 20.0);
    CHECK(!res.empty(), "mindkét csatorna: eredmény érkezett");
    {
        std::lock_guard<std::mutex> lk(w.mu);
        size_t firstL = 0, firstR = 0;
        while (firstL < w.playedL.size() && w.playedL[firstL] == 0.0f) ++firstL;
        while (firstR < w.playedR.size() && w.playedR[firstR] == 0.0f) ++firstR;
        CHECK(firstR > firstL + 48000, "a jobb sweep a bal után szól (%zu / %zu minta)", firstL, firstR);
    }
    const double lr = jsonNum(res, "lr_delay_ms");
    std::printf("   -> bal–jobb különbség: %.3f ms (szoba: +1,000 ms)\n", lr);
    CHECK(std::fabs(lr - 1.0) < 0.05, "a bal–jobb késleltetés-különbség helyes: %.3f ms", lr);
    CHECK(res.find("\"lr_polarity_differs\":true") != std::string::npos, "az eltérő polaritás felismerve");
    CHECK(res.find("polaritása eltér") != std::string::npos, "az összefoglaló figyelmeztet a bekötésre");
    CHECK(res.find("measured_left_dbs") != std::string::npos && res.find("measured_right_dbs") != std::string::npos,
          "mindkét csatorna görbéje az eredményben");

    // Leállítás mérés közben: nincs eredmény, a lánc visszaáll
    { std::lock_guard<std::mutex> lk(w.mu); w.posts.clear(); }
    cfg.durationSec = 3.0f;
    CHECK(s.startMeasurement(cfg, "mic1", err), "harmadik mérés elindul");
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    s.stop();
    const std::string st = waitFor(w, s, "measurement_stopped", 5.0);
    CHECK(!st.empty(), "leállítva: measurement_stopped");
    while (s.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
        std::lock_guard<std::mutex> lk(w.mu);
        bool result = false;
        for (const auto& p : w.posts) if (p.find("measurement_result") != std::string::npos) result = true;
        CHECK(!result, "leállítás után nincs eredmény");
        CHECK(w.bypassOn == w.bypassOff && !w.lastBypass, "leállítás után a lánc visszaállt (%d/%d)", w.bypassOn, w.bypassOff);
        w.posts.clear();
    }

    // Szintpróba: mic_level üzenetek, leállítható
    CHECK(s.startLevelCheck("mic1", -20.0f, "both", err), "a szintpróba elindul");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    s.stop();
    while (s.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
        std::lock_guard<std::mutex> lk(w.mu);
        int levels = 0;
        for (const auto& p : w.posts) if (p.find("\"type\":\"mic_level\"") != std::string::npos) ++levels;
        CHECK(levels >= 2, "szintpróba: %d mic_level üzenet", levels);
        CHECK(w.bypassOn == w.bypassOff && !w.lastBypass, "szintpróba után a lánc visszaállt");
        w.posts.clear();
    }

    // Mikrofon-engedély hiánya: érthető hiba, a lánc érintetlen
    w.failMic = true;
    const int onBefore = w.bypassOn;
    CHECK(s.startMeasurement(cfg, "mic1", err), "indítás engedély nélkül");
    const std::string e = waitFor(w, s, "measurement_error", 5.0);
    CHECK(e.find("MIC_permission") != std::string::npos, "hiba: MIC_permission (%s)", e.c_str());
    CHECK(w.bypassOn == onBefore, "mikrofonhiba esetén a lánc nem változott");
}

static void testRealisticMicClickDcAndCal() {
    std::printf("[7] Valósághű álmikrofon: induláskori kattanás, DC-eltolás, 150 ms USB-késés, +100 ppm drift, mikrofon-kalibráció\n");
    auto runOnce = [](FakeWorld& w, MeasureConfig cfg) -> std::string {
        w.posts.clear();
        MeasurementSession s(fakeDeps(w));
        std::string err;
        CHECK(s.startMeasurement(cfg, "mic1", err), "mérés elindul (%s)", err.c_str());
        std::string res = waitFor(w, s, "measurement_result", 15.0);
        if (res.empty()) {
            std::lock_guard<std::mutex> lk(w.mu);
            for (const auto& p : w.posts) {
                if (p.find("measurement_error") != std::string::npos) {
                    std::printf("   -> váratlan hiba: %s\n", p.c_str());
                }
            }
        }
        return res;
    };

    MeasureConfig baseCfg;
    baseCfg.durationSec = 1.0f;
    baseCfg.channel = "left";
    baseCfg.targetType = "flat";
    baseCfg.smoothingFraction = 12;

    // 7a) Referencia (sík szoba, tiszta mikrofon)
    FakeWorld wRef;
    wRef.roomL.acousticFilters.clear();
    wRef.roomL.noiseLevel = 0.0f;
    const std::string resRef = runOnce(wRef, baseCfg);
    CHECK(!resRef.empty(), "7a referencia sikeres");
    const auto fRef = jsonArr(resRef, "freqs");
    const auto mRef = jsonArr(resRef, "measured_dbs");
    CHECK(fRef.size() > 100 && fRef.size() == mRef.size(), "7a görbe érvényes (%zu)", fRef.size());

    auto maxDiff = [&](const std::vector<double>& m, double fLo = 30.0, double fHi = 16000.0) {
        double md = 0.0;
        for (size_t i = 0; i < fRef.size() && i < m.size(); ++i) {
            if (fRef[i] >= fLo && fRef[i] <= fHi) md = std::max(md, std::fabs(m[i] - mRef[i]));
        }
        return md;
    };

    // 7b) Induláskori 0 dBFS kattanás (első 50 ms-ban): nincs téves MIC_CLIPPING / NOISY_ROOM, görbe ±0,3 dB
    FakeWorld wClick = {};
    wClick.roomL.acousticFilters.clear();
    wClick.roomL.noiseLevel = 0.0f;
    wClick.startClick = true;
    const std::string resClick = runOnce(wClick, baseCfg);
    CHECK(!resClick.empty(), "7b kattanás mellett nincs téves hiba");
    const double dClick = maxDiff(jsonArr(resClick, "measured_dbs"));
    std::printf("   -> kattanás eltérés a referenciától: %.3f dB\n", dClick);
    CHECK(dClick < 0.3, "7b kattanás mellett a görbe változatlan (±0,3 dB, mért: %.3f)", dClick);

    // 7c) DC-eltolás (+0,15): nincs téves NOISY_ROOM / MIC_CLIPPING, görbe ±0,3 dB
    FakeWorld wDc = {};
    wDc.roomL.acousticFilters.clear();
    wDc.roomL.noiseLevel = 0.0f;
    wDc.dcOffset = 0.15f;
    const std::string resDc = runOnce(wDc, baseCfg);
    CHECK(!resDc.empty(), "7c DC-eltolás mellett nincs téves hiba");
    const double dDc = maxDiff(jsonArr(resDc, "measured_dbs"));
    std::printf("   -> DC-eltolás eltérés a referenciától: %.3f dB\n", dDc);
    CHECK(dDc < 0.3, "7c DC-eltolás mellett a görbe változatlan (±0,3 dB, mért: %.3f)", dDc);

    // 7d) 150 ms USB-késés + +100 ppm órajel-eltérés
    FakeWorld wUsb = {};
    wUsb.roomL.acousticFilters.clear();
    wUsb.roomL.noiseLevel = 0.0f;
    wUsb.usbLatencyMs = 150.0f;
    wUsb.clockDriftPpm = 100.0;
    const std::string resUsb = runOnce(wUsb, baseCfg);
    CHECK(!resUsb.empty(), "7d 150 ms USB-késés + 100 ppm drift sikeres");
    const auto mUsb = jsonArr(resUsb, "measured_dbs");
    const double dUsbMid = maxDiff(mUsb, 40.0, 5000.0);
    const double dUsbHi = maxDiff(mUsb, 40.0, 12000.0);
    std::printf("   -> 150 ms USB-késés + 100 ppm drift eltérés: 40-5k %.3f dB, 40-12k %.3f dB\n", dUsbMid, dUsbHi);
    CHECK(dUsbMid < 0.4 && dUsbHi < 1.3, "7d USB-késés + drift görbe stabil (40-5k: %.3f, 40-12k: %.3f)", dUsbMid, dUsbHi);

    // 7e) Ismert mikrofon-frekvenciamenet (+3 dB 10 kHz-en, -2 dB 30 Hz-en): kalibráció nélkül látszik, fájllal sík (±0,5 dB)
    FakeWorld wMic = {};
    wMic.roomL.acousticFilters.clear();
    wMic.roomL.noiseLevel = 0.0f;
    wMic.micResponse = {
        { 1, 10000.0f, +3.0f, 1.5f },
        { 1, 30.0f, -2.0f, 1.5f }
    };
    const std::string resNoCal = runOnce(wMic, baseCfg);
    CHECK(!resNoCal.empty(), "7e kalibrálatlan mikrofon mérése sikeres");
    const auto mNoCal = jsonArr(resNoCal, "measured_dbs");
    auto atF = [&](const std::vector<double>& m, double targetF) {
        double best = 1e9, v = 0.0;
        for (size_t i = 0; i < fRef.size() && i < m.size(); ++i) {
            double d = std::fabs(std::log(fRef[i] / targetF));
            if (d < best) { best = d; v = m[i]; }
        }
        return v;
    };
    const double uncal10k = atF(mNoCal, 10000.0);
    const double uncal30 = atF(mNoCal, 30.0);
    std::printf("   -> mikrofon kalibráció nélkül: 30 Hz %+.2f dB, 10 kHz %+.2f dB\n", uncal30, uncal10k);
    CHECK(uncal10k > 2.0 && uncal30 < -1.2, "7e kalibráció nélkül látszik a mikrofon saját menete");

    // Kalibrációs fájl előállítása az ismert szűrők analitikus menete alapján
    MeasureConfig calCfg = baseCfg;
    for (int i = 0; i < 60; ++i) {
        const float f = static_cast<float>(20.0 * std::pow(20000.0 / 20.0, static_cast<double>(i) / 59.0));
        float db = 0.0f;
        for (const auto& b : wMic.micResponse) db += evalFilterDb(b, f);
        calCfg.micCal.push_back({ f, db, 0.0f });
    }
    const std::string resCal = runOnce(wMic, calCfg);
    CHECK(!resCal.empty(), "7e kalibrált mikrofon mérése sikeres");
    const auto mCal = jsonArr(resCal, "measured_dbs");
    const double cal10k = atF(mCal, 10000.0);
    const double cal30 = atF(mCal, 30.0);
    std::printf("   -> mikrofon kalibrációval: 30 Hz %+.2f dB, 10 kHz %+.2f dB\n", cal30, cal10k);
    CHECK(std::fabs(cal10k) < 0.5 && std::fabs(cal30) < 0.5, "7e kalibrációval az eredmény sík (±0,5 dB)");

    // 0°/90° keverés dokumentálása: ha 0°-os (magasban +3 dB eltérésű) fájlt adunk meg a fenti mikrofonhoz
    MeasureConfig wrongAngleCfg = baseCfg;
    wrongAngleCfg.micCal = { {20.0f, 0.0f, 0.0f}, {1000.0f, 0.0f, 0.0f}, {10000.0f, 0.0f, 0.0f}, {20000.0f, 0.0f, 0.0f} };
    const std::string resWrong = runOnce(wMic, wrongAngleCfg);
    const double wrong10k = atF(jsonArr(resWrong, "measured_dbs"), 10000.0);
    std::printf("   -> 0°/90° keverés (eltérő kalibrációs fájl): 10 kHz %+.2f dB eltérés\n", wrong10k);
    CHECK(std::fabs(wrong10k - cal10k) > 2.0, "7e rossz szögű fájl mérhető eltérést ad");
}

static void testTargetCurveAndAutoEqLimitsAndVarSmooth() {
    std::printf("[8] Célgörbe modell, korrekciós határok, összegzett preamp és változó simítás\n");

    auto makeFlat = []() {
        std::vector<FreqPoint> pts;
        for (int i = 0; i < 240; ++i) {
            const float f = static_cast<float>(20.0 * std::pow(20000.0 / 20.0, static_cast<double>(i) / 239.0));
            pts.push_back({ f, 0.0f, 0.0f });
        }
        return pts;
    };

    // 8a) Egységes célgörbe (balanced: 20 Hz +4 dB, 160..1000 Hz 0 dB, 20 kHz -3 dB)
    {
        MeasurementManager mgr;
        MeasureConfig cfg;
        cfg.targetType = "room_balanced";
        MeasureResult res;
        res.smoothedResponse = makeFlat();
        mgr.finalize(cfg, res);
        const float t20 = respAt(res.targetCurve, 20.0f);
        const float t200 = respAt(res.targetCurve, 200.0f);
        const float t1k = respAt(res.targetCurve, 1000.0f);
        const float t20k = respAt(res.targetCurve, 20000.0f);
        std::printf("   -> balanced cél: 20 Hz %+.2f, 200 Hz %+.2f, 1 kHz %+.2f, 20 kHz %+.2f dB\n", t20, t200, t1k, t20k);
        CHECK(std::fabs(t20 - 4.0f) < 0.2f && std::fabs(t200) < 0.2f && std::fabs(t1k) < 0.2f && std::fabs(t20k - (-3.0f)) < 0.2f,
              "8a balanced célgörbe: +4 dB @ 20 Hz, 0 dB @ 160-1k Hz, -3 dB @ 20 kHz");
    }

    // 8b) Hangszóró alsó határa alatt (50 Hz alatti esés) tilos emelni
    {
        auto pts = makeFlat();
        for (auto& p : pts) {
            if (p.freq < 55.0f) {
                const double oct = std::log2(55.0 / p.freq);
                p.magDb = static_cast<float>(-18.0 * oct); // 40 Hz: -8.3 dB, 20 Hz: -26 dB
            }
        }
        auto eq = calculateAutoEq(pts, {}, 10, 3.0f, -24.0f);
        for (const auto& b : eq.bands) {
            if (b.gainDb > 0.1f) {
                CHECK(b.freq >= 45.0f, "8b nincs emelés a hangszóró alsó határa alatt (%.1f Hz, %+.2f dB)", b.freq, b.gainDb);
            }
        }
    }

    // 8c) Mély, keskeny kioltás (Q=6, -10 dB @ 120 Hz): nincs keskeny emelés (Q <= 3) és összefoglaló figyelmeztet
    {
        auto pts = makeFlat();
        for (auto& p : pts) {
            const double w = p.freq / 120.0;
            p.magDb -= static_cast<float>(10.0 / (1.0 + 36.0 * (w - 1.0 / w) * (w - 1.0 / w)));
        }
        MeasurementManager mgr;
        MeasureConfig cfg;
        cfg.targetType = "flat";
        MeasureResult res;
        res.smoothedResponse = pts;
        mgr.finalize(cfg, res);
        for (const auto& b : res.correctionBands) {
            if (b.gainDb > 0.1f) {
                CHECK(b.q <= 3.05f, "8c nincs keskeny emelés (%.1f Hz, %+.2f dB, Q=%.2f)", b.freq, b.gainDb, b.q);
            }
        }
        CHECK(res.humanSummaryHu.find("EQ nem javítja") != std::string::npos,
              "8c összefoglaló szól a kihagyott mély kioltásról (%s)", res.humanSummaryHu.c_str());
    }

    // 8d) Felső korrekciós határ (500 Hz fölött csak széles Q <= 1 és kis ±2.5 dB sáv, vagy semmi)
    {
        auto pts = makeFlat();
        for (auto& p : pts) {
            const double w2k = p.freq / 2000.0;
            p.magDb += static_cast<float>(7.0 / (1.0 + 25.0 * (w2k - 1.0 / w2k) * (w2k - 1.0 / w2k)));
        }
        auto eq = calculateAutoEq(pts, {}, 10, 3.0f, -24.0f);
        for (const auto& b : eq.bands) {
            if (b.freq > 500.0f) {
                CHECK(b.q <= 1.05f && std::fabs(b.gainDb) <= 2.6f,
                      "8d 500 Hz fölött csak széles, kis sáv (%.1f Hz, %+.2f dB, Q=%.2f)", b.freq, b.gainDb, b.q);
            }
        }
    }

    // 8e) Összegzett emelés <= maxBoostDb ÉS targetPreampDb <= -(max összegzett emelés)
    {
        auto pts = makeFlat();
        for (auto& p : pts) {
            if (p.freq >= 55.0f && p.freq <= 260.0f) p.magDb = -5.5f;
        }
        auto eq = calculateAutoEq(pts, {}, 10, 3.0f, -24.0f);
        float maxSumBoost = 0.0f;
        for (const auto& p : pts) {
            float sum = 0.0f;
            for (const auto& b : eq.bands) sum += evalFilterDb(b, p.freq);
            if (sum > maxSumBoost) maxSumBoost = sum;
        }
        std::printf("   -> összegzett max emelés: %+.2f dB, targetPreampDb: %+.2f dB\n", maxSumBoost, eq.targetPreampDb);
        CHECK(maxSumBoost <= 3.1f, "8e összegzett emelés <= 3.0 dB (mért: %.2f dB)", maxSumBoost);
        CHECK(eq.targetPreampDb <= -maxSumBoost + 0.05f, "8e targetPreampDb <= -maxBoost (%.2f vs %.2f)", eq.targetPreampDb, -maxSumBoost);
    }

    // 8f) Változó simítás (smoothingFraction == -1): 80 Hz-en finom (1/24), 5 kHz-en durva (1/3)
    {
        auto pts = makeFlat();
        for (auto& p : pts) {
            const double w80 = p.freq / 80.0;
            const double w5k = p.freq / 5000.0;
            p.magDb += static_cast<float>(8.0 / (1.0 + 64.0 * (w80 - 1.0 / w80) * (w80 - 1.0 / w80)));
            p.magDb += static_cast<float>(8.0 / (1.0 + 64.0 * (w5k - 1.0 / w5k) * (w5k - 1.0 / w5k)));
        }
        auto smVar = smoothSpectrum(pts, -1);
        auto sm24 = smoothSpectrum(pts, 24);
        auto sm3 = smoothSpectrum(pts, 3);
        const float v80 = respAt(smVar, 80.0f), v80_24 = respAt(sm24, 80.0f);
        const float v5k = respAt(smVar, 5000.0f), v5k_3 = respAt(sm3, 5000.0f);
        std::printf("   -> változó simítás: 80 Hz %+.2f (1/24: %+.2f), 5 kHz %+.2f (1/3: %+.2f) dB\n", v80, v80_24, v5k, v5k_3);
        CHECK(std::fabs(v80 - v80_24) < 0.05f && std::fabs(v5k - v5k_3) < 0.05f && v5k < v80 - 1.5f,
              "8f változó simítás: 80 Hz=1/24 (%.2f vs %.2f), 5 kHz=1/3 (%.2f vs %.2f)", v80, v80_24, v5k, v5k_3);
    }
}

static void testMultiPointPowerAvgAndRecompute() {
    std::printf("[9] Több mérési pont teljesítményátlaga (5 pont), pont törlése és measurement_recompute\n");
    FakeWorld w;
    w.roomL.noiseLevel = 0.0f;
    MeasurementSession s(fakeDeps(w));

    MeasureConfig cfg;
    cfg.durationSec = 0.5f;
    cfg.channel = "left";
    cfg.targetType = "flat";
    cfg.smoothingFraction = 12;

    auto atF = [](const std::vector<double>& freqs, const std::vector<double>& vals, double targetF) {
        double best = 1e9, v = 0.0;
        for (size_t i = 0; i < freqs.size() && i < vals.size(); ++i) {
            double d = std::fabs(std::log(freqs[i] / targetF));
            if (d < best) { best = d; v = vals[i]; }
        }
        return v;
    };

    // 5 pont: a 3. pontban mély (-18 dB) kioltás 80 Hz-en, a többiben sík (0 dB)
    // Teljesítményátlag 80 Hz-en: 10*log10((4*1 + 10^-1.8)/5) ≈ -0.95 dB (dB-átlag -3.6 dB lenne!)
    std::string lastRes;
    for (int pt = 0; pt < 5; ++pt) {
        { std::lock_guard<std::mutex> lk(w.mu); w.posts.clear(); }
        w.roomL.acousticFilters.clear();
        if (pt == 2) {
            w.roomL.acousticFilters = { { 1, 80.0f, -18.0f, 3.0f } };
        }
        std::string err;
        CHECK(s.startMeasurement(cfg, "mic1", err, /*append=*/pt > 0), "pont %d elindul (%s)", pt + 1, err.c_str());
        lastRes = waitFor(w, s, "measurement_result", 15.0);
        CHECK(!lastRes.empty(), "pont %d eredmény érkezett", pt + 1);
    }

    CHECK(s.pointCount() == 5, "5 pont van tárolva (kapott: %zu)", s.pointCount());
    CHECK(static_cast<int>(jsonNum(lastRes, "point_count")) == 5, "JSON point_count == 5");
    const auto f5 = jsonArr(lastRes, "freqs");
    const auto m5 = jsonArr(lastRes, "measured_dbs");
    const double v80_5 = atF(f5, m5, 80.0);
    std::printf("   -> 5 pontos teljesítményátlag 80 Hz-en: %+.2f dB (várt ~ -1.0 dB, NEM -3.6 dB)\n", v80_5);
    CHECK(v80_5 > -2.0 && v80_5 < -0.3, "teljesítményátlag: egy pont mély kioltása nem dominál (80 Hz: %.2f dB)", v80_5);

    // 3. pont (id=3) törlése -> marad 4 sík pont, 80 Hz ~ 0 dB
    { std::lock_guard<std::mutex> lk(w.mu); w.posts.clear(); }
    std::string derr;
    CHECK(s.deletePoint(3, cfg, derr), "3. pont törlése sikeres (%s)", derr.c_str());
    const std::string resAfterDel = waitFor(w, s, "measurement_result", 2.0);
    CHECK(!resAfterDel.empty(), "törlés után új eredmény érkezett");
    CHECK(static_cast<int>(jsonNum(resAfterDel, "point_count")) == 4, "törlés után point_count == 4");
    const double v80_4 = atF(jsonArr(resAfterDel, "freqs"), jsonArr(resAfterDel, "measured_dbs"), 80.0);
    std::printf("   -> rossz pont törlése után 80 Hz: %+.2f dB\n", v80_4);
    CHECK(std::fabs(v80_4) < 0.5, "rossz pont törlése után sík (80 Hz: %.2f dB)", v80_4);

    // measurement_recompute: célváltás flat -> egyéni (+5 dB mély, -2.5 dB magas, 180 Hz töréspont)
    { std::lock_guard<std::mutex> lk(w.mu); w.posts.clear(); }
    MeasureConfig cfg2 = cfg;
    cfg2.targetType = "room_balanced";
    cfg2.customTargetParams = true;
    cfg2.targetBassDb = 5.0f;
    cfg2.targetTrebleDb = -2.5f;
    cfg2.targetKneeHz = 180.0f;
    std::string rerr;
    CHECK(s.recompute(cfg2, rerr), "recompute sikeres (%s)", rerr.c_str());
    const std::string resRec = waitFor(w, s, "measurement_result", 2.0);
    CHECK(!resRec.empty(), "recompute eredmény érkezett");
    CHECK(resRec.find("\"recomputed\":true") != std::string::npos, "recomputed:true jelzés megvan");
    const auto fR = jsonArr(resRec, "freqs");
    const auto tR = jsonArr(resRec, "target_dbs");
    const double t20 = atF(fR, tR, 20.0), t20k = atF(fR, tR, 20000.0);
    const double recPreamp = jsonNum(resRec, "recommended_preamp_db");
    std::printf("   -> recompute cél: 20 Hz %+.2f dB, 20 kHz %+.2f dB; javasolt preamp: %+.2f dB\n", t20, t20k, recPreamp);
    CHECK(std::fabs(t20 - 5.0) < 0.2 && std::fabs(t20k - (-2.5)) < 0.2, "recompute új célgörbe helyes");
    CHECK(resRec.find("\"bands\":[{") != std::string::npos, "recompute új sávokat generált a mélyemeléshez");
    CHECK(recPreamp <= -1.0, "recompute pozitív emeléshez negatív recommended_preamp_db jár (%.2f)", recPreamp);
}

static void testLevelCheckAgcDetection() {
    std::printf("[10] Szintpróba: automatikus erősítés (AGC) észlelése a 6 dB-es lépcsővel\n");
    auto runCheck = [](bool agcOn) -> bool {
        FakeWorld w;
        w.enableAgc = agcOn;
        MeasurementSession s(fakeDeps(w));
        std::string err;
        CHECK(s.startLevelCheck("mic1", -20.0f, "left", err), "szintpróba indul (agc=%d)", agcOn ? 1 : 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(4800));
        s.stop();
        while (s.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard<std::mutex> lk(w.mu);
        for (const auto& p : w.posts) {
            if (p.find("\"agc_warning\":true") != std::string::npos) return true;
        }
        return false;
    };

    const bool warnOff = runCheck(false);
    const bool warnOn = runCheck(true);
    std::printf("   -> AGC ki: figyelmeztetés=%d (várt 0); AGC be: figyelmeztetés=%d (várt 1)\n", warnOff ? 1 : 0, warnOn ? 1 : 0);
    CHECK(!warnOff, "10a AGC nélkül nincs téves figyelmeztetés");
    CHECK(warnOn, "10b AGC mellett figyelmeztetés érkezik");
}

int main() {
    std::printf("==========================================\n");
    std::printf("Aqulify Szobamérő Mag Tesztek (include/room/)\n");
    std::printf("==========================================\n");

    testSweepAndInverse();
    testSyntheticRoomDeconvolution();
    testSmoothingAndAutoEq();
    testMicCalibration();
    testProcessMeasurementEndToEnd();
    testMeasurementSession();
    testRealisticMicClickDcAndCal();
    testTargetCurveAndAutoEqLimitsAndVarSmooth();
    testMultiPointPowerAvgAndRecompute();
    testLevelCheckAgcDetection();

    std::printf("\n==========================================\n");
    std::printf("SIKER: Minden szobamérő mag teszt sikeres!\n");
    std::printf("==========================================\n");
    return 0;
}
