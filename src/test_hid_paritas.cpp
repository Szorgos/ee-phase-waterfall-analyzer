// Rendszerhang-híd paritásteszt: a motor DSP-lánca (LiveUpsampleBridge + AetherChain / ChainPlayerDsp)
// és az Equalizer APO plugin lánca (AetherChain a forrás mintavételén) azonos ParamData mellett
// egyező frekvenciaválaszt és szintet ad (a fejtér levonásával), valamint a híd indulásakor/leállásakor
// az átengedés és a visszaállás helyesen működik. Hangeszköz nélkül, platformfüggetlenül fut.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "aether_chain.hpp"
#include "audio_options.hpp"
#include "chain_player_dsp.hpp"
#include "live_upsample_bridge.hpp"
#include "param_block.hpp"

using namespace ee_analyzer;

namespace {

int g_ok = 0, g_fail = 0;

#define CHECK(cond, ...)                                   \
    do {                                                   \
        if (cond) { ++g_ok; std::printf("  [OK]   "); }    \
        else { ++g_fail; std::printf("  [HIBA] "); }       \
        std::printf(__VA_ARGS__);                          \
        std::printf("\n");                                 \
    } while (0)

constexpr double kPi = 3.14159265358979323846;

// Vizsgált hangmagasságok az átviteli sávban (44,1 és 48 kHz Nyquist-határa alatt)
constexpr double kTestFreqs[] = { 80.0, 160.0, 315.0, 630.0, 1000.0, 2000.0, 4000.0, 8000.0, 12000.0, 16000.0, 18000.0 };
constexpr size_t kNumFreqs = sizeof(kTestFreqs) / sizeof(kTestFreqs[0]);

/** Determinisztikus több-szinuszos sztereó próbajel generálása adott csúcsamplitúdóval. */
void generateMultitone(uint32_t sr, size_t frames, float peakAmp, std::vector<float>& L, std::vector<float>& R) {
    L.resize(frames);
    R.resize(frames);
    double maxAbs = 0.0;
    std::vector<double> tmpL(frames, 0.0), tmpR(frames, 0.0);
    for (size_t k = 0; k < kNumFreqs; ++k) {
        const double w = 2.0 * kPi * kTestFreqs[k] / static_cast<double>(sr);
        const double phL = kPi * static_cast<double>(k * k) / static_cast<double>(kNumFreqs);
        const double phR = phL + 0.37 * static_cast<double>(k + 1);
        for (size_t i = 0; i < frames; ++i) {
            tmpL[i] += std::sin(w * static_cast<double>(i) + phL);
            tmpR[i] += std::sin(w * static_cast<double>(i) + phR);
        }
    }
    for (size_t i = 0; i < frames; ++i)
        maxAbs = std::max(maxAbs, std::max(std::abs(tmpL[i]), std::abs(tmpR[i])));
    const double scale = (maxAbs > 0.0) ? (static_cast<double>(peakAmp) / maxAbs) : 1.0;
    for (size_t i = 0; i < frames; ++i) {
        L[i] = static_cast<float>(tmpL[i] * scale);
        R[i] = static_cast<float>(tmpR[i] * scale);
    }
}

/** Egy hangmagasság amplitúdója (dB) Hann-ablakolt DFT-vel egy beállt szakaszon. */
double measureToneDb(const std::vector<float>& x, uint32_t sr, size_t start, size_t len, double freqHz) {
    double re = 0.0, im = 0.0, winSum = 0.0;
    const double w = 2.0 * kPi * freqHz / static_cast<double>(sr);
    const double invNm1 = 2.0 * kPi / static_cast<double>(len - 1);
    for (size_t i = 0; i < len; ++i) {
        const double win = 0.5 - 0.5 * std::cos(invNm1 * static_cast<double>(i));
        const double s = static_cast<double>(x[start + i]) * win;
        const double ang = w * static_cast<double>(i);
        re += s * std::cos(ang);
        im -= s * std::sin(ang);
        winSum += win;
    }
    const double amp = 2.0 * std::hypot(re, im) / std::max(winSum, 1e-12);
    return 20.0 * std::log10(std::max(amp, 1e-12));
}

double measureRmsDb(const std::vector<float>& x, size_t start, size_t len) {
    double sum = 0.0;
    for (size_t i = 0; i < len; ++i) {
        const double v = static_cast<double>(x[start + i]);
        sum += v * v;
    }
    return 20.0 * std::log10(std::max(std::sqrt(sum / static_cast<double>(len)), 1e-12));
}

/** A SourceManager::pluginParamsFor megfelelője: híd alatt minden modul ki (csak kFlagInitialized marad). */
ParamData pluginParamsFor(const ParamData& p, bool bridgeActive) {
    if (!bridgeActive) return p;
    ParamData q = p;
    q.flags &= kFlagInitialized;
    return q;
}

/** Plugin út: AetherChain a forrás mintavételén (480 keretes blokkokban). */
void runPluginPath(const std::vector<float>& inL, const std::vector<float>& inR,
                   uint32_t sr, const ParamData& p,
                   std::vector<float>& outL, std::vector<float>& outR) {
    AetherChain chain;
    chain.setSampleRate(sr);
    chain.applyParamsRT(p);
    const size_t blk = 480;
    std::vector<float> zL(blk, 0.0f), zR(blk, 0.0f);
    for (int k = 0; k < 10; ++k) chain.process(zL.data(), zR.data(), blk);

    outL = inL;
    outR = inR;
    for (size_t off = 0; off < outL.size(); off += blk) {
        const size_t n = std::min(blk, outL.size() - off);
        chain.process(outL.data() + off, outR.data() + off, n);
    }
}

/** Híd út: LiveUpsampleBridge (srcRate -> dstRate) + motor AetherChain dstRate-en (960 keretes blokkokban). */
void runBridgePath(const std::vector<float>& inL, const std::vector<float>& inR,
                   uint32_t srcRate, uint32_t dstRate, const audioopt::AudioOptions& opts,
                   const ParamData& p,
                   std::vector<float>& outL, std::vector<float>& outR) {
    livebridge::LiveUpsampleBridge bridge;
    if (!bridge.configure(srcRate, dstRate, opts, nullptr, /*enableDriftControl=*/false)) return;

    AetherChain chain;
    chain.setSampleRate(dstRate);
    chain.applyParamsRT(p);
    const size_t blkOut = 960;
    std::vector<float> zL(blkOut, 0.0f), zR(blkOut, 0.0f);
    for (int k = 0; k < 10; ++k) chain.process(zL.data(), zR.data(), blkOut);

    const size_t totalIn = inL.size();
    std::vector<float> inBuf(1024 * 2);
    std::vector<float> outInterleaved;
    outInterleaved.reserve(static_cast<size_t>(totalIn * (static_cast<double>(dstRate) / srcRate) + 16384) * 2);
    std::vector<float> pullBuf(blkOut * 2);

    auto drain = [&] {
        while (bridge.outAvailableFrames() >= blkOut) {
            if (bridge.pullOutput(pullBuf.data(), blkOut) == 0) break;
            for (size_t i = 0; i < blkOut; ++i) { zL[i] = pullBuf[2 * i]; zR[i] = pullBuf[2 * i + 1]; }
            chain.process(zL.data(), zR.data(), blkOut);
            for (size_t i = 0; i < blkOut; ++i) {
                outInterleaved.push_back(zL[i]);
                outInterleaved.push_back(zR[i]);
            }
        }
    };

    size_t pos = 0;
    while (pos < totalIn) {
        const size_t chunk = std::min<size_t>(480, totalIn - pos);
        for (size_t i = 0; i < chunk; ++i) {
            inBuf[2 * i] = inL[pos + i];
            inBuf[2 * i + 1] = inR[pos + i];
        }
        bridge.pushInput(inBuf.data(), chunk);
        pos += chunk;
        while (bridge.stepOnce()) {}
        drain();
    }
    std::fill(inBuf.begin(), inBuf.end(), 0.0f);
    for (int pad = 0; pad < 40; ++pad) {
        bridge.pushInput(inBuf.data(), 480);
        while (bridge.stepOnce()) {}
        drain();
    }

    const size_t nOut = outInterleaved.size() / 2;
    outL.resize(nOut);
    outR.resize(nOut);
    for (size_t i = 0; i < nOut; ++i) {
        outL[i] = outInterleaved[2 * i];
        outR[i] = outInterleaved[2 * i + 1];
    }
}

struct CompareResult {
    bool valid = false;
    double rmsDiffDb = 0.0;      // híd - plugin
    double maxToneDiffDb = 0.0;  // legnagyobb |híd - plugin|
    double maxShapeDiffDb = 0.0; // hangszín-eltérés az átlagos szintkülönbség levonása után
};

CompareResult compareOutputs(const std::vector<float>& plugL, uint32_t srcRate,
                             const std::vector<float>& brL, uint32_t dstRate) {
    CompareResult r;
    const size_t pStart = srcRate / 2;
    const size_t pLen = srcRate / 2;
    const size_t bStart = dstRate / 2;
    const size_t bLen = dstRate / 2;
    if (plugL.size() < pStart + pLen || brL.size() < bStart + bLen) return r;
    r.valid = true;
    r.rmsDiffDb = measureRmsDb(brL, bStart, bLen) - measureRmsDb(plugL, pStart, pLen);
    double diffs[kNumFreqs] = {};
    double meanDiff = 0.0;
    for (size_t k = 0; k < kNumFreqs; ++k) {
        diffs[k] = measureToneDb(brL, dstRate, bStart, bLen, kTestFreqs[k]) -
                   measureToneDb(plugL, srcRate, pStart, pLen, kTestFreqs[k]);
        meanDiff += diffs[k];
        r.maxToneDiffDb = std::max(r.maxToneDiffDb, std::abs(diffs[k]));
    }
    meanDiff /= static_cast<double>(kNumFreqs);
    for (size_t k = 0; k < kNumFreqs; ++k)
        r.maxShapeDiffDb = std::max(r.maxShapeDiffDb, std::abs(diffs[k] - meanDiff));
    return r;
}

ParamData makeLimiterParams() {
    ParamData p = transparentParams();
    p.flags = kFlagLimiter | kFlagInitialized;
    p.preampDb = -7.15f; // EQ kikapcsolva -> nem hat
    p.limMode = 0;
    p.limOversampling = 22; // True Peak 24 bit
    p.limThresholdDb = -1.0f;
    p.limLookaheadMs = 5.0f;
    p.limAttackMs = 5.0f;
    p.limReleaseMs = 5.0f;
    return p;
}

ParamData makeEqParams() {
    ParamData p = transparentParams();
    p.flags = kFlagEq | kFlagLimiter | kFlagInitialized;
    p.preampDb = -3.0f;
    p.numBands = 2;
    p.bands[0].type = static_cast<int32_t>(EqBandType::Bell);
    p.bands[0].freq = 200.0f;
    p.bands[0].gainDb = 6.0f;
    p.bands[0].q = 1.4f;
    p.bands[1].type = static_cast<int32_t>(EqBandType::Bell);
    p.bands[1].freq = 4000.0f;
    p.bands[1].gainDb = -4.0f;
    p.bands[1].q = 1.4f;
    return p;
}

} // namespace

