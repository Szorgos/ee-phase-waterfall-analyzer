#pragma once
/**
 * @file player_output_coreaudio_exclusive.hpp
 * @brief A beépített lejátszó kizárólagos (hog mode) Core Audio hangkimenete macOS-re.
 *
 * A lejátszó közvetlenül a jelenlegi alapértelmezett valódi kimeneti eszközt (nem a BlackHole-t és nem a
 * saját aggregált eszközt) foglalja le kizárólagos módban (`kAudioDevicePropertyHogMode`), a mintavételi
 * frekvenciát a fájl saját mintavételére állítja (`kAudioDevicePropertyNominalSampleRate`), és egy
 * `kAudioUnitSubType_HALOutput` egységen keresztül 32 bites lebegőpontos sztereó jelet küld a DAC-ra.
 * A dekódolt mintákon a render visszahívásban lefut a közös `ExclusiveDsp` (AetherChain + digitális hangerő).
 * Ha az eszköz nem támogatja a hog módot vagy a fájl mintavételi frekvenciáját, a megnyitás `false`-szal
 * és magyar nyelvű indoklással tér vissza, így a `SelectablePlayerOutput` automatikusan a megosztott
 * Core Audio kimenetre lép vissza. Bezáráskor az eredeti mintavétel visszaáll, és a hog mód felszabadul.
 *
 * Források (Apple Developer Documentation, ellenőrizve: 2026-11-15):
 *   - kAudioDevicePropertyHogMode:
 *     https://developer.apple.com/documentation/coreaudio/kaudiodevicepropertyhogmode
 *   - kAudioDevicePropertyNominalSampleRate:
 *     https://developer.apple.com/documentation/coreaudio/kaudiodevicepropertynominalsamplerate
 *   - kAudioDevicePropertyAvailableNominalSampleRates:
 *     https://developer.apple.com/documentation/coreaudio/kaudiodevicepropertyavailablenominalsamplerates
 *   - kAudioUnitSubType_HALOutput:
 *     https://developer.apple.com/documentation/audiotoolbox/kaudiounitsubtype_haloutput
 *   - kAudioOutputUnitProperty_CurrentDevice:
 *     https://developer.apple.com/documentation/audiotoolbox/kaudiooutputunitproperty_currentdevice
 *   - AudioObjectGetPropertyData / AudioObjectSetPropertyData:
 *     https://developer.apple.com/documentation/coreaudio/1422526-audioobjectgetpropertydata
 *     https://developer.apple.com/documentation/coreaudio/1422455-audioobjectsetpropertydata
 */
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "exclusive_dsp.hpp"
#include "file_player.hpp"
#include "watchdog.hpp"

namespace ee_analyzer::player {

class CoreAudioExclusiveOutput : public PlayerOutput {
public:
    CoreAudioExclusiveOutput() = default;
    ~CoreAudioExclusiveOutput() override { close(); }

    /** A közvetlen kimenetben futó DSP-lánc (nem birtokolt; a motor élettartama alatt él). */
    void setExclusiveDsp(ExclusiveDsp* dsp) { dsp_ = dsp; }

    std::vector<uint32_t> supportedRates() const override {
        static const uint32_t kCandidates[] = { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000 };
        const AudioObjectID dev = chooseRealOutputDevice();
        std::vector<uint32_t> res;
        if (dev != kAudioObjectUnknown) {
            for (uint32_t r : kCandidates) {
                if (supportsSampleRate(dev, static_cast<Float64>(r))) res.push_back(r);
            }
        }
        if (res.empty()) return { 44100, 48000, 88200, 96000, 176400, 192000 };
        return res;
    }
    uint32_t actualRate() const override {
        return open_.load() ? rate_ : 0;
    }

