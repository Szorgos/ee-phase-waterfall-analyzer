#pragma once
/**
 * @file watchdog.hpp
 * @brief Őrszem: a fontos szálak „szívverése” és a beragadt szál felismerése (beépített hibajelentés, H1).
 *
 * - Egy szál a futása elején regisztrál (Heartbeat, név szerint), és a ciklusa elején jelez: beat("bridge: loop").
 *   A jelzés foglalás- és zármentes: egy atomikus időbélyeg és egy atomikus mutató egy szövegkonstansra (a címke
 *   mindig szövegliterál, statikus élettartamú). A valós idejű szálon a címke nélküli beat() is elég.
 * - A Watchdog::check() (az őrszál hívja) a regiszter atomikus mezőit olvassa; a megfigyelt szálak zárait nem
 *   veszi fel, így egy beragadt szál nem akaszthatja meg.
 * - Az őrszem csak figyel és jelent: nem öl szálat, nem indít újra semmit.
 * - „expect”: ha meg van adva, a szál csak akkor számít beragadtnak, ha a jelző igaz (pl. a kimeneti visszahívás
 *   csak szóló hangfolyamnál fut).
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace aq::wd {

inline int64_t nowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr int kMaxSlots = 32;
constexpr size_t kNameLen = 24;

struct Slot {
    std::atomic<int> used{ 0 };                       // 0: szabad, 1: foglalt
    std::atomic<uint32_t> generation{ 0 };            // minden foglaláskor nő (az újrafelhasznált hely felismerése)
    char name[kNameLen] = {};
    std::atomic<int64_t> lastNs{ 0 };                 // az utolsó szívverés (0: még nem jelzett)
    std::atomic<const char*> label{ "" };             // az utolsó „most itt tartok” címke (szövegliterál)
    std::atomic<const std::atomic<bool>*> expect{ nullptr };
};

class Registry {
public:
    /** A folyamat közös regisztere (szándékosan soha nem szűnik meg: a kilépés közbeni szálak is jelezhetnek). */
    static Registry& global() {
        static Registry* r = new Registry();
        return *r;
    }

    Slot* acquire(const char* name, const std::atomic<bool>* expect) {
        std::lock_guard<std::mutex> lk(m_);   // csak regisztráláskor (a szál elején), nem a szívverésnél
        for (auto& s : slots_) {
            if (s.used.load(std::memory_order_relaxed)) continue;
            std::memset(s.name, 0, sizeof(s.name));
            std::strncpy(s.name, name ? name : "?", kNameLen - 1);
            s.lastNs.store(0, std::memory_order_relaxed);
            s.label.store("", std::memory_order_relaxed);
            s.expect.store(expect, std::memory_order_relaxed);
            s.generation.fetch_add(1, std::memory_order_relaxed);
            s.used.store(1, std::memory_order_release);
            return &s;
        }
        return nullptr;   // betelt: a szál nem figyelt (a motor ettől változatlanul működik)
    }

    void release(Slot* s) {
        if (!s) return;
        std::lock_guard<std::mutex> lk(m_);
        s->used.store(0, std::memory_order_release);
    }

    Slot* slots() { return slots_; }
    const Slot* slots() const { return slots_; }

private:
    std::mutex m_;
    Slot slots_[kMaxSlots];
};

/** Egy szál szívverése (RAII: a konstruktor regisztrál, a destruktor felszabadít). */
class Heartbeat {
public:
    explicit Heartbeat(const char* name, const std::atomic<bool>* expect = nullptr, Registry* reg = nullptr)
        : reg_(reg ? reg : &Registry::global()), slot_(reg_->acquire(name, expect)) {}
    ~Heartbeat() { reg_->release(slot_); }
    Heartbeat(const Heartbeat&) = delete;
    Heartbeat& operator=(const Heartbeat&) = delete;

    /** Szívverés címkével (a címke szövegliterál). Foglalás- és zármentes. */
    void beat(const char* label) noexcept {
        if (!slot_) return;
        slot_->label.store(label, std::memory_order_relaxed);
        slot_->lastNs.store(nowNs(), std::memory_order_release);
    }
    /** Szívverés csak időbélyeggel (valós idejű visszahíváshoz). */
    void beat() noexcept {
        if (slot_) slot_->lastNs.store(nowNs(), std::memory_order_release);
    }
    /** Csak a címke (pl. a regisztráláskor a valós idejű visszahívás helyett, amely utána csak beat()-et hív). */
    void setLabel(const char* label) noexcept {
        if (slot_) slot_->label.store(label, std::memory_order_relaxed);
    }
    bool registered() const { return slot_ != nullptr; }

private:
    Registry* reg_;
    Slot* slot_;
};

