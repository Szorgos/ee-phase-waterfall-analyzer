/**
 * @file music_library.cpp
 * @brief A 2.x zenetára (lásd music_library.hpp).
 */
#include "music_library.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

#include "audio_decoder.hpp"
#include "media_session.hpp"
#include "mini_json.hpp"

namespace fs = std::filesystem;

namespace ee_analyzer::library {

namespace {

constexpr size_t kMaxTracks = 200000;          // ennél több fájlt nem indexelünk (védelem)
constexpr int kMaxDepth = 32;
constexpr size_t kMaxCoverBytes = 8u << 20;

std::string utf8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path fromUtf8(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string idOf(char prefix, const std::string& key) {
    return std::string(1, prefix) + media::hashId(key);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/** Rendezéshez: kisbetűs, a számokat számként hasonlítja ("2" < "10"), az elöl álló "The " / "A " nélkül nem bonyolítunk. */
bool naturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const unsigned char ca = static_cast<unsigned char>(a[i]), cb = static_cast<unsigned char>(b[j]);
        if (std::isdigit(ca) && std::isdigit(cb)) {
            size_t ei = i, ej = j;
            while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei]))) ++ei;
            while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej]))) ++ej;
            const std::string na = a.substr(i, ei - i), nb = b.substr(j, ej - j);
            const size_t za = na.find_first_not_of('0'), zb = nb.find_first_not_of('0');
            const std::string sa = za == std::string::npos ? "0" : na.substr(za), sb = zb == std::string::npos ? "0" : nb.substr(zb);
            if (sa.size() != sb.size()) return sa.size() < sb.size();
            if (sa != sb) return sa < sb;
            i = ei;
            j = ej;
            continue;
        }
        const int la = std::tolower(ca), lb = std::tolower(cb);
        if (la != lb) return la < lb;
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

std::string coverIn(const fs::path& dir) {
    static const char* names[] = { "cover", "folder", "front", "album", "albumart" };
    static const char* exts[] = { ".jpg", ".jpeg", ".png" };
    std::error_code ec;
    std::map<std::string, std::string> found;   // kisbetűs név -> valódi útvonal
    for (const auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        if (!e.is_regular_file(ec)) continue;
        found.emplace(lower(utf8(e.path().filename())), utf8(e.path()));
    }
    for (const char* n : names)
        for (const char* x : exts) {
            auto it = found.find(std::string(n) + x);
            if (it != found.end()) return it->second;
        }
    return {};
}

bool readFile(const std::string& path, std::string& out, size_t maxBytes) {
    std::ifstream f(fromUtf8(path), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const auto size = static_cast<size_t>(f.tellg());
    if (size == 0 || size > maxBytes) return false;
    f.seekg(0);
    out.resize(size);
    return static_cast<bool>(f.read(out.data(), static_cast<std::streamsize>(size)));
}

std::string mimeOf(const std::string& path) {
    const std::string l = lower(path);
    return l.size() > 4 && l.compare(l.size() - 4, 4, ".png") == 0 ? "image/png" : "image/jpeg";
}

void appendNum(std::string& s, const char* key, double v) {
    char b[64];
    std::snprintf(b, sizeof(b), ",\"%s\":%.3f", key, v);
    s += b;
}

std::string q(const std::string& v) { return "\"" + media::jsonEscape(v) + "\""; }

/** A fájl módosítási ideje Unix-időben (s): a „felkerülés” ideje az okos listákhoz; 0, ha nem olvasható. */
int64_t fileTime(const fs::path& p) {
    std::error_code ec;
    const auto ft = fs::last_write_time(p, ec);
    if (ec) return 0;
    // a fájlrendszer órájáról a rendszerórára a mostani eltéréssel (a clock_cast nem minden szabványkönyvtárban van meg)
    const auto sys = std::chrono::system_clock::now() +
                     std::chrono::duration_cast<std::chrono::system_clock::duration>(ft - fs::file_time_type::clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

std::string trackJson(const Track& t) {
    std::string s = "{\"id\":" + q(t.id) + ",\"title\":" + q(t.title) + ",\"artist\":" + q(t.artist) +
                    ",\"album\":" + q(t.album) + ",\"genre\":" + q(t.genre) + ",\"year\":" + std::to_string(t.year) +
                    ",\"track\":" + std::to_string(t.track) + ",\"disc\":" + std::to_string(t.disc);
    appendNum(s, "duration", t.duration);
    s += ",\"rate\":" + std::to_string(t.rate) + ",\"channels\":" + std::to_string(t.channels) + ",\"file\":" + q(t.file) + "}";
    return s;
}

} // namespace

// ---------------------------------------------------------------- bejárás

std::string MusicLibrary::defaultMusicDir() {
#ifdef _WIN32
    static const GUID kFolderMusic = { 0x4BD8D571, 0x6D19, 0x48D3, { 0xBE, 0x97, 0x42, 0x22, 0x20, 0x08, 0x0E, 0x43 } };
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(kFolderMusic, 0, nullptr, &p)) && p) out = utf8(fs::path(p));
    if (p) CoTaskMemFree(p);
    return out;
#else
    const char* home = std::getenv("HOME");
    if (!home) return {};
    // XDG: ~/.config/user-dirs.dirs, pl. XDG_MUSIC_DIR="$HOME/Zene" (magyar rendszeren)
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const std::string cfg = (xdg && *xdg ? std::string(xdg) : std::string(home) + "/.config") + "/user-dirs.dirs";
    std::ifstream f(cfg);
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("XDG_MUSIC_DIR=", 0) != 0) continue;
        std::string v = line.substr(14);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        if (v.rfind("$HOME", 0) == 0) v = std::string(home) + v.substr(5);
        if (!v.empty()) return v;
    }
    return std::string(home) + "/Music";
