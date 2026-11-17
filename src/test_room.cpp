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
            // a szoba válasza a lejátszás kezdetétől, 10 ms rendszerkéséssel; bal + jobb a levegőben összeadódik
            auto yl = w_.roomL.simulateResponse(w_.playedL, 48000);
            auto yr = w_.roomR.simulateResponse(w_.playedR, 48000);
            const size_t off = static_cast<size_t>(w_.playAtFrames) + 480;
            for (size_t i = 0; i + off < n && i < yl.size(); ++i) out[i + off] += yl[i] + (i < yr.size() ? yr[i] : 0.0f);
        }
        uint32_t seed = 777;
        for (auto& v : out) { seed = seed * 1664525u + 1013904223u; v += (static_cast<float>(seed & 0xFFFF) / 32768.0f - 1.0f) * 0.0001f; }
        return out;
    }
    bool running() const override { return w_.recording; }
    float takePeak() override { return 0.1f; }
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

    std::printf("\n==========================================\n");
    std::printf("SIKER: Minden szobamérő mag teszt sikeres!\n");
    std::printf("==========================================\n");
    return 0;
}
