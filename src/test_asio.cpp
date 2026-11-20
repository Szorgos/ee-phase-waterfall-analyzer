// Windows ASIO-kimenet motor tesztje (K3–K10 és 7.3 véletlen-mag: 40 eset).
// A hamis ASIO-meghajtó (FakeAsioDriver) a Steinberg ASIO SDK IASIO COM-felületét valósítja meg.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"
#include "file_player.hpp"
#include "watchdog.hpp"

#if __has_include("../windows/engine/player_output_asio.hpp")
#include "../windows/engine/player_output_asio.hpp"
#else
// Befagyasztott tesztcommit állapota a termékkód megvalósítása előtt:
// a tesztek valódi hívásokkal futnak, és implementáció hiányában elbuknak.
namespace ee_analyzer::player {
struct AsioDriverInfo {
    std::string keyName;
    std::string clsid;
    std::string description;
};
class AsioPlayerOutput : public PlayerOutput {
public:
    using DriverFactory = std::function<IASIO*(std::string* errorOut)>;
    explicit AsioPlayerOutput(std::string = {}, DriverFactory = {}) {}
    void setDriver(std::string, DriverFactory = {}) {}
    void invalidateRatesCache() const {}
    static std::vector<AsioDriverInfo> listDrivers(HKEY = HKEY_LOCAL_MACHINE, const wchar_t* = L"SOFTWARE\\ASIO") { return {}; }
    bool open(uint32_t, RenderFn, std::string* err = nullptr) override { if (err) *err = "not_implemented"; return false; }
    bool openDop(uint32_t, RenderDopFn, std::string* err = nullptr) override { if (err) *err = "not_implemented"; return false; }
    void close() override {}
    void setActive(bool) override {}
    void lock() override {}
    void unlock() override {}
    void flush() override {}
    uint64_t consumedFrames() const override { return 0; }
    bool settlesOnRateChange() const override { return false; }
    std::vector<uint32_t> supportedRates() const override { return {}; }
    uint32_t actualRate() const override { return 0; }
    bool isOpen() const { return false; }
    bool isRunning() const { return false; }
    bool isNativeDsdActive() const { return false; }
    bool isDopPcmActive() const { return false; }
    uint32_t resetCount() const { return 0; }
    std::string lastError() const { return "not_implemented"; }
    std::string lastDriverMessage() const { return {}; }
    long bufferSize() const { return 0; }
    ASIOSampleType sampleType() const { return 0; }
};
} // namespace ee_analyzer::player
#endif

using namespace ee_analyzer;
using namespace ee_analyzer::player;

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { ++g_fail; std::printf("  [HIBA] " __VA_ARGS__); } \
    else { std::printf("  [OK]   " __VA_ARGS__); } \
    std::printf("\n"); \
} while (0)

// Globális valós idejű foglalás-számláló a K9 ellenőrzéshez
thread_local volatile bool g_rtTrackAlloc = false;
std::atomic<uint64_t> g_rtAllocCount{0};

void* operator new(std::size_t size) {
    if (g_rtTrackAlloc) g_rtAllocCount.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

// ---------------------------------------------------------------------------
// Független referencia-átalakítók (a termékkódtól teljesen függetlenül számolva)
// ---------------------------------------------------------------------------

static uint8_t refReverseBits8(uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; ++i) {
        r = static_cast<uint8_t>((r << 1) | ((b >> i) & 1u));
    }
    return r;
}

