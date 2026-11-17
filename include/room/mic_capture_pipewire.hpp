#pragma once
/**
 * @file mic_capture_pipewire.hpp
 * @brief Mikrofonfelvétel PipeWire-rel (Linux, Raspberry Pi) – a `mic_capture.hpp` felületének megvalósítása.
 *
 * Saját PipeWire-kapcsolatot és saját szálat (pw_thread_loop) használ, a motor hangútjától függetlenül: a motor
 * eseményhurkát és valós idejű szálát nem érinti. A felvétel monó, 32 bites lebegőpontos, a kért mintavételen
 * (a PipeWire szükség szerint átalakít és lekever). A valós idejű feldolgozó (process) nem foglal memóriát.
 *
 * Forrás: PipeWire API-dokumentáció (docs.pipewire.org: pw_thread_loop, pw_stream, pw_registry; stream-tulajdonságok:
 * target.object, stream.capture.sink).
 */
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/dict.h>

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "mic_capture.hpp"

namespace aqulify::room {

class PipeWireMicCapture final : public MicCapture {
public:
    PipeWireMicCapture() {
        pw_init(nullptr, nullptr);
        tloop_ = pw_thread_loop_new("aqulify-mic", nullptr);
        if (!tloop_) return;
        context_ = pw_context_new(pw_thread_loop_get_loop(tloop_), nullptr, 0);
        if (!context_) return;
        if (pw_thread_loop_start(tloop_) < 0) return;
        pw_thread_loop_lock(tloop_);
        core_ = pw_context_connect(context_, nullptr, 0);
        if (core_) {
            static const pw_core_events coreEvents = [] {
                pw_core_events e{};
                e.version = PW_VERSION_CORE_EVENTS;
                e.done = onCoreDone;
                return e;
            }();
            pw_core_add_listener(core_, &coreListener_, &coreEvents, this);
        }
        pw_thread_loop_unlock(tloop_);
    }

    ~PipeWireMicCapture() override {
        stop();
        if (tloop_) pw_thread_loop_stop(tloop_);
        if (core_) {
            spa_hook_remove(&coreListener_);
            pw_core_disconnect(core_);
        }
        if (context_) pw_context_destroy(context_);
        if (tloop_) pw_thread_loop_destroy(tloop_);
    }

    bool ok() const { return core_ != nullptr; }

    std::vector<InputDevice> listInputs(bool includeMonitors) override {
        std::vector<InputDevice> out;
        if (!core_) return out;
        pw_thread_loop_lock(tloop_);
        found_.clear();
        pw_registry* reg = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
        spa_hook regListener{};
        static const pw_registry_events regEvents = [] {
            pw_registry_events e{};
            e.version = PW_VERSION_REGISTRY_EVENTS;
            e.global = onRegistryGlobal;
            return e;
        }();
        pw_registry_add_listener(reg, &regListener, &regEvents, this);
        // A regiszter a meglévő csomópontokat azonnal elküldi; a sync "done" válasza után mind megjött
        syncSeq_ = pw_core_sync(core_, PW_ID_CORE, 0);
        syncDone_ = false;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!syncDone_ && std::chrono::steady_clock::now() < until) pw_thread_loop_timed_wait(tloop_, 1);
        spa_hook_remove(&regListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(reg));
        for (const auto& d : found_)
            if (!d.isMonitor || includeMonitors) out.push_back(d);
        pw_thread_loop_unlock(tloop_);
        return out;
    }

    bool start(const std::string& deviceId, uint32_t sampleRate, double maxSeconds, std::string& error) override {
        if (!core_) { error = "backend"; return false; }
        stop();
        if (sampleRate < 8000 || sampleRate > 384000 || !(maxSeconds > 0.0) || maxSeconds > 600.0) {
            error = "format";
            return false;
        }
        buffer_.reset(static_cast<size_t>(maxSeconds * sampleRate) + 1);
        // Monitor-e a cél (egy kimenet hangjának felvétele): a listából derül ki
        bool monitor = false;
        if (!deviceId.empty())
            for (const auto& d : listInputs(true))
                if (d.id == deviceId) monitor = d.isMonitor;

        pw_thread_loop_lock(tloop_);
        pw_properties* props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Production",
            PW_KEY_NODE_NAME, "aqulify_measure_mic",
            PW_KEY_NODE_DESCRIPTION, "Aqulify mérés",
            PW_KEY_NODE_DONT_RECONNECT, "true",
            nullptr);
        if (!deviceId.empty()) {
            // Ha a kiválasztott eszköz nincs meg, ne essen vissza csendben az alapértelmezett mikrofonra
            pw_properties_set(props, "node.dont-fallback", "true");
#ifdef PW_KEY_TARGET_OBJECT
            pw_properties_set(props, PW_KEY_TARGET_OBJECT, deviceId.c_str());
#else
            pw_properties_set(props, PW_KEY_NODE_TARGET, deviceId.c_str());
#endif
        }
        if (monitor) pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
        stream_ = pw_stream_new(core_, "aqulify-measure", props);
        if (!stream_) {
            pw_thread_loop_unlock(tloop_);
            error = "backend";
            return false;
        }
        static const pw_stream_events streamEvents = [] {
            pw_stream_events e{};
            e.version = PW_VERSION_STREAM_EVENTS;
            e.process = onProcess;
            e.state_changed = onState;
            return e;
        }();
        state_ = PW_STREAM_STATE_UNCONNECTED;
        streamError_.clear();
        pw_stream_add_listener(stream_, &streamListener_, &streamEvents, this);

