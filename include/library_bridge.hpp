#pragma once
/**
 * @file library_bridge.hpp
 * @brief A zenetár bekötése a WebSocket-hídba (2.x), a Linuxos és a Windows-os motor közös kódja.
 *
 * Üzenetek (a protokoll leírása: docs/remote-api.md, „Zenetár”):
 *   library_info                         -> library (a mappák listája csak helyi kliensnek)
 *   library_set_roots {roots:[…]}        csak helyi kliens: a zenei mappák beállítása, mentés, újrabejárás -> library
 *   library_rescan                       csak helyi kliens -> library
 *   library_albums {offset, limit}       -> library_albums
 *   library_album {id}                   -> library_album (ismeretlen azonosítónál library_error)
 *   library_folder {id}                  -> library_folder (üres azonosító: a beállított mappák)
 *   library_play {id}                    egy szám lejátszása azonosító alapján (egytételes sor) -> player_result
 * A borító (library_art) a hídban van (base64).
 *
 * Lejátszási sor (3b, 3c; a hálózatról is, mert csak azonosítók utaznak). A forrás (source): "album" / "folder"
 * (id), "playlist" (id), "queue" (a mostani sor), "tracks" (ids:[…]).
 *   queue_play {source…, start}          új sor, lejátszás a start azonosítójú számtól -> queue (hibánál queue_result)
 *   queue_get                            -> queue
 *   queue_jump {index}                   -> queue
 *   queue_add {source…, next}            a hallott tétel után (next: true) vagy a végére -> queue
 *   queue_remove {index}, queue_move {from, to}, queue_clear                       -> queue
 *   queue_repeat {mode: off|all|one}, queue_shuffle {on}                            -> queue
 *   A következő / előző a meglévő "media" paranccsal; az állapotüzenet "queue" mezője: {rev, index, count, repeat,
 *   shuffle}.
 * Lejátszási listák (3c):
 *   playlist_list                        -> playlists {rev, items:[{id, name, count, duration, created, updated}]}
 *   playlist_get {id}                    -> playlist {id, name, …, items:[{…, available}]}
 *   playlist_create {name, source…?}     -> playlist (a forrás nélkül üres lista)
 *   playlist_rename {id, name}, playlist_delete {id}                               -> playlist, illetve playlists
 *   playlist_add {id, source…, position?}, playlist_remove {id, indices:[…]}, playlist_move {id, from, to} -> playlist
 *   Hibánál playlist_result {ok:false, error}; az állapotüzenet "playlists" mezője: {rev, count, stats}.
 * Okos listák (3c2; a szabályok: smart_lists.hpp):
 *   playlist_create {name, rules}        -> playlist (smart: true, rules; a tartalom minden lekéréskor számolódik)
 *   playlist_update {id, name?, rules?}  -> playlist (a rules csak okos listánál)
 *   Okos listához kézzel nem adható és nem törölhető szám ("smart" hiba). A "stats" a lejátszási napló változásainak
 *   száma: ha nő, az okos listák tartalma változhatott.
 */
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "file_player.hpp"
#include "mini_json.hpp"
#include "music_library.hpp"
#include "phone_cache.hpp"
#include "play_queue.hpp"
#include "play_stats.hpp"
#include "playlists.hpp"
#include "smart_lists.hpp"
#include "ws_bridge.hpp"

