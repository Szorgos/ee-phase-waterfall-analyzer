#pragma once
/**
 * @file wasapi_loopback.hpp
 * @brief WASAPI loopback rögzítés: az alapértelmezett lejátszóeszköz kimenetét "hallgatja le".
 *
 * Saját szálon fut, automatikusan követi az alapértelmezett eszköz váltását,
 * és hiba (pl. eszköz kihúzása) esetén újraindul. Csendben is ír (nullákat),
 * hogy a vízesés szépen lecsengjen.
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
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tap_ring.hpp"

namespace ee_analyzer::win {

// GUID-ok közvetlenül definiálva, hogy MinGW és MSVC alatt is ugyanúgy forduljon
static const CLSID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID kIID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID kIID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID kIID_IAudioCaptureClient = {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
static const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const PROPERTYKEY kPKEY_Device_FriendlyName = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};
static const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

template <class T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** put() { reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

class WasapiLoopback {
public:
    using Ring = StereoTapRing<65536>;

    ~WasapiLoopback() { stop(); }

    void start() {
        if (running_.exchange(true)) return;
        thread_ = std::thread(&WasapiLoopback::threadMain, this);
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
    }

    const Ring& ring() const { return ring_; }
    uint32_t sampleRate() const { return sampleRate_.load(); }
    bool active() const { return active_.load(); }

    std::string deviceName() const {
        std::lock_guard<std::mutex> lk(nameMutex_);
        return deviceName_;
    }

private:
    void threadMain() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (running_) {
            if (!captureSession()) {
                active_ = false;
                // Hiba vagy eszközváltás: rövid szünet, majd újrapróbálkozás
                for (int i = 0; i < 10 && running_; ++i) Sleep(100);
            }
        }
        active_ = false;
        CoUninitialize();
    }

    static std::string wideToUtf8(const wchar_t* w) {
        if (!w) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1) return {};
        std::string s(static_cast<size_t>(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
        return s;
    }

    static std::wstring defaultDeviceId(IMMDeviceEnumerator* en) {
        ComPtr<IMMDevice> dev;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.put()))) return {};
        LPWSTR id = nullptr;
        std::wstring out;
        if (SUCCEEDED(dev->GetId(&id)) && id) {
            out = id;
            CoTaskMemFree(id);
        }
        return out;
    }

    /** Egy rögzítési munkamenet; false-szal tér vissza, ha újra kell indítani. */
    bool captureSession() {
        ComPtr<IMMDeviceEnumerator> en;
        if (FAILED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator,
                                    reinterpret_cast<void**>(en.put())))) {
            return false;
        }
        ComPtr<IMMDevice> dev;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.put()))) return false;

        std::wstring devId;
        {
            LPWSTR id = nullptr;
            if (SUCCEEDED(dev->GetId(&id)) && id) { devId = id; CoTaskMemFree(id); }
        }
        {
            ComPtr<IPropertyStore> props;
            std::string name = "Ismeretlen eszköz";
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.put()))) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(props->GetValue(kPKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
                    name = wideToUtf8(pv.pwszVal);
                }
                PropVariantClear(&pv);
            }
            std::lock_guard<std::mutex> lk(nameMutex_);
            if (deviceName_ != name) std::cout << "[Loopback] Lejátszóeszköz: " << name << "\n";
            deviceName_ = name;
        }

        ComPtr<IAudioClient> client;
        if (FAILED(dev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.put())))) {
            return false;
        }
        WAVEFORMATEX* wf = nullptr;
        if (FAILED(client->GetMixFormat(&wf)) || !wf) return false;

        const REFERENCE_TIME bufferDuration = 2000000; // 200 ms
        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, bufferDuration, 0, wf, nullptr);
        if (FAILED(hr)) {
            CoTaskMemFree(wf);
            return false;
        }

        enum class Fmt { F32, I16, I24, I32, Unknown } fmt = Fmt::Unknown;
        const WORD channels = wf->nChannels;
        const WORD bits = wf->wBitsPerSample;
        const DWORD rate = wf->nSamplesPerSec;
        bool isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        bool isPcm = wf->wFormatTag == WAVE_FORMAT_PCM;
        if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
            isFloat = std::memcmp(&ext->SubFormat, &kSubtypeFloat, sizeof(GUID)) == 0;
            isPcm = std::memcmp(&ext->SubFormat, &kSubtypePcm, sizeof(GUID)) == 0;
        }
        if (isFloat && bits == 32) fmt = Fmt::F32;
        else if (isPcm && bits == 16) fmt = Fmt::I16;
        else if (isPcm && bits == 24) fmt = Fmt::I24;
        else if (isPcm && bits == 32) fmt = Fmt::I32;
        const UINT32 blockAlign = wf->nBlockAlign;
        CoTaskMemFree(wf);
        if (fmt == Fmt::Unknown || channels == 0) {
            std::cerr << "[Loopback] Nem támogatott keverési formátum.\n";
            return false;
        }

        ComPtr<IAudioCaptureClient> capture;
        if (FAILED(client->GetService(kIID_IAudioCaptureClient, reinterpret_cast<void**>(capture.put())))) return false;
        if (FAILED(client->Start())) return false;

        sampleRate_ = rate;
        active_ = true;

        std::vector<float> lr;
        auto lastData = std::chrono::steady_clock::now();
        auto lastDeviceCheck = lastData;

        while (running_) {
            Sleep(10);

            UINT32 packet = 0;
            hr = capture->GetNextPacketSize(&packet);
            if (FAILED(hr)) break; // pl. AUDCLNT_E_DEVICE_INVALIDATED

            bool gotData = false;
            while (packet > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;

                lr.resize(static_cast<size_t>(frames) * 2);
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
                    std::fill(lr.begin(), lr.end(), 0.0f);
                } else {
                    for (UINT32 i = 0; i < frames; ++i) {
                        const BYTE* frame = data + static_cast<size_t>(i) * blockAlign;
                        const float l = sampleAt(frame, 0, fmt);
                        const float r = channels > 1 ? sampleAt(frame, 1, fmt) : l;
                        lr[2 * i] = l;
                        lr[2 * i + 1] = r;
                    }
                }
                ring_.writeInterleaved(lr.data(), frames);
                capture->ReleaseBuffer(frames);
                gotData = true;

                hr = capture->GetNextPacketSize(&packet);
                if (FAILED(hr)) break;
            }
            if (FAILED(hr)) break;

            const auto now = std::chrono::steady_clock::now();
            if (gotData) {
                lastData = now;
            } else if (now - lastData > std::chrono::milliseconds(50)) {
                // Nincs lejátszás: a WASAPI nem ad csomagot -> csendet írunk, hogy a kijelző lecsengjen
                const auto gap = std::chrono::duration_cast<std::chrono::microseconds>(now - lastData).count();
                const uint32_t frames = static_cast<uint32_t>(std::min<long long>(gap * rate / 1000000LL, 8192));
                if (frames > 0) {
                    lr.assign(static_cast<size_t>(frames) * 2, 0.0f);
                    ring_.writeInterleaved(lr.data(), frames);
                    lastData = now;
                }
            }

            if (now - lastDeviceCheck > std::chrono::seconds(1)) {
                lastDeviceCheck = now;
                if (defaultDeviceId(en.p) != devId) {
                    std::cout << "[Loopback] Az alapértelmezett lejátszóeszköz megváltozott, újracsatlakozás...\n";
                    break;
                }
            }
        }

        client->Stop();
        active_ = false;
        return false;
    }

    template <class F>
    static float sampleAt(const BYTE* frame, int ch, F fmt) {
        using E = decltype(fmt);
        switch (fmt) {
            case E::F32: { float v; std::memcpy(&v, frame + ch * 4, 4); return v; }
            case E::I16: { int16_t v; std::memcpy(&v, frame + ch * 2, 2); return v / 32768.0f; }
            case E::I24: {
                const BYTE* p = frame + ch * 3;
                int32_t v = (static_cast<int32_t>(p[0]) << 8) | (static_cast<int32_t>(p[1]) << 16) | (static_cast<int32_t>(p[2]) << 24);
                return static_cast<float>(v / 2147483648.0);
            }
            case E::I32: { int32_t v; std::memcpy(&v, frame + ch * 4, 4); return static_cast<float>(v / 2147483648.0); }
            default: return 0.0f;
        }
    }

    Ring ring_;
    std::atomic<bool> running_{false};
    std::atomic<bool> active_{false};
    std::atomic<uint32_t> sampleRate_{48000};
    mutable std::mutex nameMutex_;
    std::string deviceName_;
    std::thread thread_;
};

} // namespace ee_analyzer::win
