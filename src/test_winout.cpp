// A Windows-motor kimenet-váltójának tesztje (C rész bekötése): WASAPI ↔ ASIO, DSP a motorban, visszalépés, DoP,
// a beállítás JSON-ja és a lejátszó újranyitása. Hangeszköz nélkül fut: hamis WASAPI-kimenet és Claude saját,
// a specifikációból (Steinberg ASIO SDK 2.3, common/asio.h) írt hamis ASIO-meghajtója.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "audio_options.hpp"
#include "exclusive_dsp.hpp"
#include "file_player.hpp"
#include "mini_json.hpp"
#include "player_output_win.hpp"
#include "apo_detect.hpp"
#include "wasapi_loopback.hpp"
#include "watchdog.hpp"

// A hangeszköz alapformátumának olvasója (1b, P3). A befagyasztott tesztcommitban még nincs meg: akkor a
// helyettesítő üres szöveget ad, és a [12] csoport elbukik.
#if __has_include("device_format.hpp")
#include "device_format.hpp"
#else
namespace aq::devfmt {
inline std::string readRegistry(HKEY, const std::wstring&) { return {}; }
}
#endif

using namespace ee_analyzer;
using namespace ee_analyzer::player;

static int g_fail = 0, g_ok = 0;
#define CHECK(cond, ...)                                   \
    do {                                                   \
        if (cond) { ++g_ok; std::printf("  [OK]   "); }    \
        else { ++g_fail; std::printf("  [HIBA] "); }       \
        std::printf(__VA_ARGS__);                          \
        std::printf("\n");                                 \
    } while (0)

// ---------------------------------------------------------------------------------------------------------------
// Hamis WASAPI-kimenet (a motor által használt felülettel)
// ---------------------------------------------------------------------------------------------------------------
struct FakeWasapi : PlayerOutput {
    int opens = 0, dopOpens = 0, closes = 0;
    bool open_ = false, active = false, wantExcl = false, wantDop_ = false;
    uint32_t rate = 0;
    RenderFn render;
    ExclusiveDsp* dsp = nullptr;
    void setExclusiveDsp(ExclusiveDsp* d) { dsp = d; }
    void setExclusive(bool on) { wantExcl = on; }
    bool wantExclusive() const { return wantExcl; }
    bool exclusiveActive() const { return open_ && wantExcl; }
    void setWantDop(bool on) { wantDop_ = on; }
    bool wantDop() const { return wantDop_; }
    bool dopActive() const { return false; }
    uint32_t currentRate() const { return rate; }
    bool quantExact() const { return true; }
    std::string statusJson(double) const {
        return "\"quantum\":480,\"bufReq\":0,\"bufMs\":10.00,\"totalMs\":10.0,\"load\":0.0,\"peak\":0.0,\"overruns\":0,"
               "\"exclusive\":{\"want\":false,\"active\":false,\"fallback\":\"\",\"hr\":\"0x00000000\",\"format\":\"\",\"exact\":true}";
    }
    bool open(uint32_t r, RenderFn fn, std::string*) override { ++opens; open_ = true; rate = r; render = std::move(fn); return true; }
    bool openDop(uint32_t, RenderDopFn, std::string* e) override { ++dopOpens; if (e) *e = "notAllowed"; return false; }
    void close() override { if (open_) ++closes; open_ = false; active = false; }
    void setActive(bool a) override { active = a; }
    void lock() override {}
    void unlock() override {}
    uint64_t consumedFrames() const override { return 7; }
    std::vector<uint32_t> supportedRates() const override { return { 44100, 48000 }; }
    uint32_t actualRate() const override { return open_ ? rate : 0; }
};

