#pragma once
/**
 * @file win_live_bridge.hpp
 * @brief Windows rendszerhang-felmintavételező híd (virtuális kábel -> saját SRC -> WASAPI Exclusive / ASIO).
 *
 * Windowson az Equalizer APO a Windows megosztott keverőjében, rögzített mintavételen fut, ezért a rendszerhang
 * (Spotify, YouTube, Tidal, böngésző stb.) valódi felmintavételezéséhez a hangot egy telepített virtuális
 * hangkábelen (pl. VB-Cable, Virtual Audio Cable, VoiceMeeter) keresztül rögzítjük WASAPI loopback-kel, majd
 * a saját motorral (libsoxr / Aqulify FFT64) és a meglévő DSP-lánccal (ExclusiveDsp) kizárólagos WASAPI vagy
 * ASIO kimeneten közvetlenül a valódi DAC-ra küldjük.
 *
 * Szigorú biztonsági szabályok:
 *  - Visszacsatolás-védelem: a rögzítési virtuális kábel és a kimeneti fizikai DAC azonosítója soha nem egyezhet.
 *  - Elsőbbség a beépített lejátszónak: amikor a FilePlayer számot nyit, a híd azonnal bezárja a DAC-kimenetet,
 *    és a lejátszás végén automatikusan visszaindul.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_options.hpp"
#include "exclusive_dsp.hpp"
#include "live_upsample_bridge.hpp"
#include "player_output_wasapi.hpp"
#include "player_output_win.hpp"
#include "upsample_engine.hpp"
#include "wasapi_loopback.hpp"

namespace ee_analyzer::win {

/** Eszközválasztás eredménye (virtuális bemenet + valódi fizikai kimenet). */
struct WinBridgeDevicePlan {
    bool ok = false;
    std::string reason;          // "no_virtual_device", "no_output_device", "loop_prevented"
    std::wstring captureId;
    std::string captureIdUtf8;
    std::string captureName;
    std::wstring outputId;       // üres, ha ASIO a kimenet
    std::string outputName;
};

/**
 * Tiszta eszközválasztó és visszacsatolás-ellenőrző (platformfüggetlen tesztelésre is alkalmas).
 * - preferredCaptureUtf8: a felhasználó által választott virtuális kábel azonosítója vagy neve (üres = első virtuális).
 * - useAsio: ha igaz, a kimenet ASIO meghajtón megy, így nem szükséges külön fizikai WASAPI eszköz.
 */
inline WinBridgeDevicePlan chooseWinBridgeDevices(const std::vector<WasapiEndpointInfo>& devices,
                                                  const std::string& preferredCaptureUtf8,
                                                  bool useAsio) {
    WinBridgeDevicePlan plan;

    // 1. Virtuális hangkábel kiválasztása
    const WasapiEndpointInfo* cap = nullptr;
    if (!preferredCaptureUtf8.empty()) {
        for (const auto& d : devices) {
            if (d.idUtf8 == preferredCaptureUtf8 || d.name == preferredCaptureUtf8) {
                cap = &d;
                break;
            }
        }
    }
    if (!cap) {
        for (const auto& d : devices) {
            if (d.isVirtual) {
                cap = &d;
                break;
            }
        }
    }
    if (!cap) {
        plan.reason = "no_virtual_device";
        return plan;
    }

    plan.captureId = cap->id;
    plan.captureIdUtf8 = cap->idUtf8;
    plan.captureName = cap->name;

    if (useAsio) {
        plan.ok = true;
        plan.outputName = "ASIO";
        return plan;
    }

    // 2. Fizikai WASAPI kimeneti eszköz kiválasztása (visszacsatolás-védelemmel)
    const WasapiEndpointInfo* out = nullptr;
    for (const auto& d : devices) {
        if (d.id != cap->id && !d.isVirtual && d.isDefault) {
            out = &d;
            break;
        }
    }
    if (!out) {
        for (const auto& d : devices) {
            if (d.id != cap->id && !d.isVirtual) {
                out = &d;
                break;
            }
        }
    }
    if (!out) {
        plan.reason = "no_output_device";
        return plan;
    }
    if (out->id == cap->id) {
        plan.reason = "loop_prevented";
        return plan;
    }

    plan.outputId = out->id;
    plan.outputName = out->name;
    plan.ok = true;
    return plan;
}

