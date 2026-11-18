#pragma once
/**
 * @file file_player.hpp
 * @brief A 2.x beépített zenelejátszója: dekódolás egy saját szálon, a hang egy rendes lejátszó-hangfolyamon át
 *        arra a kimenetre megy, amelyen az Aqulify DSP fut (Linuxon az „Aqulify” virtuális eszköz), így a teljes
 *        DSP-láncon (EQ, A/B, mérők, vízesés) változatlanul átmegy.
 *
 * Szálak és zárak:
 *  - a vezérlés (open / play / pause / seek / stop, info) bármely szálról jöhet, a m_ zár alatt fut;
 *  - a dekódoló szál a gyűrűpuffert tölti (1 s), szintén a m_ alatt dekódol;
 *  - a kimenet szála a render()-t hívja, a kimenet saját zárja alatt (pl. pw_thread_loop), a m_-t soha nem veszi el.
 *  A zárak sorrendje mindig: m_, utána a kimenet zárja; így nincs holtpont.
 * A kimenet mindig sztereó: monó fájlnál a két csatorna azonos, kettőnél több csatornánál a bal / jobb az elülső,
 * a középső (-3 dB) és a hátsó csatornák (-3 dB) összege (ITU-szerű lekeverés).
 *
 * Szünetmentes váltás (lejátszási sor): ha a következő számot megadó függvény (setNextProvider) be van állítva, a
 * dekóder a fájl végén a következő számmal folytatja ugyanabban a pufferben, ha a mintavételi frekvenciája azonos;
 * a két szám mintapontosan egymás után szól. A számok egy közös idővonalon „szakaszok”; a „most szól” adatai és a
 * pozíció a hallott szakaszé, így a váltás a ténylegesen hallott ponton látszik. Eltérő mintavételnél a következő
 * szám a mostani vége után új hangfolyamon indul (rövid szünettel).
 *
 * ReplayGain (setReplayGain): a szám vagy az album hangosságcímkéje szerinti erősítés, előerősítéssel; a csúcs-címke
 * alapján a túlvezérlés elleni korláttal. Kikapcsolva a minták érintetlenek (bitpontos lejátszás). Előbetöltés
 * (setPreload): a fájl lejátszás előtt a memóriába (Linuxon a RAM-ban lévő futásidejű mappába) másolódik, így a
 * lejátszás alatt nincs lemez- vagy hálózati olvasás (pl. elalvó külső lemez, NAS).
 */
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_decoder.hpp"
#include "dsd.hpp"
#include "media_session.hpp"
#include "upsample_engine.hpp"

namespace ee_analyzer::player {

/** A platform hangkimenete (Linux: PipeWire-hangfolyam). A render a kimenet saját szálán, a zárja alatt fut. */
class PlayerOutput {
public:
    using RenderFn = std::function<void(float* stereo, size_t frames)>;
    using RenderDopFn = std::function<void(int32_t* stereoDop24, size_t frames)>;
    virtual ~PlayerOutput() = default;
    /** Új hangfolyam a megadott mintavételi frekvenciával (2 csatorna, float); inaktív állapotban indul. */
    virtual bool open(uint32_t sampleRate, RenderFn render, std::string* error) = 0;
    /**
     * Natív DSD (DoP 1.1) hangfolyam megnyitása (2 csatorna, 24 bites DoP szavak 32 bites egészben az alsó 24 biten:
     * [23:16]=0x05/0xFA marker, [15:8]=b0, [7:0]=b1; a kimenet 24-in-32 MSB-igazítottan adja át az eszköznek).
     * Ha a kimenet nem támogatja vagy az eszköz elutasítja a formátumot, false (error: az ok), és a lejátszó PCM-re lép vissza.
     */
    virtual bool openDop(uint32_t dopSampleRate, RenderDopFn render, std::string* error) {
        (void)dopSampleRate;
        (void)render;
        if (error) *error = "format";
        return false;
    }
    virtual void close() = 0;
    /** false: szünet; a kimenet a már átvett, még meg nem szólalt mintákat megtartja (folytatáskor azok szólnak). */
    virtual void setActive(bool active) = 0;
    /** Amíg zárva van, a render nem fut (a lejátszó ilyenkor üríti a puffert tekeréskor). */
    virtual void lock() = 0;
    virtual void unlock() = 0;
    /** Tekerés: a kimenet saját pufferének eldobása (a lock() alatt hívódik). */
    virtual void flush() {}
    /**
     * A hangeszköz által az utolsó megnyitás / flush óta ténylegesen megszólaltatott keretek (a kimenet pufferében
     * várakozók nélkül); UINT64_MAX: nem ismert (ilyenkor a pozíció a kimenetnek átadott keretekből számol).
     */
    virtual uint64_t consumedFrames() const { return UINT64_MAX; }
    /**
     * Kizárólagos (bitpontos) hangfolyam szól-e: ilyenkor a DAC a mintavétel váltásakor új órajelre áll (relék, PLL),
     * és ez alatt a szám eleje csattanhat vagy levágódhat. Ha igaz, a lejátszó az új hangfolyamot rövid csenddel
     * kezdi (FilePlayer::kRateSettleMs). Megosztott módban a rendszer keverője igazít, a DAC nem vált: false.
     */
    virtual bool settlesOnRateChange() const { return false; }
    /** A kimenet által támogatott mintavételi frekvenciák (alapértelmezett: 44.1..192k). */
    virtual std::vector<uint32_t> supportedRates() const {
        return { 44100, 48000, 88200, 96000, 176400, 192000 };
    }
    /** A hardver / keverő ténylegesen futó mintavétele (0: a kért mintavétel érvényes). */
    virtual uint32_t actualRate() const { return 0; }
};

class FilePlayer {
public:
    enum class State { Idle, Playing, Paused, Ended };

