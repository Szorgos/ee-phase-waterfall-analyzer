#pragma once
/**
 * @file player_output_asio.hpp
 * @brief A beépített lejátszó ASIO hangkimenete Windowson (Steinberg ASIO SDK 2.3.4, GPLv3).
 *
 * Kizárólagos, közvetlen meghajtó-elérés (PCM 44,1 … 768 kHz, 5 kis-endián mintatípus; natív DSD64 … DSD1024
 * ASIOSTDSDInt8MSB1 és ASIOSTDSDInt8LSB1 formátumban; tartalékként DoP 1.1 PCM-ben).
 * A meghajtó saját COM STA vezérlőszálon nyílik meg; a bufferSwitch / bufferSwitchTimeInfo hangszál-visszahívás
 * előre lefoglalt munkapufferekkel, 0 memória-foglalással és blokkoló I/O nélkül fut.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <avrt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "asiosys.h"
#include "asio.h"
#ifndef __ASIODRIVER_FWD_DEFINED__
#include "iasiodrv.h"
#endif
#include "dsd.hpp"
#include "file_player.hpp"
#include "pcm_convert.hpp"

namespace ee_analyzer::player {

/** A Windows registryben (HKLM\SOFTWARE\ASIO) regisztrált ASIO-meghajtó adatai. */
struct AsioDriverInfo {
    std::string keyName;      // Az alkulcs neve a SOFTWARE\ASIO alatt
    std::string clsid;        // "{...}" CLSID karakterlánc
    std::string description;  // Description érték (ha üres: keyName)
};

class AsioPlayerOutput : public PlayerOutput {
public:
    using DriverFactory = std::function<IASIO*(std::string* errorOut)>;

    explicit AsioPlayerOutput(std::string driverIdOrName = {}, DriverFactory factory = {})
        : driverIdOrName_(std::move(driverIdOrName)), factory_(std::move(factory)) {}

    ~AsioPlayerOutput() override { close(); }

    AsioPlayerOutput(const AsioPlayerOutput&) = delete;
    AsioPlayerOutput& operator=(const AsioPlayerOutput&) = delete;

    /**
     * Meghajtó-azonosító (és opcionális teszt-gyár) módosítása zárt állapotban;
     * új meghajtó választásakor érvényteleníti a támogatott mintavételek gyorsítótárát.
     */
    void setDriver(std::string driverIdOrName, DriverFactory factory = {}) {
        close();
        if (driverIdOrName_ != driverIdOrName || static_cast<bool>(factory)) {
            invalidateRatesCache();
        }
        driverIdOrName_ = std::move(driverIdOrName);
        if (factory) factory_ = std::move(factory);
    }

    /** A támogatott mintavételek gyorsítótárának kézi érvénytelenítése. */
    void invalidateRatesCache() const {
        std::lock_guard<std::mutex> lk(ratesCacheMutex_);
        ratesCacheValid_ = false;
        cachedRates_.clear();
    }

    /**
     * A telepített ASIO-meghajtók felsorolása a Windows registryből (alapértelmezetten HKLM\SOFTWARE\ASIO),
     * kizárólag olvasási joggal. Nem dob kivételt; üres gépen üres listát ad vissza.
     */
    static std::vector<AsioDriverInfo> listDrivers(HKEY rootKey = HKEY_LOCAL_MACHINE,
                                                   const wchar_t* subKeyPath = L"SOFTWARE\\ASIO") {
        std::vector<AsioDriverInfo> out;
        if (!subKeyPath) return out;
        HKEY hAsio = nullptr;
        if (RegOpenKeyExW(rootKey, subKeyPath, 0, KEY_READ, &hAsio) != ERROR_SUCCESS || !hAsio) {
            return out;
        }
        wchar_t subName[256]{};
        for (DWORD idx = 0;; ++idx) {
            DWORD subLen = static_cast<DWORD>(std::size(subName));
            const LSTATUS st = RegEnumKeyExW(hAsio, idx, subName, &subLen, nullptr, nullptr, nullptr, nullptr);
            if (st != ERROR_SUCCESS) break;

            HKEY hSub = nullptr;
            if (RegOpenKeyExW(hAsio, subName, 0, KEY_READ, &hSub) != ERROR_SUCCESS || !hSub) continue;

            std::wstring clsidW = readRegSz(hSub, L"CLSID");
            std::wstring descW = readRegSz(hSub, L"Description");
            RegCloseKey(hSub);

            if (clsidW.empty()) continue;
            CLSID parsedClsid{};
            if (FAILED(CLSIDFromString(clsidW.c_str(), &parsedClsid))) continue;

            AsioDriverInfo info;
            info.keyName = wideToUtf8(subName);
            info.clsid = wideToUtf8(clsidW.c_str());
            info.description = descW.empty() ? info.keyName : wideToUtf8(descW.c_str());
            out.push_back(std::move(info));
        }
        RegCloseKey(hAsio);
        return out;
    }

