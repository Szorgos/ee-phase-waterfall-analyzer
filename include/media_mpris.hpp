#pragma once
/**
 * @file media_mpris.hpp
 * @brief „Most szól” és lejátszásvezérlés Linuxon (MPRIS, a munkamenet D-Bus buszán).
 *
 * Az MPRIS-t a lejátszók többsége támogatja (böngészők – így a TIDAL webes lejátszója –, tidal-hifi, Spotify,
 * Strawberry, VLC…). A busz eléréséhez a systemd `busctl` programját használjuk JSON kimenettel (shell nélkül,
 * paraméterlistával indítva), így a motor új könyvtárfüggőséget nem kap. Ha nincs `busctl`, a szolgáltatás nem elérhető.
 */
#include <spawn.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "media_session.hpp"
#include "mini_json.hpp"

extern char** environ;

namespace ee_analyzer::media {

class MprisSession {
public:
    MprisSession() = default;
    ~MprisSession() { stop(); }
    MprisSession(const MprisSession&) = delete;
    MprisSession& operator=(const MprisSession&) = delete;

    void start() {
        if (running_.exchange(true)) return;
        thread_ = std::thread([this] { loop(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
    }

    Info snapshot() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return info_;
    }

    std::string json() const { return toJson(snapshot()); }

    /** Parancs a kiválasztott lejátszónak: a lekérdező szál hajtja végre (a hívó, pl. a híd, nem vár rá). */
    void command(const std::string& action, double value) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (pending_.size() < 16) pending_.push_back({ action, value });
        }
        wake_ = true;
    }

    /** A borító: helyi fájl (file://) bájtjai, vagy a lejátszó https címe. */

    bool art(const std::string& id, std::string& mime, std::string& bytes, std::string& url) const {
        std::string artUrl;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (id != info_.artId) return false;
            artUrl = artUrl_;
        }
        if (artUrl.rfind("https://", 0) == 0) {
            url = artUrl;
            return true;
        }
        if (artUrl.rfind("file://", 0) != 0) return false;
        const std::string path = urlDecode(artUrl.substr(7));
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (bytes.empty() || bytes.size() > (2u << 20)) return false;
        mime = bytes.size() > 3 && static_cast<unsigned char>(bytes[0]) == 0x89 && bytes[1] == 'P' ? "image/png"
             : bytes.size() > 3 && bytes[0] == 'R' && bytes[1] == 'I' ? "image/webp" : "image/jpeg";
        return true;
    }

private:
    static constexpr const char* kPath = "/org/mpris/MediaPlayer2";
    static constexpr const char* kPlayer = "org.mpris.MediaPlayer2.Player";

    static bool validBusName(const std::string& n) {
        return !n.empty() && n.size() < 256 &&
               std::all_of(n.begin(), n.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-'; });
    }
    static bool validObjectPath(const std::string& p) {
        return !p.empty() && p[0] == '/' && p.size() < 512 &&
               std::all_of(p.begin(), p.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '/'; });
    }

