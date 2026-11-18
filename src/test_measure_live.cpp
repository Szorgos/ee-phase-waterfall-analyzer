/**
 * @file test_measure_live.cpp
 * @brief A teljes szobamérés élő tesztje (tests/live/measure_live.sh indítja elszigetelt PipeWire-ben).
 *
 * Valódi út, mikrofon helyett visszacsatolással: a mérésvezénylő (room/measurement_session.hpp) a motor beépített
 * lejátszójával (FilePlayer + PwPlayerOutput) a `dac_st` sztereó null-kimenetre játssza a mérőjelet, a PipeWire-es
 * felvevő a `dac_st` monitorjáról vesz fel, majd a kiértékelés lefut. A hurok digitális, így a mért görbe sík, a
 * bal–jobb különbség 0 – ezt ellenőrizzük, a lánc mérési módjának be- és kikapcsolásával együtt.
 */
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "file_player.hpp"
#include "player_output_pw.hpp"
#include "room/measurement_session.hpp"

using namespace aqulify::room;
namespace player = ee_analyzer::player;

static int g_fail = 0;
#define CHECK(c, ...) do { const bool ok_ = static_cast<bool>(c); std::printf("  [%s] ", ok_ ? "OK" : "HIBA"); std::printf(__VA_ARGS__); std::printf("\n"); if (!ok_) ++g_fail; } while (0)

static std::mutex g_mu;
static std::vector<std::string> g_posts;
static std::atomic<int> g_bypassOn{0}, g_bypassOff{0};

