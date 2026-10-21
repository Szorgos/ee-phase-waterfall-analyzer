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
 */
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aether_chain.hpp"
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
    friend struct Impl;
};

} // namespace ee_analyzer::mac
