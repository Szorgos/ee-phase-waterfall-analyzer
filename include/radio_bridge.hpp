#pragma once
/**
 * @file radio_bridge.hpp
 * @brief Internetrádió: a felhasználó állomáslistája (mentés a platform tárolójába, radio.json), a lejátszás vezérlője
 *        és a WebSocket-híd üzenetei. A Linuxos, a Windows-os és a macOS-es motor közös kódja.
 *
 * Üzenetek (a leírás: docs/remote-api.md, „Internetrádió”):
 *   radio_list                       -> radio {rev, items:[{id, name, url}]}
 *   radio_play {id}                  a lista egy állomása (a hálózatról is) -> radio_result {ok}
 *   radio_stop                       -> radio_result {ok}
 *   radio_add {name, url}            helyi / admin kliens -> radio
 *   radio_update {id, name?, url?}   helyi / admin kliens -> radio
 *   radio_remove {id}                helyi / admin kliens -> radio
 *   radio_move {from, to}            helyi / admin kliens -> radio
 *   Hibánál radio_result {ok:false, error}. Az állapotüzenet "radio" mezője: {rev, count, state, station, error,
 *   codec, bitrate, reconnects}; state: idle | connecting | buffering | playing | reconnecting | error.
 *   A „most szól” a meglévő "media" mezőben jön (live: true; title / artist a folyam címéből, album az állomás neve).
 * A hálózatról tetszőleges cím nem játszható le, csak a listában lévő állomás; a listát a saját gép vagy egy adminisztrátor
 * távirányító szerkesztheti (a híd a többi távoli kliensnél "remote" hibát ad).
 */
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "file_player.hpp"
#include "media_session.hpp"
#include "mini_json.hpp"
#include "radio_net.hpp"
#include "radio_stream.hpp"
#include "ws_bridge.hpp"

namespace ee_analyzer::radio {

struct Station {
    std::string id, name, url;
};

/** A felhasználó saját állomáslistája. Előre betöltött állomás nincs. Szálbiztos. */
class StationStore {
public:
    using Save = std::function<void(const std::string& doc)>;
    static constexpr size_t kMaxStations = 500;
    static constexpr size_t kMaxNameBytes = 120;

    explicit StationStore(Save save) : save_(std::move(save)) {}

    /** A mentett dokumentum betöltése (a hibás tételek kimaradnak). */
    void load(const std::string& doc) {
        json::Value v;
        std::lock_guard<std::mutex> lk(m_);
        items_.clear();
        if (doc.empty() || !json::parse(doc, v)) return;
        const json::Value* a = v.get("stations");
        if (!a || a->type != json::Value::Type::Array) return;
        for (const auto& e : a->arr) {
            if (items_.size() >= kMaxStations) break;
            auto s = [&](const char* k) { const json::Value* x = e.get(k); return x ? x->stringOr("") : std::string(); };
            Station st{ s("id"), cleanText(s("name"), kMaxNameBytes), s("url") };
            RadioUrl u;
            if (!validId(st.id) || st.name.empty() || !parseRadioUrl(st.url, u, nullptr)) continue;
            if (indexLocked(st.id) >= 0) continue;
            items_.push_back(std::move(st));
        }
        ++rev_;
    }

    std::vector<Station> all() const { std::lock_guard<std::mutex> lk(m_); return items_; }
    size_t count() const { std::lock_guard<std::mutex> lk(m_); return items_.size(); }
    uint64_t rev() const { std::lock_guard<std::mutex> lk(m_); return rev_; }
    bool get(const std::string& id, Station& out) const {
        std::lock_guard<std::mutex> lk(m_);
        const int i = indexLocked(id);
        if (i < 0) return false;
        out = items_[static_cast<size_t>(i)];
        return true;
    }

