/**
 * @file play_queue.cpp
 * @brief A 2.x lejátszási sora (lásd play_queue.hpp).
 */
#include "play_queue.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <unordered_set>

#include "mini_json.hpp"

namespace ee_analyzer::player {

namespace {
constexpr int kMaxSkip = 16;   // egymás után legfeljebb ennyi nem nyitható tételt ugrik át

std::string q(const std::string& v) { return "\"" + media::jsonEscape(v) + "\""; }

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.3f", v);
    return b;
}

std::string itemJson(const QueueItem& it) {
    return "{\"id\":" + q(it.id) + ",\"title\":" + q(it.title) + ",\"artist\":" + q(it.artist) + ",\"album\":" +
           q(it.album) + ",\"duration\":" + num(it.duration) + "}";
}
} // namespace

const char* PlayQueue::repeatName(Repeat r) {
    return r == Repeat::All ? "all" : r == Repeat::One ? "one" : "off";
}

bool PlayQueue::parseRepeat(const std::string& s, Repeat& out) {
    if (s == "off") out = Repeat::Off;
    else if (s == "all") out = Repeat::All;
    else if (s == "one") out = Repeat::One;
    else return false;
    return true;
}

PlayQueue::PlayQueue(FilePlayer& player, Resolve resolve, Save save)
    : player_(player), resolve_(std::move(resolve)), save_(std::move(save)) {
    // A lejátszó a szám végén ezt kérdezi: a következő tétel útvonala és jele (a lejátszó zárja alatt fut)
    player_.setNextProvider([this](uint64_t after, std::string& path, uint64_t& token) {
        std::string id;
        {
            std::lock_guard<std::mutex> lk(m_);
            const int i = indexOfLocked(after);
            if (i < 0) return false;
            const size_t n = items_.size();
            size_t j;
            if (repeat_ == Repeat::One) j = static_cast<size_t>(i);
            else if (static_cast<size_t>(i) + 1 < n) j = static_cast<size_t>(i) + 1;
            else if (repeat_ == Repeat::All && n > 0) j = 0;
            else return false;
            id = items_[j].item.id;
            token = items_[j].uid;
        }
        path.clear();
        if (!resolve_ || !resolve_(id, path)) path.clear();   // ismeretlen tétel: üres útvonal, a lejátszó átugorja
        return true;
    });
    tick_ = std::thread([this] { trackLoop(); });
}

PlayQueue::~PlayQueue() {
    {
        std::lock_guard<std::mutex> lk(tickM_);
        quit_ = true;
    }
    tickCv_.notify_all();
    if (tick_.joinable()) tick_.join();
    player_.setNextProvider(nullptr);
}

void PlayQueue::setPlayedCallback(std::function<void(const std::string& id)> played) {
    std::lock_guard<std::mutex> lk(tickM_);
    played_ = std::move(played);
}

void PlayQueue::trackLoop() {
    auto last = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lk(tickM_);
    while (!quit_) {
        tickCv_.wait_for(lk, std::chrono::milliseconds(250));
        if (quit_) break;
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        auto cb = played_;
        lk.unlock();
        // a lejátszó és a sor zárja a mérőszál saját zárja nélkül (a zárak sorrendje: lejátszó, utána a sor)
        const uint64_t token = player_.currentToken();
        const media::Info info = player_.info();
        if (tracker_.update(token, info.position, info.duration, info.playing, dt) && cb) {
            std::string id;
            {
                std::lock_guard<std::mutex> q(m_);
                const int i = indexOfLocked(token);
                if (i >= 0) id = items_[static_cast<size_t>(i)].item.id;
            }
            if (!id.empty()) cb(id);
        }
        lk.lock();
    }
}

int PlayQueue::indexOfLocked(uint64_t uid) const {
    if (uid == 0) return -1;
    auto it = pos_.find(uid);
    return it == pos_.end() ? -1 : static_cast<int>(it->second);
}

int PlayQueue::currentIndexLocked(uint64_t token, bool hasTrack) const {
    return indexOfLocked(hasTrack ? token : currentUid_);   // lejátszás nélkül: a legutóbbi (mentett) tétel
}