class WinLiveBridge {
public:
    WinLiveBridge()
        : loopback_("live-loopback"),
          output_(std::make_unique<player::WasapiPlayerOutput>("live-wasapi"),
                  std::make_unique<player::AsioPlayerOutput>()) {}

    ~WinLiveBridge() { stop(); }

    WinLiveBridge(const WinLiveBridge&) = delete;
    WinLiveBridge& operator=(const WinLiveBridge&) = delete;

    void start(player::ExclusiveDsp* dsp, std::function<bool()> isPlayerActive) {
        std::lock_guard<std::mutex> lk(mu_);
        if (started_) return;
        dsp_ = dsp;
        isPlayerActive_ = std::move(isPlayerActive);
        output_.setExclusiveDsp(dsp_);
        quit_ = false;
        started_ = true;
        watchThread_ = std::thread(&WinLiveBridge::watchLoop, this);
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!started_) return;
            quit_ = true;
        }
        cv_.notify_all();
        if (watchThread_.joinable()) watchThread_.join();
        teardownPipeline();
        std::lock_guard<std::mutex> lk(mu_);
        started_ = false;
    }

    void setOptions(const audioopt::AudioOptions& o) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            opts_ = o;
            wake_ = true;
        }
        cv_.notify_all();
    }

    /** Szinkron felfüggesztés, mielőtt a beépített lejátszó (FilePlayer) kinyitná a DAC-ot. */
    void suspendForPlayer() {
        suspendedForPlayer_.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> plk(pipeMu_);
        teardownPipelineLocked();
    }

    /** A beépített lejátszó bezárása után a figyelőszál újranyithatja a rendszerhang-hidat. */
    void resumeAfterPlayer() {
        suspendedForPlayer_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(mu_);
            wake_ = true;
        }
        cv_.notify_all();
    }

    bool isActive() const { return active_.load(std::memory_order_acquire); }
    uint32_t actualRate() const { return active_.load(std::memory_order_acquire) ? appliedDstRate_.load(std::memory_order_relaxed) : 0; }

    /** A telepített virtuális kábelek listája JSON-tömbként (caps.virtualCables). */
    static std::string virtualCablesJson() {
        std::string j = "[";
        bool first = true;
        for (const auto& d : WasapiLoopback::listRenderEndpoints()) {
            if (!d.isVirtual) continue;
            if (!first) j += ",";
            first = false;
            j += "{\"id\":\"" + audioopt::jsonStringBody(d.idUtf8) + "\",\"name\":\"" +
                 audioopt::jsonStringBody(d.name) + "\",\"isDefault\":" + (d.isDefault ? "true" : "false") + "}";
        }
        j += "]";
        return j;
    }

    /** Állapot JSON mező (vesszővel kezdve, a cb.statusFields-hez). */
    std::string statusJson() const {
        const bool act = active_.load(std::memory_order_acquire);
        const auto st = act ? bridge_.status() : livebridge::BridgeStatus{};
        std::string reason, capName, outName;
        uint32_t src = 0, tgt = 0;
        {
            std::lock_guard<std::mutex> lk(statusMu_);
            reason = lastReason_;
            capName = captureName_;
            outName = outputName_;
            src = lastSrcRate_;
            tgt = lastTargetRate_;
        }
        const char* conv = act
            ? (st.engineType == 1 ? "fft64" : "libsoxr")
            : "none";
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      ",\"liveUpsample\":{\"active\":%s,\"ownEngine\":%s,\"engine\":%d,"
                      "\"srcRate\":%u,\"targetRate\":%u,\"actualDacRate\":%u,"
                      "\"latencyMs\":%.1f,\"driftPpm\":%.1f,\"underruns\":%llu,"
                      "\"reason\":\"%s\",\"converter\":\"%s\","
                      "\"captureDevice\":\"%s\",\"outputDevice\":\"%s\"}",
                      act ? "true" : "false",
                      act ? "true" : "false",
                      act ? st.engineType : 0,
                      act ? st.srcRate : src,
                      act ? st.dstRate : tgt,
                      act ? output_.actualRate() : 0u,
                      act ? st.latencyMs : 0.0,
                      act ? st.driftPpm : 0.0,
                      static_cast<unsigned long long>(act ? st.underruns : 0),
                      audioopt::jsonStringBody(reason).c_str(),
                      conv,
                      audioopt::jsonStringBody(capName).c_str(),
                      audioopt::jsonStringBody(outName).c_str());
        return buf;
    }