static std::vector<uint8_t> refExpectedPcmBytes(const std::vector<float>& chSamples, ASIOSampleType type) {
    std::vector<uint8_t> out;
    for (float x : chSamples) {
        if (type == ASIOSTInt32LSB) {
            const double scale = 2147483648.0; // 2^31
            long long r = std::llround(static_cast<double>(x) * scale);
            if (r > 2147483647LL) r = 2147483647LL;
            if (r < -2147483648LL) r = -2147483648LL;
            const uint32_t u = static_cast<uint32_t>(static_cast<int32_t>(r));
            out.push_back(static_cast<uint8_t>(u & 0xFFu));
            out.push_back(static_cast<uint8_t>((u >> 8) & 0xFFu));
            out.push_back(static_cast<uint8_t>((u >> 16) & 0xFFu));
            out.push_back(static_cast<uint8_t>((u >> 24) & 0xFFu));
        } else if (type == ASIOSTInt24LSB) {
            const double scale = 8388608.0; // 2^23
            long long r = std::llround(static_cast<double>(x) * scale);
            if (r > 8388607LL) r = 8388607LL;
            if (r < -8388608LL) r = -8388608LL;
            const uint32_t u = static_cast<uint32_t>(static_cast<int32_t>(r));
            out.push_back(static_cast<uint8_t>(u & 0xFFu));
            out.push_back(static_cast<uint8_t>((u >> 8) & 0xFFu));
            out.push_back(static_cast<uint8_t>((u >> 16) & 0xFFu));
        } else if (type == ASIOSTInt16LSB) {
            const double scale = 32768.0; // 2^15
            long long r = std::llround(static_cast<double>(x) * scale);
            if (r > 32767LL) r = 32767LL;
            if (r < -32768LL) r = -32768LL;
            const uint16_t u = static_cast<uint16_t>(static_cast<int16_t>(r));
            out.push_back(static_cast<uint8_t>(u & 0xFFu));
            out.push_back(static_cast<uint8_t>((u >> 8) & 0xFFu));
        } else if (type == ASIOSTFloat32LSB) {
            uint8_t b[4];
            std::memcpy(b, &x, 4);
            out.insert(out.end(), b, b + 4);
        } else if (type == ASIOSTFloat64LSB) {
            const double d = static_cast<double>(x);
            uint8_t b[8];
            std::memcpy(b, &d, 8);
            out.insert(out.end(), b, b + 8);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Hamis ASIO-meghajtó (IASIO megvalósítás a Steinberg ASIO 2.3 specifikáció szerint)
// ---------------------------------------------------------------------------

struct FakeAsioState {
    static constexpr size_t kGuardBytes = 64;
    static constexpr uint8_t kGuardPattern = 0xA5;

    std::set<uint64_t> supportedRates{ 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000 };
    std::set<uint64_t> supportedDsdBitRates{ 2822400, 3072000, 5644800, 6144000, 11289600, 12288000, 22579200, 24576000, 45158400, 49152000 };
    ASIOSampleType pcmType = ASIOSTInt32LSB;
    ASIOSampleType dsdType = ASIOSTDSDInt8MSB1;
    bool allowNativeDsd = true;
    long numOutputChannels = 2;

    // PCM pufferméretek (mintakeretben)
    long pcmMinBufferSize = 64;
    long pcmMaxBufferSize = 4096;
    long preferredBufferSize = 128;
    long pcmGranularity = -1;

    // DSD pufferméretek (1-bites DSD-mintában: DSD64-nél pl. min 512, pref 4096, max 32768, gran 8)
    long dsdMinBufferSize = 512;
    long dsdMaxBufferSize = 32768;
    long dsdPreferredBufferSize = 4096;
    long dsdGranularity = 8;

    bool failInit = false;
    bool failCreateBuffers = false;
    bool failStart = false;
    std::atomic<bool> deviceLost{false};

    std::atomic<int> initCalls{0};
    std::atomic<int> startCalls{0};
    std::atomic<int> stopCalls{0};
    std::atomic<int> disposeCalls{0};
    std::atomic<int> canRateCalls{0};
    std::atomic<int> outputReadyCalls{0};
    std::atomic<double> currentRate{48000.0};
    std::atomic<long> currentIoFormat{kASIOPCMFormat};

    ASIOCallbacks callbacks{};
    bool hasCallbacks = false;
    long createdBufferSize = 0;
    size_t payloadSize = 0;
    std::vector<uint8_t> buffers[2][2]; // [channel][doubleBufferIdx], méret: payloadSize + kGuardBytes

    /**
     * A Steinberg ASIO 2.3 specifikáció (common/asio.h: "DSD operation and buffer layout", "8 samples per byte")
     * szerinti csatornánkénti pufferméret bájtban:
     * - DSD módban: bufferSize 1-bites DSD-minta -> bufferSize / 8 bájt;
     * - PCM módban: bufferSize keret * bájt/minta.
     */
    size_t computePayloadBytes(long bufferSize, ASIOSampleType t) const {
        const size_t sz = static_cast<size_t>(bufferSize > 0 ? bufferSize : 0);
        switch (t) {
            case ASIOSTInt16LSB: return sz * 2u;
            case ASIOSTInt24LSB: return sz * 3u;
            case ASIOSTInt32LSB:
            case ASIOSTInt32LSB24:
            case ASIOSTFloat32LSB: return sz * 4u;
            case ASIOSTFloat64LSB: return sz * 8u;
            case ASIOSTDSDInt8LSB1:
            case ASIOSTDSDInt8MSB1: return sz / 8u;
            default: return sz * 4u;
        }
    }

    std::vector<uint8_t> payload(int ch, int b) const {
        if (buffers[ch][b].size() < payloadSize) return {};
        return std::vector<uint8_t>(buffers[ch][b].begin(), buffers[ch][b].begin() + static_cast<std::ptrdiff_t>(payloadSize));
    }

    bool checkGuardBytes() const {
        for (int ch = 0; ch < 2; ++ch) {
            for (int b = 0; b < 2; ++b) {
                if (buffers[ch][b].size() < payloadSize + kGuardBytes) return false;
                for (size_t g = payloadSize; g < buffers[ch][b].size(); ++g) {
                    if (buffers[ch][b][g] != kGuardPattern) return false;
                }
            }
        }
        return true;
    }

    void triggerBufferSwitch(long idx) {
        if (!hasCallbacks) return;
        if (callbacks.bufferSwitch) {
            callbacks.bufferSwitch(idx, ASIOTrue);
        } else if (callbacks.bufferSwitchTimeInfo) {
            ASIOTime t{};
            callbacks.bufferSwitchTimeInfo(&t, idx, ASIOTrue);
        }
    }

    long triggerResetRequest() {
        if (!hasCallbacks || !callbacks.asioMessage) return 0;
        return callbacks.asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
    }

    long querySelectorSupported(long sel) {
        if (!hasCallbacks || !callbacks.asioMessage) return -1;
        return callbacks.asioMessage(kAsioSelectorSupported, sel, nullptr, nullptr);
    }

    long sendBufferSizeChange(long newSize) {
        if (!hasCallbacks || !callbacks.asioMessage) return -1;
        return callbacks.asioMessage(kAsioBufferSizeChange, newSize, nullptr, nullptr);
    }
};

class FakeAsioDriver : public IASIO {
public:
    explicit FakeAsioDriver(std::shared_ptr<FakeAsioState> st) : st_(std::move(st)) {}
    virtual ~FakeAsioDriver() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = static_cast<IASIO*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    ASIOBool init(void*) override {
        st_->initCalls.fetch_add(1);
        if (st_->failInit) return ASIOFalse;
        return ASIOTrue;
    }
    void getDriverName(char* name) override {
        if (name) std::strcpy(name, "AqulifyFakeASIO");
    }
    long getDriverVersion() override { return 2; }
    void getErrorMessage(char* string) override {
        if (string) std::strcpy(string, st_->failInit ? "Simulated init error" : "OK");
    }
    ASIOError start() override {
        st_->startCalls.fetch_add(1);
        if (st_->failStart) return ASE_HWMalfunction;
        return ASE_OK;
    }
    ASIOError stop() override {
        st_->stopCalls.fetch_add(1);
        return ASE_OK;
    }
    ASIOError getChannels(long* numIn, long* numOut) override {
        if (numIn) *numIn = 0;
        if (numOut) *numOut = st_->numOutputChannels;
        return ASE_OK;
    }
    ASIOError getLatencies(long* inLat, long* outLat) override {
        const long lat = (st_->currentIoFormat.load() == kASIODSDFormat) ? st_->dsdPreferredBufferSize : st_->preferredBufferSize;
        if (inLat) *inLat = lat;
        if (outLat) *outLat = lat;
        return ASE_OK;
    }
    ASIOError getBufferSize(long* minS, long* maxS, long* prefS, long* gran) override {
        if (st_->currentIoFormat.load() == kASIODSDFormat) {
            if (minS) *minS = st_->dsdMinBufferSize;
            if (maxS) *maxS = st_->dsdMaxBufferSize;
            if (prefS) *prefS = st_->dsdPreferredBufferSize;
            if (gran) *gran = st_->dsdGranularity;
        } else {
            if (minS) *minS = st_->pcmMinBufferSize;
            if (maxS) *maxS = st_->pcmMaxBufferSize;
            if (prefS) *prefS = st_->preferredBufferSize;
            if (gran) *gran = st_->pcmGranularity;
        }
        return ASE_OK;
    }
    ASIOError canSampleRate(ASIOSampleRate sRate) override {
        st_->canRateCalls.fetch_add(1);
        const uint64_t r = static_cast<uint64_t>(std::llround(sRate));
        if (st_->currentIoFormat.load() == kASIODSDFormat) {
            return st_->supportedDsdBitRates.count(r) ? ASE_OK : ASE_NoClock;
        }
        return st_->supportedRates.count(r) ? ASE_OK : ASE_NoClock;
    }
    ASIOError getSampleRate(ASIOSampleRate* currentRate) override {
        if (currentRate) *currentRate = st_->currentRate.load();
        return ASE_OK;
    }
    ASIOError setSampleRate(ASIOSampleRate sampleRate) override {
        if (canSampleRate(sampleRate) != ASE_OK) return ASE_NoClock;
        st_->currentRate.store(sampleRate);
        return ASE_OK;
    }
    ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) override {
        if (clocks && numSources && *numSources >= 1) {
            clocks[0].index = 0;
            clocks[0].associatedChannel = -1;
            clocks[0].associatedGroup = -1;
            clocks[0].isCurrentSource = ASIOTrue;
            std::strcpy(clocks[0].name, "Internal");
            *numSources = 1;
        }
        return ASE_OK;
    }
    ASIOError setClockSource(long) override { return ASE_OK; }
    ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override {
        if (st_->deviceLost.load()) return ASE_NotPresent;
        if (sPos) { sPos->hi = 0; sPos->lo = 0; }
        if (tStamp) { tStamp->hi = 0; tStamp->lo = 0; }
        return ASE_OK;
    }
    ASIOError getChannelInfo(ASIOChannelInfo* info) override {
        if (!info || info->isInput == ASIOTrue || info->channel < 0 || info->channel >= st_->numOutputChannels)
            return ASE_InvalidParameter;
        info->isActive = ASIOTrue;
        info->channelGroup = 0;
        info->type = (st_->currentIoFormat.load() == kASIODSDFormat) ? st_->dsdType : st_->pcmType;
        std::snprintf(info->name, sizeof(info->name), "Out %ld", info->channel);
        return ASE_OK;
    }
    ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) override {
        if (st_->failCreateBuffers || !bufferInfos || !callbacks || numChannels != 2 || bufferSize <= 0)
            return ASE_NoMemory;
        if (st_->currentIoFormat.load() == kASIODSDFormat) {
            // Az ASIO DSD specifikáció szerint a DSD pufferméretnek 8-cal oszthatónak és [min, max] közöttinek kell lennie
            if (bufferSize < st_->dsdMinBufferSize || bufferSize > st_->dsdMaxBufferSize || (bufferSize % 8) != 0)
                return ASE_InvalidMode;
        }
        const ASIOSampleType t = (st_->currentIoFormat.load() == kASIODSDFormat) ? st_->dsdType : st_->pcmType;
        const size_t bytes = st_->computePayloadBytes(bufferSize, t);
        st_->createdBufferSize = bufferSize;
        st_->payloadSize = bytes;
        const size_t extraGuard = static_cast<size_t>(bufferSize > 0 ? bufferSize : 0) * 2u + FakeAsioState::kGuardBytes;
        for (int ch = 0; ch < 2; ++ch) {
            for (int b = 0; b < 2; ++b) {
                st_->buffers[ch][b].assign(bytes + extraGuard, FakeAsioState::kGuardPattern);
                std::memset(st_->buffers[ch][b].data(), 0, bytes);
                bufferInfos[ch].buffers[b] = st_->buffers[ch][b].data();
            }
        }
        st_->callbacks = *callbacks;
        st_->hasCallbacks = true;
        return ASE_OK;
    }
    ASIOError disposeBuffers() override {
        st_->disposeCalls.fetch_add(1);
        st_->hasCallbacks = false;
        return ASE_OK;
    }
    ASIOError controlPanel() override { return ASE_NotPresent; }
    ASIOError future(long selector, void* opt) override {
        if (selector == kAsioSetIoFormat && opt) {
            auto* f = static_cast<ASIOIoFormat*>(opt);
            if (f->FormatType == kASIODSDFormat) {
                if (!st_->allowNativeDsd) {
                    f->FormatType = kASIOFormatInvalid;
                    return ASE_InvalidMode;
                }
                st_->currentIoFormat.store(kASIODSDFormat);
                return ASE_SUCCESS;
            }
            if (f->FormatType == kASIOPCMFormat) {
                st_->currentIoFormat.store(kASIOPCMFormat);
                return ASE_SUCCESS;
            }
            f->FormatType = kASIOFormatInvalid;
            return ASE_InvalidParameter;
        }
        if (selector == kAsioGetIoFormat && opt) {
            auto* f = static_cast<ASIOIoFormat*>(opt);
            f->FormatType = st_->currentIoFormat.load();
            return ASE_SUCCESS;
        }
        if (selector == kAsioCanDoIoFormat && opt) {
            auto* f = static_cast<ASIOIoFormat*>(opt);
            if (f->FormatType == kASIODSDFormat && !st_->allowNativeDsd) {
                f->FormatType = kASIOFormatInvalid;
                return ASE_InvalidMode;
            }
            return ASE_SUCCESS;
        }
        return ASE_NotPresent;
    }
    ASIOError outputReady() override {
        st_->outputReadyCalls.fetch_add(1);
        return ASE_OK;
    }

private:
    std::atomic<ULONG> ref_{1};
    std::shared_ptr<FakeAsioState> st_;
};

// ---------------------------------------------------------------------------
// K3: Meghajtó-lista (registry olvasás) és gyáron keresztüli betöltés
// ---------------------------------------------------------------------------
static void testK3DriverListAndFactory() {
    std::printf("\n[K3] Meghajtó-lista (registry) és hamis meghajtó betöltése a gyáron át\n");
    bool noThrow = true;
    std::vector<AsioDriverInfo> realList;
    try {
        realList = AsioPlayerOutput::listDrivers();
    } catch (...) {
        noThrow = false;
    }
    CHECK(noThrow, "K3: HKLM\\SOFTWARE\\ASIO olvasása nem dob kivételt (talált meghajtók: %zu)", realList.size());

    const auto emptyList = AsioPlayerOutput::listDrivers(HKEY_CURRENT_USER, L"Software\\AqulifyNonExistentAsioKey_12345");
    CHECK(emptyList.empty(), "K3: nem létező registry kulcsra üres lista (%zu)", emptyList.size());

    const wchar_t* testSubKey = L"Software\\AqulifyAsioTest_K3";
    RegDeleteTreeW(HKEY_CURRENT_USER, testSubKey);
    HKEY hRoot = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, testSubKey, 0, nullptr, 0, KEY_WRITE, nullptr, &hRoot, nullptr) == ERROR_SUCCESS) {
        HKEY hDrv1 = nullptr;
        if (RegCreateKeyExW(hRoot, L"TestDAC ASIO Driver", 0, nullptr, 0, KEY_WRITE, nullptr, &hDrv1, nullptr) == ERROR_SUCCESS) {
            const wchar_t* clsid1 = L"{12345678-1234-1234-1234-123456789ABC}";
            const wchar_t* desc1 = L"TestDAC USB ASIO";
            RegSetValueExW(hDrv1, L"CLSID", 0, REG_SZ, reinterpret_cast<const BYTE*>(clsid1), static_cast<DWORD>((std::wcslen(clsid1) + 1) * sizeof(wchar_t)));
            RegSetValueExW(hDrv1, L"Description", 0, REG_SZ, reinterpret_cast<const BYTE*>(desc1), static_cast<DWORD>((std::wcslen(desc1) + 1) * sizeof(wchar_t)));
            RegCloseKey(hDrv1);
        }
        HKEY hDrv2 = nullptr;
        if (RegCreateKeyExW(hRoot, L"BrokenDriver", 0, nullptr, 0, KEY_WRITE, nullptr, &hDrv2, nullptr) == ERROR_SUCCESS) {
            const wchar_t* clsid2 = L"not-a-valid-clsid";
            RegSetValueExW(hDrv2, L"CLSID", 0, REG_SZ, reinterpret_cast<const BYTE*>(clsid2), static_cast<DWORD>((std::wcslen(clsid2) + 1) * sizeof(wchar_t)));
            RegCloseKey(hDrv2);
        }
        RegCloseKey(hRoot);
    }
    const auto parsed = AsioPlayerOutput::listDrivers(HKEY_CURRENT_USER, testSubKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, testSubKey);
    CHECK(parsed.size() == 1 && parsed[0].clsid == "{12345678-1234-1234-1234-123456789ABC}" && parsed[0].description == "TestDAC USB ASIO",
          "K3: registry alkulcsok (CLSID + Description) pontos kiolvasása és érvénytelen CLSID kiszűrése");

    auto st = std::make_shared<FakeAsioState>();
    AsioPlayerOutput out("FakeASIO", [st](std::string*) -> IASIO* { return new FakeAsioDriver(st); });
    std::string err;
    const bool opened = out.open(48000, [](float* s, size_t n) { std::memset(s, 0, n * 2 * sizeof(float)); }, &err);
    CHECK(opened && st->initCalls.load() == 1 && out.isOpen(), "K3: hamis meghajtó betöltése a gyáron át sikeres (err='%s')", err.c_str());
    out.close();
}

