#pragma once
/**
 * @file player_output_alsa.hpp
 * @brief A beépített lejátszó közvetlen ALSA-kimenete Linuxon (kizárólagos, bitpontos mód): a DAC-ot (hw: eszköz) a
 *        lejátszó egyedül használja, a PipeWire keverője és átalakítója kimarad.
 *
 * - Eszközfoglalás: amíg a kimenet nyitva van, a lejátszó a szabványos D-Bus-os foglalással
 *   (org.freedesktop.ReserveDevice1.Audio<N>) elkéri a hangkártyát; a PipeWire ilyenkor elengedi. Bezáráskor a
 *   foglalás megszűnik, és a PipeWire visszaveszi az eszközt. (Session-busz nélkül csak megnyitni próbálja.)
 * - Formátum: a fájl saját mintavétele, átalakítás nélkül (ha a DAC nem tudja, a megnyitás nem sikerül, és a
 *   lejátszó a PipeWire-en át szól); a minta formátuma az első, amelyet a DAC elfogad: 32 bit → 24 bit (3 bájt) →
 *   16 bit. A DSP-lánc (ExclusiveDsp) a kimenet szálán fut.
 * - Kattanásmentes váltás: settlesOnRateChange() igaz, a lejátszó az új hangfolyamot rövid csenddel kezdi.
 */
#include <alsa/asoundlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

#if defined(AQ_HAVE_SYSTEMD)
#include <systemd/sd-bus.h>
#endif

#include "dsd.hpp"
#include "exclusive_dsp.hpp"
#include "file_player.hpp"
#include "pcm_convert.hpp"
#include "watchdog.hpp"

namespace ee_analyzer::player {

/** Egy ALSA-hangkártya a választáshoz. */
struct AlsaCard {
    int index = -1;
    std::string id, name;
    bool usb = false;
};

/** A rendszer hangkártyái (ALSA). */
inline std::vector<AlsaCard> listAlsaCards() {
    std::vector<AlsaCard> out;
    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        AlsaCard c;
        c.index = card;
        char* id = nullptr;
        char* name = nullptr;
        // az azonosító a vezérlő eszközből (hw:N), a név a kártyáé
        snd_ctl_t* ctl = nullptr;
        const std::string hw = "hw:" + std::to_string(card);
        if (snd_ctl_open(&ctl, hw.c_str(), 0) == 0) {
            snd_ctl_card_info_t* info;
            snd_ctl_card_info_alloca(&info);
            if (snd_ctl_card_info(ctl, info) == 0) {
                c.id = snd_ctl_card_info_get_id(info);
                c.name = snd_ctl_card_info_get_name(info);
            }
            snd_ctl_close(ctl);
        }
        if (c.name.empty() && snd_card_get_name(card, &name) == 0 && name) { c.name = name; free(name); }
        (void)id;
        c.usb = access(("/proc/asound/card" + std::to_string(card) + "/usbid").c_str(), F_OK) == 0;
        out.push_back(std::move(c));
    }
    return out;
}

/** Az alapértelmezett közvetlen eszköz: az első USB-s hangkártya (DAC), ha nincs, az első kártya; "" ha nincs kártya. */
inline std::string defaultAlsaDevice() {
    const auto cards = listAlsaCards();
    for (const auto& c : cards) if (c.usb) return "hw:" + std::to_string(c.index) + ",0";
    return cards.empty() ? std::string() : "hw:" + std::to_string(cards.front().index) + ",0";
}