    /** Új állomás; "" siker (idOut: az azonosítója), különben a hiba kódja: "name", "url", "full". */
    std::string add(const std::string& name, const std::string& url, std::string& idOut) {
        const std::string n = cleanText(name, kMaxNameBytes);
        RadioUrl u;
        if (n.empty()) return "name";
        if (!parseRadioUrl(url, u, nullptr)) return "url";
        std::string doc;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (items_.size() >= kMaxStations) return "full";
            std::string id;
            do id = newId(); while (indexLocked(id) >= 0);
            items_.push_back({ id, n, url });
            idOut = id;
            ++rev_;
            doc = docLocked();
        }
        if (save_) save_(doc);
        return "";
    }

    /** Név és / vagy cím módosítása (a nullptr mező marad). */
    std::string update(const std::string& id, const std::string* name, const std::string* url) {
        std::string n;
        if (name && (n = cleanText(*name, kMaxNameBytes)).empty()) return "name";
        RadioUrl u;
        if (url && !parseRadioUrl(*url, u, nullptr)) return "url";
        std::string doc;
        {
            std::lock_guard<std::mutex> lk(m_);
            const int i = indexLocked(id);
            if (i < 0) return "unknown";
            if (name) items_[static_cast<size_t>(i)].name = n;
            if (url) items_[static_cast<size_t>(i)].url = *url;
            ++rev_;
            doc = docLocked();
        }
        if (save_) save_(doc);
        return "";
    }

    std::string remove(const std::string& id) {
        std::string doc;
        {
            std::lock_guard<std::mutex> lk(m_);
            const int i = indexLocked(id);
            if (i < 0) return "unknown";
            items_.erase(items_.begin() + i);
            ++rev_;
            doc = docLocked();
        }
        if (save_) save_(doc);
        return "";
    }

    std::string move(size_t from, size_t to) {
        std::string doc;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (from >= items_.size() || to >= items_.size()) return "index";
            Station s = items_[from];
            items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(from));
            items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(to), std::move(s));
            ++rev_;
            doc = docLocked();
        }
        if (save_) save_(doc);
        return "";
    }

    std::string doc() const { std::lock_guard<std::mutex> lk(m_); return docLocked(); }

    /** A "radio" válaszüzenet (a lista). */
    std::string listJson() const {
        std::lock_guard<std::mutex> lk(m_);
        std::string s = "{\"type\":\"radio\",\"rev\":" + std::to_string(rev_) + ",\"items\":[";
        for (size_t i = 0; i < items_.size(); ++i)
            s += (i ? ",{" : "{") + std::string("\"id\":\"") + media::jsonEscape(items_[i].id) + "\",\"name\":\"" +
                 media::jsonEscape(items_[i].name) + "\",\"url\":\"" + media::jsonEscape(items_[i].url) + "\"}";
        return s + "]}";
    }

private:
    static bool validId(const std::string& id) {
        if (id.empty() || id.size() > 32) return false;
        for (char c : id)
            if (!std::isalnum(static_cast<unsigned char>(c))) return false;
        return true;
    }
    int indexLocked(const std::string& id) const {
        for (size_t i = 0; i < items_.size(); ++i)
            if (items_[i].id == id) return static_cast<int>(i);
        return -1;
    }
    std::string newId() {
        static const char* hex = "0123456789abcdef";
        std::string s = "r";
        uint64_t v = rng_();
        for (int i = 0; i < 10; ++i, v >>= 4) s += hex[v & 15];
        return s;
    }
    std::string docLocked() const {
        std::string s = "{\"version\":1,\"stations\":[";
        for (size_t i = 0; i < items_.size(); ++i)
            s += (i ? ",{" : "{") + std::string("\"id\":\"") + media::jsonEscape(items_[i].id) + "\",\"name\":\"" +
                 media::jsonEscape(items_[i].name) + "\",\"url\":\"" + media::jsonEscape(items_[i].url) + "\"}";
        return s + "]}\n";
    }

    Save save_;
    mutable std::mutex m_;
    std::vector<Station> items_;
    uint64_t rev_ = 1;
    std::mt19937_64 rng_{ std::random_device{}() };
};

