/**
 * @file mac_engine.mm
 * @brief Aqulify motor macOS-re: Core Audio Tap vagy BlackHole forrás, privát aggregált eszköz, AetherChain.
 *
 * Minden vezérlő művelet (indítás, leállítás, újraépítés, eszközváltás) a motor saját soros dispatch
 * sorában fut; a Core Audio értesítései is ide érkeznek. A valós idejű IOProc nem foglal memóriát és nem zárol.
 */
#import <Foundation/Foundation.h>
#import <CoreAudio/CoreAudio.h>
#if __has_include(<CoreAudio/AudioHardwareTapping.h>)
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#define AQ_HAVE_TAP_SDK 1
#else
#define AQ_HAVE_TAP_SDK 0
#endif

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>

#include "mac_engine.hpp"

namespace ee_analyzer::mac {

namespace {

constexpr UInt32 kMaxFrames = 16384;   // ennél nagyobb IO puffer esetén darabokban dolgozunk
const AudioObjectPropertySelector kSystemSelectors[] = { kAudioHardwarePropertyDefaultOutputDevice, kAudioHardwarePropertyDevices };

// ---------------------------------------------------------------- Core Audio segédek

std::string toStd(CFStringRef s) {
    if (!s) return {};
    char buf[512];
    if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) return buf;
    return {};
}

AudioObjectPropertyAddress addr(AudioObjectPropertySelector sel, AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return AudioObjectPropertyAddress{ sel, scope, kAudioObjectPropertyElementMain };
}

std::vector<AudioObjectID> allDevices() {
    const auto a = addr(kAudioHardwarePropertyDevices);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr) return {};
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, ids.data()) != noErr) return {};
    ids.resize(size / sizeof(AudioObjectID));
    return ids;
}

std::string stringProp(AudioObjectID obj, AudioObjectPropertySelector sel) {
    const auto a = addr(sel);
    CFStringRef s = nullptr;
    UInt32 size = sizeof(s);
    if (AudioObjectGetPropertyData(obj, &a, 0, nullptr, &size, &s) != noErr || !s) return {};
    std::string out = toStd(s);
    CFRelease(s);
    return out;
}

std::string deviceUid(AudioObjectID dev) { return stringProp(dev, kAudioDevicePropertyDeviceUID); }
std::string deviceName(AudioObjectID dev) { return stringProp(dev, kAudioObjectPropertyName); }

UInt32 channelCount(AudioObjectID dev, AudioObjectPropertyScope scope) {
    const auto a = addr(kAudioDevicePropertyStreamConfiguration, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) != noErr || size == 0) return 0;
    std::vector<uint8_t> buf(size);
    auto* list = reinterpret_cast<AudioBufferList*>(buf.data());
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, list) != noErr) return 0;
    UInt32 n = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) n += list->mBuffers[i].mNumberChannels;
    return n;
}

template <typename T>
bool getProp(AudioObjectID obj, AudioObjectPropertySelector sel, T& out, AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    const auto a = addr(sel, scope);
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(obj, &a, 0, nullptr, &size, &out) == noErr;
}

AudioObjectID defaultOutputDevice() {
    AudioObjectID dev = kAudioObjectUnknown;
    getProp(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, dev);
    return dev;
}

bool setDefaultOutputDevice(AudioObjectID dev) {
    const auto a = addr(kAudioHardwarePropertyDefaultOutputDevice);
    return AudioObjectSetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, sizeof(dev), &dev) == noErr;
}

bool isBlackHole(AudioObjectID dev) {
    return deviceUid(dev).find("BlackHole") != std::string::npos || deviceName(dev).find("BlackHole") != std::string::npos;
}

bool isUsableOutput(AudioObjectID dev) {
    UInt32 transport = 0, hidden = 0;
    getProp(dev, kAudioDevicePropertyTransportType, transport);
    getProp(dev, kAudioDevicePropertyIsHidden, hidden);
    return transport != kAudioDeviceTransportTypeAggregate && hidden == 0 &&
           channelCount(dev, kAudioObjectPropertyScopeOutput) > 0 && !isBlackHole(dev);
}

