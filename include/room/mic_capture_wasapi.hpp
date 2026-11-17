#pragma once
/**
 * @file mic_capture_wasapi.hpp
 * @brief Mikrofonfelvétel WASAPI-val (Windows) – a `mic_capture.hpp` felületének megvalósítása.
 *
 * Megosztott módú felvétel a kiválasztott bemenetről (eCapture végpont), vagy – teszthez és visszacsatolt méréshez –
 * egy kimenet hangjáról (eRender végpont, loopback). A kért formátum monó, 32 bites lebegőpontos, a kért mintavételen;
 * az átalakítást a Windows audiomotorja végzi (AUTOCONVERTPCM + SRC_DEFAULT_QUALITY). Saját szálon fut.
 * Ha a Windows adatvédelmi beállítása tiltja a mikrofont, az Activate / Initialize E_ACCESSDENIED-del tér vissza:
 * ilyenkor "permission" hiba, nem néma felvétel.
 *
 * Forrás: Microsoft Learn – Core Audio APIs: IMMDeviceEnumerator, IAudioClient::Initialize (AUDCLNT_STREAMFLAGS_*),
 * IAudioCaptureClient; Capturing a Stream; Loopback Recording.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mic_capture.hpp"

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

namespace aqulify::room {

namespace wasapi_detail {
// GUID-ok közvetlenül (MinGW és MSVC alatt egyformán fordul; ugyanazok, mint a windows/engine/wasapi_loopback.hpp-ban)
static const CLSID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID kIID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID kIID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID kIID_IAudioCaptureClient = {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
static const PROPERTYKEY kPKEY_FriendlyName = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

template <class T>
struct Com {
    T* p = nullptr;
    ~Com() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** put() { reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

inline std::string utf8(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

inline std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

/** COM a hívó szálon, ha még nincs (a hívás végén csak azt bontja, amit maga nyitott). */
struct ComScope {
    bool own = false;
    ComScope() { own = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~ComScope() { if (own) CoUninitialize(); }
};
} // namespace wasapi_detail

class WasapiMicCapture final : public MicCapture {
public:
    ~WasapiMicCapture() override { stop(); }

    std::vector<InputDevice> listInputs(bool includeMonitors) override {
        using namespace wasapi_detail;
        std::vector<InputDevice> out;
        ComScope com;
        Com<IMMDeviceEnumerator> en;
        if (FAILED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator,
                                    reinterpret_cast<void**>(en.put()))))
            return out;
        auto add = [&](EDataFlow flow, bool monitor) {
            std::wstring defId;
            {
                Com<IMMDevice> def;
                LPWSTR id = nullptr;
                if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, def.put())) && SUCCEEDED(def->GetId(&id)) && id) {
                    defId = id;
                    CoTaskMemFree(id);
                }
            }
            Com<IMMDeviceCollection> col;
            if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, col.put()))) return;
            UINT n = 0;
            col->GetCount(&n);
            for (UINT i = 0; i < n; ++i) {
                Com<IMMDevice> dev;
                if (FAILED(col->Item(i, dev.put()))) continue;
                LPWSTR id = nullptr;
                if (FAILED(dev->GetId(&id)) || !id) continue;
                InputDevice d;
                d.id = utf8(id);
                d.isDefault = defId == id;
                d.isMonitor = monitor;
                CoTaskMemFree(id);
                Com<IPropertyStore> props;
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.put()))) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(props->GetValue(kPKEY_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) d.name = utf8(pv.pwszVal);
                    PropVariantClear(&pv);
                }
                if (d.name.empty()) d.name = d.id;
                out.push_back(std::move(d));
            }
        };
        add(eCapture, false);
        if (includeMonitors) add(eRender, true);
        return out;
    }

    bool start(const std::string& deviceId, uint32_t sampleRate, double maxSeconds, std::string& error) override {
        stop();
        if (sampleRate < 8000 || sampleRate > 384000 || !(maxSeconds > 0.0) || maxSeconds > 600.0) {
            error = "format";
            return false;
        }
        buffer_.reset(static_cast<size_t>(maxSeconds * sampleRate) + 1);
        {
            std::lock_guard<std::mutex> lk(m_);
            started_ = false;
            startError_.clear();
        }
        running_ = true;
        thread_ = std::thread(&WasapiMicCapture::threadMain, this, deviceId, sampleRate);
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, std::chrono::seconds(3), [this] { return started_ || !startError_.empty(); });
        if (started_) return true;
        error = startError_.empty() ? "no_device" : startError_;
        lk.unlock();
        stop();
        return false;
    }

    std::vector<float> stop() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        return buffer_.take();
    }

    bool running() const override { return running_; }
    float takePeak() override { return buffer_.takePeak(); }
    size_t recordedFrames() const override { return buffer_.written(); }