// ---------------------------------------------------------------------------------------------------------------
// Claude saját hamis ASIO-meghajtója (független a test_asio.cpp-től)
// ---------------------------------------------------------------------------------------------------------------
struct DrvState {
    ASIOSampleType pcmType = ASIOSTFloat32LSB;
    ASIOSampleType dsdType = ASIOSTDSDInt8MSB1;
    bool dsdOk = false;
    std::set<long> pcmRates{ 44100, 96000 };
    std::set<long> dsdRates{ 2822400 };
    long pcmBuf = 256;
    long dsdBuf = 4096;                       // DSD-ben bitben (a specifikáció szerint)
    long ioFormat = kASIOPCMFormat;
    double rate = 44100;
    ASIOCallbacks cb{};
    bool haveCb = false;
    std::vector<uint8_t> buf[2][2];
    size_t payload = 0;
    int inits = 0;
    static constexpr size_t kGuard = 32;
    size_t bytesFor(long frames) const {
        if (ioFormat == kASIODSDFormat) return static_cast<size_t>(frames) / 8u;   // 8 DSD-minta egy bájtban
        switch (pcmType) {
        case ASIOSTInt16LSB: return frames * 2u;
        case ASIOSTInt24LSB: return frames * 3u;
        case ASIOSTFloat64LSB: return frames * 8u;
        default: return frames * 4u;
        }
    }
    bool guardsOk() const {
        for (auto& c : buf)
            for (auto& b : c)
                for (size_t i = payload; i < b.size(); ++i)
                    if (b[i] != 0xC3) return false;
        return true;
    }
};

class MyFakeAsio : public IASIO {
public:
    explicit MyFakeAsio(std::shared_ptr<DrvState> s) : s_(std::move(s)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** p) override { *p = this; AddRef(); return S_OK; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG r = --ref_; if (!r) delete this; return r; }
    ASIOBool init(void*) override { ++s_->inits; return ASIOTrue; }
    void getDriverName(char* n) override { std::strcpy(n, "ClaudeFake"); }
    long getDriverVersion() override { return 1; }
    void getErrorMessage(char* s) override { std::strcpy(s, ""); }
    ASIOError start() override { return ASE_OK; }
    ASIOError stop() override { return ASE_OK; }
    ASIOError getChannels(long* i, long* o) override { *i = 0; *o = 2; return ASE_OK; }
    ASIOError getLatencies(long* i, long* o) override { *i = *o = 0; return ASE_OK; }
    ASIOError getBufferSize(long* mn, long* mx, long* pr, long* gr) override {
        const long b = s_->ioFormat == kASIODSDFormat ? s_->dsdBuf : s_->pcmBuf;
        *mn = *mx = *pr = b;
        *gr = 0;
        return ASE_OK;
    }
    ASIOError canSampleRate(ASIOSampleRate r) override {
        const long v = static_cast<long>(std::llround(r));
        return (s_->ioFormat == kASIODSDFormat ? s_->dsdRates.count(v) : s_->pcmRates.count(v)) ? ASE_OK : ASE_NoClock;
    }
    ASIOError getSampleRate(ASIOSampleRate* r) override { *r = s_->rate; return ASE_OK; }
    ASIOError setSampleRate(ASIOSampleRate r) override {
        if (canSampleRate(r) != ASE_OK) return ASE_NoClock;
        s_->rate = r;
        return ASE_OK;
    }
    ASIOError getClockSources(ASIOClockSource*, long* n) override { *n = 0; return ASE_OK; }
    ASIOError setClockSource(long) override { return ASE_OK; }
    ASIOError getSamplePosition(ASIOSamples* sp, ASIOTimeStamp* ts) override { *sp = {}; *ts = {}; return ASE_OK; }
    ASIOError getChannelInfo(ASIOChannelInfo* ci) override {
        ci->isActive = ASIOTrue;
        ci->channelGroup = 0;
        ci->type = s_->ioFormat == kASIODSDFormat ? s_->dsdType : s_->pcmType;
        std::strcpy(ci->name, "out");
        return ASE_OK;
    }
    ASIOError createBuffers(ASIOBufferInfo* bi, long n, long size, ASIOCallbacks* cb) override {
        if (n != 2) return ASE_InvalidParameter;
        s_->payload = s_->bytesFor(size);
        for (int c = 0; c < 2; ++c)
            for (int b = 0; b < 2; ++b) {
                s_->buf[c][b].assign(s_->payload + DrvState::kGuard, 0xC3);
                bi[c].buffers[b] = s_->buf[c][b].data();
            }
        s_->cb = *cb;
        s_->haveCb = true;
        return ASE_OK;
    }
    ASIOError disposeBuffers() override { s_->haveCb = false; return ASE_OK; }
    ASIOError controlPanel() override { return ASE_NotPresent; }
    ASIOError future(long sel, void* opt) override {
        if (sel == kAsioSetIoFormat && opt) {
            auto* f = static_cast<ASIOIoFormat*>(opt);
            if (f->FormatType == kASIODSDFormat && !s_->dsdOk) { f->FormatType = kASIOFormatInvalid; return ASE_InvalidMode; }
            s_->ioFormat = f->FormatType;
            return ASE_SUCCESS;
        }
        return ASE_InvalidParameter;
    }
    ASIOError outputReady() override { return ASE_OK; }
private:
    std::shared_ptr<DrvState> s_;
    ULONG ref_ = 1;
};