    /**
     * A következő szám a sorból: afterToken a mostani szám jele; true esetén path és token a következőé. A dekódoló
     * szál hívja, a lejátszó zárja alatt: a függvény nem hívhatja vissza a lejátszót.
     */
    using NextProvider = std::function<bool(uint64_t afterToken, std::string& path, uint64_t& token)>;

    explicit FilePlayer(std::unique_ptr<PlayerOutput> out);
    ~FilePlayer();
    FilePlayer(const FilePlayer&) = delete;
    FilePlayer& operator=(const FilePlayer&) = delete;

    /**
     * A fájl megnyitása és lejátszása (az előző leáll). token: a sor jele a számhoz (0: nem a sorból).
     * false: nem nyitható (error: a magyarázat).
     */
    bool open(const std::string& path, std::string* error = nullptr, uint64_t token = 0, bool bypassUpsample = false);
    void setNextProvider(NextProvider next);
    /**
     * A sor megváltozott: az előre betöltött következő szám(ok) kiesnek, és a lejátszó a mostani szám végén újra
     * kérdezi a következőt. A kimenetnek már átadott minták maradnak (a hallott szám nem szakad meg).
     */
    void dropPrefetched();
    /** A hallott szám jele (0: nincs szám, vagy nem a sorból). */
    uint64_t currentToken() const;
    void play();
    void pause();
    void toggle();
    void stop();
    /** Tekerés másodpercre (csak ismert hosszú fájlnál). */
    void seek(double seconds);
    /** A "media" parancsok (play / pause / toggle / seek); a next / previous a lejátszási soré (PlayQueue). */
    void command(const std::string& action, double value);

    bool hasTrack() const { return state_.load() != State::Idle; }
    State state() const { return state_.load(); }
    double position() const;
    double duration() const;
    /** ReplayGain: mode 0 = ki, 1 = szám, 2 = album (ha nincs albumérték, a számé); az új beállítás a még nem
     *  dekódolt részre hat. */
    void setReplayGain(int mode, float preampDb, bool preventClip);
    /** Előbetöltés a memóriába (a következő megnyitott számtól; legfeljebb kPreloadMaxBytes méretű fájlnál). */
    void setPreload(bool on) { preload_ = on; }
    /** Felmintavételezés bekapcsolása (vagy kikapcsolása). */
    void setUpsample(bool on);
    /** Felmintavételezés beállításainak átadása az AudioOptions alapján. */
    void setUpsampleOptions(const audioopt::AudioOptions& opts);
    bool isUpsampleActive() const { return upsampleActive_.load(); }
    uint32_t upsampleSrcRate() const { return upsampleSrcRate_.load(); }
    uint32_t upsampleTargetRate() const { return upsampleTargetRate_.load(); }
    uint64_t upsampleClippedSamples() const;
    uint64_t upsampleProcessedFrames() const;
    double upsampleDelayMs() const;
    std::string upsampleReason() const;
    /** A felmintavételezés ténylegesen használt szálainak száma (1 vagy 2; 0, ha nincs átalakítás). */
    int upsampleThreadsInUse() const { return upsampleActive_.load() ? upsampleThreadsUsed_.load() : 0; }
    /** Hangkimaradások: a kimenet több keretet kért, mint amennyi a pufferben volt (a fájl vége előtt). */
    uint64_t underruns() const { return underruns_.load(std::memory_order_relaxed); }
    /** A dekóder-szál tényleges rögzítése (-1: nincs rögzítve; csak Linux / Windows). */
    int dedicatedCoreApplied() const { return coreApplied_.load(); }
    static constexpr uint64_t kPreloadMaxBytes = 400ull << 20;
    /** Natív DSD (DoP 1.1) kérése (csak kizárólagos kimeneten; elutasításkor a lejátszó PCM-re lép vissza). */
    void setDsdDop(bool on);
    bool dsdDop() const { return dsdDop_.load(); }
    /** Most ténylegesen DoP-kereteket ad-e a lejátszó. */
    bool isDopActive() const { return dopActive_.load(); }
    /** A szóló DSD-fájl eredeti 1 bites mintavétele (pl. 2822400 DSD64-nél; 0, ha nem DSD). */
    uint32_t dopDsdRate() const { return dopDsdRate_.load(); }
    /** Ha a DoP nem indult el és a lejátszó PCM-re lépett vissza, az elutasítás oka (pl. "format", "busy"). */
    std::string dopFallbackReason() const;
    /** A hallott szám ReplayGain-erősítése dB-ben (0: nincs változtatás). */
    float currentGainDb() const;