AudioObjectID findBlackHole() {
    // A 2 csatornás BlackHole az elsődleges (a rendszer kimenete erre áll); más változat is jó
    AudioObjectID any = kAudioObjectUnknown;
    for (AudioObjectID d : allDevices()) {
        if (!isBlackHole(d) || channelCount(d, kAudioObjectPropertyScopeInput) < 2) continue;
        if (deviceName(d).find("2ch") != std::string::npos) return d;
        if (any == kAudioObjectUnknown) any = d;
    }
    return any;
}

AudioObjectID deviceForUid(const std::string& uid) {
    if (uid.empty()) return kAudioObjectUnknown;
    for (AudioObjectID d : allDevices())
        if (deviceUid(d) == uid) return d;
    return kAudioObjectUnknown;
}

/** A kimeneti eszköz sztereó csatornái (0-tól számozva; Audio MIDI Setup → Hangszórók beállítása). */
void preferredStereo(AudioObjectID dev, UInt32& left, UInt32& right) {
    UInt32 ch[2] = { 1, 2 };
    const auto a = addr(kAudioDevicePropertyPreferredChannelsForStereo, kAudioObjectPropertyScopeOutput);
    UInt32 size = sizeof(ch);
    AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, ch);
    left = ch[0] > 0 ? ch[0] - 1 : 0;
    right = ch[1] > 0 ? ch[1] - 1 : 1;
}

// ---------------------------------------------------------------- rendszerhang-rögzítési engedély (TCC)
// A Core Audio Tap a "Rendszerhang-felvétel" engedélyt kéri. Az állapot lekérdezésére nincs nyilvános
// függvény: a rendszer TCC keretrendszerét (ugyanazt, amit a többi hangrögzítő is használ) futás közben
// töltjük be; ha nem érhető el, a rendszer az első olvasáskor maga kérdez.

enum class Tcc { Authorized, Denied, Unknown };

void* tccHandle() {
    static void* h = dlopen("/System/Library/PrivateFrameworks/TCC.framework/Versions/A/TCC", RTLD_NOW);
    return h;
}

Tcc tccPreflight() {
    void* h = tccHandle();
    if (!h) return Tcc::Unknown;
    using Fn = long (*)(CFStringRef, CFDictionaryRef);
    auto f = reinterpret_cast<Fn>(dlsym(h, "TCCAccessPreflight"));
    if (!f) return Tcc::Unknown;
    const long r = f(CFSTR("kTCCServiceAudioCapture"), nullptr);
    return r == 0 ? Tcc::Authorized : r == 1 ? Tcc::Denied : Tcc::Unknown;
}

bool tccRequest(void (^done)(bool granted)) {
    void* h = tccHandle();
    if (!h) return false;
    using Fn = void (*)(CFStringRef, CFDictionaryRef, void (^)(BOOL));
    auto f = reinterpret_cast<Fn>(dlsym(h, "TCCAccessRequest"));
    if (!f) return false;
    f(CFSTR("kTCCServiceAudioCapture"), nullptr, ^(BOOL granted) { done(granted); });
    return true;
}

bool tapApiAvailable() {
#if AQ_HAVE_TAP_SDK
    if (@available(macOS 14.2, *)) return true;
#endif
    return false;
}

} // namespace

// ======================================================================== Impl

struct Engine::Impl {
    Engine* owner = nullptr;
    dispatch_queue_t queue = nullptr;

    AudioObjectID tapId = kAudioObjectUnknown;
    AudioObjectID aggId = kAudioObjectUnknown;
    AudioDeviceIOProcID procId = nullptr;
    AudioObjectID outputDev = kAudioObjectUnknown;
    AudioObjectID sourceDev = kAudioObjectUnknown;   // BlackHole módban
    AudioObjectID restoreDefault = kAudioObjectUnknown;
    bool weSetDefault = false;

    // vezérlés (csak a sorban)
    std::string selectedUid;
    bool tapDenied = false;
    bool waitingForPermission = false;
    bool started = false;

    // a felületnek (bármely szálról)
    mutable std::mutex infoMutex;
    std::string activeUid, activeName, sourceName, selectedUidCopy;

    // valós idejű állapot (csak az IOProc)
    UInt32 inputSkip = 0, outLeft = 0, outRight = 1;
    uint32_t appliedSeq = 0;
    std::vector<float> L, R;

    AudioObjectPropertyListenerBlock systemListener = nil;
    std::string lastSignature;
    AudioObjectPropertyListenerBlock rateListener = nil;