private:
    void setStatus(const std::string& reason, const std::string& capName, const std::string& outName,
                   uint32_t srcRate, uint32_t targetRate) {
        std::lock_guard<std::mutex> lk(statusMu_);
        lastReason_ = reason;
        captureName_ = capName;
        outputName_ = outName;
        lastSrcRate_ = srcRate;
        lastTargetRate_ = targetRate;
    }

    void teardownPipeline() {
        std::lock_guard<std::mutex> plk(pipeMu_);
        teardownPipelineLocked();
    }

    void teardownPipelineLocked() {
        if (!pipeRunning_ && !loopbackRunning_) return;
        active_.store(false, std::memory_order_release);
        output_.setActive(false);
        output_.close();
        loopback_.setFrameCallback(nullptr);
        loopback_.stop();
        loopbackRunning_ = false;
        bridge_.stop();
        pipeRunning_ = false;
        appliedSrcRate_ = 0;
        appliedDstRate_.store(0, std::memory_order_relaxed);
        appliedCapId_.clear();
        appliedOutId_.clear();
    }

    static bool sameBridgeOpts(const audioopt::AudioOptions& a, const audioopt::AudioOptions& b) {
        return a.upsampleEngine == b.upsampleEngine &&
               a.upsamplePreset == b.upsamplePreset &&
               a.upsamplePrecision == b.upsamplePrecision &&
               std::abs(a.upsamplePhase - b.upsamplePhase) < 1e-4 &&
               std::abs(a.upsamplePassbandEnd - b.upsamplePassbandEnd) < 1e-4 &&
               std::abs(a.upsampleStopbandBegin - b.upsampleStopbandBegin) < 1e-4 &&
               std::abs(a.upsampleHeadroomDb - b.upsampleHeadroomDb) < 1e-3f &&
               a.upsampleThreads == b.upsampleThreads &&
               a.dedicatedCore == b.dedicatedCore &&
               a.asio == b.asio &&
               a.asioDriver == b.asioDriver;
    }

    void watchLoop() {
        for (;;) {
            audioopt::AudioOptions o;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait_for(lk, std::chrono::milliseconds(1000), [&] { return wake_ || quit_; });
                if (quit_) return;
                wake_ = false;
                o = opts_;
            }

            const bool playerBusy = suspendedForPlayer_.load(std::memory_order_acquire) ||
                                    (isPlayerActive_ && isPlayerActive_());
            if (playerBusy || !o.upsampleSystemAudio || o.upsampleRule == audioopt::UpsampleRule::Off) {
                teardownPipeline();
                setStatus(playerBusy ? "player_active" : "off", "", "", 0, 0);
                continue;
            }

            const auto devices = WasapiLoopback::listRenderEndpoints();
            const WinBridgeDevicePlan plan = chooseWinBridgeDevices(devices, o.systemCaptureDevice, o.asio);
            if (!plan.ok) {
                teardownPipeline();
                setStatus(plan.reason, plan.captureName, plan.outputName, 0, 0);
                continue;
            }

            std::lock_guard<std::mutex> plk(pipeMu_);
            // Ha közben a lejátszó elindult, azonnal engedjük el
            if (suspendedForPlayer_.load(std::memory_order_acquire) || (isPlayerActive_ && isPlayerActive_())) {
                teardownPipelineLocked();
                setStatus("player_active", "", "", 0, 0);
                continue;
            }

            // 1. Virtuális kábel loopback indítása / átváltása
            if (!loopbackRunning_ || appliedCapId_ != plan.captureId) {
                if (pipeRunning_) teardownPipelineLocked();
                loopback_.setTargetDeviceId(plan.captureId);
                loopback_.setFrameCallback([this](const float* lr, size_t frames, uint32_t) {
                    if (active_.load(std::memory_order_relaxed)) bridge_.pushInput(lr, frames);
                });
                loopback_.start();
                loopbackRunning_ = true;
                appliedCapId_ = plan.captureId;
                // Rövid várakozás, hogy a loopback beolvassa a kábel mintavételi frekvenciáját
                for (int i = 0; i < 20 && !loopback_.active(); ++i) Sleep(25);
            }

            const uint32_t srcRate = loopback_.sampleRate();
            if (!loopback_.active() || srcRate < 8000) {
                setStatus("waiting_capture", plan.captureName, plan.outputName, srcRate, 0);
                continue;
            }

            // 2. Cél mintavétel kiszámítása a valódi DAC támogatott rátái alapján
            output_.setExclusive(true);
            output_.setAsio(o.asio, o.asioDriver);
            if (!o.asio) output_.wasapi().setTargetDeviceId(plan.outputId);
            const auto supported = output_.supportedRates();
            std::string reason;
            const uint32_t dstRate = UpsampleEngine::selectTargetRate(
                srcRate, o.upsampleRule, supported, o.upsampleCustom,
                /*bypassUpsample=*/false, /*isDsd=*/false, &reason, o.upsampleMaxRate);

            if (dstRate <= srcRate) {
                if (pipeRunning_) {
                    active_.store(false, std::memory_order_release);
                    output_.setActive(false);
                    output_.close();
                    bridge_.stop();
                    pipeRunning_ = false;
                }
                setStatus(reason.empty() ? "same_rate" : reason, plan.captureName, plan.outputName, srcRate, dstRate);
                continue;
            }

            const bool needReopen = !pipeRunning_ || appliedSrcRate_ != srcRate ||
                                    appliedDstRate_.load(std::memory_order_relaxed) != dstRate ||
                                    appliedOutId_ != plan.outputId ||
                                    !sameBridgeOpts(appliedOpts_, o);
            if (!needReopen) {
                setStatus("", plan.captureName, plan.outputName, srcRate, dstRate);
                continue;
            }

            // 3. Újranyitás az új mintavétellel / motorral
            active_.store(false, std::memory_order_release);
            output_.setActive(false);
            output_.close();
            bridge_.stop();

            if (!bridge_.configure(srcRate, dstRate, o) || !bridge_.start()) {
                pipeRunning_ = false;
                setStatus("bridge_failed", plan.captureName, plan.outputName, srcRate, dstRate);
                continue;
            }
            std::string err;
            if (!output_.open(dstRate, [this](float* dst, size_t n) { bridge_.pullOutput(dst, n); }, &err)) {
                bridge_.stop();
                pipeRunning_ = false;
                setStatus(err.empty() ? "open_failed" : err, plan.captureName, plan.outputName, srcRate, dstRate);
                std::cerr << "[Felmintavételezés/Win] A kimenet nem nyitható meg (" << plan.outputName
                          << ", " << dstRate << " Hz): " << err << "\n";
                continue;
            }

            appliedSrcRate_ = srcRate;
            appliedDstRate_.store(dstRate, std::memory_order_relaxed);
            appliedOutId_ = plan.outputId;
            appliedOpts_ = o;
            pipeRunning_ = true;
            active_.store(true, std::memory_order_release);
            output_.setActive(true);
            setStatus("", plan.captureName, plan.outputName, srcRate, dstRate);
            std::cout << "[Felmintavételezés/Win] Rendszerhang aktív: " << plan.captureName << " (" << srcRate
                      << " Hz) -> " << plan.outputName << " (" << dstRate << " Hz, "
                      << (o.upsampleEngine == 1 ? "Aqulify FFT64" : "libsoxr")
                      << ").\n";
        }
    }

    player::ExclusiveDsp* dsp_ = nullptr;
    std::function<bool()> isPlayerActive_;
    WasapiLoopback loopback_;
    livebridge::LiveUpsampleBridge bridge_;
    player::BasicWinPlayerOutput<player::WasapiPlayerOutput> output_;

    std::mutex mu_;
    std::condition_variable cv_;
    audioopt::AudioOptions opts_;
    bool started_ = false;
    bool wake_ = false;
    bool quit_ = false;
    std::thread watchThread_;

    std::atomic<bool> suspendedForPlayer_{false};
    std::atomic<bool> active_{false};
    std::atomic<uint32_t> appliedDstRate_{0};

    std::mutex pipeMu_;
    bool loopbackRunning_ = false;
    bool pipeRunning_ = false;
    uint32_t appliedSrcRate_ = 0;
    std::wstring appliedCapId_, appliedOutId_;
    audioopt::AudioOptions appliedOpts_;

    mutable std::mutex statusMu_;
    std::string lastReason_ = "off";
    std::string captureName_, outputName_;
    uint32_t lastSrcRate_ = 0, lastTargetRate_ = 0;
};

} // namespace ee_analyzer::win