#endif
}

MusicLibrary::~MusicLibrary() { stopScan(); }

void MusicLibrary::setCacheStorage(std::function<std::string()> loadCache, std::function<void(const std::string&)> saveCache) {
    std::lock_guard<std::mutex> lk(m_);
    loadCache_ = std::move(loadCache);
    saveCache_ = std::move(saveCache);
}

void MusicLibrary::setRoots(std::vector<std::string> roots) {
    stopScan();
    {
        std::lock_guard<std::mutex> lk(m_);
        // ismétlődés és üres elem nélkül, a megadott sorrendben
        std::vector<std::string> clean;
        for (auto& r : roots)
            if (!r.empty() && std::find(clean.begin(), clean.end(), r) == clean.end()) clean.push_back(std::move(r));
        roots_ = std::move(clean);

        // Gyorsítótár azonnali betöltése induláskor
        if (loadCache_) {
            const std::string doc = loadCache_();
            auto cachedIdx = parseCache(doc, roots_);
            if (cachedIdx && !cachedIdx->tracks.empty()) {
                index_ = std::move(cachedIdx);
                cached_ = true;
            } else {
                index_ = std::make_shared<Index>();
                cached_ = false;
            }
        } else {
            index_ = std::make_shared<Index>();
            cached_ = false;
        }
    }
    startScan();
}

std::vector<std::string> MusicLibrary::roots() const {
    std::lock_guard<std::mutex> lk(m_);
    return roots_;
}

void MusicLibrary::rescan() {
    if (!thread_.joinable()) {
        startScan();
    } else {
        scanning_ = true;
        rescanRequested_ = true;
        cv_.notify_all();
    }
}

void MusicLibrary::stopScan() {
    cancel_ = true;
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    cancel_ = false;
    scanning_ = false;
}

void MusicLibrary::startScan() {
    stopScan();
    cancel_ = false;
    rescanRequested_ = false;
    scanning_ = true;
    thread_ = std::thread([this] {
        // Rövid várakozás (20 ms), hogy induláskor a felület a gyorsítótárból azonnal, I/O-versenyhelyzet nélkül megjelenhessen
        {
            std::unique_lock<std::mutex> lk(cvMutex_);
            cv_.wait_for(lk, std::chrono::milliseconds(20), [this] { return cancel_.load(); });
        }
        while (!cancel_.load()) {
            scanned_ = 0;
            const auto r = roots();
            auto idx = scan(r);
            if (!cancel_.load()) {
                std::lock_guard<std::mutex> lk(m_);
                index_ = std::move(idx);
                cached_ = false;
            }
            scanning_ = false;
            if (saveCache_ && !cancel_.load()) {
                const auto snap = snapshot();
                saveCache_(serializeCache(*snap, roots()));
            }

            // Időszakos ellenőrzés (10 perc) vagy kézi újrabejárás jelzése
            std::unique_lock<std::mutex> lk(cvMutex_);
            cv_.wait_for(lk, std::chrono::minutes(10), [this] {
                return cancel_.load() || rescanRequested_.load();
            });
            if (cancel_.load()) break;
            rescanRequested_ = false;
            scanning_ = true;
        }
        scanning_ = false;
    });
}