    void setInfo(const std::string& uid, const std::string& name, const std::string& src) {
        std::lock_guard<std::mutex> lk(infoMutex);
        activeUid = uid;
        activeName = name;
        sourceName = src;
        selectedUidCopy = selectedUid;
    }

    // ------------------------------------------------------------ IOProc

    static OSStatus ioProc(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* in, const AudioTimeStamp*,
                           AudioBufferList* out, const AudioTimeStamp*, void* ctx) {
        static_cast<Impl*>(ctx)->process(in, out);
        return noErr;
    }

    void process(const AudioBufferList* in, AudioBufferList* out) {
        if (!out || out->mNumberBuffers == 0) return;
        const UInt32 ch0 = std::max<UInt32>(1, out->mBuffers[0].mNumberChannels);
        UInt32 frames = out->mBuffers[0].mDataByteSize / (sizeof(float) * ch0);
        for (UInt32 b = 0; b < out->mNumberBuffers; ++b) std::memset(out->mBuffers[b].mData, 0, out->mBuffers[b].mDataByteSize);
        if (frames == 0) return;

        // új beállítások a felületről (nem blokkoló seqlock)
        const uint32_t seq = owner->params.currentSeq();
        if (seq != appliedSeq) {
            ParamData pd;
            uint32_t readSeq = 0;
            if (owner->params.tryRead(pd, readSeq)) {
                owner->chain.applyParamsRT(pd);
                appliedSeq = readSeq;
            }
        }

        for (UInt32 off = 0; off < frames; off += kMaxFrames) {
            const UInt32 n = std::min(kMaxFrames, frames - off);
            readSource(in, off, n);
            owner->chain.process(L.data(), R.data(), n);
            owner->visualizerTap.write(L.data(), R.data(), n);
            writeOutput(out, off, n);
        }
    }

    /** A forrás két csatornája (az aggregált eszköz bemenetei közül a kimeneti eszközé után). */
    void readSource(const AudioBufferList* in, UInt32 off, UInt32 n) {
        float* l = L.data();
        float* r = R.data();
        bool haveL = false, haveR = false;
        if (in) {
            UInt32 base = 0;
            for (UInt32 b = 0; b < in->mNumberBuffers && !(haveL && haveR); ++b) {
                const AudioBuffer& buf = in->mBuffers[b];
                const UInt32 nch = std::max<UInt32>(1, buf.mNumberChannels);
                const float* data = static_cast<const float*>(buf.mData);
                const UInt32 avail = buf.mDataByteSize / (sizeof(float) * nch);
                for (UInt32 c = 0; c < nch; ++c) {
                    const UInt32 g = base + c;
                    float* dst = g == inputSkip ? l : g == inputSkip + 1 ? r : nullptr;
                    if (!dst) continue;
                    for (UInt32 i = 0; i < n; ++i) dst[i] = (data && off + i < avail) ? data[(off + i) * nch + c] : 0.0f;
                    (dst == l ? haveL : haveR) = true;
                }
                base += nch;
            }
        }
        if (!haveL) std::memset(l, 0, n * sizeof(float));
        if (!haveR) std::memcpy(r, l, n * sizeof(float));   // monó forrás: mindkét csatornára
    }

    /** A feldolgozott jel a kimeneti eszköz sztereó csatornáira (a többi csatorna csend marad). */
    void writeOutput(AudioBufferList* out, UInt32 off, UInt32 n) {
        UInt32 base = 0;
        for (UInt32 b = 0; b < out->mNumberBuffers; ++b) {
            AudioBuffer& buf = out->mBuffers[b];
            const UInt32 nch = std::max<UInt32>(1, buf.mNumberChannels);
            float* data = static_cast<float*>(buf.mData);
            const UInt32 avail = buf.mDataByteSize / (sizeof(float) * nch);
            for (UInt32 c = 0; c < nch && data; ++c) {
                const UInt32 g = base + c;
                const float* src = g == outLeft ? L.data() : g == outRight ? R.data() : nullptr;
                if (!src) continue;
                if (outLeft == outRight) {   // monó kimenet
                    for (UInt32 i = 0; i < n && off + i < avail; ++i) data[(off + i) * nch + c] = 0.5f * (L[i] + R[i]);
                } else {
                    for (UInt32 i = 0; i < n && off + i < avail; ++i) data[(off + i) * nch + c] = src[i];
                }
            }
            base += nch;
        }
    }

