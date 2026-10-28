#pragma once
/**
 * @file play_queue.hpp
 * @brief A 2.x lejátszási sora: album, mappa, lista vagy számlista lejátszása egy adott számtól, következő / előző,
 *        a szám vége után a következő (azonos mintavételnél szünetmentesen, a FilePlayer szakaszaival); a sor
 *        szerkesztése, ismétlés és keverés.
 *
 * - A sor a motorban van: a telefon és a PC ugyanazt látja és vezérli. A tételek a zenetár azonosítói; az útvonalat
 *   a resolve függvény adja (a hálózatról útvonal nem érkezik).
 * - Minden tétel saját, állandó jelet kap (uid); a lejátszó ezt kapja a számhoz (token). Így szerkesztés (beszúrás,
 *   törlés, átrendezés, keverés) után is egyértelmű, melyik tétel szól; a régi sor jele az újjal nem keveredik.
 * - Szerkesztés után a lejátszó az előre betöltött következő számot eldobja (dropPrefetched), és a mostani szám végén
 *   újra kérdez: mindig a sor szerinti szám jön.
 * - Zárak: a lejátszó a következő számot a saját zárja alatt kéri (provider), ezért a sor a saját zárja alatt soha
 *   nem hívja a lejátszót (a zárak sorrendje: lejátszó, utána a sor).
 * - Mentés: a tételek, a hallott tétel, az ismétlés, a keverés és a keverés előtti sorrend (queue.json);
 *   újraindítás után a sor visszaáll, de magától nem indul el, és a lejátszót sem nyitja meg.
 */
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "file_player.hpp"
#include "play_stats.hpp"

namespace ee_analyzer::player {

struct QueueItem {
    std::string id, title, artist, album;
    double duration = 0.0;
};

class PlayQueue {
public:
    /** A zenetár azonosítójához a fájl útvonala; false: ismeretlen (a sor átugorja). */
    using Resolve = std::function<bool(const std::string& id, std::string& path)>;
    using Save = std::function<void(const std::string& doc)>;
    enum class Repeat { Off, All, One };

    /** Legfeljebb ennyi tétel lehet a sorban. */
    static constexpr size_t kMaxItems = 10000;
    /** Az „előző” ennyi másodperc lejátszás után a szám elejére ugrik (előtte az előző számra). */
    static constexpr double kRestartSeconds = 3.0;

    PlayQueue(FilePlayer& player, Resolve resolve, Save save = nullptr);
    ~PlayQueue();
    PlayQueue(const PlayQueue&) = delete;
    PlayQueue& operator=(const PlayQueue&) = delete;

    /** Új sor és lejátszás a start indexű tételtől (nem nyitható tételnél a következőtől). Keverésnél a start az első. */
    bool play(std::vector<QueueItem> items, size_t start, std::string* error = nullptr);
    /** Új sor beállítása a start indexű tételtől, azonnali lejátszás nélkül. */
    bool load(std::vector<QueueItem> items, size_t start, std::string* error = nullptr);
    /** Ugrás a sor egy tételére. */
    bool jump(size_t index, std::string* error = nullptr);
    /** Következő tétel (az utolsó után az egész sor ismétlésénél az első). */
    bool next();
    /** kRestartSeconds után (vagy az első tételnél) a szám elejére, különben az előző tételre. */
    bool previous();
    /** A "media" parancsok: next / previous a soré, a többi a lejátszóé. */
    void command(const std::string& action, double value);

    // ---- szerkesztés (false: hibás index, vagy túl sok tétel)
    /** Hozzáadás a hallott tétel után (next = true, „lejátszás következőként”), különben a sor végére. */
    bool add(std::vector<QueueItem> items, bool next);
    /** Tétel törlése; a hallott tétel törlésekor a helyére lépő szól (ha nincs ilyen, a lejátszás leáll). */
    bool remove(size_t index);
    bool move(size_t from, size_t to);
    /** A sor ürítése (a sorból szóló lejátszás leáll). */
    void clear();
    void setRepeat(Repeat mode);
    /** Keverés: a hallott tétel után következők véletlen sorrendben; kikapcsoláskor az eredeti sorrend áll vissza. */
    void setShuffle(bool on);
    Repeat repeat() const;
    bool shuffle() const;

    /** A „most szól” adatai a sor szerinti következő / előző lehetőséggel. */
    media::Info mediaInfo() const;
    /** {"type":"queue", rev, index, repeat, shuffle, items:[…]} (index -1: a lejátszó nem a sorból játszik). */
    std::string json() const;
    /** Az állapotüzenet "queue" mezője: {rev, index, count, repeat, shuffle}; üres, ha a sor üres. Sorszámváltáskor menti a sort. */
    std::string statusJson();
    /** A mentett sor visszaállítása (lejátszás nélkül). false: hibás vagy üres dokumentum. */
    bool restore(const std::string& doc);
    std::string doc() const;
    /** A hallott tétel indexe (-1: nincs, vagy a lejátszó nem a sorból játszik; lejátszás nélkül a mentett). */
    int currentIndex() const;
    size_t size() const;
    /** A sor tételei (a „sor mentése listaként”). */
    std::vector<QueueItem> items() const;

    static const char* repeatName(Repeat r);
    static bool parseRepeat(const std::string& s, Repeat& out);

    /**
     * Lejátszáskor (a ListenTracker szabálya szerint, lejátszásonként egyszer) hívódik a tétel zenetár-azonosítójával;
     * a napló (PlayStats) ezzel számol. A sor saját mérőszála hívja, a sor zárja nélkül.
     */
    void setPlayedCallback(std::function<void(const std::string& id)> played);

private:
    struct Entry {
        QueueItem item;
        uint64_t uid = 0;
    };

    int indexOfLocked(uint64_t uid) const;
    int currentIndexLocked(uint64_t token, bool hasTrack) const;
    /** A tétel megnyitása; nem nyitható tételnél dir irányban a következő (legfeljebb néhányat ugrik át). */
    bool openIndex(size_t index, int dir, std::string* error);
    void reindexLocked();
    void changedLocked();            // rev++, mentés
    std::string docLocked(int index) const;
    std::vector<Entry> makeEntriesLocked(std::vector<QueueItem> items);
    void shuffleAfterLocked(int keep);
    /** A sor változása után: a lejátszó előre betöltött része kiesik (a zár nélkül hívandó). */
    void refreshPlayer();

    FilePlayer& player_;
    Resolve resolve_;
    Save save_;
    mutable std::mutex m_;
    std::vector<Entry> items_;
    std::unordered_map<uint64_t, size_t> pos_;   // uid -> index
    std::vector<uint64_t> original_;             // keverésnél a keverés előtti sorrend
    uint64_t nextUid_ = 1;                       // a jelek soha nem 0-k (a 0 jel: nem a sorból)
    uint64_t rev_ = 0;                           // a tételek változásainak száma
    uint64_t currentUid_ = 0;                    // a legutóbb ismert hallott tétel (lejátszás nélkül a mentett)
    uint64_t savedUid_ = 0;                      // a legutóbb mentett hallott tétel
    Repeat repeat_ = Repeat::Off;
    bool shuffle_ = false;
    std::mt19937 rng_{ std::random_device{}() };

    // a hallott idő mérése (0,25 s-onként): a lejátszási naplóhoz
    void trackLoop();
    std::function<void(const std::string& id)> played_;
    ListenTracker tracker_;
    std::mutex tickM_;
    std::condition_variable tickCv_;
    bool quit_ = false;
    std::thread tick_;
};

} // namespace ee_analyzer::player