bool MusicLibrary::waitIdle(int timeoutMs) const {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (scanning_.load()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

std::shared_ptr<const Index> MusicLibrary::snapshot() const {
    std::lock_guard<std::mutex> lk(m_);
    return index_;
}

static void buildAlbumsAndMaps(Index& idx);

std::shared_ptr<Index> MusicLibrary::scan(const std::vector<std::string>& roots) {
    auto idx = std::make_shared<Index>();
    std::set<std::string> seenRoots;

    std::unordered_map<std::string, const Track*> cachedByPath;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (index_) {
            for (const auto& t : index_->tracks) {
                if (t.size > 0 && t.mtime > 0) cachedByPath.emplace(t.path, &t);
            }
        }
    }

    // mappa bejárása (mélységi), a mappák és számok indexbe vétele; visszaadja a mappa indexét (vagy SIZE_MAX)
    std::function<size_t(const fs::path&, const std::string&, int)> walk = [&](const fs::path& dir, const std::string& parentId,
                                                                               int depth) -> size_t {
        if (cancel_.load() || depth > kMaxDepth || idx->tracks.size() >= kMaxTracks) return SIZE_MAX;
        Folder f;
        f.path = utf8(dir);
        f.id = idOf('f', f.path);
        f.name = utf8(dir.filename());
        if (f.name.empty()) f.name = f.path;
        f.parentId = parentId;
        const size_t self = idx->folders.size();
        idx->folders.push_back(f);
        std::vector<fs::path> dirs, files;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
            const std::string name = utf8(e.path().filename());
            if (name.empty() || name[0] == '.') continue;                 // rejtett
            std::error_code ec2;
            const auto st = e.symlink_status(ec2);
            if (fs::is_symlink(st)) continue;                              // szimbolikus link: nem követjük (hurok, kilépés a mappából)
            if (fs::is_directory(st)) dirs.push_back(e.path());
            else if (fs::is_regular_file(st) && player::isSupportedFile(name)) files.push_back(e.path());
        }
        std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) { return naturalLess(utf8(a.filename()), utf8(b.filename())); });
        std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return naturalLess(utf8(a.filename()), utf8(b.filename())); });
        for (const auto& p : files) {
            if (cancel_.load() || idx->tracks.size() >= kMaxTracks) break;
            const std::string path = utf8(p);
            std::error_code ecSz;
            const uint64_t sz = fs::file_size(p, ecSz);
            const int64_t mt = fileTime(p);
            if (!ecSz) {
                auto it = cachedByPath.find(path);
                if (it != cachedByPath.end() && it->second->size == sz && it->second->mtime == mt) {
                    Track t = *it->second;
                    t.folderId = idx->folders[self].id;
                    idx->folders[self].tracks.push_back(idx->tracks.size());
                    idx->tracks.push_back(std::move(t));
                    ++scanned_;
                    continue;
                }
            }
            auto dec = player::openDecoder(path, nullptr, true);
            ++scanned_;
            if (!dec) continue;
            Track t;
            t.path = path;
            t.id = idOf('t', path);
            t.file = utf8(p.filename());
            t.size = sz;
            t.mtime = mt;
            t.added = mt;
            const auto& tg = dec->tags();
            t.title = tg.title.empty() ? utf8(p.stem()) : tg.title;
            t.artist = tg.artist;
            t.album = tg.album;
            t.albumArtist = tg.albumArtist;
            t.track = tg.track;
            t.disc = tg.disc;
            t.year = tg.year;
            t.genre = tg.genre;
            t.rate = dec->sampleRate();
            t.channels = dec->channels();
            t.duration = t.rate && dec->totalFrames() ? static_cast<double>(dec->totalFrames()) / t.rate : 0.0;
            t.embeddedArt = !tg.art.empty();
            t.folderId = idx->folders[self].id;
            idx->folders[self].tracks.push_back(idx->tracks.size());
            idx->tracks.push_back(std::move(t));
        }
        if (!idx->folders[self].tracks.empty()) idx->folders[self].coverFile = coverIn(dir);
        size_t total = idx->folders[self].tracks.size();
        for (const auto& d : dirs) {
            const size_t child = walk(d, idx->folders[self].id, depth + 1);
            if (child == SIZE_MAX) continue;
            idx->folders[self].folders.push_back(child);
            total += idx->folders[child].totalTracks;
        }
        idx->folders[self].totalTracks = total;
        return self;
    };

    for (const auto& r : roots) {
        std::error_code ec;
        const fs::path root = fs::weakly_canonical(fromUtf8(r), ec);
        if (ec || !fs::is_directory(root, ec) || !seenRoots.insert(utf8(root)).second) continue;
        const size_t i = walk(root, std::string(), 0);
        if (i != SIZE_MAX) {
            idx->folders[i].name = utf8(root.filename()).empty() ? utf8(root) : utf8(root.filename());
            idx->roots.push_back(i);
        }
    }

    // üres részfák nem látszanak (a gyökér igen, hogy a felhasználó lássa a beállított mappát)
    for (auto& f : idx->folders) {
        std::vector<size_t> keep;
        for (size_t c : f.folders)
            if (idx->folders[c].totalTracks > 0) keep.push_back(c);
        f.folders = std::move(keep);
    }

    buildAlbumsAndMaps(*idx);
    return idx;
}

