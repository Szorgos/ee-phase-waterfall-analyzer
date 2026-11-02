#pragma once
/**
 * @file mac_engine.hpp
 * @brief Aqulify motor macOS-re (Core Audio).
 *
 * A rendszerhang forrása:
 *   - Core Audio Tap (macOS 14.2+): az összes alkalmazás hangja (a saját folyamatunk nélkül), az eredeti
 *     kimenet némítva, amíg a motor olvassa; illesztőprogram nem kell, csak a rendszerhang-rögzítési engedély;
 *   - BlackHole (régebbi macOS, vagy ha a Tap nem engedélyezett): a rendszer kimenete a BlackHole 2ch virtuális
 *     eszköz, a motor annak bemenetéről olvas.
 * Mindkét esetben egy privát aggregált eszköz készül (fő eszköz: a valódi kimenet, mellette a forrás
 * sodródás-kiegyenlítéssel), így egyetlen IOProc olvas és ír, közös órajellel. A feldolgozás a közös
 * AetherChain (ugyanaz, mint Linuxon és Windowson).
 *
 * Több kimenet (2–4 hangkártya): a további kimeneti eszközök is az aggregált eszközbe kerülnek (sodródás-
 * kiegyenlítéssel, a Core Audio saját újramintavételezésével), így ugyanaz az IOProc írja őket, közös órán.
 * A DSP a MultiOutputEngine (független zónák vagy aktív keresztváltó), az 1. kimenet a fő lánc.
 */
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aether_chain.hpp"
#include "multi_output_config.hpp"
#include "param_block.hpp"
#include "tap_ring.hpp"

namespace ee_analyzer::mac {

enum class Source { None, ProcessTap, BlackHole };
/** Miért nem fut a feldolgozás (a felület ennek megfelelő útmutatót mutat). */
enum class Problem { None, Permission, NoSource, Device };

struct OutputDevice {
    std::string uid, name;
    bool isDefault = false;
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    AetherChain chain;
    StereoTapRing<65536> visualizerTap;
    ParamBlock params;

    /** Indítás (aszinkron, a motor saját sorában; a rendszerhang-engedély kérése is itt történik). */
    void start();
    /** Újrapróbálás, ha nem fut (pl. a felhasználó közben engedélyt adott, vagy telepítette a BlackHole-t). */
    void retry();
    /** Leállítás (szinkron): BlackHole módban a rendszer kimenete visszaáll az eredeti eszközre. */
    void stop();

    /** A kimeneti eszközök (BlackHole és a saját aggregált eszköz nélkül). */
    std::vector<OutputDevice> outputDevices() const;
    /** A választott kimenet ("" = a rendszer alapértelmezettje), és amin most szól. */
    std::string selectedOutputUid() const;
    std::string activeOutputUid() const;
    std::string activeOutputName() const;
    /** Kimenet választása (UID, vagy "" = a rendszer alapértelmezettje); az aggregált eszköz újraépül. */
    void setOutput(const std::string& uid);

    /**
     * Több kimenet: a beállítás és a 2–4. kimenet eszközének UID-je (üres: nincs). Eszközváltozásnál az aggregált
     * eszköz újraépül; DSP-változásnál csak a hangszál kapja meg az új beállítást.
     */
    void setMultiOutput(const multiout::MultiConfig& cfg, const std::array<std::string, 3>& targetUids);
    /** A 2–4. kimenet állapota JSON-ként: [{"output","target","streaming","underruns"}…]. */
    std::string multiStatusJson() const;
    /** A k-adik kimenet (1–3) saját lánca (pl. impulzusválasz egy zónának). */
    AetherChain& zoneChain(int k);

    /**
     * Pufferméret (mintában, 0: a rendszer alapértéke): az aggregált eszköz IO-puffere (kAudioDevicePropertyBufferFrameSize,
     * az eszköz által engedett tartományba igazítva). Az IOProc folyamatosan fut, így a hangkártya nem alszik el.
     */
    void setBufferFrames(uint32_t frames);
    /** A ténylegesen beállított IO-puffer mintában (0: még nincs eszköz). */
    uint32_t bufferFrames() const { return ioFrames_.load(); }
    /** A feldolgozás átlagos terhelése a puffer idejéhez képest (0–1), a csúcs az előző lekérdezés óta, a túllépések. */
    float dspLoad() const { return loadAvg_.load(std::memory_order_relaxed); }
    float takeDspPeak() { return loadPeak_.exchange(0.0f, std::memory_order_relaxed); }
    uint32_t overruns() const { return overruns_.load(std::memory_order_relaxed); }

    /**
     * A kimeneti eszköz hangereje (0–1, a Core Audio skálája, mint a menüsor hangerő-szabályzójáé) és némítása.
     * false: az eszköznek nincs állítható hangereje. level < 0 / mute < 0: az adott érték nem változik.
     */
    bool outputVolume(float& level, bool& muted) const;
    void setOutputVolume(float level, int mute);

    Source source() const { return source_.load(); }
    /** A forrás neve a felületnek ("Core Audio Tap", "BlackHole 2ch", vagy üres). */
    std::string sourceName() const;
    Problem problem() const { return problem_.load(); }
    bool processing() const { return processing_.load(); }
    uint32_t sampleRate() const { return sampleRate_.load(); }

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    std::atomic<Source> source_{Source::None};
    std::atomic<Problem> problem_{Problem::None};
    std::atomic<bool> processing_{false};
    std::atomic<uint32_t> sampleRate_{48000};
    std::atomic<uint32_t> wantFrames_{0}, ioFrames_{0};
    std::atomic<float> loadAvg_{0.0f}, loadPeak_{0.0f};
    std::atomic<uint32_t> overruns_{0};
    friend struct Impl;
};

} // namespace ee_analyzer::mac
