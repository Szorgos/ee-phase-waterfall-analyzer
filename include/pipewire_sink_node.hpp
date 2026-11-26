#pragma once
/**
 * @file pipewire_sink_node.hpp
 * @brief Aqulify virtuális hangeszköz PipeWire alatt.
 *
 * Két pw_stream (ugyanaz a felépítés, mint a PipeWire saját module-filter-chain-jénél):
 *  - "Aqulify" virtuális kimeneti eszköz (media.class = Audio/Sink): ide játszanak az alkalmazások,
 *    ha a hangbeállításokban ezt választod (vagy ez az alapértelmezett kimenet);
 *  - lejátszó stream (Stream/Output/Audio), amelyet a munkamenet-kezelő (WirePlumber) automatikusan
 *    a valódi hangkártyára / DAC-ra köt.
 * A közös node.link-group miatt a WirePlumber a kettőt soha nem köti egymásba (nincs visszacsatolás).
 * A feldolgozás a lejátszó stream valós idejű szálán fut (a capture stream ezt indítja), egy
 * quantumnyi többletkésleltetés nélkül.
 *
 * Több kimenet (2–4 hangkártya): a további kimenetek külön lejátszó streamek, megadott céleszközzel
 * (target.object). A közös node.group miatt a PipeWire egy ütemezőhöz (driverhez) rendeli őket, a többi
 * hangkártya követőként, adaptív újramintavételezéssel fut, így az órajelek eltérése nem halmozódik.
 * A fő hangszál a kimenetek jelét gyűrűpufferekbe írja; a további streamek két quantumnyi előtöltéssel olvassák.
 * A kimenetek közti időeltolódás így legfeljebb két quantum, és indításonként eltérhet (független zónáknál nem
 * hallható; keresztváltónál a pontos igazítás még nincs megoldva).
 * A céleszköz eltűnésekor a stream nem kapcsolódik át máshová (node.dont-reconnect).
 *
 * Haladó hangkimenet (audio_options.hpp): a pufferméret a csomópontok node.latency kérése (futás közben is
 * változtatható; a gráf a legkisebb kérést veszi). Ébren tartásnál egy csendes, nem passzív hangfolyam szól a saját
 * virtuális eszközünkre, így a lánc és a hangkártya akkor sem alszik el, ha éppen senki nem játszik.
 */
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "aether_chain.hpp"
#include "audio_options.hpp"
#include "live_upsample_bridge.hpp"
#include "multi_output.hpp"
#include "aq_i18n.hpp"
#include "param_block.hpp"
#include "pw_live_upsample_policy.hpp"
#include "pw_reconnect_policy.hpp"
#include "tap_ring.hpp"

namespace ee_analyzer {

/**
 * A PipeWire lezárása a folyamat legvégén: a main() elején deklarálva ez szűnik meg utoljára, így a többi PipeWire-t
 * használó objektum (pl. a beépített lejátszó saját hurka) még lezárás előtt leáll.
 */
struct PwDeinitGuard {
    bool armed = false;   // a pw_init() megtörtént (AetherSinkNode::start)
    ~PwDeinitGuard() { if (armed) pw_deinit(); }
};

class AetherSinkNode {
public:
    struct pw_main_loop* loop = nullptr;
    struct pw_context* context = nullptr;
    struct pw_core* core = nullptr;
    struct pw_stream* capture = nullptr;
    struct pw_stream* playback = nullptr;
    struct spa_hook captureListener{};
    struct spa_hook playbackListener{};
    struct spa_hook coreListener{};

    AetherChain chain;
    StereoTapRing<65536> visualizerTap;
    ParamBlock params;

    std::atomic<uint32_t> sampleRate{48000};
    std::atomic<uint32_t> graphRate{0};
    uint32_t actualGraphRate() const {
        const uint32_t tr = targetSinkRate_.load(std::memory_order_relaxed);
        if (tr > 0) return tr;
        const uint32_t g = graphRate.load(std::memory_order_relaxed);
        return g > 0 ? g : sampleRate.load(std::memory_order_relaxed);
    }
    std::atomic<bool> playbackStreaming{false};
    std::atomic<bool> coreError{false};

    // Hibajelentés-diagnosztika („nincs hang”): hová megy a kimenet, milyen állapotú a kimeneti stream, és mikor jött
    // utoljára nem néma hang az Aqulify eszközre. A valós idejű szálak csak atomikus értéket írnak.
    std::atomic<int> playbackState{PW_STREAM_STATE_UNCONNECTED};
    std::atomic<int64_t> lastInputSignalNs{0};   // CLOCK_MONOTONIC; 0: még nem jött hang
    /** A fő kimenet céleszközének node.name-je; üres: a PipeWire alapértelmezett kimenete. */
    std::string outputTarget() const { std::lock_guard<std::mutex> lk(diagMu_); return outputTarget_; }
    /** A kimeneti stream állapota szövegesen (a jelentéshez). */
    const char* playbackStateName() const {
        switch (playbackState.load(std::memory_order_relaxed)) {
            case PW_STREAM_STATE_ERROR: return "error";
            case PW_STREAM_STATE_UNCONNECTED: return "unconnected";
            case PW_STREAM_STATE_CONNECTING: return "connecting";
            case PW_STREAM_STATE_PAUSED: return "paused";
            case PW_STREAM_STATE_STREAMING: return "streaming";
            default: return "unknown";
        }
    }
    /** Hány ms-a jött utoljára nem néma hang az Aqulify eszközre (-1: az indulás óta még nem). */
    int64_t inputSignalAgeMs() const {
        const int64_t t = lastInputSignalNs.load(std::memory_order_relaxed);
        return t > 0 ? (monoNs() - t) / 1000000 : -1;
    }
    // Őrszem (beépített hibajelentés): a kimeneti visszahívás csak időbélyeget ír (valós idejű szál); jelzést csak
    // szóló hangfolyamnál várunk, és kizárólagos ALSA mód alatt (amikor a DAC a PipeWire alól elvész) nem.
    std::atomic<bool> externalExclusiveActive_{false};
    std::atomic<bool> outputHeartbeatExpected_{false};
    aq::wd::Heartbeat outputHeartbeat{ "pw-output", &outputHeartbeatExpected_ };

    void updateOutputHeartbeatExpected() {
        outputHeartbeatExpected_.store(
            playbackStreaming.load(std::memory_order_acquire) && !externalExclusiveActive_.load(std::memory_order_acquire),
            std::memory_order_release);
    }

    /** Kizárólagos ALSA mód jelzése: amíg aktív, a pw-output őrszem nem riaszt; elengedéskor újracsatlakozunk, ha kell. */
    void setExternalExclusiveActive(bool active) {
        const bool prev = externalExclusiveActive_.exchange(active, std::memory_order_acq_rel);
        updateOutputHeartbeatExpected();
        if (prev && !active && loop && targetEvent_) {
            pw_loop_signal_event(pw_main_loop_get_loop(loop), targetEvent_);
        }
    }

    /** Van-e legalább egy valódi (nem saját, nem dummy/auto_null) külső Audio/Sink a regiszterben. */
    bool hasPhysicalSink() const { return physicalSinkCount_.load(std::memory_order_acquire) > 0; }

    /** A fő kimenet valódi fizikai hangkártyára van-e kötve (streaming vagy passzív paused; nem hibás, nem dummy). */
    bool hasReadyPhysicalOutput() const {
        if (physicalSinkCount_.load(std::memory_order_acquire) <= 0) return false;
        const int st = playbackState.load(std::memory_order_acquire);
        if (st != PW_STREAM_STATE_STREAMING && st != PW_STREAM_STATE_PAUSED) return false;
        const std::string t = outputTarget();
        return !t.empty() && !pw_reconnect::isDummyOrNullSink(t);
    }

    static constexpr int kExtraZones = multiout::kMaxOutputs - 1;
    static constexpr uint32_t kZoneRing = 32768;      // kb. 0,7 s 48 kHz-en
    static constexpr uint32_t kScratch = 16384;       // a legnagyobb feldolgozott quantum
    static constexpr int kKeepAlivePauseMs = 1500;    // forrás-követésnél új külső hangfolyamkor ennyi ideig nincs ébren tartás

    /** Egy további kimenet (2.–4.): lejátszó stream a céleszköz felé és a gyűrűpuffere. */
    struct ZoneStream {
        AetherSinkNode* owner = nullptr;
        int index = 0;                                // 1–3 (a 0. a fő kimenet)
        struct pw_stream* stream = nullptr;
        struct spa_hook listener{};
        multiout::StereoSpsc<kZoneRing> ring;
        std::string target;                           // a céleszköz node.name-je (csak a fő hurok szála)
        std::atomic<bool> fresh{false};               // új stream: az elavult puffertartalom eldobandó
        std::atomic<bool> streaming{false};
        std::atomic<uint32_t> underruns{0};
        std::atomic<uint32_t> availMin{~0u}, availMax{0}, reads{0}, lastN{0};   // diagnosztika: töltöttség olvasáskor
        bool primed = false;                          // csak a stream saját szála
        int createdQuality = -1;                      // az újramintavételezés minősége a létrehozáskor
    };

    explicit AetherSinkNode(uint32_t initialRate = 48000) {
        outputHeartbeat.setLabel("pw-output: process");
        sampleRate.store(initialRate);
        chain.setSampleRate(initialRate);
        chain.startWorker();
        multi_.setExternalMainChain(&chain);
        multi_.setSampleRate(initialRate);
        multi_.startWorkers();
        inL_.assign(kScratch, 0.0f);
        inR_.assign(kScratch, 0.0f);
        capInterleaved_.assign(kScratch * 2, 0.0f);
        playInterleaved_.assign(kScratch * 2, 0.0f);
        for (int k = 0; k < kExtraZones; ++k) {
            zones_[k] = std::make_unique<ZoneStream>();
            zones_[k]->owner = this;
            zones_[k]->index = k + 1;
            zoneL_[k].assign(kScratch, 0.0f);
            zoneR_[k].assign(kScratch, 0.0f);
        }
    }

    /**
     * Több kimenet beállítása (bármely szálról). targets[k]: a (k+1). kimenet céleszköze (node.name; üres: nincs).
     * A streamek a fő hurok szálán jönnek létre vagy szűnnek meg; a DSP-beállítás a hangszálon lép életbe.
     */
    void setMultiOutput(const multiout::MultiConfig& cfg, const std::array<std::string, kExtraZones>& targets) {
        multiCfg_.write(cfg);
        {
            std::lock_guard<std::mutex> lk(zoneMutex_);
            for (int k = 0; k < kExtraZones; ++k) {
                const bool used = cfg.enabled && k + 1 < cfg.numOutputs;
                wantedTargets_[k] = used ? targets[k] : std::string();
            }
        }
        if (loop && zoneEvent_) pw_loop_signal_event(pw_main_loop_get_loop(loop), zoneEvent_);
    }

    /**
     * Pufferméret-kérés (mintában, 0: automatikus) és a hangkártya ébren tartása (bármely szálról). A változás a fő
     * hurok szálán lép életbe; az újramintavételezés minőségének változásakor a további kimenetek újraépülnek.
     * autoFrames: a rendszer alapértelmezett quantuma. A PipeWire a már beállított node.latency-t nem törli, ezért
     * automatikusra visszaváltáskor ezt kérjük (a hangfolyam létrehozásakor automatikusnál nincs kérés).
     */
    void setAudioOptions(int bufferFrames, bool keepAwake, int autoFrames = 1024) {
        latencyReq_.store(static_cast<uint32_t>(std::max(0, bufferFrames)));
        autoLatency_.store(static_cast<uint32_t>(std::max(32, autoFrames)));
        keepAwakeWanted_.store(keepAwake);
        if (loop && optsEvent_) pw_loop_signal_event(pw_main_loop_get_loop(loop), optsEvent_);
    }

    /**
     * Forrás-követés (bármely szálról): ha be van kapcsolva és az ébren tartás is, egy új külső hangfolyam
     * indulásakor az ébren tartás rövid időre szünetel, hogy a gráf (ha tétlen) át tudjon váltani a forrás
     * mintavételére. Egyéb módban nincs szünet.
     */
    void setRateFollow(bool on) { rateFollow_.store(on); }

