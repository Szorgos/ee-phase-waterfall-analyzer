#pragma once
/**
 * @file player_output_win.hpp
 * @brief A beépített lejátszó Windowsos kimenete: WASAPI (megosztott / kizárólagos) vagy ASIO, a beállítás szerint.
 *
 * A motor egyetlen kimenetet ad a lejátszónak; ez a váltó dönti el megnyitáskor, hogy a hangfolyam a WASAPI-n vagy a
 * DAC gyártójának ASIO-meghajtóján át szól. ASIO-n a lejátszó egyedül használja az eszközt (mint kizárólagos
 * WASAPI-nál): a Windows keverője és az Equalizer APO kimarad, a DSP-lánc a motorban fut (ExclusiveDsp), a
 * hangkártya a meghajtó saját pufferméretével kéri a blokkokat.
 *
 * Ha az ASIO nem indul el (nincs meghajtó, foglalt, nem támogatott mintavétel vagy formátum), a kimenet a WASAPI-ra
 * lép vissza, a zene szól tovább, és az állapot (statusJson, "asio" objektum) megmondja, miért. Natív DSD-nél az
 * ASIO-meghajtó elutasítása után nincs WASAPI-DoP kísérlet: a lejátszó PCM-re alakít, és azt is ASIO-n próbálja.
 *
 * A WASAPI-oldal sablonparaméter, hogy a teszt hangeszköz nélkül, hamis WASAPI-kimenettel is futhasson.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "exclusive_dsp.hpp"
#include "file_player.hpp"
#include "player_output_asio.hpp"

namespace ee_analyzer::player {

template <class W>
class BasicWinPlayerOutput : public PlayerOutput {
public:
    enum class Active { None, Wasapi, Asio };

    explicit BasicWinPlayerOutput(std::unique_ptr<W> wasapi = std::make_unique<W>(),
                                  std::unique_ptr<AsioPlayerOutput> asio = std::make_unique<AsioPlayerOutput>())
        : wasapi_(std::move(wasapi)), asio_(std::move(asio)) {}
    ~BasicWinPlayerOutput() override { close(); }

    BasicWinPlayerOutput(const BasicWinPlayerOutput&) = delete;
    BasicWinPlayerOutput& operator=(const BasicWinPlayerOutput&) = delete;

    W& wasapi() { return *wasapi_; }
    AsioPlayerOutput& asio() { return *asio_; }

    // ---- a motor felé: ugyanaz a felület, mint a WASAPI-kimeneté ----
    void setExclusiveDsp(ExclusiveDsp* dsp) {
        dsp_ = dsp;
        wasapi_->setExclusiveDsp(dsp);
    }
    void setExclusive(bool on) { wasapi_->setExclusive(on); }
    bool wantExclusive() const { return wasapi_->wantExclusive(); }
    void setWantDop(bool on) { wasapi_->setWantDop(on); }
    bool wantDop() const { return wasapi_->wantDop(); }

    /**
     * Az ASIO kérése és a meghajtó (CLSID, registry-név vagy leírás; üres: az első telepített). A következő
     * megnyitástól érvényes; szóló számnál a motor a FilePlayer::reopenOutput()-tal nyittatja újra.
     */
    void setAsio(bool on, const std::string& driver) {
        std::lock_guard<std::mutex> lk(cfgMutex_);
        wantAsio_ = on;
        driver_ = driver;
    }
    bool wantAsio() const {
        std::lock_guard<std::mutex> lk(cfgMutex_);
        return wantAsio_;
    }
    std::string asioDriverWanted() const {
        std::lock_guard<std::mutex> lk(cfgMutex_);
        return driver_;
    }
    /** Most ASIO-n szól-e (nyitott ASIO-hangfolyam). */
    bool asioActive() const { return active_.load() == Active::Asio && asio_->isOpen(); }

    /** Kizárólagos-e a kimenet (kizárólagos WASAPI vagy ASIO): ilyenkor a DSP-lánc a motorban fut. */
    bool exclusiveActive() const {
        return active_.load() == Active::Asio ? asio_->isOpen() : wasapi_->exclusiveActive();
    }
    /** Natív DSD szól-e (WASAPI DoP, ASIO natív DSD vagy ASIO DoP PCM-ben). */
    bool dopActive() const {
        if (active_.load() == Active::Asio) return asio_->isNativeDsdActive() || asio_->isDopPcmActive();
        return wasapi_->dopActive();
    }
    uint32_t currentRate() const { return active_.load() == Active::Asio ? rate_.load() : wasapi_->currentRate(); }
    /** Bitpontos-e a kimenet formátumára alakítás (ASIO: lebegőpontos, 32 és 24 bites egész igen; 16 bites nem). */
    bool quantExact() const {
        if (active_.load() == Active::Asio) {
            const ASIOSampleType t = asio_->sampleType();
            return t != ASIOSTInt16LSB;
        }
        return wasapi_->quantExact();
    }

    /** A jelút adatai a felületnek (a "path" objektum mezői, vessző nélkül kezdve), az "asio" objektummal együtt. */
    std::string statusJson(double dspMs) const {
        const Active act = active_.load();
        std::string asioErr, asioDetail, drvName;
        bool want = false;
        {
            std::lock_guard<std::mutex> lk(cfgMutex_);
            want = wantAsio_;
            asioErr = asioError_;
            asioDetail = asioDetail_;
            drvName = driverName_;
        }
        if (act == Active::Asio && !asio_->isOpen() && asioErr.empty()) asioErr = asio_->lastError();   // magától leállt
        if (act == Active::Asio && asio_->isOpen()) {
            const uint32_t rate = rate_.load() ? rate_.load() : 48000;
            const long buf = asio_->bufferSize();
            const bool dsdNative = asio_->isNativeDsdActive();
            // natív DSD-ben a puffer DSD-bitben értendő: az időtartamhoz a DoP-keretráta (rate) 16 bitje tartozik
            const double frames = dsdNative ? static_cast<double>(buf) / 16.0 : static_cast<double>(buf);
            const double bufMs = 1000.0 * frames / rate;
            const double totalMs = 2.0 * bufMs + dspMs;
            const std::string fmt = formatName(asio_->sampleType());
            char b[900];
            std::snprintf(b, sizeof(b),
                          "\"quantum\":%ld,\"bufReq\":0,\"bufMs\":%.2f,\"totalMs\":%.1f,\"load\":%.1f,\"peak\":%.1f,"
                          "\"overruns\":%u,\"exclusive\":{\"want\":false,\"active\":false,\"fallback\":\"\",\"hr\":\"0x00000000\","
                          "\"format\":\"\",\"exact\":%s},\"asio\":{\"want\":true,\"active\":true,\"driver\":\"%s\","
                          "\"error\":\"\",\"detail\":\"\",\"format\":\"%s\",\"buffer\":%ld,\"nativeDsd\":%s,\"dopPcm\":%s}",
                          buf, bufMs, totalMs, 100.0 * loadAvg_.load(), 100.0 * loadPeak_.exchange(0.0f), overruns_.load(),
                          quantExact() ? "true" : "false", audioopt::jsonStringBody(drvName).c_str(), fmt.c_str(), buf,
                          dsdNative ? "true" : "false", asio_->isDopPcmActive() ? "true" : "false");
            return b;
        }
        std::string s = wasapi_->statusJson(dspMs);
        if (want || act == Active::Asio) {
            s += ",\"asio\":{\"want\":true,\"active\":false,\"driver\":\"" + audioopt::jsonStringBody(drvName) +
                 "\",\"error\":\"" + audioopt::jsonStringBody(asioErr.empty() ? "not_open" : asioErr) +
                 "\",\"detail\":\"" + audioopt::jsonStringBody(asioDetail) + "\"}";
        }
        return s;
    }

    /**
     * Az ASIO-hangfolyam magától leállt (a meghajtó resetet kért, vagy az eszköz eltűnt): a motor ilyenkor a
     * FilePlayer::reopenOutput()-tal nyittatja újra a szóló számot. Egyszer jelez (acknowledgeAsioStop után nem).
     */
    bool asioNeedsReopen() const {
        return active_.load() == Active::Asio && !asio_->isOpen() && !stopAcked_.load();
    }
    void acknowledgeAsioStop() {
        stopAcked_ = true;
        noteAsioResult(asio_->lastError(), asio_->lastDriverMessage());
    }

    /** Az ASIO legutóbbi hibakódja (üres: nincs; pl. "no_driver", "busy", "format", "reset", "device_lost"). */
    std::string asioError() const {
        std::lock_guard<std::mutex> lk(cfgMutex_);
        return asioError_;
    }

    // ---- PlayerOutput ----
    bool open(uint32_t sampleRate, RenderFn render, std::string* error) override {
        close();
        if (tryAsio()) {
            auto wrapped = [this, r = render](float* s, size_t n) {
                const auto t0 = std::chrono::steady_clock::now();
                if (r) r(s, n);
                else std::fill(s, s + n * 2, 0.0f);
                if (dsp_) dsp_->process(s, n);
                noteLoad(t0, n);
            };
            std::string e;
            if (asio_->open(sampleRate, wrapped, &e)) {
                rate_ = sampleRate;
                loadAvg_ = 0.0f;
                loadPeak_ = 0.0f;
                overruns_ = 0;
                if (dsp_) dsp_->prepare(sampleRate, static_cast<uint32_t>(asio_->bufferSize()), ditherBitsFor(asio_->sampleType()));
                stopAcked_ = false;
                active_ = Active::Asio;
                noteAsioResult({}, {});
                std::cout << "[Lejátszó] ASIO: " << formatName(asio_->sampleType()) << ", " << sampleRate << " Hz, "
                          << asio_->bufferSize() << " keret periódusonként.\n";
                return true;
            }
            noteAsioResult(e, asio_->lastDriverMessage());
            std::cout << "[Lejátszó] Az ASIO nem indult el (" << e << "); WASAPI-n szól.\n";
        }
        active_ = Active::Wasapi;
        if (wasapi_->open(sampleRate, std::move(render), error)) return true;
        active_ = Active::None;
        return false;
    }

    bool openDop(uint32_t dopSampleRate, RenderDopFn render, std::string* error) override {
        close();
        if (tryAsio()) {
            std::string e;
            if (asio_->openDop(dopSampleRate, render, &e)) {
                rate_ = dopSampleRate;
                stopAcked_ = false;
                active_ = Active::Asio;
                noteAsioResult({}, {});
                std::cout << "[Lejátszó] ASIO, natív DSD: " << (asio_->isNativeDsdActive() ? "natív" : "DoP PCM-ben")
                          << ", " << dopSampleRate << " Hz DoP-keretráta.\n";
                return true;
            }
            noteAsioResult(e, asio_->lastDriverMessage());
            // ASIO-t kértek: a WASAPI-DoP-t nem próbáljuk, a lejátszó PCM-re alakít (és azt ASIO-n nyitja)
            if (error) *error = e.empty() ? "format" : e;
            return false;
        }
        active_ = Active::Wasapi;
        if (wasapi_->openDop(dopSampleRate, std::move(render), error)) return true;
        active_ = Active::None;
        return false;
    }

    void close() override {
        asio_->close();
        wasapi_->close();
        active_ = Active::None;
    }
    void setActive(bool active) override {
        switch (active_.load()) {
        case Active::Asio: asio_->setActive(active); break;
        case Active::Wasapi: wasapi_->setActive(active); break;
        default: break;
        }
    }
    // a lock() / unlock() mindkét kimenet render-zárát fogja (rögzített sorrendben), így a nyitás közbeni váltás sem
    // okozhat zár nélküli tekerést
    void lock() override {
        wasapi_->lock();
        asio_->lock();
    }
    void unlock() override {
        asio_->unlock();
        wasapi_->unlock();
    }
    void flush() override {
        switch (active_.load()) {
        case Active::Asio: asio_->flush(); break;
        case Active::Wasapi: wasapi_->flush(); break;
        default: break;
        }
    }
    uint64_t consumedFrames() const override {
        switch (active_.load()) {
        case Active::Asio: return asio_->consumedFrames();
        case Active::Wasapi: return wasapi_->consumedFrames();
        default: return 0;
        }
    }
    bool settlesOnRateChange() const override {
        return active_.load() == Active::Asio ? true : wasapi_->settlesOnRateChange();
    }
    std::vector<uint32_t> supportedRates() const override {
        if (wantAsio()) {
            syncDriver();
            std::vector<uint32_t> r = asio_->supportedRates();
            if (!r.empty()) return r;
        }
        return wasapi_->supportedRates();
    }
    uint32_t actualRate() const override {
        switch (active_.load()) {
        case Active::Asio: return asio_->actualRate();
        case Active::Wasapi: return wasapi_->actualRate();
        default: return 0;
        }
    }

    /** A kimenet bitmélysége a lánc ditheréhez (mint a WASAPI-nál: 16 vagy 24; lebegőpontosnál 0). */
    static int ditherBitsFor(ASIOSampleType t) {
        switch (t) {
        case ASIOSTInt16LSB: return 16;
        case ASIOSTInt24LSB:
        case ASIOSTInt32LSB: return 24;
        default: return 0;
        }
    }
    static std::string formatName(ASIOSampleType t) {
        switch (t) {
        case ASIOSTInt16LSB: return "16 bit";
        case ASIOSTInt24LSB: return "24 bit";
        case ASIOSTInt32LSB: return "32 bit";
        case ASIOSTInt32LSB24: return "24/32 bit";
        case ASIOSTFloat32LSB: return "32 bit float";
        case ASIOSTFloat64LSB: return "64 bit float";
        case ASIOSTDSDInt8LSB1: return "DSD (LSB first)";
        case ASIOSTDSDInt8MSB1: return "DSD (MSB first)";
        default: return "?";
        }
    }