    bool open(uint32_t sampleRate, RenderFn render, std::string* error = nullptr) override {
        return startSession(sampleRate, std::move(render), nullptr, false, error);
    }

    bool openDop(uint32_t dopSampleRate, RenderDopFn render, std::string* error = nullptr) override {
        return startSession(dopSampleRate, nullptr, std::move(render), true, error);
    }

    void close() override {
        if (thread_.joinable()) {
            quit_.store(true, std::memory_order_release);
            if (wake_) SetEvent(wake_);
            thread_.join();
        }
        if (wake_) {
            CloseHandle(wake_);
            wake_ = nullptr;
        }
        isOpen_.store(false, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        nativeDsd_.store(false, std::memory_order_release);
        dopPcm_.store(false, std::memory_order_release);
        active_.store(false, std::memory_order_release);
        AsioPlayerOutput* self = this;
        s_activeInstance.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
        std::lock_guard<std::mutex> lk(renderMutex_);
        render_ = nullptr;
        renderDop_ = nullptr;
    }

    void setActive(bool active) override {
        active_.store(active, std::memory_order_release);
        if (wake_) SetEvent(wake_);
    }

    void lock() override { renderMutex_.lock(); }
    void unlock() override { renderMutex_.unlock(); }

    void flush() override {
        consumedFrames_.store(0, std::memory_order_release);
        flushReq_.store(true, std::memory_order_release);
        if (wake_) SetEvent(wake_);
    }

    uint64_t consumedFrames() const override {
        return flushReq_.load(std::memory_order_acquire) ? 0 : consumedFrames_.load(std::memory_order_acquire);
    }

    bool settlesOnRateChange() const override { return true; }

    std::vector<uint32_t> supportedRates() const override {
        static constexpr uint32_t kCandidates[] = {
            44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000
        };
        std::vector<uint32_t> res;
        if (isOpen_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(drvQueryMutex_);
            if (drv_) {
                for (uint32_t r : kCandidates) {
                    if (drv_->canSampleRate(static_cast<ASIOSampleRate>(r)) == ASE_OK) {
                        res.push_back(r);
                    }
                }
                std::lock_guard<std::mutex> clk(ratesCacheMutex_);
                cachedRates_ = res;
                cachedDriverId_ = driverIdOrName_;
                ratesCacheValid_ = true;
                return res;
            }
        }
        {
            std::lock_guard<std::mutex> clk(ratesCacheMutex_);
            if (ratesCacheValid_ && cachedDriverId_ == driverIdOrName_) {
                return cachedRates_;
            }
        }
        // Zárt állapotban egyszer lekérdezzük egy STA szálon, majd meghajtónként gyorsítótárazzuk
        std::thread probeThread([&]() {
            const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            std::string err;
            IASIO* drv = createDriverInstance(&err);
            if (drv) {
                if (drv->init(nullptr) == ASIOTrue) {
                    ASIOIoFormat pcmFmt{};
                    pcmFmt.FormatType = kASIOPCMFormat;
                    drv->future(kAsioSetIoFormat, &pcmFmt);
                    for (uint32_t r : kCandidates) {
                        if (drv->canSampleRate(static_cast<ASIOSampleRate>(r)) == ASE_OK) {
                            res.push_back(r);
                        }
                    }
                }
                drv->Release();
            }
            if (SUCCEEDED(hrCom)) CoUninitialize();
        });
        probeThread.join();
        {
            std::lock_guard<std::mutex> clk(ratesCacheMutex_);
            cachedRates_ = res;
            cachedDriverId_ = driverIdOrName_;
            ratesCacheValid_ = true;
        }
        return res;
    }

    uint32_t actualRate() const override {
        if (!isOpen_.load(std::memory_order_acquire)) return 0;
        std::lock_guard<std::mutex> lk(drvQueryMutex_);
        if (!drv_) return 0;
        ASIOSampleRate sr = 0.0;
        if (drv_->getSampleRate(&sr) != ASE_OK || sr <= 0.0) return 0;
        return static_cast<uint32_t>(std::llround(sr));
    }

    bool isOpen() const { return isOpen_.load(std::memory_order_acquire); }
    bool isRunning() const { return running_.load(std::memory_order_acquire); }
    bool isNativeDsdActive() const { return nativeDsd_.load(std::memory_order_acquire); }
    bool isDopPcmActive() const { return dopPcm_.load(std::memory_order_acquire); }
    uint32_t resetCount() const { return resetCount_.load(std::memory_order_acquire); }
    long bufferSize() const { return bufferSize_; }
    ASIOSampleType sampleType() const { return sampleType_; }

    std::string lastError() const {
        std::lock_guard<std::mutex> lk(errMutex_);
        return lastError_;
    }

    std::string lastDriverMessage() const {
        std::lock_guard<std::mutex> lk(errMutex_);
        return lastDriverMessage_;
    }

private:
    static inline std::atomic<AsioPlayerOutput*> s_activeInstance{nullptr};
    static inline ASIOTime s_asioTime{};

    static std::string wideToUtf8(const wchar_t* w) {
        if (!w || !*w) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1) return {};
        std::string s(static_cast<size_t>(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
        return s;
    }

    static std::wstring utf8ToWide(const std::string& s) {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        if (n <= 1) return {};
        std::wstring w(static_cast<size_t>(n - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    }

    static std::wstring readRegSz(HKEY hKey, const wchar_t* valName) {
        DWORD type = 0, bytes = 0;
        if (RegQueryValueExW(hKey, valName, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS) return {};
        if ((type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) return {};
        std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(hKey, valName, nullptr, &type, reinterpret_cast<LPBYTE>(buf.data()), &bytes) != ERROR_SUCCESS)
            return {};
        return std::wstring(buf.data());
    }

    void setLastError(std::string e, std::string drvMsg = {}) {
        std::lock_guard<std::mutex> lk(errMutex_);
        lastError_ = std::move(e);
        if (!drvMsg.empty()) lastDriverMessage_ = std::move(drvMsg);
    }

    IASIO* createDriverInstance(std::string* errorOut) const {
        if (factory_) {
            return factory_(errorOut);
        }
        const auto drivers = listDrivers();
        if (drivers.empty()) {
            if (errorOut) *errorOut = "no_driver";
            return nullptr;
        }
        const AsioDriverInfo* chosen = &drivers[0];
        if (!driverIdOrName_.empty()) {
            chosen = nullptr;
            for (const auto& d : drivers) {
                if (d.clsid == driverIdOrName_ || d.keyName == driverIdOrName_ || d.description == driverIdOrName_) {
                    chosen = &d;
                    break;
                }
            }
            if (!chosen) {
                if (errorOut) *errorOut = "driver_not_found";
                return nullptr;
            }
        }
        CLSID clsid{};
        const std::wstring wClsid = utf8ToWide(chosen->clsid);
        if (FAILED(CLSIDFromString(wClsid.c_str(), &clsid))) {
            if (errorOut) *errorOut = "invalid_clsid";
            return nullptr;
        }
        IASIO* drv = nullptr;
        HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void**>(&drv));
        if (FAILED(hr) || !drv) {
            if (errorOut) *errorOut = "com_create_failed";
            return nullptr;
        }
        return drv;
    }

    bool startSession(uint32_t rate, RenderFn renderPcm, RenderDopFn renderDop, bool dopMode, std::string* error) {
        // Egyszerre csak egy ASIO-meghajtó példány lehet nyitva (az ASIO visszahívások globálisak)
        AsioPlayerOutput* expected = nullptr;
        if (!s_activeInstance.compare_exchange_strong(expected, this, std::memory_order_acq_rel)) {
            if (expected != this) {
                setLastError("busy");
                if (error) *error = "busy";
                return false;
            }
        }
        close();
        expected = nullptr;
        if (!s_activeInstance.compare_exchange_strong(expected, this, std::memory_order_acq_rel) && expected != this) {
            setLastError("busy");
            if (error) *error = "busy";
            return false;
        }

        {
            std::lock_guard<std::mutex> lk(renderMutex_);
            render_ = std::move(renderPcm);
            renderDop_ = std::move(renderDop);
        }
        reqRate_ = rate;
        dopMode_ = dopMode;
        active_.store(false, std::memory_order_release);
        quit_.store(false, std::memory_order_release);
        flushReq_.store(false, std::memory_order_release);
        resetRequested_.store(false, std::memory_order_release);
        consumedFrames_.store(0, std::memory_order_release);
        nativeDsd_.store(false, std::memory_order_release);
        dopPcm_.store(false, std::memory_order_release);
        isOpen_.store(false, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        setLastError({});

        {
            std::lock_guard<std::mutex> lk(initMutex_);
            initState_ = 0;
            initError_.clear();
            initDriverMsg_.clear();
        }
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        thread_ = std::thread([this] { workerLoop(); });

        std::unique_lock<std::mutex> lk(initMutex_);
        initCv_.wait(lk, [this] { return initState_ != 0; });
        const int st = initState_;
        const std::string errStr = initError_;
        const std::string drvMsg = initDriverMsg_;
        lk.unlock();

        if (st < 0) {
            close();
            setLastError(errStr.empty() ? "open_failed" : errStr, drvMsg);
            if (error) *error = lastError();
            return false;
        }
        return true;
    }

    static bool isSupportedPcmType(ASIOSampleType t) {
        return t == ASIOSTInt32LSB || t == ASIOSTInt24LSB || t == ASIOSTInt16LSB ||
               t == ASIOSTFloat32LSB || t == ASIOSTFloat64LSB;
    }

    /**
     * Egy csatorna egy félpufferének mérete bájtban a Steinberg ASIO 2.3 specifikáció szerint:
     * - PCM módban: bufferSize (keret) * bájt/minta;
     * - Natív DSD módban (common/asio.h, Sony DSD support): bufferSize 1-bites DSD-mintában értendő
     *   ("8 samples per byte"), így a csatornánkénti pufferméret pontosan bufferSize / 8 bájt.
     */
    static size_t channelBufferBytes(long bufferSize, ASIOSampleType t) {
        if (bufferSize <= 0) return 0;
        const size_t sz = static_cast<size_t>(bufferSize);
        switch (t) {
            case ASIOSTInt16LSB: return sz * 2u;
            case ASIOSTInt24LSB: return sz * 3u;
            case ASIOSTInt32LSB:
            case ASIOSTInt32LSB24:
            case ASIOSTFloat32LSB: return sz * 4u;
            case ASIOSTFloat64LSB: return sz * 8u;
            case ASIOSTDSDInt8LSB1:
            case ASIOSTDSDInt8MSB1: return sz / 8u;
            default: return 0;
        }
    }

    /**
     * Natív DSD módban a meghajtó által adott [minSz, maxSz] és granularity korlátok között
     * olyan pufferméretet (DSD-bitben) választ, amely 16-tal osztható (1 DoP-keret = 16 DSD-bit = 2 DSD-bájt).
     * Ha nincs ilyen megengedett méret, 0-t ad vissza.
     */
    static long chooseDsdBufferSize(long minSz, long maxSz, long prefSz, long gran) {
        if (minSz <= 0 || maxSz < minSz) return 0;
        auto valid16 = [&](long s) { return s >= 16 && s >= minSz && s <= maxSz && (s % 16) == 0; };
        if (gran == 0) {
            return (prefSz == minSz && valid16(prefSz)) ? prefSz : 0;
        }
        if (gran < 0) {
            long best = 0;
            long bestDist = LONG_MAX;
            for (long s = minSz; s > 0 && s <= maxSz; s *= 2) {
                if (valid16(s)) {
                    const long d = std::abs(s - prefSz);
                    if (d < bestDist) { bestDist = d; best = s; }
                }
                if (s > maxSz / 2) break;
            }
            return best;
        }
        if (valid16(prefSz) && ((prefSz - minSz) % gran) == 0) return prefSz;
        long best = 0;
        long bestDist = LONG_MAX;
        for (long s = minSz; s <= maxSz; s += gran) {
            if (valid16(s)) {
                const long d = std::abs(s - prefSz);
                if (d < bestDist) { bestDist = d; best = s; }
            }
            if (maxSz - s < gran) break;
        }
        return best;
    }

    void clearDriverBuffers() {
        const size_t bytes = channelBufferBytes(bufferSize_, sampleType_);
        const uint8_t fillByte = nativeDsd_.load(std::memory_order_relaxed) ? dsd::kDsdSilenceByte : 0u;
        for (int ch = 0; ch < 2; ++ch) {
            for (int b = 0; b < 2; ++b) {
                if (bufferInfos_[ch].buffers[b] && bytes > 0) {
                    std::memset(bufferInfos_[ch].buffers[b], fillByte, bytes);
                }
            }
        }
    }

    void workerLoop() {
        const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        DWORD mmcssIdx = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssIdx);

        std::string err;
        IASIO* drv = createDriverInstance(&err);
        bool buffersCreated = false;
        bool started = false;

        auto failInitAndNotify = [&](const std::string& reason, const std::string& drvMsg = {}) {
            if (drv) {
                if (started) drv->stop();
                if (buffersCreated) drv->disposeBuffers();
                {
                    std::lock_guard<std::mutex> qlk(drvQueryMutex_);
                    drv_ = nullptr;
                }
                drv->Release();
                drv = nullptr;
            }
            {
                std::lock_guard<std::mutex> lk(initMutex_);
                initError_ = reason;
                initDriverMsg_ = drvMsg;
                initState_ = -1;
            }
            initCv_.notify_all();
        };

        if (!drv) {
            failInitAndNotify(err.empty() ? "no_driver" : err);
            if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
            if (SUCCEEDED(hrCom)) CoUninitialize();
            return;
        }

        if (drv->init(nullptr) != ASIOTrue) {
            char msg[128]{};
            drv->getErrorMessage(msg);
            failInitAndNotify("init_failed", msg);
            if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
            if (SUCCEEDED(hrCom)) CoUninitialize();
            return;
        }

        long numIn = 0, numOut = 0;
        if (drv->getChannels(&numIn, &numOut) != ASE_OK || numOut < 2) {
            failInitAndNotify("channels");
            if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
            if (SUCCEEDED(hrCom)) CoUninitialize();
            return;
        }

        if (!dopMode_) {
            ASIOIoFormat pcmFmt{};
            pcmFmt.FormatType = kASIOPCMFormat;
            drv->future(kAsioSetIoFormat, &pcmFmt);

            const ASIOSampleRate sr = static_cast<ASIOSampleRate>(reqRate_);
            if (drv->canSampleRate(sr) != ASE_OK || drv->setSampleRate(sr) != ASE_OK) {
                failInitAndNotify("rate");
                if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
                if (SUCCEEDED(hrCom)) CoUninitialize();
                return;
            }
            ASIOChannelInfo ci0{}, ci1{};
            ci0.channel = 0; ci0.isInput = ASIOFalse;
            ci1.channel = 1; ci1.isInput = ASIOFalse;
            if (drv->getChannelInfo(&ci0) != ASE_OK || drv->getChannelInfo(&ci1) != ASE_OK ||
                ci0.type != ci1.type || !isSupportedPcmType(ci0.type)) {
                failInitAndNotify("format");
                if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
                if (SUCCEEDED(hrCom)) CoUninitialize();
                return;
            }
            sampleType_ = ci0.type;
            nativeDsd_.store(false, std::memory_order_release);
            dopPcm_.store(false, std::memory_order_release);
        } else {
            // 1. Natív DSD próba (kAsioSetIoFormat -> kASIODSDFormat + canSampleRate(dopRate * 16))
            ASIOIoFormat dsdFmt{};
            dsdFmt.FormatType = kASIODSDFormat;
            const ASIOSampleRate dsdBitRate = static_cast<ASIOSampleRate>(reqRate_) * 16.0;
            bool configured = false;
            if (drv->future(kAsioSetIoFormat, &dsdFmt) == ASE_SUCCESS &&
                dsdFmt.FormatType == kASIODSDFormat &&
                drv->canSampleRate(dsdBitRate) == ASE_OK &&
                drv->setSampleRate(dsdBitRate) == ASE_OK) {
                ASIOChannelInfo ci0{}, ci1{};
                ci0.channel = 0; ci0.isInput = ASIOFalse;
                ci1.channel = 1; ci1.isInput = ASIOFalse;
                if (drv->getChannelInfo(&ci0) == ASE_OK && drv->getChannelInfo(&ci1) == ASE_OK &&
                    ci0.type == ci1.type &&
                    (ci0.type == ASIOSTDSDInt8LSB1 || ci0.type == ASIOSTDSDInt8MSB1)) {
                    sampleType_ = ci0.type;
                    nativeDsd_.store(true, std::memory_order_release);
                    dopPcm_.store(false, std::memory_order_release);
                    configured = true;
                }
            }
            // 2. Ha a meghajtó nem tud natív DSD-t: DoP PCM-ként (24 bit a 32-ben vagy 24 bit)
            if (!configured) {
                ASIOIoFormat pcmFmt{};
                pcmFmt.FormatType = kASIOPCMFormat;
                drv->future(kAsioSetIoFormat, &pcmFmt);
                const ASIOSampleRate dopSr = static_cast<ASIOSampleRate>(reqRate_);
                if (drv->canSampleRate(dopSr) == ASE_OK && drv->setSampleRate(dopSr) == ASE_OK) {
                    ASIOChannelInfo ci0{}, ci1{};
                    ci0.channel = 0; ci0.isInput = ASIOFalse;
                    ci1.channel = 1; ci1.isInput = ASIOFalse;
                    if (drv->getChannelInfo(&ci0) == ASE_OK && drv->getChannelInfo(&ci1) == ASE_OK &&
                        ci0.type == ci1.type &&
                        (ci0.type == ASIOSTInt32LSB || ci0.type == ASIOSTInt24LSB || ci0.type == ASIOSTInt32LSB24)) {
                        sampleType_ = ci0.type;
                        nativeDsd_.store(false, std::memory_order_release);
                        dopPcm_.store(true, std::memory_order_release);
                        configured = true;
                    }
                }
            }
            if (!configured) {
                failInitAndNotify("format");
                if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
                if (SUCCEEDED(hrCom)) CoUninitialize();
                return;
            }
        }

        long minSz = 0, maxSz = 0, prefSz = 0, gran = 0;
        if (drv->getBufferSize(&minSz, &maxSz, &prefSz, &gran) != ASE_OK) {
            failInitAndNotify("buffer");
            if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
            if (SUCCEEDED(hrCom)) CoUninitialize();
            return;
        }
        long bufSz = 0;
        if (nativeDsd_.load(std::memory_order_relaxed)) {
            bufSz = chooseDsdBufferSize(minSz, maxSz, prefSz, gran);
        } else {
            bufSz = (prefSz > 0) ? prefSz : ((minSz > 0) ? minSz : 512);
        }
        if (bufSz <= 0 || bufSz > (1 << 22)) {
            failInitAndNotify("buffer");
            if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
            if (SUCCEEDED(hrCom)) CoUninitialize();
            return;
        }
        bufferSize_ = bufSz;

        // Előre lefoglaljuk a munkapuffereket a vezérlőszálon, hogy a bufferSwitch alatt 0 foglalás legyen
        tmpFloat_.assign(static_cast<size_t>(bufSz) * 2, 0.0f);
        tmpDop_.assign(static_cast<size_t>(bufSz) * 2, 0);

        std::memset(bufferInfos_, 0, sizeof(bufferInfos_));
        bufferInfos_[0].isInput = ASIOFalse;
        bufferInfos_[0].channelNum = 0;
        bufferInfos_[1].isInput = ASIOFalse;
        bufferInfos_[1].channelNum = 1;

        callbacks_.bufferSwitch = &AsioPlayerOutput::cbBufferSwitch;
        callbacks_.sampleRateDidChange = &AsioPlayerOutput::cbSampleRateDidChange;
        callbacks_.asioMessage = &AsioPlayerOutput::cbAsioMessage;
        callbacks_.bufferSwitchTimeInfo = &AsioPlayerOutput::cbBufferSwitchTimeInfo;

        if (drv->createBuffers(bufferInfos_, 2, bufSz, &callbacks_) != ASE_OK ||
            !bufferInfos_[0].buffers[0] || !bufferInfos_[0].buffers[1] ||
            !bufferInfos_[1].buffers[0] || !bufferInfos_[1].buffers[1]) {
            failInitAndNotify("create_buffers");
            if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
            if (SUCCEEDED(hrCom)) CoUninitialize();
            return;
        }
        buffersCreated = true;
        clearDriverBuffers();

        {
            std::lock_guard<std::mutex> qlk(drvQueryMutex_);
            drv_ = drv;
        }
        isOpen_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(initMutex_);
            initState_ = 1;
        }
        initCv_.notify_all();

        while (!quit_.load(std::memory_order_acquire)) {
            if (resetRequested_.exchange(false, std::memory_order_acq_rel)) {
                invalidateRatesCache();
                setLastError("reset");
                break;
            }
            if (flushReq_.exchange(false, std::memory_order_acq_rel)) {
                std::lock_guard<std::mutex> lk(renderMutex_);
                consumedFrames_.store(0, std::memory_order_release);
                clearDriverBuffers();
            }
            const bool wantActive = active_.load(std::memory_order_acquire);
            if (wantActive && !started) {
                if (drv->start() != ASE_OK) {
                    setLastError("start_failed");
                    break;
                }
                started = true;
                running_.store(true, std::memory_order_release);
            } else if (!wantActive && started) {
                drv->stop();
                started = false;
                running_.store(false, std::memory_order_release);
            }

            if (started) {
                ASIOSamples sp{};
                ASIOTimeStamp ts{};
                const ASIOError pe = drv->getSamplePosition(&sp, &ts);
                if (pe == ASE_NotPresent || pe == ASE_HWMalfunction) {
                    setLastError(pe == ASE_NotPresent ? "device_lost" : "hw_malfunction");
                    break;
                }
            }

            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (wake_) {
                MsgWaitForMultipleObjects(1, &wake_, FALSE, 5, QS_ALLINPUT);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        running_.store(false, std::memory_order_release);
        isOpen_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> qlk(drvQueryMutex_);
            drv_ = nullptr;
        }
        if (drv) {
            if (started) drv->stop();
            if (buffersCreated) drv->disposeBuffers();
            drv->Release();
            drv = nullptr;
        }
        AsioPlayerOutput* self = this;
        s_activeInstance.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        if (SUCCEEDED(hrCom)) CoUninitialize();
    }

    // -----------------------------------------------------------------------
    // Valós idejű ASIO visszahívások (hangszál: 0 foglalás, 0 blokkoló I/O)
    // -----------------------------------------------------------------------
    static void cbBufferSwitch(long doubleBufferIndex, ASIOBool /*directProcess*/) {
        AsioPlayerOutput* self = s_activeInstance.load(std::memory_order_acquire);
        if (self) self->processBufferSwitch(doubleBufferIndex);
    }

    static ASIOTime* cbBufferSwitchTimeInfo(ASIOTime* params, long doubleBufferIndex, ASIOBool /*directProcess*/) {
        AsioPlayerOutput* self = s_activeInstance.load(std::memory_order_acquire);
        if (self) self->processBufferSwitch(doubleBufferIndex);
        return params ? params : &s_asioTime;
    }

    static void cbSampleRateDidChange(ASIOSampleRate /*sRate*/) {}

    static long cbAsioMessage(long selector, long value, void* /*message*/, double* /*opt*/) {
        switch (selector) {
            case kAsioSelectorSupported:
                // A kAsioBufferSizeChange szándékosan nincs a támogatott listában (0L),
                // így a meghajtó a specifikáció szerint kAsioResetRequest-et használ helyette.
                if (value == kAsioSelectorSupported || value == kAsioEngineVersion ||
                    value == kAsioResetRequest || value == kAsioResyncRequest ||
                    value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo) {
                    return 1L;
                }
                return 0L;
            case kAsioEngineVersion:
                return 2L;
            case kAsioResetRequest: {
                AsioPlayerOutput* self = s_activeInstance.load(std::memory_order_acquire);
                if (self) {
                    self->invalidateRatesCache();
                    self->resetCount_.fetch_add(1, std::memory_order_relaxed);
                    self->resetRequested_.store(true, std::memory_order_release);
                    if (self->wake_) SetEvent(self->wake_);
                }
                return 1L;
            }
            case kAsioBufferSizeChange:
                return 0L;
            case kAsioResyncRequest:
            case kAsioLatenciesChanged:
            case kAsioSupportsTimeInfo:
                return 1L;
            default:
                return 0L;
        }
    }

    void processBufferSwitch(long idx) noexcept {
        if ((idx != 0 && idx != 1) || !isOpen_.load(std::memory_order_acquire)) return;
        uint8_t* dstL = static_cast<uint8_t*>(bufferInfos_[0].buffers[idx]);
        uint8_t* dstR = static_cast<uint8_t*>(bufferInfos_[1].buffers[idx]);
        if (!dstL || !dstR) return;

        const size_t n = static_cast<size_t>(bufferSize_);
        std::lock_guard<std::mutex> lk(renderMutex_);

        if (!active_.load(std::memory_order_relaxed) || flushReq_.load(std::memory_order_relaxed)) {
            const size_t bytes = channelBufferBytes(bufferSize_, sampleType_);
            const uint8_t fillByte = nativeDsd_.load(std::memory_order_relaxed) ? dsd::kDsdSilenceByte : 0u;
            std::memset(dstL, fillByte, bytes);
            std::memset(dstR, fillByte, bytes);
            return;
        }

        if (nativeDsd_.load(std::memory_order_relaxed)) {
            const size_t dopFrames = n / 16u;
            int32_t* dop = tmpDop_.data();
            if (renderDop_) {
                renderDop_(dop, dopFrames);
            } else {
                for (size_t i = 0; i < dopFrames * 2; ++i) {
                    dop[i] = dsd::packDopSample(dsd::kDsdSilenceByte, dsd::kDsdSilenceByte, 0x05u);
                }
            }
            const bool lsb1 = (sampleType_ == ASIOSTDSDInt8LSB1);
            for (size_t i = 0; i < dopFrames; ++i) {
                const uint32_t wL = static_cast<uint32_t>(dop[2 * i]);
                const uint32_t wR = static_cast<uint32_t>(dop[2 * i + 1]);
                uint8_t b0L = static_cast<uint8_t>((wL >> 8) & 0xFFu);
                uint8_t b1L = static_cast<uint8_t>(wL & 0xFFu);
                uint8_t b0R = static_cast<uint8_t>((wR >> 8) & 0xFFu);
                uint8_t b1R = static_cast<uint8_t>(wR & 0xFFu);
                if (lsb1) {
                    b0L = dsd::reverseBits8(b0L);
                    b1L = dsd::reverseBits8(b1L);
                    b0R = dsd::reverseBits8(b0R);
                    b1R = dsd::reverseBits8(b1R);
                }
                dstL[2 * i]     = b0L;
                dstL[2 * i + 1] = b1L;
                dstR[2 * i]     = b0R;
                dstR[2 * i + 1] = b1R;
            }
            if (drv_) drv_->outputReady();
            consumedFrames_.fetch_add(dopFrames, std::memory_order_release);
            return;
        }

        if (dopPcm_.load(std::memory_order_relaxed)) {
            int32_t* dop = tmpDop_.data();
            if (renderDop_) {
                renderDop_(dop, n);
            } else {
                for (size_t i = 0; i < n; ++i) {
                    const uint8_t marker = (i & 1u) ? 0xFAu : 0x05u;
                    const int32_t w = dsd::packDopSample(dsd::kDsdSilenceByte, dsd::kDsdSilenceByte, marker);
                    dop[2 * i] = w;
                    dop[2 * i + 1] = w;
                }
            }
            if (sampleType_ == ASIOSTInt32LSB) {
                for (size_t i = 0; i < n; ++i) {
                    const uint32_t wL = (static_cast<uint32_t>(dop[2 * i]) & 0x00FFFFFFu) << 8;
                    const uint32_t wR = (static_cast<uint32_t>(dop[2 * i + 1]) & 0x00FFFFFFu) << 8;
                    std::memcpy(dstL + i * 4, &wL, 4);
                    std::memcpy(dstR + i * 4, &wR, 4);
                }
            } else if (sampleType_ == ASIOSTInt24LSB) {
                for (size_t i = 0; i < n; ++i) {
                    const uint32_t uL = static_cast<uint32_t>(dop[2 * i]);
                    dstL[i * 3]     = static_cast<uint8_t>(uL & 0xFFu);
                    dstL[i * 3 + 1] = static_cast<uint8_t>((uL >> 8) & 0xFFu);
                    dstL[i * 3 + 2] = static_cast<uint8_t>((uL >> 16) & 0xFFu);
                    const uint32_t uR = static_cast<uint32_t>(dop[2 * i + 1]);
                    dstR[i * 3]     = static_cast<uint8_t>(uR & 0xFFu);
                    dstR[i * 3 + 1] = static_cast<uint8_t>((uR >> 8) & 0xFFu);
                    dstR[i * 3 + 2] = static_cast<uint8_t>((uR >> 16) & 0xFFu);
                }
            } else if (sampleType_ == ASIOSTInt32LSB24) {
                for (size_t i = 0; i < n; ++i) {
                    const uint32_t wL = static_cast<uint32_t>(dop[2 * i]) & 0x00FFFFFFu;
                    const uint32_t wR = static_cast<uint32_t>(dop[2 * i + 1]) & 0x00FFFFFFu;
                    std::memcpy(dstL + i * 4, &wL, 4);
                    std::memcpy(dstR + i * 4, &wR, 4);
                }
            }
        } else {
            float* st = tmpFloat_.data();
            if (render_) {
                render_(st, n);
            } else {
                std::memset(st, 0, n * 2 * sizeof(float));
            }
            if (sampleType_ == ASIOSTInt32LSB) {
                for (size_t i = 0; i < n; ++i) {
                    pcm::floatToInt(&st[2 * i],     dstL + i * 4, 1, 32, 32);
                    pcm::floatToInt(&st[2 * i + 1], dstR + i * 4, 1, 32, 32);
                }
            } else if (sampleType_ == ASIOSTInt24LSB) {
                for (size_t i = 0; i < n; ++i) {
                    pcm::floatToInt(&st[2 * i],     dstL + i * 3, 1, 24, 24);
                    pcm::floatToInt(&st[2 * i + 1], dstR + i * 3, 1, 24, 24);
                }
            } else if (sampleType_ == ASIOSTInt16LSB) {
                for (size_t i = 0; i < n; ++i) {
                    pcm::floatToInt(&st[2 * i],     dstL + i * 2, 1, 16, 16);
                    pcm::floatToInt(&st[2 * i + 1], dstR + i * 2, 1, 16, 16);
                }
            } else if (sampleType_ == ASIOSTFloat32LSB) {
                for (size_t i = 0; i < n; ++i) {
                    std::memcpy(dstL + i * 4, &st[2 * i],     4);
                    std::memcpy(dstR + i * 4, &st[2 * i + 1], 4);
                }
            } else if (sampleType_ == ASIOSTFloat64LSB) {
                for (size_t i = 0; i < n; ++i) {
                    const double dL = static_cast<double>(st[2 * i]);
                    const double dR = static_cast<double>(st[2 * i + 1]);
                    std::memcpy(dstL + i * 8, &dL, 8);
                    std::memcpy(dstR + i * 8, &dR, 8);
                }
            }
        }

        if (drv_) drv_->outputReady();
        consumedFrames_.fetch_add(n, std::memory_order_release);
    }

    std::string driverIdOrName_;
    DriverFactory factory_;

    uint32_t reqRate_ = 48000;
    bool dopMode_ = false;
    long bufferSize_ = 0;
    ASIOSampleType sampleType_ = ASIOSTInt32LSB;
    ASIOBufferInfo bufferInfos_[2]{};
    ASIOCallbacks callbacks_{};

    std::vector<float> tmpFloat_;
    std::vector<int32_t> tmpDop_;

    RenderFn render_;
    RenderDopFn renderDop_;
    std::mutex renderMutex_;

    mutable std::mutex drvQueryMutex_;
    IASIO* drv_ = nullptr;

    mutable std::mutex ratesCacheMutex_;
    mutable std::vector<uint32_t> cachedRates_;
    mutable std::string cachedDriverId_;
    mutable bool ratesCacheValid_ = false;

    std::thread thread_;
    HANDLE wake_ = nullptr;
    std::atomic<bool> active_{false};
    std::atomic<bool> quit_{false};
    std::atomic<bool> flushReq_{false};
    std::atomic<bool> resetRequested_{false};
    std::atomic<bool> isOpen_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> nativeDsd_{false};
    std::atomic<bool> dopPcm_{false};
    std::atomic<uint64_t> consumedFrames_{0};
    std::atomic<uint32_t> resetCount_{0};

    std::mutex initMutex_;
    std::condition_variable initCv_;
    int initState_ = 0;
    std::string initError_;
    std::string initDriverMsg_;

    mutable std::mutex errMutex_;
    std::string lastError_;
    std::string lastDriverMessage_;
};

} // namespace ee_analyzer::player