static void buildAlbumsAndMaps(Index& idx) {
    // albumok: albumelőadó (ennek hiányában előadó) + albumcím; címke nélkül a mappa
    std::map<std::string, size_t> albumByKey;
    std::unordered_map<std::string, size_t> folderIdx;
    for (size_t i = 0; i < idx.folders.size(); ++i) folderIdx.emplace(idx.folders[i].id, i);
    for (size_t i = 0; i < idx.tracks.size(); ++i) {
        Track& t = idx.tracks[i];
        const Folder& fo = idx.folders[folderIdx[t.folderId]];
        std::string key, title;
        if (!t.album.empty()) {
            key = "a|" + lower(t.albumArtist.empty() ? t.artist : t.albumArtist) + "|" + lower(t.album);
            title = t.album;
        } else {
            key = "f|" + fo.path;
            title = fo.name;
        }
        auto it = albumByKey.find(key);
        if (it == albumByKey.end()) {
            Album a;
            a.id = idOf('a', key);
            a.title = title;
            a.artist = t.albumArtist.empty() ? t.artist : t.albumArtist;
            it = albumByKey.emplace(key, idx.albums.size()).first;
            idx.albums.push_back(std::move(a));
        }
        Album& a = idx.albums[it->second];
        if (t.albumArtist.empty() && !a.various && !a.tracks.empty() && t.artist != idx.tracks[a.tracks.front()].artist) {
            a.various = true;
            a.artist.clear();
        }
        if (!a.year && t.year) a.year = t.year;
        a.duration += t.duration;
        if (a.artTrack.empty() && t.embeddedArt) a.artTrack = t.path;
        if (a.coverFile.empty()) a.coverFile = fo.coverFile;
        t.albumId = a.id;
        a.tracks.push_back(i);
    }
    for (Album& a : idx.albums)
        std::sort(a.tracks.begin(), a.tracks.end(), [&](size_t x, size_t y) {
            const Track &tx = idx.tracks[x], &ty = idx.tracks[y];
            if (tx.disc != ty.disc) return tx.disc < ty.disc;
            if (tx.track != ty.track) return tx.track < ty.track;
            return naturalLess(tx.file, ty.file);
        });
    std::sort(idx.albums.begin(), idx.albums.end(), [](const Album& x, const Album& y) {
        const std::string ax = x.various ? std::string("\xff") : x.artist, ay = y.various ? std::string("\xff") : y.artist;
        if (lower(ax) != lower(ay)) return naturalLess(ax, ay);
        if (x.year != y.year) return x.year < y.year;
        return naturalLess(x.title, y.title);
    });
    for (size_t i = 0; i < idx.tracks.size(); ++i) idx.trackById.emplace(idx.tracks[i].id, i);
    for (size_t i = 0; i < idx.albums.size(); ++i) idx.albumById.emplace(idx.albums[i].id, i);
    idx.folderById = std::move(folderIdx);
}

