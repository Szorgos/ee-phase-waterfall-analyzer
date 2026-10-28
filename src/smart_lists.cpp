/**
 * @file smart_lists.cpp
 * @brief Okos listák (lásd smart_lists.hpp).
 */
#include "smart_lists.hpp"

#include <algorithm>
#include <cstdio>
#include <random>

#include "media_session.hpp"

namespace ee_analyzer::library {

namespace {
constexpr size_t kMaxConditions = 20;
constexpr size_t kMaxText = 200;

enum class Kind { Text, Number, Date };

bool fieldKind(const std::string& f, Kind& k) {
    if (f == "artist" || f == "album" || f == "title" || f == "genre") k = Kind::Text;
    else if (f == "year" || f == "duration" || f == "plays") k = Kind::Number;
    else if (f == "added" || f == "lastPlayed") k = Kind::Date;
    else return false;
    return true;
}

bool opValid(Kind k, const std::string& field, const std::string& op) {
    switch (k) {
        case Kind::Text: return op == "contains" || op == "notContains" || op == "is" || op == "isNot" || op == "startsWith";
        case Kind::Number: return op == "eq" || op == "ne" || op == "gt" || op == "ge" || op == "lt" || op == "le";
        case Kind::Date: return op == "inLast" || op == "notInLast" || (op == "never" && field == "lastPlayed");
    }
    return false;
}

bool sortValid(const std::string& s) {
    static const char* const kSorts[] = { "added", "lastPlayed", "plays", "year", "artist", "album", "title", "duration", "random" };
    return std::find_if(std::begin(kSorts), std::end(kSorts), [&](const char* x) { return s == x; }) != std::end(kSorts);
}

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.10g", v);
    return b;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}
} // namespace

std::string foldCase(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 'A' && c <= 'Z') { out += static_cast<char>(c + 32); continue; }
        if (c >= 0xC2 && c <= 0xC5 && i + 1 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) {
            uint32_t cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) cp += 0x20;                       // Latin-1: À–Þ
            else if ((cp >= 0x100 && cp <= 0x137) || (cp >= 0x14A && cp <= 0x177)) cp |= 1u;   // páros: nagybetű (Ő, Ű…)
            else if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E)) { if (cp & 1u) ++cp; }
            else if (cp == 0x178) cp = 0xFF;
            appendUtf8(out, cp);
            ++i;
            continue;
        }
        out += static_cast<char>(c);
    }
    return out;
}

bool parseSmartRules(const json::Value& v, SmartRules& out, std::string& error) {
    SmartRules r;
    if (v.type != json::Value::Type::Object) { error = "rules"; return false; }
    if (const json::Value* m = v.get("match")) {
        const std::string s = m->stringOr("");
        if (s == "any") r.any = true;
        else if (s != "all") { error = "match"; return false; }
    }
    if (const json::Value* c = v.get("conditions")) {
        if (c->type != json::Value::Type::Array || c->arr.size() > kMaxConditions) { error = "conditions"; return false; }
        for (const auto& e : c->arr) {
            SmartCondition sc;
            const json::Value* f = e.get("field");
            const json::Value* o = e.get("op");
            sc.field = f ? f->stringOr("") : "";
            sc.op = o ? o->stringOr("") : "";
            Kind k;
            if (!fieldKind(sc.field, k) || !opValid(k, sc.field, sc.op)) { error = "condition"; return false; }
            const json::Value* val = e.get("value");
            if (k == Kind::Text) {
                if (!val || val->type != json::Value::Type::String || val->str.size() > kMaxText) { error = "value"; return false; }
                sc.text = val->str;
            } else if (sc.op != "never") {
                if (!val || (val->type != json::Value::Type::Number && val->type != json::Value::Type::String)) { error = "value"; return false; }
                sc.number = val->numberOr(0.0);
                if (k == Kind::Date && (sc.number < 0 || sc.number > 100000)) { error = "value"; return false; }
            }
            r.conditions.push_back(std::move(sc));
        }
    }
    if (const json::Value* s = v.get("sort")) {
        r.sort = s->stringOr("");
        if (!sortValid(r.sort)) { error = "sort"; return false; }
    }
    // alapból: időnél, számnál csökkenő (legújabb, legtöbb elöl), szövegnél növekvő
    r.desc = !(r.sort == "artist" || r.sort == "album" || r.sort == "title");
    if (const json::Value* d = v.get("desc")) {
        if (d->type != json::Value::Type::Bool) { error = "desc"; return false; }
        r.desc = d->b;
    }
    if (const json::Value* l = v.get("limit")) {
        const double n = l->numberOr(-1.0);
        if (n < 0 || n > static_cast<double>(player::PlayQueue::kMaxItems)) { error = "limit"; return false; }
        r.limit = static_cast<size_t>(n);
    }
    out = std::move(r);
    return true;
}

std::string smartRulesJson(const SmartRules& r) {
    std::string s = std::string("{\"match\":\"") + (r.any ? "any" : "all") + "\",\"conditions\":[";
    for (size_t i = 0; i < r.conditions.size(); ++i) {
        const auto& c = r.conditions[i];
        Kind k = Kind::Text;
        fieldKind(c.field, k);
        s += (i ? ",{" : "{") + std::string("\"field\":\"") + c.field + "\",\"op\":\"" + c.op + "\"";
        if (k == Kind::Text) s += ",\"value\":\"" + media::jsonEscape(c.text) + "\"";
        else if (c.op != "never") s += ",\"value\":" + num(c.number);
        s += "}";
    }
    return s + "],\"sort\":\"" + r.sort + "\",\"desc\":" + (r.desc ? "true" : "false") + ",\"limit\":" +
           std::to_string(r.limit) + "}";
}