    static std::string urlDecode(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
                out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
                i += 2;
            } else out += s[i];
        }
        return out;
    }

    /** Program futtatása paraméterlistával (shell nélkül); a szabványos kimenet, legfeljebb ~2 s várakozással. */
    static bool run(const std::vector<std::string>& argv, std::string* out = nullptr) {
        int pipefd[2];
        if (pipe(pipefd) != 0) return false;
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addclose(&fa, pipefd[0]);
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        pid_t pid = -1;
        const int rc = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        close(pipefd[1]);
        if (rc != 0) { close(pipefd[0]); return false; }
        std::string buf;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        char tmp[4096];
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
            if (left <= 0) { kill(pid, SIGKILL); break; }
            pollfd p{ pipefd[0], POLLIN, 0 };
            if (::poll(&p, 1, static_cast<int>(left)) <= 0) continue;
            const ssize_t n = read(pipefd[0], tmp, sizeof(tmp));
            if (n <= 0) break;
            if (buf.size() < (4u << 20)) buf.append(tmp, static_cast<size_t>(n));
        }
        close(pipefd[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        if (out) *out = std::move(buf);
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    /** busctl --json=short: {"type":"…","data":[ … ]} – a hívás visszatérési értékeinek listája. */
    static bool callJson(const std::vector<std::string>& argv, json::Value& data) {
        std::string out;
        if (!run(argv, &out)) return false;
        json::Value v;
        if (!json::parse(out, v)) return false;
        const json::Value* d = v.get("data");
        if (!d || d->type != json::Value::Type::Array || d->arr.empty()) return false;
        data = d->arr[0];
        return true;
    }

    /** Egy a{sv} szótár egy mezője (busctl: {"név":{"type":"s","data":…}}). */
    static const json::Value* field(const json::Value& dict, const char* key) {
        const json::Value* e = dict.get(key);
        return e ? e->get("data") : nullptr;
    }
    static std::string str(const json::Value& dict, const char* key) {
        const json::Value* d = field(dict, key);
        if (!d) return {};
        if (d->type == json::Value::Type::String) return d->str;
        if (d->type == json::Value::Type::Array) {   // pl. xesam:artist (as)
            std::string s;
            for (const auto& x : d->arr)
                if (x.type == json::Value::Type::String) s += (s.empty() ? "" : ", ") + x.str;
            return s;
        }
        return {};
    }
    static bool flag(const json::Value& dict, const char* key) {
        const json::Value* d = field(dict, key);
        return d && ((d->type == json::Value::Type::Bool && d->b) || d->numberOr(0.0) != 0.0);
    }

    struct Player {
        std::string name, identity, status, trackId, artUrl;
        Info info;
    };

    bool readPlayer(const std::string& name, Player& p) {
        json::Value props;
        if (!callJson({ "busctl", "--user", "--json=short", "call", name, kPath, "org.freedesktop.DBus.Properties",
                        "GetAll", "s", kPlayer }, props))
            return false;
        p.name = name;
        if (auto it = identities_.find(name); it != identities_.end()) {
            p.identity = it->second;
        } else {
            json::Value root;
            if (callJson({ "busctl", "--user", "--json=short", "call", name, kPath, "org.freedesktop.DBus.Properties",
                           "GetAll", "s", "org.mpris.MediaPlayer2" }, root))
                p.identity = str(root, "Identity");
            if (p.identity.empty()) p.identity = name.substr(std::strlen("org.mpris.MediaPlayer2."));
            identities_[name] = p.identity;
        }
        p.status = str(props, "PlaybackStatus");
        Info& m = p.info;
        m.available = true;
        m.app = knownApp(p.identity + " " + name);
        if (m.app.empty()) m.app = p.identity;
        m.playing = p.status == "Playing";
        m.canPlay = flag(props, "CanPlay");
        m.canPause = flag(props, "CanPause");
        m.canNext = flag(props, "CanGoNext");
        m.canPrevious = flag(props, "CanGoPrevious");
        m.canSeek = flag(props, "CanSeek");
        if (const json::Value* pos = field(props, "Position")) m.position = pos->numberOr(0.0) / 1e6;
        m.positionAt = std::chrono::steady_clock::now();
        if (const json::Value* meta = field(props, "Metadata")) {
            m.title = str(*meta, "xesam:title");
            m.artist = str(*meta, "xesam:artist");
            m.album = str(*meta, "xesam:album");
            if (const json::Value* len = field(*meta, "mpris:length")) m.duration = len->numberOr(0.0) / 1e6;
            p.trackId = str(*meta, "mpris:trackid");
            p.artUrl = str(*meta, "mpris:artUrl");
            if (!p.artUrl.empty()) m.artId = hashId(p.artUrl);
        }
        return true;
    }

    void refresh() {
        json::Value names;
        std::vector<Player> players;
        if (callJson({ "busctl", "--user", "--json=short", "call", "org.freedesktop.DBus", "/org/freedesktop/DBus",
                       "org.freedesktop.DBus", "ListNames" }, names) &&
            names.type == json::Value::Type::Array) {
            for (const auto& n : names.arr) {
                if (n.type != json::Value::Type::String || n.str.rfind("org.mpris.MediaPlayer2.", 0) != 0 || !validBusName(n.str))
                    continue;
                Player p;
                if (readPlayer(n.str, p)) players.push_back(std::move(p));
            }
        } else if (!warned_) {
            warned_ = true;
            std::cerr << "[Média] A munkamenet D-Bus nem érhető el (busctl), a „most szól” nem működik.\n";
        }

        // Melyik lejátszó: ami épp szól (a legutóbbi előnyben), különben a legutóbbi, különben az első szüneteltetett
        const Player* pick = nullptr;
        for (const auto& p : players)
            if (p.info.playing && (!pick || p.name == lastActive_)) pick = &p;
        if (!pick)
            for (const auto& p : players)
                if (p.name == lastActive_) pick = &p;
        if (!pick)
            for (const auto& p : players)
                if (p.status == "Paused") { pick = &p; break; }

        std::lock_guard<std::mutex> lk(mutex_);
        if (pick) {
            info_ = pick->info;
            active_ = pick->name;
            trackId_ = pick->trackId;
            artUrl_ = pick->artUrl;
            if (pick->info.playing) lastActive_ = pick->name;
        } else {
            info_ = Info{};
            active_.clear();
            trackId_.clear();
            artUrl_.clear();
        }
    }

    void execute(const std::string& action, double value) {
        std::string dest, track;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            dest = active_;
            track = trackId_;
        }
        if (dest.empty()) return;
        const char* method = action == "play" ? "Play" : action == "pause" ? "Pause" : action == "toggle" ? "PlayPause"
                           : action == "next" ? "Next" : action == "previous" ? "Previous" : nullptr;
        if (method) {
            run({ "busctl", "--user", "call", dest, kPath, kPlayer, method });
        } else if (action == "seek" && validObjectPath(track)) {
            const long long us = static_cast<long long>(std::max(0.0, value) * 1e6);
            run({ "busctl", "--user", "call", dest, kPath, kPlayer, "SetPosition", "ox", track, std::to_string(us) });
        }
    }

    void loop() {
        while (running_) {
            std::vector<std::pair<std::string, double>> todo;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                todo.swap(pending_);
            }
            for (const auto& [a, v] : todo) execute(a, v);
            refresh();
            for (int i = 0; i < 20 && running_ && !wake_.exchange(false); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));   // kb. 1 s-onként, parancs után azonnal
        }
    }

    std::atomic<bool> running_{false};
    std::atomic<bool> wake_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    Info info_;
    std::string active_, trackId_, artUrl_;
    std::vector<std::pair<std::string, double>> pending_;
    // csak a lekérdező szál
    std::string lastActive_;
    std::map<std::string, std::string> identities_;
    bool warned_ = false;
};

} // namespace ee_analyzer::media