std::shared_ptr<Index> buildIndexFromTracks(const std::vector<std::string>& roots, std::vector<Track> tracks) {
    auto idx = std::make_shared<Index>();
    if (roots.empty()) return idx;

    std::map<std::string, size_t> folderByPath;
    for (const auto& r : roots) {
        std::error_code ec;
        fs::path rp = fs::weakly_canonical(fromUtf8(r), ec);
        if (ec) rp = fromUtf8(r);
        std::string rPath = utf8(rp);
        if (folderByPath.find(rPath) != folderByPath.end()) continue;

        Folder f;
        f.path = rPath;
        f.id = idOf('f', rPath);
        f.name = utf8(rp.filename()).empty() ? rPath : utf8(rp.filename());
        const size_t fi = idx->folders.size();
        folderByPath[rPath] = fi;
        idx->folders.push_back(std::move(f));
        idx->roots.push_back(fi);
    }

    auto ensureFolder = [&](const fs::path& dir) -> size_t {
        std::string dPath = utf8(dir);
        auto it = folderByPath.find(dPath);
        if (it != folderByPath.end()) return it->second;

        Folder f;
        f.path = dPath;
        f.id = idOf('f', dPath);
        f.name = utf8(dir.filename()).empty() ? dPath : utf8(dir.filename());
        const size_t fi = idx->folders.size();
        folderByPath[dPath] = fi;
        idx->folders.push_back(std::move(f));
        return fi;
    };

    for (const auto& t : tracks) {
        fs::path p = fromUtf8(t.path);
        fs::path dir = p.parent_path();
        if (!dir.empty()) ensureFolder(dir);
    }

    for (size_t i = 0; i < idx->folders.size(); ++i) {
        fs::path dir = fromUtf8(idx->folders[i].path);
        fs::path parent = dir.parent_path();
        auto it = folderByPath.find(utf8(parent));
        if (it != folderByPath.end() && it->second != i) {
            idx->folders[i].parentId = idx->folders[it->second].id;
            idx->folders[it->second].folders.push_back(i);
        }
    }

    idx->tracks = std::move(tracks);
    for (size_t i = 0; i < idx->tracks.size(); ++i) {
        fs::path p = fromUtf8(idx->tracks[i].path);
        fs::path dir = p.parent_path();
        auto it = folderByPath.find(utf8(dir));
        if (it != folderByPath.end()) {
            idx->folders[it->second].tracks.push_back(i);
            idx->tracks[i].folderId = idx->folders[it->second].id;
        }
    }

    for (auto& f : idx->folders) {
        if (!f.tracks.empty()) {
            f.coverFile = coverIn(fromUtf8(f.path));
        }
        f.totalTracks = f.tracks.size();
    }
    for (size_t i = idx->folders.size(); i > 0; --i) {
        const size_t fi = i - 1;
        for (size_t child : idx->folders[fi].folders) {
            idx->folders[fi].totalTracks += idx->folders[child].totalTracks;
        }
    }

    for (auto& f : idx->folders) {
        std::vector<size_t> keep;
        for (size_t c : f.folders)
            if (idx->folders[c].totalTracks > 0) keep.push_back(c);
        f.folders = std::move(keep);
    }

    buildAlbumsAndMaps(*idx);
    return idx;
}

std::string serializeCache(const Index& idx, const std::vector<std::string>& roots) {
    std::string s = "{\"version\":1,\"roots\":[";
    for (size_t i = 0; i < roots.size(); ++i) s += (i ? "," : "") + q(roots[i]);
    s += "],\"tracks\":[";
    for (size_t i = 0; i < idx.tracks.size(); ++i) {
        const Track& t = idx.tracks[i];
        if (i) s += ',';
        s += "{\"path\":" + q(t.path) +
             ",\"size\":" + std::to_string(t.size) +
             ",\"mtime\":" + std::to_string(t.mtime) +
             ",\"title\":" + q(t.title) +
             ",\"artist\":" + q(t.artist) +
             ",\"album\":" + q(t.album) +
             ",\"albumArtist\":" + q(t.albumArtist) +
             ",\"genre\":" + q(t.genre) +
             ",\"track\":" + std::to_string(t.track) +
             ",\"disc\":" + std::to_string(t.disc) +
             ",\"year\":" + std::to_string(t.year) +
             ",\"duration\":" + std::to_string(t.duration) +
             ",\"rate\":" + std::to_string(t.rate) +
             ",\"channels\":" + std::to_string(t.channels) +
             ",\"art\":" + (t.embeddedArt ? "true" : "false") + "}";
    }
    s += "]}";
    return s;
}