    /**
     * Saját motorral (libsoxr / Aqulify FFT64) történő élő rendszerhang-felmintavételezés be- vagy kikapcsolása
     * (bármely szálról). A playback stream szétválasztása / újraösszekapcsolása a PipeWire fő hurkának szálán
     * történik; a valós idejű szál nem foglal és nem zárol.
     */
    void setLiveUpsample(bool enable, uint32_t srcRate, uint32_t dstRate, const audioopt::AudioOptions& opts) {
        {
            std::lock_guard<std::mutex> lk(liveUpsMu_);
            liveUpsWanted_.enable = enable && srcRate > 0 && dstRate > srcRate;
            liveUpsWanted_.srcRate = srcRate;
            liveUpsWanted_.dstRate = dstRate;
            liveUpsWanted_.opts = opts;
        }
        if (loop && liveUpsEvent_) pw_loop_signal_event(pw_main_loop_get_loop(loop), liveUpsEvent_);
    }

    /** Aktív-e a saját motorral működő élő rendszerhang-felmintavételezés (csak ha a stream-ráták is egyeznek). */
    bool liveUpsampleActive() const {
        return liveUpsActive_.load(std::memory_order_acquire) &&
               liveBridgeRateMatched_.load(std::memory_order_acquire);
    }

    /** Szétválasztott (link-group nélküli, node.dont-reconnect=true) módban van-e a playback stream. */
    bool liveUpsampleDecoupled() const { return liveUpsActive_.load(std::memory_order_acquire); }

    /** A ráta-invariáns őrző utolsó visszalépési ok-kódja (pl. "dac_unsupported", "no_output_device", vagy üres). */
    std::string liveUpsampleGuardReason() const {
        std::lock_guard<std::mutex> lk(liveUpsGuardMu_);
        return liveUpsGuardReason_;
    }

    /** A ráta-invariáns őrző ping-pong tiltásának törlése (pl. beállítás- vagy kimenetváltáskor). */
    void resetLiveUpsampleGuard() {
        {
            std::lock_guard<std::mutex> lk(liveUpsMu_);
            liveUpsGuardResetWanted_ = true;
        }
        if (loop && liveUpsEvent_) pw_loop_signal_event(pw_main_loop_get_loop(loop), liveUpsEvent_);
    }

    /** A capture és playback streamek tényleges, PipeWire által kitárgyalt mintavételei (diagnosztika). */
    uint32_t captureRate() const { return captureRate_.load(std::memory_order_relaxed); }
    uint32_t playbackRate() const { return playbackRate_.load(std::memory_order_relaxed); }

    /** A saját motoros élő felmintavételező híd telemetriája (szálbiztos). */
    livebridge::BridgeStatus liveUpsampleStatus() const { return liveBridge_.status(); }

    /** A felhasználó által választott kimeneti eszköz (bármely szálról). Saját aqulify* vagy dummy csomópont elutasítva. */
    void setPreferredOutput(const std::string& target) {
        bool valid = false;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(targetMu_);
            if (target.empty() || (!pw_reconnect::isOwnAqulifyNode(target, sinkName_) && !pw_reconnect::isDummyOrNullSink(target))) {
                changed = (preferredTarget_ != target);
                preferredTarget_ = target;
                valid = !target.empty();
            }
        }
        // Kimenetváltáskor az élő felmintavételezés őrzője újraértékelhet (pl. hangszóró -> USB DAC)
        if (changed && loop && liveUpsEvent_) {
            pw_loop_signal_event(pw_main_loop_get_loop(loop), liveUpsEvent_);
        }
        // Ha a kimeneti stream hibás vagy még nincs fizikai célra rögzítve, ébresszük a fő hurkot az újracsatlakozáshoz
        if (valid && loop && targetEvent_) {
            const int st = playbackState.load(std::memory_order_relaxed);
            const std::string cur = outputTarget();
            if (st == PW_STREAM_STATE_ERROR || st == PW_STREAM_STATE_UNCONNECTED ||
                cur.empty() || pw_reconnect::isDummyOrNullSink(cur)) {
                pw_loop_signal_event(pw_main_loop_get_loop(loop), targetEvent_);
            }
        }
    }

    /** Külső áthelyezés (pactl move-sink-input) után az aktuális céleszköz frissítése a diagnosztikához. */
    void noteCurrentOutputTarget(const std::string& target) {
        if (target.empty() || pw_reconnect::isOwnAqulifyNode(target, sinkName_) || pw_reconnect::isDummyOrNullSink(target)) return;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(diagMu_);
            changed = (outputTarget_ != target);
            outputTarget_ = target;
        }
        if (changed && loop && liveUpsEvent_) {
            pw_loop_signal_event(pw_main_loop_get_loop(loop), liveUpsEvent_);
        }
    }

    /** A regiszterben látott legmagasabb prioritású valódi fizikai nyelő (bármely szálról). */
    std::string bestPhysicalSink(const std::string& preferred = {}) const {
        std::vector<pw_reconnect::SinkCandidate> cands;
        {
            std::lock_guard<std::mutex> lk(sinksMu_);
            cands.reserve(knownSinksById_.size());
            for (const auto& [id, ks] : knownSinksById_) cands.push_back(ks);
        }
        return pw_reconnect::resolveTargetOrFallbackByPriority(preferred, cands, sinkName_);
    }

    /** Kérés a fő huroknak: ellenőrizze a kimeneti streamet, és ha hibás / rögzítetlen, csatlakozzon újra. */
    void requestReconnectIfNeeded() {
        if (loop && targetEvent_) pw_loop_signal_event(pw_main_loop_get_loop(loop), targetEvent_);
    }

    /** A legutóbbi feldolgozott blokk hossza (a gráf tényleges quantuma) mintában. */
    uint32_t actualQuantum() const { return lastQuantum_.load(std::memory_order_relaxed); }
    /** A feldolgozás átlagos terhelése a quantum idejéhez képest (0–1, csúszó átlag). */
    float dspLoad() const { return loadAvg_.load(std::memory_order_relaxed); }
    /** A legnagyobb terhelés az előző lekérdezés óta (a lekérdezés nullázza). */
    float takeDspPeak() { return loadPeak_.exchange(0.0f, std::memory_order_relaxed); }
    /** Hányszor tartott a feldolgozás tovább, mint egy quantum ideje (akadásveszély) az indítás óta. */
    uint32_t overruns() const { return overruns_.load(std::memory_order_relaxed); }
    /** Fut-e az ébren tartó hangfolyam. */
    bool keepAwakeActive() const { return keepAliveStreaming_.load(); }
    /** Van-e egynél több aqulify_sink csomópont a PipeWire-gráfban (pl. régi, zár nélküli motor is fut). */
    bool hasDuplicateAqulifySink() const { return duplicateAqulifySink_.load(std::memory_order_relaxed); }

    /** A k-adik kimenet (1–3) saját lánca, pl. impulzusválasz egy zónának (submitIr szálbiztos). */
    AetherChain& zoneChain(int k) { return multi_.chain(k); }

    /** A további kimenetek állapota JSON-ként (a felületnek): céleszköz, szól-e, alulcsordulások. */
    std::string multiStatusJson() {
        std::lock_guard<std::mutex> lk(zoneMutex_);
        std::string s = "[";
        for (int k = 0; k < kExtraZones; ++k) {
            if (k) s += ",";
            s += "{\"output\":" + std::to_string(k + 2) + ",\"target\":\"" + wantedTargets_[k] + "\",\"streaming\":" +
                 (zones_[k]->streaming.load() ? "true" : "false") + ",\"underruns\":" + std::to_string(zones_[k]->underruns.load()) +
                 ",\"fillMin\":" + std::to_string(zones_[k]->availMin.load() == ~0u ? 0 : zones_[k]->availMin.load()) +
                 ",\"fillMax\":" + std::to_string(zones_[k]->availMax.load()) + ",\"reads\":" + std::to_string(zones_[k]->reads.load()) +
                 ",\"quantum\":" + std::to_string(zones_[k]->lastN.load()) + "}";
        }
        return s + "]";
    }

    ~AetherSinkNode() { shutdown(); }

    AetherSinkNode(const AetherSinkNode&) = delete;
    AetherSinkNode& operator=(const AetherSinkNode&) = delete;

    /** Csatlakozás a PipeWire démonhoz és a két stream létrehozása. */
    bool start(int argc, char* argv[], const std::string& sinkName = "aqulify_sink",
               const std::string& description = "Aqulify") {
        pw_init(&argc, &argv);
        loop = pw_main_loop_new(nullptr);
        if (!loop) return fail("a PipeWire eseményhurok nem hozható létre");
        context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
        if (!context) return fail("a PipeWire környezet nem hozható létre");
        core = pw_context_connect(context, nullptr, 0);
        if (!core) return fail("nem érhető el a PipeWire démon (fut a pipewire szolgáltatás?)");

        static const struct pw_core_events coreEvents = [] {
            pw_core_events e{};
            e.version = PW_VERSION_CORE_EVENTS;
            e.error = onCoreError;
            return e;
        }();
        pw_core_add_listener(core, &coreListener, &coreEvents, this);

        const std::string group = "aqulify-" + std::to_string(static_cast<long>(getpid()));

        // 1. Virtuális kimeneti eszköz (ide szólnak az alkalmazások)
        struct pw_properties* cprops = pw_properties_new(
            PW_KEY_MEDIA_CLASS, "Audio/Sink",
            PW_KEY_NODE_NAME, sinkName.c_str(),
            PW_KEY_NODE_DESCRIPTION, description.c_str(),
            PW_KEY_NODE_VIRTUAL, "true",
            PW_KEY_NODE_GROUP, group.c_str(),
            PW_KEY_NODE_LINK_GROUP, group.c_str(),
            PW_KEY_AUDIO_CHANNELS, "2",
            SPA_KEY_AUDIO_POSITION, "FL,FR",
            "device.icon-name", "audio-card",
            nullptr);
        applyLatencyProp(cprops);
        capture = pw_stream_new(core, (sinkName + "_input").c_str(), cprops);
        if (!capture) return fail("a virtuális eszköz nem hozható létre");

        static const struct pw_stream_events captureEvents = [] {
            pw_stream_events e{};
            e.version = PW_VERSION_STREAM_EVENTS;
            e.process = onCaptureProcess;
            e.param_changed = onParamChanged;
            return e;
        }();
        pw_stream_add_listener(capture, &captureListener, &captureEvents, this);
        group_ = group;
        appliedCaptureGroup_ = group;
        sinkName_ = sinkName;
        description_ = description;
        zoneEvent_ = pw_loop_add_event(pw_main_loop_get_loop(loop), onZoneEvent, this);
        optsEvent_ = pw_loop_add_event(pw_main_loop_get_loop(loop), onOptsEvent, this);
        liveUpsEvent_ = pw_loop_add_event(pw_main_loop_get_loop(loop), onLiveUpsEvent, this);
        targetEvent_ = pw_loop_add_event(pw_main_loop_get_loop(loop), onTargetEvent, this);
        appliedLatency_ = latencyReq_.load();

        // Formátum: 32 bites lebegőpontos, sztereó, nem-interleaved; a mintavételt a gráf adja (rate = 0)
        uint8_t buf[1024];
        struct spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32P;
        info.channels = 2;
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;

        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        const struct spa_pod* cparams[1] = { spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info) };
        int captureFlags = PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;
#if PW_CHECK_VERSION(0, 3, 73)
        captureFlags |= PW_STREAM_FLAG_ASYNC; // a buffereket a lejátszó stream szála veszi ki
