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
 * dekódoló szála a lock() / unlock() párral üríti a puffert tekeréskor. Ha az eszköz megszűnik
 * (AUDCLNT_E_DEVICE_INVALIDATED), a szál az új alapértelmezett eszközön nyitja meg újra. Ha a régi eszköz megmarad, de
 * az alapértelmezett megváltozik (pl. bedugott USB DAC), azt a szál másodpercenként ellenőrzi, és ha az új eszköz
 * tartósan az alapértelmezett (device_follow.hpp), arra vált át. A naplósorok az eszköz nevét is tartalmazzák.
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

#include "device_follow.hpp"
#include "exclusive_dsp.hpp"
#include "file_player.hpp"
#include "pcm_convert.hpp"
#include "watchdog.hpp"
#include "wasapi_loopback.hpp"   // GUID-ok és ComPtr

namespace ee_analyzer::player {

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
    explicit WasapiPlayerOutput(const char* hbName = "player-wasapi") : hb_(hbName ? hbName : "player-wasapi", &hbRunning_) {}
    ~WasapiPlayerOutput() override { close(); }

    /** Opcionális céleszköz WASAPI-azonosítója (üres = alapértelmezett lejátszóeszköz). */
    void setTargetDeviceId(const std::wstring& id) {
        std::lock_guard<std::mutex> lk(targetMutex_);
        if (targetDeviceId_ == id) return;
        targetDeviceId_ = id;
        reopenReq_ = true;
        if (wake_) SetEvent(wake_);
    }
    std::wstring targetDeviceId() const {
        std::lock_guard<std::mutex> lk(targetMutex_);
        return targetDeviceId_;
    }

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
    /**
     * A legutóbbi nyitásnál miért nem indult a kizárólagos mód ("busy", "format", "notAllowed", "error"; üres: sikerült
     * vagy nem kérték). Sikertelen nyitás után is olvasható (a megosztott visszalépés is meghiúsulhatott).
     */
    std::string lastExclusiveFailure() const {
        std::lock_guard<std::mutex> lk(infoMutex_);
        return lastExclFail_;
    }
    /** Natív DSD (DoP 1.1) kérése (a jelút-állapot számára). */
    void setWantDop(bool on) { wantDop_.store(on); }
    bool wantDop() const { return wantDop_.load(); }
    /** Most natív DSD (DoP 1.1) hangfolyam szól-e kizárólagos módban. */
    bool dopActive() const { return dopOn_.load(); }
    /** Az éppen nyitott hangfolyam mintavételi frekvenciája (Hz). */
    uint32_t currentRate() const { return rate_; }
    /** Kizárólagos módban a DAC a mintavétel váltásakor új órajelre áll: a lejátszó rövid csenddel kezd. */
    bool settlesOnRateChange() const override { return exclusiveOn_.load(); }
    std::vector<uint32_t> supportedRates() const override {
        static const uint32_t kCandidates[] = { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000 };
        if (wantExclusive_.load()) {
            // Kizárólagos módban a cél (vagy alapértelmezett) WASAPI eszköz támogatott mintavételei
            std::vector<uint32_t> res;
            win::ComPtr<IMMDevice> dev;
            if (openTargetDevice(dev)) {
                win::ComPtr<IAudioClient> cli;
                if (SUCCEEDED(dev->Activate(win::kIID_IAudioClient, CLSCTX_ALL, nullptr,
                                            reinterpret_cast<void**>(cli.put())))) {
                    static const WasapiFormat kProbeFmts[] = {
                        { 32, 32, true,  0, "32 bit float" },
                        { 32, 32, false, 0, "32 bit int" },
                        { 32, 24, false, 0, "24 bit (32)" },
                        { 16, 16, false, 0, "16 bit" },
                    };
                    for (uint32_t r : kCandidates) {
                        for (const WasapiFormat& f : kProbeFmts) {
                            WAVEFORMATEXTENSIBLE wf = makeFormat(r, f);
                            if (cli->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                       reinterpret_cast<const WAVEFORMATEX*>(&wf), nullptr) == S_OK) {
                                res.push_back(r);
                                break;
                            }
                        }
                    }
                }
            }
            if (!res.empty()) return res;
        } else {
            // Megosztott módban a Windows keverője a megosztott formátum (mix format) mintavételén fut, minden mást ő
            // alakít át: a lejátszó ide alakít (fel- vagy lefelé) a választott motorral
            win::ComPtr<IMMDevice> dev;
            if (openTargetDevice(dev)) {
                win::ComPtr<IAudioClient> cli;
                WAVEFORMATEX* mix = nullptr;
                if (SUCCEEDED(dev->Activate(win::kIID_IAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(cli.put()))) &&
                    SUCCEEDED(cli->GetMixFormat(&mix)) && mix) {
                    const uint32_t r = mix->nSamplesPerSec;
                    CoTaskMemFree(mix);
                    if (r >= 8000) return { r };
                }
            }
        }
        return { 44100, 48000, 88200, 96000, 176400, 192000 };
    }
    uint32_t actualRate() const override {
        return thread_.joinable() ? rate_ : 0;
    }
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
        char buf[640];
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
            renderDop_ = nullptr;
        }
        dopMode_ = false;
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

    /**
     * Natív DSD (DoP 1.1) hangfolyam megnyitása kizárólagos WASAPI kimeneten (24 bit 32 bites tárolóban, MSB-igazítva).
     * Ha a kizárólagos mód nincs bekapcsolva vagy a hangeszköz nem fogadja el a DoP-mintavételt / formátumot, false-szal
     * tér vissza (error: "format", "busy", "notAllowed", "error"), és a FilePlayer a DSD->PCM átalakításra lép vissza.
     */
    bool openDop(uint32_t dopSampleRate, RenderDopFn render, std::string* error) override {
        close();
        if (!wantExclusive_.load() || !dsp_) {
            if (error) *error = "notAllowed";
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(renderMutex_);
            render_ = nullptr;
            renderDop_ = std::move(render);
        }
        dopMode_ = true;
        rate_ = dopSampleRate;
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
        std::unique_lock<std::mutex> lk(initMutex_);
        initCv_.wait(lk, [this] { return initState_ != 0; });
        if (initState_ < 0) {
            lk.unlock();
            std::string fb;
            {
                std::lock_guard<std::mutex> ik(infoMutex_);
                fb = infoDopFallback_;
            }
            close();
            if (error) *error = fb.empty() ? "format" : fb;
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
        dopOn_ = false;
        dopMode_ = false;
        std::lock_guard<std::mutex> lk(renderMutex_);
        render_ = nullptr;
        renderDop_ = nullptr;
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

    /**
     * 24 bites DoP szó (az int32_t alsó 24 bitjén: [23:16]=marker, [15:8]=b0, [7:0]=b1) bitpontos átadása a WASAPI
     * kizárólagos pufferébe: 32 bites keretben (24/32 vagy 32/32) a WAVEFORMATEXTENSIBLE előírása szerint MSB-igazítva
     * (8 bittel balra tolva, a felső 24 biten: [31:24]=marker, [23:16]=b0, [15:8]=b1, [7:0]=0), 24 bites tömörített
     * keretben 3 kis-endián bájton (b1, b0, marker).
     */
    static void convertDop(const int32_t* in, BYTE* out, size_t samples, const WasapiFormat& f) {
        if (f.bits == 32) {
            for (size_t i = 0; i < samples; ++i) {
                const uint32_t w = (static_cast<uint32_t>(in[i]) & 0x00FFFFFFu) << 8;
                std::memcpy(out + i * 4, &w, 4);
            }
        } else if (f.bits == 24) {
            for (size_t i = 0; i < samples; ++i) {
                const uint32_t u = static_cast<uint32_t>(in[i]);
                out[i * 3]     = static_cast<uint8_t>(u & 0xFFu);
                out[i * 3 + 1] = static_cast<uint8_t>((u >> 8) & 0xFFu);
                out[i * 3 + 2] = static_cast<uint8_t>((u >> 16) & 0xFFu);
            }
        }
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
        bool dop = false;
        std::wstring devId;           // az eszköz WASAPI-azonosítója (az eszközkövetéshez)
        std::string devName;          // az eszköz neve (a naplóhoz)
        WasapiFormat fmt;
        std::vector<float> tmp;       // kizárólagos PCM módban: a lejátszó és a lánc puffere (float, sztereó)
        std::vector<int32_t> tmpDop;  // kizárólagos DoP módban: 24 bites DoP szavak puffere (int32_t, sztereó)
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

    bool openTargetDevice(win::ComPtr<IMMDevice>& dev) const {
        const std::wstring target = targetDeviceId();
        if (target.empty()) return defaultDevice(dev);
        win::ComPtr<IMMDeviceEnumerator> en;
        if (FAILED(CoCreateInstance(win::kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, win::kIID_IMMDeviceEnumerator,
                                    reinterpret_cast<void**>(en.put()))))
            return false;
        return SUCCEEDED(en->GetDevice(target.c_str(), dev.put()));
    }

    static bool defaultDevice(win::ComPtr<IMMDevice>& dev) {
        win::ComPtr<IMMDeviceEnumerator> en;
        if (FAILED(CoCreateInstance(win::kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, win::kIID_IMMDeviceEnumerator,
                                    reinterpret_cast<void**>(en.put()))))
            return false;
        return SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.put()));
    }

    static std::wstring deviceId(IMMDevice* dev) {
        LPWSTR id = nullptr;
        std::wstring out;
        if (dev && SUCCEEDED(dev->GetId(&id)) && id) {
            out = id;
            CoTaskMemFree(id);
        }
        return out;
    }

    /** Az eszköz neve UTF-8-ban (a naplóhoz), pl. "Hangszórók (HIFI DSD)". */
    static std::string deviceName(IMMDevice* dev) {
        std::string name = "ismeretlen eszköz";
        win::ComPtr<IPropertyStore> props;
        if (dev && SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.put()))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(win::kPKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
                const int n = WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                if (n > 1) {
                    name.assign(static_cast<size_t>(n - 1), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, name.data(), n, nullptr, nullptr);
                }
            }
            PropVariantClear(&pv);
        }
        return name;
    }

    /** A most alapértelmezett lejátszóeszköz azonosítója (üres, ha nincs). */
    static std::wstring defaultDeviceIdNow(IMMDeviceEnumerator* en) {
        if (!en) return {};
        win::ComPtr<IMMDevice> dev;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.put()))) return {};
        return deviceId(dev.p);
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

    /**
     * Kizárólagos DoP 1.1 hangfolyam megnyitása: előnyben a 24 bit 32 bites keretben (WAVEFORMATEXTENSIBLE
     * wBitsPerSample = 32, wValidBitsPerSample = 24, KSDATAFORMAT_SUBTYPE_PCM), majd a 32/32 és a 24/24 egész PCM.
     * 16 bites vagy lebegőpontos formátumon a DoP nem működik.
     */
    bool openDopExclusive(Stream& s, IMMDevice* dev, HRESULT& hr) {
        static const WasapiFormat kDopFormats[] = {
            { 32, 24, false, 0, "24/32 bit DoP" },
            { 32, 32, false, 0, "32 bit DoP" },
            { 24, 24, false, 0, "24 bit DoP" },
        };
        hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
        for (const WasapiFormat& f : kDopFormats) {
            WAVEFORMATEXTENSIBLE wf = makeFormat(rate_, f);
            REFERENCE_TIME period = 0;
            for (int attempt = 0; attempt < 3; ++attempt) {
                s.render.reset();
                s.client.reset();
                if (FAILED(hr = dev->Activate(win::kIID_IAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(s.client.put()))))
                    return false;
                hr = s.client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, reinterpret_cast<WAVEFORMATEX*>(&wf), nullptr);
                if (FAILED(hr)) break;
                if (period == 0) {
                    REFERENCE_TIME def = 0, minimum = 0;
                    if (FAILED(s.client->GetDevicePeriod(&def, &minimum))) def = 100000;
                    period = std::max<REFERENCE_TIME>({ def, minimum, 100000 });
                }
                hr = s.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                          reinterpret_cast<WAVEFORMATEX*>(&wf), nullptr);
                if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
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
                s.dop = true;
                if (!finishStream(s)) { hr = E_FAIL; return false; }
                s.tmpDop.assign(static_cast<size_t>(s.bufferFrames) * 2, 0);
                return true;
            }
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

    /** Megnyitás a cél (vagy alapértelmezett) eszközön (kizárólagos, ha kérték és van DSP; különben / sikertelenül megosztott). */
    bool openStream(Stream& s) {
        win::ComPtr<IMMDevice> dev;
        if (!openTargetDevice(dev)) return false;
        s.devId = deviceId(dev.p);
        s.devName = deviceName(dev.p);
        if (dopMode_) {
            if (!wantExclusive_ || !dsp_) return false;
            HRESULT why = S_OK;
            if (!openDopExclusive(s, dev.p, why)) {
                const std::string fb = fallbackName(why);
                char hx[16];
                std::snprintf(hx, sizeof(hx), "0x%08lX", static_cast<unsigned long>(why));
                std::cout << "[Lejátszó] A natív DSD (DoP) nem indult el (" << fb << ", " << hx
                          << ", eszköz: " << s.devName << "); PCM-re alakítva szól.\n";
                {
                    std::lock_guard<std::mutex> lk(infoMutex_);
                    infoDopFallback_ = fb;
                    infoDopHr_ = why;
                }
                s.render.reset();
                s.client.reset();
                return false;
            }
            std::cout << "[Lejátszó] Natív DSD (DoP, WASAPI exclusive): " << s.fmt.name << ", " << rate_ << " Hz, "
                      << s.bufferFrames << " keret periódusonként (DSP és digitális hangerő kihagyva), eszköz: "
                      << s.devName << ".\n";
            {
                std::lock_guard<std::mutex> lk(infoMutex_);
                infoFallback_.clear();
                infoHr_ = S_OK;
                infoDopFallback_.clear();
                infoDopHr_ = S_OK;
                infoFormat_ = s.fmt.name;
                infoBuffer_ = s.bufferFrames;
            }
            exclusiveOn_ = true;
            dopOn_ = true;
            return true;
        }
        std::string fallback;
        HRESULT why = S_OK;
        {
            std::lock_guard<std::mutex> lk(infoMutex_);
            lastExclFail_.clear();
        }
        if (wantExclusive_ && dsp_) {
            if (openExclusive(s, dev.p, why)) {
                dsp_->prepare(rate_, s.bufferFrames, s.fmt.ditherBits);
                std::cout << "[Lejátszó] Kizárólagos mód (WASAPI exclusive): " << s.fmt.name << ", " << rate_ << " Hz, "
                          << s.bufferFrames << " keret periódusonként, eszköz: " << s.devName << ".\n";
            } else {
                fallback = fallbackName(why);
                char hx[16];
                std::snprintf(hx, sizeof(hx), "0x%08lX", static_cast<unsigned long>(why));
                std::cout << "[Lejátszó] A kizárólagos mód nem indult el (" << fallback << ", " << hx
                          << ", eszköz: " << s.devName << "); megosztott módban szól.\n";
                {
                    std::lock_guard<std::mutex> lk(infoMutex_);
                    lastExclFail_ = fallback;   // akkor is megmarad, ha a megosztott nyitás sem sikerül
                }
                s.render.reset();
                s.client.reset();
            }
        }
        if (!s.exclusive && !openShared(s, dev.p)) return false;
        if (!s.exclusive) std::cout << "[Lejátszó] Megosztott mód (WASAPI shared), eszköz: " << s.devName << ".\n";
        {
            std::lock_guard<std::mutex> lk(infoMutex_);
            infoFallback_ = fallback;
            infoHr_ = why;
            infoFormat_ = s.exclusive ? s.fmt.name : "";
            infoBuffer_ = s.bufferFrames;
        }
        exclusiveOn_ = s.exclusive;
        dopOn_ = false;
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
        if (s.dop) {
            int32_t* d = s.tmpDop.data();
            if (renderDop_) {
                renderDop_(d, n);
            } else {
                for (UINT32 i = 0; i < n; ++i) {
                    const uint8_t marker = ((written_ + i) & 1u) ? 0xFAu : 0x05u;
                    const int32_t w = dsd::packDopSample(dsd::kDsdSilenceByte, dsd::kDsdSilenceByte, marker);
                    d[2 * i] = w;
                    d[2 * i + 1] = w;
                }
            }
            // DoP módban a DSP-lánc, a digitális hangerő és a mérők (dsp_->process) teljesen kimaradnak
            convertDop(d, data, static_cast<size_t>(n) * 2, s.fmt);
        } else if (s.exclusive) {
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
        // Az alapértelmezett eszköz követése (bedugott USB DAC): másodpercenkénti ellenőrzés, váltás csak tartós változásra
        DefaultDeviceFollow<std::wstring> follow;
        win::ComPtr<IMMDeviceEnumerator> en;
        CoCreateInstance(win::kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, win::kIID_IMMDeviceEnumerator,
                         reinterpret_cast<void**>(en.put()));
        auto lastDeviceCheck = std::chrono::steady_clock::now();
        if (ok) follow.opened(stream->devId);
        else stream.reset();
        hb_.beat("wasapi: start");
        hbRunning_ = true;
        while (!quit_) {
            hb_.beat("wasapi: loop");
            if (reopenReq_.exchange(false) && stream) {   // mód váltása (kizárólagos / megosztott): új hangfolyam
                std::lock_guard<std::mutex> lk(renderMutex_);
                stream.reset();
                SetEvent(wake_);                        // az újranyitás azonnal, várakozás nélkül
            }
            // Natív DSD (DoP) alatt nem vált: az új eszköz nem biztos, hogy fogadja a DoP-ot, és a DoP-hangfolyam nem esik
            // vissza PCM-re menet közben; a következő szám megnyitása már az új alapértelmezett eszközön történik.
            const auto now = std::chrono::steady_clock::now();
            if (stream && en && !dopMode_ && now - lastDeviceCheck >= std::chrono::seconds(1)) {
                lastDeviceCheck = now;
                hb_.beat("wasapi: device check");
                if (targetDeviceId().empty() && follow.observe(defaultDeviceIdNow(en.p), now)) {
                    std::cout << "[Lejátszó] Az alapértelmezett lejátszóeszköz megváltozott (eddig: " << stream->devName
                              << "); a hangkimenet átvált.\n";
                    std::lock_guard<std::mutex> lk(renderMutex_);
                    stream.reset();
                    SetEvent(wake_);
                }
            }
            if (!stream) {                              // újranyitás (eszközváltás után), kis szünetekkel
                exclusiveOn_ = false;
                dopOn_ = false;
                if (WaitForSingleObject(wake_, 300) == WAIT_OBJECT_0 && quit_) break;
                stream = std::make_unique<Stream>();
                if (!openStream(*stream)) { stream.reset(); continue; }
                follow.opened(stream->devId);
                written_ = consumed_.load();           // a régi pufferben maradt rész elveszett: a pozíció nem ugrik vissza
                std::cout << "[Lejátszó] A hangkimenet újranyitva az alapértelmezett lejátszóeszközön: " << stream->devName
                          << ".\n";
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
            hb_.beat("wasapi: fill");
            if (active_ && stream->started && !fill(*stream)) stream.reset();   // AUDCLNT_E_DEVICE_INVALIDATED stb.
        }
        hb_.beat("wasapi: close");
        stream.reset();
        en.reset();
        exclusiveOn_ = false;
        dopOn_ = false;
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        CoUninitialize();
        hbRunning_ = false;
    }

    uint32_t rate_ = 48000;
    RenderFn render_;
    RenderDopFn renderDop_;
    std::mutex renderMutex_;
    std::thread thread_;
    // Őrszem (beépített hibajelentés): a kimeneti szál körönként jelez (legfeljebb 300 ms-onként); csak futó szálnál
    // várunk jelzést. A regisztráció a létrehozó szálon: a hangszálon csak atomikus írás van.
    std::atomic<bool> hbRunning_{false};
    aq::wd::Heartbeat hb_;
    mutable std::mutex targetMutex_;
    std::wstring targetDeviceId_;
    HANDLE wake_ = nullptr;
    std::atomic<bool> active_{false}, quit_{false}, flushReq_{false};
    uint64_t written_ = 0;                  // az eszköznek átadott keretek (a szál írja, a render zárja alatt)
    std::atomic<uint64_t> consumed_{0};     // ebből már megszólalt (átadott − a pufferben várakozó)
    std::mutex initMutex_;
    std::condition_variable initCv_;
    int initState_ = 0;

    // kizárólagos mód és natív DSD (DoP 1.1)
    ExclusiveDsp* dsp_ = nullptr;
    bool dopMode_ = false;
    std::atomic<bool> wantExclusive_{false}, exclusiveOn_{false}, wantDop_{false}, dopOn_{false};
    std::atomic<bool> quantLoss_{false}, reopenReq_{false};
    // a felületnek
    mutable std::mutex infoMutex_;
    std::string infoFallback_, infoFormat_, infoDopFallback_;
    std::string lastExclFail_;   // a legutóbbi nyitásnál a kizárólagos mód sikertelenségének oka (üres: sikerült / nem kérték)
    HRESULT infoHr_ = S_OK, infoDopHr_ = S_OK;
    UINT32 infoBuffer_ = 0;
    mutable std::atomic<float> loadAvg_{0.0f}, loadPeak_{0.0f};
    std::atomic<uint32_t> overruns_{0};
};

} // namespace ee_analyzer::player
