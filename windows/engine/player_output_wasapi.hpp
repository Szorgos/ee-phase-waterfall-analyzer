#pragma once
/**
 * @file player_output_wasapi.hpp
 * @brief A beépített lejátszó hangkimenete Windowson: WASAPI, osztott mód, az alapértelmezett lejátszóeszközre
 *        (eConsole) – arra, amelyen az Equalizer APO-ban az Aqulify plugin fut, így a hang a DSP-láncon átmegy,
 *        ugyanúgy, mint bármely más alkalmazásé.
 *
 * Saját szál (MMCSS „Audio”, eseményvezérelt puffer, kb. 100 ms). A hangfolyam a fájl saját mintavételi
 * frekvenciájával nyílik (32 bites float, sztereó); ha ez eltér az eszközétől, a Windows saját átalakítója igazítja
 * (AUTOCONVERTPCM, SRC_DEFAULT_QUALITY). A render a szál zárja alatt fut; a lejátszó dekódoló szála a lock() /
 * unlock() párral üríti a puffert tekeréskor. Ha az eszköz megszűnik vagy az alapértelmezett eszköz változik
 * (AUDCLNT_E_DEVICE_INVALIDATED), a szál az új alapértelmezett eszközön nyitja meg újra.
 */
#include <windows.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "file_player.hpp"
#include "wasapi_loopback.hpp"   // GUID-ok és ComPtr

namespace ee_analyzer::player {

class WasapiPlayerOutput : public PlayerOutput {
public:
    ~WasapiPlayerOutput() override { close(); }

    bool open(uint32_t sampleRate, RenderFn render, std::string* error) override {
        close();
        {
            std::lock_guard<std::mutex> lk(renderMutex_);
            render_ = std::move(render);
        }
        rate_ = sampleRate;
        active_ = false;
        quit_ = false;
        flushReq_ = false;
        written_ = 0;
        consumed_ = 0;
        initState_ = 0;
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        thread_ = std::thread([this] { run(); });
        // az első megnyitás eredménye (a hangeszköz a szálon nyílik meg, ott is marad)
        std::unique_lock<std::mutex> lk(initMutex_);
        initCv_.wait(lk, [this] { return initState_ != 0; });
        if (initState_ < 0) {
            lk.unlock();
            close();
            if (error) *error = "A Windows hangkimenete nem nyitható meg (nincs alapértelmezett lejátszóeszköz?).";
            return false;
        }
        return true;
    }

    void close() override {
        if (thread_.joinable()) {
            quit_ = true;
            if (wake_) SetEvent(wake_);
            thread_.join();
        }
        if (wake_) { CloseHandle(wake_); wake_ = nullptr; }
        std::lock_guard<std::mutex> lk(renderMutex_);
        render_ = nullptr;
    }

    void setActive(bool active) override {
        active_ = active;
        if (wake_) SetEvent(wake_);
    }

    void lock() override { renderMutex_.lock(); }
    void unlock() override { renderMutex_.unlock(); }

    /** Tekerés: a WASAPI-puffer eldobását a szál végzi (Stop + Reset), a következő ébredéskor. */
    void flush() override {
        consumed_ = 0;      // a pozíció a tekerés helyétől indul (a szál a saját számlálóit is nullázza)
        flushReq_ = true;
        if (wake_) SetEvent(wake_);
    }

    uint64_t consumedFrames() const override { return flushReq_.load() ? 0 : consumed_.load(); }

private:
    static constexpr IID kIID_IAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};

    struct Stream {
        win::ComPtr<IAudioClient> client;
        win::ComPtr<IAudioRenderClient> render;
        HANDLE event = nullptr;
        UINT32 bufferFrames = 0;
        bool started = false;
        ~Stream() {
            if (client && started) client->Stop();
            render.reset();
            client.reset();
            if (event) CloseHandle(event);
        }
    };

