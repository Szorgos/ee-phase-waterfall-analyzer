#pragma once
/**
 * @file player_output_coreaudio.hpp
 * @brief A beépített lejátszó hangkimenete macOS-en: Core Audio Output AudioUnit (kAudioUnitSubType_DefaultOutput
 *        vagy kAudioUnitSubType_HALOutput), 32-bites lebegőpontos sztereó PCM folyam.
 *
 * A hang az alapértelmezett kimeneti eszközre (BlackHole módban maga a BlackHole, ahonnan az Aqulify motor olvas
 * és feldolgoz, így a hang a teljes DSP-láncon átmegy) vagy a megadott AudioObjectID eszközre megy.
 */
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "file_player.hpp"

namespace ee_analyzer::player {

class CoreAudioPlayerOutput : public PlayerOutput {
public:
    explicit CoreAudioPlayerOutput(AudioObjectID targetDev = kAudioObjectUnknown)
        : targetDev_(targetDev) {}

    ~CoreAudioPlayerOutput() override {
        close();
    }

    bool open(uint32_t sampleRate, RenderFn render, std::string* error) override {
        close();
        {
            std::lock_guard<std::mutex> lk(renderMutex_);
            render_ = std::move(render);
        }
        rate_ = sampleRate;
        active_ = false;
        consumed_ = 0;

        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Output;
        desc.componentSubType = (targetDev_ != kAudioObjectUnknown) ? kAudioUnitSubType_HALOutput : kAudioUnitSubType_DefaultOutput;
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;
        desc.componentFlags = 0;
        desc.componentFlagsMask = 0;

        AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
        if (!comp) {
            if (error) *error = "A Core Audio kimeneti komponens nem található.";
            return false;
        }

        OSStatus st = AudioComponentInstanceNew(comp, &unit_);
        if (st != noErr || !unit_) {
            if (error) *error = "Az AudioUnit kimeneti példány nem hozható létre (hiba: " + std::to_string(st) + ").";
            return false;
        }

        // Célzott eszköz beállítása HALOutput esetén
        if (targetDev_ != kAudioObjectUnknown) {
            st = AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice,
                                      kAudioUnitScope_Global, 0, &targetDev_, sizeof(targetDev_));
            if (st != noErr) {
                AudioComponentInstanceDispose(unit_);
                unit_ = nullptr;
                if (error) *error = "A célzott kimeneti eszköz nem állítható be az AudioUnit számára (hiba: " + std::to_string(st) + ").";
                return false;
            }
        }

        // Bemeneti formátum az Output AudioUnit 0-s bemeneti buszára (kAudioUnitScope_Input)
        // 32 bites natív float, 2 csatorna sztereó
        AudioStreamBasicDescription fmt{};
        fmt.mSampleRate = static_cast<Float64>(sampleRate);
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
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
            if (error) *error = "A kimeneti formátum nem állítható be az AudioUnit számára (hiba: " + std::to_string(st) + ").";
            return false;
        }

        // Render visszahívás regisztrálása
        AURenderCallbackStruct cbStruct{};
        cbStruct.inputProc = &onRender;
        cbStruct.inputProcRefCon = this;
        st = AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback,
                                  kAudioUnitScope_Input, 0, &cbStruct, sizeof(cbStruct));
        if (st != noErr) {
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
            if (error) *error = "A render visszahívás nem állítható be az AudioUnit számára (hiba: " + std::to_string(st) + ").";
            return false;
        }

        st = AudioUnitInitialize(unit_);
        if (st != noErr) {
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
            if (error) *error = "Az AudioUnit inicializálása sikertelen (hiba: " + std::to_string(st) + ").";
            return false;
        }

        st = AudioOutputUnitStart(unit_);
        if (st != noErr) {
            AudioUnitUninitialize(unit_);
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
            if (error) *error = "Az AudioUnit indítása sikertelen (hiba: " + std::to_string(st) + ").";
            return false;
        }

        return true;
    }

    void close() override {
        if (unit_) {
            AudioOutputUnitStop(unit_);
            AudioUnitUninitialize(unit_);
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
        }
        std::lock_guard<std::mutex> lk(renderMutex_);
        render_ = nullptr;
    }

    void setActive(bool active) override {
        active_.store(active);
    }

    void lock() override { renderMutex_.lock(); }
    void unlock() override { renderMutex_.unlock(); }

    void flush() override {
        consumed_ = 0;
        if (unit_) {
            AudioUnitReset(unit_, kAudioUnitScope_Global, 0);
        }
    }

    uint64_t consumedFrames() const override { return consumed_.load(); }

private:
    static OSStatus onRender(void* inRefCon, AudioUnitRenderActionFlags* /*ioActionFlags*/,
                             const AudioTimeStamp* /*inTimeStamp*/, UInt32 /*inBusNumber*/,
                             UInt32 inNumberFrames, AudioBufferList* ioData) {
        auto* self = static_cast<CoreAudioPlayerOutput*>(inRefCon);
        if (!ioData || ioData->mNumberBuffers == 0) return noErr;

        float* outData = static_cast<float*>(ioData->mBuffers[0].mData);
        const UInt32 channels = ioData->mBuffers[0].mNumberChannels;

        if (channels == 2 && outData) {
            // Normál eset: 1 sztereó interleaved puffer
            std::unique_lock<std::mutex> lk(self->renderMutex_, std::try_to_lock);
            if (lk.owns_lock() && self->active_.load() && self->render_) {
                self->render_(outData, inNumberFrames);
                self->consumed_ += inNumberFrames;
            } else {
                std::memset(outData, 0, inNumberFrames * 2 * sizeof(float));
            }
        } else if (ioData->mNumberBuffers >= 2 && outData) {
            // Non-interleaved eset: külön L és R puffer
            float* outR = static_cast<float*>(ioData->mBuffers[1].mData);
            std::unique_lock<std::mutex> lk(self->renderMutex_, std::try_to_lock);
            if (lk.owns_lock() && self->active_.load() && self->render_ && outR) {
                if (self->interleaved_.size() < inNumberFrames * 2) {
                    self->interleaved_.resize(inNumberFrames * 2);
                }
                float* buf = self->interleaved_.data();
                self->render_(buf, inNumberFrames);
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
            // Egyéb formátum esetén tiszta csend
            for (UInt32 b = 0; b < ioData->mNumberBuffers; ++b) {
                if (ioData->mBuffers[b].mData) {
                    std::memset(ioData->mBuffers[b].mData, 0, ioData->mBuffers[b].mDataByteSize);
                }
            }
        }
        return noErr;
    }

    AudioObjectID targetDev_ = kAudioObjectUnknown;
    AudioComponentInstance unit_ = nullptr;
    uint32_t rate_ = 44100;
    std::atomic<bool> active_{false};
    std::atomic<uint64_t> consumed_{0};
    std::mutex renderMutex_;
    RenderFn render_;
    std::vector<float> interleaved_;
};

} // namespace ee_analyzer::player
