#pragma once
/**
 * @file mic_capture.hpp
 * @brief Mikrofonfelvétel a szobaméréshez – platformfüggetlen felület.
 *
 * A mérés felépítése: a mérőjelet (sweep) a motor meglévő kimenete játssza le (a saját DSP-lánc nélkül), közben ez
 * a felvevő a kiválasztott bemenetről vesz fel. Mintapontos szinkron nem kell: a dekonvolúció a felvételben
 * megtalálja az impulzusválaszt, és mivel minden csatorna ugyanazon az úton mérődik, a csatornák közti késleltetés
 * is helyes.
 *
 * Megvalósítások: Linux / Pi – PipeWire (`mic_capture_pipewire.hpp`), Windows – WASAPI, macOS – Core Audio.
 * A felvétel monó (több csatornás bemenetnél az első csatorna), 32 bites lebegőpontos.
 */
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aqulify::room {

/** Egy bemeneti eszköz (mikrofon vagy hangkártya-bemenet). */
struct InputDevice {
    std::string id;        // a platform azonosítója (PipeWire: node.name; WASAPI: endpoint ID; Core Audio: UID)
    std::string name;      // a felhasználónak mutatott név
    bool isDefault = false;
    bool isMonitor = false;  // egy kimenet „monitora” (csak teszteléshez / visszacsatolt méréshez)
};

/**
 * Felvételi puffer: a valós idejű szál ír bele (foglalás nélkül), a vezérlő szál olvassa a felvétel végén.
 * A kapacitás a start előtt foglalódik; ha betelik, a további minták elvesznek (a hossz így is ismert).
 */
class CaptureBuffer {
public:
    void reset(size_t capacity) {
        data_.assign(capacity, 0.0f);
        written_.store(0, std::memory_order_relaxed);
        peak_.store(0.0f, std::memory_order_relaxed);
        overflow_.store(false, std::memory_order_relaxed);
    }
    /** A valós idejű szálról: n minta, `stride` lépésközzel (interleaved bemenetnél a csatornaszám). */
    void push(const float* src, size_t n, size_t stride = 1) {
        size_t w = written_.load(std::memory_order_relaxed);
        float pk = 0.0f;
        size_t i = 0;
        for (; i < n && w < data_.size(); ++i, ++w) {
            const float v = src[i * stride];
            data_[w] = v;
            const float a = std::fabs(v);
            if (a > pk) pk = a;
        }
        if (i < n) overflow_.store(true, std::memory_order_relaxed);
        written_.store(w, std::memory_order_release);
        // csúcsszint a szintjelzőhöz: lassan csengő maximum (a kiolvasó nullázza)
        float cur = peak_.load(std::memory_order_relaxed);
        while (pk > cur && !peak_.compare_exchange_weak(cur, pk, std::memory_order_relaxed)) {}
    }
    size_t written() const { return written_.load(std::memory_order_acquire); }
    bool overflowed() const { return overflow_.load(std::memory_order_relaxed); }
    /** A legutóbbi kiolvasás óta mért csúcs (0..1), és nullázza. */
    float takePeak() { return peak_.exchange(0.0f, std::memory_order_relaxed); }
    std::vector<float> take() {
        const size_t n = written();
        return std::vector<float>(data_.begin(), data_.begin() + static_cast<std::ptrdiff_t>(n));
    }

private:
    std::vector<float> data_;
    std::atomic<size_t> written_{0};
    std::atomic<float> peak_{0.0f};
    std::atomic<bool> overflow_{false};
};

class MicCapture {
public:
    virtual ~MicCapture() = default;

    /** A rendszer bemeneti eszközei. includeMonitors: a kimenetek monitorai is (teszt, visszacsatolt mérés). */
    virtual std::vector<InputDevice> listInputs(bool includeMonitors = false) = 0;

    /**
     * Felvétel indítása a megadott eszközről (üres id: az alapértelmezett), a kért mintavétellel (a rendszer
     * szükség szerint átalakítja), legfeljebb maxSeconds hosszan. false + error: "no_device", "busy", "permission",
     * "format", "backend".
     */
    virtual bool start(const std::string& deviceId, uint32_t sampleRate, double maxSeconds, std::string& error) = 0;

    /** A felvétel leállítása; visszaadja a felvett mintákat (monó). */
    virtual std::vector<float> stop() = 0;

    /** Fut-e a felvétel. */
    virtual bool running() const = 0;

    /** Szintjelzőhöz: a legutóbbi hívás óta mért csúcs (0..1, lineáris). */
    virtual float takePeak() = 0;

    /** Eddig felvett minták száma. */
    virtual size_t recordedFrames() const = 0;
};

/** A platform felvevője (nullptr, ha ezen a platformon nincs). */
std::unique_ptr<MicCapture> createMicCapture();

/** dBFS a lineáris csúcsból (−120 alatt −120). */
inline double peakDbfs(float peak) { return peak > 1e-6f ? 20.0 * std::log10(static_cast<double>(peak)) : -120.0; }

} // namespace aqulify::room