std::vector<player::QueueItem> evaluateSmart(const SmartRules& r, const Index& idx,
                                             const std::unordered_map<std::string, player::TrackStats>& stats, int64_t now) {
    // a szöveges feltételek kisbetűs alakja egyszer
    std::vector<std::string> folded;
    for (const auto& c : r.conditions) folded.push_back(foldCase(c.text));
    const player::TrackStats none{};
    auto statsOf = [&](const Track& t) -> const player::TrackStats& {
        auto it = stats.find(t.id);
        return it == stats.end() ? none : it->second;
    };
    auto test = [&](const Track& t, size_t ci) {
        const SmartCondition& c = r.conditions[ci];
        Kind k = Kind::Text;
        fieldKind(c.field, k);
        if (k == Kind::Text) {
            const std::string& raw = c.field == "artist" ? t.artist : c.field == "album" ? t.album
                                   : c.field == "title" ? t.title : t.genre;
            const std::string v = foldCase(raw), &x = folded[ci];
            if (c.op == "contains") return v.find(x) != std::string::npos;
            if (c.op == "notContains") return v.find(x) == std::string::npos;
            if (c.op == "is") return v == x;
            if (c.op == "isNot") return v != x;
            return v.rfind(x, 0) == 0;   // startsWith
        }
        if (k == Kind::Number) {
            const double v = c.field == "year" ? t.year : c.field == "duration" ? t.duration
                           : static_cast<double>(statsOf(t).plays);
            if (c.op == "eq") return v == c.number;
            if (c.op == "ne") return v != c.number;
            if (c.op == "gt") return v > c.number;
            if (c.op == "ge") return v >= c.number;
            if (c.op == "lt") return v < c.number;
            return v <= c.number;
        }
        const int64_t since = now - static_cast<int64_t>(c.number * 86400.0);
        if (c.field == "added") {
            if (c.op == "inLast") return t.added > 0 && t.added >= since;
            return t.added > 0 && t.added < since;   // notInLast
        }
        const player::TrackStats& st = statsOf(t);
        if (c.op == "never") return st.plays == 0;
        if (c.op == "inLast") return st.last > 0 && st.last >= since;
        return st.last == 0 || st.last < since;      // notInLast: (ilyen régen vagy) még soha
    };
    auto matches = [&](const Track& t) {
        if (r.conditions.empty()) return true;
        for (size_t i = 0; i < r.conditions.size(); ++i) {
            const bool ok = test(t, i);
            if (r.any && ok) return true;
            if (!r.any && !ok) return false;
        }
        return !r.any;
    };

    // album sorrend (előadó, év, cím; lemez, sorszám) az egyenlő kulcsú számokhoz
    std::vector<size_t> order;
    order.reserve(idx.tracks.size());
    std::vector<char> seen(idx.tracks.size(), 0);
    for (const auto& a : idx.albums)
        for (size_t t : a.tracks)
            if (t < idx.tracks.size() && !seen[t]) { seen[t] = 1; order.push_back(t); }
    for (size_t t = 0; t < idx.tracks.size(); ++t) if (!seen[t]) order.push_back(t);

    std::vector<size_t> hit;
    for (size_t t : order) if (matches(idx.tracks[t])) hit.push_back(t);

    if (r.sort == "random") {
        std::mt19937 rng{ std::random_device{}() };
        std::shuffle(hit.begin(), hit.end(), rng);
    } else {
        auto numKey = [&](size_t t) -> double {
            const Track& x = idx.tracks[t];
            if (r.sort == "added") return static_cast<double>(x.added);
            if (r.sort == "lastPlayed") return static_cast<double>(statsOf(x).last);
            if (r.sort == "plays") return statsOf(x).plays;
            if (r.sort == "year") return x.year;
            return x.duration;
        };
        const bool text = r.sort == "artist" || r.sort == "album" || r.sort == "title";
        auto textKey = [&](size_t t) {
            const Track& x = idx.tracks[t];
            return foldCase(r.sort == "artist" ? x.artist : r.sort == "album" ? x.album : x.title);
        };
        std::stable_sort(hit.begin(), hit.end(), [&](size_t a, size_t b) {
            if (text) {
                const std::string ka = textKey(a), kb = textKey(b);
                return r.desc ? kb < ka : ka < kb;
            }
            const double ka = numKey(a), kb = numKey(b);
            return r.desc ? kb < ka : ka < kb;
        });
    }
    const size_t cap = r.limit ? std::min(r.limit, player::PlayQueue::kMaxItems) : player::PlayQueue::kMaxItems;
    if (hit.size() > cap) hit.resize(cap);
    std::vector<player::QueueItem> items;
    items.reserve(hit.size());
    for (size_t t : hit) items.push_back(queueItem(idx.tracks[t]));
    return items;
}

std::vector<std::pair<std::string, std::string>> defaultSmartLists(bool hu) {
    auto rules = [](const char* field, const char* op, double value, const char* sort, size_t limit) {
        SmartRules r;
        SmartCondition c;
        c.field = field;
        c.op = op;
        c.number = value;
        r.conditions.push_back(c);
        r.sort = sort;
        r.desc = true;
        r.limit = limit;
        return smartRulesJson(r);
    };
    return {
        { hu ? "Legutóbb hozzáadott" : "Recently added", rules("added", "inLast", 30, "added", 100) },
        { hu ? "Legtöbbet játszott" : "Most played", rules("plays", "gt", 0, "plays", 100) },
        { hu ? "Nemrég játszott" : "Recently played", rules("lastPlayed", "inLast", 14, "lastPlayed", 50) },
        { hu ? "Soha nem játszott" : "Never played", rules("lastPlayed", "never", 0, "added", 100) },
    };
}

} // namespace ee_analyzer::library
