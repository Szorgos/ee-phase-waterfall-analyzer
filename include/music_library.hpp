#pragma once
/**
 * @file music_library.hpp
 * @brief A 2.x zenetára: a megadott zenei mappák és almappáik bejárása, index (szám, album, mappa), borítók.
 *
 * - A bejárás háttérszálon fut; közben a régi index érvényes, a végén egyben cserélődik.
 * - Azonosítók: a teljes útvonalból (szám, mappa), illetve az albumelőadó + album nevéből képzett kivonat ("t…",
 *   "f…", "a…"), így újrabejárás után is ugyanazok (a telefon és a lejátszási listák ezekre hivatkozhatnak).
 * - A hálózatról jövő kérés csak azonosítót adhat meg; útvonalat a motor a saját gépén kívülre nem ad ki (a mappák
 *   listája csak helyi kliensnek jár), és csak a megadott mappákon belüli fájl játszható le.
 * - Album: azonos albumelőadó (ennek hiányában előadó) és albumcím; album címke nélkül a mappa.
 * - Borító: a fájlba ágyazott kép (az előlap elsőbbséget kap), különben a mappa képe (cover / folder / front /
 *   album / albumart .jpg / .jpeg / .png).
 * Az útvonalak UTF-8 szövegek (Windowson is).
 */
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ee_analyzer::library {

struct Track {
    std::string id, path, file, title, artist, album, albumArtist, genre;
    std::string folderId, albumId;
    int track = 0, disc = 0, year = 0;
    int64_t added = 0;                   // a fájl módosítási ideje (Unix-idő, s): a „felkerülés” az okos listákhoz
    uint64_t size = 0;                   // a fájl mérete (bájtban): a gyorsítótár érvényességéhez
    int64_t mtime = 0;                   // a fájl utolsó módosítási ideje (s)
    double duration = 0.0;               // 0: ismeretlen
    uint32_t rate = 0, channels = 0;
    bool embeddedArt = false;
};

struct Album {
    std::string id, title, artist;       // artist üres és various = true, ha több előadóé és nincs albumelőadó
    bool various = false;
    int year = 0;
    std::vector<size_t> tracks;          // az Index::tracks indexei, lemez / sorszám / fájlnév szerint
    double duration = 0.0;
    std::string artTrack;                // beágyazott borítójú szám útvonala (üres: nincs)
    std::string coverFile;               // a mappa képe (üres: nincs)
};

struct Folder {
    std::string id, path, name, parentId;
    std::vector<size_t> folders;         // az Index::folders indexei
    std::vector<size_t> tracks;          // közvetlenül ebben a mappában
    size_t totalTracks = 0;              // az egész részfában
    std::string coverFile;
};

struct Index {
    std::vector<Track> tracks;
    std::vector<Album> albums;           // előadó, év, cím szerint
    std::vector<Folder> folders;
    std::vector<size_t> roots;
    std::unordered_map<std::string, size_t> trackById, albumById, folderById;
};

class MusicLibrary {
public:
    MusicLibrary() = default;
    ~MusicLibrary();
    MusicLibrary(const MusicLibrary&) = delete;
    MusicLibrary& operator=(const MusicLibrary&) = delete;

    /** A zenei mappák beállítása és az újrabejárás indítása (a futó bejárás leáll). */
    void setRoots(std::vector<std::string> roots);
    std::vector<std::string> roots() const;
    void rescan();
    bool scanning() const { return scanning_.load(); }
    /** A gyorsítótárból van-e az index (amíg a háttérbejárás nem zárult le). */
    bool cached() const { return cached_.load(); }
    /** Megvárja a bejárás végét (tesztek); false: időtúllépés. */
    bool waitIdle(int timeoutMs) const;
    std::shared_ptr<const Index> snapshot() const;

    /** Gyorsítótár perzisztencia kezelőinek beállítása (betöltés és mentés). */
    void setCacheStorage(std::function<std::string()> loadCache, std::function<void(const std::string&)> saveCache);

    // ---- a protokoll üzenetei (JSON, "type" mezővel)
    /** {"type":"library", scanning, scanned, tracks, albums, folders[, roots]}; a mappák csak helyi kliensnek. */
    std::string infoJson(bool includeRoots) const;
    /** {"type":"library_albums", offset, total, items:[…]} */
    std::string albumsJson(size_t offset, size_t limit) const;
    /** {"type":"library_album", …, tracks:[…]}; üres: ismeretlen azonosító. */
    std::string albumJson(const std::string& id) const;
    /** {"type":"library_folder", id, name, parent, folders:[…], tracks:[…]}; az üres azonosító a gyökér (a mappák). */
    std::string folderJson(const std::string& id) const;
    /** A szám fájlja (csak indexelt szám, a megadott mappákon belül); false: ismeretlen. */
    bool trackPath(const std::string& id, std::string& path) const;
    /** Borító album, mappa vagy szám azonosítójához; false: nincs. */
    bool art(const std::string& id, std::string& mime, std::string& bytes) const;
    /** Okos párosítás (4b): megegyezik-e a telefonos szám egy helyi számmal (cím, előadó, időtartam ±2.5s). */
    bool matchLocalTrack(const std::string& title, const std::string& artist, double duration, std::string& outId) const;
    /** Szám végleges másolása a PC zenei könyvtárába (4c: Aqulify a telefonról). */
    bool importTrack(const std::string& sourcePath, const std::string& artist,
                     const std::string& album, const std::string& filename,
                     std::string& outPath);

    /** Az alapértelmezett zenei mappa (Linux: XDG_MUSIC_DIR, ~/Music; Windows: a Zene ismert mappa). */
    static std::string defaultMusicDir();

private:
    void startScan();
    void stopScan();
    std::shared_ptr<Index> scan(const std::vector<std::string>& roots);

    mutable std::mutex m_;
    std::vector<std::string> roots_;
    std::shared_ptr<const Index> index_ = std::make_shared<Index>();
    std::thread thread_;
    std::atomic<bool> scanning_{false}, cancel_{false};
    std::atomic<bool> rescanRequested_{false}, cached_{false};
    std::atomic<size_t> scanned_{0};
    std::mutex cvMutex_;
    std::condition_variable cv_;
    std::function<std::string()> loadCache_;
    std::function<void(const std::string&)> saveCache_;
};

/** Gyorsítótár szerializálás JSON formátumba (version: 1). */
std::string serializeCache(const Index& idx, const std::vector<std::string>& roots);
/** Gyorsítótár deszerializálás JSON-ból; nullptr, ha sérült vagy nem egyezik a verzió / gyökér. */
std::shared_ptr<Index> parseCache(const std::string& doc, const std::vector<std::string>& roots);
/** Teljes Index felépítése meglévő Track listából (gyorsítótár azonnali betöltéséhez). */
std::shared_ptr<Index> buildIndexFromTracks(const std::vector<std::string>& roots, std::vector<Track> tracks);

/**
 * A hangfájl mappájának képe (cover / folder / front / album / albumart .jpg / .jpeg / .png), ugyanúgy, mint a
 * zenetárban; a lejátszó ezt mutatja, ha a fájlba nincs borító ágyazva. false: nincs ilyen kép.
 */
bool folderCover(const std::string& audioPath, std::string& mime, std::string& bytes);

} // namespace ee_analyzer::library