    // ------------------------------------------------------------ felépítés (a sorban)

    void teardown() {
        if (aggId != kAudioObjectUnknown && procId) {
            AudioDeviceStop(aggId, procId);
            AudioDeviceDestroyIOProcID(aggId, procId);
        }
        procId = nullptr;
        if (aggId != kAudioObjectUnknown && rateListener) {
            const auto a = addr(kAudioDevicePropertyNominalSampleRate);
            AudioObjectRemovePropertyListenerBlock(aggId, &a, queue, rateListener);
        }
        rateListener = nil;
        if (aggId != kAudioObjectUnknown) AudioHardwareDestroyAggregateDevice(aggId);
        aggId = kAudioObjectUnknown;
#if AQ_HAVE_TAP_SDK
        if (tapId != kAudioObjectUnknown) {
            if (@available(macOS 14.2, *)) AudioHardwareDestroyProcessTap(tapId);
        }
#endif
        tapId = kAudioObjectUnknown;
        owner->processing_ = false;
    }

    /** A kimenet: a választott eszköz, különben a rendszer alapértelmezettje, különben az első valódi kimenet. */
    AudioObjectID chooseOutput() const {
        if (AudioObjectID d = deviceForUid(selectedUid); d != kAudioObjectUnknown && isUsableOutput(d)) return d;
        if (restoreDefault != kAudioObjectUnknown && isUsableOutput(restoreDefault) && owner->source_ == Source::BlackHole)
            return restoreDefault;
        if (AudioObjectID d = defaultOutputDevice(); d != kAudioObjectUnknown && isUsableOutput(d)) return d;
        for (AudioObjectID d : allDevices())
            if (isUsableOutput(d)) return d;
        return kAudioObjectUnknown;
    }