#endif
        if (pw_stream_connect(capture, PW_DIRECTION_INPUT, PW_ID_ANY,
                              static_cast<pw_stream_flags>(captureFlags), cparams, 1) < 0)
            return fail("a virtuális eszköz nem csatlakoztatható");

        // Regiszter létrehozása még a lejátszó stream előtt: külső Stream/Output/Audio (forrás-követés)
        // és külső Audio/Sink (célválasztás, DAC visszatérés) figyelése
        struct pw_loop* l = pw_main_loop_get_loop(loop);
        registry_ = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
        if (registry_) {
            static const struct pw_registry_events regEvents = [] {
                pw_registry_events e{};
                e.version = PW_VERSION_REGISTRY_EVENTS;
                e.global = onRegistryGlobal;
                e.global_remove = onRegistryGlobalRemove;
                return e;
            }();
            pw_registry_add_listener(registry_, &registryListener_, &regEvents, this);
        }

        // 2. Lejátszó stream a valódi hangeszköz felé (hiba után a fő hurok időzítője újracsatlakoztatja)
        reconnectTimer_ = pw_loop_add_timer(l, onReconnectTimer, this);
        liveUpsGuardTimer_ = pw_loop_add_timer(l, onLiveUpsGuardTimer, this);
        if (!createAndConnectPlayback()) return fail("a lejátszó stream nem csatlakoztatható");
        if (keepAwakeWanted_.load()) createKeepAlive();

        // Az ébren tartás visszakapcsolása a szünet után (negyedmásodpercenként ellenőrzi)
        resumeTimer_ = pw_loop_add_timer(l, onResumeTimer, this);
        struct timespec iv{ 0, 250 * 1000000L };
        pw_loop_update_timer(l, resumeTimer_, &iv, &iv, false);
        return true;
    }

    /** Fő eseményhurok (blokkol, amíg quit() nem hívódik). */
    void run() { if (loop) pw_main_loop_run(loop); }

    /** Szálbiztos leállítás-kérés (szignálkezelőből is hívható). */
    void quit() { if (loop) pw_main_loop_quit(loop); }

    /** Periodikus feladat a fő hurokban (pl. beállítások mentése). */
    struct spa_source* addTimer(void (*fn)(void*, uint64_t), void* data, uint64_t intervalMs) {
        if (!loop) return nullptr;
        struct pw_loop* l = pw_main_loop_get_loop(loop);
        struct spa_source* s = pw_loop_add_timer(l, fn, data);
        struct timespec value{ static_cast<time_t>(intervalMs / 1000), static_cast<long>((intervalMs % 1000) * 1000000) };
        pw_loop_update_timer(l, s, &value, &value, false);
        return s;
    }

    void shutdown() {
        reconnect_.reset();
        liveBridgeRateMatched_.store(false, std::memory_order_release);
        liveUpsActive_.store(false, std::memory_order_release);
        if (loop) {
            struct pw_loop* l = pw_main_loop_get_loop(loop);
            if (reconnectTimer_) { pw_loop_destroy_source(l, reconnectTimer_); reconnectTimer_ = nullptr; }
            if (liveUpsGuardTimer_) { pw_loop_destroy_source(l, liveUpsGuardTimer_); liveUpsGuardTimer_ = nullptr; }
            if (resumeTimer_) { pw_loop_destroy_source(l, resumeTimer_); resumeTimer_ = nullptr; }
            if (liveUpsEvent_) { pw_loop_destroy_source(l, liveUpsEvent_); liveUpsEvent_ = nullptr; }
            if (targetEvent_) { pw_loop_destroy_source(l, targetEvent_); targetEvent_ = nullptr; }
        }
        for (auto& z : zones_) if (z && z->stream) { pw_stream_destroy(z->stream); z->stream = nullptr; }
        destroyKeepAlive();
        chain.stopWorker();
        multi_.stopWorkers();
        // Előbb a playback streamet bontjuk szálbiztosan (capture RT in-flight megvárásával), utána a capture-t
        setCaptureDecoupledProps(false, 0);
        destroyPlaybackSafe();
        if (capture) { pw_stream_destroy(capture); capture = nullptr; }
        waitRtIdle(captureInFlight_, "capture");
        liveBridge_.stop();
        unbindTargetSinkMonitor();
        if (registry_) { pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(registry_)); registry_ = nullptr; }
        {
            std::lock_guard<std::mutex> lk(sinksMu_);
            knownSinksById_.clear();
        }
        physicalSinkCount_.store(0, std::memory_order_release);
        if (core) { pw_core_disconnect(core); core = nullptr; }
        if (context) { pw_context_destroy(context); context = nullptr; }
        // a pw_deinit()-et a hívó végzi a legvégén (PwDeinitGuard): a lejátszó saját PipeWire-hurka ezután áll le
        if (loop) { pw_main_loop_destroy(loop); loop = nullptr; }
    }