// ---------------------------------------------------------------------------
// K4: PCM-formátumok (5 mintatípus × 5 mintavétel = 25 eset, 24 bites bitpontosság, őrbájtok)
// ---------------------------------------------------------------------------
static void testK4PcmFormats() {
    std::printf("\n[K4] PCM-formátumok: 5 mintatípus × 5 mintavétel (44,1 / 96 / 192 / 384 / 768 kHz)\n");
    const std::vector<ASIOSampleType> types = {
        ASIOSTInt32LSB, ASIOSTInt24LSB, ASIOSTInt16LSB, ASIOSTFloat32LSB, ASIOSTFloat64LSB
    };
    const std::vector<uint32_t> rates = { 44100, 96000, 192000, 384000, 768000 };
    const size_t frames = 64;

    std::vector<int32_t> exact24L(frames), exact24R(frames);
    std::vector<float> expectedL(frames), expectedR(frames);
    for (size_t i = 0; i < frames; ++i) {
        int32_t vL = static_cast<int32_t>((static_cast<int64_t>(i) * 321097 - 4000000) & 0x7FFFFF);
        if (i & 1) vL = -vL;
        int32_t vR = static_cast<int32_t>((static_cast<int64_t>(i) * 512341 - 2000000) & 0x7FFFFF);
        if ((i & 2) == 0) vR = -vR;
        if (i == 0) { vL = 0x123456; vR = -0x654321; }
        if (i == 1) { vL = 8388607; vR = -8388608; }
        exact24L[i] = vL;
        exact24R[i] = vR;
        expectedL[i] = static_cast<float>(std::ldexp(static_cast<double>(vL), -23));
        expectedR[i] = static_cast<float>(std::ldexp(static_cast<double>(vR), -23));
    }

    int passCount = 0;
    bool exact24BitOk = true;
    bool allGuardsOk = true;
    for (ASIOSampleType t : types) {
        for (uint32_t sr : rates) {
            auto st = std::make_shared<FakeAsioState>();
            st->pcmType = t;
            st->preferredBufferSize = static_cast<long>(frames);
            AsioPlayerOutput out("Fake", [st](std::string*) -> IASIO* { return new FakeAsioDriver(st); });
            std::string err;
            bool ok = out.open(sr, [&](float* stereo, size_t n) {
                for (size_t i = 0; i < n; ++i) {
                    stereo[2 * i]     = expectedL[i % frames];
                    stereo[2 * i + 1] = expectedR[i % frames];
                }
            }, &err);
            out.setActive(true);
            for (int wait = 0; wait < 100 && !out.isRunning(); ++wait)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            st->triggerBufferSwitch(0);

            const std::vector<uint8_t> refL = refExpectedPcmBytes(expectedL, t);
            const std::vector<uint8_t> refR = refExpectedPcmBytes(expectedR, t);
            const std::vector<uint8_t> gotL = st->payload(0, 0);
            const std::vector<uint8_t> gotR = st->payload(1, 0);
            const bool matchL = (gotL == refL);
            const bool matchR = (gotR == refR);
            const bool guardsOk = st->checkGuardBytes();
            if (!guardsOk) allGuardsOk = false;

            if (t == ASIOSTInt24LSB && gotL.size() == frames * 3) {
                for (size_t i = 0; i < frames; ++i) {
                    const uint8_t* p = &gotL[i * 3];
                    int32_t got = static_cast<int32_t>(p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16));
                    if (got & 0x800000) got |= ~0xFFFFFF;
                    if (got != exact24L[i]) exact24BitOk = false;
                }
            } else if (t == ASIOSTInt32LSB && gotL.size() == frames * 4) {
                for (size_t i = 0; i < frames; ++i) {
                    int32_t got32 = 0;
                    std::memcpy(&got32, &gotL[i * 4], 4);
                    if ((got32 >> 8) != exact24L[i] || (got32 & 0xFF) != 0) exact24BitOk = false;
                }
            }

            out.close();
            if (ok && matchL && matchR && guardsOk && std::llround(st->currentRate.load()) == sr) {
                ++passCount;
            }
        }
    }
    CHECK(passCount == 25, "K4: mind a 25 (5 mintatípus × 5 mintavétel) kombináció bájtról bájtra egyezik (%d/25)", passCount);
    CHECK(exact24BitOk, "K4: 24 bites forrás egész formátumon (ASIOSTInt24LSB és ASIOSTInt32LSB) bitpontosan jut át");
    CHECK(allGuardsOk, "K4: a PCM pufferek mögötti 64 őrbájt (0xA5) sértetlen maradt mind a 25 esetben");

    auto stBad = std::make_shared<FakeAsioState>();
    stBad->pcmType = ASIOSTInt32MSB;
    AsioPlayerOutput outBad("Fake", [stBad](std::string*) -> IASIO* { return new FakeAsioDriver(stBad); });
    std::string errBad;
    const bool okBad = outBad.open(48000, [](float*, size_t) {}, &errBad);
    CHECK(!okBad && errBad == "format", "K4: nem támogatott mintatípusra érthető hiba (err='%s')", errBad.c_str());
    outBad.close();
}