    bool createTap(std::string& tapUid) {
#if AQ_HAVE_TAP_SDK
        if (@available(macOS 14.2, *)) {
            AudioObjectID own = kAudioObjectUnknown;
            pid_t pid = getpid();
            const auto pa = addr(kAudioHardwarePropertyTranslatePIDToProcessObject);
            UInt32 psize = sizeof(own);
            AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, sizeof(pid), &pid, &psize, &own);
            NSArray<NSNumber*>* exclude = own != kAudioObjectUnknown ? @[ @(own) ] : @[];
            // Az SDK a CATapDescription osztályt nem jelöli 14.2-esnek: a közvetlen hivatkozás erős kötést adna, és a
            // program macOS 14.2 előtt el sem indulna. Ezért név szerint, futásidőben kérjük el.
            Class tapClass = NSClassFromString(@"CATapDescription");
            if (!tapClass) return false;
            // Minden alkalmazás hangja, a saját kimenetünk nélkül (különben visszacsatolás lenne)
            CATapDescription* desc = [[tapClass alloc] initStereoGlobalTapButExcludeProcesses:exclude];
            desc.name = @"Aqulify";
            desc.privateTap = YES;
            desc.muteBehavior = CATapMutedWhenTapped;   // az eredeti hang néma, amíg a motor olvassa
            AudioObjectID tap = kAudioObjectUnknown;
            const OSStatus st = AudioHardwareCreateProcessTap(desc, &tap);
            if (st != noErr || tap == kAudioObjectUnknown) {
                std::cerr << "[Core Audio] A rendszerhang-tap nem hozható létre (" << st << ").\n";
                return false;
            }
            tapId = tap;
            tapUid = desc.UUID.UUIDString.UTF8String;
            return true;
        }
#endif
        (void)tapUid;
        return false;
    }

    bool createAggregate(AudioObjectID output, const std::string& tapUid, AudioObjectID blackHole) {
        NSString* outUid = [NSString stringWithUTF8String:deviceUid(output).c_str()];
        NSMutableArray* subs = [NSMutableArray arrayWithObject:@{ @kAudioSubDeviceUIDKey: outUid }];
        if (blackHole != kAudioObjectUnknown) {
            NSString* bhUid = [NSString stringWithUTF8String:deviceUid(blackHole).c_str()];
            [subs addObject:@{ @kAudioSubDeviceUIDKey: bhUid, @kAudioSubDeviceDriftCompensationKey: @YES }];
        }
        NSMutableDictionary* desc = [@{
            @kAudioAggregateDeviceNameKey: @"Aqulify",
            @kAudioAggregateDeviceUIDKey: [NSString stringWithFormat:@"io.github.szorgos.aqulify.aggregate.%@", NSUUID.UUID.UUIDString],
            @kAudioAggregateDeviceMainSubDeviceKey: outUid,
            @kAudioAggregateDeviceIsPrivateKey: @YES,
            @kAudioAggregateDeviceIsStackedKey: @NO,
            @kAudioAggregateDeviceSubDeviceListKey: subs,
        } mutableCopy];
#if AQ_HAVE_TAP_SDK
        if (!tapUid.empty()) {
            if (@available(macOS 14.2, *)) {
                desc[@kAudioAggregateDeviceTapListKey] =
                    @[ @{ @kAudioSubTapUIDKey: [NSString stringWithUTF8String:tapUid.c_str()], @kAudioSubTapDriftCompensationKey: @YES } ];
                desc[@kAudioAggregateDeviceTapAutoStartKey] = @YES;
            }
        }
#endif
        AudioObjectID agg = kAudioObjectUnknown;
        const OSStatus st = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)desc, &agg);
        if (st != noErr || agg == kAudioObjectUnknown) {
            std::cerr << "[Core Audio] Az aggregált eszköz nem hozható létre (" << st << ").\n";
            return false;
        }
        aggId = agg;
        return true;
    }

    void fail(Problem p) {
        teardown();
        owner->problem_ = p;
        owner->source_ = Source::None;
        setInfo("", "", "");
    }

    /** A forrás és a kimenet kiválasztása, az aggregált eszköz felépítése és az IO indítása. */
    void rebuild() {
        teardown();
        if (!started) return;

        const bool tapOk = tapApiAvailable() && !tapDenied;
        const AudioObjectID blackHole = tapOk ? kAudioObjectUnknown : findBlackHole();
        if (!tapOk && blackHole == kAudioObjectUnknown) {
            restoreSystemDefault();
            fail(tapApiAvailable() ? Problem::Permission : Problem::NoSource);
            std::cerr << (tapApiAvailable() ? "[Core Audio] Nincs engedély a rendszerhang rögzítésére, és nincs BlackHole.\n"
                                            : "[Core Audio] Ehhez a macOS-hez (14.2 előtt) a BlackHole 2ch kell.\n");
            return;
        }
        owner->source_ = tapOk ? Source::ProcessTap : Source::BlackHole;
        if (tapOk) restoreSystemDefault();

        const AudioObjectID output = chooseOutput();
        if (output == kAudioObjectUnknown) { fail(Problem::Device); return; }

        // BlackHole módban a rendszer kimenete a BlackHole (az alkalmazások oda szólnak), a motor a valódi eszközre
        if (!tapOk) {
            const AudioObjectID cur = defaultOutputDevice();
            if (cur != blackHole) {
                if (!weSetDefault) restoreDefault = cur != kAudioObjectUnknown && !isBlackHole(cur) ? cur : output;
                weSetDefault = setDefaultOutputDevice(blackHole) || weSetDefault;
            }
        }

        std::string tapUid;
        if (tapOk && !createTap(tapUid)) { fail(Problem::Device); return; }
        if (!createAggregate(output, tapUid, tapOk ? kAudioObjectUnknown : blackHole)) { fail(Problem::Device); return; }

        outputDev = output;
        sourceDev = blackHole;
        inputSkip = channelCount(output, kAudioObjectPropertyScopeInput);   // a kimeneti eszköz saját bemenetei elöl
        preferredStereo(output, outLeft, outRight);
        if (channelCount(output, kAudioObjectPropertyScopeOutput) < 2) outLeft = outRight = 0;

        Float64 sr = 48000.0;
        getProp(aggId, kAudioDevicePropertyNominalSampleRate, sr);
        if (!(sr >= 8000.0)) sr = 48000.0;
        owner->sampleRate_ = static_cast<uint32_t>(std::lround(sr));
        owner->chain.setSampleRate(sr);   // az IO még nem fut

        // Mintavétel-váltás (pl. Audio MIDI Setupban): újraindítás az új értékkel
        Impl* impl = this;
        // (a figyelőt a saját értesítéséből nem távolítjuk el: az újraépítés a sorban utána fut)
        rateListener = ^(UInt32, const AudioObjectPropertyAddress*) {
            dispatch_async(impl->queue, ^{
                Float64 now = 0.0;
                if (impl->aggId != kAudioObjectUnknown && getProp(impl->aggId, kAudioDevicePropertyNominalSampleRate, now) &&
                    now >= 8000.0 && static_cast<uint32_t>(std::lround(now)) != impl->owner->sampleRate_.load())
                    impl->rebuild();
            });
        };
        const auto ra = addr(kAudioDevicePropertyNominalSampleRate);
        AudioObjectAddPropertyListenerBlock(aggId, &ra, queue, rateListener);

        if (AudioDeviceCreateIOProcID(aggId, &Impl::ioProc, this, &procId) != noErr || !procId) { fail(Problem::Device); return; }
        disableUnusedInputs(output);
        if (AudioDeviceStart(aggId, procId) != noErr) { fail(Problem::Device); return; }

        owner->problem_ = Problem::None;
        owner->processing_ = true;
        const std::string src = tapOk ? "Core Audio Tap" : deviceName(blackHole);
        setInfo(deviceUid(output), deviceName(output), src);
        std::cout << "[Core Audio] Forrás: " << src << ", kimenet: " << deviceName(output) << ", " << sr << " Hz\n";
    }

    /** A kimeneti eszköz saját bemeneteit (pl. egy hangkártya mikrofonbemenetét) nem olvassuk. */
    void disableUnusedInputs(AudioObjectID output) {
        const auto sa = addr(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput);
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(output, &sa, 0, nullptr, &size) != noErr) return;
        const UInt32 ownStreams = size / sizeof(AudioStreamID);
        if (ownStreams == 0) return;
        const auto ua = addr(kAudioDevicePropertyIOProcStreamUsage, kAudioObjectPropertyScopeInput);
        if (AudioObjectGetPropertyDataSize(aggId, &ua, 0, nullptr, &size) != noErr || size < sizeof(AudioHardwareIOProcStreamUsage)) return;
        std::vector<uint8_t> buf(size);
        auto* usage = reinterpret_cast<AudioHardwareIOProcStreamUsage*>(buf.data());
        usage->mIOProc = reinterpret_cast<void*>(procId);
        if (AudioObjectGetPropertyData(aggId, &ua, 0, nullptr, &size, usage) != noErr) return;
        for (UInt32 i = 0; i < usage->mNumberStreams && i < ownStreams; ++i) usage->mStreamIsOn[i] = 0;
        AudioObjectSetPropertyData(aggId, &ua, 0, nullptr, size, usage);
    }

    /** A rendszer eszközei és alapértelmezett kimenete (a saját aggregált eszközünk nélkül) egy szövegben. */
    static std::string systemSignature() {
        std::string sig = deviceUid(defaultOutputDevice()) + "|";
        for (AudioObjectID d : allDevices()) {
            const std::string uid = deviceUid(d);
            if (uid.rfind("io.github.szorgos.aqulify.", 0) == 0) continue;
            sig += uid + ";";
        }
        return sig;
    }

    void restoreSystemDefault() {
        if (weSetDefault && restoreDefault != kAudioObjectUnknown && isBlackHole(defaultOutputDevice()))
            setDefaultOutputDevice(restoreDefault);
        weSetDefault = false;
    }

    /** Indítás: Tap esetén először az engedély (ha még nincs döntés, a rendszer megkérdezi). */
    void startOnQueue() {
        started = true;
        if (tapApiAvailable()) {
            const Tcc t = tccPreflight();
            if (t == Tcc::Denied) {
                tapDenied = true;
            } else if (t == Tcc::Unknown && !waitingForPermission) {
                waitingForPermission = true;
                owner->problem_ = Problem::Permission;
                dispatch_queue_t q = queue;
                Impl* impl = this;
                const bool asked = tccRequest(^(bool granted) {
                    dispatch_async(q, ^{
                        impl->waitingForPermission = false;
                        impl->tapDenied = !granted;
                        impl->rebuild();
                    });
                });
                if (asked) return;
                waitingForPermission = false;   // nincs TCC: a rendszer az első olvasáskor kérdez
            }
        }
        rebuild();
    }

    /** Rendszerszintű értesítések: alapértelmezett kimenet és eszközlista változása. */
    void listenSystem() {
        Impl* impl = this;
        lastSignature = systemSignature();
        systemListener = ^(UInt32, const AudioObjectPropertyAddress*) {
            if (!impl->started) return;
            // A saját aggregált eszközünk létrehozása és törlése is értesítést küld: azzal nem foglalkozunk
            const std::string sig = systemSignature();
            if (sig == impl->lastSignature) return;
            impl->lastSignature = sig;

            const AudioObjectID def = defaultOutputDevice();
            const bool realDefault = def != kAudioObjectUnknown && !isBlackHole(def) && isUsableOutput(def);
            const bool lost = impl->outputDev != kAudioObjectUnknown && deviceUid(impl->outputDev).empty();
            const bool recover = !impl->owner->processing_.load() && !impl->waitingForPermission;
            bool need = lost || recover;
            if (impl->owner->source_ == Source::BlackHole) {
                // A felhasználó másik kimenetet választott a rendszerben: a motor ott szóljon, a rendszer
                // kimenete pedig visszaáll a BlackHole-ra (különben a hang feldolgozás nélkül szólna)
                if (realDefault) {
                    impl->restoreDefault = def;
                    impl->weSetDefault = false;
                    need = true;
                }
                if (impl->sourceDev != kAudioObjectUnknown && deviceUid(impl->sourceDev).empty()) need = true;
            } else if (impl->selectedUid.empty() && realDefault && def != impl->outputDev) {
                need = true;   // a rendszer alapértelmezett kimenetét követjük
            }
            if (need) impl->rebuild();
        };
        for (AudioObjectPropertySelector sel : kSystemSelectors) {
            const auto a = addr(sel);
            AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &a, queue, systemListener);
        }
    }

    void unlistenSystem() {
        if (!systemListener) return;
        for (AudioObjectPropertySelector sel : kSystemSelectors) {
            const auto a = addr(sel);
            AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &a, queue, systemListener);
        }
        systemListener = nil;
    }
};