/**
 * A rádió lejátszásának vezérlője: a kapcsolódás saját szálon fut (a híd és a lejátszó nem vár a hálózatra); az
 * újabb kérés (másik állomás, leállítás) a folyamatban lévőt megszakítja. Formátumváltáskor (pl. a szerver más
 * mintavétellel folytatja) az állomást magától újraindítja.
 */
class RadioController {
public:
    RadioController(player::FilePlayer& fp, StationStore& store, NetOptions net = {})
        : fp_(fp), store_(store), net_(std::move(net)) {
        th_ = std::thread([this] { loop(); });
    }
    ~RadioController() {
        {
            std::lock_guard<std::mutex> lk(m_);
            quit_ = true;
            cancel_ = true;
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }
    RadioController(const RadioController&) = delete;
    RadioController& operator=(const RadioController&) = delete;

    /** Az állomás indítása (aszinkron); "" siker, különben "unknown". */
    std::string play(const std::string& id) {
        Station s;
        if (!store_.get(id, s)) return "unknown";
        {
            std::lock_guard<std::mutex> lk(m_);
            want_ = id;
            ++gen_;
            cancel_ = true;
            connecting_ = true;
            station_ = id;
            error_.clear();
        }
        cv_.notify_all();
        return "";
    }

    /** A rádió leállítása (ha éppen a rádió szól; a többi lejátszást nem érinti). */
    void stop() {
        std::shared_ptr<RadioDecoder::Core> core;
        {
            std::lock_guard<std::mutex> lk(m_);
            want_.clear();
            ++gen_;
            cancel_ = true;
            connecting_ = false;
            error_.clear();
            station_.clear();
            core = core_;
            core_.reset();
        }
        cv_.notify_all();
        if (core && !RadioDecoder::closed(*core)) fp_.stop();
    }

    /** Az állapotüzenet "radio" mezője. */
    std::string statusJson() const {
        std::string state = "idle", station, error, codec;
        int bitrate = 0;
        uint64_t reconnects = 0;
        {
            std::lock_guard<std::mutex> lk(m_);
            station = station_;
            if (connecting_) state = "connecting";
            else if (!error_.empty()) { state = "error"; error = error_; }
            else if (core_ && !RadioDecoder::closed(*core_)) {
                const RadioStatus st = RadioDecoder::statusOf(*core_);
                state = st.state == RadioState::Ended ? "idle" : stateName(st.state);
                if (st.state == RadioState::Failed || st.state == RadioState::Reconnecting) error = st.error;
                codec = st.codec;
                bitrate = st.bitrate;
                reconnects = st.reconnects;
            } else {
                station.clear();
            }
        }
        std::string s = "{\"rev\":" + std::to_string(store_.rev()) + ",\"count\":" + std::to_string(store_.count()) +
                        ",\"state\":\"" + state + "\",\"station\":\"" + media::jsonEscape(station) + "\"";
        if (!error.empty()) s += ",\"error\":\"" + media::jsonEscape(error) + "\"";
        if (!codec.empty()) s += ",\"codec\":\"" + codec + "\",\"bitrate\":" + std::to_string(bitrate);
        if (reconnects) s += ",\"reconnects\":" + std::to_string(reconnects);
        return s + "}";
    }

private:
    void loop() {
        std::unique_lock<std::mutex> lk(m_);
        uint64_t seen = 0;
        while (!quit_) {
            cv_.wait_for(lk, std::chrono::seconds(1), [&] { return quit_ || gen_ != seen; });
            if (quit_) break;
            // formátumváltás miatt véget ért folyam: ugyanaz az állomás újra (új hangfolyam a lejátszóban)
            if (gen_ == seen && core_ && !RadioDecoder::closed(*core_)) {
                const RadioStatus st = RadioDecoder::statusOf(*core_);
                if (st.state == RadioState::Failed && st.error == "formatChange" && !station_.empty()) {
                    want_ = station_;
                    ++gen_;
                }
            }
            if (gen_ == seen) continue;
            seen = gen_;
            const std::string id = want_;
            if (id.empty()) continue;
            Station s;
            if (!store_.get(id, s)) { connecting_ = false; error_ = "unknown"; continue; }
            cancel_ = false;
            connecting_ = true;
            lk.unlock();
            RadioDecoder::Config cfg;
            cfg.url = s.url;
            cfg.station = s.name;
            cfg.net = net_;
            std::string err;
            auto dec = RadioDecoder::open(cfg, err, &cancel_);
            std::shared_ptr<RadioDecoder::Core> core = dec ? dec->core() : nullptr;
            lk.lock();
            if (quit_ || gen_ != seen) continue;   // közben új kérés jött: ez a kapcsolat elveszik (a dekóder megszűnik)
            if (!dec) {
                connecting_ = false;
                error_ = err;
                continue;
            }
            lk.unlock();
            std::string perr;
            const bool ok = fp_.openStream(std::move(dec), s.name, &perr);
            lk.lock();
            connecting_ = false;
            if (!ok) { error_ = perr.empty() ? "output" : perr; continue; }
            core_ = core;
            error_.clear();
            if (gen_ != seen && want_.empty()) {
                // a lejátszó átadása közben leállítást kértek: most állítjuk le
                core_.reset();
                lk.unlock();
                fp_.stop();
                lk.lock();
            }
        }
    }

    player::FilePlayer& fp_;
    StationStore& store_;
    NetOptions net_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::thread th_;
    bool quit_ = false;
    std::atomic<bool> cancel_{ false };
    std::string want_, station_, error_;
    uint64_t gen_ = 0;
    bool connecting_ = false;
    std::shared_ptr<RadioDecoder::Core> core_;
};

/** A rádió üzenetei a hídon. */
inline void attachRadio(BridgeCallbacks& cb, RadioController& ctl, StationStore& store) {
    cb.radioStatus = [&ctl] { return ctl.statusJson(); };
    cb.radioRequest = [&ctl, &store](const std::string& type, const json::Value& msg, bool remote) -> std::string {
        auto str = [&](const char* k) { const json::Value* v = msg.get(k); return v ? v->stringOr("") : std::string(); };
        auto fail = [](const std::string& e) {
            return "{\"type\":\"radio_result\",\"ok\":false,\"error\":\"" + media::jsonEscape(e) + "\"}";
        };
        const std::string ok = "{\"type\":\"radio_result\",\"ok\":true}";
        if (type == "radio_list") return store.listJson();
        if (type == "radio_play") {
            const std::string e = ctl.play(str("id"));
            return e.empty() ? ok : fail(e);
        }
        if (type == "radio_stop") { ctl.stop(); return ok; }
        if (remote) return fail("remote");   // a listát csak a saját gép / admin távirányító szerkesztheti
        if (type == "radio_add") {
            std::string id;
            const std::string e = store.add(str("name"), str("url"), id);
            return e.empty() ? store.listJson() : fail(e);
        }
        if (type == "radio_update") {
            const json::Value* n = msg.get("name");
            const json::Value* u = msg.get("url");
            if (!n && !u) return fail("empty");
            const std::string ns = n ? n->stringOr("") : std::string(), us = u ? u->stringOr("") : std::string();
            const std::string e = store.update(str("id"), n ? &ns : nullptr, u ? &us : nullptr);
            return e.empty() ? store.listJson() : fail(e);
        }
        if (type == "radio_remove") {
            const std::string e = store.remove(str("id"));
            return e.empty() ? store.listJson() : fail(e);
        }
        if (type == "radio_move") {
            const json::Value* f = msg.get("from");
            const json::Value* t = msg.get("to");
            const double fd = f ? f->numberOr(-1) : -1, td = t ? t->numberOr(-1) : -1;
            if (fd < 0 || td < 0 || fd > 1e6 || td > 1e6) return fail("index");
            const std::string e = store.move(static_cast<size_t>(fd), static_cast<size_t>(td));
            return e.empty() ? store.listJson() : fail(e);
        }
        return fail("unsupported");
    };
}

} // namespace ee_analyzer::radio