int PlayQueue::currentIndex() const {
    const uint64_t token = player_.currentToken();
    const bool hasTrack = player_.hasTrack();
    std::lock_guard<std::mutex> lk(m_);
    return currentIndexLocked(token, hasTrack);
}

size_t PlayQueue::size() const {
    std::lock_guard<std::mutex> lk(m_);
    return items_.size();
}

std::vector<QueueItem> PlayQueue::items() const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<QueueItem> v;
    v.reserve(items_.size());
    for (const auto& e : items_) v.push_back(e.item);
    return v;
}

PlayQueue::Repeat PlayQueue::repeat() const {
    std::lock_guard<std::mutex> lk(m_);
    return repeat_;
}

bool PlayQueue::shuffle() const {
    std::lock_guard<std::mutex> lk(m_);
    return shuffle_;
}

void PlayQueue::reindexLocked() {
    pos_.clear();
    for (size_t i = 0; i < items_.size(); ++i) pos_[items_[i].uid] = i;
}

void PlayQueue::changedLocked() {
    ++rev_;
    if (save_) {
        save_(docLocked(indexOfLocked(currentUid_)));
        savedUid_ = currentUid_;
    }
}

std::vector<PlayQueue::Entry> PlayQueue::makeEntriesLocked(std::vector<QueueItem> items) {
    std::vector<Entry> v;
    v.reserve(items.size());
    for (auto& it : items) v.push_back(Entry{ std::move(it), nextUid_++ });
    return v;
}

void PlayQueue::shuffleAfterLocked(int keep) {
    const size_t from = keep < 0 ? 0 : static_cast<size_t>(keep) + 1;
    if (from < items_.size()) std::shuffle(items_.begin() + static_cast<std::ptrdiff_t>(from), items_.end(), rng_);
}

void PlayQueue::refreshPlayer() {
    if (player_.hasTrack()) player_.dropPrefetched();
}

bool PlayQueue::openIndex(size_t index, int dir, std::string* error) {
    std::string lastError = "unknown";
    for (int tries = 0; tries < kMaxSkip; ++tries) {
        std::string id;
        uint64_t uid;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (index >= items_.size()) break;
            id = items_[index].item.id;
            uid = items_[index].uid;
        }
        std::string path, e;
        if (resolve_ && resolve_(id, path) && player_.open(path, &e, uid)) {
            std::lock_guard<std::mutex> lk(m_);
            currentUid_ = uid;
            if (save_) { save_(docLocked(indexOfLocked(uid))); savedUid_ = uid; }
            return true;
        }
        if (!e.empty()) lastError = e;
        if (dir < 0 && index == 0) break;
        index = dir < 0 ? index - 1 : index + 1;
    }
    if (error) *error = lastError;
    return false;
}

bool PlayQueue::play(std::vector<QueueItem> items, size_t start, std::string* error) {
    if (items.empty()) {
        if (error) *error = "empty";
        return false;
    }
    if (items.size() > kMaxItems) items.resize(kMaxItems);
    {
        std::lock_guard<std::mutex> lk(m_);
        items_ = makeEntriesLocked(std::move(items));
        start = std::min(start, items_.size() - 1);
        original_.clear();
        if (shuffle_) {
            // keverésnél a választott szám szól először, utána a többi véletlen sorrendben
            for (const auto& e : items_) original_.push_back(e.uid);
            std::rotate(items_.begin(), items_.begin() + static_cast<std::ptrdiff_t>(start),
                        items_.begin() + static_cast<std::ptrdiff_t>(start) + 1);
            shuffleAfterLocked(0);
            start = 0;
        }
        reindexLocked();
        currentUid_ = items_[start].uid;
        changedLocked();
    }
    return openIndex(start, +1, error);
}