/** A DSP a motorban: a teszt felezi a jelet, és rögzíti a prepare paramétereit. */
struct HalfDsp : ExclusiveDsp {
    uint32_t rate = 0, block = 0;
    int dither = -1, processed = 0;
    void prepare(uint32_t r, uint32_t b, int d) override { rate = r; block = b; dither = d; }
    void process(float* s, size_t n) override { for (size_t i = 0; i < n * 2; ++i) s[i] *= 0.5f; ++processed; }
};

using TestOut = BasicWinPlayerOutput<FakeWasapi>;

static std::unique_ptr<TestOut> makeOut(std::shared_ptr<DrvState> st, bool driverPresent = true) {
    auto fw = std::make_unique<FakeWasapi>();
    auto factory = [st, driverPresent](std::string* err) -> IASIO* {
        if (!driverPresent) { if (err) *err = "no_driver"; return nullptr; }
        return new MyFakeAsio(st);
    };
    return std::make_unique<TestOut>(std::move(fw), std::make_unique<AsioPlayerOutput>(std::string(), factory));
}

static bool waitFor(const std::function<bool()>& f) {
    for (int i = 0; i < 500; ++i) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    return false;
}

static bool parseStatus(const std::string& fields, json::Value& v) { return json::parse("{" + fields + "}", v); }