int main() {
    std::printf("========================================================\n");
    std::printf(" Aqulify – Rendszerhang-híd vs. plugin paritásteszt\n");
    std::printf("========================================================\n");

    // [1] 48 kHz plugin vs 48 -> 96 kHz híd (csak Limiter, EQ ki) mindhárom SRC-motorral
    std::printf("\n[1] Csak Limiter (EQ ki), 48 kHz vs 48 -> 96 kHz, mindhárom SRC-motor\n");
    {
        const uint32_t sr = 48000, dst = 96000;
        std::vector<float> inLowL, inLowR, inHotL, inHotR;
        generateMultitone(sr, sr * 2, 0.25f, inLowL, inLowR);
        generateMultitone(sr, sr * 2, 0.95f, inHotL, inHotR);
        const ParamData p = makeLimiterParams();
        std::vector<float> pLowL, pLowR, pHotL, pHotR;
        runPluginPath(inLowL, inLowR, sr, p, pLowL, pLowR);
        runPluginPath(inHotL, inHotR, sr, p, pHotL, pHotR);

        for (int eng = 0; eng <= 2; ++eng) {
            audioopt::AudioOptions opts;
            opts.upsampleRule = audioopt::UpsampleRule::Double;
            opts.upsampleEngine = eng;
            opts.upsampleHeadroomDb = 0.0f;
            std::vector<float> bL, bR;
            runBridgePath(inLowL, inLowR, sr, dst, opts, p, bL, bR);
            const auto rLow = compareOutputs(pLowL, sr, bL, dst);
            CHECK(rLow.valid && rLow.maxToneDiffDb < 0.10 && rLow.maxShapeDiffDb < 0.05,
                  "%s (-12 dBFS, 0 dB fejtér): RMS %+.3f dB, max %.3f dB, hangszín %.3f dB",
                  audioopt::upsampleEngineLabel(eng), rLow.rmsDiffDb, rLow.maxToneDiffDb, rLow.maxShapeDiffDb);

            runBridgePath(inHotL, inHotR, sr, dst, opts, p, bL, bR);
            const auto rHot = compareOutputs(pHotL, sr, bL, dst);
            CHECK(rHot.valid && rHot.maxToneDiffDb < 0.35 && rHot.maxShapeDiffDb < 0.10,
                  "%s (-0.45 dBFS, 0 dB fejtér): RMS %+.3f dB, max %.3f dB, hangszín %.3f dB",
                  audioopt::upsampleEngineLabel(eng), rHot.rmsDiffDb, rHot.maxToneDiffDb, rHot.maxShapeDiffDb);

            // Alapértelmezett -1,0 dB fejtér: egyenletes -1 dB szinteltérés, hangszín változatlan
            opts.upsampleHeadroomDb = -1.0f;
            runBridgePath(inLowL, inLowR, sr, dst, opts, p, bL, bR);
            const auto rHr = compareOutputs(pLowL, sr, bL, dst);
            CHECK(rHr.valid && std::abs(rHr.rmsDiffDb - (-1.0)) < 0.05 && rHr.maxShapeDiffDb < 0.05,
                  "%s (-12 dBFS, -1 dB fejtér): RMS %+.3f dB (várt -1,0), hangszín %.3f dB",
                  audioopt::upsampleEngineLabel(eng), rHr.rmsDiffDb, rHr.maxShapeDiffDb);
        }
    }

    // [2] 44,1 kHz plugin vs 44,1 -> 88,2 kHz híd, aktív modulokkal (0 dB fejtér)
    std::printf("\n[2] Aktív DSP-modulok paritása (44,1 kHz vs 44,1 -> 88,2 kHz, libsoxr, 0 dB fejtér)\n");
    {
        const uint32_t sr = 44100, dst = 88200;
        std::vector<float> inL, inR;
        generateMultitone(sr, sr * 2, 0.25f, inL, inR);
        audioopt::AudioOptions opts;
        opts.upsampleRule = audioopt::UpsampleRule::Double;
        opts.upsampleEngine = 0;
        opts.upsampleHeadroomDb = 0.0f;

        auto verify = [&](const char* name, const ParamData& p, double maxTone, double maxShape) {
            std::vector<float> pL, pR, bL, bR;
            runPluginPath(inL, inR, sr, p, pL, pR);
            runBridgePath(inL, inR, sr, dst, opts, p, bL, bR);
            const auto r = compareOutputs(pL, sr, bL, dst);
            CHECK(r.valid && r.maxToneDiffDb < maxTone && r.maxShapeDiffDb < maxShape,
                  "%s: RMS %+.3f dB, max %.3f dB (<%.2f), hangszín %.3f dB (<%.2f)",
                  name, r.rmsDiffDb, r.maxToneDiffDb, maxTone, r.maxShapeDiffDb, maxShape);
        };

        verify("EQ (IIR, decramp Off) + preamp -3 dB + Limiter", makeEqParams(), 0.25, 0.20);

        {
            ParamData p = transparentParams();
            p.flags = kFlagLoudness | kFlagInitialized;
            p.loudVolumeDb = -20.0f;
            verify("Loudness (ISO 226, -20 dB, FFT 4096)", p, 0.25, 0.20);
        }
        {
            ParamData p = transparentParams();
            p.flags = kFlagBass | kFlagInitialized;
            p.bassAmountDb = 6.0f;
            p.bassHarmonics = 8.5f;
            p.bassScopeHz = 120.0f;
            verify("Bass Enhancer (+6 dB, 120 Hz)", p, 0.20, 0.15);
        }
        {
            ParamData p = transparentParams();
            p.flags = kFlagCrossfeed | kFlagInitialized;
            p.crossfeedFcut = 700.0f;
            p.crossfeedFeedDb = 4.5f;
            verify("Crossfeed (bs2b 700 Hz / 4,5 dB)", p, 0.30, 0.20);
        }
        {
            ParamData p = transparentParams();
            p.flags = kFlagMono | kFlagInitialized;
            p.subsonicHz = 30;
            p.monoAmount = 100.0f;
            p.monoFreqHz = 120.0f;
            verify("Mono + Subsonic (30 Hz, 100% @120 Hz)", p, 0.15, 0.10);
        }
        {
            ParamData p = transparentParams();
            p.flags = kFlagOutStage | kFlagInitialized;
            p.chGainDb[0] = -2.0f;
            p.chGainDb[1] = -2.0f;
            verify("OutStage (chGain -2 dB)", p, 0.05, 0.05);
        }
    }

    // [3] Motor ExclusiveDsp (ChainPlayerDsp) indítási inicializálás és közvetlen paritás
    std::printf("\n[3] Motor ExclusiveDsp (ChainPlayerDsp) indítási lánc és közvetlen plugin-paritás\n");
    {
        const uint32_t sr = 48000;
        std::vector<float> inL, inR;
        generateMultitone(sr, sr * 2, 0.25f, inL, inR);
        const ParamData pEq = makeEqParams();

        // Plugin kimenet
        std::vector<float> pL, pR;
        runPluginPath(inL, inR, sr, pEq, pL, pR);

        // Motor kimenet: csak a konstruktor kapja meg a visszaállított pEq-t (külön setParams nélkül!)
        player::ChainPlayerDsp dsp(pEq);
        dsp.prepare(sr, 480, 0);
        std::vector<float> zero(480 * 2, 0.0f);
        for (int k = 0; k < 10; ++k) dsp.process(zero.data(), 480);

        std::vector<float> mL(inL.size()), mR(inR.size());
        std::vector<float> blk(480 * 2);
        for (size_t off = 0; off < inL.size(); off += 480) {
            const size_t n = std::min<size_t>(480, inL.size() - off);
            for (size_t i = 0; i < n; ++i) { blk[2 * i] = inL[off + i]; blk[2 * i + 1] = inR[off + i]; }
            dsp.process(blk.data(), n);
            for (size_t i = 0; i < n; ++i) { mL[off + i] = blk[2 * i]; mR[off + i] = blk[2 * i + 1]; }
        }
        const auto r = compareOutputs(pL, sr, mL, sr);
        CHECK(r.valid && r.maxToneDiffDb < 0.05 && std::abs(r.rmsDiffDb) < 0.05 && !dsp.transparent(),
              "ChainPlayerDsp(initial) == plugin AetherChain: RMS %+.4f dB, max %.4f dB",
              r.rmsDiffDb, r.maxToneDiffDb);
    }

    // [4] Híd indulás (plugin átenged) és leállás (plugin visszakapja a teljes láncot)
    std::printf("\n[4] Híd indulás / leállás: plugin átengedése és teljes visszaállása\n");
    {
        const uint32_t sr = 48000;
        std::vector<float> inL, inR;
        generateMultitone(sr, sr * 2, 0.25f, inL, inR);
        const ParamData pFull = makeEqParams();

        // Referencia: teljes lánc a pluginon
        std::vector<float> refL, refR;
        runPluginPath(inL, inR, sr, pFull, refL, refR);

        // Plugin életciklusa: először híd BE (átengedés), majd híd KI (visszaállás pFull-ra)
        AetherChain plugin;
        plugin.setSampleRate(sr);
        plugin.applyParamsRT(pFull);
        std::vector<float> zL(480, 0.0f), zR(480, 0.0f);
        for (int k = 0; k < 10; ++k) plugin.process(zL.data(), zR.data(), 480);

        // Híd elindul -> plugin átengedő beállítást kap
        const ParamData pBypass = pluginParamsFor(pFull, true);
        CHECK(!AetherChain::modifiesSignal(pBypass) && pBypass.has(kFlagInitialized),
              "pluginParamsFor(p, true): minden modul ki, kFlagInitialized megmarad");
        plugin.applyParamsRT(pBypass);
        for (int k = 0; k < 10; ++k) plugin.process(zL.data(), zR.data(), 480);

        std::vector<float> bypL = inL, bypR = inR;
        for (size_t off = 0; off < bypL.size(); off += 480)
            plugin.process(bypL.data() + off, bypR.data() + off, std::min<size_t>(480, bypL.size() - off));
        const auto rByp = compareOutputs(inL, sr, bypL, sr);
        CHECK(rByp.valid && rByp.maxToneDiffDb < 0.01 && std::abs(rByp.rmsDiffDb) < 0.01,
              "híd alatt a plugin átlátszó (bemenethez képest max %.4f dB, RMS %+.4f dB)",
              rByp.maxToneDiffDb, rByp.rmsDiffDb);

        // Híd leáll -> plugin visszakapja a teljes láncot
        const ParamData pRestored = pluginParamsFor(pFull, false);
        plugin.applyParamsRT(pRestored);
        for (int k = 0; k < 10; ++k) plugin.process(zL.data(), zR.data(), 480);

        std::vector<float> resL = inL, resR = inR;
        for (size_t off = 0; off < resL.size(); off += 480)
            plugin.process(resL.data() + off, resR.data() + off, std::min<size_t>(480, resL.size() - off));
        const auto rRes = compareOutputs(refL, sr, resL, sr);
        CHECK(rRes.valid && rRes.maxToneDiffDb < 0.05 && std::abs(rRes.rmsDiffDb) < 0.05,
              "híd leállása után a plugin visszaáll a teljes láncra (refhez képest max %.4f dB, RMS %+.4f dB)",
              rRes.maxToneDiffDb, rRes.rmsDiffDb);
    }

    std::printf("\n%s (%d hiba, %d rendben)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail, g_ok);
    return g_fail ? 1 : 0;
}
