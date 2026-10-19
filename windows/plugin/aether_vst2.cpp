/**
 * @file aether_vst2.cpp
 * @brief AetherAudio DSP plugin Equalizer APO-hoz (VST 2.4 bináris interfész).
 *
 * Mit csinál:
 *   - lefuttatja a projekt DSP láncát (Preamp -> EQ -> Crossfeed -> Convolver -> Limiter) az audiodg.exe-ben,
 *   - a feldolgozott sztereó jelet közös memórián át átadja az AetherAudio.exe motornak (3D vízesés, fázis),
 *   - a GUI-ban állított beállításokat élőben átveszi a motortól (zármentes seqlock),
 *   - az utolsó beállítást a C:\ProgramData\AetherAudio\params.bin fájlból tölti be, így motor nélkül is
 *     a legutóbb beállított lánccal szól.
 *
 * Alapállapotban (nincs mentett beállítás) bit-pontosan átlátszó: csak "lehallgat".
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xmmintrin.h>
#include <pmmintrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "vst2_abi.hpp"
#include "param_apply.hpp"
#include "../common/win_shared.hpp"

using namespace ee_analyzer;

namespace {

constexpr int32_t kUniqueId = vst2::fourCC('A', 'e', 't', 'h');
constexpr int32_t kVersion = 1000; // 1.0.0

enum ParamIndex : int32_t {
    kParamBypass = 0,  // 1 = a DSP lánc kikerülve (a jel továbbra is megy az analizátorba)
    kParamRemote = 1,  // 1 = a GUI (AetherAudio.exe) vezérli a láncot
    kNumParams
};

void copyString(void* dst, const char* src, size_t maxLen) {
    if (!dst) return;
    std::strncpy(static_cast<char*>(dst), src, maxLen - 1);
    static_cast<char*>(dst)[maxLen - 1] = '\0';
}

/** Egyszerű diagnosztikai napló: C:\ProgramData\AetherAudio\plugin_status.txt (nem valós idejű szálból!). */
void logStatus(const std::string& line) {
    OutputDebugStringA(("[AetherAudio] " + line + "\n").c_str());
    std::wstring path = win::paramFilePath(false);
    path = path.substr(0, path.find_last_of(L'\\')) + L"\\plugin_status.txt";
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char buf[640];
    const int n = std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u  [pid %lu] %s\r\n", t.wYear, t.wMonth,
                                t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId(), line.c_str());
    DWORD written = 0;
    if (n > 0) WriteFile(f, buf, static_cast<DWORD>(std::min<int>(n, sizeof(buf) - 1)), &written, nullptr);
    CloseHandle(f);
}

std::string processName() {
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, n);
    const size_t s = p.find_last_of("\\/");
    return s == std::string::npos ? p : p.substr(s + 1);
}

class AetherPlugin {
public:
    explicit AetherPlugin(vst2::HostCallback host) : host_(host) {
        std::memset(&effect_, 0, sizeof(effect_));
        effect_.magic = vst2::kEffectMagic;
        effect_.dispatcher = &AetherPlugin::dispatcherProc;
        effect_.process = &AetherPlugin::processAccumulating;
        effect_.setParameter = &AetherPlugin::setParameterProc;
        effect_.getParameter = &AetherPlugin::getParameterProc;
        effect_.processReplacing = &AetherPlugin::processReplacingProc;
        effect_.processDoubleReplacing = nullptr;
        effect_.numPrograms = 1;
        effect_.numParams = kNumParams;
        effect_.numInputs = 2;
        effect_.numOutputs = 2;
        effect_.flags = vst2::effFlagsCanReplacing;
        effect_.ioRatio = 1.0f;
        effect_.object = this;
        effect_.uniqueID = kUniqueId;
        effect_.version = kVersion;

        token_ = static_cast<uint32_t>(GetCurrentProcessId()) * 2654435761u ^
                 static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this) >> 4);
        if (token_ == 0) token_ = 1;
    }

    vst2::AEffect* effect() { return &effect_; }