// ---------------------------------------------------------------------------
// K5: supportedRates() (canSampleRate + zárt állapotú gyorsítótár) és actualRate()
// ---------------------------------------------------------------------------
static void testK5CapabilitiesAndRates() {
    std::printf("\n[K5] supportedRates() (canSampleRate, gyorsítótár) és actualRate() (getSampleRate)\n");
    auto st = std::make_shared<FakeAsioState>();
    st->supportedRates = { 44100, 48000, 96000, 384000, 768000 };
    AsioPlayerOutput out("FakeDrvA", [st](std::string*) -> IASIO* { return new FakeAsioDriver(st); });

    const std::vector<uint32_t> expected = { 44100, 48000, 96000, 384000, 768000 };
    const std::vector<uint32_t> got1 = out.supportedRates();
    const int initAfter1 = st->initCalls.load();
    const std::vector<uint32_t> got2 = out.supportedRates();
    const int initAfter2 = st->initCalls.load();

    CHECK(got1 == expected && got2 == expected && st->canRateCalls.load() == 10,
          "K5: supportedRates() a meghajtó canSampleRate lekérdezéséből adja a pontos listát (hívások: %d)",
          st->canRateCalls.load());
    CHECK(initAfter1 == 1 && initAfter2 == 1,
          "K5: két egymás utáni zárt állapotú supportedRates() hívásnál az initCalls csak egyszer nő (%d -> %d)",
          initAfter1, initAfter2);
    CHECK(out.actualRate() == 0, "K5: zárt állapotban actualRate() == 0");
    CHECK(out.settlesOnRateChange() == true, "K5: settlesOnRateChange() == true");

    std::string err;
    const bool opened = out.open(384000, [](float* s, size_t n) { std::memset(s, 0, n * 2 * sizeof(float)); }, &err);
    CHECK(opened && out.actualRate() == 384000, "K5: nyitott állapotban actualRate() == %u Hz (getSampleRate)", out.actualRate());

    // Reset után a gyorsítótár érvénytelenítődik, így zárt állapotban újra lekérdez
    st->triggerResetRequest();
    for (int w = 0; w < 200 && out.isOpen(); ++w)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    out.close();

    const int initBeforeReprobe = st->initCalls.load();
    st->supportedRates = { 44100, 48000, 192000 };
    const std::vector<uint32_t> gotAfterReset = out.supportedRates();
    const int initAfterReset = st->initCalls.load();
    const std::vector<uint32_t> expectedAfterReset = { 44100, 48000, 192000 };
    CHECK(gotAfterReset == expectedAfterReset && initAfterReset == initBeforeReprobe + 1,
          "K5: kAsioResetRequest után a supportedRates() gyorsítótár érvénytelenítődött és újra lekérdezett (%d -> %d)",
          initBeforeReprobe, initAfterReset);

    // Másik meghajtóra váltáskor (setDriver) szintén újra lekérdez
    out.setDriver("FakeDrvB", [st](std::string*) -> IASIO* { return new FakeAsioDriver(st); });
    const std::vector<uint32_t> gotNewDrv = out.supportedRates();
    CHECK(gotNewDrv == expectedAfterReset && st->initCalls.load() == initAfterReset + 1,
          "K5: másik meghajtóra váltás (setDriver) után a supportedRates() újra lekérdez (%d -> %d)",
          initAfterReset, st->initCalls.load());
}

