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
 * A fő hangszál a kimenetek jelét gyűrűpufferekbe írja; a további streamek egy quantumnyi előtöltéssel
 * olvassák. A céleszköz eltűnésekor a stream nem kapcsolódik át máshová (node.dont-reconnect).
 */
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

#include "aether_chain.hpp"
#include "multi_output.hpp"
#include "aq_i18n.hpp"
#include "param_block.hpp"
#include "tap_ring.hpp"

namespace ee_analyzer {

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
    std::atomic<bool> playbackStreaming{false};
    std::atomic<bool> coreError{false};

    static constexpr int kExtraZones = multiout::kMaxOutputs - 1;
    static constexpr uint32_t kZoneRing = 32768;      // kb. 0,7 s 48 kHz-en
    static constexpr uint32_t kScratch = 16384;       // a legnagyobb feldolgozott quantum

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
        bool primed = false;                          // csak a stream saját szála
    };

    explicit AetherSinkNode(uint32_t initialRate = 48000) {
        sampleRate.store(initialRate);
        chain.setSampleRate(initialRate);
        chain.startWorker();
        multi_.setExternalMainChain(&chain);
        multi_.setSampleRate(initialRate);
        multi_.startWorkers();
        inL_.assign(kScratch, 0.0f);
        inR_.assign(kScratch, 0.0f);
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

    /** A k-adik kimenet (1–3) saját lánca, pl. impulzusválasz egy zónának (submitIr szálbiztos). */
    AetherChain& zoneChain(int k) { return multi_.chain(k); }

    /** A további kimenetek állapota JSON-ként (a felületnek): céleszköz, szól-e, alulcsordulások. */
    std::string multiStatusJson() {
        std::lock_guard<std::mutex> lk(zoneMutex_);
        std::string s = "[";
        for (int k = 0; k < kExtraZones; ++k) {
            if (k) s += ",";
            s += "{\"output\":" + std::to_string(k + 2) + ",\"target\":\"" + wantedTargets_[k] + "\",\"streaming\":" +
                 (zones_[k]->streaming.load() ? "true" : "false") + ",\"underruns\":" + std::to_string(zones_[k]->underruns.load()) + "}";
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
        capture = pw_stream_new(core, (sinkName + "_input").c_str(), cprops);

        // 2. Lejátszó stream a valódi hangeszköz felé
        struct pw_properties* pprops = pw_properties_new(
            PW_KEY_MEDIA_CLASS, "Stream/Output/Audio",
            PW_KEY_NODE_NAME, (sinkName + "_output").c_str(),
            PW_KEY_NODE_DESCRIPTION, aq_i18n::fmt(aq_i18n::Msg::pw_outputNode, { { "name", description } }).c_str(),
            PW_KEY_NODE_GROUP, group.c_str(),
            PW_KEY_NODE_LINK_GROUP, group.c_str(),
            PW_KEY_NODE_PASSIVE, "true",
            PW_KEY_AUDIO_CHANNELS, "2",
            SPA_KEY_AUDIO_POSITION, "FL,FR",
            PW_KEY_MEDIA_ROLE, "DSP",
            nullptr);
        playback = pw_stream_new(core, (sinkName + "_output").c_str(), pprops);
        if (!capture || !playback) return fail("a streamek nem hozhatók létre");

        static const struct pw_stream_events captureEvents = [] {
            pw_stream_events e{};
            e.version = PW_VERSION_STREAM_EVENTS;
            e.process = onCaptureProcess;
            e.param_changed = onParamChanged;
            return e;
        }();
        static const struct pw_stream_events playbackEvents = [] {
            pw_stream_events e{};
            e.version = PW_VERSION_STREAM_EVENTS;
            e.process = onPlaybackProcess;
            e.state_changed = onPlaybackState;
            return e;
        }();
        pw_stream_add_listener(capture, &captureListener, &captureEvents, this);
        pw_stream_add_listener(playback, &playbackListener, &playbackEvents, this);
        group_ = group;
        sinkName_ = sinkName;
        description_ = description;
        zoneEvent_ = pw_loop_add_event(pw_main_loop_get_loop(loop), onZoneEvent, this);

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

        struct spa_pod_builder b2 = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        const struct spa_pod* pparams[1] = { spa_format_audio_raw_build(&b2, SPA_PARAM_EnumFormat, &info) };
        if (pw_stream_connect(playback, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                              static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                                           PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_TRIGGER),
                              pparams, 1) < 0)
            return fail("a lejátszó stream nem csatlakoztatható");
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
        for (auto& z : zones_) if (z && z->stream) { pw_stream_destroy(z->stream); z->stream = nullptr; }
        chain.stopWorker();
        multi_.stopWorkers();
        if (capture) { pw_stream_destroy(capture); capture = nullptr; }
        if (playback) { pw_stream_destroy(playback); playback = nullptr; }
        if (core) { pw_core_disconnect(core); core = nullptr; }
        if (context) { pw_context_destroy(context); context = nullptr; }
        if (loop) { pw_main_loop_destroy(loop); loop = nullptr; pw_deinit(); }
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
    std::string group_, sinkName_, description_;
    std::vector<float> inL_, inR_;
    std::array<std::vector<float>, kExtraZones> zoneL_, zoneR_;

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
            if (z.stream && z.target == want[k]) continue;
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

        if (z->fresh.exchange(false)) {
            z->ring.skip(z->ring.available());
            z->primed = false;
        }
        const uint32_t target = 2 * n;
        uint32_t got = 0;
        if (!z->primed && z->ring.available() >= target) z->primed = true;
        if (z->primed) {
            const uint32_t avail = z->ring.available();
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

    bool fail(const char* what) {
        std::cerr << "[Hiba] PipeWire: " << what << "\n";
        return false;
    }

    static void onCoreError(void* data, uint32_t id, int seq, int res, const char* message) {
        auto* self = static_cast<AetherSinkNode*>(data);
        (void)seq;
        std::cerr << "[PipeWire] hiba (id " << id << "): " << (message ? message : "") << " (" << spa_strerror(res) << ")\n";
        if (id == PW_ID_CORE && res == -EPIPE) {
            self->coreError = true;
            self->quit(); // a démon leállt: kilépünk, a systemd újraindít
        }
    }

    static void onParamChanged(void* data, uint32_t id, const struct spa_pod* param) {
        auto* self = static_cast<AetherSinkNode*>(data);
        if (!param || id != SPA_PARAM_Format) return;
        struct spa_audio_info_raw info{};
        if (spa_format_audio_raw_parse(param, &info) >= 0 && info.rate > 0) self->sampleRate.store(info.rate);
    }

    static void onPlaybackState(void* data, enum pw_stream_state old, enum pw_stream_state state, const char* error) {
        auto* self = static_cast<AetherSinkNode*>(data);
        (void)old;
        self->playbackStreaming = (state == PW_STREAM_STATE_STREAMING);
        if (state == PW_STREAM_STATE_ERROR) std::cerr << "[PipeWire] lejátszó stream hiba: " << (error ? error : "") << "\n";
    }

    static void onCaptureProcess(void* data) {
        auto* self = static_cast<AetherSinkNode*>(data);
        pw_stream_trigger_process(self->playback);
    }

    static void onPlaybackProcess(void* data) {
        auto* self = static_cast<AetherSinkNode*>(data);

        // A legfrissebb bemeneti buffer (a régebbieket visszaadjuk)
        struct pw_buffer* in = nullptr;
        while (struct pw_buffer* t = pw_stream_dequeue_buffer(self->capture)) {
            if (in) pw_stream_queue_buffer(self->capture, in);
            in = t;
        }
        struct pw_buffer* out = pw_stream_dequeue_buffer(self->playback);
        if (!out) {
            if (in) pw_stream_queue_buffer(self->capture, in);
            return;
        }

        struct spa_buffer* ob = out->buffer;
        if (ob->n_datas < 2 || !ob->datas[0].data || !ob->datas[1].data) {
            pw_stream_queue_buffer(self->playback, out);
            if (in) pw_stream_queue_buffer(self->capture, in);
            return;
        }
        float* outL = static_cast<float*>(ob->datas[0].data);
        float* outR = static_cast<float*>(ob->datas[1].data);
        uint32_t maxFrames = std::min(ob->datas[0].maxsize, ob->datas[1].maxsize) / sizeof(float);

        uint32_t n = 0;
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
        pw_stream_queue_buffer(self->playback, out);
        if (in) pw_stream_queue_buffer(self->capture, in);
    }
};

} // namespace ee_analyzer
