#pragma once
/**
 * @file mic_capture_coreaudio.hpp
 * @brief Mikrofonfelvétel Core Audióval (macOS) – a `mic_capture.hpp` felületének megvalósítása.
 *
 * AudioQueue-bemenet a kiválasztott eszközről (azonosító: az eszköz UID-ja), monó, 32 bites lebegőpontos, a kért
 * mintavételen (az átalakítást a rendszer végzi). Az első felvételkor a macOS engedélyt kér (az Info.plist
 * NSMicrophoneUsageDescription szövegével); ha a felhasználó nem engedi, a felvétel csendes marad – ezt a hívó a
 * szintből látja, és a varázsló elmagyarázza (Rendszerbeállítások → Adatvédelem → Mikrofon).
 *
 * Forrás: Apple Developer Documentation – Audio Queue Services (AudioQueueNewInput, kAudioQueueProperty_CurrentDevice),
 * Core Audio AudioObject API (kAudioHardwarePropertyDevices, kAudioDevicePropertyStreamConfiguration,
 * kAudioDevicePropertyDeviceUID, kAudioObjectPropertyName, kAudioHardwarePropertyDefaultInputDevice).
 */
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "mic_capture.hpp"

namespace aqulify::room {

namespace ca_detail {
inline std::string cfToUtf8(CFStringRef s) {
    if (!s) return {};
    char buf[512];
    if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) return buf;
    return {};
}

inline std::string deviceString(AudioObjectID dev, AudioObjectPropertySelector sel) {
    AudioObjectPropertyAddress a{ sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef s = nullptr;
    UInt32 sz = sizeof(s);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, &s) != noErr || !s) return {};
    std::string out = cfToUtf8(s);
    CFRelease(s);
    return out;
}

/** Van-e az eszköznek bemeneti csatornája. */
inline bool hasInput(AudioObjectID dev) {
    AudioObjectPropertyAddress a{ kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeInput,
                                  kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &sz) != noErr || sz == 0) return false;
    std::vector<uint8_t> buf(sz);
    auto* list = reinterpret_cast<AudioBufferList*>(buf.data());
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, list) != noErr) return false;
    UInt32 ch = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) ch += list->mBuffers[i].mNumberChannels;
    return ch > 0;
}
} // namespace ca_detail

class CoreAudioMicCapture final : public MicCapture {
public:
    ~CoreAudioMicCapture() override { stop(); }

    std::vector<InputDevice> listInputs(bool /*includeMonitors: macOS-en a kimenet nem vehető fel közvetlenül*/) override {
        using namespace ca_detail;
        std::vector<InputDevice> out;
        AudioObjectPropertyAddress a{ kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        UInt32 sz = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &sz) != noErr) return out;
        std::vector<AudioObjectID> ids(sz / sizeof(AudioObjectID));
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, ids.data()) != noErr) return out;
        AudioObjectID def = 0;
        {
            AudioObjectPropertyAddress d{ kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain };
            UInt32 dsz = sizeof(def);
            AudioObjectGetPropertyData(kAudioObjectSystemObject, &d, 0, nullptr, &dsz, &def);
        }
        for (AudioObjectID id : ids) {
            if (!hasInput(id)) continue;
            InputDevice dev;
            dev.id = deviceString(id, kAudioDevicePropertyDeviceUID);
            dev.name = deviceString(id, kAudioObjectPropertyName);
            if (dev.id.empty()) continue;
            if (dev.name.empty()) dev.name = dev.id;
            dev.isDefault = id == def;
            out.push_back(std::move(dev));
        }
        return out;
    }

    bool start(const std::string& deviceId, uint32_t sampleRate, double maxSeconds, std::string& error) override {
        stop();
        if (sampleRate < 8000 || sampleRate > 384000 || !(maxSeconds > 0.0) || maxSeconds > 600.0) {
            error = "format";
            return false;
        }
        if (!deviceId.empty()) {
            bool found = false;
            for (const auto& d : listInputs(false)) found |= d.id == deviceId;
            if (!found) { error = "no_device"; return false; }
        }
        buffer_.reset(static_cast<size_t>(maxSeconds * sampleRate) + 1);

        AudioStreamBasicDescription f{};
        f.mSampleRate = sampleRate;
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        f.mBitsPerChannel = 32;
        f.mChannelsPerFrame = 1;
        f.mBytesPerFrame = 4;
        f.mFramesPerPacket = 1;
        f.mBytesPerPacket = 4;
        // A visszahívás a queue saját szálán fut (nincs futási hurok a hívó szálon)
        if (AudioQueueNewInput(&f, onInput, this, nullptr, nullptr, 0, &queue_) != noErr || !queue_) {
            error = "backend";
            queue_ = nullptr;
            return false;
        }
        if (!deviceId.empty()) {
            CFStringRef uid = CFStringCreateWithCString(nullptr, deviceId.c_str(), kCFStringEncodingUTF8);
            const OSStatus st = AudioQueueSetProperty(queue_, kAudioQueueProperty_CurrentDevice, &uid, sizeof(uid));
            CFRelease(uid);
            if (st != noErr) { error = "no_device"; disposeQueue(); return false; }
        }
        const UInt32 bufBytes = static_cast<UInt32>(sampleRate / 50) * 4;   // 20 ms-os pufferek
        for (int i = 0; i < 4; ++i) {
            AudioQueueBufferRef b = nullptr;
            if (AudioQueueAllocateBuffer(queue_, bufBytes, &b) != noErr || !b) { error = "backend"; disposeQueue(); return false; }
            AudioQueueEnqueueBuffer(queue_, b, 0, nullptr);
        }
        running_ = true;
        const OSStatus st = AudioQueueStart(queue_, nullptr);
        if (st != noErr) {
            running_ = false;
            error = st == kAudioQueueErr_InvalidDevice ? "no_device" : "permission";
            disposeQueue();
            return false;
        }
        return true;
    }

    std::vector<float> stop() override {
        if (!queue_) return buffer_.take();
        running_ = false;
        AudioQueueStop(queue_, true);
        disposeQueue();
        return buffer_.take();
    }

    bool running() const override { return running_; }
    float takePeak() override { return buffer_.takePeak(); }
    size_t recordedFrames() const override { return buffer_.written(); }

private:
    void disposeQueue() {
        if (queue_) AudioQueueDispose(queue_, true);
        queue_ = nullptr;
    }

    static void onInput(void* user, AudioQueueRef q, AudioQueueBufferRef b, const AudioTimeStamp*, UInt32 packets,
                        const AudioStreamPacketDescription*) {
        auto* self = static_cast<CoreAudioMicCapture*>(user);
        if (packets > 0 && b->mAudioData) self->buffer_.push(static_cast<const float*>(b->mAudioData), packets);
        if (self->running_) AudioQueueEnqueueBuffer(q, b, 0, nullptr);
    }

    AudioQueueRef queue_ = nullptr;
    std::atomic<bool> running_{false};
    CaptureBuffer buffer_;
};

} // namespace aqulify::room