        uint8_t buf[512];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32;
        info.channels = 1;
        info.rate = sampleRate;
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
        const spa_pod* params[1] = { spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info) };
        const int rc = pw_stream_connect(stream_, PW_DIRECTION_INPUT, PW_ID_ANY,
                                         static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                                                      PW_STREAM_FLAG_RT_PROCESS),
                                         params, 1);
        if (rc < 0) {
            destroyStreamLocked();
            pw_thread_loop_unlock(tloop_);
            error = "backend";
            return false;
        }
        // Megvárjuk, hogy a stream valóban fusson (vagy hibára fusson); legfeljebb 3 s
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (state_ != PW_STREAM_STATE_STREAMING && state_ != PW_STREAM_STATE_ERROR &&
               std::chrono::steady_clock::now() < until)
            pw_thread_loop_timed_wait(tloop_, 1);
        const bool streaming = state_ == PW_STREAM_STATE_STREAMING;
        if (!streaming) {
            error = state_ == PW_STREAM_STATE_ERROR ? "busy" : "no_device";
            destroyStreamLocked();
            pw_thread_loop_unlock(tloop_);
            return false;
        }
        running_ = true;
        pw_thread_loop_unlock(tloop_);
        return true;
    }

    std::vector<float> stop() override {
        if (!tloop_ || !stream_) return {};
        pw_thread_loop_lock(tloop_);
        destroyStreamLocked();
        running_ = false;
        pw_thread_loop_unlock(tloop_);
        return buffer_.take();
    }

    bool running() const override { return running_; }
    float takePeak() override { return buffer_.takePeak(); }
    size_t recordedFrames() const override { return buffer_.written(); }

private:
    void destroyStreamLocked() {
        if (!stream_) return;
        spa_hook_remove(&streamListener_);
        pw_stream_disconnect(stream_);
        pw_stream_destroy(stream_);
        stream_ = nullptr;
    }

    static void onCoreDone(void* data, uint32_t id, int seq) {
        auto* self = static_cast<PipeWireMicCapture*>(data);
        if (id == PW_ID_CORE && seq == self->syncSeq_) {
            self->syncDone_ = true;
            pw_thread_loop_signal(self->tloop_, false);
        }
    }

    static void onRegistryGlobal(void* data, uint32_t, uint32_t, const char* type, uint32_t, const spa_dict* props) {
        auto* self = static_cast<PipeWireMicCapture*>(data);
        if (!props || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
        const char* cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        if (!cls || !name) return;
        const bool source = std::strcmp(cls, "Audio/Source") == 0 || std::strcmp(cls, "Audio/Source/Virtual") == 0;
        const bool sink = std::strcmp(cls, "Audio/Sink") == 0;
        if (!source && !sink) return;
        const char* desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
        if (!desc) desc = spa_dict_lookup(props, PW_KEY_NODE_NICK);
        InputDevice d;
        d.id = name;
        d.name = desc ? desc : name;
        d.isMonitor = sink;
        self->found_.push_back(std::move(d));
    }

    static void onState(void* data, pw_stream_state, pw_stream_state state, const char* err) {
        auto* self = static_cast<PipeWireMicCapture*>(data);
        self->state_ = state;
        if (err) self->streamError_ = err;
        pw_thread_loop_signal(self->tloop_, false);
    }

    /** Valós idejű szál: a puffer másolása foglalás nélkül. */
    static void onProcess(void* data) {
        auto* self = static_cast<PipeWireMicCapture*>(data);
        pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
        if (!b) return;
        spa_buffer* sb = b->buffer;
        if (sb->n_datas > 0 && sb->datas[0].data && sb->datas[0].chunk) {
            const spa_chunk* c = sb->datas[0].chunk;
            const auto* p = reinterpret_cast<const float*>(static_cast<const uint8_t*>(sb->datas[0].data) + c->offset);
            self->buffer_.push(p, c->size / sizeof(float));
        }
        pw_stream_queue_buffer(self->stream_, b);
    }

    pw_thread_loop* tloop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    spa_hook coreListener_{};
    pw_stream* stream_ = nullptr;
    spa_hook streamListener_{};
    pw_stream_state state_ = PW_STREAM_STATE_UNCONNECTED;
    std::string streamError_;
    int syncSeq_ = 0;
    bool syncDone_ = false;
    std::atomic<bool> running_{false};
    std::vector<InputDevice> found_;
    CaptureBuffer buffer_;
};

} // namespace aqulify::room
