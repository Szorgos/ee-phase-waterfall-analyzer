/**
 * @file playlists.cpp
 * @brief A 2.x lejátszási listái (lásd playlists.hpp).
 */
#include "playlists.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "mini_json.hpp"

namespace ee_analyzer::player {

namespace {
std::string q(const std::string& v) { return "\"" + media::jsonEscape(v) + "\""; }

int64_t now() { return static_cast<int64_t>(std::time(nullptr)); }

std::string itemJson(const QueueItem& it) {
    char d[32];
    std::snprintf(d, sizeof(d), "%.3f", it.duration);
    return "{\"id\":" + q(it.id) + ",\"title\":" + q(it.title) + ",\"artist\":" + q(it.artist) + ",\"album\":" +
           q(it.album) + ",\"duration\":" + d + "}";
}
} // namespace

PlaylistStore::PlaylistStore(Save save) : save_(std::move(save)) {}

std::string PlaylistStore::cleanName(const std::string& name) {
    size_t a = 0, b = name.size();
    while (a < b && static_cast<unsigned char>(name[a]) <= ' ') ++a;
    while (b > a && static_cast<unsigned char>(name[b - 1]) <= ' ') --b;
    std::string s = name.substr(a, b - a);
    for (char& c : s) if (static_cast<unsigned char>(c) < ' ') c = ' ';   // vezérlőkarakter nem lehet a névben
    if (s.size() > kMaxName) {
        size_t cut = kMaxName;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;   // UTF-8 karakterhatár
        s.resize(cut);
    }
    return s;
}

Playlist* PlaylistStore::findLocked(const std::string& id) {
    for (auto& l : lists_) if (l.id == id) return &l;
    return nullptr;
}

void PlaylistStore::changedLocked() {
    ++rev_;
    if (save_) save_(docLocked());
}

std::string PlaylistStore::doc() const {
    std::lock_guard<std::mutex> lk(m_);
    return docLocked();
}

std::string PlaylistStore::docLocked() const {
    std::string s = std::string("{\"version\":2,\"seeded\":") + (seeded_ ? "true" : "false") + ",\"lists\":[";
    for (size_t i = 0; i < lists_.size(); ++i) {
        const Playlist& l = lists_[i];
        s += (i ? ",{" : "{") + std::string("\"id\":") + q(l.id) + ",\"name\":" + q(l.name) + ",\"created\":" +
             std::to_string(l.created) + ",\"updated\":" + std::to_string(l.updated);
        if (l.smart) s += ",\"smart\":true,\"rules\":" + q(l.rules);   // a szabályok JSON-ja szövegként
        s += ",\"items\":[";
        for (size_t k = 0; k < l.items.size(); ++k) s += (k ? "," : "") + itemJson(l.items[k]);
        s += "]}";
    }
    return s + "]}";
}

bool PlaylistStore::load(const std::string& doc) {
    json::Value v;
    if (doc.empty() || !json::parse(doc, v)) return false;
    const json::Value* arr = v.get("lists");
    if (!arr || arr->type != json::Value::Type::Array) return false;
    std::vector<Playlist> lists;
    for (const auto& e : arr->arr) {
        if (lists.size() >= kMaxLists) break;
        auto str = [](const json::Value& o, const char* k) { const json::Value* x = o.get(k); return x ? x->stringOr("") : std::string(); };
        Playlist l;
        l.id = str(e, "id");
        l.name = cleanName(str(e, "name"));
        if (l.id.size() < 2 || l.id.size() > 32 || l.id[0] != 'p' || l.name.empty()) continue;
        if (std::any_of(lists.begin(), lists.end(), [&](const Playlist& x) { return x.id == l.id; })) continue;
        if (const json::Value* c = e.get("created")) l.created = static_cast<int64_t>(c->numberOr(0.0));
        if (const json::Value* u = e.get("updated")) l.updated = static_cast<int64_t>(u->numberOr(0.0));
        if (const json::Value* sm = e.get("smart"); sm && sm->type == json::Value::Type::Bool && sm->b) {
            l.smart = true;
            l.rules = str(e, "rules");
            if (l.rules.empty() || l.rules.size() > 16384) continue;
        }
        if (const json::Value* items = e.get("items"); !l.smart && items && items->type == json::Value::Type::Array)
            for (const auto& it : items->arr) {
                if (l.items.size() >= kMaxItems) break;
                QueueItem qi;
                qi.id = str(it, "id");
                if (qi.id.empty() || qi.id.size() > 64) continue;
                qi.title = str(it, "title");
                qi.artist = str(it, "artist");
                qi.album = str(it, "album");
                if (const json::Value* d = it.get("duration")) qi.duration = d->numberOr(0.0);
                l.items.push_back(std::move(qi));
            }
        lists.push_back(std::move(l));
    }
    const json::Value* sd = v.get("seeded");
    std::lock_guard<std::mutex> lk(m_);
    lists_ = std::move(lists);
    seeded_ = sd && sd->type == json::Value::Type::Bool && sd->b;
    ++rev_;
    return true;
}

std::string PlaylistStore::newIdLocked() {
    std::string id;
    do {
        char b[24];
        std::snprintf(b, sizeof(b), "p%016llx", static_cast<unsigned long long>(rng_()));
        id = b;
    } while (findLocked(id));
    return id;
}

std::string PlaylistStore::createSmart(const std::string& name, const std::string& rules, std::string& id) {
    const std::string n = cleanName(name);
    if (n.empty()) return "name";
    if (rules.empty()) return "rules";
    std::lock_guard<std::mutex> lk(m_);
    if (lists_.size() >= kMaxLists) return "full";
    id = newIdLocked();
    Playlist l;
    l.id = id;
    l.name = n;
    l.created = l.updated = now();
    l.smart = true;
    l.rules = rules;
    lists_.push_back(std::move(l));
    changedLocked();
    return "";
}

std::string PlaylistStore::setRules(const std::string& id, const std::string& rules) {
    if (rules.empty()) return "rules";
    std::lock_guard<std::mutex> lk(m_);
    Playlist* l = findLocked(id);
    if (!l) return "unknown";
    if (!l->smart) return "notSmart";
    l->rules = rules;
    l->updated = now();
    changedLocked();
    return "";
}

void PlaylistStore::seedOnce(const std::vector<std::pair<std::string, std::string>>& lists) {
    std::lock_guard<std::mutex> lk(m_);
    if (seeded_) return;
    for (const auto& [name, rules] : lists) {
        if (lists_.size() >= kMaxLists) break;
        Playlist l;
        l.id = newIdLocked();
        l.name = cleanName(name);
        l.created = l.updated = now();
        l.smart = true;
        l.rules = rules;
        lists_.push_back(std::move(l));
    }
    seeded_ = true;
    changedLocked();
}

std::string PlaylistStore::create(const std::string& name, std::vector<QueueItem> items, std::string& id) {
    const std::string n = cleanName(name);
    if (n.empty()) return "name";
    std::lock_guard<std::mutex> lk(m_);
    if (lists_.size() >= kMaxLists) return "full";
    id = newIdLocked();
    Playlist l;
    l.id = id;
    l.name = n;
    l.created = l.updated = now();
    if (items.size() > kMaxItems) items.resize(kMaxItems);
    l.items = std::move(items);
    lists_.push_back(std::move(l));
    changedLocked();
    return "";
}

std::string PlaylistStore::rename(const std::string& id, const std::string& name) {
    const std::string n = cleanName(name);
    if (n.empty()) return "name";
    std::lock_guard<std::mutex> lk(m_);
    Playlist* l = findLocked(id);
    if (!l) return "unknown";
    l->name = n;
    l->updated = now();
    changedLocked();
    return "";
}

std::string PlaylistStore::remove(const std::string& id) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = std::find_if(lists_.begin(), lists_.end(), [&](const Playlist& l) { return l.id == id; });
    if (it == lists_.end()) return "unknown";
    lists_.erase(it);
    changedLocked();
    return "";
}