private:
    /** Kell-e ASIO-n próbálkozni; ha igen, a meghajtót is beállítja (zárt állapotban). */
    bool tryAsio() {
        std::string drv;
        {
            std::lock_guard<std::mutex> lk(cfgMutex_);
            if (!wantAsio_) return false;
            drv = driver_;
        }
        if (!dsp_) {
            noteAsioResult("no_dsp", {});
            return false;
        }
        syncDriver();
        {
            std::lock_guard<std::mutex> lk(cfgMutex_);
            driverName_ = resolveDriverName(drv);
        }
        return true;
    }
    /** A kért meghajtó átadása az ASIO-kimenetnek (csak ha változott: a setDriver zár és a gyorsítótárat üríti). */
    void syncDriver() const {
        if (asio_->isOpen()) return;   // nyitott kimeneten nem váltunk (a motor újranyittatja; akkor zárt lesz)
        std::string drv;
        {
            std::lock_guard<std::mutex> lk(cfgMutex_);
            drv = driver_;
            if (driverApplied_ && drv == appliedDriver_) return;
            appliedDriver_ = drv;
            driverApplied_ = true;
        }
        asio_->setDriver(drv);
    }
    static std::string resolveDriverName(const std::string& want) {
        const auto list = AsioPlayerOutput::listDrivers();
        if (want.empty()) return list.empty() ? std::string() : list.front().description;
        for (const auto& d : list)
            if (d.clsid == want || d.keyName == want || d.description == want) return d.description;
        return want;
    }
    void noteAsioResult(const std::string& err, const std::string& detail) {
        std::lock_guard<std::mutex> lk(cfgMutex_);
        asioError_ = err;
        asioDetail_ = detail;
    }
    void noteLoad(std::chrono::steady_clock::time_point t0, size_t frames) {
        const uint32_t rate = rate_.load(std::memory_order_relaxed);
        if (frames == 0 || rate == 0) return;
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const float load = static_cast<float>(dt * rate / frames);
        const float avg = loadAvg_.load(std::memory_order_relaxed);
        loadAvg_.store(avg + 0.05f * (load - avg), std::memory_order_relaxed);
        if (load > loadPeak_.load(std::memory_order_relaxed)) loadPeak_.store(load, std::memory_order_relaxed);
        if (load > 1.0f) overruns_.fetch_add(1, std::memory_order_relaxed);
    }

    std::unique_ptr<W> wasapi_;
    std::unique_ptr<AsioPlayerOutput> asio_;
    ExclusiveDsp* dsp_ = nullptr;
    std::atomic<Active> active_{Active::None};
    std::atomic<uint32_t> rate_{0};
    mutable std::atomic<float> loadAvg_{0.0f}, loadPeak_{0.0f};
    std::atomic<uint32_t> overruns_{0};
    std::atomic<bool> stopAcked_{false};

    mutable std::mutex cfgMutex_;
    bool wantAsio_ = false;
    std::string driver_;
    mutable std::string appliedDriver_;
    mutable bool driverApplied_ = false;
    std::string driverName_;
    std::string asioError_;
    std::string asioDetail_;
};

} // namespace ee_analyzer::player