std::shared_ptr<Index> parseCache(const std::string& doc, const std::vector<std::string>& roots) {
    if (doc.empty() || doc.size() > 64 * 1024 * 1024) return nullptr;
    json::Value rootVal;
    if (!json::parse(doc, rootVal) || rootVal.type != json::Value::Type::Object) return nullptr;

    const json::Value* ver = rootVal.get("version");
    if (!ver || ver->type != json::Value::Type::Number || ver->num != 1.0) return nullptr;

    const json::Value* rArr = rootVal.get("roots");
    if (!rArr || rArr->type != json::Value::Type::Array) return nullptr;
    std::vector<std::string> cachedRoots;
    for (const auto& r : rArr->arr)
        if (r.type == json::Value::Type::String) cachedRoots.push_back(r.str);
    if (cachedRoots != roots) return nullptr;

    const json::Value* tArr = rootVal.get("tracks");
    if (!tArr || tArr->type != json::Value::Type::Array) return nullptr;

    std::vector<Track> tracks;
    tracks.reserve(tArr->arr.size());
    for (const auto& tv : tArr->arr) {
        if (tv.type != json::Value::Type::Object) continue;
        auto s = [&](const char* k) { const auto* v = tv.get(k); return (v && v->type == json::Value::Type::String) ? v->str : std::string(); };
        auto n = [&](const char* k, double def) { const auto* v = tv.get(k); return v ? v->numberOr(def) : def; };
        auto b = [&](const char* k) { const auto* v = tv.get(k); return v && v->type == json::Value::Type::Bool && v->b; };

        Track t;
        t.path = s("path");
        if (t.path.empty()) continue;
        t.id = idOf('t', t.path);
        t.size = static_cast<uint64_t>(std::max(0.0, n("size", 0.0)));
        t.mtime = static_cast<int64_t>(n("mtime", 0.0));
        t.added = t.mtime;
        t.title = s("title");
        t.artist = s("artist");
        t.album = s("album");
        t.albumArtist = s("albumArtist");
        t.genre = s("genre");
        t.track = static_cast<int>(n("track", 0.0));
        t.disc = static_cast<int>(n("disc", 0.0));
        t.year = static_cast<int>(n("year", 0.0));
        t.duration = n("duration", 0.0);
        t.rate = static_cast<uint32_t>(n("rate", 0.0));
        t.channels = static_cast<uint32_t>(n("channels", 0.0));
        t.embeddedArt = b("art");

        size_t slash = t.path.find_last_of("/\\");
        t.file = (slash == std::string::npos) ? t.path : t.path.substr(slash + 1);
        tracks.push_back(std::move(t));
    }
    if (tracks.empty()) return nullptr;

    return buildIndexFromTracks(roots, std::move(tracks));
}

// ---------------------------------------------------------------- lekérdezések

std::string MusicLibrary::infoJson(bool includeRoots) const {
    const auto idx = snapshot();
    std::string s = "{\"type\":\"library\",\"scanning\":";
    s += scanning_.load() ? "true" : "false";
    s += ",\"scanned\":" + std::to_string(scanned_.load()) + ",\"tracks\":" + std::to_string(idx->tracks.size()) +
         ",\"albums\":" + std::to_string(idx->albums.size()) + ",\"folders\":" + std::to_string(idx->folders.size()) +
         ",\"cached\":" + (cached_.load() ? "true" : "false");
    if (includeRoots) {
        s += ",\"roots\":[";
        const auto r = roots();
        for (size_t i = 0; i < r.size(); ++i) s += (i ? "," : "") + q(r[i]);
        s += "]";
    }
    return s + "}";
}