bool PlayQueue::load(std::vector<QueueItem> items, size_t start, std::string* error) {
    if (items.empty()) {
        if (error) *error = "empty";
        return false;
    }
    if (items.size() > kMaxItems) items.resize(kMaxItems);
    {
        std::lock_guard<std::mutex> lk(m_);
        items_ = makeEntriesLocked(std::move(items));
        start = std::min(start, items_.size() - 1);
        original_.clear();
        if (shuffle_) {
            for (const auto& e : items_) original_.push_back(e.uid);
            std::rotate(items_.begin(), items_.begin() + static_cast<std::ptrdiff_t>(start),
                        items_.begin() + static_cast<std::ptrdiff_t>(start) + 1);
            shuffleAfterLocked(0);
            start = 0;
        }
        reindexLocked();
        currentUid_ = items_[start].uid;
        changedLocked();
    }
    return true;
}

bool PlayQueue::jump(size_t index, std::string* error) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (index >= items_.size()) {
            if (error) *error = "index";
            return false;
        }
    }
    return openIndex(index, +1, error);
}

bool PlayQueue::next() {
    const int i = currentIndex();
    if (i < 0) return false;
    size_t j;
    {
        std::lock_guard<std::mutex> lk(m_);
        const size_t n = items_.size();
        if (static_cast<size_t>(i) + 1 < n) j = static_cast<size_t>(i) + 1;
        else if (repeat_ == Repeat::All && n > 0) j = 0;
        else return false;
    }
    return openIndex(j, +1, nullptr);
}

bool PlayQueue::previous() {
    const int i = currentIndex();
    if (i < 0 || !player_.hasTrack()) return false;
    if (i == 0 || player_.position() > kRestartSeconds) {
        player_.seek(0.0);
        return true;
    }
    return openIndex(static_cast<size_t>(i) - 1, -1, nullptr);
}

void PlayQueue::command(const std::string& action, double value) {
    if (action == "next") next();
    else if (action == "previous") previous();
    else player_.command(action, value);
}

bool PlayQueue::add(std::vector<QueueItem> items, bool next) {
    const uint64_t token = player_.currentToken();
    const bool hasTrack = player_.hasTrack();
    {
        std::lock_guard<std::mutex> lk(m_);
        if (items_.size() >= kMaxItems || items.empty()) return false;
        if (items_.size() + items.size() > kMaxItems) items.resize(kMaxItems - items_.size());
        const int cur = currentIndexLocked(token, hasTrack);
        if (cur >= 0) currentUid_ = items_[static_cast<size_t>(cur)].uid;
        std::vector<Entry> add = makeEntriesLocked(std::move(items));
        const size_t at = next && cur >= 0 ? static_cast<size_t>(cur) + 1 : items_.size();
        if (shuffle_) {
            // a keverés előtti sorrendben is ugyanígy: a hallott tétel után, illetve a végére
            auto o = original_.end();
            if (next && cur >= 0) {
                o = std::find(original_.begin(), original_.end(), currentUid_);
                if (o != original_.end()) ++o;
            }
            std::vector<uint64_t> uids;
            for (const auto& e : add) uids.push_back(e.uid);
            original_.insert(o, uids.begin(), uids.end());
        }
        items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(at), std::make_move_iterator(add.begin()),
                      std::make_move_iterator(add.end()));
        reindexLocked();
        changedLocked();
    }
    refreshPlayer();
    return true;
}

bool PlayQueue::remove(size_t index) {
    const uint64_t token = player_.currentToken();
    const bool hasTrack = player_.hasTrack();
    bool playingRemoved = false;
    size_t remaining = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (index >= items_.size()) return false;
        const int cur = currentIndexLocked(token, hasTrack);
        const uint64_t uid = items_[index].uid;
        playingRemoved = hasTrack && cur == static_cast<int>(index);
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(index));
        original_.erase(std::remove(original_.begin(), original_.end(), uid), original_.end());
        reindexLocked();
        remaining = items_.size();
        if (currentUid_ == uid) currentUid_ = index < items_.size() ? items_[index].uid : 0;
        changedLocked();
    }
    if (playingRemoved) {
        // a hallott tétel törlése: a helyére lépő szól; ha nincs ilyen, a lejátszás leáll
        if (index < remaining) openIndex(index, +1, nullptr);
        else player_.stop();
    } else {
        refreshPlayer();
    }
    return true;
}