std::string PlaylistStore::add(const std::string& id, std::vector<QueueItem> items, long position) {
    std::lock_guard<std::mutex> lk(m_);
    Playlist* l = findLocked(id);
    if (!l) return "unknown";
    if (l->smart) return "smart";
    if (items.empty()) return "empty";
    if (l->items.size() >= kMaxItems) return "full";
    if (l->items.size() + items.size() > kMaxItems) items.resize(kMaxItems - l->items.size());
    const size_t at = position < 0 || static_cast<size_t>(position) > l->items.size() ? l->items.size()
                                                                                      : static_cast<size_t>(position);
    l->items.insert(l->items.begin() + static_cast<std::ptrdiff_t>(at), std::make_move_iterator(items.begin()),
                    std::make_move_iterator(items.end()));
    l->updated = now();
    changedLocked();
    return "";
}

std::string PlaylistStore::removeItems(const std::string& id, std::vector<size_t> indices) {
    std::lock_guard<std::mutex> lk(m_);
    Playlist* l = findLocked(id);
    if (!l) return "unknown";
    if (l->smart) return "smart";
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    size_t removed = 0;
    for (auto it = indices.rbegin(); it != indices.rend(); ++it)   // hátulról, hogy az indexek ne csússzanak el
        if (*it < l->items.size()) {
            l->items.erase(l->items.begin() + static_cast<std::ptrdiff_t>(*it));
            ++removed;
        }
    if (!removed) return "index";
    l->updated = now();
    changedLocked();
    return "";
}

std::string PlaylistStore::move(const std::string& id, size_t from, size_t to) {
    std::lock_guard<std::mutex> lk(m_);
    Playlist* l = findLocked(id);
    if (!l) return "unknown";
    if (l->smart) return "smart";
    if (from >= l->items.size() || to >= l->items.size()) return "index";
    if (from != to) {
        QueueItem e = std::move(l->items[from]);
        l->items.erase(l->items.begin() + static_cast<std::ptrdiff_t>(from));
        l->items.insert(l->items.begin() + static_cast<std::ptrdiff_t>(to), std::move(e));
        l->updated = now();
        changedLocked();
    }
    return "";
}

bool PlaylistStore::get(const std::string& id, Playlist& out) const {
    std::lock_guard<std::mutex> lk(m_);
    for (const auto& l : lists_)
        if (l.id == id) { out = l; return true; }
    return false;
}

std::vector<Playlist> PlaylistStore::all() const {
    std::lock_guard<std::mutex> lk(m_);
    return lists_;
}

size_t PlaylistStore::count() const {
    std::lock_guard<std::mutex> lk(m_);
    return lists_.size();
}

uint64_t PlaylistStore::rev() const {
    std::lock_guard<std::mutex> lk(m_);
    return rev_;
}

} // namespace ee_analyzer::player
