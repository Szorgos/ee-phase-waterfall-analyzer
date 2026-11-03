#pragma once
/**
 * @file player_output_wasapi.hpp
 * @brief A beépített lejátszó hangkimenete Windowson: WASAPI az alapértelmezett lejátszóeszközre (eConsole).
 *
 * Megosztott mód (alapértelmezés): a hangfolyam azon az eszközön szól, amelyen az Equalizer APO-ban az Aqulify plugin
 * fut, így a hang a DSP-láncon átmegy, ugyanúgy, mint bármely más alkalmazásé. A hangfolyam a fájl saját mintavételi
 * frekvenciájával nyílik (32 bites float, sztereó); ha ez eltér az eszközétől, a Windows saját átalakítója igazítja
 * (AUTOCONVERTPCM, SRC_DEFAULT_QUALITY).
 *
 * Kizárólagos mód (setExclusive, kísérleti): a lejátszó egyedül használja az eszközt, a Windows keverője és az
 * Equalizer APO kimarad. A hangfolyam a fájl mintavételén nyílik; a formátum az első, amelyet az eszköz elfogad:
 * 32 bites float → 32 bites egész → 24 bit 32 bites keretben → 24 bit → 16 bit. A DSP-láncot ilyenkor a motor adja
 * (ExclusiveDsp), és ez a szál futtatja a puffer kitöltésekor; a kimenet mindig egy teljes periódus (állandó blokk,
 * a konvolver ehhez igazodik). Ha a kizárólagos megnyitás nem sikerül (foglalt eszköz, nem engedélyezett, nem
 * támogatott formátum vagy mintavétel), a kimenet a megosztott módra lép vissza, és ezt az állapot (statusJson) jelzi.
 *
 * Saját szál (MMCSS „Audio” / „Pro Audio”, eseményvezérelt puffer). A render a szál zárja alatt fut; a lejátszó
 * dekódoló szála a lock() / unlock() párral üríti a puffert tekeréskor. Ha az eszköz megszűnik vagy az alapértelmezett
 * eszköz változik (AUDCLNT_E_DEVICE_INVALIDATED), a szál az új alapértelmezett eszközön nyitja meg újra.
 */
#include <windows.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "file_player.hpp"
#include "pcm_convert.hpp"
#include "wasapi_loopback.hpp"   // GUID-ok és ComPtr

namespace ee_analyzer::player {

/** A kizárólagos mód DSP-je (a motor adja): a kimenet szála hívja. */
class ExclusiveDsp {
public:
    virtual ~ExclusiveDsp() = default;
    /**
     * Új kizárólagos hangfolyam, a feldolgozás előtt: mintavétel, blokkméret (keret), és a kimenet bitmélysége a
     * ditherhez (16 vagy 24; 0: lebegőpontos kimenet, a beállított érték marad).
     */
    virtual void prepare(uint32_t rate, uint32_t blockFrames, int ditherBits) = 0;
    /** Helyben, sztereó (váltakozó L, R) keretek; a blokk mindig a prepare blockFrames értéke. */
    virtual void process(float* stereo, size_t frames) = 0;
    /** A DSP késleltetése mintában (a jelút kijelzéséhez; bármely szálról). */
    virtual size_t latencySamples() const { return 0; }
};

/** A kimenet formátuma (kizárólagos módban az eszközé). */
struct WasapiFormat {
    WORD bits = 32;          // a minta mérete a pufferben
    WORD valid = 32;         // ebből érvényes
    bool isFloat = true;
    int ditherBits = 0;      // a lánc ditherének bitmélysége (0: lebegőpontos, a beállítás marad)
    const char* name = "32 bit float";
};

class WasapiPlayerOutput : public PlayerOutput {
public:
    ~WasapiPlayerOutput() override { close(); }

    /** A kizárólagos mód DSP-je (a motor élettartamáig él). nullptr: nincs (ilyenkor nincs kizárólagos mód). */
    void setExclusiveDsp(ExclusiveDsp* dsp) { dsp_ = dsp; }
    /** A kizárólagos mód kérése; szóló hangfolyamnál azonnal (a szál újranyitja, a pufferben lévő rövid rész kimarad). */
    void setExclusive(bool on) {
        if (wantExclusive_.exchange(on) == on) return;
        reopenReq_ = true;
        if (wake_) SetEvent(wake_);
    }
    bool wantExclusive() const { return wantExclusive_.load(); }
    /** Most kizárólagos módban szól-e (nyitott hangfolyam). */
    bool exclusiveActive() const { return exclusiveOn_.load(); }
    /** A legutóbbi kizárólagos megnyitás bitpontos volt-e eddig (nem veszett el bit a kimenet formátumára alakításkor). */
    bool quantExact() const { return !quantLoss_.load(); }