bool PlayQueue::move(size_t from, size_t to) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (from >= items_.size() || to >= items_.size()) return false;
        if (from == to) return true;
        Entry e = std::move(items_[from]);
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(from));
        items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(to), std::move(e));
        reindexLocked();
        changedLocked();
    }
    refreshPlayer();
    return true;
}

void PlayQueue::clear() {
    const uint64_t token = player_.currentToken();
    const bool hasTrack = player_.hasTrack();
    bool fromQueue;
    {
        std::lock_guard<std::mutex> lk(m_);
        fromQueue = hasTrack && indexOfLocked(token) >= 0;
        items_.clear();
        pos_.clear();
        original_.clear();
        currentUid_ = 0;
        changedLocked();
    }
    if (fromQueue) player_.stop();
}

void PlayQueue::setRepeat(Repeat mode) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (repeat_ == mode) return;
        repeat_ = mode;
        changedLocked();
    }
    refreshPlayer();   // a következő szám ettől függ (pl. egy szám ismétlése)
}

void PlayQueue::setShuffle(bool on) {
    const uint64_t token = player_.currentToken();
    const bool hasTrack = player_.hasTrack();
    {
        std::lock_guard<std::mutex> lk(m_);
        if (shuffle_ == on) return;
        const int cur = currentIndexLocked(token, hasTrack);
        if (cur >= 0) currentUid_ = items_[static_cast<size_t>(cur)].uid;
        if (on) {
            original_.clear();
            for (const auto& e : items_) original_.push_back(e.uid);
            // a hallott tétel a helyén marad, az utána következők keverednek (lejátszás nélkül az egész sor)
            shuffleAfterLocked(cur);
        } else {
            // az eredeti sorrend; a keverés közben hozzáadott, de az eredetiben nem szereplő tételek a végére
            std::vector<Entry> ordered;
            ordered.reserve(items_.size());
            std::unordered_set<uint64_t> used;
            for (uint64_t uid : original_) {
                const int i = indexOfLocked(uid);
                if (i >= 0 && used.insert(uid).second) ordered.push_back(std::move(items_[static_cast<size_t>(i)]));
            }
            for (auto& e : items_)
                if (e.uid != 0 && !used.count(e.uid)) ordered.push_back(std::move(e));
            items_ = std::move(ordered);
            original_.clear();
        }
        shuffle_ = on;
        reindexLocked();
        changedLocked();
    }
    refreshPlayer();
}

media::Info PlayQueue::mediaInfo() const {
    media::Info m = player_.info();
    const uint64_t token = player_.currentToken();
    std::lock_guard<std::mutex> lk(m_);
    const int i = indexOfLocked(token);
    const size_t n = items_.size();
    m.canNext = m.available && i >= 0 && (static_cast<size_t>(i) + 1 < n || repeat_ == Repeat::All);
    m.canPrevious = m.available && i >= 0;
    return m;
}

std::string PlayQueue::json() const {
    const uint64_t token = player_.currentToken();
    const bool hasTrack = player_.hasTrack();
    std::lock_guard<std::mutex> lk(m_);
    std::string s = "{\"type\":\"queue\",\"rev\":" + std::to_string(rev_) + ",\"index\":" +
                    std::to_string(currentIndexLocked(token, hasTrack)) + ",\"repeat\":\"" + repeatName(repeat_) +
                    "\",\"shuffle\":" + (shuffle_ ? "true" : "false") + ",\"items\":[";
    for (size_t i = 0; i < items_.size(); ++i) s += (i ? "," : "") + itemJson(items_[i].item);
    return s + "]}";
}