/** Egy esemény: új beragadás vagy helyreállás. */
struct Event {
    std::string thread;
    std::string label;
    double seconds = 0.0;   // ennyi ideje nem jelzett (helyreállásnál: a kiesés hossza)
    bool recovered = false;
};

class Watchdog {
public:
    explicit Watchdog(Registry& reg, double thresholdSec = 5.0) : reg_(reg), thresholdNs_(toNs(thresholdSec)) {}

    double threshold() const { return static_cast<double>(thresholdNs_) / 1e9; }
    void setThreshold(double sec) { thresholdNs_ = toNs(sec); }

    /** Egy ellenőrzés: az új beragadások (epizódonként egyszer) és a helyreállások. */
    std::vector<Event> check(int64_t now) {
        std::vector<Event> out;
        Slot* slots = reg_.slots();
        for (int i = 0; i < kMaxSlots; ++i) {
            Slot& s = slots[i];
            Track& t = track_[i];
            if (!s.used.load(std::memory_order_acquire)) { t = Track{}; continue; }
            const uint32_t gen = s.generation.load(std::memory_order_relaxed);
            if (gen != t.gen) t = Track{ gen, false, 0 };
            const int64_t last = s.lastNs.load(std::memory_order_acquire);
            const std::atomic<bool>* ex = s.expect.load(std::memory_order_relaxed);
            const bool expected = !ex || ex->load(std::memory_order_relaxed);
            if (last == 0) continue;   // még nem kezdett el dolgozni
            const int64_t age = now - last;
            if (expected && age > thresholdNs_ && !t.reported) {
                t.reported = true;
                t.since = last;
                out.push_back(Event{ copyName(s), copyLabel(s), static_cast<double>(age) / 1e9, false });
            } else if (t.reported && (age <= thresholdNs_ || !expected)) {
                t.reported = false;
                out.push_back(Event{ copyName(s), copyLabel(s), static_cast<double>(now - t.since) / 1e9, true });
            }
        }
        return out;
    }

    /** Az összes regisztrált szál állapota JSON-tömbként (az állapotfájlba). */
    std::string heartbeatsJson(int64_t now) const {
        std::string j = "[";
        bool first = true;
        const Slot* slots = reg_.slots();
        for (int i = 0; i < kMaxSlots; ++i) {
            const Slot& s = slots[i];
            if (!s.used.load(std::memory_order_acquire)) continue;
            const int64_t last = s.lastNs.load(std::memory_order_acquire);
            const std::atomic<bool>* ex = s.expect.load(std::memory_order_relaxed);
            const bool expected = !ex || ex->load(std::memory_order_relaxed);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%lld", last == 0 ? -1LL : static_cast<long long>((now - last) / 1000000));
            if (!first) j += ",";
            first = false;
            j += "{\"thread\":\"" + esc(copyName(s)) + "\",\"label\":\"" + esc(copyLabel(s)) + "\",\"age_ms\":" + buf +
                 ",\"expected\":" + (expected ? "true" : "false") + ",\"stalled\":" + (track_[i].reported ? "true" : "false") + "}";
        }
        return j + "]";
    }

    static std::string esc(const std::string& in) {
        std::string o;
        for (char c : in) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
            else o += c;
        }
        return o;
    }

private:
    struct Track {
        uint32_t gen = 0;
        bool reported = false;
        int64_t since = 0;
    };
    static int64_t toNs(double sec) { return static_cast<int64_t>(sec * 1e9); }
    static std::string copyName(const Slot& s) {
        char n[kNameLen];
        std::memcpy(n, s.name, kNameLen);
        n[kNameLen - 1] = 0;
        return n;
    }
    static std::string copyLabel(const Slot& s) {
        const char* l = s.label.load(std::memory_order_relaxed);
        return l ? l : "";
    }

    Registry& reg_;
    int64_t thresholdNs_;
    Track track_[kMaxSlots];
};

} // namespace aq::wd
