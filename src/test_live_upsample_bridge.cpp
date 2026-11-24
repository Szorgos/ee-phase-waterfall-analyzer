// Egységteszt: platformfüggetlen élő rendszerhang-felmintavételező híd (include/live_upsample_bridge.hpp)
// [L1] AudioOptions rendszerhang mezők (JSON oda-vissza, sanitize) és virtuális kábel felismerés
// [L2] Élő felmintavételezési minőség: libsoxr + Aqulify FFT64 (50/25/0 fázis), 44,1->96 kHz és 48->192 kHz
// [L3] Órajel-drift stresszteszt: +40 ppm és -40 ppm 30 másodpercen át, 0 underrun, 0 overrun
// [L4] Valós idejű foglalásmentesség: pushInput() és pullOutput() alatt 0 heap-allokáció
// [L5] Aszinkron munkaszál, DSP visszahívás, rátaváltás (44,1 -> 48 kHz) és szálbiztos leállítás
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "audio_options.hpp"
#include "live_upsample_bridge.hpp"

static std::atomic<bool> g_trackAlloc{false};
static std::atomic<uint64_t> g_allocCount{0};

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(std::size_t sz) {
    if (g_trackAlloc.load(std::memory_order_relaxed)) {
        g_allocCount.fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(sz ? sz : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

using namespace ee_analyzer;
using namespace ee_analyzer::livebridge;

static int g_fail = 0;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        const bool c = (cond);                                                \
        if (c) std::printf("  [OK]   %s\n", msg);                             \
        else { std::printf("  [HIBA] %s\n", msg); ++g_fail; }                 \
    } while (0)

static constexpr double kPi = 3.14159265358979323846;

static double db(double v) { return 20.0 * std::log10(std::max(v, 1e-30)); }

/** Hann-ablakolt egypontos DFT amplitúdó sztereó csatornán (0=L, 1=R). */
static double toneAmp(const std::vector<float>& stereo, int ch, size_t startFrame, size_t lenFrames,
                      double freqHz, double rateHz) {
    double cr = 0.0, ci = 0.0, wsum = 0.0;
    for (size_t i = 0; i < lenFrames; ++i) {
        const double w = 0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(lenFrames)));
        const double x = static_cast<double>(stereo[(startFrame + i) * 2 + ch]) * w;
        const double ph = 2.0 * kPi * freqHz * static_cast<double>(i) / rateHz;
        cr += x * std::cos(ph);
        ci -= x * std::sin(ph);
        wsum += w;
    }
    return 2.0 * std::hypot(cr, ci) / std::max(wsum, 1e-12);
}

