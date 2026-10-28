#pragma once
/**
 * @file playlists.hpp
 * @brief A 2.x lejátszási listái: a motor tárolja őket (playlists.json), így a telefon és a PC ugyanazt látja, a zene
 *        másolása nélkül.
 *
 * - A lista a zenetár azonosítóira hivatkozik; minden számnál a cím, az előadó, az album és a hossz is mentődik, így
 *   a lista akkor is olvasható, ha a zenetár éppen újraindexel, vagy egy fájl már nincs meg (a lejátszáskor a sor
 *   átugorja).
 * - Az azonosító „p” + 16 hexa jegy (véletlen), átnevezéskor nem változik.
 * - Okos lista (3c2): számok helyett szabályok (smart_lists.hpp); a tartalmat a híd számolja lekéréskor. Az első
 *   induláskor a négy alaplista kerül fel (seedOnce), a mentésben a "seeded" jelzi, hogy törlés után se jöjjenek vissza.
 * - Minden módosítás után mentés (a save függvény); a rev a változások száma (a kliens ebből látja, hogy újra kell
 *   kérnie).
 */
#include <cstdint>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "play_queue.hpp"

namespace ee_analyzer::player {

struct Playlist {
    std::string id, name;
    int64_t created = 0, updated = 0;   // Unix-idő (s)
    std::vector<QueueItem> items;       // okos listánál üres (a tartalmat a szabályok adják)
    bool smart = false;
    std::string rules;                  // okos lista: a szabályok egységes JSON alakja (smart_lists.hpp)
};

class PlaylistStore {
public:
    using Save = std::function<void(const std::string& doc)>;
    static constexpr size_t kMaxLists = 1000;
    static constexpr size_t kMaxItems = 10000;
    static constexpr size_t kMaxName = 200;   // bájt (UTF-8)

    explicit PlaylistStore(Save save = nullptr);

    /** A mentett dokumentum betöltése (mentés nélkül). false: hibás dokumentum (a tároló üres marad). */
    bool load(const std::string& doc);
    std::string doc() const;

    // A műveletek eredménye: üres szöveg = sikerült, különben a hiba kódja
    // ("name", "unknown", "full", "index", "empty", "smart": okos listához kézzel nem adható szám, "notSmart").
    std::string create(const std::string& name, std::vector<QueueItem> items, std::string& id);
    /** Okos lista (a szabályokat a hívó ellenőrzi, és az egységes JSON alakot adja). */
    std::string createSmart(const std::string& name, const std::string& rules, std::string& id);
    std::string setRules(const std::string& id, const std::string& rules);
    /** A beépített alaplisták (név, szabályok) egyszer, az első induláskor; később a felhasználó törölheti őket. */
    void seedOnce(const std::vector<std::pair<std::string, std::string>>& lists);
    std::string rename(const std::string& id, const std::string& name);
    std::string remove(const std::string& id);
    /** Hozzáadás a position helyére (a lista végénél nagyobb vagy negatív: a végére). */
    std::string add(const std::string& id, std::vector<QueueItem> items, long position = -1);
    /** Tételek törlése (az indexek sorrendje mindegy, a hibás indexek kimaradnak). */
    std::string removeItems(const std::string& id, std::vector<size_t> indices);
    std::string move(const std::string& id, size_t from, size_t to);

    bool get(const std::string& id, Playlist& out) const;
    std::vector<Playlist> all() const;
    size_t count() const;
    uint64_t rev() const;

    /** A név ellenőrzése és tisztítása (szóközök a két végén; legfeljebb kMaxName bájt, karakterhatáron vágva). */
    static std::string cleanName(const std::string& name);

private:
    Playlist* findLocked(const std::string& id);
    void changedLocked();
    std::string docLocked() const;
    std::string newIdLocked();

    Save save_;
    mutable std::mutex m_;
    std::vector<Playlist> lists_;
    uint64_t rev_ = 0;
    bool seeded_ = false;               // az alaplisták már felkerültek (a mentésben is)
    std::mt19937_64 rng_{ std::random_device{}() };
};

} // namespace ee_analyzer::player