std::string MusicLibrary::albumsJson(size_t offset, size_t limit) const {
    const auto idx = snapshot();
    limit = std::clamp<size_t>(limit, 1, 500);
    std::string s = "{\"type\":\"library_albums\",\"offset\":" + std::to_string(offset) + ",\"total\":" +
                    std::to_string(idx->albums.size()) + ",\"items\":[";
    for (size_t i = offset, n = 0; i < idx->albums.size() && n < limit; ++i, ++n) {
        const Album& a = idx->albums[i];
        if (n) s += ',';
        s += "{\"id\":" + q(a.id) + ",\"title\":" + q(a.title) + ",\"artist\":" + q(a.artist) + ",\"various\":" +
             (a.various ? "true" : "false") + ",\"year\":" + std::to_string(a.year) + ",\"tracks\":" + std::to_string(a.tracks.size());
        appendNum(s, "duration", a.duration);
        s += std::string(",\"art\":") + (a.artTrack.empty() && a.coverFile.empty() ? "false" : "true") + "}";
    }
    return s + "]}";
}

std::string MusicLibrary::albumJson(const std::string& id) const {
    const auto idx = snapshot();
    auto it = idx->albumById.find(id);
    if (it == idx->albumById.end()) return {};
    const Album& a = idx->albums[it->second];
    std::string s = "{\"type\":\"library_album\",\"id\":" + q(a.id) + ",\"title\":" + q(a.title) + ",\"artist\":" + q(a.artist) +
                    ",\"various\":" + (a.various ? "true" : "false") + ",\"year\":" + std::to_string(a.year);
    appendNum(s, "duration", a.duration);
    s += std::string(",\"art\":") + (a.artTrack.empty() && a.coverFile.empty() ? "false" : "true") + ",\"tracks\":[";
    for (size_t k = 0; k < a.tracks.size(); ++k) s += (k ? "," : "") + trackJson(idx->tracks[a.tracks[k]]);
    return s + "]}";
}

std::string MusicLibrary::folderJson(const std::string& id) const {
    const auto idx = snapshot();
    auto folderEntry = [&](const Folder& f) {
        return "{\"id\":" + q(f.id) + ",\"name\":" + q(f.name) + ",\"tracks\":" + std::to_string(f.totalTracks) +
               ",\"art\":" + (f.coverFile.empty() ? "false" : "true") + "}";
    };
    if (id.empty()) {
        std::string s = "{\"type\":\"library_folder\",\"id\":\"\",\"name\":\"\",\"parent\":null,\"folders\":[";
        for (size_t k = 0; k < idx->roots.size(); ++k) s += (k ? "," : "") + folderEntry(idx->folders[idx->roots[k]]);
        return s + "],\"tracks\":[]}";
    }
    auto it = idx->folderById.find(id);
    if (it == idx->folderById.end()) return {};
    const Folder& f = idx->folders[it->second];
    std::string s = "{\"type\":\"library_folder\",\"id\":" + q(f.id) + ",\"name\":" + q(f.name) + ",\"parent\":" +
                    (f.parentId.empty() ? std::string("\"\"") : q(f.parentId)) + ",\"art\":" +
                    (f.coverFile.empty() ? "false" : "true") + ",\"folders\":[";
    for (size_t k = 0; k < f.folders.size(); ++k) s += (k ? "," : "") + folderEntry(idx->folders[f.folders[k]]);
    s += "],\"tracks\":[";
    for (size_t k = 0; k < f.tracks.size(); ++k) s += (k ? "," : "") + trackJson(idx->tracks[f.tracks[k]]);
    return s + "]}";
}

bool MusicLibrary::trackPath(const std::string& id, std::string& path) const {
    const auto idx = snapshot();
    auto it = idx->trackById.find(id);
    if (it == idx->trackById.end()) return false;
    path = idx->tracks[it->second].path;
    return true;
}