private:
    void fail(const std::string& e) {
        std::lock_guard<std::mutex> lk(m_);
        startError_ = e;
        cv_.notify_all();
    }

    void threadMain(std::string deviceId, uint32_t sampleRate) {
        using namespace wasapi_detail;
        ComScope com;
        Com<IMMDeviceEnumerator> en;
        if (FAILED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator,
                                    reinterpret_cast<void**>(en.put())))) {
            fail("backend");
            running_ = false;
            return;
        }
        Com<IMMDevice> dev;
        bool loopback = false;
        HRESULT hr;
        if (deviceId.empty()) {
            hr = en->GetDefaultAudioEndpoint(eCapture, eConsole, dev.put());
        } else {
            hr = en->GetDevice(wide(deviceId).c_str(), dev.put());
            if (SUCCEEDED(hr)) {
                // a kimeneti végpont „monitorja”: loopback-felvétel
                Com<IMMEndpoint> ep;
                static const IID kIID_IMMEndpoint = {0x1BE09788, 0x6894, 0x4089, {0x85, 0x86, 0x9A, 0x2A, 0x6C, 0x26, 0x5A, 0xC5}};
                EDataFlow flow = eCapture;
                if (SUCCEEDED(dev->QueryInterface(kIID_IMMEndpoint, reinterpret_cast<void**>(ep.put()))) && ep)
                    ep->GetDataFlow(&flow);
                loopback = flow == eRender;
            }
        }
        if (FAILED(hr) || !dev) { fail("no_device"); running_ = false; return; }

        Com<IAudioClient> client;
        hr = dev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.put()));
        if (FAILED(hr)) { fail(hr == E_ACCESSDENIED ? "permission" : "busy"); running_ = false; return; }

        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 1;
        wf.nSamplesPerSec = sampleRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = 4;
        wf.nAvgBytesPerSec = sampleRate * 4;
        DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000 /* 200 ms */, 0, &wf, nullptr);
        if (FAILED(hr)) {
            fail(hr == E_ACCESSDENIED ? "permission" : hr == AUDCLNT_E_DEVICE_IN_USE ? "busy" : "format");
            running_ = false;
            return;
        }
        Com<IAudioCaptureClient> cap;
        if (FAILED(client->GetService(kIID_IAudioCaptureClient, reinterpret_cast<void**>(cap.put()))) ||
            FAILED(client->Start())) {
            fail("backend");
            running_ = false;
            return;
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            started_ = true;
            cv_.notify_all();
        }
        std::vector<float> zeros(48000, 0.0f);
        while (running_) {
            Sleep(5);
            UINT32 packet = 0;
            if (FAILED(cap->GetNextPacketSize(&packet))) break;   // pl. az eszközt kihúzták
            while (packet > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD fl = 0;
                if (FAILED(cap->GetBuffer(&data, &frames, &fl, nullptr, nullptr))) { packet = 0; break; }
                if ((fl & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
                    for (UINT32 left = frames; left > 0;) {
                        const UINT32 k = left < zeros.size() ? left : static_cast<UINT32>(zeros.size());
                        buffer_.push(zeros.data(), k);
                        left -= k;
                    }
                } else {
                    buffer_.push(reinterpret_cast<const float*>(data), frames);
                }
                cap->ReleaseBuffer(frames);
                if (FAILED(cap->GetNextPacketSize(&packet))) { packet = 0; break; }
            }
        }
        client->Stop();
        running_ = false;
    }

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex m_;
    std::condition_variable cv_;
    bool started_ = false;
    std::string startError_;
    CaptureBuffer buffer_;
};

} // namespace aqulify::room