    /**
     * A jelút adatai a felületnek (a "path" objektum mezői, vessző nélkül kezdve): a kért és a tényleges mód, a
     * visszalépés oka, a kimenet formátuma, a puffer, a terhelés. dspMs: a DSP késleltetése (az összesbe számít).
     */
    std::string statusJson(double dspMs) const {
        std::lock_guard<std::mutex> lk(infoMutex_);
        const uint32_t rate = rate_ ? rate_ : 48000;
        const double bufMs = 1000.0 * static_cast<double>(infoBuffer_) / rate;
        // kizárólagos módban két periódus van úton (az egyiket az eszköz játssza, a másikat töltjük)
        const double totalMs = (exclusiveOn_.load() ? 2.0 * bufMs : bufMs) + dspMs;
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "\"quantum\":%u,\"bufReq\":0,\"bufMs\":%.2f,\"totalMs\":%.1f,\"load\":%.1f,\"peak\":%.1f,\"overruns\":%u,"
                      "\"exclusive\":{\"want\":%s,\"active\":%s,\"fallback\":\"%s\",\"hr\":\"0x%08lX\",\"format\":\"%s\","
                      "\"exact\":%s}",
                      infoBuffer_, bufMs, totalMs, 100.0 * loadAvg_.load(), 100.0 * loadPeak_.exchange(0.0f), overruns_.load(),
                      wantExclusive_.load() ? "true" : "false", exclusiveOn_.load() ? "true" : "false",
                      infoFallback_.c_str(), static_cast<unsigned long>(infoHr_), infoFormat_.c_str(),
                      quantLoss_.load() ? "false" : "true");
        return buf;
    }

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
        quantLoss_ = false;
        reopenReq_ = false;
        overruns_ = 0;
        loadAvg_ = 0.0f;
        loadPeak_ = 0.0f;
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
        exclusiveOn_ = false;
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

