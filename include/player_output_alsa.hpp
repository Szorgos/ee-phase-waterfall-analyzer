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

#include "exclusive_dsp.hpp"
#include "file_player.hpp"
#include "pcm_convert.hpp"

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
        }
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
        initCv_.wait(lk, [this] { return initState_ != 0; });
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
        std::lock_guard<std::mutex> lk(renderMutex_);
        render_ = nullptr;
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

    bool isOpen() const { return open_.load(); }
    bool quantExact() const { return !quantLoss_.load(); }
    /** A legutóbbi megnyitás leírása (eszköz, formátum) vagy a hiba oka. */
    std::string describe() const { std::lock_guard<std::mutex> lk(infoMutex_); return info_; }
    std::string lastError() const { std::lock_guard<std::mutex> lk(infoMutex_); return error_; }
    std::string device() const { std::lock_guard<std::mutex> lk(infoMutex_); return openedDevice_; }
    uint32_t overruns() const { return overruns_.load(); }
    /** A kimeneti szál valós idejű (SCHED_FIFO) prioritása, vagy 0, ha normál prioritással fut. */
    int realtimePriority() const { return rtPriority_.load(); }

private:
    void setError(const std::string& e) { std::lock_guard<std::mutex> lk(infoMutex_); error_ = e; }

    /** A hangkártya megnyitása és beállítása a szálon; false: a hiba oka a lastError()-ban. */
    bool openDevice(snd_pcm_t*& pcm, AlsaFormat& f, snd_pcm_uframes_t& period, bool& canPause) {
        std::string dev;
        { std::lock_guard<std::mutex> lk(infoMutex_); dev = device_; }
        if (dev.empty()) dev = defaultAlsaDevice();
        if (dev.empty()) { setError("nincs hangkártya"); return false; }
        std::string why;
        if (!reservation_.acquire(alsaCardIndex(dev), &why)) { setError(why); return false; }
        int r = snd_pcm_open(&pcm, dev.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
        if (r < 0) {
            setError(std::string("a hangkártya nem nyitható meg (") + dev + "): " + snd_strerror(r));
            reservation_.release();
            pcm = nullptr;
            return false;
        }
        snd_pcm_hw_params_t* hw;
        snd_pcm_hw_params_alloca(&hw);
        snd_pcm_hw_params_any(pcm, hw);
        snd_pcm_hw_params_set_rate_resample(pcm, hw, 0);                 // átalakítás nélkül
        snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
        const AlsaFormat formats[] = {
            { SND_PCM_FORMAT_S32_LE, 32, 32, 0, "32 bit" },
            { SND_PCM_FORMAT_S24_3LE, 24, 24, 24, "24 bit" },
            { SND_PCM_FORMAT_S16_LE, 16, 16, 16, "16 bit" },
        };
        bool fmtOk = false;
        for (const auto& c : formats) {
            if (snd_pcm_hw_params_test_format(pcm, hw, c.fmt) == 0 && snd_pcm_hw_params_set_format(pcm, hw, c.fmt) == 0) {
                f = c;
                fmtOk = true;
                break;
            }
        }
        const unsigned rate = rate_;
        auto fail = [&](const std::string& e) {
            setError(e);
            snd_pcm_close(pcm);
            pcm = nullptr;
            reservation_.release();
            return false;
        };
        if (!fmtOk) return fail("a hangkártya nem fogad 32, 24 vagy 16 bites egész mintát");
        if (snd_pcm_hw_params_set_channels(pcm, hw, 2) < 0) return fail("a hangkártya nem fogad sztereó jelet");
        if (snd_pcm_hw_params_set_rate(pcm, hw, rate, 0) < 0)
            return fail("a hangkártya nem tudja ezt a mintavételt: " + std::to_string(rate) + " Hz");
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
            std::snprintf(buf, sizeof(buf), "%s, %s, %u Hz, %lu keret periódusonként", dev.c_str(), f.name, rate,
                          static_cast<unsigned long>(period));
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
        if (ok && dsp_) dsp_->prepare(rate_, static_cast<uint32_t>(period), f.ditherBits);
        open_ = ok;
        {
            std::lock_guard<std::mutex> lk(initMutex_);
            initState_ = ok ? 1 : -1;
        }
        initCv_.notify_all();
        if (!ok) return;
        std::cout << "[Lejátszó] Közvetlen ALSA-kimenet (kizárólagos): " << describe() << ".\n";

        std::vector<float> buf(period * 2);
        std::vector<uint8_t> out(period * 2 * static_cast<size_t>(f.bits / 8));
        bool running = false, paused = false;
        uint64_t sinceFlush = 0;
        while (!quit_) {
            reservation_.process();
            if (flushReq_.exchange(false)) {
                snd_pcm_drop(pcm);
                snd_pcm_prepare(pcm);
                running = paused = false;
                sinceFlush = 0;
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
            {
                std::lock_guard<std::mutex> lk(renderMutex_);
                if (render_) render_(buf.data(), period);
                else std::fill(buf.begin(), buf.end(), 0.0f);
            }
            if (dsp_) dsp_->process(buf.data(), period);
            if (!pcm::floatToInt(buf.data(), out.data(), period * 2, f.bits, f.valid)) quantLoss_ = true;
            const uint8_t* p = out.data();
            snd_pcm_uframes_t left = period;
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
            }
            running = true;
            snd_pcm_sframes_t delay = 0;
            if (snd_pcm_delay(pcm, &delay) < 0) delay = 0;
            const uint64_t d = delay > 0 ? static_cast<uint64_t>(delay) : 0;
            consumed_ = sinceFlush > d ? sinceFlush - d : 0;
        }
        snd_pcm_drop(pcm);
        snd_pcm_close(pcm);
        reservation_.release();
        open_ = false;
    }

    std::string device_;
    ExclusiveDsp* dsp_ = nullptr;
    AudioDeviceReservation reservation_;
    RenderFn render_;
    std::mutex renderMutex_;
    std::thread thread_;
    std::atomic<uint32_t> rate_{0};
    std::atomic<bool> active_{false}, quit_{false}, flushReq_{false}, open_{false}, quantLoss_{false};
    std::atomic<int> rtPriority_{0};
    std::atomic<uint64_t> written_{0}, consumed_{0};
    std::atomic<uint32_t> overruns_{0};
    std::mutex initMutex_, wakeMutex_;
    std::condition_variable initCv_, wakeCv_;
    int initState_ = 0;
    mutable std::mutex infoMutex_;
    std::string info_, error_, openedDevice_;
};

} // namespace ee_analyzer::player