    /** A hangfolyam mintavétele (a hallott fájlé); 0: nincs szám. */
    uint32_t streamRate() const;
    /** A „most szól” adatai a meglévő protokoll szerint (media::toJson). */
    media::Info info() const;
    /** A borító, ha az azonosító a mostani száméval egyezik. */
    bool art(const std::string& id, std::string& mime, std::string& bytes) const;

    /** A kimenet szála hívja: frames sztereó keret; ha nincs elég adat, csenddel tölt. */
    void render(float* stereo, size_t frames);
    /** A kimenet szála hívja DoP módban: frames sztereó 24 bites DoP szó; csendnél kDsdSilenceByte (0xAA) csomagolva. */
    void renderDop(int32_t* stereoDop24, size_t frames);
    /** A pufferben lévő, még meg nem szólaltatott keretek (diagnosztika, tesztek). */
    size_t bufferedFrames() const { return static_cast<size_t>(wr_.load() - rd_.load()); }
    /** A dekóder elérte-e a fájl végét (a puffer ettől még tartalmazhat adatot). */
    bool decodeAtEnd() const { return eof_.load(); }

    /**
     * Kattanásmentes váltás kizárólagos módban: minden új hangfolyam (első indítás, eltérő mintavételű szám) és a
     * leállt kimenet újraindítása (a szám vége után új szám) kRateSettleMs ezredmásodperc csenddel kezd, amíg a DAC
     * az órajelre áll és elnémítását feloldja. Azonos mintavételű új számnál a hangfolyam nem zárul be (open() a
     * meglévőn folytatja), így a szóló zene közbeni számváltás azonnali és csend nélküli; a szünetmentes váltás is.
     */
    static constexpr uint32_t kRateSettleMs = 800;
    /** Az új hangfolyam elején még hátralévő csend kerete (tesztekhez). */
    uint64_t leadInFrames() const { return leadIn_.load(); }
    uint32_t decideTargetRate(uint32_t srcRate, bool bypassUpsample, bool isDsd) const;

private:
    /** Egy szám az idővonalon (az idővonal a tekerés / megnyitás óta folyamatos, az első szám kereteiben indul). */
    /** Az előbetöltött fájl ideiglenes másolata (megszűnéskor törlődik). */
    struct TempCopy {
        std::string path;
        TempCopy() = default;
        explicit TempCopy(std::string p) : path(std::move(p)) {}
        TempCopy(const TempCopy&) = delete;
        TempCopy& operator=(const TempCopy&) = delete;
        ~TempCopy();
    };
    struct Segment {
        // A sorrend számít: a tagok fordított sorrendben szűnnek meg, így előbb a dekóder zárja be a fájlt, és csak
        // utána törlődik az előbetöltött másolat (Windowson nyitott fájl nem törölhető)
        std::unique_ptr<TempCopy> temp;     // előbetöltésnél a memóriába másolt fájl
        std::unique_ptr<AudioDecoder> dec;
        std::unique_ptr<dsd::DsdRawSource> dopRaw; // natív DSD (DoP) módban a nyers MSB-first DSD bájtforrás
        uint32_t dopRate = 0;               // DoP keret-mintavétel (pl. 176400 DSD64-nél)
        uint32_t dsdRate = 0;               // eredeti 1 bites DSD mintavétel (pl. 2822400)
        uint64_t dopTotal = 0;              // a fájl hossza DoP keretben (1 keret = 2 DSD bájt / csatorna)
        std::string dopFallback;            // ha a DoP nem nyitható, a visszalépés oka
        float gain = 1.0f;                  // ReplayGain (lineáris)
        std::string path, title, artId;
        std::string coverMime, cover;       // a mappa képe, ha a fájlba nincs borító ágyazva
        uint32_t channels = 0;
        uint64_t total = 0;                 // a fájl hossza keretben (0: ismeretlen)
        uint64_t start = 0;                 // az idővonalon az első kerete
        uint64_t end = UINT64_MAX;          // az idővonalon az utolsó utáni kerete (a dekódolás végén derül ki)
        uint64_t token = 0;
        bool bypassUpsample = false;
    };