    /** float → az eszköz egész formátuma; false, ha valamelyik minta nem esett pontosan a rácsra (nem bitpontos). */
    static bool convert(const float* in, BYTE* out, size_t samples, const WasapiFormat& f) {
        if (f.isFloat) {
            std::memcpy(out, in, samples * sizeof(float));
            return true;
        }
        return pcm::floatToInt(in, out, samples, f.bits, f.valid);
    }

private:
    static constexpr IID kIID_IAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};

    struct Stream {
        win::ComPtr<IAudioClient> client;
        win::ComPtr<IAudioRenderClient> render;
        HANDLE event = nullptr;
        UINT32 bufferFrames = 0;
        bool started = false;
        bool exclusive = false;
        WasapiFormat fmt;
        std::vector<float> tmp;    // kizárólagos módban: a lejátszó és a lánc puffere (float, sztereó)
        ~Stream() {
            if (client && started) client->Stop();
            render.reset();
            client.reset();
            if (event) CloseHandle(event);
        }
    };

    static WAVEFORMATEXTENSIBLE makeFormat(uint32_t rate, const WasapiFormat& f) {
        WAVEFORMATEXTENSIBLE wf{};
        wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wf.Format.nChannels = 2;
        wf.Format.nSamplesPerSec = rate;
        wf.Format.wBitsPerSample = f.bits;
        wf.Format.nBlockAlign = static_cast<WORD>(2 * f.bits / 8);
        wf.Format.nAvgBytesPerSec = rate * wf.Format.nBlockAlign;
        wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wf.Samples.wValidBitsPerSample = f.valid;
        wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        wf.SubFormat = f.isFloat ? win::kSubtypeFloat : win::kSubtypePcm;
        return wf;
    }

    static bool defaultDevice(win::ComPtr<IMMDevice>& dev) {
        win::ComPtr<IMMDeviceEnumerator> en;
        if (FAILED(CoCreateInstance(win::kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, win::kIID_IMMDeviceEnumerator,
                                    reinterpret_cast<void**>(en.put()))))
            return false;
        return SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.put()));
    }

    /** Az esemény, a pufferméret és a render-szolgáltatás (az Initialize után). */
    static bool finishStream(Stream& s) {
        s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return s.event && SUCCEEDED(s.client->SetEventHandle(s.event)) && SUCCEEDED(s.client->GetBufferSize(&s.bufferFrames)) &&
               SUCCEEDED(s.client->GetService(kIID_IAudioRenderClient, reinterpret_cast<void**>(s.render.put())));
    }

    /** Megosztott mód (a mostani működés): 32 bites float, a Windows igazítja a mintavételt. */
    bool openShared(Stream& s, IMMDevice* dev) {
        if (FAILED(dev->Activate(win::kIID_IAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(s.client.put()))))
            return false;
        s.fmt = WasapiFormat{};
        WAVEFORMATEXTENSIBLE wf = makeFormat(rate_, s.fmt);
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        const REFERENCE_TIME buffer = 1000000;   // 100 ms
        if (FAILED(s.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buffer, 0,
                                        reinterpret_cast<WAVEFORMATEX*>(&wf), nullptr)))
            return false;
        s.exclusive = false;
        return finishStream(s);
    }

    /**
     * Kizárólagos mód: az első formátum, amelyet az eszköz a fájl mintavételén elfogad. hr: a hiba oka, ha nem sikerült
     * (AUDCLNT_E_DEVICE_IN_USE, AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED, AUDCLNT_E_UNSUPPORTED_FORMAT, ...).
     */
    bool openExclusive(Stream& s, IMMDevice* dev, HRESULT& hr) {
        static const WasapiFormat kFormats[] = {
            { 32, 32, true, 0, "32 bit float" },
            { 32, 32, false, 24, "32 bit" },
            { 32, 24, false, 24, "24/32 bit" },     // 24 érvényes bit 32 bites keretben
            { 24, 24, false, 24, "24 bit" },
            { 16, 16, false, 16, "16 bit" },
        };
        hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
        for (const WasapiFormat& f : kFormats) {
            WAVEFORMATEXTENSIBLE wf = makeFormat(rate_, f);
            // a periódus: 10 ms vagy az eszköz alapértéke, ha az nagyobb (egy-egy ébredéskor egy teljes periódus megy ki)
            REFERENCE_TIME period = 0;
            for (int attempt = 0; attempt < 3; ++attempt) {
                s.render.reset();
                s.client.reset();
                if (FAILED(hr = dev->Activate(win::kIID_IAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(s.client.put()))))
                    return false;
                if (period == 0) {
                    REFERENCE_TIME def = 0, minimum = 0;
                    if (FAILED(s.client->GetDevicePeriod(&def, &minimum))) def = 100000;
                    period = std::max<REFERENCE_TIME>({ def, minimum, 100000 });
                }
                hr = s.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                          reinterpret_cast<WAVEFORMATEX*>(&wf), nullptr);
                if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                    // az eszköz igazított pufferméretéhez tartozó periódus, új IAudioClienttel
                    UINT32 frames = 0;
                    if (FAILED(s.client->GetBufferSize(&frames)) || frames == 0) break;
                    period = static_cast<REFERENCE_TIME>(10000000.0 * frames / rate_ + 0.5);
                    continue;
                }
                break;
            }
            if (SUCCEEDED(hr)) {
                s.fmt = f;
                s.exclusive = true;
                if (!finishStream(s)) { hr = E_FAIL; return false; }
                s.tmp.assign(static_cast<size_t>(s.bufferFrames) * 2, 0.0f);
                return true;
            }
            // foglalt vagy tiltott eszköz: más formátummal sem fog menni
            if (hr == AUDCLNT_E_DEVICE_IN_USE || hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED || hr == AUDCLNT_E_DEVICE_INVALIDATED)
                return false;
        }
        return false;
    }

    static const char* fallbackName(HRESULT hr) {
        if (hr == AUDCLNT_E_DEVICE_IN_USE) return "busy";
        if (hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) return "notAllowed";
        if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) return "format";
        return "error";
    }

    /** Megnyitás az alapértelmezett eszközön (kizárólagos, ha kérték és van DSP; különben / sikertelenül megosztott). */
    bool openStream(Stream& s) {
        win::ComPtr<IMMDevice> dev;
        if (!defaultDevice(dev)) return false;
        std::string fallback;
        HRESULT why = S_OK;
        if (wantExclusive_ && dsp_) {
            if (openExclusive(s, dev.p, why)) {
                dsp_->prepare(rate_, s.bufferFrames, s.fmt.ditherBits);
                std::cout << "[Lejátszó] Kizárólagos mód (WASAPI exclusive): " << s.fmt.name << ", " << rate_ << " Hz, "
                          << s.bufferFrames << " keret periódusonként.\n";
            } else {
                fallback = fallbackName(why);
                char hx[16];
                std::snprintf(hx, sizeof(hx), "0x%08lX", static_cast<unsigned long>(why));
                std::cout << "[Lejátszó] A kizárólagos mód nem indult el (" << fallback << ", " << hx
                          << "); megosztott módban szól.\n";
                s.render.reset();
                s.client.reset();
            }
        }
        if (!s.exclusive && !openShared(s, dev.p)) return false;
        {
            std::lock_guard<std::mutex> lk(infoMutex_);
            infoFallback_ = fallback;
            infoHr_ = why;
            infoFormat_ = s.exclusive ? s.fmt.name : "";
            infoBuffer_ = s.bufferFrames;
        }
        exclusiveOn_ = s.exclusive;
        return true;
    }

    /** A terhelés: a kitöltés ideje a puffer idejéhez képest. */
    void noteLoad(std::chrono::steady_clock::time_point t0, UINT32 frames) {
        if (frames == 0 || rate_ == 0) return;
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const float load = static_cast<float>(dt * rate_ / frames);
        const float avg = loadAvg_.load(std::memory_order_relaxed);
        loadAvg_.store(avg + 0.05f * (load - avg), std::memory_order_relaxed);
        if (load > loadPeak_.load(std::memory_order_relaxed)) loadPeak_.store(load, std::memory_order_relaxed);
        if (load > 1.0f) overruns_.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * A puffer szabad részének kitöltése; false: az eszköz megszűnt (újranyitás kell). A render zárja alatt fut, így
     * a tekerés (flush) és a kitöltés nem keveredhet: tekerés után az új hely mintái nem kerülnek a régi pufferbe.
     * Kizárólagos módban mindig egy teljes periódus (eseményvezérelt kizárólagos mód), a DSP-láncon át.
     */
    bool fill(Stream& s) {
        std::lock_guard<std::mutex> lk(renderMutex_);
        if (flushReq_) return true;                 // előbb a puffer eldobása (a szál következő köre)
        const auto t0 = std::chrono::steady_clock::now();
        UINT32 padding = 0;
        if (FAILED(s.client->GetCurrentPadding(&padding))) return false;
        if (padding > s.bufferFrames) padding = s.bufferFrames;
        if (written_ >= padding) consumed_ = written_ - padding;
        const UINT32 n = s.exclusive ? s.bufferFrames : s.bufferFrames - padding;
        if (n == 0) return true;
        BYTE* data = nullptr;
        const HRESULT hr = s.render->GetBuffer(n, &data);
        if (hr == AUDCLNT_E_BUFFER_TOO_LARGE) return true;   // kizárólagos módban: az eszköz még nem kérte (pl. folytatás)
        if (FAILED(hr) || !data) return false;
        if (s.exclusive) {
            float* f = s.tmp.data();
            if (render_) render_(f, n);
            else std::fill(f, f + static_cast<size_t>(n) * 2, 0.0f);
            if (dsp_) dsp_->process(f, n);
            if (!convert(f, data, static_cast<size_t>(n) * 2, s.fmt)) quantLoss_.store(true, std::memory_order_relaxed);
        } else {
            if (render_) render_(reinterpret_cast<float*>(data), n);
            else std::memset(data, 0, n * 8);
        }
        if (FAILED(s.render->ReleaseBuffer(n, 0))) return false;
        written_ += n;
        noteLoad(t0, n);
        return true;
    }

    void run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(wantExclusive_ ? L"Pro Audio" : L"Audio", &taskIndex);
        auto stream = std::make_unique<Stream>();
        const bool ok = openStream(*stream);
        {
            std::lock_guard<std::mutex> lk(initMutex_);
            initState_ = ok ? 1 : -1;
        }
        initCv_.notify_all();
        if (!ok) stream.reset();
        while (!quit_) {
            if (reopenReq_.exchange(false) && stream) {   // mód váltása (kizárólagos / megosztott): új hangfolyam
                std::lock_guard<std::mutex> lk(renderMutex_);
                stream.reset();
                SetEvent(wake_);                        // az újranyitás azonnal, várakozás nélkül
            }
            if (!stream) {                              // újranyitás (eszközváltás után), kis szünetekkel
                exclusiveOn_ = false;
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
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 200);
            if (quit_) break;
            // kizárólagos módban csak az eszköz jelzésére (egy periódus szabad); megosztottban bármikor a szabad rész
            if (stream->exclusive && w != WAIT_OBJECT_0) {
                UINT32 pad = 0;   // 200 ms jelzés nélkül: megszűnt-e az eszköz
                if (w == WAIT_TIMEOUT && active_ && FAILED(stream->client->GetCurrentPadding(&pad))) stream.reset();
                continue;
            }
            if (active_ && stream->started && !fill(*stream)) stream.reset();   // AUDCLNT_E_DEVICE_INVALIDATED stb.
        }
        stream.reset();
        exclusiveOn_ = false;
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

    // kizárólagos mód
    ExclusiveDsp* dsp_ = nullptr;
    std::atomic<bool> wantExclusive_{false}, exclusiveOn_{false}, quantLoss_{false}, reopenReq_{false};
    // a felületnek
    mutable std::mutex infoMutex_;
    std::string infoFallback_, infoFormat_;
    HRESULT infoHr_ = S_OK;
    UINT32 infoBuffer_ = 0;
    mutable std::atomic<float> loadAvg_{0.0f}, loadPeak_{0.0f};
    std::atomic<uint32_t> overruns_{0};
};

} // namespace ee_analyzer::player