namespace ee_analyzer::library {

/** A mentett mappalista ({"roots":[…]}) olvasása; üres: nincs mentve. */
inline std::vector<std::string> parseRootsDoc(const std::string& doc) {
    std::vector<std::string> out;
    json::Value v;
    if (doc.empty() || !json::parse(doc, v)) return out;
    if (const json::Value* r = v.get("roots"); r && r->type == json::Value::Type::Array)
        for (const auto& e : r->arr)
            if (e.type == json::Value::Type::String && !e.str.empty()) out.push_back(e.str);
    return out;
}

inline std::string rootsDoc(const std::vector<std::string>& roots) {
    std::string s = "{\"roots\":[";
    for (size_t i = 0; i < roots.size(); ++i) s += (i ? ",\"" : "\"") + media::jsonEscape(roots[i]) + "\"";
    return s + "]}";
}

/**
 * A queue_play forrása a zenetárból: album (lemez / sorszám szerint), mappa (a benne lévő számok, almappák nélkül),
 * vagy azonosítók listája (az ismeretlenek kimaradnak). start: a kezdő szám indexe (a startId alapján, különben 0).
 */
inline std::vector<player::QueueItem> queueSource(const Index& idx, const std::string& source, const std::string& id,
                                                  const std::vector<std::string>& ids, const std::string& startId,
                                                  size_t& start) {
    std::vector<player::QueueItem> items;
    auto add = [&](size_t t) { if (items.size() < player::PlayQueue::kMaxItems) items.push_back(queueItem(idx.tracks[t])); };
    if (source == "album") {
        if (auto it = idx.albumById.find(id); it != idx.albumById.end())
            for (size_t t : idx.albums[it->second].tracks) add(t);
    } else if (source == "folder") {
        if (auto it = idx.folderById.find(id); it != idx.folderById.end())
            for (size_t t : idx.folders[it->second].tracks) add(t);
    } else if (source == "tracks") {
        for (const auto& tid : ids)
            if (auto it = idx.trackById.find(tid); it != idx.trackById.end()) add(it->second);
    }
    start = 0;
    for (size_t i = 0; i < items.size(); ++i)
        if (!startId.empty() && items[i].id == startId) { start = i; break; }
    return items;
}

/** Egy lista tartalma: a tárolt számok, okos listánál a szabályok szerint most kiszámolva. */
inline std::vector<player::QueueItem> listItems(const player::Playlist& p, MusicLibrary& lib, const player::PlayStats& stats) {
    if (!p.smart) return p.items;
    json::Value v;
    SmartRules r;
    std::string e;
    if (!json::parse(p.rules, v) || !parseSmartRules(v, r, e)) return {};
    return evaluateSmart(r, *lib.snapshot(), stats.snapshot(), static_cast<int64_t>(std::time(nullptr)));
}

/**
 * Az üzenet forrásának tételei: a zenetárból (album, folder, tracks), egy listából (playlist) vagy a mostani sorból
 * (queue). start: a "start" azonosítójú tétel indexe (különben 0).
 */
inline std::vector<player::QueueItem> messageItems(const json::Value& msg, MusicLibrary& lib, player::PlayQueue& queue,
                                                   player::PlaylistStore& lists, const player::PlayStats& stats,
                                                   size_t& start) {
    auto str = [&](const char* k) { const json::Value* v = msg.get(k); return v ? v->stringOr("") : std::string(); };
    const std::string source = str("source"), startId = str("start");
    std::vector<player::QueueItem> items;
    start = 0;
    if (source == "playlist") {
        player::Playlist p;
        if (lists.get(str("id"), p)) items = listItems(p, lib, stats);
    } else if (source == "queue") {
        items = queue.items();
    } else if (source == "phone_track") {
        player::QueueItem it;
        it.id = str("track_id").empty() ? str("id") : str("track_id");
        it.title = str("title").empty() ? "Phone Track" : str("title");
        it.artist = str("artist");
        it.album = str("album");
        const json::Value* durV = msg.get("duration");
        it.duration = durV ? durV->numberOr(0.0) : 0.0;
        if (!it.id.empty()) items.push_back(std::move(it));
    } else if (source == "phone_tracks") {
        if (const json::Value* arr = msg.get("items"); arr && arr->type == json::Value::Type::Array) {
            for (const auto& el : arr->arr) {
                if (items.size() >= player::PlayQueue::kMaxItems) break;
                auto s = [](const json::Value& o, const char* k) {
                    const json::Value* x = o.get(k); return x ? x->stringOr("") : std::string();
                };
                player::QueueItem it;
                it.id = s(el, "id");
                it.title = s(el, "title").empty() ? "Phone Track" : s(el, "title");
                it.artist = s(el, "artist");
                it.album = s(el, "album");
                const json::Value* durV = el.get("duration");
                it.duration = durV ? durV->numberOr(0.0) : 0.0;
                if (!it.id.empty() && it.id.size() <= 64) items.push_back(std::move(it));
            }
        }
    } else {
        std::vector<std::string> ids;
        if (const json::Value* a = msg.get("ids"); a && a->type == json::Value::Type::Array)
            for (const auto& e : a->arr)
                if (e.type == json::Value::Type::String && !e.str.empty() && e.str.size() <= 64 &&
                    ids.size() < player::PlayQueue::kMaxItems)
                    ids.push_back(e.str);
        return queueSource(*lib.snapshot(), source, str("id"), ids, startId, start);
    }
    for (size_t i = 0; i < items.size(); ++i)
        if (!startId.empty() && items[i].id == startId) { start = i; break; }
    return items;
}

inline std::string playlistsJson(const player::PlaylistStore& lists, MusicLibrary& lib, const player::PlayStats& stats) {
    std::string s = "{\"type\":\"playlists\",\"rev\":" + std::to_string(lists.rev()) + ",\"items\":[";
    const auto all = lists.all();
    for (size_t i = 0; i < all.size(); ++i) {
        const auto& l = all[i];
        const auto items = listItems(l, lib, stats);
        double dur = 0.0;
        for (const auto& it : items) dur += it.duration;
        char d[32];
        std::snprintf(d, sizeof(d), "%.3f", dur);
        s += (i ? ",{" : "{") + std::string("\"id\":\"") + media::jsonEscape(l.id) + "\",\"name\":\"" +
             media::jsonEscape(l.name) + "\",\"smart\":" + (l.smart ? "true" : "false") + ",\"count\":" +
             std::to_string(items.size()) + ",\"duration\":" + d + ",\"created\":" + std::to_string(l.created) +
             ",\"updated\":" + std::to_string(l.updated) + "}";
    }
    return s + "]}";
}

/**
 * Egy lista a számaival (okos listánál a szabályokkal is); available: a szám megvan-e a zenetárban (bejárás közben a
 * régi index szerint).
 */
inline std::string playlistJson(const player::Playlist& l, const std::vector<player::QueueItem>& items, const Index& idx,
                                bool scanning, const MusicLibrary* lib = nullptr,
                                player::PhoneCache* phoneCache = nullptr, bool hasRemoteClient = false) {
    std::string s = "{\"type\":\"playlist\",\"id\":\"" + media::jsonEscape(l.id) + "\",\"name\":\"" +
                    media::jsonEscape(l.name) + "\",\"smart\":" + (l.smart ? "true" : "false");
    if (l.smart) {
        // a tárolt szabályok újra ellenőrizve, egységes alakban (a playlists.json kézzel is szerkeszthető)
        json::Value v;
        SmartRules r;
        std::string e;
        s += ",\"rules\":" + (json::parse(l.rules, v) && parseSmartRules(v, r, e) ? smartRulesJson(r) : std::string("null"));
    }
    s += ",\"created\":" + std::to_string(l.created) + ",\"updated\":" + std::to_string(l.updated) + ",\"scanning\":" +
         (scanning ? "true" : "false") + ",\"items\":[";
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        char d[32];
        std::snprintf(d, sizeof(d), "%.3f", it.duration);
        bool avail = false;
        std::string src = "library";
        bool matched = false;
        bool cached = false;

        if (it.id.rfind("p_", 0) == 0) {
            src = "phone";
            std::string matchId;
            if (lib && lib->matchLocalTrack(it.title, it.artist, it.duration, matchId)) {
                matched = true;
                avail = true;
                std::string matchPath;
                if (phoneCache && lib->trackPath(matchId, matchPath)) {
                    phoneCache->setAlias(it.id, matchPath);
                }
            } else if (phoneCache && phoneCache->hasTrack(it.id)) {
                cached = true;
                avail = true;
            } else if (hasRemoteClient) {
                avail = true;
            } else {
                avail = false;
            }
        } else {
            avail = (idx.trackById.count(it.id) > 0);
        }

        s += (i ? ",{" : "{") + std::string("\"id\":\"") + media::jsonEscape(it.id) + "\",\"title\":\"" +
             media::jsonEscape(it.title) + "\",\"artist\":\"" + media::jsonEscape(it.artist) + "\",\"album\":\"" +
             media::jsonEscape(it.album) + "\",\"duration\":" + d + ",\"available\":" +
             (avail ? "true" : "false") + ",\"source\":\"" + src + "\"" +
             (matched ? ",\"matched\":true" : "") +
             (cached ? ",\"cached\":true" : "") + "}";
    }
    return s + "]}";
}

