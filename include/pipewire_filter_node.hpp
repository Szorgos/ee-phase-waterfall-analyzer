#pragma once

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <memory>
#include <iostream>
#include <vector>

#include "aether_chain.hpp"
#include "tap_ring.hpp"

namespace ee_analyzer {

/**
 * @brief PipeWire Port leíró a sztereó bemenethez és kimenethez
 */
struct PortData {
    void* port = nullptr;
    int channel = 0; // 0 = Bal (L), 1 = Jobb (R)
};

/**
 * @brief Natív Linux PipeWire Audio Filter Csomópont (pw_filter)
 * Kétcsatornás szűrőcsomópont, amely beépül a PipeWire / EasyEffects audiográfba,
 * átvezeti a hangot a DspPipeline-on, és párhuzamosan másolja a lock-free ringbufferbe.
 */
class PipeWireFilterNode {
public:
    struct pw_main_loop* loop = nullptr;
    struct pw_filter* filter = nullptr;
    struct spa_hook filterListener;

    PortData inPortL;
    PortData inPortR;
    PortData outPortL;
    PortData outPortR;

    AetherChain chain;                     // teljes DSP lánc (háttérszálas FIR / konvolver)
    StereoTapRing<65536> visualizerTap;   // feldolgozott sztereó jel az analizátornak
    ParamBlock params;                     // GUI -> audio szál (seqlock)
    uint32_t appliedParamSeq = 0;

    uint32_t sampleRate = 48000;
    bool isRunning = false;

    PipeWireFilterNode(uint32_t sr = 48000)
        : sampleRate(sr) {
        chain.setSampleRate(sr);
        chain.startWorker();
    }

    ~PipeWireFilterNode() {
        stop();
    }

    /**
     * @brief PipeWire szűrőcsomópont inicializálása és regisztrációja
     */
    bool init(int argc, char* argv[], const char* nodeName = "EasyEffects-Phase-Waterfall-Analyzer") {
        pw_init(&argc, &argv);

        loop = pw_main_loop_new(nullptr);
        if (!loop) {
            std::cerr << "[Hiba] Nem sikerült létrehozni a PipeWire fő eseményhurkot!\n";
            return false;
        }

        // Csomópont tulajdonságainak definiálása (PipeWire / EasyEffects felismeréshez)
        struct pw_properties* props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Filter",
            PW_KEY_MEDIA_ROLE, "DSP",
            PW_KEY_NODE_NAME, nodeName,
            PW_KEY_NODE_DESCRIPTION, "EasyEffects Phase & Acoustic Waterfall Filter",
            nullptr
        );

        static const struct pw_filter_events filterEvents = {
            .version = PW_VERSION_FILTER_EVENTS,
            .destroy = nullptr,
            .state_changed = nullptr,
            .io_changed = nullptr,
            .param_changed = nullptr,
            .add_buffer = nullptr,
            .remove_buffer = nullptr,
            .process = onProcessCallback,
            .drained = nullptr,
            .command = nullptr
        };

        filter = pw_filter_new_simple(
            pw_main_loop_get_loop(loop),
            nodeName,
            props,
            &filterEvents,
            this
        );

        if (!filter) {
            std::cerr << "[Hiba] Nem sikerült létrehozni a pw_filter objektumot!\n";
            return false;
        }

        // Bemeneti és kimeneti sztereó portok regisztrálása
        setupPorts();

        return true;
    }