    bool open(uint32_t sampleRate, RenderFn render, std::string* error) override {
        close();
        if (sampleRate == 0 || !render) {
            if (error) *error = "érvénytelen mintavétel vagy hiányzó render függvény";
            return false;
        }

        const AudioObjectID dev = chooseRealOutputDevice();
        if (dev == kAudioObjectUnknown) {
            if (error) *error = "nincs használható valódi kimeneti hangeszköz";
            return false;
        }

        const std::string devName = deviceName(dev);
        devId_ = dev;
        rate_ = sampleRate;
        consumed_ = 0;
        active_ = true;

        auto fail = [&](const std::string& msg) {
            if (error) *error = msg;
            close();
            return false;
        };

        // 1. Ellenőrzés: támogatja-e az eszköz a fájl mintavételi frekvenciáját
        const Float64 wantRate = static_cast<Float64>(sampleRate);
        if (!supportsSampleRate(dev, wantRate)) {
            return fail("a hangkártya (" + (devName.empty() ? std::string("Core Audio") : devName) +
                        ") nem támogatja a(z) " + std::to_string(sampleRate) + " Hz-es mintavételt");
        }

        // 2. Kizárólagos (hog) mód igénylése a saját folyamatazonosítóval
        const auto ha = propAddr(dev, kAudioDevicePropertyHogMode);
        pid_t currentOwner = -1;
        UInt32 ownerSize = sizeof(currentOwner);
        if (AudioObjectGetPropertyData(dev, &ha, 0, nullptr, &ownerSize, &currentOwner) == noErr &&
            currentOwner != -1 && currentOwner != getpid()) {
            return fail("a hangkártyát már egy másik folyamat használja kizárólagos módban (PID: " +
                        std::to_string(currentOwner) + ")");
        }

        pid_t myPid = getpid();
        OSStatus st = AudioObjectSetPropertyData(dev, &ha, 0, nullptr, sizeof(myPid), &myPid);
        if (st != noErr) {
            return fail("a hangkártya nem engedélyezte a kizárólagos (hog) módot (Core Audio hiba: " +
                        std::to_string(st) + ")");
        }
        hogged_ = true;

        // 3. Eredeti mintavétel mentése és átállítás a fájl mintavételére
        Float64 curRate = 0.0;
        if (getProp(dev, kAudioDevicePropertyNominalSampleRate, curRate) && curRate > 0.0) {
            origRate_ = curRate;
        }
        if (std::abs(curRate - wantRate) > 0.5) {
            const auto ra = propAddr(dev, kAudioDevicePropertyNominalSampleRate);
            st = AudioObjectSetPropertyData(dev, &ra, 0, nullptr, sizeof(wantRate), &wantRate);
            if (st != noErr) {
                return fail("a mintavételi frekvencia nem állítható be (" + std::to_string(sampleRate) +
                            " Hz, hiba: " + std::to_string(st) + ")");
            }
            rateChanged_ = true;
            // Egyes USB DAC-ok aszinkron módon váltanak órajelet: legfeljebb 500 ms-ig várunk a visszaigazolásra
            Float64 nowRate = curRate;
            for (int i = 0; i < 50; ++i) {
                if (getProp(dev, kAudioDevicePropertyNominalSampleRate, nowRate) &&
                    std::abs(nowRate - wantRate) <= 1.0) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (std::abs(nowRate - wantRate) > 1.0) {
                return fail("a hangkártya nem állt át a kért mintavételre (" + std::to_string(sampleRate) + " Hz)");
            }
        }

        // 4. HALOutput AudioUnit létrehozása és az eszközre kötése
        {
            std::lock_guard<std::mutex> lk(renderMutex_);
            render_ = std::move(render);
        }

        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Output;
        desc.componentSubType = kAudioUnitSubType_HALOutput;
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;

        AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
        if (!comp) return fail("a Core Audio HALOutput komponens nem található");

        st = AudioComponentInstanceNew(comp, &unit_);
        if (st != noErr || !unit_) {
            unit_ = nullptr;
            return fail("az AudioUnit példányosítása sikertelen (hiba: " + std::to_string(st) + ")");
        }

        st = AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice,
                                  kAudioUnitScope_Global, 0, &devId_, sizeof(devId_));
        if (st != noErr) {
            return fail("a kimeneti eszköz nem rendelhető az AudioUnit-hoz (hiba: " + std::to_string(st) + ")");
        }

        // 32 bites lebegőpontos sztereó bemeneti formátum a HALOutput 0-s buszán
        AudioStreamBasicDescription fmt{};
        fmt.mSampleRate = wantRate;
        fmt.mFormatID = kAudioFormatLinearPCM;
        fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        fmt.mBytesPerPacket = 2 * sizeof(float);
        fmt.mFramesPerPacket = 1;
        fmt.mBytesPerFrame = 2 * sizeof(float);
        fmt.mChannelsPerFrame = 2;
        fmt.mBitsPerChannel = 32;

        st = AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));
        if (st != noErr) {
            return fail("a 32 bites float sztereó formátum nem állítható be (hiba: " + std::to_string(st) + ")");
        }

