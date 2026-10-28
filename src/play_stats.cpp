/**
 * @file play_stats.cpp
 * @brief A 2.x lejátszási naplója (lásd play_stats.hpp).
 */
#include "play_stats.hpp"

#include <algorithm>
#include <ctime>

#include "media_session.hpp"
#include "mini_json.hpp"

namespace ee_analyzer::player {

PlayStats::PlayStats(Save save) : save_(std::move(save)) {}

bool PlayStats::load(const std::string& doc) {
    json::Value v;
    if (doc.empty() || !json::parse(doc, v)) return false;
    const json::Value* t = v.get("tracks");
    if (!t || t->type != json::Value::Type::Object) return false;
    std::unordered_map<std::string, TrackStats> m;
    for (const auto& [id, e] : t->obj) {
        if (id.empty() || id.size() > 64 || e.type != json::Value::Type::Object) continue;
        TrackStats s;
        if (const json::Value* p = e.get("plays")) s.plays = static_cast<uint32_t>(std::clamp(p->numberOr(0.0), 0.0, 4e9));
        if (const json::Value* l = e.get("last")) s.last = static_cast<int64_t>(std::max(0.0, l->numberOr(0.0)));
        if (s.plays || s.last) m[id] = s;
    }
    std::lock_guard<std::mutex> lk(m_);
    stats_ = std::move(m);
    ++rev_;
    return true;
}

std::string PlayStats::docLocked() const {
    std::string s = "{\"version\":1,\"tracks\":{";
    bool first = true;
    for (const auto& [id, st] : stats_) {
        s += (first ? "\"" : ",\"") + media::jsonEscape(id) + "\":{\"plays\":" + std::to_string(st.plays) +
             ",\"last\":" + std::to_string(st.last) + "}";
        first = false;
    }
    return s + "}}";
}

std::string PlayStats::doc() const {
    std::lock_guard<std::mutex> lk(m_);
    return docLocked();
}

void PlayStats::record(const std::string& id, int64_t when) {
    if (id.empty() || id.size() > 64) return;
    std::lock_guard<std::mutex> lk(m_);
    TrackStats& s = stats_[id];
    ++s.plays;
    s.last = when ? when : static_cast<int64_t>(std::time(nullptr));
    ++rev_;
    if (save_) save_(docLocked());
}

TrackStats PlayStats::get(const std::string& id) const {
    std::lock_guard<std::mutex> lk(m_);
    auto it = stats_.find(id);
    return it == stats_.end() ? TrackStats{} : it->second;
}

std::unordered_map<std::string, TrackStats> PlayStats::snapshot() const {
    std::lock_guard<std::mutex> lk(m_);
    return stats_;
}

uint64_t PlayStats::rev() const {
    std::lock_guard<std::mutex> lk(m_);
    return rev_;
}

bool ListenTracker::update(uint64_t token, double position, double duration, bool playing, double dt) {
    if (token != token_) {                  // új tétel (vagy a lejátszás véget ért): új mérés
        token_ = token;
        lastPos_ = position;
        heard_ = 0.0;
        counted_ = false;
        return false;
    }
    if (token == 0 || !playing) {
        lastPos_ = position;
        return false;
    }
    const double d = position - lastPos_;
    lastPos_ = position;
    if (d < -2.0) {                         // újrakezdés: az „előző”, egy szám ismétlése, visszatekerés
        heard_ = std::max(0.0, position);
        counted_ = false;
    } else if (d > 0.0 && d <= dt * 2.0 + 0.25) {
        heard_ += d;                        // a valós idejű haladás számít, az előre tekerés nem
    }
    const double need = duration > 0.0 ? std::min(kCountSeconds, duration / 2.0) : kCountSeconds;
    if (!counted_ && heard_ >= need) {
        counted_ = true;
        return true;
    }
    return false;
}

} // namespace ee_analyzer::player
