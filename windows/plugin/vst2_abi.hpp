#pragma once
/**
 * @file vst2_abi.hpp
 * @brief Minimális, saját (clean-room) VST 2.4 bináris interfész-leírás.
 *
 * Nem a Steinberg VST2 SDK része: csak azokat a struktúrákat és opkódokat írja le,
 * amelyek egy egyszerű effekt-plugin betöltéséhez kellenek (pl. Equalizer APO VSTPlugin szűrő).
 * Az Equalizer APO kizárólag VST 2.4 pluginokat tud betölteni, ezért ezt az interfészt használjuk.
 */
#include <cstdint>

namespace vst2 {

struct AEffect;

using HostCallback = intptr_t (*)(AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
using DispatcherProc = intptr_t (*)(AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
using ProcessProc = void (*)(AEffect* effect, float** inputs, float** outputs, int32_t sampleFrames);
using ProcessDoubleProc = void (*)(AEffect* effect, double** inputs, double** outputs, int32_t sampleFrames);
using SetParameterProc = void (*)(AEffect* effect, int32_t index, float parameter);
using GetParameterProc = float (*)(AEffect* effect, int32_t index);

constexpr int32_t fourCC(char a, char b, char c, char d) {
    return (static_cast<int32_t>(a) << 24) | (static_cast<int32_t>(b) << 16) |
           (static_cast<int32_t>(c) << 8) | static_cast<int32_t>(d);
}

constexpr int32_t kEffectMagic = fourCC('V', 's', 't', 'P');

struct AEffect {
    int32_t magic;
    DispatcherProc dispatcher;
    ProcessProc process;              // elavult (akkumuláló) feldolgozás
    SetParameterProc setParameter;
    GetParameterProc getParameter;
    int32_t numPrograms;
    int32_t numParams;
    int32_t numInputs;
    int32_t numOutputs;
    int32_t flags;
    intptr_t resvd1;
    intptr_t resvd2;
    int32_t initialDelay;
    int32_t realQualities;
    int32_t offQualities;
    float ioRatio;
    void* object;
    void* user;
    int32_t uniqueID;
    int32_t version;
    ProcessProc processReplacing;
    ProcessDoubleProc processDoubleReplacing;
    char future[56];
};

// AEffect::flags
enum : int32_t {
    effFlagsHasEditor = 1 << 0,
    effFlagsCanReplacing = 1 << 4,
    effFlagsProgramChunks = 1 << 5,
    effFlagsIsSynth = 1 << 8,
    effFlagsNoSoundInStop = 1 << 9,
    effFlagsCanDoubleReplacing = 1 << 12,
};

// Plugin felé küldött opkódok (dispatcher)
enum : int32_t {
    effOpen = 0,
    effClose = 1,
    effSetProgram = 2,
    effGetProgram = 3,
    effSetProgramName = 4,
    effGetProgramName = 5,
    effGetParamLabel = 6,
    effGetParamDisplay = 7,
    effGetParamName = 8,
    effSetSampleRate = 10,
    effSetBlockSize = 11,
    effMainsChanged = 12,
    effEditGetRect = 13,
    effEditOpen = 14,
    effEditClose = 15,
    effEditIdle = 19,
    effGetChunk = 23,
    effSetChunk = 24,
    effProcessEvents = 25,
    effCanBeAutomated = 26,
    effString2Parameter = 27,
    effGetProgramNameIndexed = 29,
    effGetInputProperties = 33,
    effGetOutputProperties = 34,
    effGetPlugCategory = 35,
    effSetSpeakerArrangement = 42,
    effSetBypass = 44,
    effGetEffectName = 45,
    effGetVendorString = 47,
    effGetProductString = 48,
    effGetVendorVersion = 49,
    effVendorSpecific = 50,
    effCanDo = 51,
    effGetTailSize = 52,
    effGetParameterProperties = 56,
    effGetVstVersion = 58,
    effStartProcess = 71,
    effStopProcess = 72,
    effSetProcessPrecision = 77,
};

// Host felé küldött opkódok (callback)
enum : int32_t {
    audioMasterAutomate = 0,
    audioMasterVersion = 1,
};

constexpr int32_t kPlugCategEffect = 1;
constexpr int32_t kVstVersion = 2400;

// Szöveghossz-korlátok
constexpr int kVstMaxParamStrLen = 8;
constexpr int kVstMaxProgNameLen = 24;
constexpr int kVstMaxEffectNameLen = 32;
constexpr int kVstMaxVendorStrLen = 64;
constexpr int kVstMaxProductStrLen = 64;

static_assert(sizeof(void*) != 8 || sizeof(AEffect) == 192, "AEffect elrendezés (64 bit) nem egyezik");

} // namespace vst2
