#pragma once
/**
 * @file player_output_pw.hpp
 * @brief A beépített lejátszó hangkimenete Linuxon: egy rendes lejátszó-hangfolyam (Stream/Output/Audio) az
 *        „Aqulify” virtuális eszközre (aqulify_sink), így a hang a teljes DSP-láncon átmegy, ugyanúgy, mint bármely
 *        más alkalmazásé, amely erre az eszközre játszik.
 *
 * Saját pw_thread_loop: a render a hurok szálán fut (nem a valós idejű adatszálon), a hurok zárja alatt; a lejátszó
 * dekódoló szála a lock() / unlock() párral üríti a puffert tekeréskor. A hangfolyam a fájl saját mintavételi
 * frekvenciájával nyílik; ha ez eltér a gráfétól, a PipeWire saját mintavétel-átalakítója igazítja (a „bitperfect”
 * lejátszás így csak egyező frekvenciánál teljesül).
 */
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "file_player.hpp"

namespace ee_analyzer::player {

class PwPlayerOutput : public PlayerOutput {
public:
    /** target: a cél csomópont neve (alapból az Aqulify virtuális eszköze). A pw_init() már megtörtént. */
    explicit PwPlayerOutput(std::string target = "aqulify_sink") : target_(std::move(target)) {}

    ~PwPlayerOutput() override {
        close();
        if (loop_) pw_thread_loop_stop(loop_);
        if (core_) pw_core_disconnect(core_);
        if (context_) pw_context_destroy(context_);
        if (loop_) pw_thread_loop_destroy(loop_);
    }

    bool open(uint32_t sampleRate, RenderFn render, std::string* error) override {
        if (!ensureLoop(error)) return false;
        close();
        pw_thread_loop_lock(loop_);
        render_ = std::move(render);
        struct pw_properties* props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Music",
            PW_KEY_APP_NAME, "Aqulify",
            PW_KEY_NODE_NAME, "aqulify_player",
            PW_KEY_NODE_DESCRIPTION, "Aqulify Player",
            "target.object", target_.c_str(),   // PW_KEY_TARGET_OBJECT (0.3.64-től)
            "node.target", target_.c_str(),     // régebbi PipeWire / WirePlumber
            nullptr);
        stream_ = pw_stream_new(core_, "aqulify_player", props);
        if (!stream_) {
            pw_thread_loop_unlock(loop_);
            if (error) *error = "A lejátszó hangfolyama nem hozható létre.";
            return false;
        }
        static const struct pw_stream_events events = [] {
            pw_stream_events e{};
            e.version = PW_VERSION_STREAM_EVENTS;
            e.process = onProcess;
            return e;
        }();
        spa_zero(listener_);   // az előző (megszüntetett) hangfolyam figyelője nem él tovább
        pw_stream_add_listener(stream_, &listener_, &events, this);
        uint8_t buf[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        struct spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32;
        info.rate = sampleRate;
        info.channels = 2;
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
        const struct spa_pod* params[1] = { spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info) };
        const int res = pw_stream_connect(stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                          static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                                       PW_STREAM_FLAG_MAP_BUFFERS |
                                                                       PW_STREAM_FLAG_INACTIVE),
                                          params, 1);
        if (res < 0) {
            pw_stream_destroy(stream_);
            stream_ = nullptr;
            pw_thread_loop_unlock(loop_);
            if (error) *error = "A lejátszó hangfolyama nem csatlakoztatható.";
            return false;
        }
        pw_thread_loop_unlock(loop_);
        return true;
    }

    void close() override {
        if (!loop_ || !stream_) return;
        pw_thread_loop_lock(loop_);
        pw_stream_destroy(stream_);
        stream_ = nullptr;
        render_ = nullptr;
        pw_thread_loop_unlock(loop_);
    }

    void setActive(bool active) override {
        if (!loop_ || !stream_) return;
        pw_thread_loop_lock(loop_);
        pw_stream_set_active(stream_, active);
        pw_thread_loop_unlock(loop_);
    }

    void lock() override { if (loop_) pw_thread_loop_lock(loop_); }
    void unlock() override { if (loop_) pw_thread_loop_unlock(loop_); }
    /** Tekerés: a hangfolyam sorban álló puffereinek eldobása (a lock() alatt hívódik, a hurok zárja megvan). */
    void flush() override { if (stream_) pw_stream_flush(stream_, false); }

private:
    bool ensureLoop(std::string* error) {
        if (core_) return true;
        loop_ = pw_thread_loop_new("aqulify-player", nullptr);
        if (loop_) context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
        if (context_ && pw_thread_loop_start(loop_) == 0) {
            pw_thread_loop_lock(loop_);
            core_ = pw_context_connect(context_, nullptr, 0);
            pw_thread_loop_unlock(loop_);
        }
        if (!core_ && error) *error = "A PipeWire nem érhető el a lejátszónak.";
        return core_ != nullptr;
    }

    static void onProcess(void* data) {
        auto* self = static_cast<PwPlayerOutput*>(data);
        struct pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
        if (!b) return;
        struct spa_data* d = &b->buffer->datas[0];
        auto* dst = static_cast<float*>(d->data);
        if (dst) {
            constexpr uint32_t stride = 2 * sizeof(float);
            const uint32_t maxFrames = d->maxsize / stride;
            const uint32_t n = b->requested ? std::min<uint32_t>(static_cast<uint32_t>(b->requested), maxFrames) : maxFrames;
            if (self->render_) self->render_(dst, n);
            else std::memset(dst, 0, n * stride);
            d->chunk->offset = 0;
            d->chunk->stride = stride;
            d->chunk->size = n * stride;
        }
        pw_stream_queue_buffer(self->stream_, b);
    }

    std::string target_;
    struct pw_thread_loop* loop_ = nullptr;
    struct pw_context* context_ = nullptr;
    struct pw_core* core_ = nullptr;
    struct pw_stream* stream_ = nullptr;
    struct spa_hook listener_{};
    RenderFn render_;
};

} // namespace ee_analyzer::player