// ---------------------------------------------------------------------------
// K6 és K7: Natív DSD (DSD64..DSD1024, MSB1/LSB1, specifikáció szerinti bufferSize DSD-bitben) és DoP PCM
// ---------------------------------------------------------------------------
static void testK6K7DsdAndDop() {
    std::printf("\n[K6 / K7] Natív DSD (DSD64..DSD1024, bufferSize DSD-mintában: bufferSize/8 bájt, bufferSize/16 DoP-keret) és DoP PCM\n");
    const std::vector<uint32_t> dopRates = { 176400, 352800, 705600, 1411200, 2822400 };
    const std::vector<ASIOSampleType> dsdTypes = { ASIOSTDSDInt8MSB1, ASIOSTDSDInt8LSB1 };

    // A Steinberg ASIO 2.3 specifikáció szerint DSD64-nél: min 512, pref 4096, max 32768 DSD-minta (bit), gran 8.
    // 4096 DSD-bit = 512 DSD-bájt csatornánként = 256 DoP-keret bufferSwitch-enként.
    const long dsdBufSamples = 4096;
    const size_t expChannelBytes = static_cast<size_t>(dsdBufSamples) / 8u; // 512 bájt
    const size_t expDopFrames = static_cast<size_t>(dsdBufSamples) / 16u;   // 256 DoP-keret

    std::vector<uint8_t> rawDsdL(expChannelBytes), rawDsdR(expChannelBytes);
    for (size_t i = 0; i < expChannelBytes; ++i) {
        uint8_t bL = static_cast<uint8_t>((i * 37 + 0x11) & 0xFF);
        if (bL == 0x05 || bL == 0xFA || refReverseBits8(bL) == 0x05 || refReverseBits8(bL) == 0xFA) bL = 0x33;
        uint8_t bR = static_cast<uint8_t>((i * 73 + 0x29) & 0xFF);
        if (bR == 0x05 || bR == 0xFA || refReverseBits8(bR) == 0x05 || refReverseBits8(bR) == 0xFA) bR = 0x47;
        rawDsdL[i] = bL;
        rawDsdR[i] = bR;
    }

    int nativePass = 0;
    bool noMarkerLeak = true;
    bool allGuardsOk = true;
    bool allFramesAndConsumedOk = true;

    for (ASIOSampleType dt : dsdTypes) {
        for (uint32_t dr : dopRates) {
            auto st = std::make_shared<FakeAsioState>();
            st->allowNativeDsd = true;
            st->dsdType = dt;
            st->dsdMinBufferSize = 512;
            st->dsdMaxBufferSize = 32768;
            st->dsdPreferredBufferSize = dsdBufSamples;
            st->dsdGranularity = 8;

            size_t gotRenderDopFrames = 0;
            AsioPlayerOutput out("Fake", [st](std::string*) -> IASIO* { return new FakeAsioDriver(st); });
            std::string err;
            bool ok = out.openDop(dr, [&](int32_t* stereoDop24, size_t n) {
                gotRenderDopFrames = n;
                for (size_t i = 0; i < n; ++i) {
                    const uint8_t marker = (i & 1) ? 0xFAu : 0x05u;
                    const uint8_t b0L = rawDsdL[(2 * i) % rawDsdL.size()];
                    const uint8_t b1L = rawDsdL[(2 * i + 1) % rawDsdL.size()];
                    const uint8_t b0R = rawDsdR[(2 * i) % rawDsdR.size()];
                    const uint8_t b1R = rawDsdR[(2 * i + 1) % rawDsdR.size()];
                    stereoDop24[2 * i]     = static_cast<int32_t>((uint32_t(marker) << 16) | (uint32_t(b0L) << 8) | b1L);
                    stereoDop24[2 * i + 1] = static_cast<int32_t>((uint32_t(marker) << 16) | (uint32_t(b0R) << 8) | b1R);
                }
            }, &err);
            out.setActive(true);
            for (int wait = 0; wait < 100 && !out.isRunning(); ++wait)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            st->triggerBufferSwitch(0);

            std::vector<uint8_t> expL(expChannelBytes), expR(expChannelBytes);
            for (size_t i = 0; i < expChannelBytes; ++i) {
                expL[i] = (dt == ASIOSTDSDInt8LSB1) ? refReverseBits8(rawDsdL[i]) : rawDsdL[i];
                expR[i] = (dt == ASIOSTDSDInt8LSB1) ? refReverseBits8(rawDsdR[i]) : rawDsdR[i];
            }
            const auto gotL = st->payload(0, 0);
            const auto gotR = st->payload(1, 0);
            for (uint8_t b : gotL) {
                if (b == 0x05 || b == 0xFA) noMarkerLeak = false;
            }
            if (!st->checkGuardBytes()) allGuardsOk = false;
            if (gotRenderDopFrames != expDopFrames || out.consumedFrames() != expDopFrames || st->payloadSize != expChannelBytes) {
                allFramesAndConsumedOk = false;
            }
            const bool rateOk = (static_cast<uint64_t>(std::llround(st->currentRate.load())) == uint64_t(dr) * 16ull);
            if (ok && out.isNativeDsdActive() && rateOk && gotL == expL && gotR == expR &&
                st->checkGuardBytes() && gotRenderDopFrames == expDopFrames && out.consumedFrames() == expDopFrames) {
                ++nativePass;
            }
            out.close();
        }
    }
    CHECK(nativePass == 10, "K6: natív DSD (DSD64..DSD1024 × MSB1/LSB1, 4096 DSD-bit = 512 bájt/csat) mind a 10 esetben bitpontos (%d/10)", nativePass);
    CHECK(allFramesAndConsumedOk, "K6: renderDop pontosan bufferSize/16 (%zu) keretet kap, és consumedFrames() pontosan bufferSize/16-tal nő", expDopFrames);
    CHECK(allGuardsOk, "K6: a DSD pufferek mögötti 64 őrbájt (0xA5) sértetlen maradt (nincs túlírás)");
    CHECK(noMarkerLeak, "K6: a DoP-jelölő (0x05 / 0xFA) soha nem kerül át a natív DSD bitfolyamba");

    // K6b: Nem 16-tal osztható preferált DSD pufferméret (pl. 4088, min 512, max 32768, gran 8) -> 16-tal osztható választás
    {
        auto st = std::make_shared<FakeAsioState>();
        st->allowNativeDsd = true;
        st->dsdType = ASIOSTDSDInt8MSB1;
        st->dsdMinBufferSize = 512;
        st->dsdMaxBufferSize = 32768;
        st->dsdPreferredBufferSize = 4088; // 8-cal osztható, de 16-tal nem!
        st->dsdGranularity = 8;

        size_t gotFrames = 0;
        AsioPlayerOutput out("Fake", [st](std::string*) -> IASIO* { return new FakeAsioDriver(st); });
        std::string err;
        bool ok = out.openDop(176400, [&](int32_t* d, size_t n) {
            gotFrames = n;
            std::memset(d, 0, n * 2 * sizeof(int32_t));
        }, &err);
        out.setActive(true);
        for (int wait = 0; wait < 100 && !out.isRunning(); ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        st->triggerBufferSwitch(0);
        const long chosen = st->createdBufferSize;
        CHECK(ok && chosen >= 512 && (chosen % 16) == 0 && gotFrames == static_cast<size_t>(chosen / 16) &&
              out.consumedFrames() == static_cast<uint64_t>(chosen / 16) && st->checkGuardBytes(),
              "K6: nem 16-tal osztható preferált DSD méret (4088) esetén 16-tal osztható méretet választott (%ld -> %zu DoP-keret)",
              chosen, gotFrames);
        out.close();
    }

    // K6c: Ha nincs 16-tal osztható megengedett méret -> érthető "buffer" hiba, összeomlás nélkül
    {
        auto st = std::make_shared<FakeAsioState>();
        st->allowNativeDsd = true;
        st->dsdMinBufferSize = 4088;
        st->dsdMaxBufferSize = 4088;
        st->dsdPreferredBufferSize = 4088;
        st->dsdGranularity = 0;
        AsioPlayerOutput out("Fake", [st](std::string*) -> IASIO* { return new FakeAsioDriver(st); });
        std::string err;
        bool ok = out.openDop(176400, [](int32_t*, size_t) {}, &err);
        CHECK(!ok && err == "buffer", "K6: ha nincs 16-tal osztható DSD pufferméret, érthető 'buffer' hiba ('%s')", err.c_str());
        out.close();
    }

    // K7 (a): Ha a meghajtó nem tud natív DSD-t, de PCM-ben tudja a dopRate-et -> DoP PCM-ként (24 bit a 32-ben)
    {
        const size_t dopPcmFrames = 64;
        auto st = std::make_shared<FakeAsioState>();
        st->allowNativeDsd = false;
        st->pcmType = ASIOSTInt32LSB;
        st->preferredBufferSize = static_cast<long>(dopPcmFrames);
        AsioPlayerOutput out("Fake", [st](std::string*) -> IASIO* { return new FakeAsioDriver(st); });
        std::string err;
        bool ok = out.openDop(176400, [&](int32_t* stereoDop24, size_t n) {
            for (size_t i = 0; i < n; ++i) {
                const uint8_t marker = (i & 1) ? 0xFAu : 0x05u;
                stereoDop24[2 * i]     = static_cast<int32_t>((uint32_t(marker) << 16) | 0x1234u);
                stereoDop24[2 * i + 1] = static_cast<int32_t>((uint32_t(marker) << 16) | 0x5678u);
            }
        }, &err);
        out.setActive(true);
        for (int wait = 0; wait < 100 && !out.isRunning(); ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        st->triggerBufferSwitch(0);

        const auto gotL = st->payload(0, 0);
        const auto gotR = st->payload(1, 0);
        bool dopWordsOk = (gotL.size() == dopPcmFrames * 4) && st->checkGuardBytes();
        for (size_t i = 0; i < dopPcmFrames && dopWordsOk; ++i) {
            uint32_t wL = 0, wR = 0;
            std::memcpy(&wL, &gotL[i * 4], 4);
            std::memcpy(&wR, &gotR[i * 4], 4);
            const uint32_t expMarker = (i & 1) ? 0xFAu : 0x05u;
            if (wL != (((expMarker << 16) | 0x1234u) << 8)) dopWordsOk = false;
            if (wR != (((expMarker << 16) | 0x5678u) << 8)) dopWordsOk = false;
        }
        CHECK(ok && !out.isNativeDsdActive() && out.isDopPcmActive() && dopWordsOk,
              "K7: natív DSD hiányában DoP PCM-ként (24 bit a 32-ben) bitpontosan működik");
        out.close();
    }

    // K7 (b): Harmadik eset – sem natív DSD, sem PCM dopRate nem támogatott -> false + error="format"
    {
        auto st = std::make_shared<FakeAsioState>();
        st->allowNativeDsd = false;
        st->supportedRates = { 44100, 48000, 96000 };
        AsioPlayerOutput out("Fake", [st](std::string*) -> IASIO* { return new FakeAsioDriver(st); });
        std::string err;
        bool ok = out.openDop(176400, [](int32_t*, size_t) {}, &err);
        CHECK(!ok && err == "format", "K7: nem támogatott DSD/DoP esetén false és error='format' (kapott: '%s')", err.c_str());
        out.close();
    }
}

// ---------------------------------------------------------------------------
// K8: Hibák (init, createBuffers, kAsioBufferSizeChange, kAsioResetRequest, eltűnő meghajtó, 2. példány)
// ---------------------------------------------------------------------------
static void testK8ErrorHandlingAndReset() {
    std::printf("\n[K8] Hibakezelés: init-hiba, createBuffers-hiba, kAsioBufferSizeChange, kAsioResetRequest, eltűnő meghajtó\n");

    // 1. Indítási (init) hiba + getErrorMessage megőrzése
    {
        auto st = std::make_shared<FakeAsioState>();
        st->failInit = true;
        AsioPlayerOutput out("Fake", [st](std::string*) -> IASIO* { return new FakeAsioDriver(st); });
        std::string err;
        bool ok = out.open(48000, [](float*, size_t) {}, &err);
        CHECK(!ok && err == "init_failed" && out.lastDriverMessage() == "Simulated init error",
              "K8: meghajtó init() hiba esetén false, 'init_failed' és meghajtó-üzenet megőrizve ('%s')",
              out.lastDriverMessage().c_str());
        out.close();
    }

    // 2. createBuffers hiba
    {
        auto st = std::make_shared<FakeAsioState>();
        st->failCreateBuffers = true;
        AsioPlayerOutput out("Fake", [st](std::string*) -> IASIO* { return new FakeAsioDriver(st); });
        std::string err;
        bool ok = out.open(48000, [](float*, size_t) {}, &err);
        CHECK(!ok && err == "create_buffers", "K8: createBuffers() hiba esetén false és 'create_buffers' ('%s')", err.c_str());
        out.close();
    }

    // 3. Második példány, kAsioBufferSizeChange, kAsioResetRequest és eltűnő meghajtó
    {
        auto st = std::make_shared<FakeAsioState>();
        AsioPlayerOutput out1("Fake1", [st](std::string*) -> IASIO* { return new FakeAsioDriver(st); });
        std::string err1;
        bool ok1 = out1.open(48000, [](float* s, size_t n) { std::memset(s, 0, n * 2 * sizeof(float)); }, &err1);
        out1.setActive(true);
        for (int wait = 0; wait < 100 && !out1.isRunning(); ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // Második példány megnyitása közben -> "busy"
        auto st2 = std::make_shared<FakeAsioState>();
        AsioPlayerOutput out2("Fake2", [st2](std::string*) -> IASIO* { return new FakeAsioDriver(st2); });
        std::string err2;
        bool ok2 = out2.open(48000, [](float*, size_t) {}, &err2);
        CHECK(ok1 && !ok2 && err2 == "busy", "K8: második ASIO-példány nyitása érthető 'busy' hibát ad ('%s')", err2.c_str());

        // kAsioBufferSizeChange: egységesen nem támogatott (0L a kAsioSelectorSupported-re és az üzenetre is)
        const long bufChgSup = st->querySelectorSupported(kAsioBufferSizeChange);
        const long bufChgResp = st->sendBufferSizeChange(1024);
        CHECK(ok1 && bufChgSup == 0L && bufChgResp == 0L && out1.isRunning(),
              "K8: kAsioBufferSizeChange egységesen 0L (supported=%ld, resp=%ld), a meghajtó kAsioResetRequest-et használhat",
              bufChgSup, bufChgResp);

        // Lejátszás közbeni kAsioResetRequest
        const long resetResp = st->triggerResetRequest();
        for (int wait = 0; wait < 200 && out1.isOpen(); ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(resetResp == 1L && out1.resetCount() >= 1 && out1.lastError() == "reset",
              "K8: kAsioResetRequest feldolgozva (resp=%ld, resetCount=%u, lastError='%s')",
              resetResp, out1.resetCount(), out1.lastError().c_str());
        out1.close();

        // Újranyitás kAsioResetRequest után
        std::string errReopen;
        bool reopened = out1.open(96000, [](float* s, size_t n) { std::memset(s, 0, n * 2 * sizeof(float)); }, &errReopen);
        CHECK(reopened && out1.actualRate() == 96000, "K8: kAsioResetRequest után a kimenet hibátlanul újranyitható (96000 Hz)");

        // Eltűnő meghajtó szimulálása lejátszás közben
        out1.setActive(true);
        for (int wait = 0; wait < 100 && !out1.isRunning(); ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        st->deviceLost.store(true);
        for (int wait = 0; wait < 200 && out1.isOpen(); ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(!out1.isOpen() && out1.lastError() == "device_lost",
              "K8: eltűnő meghajtó észlelve, nincs összeomlás (lastError='%s')", out1.lastError().c_str());
        out1.close();
    }
}

// ---------------------------------------------------------------------------
// K9 és K10: 0 foglalás a bufferSwitch alatt + consumedFrames() pontosság
// ---------------------------------------------------------------------------
static void testK9K10RealtimeAndConsumed() {
    std::printf("\n[K9 / K10] Valós idejű foglalásmentesség (bufferSwitch) és consumedFrames()\n");
    auto st = std::make_shared<FakeAsioState>();
    st->pcmType = ASIOSTInt32LSB;
    st->preferredBufferSize = 256;

    std::atomic<int> renderCalls{0};
    AsioPlayerOutput out("Fake", [st](std::string*) -> IASIO* { return new FakeAsioDriver(st); });
    std::string err;
    bool opened = out.open(48000, [&](float* stereo, size_t n) {
        renderCalls.fetch_add(1, std::memory_order_relaxed);
        for (size_t i = 0; i < n * 2; ++i) stereo[i] = 0.25f;
    }, &err);
    out.setActive(true);
    for (int wait = 0; wait < 100 && !out.isRunning(); ++wait)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    st->triggerBufferSwitch(0);
    renderCalls.store(0);

    g_rtAllocCount.store(0, std::memory_order_seq_cst);
    g_rtTrackAlloc = true;
    for (int i = 0; i < 20; ++i) {
        st->triggerBufferSwitch(i & 1);
    }
    g_rtTrackAlloc = false;

    const int gotCalls = renderCalls.load();
    const uint64_t allocs = g_rtAllocCount.load(std::memory_order_seq_cst);
    CHECK(opened && gotCalls == 20, "K9: mind a 20 bufferSwitch render-hívás ténylegesen lefutott (%d/20)", gotCalls);
    CHECK(opened && gotCalls == 20 && allocs == 0,
          "K9: bufferSwitch alatt 0 memória-foglalás (mért: %llu)", static_cast<unsigned long long>(allocs));
    std::printf("K_METRIC: {\"test\":\"K9_rt_allocs\",\"allocs\":%llu,\"calls\":%d}\n",
                static_cast<unsigned long long>(allocs), gotCalls);

    const uint64_t consumed = out.consumedFrames();
    const uint64_t expectedFrames = 21ull * 256ull;
    const uint64_t diff = (consumed > expectedFrames) ? (consumed - expectedFrames) : (expectedFrames - consumed);
    CHECK(opened && consumed > 0 && diff <= 256,
          "K10: consumedFrames() = %llu (várt: %llu ± 256)",
          static_cast<unsigned long long>(consumed), static_cast<unsigned long long>(expectedFrames));
    std::printf("K_METRIC: {\"test\":\"K10_consumed\",\"consumed\":%llu,\"expected\":%llu,\"diff\":%llu}\n",
                static_cast<unsigned long long>(consumed), static_cast<unsigned long long>(expectedFrames),
                static_cast<unsigned long long>(diff));

    out.flush();
    for (int wait = 0; wait < 50 && out.consumedFrames() != 0; ++wait)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(opened && out.consumedFrames() == 0, "K10: flush() után consumedFrames() == 0");
    out.close();
}

// ---------------------------------------------------------------------------
// 7.3: Véletlen-mag teszt (AQ_CHECK_SEED, 40 véletlen eset)
// ---------------------------------------------------------------------------
static void testSeed40(uint64_t seed) {
    std::printf("\n[7.3] Véletlen-mag tesztek (AQ_CHECK_SEED=%llu, 40 véletlen eset)\n",
                static_cast<unsigned long long>(seed));
    std::mt19937_64 rng(seed);
    const std::vector<uint32_t> allCandidates = { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000 };
    const std::vector<ASIOSampleType> pcmTypes = { ASIOSTInt32LSB, ASIOSTInt24LSB, ASIOSTInt16LSB, ASIOSTFloat32LSB, ASIOSTFloat64LSB };
    const std::vector<uint32_t> dopRates = { 176400, 352800, 705600, 1411200, 2822400 };

    int passedCases = 0;
    for (int c = 0; c < 40; ++c) {
        auto st = std::make_shared<FakeAsioState>();
        st->preferredBufferSize = (c % 2 == 0) ? 64 : 128;
        st->dsdPreferredBufferSize = (c % 2 == 0) ? 2048 : 4096;

        st->supportedRates.clear();
        std::vector<uint32_t> expSupported;
        for (uint32_t r : allCandidates) {
            if ((rng() % 100) < 65 || r == 48000) {
                st->supportedRates.insert(r);
                expSupported.push_back(r);
            }
        }

        const int mode = static_cast<int>(rng() % 4);
        bool caseOk = false;

        if (mode == 0 || mode == 1) {
            const ASIOSampleType pt = pcmTypes[rng() % pcmTypes.size()];
            const uint32_t sr = expSupported[rng() % expSupported.size()];
            st->pcmType = pt;
            const size_t nFrames = static_cast<size_t>(st->preferredBufferSize);
            std::vector<float> sigL(nFrames), sigR(nFrames);
            for (size_t i = 0; i < nFrames; ++i) {
                int32_t qL = static_cast<int32_t>((rng() % 16000000LL) - 8000000LL);
                int32_t qR = static_cast<int32_t>((rng() % 16000000LL) - 8000000LL);
                sigL[i] = static_cast<float>(std::ldexp(static_cast<double>(qL), -23));
                sigR[i] = static_cast<float>(std::ldexp(static_cast<double>(qR), -23));
            }
            AsioPlayerOutput out("FakeSeed", [st](std::string*) -> IASIO* { return new FakeAsioDriver(st); });
            const auto gotRates = out.supportedRates();
            std::string err;
            bool ok = out.open(sr, [&](float* s, size_t n) {
                for (size_t i = 0; i < n; ++i) {
                    s[2 * i]     = sigL[i % nFrames];
                    s[2 * i + 1] = sigR[i % nFrames];
                }
            }, &err);
            out.setActive(true);
            for (int w = 0; w < 100 && !out.isRunning(); ++w)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const int nBlocks = 1 + static_cast<int>(rng() % 4);
            for (int b = 0; b < nBlocks; ++b) st->triggerBufferSwitch(b & 1);
            const auto expL = refExpectedPcmBytes(sigL, pt);
            const auto expR = refExpectedPcmBytes(sigR, pt);
            const int lastIdx = (nBlocks - 1) & 1;
            const uint64_t expectedConsumed = static_cast<uint64_t>(nBlocks) * nFrames;
            caseOk = ok && (gotRates == expSupported) && (out.actualRate() == sr) &&
                     (st->payload(0, lastIdx) == expL) && (st->payload(1, lastIdx) == expR) &&
                     st->checkGuardBytes() && (out.consumedFrames() == expectedConsumed);
            out.close();
        } else if (mode == 2) {
            const uint32_t dr = dopRates[rng() % dopRates.size()];
            const ASIOSampleType dt = (rng() & 1) ? ASIOSTDSDInt8LSB1 : ASIOSTDSDInt8MSB1;
            st->allowNativeDsd = true;
            st->dsdType = dt;
            const size_t dsdBytes = static_cast<size_t>(st->dsdPreferredBufferSize) / 8u;
            const size_t dopFrames = static_cast<size_t>(st->dsdPreferredBufferSize) / 16u;
            std::vector<uint8_t> dsdL(dsdBytes), dsdR(dsdBytes);
            for (size_t i = 0; i < dsdBytes; ++i) {
                dsdL[i] = static_cast<uint8_t>(rng() & 0xFF);
                dsdR[i] = static_cast<uint8_t>(rng() & 0xFF);
            }
            AsioPlayerOutput out("FakeSeed", [st](std::string*) -> IASIO* { return new FakeAsioDriver(st); });
            std::string err;
            bool ok = out.openDop(dr, [&](int32_t* d, size_t n) {
                for (size_t i = 0; i < n; ++i) {
                    const uint8_t m = (i & 1) ? 0xFAu : 0x05u;
                    d[2 * i]     = static_cast<int32_t>((uint32_t(m) << 16) | (uint32_t(dsdL[2 * i]) << 8) | dsdL[2 * i + 1]);
                    d[2 * i + 1] = static_cast<int32_t>((uint32_t(m) << 16) | (uint32_t(dsdR[2 * i]) << 8) | dsdR[2 * i + 1]);
                }
            }, &err);
            out.setActive(true);
            for (int w = 0; w < 100 && !out.isRunning(); ++w)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            st->triggerBufferSwitch(0);
            std::vector<uint8_t> expL(dsdBytes), expR(dsdBytes);
            for (size_t i = 0; i < dsdBytes; ++i) {
                expL[i] = (dt == ASIOSTDSDInt8LSB1) ? refReverseBits8(dsdL[i]) : dsdL[i];
                expR[i] = (dt == ASIOSTDSDInt8LSB1) ? refReverseBits8(dsdR[i]) : dsdR[i];
            }
            caseOk = ok && out.isNativeDsdActive() && (st->payload(0, 0) == expL) && (st->payload(1, 0) == expR) &&
                     st->checkGuardBytes() && (out.consumedFrames() == dopFrames);
            out.close();
        } else {
            if (rng() & 1) st->failInit = true;
            else st->failCreateBuffers = true;
            AsioPlayerOutput out("FakeSeed", [st](std::string*) -> IASIO* { return new FakeAsioDriver(st); });
            std::string err;
            bool ok = out.open(48000, [](float*, size_t) {}, &err);
            caseOk = !ok && !err.empty();
            out.close();
        }
        if (caseOk) ++passedCases;
    }
    CHECK(passedCases == 40, "7.3 SEED: mind a 40 véletlen konfiguráció egyezik a független elvárással (%d/40)", passedCases);
    std::printf("K_METRIC: {\"test\":\"SEED40\",\"seed\":%llu,\"passed\":%d}\n",
                static_cast<unsigned long long>(seed), passedCases);
}

// ---------------------------------------------------------------------------
// K11: Őrszem (beépített hibajelentés, 1b): az ASIO-kimenet szála és a meghajtó visszahívása jelez, a megálló
// visszahívást az őrszem észreveszi, leállítás után nincs beragadás
// ---------------------------------------------------------------------------
static const aq::wd::Slot* findHeartbeat(const char* name) {
    const aq::wd::Slot* slots = aq::wd::Registry::global().slots();
    for (int i = 0; i < aq::wd::kMaxSlots; ++i)
        if (slots[i].used.load() && std::strncmp(slots[i].name, name, aq::wd::kNameLen) == 0) return &slots[i];
    return nullptr;
}

static int countEvents(const std::vector<aq::wd::Event>& ev, const char* name, bool recovered) {
    int n = 0;
    for (const auto& e : ev) n += (e.thread == name && e.recovered == recovered) ? 1 : 0;
    return n;
}

static void testK11Heartbeat() {
    std::printf("\n[K11] Őrszem: az ASIO-kimenet szála és a meghajtó visszahívása jelez; leállítás után nincs beragadás\n");
    auto st = std::make_shared<FakeAsioState>();
    st->preferredBufferSize = 64;
    AsioPlayerOutput out("Fake", [st](std::string*) -> IASIO* { return new FakeAsioDriver(st); });
    std::string err;
    const bool opened = out.open(48000, [](float* s, size_t n) { std::memset(s, 0, n * 2 * sizeof(float)); }, &err);
    out.setActive(true);
    for (int w = 0; w < 200 && !out.isRunning(); ++w) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(opened && out.isRunning(), "K11: a hamis meghajtó elindult (err='%s')", err.c_str());

    // 1. A kimeneti szál regisztrált, és a hurok közben jelez
    const aq::wd::Slot* w = findHeartbeat("player-asio");
    CHECK(w != nullptr, "K11: a kimeneti szál regisztrált (player-asio)");
    int64_t w1 = w ? w->lastNs.load() : 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    int64_t w2 = w ? w->lastNs.load() : 0;
    const char* wl = w ? w->label.load() : "";
    CHECK(w1 > 0 && w2 > w1, "K11: a kimeneti szál a hurok közben jelez (%.0f ms alatt új szívverés)", (w2 - w1) / 1e6);
    CHECK(wl && std::strncmp(wl, "asio:", 5) == 0, "K11: a címke a hurok helyét mondja (\"%s\")", wl ? wl : "");

    // 2. A meghajtó visszahívása (bufferSwitch) külön jelez
    const aq::wd::Slot* cb = findHeartbeat("player-asio-cb");
    CHECK(cb != nullptr, "K11: a meghajtó visszahívása regisztrált (player-asio-cb)");
    const int64_t c1 = cb ? cb->lastNs.load() : 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    st->triggerBufferSwitch(0);
    const int64_t c2 = cb ? cb->lastNs.load() : 0;
    CHECK(c2 > c1, "K11: a bufferSwitch szívverést ad");

    // 3. Futás közben (a visszahívás 20 ms-onként jön) az őrszem nem jelez
    aq::wd::Watchdog wd(aq::wd::Registry::global(), 0.3);
    std::vector<aq::wd::Event> ev;
    for (int i = 0; i < 40; ++i) {
        st->triggerBufferSwitch(i & 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for (auto& e : wd.check(aq::wd::nowNs())) ev.push_back(e);
    }
    CHECK(countEvents(ev, "player-asio", false) == 0 && countEvents(ev, "player-asio-cb", false) == 0,
          "K11: rendes futás közben nincs beragadás (%zu esemény)", ev.size());

    // 4. A meghajtó nem hív vissza (a hamis meghajtó „elhallgat”): az őrszem a visszahívást jelzi, a szálat nem
    ev.clear();
    for (int i = 0; i < 40; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for (auto& e : wd.check(aq::wd::nowNs())) ev.push_back(e);
    }
    CHECK(countEvents(ev, "player-asio-cb", false) == 1, "K11: a megálló visszahívást az őrszem jelzi (player-asio-cb)");
    CHECK(countEvents(ev, "player-asio", false) == 0, "K11: a kimeneti szál közben is jelez (nincs hamis riasztás)");

    // 5. Szünet (a meghajtó leáll): a visszahívástól nem várunk jelzést
    out.setActive(false);
    for (int i = 0; i < 200 && out.isRunning(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ev.clear();
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for (auto& e : wd.check(aq::wd::nowNs())) ev.push_back(e);
    }
    CHECK(!out.isRunning() && countEvents(ev, "player-asio-cb", false) == 0 && countEvents(ev, "player-asio", false) == 0,
          "K11: szünetben nincs beragadás-jelzés");

    // 6. Leállítás után a szálak nincsenek a regiszterben, és nem jeleznek beragadást
    out.close();
    ev.clear();
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for (auto& e : wd.check(aq::wd::nowNs())) ev.push_back(e);
    }
    // (a regiszterben maradhat, ha a kimenet objektuma él; ekkor a jelzője hamis: nem várunk tőle szívverést)
    auto idle = [](const aq::wd::Slot* s) {
        if (!s) return true;
        const std::atomic<bool>* e = s->expect.load();
        return e != nullptr && !e->load();
    };
    CHECK(idle(findHeartbeat("player-asio")) && idle(findHeartbeat("player-asio-cb")),
          "K11: leállítás után az őrszem nem vár jelzést a szálaktól");
    CHECK(countEvents(ev, "player-asio", false) == 0 && countEvents(ev, "player-asio-cb", false) == 0,
          "K11: leállítás után nincs beragadás-jelzés");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* seedEnv = std::getenv("AQ_CHECK_SEED");
    const uint64_t seed = (seedEnv && *seedEnv) ? std::strtoull(seedEnv, nullptr, 10) : 20261118ull;
    std::printf("AQ_CHECK_SEED=%llu\n", static_cast<unsigned long long>(seed));
    std::printf("========================================================\n");
    std::printf(" Aqulify – Windows ASIO-kimenet motor teszt (test_asio)\n");
    std::printf("========================================================\n");

    testK3DriverListAndFactory();
    testK4PcmFormats();
    testK5CapabilitiesAndRates();
    testK6K7DsdAndDop();
    testK8ErrorHandlingAndReset();
    testK9K10RealtimeAndConsumed();
    testSeed40(seed);
    testK11Heartbeat();

    // K12: DSD->DSD váltás és lezárás: DoP PCM pufferben soha nem 0x00 (mindig váltakozó 0x05/0xFA + 0xAA),
    // futó ASIO flush() nem töri meg a DoP-jelölőt, close() előtt csendes lezárás fut.
    {
        auto st = std::make_shared<FakeAsioState>();
        st->allowNativeDsd = false;   // DoP PCM ág (ASIOSTInt32LSB)
        st->pcmType = ASIOSTInt32LSB;
        st->preferredBufferSize = 128;
        st->supportedRates.insert(176400);

        AsioPlayerOutput out("FakeDopSwitch", [st](std::string*) -> IASIO* { return new FakeAsioDriver(st); });
        std::string err;
        uint8_t phase = 0;
        const bool ok = out.openDop(176400, [&](int32_t* d, size_t n) {
            for (size_t i = 0; i < n; ++i) {
                const uint8_t m = ((phase + i) & 1u) ? 0xFAu : 0x05u;
                const int32_t w = dsd::packDopSample(0x5Au, 0xA5u, m);
                d[2 * i] = w;
                d[2 * i + 1] = w;
            }
            phase = static_cast<uint8_t>((phase + n) & 1u);
        }, &err);

        // 1. Indulás előtti driver-pufferek (clearDriverBuffers) is érvényes DoP 1.1 csendet tartalmaznak (nem 0x00!)
        auto checkDopSilenceBuf = [](const std::vector<uint8_t>& buf) {
            if (buf.size() < 8 || (buf.size() % 4) != 0) return false;
            const size_t n = buf.size() / 4;
            uint8_t prev = 0;
            for (size_t i = 0; i < n; ++i) {
                uint32_t w = 0;
                std::memcpy(&w, buf.data() + i * 4, 4);
                const uint8_t m = static_cast<uint8_t>((w >> 24) & 0xFFu);
                const uint16_t pay = static_cast<uint16_t>((w >> 8) & 0xFFFFu);
                if ((m != 0x05u && m != 0xFAu) || pay != 0xAAAAu) return false;
                if (i > 0 && m == prev) return false;
                prev = m;
            }
            return true;
        };
        const bool initBufOk = checkDopSilenceBuf(st->payload(0, 0)) && checkDopSilenceBuf(st->payload(1, 0)) &&
                               checkDopSilenceBuf(st->payload(0, 1)) && checkDopSilenceBuf(st->payload(1, 1));
        CHECK(ok && out.isDopPcmActive() && initBufOk,
              "K12: DoP PCM kezdeti driver-pufferekben érvényes 0x05/0xFA + 0xAAAA DoP-csend (soha nem 0x00)");

        // 2. Futás közbeni flush(): nem törli nullára a puffert és nem szakítja meg a DoP-jelölőt
        out.setActive(true);
        for (int i = 0; i < 100 && !out.isRunning(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        st->triggerBufferSwitch(0);
        out.flush();
        st->triggerBufferSwitch(1);
        const auto afterFlush = st->payload(0, 1);
        bool flushDopOk = afterFlush.size() == 128 * 4;
        for (size_t i = 0; i < 128 && flushDopOk; ++i) {
            uint32_t w = 0;
            std::memcpy(&w, afterFlush.data() + i * 4, 4);
            const uint8_t m = static_cast<uint8_t>((w >> 24) & 0xFFu);
            const uint16_t pay = static_cast<uint16_t>((w >> 8) & 0xFFFFu);
            const uint8_t expM = (i & 1u) ? 0xFAu : 0x05u;
            if (m != expM || pay != 0x5AA5u) flushDopOk = false;
        }
        CHECK(flushDopOk, "K12: futó ASIO flush() után folyamatos DoP-keretek (nincs 0x00-lyuk)");
        out.close();
    }

    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