static std::string waitFor(MeasurementSession& s, const char* type, double maxSec) {
    const std::string key = std::string("\"type\":\"") + type + "\"";
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(maxSec * 1000));
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            for (const auto& p : g_posts) if (p.find(key) != std::string::npos) return p;
        }
        if (!s.busy() || std::chrono::steady_clock::now() > until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::lock_guard<std::mutex> lk(g_mu);
            for (const auto& p : g_posts) if (p.find(key) != std::string::npos) return p;
            return std::string();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

static std::vector<double> jsonArr(const std::string& j, const std::string& key) {
    std::vector<double> v;
    auto p = j.find("\"" + key + "\":[");
    if (p == std::string::npos) return v;
    p += key.size() + 4;
    while (p < j.size() && j[p] != ']') {
        char* e = nullptr;
        v.push_back(std::strtod(j.c_str() + p, &e));
        p = static_cast<size_t>(e - j.c_str());
        if (p < j.size() && j[p] == ',') ++p;
    }
    return v;
}

static double jsonNum(const std::string& j, const std::string& key) {
    const auto p = j.find("\"" + key + "\":");
    return p == std::string::npos ? NAN : std::atof(j.c_str() + p + key.size() + 3);
}

static void lastErrors() {
    std::lock_guard<std::mutex> lk(g_mu);
    for (const auto& p : g_posts) if (p.find("measurement_error") != std::string::npos) std::printf("     %s\n", p.c_str());
}

int main() {
    std::printf("[1] Élő mérés: a motor lejátszója -> dac_st (sztereó null-kimenet) -> monitor-felvétel -> kiértékelés\n");
    player::FilePlayer fp(std::make_unique<player::PwPlayerOutput>("dac_st"));
    SessionDeps d;
    d.bypassChain = [](bool on) { (on ? g_bypassOn : g_bypassOff)++; };
    d.playFile = [&fp](const std::string& path, std::string& err) { return fp.open(path, &err, 0, /*bypassUpsample=*/true); };
    d.stopPlayback = [&fp] { fp.stop(); };
    d.pauseMedia = [] {};
    d.makeMic = [] { return createMicCapture(); };
    d.post = [](const std::string& s) { std::lock_guard<std::mutex> lk(g_mu); g_posts.push_back(s); };
    MeasurementSession s(std::move(d));

    const std::string list = s.listInputsJson();
    CHECK(list.find("\"available\":true") != std::string::npos, "a felvevő elérhető: %.120s", list.c_str());

    for (const char* ch : { "left", "both" }) {
        { std::lock_guard<std::mutex> lk(g_mu); g_posts.clear(); }
        MeasureConfig cfg;
        cfg.channel = ch;
        cfg.durationSec = 2.0f;
        cfg.levelDb = -12.0f;
        cfg.targetType = "flat";
        cfg.smoothingFraction = 12;
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(s.startMeasurement(cfg, "dac_st", err), "%s: a mérés elindul", ch);
        const std::string res = waitFor(s, "measurement_result", 30.0);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(!res.empty(), "%s: eredmény érkezett (%.1f s)", ch, secs);
        if (res.empty()) { lastErrors(); continue; }
        const auto f = jsonArr(res, "freqs"), m = jsonArr(res, "measured_dbs");
        double maxDev = 0.0, atF = 0.0;
        for (size_t i = 0; i < f.size() && i < m.size(); ++i)
            if (f[i] >= 30.0 && f[i] <= 16000.0 && std::fabs(m[i]) > maxDev) { maxDev = std::fabs(m[i]); atF = f[i]; }
        std::printf("     görbe: %zu pont, legnagyobb eltérés 30 Hz – 16 kHz: %.2f dB (%.0f Hz); SNR %.1f dB\n",
                    f.size(), maxDev, atF, jsonNum(res, "snr_db"));
        CHECK(f.size() > 100 && maxDev < 1.0, "%s: a digitális hurok görbéje sík (±1 dB): %.2f dB", ch, maxDev);
        CHECK(jsonNum(res, "snr_db") > 40.0, "%s: jel-zaj viszony > 40 dB", ch);
        if (std::string(ch) == "both") {
            const double lr = jsonNum(res, "lr_delay_ms");
            std::printf("     bal–jobb különbség: %.3f ms\n", lr);
            CHECK(std::fabs(lr) < 0.05, "both: a bal–jobb különbség ~0 (%.3f ms)", lr);
            CHECK(res.find("\"lr_polarity_differs\":false") != std::string::npos, "both: azonos polaritás");
        }
        CHECK(g_bypassOn.load() == g_bypassOff.load(), "%s: a lánc visszaállt (%d/%d)", ch, g_bypassOn.load(), g_bypassOff.load());
    }

    std::printf("[2] Szintpróba élőben (mic_level) és leállítás\n");
    {
        { std::lock_guard<std::mutex> lk(g_mu); g_posts.clear(); }
        std::string err;
        CHECK(s.startLevelCheck("dac_st", -20.0f, "both", err), "a szintpróba elindul");
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        s.stop();
        for (int i = 0; i < 100 && s.busy(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        double maxPk = -200.0;
        int n = 0;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            for (const auto& p : g_posts)
                if (p.find("\"type\":\"mic_level\"") != std::string::npos) { ++n; maxPk = std::max(maxPk, jsonNum(p, "peak_db")); }
        }
        std::printf("     %d szintüzenet, legnagyobb csúcs: %.1f dBFS (a jel: −20 dBFS, a monitor monóra keverve)\n", n, maxPk);
        CHECK(n >= 10, "szintüzenetek érkeznek (%d)", n);
        CHECK(maxPk > -30.0 && maxPk < -10.0, "a mért szint a lejátszott szint körül: %.1f dBFS", maxPk);
        CHECK(!s.busy() && g_bypassOn.load() == g_bypassOff.load(), "leállt, a lánc visszaállt");
    }

    std::printf("[3] Nem létező mikrofon: érthető hiba, a lánc érintetlen\n");
    {
        { std::lock_guard<std::mutex> lk(g_mu); g_posts.clear(); }
        const int on = g_bypassOn.load();
        MeasureConfig cfg;
        std::string err;
        s.startMeasurement(cfg, "nincs-ilyen-mikrofon", err);
        const std::string e = waitFor(s, "measurement_error", 10.0);
        CHECK(e.find("MIC_no_device") != std::string::npos, "hiba: %s", e.c_str());
        CHECK(g_bypassOn.load() == on, "a lánc nem változott");
    }


    std::printf("[K15] Szobamérés bekapcsolt felmintavételezés (×2, minimumfázis) mellett: megkerülő út\n");
    {
        for (int i = 0; i < 100 && s.busy(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        { std::lock_guard<std::mutex> lk(g_mu); g_posts.clear(); }
        ee_analyzer::audioopt::AudioOptions uo;
        uo.upsampleRule = ee_analyzer::audioopt::UpsampleRule::Double;
        uo.upsamplePreset = 2; // Minimumfázis
        uo.upsamplePhase = 0.0;
        uo.upsampleHeadroomDb = -1.0f;
        fp.setUpsampleOptions(uo);
        const uint64_t procBefore = fp.upsampleProcessedFrames();

        MeasureConfig cfg;
        cfg.channel = "left";
        cfg.durationSec = 2.0f;
        cfg.levelDb = -12.0f;
        cfg.targetType = "flat";
        cfg.smoothingFraction = 12;
        std::string err;
        CHECK(s.startMeasurement(cfg, "dac_st", err), "K15: a mérés elindul bekapcsolt felmintavételezéssel");
        const std::string res = waitFor(s, "measurement_result", 30.0);
        CHECK(!res.empty(), "K15: mérési eredmény érkezett");
        if (!res.empty()) {
            const auto f = jsonArr(res, "freqs"), m = jsonArr(res, "measured_dbs");
            double maxDev = 0.0;
            for (size_t i = 0; i < f.size() && i < m.size(); ++i)
                if (f[i] >= 30.0 && f[i] <= 16000.0 && std::fabs(m[i]) > maxDev) maxDev = std::fabs(m[i]);
            CHECK(f.size() > 100 && maxDev < 1.0, "K15: a görbe sík (±1 dB): %.2f dB", maxDev);
            CHECK(jsonNum(res, "snr_db") > 40.0, "K15: SNR > 40 dB (%.1f dB)", jsonNum(res, "snr_db"));
        }
        const uint64_t procAfter = fp.upsampleProcessedFrames();
        CHECK(procAfter == procBefore && !fp.isUpsampleActive() && fp.upsampleReason() == "bypass",
              "K15: a felmintavételező nem futott a mérés alatt (feldolgozott keretek: %llu -> %llu, ok: %s)",
              static_cast<unsigned long long>(procBefore),
              static_cast<unsigned long long>(procAfter),
              fp.upsampleReason().c_str());
    }

    std::printf(g_fail ? "\nHIBA: %d ellenőrzés nem sikerült\n" : "\nMINDEN TESZT SIKERES (0 hiba)\n", g_fail);
    return g_fail ? 1 : 0;
}