    std::unique_ptr<Segment> makeSegment(const std::string& path, std::string* error, uint64_t token, bool bypassUpsample = false);
    float gainFor(const TrackTags& t) const;
    void decodeLoop();
    void stopLocked();
    void seekLocked(uint64_t frame);
    /** Új hangfolyam az első szakasszal (megnyitás, illetve eltérő mintavételű / módú következő szám). */
    bool startStreamLocked(std::unique_ptr<Segment> seg, std::string* error, bool modeChanged = false);
    /** Új szám a meglévő hangfolyamon (azonos mintavétel, kizárólagos mód); streamStopped: a kimenet állt. */
    void replaceTrackLocked(std::unique_ptr<Segment> seg, bool streamStopped);
    /** Dekódolás a puffer szabad részébe; false: az idővonal vége (nincs több azonos mintavételű szám). */
    bool fillLocked();
    /** A fájl végén: a következő szám a sorból (azonos mintavételnél a szakaszok végére, különben a pending_-be). */
    bool appendNextLocked();
    /** A ténylegesen hallott pozíció keretben: a tekerés helye + a kimenet által már megszólaltatott rész. */
    uint64_t audibleFrame() const;
    /** A hallott szakasz indexe a segs_-ben. */
    size_t audibleIndexLocked() const;
    const Segment* audibleLocked() const;

    std::unique_ptr<PlayerOutput> out_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::thread thread_;
    bool quit_ = false;

    std::deque<Segment> segs_;              // elöl a hallott (vagy korábbi), hátul a dekódolás alatt álló szám
    std::unique_ptr<Segment> pending_;      // a következő szám eltérő mintavétellel: a mostani vége után indul
    NextProvider next_;
    uint32_t rate_ = 0;
    std::vector<float> decodeBuf_;

    // gyűrűpuffer (sztereó keretek); írás: dekódoló szál, olvasás: kimenet szála
    std::vector<float> ring_;
    std::vector<int32_t> dopRing_;          // DoP módban: sztereó 24 bites DoP szavak (32 bites egészben)
    std::vector<uint8_t> dopRawBuf_;        // nyers DSD bájtok (interleaved)
    std::vector<uint8_t> dopPackBuf_;       // csatornánként 2 egymást követő DSD bájt a packDopFrames számára
    std::vector<int32_t> dopFrameBuf_;      // ideiglenes DoP keretpuffer
    uint8_t dopPhase_ = 0;                  // a dekóder DoP-jelölő fázisa (0 -> 0x05, 1 -> 0xFA)
    uint8_t dopRenderPhase_ = 0;            // a kimeneti szál DoP-jelölő fázisa (folytonos csend/keret váltakozás)
    size_t ringFrames_ = 0;
    std::atomic<uint64_t> wr_{0}, rd_{0};
    std::atomic<bool> eof_{false}, finished_{false};
    std::atomic<uint64_t> played_{0};   // a kimenetnek átadott keretek (a fájl elejétől számolva)
    std::atomic<uint64_t> base_{0};     // az utolsó tekerés (vagy a megnyitás) helye
    std::atomic<State> state_{State::Idle};
    std::atomic<int> rgMode_{0};
    std::atomic<float> rgPreampDb_{0.0f};
    std::atomic<bool> rgPreventClip_{true};
    std::atomic<bool> preload_{false};
    std::atomic<bool> dsdDop_{false};
    std::atomic<bool> dopActive_{false};
    std::atomic<uint32_t> dopDsdRate_{0};
    std::string dopFallback_;
    // kattanásmentes váltás (lásd kRateSettleMs): a hátralévő és az összes csend a hangfolyam elején
    std::atomic<uint64_t> leadIn_{0}, leadTotal_{0};

    // Felmintavételezés belső állapota és átalakító motorja
    audioopt::AudioOptions upsampleOpts_;
    bool upsampleEnabled_ = false;
    std::atomic<bool> upsampleActive_{false};
    std::atomic<uint32_t> upsampleSrcRate_{0};
    std::atomic<uint32_t> upsampleTargetRate_{0};
    mutable std::string upsampleReason_{ "off" };
    UpsampleEngine upsampler_;
    std::vector<float> upsampleOutBuf_;
    std::atomic<int> upsampleThreadsUsed_{1};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<int> coreWanted_{-1};    // a beállítás szerint kért mag (-1: nincs rögzítés)
    std::atomic<int> coreApplied_{-1};   // a dekóder-szálon ténylegesen beállított mag
    std::vector<float> stereoMixBuf_;
};

} // namespace ee_analyzer::player