int main() {
    std::printf("[L1] AudioOptions rendszerhang mezők + virtuális hangkábel felismerés\n");
    {
        audioopt::AudioOptions o;
        CHECK(!o.upsampleSystemAudio && o.systemCaptureDevice.empty(), "alapértelmezett: rendszerhang-felmintavételezés ki");
        o.upsampleSystemAudio = true;
        o.systemCaptureDevice = "CABLE Input (VB-Audio Virtual Cable)";
        json::Value jv;
        const bool parsed = json::parse(audioopt::toJson(o), jv);
        const audioopt::AudioOptions o2 = audioopt::fromJson(jv);
        CHECK(parsed && o2.upsampleSystemAudio && o2.systemCaptureDevice == o.systemCaptureDevice && o2 == o,
              "upsampleSystemAudio és systemCaptureDevice JSON oda-vissza pontos");

        audioopt::AudioOptions bad;
        bad.systemCaptureDevice = "bad\x01name";
        audioopt::sanitize(bad);
        CHECK(bad.systemCaptureDevice.empty(), "sanitize elutasítja a vezérlőkarakteres eszköznevet");

        CHECK(isVirtualCableName("CABLE Input (VB-Audio Virtual Cable)"), "VB-Audio kábel felismerve");
        CHECK(isVirtualCableName("BlackHole 2ch"), "BlackHole felismerve");
        CHECK(isVirtualCableName("VoiceMeeter Input"), "VoiceMeeter felismerve");
        CHECK(!isVirtualCableName("Focusrite Scarlett 2i2 USB"), "valódi USB DAC nem virtuális kábel");
    }

    std::printf("[L2] Élő híd minőség: libsoxr + Aqulify FFT64 (50 / 25 / 0 fázis)\n");
    {
        struct Case { int eng; double phase; uint32_t sr; uint32_t dr; const char* name; };
        const Case cases[] = {
            {0, 25.0, 44100,  96000, "libsoxr köztes 44,1 -> 96 kHz"},
            {1, 50.0, 44100,  96000, "Aqulify FFT64 lineáris 44,1 -> 96 kHz"},
            {1, 25.0, 44100,  96000, "Aqulify FFT64 köztes 44,1 -> 96 kHz"},
            {1,  0.0, 44100,  96000, "Aqulify FFT64 minimum 44,1 -> 96 kHz"},
            {1, 50.0, 48000, 192000, "Aqulify FFT64 lineáris 48 -> 192 kHz"},
        };
        for (const auto& tc : cases) {
            audioopt::AudioOptions o;
            o.upsampleEngine = tc.eng;
            o.upsamplePhase = tc.phase;
            o.upsamplePassbandEnd = 0.95;
            o.upsampleStopbandBegin = 1.00;
            o.upsamplePrecision = 28;
            o.upsampleHeadroomDb = 0.0f;

            LiveUpsampleBridge br;
            CHECK(br.configure(tc.sr, tc.dr, o, nullptr, /*enableDriftControl=*/false), tc.name);

            // 1,5 másodperc 10 kHz sztereó szinusz szinkron léptetéssel (0 drift)
            const size_t totalIn = static_cast<size_t>(tc.sr) * 3 / 2;
            const size_t blkIn = 512;
            const size_t blkOut = static_cast<size_t>(std::llround(static_cast<double>(blkIn) * tc.dr / tc.sr));
            std::vector<float> inBlk(blkIn * 2), outBlk(blkOut * 2), allOut;
            allOut.reserve(static_cast<size_t>(tc.dr) * 2 * 2);

            size_t inPos = 0;
            while (inPos < totalIn) {
                const size_t n = std::min(blkIn, totalIn - inPos);
                for (size_t i = 0; i < n; ++i) {
                    const float v = 0.5f * std::sin(2.0f * static_cast<float>(kPi) * 10000.0f *
                                                    static_cast<float>(inPos + i) / static_cast<float>(tc.sr));
                    inBlk[2 * i] = v;
                    inBlk[2 * i + 1] = -v;
                }
                br.pushInput(inBlk.data(), n);
                while (br.stepOnce()) {}
                if (br.outAvailableFrames() >= br.targetFillFrames()) {
                    const size_t got = br.pullOutput(outBlk.data(), blkOut);
                    allOut.insert(allOut.end(), outBlk.begin(), outBlk.begin() + static_cast<std::ptrdiff_t>(got * 2));
                }
                inPos += n;
            }
            while (br.outAvailableFrames() >= blkOut) {
                const size_t got = br.pullOutput(outBlk.data(), blkOut);
                allOut.insert(allOut.end(), outBlk.begin(), outBlk.begin() + static_cast<std::ptrdiff_t>(got * 2));
            }

            const size_t totalOut = allOut.size() / 2;
            const size_t measLen = std::min<size_t>(32768, totalOut / 2);
            const size_t measStart = totalOut > measLen + 8192 ? (totalOut - measLen - 4096) : 0;
            const double a10 = toneAmp(allOut, 0, measStart, measLen, 10000.0, tc.dr);
            const double imgFreq = static_cast<double>(tc.sr) - 10000.0;  // első tükörkép (34,1 / 38 kHz)
            const double aImg = toneAmp(allOut, 0, measStart, measLen, imgFreq, tc.dr);
            const BridgeStatus st = br.status();
            std::printf("         %s: 10 kHz=%.4f dB, tükörkép (%.1f kHz)=%.1f dB, underrun=%llu, overrun=%llu\n",
                        tc.name, db(a10 / 0.5), imgFreq / 1000.0, db(aImg / 0.5),
                        static_cast<unsigned long long>(st.underruns), static_cast<unsigned long long>(st.overruns));
            CHECK(totalOut > tc.dr && std::abs(db(a10 / 0.5)) <= 0.03 && db(aImg / 0.5) <= -120.0 &&
                  st.underruns == 0 && st.overruns == 0,
                  "átvitel ±0,03 dB, tükörkép ≤ −120 dB, 0 underrun és 0 overrun");
        }
    }

    std::printf("[L3] Órajel-drift stresszteszt (+40 ppm és −40 ppm, 30 s szimulált folyam)\n");
    {
        for (double driftPpm : {+40.0, -40.0}) {
            audioopt::AudioOptions o;
            o.upsampleEngine = 0;  // libsoxr (és külön teszt FFT64-gyel alább)
            o.upsamplePhase = 25.0;
            o.upsampleHeadroomDb = 0.0f;

            const uint32_t sr = 48000, dr = 96000;
            LiveUpsampleBridge br;
            CHECK(br.configure(sr, dr, o, nullptr, /*enableDriftControl=*/true), "drift bridge configure");

            // 30 másodperc szimuláció: a kimenet pontosan dr, a bemenet (1 + driftPpm*1e-6) * sr ütemben érkezik!
            const double ratioInPerOutTick = (static_cast<double>(sr) / static_cast<double>(dr)) * (1.0 + driftPpm * 1e-6);
            const size_t outTick = 480;  // 5 ms-os DAC callback-ek
            const size_t totalTicks = 6000;  // 30 s
            double inAcc = 0.0;
            uint64_t inSampleIdx = 0;
            std::vector<float> inBlk, outBlk(outTick * 2), tailOut;
            tailOut.reserve(65536 * 2);

            for (size_t tick = 0; tick < totalTicks; ++tick) {
                inAcc += static_cast<double>(outTick) * ratioInPerOutTick;
                const size_t nIn = static_cast<size_t>(inAcc);
                inAcc -= static_cast<double>(nIn);
                inBlk.resize(nIn * 2);
                for (size_t i = 0; i < nIn; ++i) {
                    const float v = 0.5f * std::sin(2.0f * static_cast<float>(kPi) * 1000.0f *
                                                    static_cast<float>(inSampleIdx++) / static_cast<float>(sr));
                    inBlk[2 * i] = v;
                    inBlk[2 * i + 1] = v;
                }
                br.pushInput(inBlk.data(), nIn);
                while (br.stepOnce()) {}
                br.pullOutput(outBlk.data(), outTick);
                if (tick >= totalTicks - 200) {
                    tailOut.insert(tailOut.end(), outBlk.begin(), outBlk.end());
                }
            }

            const BridgeStatus st = br.status();
            const double a1k = toneAmp(tailOut, 0, 0, 32768, 1000.0, dr);
            std::printf("         drift=%+.0f ppm: szabályzott drift=%+.1f ppm, puffer=%.1f ms, 1 kHz=%.4f dB, underrun=%llu, overrun=%llu\n",
                        driftPpm, st.driftPpm, st.latencyMs, db(a1k / 0.5),
                        static_cast<unsigned long long>(st.underruns), static_cast<unsigned long long>(st.overruns));
            CHECK(st.underruns == 0 && st.overruns == 0 && std::abs(st.driftPpm - driftPpm) <= 20.0 &&
                  std::abs(db(a1k / 0.5)) <= 0.05,
                  "30 s ±40 ppm órajel-eltérés: 0 underrun, 0 overrun, stabil követés és tiszta 1 kHz jel");
        }

        // Ugyanez Aqulify FFT64 motorral is (+35 ppm, 15 s)
        {
            audioopt::AudioOptions o;
            o.upsampleEngine = 1;
            o.upsamplePhase = 50.0;
            o.upsampleHeadroomDb = 0.0f;
            const uint32_t sr = 48000, dr = 96000;
            LiveUpsampleBridge br;
            br.configure(sr, dr, o, nullptr, true);
            const double ratioInPerOutTick = (static_cast<double>(sr) / static_cast<double>(dr)) * (1.0 + 35.0e-6);
            const size_t outTick = 480;
            double inAcc = 0.0;
            std::vector<float> inBlk, outBlk(outTick * 2);
            for (size_t tick = 0; tick < 3000; ++tick) {
                inAcc += static_cast<double>(outTick) * ratioInPerOutTick;
                const size_t nIn = static_cast<size_t>(inAcc);
                inAcc -= static_cast<double>(nIn);
                inBlk.assign(nIn * 2, 0.1f);
                br.pushInput(inBlk.data(), nIn);
                while (br.stepOnce()) {}
                br.pullOutput(outBlk.data(), outTick);
            }
            const BridgeStatus st = br.status();
            std::printf("         FFT64 drift=+35 ppm (15 s): szabályzott=%.1f ppm, underrun=%llu, overrun=%llu\n",
                        st.driftPpm, static_cast<unsigned long long>(st.underruns), static_cast<unsigned long long>(st.overruns));
            CHECK(st.underruns == 0 && st.overruns == 0 && st.prefilled,
                  "Aqulify FFT64 +35 ppm drift (15 s): 0 underrun és 0 overrun");
        }
    }

    std::printf("[L4] Valós idejű foglalásmentesség (pushInput és pullOutput = 0 heap allokáció)\n");
    {
        audioopt::AudioOptions o;
        o.upsampleEngine = 1;
        o.upsamplePhase = 25.0;
        LiveUpsampleBridge br;
        br.configure(44100, 96000, o, nullptr, true);

        std::vector<float> inBlk(512 * 2, 0.25f), outBlk(1024 * 2, 0.0f);
        // Bemelegítés
        for (int i = 0; i < 40; ++i) {
            br.pushInput(inBlk.data(), 512);
            while (br.stepOnce()) {}
        }

        g_allocCount.store(0, std::memory_order_relaxed);
        g_trackAlloc.store(true, std::memory_order_release);
        for (int i = 0; i < 50; ++i) {
            br.pushInput(inBlk.data(), 512);
            br.pullOutput(outBlk.data(), 512);
        }
        g_trackAlloc.store(false, std::memory_order_release);
        const uint64_t allocs = g_allocCount.load(std::memory_order_relaxed);
        std::printf("         100 RT hívás alatt mért heap allokációk: %llu\n", static_cast<unsigned long long>(allocs));
        CHECK(allocs == 0, "pushInput() és pullOutput() 0 dinamikus memóriafoglalással fut");
    }

    std::printf("[L5] Aszinkron munkaszál, DSP-visszahívás és rátaváltás (44,1 -> 48 kHz)\n");
    {
        std::atomic<uint32_t> dspSeenRate{0};
        std::atomic<uint64_t> dspFrames{0};
        audioopt::AudioOptions o;
        o.upsampleEngine = 1;
        o.upsamplePhase = 50.0;
        o.upsampleHeadroomDb = 0.0f;

        LiveUpsampleBridge br;
        auto dspCb = [&](float* st, size_t n, uint32_t rate) {
            dspSeenRate.store(rate, std::memory_order_relaxed);
            dspFrames.fetch_add(n, std::memory_order_relaxed);
            for (size_t i = 0; i < n * 2; ++i) st[i] *= 0.5f;  // -6 dB próba DSP
        };

        CHECK(br.configure(44100, 88200, o, dspCb, true) && br.start(), "aszinkron híd indítása (44,1 -> 88,2 kHz)");
        std::vector<float> inBlk(1024 * 2, 0.4f), outBlk(1024 * 2, 0.0f);
        for (int i = 0; i < 40; ++i) {
            br.pushInput(inBlk.data(), 1024);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for (int i = 0; i < 100 && !br.status().prefilled; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const size_t got = br.pullOutput(outBlk.data(), 1024);
        CHECK(got == 1024 && dspSeenRate.load() == 88200 && dspFrames.load() > 10000,
              "aszinkron munkaszál: 88,2 kHz DSP visszahívás és sikeres pullOutput");

        // Rátaváltás futás közben (44,1 -> 48 kHz bemenet, 96 kHz cél)
        dspSeenRate.store(0);
        CHECK(br.configure(48000, 96000, o, dspCb, true) && br.start(), "újrakonfigurálás 48 -> 96 kHz");
        for (int i = 0; i < 40; ++i) {
            br.pushInput(inBlk.data(), 1024);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for (int i = 0; i < 100 && !br.status().prefilled; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const size_t got2 = br.pullOutput(outBlk.data(), 1024);
        br.stop();
        CHECK(got2 == 1024 && dspSeenRate.load() == 96000 && !br.isRunning(),
              "rátaváltás után 96 kHz DSP aktív és stop() szálbiztosan leállt");
    }

    std::printf("[L6] Párhuzamos stresszteszt: configure() + status() + pushInput() + pullOutput() egyidejűleg\n");
    {
        LiveUpsampleBridge br;
        audioopt::AudioOptions initOpt;
        initOpt.upsampleEngine = 0;
        initOpt.upsamplePhase = 25.0;
        CHECK(br.configure(44100, 96000, initOpt, nullptr, true) && br.start(), "stresszteszt: híd indul");

        std::atomic<bool> stopFlag{false};
        std::atomic<uint64_t> statusReads{0};
        std::atomic<uint64_t> badStatus{0};

        std::thread tCap([&] {
            std::vector<float> blk(512 * 2, 0.2f);
            while (!stopFlag.load(std::memory_order_relaxed)) {
                br.pushInput(blk.data(), 512);
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
        });
        std::thread tPlay([&] {
            std::vector<float> blk(512 * 2, 0.0f);
            while (!stopFlag.load(std::memory_order_relaxed)) {
                br.pullOutput(blk.data(), 512);
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
        });
        std::thread tStatus([&] {
            while (!stopFlag.load(std::memory_order_relaxed)) {
                const BridgeStatus st = br.status();
                statusReads.fetch_add(1, std::memory_order_relaxed);
                if (std::isnan(st.latencyMs) || std::isnan(st.driftPpm)) {
                    badStatus.fetch_add(1, std::memory_order_relaxed);
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });

        // Fő szál: gyors paraméter- és motorváltások (csúszkahúzás szimulációja, beleértve a #40-es stopband=1.15174 értéket)
        int cfgOk = 0;
        for (int i = 0; i < 16; ++i) {
            audioopt::AudioOptions o;
            o.upsampleEngine = (i % 2 == 0) ? 0 : 1;
            o.upsamplePhase = (i % 3 == 0) ? 50.0 : 25.0;
            o.upsamplePassbandEnd = 0.95;
            o.upsampleStopbandBegin = (i == 5) ? 1.15174 : (1.00 + 0.03 * (i % 5));
            if (br.configure((i % 2 == 0) ? 44100 : 48000, 96000, o, nullptr, true) && br.start()) ++cfgOk;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        stopFlag.store(true, std::memory_order_release);
        tCap.join();
        tPlay.join();
        tStatus.join();
        br.stop();
        std::printf("         újrakonfigurálások: %d/16, párhuzamos status() hívások: %llu, hibás: %llu\n",
                    cfgOk, static_cast<unsigned long long>(statusReads.load()),
                    static_cast<unsigned long long>(badStatus.load()));
        CHECK(cfgOk == 16 && statusReads.load() > 100 && badStatus.load() == 0 && !br.isRunning(),
              "párhuzamos configure/status/push/pull: összeomlás és NaN nélkül lefutott");
    }

    if (g_fail == 0) std::printf("\nMINDEN LIVE BRIDGE TESZT SIKERES (0 hiba)\n");
    else std::printf("\n%d HIBA: SIKERTELEN\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
