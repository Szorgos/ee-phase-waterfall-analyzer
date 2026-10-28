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
#include "media_session.hpp"

namespace ee_analyzer::player {

/** A platform hangkimenete (Linux: PipeWire-hangfolyam). A render a kimenet saját szálán, a zárja alatt fut. */
class PlayerOutput {
public:
    using RenderFn = std::function<void(float* stereo, size_t frames)>;
    virtual ~PlayerOutput() = default;
    /** Új hangfolyam a megadott mintavételi frekvenciával (2 csatorna, float); inaktív állapotban indul. */
    virtual bool open(uint32_t sampleRate, RenderFn render, std::string* error) = 0;
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
    bool open(const std::string& path, std::string* error = nullptr, uint64_t token = 0);
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
    /** A „most szól” adatai a meglévő protokoll szerint (media::toJson). */
    media::Info info() const;
    /** A borító, ha az azonosító a mostani száméval egyezik. */
    bool art(const std::string& id, std::string& mime, std::string& bytes) const;

    /** A kimenet szála hívja: frames sztereó keret; ha nincs elég adat, csenddel tölt. */
    void render(float* stereo, size_t frames);
    /** A pufferben lévő, még meg nem szólaltatott keretek (diagnosztika, tesztek). */
    size_t bufferedFrames() const { return static_cast<size_t>(wr_.load() - rd_.load()); }
    /** A dekóder elérte-e a fájl végét (a puffer ettől még tartalmazhat adatot). */
    bool decodeAtEnd() const { return eof_.load(); }

private:
    /** Egy szám az idővonalon (az idővonal a tekerés / megnyitás óta folyamatos, az első szám kereteiben indul). */
    struct Segment {
        std::unique_ptr<AudioDecoder> dec;
        std::string path, title, artId;
        std::string coverMime, cover;       // a mappa képe, ha a fájlba nincs borító ágyazva
        uint32_t channels = 0;
        uint64_t total = 0;                 // a fájl hossza keretben (0: ismeretlen)
        uint64_t start = 0;                 // az idővonalon az első kerete
        uint64_t end = UINT64_MAX;          // az idővonalon az utolsó utáni kerete (a dekódolás végén derül ki)
        uint64_t token = 0;
    };

    static std::unique_ptr<Segment> makeSegment(const std::string& path, std::string* error, uint64_t token);
    void decodeLoop();
    void stopLocked();
    void seekLocked(uint64_t frame);
    /** Új hangfolyam az első szakasszal (megnyitás, illetve eltérő mintavételű következő szám). */
    bool startStreamLocked(std::unique_ptr<Segment> seg, std::string* error);
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
    size_t ringFrames_ = 0;
    std::atomic<uint64_t> wr_{0}, rd_{0};
    std::atomic<bool> eof_{false}, finished_{false};
    std::atomic<uint64_t> played_{0};   // a kimenetnek átadott keretek (a fájl elejétől számolva)
    std::atomic<uint64_t> base_{0};     // az utolsó tekerés (vagy a megnyitás) helye
    std::atomic<State> state_{State::Idle};
};

} // namespace ee_analyzer::player