bool MusicLibrary::art(const std::string& id, std::string& mime, std::string& bytes) const {
    const auto idx = snapshot();
    auto embedded = [&](const std::string& path) {
        if (path.empty()) return false;
        auto d = player::openDecoder(path, nullptr, true);
        if (!d || d->tags().art.empty()) return false;
        mime = d->tags().artMime;
        bytes = d->tags().art;
        return true;
    };
    auto file = [&](const std::string& path) {
        if (path.empty() || !readFile(path, bytes, kMaxCoverBytes)) return false;
        mime = mimeOf(path);
        return true;
    };
    if (auto a = idx->albumById.find(id); a != idx->albumById.end()) {
        const Album& al = idx->albums[a->second];
        return embedded(al.artTrack) || file(al.coverFile);
    }
    if (auto f = idx->folderById.find(id); f != idx->folderById.end()) {
        const Folder& fo = idx->folders[f->second];
        if (file(fo.coverFile)) return true;
        for (size_t t : fo.tracks)
            if (idx->tracks[t].embeddedArt) return embedded(idx->tracks[t].path);
        return false;
    }
    if (auto t = idx->trackById.find(id); t != idx->trackById.end()) {
        const Track& tr = idx->tracks[t->second];
        if (tr.embeddedArt && embedded(tr.path)) return true;
        auto f = idx->folderById.find(tr.folderId);
        return f != idx->folderById.end() && file(idx->folders[f->second].coverFile);
    }
    return false;
}

bool MusicLibrary::matchLocalTrack(const std::string& title, const std::string& artist, double duration, std::string& outId) const {
    if (title.empty()) return false;
    auto snap = snapshot();
    if (!snap || snap->tracks.empty()) return false;

    auto normalize = [](const std::string& s) {
        std::string r;
        r.reserve(s.size());
        for (unsigned char c : s) {
            if (c >= 'A' && c <= 'Z') r += static_cast<char>(c + ('a' - 'A'));
            else if (c > 32) r += static_cast<char>(c);
        }
        return r;
    };
    const std::string tNorm = normalize(title);
    const std::string aNorm = normalize(artist);
    if (tNorm.empty()) return false;

    for (const auto& tr : snap->tracks) {
        if (normalize(tr.title) == tNorm) {
            if (!aNorm.empty() && !tr.artist.empty()) {
                if (normalize(tr.artist) != aNorm) continue;
            }
            if (duration > 0.0 && tr.duration > 0.0) {
                if (std::abs(duration - tr.duration) > 2.5) continue;
            }
            outId = tr.id;
            return true;
        }
    }
    return false;
}

static std::string sanitizePathSegment(const std::string& in, const std::string& def) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || static_cast<unsigned char>(c) < 32) {
            out += '_';
        } else {
            out += c;
        }
    }
    size_t start = out.find_first_not_of(" .");
    size_t end = out.find_last_not_of(" .");
    if (start == std::string::npos || end == std::string::npos) return def;
    out = out.substr(start, end - start + 1);
    if (out.empty() || out == "." || out == "..") return def;
    return out.substr(0, 100);
}

bool MusicLibrary::importTrack(const std::string& sourcePath, const std::string& artist,
                               const std::string& album, const std::string& filename,
                               std::string& outPath) {
    if (sourcePath.empty()) return false;
    std::error_code ec;
    fs::path src = fromUtf8(sourcePath);
    if (!fs::exists(src, ec) || !fs::is_regular_file(src, ec)) return false;

    std::string targetRoot;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!roots_.empty()) targetRoot = roots_[0];
    }
    if (targetRoot.empty()) targetRoot = defaultMusicDir();
    if (targetRoot.empty()) return false;

    const std::string safeArtist = sanitizePathSegment(artist, "Ismeretlen előadó");
    const std::string safeAlbum = sanitizePathSegment(album, "Ismeretlen album");
    std::string safeFilename = sanitizePathSegment(filename, utf8(src.filename()));
    if (safeFilename.empty()) safeFilename = "track.flac";

    fs::path destDir = fromUtf8(targetRoot) / u8"Aqulify a telefonról" / fromUtf8(safeArtist) / fromUtf8(safeAlbum);
    fs::create_directories(destDir, ec);
    if (ec) return false;

    fs::path destFile = destDir / fromUtf8(safeFilename);
    if (fs::exists(destFile, ec)) {
        if (fs::file_size(src, ec) == fs::file_size(destFile, ec)) {
            outPath = utf8(destFile);
            rescan();
            return true;
        }
    }

    fs::copy_file(src, destFile, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;

    outPath = utf8(destFile);
    rescan();
    return true;
}

bool folderCover(const std::string& audioPath, std::string& mime, std::string& bytes) {
    const std::string cover = coverIn(fs::path(fromUtf8(audioPath)).parent_path());
    if (cover.empty() || !readFile(cover, bytes, kMaxCoverBytes)) return false;
    mime = mimeOf(cover);
    return true;
}

} // namespace ee_analyzer::library