    /** Megnyitás az alapértelmezett eszközön; false: nem sikerült. */
    bool openStream(Stream& s) {
        win::ComPtr<IMMDeviceEnumerator> en;
        if (FAILED(CoCreateInstance(win::kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, win::kIID_IMMDeviceEnumerator,
                                    reinterpret_cast<void**>(en.put()))))
            return false;
        win::ComPtr<IMMDevice> dev;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.put()))) return false;
        if (FAILED(dev->Activate(win::kIID_IAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(s.client.put()))))
            return false;
        WAVEFORMATEXTENSIBLE wf{};
        wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wf.Format.nChannels = 2;
        wf.Format.nSamplesPerSec = rate_;
        wf.Format.wBitsPerSample = 32;
        wf.Format.nBlockAlign = 8;
        wf.Format.nAvgBytesPerSec = rate_ * 8;
        wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wf.Samples.wValidBitsPerSample = 32;
        wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        wf.SubFormat = win::kSubtypeFloat;
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        const REFERENCE_TIME buffer = 1000000;   // 100 ms
        if (FAILED(s.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buffer, 0,
                                        reinterpret_cast<WAVEFORMATEX*>(&wf), nullptr)))
            return false;
        s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!s.event || FAILED(s.client->SetEventHandle(s.event)) || FAILED(s.client->GetBufferSize(&s.bufferFrames)) ||
            FAILED(s.client->GetService(kIID_IAudioRenderClient, reinterpret_cast<void**>(s.render.put()))))
            return false;
        return true;
    }

    /**
     * A puffer szabad részének kitöltése; false: az eszköz megszűnt (újranyitás kell). A render zárja alatt fut, így
     * a tekerés (flush) és a kitöltés nem keveredhet: tekerés után az új hely mintái nem kerülnek a régi pufferbe.
     */
    bool fill(Stream& s) {
        std::lock_guard<std::mutex> lk(renderMutex_);
        if (flushReq_) return true;                 // előbb a puffer eldobása (a szál következő köre)
        UINT32 padding = 0;
        if (FAILED(s.client->GetCurrentPadding(&padding))) return false;
        if (written_ >= padding) consumed_ = written_ - padding;
        const UINT32 n = s.bufferFrames - padding;
        if (n == 0) return true;
        BYTE* data = nullptr;
        if (FAILED(s.render->GetBuffer(n, &data)) || !data) return false;
        if (render_) render_(reinterpret_cast<float*>(data), n);
        else std::memset(data, 0, n * 8);
        if (FAILED(s.render->ReleaseBuffer(n, 0))) return false;
        written_ += n;
        return true;
    }

    void run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
        auto stream = std::make_unique<Stream>();
        const bool ok = openStream(*stream);
        {
            std::lock_guard<std::mutex> lk(initMutex_);
            initState_ = ok ? 1 : -1;
        }
        initCv_.notify_all();
        if (!ok) stream.reset();
        while (!quit_) {
            if (!stream) {                              // újranyitás (eszközváltás után), kis szünetekkel
                if (WaitForSingleObject(wake_, 300) == WAIT_OBJECT_0 && quit_) break;
                stream = std::make_unique<Stream>();
                if (!openStream(*stream)) { stream.reset(); continue; }
                written_ = consumed_.load();           // a régi pufferben maradt rész elveszett: a pozíció nem ugrik vissza
                std::cout << "[Lejátszó] A hangkimenet újranyitva (az alapértelmezett lejátszóeszközön).\n";
            }
            if (flushReq_) {                            // tekerés: a pufferben várakozó régi minták eldobása
                std::lock_guard<std::mutex> lk(renderMutex_);
                if (stream->started) stream->client->Stop();
                stream->client->Reset();
                stream->started = false;
                written_ = 0;
                consumed_ = 0;
                flushReq_ = false;
            }
            if (active_ && !stream->started) {
                // indítás előtt egy teljes puffernyi adat, hogy az első pillanatban ne legyen alulfutás
                if (!fill(*stream) || FAILED(stream->client->Start())) { stream.reset(); continue; }
                stream->started = true;
            } else if (!active_ && stream->started) {
                stream->client->Stop();                // szünet: a pufferben maradt minták folytatáskor szólnak
                stream->started = false;
            }
            if (!stream->started) {
                WaitForSingleObject(wake_, 200);
                continue;
            }
            const HANDLE waits[2] = { stream->event, wake_ };
            WaitForMultipleObjects(2, waits, FALSE, 200);
            if (quit_) break;
            if (active_ && stream->started && !fill(*stream)) stream.reset();   // AUDCLNT_E_DEVICE_INVALIDATED stb.
        }
        stream.reset();
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        CoUninitialize();
    }

    uint32_t rate_ = 48000;
    RenderFn render_;
    std::mutex renderMutex_;
    std::thread thread_;
    HANDLE wake_ = nullptr;
    std::atomic<bool> active_{false}, quit_{false}, flushReq_{false};
    uint64_t written_ = 0;                  // az eszköznek átadott keretek (a szál írja, a render zárja alatt)
    std::atomic<uint64_t> consumed_{0};     // ebből már megszólalt (átadott − a pufferben várakozó)
    std::mutex initMutex_;
    std::condition_variable initCv_;
    int initState_ = 0;
};

} // namespace ee_analyzer::player