        AURenderCallbackStruct cbStruct{};
        cbStruct.inputProc = &onRender;
        cbStruct.inputProcRefCon = this;
        st = AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback,
                                  kAudioUnitScope_Input, 0, &cbStruct, sizeof(cbStruct));
        if (st != noErr) {
            return fail("a render visszahívás nem állítható be (hiba: " + std::to_string(st) + ")");
        }

        st = AudioUnitInitialize(unit_);
        if (st != noErr) {
            return fail("az AudioUnit inicializálása sikertelen (hiba: " + std::to_string(st) + ")");
        }

        // 5. A DSP-lánc felkészítése a valós idejű blokkméretre (ditherBits = 0: 32 bites float kimenet)
        UInt32 bufFrames = 0;
        getProp(dev, kAudioDevicePropertyBufferFrameSize, bufFrames);
        UInt32 maxSlice = 0;
        UInt32 sliceSize = sizeof(maxSlice);
        AudioUnitGetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice,
                             kAudioUnitScope_Global, 0, &maxSlice, &sliceSize);
        const uint32_t maxFrames = std::max<uint32_t>({ 512u, bufFrames, maxSlice });
        {
            std::lock_guard<std::mutex> lk(renderMutex_);
            interleaved_.assign(static_cast<size_t>(maxFrames) * 2, 0.0f);
        }
        if (dsp_) dsp_->prepare(sampleRate, maxFrames, 0);

        st = AudioOutputUnitStart(unit_);
        if (st != noErr) {
            return fail("az AudioUnit indítása sikertelen (hiba: " + std::to_string(st) + ")");
        }

        hb_.beat("coreaudio-hog: render");   // a címke itt; a valós idejű visszahívás csak időbélyeget ír
        open_ = true;
        std::cout << "[Lejátszó] Core Audio kizárólagos kimenet (hog mode): "
                  << (devName.empty() ? std::string("alapértelmezett") : devName)
                  << ", 32 bit float, " << sampleRate << " Hz, "
                  << (bufFrames ? bufFrames : maxFrames) << " keret.\n";
        return true;
    }

    void close() override {
        open_ = false;
        active_ = false;
        if (unit_) {
            AudioOutputUnitStop(unit_);
            AudioUnitUninitialize(unit_);
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
        }
        if (rateChanged_ && devId_ != kAudioObjectUnknown && origRate_ > 0.0) {
            const auto ra = propAddr(devId_, kAudioDevicePropertyNominalSampleRate);
            AudioObjectSetPropertyData(devId_, &ra, 0, nullptr, sizeof(origRate_), &origRate_);
        }
        rateChanged_ = false;
        origRate_ = 0.0;

        if (hogged_ && devId_ != kAudioObjectUnknown) {
            pid_t rel = -1;
            const auto ha = propAddr(devId_, kAudioDevicePropertyHogMode);
            AudioObjectSetPropertyData(devId_, &ha, 0, nullptr, sizeof(rel), &rel);
        }
        hogged_ = false;
        devId_ = kAudioObjectUnknown;

        std::lock_guard<std::mutex> lk(renderMutex_);
        render_ = nullptr;
    }

    void setActive(bool active) override { active_.store(active); }
    void lock() override { renderMutex_.lock(); }
    void unlock() override { renderMutex_.unlock(); }

    void flush() override {
        consumed_ = 0;
        if (unit_) {
            AudioUnitReset(unit_, kAudioUnitScope_Global, 0);
        }
    }

    uint64_t consumedFrames() const override { return consumed_.load(); }
    bool settlesOnRateChange() const override { return open_.load(); }