    /**
     * @brief Sztereó portok felkonfigurálása a szűrőn
     */
    void setupPorts() {
        // Bal bemenet (Input Left)
        inPortL.channel = 0;
        inPortL.port = pw_filter_add_port(filter,
            PW_DIRECTION_INPUT,
            PW_FILTER_PORT_FLAG_MAP_BUFFERS,
            sizeof(PortData),
            pw_properties_new(
                PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                PW_KEY_PORT_NAME, "input_FL",
                PW_KEY_AUDIO_CHANNEL, "FL",
                nullptr
            ),
            nullptr, 0);

        // Jobb bemenet (Input Right)
        inPortR.channel = 1;
        inPortR.port = pw_filter_add_port(filter,
            PW_DIRECTION_INPUT,
            PW_FILTER_PORT_FLAG_MAP_BUFFERS,
            sizeof(PortData),
            pw_properties_new(
                PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                PW_KEY_PORT_NAME, "input_FR",
                PW_KEY_AUDIO_CHANNEL, "FR",
                nullptr
            ),
            nullptr, 0);

        // Bal kimenet (Output Left)
        outPortL.channel = 0;
        outPortL.port = pw_filter_add_port(filter,
            PW_DIRECTION_OUTPUT,
            PW_FILTER_PORT_FLAG_MAP_BUFFERS,
            sizeof(PortData),
            pw_properties_new(
                PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                PW_KEY_PORT_NAME, "output_FL",
                PW_KEY_AUDIO_CHANNEL, "FL",
                nullptr
            ),
            nullptr, 0);

        // Jobb kimenet (Output Right)
        outPortR.channel = 1;
        outPortR.port = pw_filter_add_port(filter,
            PW_DIRECTION_OUTPUT,
            PW_FILTER_PORT_FLAG_MAP_BUFFERS,
            sizeof(PortData),
            pw_properties_new(
                PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                PW_KEY_PORT_NAME, "output_FR",
                PW_KEY_AUDIO_CHANNEL, "FR",
                nullptr
            ),
            nullptr, 0);
    }

    /**
     * @brief A csomópont csatlakoztatása a PipeWire gráfhoz
     */
    bool connect() {
        if (!filter) return false;

        int res = pw_filter_connect(filter,
            PW_FILTER_FLAG_RT_PROCESS,
            nullptr, 0);

        if (res < 0) {
            std::cerr << "[Hiba] Nem sikerült csatlakozni a PipeWire démonhoz: " << res << "\n";
            return false;
        }

        isRunning = true;
        return true;
    }

    /**
     * @brief Fő eseményhurok futtatása (blokkoló hívás)
     */
    void run() {
        if (loop) {
            pw_main_loop_run(loop);
        }
    }

    /**
     * @brief Leállítás és erőforrások felszabadítása
     */
    void stop() {
        chain.stopWorker();
        if (!isRunning) return;
        isRunning = false;

        if (filter) {
            pw_filter_disconnect(filter);
            pw_filter_destroy(filter);
            filter = nullptr;
        }

        if (loop) {
            pw_main_loop_quit(loop);
            pw_main_loop_destroy(loop);
            loop = nullptr;
        }
        pw_deinit();
    }

    /**
     * @brief Valós idejű PipeWire feldolgozó visszahívás (Process Callback)
     */
    static void onProcessCallback(void* userdata, struct spa_io_position* position) {
        auto* self = static_cast<PipeWireFilterNode*>(userdata);
        if (!self || !position) return;

        const uint32_t nSamples = position->clock.duration;
        if (nSamples == 0) return;

        // Puffer mutatók lekérése
        float* inL = static_cast<float*>(pw_filter_get_dsp_buffer(self->inPortL.port, nSamples));
        float* inR = static_cast<float*>(pw_filter_get_dsp_buffer(self->inPortR.port, nSamples));
        float* outL = static_cast<float*>(pw_filter_get_dsp_buffer(self->outPortL.port, nSamples));
        float* outR = static_cast<float*>(pw_filter_get_dsp_buffer(self->outPortR.port, nSamples));

        if (!outL || !outR) return;

        // Ha van bemenet, átmásoljuk a kimeneti pufferbe a helyben történő szűréshez
        if (inL) {
            std::copy(inL, inL + nSamples, outL);
        } else {
            std::fill(outL, outL + nSamples, 0.0f);
        }

        if (inR) {
            std::copy(inR, inR + nSamples, outR);
        } else {
            std::fill(outR, outR + nSamples, 0.0f);
        }

        // GUI-ból érkezett új beállítások átvétele (nem blokkoló seqlock olvasás)
        const uint32_t seq = self->params.currentSeq();
        if (seq != self->appliedParamSeq) {
            ParamData pd;
            uint32_t readSeq = 0;
            if (self->params.tryRead(pd, readSeq)) {
                self->chain.applyParamsRT(pd);
                self->appliedParamSeq = readSeq;
            }
        }

        // Valós idejű DSP lánc (Preamp -> EQ -> Bass -> Crossfeed -> Convolver -> Limiter)
        self->chain.process(outL, outR, nSamples);

        // Zármentes átadás az analizátor / 3D vízesés szálnak (sztereó)
        self->visualizerTap.write(outL, outR, nSamples);
    }
};

} // namespace ee_analyzer
