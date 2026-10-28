#pragma once
/**
 * @file smart_lists.hpp
 * @brief Okos listák (2.x, 3c2): szabályok, amelyekből a motor minden lekéréskor kiszámolja a lista tartalmát a
 *        zenetárból és a lejátszási naplóból.
 *
 * Szabályok (JSON): {"match": "all" | "any", "conditions": [{"field", "op", "value"}…], "sort", "desc", "limit"}
 *   szöveg:  artist, album, title, genre        op: contains, notContains, is, isNot, startsWith (kis- és nagybetű
 *                                                  nem számít, az ékezetes betűknél sem)
 *   szám:    year, duration (s), plays          op: eq, ne, gt, ge, lt, le
 *   idő:     added, lastPlayed                  op: inLast, notInLast (value: nap); lastPlayed: never (value nélkül)
 *   sort:    added, lastPlayed, plays, year, artist, album, title, duration, random (alapból: added)
 *   limit:   0 = nincs korlát (legfeljebb a sor határa, 10 000)
 * Feltétel nélkül minden szám beletartozik. Az egyenlő rendezési kulcsú számok az album sorrendjében (előadó, album,
 * lemez, sorszám) maradnak.
 */
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "mini_json.hpp"
#include "music_library.hpp"
#include "play_queue.hpp"
#include "play_stats.hpp"

namespace ee_analyzer::library {

/** A sor tétele a zenetár egy számából (cím nélkül a fájlnév, kiterjesztés nélkül). */
inline player::QueueItem queueItem(const Track& t) {
    player::QueueItem it;
    it.id = t.id;
    it.title = t.title;
    if (it.title.empty()) {
        it.title = t.file;
        const size_t dot = it.title.find_last_of('.');
        if (dot != std::string::npos && dot > 0) it.title.resize(dot);
    }
    it.artist = t.artist;
    it.album = t.album;
    it.duration = t.duration;
    return it;
}

struct SmartCondition {
    std::string field, op, text;
    double number = 0.0;
};

struct SmartRules {
    bool any = false;
    std::vector<SmartCondition> conditions;
    std::string sort = "added";
    bool desc = true;
    size_t limit = 0;
};

/** A szabályok ellenőrzése és beolvasása; false: hibás (error: a hibás rész rövid leírása). */
bool parseSmartRules(const json::Value& v, SmartRules& out, std::string& error);
/** A szabályok egységes JSON alakja (ezt tárolja a lista). */
std::string smartRulesJson(const SmartRules& r);
/** A lista tartalma a szabályok szerint; now: Unix-idő (s). */
std::vector<player::QueueItem> evaluateSmart(const SmartRules& r, const Index& idx,
                                             const std::unordered_map<std::string, player::TrackStats>& stats, int64_t now);
/** Kisbetűs alak (ASCII, Latin-1 és Latin Extended-A, pl. „Ő” -> „ő”) a kis- és nagybetűt nem figyelő összevetéshez. */
std::string foldCase(const std::string& s);

/** A beépített alaplisták (név, szabályok JSON), az első induláskor kerülnek a listák közé. hungarian: a nevek nyelve. */
std::vector<std::pair<std::string, std::string>> defaultSmartLists(bool hungarian);

} // namespace ee_analyzer::library