// ======================================================================== Engine

Engine::Engine() : p_(std::make_unique<Impl>()) {
    p_->owner = this;
    p_->queue = dispatch_queue_create("io.github.szorgos.aqulify.engine", DISPATCH_QUEUE_SERIAL);
    p_->L.assign(kMaxFrames, 0.0f);
    p_->R.assign(kMaxFrames, 0.0f);
    chain.setSampleRate(48000);
    chain.startWorker();
}

Engine::~Engine() {
    stop();
    chain.stopWorker();
}

void Engine::start() {
    Impl* p = p_.get();
    dispatch_async(p->queue, ^{
        if (p->started) return;
        p->listenSystem();
        p->startOnQueue();
    });
}

void Engine::retry() {
    Impl* p = p_.get();
    dispatch_async(p->queue, ^{
        if (!p->started || processing_.load() || p->waitingForPermission) return;
        if (tapApiAvailable() && p->tapDenied && tccPreflight() == Tcc::Authorized) p->tapDenied = false;
        p->rebuild();
    });
}

void Engine::stop() {
    Impl* p = p_.get();
    dispatch_sync(p->queue, ^{
        p->started = false;
        p->unlistenSystem();
        p->teardown();
        p->restoreSystemDefault();
    });
}

std::vector<OutputDevice> Engine::outputDevices() const {
    std::vector<OutputDevice> v;
    const AudioObjectID def = defaultOutputDevice();
    for (AudioObjectID d : allDevices()) {
        if (!isUsableOutput(d)) continue;
        v.push_back({ deviceUid(d), deviceName(d), d == def });
    }
    return v;
}

std::string Engine::selectedOutputUid() const {
    std::lock_guard<std::mutex> lk(p_->infoMutex);
    return p_->selectedUidCopy;
}

std::string Engine::activeOutputUid() const {
    std::lock_guard<std::mutex> lk(p_->infoMutex);
    return p_->activeUid;
}

std::string Engine::activeOutputName() const {
    std::lock_guard<std::mutex> lk(p_->infoMutex);
    return p_->activeName;
}

std::string Engine::sourceName() const {
    std::lock_guard<std::mutex> lk(p_->infoMutex);
    return p_->sourceName;
}

void Engine::setOutput(const std::string& uid) {
    Impl* p = p_.get();
    const std::string u = uid;
    dispatch_async(p->queue, ^{
        p->selectedUid = u;
        {
            std::lock_guard<std::mutex> lk(p->infoMutex);
            p->selectedUidCopy = u;
        }
        p->rebuild();
    });
}

} // namespace ee_analyzer::mac