private:
    uint32_t appliedParamSeq_ = 0;
    uint32_t appliedMultiSeq_ = 0;
    bool multiOn_ = false;                            // csak a hangszál
    multiout::MultiOutputEngine multi_;
    multiout::SeqBox<multiout::MultiConfig> multiCfg_;
    std::array<std::unique_ptr<ZoneStream>, kExtraZones> zones_{};
    std::array<std::string, kExtraZones> wantedTargets_{};
    std::mutex zoneMutex_;
    struct spa_source* zoneEvent_ = nullptr;
    struct spa_source* optsEvent_ = nullptr;
    std::atomic<uint32_t> latencyReq_{0};             // a kért pufferméret mintában (0: automatikus)
    std::atomic<uint32_t> autoLatency_{1024};         // automatikusra visszaváltáskor kért érték
    uint32_t appliedLatency_ = 0;                     // csak a fő hurok szála
    std::atomic<bool> keepAwakeWanted_{false};
    struct pw_stream* keepAlive_ = nullptr;           // csak a fő hurok szála
    struct spa_hook keepAliveListener_{};
    std::atomic<bool> keepAliveStreaming_{false};
    std::atomic<bool> rateFollow_{false};             // a forrást követi (setRateFollow)
    std::chrono::steady_clock::time_point keepAlivePauseUntil_{};   // csak a fő hurok szála
    struct pw_registry* registry_ = nullptr;          // csak a fő hurok szála
    struct spa_hook registryListener_{};
    struct spa_source* resumeTimer_ = nullptr;        // csak a fő hurok szála
    struct spa_source* reconnectTimer_ = nullptr;     // csak a fő hurok szála (lejátszó stream újracsatlakozás)
    struct spa_source* targetEvent_ = nullptr;        // csak a fő hurok szála (célváltozás / ALSA elengedés ébresztése)
    pw_reconnect::State reconnect_;                   // csak a fő hurok szála
    mutable std::mutex sinksMu_;
    std::unordered_map<uint32_t, pw_reconnect::SinkCandidate> knownSinksById_; // külső (nem aqulify*, nem dummy) Audio/Sink csomópontok
    std::atomic<int> physicalSinkCount_{0};
    std::mutex targetMu_;
    std::string preferredTarget_;                     // felhasználó által választott kimenet (ha van)
    mutable std::mutex diagMu_;
    std::string outputTarget_;                        // a fő kimenet ténylegesen kért céleszköze (jelentéshez)

    // Szálbiztos RT életciklus: a playback stream törlése és a liveBridge_ újrakonfigurálása csak akkor fut,
    // miután az RT data-loop callbackjei (onCaptureProcess / onPlaybackProcess) kiléptek (0 lock az RT szálon).
    std::atomic<struct pw_stream*> playbackPtr_{nullptr};
    std::atomic<bool> playbackTriggerReady_{false};
    std::atomic<int> captureInFlight_{0};
    std::atomic<int> playbackInFlight_{0};

    struct RtInFlightGuard {
        std::atomic<int>& counter;
        explicit RtInFlightGuard(std::atomic<int>& c) noexcept : counter(c) {
            counter.fetch_add(1, std::memory_order_acq_rel);
        }
        ~RtInFlightGuard() noexcept {
            counter.fetch_sub(1, std::memory_order_release);
        }
    };

    static void waitRtIdle(std::atomic<int>& counter, const char* name) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (counter.load(std::memory_order_acquire) > 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        const int rem = counter.load(std::memory_order_acquire);
        if (rem > 0) {
            std::cerr << "[PipeWire] Figyelem: a(z) " << name
                      << " RT visszahívás 200 ms után sem lépett ki (aktív: " << rem << ").\n";
        }
    }

    /** A playback stream szálbiztos megszüntetése a fő hurok szálán: előbb elrejti az RT capture elől, majd megvárja. */
    void destroyPlaybackSafe() {
        playbackTriggerReady_.store(false, std::memory_order_release);
        liveBridgeRateMatched_.store(false, std::memory_order_release);
        playbackStreaming.store(false, std::memory_order_release);
        playbackRate_.store(0, std::memory_order_relaxed);
        driverClockRate_.store(0, std::memory_order_relaxed);
        updateOutputHeartbeatExpected();
        struct pw_stream* old = playbackPtr_.exchange(nullptr, std::memory_order_acq_rel);
        playback = nullptr;
        waitRtIdle(captureInFlight_, "capture");
        if (old) {
            pw_stream_destroy(old);
        }
        waitRtIdle(playbackInFlight_, "playback");
    }
    static int64_t monoNs() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
    }
    static int64_t monoMs() { return monoNs() / 1000000; }
    /** Nem néma bemenet jelzése (valós idejű szál: csak egy atomikus írás, ha a csúcs ~−100 dBFS fölött van). */
    void noteInput(const float* a, const float* b, uint32_t n) {
        float pk = 0.0f;
        for (uint32_t i = 0; i < n; ++i) pk = std::max(pk, std::max(std::fabs(a[i]), std::fabs(b[i])));
        if (pk > 1e-5f) lastInputSignalNs.store(monoNs(), std::memory_order_relaxed);
    }
    std::vector<uint32_t> aqulifySinkIds_;            // látott aqulify_sink csomópontok (dupla példány jelzéséhez)
    std::atomic<bool> duplicateAqulifySink_{false};
    std::atomic<uint32_t> lastQuantum_{0};
    std::atomic<float> loadAvg_{0.0f}, loadPeak_{0.0f};
    std::atomic<uint32_t> overruns_{0};
    std::string group_, sinkName_, description_;
    std::vector<float> inL_, inR_;
    std::vector<float> capInterleaved_, playInterleaved_;
    std::array<std::vector<float>, kExtraZones> zoneL_, zoneR_;

    // Saját motoros (libsoxr / Aqulify FFT64 / FFT-DD) élő rendszerhang-felmintavételezés
    struct LiveUpsRequest {
        bool enable = false;
        uint32_t srcRate = 0;
        uint32_t dstRate = 0;
        audioopt::AudioOptions opts;
    };
    livebridge::LiveUpsampleBridge liveBridge_;
    std::atomic<bool> liveUpsActive_{false};          // true: capture -> liveBridge_ -> playback (szétválasztott)
    std::atomic<bool> liveBridgeRateMatched_{false};  // RT kapu: csak ha captureRate == bridgeSrc és playbackRate == bridgeDst
    std::atomic<uint32_t> captureRate_{48000};        // a capture stream aktuális mintavétele (SPA_PARAM_Format)
    std::atomic<uint32_t> playbackRate_{0};           // a playback stream aktuális mintavétele (SPA_PARAM_Format)
    std::atomic<uint32_t> driverClockRate_{0};        // a playback stream pw_stream_get_time_n órajele
    std::atomic<uint32_t> targetSinkRate_{0};         // a fizikai célszköz (pw_node) valódi SPA_PARAM_Format mintavétele
    struct pw_node* targetNodeProxy_ = nullptr;       // csak a fő hurok szála (cél-sink valódi rátájának figyelése)
    struct spa_hook targetNodeListener_{};
    uint32_t targetNodeId_ = 0;                       // csak a fő hurok szála
    std::atomic<uint32_t> liveBridgeSrcRate_{0};      // a híd konfigurált bemeneti mintavétele (RT kapuhoz)
    std::atomic<uint32_t> liveBridgeDstRate_{0};      // a híd konfigurált kimeneti mintavétele (RT kapuhoz)
    struct spa_source* liveUpsEvent_ = nullptr;       // csak a fő hurok szála
    struct spa_source* liveUpsGuardTimer_ = nullptr;  // csak a fő hurok szála (ráta-invariáns őrző időzítő)
    std::mutex liveUpsMu_;
    LiveUpsRequest liveUpsWanted_;                    // liveUpsMu_ alatt
    bool liveUpsGuardResetWanted_ = false;            // liveUpsMu_ alatt
    LiveUpsRequest liveUpsApplied_;                   // csak a fő hurok szála
    std::string liveUpsAppliedTarget_;                // a szétválasztott playback stream aktuális fizikai célja (fő hurok)
    uint32_t appliedCaptureForceRate_ = 0;            // a capture streamen beállított node.force-rate (csak a fő hurok szála)
    std::string appliedCaptureGroup_;                 // a capture stream aktuális node.group tulajdonsága (csak a fő hurok szála)
    std::string dummyDriverGroup_{"pipewire.dummy"};  // a PipeWire Dummy-Driver csoportneve (regiszterből frissítve)
    pw_live_ups::RateInvariantGuard liveUpsGuard_;    // csak a fő hurok szála
    mutable std::mutex liveUpsGuardMu_;
    std::string liveUpsGuardReason_;                  // liveUpsGuardMu_ alatt

    /**
     * A capture stream (aqulify_sink) node.force-rate és node.group tulajdonságainak frissítése (stream törlése nélkül).
     * Szétválasztott módban a Dummy-Driver csoportjába (pipewire.dummy) és a forrás natív mintavételére (srcRate)
     * helyezi a capture streamet, így teljesen külön órajel-szigeten fut a fizikai DAC-ot hajtó playback streamtől.
     * Közvetlen módba visszatérve visszaállítja a saját aqulify-<pid> csoportot és a 0 (szabad) force-rate-et.
     */
    void setCaptureDecoupledProps(bool decoupled, uint32_t srcRate) {
        if (!capture) return;
        const uint32_t wantRate = decoupled ? srcRate : 0;
        const std::string wantGroup = decoupled ? dummyDriverGroup_ : group_;
        if (wantRate == appliedCaptureForceRate_ && wantGroup == appliedCaptureGroup_) return;
        appliedCaptureForceRate_ = wantRate;
        appliedCaptureGroup_ = wantGroup;
        char rateBuf[24];
        std::snprintf(rateBuf, sizeof(rateBuf), "%u", wantRate);
        struct spa_dict_item items[2] = {
            SPA_DICT_ITEM_INIT(PW_KEY_NODE_FORCE_RATE, rateBuf),
            SPA_DICT_ITEM_INIT(PW_KEY_NODE_GROUP, wantGroup.c_str()),
        };
        struct spa_dict dict = SPA_DICT_INIT(items, 2);
        pw_stream_update_properties(capture, &dict);
    }

    /** A cél fizikai Audio/Sink csomópont valódi SPA_PARAM_Format mintavételének figyelése (fő hurok szála). */
    void unbindTargetSinkMonitor() {
        if (targetNodeProxy_) {
            spa_hook_remove(&targetNodeListener_);
            pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(targetNodeProxy_));
            targetNodeProxy_ = nullptr;
        }
        targetNodeId_ = 0;
        targetSinkRate_.store(0, std::memory_order_relaxed);
    }

    void bindTargetSinkMonitorByName(const std::string& targetName) {
        if (!registry_ || targetName.empty()) {
            unbindTargetSinkMonitor();
            return;
        }
        uint32_t foundId = 0;
        {
            std::lock_guard<std::mutex> lk(sinksMu_);
            for (const auto& [id, ks] : knownSinksById_) {
                if (ks.name == targetName) { foundId = id; break; }
            }
        }
        if (foundId == 0) {
            unbindTargetSinkMonitor();
            return;
        }
        if (targetNodeProxy_ && targetNodeId_ == foundId) return;
        unbindTargetSinkMonitor();
        auto* node = static_cast<struct pw_node*>(
            pw_registry_bind(registry_, foundId, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
        if (!node) return;
        static const struct pw_node_events nodeEvents = [] {
            pw_node_events e{};
            e.version = PW_VERSION_NODE_EVENTS;
            e.param = onTargetNodeParam;
            return e;
        }();
        spa_zero(targetNodeListener_);
        pw_node_add_listener(node, &targetNodeListener_, &nodeEvents, this);
        uint32_t paramIds[1] = { SPA_PARAM_Format };
        pw_node_subscribe_params(node, paramIds, 1);
        targetNodeProxy_ = node;
        targetNodeId_ = foundId;
    }

    static void onTargetNodeParam(void* data, int, uint32_t id, uint32_t, uint32_t, const struct spa_pod* param) {
        auto* self = static_cast<AetherSinkNode*>(data);
        if (!param || id != SPA_PARAM_Format) return;
        struct spa_audio_info_raw info{};
        if (spa_format_audio_raw_parse(param, &info) >= 0 && info.rate > 0) {
            self->targetSinkRate_.store(info.rate, std::memory_order_relaxed);
            if (self->liveUpsActive_.load(std::memory_order_acquire) && self->loop && self->liveUpsEvent_) {
                pw_loop_signal_event(pw_main_loop_get_loop(self->loop), self->liveUpsEvent_);
            }
        }
    }

    void setLiveUpsGuardReason(const std::string& reason) {
        std::lock_guard<std::mutex> lk(liveUpsGuardMu_);
        liveUpsGuardReason_ = reason;
    }

    void updateLiveBridgeRateGate() {
        const bool ok = liveUpsActive_.load(std::memory_order_acquire) &&
                        pw_live_ups::ratesMatchBridge(
                            captureRate_.load(std::memory_order_relaxed),
                            playbackRate_.load(std::memory_order_relaxed),
                            liveBridgeSrcRate_.load(std::memory_order_relaxed),
                            liveBridgeDstRate_.load(std::memory_order_relaxed));
        liveBridgeRateMatched_.store(ok, std::memory_order_release);
    }

    void startLiveUpsGuardTimer() {
        if (!loop || !liveUpsGuardTimer_) return;
        struct pw_loop* l = pw_main_loop_get_loop(loop);
        struct timespec iv{ 0, pw_live_ups::kGuardCheckIntervalMs * 1000000L };
        pw_loop_update_timer(l, liveUpsGuardTimer_, &iv, &iv, false);
    }

    void stopLiveUpsGuardTimer() {
        if (!loop || !liveUpsGuardTimer_) return;
        struct pw_loop* l = pw_main_loop_get_loop(loop);
        struct timespec zero{ 0, 0 };
        pw_loop_update_timer(l, liveUpsGuardTimer_, &zero, &zero, false);
    }

    /** Aktuálisan elérhető, érvényes külső fizikai céleszköz (üres, ha nincs biztonságos cél). */
    std::string resolvePhysicalTargetForLiveUps() {
        std::string pref;
        {
            std::lock_guard<std::mutex> lk(targetMu_);
            pref = preferredTarget_;
        }
        std::vector<pw_reconnect::SinkCandidate> known;
        {
            std::lock_guard<std::mutex> lk(sinksMu_);
            known.reserve(knownSinksById_.size());
            for (const auto& [id, ks] : knownSinksById_) known.push_back(ks);
        }
        std::string target = pw_reconnect::resolveTargetOrFallbackByPriority(pref, known, sinkName_);
        if (!pw_live_ups::hasValidPhysicalTarget(target, sinkName_)) {
            const std::string cur = outputTarget();
            if (pw_live_ups::hasValidPhysicalTarget(cur, sinkName_)) {
                for (const auto& ks : known) {
                    if (ks.name == cur) { target = cur; break; }
                }
            }
        }
        return pw_live_ups::hasValidPhysicalTarget(target, sinkName_) ? target : std::string();
    }

    /** Ráta-eltérés vagy hiányzó fizikai cél esetén tiszta visszalépés közvetlen módba (0,5x/2x védelem). */
    void fallbackLiveUpsToDirect(const pw_live_ups::FallbackKey& key, const char* reasonCode,
                                 const char* detailHu, pw_live_ups::GuardVerdict verdict) {
        const uint32_t capR = captureRate_.load(std::memory_order_relaxed);
        const uint32_t playR = playbackRate_.load(std::memory_order_relaxed);
        const uint32_t tSinkR = targetSinkRate_.load(std::memory_order_relaxed);
        const uint32_t clkR = tSinkR > 0 ? tSinkR : driverClockRate_.load(std::memory_order_relaxed);
        const uint32_t bSrc = liveUpsApplied_.srcRate;
        const uint32_t bDst = liveUpsApplied_.dstRate;
        stopLiveUpsGuardTimer();
        unbindTargetSinkMonitor();
        liveUpsGuard_.stopSession();
        if (key.valid()) liveUpsGuard_.blockKey(key, reasonCode, verdict);
        else liveUpsGuard_.setTransientReason(reasonCode);
        setLiveUpsGuardReason(reasonCode);
        liveBridgeRateMatched_.store(false, std::memory_order_release);
        liveBridgeSrcRate_.store(0, std::memory_order_relaxed);
        liveBridgeDstRate_.store(0, std::memory_order_relaxed);
        liveUpsActive_.store(false, std::memory_order_release);
        setCaptureDecoupledProps(false, 0);
        destroyPlaybackSafe();
        liveBridge_.stop();
        liveUpsApplied_ = LiveUpsRequest{};
        liveUpsAppliedTarget_.clear();
        createAndConnectPlayback();
        std::cout << "[Felmintavételezés] Saját motor (rendszerhang) visszalépett közvetlen módba ("
                  << (detailHu && *detailHu ? detailHu : reasonCode)
                  << "; kért: " << bSrc << " -> " << bDst
                  << " Hz, capture=" << capR << " Hz, playback=" << playR
                  << " Hz, DAC=" << clkR << " Hz).\n";
    }

    /** A fő hurok szálán: az aktív szétválasztott híd ráta-invariánsának ellenőrzése. */
    void evaluateLiveUpsGuard() {
        if (!liveUpsActive_.load(std::memory_order_acquire) || !liveUpsApplied_.enable) return;
        updateLiveBridgeRateGate();
        const std::string target = resolvePhysicalTargetForLiveUps();
        if (!pw_live_ups::hasValidPhysicalTarget(target, sinkName_)) {
            fallbackLiveUpsToDirect(pw_live_ups::FallbackKey{}, "no_output_device",
                                    "nincs érvényes fizikai kimeneti eszköz", pw_live_ups::GuardVerdict::Ok);
            return;
        }
        const uint32_t tSinkR = targetSinkRate_.load(std::memory_order_relaxed);
        const uint32_t effectiveDacClock = tSinkR > 0 ? tSinkR : driverClockRate_.load(std::memory_order_relaxed);
        const auto verdict = liveUpsGuard_.evaluate(
            monoMs(),
            captureRate_.load(std::memory_order_relaxed),
            playbackRate_.load(std::memory_order_relaxed),
            effectiveDacClock,
            playbackStreaming.load(std::memory_order_acquire));
        if (verdict == pw_live_ups::GuardVerdict::FallbackCaptureMismatch ||
            verdict == pw_live_ups::GuardVerdict::FallbackPlaybackMismatch ||
            verdict == pw_live_ups::GuardVerdict::FallbackDacRateMismatch) {
            const pw_live_ups::FallbackKey key{
                target, liveUpsApplied_.srcRate, liveUpsApplied_.dstRate,
                static_cast<int>(liveUpsApplied_.opts.rateMode)
            };
            fallbackLiveUpsToDirect(key,
                                    pw_live_ups::verdictReasonCode(verdict),
                                    pw_live_ups::verdictDetailHu(verdict),
                                    verdict);
        }
    }

    static void onLiveUpsGuardTimer(void* data, uint64_t) {
        static_cast<AetherSinkNode*>(data)->evaluateLiveUpsGuard();
    }

    static bool sameLiveUpsConfig(const LiveUpsRequest& a, const LiveUpsRequest& b) {
        return a.enable == b.enable && a.srcRate == b.srcRate && a.dstRate == b.dstRate &&
               a.opts.rateMode == b.opts.rateMode &&
               a.opts.upsampleEngine == b.opts.upsampleEngine &&
               a.opts.upsamplePreset == b.opts.upsamplePreset &&
               a.opts.upsamplePrecision == b.opts.upsamplePrecision &&
               std::abs(a.opts.upsamplePhase - b.opts.upsamplePhase) < 1e-4 &&
               std::abs(a.opts.upsamplePassbandEnd - b.opts.upsamplePassbandEnd) < 1e-4 &&
               std::abs(a.opts.upsampleStopbandBegin - b.opts.upsampleStopbandBegin) < 1e-4 &&
               std::abs(a.opts.upsampleHeadroomDb - b.opts.upsampleHeadroomDb) < 1e-3f &&
               a.opts.upsampleThreads == b.opts.upsampleThreads &&
               a.opts.dedicatedCore == b.opts.dedicatedCore;
    }

    /** A fő hurok szálán: a saját motoros rendszerhang-felmintavételezés be-/kikapcsolása és újrakonfigurálása. */
    static void onLiveUpsEvent(void* data, uint64_t) {
        auto* self = static_cast<AetherSinkNode*>(data);
        LiveUpsRequest want;
        bool resetGuard = false;
        {
            std::lock_guard<std::mutex> lk(self->liveUpsMu_);
            want = self->liveUpsWanted_;
            resetGuard = self->liveUpsGuardResetWanted_;
            self->liveUpsGuardResetWanted_ = false;
        }
        if (resetGuard) {
            self->liveUpsGuard_.clearBlocked();
            self->setLiveUpsGuardReason("");
        }

        const std::string target = self->resolvePhysicalTargetForLiveUps();
        const bool targetChanged = pw_live_ups::needsPlaybackRebuildForTarget(
            self->liveUpsApplied_.enable, self->liveUpsAppliedTarget_, target);
        self->liveUpsGuard_.onTargetChanged(target);
        if (want.enable) {
            self->liveUpsGuard_.onRateModeChanged(static_cast<int>(want.opts.rateMode));
        }

        if (want.enable) {
            // Visszacsatolás-védelem: érvényes külső fizikai kimenet nélkül nem lépünk szétválasztott módba
            if (!pw_live_ups::hasValidPhysicalTarget(target, self->sinkName_)) {
                if (self->liveUpsApplied_.enable) {
                    self->fallbackLiveUpsToDirect(pw_live_ups::FallbackKey{}, "no_output_device",
                                                  "nincs érvényes fizikai kimeneti eszköz",
                                                  pw_live_ups::GuardVerdict::Ok);
                } else {
                    self->liveUpsGuard_.setTransientReason("no_output_device");
                    self->setLiveUpsGuardReason("no_output_device");
                }
                return;
            }
            // Ping-pong védelem: ha erre a (kimenet, forrás->cél, rateMode) négyesre az őrző már visszalépett,
            // nem indítjuk újra ciklikusan, csak megtartjuk az őszinte ok-kódot
            const pw_live_ups::FallbackKey key{
                target, want.srcRate, want.dstRate, static_cast<int>(want.opts.rateMode)
            };
            if (self->liveUpsGuard_.isBlocked(key)) {
                self->setLiveUpsGuardReason(self->liveUpsGuard_.lastReason());
                return;
            }
            if (!targetChanged && sameLiveUpsConfig(want, self->liveUpsApplied_)) {
                self->evaluateLiveUpsGuard();
                return;
            }
        } else {
            if (!self->liveUpsApplied_.enable) {
                if (self->liveUpsGuard_.lastReason() == "no_output_device") {
                    self->liveUpsGuard_.setTransientReason("");
                    self->setLiveUpsGuardReason("");
                }
                return;
            }
        }

        // Ha mindkét állapot aktív, a kimeneti eszköz ÉS a célmintavétel (dstRate) nem változott, a playback stream maradhat,
        // de a liveBridge_ újrakonfigurálása előtt megvárjuk a futó RT capture/playback hívásokat!
        if (!targetChanged && want.enable && self->liveUpsApplied_.enable && want.dstRate == self->liveUpsApplied_.dstRate) {
            self->liveBridgeRateMatched_.store(false, std::memory_order_release);
            self->liveUpsActive_.store(false, std::memory_order_release);
            self->waitRtIdle(self->captureInFlight_, "capture");
            self->waitRtIdle(self->playbackInFlight_, "playback");
            if (self->liveBridge_.configure(want.srcRate, want.dstRate, want.opts) && self->liveBridge_.start()) {
                self->liveUpsApplied_ = want;
                self->setCaptureDecoupledProps(true, want.srcRate);
                self->liveBridgeSrcRate_.store(want.srcRate, std::memory_order_relaxed);
                self->liveBridgeDstRate_.store(want.dstRate, std::memory_order_relaxed);
                self->liveUpsGuard_.resetSession(monoMs(), want.srcRate, want.dstRate);
                self->setLiveUpsGuardReason("");
                self->liveUpsActive_.store(true, std::memory_order_release);
                self->updateLiveBridgeRateGate();
                self->startLiveUpsGuardTimer();
                std::cout << "[Felmintavételezés] Saját motor (rendszerhang) frissítve: " << want.srcRate << " -> "
                          << want.dstRate << " Hz ("
                          << audioopt::upsampleEngineLabel(want.opts.upsampleEngine)
                          << ").\n";
                self->evaluateLiveUpsGuard();
                return;
            }
        }

        // Üzemmód-, kimenet- vagy célmintavétel-váltás: a playback streamet szálbiztosan leállítjuk, majd az új módban/céllal nyitjuk újra
        self->stopLiveUpsGuardTimer();
        self->unbindTargetSinkMonitor();
        self->liveUpsGuard_.stopSession();
        self->liveBridgeRateMatched_.store(false, std::memory_order_release);
        self->liveBridgeSrcRate_.store(0, std::memory_order_relaxed);
        self->liveBridgeDstRate_.store(0, std::memory_order_relaxed);
        self->liveUpsActive_.store(false, std::memory_order_release);
        self->destroyPlaybackSafe();
        if (want.enable) {
            self->liveUpsApplied_ = want;
            self->liveUpsAppliedTarget_ = target;
            const bool bridgeOk = self->liveBridge_.configure(want.srcRate, want.dstRate, want.opts) &&
                                  self->liveBridge_.start();
            if (!bridgeOk) {
                self->setCaptureDecoupledProps(false, 0);
                self->liveUpsApplied_ = LiveUpsRequest{};
                self->liveUpsAppliedTarget_.clear();
                self->createAndConnectPlayback();
                std::cerr << "[Felmintavételezés] A saját motoros híd nem indítható; visszatérés alapmódba.\n";
                return;
            }
            // A capture stream (aqulify_sink) külön órajel-szigeten (Dummy-Driver) marad a forrás natív mintavételén
            self->setCaptureDecoupledProps(true, want.srcRate);
            self->bindTargetSinkMonitorByName(target);
            self->liveBridgeSrcRate_.store(want.srcRate, std::memory_order_relaxed);
            self->liveBridgeDstRate_.store(want.dstRate, std::memory_order_relaxed);
            self->liveUpsGuard_.resetSession(monoMs(), want.srcRate, want.dstRate);
            self->setLiveUpsGuardReason("");
            self->liveUpsActive_.store(true, std::memory_order_release);
            self->updateLiveBridgeRateGate();
            if (!self->createAndConnectPlayback()) {
                self->unbindTargetSinkMonitor();
                self->liveUpsGuard_.stopSession();
                self->liveBridgeRateMatched_.store(false, std::memory_order_release);
                self->liveBridgeSrcRate_.store(0, std::memory_order_relaxed);
                self->liveBridgeDstRate_.store(0, std::memory_order_relaxed);
                self->liveUpsActive_.store(false, std::memory_order_release);
                self->setCaptureDecoupledProps(false, 0);
                self->waitRtIdle(self->captureInFlight_, "capture");
                self->liveBridge_.stop();
                self->liveUpsApplied_ = LiveUpsRequest{};
                self->liveUpsAppliedTarget_.clear();
                self->createAndConnectPlayback();
                std::cerr << "[Felmintavételezés] A szétválasztott kimeneti stream nem indítható; visszatérés alapmódba.\n";
                return;
            }
            self->startLiveUpsGuardTimer();
            std::cout << "[Felmintavételezés] Saját motor (rendszerhang): " << want.srcRate << " -> "
                      << want.dstRate << " Hz ("
                      << audioopt::upsampleEngineLabel(want.opts.upsampleEngine)
                      << ", kimenet: " << target << ").\n";
        } else {
            self->setCaptureDecoupledProps(false, 0);
            self->liveBridge_.stop();
            self->liveUpsApplied_ = LiveUpsRequest{};
            self->liveUpsAppliedTarget_.clear();
            self->createAndConnectPlayback();
            std::cout << "[Felmintavételezés] Saját motor (rendszerhang): ki (visszatérés közvetlen módba).\n";
        }
    }

    /** A fő hurok szálán: a kívánt céleszközöknek megfelelő streamek létrehozása / megszüntetése. */
    static void onZoneEvent(void* data, uint64_t) {
        auto* self = static_cast<AetherSinkNode*>(data);
        std::array<std::string, kExtraZones> want;
        {
            std::lock_guard<std::mutex> lk(self->zoneMutex_);
            want = self->wantedTargets_;
        }
        for (int k = 0; k < kExtraZones; ++k) {
            ZoneStream& z = *self->zones_[k];
            if (z.stream && z.target == want[k] && z.createdQuality == audioopt::resampleQuality().load()) continue;
            if (z.stream) {
                pw_stream_destroy(z.stream);
                z.stream = nullptr;
                z.streaming = false;
                std::cout << "[PipeWire] " << (k + 2) << ". kimenet leállítva (" << z.target << ")\n";
            }
            z.target = want[k];
            if (!z.target.empty()) self->createZoneStream(z);
        }
    }

    void createZoneStream(ZoneStream& z) {
        const std::string name = sinkName_ + "_zone" + std::to_string(z.index + 1) + "_output";
        const std::string desc = description_ + " – " + std::to_string(z.index + 1) + ". kimenet";
        struct pw_properties* props = pw_properties_new(
            PW_KEY_MEDIA_CLASS, "Stream/Output/Audio",
            PW_KEY_NODE_NAME, name.c_str(),
            PW_KEY_NODE_DESCRIPTION, desc.c_str(),
            PW_KEY_NODE_GROUP, group_.c_str(),           // egy ütemező: az órajelek eltérését a PipeWire kezeli
            PW_KEY_NODE_LINK_GROUP, group_.c_str(),      // soha nem kötődik a saját virtuális eszközünkre
            PW_KEY_TARGET_OBJECT, z.target.c_str(),
            PW_KEY_NODE_DONT_RECONNECT, "true",          // a céleszköz eltűnésekor nem szól máshol
            PW_KEY_NODE_PASSIVE, "true",
            PW_KEY_AUDIO_CHANNELS, "2",
            SPA_KEY_AUDIO_POSITION, "FL,FR",
            PW_KEY_MEDIA_ROLE, "DSP",
            nullptr);
        z.createdQuality = audioopt::resampleQuality().load();
        if (z.createdQuality >= 0) pw_properties_set(props, "resample.quality", std::to_string(z.createdQuality).c_str());
        applyLatencyProp(props);
        z.stream = pw_stream_new(core, name.c_str(), props);
        if (!z.stream) {
            std::cerr << "[PipeWire] a(z) " << (z.index + 1) << ". kimenet streamje nem hozható létre\n";
            return;
        }
        static const struct pw_stream_events zoneEvents = [] {
            pw_stream_events e{};
            e.version = PW_VERSION_STREAM_EVENTS;
            e.process = onZoneProcess;
            e.state_changed = onZoneState;
            return e;
        }();
        pw_stream_add_listener(z.stream, &z.listener, &zoneEvents, &z);
        uint8_t buf[1024];
        struct spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32P;
        info.channels = 2;
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        const struct spa_pod* params[1] = { spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info) };
        z.fresh = true;
        z.underruns = 0;
        if (pw_stream_connect(z.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                              static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                                           PW_STREAM_FLAG_RT_PROCESS),
                              params, 1) < 0) {
            std::cerr << "[PipeWire] a(z) " << (z.index + 1) << ". kimenet nem csatlakoztatható\n";
            pw_stream_destroy(z.stream);
            z.stream = nullptr;
            return;
        }
        std::cout << "[PipeWire] " << (z.index + 1) << ". kimenet: " << z.target << "\n";
    }

    static void onZoneState(void* data, enum pw_stream_state, enum pw_stream_state state, const char* error) {
        auto* z = static_cast<ZoneStream*>(data);
        z->streaming = (state == PW_STREAM_STATE_STREAMING);
        if (state == PW_STREAM_STATE_ERROR)
            std::cerr << "[PipeWire] " << (z->index + 1) << ". kimenet hiba: " << (error ? error : "") << "\n";
    }

    /**
     * A további kimenet saját valós idejű szála: a gyűrűpufferből olvas, két quantumnyi előtöltéssel. Hiánynál
     * csend és újratöltés (alulcsordulás); túl sok tárolt adatnál a legrégebbit eldobja (a késleltetés korlátja).
     */
    static void onZoneProcess(void* data) {
        auto* z = static_cast<ZoneStream*>(data);
        struct pw_buffer* out = pw_stream_dequeue_buffer(z->stream);
        if (!out) return;
        struct spa_buffer* ob = out->buffer;
        if (ob->n_datas < 2 || !ob->datas[0].data || !ob->datas[1].data) {
            pw_stream_queue_buffer(z->stream, out);
            return;
        }
        float* L = static_cast<float*>(ob->datas[0].data);
        float* R = static_cast<float*>(ob->datas[1].data);
        const uint32_t maxFrames = std::min(ob->datas[0].maxsize, ob->datas[1].maxsize) / sizeof(float);
        uint32_t n = out->requested ? std::min<uint32_t>(static_cast<uint32_t>(out->requested), maxFrames) : 0;
        if (n == 0) n = std::min<uint32_t>(1024, maxFrames);

        if (z->fresh.exchange(false)) z->primed = false;
        const uint32_t avail = z->ring.available();
        if (avail < z->availMin.load(std::memory_order_relaxed)) z->availMin.store(avail, std::memory_order_relaxed);
        if (avail > z->availMax.load(std::memory_order_relaxed)) z->availMax.store(avail, std::memory_order_relaxed);
        z->reads.fetch_add(1, std::memory_order_relaxed);
        z->lastN.store(n, std::memory_order_relaxed);
        // Előtöltés: két quantum; ha ennél jóval több gyűlt össze (pl. a stream egy ideig nem futott), a régi adat
        // eldobódik, így a késleltetés korlátos marad
        const uint32_t target = 2 * n;
        uint32_t got = 0;
        if (!z->primed && avail >= target) z->primed = true;
        if (z->primed) {
            if (avail > target + 4 * n) z->ring.skip(avail - target);
            got = z->ring.read(L, R, n);
            if (got < n) {
                z->underruns.fetch_add(1, std::memory_order_relaxed);
                z->primed = false;
            }
        }
        if (got < n) {
            std::memset(L + got, 0, (n - got) * sizeof(float));
            std::memset(R + got, 0, (n - got) * sizeof(float));
        }
        for (uint32_t i = 0; i < 2; ++i) {
            ob->datas[i].chunk->offset = 0;
            ob->datas[i].chunk->size = n * sizeof(float);
            ob->datas[i].chunk->stride = sizeof(float);
        }
        pw_stream_queue_buffer(z->stream, out);
    }

    /** A kért pufferméret a létrehozandó hangfolyam tulajdonságai közé (node.latency = N/mintavétel). */
    void applyLatencyProp(struct pw_properties* props) const {
        const uint32_t n = latencyReq_.load();
        if (n > 0) pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u", n, sampleRate.load());
    }

    /** A fő hurok szálán: a pufferméret frissítése a meglévő hangfolyamokon, az ébren tartás be / ki. */
    static void onOptsEvent(void* data, uint64_t) {
        auto* self = static_cast<AetherSinkNode*>(data);
        const uint32_t n = self->latencyReq_.load();
        if (n != self->appliedLatency_) {
            self->appliedLatency_ = n;
            char val[32];
            std::snprintf(val, sizeof(val), "%u/%u", n > 0 ? n : self->autoLatency_.load(), self->sampleRate.load());
            struct spa_dict_item item = SPA_DICT_ITEM_INIT(PW_KEY_NODE_LATENCY, val);
            struct spa_dict dict = SPA_DICT_INIT(&item, 1);
            for (struct pw_stream* st : { self->capture, self->playback, self->keepAlive_ })
                if (st) pw_stream_update_properties(st, &dict);
            for (auto& z : self->zones_) if (z && z->stream) pw_stream_update_properties(z->stream, &dict);
            std::cout << "[PipeWire] Pufferméret: " << (n ? std::to_string(n) + " minta" : std::string("automatikus")) << "\n";
        }
        const bool want = self->keepAwakeWanted_.load();
        if (want && !self->keepAlive_ && !self->keepAlivePaused()) self->createKeepAlive();
        else if (!want && self->keepAlive_) self->destroyKeepAlive();
        onZoneEvent(data, 0);   // az újramintavételezés minősége változhatott
    }

    /**
     * Ébren tartás: csendes, nem passzív hangfolyam a saját virtuális eszközünkre (nem a link-group tagja, így a
     * WirePlumber rá köti). Amíg szól, a lánc fut, és a hangkártya nem függesztődik fel.
     */
    void createKeepAlive(bool silent = false) {
        if (keepAlive_ || !core) return;
        struct pw_properties* props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_NODE_NAME, (sinkName_ + "_keepalive").c_str(),
            PW_KEY_NODE_DESCRIPTION, (description_ + " – ébren tartás").c_str(),
            PW_KEY_APP_NAME, "Aqulify",
            PW_KEY_TARGET_OBJECT, sinkName_.c_str(),
            PW_KEY_NODE_DONT_RECONNECT, "true",
            PW_KEY_AUDIO_CHANNELS, "2",
            SPA_KEY_AUDIO_POSITION, "FL,FR",
            nullptr);
        applyLatencyProp(props);
        keepAlive_ = pw_stream_new(core, (sinkName_ + "_keepalive").c_str(), props);
        if (!keepAlive_) return;
        static const struct pw_stream_events events = [] {
            pw_stream_events e{};
            e.version = PW_VERSION_STREAM_EVENTS;
            e.process = onKeepAliveProcess;
            e.state_changed = onKeepAliveState;
            return e;
        }();
        spa_zero(keepAliveListener_);
        pw_stream_add_listener(keepAlive_, &keepAliveListener_, &events, this);
        uint8_t buf[1024];
        struct spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32P;
        info.channels = 2;
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        const struct spa_pod* params[1] = { spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info) };
        if (pw_stream_connect(keepAlive_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                              static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                                           PW_STREAM_FLAG_RT_PROCESS),
                              params, 1) < 0) {
            pw_stream_destroy(keepAlive_);
            keepAlive_ = nullptr;
            std::cerr << "[PipeWire] az ébren tartó hangfolyam nem csatlakoztatható\n";
            return;
        }
        if (!silent) std::cout << "[PipeWire] Hangkártya ébren tartása: be\n";
    }

    void destroyKeepAlive(bool silent = false) {
        if (!keepAlive_) return;
        pw_stream_destroy(keepAlive_);
        keepAlive_ = nullptr;
        keepAliveStreaming_ = false;
        if (!silent) std::cout << "[PipeWire] Hangkártya ébren tartása: ki\n";
    }

    /** Ébren tartás szünetel-e (forrás-követés: új külső hangfolyam). A fő hurok szálán hívandó. */
    bool keepAlivePaused() const { return std::chrono::steady_clock::now() < keepAlivePauseUntil_; }

    /** Lejátszó stream létrehozása és csatlakoztatása (a fő hurok szálán: induláskor és újracsatlakozáskor). */
    bool createAndConnectPlayback() {
        if (!core) return false;
        std::string pref;
        {
            std::lock_guard<std::mutex> lk(targetMu_);
            pref = preferredTarget_;
        }
        std::vector<pw_reconnect::SinkCandidate> known;
        {
            std::lock_guard<std::mutex> lk(sinksMu_);
            known.reserve(knownSinksById_.size());
            for (const auto& [id, ks] : knownSinksById_) known.push_back(ks);
        }
        const std::string target = pw_reconnect::resolveTargetOrFallbackByPriority(pref, known, sinkName_);

        const bool decoupled = pw_live_ups::canUseDecoupledPlayback(
            liveUpsApplied_.enable, liveUpsApplied_.dstRate, target, sinkName_);
        struct pw_properties* pprops = pw_properties_new(
            PW_KEY_MEDIA_CLASS, "Stream/Output/Audio",
            PW_KEY_NODE_NAME, (sinkName_ + "_output").c_str(),
            PW_KEY_NODE_DESCRIPTION, aq_i18n::fmt(aq_i18n::Msg::pw_outputNode, { { "name", description_ } }).c_str(),
            PW_KEY_NODE_PASSIVE, decoupled ? "false" : "true",
            PW_KEY_AUDIO_CHANNELS, "2",
            SPA_KEY_AUDIO_POSITION, "FL,FR",
            PW_KEY_MEDIA_ROLE, "DSP",
            nullptr);
        if (!decoupled) {
            pw_properties_set(pprops, PW_KEY_NODE_GROUP, group_.c_str());
            pw_properties_set(pprops, PW_KEY_NODE_LINK_GROUP, group_.c_str());
        } else {
            // Szétválasztott mód (2 külön PipeWire driver-sziget): a node.group és a node.link-group is elhagyva,
            // így a capture (Dummy-Driver @ srcRate) és a playback (fizikai DAC @ dstRate) független órajelen fut.
            // Visszacsatolás-védelem: target garantáltan validált fizikai nyelő + node.dont-reconnect = true.
            pw_properties_setf(pprops, PW_KEY_NODE_FORCE_RATE, "%u", liveUpsApplied_.dstRate);
            pw_properties_set(pprops, PW_KEY_NODE_DONT_RECONNECT, "true");
        }
        if (!target.empty()) pw_properties_set(pprops, PW_KEY_TARGET_OBJECT, target.c_str());
        { std::lock_guard<std::mutex> lk(diagMu_); outputTarget_ = target; }
        applyLatencyProp(pprops);
        // Lokális mutatóval hozzuk létre és csatlakoztatjuk; trigger engedélyezés csak sikeres connect után!
        struct pw_stream* s = pw_stream_new(core, (sinkName_ + "_output").c_str(), pprops);
        if (!s) return false;

        static const struct pw_stream_events playbackEvents = [] {
            pw_stream_events e{};
            e.version = PW_VERSION_STREAM_EVENTS;
            e.process = onPlaybackProcess;
            e.state_changed = onPlaybackState;
            e.param_changed = onPlaybackParamChanged;
            return e;
        }();
        spa_zero(playbackListener);
        pw_stream_add_listener(s, &playbackListener, &playbackEvents, this);

        uint8_t buf[1024];
        struct spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32P;
        info.channels = 2;
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
        if (decoupled) info.rate = liveUpsApplied_.dstRate;
        struct spa_pod_builder b2 = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        const struct spa_pod* pparams[1] = { spa_format_audio_raw_build(&b2, SPA_PARAM_EnumFormat, &info) };
        int pflags = PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;
        if (!decoupled) pflags |= PW_STREAM_FLAG_TRIGGER;
        playbackTriggerReady_.store(false, std::memory_order_release);
        playback = s;
        playbackPtr_.store(s, std::memory_order_release);
        if (pw_stream_connect(s, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                              static_cast<pw_stream_flags>(pflags),
                              pparams, 1) < 0) {
            playbackPtr_.store(nullptr, std::memory_order_release);
            playback = nullptr;
            waitRtIdle(playbackInFlight_, "playback");
            pw_stream_destroy(s);
            return false;
        }
        playbackTriggerReady_.store(!decoupled, std::memory_order_release);
        return true;
    }

    /** Egyszeri (one-shot) újracsatlakozási időzítő ütemezése a PipeWire fő hurkában. */
    void scheduleReconnect(int delayMs) {
        if (!loop || !reconnectTimer_ || delayMs <= 0) return;
        struct pw_loop* l = pw_main_loop_get_loop(loop);
        struct timespec val{ static_cast<time_t>(delayMs / 1000), static_cast<long>((delayMs % 1000) * 1000000L) };
        struct timespec zero{ 0, 0 };
        pw_loop_update_timer(l, reconnectTimer_, &val, &zero, false);
    }

    void cancelReconnectTimer() {
        if (!loop || !reconnectTimer_) return;
        struct pw_loop* l = pw_main_loop_get_loop(loop);
        struct timespec zero{ 0, 0 };
        pw_loop_update_timer(l, reconnectTimer_, &zero, &zero, false);
    }

    /** Fő hurkos időzítő: a hibás lejátszó stream megszüntetése és újranyitása (nem az onPlaybackState-ből!). */
    static void onReconnectTimer(void* data, uint64_t) {
        auto* self = static_cast<AetherSinkNode*>(data);
        if (!self->reconnect_.pending() || !self->core) return;
        if (self->liveUpsActive_.load(std::memory_order_acquire)) {
            const std::string target = self->resolvePhysicalTargetForLiveUps();
            if (!pw_live_ups::canUseDecoupledPlayback(self->liveUpsApplied_.enable, self->liveUpsApplied_.dstRate, target, self->sinkName_)) {
                self->fallbackLiveUpsToDirect({}, "no_output_device",
                                              "nincs érvényes fizikai kimeneti eszköz", pw_live_ups::GuardVerdict::Ok);
                return;
            }
            self->liveUpsAppliedTarget_ = target;
            self->bindTargetSinkMonitorByName(target);
        }
        self->destroyPlaybackSafe();
        if (!self->createAndConnectPlayback()) {
            const auto a = self->reconnect_.onAttemptFailed();
            if (a.schedule) self->scheduleReconnect(a.delayMs);
        }
    }

    /** Fő hurkos esemény: új céleszköz beállításakor vagy ALSA elengedésekor hibás/rögzítetlen stream újracsatlakoztatása. */
    static void onTargetEvent(void* data, uint64_t) {
        auto* self = static_cast<AetherSinkNode*>(data);
        if (!self->core || self->externalExclusiveActive_.load(std::memory_order_acquire)) return;
        const int st = self->playbackState.load(std::memory_order_relaxed);
        const std::string cur = self->outputTarget();
        const bool broken = self->reconnect_.pending() || st == PW_STREAM_STATE_ERROR || st == PW_STREAM_STATE_UNCONNECTED;
        const bool unpinnedWithPhysical = !self->knownSinksById_.empty() &&
                                          (cur.empty() || pw_reconnect::isDummyOrNullSink(cur));
        if (broken || unpinnedWithPhysical) {
            const auto a = self->reconnect_.requestImmediate(pw_reconnect::kNewSinkDelayMs);
            if (a.schedule) self->scheduleReconnect(a.delayMs);
        }
    }

    /**
     * Új csomópont a gráfban (a fő hurok szálán):
     * - külső Audio/Sink: nyilvántartjuk (célválasztáshoz), és ha a lejátszó stream újracsatlakozásra vár,
     *   vagy még nem rögzült valódi fizikai kimenetre (pl. indulás / auto_null), azonnal ütemezzük az újranyitást;
     * - külső Stream/Output/Audio: forrás-követésnél az ébren tartás rövid szüneteltetése (némán;
     *   csak a tényleges mintavétel-váltás naplózódik az onPlaybackParamChanged-ben).
     */
    static void onRegistryGlobal(void* data, uint32_t id, uint32_t, const char* type, uint32_t, const struct spa_dict* props) {
        auto* self = static_cast<AetherSinkNode*>(data);
        if (!type || !props || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
        const char* cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        const char* nodeName = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        if (nodeName && std::strcmp(nodeName, "Dummy-Driver") == 0) {
            if (const char* dg = spa_dict_lookup(props, PW_KEY_NODE_GROUP); dg && *dg) {
                self->dummyDriverGroup_ = dg;
            }
        }
        if (!cls) return;
        if (std::strcmp(cls, "Audio/Sink") == 0) {
            // Második Aqulify virtuális eszköz észlelése (régi, flock nélküli 2.7.0–2.8.3 motor)
            if (nodeName && self->sinkName_ == nodeName) {
                if (std::find(self->aqulifySinkIds_.begin(), self->aqulifySinkIds_.end(), id) == self->aqulifySinkIds_.end())
                    self->aqulifySinkIds_.push_back(id);
                self->duplicateAqulifySink_.store(self->aqulifySinkIds_.size() > 1, std::memory_order_relaxed);
            } else if (nodeName && !pw_reconnect::isOwnAqulifyNode(nodeName, self->sinkName_) &&
                       !pw_reconnect::isDummyOrNullSink(nodeName)) {
                int prio = 0;
                if (const char* ps = spa_dict_lookup(props, PW_KEY_PRIORITY_SESSION)) prio = std::atoi(ps);
                else if (const char* pd = spa_dict_lookup(props, PW_KEY_PRIORITY_DRIVER)) prio = std::atoi(pd);
                int count = 0;
                {
                    std::lock_guard<std::mutex> lk(self->sinksMu_);
                    self->knownSinksById_[id] = { nodeName, prio };
                    count = static_cast<int>(self->knownSinksById_.size());
                }
                self->physicalSinkCount_.store(count, std::memory_order_release);
                {
                    std::string pref;
                    { std::lock_guard<std::mutex> lk(self->targetMu_); pref = self->preferredTarget_; }
                    const bool needLiveReeval =
                        self->liveUpsGuard_.lastReason() == "no_output_device" ||
                        (self->liveUpsActive_.load(std::memory_order_acquire) &&
                         (self->targetNodeId_ == 0 || (!pref.empty() && pref == nodeName && self->liveUpsAppliedTarget_ != pref)));
                    if (needLiveReeval && self->loop && self->liveUpsEvent_) {
                        pw_loop_signal_event(pw_main_loop_get_loop(self->loop), self->liveUpsEvent_);
                    }
                }
                if (!self->externalExclusiveActive_.load(std::memory_order_acquire)) {
                    const auto a = self->reconnect_.onNewExternalSink(nodeName, self->sinkName_);
                    if (a.schedule) {
                        self->scheduleReconnect(a.delayMs);
                    } else {
                        std::string pref;
                        { std::lock_guard<std::mutex> lk(self->targetMu_); pref = self->preferredTarget_; }
                        const std::string curTarget = self->outputTarget();
                        const int st = self->playbackState.load(std::memory_order_relaxed);
                        const bool notRunning = (st == PW_STREAM_STATE_ERROR || st == PW_STREAM_STATE_UNCONNECTED);
                        const bool dummyTarget = pw_reconnect::isDummyOrNullSink(curTarget);
                        const bool prefReturned = !pref.empty() && pref == nodeName && curTarget != pref;
                        // Működő (PAUSED/STREAMING) streamet üres curTarget miatt ne kössünk át önkényesen:
                        // azt a defaultThread rögzíti a tényleges sinkre, kivéve ha hibás/dummy vagy a választott DAC tért vissza.
                        if (notRunning || dummyTarget || prefReturned) {
                            const auto req = self->reconnect_.requestImmediate(pw_reconnect::kNewSinkDelayMs);
                            if (req.schedule) self->scheduleReconnect(req.delayMs);
                        }
                    }
                }
            }
            return;
        }
        if (std::strcmp(cls, "Stream/Output/Audio") != 0) return;
        // a saját streamjeink (fő kimenet, zónák, lejátszó, ébren tartás) nem számítanak: nevük „aqulify…”,
        // az ébren tartó és a lejátszó alkalmazásneve „Aqulify” (az application.process.id nem megbízható)
        const char* appName = spa_dict_lookup(props, PW_KEY_APP_NAME);
        if (nodeName && std::strncmp(nodeName, "aqulify", 7) == 0) return;
        if (appName && std::strcmp(appName, "Aqulify") == 0) return;
        if (!self->rateFollow_.load() || !self->keepAwakeWanted_.load()) return;
        self->keepAlivePauseUntil_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(kKeepAlivePauseMs);
        if (self->keepAlive_) self->destroyKeepAlive(/*silent=*/true);
    }

    static void onRegistryGlobalRemove(void* data, uint32_t id) {
        auto* self = static_cast<AetherSinkNode*>(data);
        if (id == self->targetNodeId_) self->unbindTargetSinkMonitor();
        bool erased = false;
        int count = 0;
        {
            std::lock_guard<std::mutex> lk(self->sinksMu_);
            erased = self->knownSinksById_.erase(id) > 0;
            count = static_cast<int>(self->knownSinksById_.size());
        }
        if (erased) {
            self->physicalSinkCount_.store(count, std::memory_order_release);
            if (self->liveUpsActive_.load(std::memory_order_acquire) && self->loop && self->liveUpsEvent_) {
                pw_loop_signal_event(pw_main_loop_get_loop(self->loop), self->liveUpsEvent_);
            }
        }
        auto it = std::find(self->aqulifySinkIds_.begin(), self->aqulifySinkIds_.end(), id);
        if (it != self->aqulifySinkIds_.end()) {
            self->aqulifySinkIds_.erase(it);
            self->duplicateAqulifySink_.store(self->aqulifySinkIds_.size() > 1, std::memory_order_relaxed);
        }
    }

    /** Az ébren tartás visszakapcsolása a szünet végén (a fő hurok szálán). */
    static void onResumeTimer(void* data, uint64_t) {
        auto* self = static_cast<AetherSinkNode*>(data);
        if (self->keepAwakeWanted_.load() && !self->keepAlive_ && !self->keepAlivePaused())
            self->createKeepAlive(/*silent=*/true);
    }

    static void onKeepAliveState(void* data, enum pw_stream_state, enum pw_stream_state state, const char*) {
        static_cast<AetherSinkNode*>(data)->keepAliveStreaming_ = (state == PW_STREAM_STATE_STREAMING);
    }

    static void onKeepAliveProcess(void* data) {
        auto* self = static_cast<AetherSinkNode*>(data);
        struct pw_buffer* out = pw_stream_dequeue_buffer(self->keepAlive_);
        if (!out) return;
        struct spa_buffer* ob = out->buffer;
        const uint32_t maxFrames = ob->n_datas ? ob->datas[0].maxsize / sizeof(float) : 0;
        uint32_t n = out->requested ? std::min<uint32_t>(static_cast<uint32_t>(out->requested), maxFrames) : 0;
        if (n == 0) n = std::min<uint32_t>(1024, maxFrames);
        for (uint32_t i = 0; i < ob->n_datas; ++i) {
            if (ob->datas[i].data) std::memset(ob->datas[i].data, 0, std::min<uint32_t>(n * sizeof(float), ob->datas[i].maxsize));
            ob->datas[i].chunk->offset = 0;
            ob->datas[i].chunk->size = n * sizeof(float);
            ob->datas[i].chunk->stride = sizeof(float);
        }
        pw_stream_queue_buffer(self->keepAlive_, out);
    }

    /** A feldolgozás terhelése: az eltelt idő a quantum idejéhez képest (valós idejű szál, foglalás nélkül). */
    void noteLoad(const struct timespec& t0, uint32_t n) {
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        const double dt = static_cast<double>(t1.tv_sec - t0.tv_sec) + 1e-9 * static_cast<double>(t1.tv_nsec - t0.tv_nsec);
        const uint32_t sr = sampleRate.load(std::memory_order_relaxed);
        if (n == 0 || sr == 0) return;
        const float load = static_cast<float>(dt * sr / n);
        lastQuantum_.store(n, std::memory_order_relaxed);
        const float avg = loadAvg_.load(std::memory_order_relaxed);
        loadAvg_.store(avg + 0.05f * (load - avg), std::memory_order_relaxed);
        if (load > loadPeak_.load(std::memory_order_relaxed)) loadPeak_.store(load, std::memory_order_relaxed);
        if (load > 1.0f) overruns_.fetch_add(1, std::memory_order_relaxed);
    }

    bool fail(const char* what) {
        std::cerr << "[Hiba] PipeWire: " << what << "\n";
        return false;
    }

    static void onCoreError(void* data, uint32_t id, int seq, int res, const char* message) {
        auto* self = static_cast<AetherSinkNode*>(data);
        (void)seq;
        // Ha a lejátszó stream épp újracsatlakozásra vár (nincs céleszköz), a köztes ENOENT próbákat nem naplózzuk újra
        if (!(res == -ENOENT && self->reconnect_.errorLogged())) {
            std::cerr << "[PipeWire] hiba (id " << id << "): " << (message ? message : "") << " (" << spa_strerror(res) << ")\n";
        }
        if (id == PW_ID_CORE && res == -EPIPE) {
            self->coreError = true;
            self->quit(); // a démon leállt: kilépünk, a systemd újraindít
        }
    }

    static void onParamChanged(void* data, uint32_t id, const struct spa_pod* param) {
        auto* self = static_cast<AetherSinkNode*>(data);
        if (!param || id != SPA_PARAM_Format) return;
        struct spa_audio_info_raw info{};
        if (spa_format_audio_raw_parse(param, &info) >= 0 && info.rate > 0) {
            self->captureRate_.store(info.rate, std::memory_order_relaxed);
            // Szétválasztott felmintavételezésnél a DSP lánc a playback mintavételén fut, nem a capture-én;
            // a ráta-invariáns kapu azonnal frissül, és a fő hurok őrzője megvizsgálja az esetleges eltérést
            if (self->liveUpsActive_.load(std::memory_order_acquire)) {
                self->updateLiveBridgeRateGate();
                if (self->loop && self->liveUpsEvent_)
                    pw_loop_signal_event(pw_main_loop_get_loop(self->loop), self->liveUpsEvent_);
            } else {
                self->sampleRate.store(info.rate, std::memory_order_relaxed);
            }
        }
    }

    static void onPlaybackParamChanged(void* data, uint32_t id, const struct spa_pod* param) {
        auto* self = static_cast<AetherSinkNode*>(data);
        if (!param || id != SPA_PARAM_Format) return;
        struct spa_audio_info_raw info{};
        if (spa_format_audio_raw_parse(param, &info) >= 0 && info.rate > 0) {
            self->playbackRate_.store(info.rate, std::memory_order_relaxed);
            const uint32_t prev = self->graphRate.exchange(info.rate, std::memory_order_relaxed);
            self->sampleRate.store(info.rate, std::memory_order_relaxed);
            if (prev > 0 && prev != info.rate) {
                std::cout << "[PipeWire] Mintavétel-követés: " << prev << " → " << info.rate << " Hz\n";
            }
            if (self->liveUpsActive_.load(std::memory_order_acquire)) {
                self->updateLiveBridgeRateGate();
                if (self->loop && self->liveUpsEvent_)
                    pw_loop_signal_event(pw_main_loop_get_loop(self->loop), self->liveUpsEvent_);
            }
        }
    }

    static void onPlaybackState(void* data, enum pw_stream_state old, enum pw_stream_state state, const char* error) {
        auto* self = static_cast<AetherSinkNode*>(data);
        (void)old;
        const bool streaming = (state == PW_STREAM_STATE_STREAMING);
        self->playbackStreaming.store(streaming, std::memory_order_release);
        self->updateOutputHeartbeatExpected();
        self->playbackState.store(static_cast<int>(state), std::memory_order_relaxed);
        if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
            self->playbackTriggerReady_.store(false, std::memory_order_release);
        }
        if (state == PW_STREAM_STATE_ERROR) {
            // A streamet itt nem szabad törölni/újranyitni: csak jelezzük, és a fő hurok időzítője végzi el.
            const auto a = self->reconnect_.onError();
            if (a.logError) std::cerr << "[PipeWire] lejátszó stream hiba: " << (error ? error : "") << "\n";
            if (a.schedule) self->scheduleReconnect(a.delayMs);
        } else if (streaming) {
            if (!self->liveUpsActive_.load(std::memory_order_acquire)) {
                self->playbackTriggerReady_.store(true, std::memory_order_release);
            }
            self->cancelReconnectTimer();
            if (self->reconnect_.onStreaming()) {
                std::cout << "[PipeWire] Lejátszó stream újracsatlakoztatva.\n" << std::flush;
            }
        }
    }

    static void onCaptureProcess(void* data) {
        auto* self = static_cast<AetherSinkNode*>(data);
        RtInFlightGuard rtGuard(self->captureInFlight_);
        if (!self->liveUpsActive_.load(std::memory_order_acquire)) {
            if (self->playbackTriggerReady_.load(std::memory_order_acquire)) {
                if (struct pw_stream* pb = self->playbackPtr_.load(std::memory_order_acquire)) {
                    pw_stream_trigger_process(pb);
                }
            }
            return;
        }
        // Szétválasztott mód (saját motoros rendszerhang-felmintavételezés): capture -> SPSC gyűrű (0 allokáció, 0 zár).
        // Csak egyező rátapár (liveBridgeRateMatched_) esetén tápláljuk a hidat, hogy 0,5x/2x tempóhiba ne léphessen fel.
        const bool gateOk = self->liveBridgeRateMatched_.load(std::memory_order_acquire);
        while (struct pw_buffer* in = pw_stream_dequeue_buffer(self->capture)) {
            if (gateOk && in->buffer->n_datas >= 2) {
                struct spa_data* d0 = &in->buffer->datas[0];
                struct spa_data* d1 = &in->buffer->datas[1];
                const uint32_t off0 = std::min(d0->chunk->offset, d0->maxsize);
                const uint32_t off1 = std::min(d1->chunk->offset, d1->maxsize);
                const uint32_t size = std::min(std::min(d0->chunk->size, d0->maxsize - off0),
                                               std::min(d1->chunk->size, d1->maxsize - off1));
                const uint32_t total = size / sizeof(float);
                if (d0->data && d1->data && total > 0) {
                    const float* sL = SPA_PTROFF(d0->data, off0, float);
                    const float* sR = SPA_PTROFF(d1->data, off1, float);
                    self->noteInput(sL, sR, total);
                    uint32_t done = 0;
                    while (done < total) {
                        const uint32_t chunk = std::min<uint32_t>(total - done, kScratch);
                        float* dst = self->capInterleaved_.data();
                        for (uint32_t i = 0; i < chunk; ++i) {
                            dst[i * 2]     = sL[done + i];
                            dst[i * 2 + 1] = sR[done + i];
                        }
                        self->liveBridge_.pushInput(dst, chunk);
                        done += chunk;
                    }
                }
            }
            pw_stream_queue_buffer(self->capture, in);
        }
    }

    static void onPlaybackProcess(void* data) {
        auto* self = static_cast<AetherSinkNode*>(data);
        RtInFlightGuard rtGuard(self->playbackInFlight_);
        struct pw_stream* pb = self->playbackPtr_.load(std::memory_order_acquire);
        if (!pb) return;
        self->outputHeartbeat.beat();
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        const bool liveUps = self->liveUpsActive_.load(std::memory_order_acquire);

        // Közvetlen módban a legfrissebb bemeneti buffer (a régebbieket visszaadjuk);
        // szétválasztott módban a capture-t az onCaptureProcess üríti a hídba.
        struct pw_buffer* in = nullptr;
        if (!liveUps) {
            while (struct pw_buffer* t = pw_stream_dequeue_buffer(self->capture)) {
                if (in) pw_stream_queue_buffer(self->capture, in);
                in = t;
            }
        }
        struct pw_buffer* out = pw_stream_dequeue_buffer(pb);
        if (!out) {
            if (in) pw_stream_queue_buffer(self->capture, in);
            return;
        }

        struct spa_buffer* ob = out->buffer;
        if (ob->n_datas < 2 || !ob->datas[0].data || !ob->datas[1].data) {
            pw_stream_queue_buffer(pb, out);
            if (in) pw_stream_queue_buffer(self->capture, in);
            return;
        }
        float* outL = static_cast<float*>(ob->datas[0].data);
        float* outR = static_cast<float*>(ob->datas[1].data);
        uint32_t maxFrames = std::min(ob->datas[0].maxsize, ob->datas[1].maxsize) / sizeof(float);

        uint32_t n = 0;
        if (liveUps) {
            n = out->requested ? std::min<uint32_t>(static_cast<uint32_t>(out->requested), maxFrames) : 0;
            if (n == 0) n = std::min<uint32_t>(1024, maxFrames);
            n = std::min<uint32_t>(n, kScratch);
            if (self->liveBridgeRateMatched_.load(std::memory_order_acquire)) {
                float* buf = self->playInterleaved_.data();
                self->liveBridge_.pullOutput(buf, n);
                for (uint32_t i = 0; i < n; ++i) {
                    outL[i] = buf[i * 2];
                    outR[i] = buf[i * 2 + 1];
                }
            } else {
                std::memset(outL, 0, n * sizeof(float));
                std::memset(outR, 0, n * sizeof(float));
            }
        } else {
            if (in && in->buffer->n_datas >= 2) {
                struct spa_data* d0 = &in->buffer->datas[0];
                struct spa_data* d1 = &in->buffer->datas[1];
                const uint32_t off0 = std::min(d0->chunk->offset, d0->maxsize);
                const uint32_t off1 = std::min(d1->chunk->offset, d1->maxsize);
                const uint32_t size = std::min(std::min(d0->chunk->size, d0->maxsize - off0),
                                               std::min(d1->chunk->size, d1->maxsize - off1));
                n = std::min<uint32_t>(size / sizeof(float), maxFrames);
                if (d0->data && d1->data && n > 0) {
                    std::memcpy(outL, SPA_PTROFF(d0->data, off0, float), n * sizeof(float));
                    std::memcpy(outR, SPA_PTROFF(d1->data, off1, float), n * sizeof(float));
                    self->noteInput(outL, outR, n);
                } else {
                    n = 0;
                }
            }
            if (n == 0) {
                // Nincs bemenet (senki nem játszik ide): csend, a gráf által kért hosszban
                n = out->requested ? std::min<uint32_t>(static_cast<uint32_t>(out->requested), maxFrames) : 0;
                if (n == 0) n = std::min<uint32_t>(1024, maxFrames);
                std::memset(outL, 0, n * sizeof(float));
                std::memset(outR, 0, n * sizeof(float));
            }
        }

        // A gráf valódi órajele a playback stream pw_time szerkezetéből (K12 / U15)
        struct pw_time pwt{};
        if (pw_stream_get_time_n(pb, &pwt, sizeof(pwt)) == 0 && pwt.rate.denom > 0) {
            self->graphRate.store(pwt.rate.denom, std::memory_order_relaxed);
            self->driverClockRate_.store(pwt.rate.denom, std::memory_order_relaxed);
        }
        // Mintavétel-váltás (allowed-rates): ritka esemény, a lánc újrahangolása itt történik
        const uint32_t sr = self->sampleRate.load(std::memory_order_relaxed);
        if (sr > 0 && static_cast<uint32_t>(self->chain.sampleRate()) != sr) {
            self->chain.setSampleRate(sr);
            self->multi_.setSampleRate(sr);
        }

        // GUI-ból érkezett új beállítások (nem blokkoló seqlock olvasás)
        const uint32_t seq = self->params.currentSeq();
        if (seq != self->appliedParamSeq_) {
            ParamData pd;
            uint32_t readSeq = 0;
            if (self->params.tryRead(pd, readSeq)) {
                self->chain.applyParamsRT(pd);
                self->appliedParamSeq_ = readSeq;
            }
        }

        // Több kimenet: új beállítás (nem blokkoló olvasás)
        const uint32_t mseq = self->multiCfg_.currentSeq();
        if (mseq != self->appliedMultiSeq_) {
            multiout::MultiConfig mc;
            uint32_t readSeq = 0;
            if (self->multiCfg_.tryRead(mc, readSeq)) {
                self->multi_.applyConfigRT(mc);
                self->multiOn_ = mc.enabled;
                self->appliedMultiSeq_ = readSeq;
            }
        }

        if (self->multiOn_ && n <= kScratch) {
            // Az 1. kimenet a fő lánc (a felület beállításai), a többi a saját láncán / sávján; a további
            // kimenetek jele a gyűrűpufferekbe kerül
            std::memcpy(self->inL_.data(), outL, n * sizeof(float));
            std::memcpy(self->inR_.data(), outR, n * sizeof(float));
            float* oL[multiout::kMaxOutputs] = { outL, self->zoneL_[0].data(), self->zoneL_[1].data(), self->zoneL_[2].data() };
            float* oR[multiout::kMaxOutputs] = { outR, self->zoneR_[0].data(), self->zoneR_[1].data(), self->zoneR_[2].data() };
            self->multi_.process(self->inL_.data(), self->inR_.data(), oL, oR, n);
            const int nOut = self->multi_.config().numOutputs;
            for (int k = 1; k < nOut; ++k) self->zones_[k - 1]->ring.write(oL[k], oR[k], n);

        } else {
            self->chain.process(outL, outR, n);
        }
        self->visualizerTap.write(outL, outR, n);

        for (uint32_t i = 0; i < 2; ++i) {
            ob->datas[i].chunk->offset = 0;
            ob->datas[i].chunk->size = n * sizeof(float);
            ob->datas[i].chunk->stride = sizeof(float);
        }
        pw_stream_queue_buffer(pb, out);
        if (in) pw_stream_queue_buffer(self->capture, in);
        self->noteLoad(t0, n);
    }
};

} // namespace ee_analyzer