/**
 * A zenetár, a lejátszási sor és a listák üzenetei a hídon. saveRoots: a mappalista mentése (a platform tárolójába,
 * pl. library.json).
 */
inline void attachLibrary(BridgeCallbacks& cb, MusicLibrary& lib, player::PlayQueue& queue, player::PlaylistStore& lists,
                          player::PlayStats& stats, std::function<void(const std::string& doc)> saveRoots,
                          player::PhoneCache* phoneCache = nullptr) {
    cb.libraryArt = [&lib, phoneCache](const std::string& id, std::string& mime, std::string& bytes) {
        if (id.rfind("p_", 0) == 0 && phoneCache) {
            std::string path;
            if (phoneCache->resolve(id, path)) {
                auto d = player::openDecoder(path, nullptr, true);
                if (d && !d->tags().art.empty()) {
                    mime = d->tags().artMime;
                    bytes = d->tags().art;
                    return true;
                }
            }
            return false;
        }
        return lib.art(id, mime, bytes);
    };
    cb.queueStatus = [&queue] { return queue.statusJson(); };
    cb.playlistsStatus = [&lists, &stats] {
        return "{\"rev\":" + std::to_string(lists.rev()) + ",\"count\":" + std::to_string(lists.count()) +
               ",\"stats\":" + std::to_string(stats.rev()) + "}";
    };
    cb.queueRequest = [&lib, &queue, &lists, &stats, phoneCache, &cb](const std::string& type, const json::Value& msg, bool) -> std::string {
        auto str = [&](const char* k) { const json::Value* v = msg.get(k); return v ? v->stringOr("") : std::string(); };
        auto idx = [&](const char* k) { const json::Value* v = msg.get(k); const double d = v ? v->numberOr(-1.0) : -1.0;
                                        return d >= 0 && d < 1e9 ? static_cast<long>(d) : -1L; };
        auto fail = [](const char* kind, const std::string& e) {
            return std::string("{\"type\":\"") + kind + "_result\",\"ok\":false,\"error\":\"" + media::jsonEscape(e) + "\"}";
        };
        if (type.rfind("playlist_", 0) == 0) {
            auto reply = [&](const std::string& id, const std::string& e) {
                if (!e.empty()) return fail("playlist", e);
                player::Playlist p;
                if (!lists.get(id, p)) return fail("playlist", "unknown");
                return playlistJson(p, listItems(p, lib, stats), *lib.snapshot(), lib.scanning(),
                                    &lib, phoneCache, cb.hasRemoteClient ? cb.hasRemoteClient() : false);
            };
            // okos lista szabályai az üzenetből: ellenőrzés, egységes alak (rulesOut); hibánál a hiba kódja
            auto rulesOf = [&](const json::Value& v, std::string& rulesOut) -> std::string {
                SmartRules r;
                std::string e;
                if (!parseSmartRules(v, r, e)) return "rules:" + e;
                rulesOut = smartRulesJson(r);
                return "";
            };
            const std::string id = str("id");
            if (type == "playlist_list") return playlistsJson(lists, lib, stats);
            if (type == "playlist_get") return reply(id, "");
            if (type == "playlist_create") {
                std::string newId;
                if (const json::Value* rv = msg.get("rules")) {
                    std::string rules;
                    const std::string e = rulesOf(*rv, rules);
                    if (!e.empty()) return fail("playlist", e);
                    return reply(newId, lists.createSmart(str("name"), rules, newId));
                }
                size_t start = 0;
                std::vector<player::QueueItem> items;
                if (!str("source").empty()) items = messageItems(msg, lib, queue, lists, stats, start);
                const std::string e = lists.create(str("name"), std::move(items), newId);
                return reply(newId, e);
            }
            if (type == "playlist_update") {
                // név és / vagy szabályok; előbb minden ellenőrzés, hogy hibánál ne változzon félig
                const json::Value* nv = msg.get("name");
                const json::Value* rv = msg.get("rules");
                if (!nv && !rv) return fail("playlist", "empty");
                std::string rules;
                if (rv) {
                    player::Playlist p;
                    if (!lists.get(id, p)) return fail("playlist", "unknown");
                    if (!p.smart) return fail("playlist", "notSmart");
                    const std::string e = rulesOf(*rv, rules);
                    if (!e.empty()) return fail("playlist", e);
                }
                if (nv && player::PlaylistStore::cleanName(nv->stringOr("")).empty()) return fail("playlist", "name");
                std::string e = nv ? lists.rename(id, nv->stringOr("")) : "";
                if (e.empty() && rv) e = lists.setRules(id, rules);
                return reply(id, e);
            }
            if (type == "playlist_rename") return reply(id, lists.rename(id, str("name")));
            if (type == "playlist_delete") {
                const std::string e = lists.remove(id);
                return e.empty() ? playlistsJson(lists, lib, stats) : fail("playlist", e);
            }
            if (type == "playlist_add") {
                size_t start = 0;
                auto items = messageItems(msg, lib, queue, lists, stats, start);
                return reply(id, lists.add(id, std::move(items), idx("position")));
            }
            if (type == "playlist_remove") {
                std::vector<size_t> indices;
                if (const json::Value* a = msg.get("indices"); a && a->type == json::Value::Type::Array)
                    for (const auto& e : a->arr) {
                        const double d = e.numberOr(-1.0);
                        if (d >= 0 && d < static_cast<double>(player::PlaylistStore::kMaxItems)) indices.push_back(static_cast<size_t>(d));
                    }
                return reply(id, lists.removeItems(id, std::move(indices)));
            }
            if (type == "playlist_move") {
                const long from = idx("from"), to = idx("to");
                if (from < 0 || to < 0) return fail("playlist", "index");
                return reply(id, lists.move(id, static_cast<size_t>(from), static_cast<size_t>(to)));
            }
            return fail("playlist", "unsupported");
        }
        if (type == "queue_get") return queue.json();
        if (type == "queue_play" || type == "queue_load" || type == "queue_set") {
            const bool doPlay = (type == "queue_play");
            size_t start = 0;
            auto items = messageItems(msg, lib, queue, lists, stats, start);
            if (items.empty()) return fail("queue", "empty");
            for (const auto& it : items) {
                if (it.id.rfind("p_", 0) == 0) {
                    std::string matchId;
                    if (lib.matchLocalTrack(it.title, it.artist, it.duration, matchId)) {
                        std::string matchPath;
                        if (phoneCache && lib.trackPath(matchId, matchPath)) phoneCache->setAlias(it.id, matchPath);
                    }
                }
            }
            std::string e;
            const bool ok = doPlay ? queue.play(std::move(items), start, &e)
                                   : queue.load(std::move(items), start, &e);
            if (!ok) return fail("queue", e);
            return queue.json();
        }
        if (type == "queue_jump") {
            const long i = idx("index");
            std::string e;
            if (i < 0 || !queue.jump(static_cast<size_t>(i), &e)) return fail("queue", e.empty() ? "index" : e);
            return queue.json();
        }
        if (type == "queue_add") {
            size_t start = 0;
            auto items = messageItems(msg, lib, queue, lists, stats, start);
            const json::Value* n = msg.get("next");
            const bool next = n && n->type == json::Value::Type::Bool && n->b;
            if (items.empty()) return fail("queue", "empty");
            for (const auto& it : items) {
                if (it.id.rfind("p_", 0) == 0) {
                    std::string matchId;
                    if (lib.matchLocalTrack(it.title, it.artist, it.duration, matchId)) {
                        std::string matchPath;
                        if (phoneCache && lib.trackPath(matchId, matchPath)) phoneCache->setAlias(it.id, matchPath);
                    }
                }
            }
            if (!queue.add(std::move(items), next)) return fail("queue", "full");
            return queue.json();
        }
        if (type == "queue_remove") {
            const long i = idx("index");
            if (i < 0 || !queue.remove(static_cast<size_t>(i))) return fail("queue", "index");
            return queue.json();
        }
        if (type == "queue_move") {
            const long from = idx("from"), to = idx("to");
            if (from < 0 || to < 0 || !queue.move(static_cast<size_t>(from), static_cast<size_t>(to))) return fail("queue", "index");
            return queue.json();
        }
        if (type == "queue_clear") {
            queue.clear();
            return queue.json();
        }
        if (type == "queue_repeat") {
            player::PlayQueue::Repeat r;
            if (!player::PlayQueue::parseRepeat(str("mode"), r)) return fail("queue", "mode");
            queue.setRepeat(r);
            return queue.json();
        }
        if (type == "queue_shuffle") {
            const json::Value* on = msg.get("on");
            if (!on || on->type != json::Value::Type::Bool) return fail("queue", "on");
            queue.setShuffle(on->b);
            return queue.json();
        }
        return fail("queue", "unsupported");
    };
    cb.libraryRequest = [&lib, &queue, saveRoots](const std::string& type, const json::Value& msg, bool remote) -> std::string {
        auto str = [&](const char* k) { const json::Value* v = msg.get(k); return v ? v->stringOr("") : std::string(); };
        auto num = [&](const char* k, double d) { const json::Value* v = msg.get(k); return v ? v->numberOr(d) : d; };
        auto err = [](const char* what) { return std::string("{\"type\":\"library_error\",\"error\":\"") + what + "\"}"; };
        if (type == "library" || type == "library_info") return lib.infoJson(!remote);
        if (type == "library_set_roots") {
            if (remote) return err("remote");
            std::vector<std::string> roots;
            if (const json::Value* r = msg.get("roots"); r && r->type == json::Value::Type::Array)
                for (const auto& e : r->arr)
                    if (e.type == json::Value::Type::String && !e.str.empty() && e.str.size() < 4096) roots.push_back(e.str);
            lib.setRoots(roots);
            if (saveRoots) saveRoots(rootsDoc(lib.roots()));
            return lib.infoJson(true);
        }
        if (type == "library_rescan") {
            if (remote) return err("remote");
            lib.rescan();
            return lib.infoJson(true);
        }
        if (type == "library_albums")
            return lib.albumsJson(static_cast<size_t>(std::max(0.0, num("offset", 0))), static_cast<size_t>(std::max(1.0, num("limit", 100))));
        if (type == "library_album") {
            const std::string s = lib.albumJson(str("id"));
            return s.empty() ? err("unknown") : s;
        }
        if (type == "library_folder") {
            const std::string s = lib.folderJson(str("id"));
            return s.empty() ? err("unknown") : s;
        }
        if (type == "library_play") {
            const std::string id = str("id");
            std::vector<player::QueueItem> items;
            if (id.rfind("p_", 0) == 0) {
                player::QueueItem it;
                it.id = id;
                it.title = str("title").empty() ? "Phone Track" : str("title");
                it.artist = str("artist");
                it.album = str("album");
                const json::Value* durV = msg.get("duration");
                it.duration = durV ? durV->numberOr(0.0) : 0.0;
                items.push_back(std::move(it));
            } else {
                size_t start = 0;
                items = queueSource(*lib.snapshot(), "tracks", "", { id }, "", start);
            }
            std::string e;
            if (items.empty()) return "{\"type\":\"player_result\",\"ok\":false,\"error\":\"unknown\"}";
            if (!queue.play(std::move(items), 0, &e))
                return "{\"type\":\"player_result\",\"ok\":false,\"error\":\"" + media::jsonEscape(e) + "\"}";
            return "{\"type\":\"player_result\",\"ok\":true}";
        }
        return err("unsupported");
    };

    if (phoneCache) {
        cb.phoneTrackRequest = [&lib, phoneCache](const std::string& type, const json::Value& msg, bool) -> std::string {
            auto str = [&](const char* k) { const json::Value* v = msg.get(k); return v ? v->stringOr("") : std::string(); };
            auto num = [&](const char* k, double d) { const json::Value* v = msg.get(k); return v ? v->numberOr(d) : d; };

            if (type == "phone_track_offer") {
                const std::string id = str("id");
                const std::string title = str("title");
                const std::string artist = str("artist");
                const double duration = num("duration", 0.0);
                const uint64_t size = static_cast<uint64_t>(std::max(0.0, num("size", 0.0)));
                const std::string ext = str("ext");

                if (!player::PhoneCache::sanitizeId(id) || !player::PhoneCache::sanitizeExt(ext)) {
                    return "{\"type\":\"phone_track_error\",\"id\":\"" + media::jsonEscape(id) + "\",\"error\":\"invalid_request\"}";
                }

                // 1. Okos párosítás helyi számmal: ha megvan a PC-n, nem kell átküldeni
                std::string localId;
                if (lib.matchLocalTrack(title, artist, duration, localId)) {
                    return "{\"type\":\"phone_track_have\",\"id\":\"" + media::jsonEscape(id) +
                           "\",\"local_id\":\"" + media::jsonEscape(localId) +
                           "\",\"title\":\"" + media::jsonEscape(title) + "\"}";
                }

                // 2. Gyorsítótár ellenőrzése: ha már átküldték korábban és a méret egyezik
                if (phoneCache->hasTrack(id, ext, size)) {
                    return "{\"type\":\"phone_track_ready\",\"id\":\"" + media::jsonEscape(id) + "\",\"status\":\"cached\"}";
                }

                // 3. Feltöltés kérése darabokban
                return "{\"type\":\"phone_track_send\",\"id\":\"" + media::jsonEscape(id) + "\",\"offset\":0,\"chunk_size\":65536}";
            }
            if (type == "phone_track_import") {
                const std::string id = str("id");
                const std::string ext = str("ext");
                const std::string title = str("title");
                const std::string artist = str("artist");
                const std::string album = str("album");
                std::string cachedPath;
                if (!phoneCache->resolve(id, cachedPath)) {
                    return "{\"type\":\"phone_track_error\",\"id\":\"" + media::jsonEscape(id) + "\",\"error\":\"not_cached\"}";
                }
                std::string outPath;
                const std::string filename = (title.empty() ? id : title) + "." + (ext.empty() ? "flac" : ext);
                if (!lib.importTrack(cachedPath, artist, album, filename, outPath)) {
                    return "{\"type\":\"phone_track_error\",\"id\":\"" + media::jsonEscape(id) + "\",\"error\":\"import_failed\"}";
                }
                phoneCache->setAlias(id, outPath);
                return "{\"type\":\"phone_track_imported\",\"id\":\"" + media::jsonEscape(id) + "\",\"ok\":true,\"path\":\"" +
                       media::jsonEscape(outPath) + "\"}";
            }
            return "{\"type\":\"phone_track_error\",\"error\":\"unsupported\"}";
        };

        cb.phoneTrackBinary = [phoneCache](const std::string& data) -> std::string {
            player::AepfHeader hdr;
            if (!player::parseAepfFrame(data, hdr)) {
                return "{\"type\":\"phone_track_error\",\"error\":\"invalid_aepf_frame\"}";
            }
            auto res = phoneCache->writeChunk(hdr.id, hdr.ext, hdr.offset, hdr.totalSize, hdr.data, hdr.dataLen, hdr.eof());
            if (!res.ok) {
                return "{\"type\":\"phone_track_error\",\"id\":\"" + media::jsonEscape(hdr.id) + "\",\"error\":\"" +
                       media::jsonEscape(res.error) + "\"}";
            }
            if (res.completed) {
                return "{\"type\":\"phone_track_ready\",\"id\":\"" + media::jsonEscape(hdr.id) + "\",\"status\":\"ready\"}";
            }
            return "";
        };
    }
}

} // namespace ee_analyzer::library