std::string PlayQueue::statusJson() {
    const uint64_t token = player_.currentToken();
    const bool hasTrack = player_.hasTrack();
    std::lock_guard<std::mutex> lk(m_);
    if (items_.empty()) return "";
    const int i = currentIndexLocked(token, hasTrack);
    if (i >= 0) currentUid_ = items_[static_cast<size_t>(i)].uid;   // a szünetmentes továbblépés is
    if (save_ && i >= 0 && currentUid_ != savedUid_) {
        save_(docLocked(i));
        savedUid_ = currentUid_;
    }
    return "{\"rev\":" + std::to_string(rev_) + ",\"index\":" + std::to_string(i) + ",\"count\":" +
           std::to_string(items_.size()) + ",\"repeat\":\"" + repeatName(repeat_) + "\",\"shuffle\":" +
           (shuffle_ ? "true" : "false") + "}";
}

std::string PlayQueue::docLocked(int index) const {
    std::string s = "{\"version\":2,\"index\":" + std::to_string(index) + ",\"repeat\":\"" + repeatName(repeat_) +
                    "\",\"shuffle\":" + (shuffle_ ? "true" : "false") + ",\"items\":[";
    for (size_t i = 0; i < items_.size(); ++i) s += (i ? "," : "") + itemJson(items_[i].item);
    s += "]";
    if (shuffle_) {
        // a keverés előtti sorrend a tételek indexeivel
        s += ",\"original\":[";
        bool first = true;
        for (uint64_t uid : original_) {
            const int i = indexOfLocked(uid);
            if (i < 0) continue;
            s += (first ? "" : ",") + std::to_string(i);
            first = false;
        }
        s += "]";
    }
    return s + "}";
}

std::string PlayQueue::doc() const {
    const uint64_t token = player_.currentToken();
    const bool hasTrack = player_.hasTrack();
    std::lock_guard<std::mutex> lk(m_);
    return docLocked(currentIndexLocked(token, hasTrack));
}

bool PlayQueue::restore(const std::string& doc) {
    json::Value v;
    if (doc.empty() || !json::parse(doc, v)) return false;
    const json::Value* arr = v.get("items");
    if (!arr || arr->type != json::Value::Type::Array) return false;
    std::vector<QueueItem> items;
    for (const auto& e : arr->arr) {
        if (items.size() >= kMaxItems) break;
        auto str = [&](const char* k) { const json::Value* x = e.get(k); return x ? x->stringOr("") : std::string(); };
        QueueItem it;
        it.id = str("id");
        if (it.id.empty() || it.id.size() > 64) continue;
        it.title = str("title");
        it.artist = str("artist");
        it.album = str("album");
        if (const json::Value* d = e.get("duration")) it.duration = d->numberOr(0.0);
        items.push_back(std::move(it));
    }
    if (items.empty()) return false;
    const json::Value* iv = v.get("index");
    const double idx = iv ? iv->numberOr(0.0) : 0.0;
    Repeat rep = Repeat::Off;
    if (const json::Value* r = v.get("repeat")) parseRepeat(r->stringOr("off"), rep);
    const json::Value* sv = v.get("shuffle");
    const bool shuf = sv && sv->type == json::Value::Type::Bool && sv->b;
    std::lock_guard<std::mutex> lk(m_);
    items_ = makeEntriesLocked(std::move(items));   // új jelek: a lejátszó semmilyen régi jele nem tartozik ide
    reindexLocked();
    repeat_ = rep;
    shuffle_ = shuf;
    original_.clear();
    if (shuffle_) {
        std::unordered_set<size_t> seen;
        if (const json::Value* o = v.get("original"); o && o->type == json::Value::Type::Array)
            for (const auto& e : o->arr) {
                const double d = e.numberOr(-1.0);
                if (d >= 0 && d < static_cast<double>(items_.size()) && seen.insert(static_cast<size_t>(d)).second)
                    original_.push_back(items_[static_cast<size_t>(d)].uid);
            }
        if (original_.empty())
            for (const auto& e : items_) original_.push_back(e.uid);
    }
    const size_t i = idx >= 0 && idx < static_cast<double>(items_.size()) ? static_cast<size_t>(idx) : 0;
    currentUid_ = savedUid_ = items_[i].uid;
    ++rev_;
    return true;
}

} // namespace ee_analyzer::player