int main() {
    std::printf("========================================================\n");
    std::printf(" Aqulify – Windows kimenet-váltó (WASAPI / ASIO) teszt\n");
    std::printf("========================================================\n");

    std::printf("\n[1] ASIO kikapcsolva: a WASAPI szól\n");
    {
        auto st = std::make_shared<DrvState>();
        auto out = makeOut(st);
        HalfDsp dsp;
        out->setExclusiveDsp(&dsp);
        std::string err;
        CHECK(out->open(44100, [](float* s, size_t n) { std::fill(s, s + n * 2, 0.1f); }, &err) && out->wasapi().opens == 1,
              "WASAPI-n nyílt meg (WASAPI-megnyitások: %d)", out->wasapi().opens);
        CHECK(!out->asioActive() && st->inits == 0, "az ASIO-meghajtó nem töltődött be (init: %d)", st->inits);
        json::Value v;
        CHECK(parseStatus(out->statusJson(0.0), v) && !v.get("asio"), "az állapotban nincs \"asio\" objektum, ha nem kérték");
        CHECK(out->consumedFrames() == 7 && out->supportedRates().size() == 2, "a WASAPI-kimenet adatai jönnek át");
        out->close();
    }

    std::printf("\n[2] ASIO bekapcsolva, működő meghajtó (32 bit float): a DSP a motorban fut\n");
    {
        auto st = std::make_shared<DrvState>();
        auto out = makeOut(st);
        HalfDsp dsp;
        out->setExclusiveDsp(&dsp);
        out->setAsio(true, "");
        std::string err;
        const bool ok = out->open(96000, [](float* s, size_t n) { std::fill(s, s + n * 2, 0.4f); }, &err);
        CHECK(ok && out->asioActive() && out->wasapi().opens == 0, "ASIO-n nyílt meg, a WASAPI nem (err='%s')", err.c_str());
        CHECK(dsp.rate == 96000 && dsp.block == 256 && dsp.dither == 0,
              "a DSP prepare: %u Hz, %u keret, dither %d (várt: 96000, 256, 0)", dsp.rate, dsp.block, dsp.dither);
        CHECK(out->exclusiveActive() && !out->dopActive() && out->currentRate() == 96000 && out->settlesOnRateChange(),
              "kizárólagosként viselkedik (a DSP-lánc és a digitális hangerő a motorban)");
        out->setActive(true);
        waitFor([&] { return st->haveCb; });
        st->cb.bufferSwitch(0, ASIOTrue);
        float l0 = 0, r0 = 0;
        std::memcpy(&l0, st->buf[0][0].data(), 4);
        std::memcpy(&r0, st->buf[1][0].data(), 4);
        CHECK(l0 == 0.2f && r0 == 0.2f && dsp.processed == 1, "a pufferbe a DSP-n átment jel került (0,4 × 0,5 = %.3f / %.3f)", l0, r0);
        CHECK(out->consumedFrames() == 256, "consumedFrames = %llu (várt 256)", static_cast<unsigned long long>(out->consumedFrames()));
        CHECK(st->guardsOk(), "a meghajtó pufferei mögötti őrbájtok sértetlenek");
        json::Value v;
        const std::string s = out->statusJson(1.5);
        const bool parsed = parseStatus(s, v);
        const json::Value* a = parsed ? v.get("asio") : nullptr;
        CHECK(a && a->get("active") && a->get("active")->b && a->get("format") && a->get("format")->str == "32 bit float" &&
              a->get("buffer") && a->get("buffer")->num == 256 && v.get("quantum") && v.get("quantum")->num == 256,
              "állapot: asio.active, formátum, puffer (%s)", s.substr(0, 160).c_str());
        out->close();
        CHECK(!out->asioActive() && out->consumedFrames() == 0, "zárás után nincs aktív kimenet");
    }

    std::printf("\n[3] ASIO 24 bites egész: a lánc dithere 24 bit, a bájtok a specifikáció szerint\n");
    {
        auto st = std::make_shared<DrvState>();
        st->pcmType = ASIOSTInt24LSB;
        auto out = makeOut(st);
        HalfDsp dsp;
        out->setExclusiveDsp(&dsp);
        out->setAsio(true, "");
        std::string err;
        out->open(44100, [](float* s, size_t n) { std::fill(s, s + n * 2, 0.5f); }, &err);
        out->setActive(true);
        waitFor([&] { return st->haveCb; });
        st->cb.bufferSwitch(1, ASIOTrue);
        const uint8_t* b = st->buf[0][1].data();
        // 0,5 × 0,5 = 0,25 → 0,25 · 2^23 = 2097152 = 0x200000 → kis-endián 00 00 20
        CHECK(dsp.dither == 24 && b[0] == 0x00 && b[1] == 0x00 && b[2] == 0x20 && b[3] == 0x00 && b[5] == 0x20,
              "dither %d, első minta %02X %02X %02X (várt 00 00 20)", dsp.dither, b[0], b[1], b[2]);
        CHECK(st->guardsOk() && st->payload == 256 * 3, "24 bites puffer: %zu bájt, őrbájtok sértetlenek", st->payload);
        CHECK(out->quantExact(), "24 bites egészre bitpontosnak jelzi");
        out->close();
    }

    std::printf("\n[4] 16 bites ASIO: dither 16, nem bitpontosnak jelzi\n");
    {
        auto st = std::make_shared<DrvState>();
        st->pcmType = ASIOSTInt16LSB;
        auto out = makeOut(st);
        HalfDsp dsp;
        out->setExclusiveDsp(&dsp);
        out->setAsio(true, "");
        std::string err;
        out->open(44100, [](float* s, size_t n) { std::fill(s, s + n * 2, 0.0f); }, &err);
        CHECK(dsp.dither == 16 && !out->quantExact(), "dither %d, quantExact %d", dsp.dither, out->quantExact() ? 1 : 0);
        out->close();
    }

    std::printf("\n[5] Nincs ASIO-meghajtó: visszalépés WASAPI-ra, a hiba az állapotban\n");
    {
        auto st = std::make_shared<DrvState>();
        auto out = makeOut(st, /*driverPresent=*/false);
        HalfDsp dsp;
        out->setExclusiveDsp(&dsp);
        out->setAsio(true, "");
        std::string err;
        const bool ok = out->open(44100, [](float* s, size_t n) { std::fill(s, s + n * 2, 0.0f); }, &err);
        CHECK(ok && !out->asioActive() && out->wasapi().opens == 1 && out->asioError() == "no_driver",
              "a zene WASAPI-n szól, asioError='%s'", out->asioError().c_str());
        json::Value v;
        const bool parsed = parseStatus(out->statusJson(0.0), v);
        const json::Value* a = parsed ? v.get("asio") : nullptr;
        CHECK(a && a->get("want") && a->get("want")->b && a->get("active") && !a->get("active")->b && a->get("error") &&
              a->get("error")->str == "no_driver", "állapot: asio.want=true, active=false, error=no_driver");
        CHECK(out->supportedRates() == std::vector<uint32_t>({ 44100, 48000 }), "meghajtó nélkül a WASAPI mintavételei");
        out->close();
    }

    std::printf("\n[6] Mintavételek ASIO-n: a meghajtó szerint\n");
    {
        auto st = std::make_shared<DrvState>();
        st->pcmRates = { 44100, 88200, 176400, 768000 };
        auto out = makeOut(st);
        HalfDsp dsp;
        out->setExclusiveDsp(&dsp);
        out->setAsio(true, "");
        const auto r = out->supportedRates();
        CHECK(r == std::vector<uint32_t>({ 44100, 88200, 176400, 768000 }), "supportedRates a meghajtóé (%zu db)", r.size());
        out->setAsio(false, "");
        CHECK(out->supportedRates() == std::vector<uint32_t>({ 44100, 48000 }), "ASIO nélkül a WASAPI-é");
    }

    std::printf("\n[7] Natív DSD ASIO-n; ha a meghajtó nem tudja, nincs WASAPI-DoP kísérlet\n");
    {
        auto st = std::make_shared<DrvState>();
        st->dsdOk = true;
        auto out = makeOut(st);
        HalfDsp dsp;
        out->setExclusiveDsp(&dsp);
        out->setAsio(true, "");
        std::string err;
        const bool ok = out->openDop(176400, [](int32_t* d, size_t n) {
            for (size_t i = 0; i < n * 2; ++i) d[i] = static_cast<int32_t>((((i / 2) & 1u) ? 0xFAu : 0x05u) << 16 | 0x9669u);
        }, &err);
        CHECK(ok && out->asioActive() && out->dopActive(), "natív DSD ASIO-n nyílt meg (err='%s')", err.c_str());
        out->setActive(true);
        waitFor([&] { return st->haveCb; });
        st->cb.bufferSwitch(0, ASIOTrue);
        CHECK(st->payload == 4096 / 8 && st->guardsOk() && st->buf[0][0][0] == 0x96 && st->buf[0][0][1] == 0x69,
              "DSD-puffer %zu bájt (4096 bit / 8), bájtok %02X %02X, őrbájtok sértetlenek", st->payload, st->buf[0][0][0],
              st->buf[0][0][1]);
        CHECK(out->consumedFrames() == 4096 / 16, "consumedFrames = %llu (4096 / 16 DoP-keret)",
              static_cast<unsigned long long>(out->consumedFrames()));
        CHECK(dsp.processed == 0, "natív DSD-ben a DSP-lánc kimarad");
        out->close();

        auto st2 = std::make_shared<DrvState>();
        st2->dsdOk = false;
        st2->pcmRates = { 44100 };   // DoP-rátát (176,4 kHz) PCM-ben sem tud
        auto out2 = makeOut(st2);
        out2->setExclusiveDsp(&dsp);
        out2->setAsio(true, "");
        std::string err2;
        const bool ok2 = out2->openDop(176400, [](int32_t*, size_t) {}, &err2);
        CHECK(!ok2 && err2 == "format" && out2->wasapi().dopOpens == 0,
              "elutasítás: false, '%s', WASAPI-DoP nem próbált (%d)", err2.c_str(), out2->wasapi().dopOpens);
        CHECK(out2->open(44100, [](float* s, size_t n) { std::fill(s, s + n * 2, 0.0f); }, &err2) && out2->asioActive(),
              "a PCM-re alakított hang ASIO-n szól");
        out2->close();
    }

    std::printf("\n[8] Váltás ASIO ↔ WASAPI újranyitással, zárak\n");
    {
        auto st = std::make_shared<DrvState>();
        auto out = makeOut(st);
        HalfDsp dsp;
        out->setExclusiveDsp(&dsp);
        out->setAsio(true, "");
        std::string err;
        out->open(44100, [](float* s, size_t n) { std::fill(s, s + n * 2, 0.0f); }, &err);
        const bool wasAsio = out->asioActive();
        out->lock();
        out->flush();
        out->unlock();
        out->setAsio(false, "");
        out->open(44100, [](float* s, size_t n) { std::fill(s, s + n * 2, 0.0f); }, &err);
        CHECK(wasAsio && !out->asioActive() && out->wasapi().opens == 1, "ASIO → WASAPI váltás újranyitással");
        out->setAsio(true, "");
        out->open(44100, [](float* s, size_t n) { std::fill(s, s + n * 2, 0.0f); }, &err);
        CHECK(out->asioActive() && out->wasapi().closes >= 1, "WASAPI → ASIO vissza, a WASAPI bezárult");
        out->close();
    }

    std::printf("\n[8b] A meghajtó resetet kér: a váltó egyszer jelzi az újranyitást, az ok az állapotban\n");
    {
        auto st = std::make_shared<DrvState>();
        auto out = makeOut(st);
        HalfDsp dsp;
        out->setExclusiveDsp(&dsp);
        out->setAsio(true, "");
        std::string err;
        out->open(44100, [](float* s, size_t n) { std::fill(s, s + n * 2, 0.0f); }, &err);
        out->setActive(true);
        waitFor([&] { return st->haveCb; });
        CHECK(!out->asioNeedsReopen(), "szóló ASIO-n nincs újranyitási jelzés");
        st->cb.asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
        const bool stopped = waitFor([&] { return !out->asio().isOpen(); });
        CHECK(stopped && out->asioNeedsReopen(), "reset után a hangfolyam leállt, és a váltó újranyitást kér");
        out->acknowledgeAsioStop();
        CHECK(!out->asioNeedsReopen(), "a jelzés egyszeri (nyugtázás után nincs)");
        json::Value v;
        const bool parsed = parseStatus(out->statusJson(0.0), v);
        const json::Value* a = parsed ? v.get("asio") : nullptr;
        CHECK(a && a->get("error") && a->get("error")->str == "reset", "állapot: asio.error = reset");
        CHECK(out->open(44100, [](float* s, size_t n) { std::fill(s, s + n * 2, 0.0f); }, &err) && out->asioActive() &&
              !out->asioNeedsReopen(), "az újranyitás után ismét ASIO-n szól");
        out->close();
    }

    std::printf("\n[9] Beállítás: asio / asioDriver JSON\n");
    {
        audioopt::AudioOptions o;
        o.asio = true;
        o.asioDriver = "My \"DAC\" \\ Á";
        json::Value v;
        const std::string j = audioopt::toJson(o);
        const bool p = json::parse(j, v);
        const audioopt::AudioOptions back = audioopt::fromJson(v);
        CHECK(p && back.asio && back.asioDriver == o.asioDriver && back == o, "oda-vissza (idézőjel, visszaper, ékezet): '%s'",
              back.asioDriver.c_str());
        json::Value old;
        json::parse("{\"exclusive\":true}", old);
        const audioopt::AudioOptions o2 = audioopt::fromJson(old);
        CHECK(!o2.asio && o2.asioDriver.empty(), "régi JSON: ASIO ki, nincs meghajtó");
        json::Value bad;
        json::parse("{\"asio\":true,\"asioDriver\":\"a\\u0001b\"}", bad);
        CHECK(audioopt::fromJson(bad).asioDriver.empty(), "vezérlőkarakteres meghajtónév elvetve");
    }

    std::printf("\n[10] A lejátszó újranyitja a kimenetet (FilePlayer::reopenOutput)\n");
    {
        // a kimenet „órajelre áll be” (mint kizárólagos WASAPI-n és ASIO-n): ilyenkor az azonos mintavételű open()
        // csak a számot cseréli – a reopenOutput()-nak ennek ellenére új hangfolyamot kell nyitnia
        struct CountOut : PlayerOutput {
            std::atomic<int> opens{0};
            bool open(uint32_t, RenderFn, std::string*) override { ++opens; return true; }
            void close() override {}
            void setActive(bool) override {}
            void lock() override {}
            void unlock() override {}
            bool settlesOnRateChange() const override { return true; }
        };
        auto co = std::make_unique<CountOut>();
        CountOut* cop = co.get();
        FilePlayer fp(std::move(co));
        fp.reopenOutput();
        CHECK(cop->opens == 0, "betöltött szám nélkül nem nyit (%d)", cop->opens.load());
        // 1 másodperces, 44,1 kHz-es 16 bites sztereó WAV
        // folyamatonként egyedi név: párhuzamosan futó példányok ne írják / töröljék egymás fájlját
        const std::filesystem::path wav = std::filesystem::temp_directory_path() /
                                          ("aq_winout_test_" + std::to_string(GetCurrentProcessId()) + ".wav");
        {
            const uint32_t sr = 44100, n = sr;
            const uint32_t dataBytes = n * 4;
            std::ofstream f(wav, std::ios::binary);
            auto u32 = [&](uint32_t x) { f.write(reinterpret_cast<const char*>(&x), 4); };
            auto u16 = [&](uint16_t x) { f.write(reinterpret_cast<const char*>(&x), 2); };
            f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVEfmt ", 8); u32(16); u16(1); u16(2); u32(sr);
            u32(sr * 4); u16(4); u16(16); f.write("data", 4); u32(dataBytes);
            std::vector<char> z(dataBytes, 0);
            f.write(z.data(), z.size());
        }
        std::string err;
        const bool opened = fp.open(wav.string(), &err);
        const int afterOpen = cop->opens;
        fp.reopenOutput();
        CHECK(opened && afterOpen >= 1 && cop->opens == afterOpen + 1, "szóló számnál újranyit (%d → %d)", afterOpen, cop->opens.load());
        const int afterReopen = cop->opens;
        fp.open(wav.string(), &err);
        CHECK(cop->opens == afterReopen, "a következő, azonos mintavételű szám nem nyit új hangfolyamot (a kényszer egyszeri: %d → %d)",
              afterReopen, cop->opens.load());
        fp.stop();
        std::error_code ec;
        std::filesystem::remove(wav, ec);
    }

    std::printf("\n[9] Equalizer APO a lejátszóeszközön (2.6.1: a regisztrációs adatbázisból, csak olvasva)\n");
    {
        using win::endpointKeyFromId;
        CHECK(endpointKeyFromId(L"{0.0.0.00000000}.{56869ca8-1234-4abc-8def-0123456789ab}") ==
                  L"{56869ca8-1234-4abc-8def-0123456789ab}",
              "a WASAPI-azonosítóból a regisztrációs kulcs");
        CHECK(endpointKeyFromId(L"").empty() && endpointKeyFromId(L"{0.0.0.00000000}").empty() &&
                  endpointKeyFromId(L"{0.0.0.00000000}.{rövid}").empty(),
              "hibás azonosítóból üres kulcs (ismeretlen, nincs riasztás)");
        CHECK(win::isEqApoClassName(L"EqualizerAPO Pre-Mix Class") && win::isEqApoClassName(L"EqualizerAPO Post-Mix Class") &&
                  !win::isEqApoClassName(L"Realtek Audio Effects") && !win::isEqApoClassName(L""),
              "az Equalizer APO osztálynév felismerése (más gyártó effektje nem az)");
        CHECK(win::apoOnRenderDevice(L"nem-azonosito") == ApoOnDevice::Unknown, "értelmezhetetlen azonosító: ismeretlen");
        // A gép valódi eszközei: a lista és az egyenkénti lekérdezés ugyanazt mondja
        const auto devs = win::listRenderDevices();
        bool consistent = true;
        for (const auto& d : devs) {
            const ApoOnDevice one = win::apoOnRenderDevice(L"{0.0.0.00000000}." + d.key);
            if (one != d.apo) consistent = false;
            std::printf("         %s %-60s APO: %s\n", d.active ? "[aktív]" : "[ -   ]", d.name.c_str(),
                        d.apo == ApoOnDevice::Yes ? "igen" : d.apo == ApoOnDevice::No ? "nem" : "?");
        }
        CHECK(consistent, "a lista és az egyenkénti lekérdezés egyezik (%zu eszköz)", devs.size());
    }

    std::printf("\n[11] Őrszem: a rendszerhang-figyelő (loopback) szála jelez; leállítás után nincs beragadás\n");
    {
        auto findHb = [](const char* name) -> const aq::wd::Slot* {
            const aq::wd::Slot* slots = aq::wd::Registry::global().slots();
            for (int i = 0; i < aq::wd::kMaxSlots; ++i)
                if (slots[i].used.load() && std::strncmp(slots[i].name, name, aq::wd::kNameLen) == 0) return &slots[i];
            return nullptr;
        };
        // Valódi WASAPI: hangeszközzel a rögzítés ciklusa, eszköz nélkül (pl. CI) az újrapróbálkozás ciklusa fut
        win::WasapiLoopback lb;
        aq::wd::Watchdog wd(aq::wd::Registry::global(), 0.5);
        lb.start();
        const aq::wd::Slot* s = nullptr;
        for (int i = 0; i < 200 && !(s && s->lastNs.load() > 0); ++i) {
            s = findHb("loopback");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(s != nullptr && s->lastNs.load() > 0, "a szál regisztrált és jelzett (loopback)");
        int stalls = 0;
        int64_t first = s ? s->lastNs.load() : 0;
        for (int i = 0; i < 50; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            for (const auto& e : wd.check(aq::wd::nowNs())) stalls += (e.thread == "loopback" && !e.recovered) ? 1 : 0;
        }
        const int64_t last = s ? s->lastNs.load() : 0;
        const char* label = s ? s->label.load() : "";
        CHECK(last > first, "a ciklus közben jelez (1 s alatt %.0f ms-mal későbbi szívverés, címke: \"%s\", eszköz: %s)",
              (last - first) / 1e6, label ? label : "", lb.active() ? "rögzít" : "nincs / újrapróbálkozik");
        CHECK(label && std::strncmp(label, "loopback:", 9) == 0, "a címke a ciklus helyét mondja");
        CHECK(stalls == 0, "futás közben nincs beragadás-jelzés (küszöb 0,5 s)");
        lb.stop();
        for (int i = 0; i < 40; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            for (const auto& e : wd.check(aq::wd::nowNs())) stalls += (e.thread == "loopback" && !e.recovered) ? 1 : 0;
        }
        CHECK(findHb("loopback") == nullptr, "leállítás után a szál kikerült a regiszterből");
        CHECK(stalls == 0, "leállítás után nincs beragadás-jelzés");
    }

    std::printf("\n[12] A hangeszköz alapformátuma a regisztrációs adatbázisból ({f19f064d-…},0, COM nélkül)\n");
    {
        const wchar_t* sub = L"Software\\AqulifyDevFmtTest_1b\\{00000000-1111-2222-3333-444444444444}\\Properties";
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\AqulifyDevFmtTest_1b");
        HKEY k = nullptr;
        bool made = RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS;
        // Egy valódi végpont értéke: 8 bájtos fejléc + WAVEFORMATEXTENSIBLE (48 kHz, 24 bit 32 bites tárolóban, sztereó)
        const uint8_t blob[] = { 0x41, 0, 0, 0, 1, 0, 0, 0, 0xfe, 0xff, 2, 0, 0x80, 0xbb, 0, 0, 0, 0xdc, 5, 0, 8, 0, 0x20, 0,
                                 0x16, 0, 0x18, 0, 3, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71 };
        if (made) {
            made = RegSetValueExW(k, L"{f19f064d-082c-4e27-bc73-6882a1bb8e4c},0", 0, REG_BINARY, blob, sizeof(blob)) == ERROR_SUCCESS;
            RegCloseKey(k);
        }
        const std::string got = aq::devfmt::readRegistry(HKEY_CURRENT_USER, sub);
        CHECK(made && got == "48000 Hz, 24 bit (32-bit container), 2 ch, PCM", "a bináris érték kiolvasva: \"%s\"", got.c_str());
        const std::string none = aq::devfmt::readRegistry(HKEY_CURRENT_USER, L"Software\\AqulifyDevFmtTest_1b\\nincs\\Properties");
        CHECK(none.empty(), "hiányzó kulcs: üres szöveg");
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\AqulifyDevFmtTest_1b");
        // A gép valódi aktív eszközei (csak kiírás: a formátum a beállítástól függ)
        for (const auto& d : win::listRenderDevices()) {
            if (!d.active) continue;
            const std::string f = aq::devfmt::readRegistry(HKEY_LOCAL_MACHINE, std::wstring(win::apo_detail::renderBase()) + d.key + L"\\Properties");
            std::printf("         %-60s %s\n", d.name.c_str(), f.empty() ? "?" : f.c_str());
        }
    }

    std::printf("\n%s (%d hiba, %d rendben)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail, g_ok);
    return g_fail ? 1 : 0;
}