private:
    static AudioObjectPropertyAddress addr(AudioObjectPropertySelector sel,
                                           AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
        return AudioObjectPropertyAddress{ sel, scope, kAudioObjectPropertyElementMain };
    }

    /** Globális, vagy ha az eszköz csak a kimeneti scope-on hirdeti, kimeneti tulajdonságcím. */
    static AudioObjectPropertyAddress propAddr(AudioObjectID dev, AudioObjectPropertySelector sel) {
        AudioObjectPropertyAddress a = addr(sel, kAudioObjectPropertyScopeGlobal);
        if (!AudioObjectHasProperty(dev, &a)) {
            a.mScope = kAudioObjectPropertyScopeOutput;
        }
        return a;
    }

    template <typename T>
    static bool getProp(AudioObjectID obj, AudioObjectPropertySelector sel, T& out,
                        AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
        AudioObjectPropertyAddress a = addr(sel, scope);
        if (obj != kAudioObjectSystemObject && scope == kAudioObjectPropertyScopeGlobal &&
            !AudioObjectHasProperty(obj, &a)) {
            a.mScope = kAudioObjectPropertyScopeOutput;
        }
        UInt32 size = sizeof(T);
        return AudioObjectGetPropertyData(obj, &a, 0, nullptr, &size, &out) == noErr;
    }

    static std::string stringProp(AudioObjectID obj, AudioObjectPropertySelector sel) {
        const auto a = addr(sel);
        CFStringRef s = nullptr;
        UInt32 size = sizeof(s);
        if (AudioObjectGetPropertyData(obj, &a, 0, nullptr, &size, &s) != noErr || !s) return {};
        char buf[512];
        std::string out;
        if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) out = buf;
        CFRelease(s);
        return out;
    }

    static std::string deviceUid(AudioObjectID dev) { return stringProp(dev, kAudioDevicePropertyDeviceUID); }
    static std::string deviceName(AudioObjectID dev) { return stringProp(dev, kAudioObjectPropertyName); }

    static UInt32 outputChannelCount(AudioObjectID dev) {
        const auto a = addr(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput);
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) != noErr || size == 0) return 0;
        std::vector<uint8_t> buf(size);
        auto* list = reinterpret_cast<AudioBufferList*>(buf.data());
        if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, list) != noErr) return 0;
        UInt32 n = 0;
        for (UInt32 i = 0; i < list->mNumberBuffers; ++i) n += list->mBuffers[i].mNumberChannels;
        return n;
    }

    /** A `mac_engine.mm` `isBlackHole` és `isUsableOutput` logikája (104–113. sor): valódi kimenet, nem BlackHole és nem aggregált. */
    static bool isBlackHole(AudioObjectID dev) {
        return deviceUid(dev).find("BlackHole") != std::string::npos ||
               deviceName(dev).find("BlackHole") != std::string::npos;
    }

    static bool isUsableOutput(AudioObjectID dev) {
        UInt32 transport = 0, hidden = 0;
        getProp(dev, kAudioDevicePropertyTransportType, transport);
        getProp(dev, kAudioDevicePropertyIsHidden, hidden);
        return transport != kAudioDeviceTransportTypeAggregate && hidden == 0 &&
               outputChannelCount(dev) > 0 && !isBlackHole(dev);
    }

    static std::vector<AudioObjectID> allDevices() {
        const auto a = addr(kAudioHardwarePropertyDevices);
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr || size == 0) return {};
        std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, ids.data()) != noErr) return {};
        ids.resize(size / sizeof(AudioObjectID));
        return ids;
    }

    static AudioObjectID defaultOutputDevice() {
        AudioObjectID dev = kAudioObjectUnknown;
        getProp(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, dev);
        return dev;
    }

    /** A jelenlegi alapértelmezett valódi kimenet (ha az alapértelmezett épp BlackHole vagy aggregált, az első valódi kimenet). */
    static AudioObjectID chooseRealOutputDevice() {
        if (AudioObjectID d = defaultOutputDevice(); d != kAudioObjectUnknown && isUsableOutput(d)) return d;
        for (AudioObjectID d : allDevices()) {
            if (isUsableOutput(d)) return d;
        }
        return kAudioObjectUnknown;
    }

    static bool supportsSampleRate(AudioObjectID dev, Float64 targetRate) {
        const auto a = propAddr(dev, kAudioDevicePropertyAvailableNominalSampleRates);
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) != noErr || size < sizeof(AudioValueRange)) {
            return true;   // ha az eszköz nem hirdet tartománylistát, a beállítási kísérlet dönt
        }
        std::vector<AudioValueRange> ranges(size / sizeof(AudioValueRange));
        if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, ranges.data()) != noErr) return true;
        ranges.resize(size / sizeof(AudioValueRange));
        for (const auto& r : ranges) {
            if (targetRate >= r.mMinimum - 0.5 && targetRate <= r.mMaximum + 0.5) return true;
        }
        return false;
    }

    static OSStatus onRender(void* inRefCon, AudioUnitRenderActionFlags* /*ioActionFlags*/,
                             const AudioTimeStamp* /*inTimeStamp*/, UInt32 /*inBusNumber*/,
                             UInt32 inNumberFrames, AudioBufferList* ioData) {
        auto* self = static_cast<CoreAudioExclusiveOutput*>(inRefCon);
        self->hb_.beat();   // őrszem: csak atomikus időbélyeg
        if (!ioData || ioData->mNumberBuffers == 0) return noErr;

        float* outData = static_cast<float*>(ioData->mBuffers[0].mData);
        const UInt32 channels = ioData->mBuffers[0].mNumberChannels;

        if (channels == 2 && outData) {
            std::unique_lock<std::mutex> lk(self->renderMutex_, std::try_to_lock);
            if (lk.owns_lock() && self->active_.load() && self->render_) {
                self->render_(outData, inNumberFrames);
                if (self->dsp_) self->dsp_->process(outData, inNumberFrames);
                self->consumed_ += inNumberFrames;
            } else {
                std::memset(outData, 0, inNumberFrames * 2 * sizeof(float));
            }
        } else if (ioData->mNumberBuffers >= 2 && outData) {
            float* outR = static_cast<float*>(ioData->mBuffers[1].mData);
            std::unique_lock<std::mutex> lk(self->renderMutex_, std::try_to_lock);
            if (lk.owns_lock() && self->active_.load() && self->render_ && outR) {
                if (self->interleaved_.size() < static_cast<size_t>(inNumberFrames) * 2) {
                    self->interleaved_.resize(static_cast<size_t>(inNumberFrames) * 2);
                }
                float* buf = self->interleaved_.data();
                self->render_(buf, inNumberFrames);
                if (self->dsp_) self->dsp_->process(buf, inNumberFrames);
                for (UInt32 i = 0; i < inNumberFrames; ++i) {
                    outData[i] = buf[2 * i];
                    outR[i] = buf[2 * i + 1];
                }
                self->consumed_ += inNumberFrames;
            } else {
                std::memset(outData, 0, inNumberFrames * sizeof(float));
                if (outR) std::memset(outR, 0, inNumberFrames * sizeof(float));
            }
        } else {
            for (UInt32 b = 0; b < ioData->mNumberBuffers; ++b) {
                if (ioData->mBuffers[b].mData) {
                    std::memset(ioData->mBuffers[b].mData, 0, ioData->mBuffers[b].mDataByteSize);
                }
            }
        }
        return noErr;
    }

    AudioObjectID devId_ = kAudioObjectUnknown;
    AudioComponentInstance unit_ = nullptr;
    ExclusiveDsp* dsp_ = nullptr;
    Float64 origRate_ = 0.0;
    bool hogged_ = false;
    bool rateChanged_ = false;
    uint32_t rate_ = 44100;
    std::atomic<bool> open_{false};
    std::atomic<bool> active_{false};
    std::atomic<uint64_t> consumed_{0};
    std::mutex renderMutex_;
    RenderFn render_;
    std::vector<float> interleaved_;
    // Őrszem (beépített hibajelentés): a render-visszahívás a megnyitott kimenet futása alatt folyamatosan jön
    // (szünetben csendet ad), így csak a megnyitott kimenettől (open_) várunk jelzést
    aq::wd::Heartbeat hb_{ "player-coreaudio-hog", &open_ };
};

} // namespace ee_analyzer::player