private:
    // ---------------------------------------------------------------- életciklus
    void open() {
        preparePipeline(pipeline_);
        current_ = transparentParams();

        ParamData stored;
        const bool haveFile = win::loadParamFile(stored) && stored.has(kFlagInitialized);
        if (haveFile) current_ = stored;

        bool createdNew = false;
        if (mapping_.create(createdNew)) {
            AetherSharedState* st = mapping_.state();
            // Ha a blokkot egy másik példány vagy a motor már feltöltötte, az a frissebb
            ParamData shared;
            uint32_t seq = 0;
            if (!createdNew && st->params.currentSeq() != 0 && st->params.tryRead(shared, seq) &&
                shared.has(kFlagInitialized)) {
                current_ = shared;
                appliedSeq_ = seq;
            } else if (current_.has(kFlagInitialized)) {
                st->params.write(current_);
                appliedSeq_ = st->params.currentSeq();
            }
            st->activeInstances.fetch_add(1);
            logStatus(std::string("plugin megnyitva, folyamat: ") + processName() + ", közös memória: " +
                      (mapping_.isGlobal() ? "Global" : "Local") + (createdNew ? " (új)" : " (meglévő)") +
                      ", mentett beállítás: " + (haveFile ? "igen" : "nem"));
        } else {
            logStatus(std::string("plugin megnyitva, folyamat: ") + processName() +
                      ", HIBA: közös memória nem hozható létre (GetLastError=" + std::to_string(GetLastError()) + ")");
        }

        setPipelineSampleRate(pipeline_, sampleRate_);
        applyParams(pipeline_, current_);
    }

    void close() {
        if (mapping_.state()) {
            AetherSharedState* st = mapping_.state();
            if (st->activeInstances.load() > 0) st->activeInstances.fetch_sub(1);
            uint32_t mine = token_;
            st->ownerToken.compare_exchange_strong(mine, 0u);
        }
        mapping_.close();
    }

    void setSampleRate(float sr) {
        if (sr <= 0.0f) return;
        sampleRate_ = sr;
        setPipelineSampleRate(pipeline_, sr);
        applyParams(pipeline_, current_);
    }

    // ---------------------------------------------------------------- hangfeldolgozás
    void processReplacing(float** inputs, float** outputs, int32_t n) {
        if (n <= 0 || !outputs || !outputs[0] || !outputs[1]) return;

        // Denormális számok kiiktatása (IIR lecsengésnél CPU-csúcsot okoznának)
        const unsigned int oldCsr = _mm_getcsr();
        _mm_setcsr(oldCsr | 0x8040); // FTZ | DAZ

        float* outL = outputs[0];
        float* outR = outputs[1];
        const float* inL = (inputs && inputs[0]) ? inputs[0] : nullptr;
        const float* inR = (inputs && inputs[1]) ? inputs[1] : inL;
        if (inL && inL != outL) std::memmove(outL, inL, sizeof(float) * static_cast<size_t>(n));
        else if (!inL) std::memset(outL, 0, sizeof(float) * static_cast<size_t>(n));
        if (inR && inR != outR) std::memmove(outR, inR, sizeof(float) * static_cast<size_t>(n));
        else if (!inR) std::memset(outR, 0, sizeof(float) * static_cast<size_t>(n));

        AetherSharedState* st = mapping_.state();

        // GUI-ból érkező új beállítások (nem blokkoló)
        if (st && remote_.load(std::memory_order_relaxed)) {
            const uint32_t seq = st->params.currentSeq();
            if (seq != appliedSeq_ && (seq & 1u) == 0) {
                ParamData pd;
                uint32_t readSeq = 0;
                if (st->params.tryRead(pd, readSeq)) {
                    current_ = pd;
                    applyParams(pipeline_, current_);
                    appliedSeq_ = readSeq;
                }
            }
        }

        if (!bypass_.load(std::memory_order_relaxed)) {
            pipeline_.processBlock(outL, outR, static_cast<size_t>(n));
        }

        // Lehallgatás az analizátornak: egyszerre csak egy példány ír (a többi hallgat)
        if (st) {
            const uint64_t now = GetTickCount64();
            uint32_t owner = st->ownerToken.load(std::memory_order_relaxed);
            bool mine = owner == token_;
            if (!mine && (owner == 0 || now - st->ownerTickMs.load(std::memory_order_relaxed) > 500)) {
                mine = st->ownerToken.compare_exchange_strong(owner, token_);
            }
            if (mine) {
                st->tap.write(outL, outR, static_cast<uint32_t>(n));
                st->sampleRate.store(static_cast<uint32_t>(sampleRate_), std::memory_order_relaxed);
                st->gainReductionDb.store(pipeline_.limiter.enabled
                                              ? static_cast<float>(pipeline_.limiter.getGainReductionDb())
                                              : 0.0f,
                                          std::memory_order_relaxed);
                st->ownerTickMs.store(now, std::memory_order_release);
            }
        }

        _mm_setcsr(oldCsr);
    }

    // ---------------------------------------------------------------- paraméterek
    void setParameter(int32_t index, float value) {
        const bool on = value >= 0.5f;
        if (index == kParamBypass) bypass_ = on;
        else if (index == kParamRemote) remote_ = on;
    }

    float getParameter(int32_t index) const {
        if (index == kParamBypass) return bypass_ ? 1.0f : 0.0f;
        if (index == kParamRemote) return remote_ ? 1.0f : 0.0f;
        return 0.0f;
    }

    intptr_t dispatcher(int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt) {
        switch (opcode) {
            case vst2::effOpen: open(); return 0;
            case vst2::effClose: close(); delete this; return 0;
            case vst2::effSetSampleRate: setSampleRate(opt); return 0;
            case vst2::effSetBlockSize: return 0;
            case vst2::effMainsChanged:
                if (value) pipeline_.reset();
                return 0;
            case vst2::effGetParamName:
                copyString(ptr, index == kParamBypass ? "Bypass" : index == kParamRemote ? "Remote" : "", 16);
                return 0;
            case vst2::effGetParamLabel:
                copyString(ptr, "", vst2::kVstMaxParamStrLen);
                return 0;
            case vst2::effGetParamDisplay:
                copyString(ptr, getParameter(index) >= 0.5f ? "BE" : "KI", vst2::kVstMaxParamStrLen);
                return 0;
            case vst2::effCanBeAutomated: return index >= 0 && index < kNumParams ? 1 : 0;
            case vst2::effGetProgram: return 0;
            case vst2::effSetProgram: return 0;
            case vst2::effGetProgramName:
            case vst2::effGetProgramNameIndexed:
                copyString(ptr, "Alap", vst2::kVstMaxProgNameLen);
                return opcode == vst2::effGetProgramNameIndexed ? 1 : 0;
            case vst2::effGetPlugCategory: return vst2::kPlugCategEffect;
            case vst2::effGetEffectName:
                copyString(ptr, "AetherAudio", vst2::kVstMaxEffectNameLen);
                return 1;
            case vst2::effGetVendorString:
                copyString(ptr, "AetherAudio", vst2::kVstMaxVendorStrLen);
                return 1;
            case vst2::effGetProductString:
                copyString(ptr, "AetherAudio DSP + Analyzer Tap", vst2::kVstMaxProductStrLen);
                return 1;
            case vst2::effGetVendorVersion: return kVersion;
            case vst2::effGetVstVersion: return vst2::kVstVersion;
            case vst2::effSetBypass: bypass_ = value != 0; return 1;
            case vst2::effCanDo: {
                const char* s = static_cast<const char*>(ptr);
                if (!s) return 0;
                if (std::strcmp(s, "bypass") == 0) return 1;
                if (std::strcmp(s, "plugAsChannelInsert") == 0) return 1;
                if (std::strcmp(s, "receiveVstEvents") == 0 || std::strcmp(s, "receiveVstMidiEvent") == 0) return -1;
                return 0;
            }
            case vst2::effGetTailSize: return 1; // 1 = nincs lecsengés-farok jelentve
            case vst2::effStartProcess:
            case vst2::effStopProcess:
                return 0;
            default:
                return 0;
        }
    }

    // ---------------------------------------------------------------- C belépési pontok
    static AetherPlugin* self(vst2::AEffect* e) { return e ? static_cast<AetherPlugin*>(e->object) : nullptr; }

    static intptr_t dispatcherProc(vst2::AEffect* e, int32_t op, int32_t idx, intptr_t val, void* ptr, float opt) {
        AetherPlugin* p = self(e);
        return p ? p->dispatcher(op, idx, val, ptr, opt) : 0;
    }
    static void processReplacingProc(vst2::AEffect* e, float** in, float** out, int32_t n) {
        if (AetherPlugin* p = self(e)) p->processReplacing(in, out, n);
    }
    static void processAccumulating(vst2::AEffect* e, float** in, float** out, int32_t n) {
        // Elavult VST1 hívás: feldolgozás ideiglenes pufferbe, majd hozzáadás
        AetherPlugin* p = self(e);
        if (!p || n <= 0 || !out || !out[0] || !out[1]) return;
        if (p->scratchL_.size() < static_cast<size_t>(n)) return; // nem foglalunk valós időben
        float* tmp[2] = { p->scratchL_.data(), p->scratchR_.data() };
        p->processReplacing(in, tmp, n);
        for (int32_t i = 0; i < n; ++i) { out[0][i] += tmp[0][i]; out[1][i] += tmp[1][i]; }
    }
    static void setParameterProc(vst2::AEffect* e, int32_t idx, float v) {
        if (AetherPlugin* p = self(e)) p->setParameter(idx, v);
    }
    static float getParameterProc(vst2::AEffect* e, int32_t idx) {
        AetherPlugin* p = self(e);
        return p ? p->getParameter(idx) : 0.0f;
    }

public:
    void allocateScratch(size_t n) { scratchL_.assign(n, 0.0f); scratchR_.assign(n, 0.0f); }

private:
    vst2::AEffect effect_;
    [[maybe_unused]] vst2::HostCallback host_;
    DspPipeline pipeline_;
    ParamData current_{};
    uint32_t appliedSeq_ = 0;
    win::SharedMapping mapping_;
    uint32_t token_ = 1;
    float sampleRate_ = 48000.0f;
    std::atomic<bool> bypass_{false};
    std::atomic<bool> remote_{true};
    std::vector<float> scratchL_, scratchR_;
};

} // namespace

extern "C" __declspec(dllexport) vst2::AEffect* VSTPluginMain(vst2::HostCallback host) {
    if (!host) return nullptr;
    AetherPlugin* p = new (std::nothrow) AetherPlugin(host);
    if (!p) return nullptr;
    p->allocateScratch(8192);
    return p->effect();
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