/** A "hw:N,…" / "hw:CARD=id,…" / "plughw:…" eszköznév kártyaszáma; -1, ha nem hardveres eszköz. */
inline int alsaCardIndex(const std::string& dev) {
    std::string s;
    if (dev.rfind("plughw:", 0) == 0) s = dev.substr(7);
    else if (dev.rfind("hw:", 0) == 0) s = dev.substr(3);
    else return -1;
    if (s.rfind("CARD=", 0) == 0) s = s.substr(5);
    const std::string card = s.substr(0, s.find(','));
    if (card.empty()) return -1;
    const bool digits = std::all_of(card.begin(), card.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
    if (digits) return std::atoi(card.c_str());
    return snd_card_get_index(card.c_str());
}

#if defined(AQ_HAVE_SYSTEMD)
/**
 * A hangkártya elkérése a D-Bus-os eszközfoglalással (org.freedesktop.ReserveDevice1, ugyanaz, amit a JACK használ):
 * ha az eszközt más (pl. a PipeWire) tartja, udvariasan elengedést kér, és az elengedés után átveszi. A foglalás
 * alatt mások elengedési kérését elutasítja.
 */
class AudioDeviceReservation {
public:
    ~AudioDeviceReservation() { release(); }

    /** true: az eszköz a miénk (vagy senki nem tartotta). error: az ok, ha nem sikerült. */
    bool acquire(int card, std::string* error) {
        release();
        if (card < 0) return true;
        if (sd_bus_open_user(&bus_) < 0) { bus_ = nullptr; return true; }   // nincs session-busz: nincs kit kérni
        sd_bus_set_method_call_timeout(bus_, 5000000);   // a másik fél nem válaszol: 5 s, ne az alapértelmezett 25 s
        name_ = "org.freedesktop.ReserveDevice1.Audio" + std::to_string(card);
        path_ = "/org/freedesktop/ReserveDevice1/Audio" + std::to_string(card);
        static const sd_bus_vtable vtable[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("RequestRelease", "i", "b", &AudioDeviceReservation::onRequestRelease, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_PROPERTY("Priority", "i", &AudioDeviceReservation::propPriority, 0, SD_BUS_VTABLE_PROPERTY_CONST),
            SD_BUS_PROPERTY("ApplicationName", "s", &AudioDeviceReservation::propName, 0, SD_BUS_VTABLE_PROPERTY_CONST),
            SD_BUS_PROPERTY("ApplicationDeviceName", "s", &AudioDeviceReservation::propDevice, 0, SD_BUS_VTABLE_PROPERTY_CONST),
            SD_BUS_VTABLE_END
        };
        if (sd_bus_add_object_vtable(bus_, &slot_, path_.c_str(), "org.freedesktop.ReserveDevice1", vtable, this) < 0) {
            if (error) *error = "a foglalás objektuma nem hozható létre";
            release();
            return false;
        }
        int r = sd_bus_request_name(bus_, name_.c_str(), 0);
        if (r == -EEXIST) {
            // más tartja: elengedést kérünk tőle (a PipeWire ilyenkor bezárja az eszközt)
            sd_bus_error err = SD_BUS_ERROR_NULL;
            sd_bus_message* reply = nullptr;
            int granted = 0;
            r = sd_bus_call_method(bus_, name_.c_str(), path_.c_str(), "org.freedesktop.ReserveDevice1", "RequestRelease",
                                   &err, &reply, "i", kPriority);
            if (r >= 0) sd_bus_message_read(reply, "b", &granted);
            sd_bus_message_unref(reply);
            sd_bus_error_free(&err);
            if (!granted) {
                if (error) *error = "a hangkártyát más program tartja, és nem engedte el";
                release();
                return false;
            }
            r = sd_bus_request_name(bus_, name_.c_str(), SD_BUS_NAME_REPLACE_EXISTING);
        }
        if (r < 0) {
            if (error) *error = "a hangkártya foglalása nem sikerült";
            release();
            return false;
        }
        owned_ = true;
        return true;
    }

    /** Mások kéréseinek kiszolgálása (nem blokkol); a kimenet szála rendszeresen hívja. */
    void process() {
        if (!bus_) return;
        for (int i = 0; i < 8 && sd_bus_process(bus_, nullptr) > 0; ++i) {}
    }

    void release() {
        if (bus_ && owned_) sd_bus_release_name(bus_, name_.c_str());
        owned_ = false;
        if (slot_) slot_ = sd_bus_slot_unref(slot_);
        if (bus_) { sd_bus_flush(bus_); bus_ = sd_bus_flush_close_unref(bus_); }
    }

private:
    static constexpr int32_t kPriority = 10;
    static int onRequestRelease(sd_bus_message* m, void*, sd_bus_error*) {
        return sd_bus_reply_method_return(m, "b", 0);   // a lejátszás alatt nem engedjük el
    }
    static int propPriority(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*, sd_bus_error*) {
        return sd_bus_message_append(reply, "i", kPriority);
    }
    static int propName(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*, sd_bus_error*) {
        return sd_bus_message_append(reply, "s", "Aqulify");
    }
    static int propDevice(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*, sd_bus_error*) {
        return sd_bus_message_append(reply, "s", "Aqulify");
    }

    sd_bus* bus_ = nullptr;
    sd_bus_slot* slot_ = nullptr;
    std::string name_, path_;
    bool owned_ = false;
};
#else
class AudioDeviceReservation {
public:
    bool acquire(int, std::string*) { return true; }
    void process() {}
    void release() {}
};
#endif

/**
 * Natív DSD-formátum az ALSA-n (Linux / Pi, kizárólagos kimenet). A hardver-paraméterekből választjuk, ebben a
 * sorrendben. Egy ALSA-keret csatornánként `bytes` DSD-bájtot visz (8 · bytes 1 bites mintát), a bájtokon belül az
 * időben első bit a legfelső (MSB-first); a _BE formátumban a bájtok időrendben, a _LE-ben fordított sorrendben
 * állnak a mintán belül. A keretráta = DSD-ráta / (8 · bytes), tehát a DoP-rátához képest: 2 / bytes-szoros.
 * Forrás: ALSA SND_PCM_FORMAT_DSD_* (alsa-lib pcm.h), ellenőrizve 2026-11-18.
 */
struct AlsaDsdFormat {
    snd_pcm_format_t fmt;
    int bytes;         // DSD-bájt / csatorna / ALSA-keret
    bool bigEndian;    // a mintán belüli bájtsorrend (U8-nál közömbös)
    const char* name;
};
inline constexpr AlsaDsdFormat kAlsaDsdFormats[] = {
    { SND_PCM_FORMAT_DSD_U32_BE, 4, true, "DSD_U32_BE" },
    { SND_PCM_FORMAT_DSD_U32_LE, 4, false, "DSD_U32_LE" },
    { SND_PCM_FORMAT_DSD_U16_BE, 2, true, "DSD_U16_BE" },
    { SND_PCM_FORMAT_DSD_U16_LE, 2, false, "DSD_U16_LE" },
    { SND_PCM_FORMAT_DSD_U8, 1, true, "DSD_U8" },
};

/**
 * DSD-bájtfolyam (csatornánként, időrendben, MSB-first) csomagolása natív ALSA DSD-keretekbe.
 * @param chBytes  csatornánként `frames * bytes` bájt (chBytes[c][k]: a c. csatorna k. bájtja)
 * @param out      `frames * 2 * bytes` bájt, sztereó keretek
 */
inline void packNativeDsd(const uint8_t* const chBytes[2], size_t frames, int bytes, bool bigEndian, uint8_t* out) {
    for (size_t n = 0; n < frames; ++n)
        for (int c = 0; c < 2; ++c)
            for (int p = 0; p < bytes; ++p)
                out[(n * 2 + static_cast<size_t>(c)) * static_cast<size_t>(bytes) + static_cast<size_t>(bigEndian ? p : bytes - 1 - p)] =
                    chBytes[c][n * static_cast<size_t>(bytes) + static_cast<size_t>(p)];
}

/**
 * DoP-szavakból (a lejátszó DoP-kimenete: [23:16] jelölő, [15:8] b0, [7:0] b1, MSB-first DSD-bájtok) natív ALSA
 * DSD-periódusok. Egy DoP-szó csatornánként 2 bájt; ha a periódus bájtszáma páratlan (DSD_U8), a maradék bájt a
 * következő periódusba kerül. A jelölő nem kerül át.
 */
class NativeDsdPacker {
public:
    void setup(int bytes, bool bigEndian, size_t maxPeriod) {
        nb_ = static_cast<size_t>(bytes);
        be_ = bigEndian;
        ch_[0].assign(maxPeriod * nb_ + 2, dsd::kDsdSilenceByte);
        ch_[1].assign(maxPeriod * nb_ + 2, dsd::kDsdSilenceByte);
        carried_ = 0;
    }
    void reset() { carried_ = 0; }
    /** Ennyi DoP-keret kell a következő `period` ALSA-kerethez. */
    size_t dopFramesFor(size_t period) const { return (period * nb_ - carried_ + 1) / 2; }
    /** `dop`: dopFramesFor(period) sztereó DoP-szó; `out`: period · 2 · nb bájt. */
    void pack(const int32_t* dop, size_t period, uint8_t* out) {
        const size_t need = period * nb_, frames = dopFramesFor(period);
        for (int c = 0; c < 2; ++c) {
            uint8_t* d = ch_[c].data();
            if (carried_) d[0] = carry_[c];
            for (size_t i = 0; i < frames; ++i) {
                const uint32_t w = static_cast<uint32_t>(dop[2 * i + static_cast<size_t>(c)]);
                d[carried_ + 2 * i] = static_cast<uint8_t>((w >> 8) & 0xFFu);
                d[carried_ + 2 * i + 1] = static_cast<uint8_t>(w & 0xFFu);
            }
        }
        const size_t total = carried_ + 2 * frames;
        if (total > need) { carry_[0] = ch_[0][need]; carry_[1] = ch_[1][need]; carried_ = 1; }
        else carried_ = 0;
        const uint8_t* src[2] = { ch_[0].data(), ch_[1].data() };
        packNativeDsd(src, period, static_cast<int>(nb_), be_, out);
    }

private:
    size_t nb_ = 4;
    bool be_ = true;
    std::vector<uint8_t> ch_[2];
    uint8_t carry_[2] = { 0, 0 };
    size_t carried_ = 0;
};

/** A kimenet formátuma. */
struct AlsaFormat {
    snd_pcm_format_t fmt = SND_PCM_FORMAT_S32_LE;
    int bits = 32, valid = 32;   // a minta mérete a pufferben és ebből az érvényes (pcm::floatToInt)
    int ditherBits = 0;          // a lánc ditherének bitmélysége (0: a beállítás marad)
    const char* name = "32 bit";
};

class AlsaPlayerOutput : public PlayerOutput {
public:
    /** device: "" = automatikus (az első USB-s DAC); különben pl. "hw:1,0" vagy "hw:CARD=DAC,DEV=0". */
    explicit AlsaPlayerOutput(std::string device = "") : device_(std::move(device)) {}
    ~AlsaPlayerOutput() override { close(); }

    void setExclusiveDsp(ExclusiveDsp* dsp) { dsp_ = dsp; }
    void setDevice(const std::string& dev) { std::lock_guard<std::mutex> lk(infoMutex_); device_ = dev; }

    bool open(uint32_t sampleRate, RenderFn render, std::string* error) override {
        close();
        {
            std::lock_guard<std::mutex> lk(renderMutex_);
            render_ = std::move(render);
            renderDop_ = nullptr;
        }
        dopMode_ = false;
        dopError_.clear();
        rate_ = sampleRate;
        active_ = false;
        quit_ = false;
        flushReq_ = false;
        written_ = 0;
        consumed_ = 0;
        initState_ = 0;
        quantLoss_ = false;
        rtPriority_ = 0;
        thread_ = std::thread([this] { run(); });
        std::unique_lock<std::mutex> lk(initMutex_);
        if (!waitInit(lk)) { lk.unlock(); return giveUpInit(error, false); }
        if (initState_ < 0) {
            lk.unlock();
            const std::string why = lastError();
            close();
            if (error) *error = why;
            return false;
        }
        return true;
    }

    void close() override {
        if (thread_.joinable()) {
            quit_ = true;
            wakeCv_.notify_all();
            thread_.join();
        }
        open_ = false;
        dopMode_ = false;
        nativeDsd_ = false;
        std::lock_guard<std::mutex> lk(renderMutex_);
        render_ = nullptr;
        renderDop_ = nullptr;
    }

    /**
     * Natív DSD (DoP 1.1) hangfolyam megnyitása közvetlen ALSA kimeneten.
     * Formátum: SND_PCM_FORMAT_S32_LE (24 bit az int32 felső 24 bitjén, MSB-igazítva) vagy SND_PCM_FORMAT_S24_3LE.
     * Ha a hardver nem fogadja el a mintavételt vagy formátumot, false-szal tér vissza ("format", "busy").
     * Források (ellenőrizve: 2026-11-16):
     *   https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m.html
     *   https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m___h_w___params.html
     */
    bool openDop(uint32_t dopSampleRate, RenderDopFn render, std::string* error) override {
        close();
        {
            std::lock_guard<std::mutex> lk(renderMutex_);
            render_ = nullptr;
            renderDop_ = std::move(render);
        }
        dopMode_ = true;
        dopError_.clear();
        rate_ = dopSampleRate;
        active_ = false;
        quit_ = false;
        flushReq_ = false;
        written_ = 0;
        consumed_ = 0;
        initState_ = 0;
        quantLoss_ = false;
        rtPriority_ = 0;
        thread_ = std::thread([this] { run(); });
        std::unique_lock<std::mutex> lk(initMutex_);
        if (!waitInit(lk)) { lk.unlock(); return giveUpInit(error, true); }
        if (initState_ < 0) {
            lk.unlock();
            const std::string why = dopError_.empty() ? "format" : dopError_;
            close();
            if (error) *error = why;
            return false;
        }
        return true;
    }

    void setActive(bool active) override {
        active_ = active;
        wakeCv_.notify_all();
    }
    void lock() override { renderMutex_.lock(); }
    void unlock() override { renderMutex_.unlock(); }
    void flush() override { flushReq_ = true; consumed_ = 0; }
    uint64_t consumedFrames() const override { return consumed_.load(); }
    bool settlesOnRateChange() const override { return open_.load(); }

    /** A DAC / ALSA eszköz által támogatott mintavételi frekvenciák lekérdezése (K11). */
    std::vector<uint32_t> supportedRates() const override {
        static const uint32_t kCandidates[] = {
            44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000
        };
        const std::string dev = device_.empty() ? defaultAlsaDevice() : device_;
        std::vector<uint32_t> supported;
        if (!dev.empty()) {
            snd_pcm_t* pcm = nullptr;
            if (snd_pcm_open(&pcm, dev.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) == 0) {
                snd_pcm_hw_params_t* hw = nullptr;
                snd_pcm_hw_params_alloca(&hw);
                if (snd_pcm_hw_params_any(pcm, hw) == 0) {
                    for (uint32_t r : kCandidates) {
                        if (snd_pcm_hw_params_test_rate(pcm, hw, r, 0) == 0) {
                            supported.push_back(r);
                        }
                    }
                }
                snd_pcm_close(pcm);
            }
        }
        if (supported.empty()) {
            std::lock_guard<std::mutex> lk(infoMutex_);
            if (!cachedRates_.empty()) return cachedRates_;
            return { 44100, 48000, 88200, 96000, 176400, 192000 };
        }
        {
            std::lock_guard<std::mutex> lk(infoMutex_);
            cachedRates_ = supported;
        }
        return supported;
    }

    /** Az éppen futó ALSA kimenet tényleges mintavétele (Hz); 0 ha inaktív. */
    uint32_t actualRate() const override {
        return open_.load() ? rate_.load() : 0;
    }

    bool isOpen() const { return open_.load(); }
    bool quantExact() const { return !quantLoss_.load(); }
    /** A legutóbbi megnyitás leírása (eszköz, formátum) vagy a hiba oka. */
    std::string describe() const { std::lock_guard<std::mutex> lk(infoMutex_); return info_; }
    std::string lastError() const { std::lock_guard<std::mutex> lk(infoMutex_); return error_; }
    std::string device() const { std::lock_guard<std::mutex> lk(infoMutex_); return openedDevice_; }
    uint32_t overruns() const { return overruns_.load(); }
    /** A kimeneti szál valós idejű (SCHED_FIFO) prioritása, vagy 0, ha normál prioritással fut. */
    int realtimePriority() const { return rtPriority_.load(); }
    bool isDopMode() const { return dopMode_.load(); }
    /** DSD-nél natív formátumban (DSD_U32 / U16 / U8) szól-e, nem DoP-ban; a formátum neve, ha igen. */
    bool isNativeDsd() const { return nativeDsd_.load(); }
    std::string nativeDsdFormat() const { std::lock_guard<std::mutex> lk(infoMutex_); return nativeName_; }
    /** A natív DSD kipróbálása DoP előtt (alapból igen; a teszt és a hibakeresés kikapcsolhatja). */
    void setPreferNativeDsd(bool on) { preferNative_ = on; }
    /** Csak a tesztnek: a megnyitás cseréje (nem blokkoló hívás, mint az snd_pcm_open NONBLOCK-kal) és az időkorlátok (ms). */
    void setOpenHooksForTest(std::function<int(snd_pcm_t**, const char*)> openFn, int retryMs, int initWaitMs) {
        pcmOpenFn_ = std::move(openFn);
        openRetryMs_ = retryMs;
        initWaitMs_ = initWaitMs;
    }
    void setDopTap(std::function<void(const int32_t*, size_t)> fn) { dopTap_ = std::move(fn); }
    void setRawTap(std::function<void(const uint8_t*, size_t, const AlsaFormat&)> fn) { rawTap_ = std::move(fn); }

private:
    void setError(const std::string& e) { std::lock_guard<std::mutex> lk(infoMutex_); error_ = e; }

    /**
     * A szál megnyitásának bevárása időkorláttal: a hívó (dekóder / híd) szál így soha nem akad el végtelenül.
     * false: az időkorlát lejárt.
     */
    bool waitInit(std::unique_lock<std::mutex>& lk) {
        return initCv_.wait_for(lk, std::chrono::milliseconds(initWaitMs_), [this] { return initState_ != 0; });
    }

    /**
     * Az időkorlát lejárt, a szál még nem jelzett vissza. NEM join-olunk: ha a szál valamelyik hívásban (a kernelben)
     * ragadt, a join ugyanúgy végtelenül várna, csak máshol. Jelezzük a szálnak a kilépést (quit_), ő a megnyitás
     * végén magától bezár és elenged mindent; a következő close() / open() / destruktor join-ol rá (addigra lejár,
     * mert a megnyitás minden lépése időkorlátos).
     */
    bool giveUpInit(std::string* error, bool dop) {
        quit_ = true;
        wakeCv_.notify_all();
        const std::string why = "a hangkártya megnyitása nem fejeződött be időben (más program tarthatja az eszközt)";
        setError(why);
        if (error) *error = dop ? "busy" : why;
        return false;
    }

    /**
     * 24 bites DoP szó (az int32_t alsó 24 bitjén: [23:16]=marker, [15:8]=b0, [7:0]=b1) bitpontos átadása az ALSA
     * kizárólagos pufferébe: 32 bites keretben (SND_PCM_FORMAT_S32_LE) a felső 24 biten (8 bittel balra tolva:
     * [31:24]=marker, [23:16]=b0, [15:8]=b1, [7:0]=0, MSB-igazítva), 24 bites tömörített keretben (SND_PCM_FORMAT_S24_3LE)
     * 3 kis-endián bájton (b1, b0, marker).
     */
    static void convertDop(const int32_t* in, uint8_t* out, size_t samples, const AlsaFormat& f) {
        if (f.bits == 32) {
            for (size_t i = 0; i < samples; ++i) {
                const uint32_t w = (static_cast<uint32_t>(in[i]) & 0x00FFFFFFu) << 8;
                std::memcpy(out + i * 4, &w, 4);
            }
        } else if (f.bits == 24) {
            for (size_t i = 0; i < samples; ++i) {
                const uint32_t u = static_cast<uint32_t>(in[i]);
                out[i * 3]     = static_cast<uint8_t>(u & 0xFFu);
                out[i * 3 + 1] = static_cast<uint8_t>((u >> 8) & 0xFFu);
                out[i * 3 + 2] = static_cast<uint8_t>((u >> 16) & 0xFFu);
            }
        }
    }

    /** A hangkártya megnyitása és beállítása a szálon; false: a hiba oka a lastError()-ban. */
    bool openDevice(snd_pcm_t*& pcm, AlsaFormat& f, snd_pcm_uframes_t& period, bool& canPause) {
        std::string dev;
        { std::lock_guard<std::mutex> lk(infoMutex_); dev = device_; }
        if (dev.empty()) dev = defaultAlsaDevice();
        if (dev.empty()) {
            setError("nincs hangkártya");
            if (dopMode_) dopError_ = "format";
            return false;
        }
        std::string why;
        if (!reservation_.acquire(alsaCardIndex(dev), &why)) {
            setError(why);
            if (dopMode_) dopError_ = "busy";
            return false;
        }
        // NEM blokkoló megnyitás: a Linux kernel (snd_pcm_open, pcm_native.c) foglalt eszköznél blokkoló módban a
        // végtelenségig várakoztat, amíg a másik használó (pl. a PipeWire) be nem zárja. Itt rövid ideig újrapróbáljuk
        // (a foglalás kéréseit közben kiszolgálva), utána "foglalt" hibával visszatérünk.
        const auto t0 = std::chrono::steady_clock::now();
        int r = 0;
        for (;;) {
            pcm = nullptr;
            r = pcmOpenFn_ ? pcmOpenFn_(&pcm, dev.c_str())
                           : snd_pcm_open(&pcm, dev.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
            if (r != -EBUSY && r != -EAGAIN) break;
            if (quit_ || std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(openRetryMs_)) break;
            reservation_.process();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (r >= 0) snd_pcm_nonblock(pcm, 0);   // a lejátszó ciklus blokkoló írásra épül
        if (r < 0) {
            const bool busy = (r == -EBUSY || r == -EAGAIN);
            setError(busy ? std::string("a hangkártyát más program tartja (") + dev + ")"
                          : std::string("a hangkártya nem nyitható meg (") + dev + "): " + snd_strerror(r));
            if (dopMode_) dopError_ = busy ? "busy" : "format";
            reservation_.release();
            pcm = nullptr;
            return false;
        }
        snd_pcm_hw_params_t* hw;
        snd_pcm_hw_params_alloca(&hw);
        snd_pcm_hw_params_any(pcm, hw);
        snd_pcm_hw_params_set_rate_resample(pcm, hw, 0);                 // átalakítás nélkül
        snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
        const AlsaFormat pcmFormats[] = {
            { SND_PCM_FORMAT_S32_LE, 32, 32, 0, "32 bit" },
            { SND_PCM_FORMAT_S24_3LE, 24, 24, 24, "24 bit" },
            { SND_PCM_FORMAT_S16_LE, 16, 16, 16, "16 bit" },
        };
        const AlsaFormat dopFormats[] = {
            { SND_PCM_FORMAT_S32_LE, 32, 24, 0, "24/32 bit DoP" },
            { SND_PCM_FORMAT_S24_3LE, 24, 24, 24, "24 bit DoP" },
        };
        bool fmtOk = false;
        unsigned hwRate = rate_;
        nativeDsd_ = false;
        if (dopMode_ && preferNative_) {
            // natív DSD előbb: az első formátum, amelyhez a hardver a megfelelő keretrátát is elfogadja
            for (const auto& d : kAlsaDsdFormats) {
                if ((static_cast<unsigned>(rate_) * 2u) % static_cast<unsigned>(d.bytes) != 0) continue;
                const unsigned nr = static_cast<unsigned>(rate_) * 2u / static_cast<unsigned>(d.bytes);
                snd_pcm_hw_params_t* t;
                snd_pcm_hw_params_alloca(&t);
                snd_pcm_hw_params_copy(t, hw);
                if (snd_pcm_hw_params_set_format(pcm, t, d.fmt) == 0 && snd_pcm_hw_params_set_channels(pcm, t, 2) == 0 &&
                    snd_pcm_hw_params_set_rate(pcm, t, nr, 0) == 0) {
                    snd_pcm_hw_params_copy(hw, t);
                    f = AlsaFormat{ d.fmt, d.bytes * 8, d.bytes * 8, 0, d.name };
                    nativeBytes_ = d.bytes;
                    nativeBigEndian_ = d.bigEndian;
                    hwRate = nr;
                    nativeDsd_ = true;
                    fmtOk = true;
                    break;
                }
            }
        }
        if (nativeDsd_) {
            // a formátum, a csatornák és a ráta már beállítva
        } else if (dopMode_) {
            for (const auto& c : dopFormats) {
                if (snd_pcm_hw_params_test_format(pcm, hw, c.fmt) == 0 && snd_pcm_hw_params_set_format(pcm, hw, c.fmt) == 0) {
                    f = c;
                    fmtOk = true;
                    break;
                }
            }
        } else {
            for (const auto& c : pcmFormats) {
                if (snd_pcm_hw_params_test_format(pcm, hw, c.fmt) == 0 && snd_pcm_hw_params_set_format(pcm, hw, c.fmt) == 0) {
                    f = c;
                    fmtOk = true;
                    break;
                }
            }
        }
        const unsigned rate = hwRate;
        auto fail = [&](const std::string& e, const char* dopCode = "format") {
            setError(e);
            if (dopMode_) dopError_ = dopCode;
            snd_pcm_close(pcm);
            pcm = nullptr;
            reservation_.release();
            return false;
        };
        if (!fmtOk) {
            return fail(dopMode_ ? "a hangkártya nem fogad 32 vagy 24 bites egész mintát DoP-hoz"
                                 : "a hangkártya nem fogad 32, 24 vagy 16 bites egész mintát", "format");
        }
        if (!nativeDsd_) {
            if (snd_pcm_hw_params_set_channels(pcm, hw, 2) < 0) return fail("a hangkártya nem fogad sztereó jelet", "format");
            if (snd_pcm_hw_params_set_rate(pcm, hw, rate, 0) < 0)
                // DoP 768 kHz fölött (DSD1024: 2,8224 MHz-es PCM) gyakorlatilag egy DAC-on sem megy: külön ok a felületnek
                return fail("a hangkártya nem tudja ezt a mintavételt: " + std::to_string(rate) + " Hz",
                            (dopMode_ && rate > 768000u) ? "rate" : "format");
        }
        period = std::max<snd_pcm_uframes_t>(64, rate / 100);              // kb. 10 ms
        int dir = 0;
        snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, &dir);
        snd_pcm_uframes_t buffer = period * 4;
        snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
        if ((r = snd_pcm_hw_params(pcm, hw)) < 0) return fail(std::string("a beállítás nem sikerült: ") + snd_strerror(r));
        snd_pcm_hw_params_get_period_size(hw, &period, &dir);
        snd_pcm_hw_params_get_buffer_size(hw, &buffer);
        canPause = snd_pcm_hw_params_can_pause(hw) != 0;
        snd_pcm_sw_params_t* sw;
        snd_pcm_sw_params_alloca(&sw);
        snd_pcm_sw_params_current(pcm, sw);
        snd_pcm_sw_params_set_start_threshold(pcm, sw, std::min<snd_pcm_uframes_t>(buffer, period * 2));
        snd_pcm_sw_params_set_avail_min(pcm, sw, period);
        snd_pcm_sw_params(pcm, sw);
        snd_pcm_prepare(pcm);
        {
            std::lock_guard<std::mutex> lk(infoMutex_);
            openedDevice_ = dev;
            char buf[192];
            if (nativeDsd_) {
                std::snprintf(buf, sizeof(buf), "%s, natív DSD %s, DSD-ráta %u Hz (%u keret/s), %lu keret periódusonként", dev.c_str(),
                              f.name, rate * 8u * static_cast<unsigned>(nativeBytes_), rate, static_cast<unsigned long>(period));
                nativeName_ = f.name;
            } else {
                std::snprintf(buf, sizeof(buf), "%s, %s, %u Hz, %lu keret periódusonként", dev.c_str(), f.name, rate,
                              static_cast<unsigned long>(period));
                nativeName_.clear();
            }
            info_ = buf;
            error_.clear();
        }
        return true;
    }

    void run() {
        // Valós idejű prioritás (SCHED_FIFO, 70, vagy az RLIMIT_RTPRIO által engedett legnagyobb) kérése a kimeneti szálra.
        // Források (ellenőrizve: 2026-11-15):
        //   - pthread_setschedparam(3): https://man7.org/linux/man-pages/man3/pthread_setschedparam.3.html
        //   - sched(7):                 https://man7.org/linux/man-pages/man7/sched.7.html
        //   - getrlimit(2):             https://man7.org/linux/man-pages/man2/getrlimit.2.html
        {
            int prio = 70;
            struct rlimit rl{};
            if (getrlimit(RLIMIT_RTPRIO, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur > 0 && rl.rlim_cur < 70) {
                prio = static_cast<int>(rl.rlim_cur);
            }
            const int pmin = sched_get_priority_min(SCHED_FIFO);
            const int pmax = sched_get_priority_max(SCHED_FIFO);
            if (pmin >= 0 && pmax >= pmin) prio = std::clamp(prio, pmin, pmax);
            sched_param sp{};
            sp.sched_priority = prio;
            if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) {
                rtPriority_ = prio;
            } else {
                rtPriority_ = 0;
                static std::atomic<bool> rtFallbackLogged{false};
                if (!rtFallbackLogged.exchange(true)) {
                    std::cout << "[Lejátszó] Közvetlen ALSA-kimenet: valós idejű prioritás (SCHED_FIFO) nem érhető el, normál prioritással fut.\n";
                }
            }
        }
        snd_pcm_t* pcm = nullptr;
        AlsaFormat f;
        snd_pcm_uframes_t period = 0;
        bool canPause = false;
        const bool ok = openDevice(pcm, f, period, canPause);
        if (ok && quit_) {
            // az open() közben feladta a várakozást (időkorlát): nincs kinek dolgozni, mindent elengedünk
            snd_pcm_close(pcm);
            reservation_.release();
            return;
        }
        if (ok && dsp_ && !dopMode_) dsp_->prepare(rate_, static_cast<uint32_t>(period), f.ditherBits);
        open_ = ok;
        {
            std::lock_guard<std::mutex> lk(initMutex_);
            initState_ = ok ? 1 : -1;
        }
        initCv_.notify_all();
        if (!ok) return;
        std::cout << "[Lejátszó] Közvetlen ALSA-kimenet (kizárólagos" << (dopMode_ ? ", DoP" : "") << "): " << describe() << ".\n";

        std::vector<float> buf;
        std::vector<int32_t> dopBuf;
        // natív DSD: a DoP-szavakból csomagoló (előre lefoglalt pufferekkel, a hangszálon nincs foglalás)
        const size_t nb = static_cast<size_t>(nativeBytes_);
        NativeDsdPacker packer;
        if (nativeDsd_) {
            packer.setup(nativeBytes_, nativeBigEndian_, period);
            dopBuf.resize((period * nb / 2 + 1) * 2);
        } else if (dopMode_) dopBuf.resize(period * 2);
        else buf.resize(period * 2);
        std::vector<uint8_t> out(period * 2 * static_cast<size_t>(f.bits / 8));
        bool running = false, paused = false;
        uint64_t sinceFlush = 0;
        hb_.beat("alsa: start");
        hbRunning_ = true;
        while (!quit_) {
            hb_.beat("alsa: loop");
            reservation_.process();
            if (flushReq_.exchange(false)) {
                snd_pcm_drop(pcm);
                snd_pcm_prepare(pcm);
                running = paused = false;
                sinceFlush = 0;
                packer.reset();
            }
            if (!active_) {
                if (running && !paused) {
                    if (canPause && snd_pcm_pause(pcm, 1) == 0) paused = true;
                    else { snd_pcm_drop(pcm); snd_pcm_prepare(pcm); running = false; }
                }
                std::unique_lock<std::mutex> lk(wakeMutex_);
                wakeCv_.wait_for(lk, std::chrono::milliseconds(50), [this] { return quit_.load() || active_.load() || flushReq_.load(); });
                continue;
            }
            if (paused) { snd_pcm_pause(pcm, 0); paused = false; }
            if (nativeDsd_) {
                // a periódushoz kell `period · nb` bájt / csatorna; egy DoP-szó 2 bájtot ad (b0, majd b1, időrendben)
                const size_t dopFrames = packer.dopFramesFor(period);
                {
                    std::lock_guard<std::mutex> lk(renderMutex_);
                    if (renderDop_) renderDop_(dopBuf.data(), dopFrames);
                    else std::fill(dopBuf.begin(), dopBuf.begin() + static_cast<std::ptrdiff_t>(dopFrames * 2),
                                   dsd::packDopSample(dsd::kDsdSilenceByte, dsd::kDsdSilenceByte, 0x05u));
                }
                // a lejátszótól kapott DoP-folyam ugyanaz, mint DoP-kimenetnél: a lehallgatópont itt is látja
                if (dopTap_) dopTap_(dopBuf.data(), dopFrames);
                packer.pack(dopBuf.data(), period, out.data());
                if (rawTap_) rawTap_(out.data(), period * 2 * nb, f);
            } else if (dopMode_) {
                {
                    std::lock_guard<std::mutex> lk(renderMutex_);
                    if (renderDop_) renderDop_(dopBuf.data(), period);
                    else {
                        for (size_t i = 0; i < period; ++i) {
                            const uint8_t marker = ((written_ + i) & 1u) ? 0xFAu : 0x05u;
                            const int32_t w = dsd::packDopSample(dsd::kDsdSilenceByte, dsd::kDsdSilenceByte, marker);
                            dopBuf[2 * i] = w;
                            dopBuf[2 * i + 1] = w;
                        }
                    }
                }
                if (dopTap_) dopTap_(dopBuf.data(), period);
                // DoP módban a DSP-lánc, a digitális hangerő és a mérők/tap teljesen kimaradnak
                convertDop(dopBuf.data(), out.data(), period * 2, f);
                if (rawTap_) rawTap_(out.data(), period * 2 * static_cast<size_t>(f.bits / 8), f);
            } else {
                {
                    std::lock_guard<std::mutex> lk(renderMutex_);
                    if (render_) render_(buf.data(), period);
                    else std::fill(buf.begin(), buf.end(), 0.0f);
                }
                if (dsp_) dsp_->process(buf.data(), period);
                if (!pcm::floatToInt(buf.data(), out.data(), period * 2, f.bits, f.valid)) quantLoss_ = true;
            }
            const uint8_t* p = out.data();
            snd_pcm_uframes_t left = period;
            hb_.beat("alsa: writei");
            while (left > 0 && !quit_) {
                snd_pcm_sframes_t n = snd_pcm_writei(pcm, p, left);
                if (n == -EAGAIN) continue;
                if (n < 0) {
                    ++overruns_;
                    if (snd_pcm_recover(pcm, static_cast<int>(n), 1) < 0) { setError(snd_strerror(static_cast<int>(n))); quit_ = true; break; }
                    continue;
                }
                p += static_cast<size_t>(n) * 2 * static_cast<size_t>(f.bits / 8);
                left -= static_cast<snd_pcm_uframes_t>(n);
                sinceFlush += static_cast<uint64_t>(n);
                written_ += static_cast<uint64_t>(n);
            }
            running = true;
            snd_pcm_sframes_t delay = 0;
            if (snd_pcm_delay(pcm, &delay) < 0) delay = 0;
            const uint64_t d = delay > 0 ? static_cast<uint64_t>(delay) : 0;
            const uint64_t playedHw = sinceFlush > d ? sinceFlush - d : 0;
            // a lejátszó DoP-keretben számol: natív DSD-nél egy ALSA-keret nb / 2 DoP-keret
            consumed_ = nativeDsd_ ? playedHw * nb / 2 : playedHw;
        }
        hb_.beat("alsa: close");
        // DSD / DoP lezárása előtt 2 periódusnyi tiszta DSD/DoP csend (kDsdSilenceByte),
        // hogy a hardveres leállítás ne vágjon bele aktív 1-bites bitfolyamba.
        if (dopMode_ && running && !paused && period > 0) {
            if (nativeDsd_) {
                std::fill(out.begin(), out.end(), dsd::kDsdSilenceByte);
            } else {
                for (size_t i = 0; i < period; ++i) {
                    const uint8_t marker = ((written_ + i) & 1u) ? 0xFAu : 0x05u;
                    const int32_t w = dsd::packDopSample(dsd::kDsdSilenceByte, dsd::kDsdSilenceByte, marker);
                    dopBuf[2 * i] = w;
                    dopBuf[2 * i + 1] = w;
                }
                convertDop(dopBuf.data(), out.data(), period * 2, f);
            }
            for (int k = 0; k < 2; ++k) {
                const snd_pcm_sframes_t n = snd_pcm_writei(pcm, out.data(), period);
                if (n < 0) break;
                written_ += static_cast<uint64_t>(n);
            }
        }
        snd_pcm_drop(pcm);
        snd_pcm_close(pcm);
        reservation_.release();
        open_ = false;
        hbRunning_ = false;
    }

    std::string device_;
    ExclusiveDsp* dsp_ = nullptr;
    AudioDeviceReservation reservation_;
    RenderFn render_;
    RenderDopFn renderDop_;
    std::mutex renderMutex_;
    std::thread thread_;
    std::atomic<uint32_t> rate_{0};
    std::atomic<bool> active_{false}, quit_{false}, flushReq_{false}, open_{false}, quantLoss_{false}, dopMode_{false};
    // Őrszem (beépített hibajelentés): a kimeneti szál körönként jelez; csak futó szálnál várunk jelzést. A regisztráció
    // itt, a létrehozó szálon történik: a valós idejű szálon csak atomikus írás van.
    std::atomic<bool> hbRunning_{false};
    aq::wd::Heartbeat hb_{ "player-alsa", &hbRunning_ };
    std::atomic<bool> nativeDsd_{false}, preferNative_{true};
    int nativeBytes_ = 4;
    bool nativeBigEndian_ = true;
    std::string nativeName_;
    std::atomic<int> rtPriority_{0};
    std::atomic<uint64_t> written_{0}, consumed_{0};
    std::atomic<uint32_t> overruns_{0};
    std::function<void(const int32_t*, size_t)> dopTap_;
    std::function<void(const uint8_t*, size_t, const AlsaFormat&)> rawTap_;
    std::mutex initMutex_, wakeMutex_;
    std::condition_variable initCv_, wakeCv_;
    int initState_ = 0;
    std::function<int(snd_pcm_t**, const char*)> pcmOpenFn_;   // üres: a valódi snd_pcm_open (csak a teszt cseréli)
    int openRetryMs_ = 3000;    // ennyi ideig próbálkozunk foglalt eszköznél
    int initWaitMs_ = 10000;    // ennyit vár az open() / openDop() a szál megnyitására
    mutable std::mutex infoMutex_;
    mutable std::vector<uint32_t> cachedRates_;
    std::string info_, error_, openedDevice_, dopError_;
};

} // namespace ee_analyzer::player
