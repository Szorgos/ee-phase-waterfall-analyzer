/**
 * @file file_player.cpp
 * @brief A beépített zenelejátszó magja (platformfüggetlen); a hangkimenet a PlayerOutput.
 */
#include "file_player.hpp"

#include <algorithm>

#include "music_library.hpp"
#include <chrono>
#include <cstring>

namespace ee_analyzer::player {

namespace {
constexpr size_t kChunkFrames = 4096;   // egyszerre ennyi keretet dekódol a szál
constexpr int kMaxSkip = 16;            // a sorban egymás után legfeljebb ennyi nem nyitható számot ugrik át

std::string fileStem(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string s = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && dot > 0) s.resize(dot);
    return s;
}
} // namespace

FilePlayer::FilePlayer(std::unique_ptr<PlayerOutput> out) : out_(std::move(out)) {
    thread_ = std::thread([this] { decodeLoop(); });
}

FilePlayer::~FilePlayer() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stopLocked();
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

std::unique_ptr<FilePlayer::Segment> FilePlayer::makeSegment(const std::string& path, std::string* error, uint64_t token) {
    auto dec = openDecoder(path, error);
    if (!dec) return nullptr;
    auto s = std::make_unique<Segment>();
    s->path = path;
    s->channels = dec->channels();
    s->total = dec->totalFrames();
    s->title = dec->tags().title.empty() ? fileStem(path) : dec->tags().title;
    if (!dec->tags().art.empty()) s->artId = media::hashId(dec->tags().art);
    else if (library::folderCover(path, s->coverMime, s->cover)) s->artId = media::hashId(s->cover);   // a mappa képe
    s->token = token;
    s->dec = std::move(dec);
    return s;
}

bool FilePlayer::open(const std::string& path, std::string* error, uint64_t token) {
    std::string err;
    auto seg = makeSegment(path, &err, token);
    if (!seg) {
        if (error) *error = err;
        return false;
    }
    std::lock_guard<std::mutex> lk(m_);
    stopLocked();
    return startStreamLocked(std::move(seg), error);
}

bool FilePlayer::startStreamLocked(std::unique_ptr<Segment> seg, std::string* error) {
    rate_ = seg->dec->sampleRate();
    seg->start = 0;
    seg->end = UINT64_MAX;
    segs_.clear();
    segs_.push_back(std::move(*seg));
    pending_.reset();
    ringFrames_ = std::max<size_t>(rate_, kChunkFrames * 2);   // kb. 1 s
    ring_.assign(ringFrames_ * 2, 0.0f);
    wr_ = rd_ = played_ = base_ = 0;
    eof_ = finished_ = false;
    std::string err;
    if (!out_->open(rate_, [this](float* s, size_t n) { render(s, n); }, &err)) {
        segs_.clear();
        state_ = State::Idle;
        if (error) *error = err.empty() ? "A hangkimenet nem nyitható meg." : err;
        return false;
    }
    fillLocked();                     // az első adag már az indítás előtt a pufferben van
    state_ = State::Playing;
    out_->setActive(true);
    cv_.notify_all();
    return true;
}

void FilePlayer::setNextProvider(NextProvider next) {
    std::lock_guard<std::mutex> lk(m_);
    next_ = std::move(next);
}

void FilePlayer::dropPrefetched() {
    std::lock_guard<std::mutex> lk(m_);
    if (state_.load() == State::Idle || state_.load() == State::Ended || segs_.empty()) return;
    pending_.reset();
    out_->lock();                       // a render most nem fut: a puffer vége biztonságosan visszavehető
    // Az a szakasz marad utolsónak, amelyből a kimenet éppen olvas (a már átadott minták nem vehetők vissza)
    const uint64_t readPos = base_.load() + rd_.load();
    size_t j = 0;
    while (j + 1 < segs_.size() && readPos >= segs_[j].end) ++j;
    while (segs_.size() > j + 1) segs_.pop_back();
    Segment& s = segs_.back();
    if (s.end != UINT64_MAX) {
        // a szám dekódolása véget ért: a puffer a végénél vágódik, a dekóder a fájl végén áll (a következő read 0),
        // így a lejátszó a szám végén újra a sortól kérdezi a következőt
        const uint64_t boundary = s.end - base_.load();
        if (boundary >= rd_.load() && boundary <= wr_.load()) wr_ = boundary;
        s.end = UINT64_MAX;
        eof_ = finished_ = false;
    }
    out_->unlock();
    cv_.notify_all();
}

void FilePlayer::stopLocked() {
    pending_.reset();
    if (state_.load() == State::Idle) return;
    state_ = State::Idle;
    out_->close();
    segs_.clear();
}

void FilePlayer::stop() {
    std::lock_guard<std::mutex> lk(m_);
    stopLocked();
}

void FilePlayer::play() {
    std::lock_guard<std::mutex> lk(m_);
    const State s = state_.load();
    if (s == State::Idle || s == State::Playing) return;
    if (s == State::Ended) seekLocked(0);   // a vége után elölről
    state_ = State::Playing;
    out_->setActive(true);
    cv_.notify_all();
}

void FilePlayer::pause() {
    std::lock_guard<std::mutex> lk(m_);
    if (state_.load() != State::Playing) return;
    state_ = State::Paused;
    out_->setActive(false);
}

void FilePlayer::toggle() {
    if (state_.load() == State::Playing) pause();
    else play();
}

void FilePlayer::seek(double seconds) {
    std::lock_guard<std::mutex> lk(m_);
    const Segment* a = audibleLocked();
    if (state_.load() == State::Idle || !a || a->total == 0 || rate_ == 0) return;
    const double s = std::clamp(seconds, 0.0, static_cast<double>(a->total) / rate_);
    seekLocked(std::min<uint64_t>(static_cast<uint64_t>(s * rate_ + 0.5), a->total));
    if (state_.load() == State::Ended) {   // a vége után tekerve: szünet az új helyen
        state_ = State::Paused;
    }
    cv_.notify_all();
}

void FilePlayer::seekLocked(uint64_t frame) {
    out_->lock();                       // a render most nem fut: a puffer biztonságosan üríthető
    // A hallott szám marad egyedül az idővonalon (a már előre dekódolt következő szám kiesik, a sor újra megadja)
    const size_t k = audibleIndexLocked();
    if (k > 0) segs_.erase(segs_.begin(), segs_.begin() + static_cast<std::ptrdiff_t>(k));
    while (segs_.size() > 1) segs_.pop_back();
    pending_.reset();
    Segment& s = segs_.front();
    if (!s.dec->seek(frame)) {
        s.dec->seek(0);
        frame = 0;
    }
    s.start = 0;
    s.end = UINT64_MAX;
    wr_ = rd_ = 0;
    played_ = base_ = frame;
    eof_ = finished_ = false;
    out_->flush();                      // a kimenet pufferében várakozó régi minták se szóljanak
    out_->unlock();
    fillLocked();
}

uint64_t FilePlayer::audibleFrame() const {
    const uint64_t base = base_.load(), played = played_.load();
    const uint64_t given = played > base ? played - base : 0;
    return base + std::min(given, out_->consumedFrames());
}

size_t FilePlayer::audibleIndexLocked() const {
    const uint64_t t = audibleFrame();
    for (size_t i = 0; i < segs_.size(); ++i)
        if (t < segs_[i].end) return i;
    return segs_.empty() ? 0 : segs_.size() - 1;
}

const FilePlayer::Segment* FilePlayer::audibleLocked() const {
    return segs_.empty() ? nullptr : &segs_[audibleIndexLocked()];
}

void FilePlayer::command(const std::string& action, double value) {
    if (action == "play") play();
    else if (action == "pause") pause();
    else if (action == "toggle") toggle();
    else if (action == "seek") seek(value);
}

uint64_t FilePlayer::currentToken() const {
    std::lock_guard<std::mutex> lk(m_);
    if (state_.load() == State::Idle) return 0;
    const Segment* a = audibleLocked();
    return a ? a->token : 0;
}

double FilePlayer::position() const {
    std::lock_guard<std::mutex> lk(m_);
    const Segment* a = audibleLocked();
    if (rate_ == 0 || !a) return 0.0;
    const uint64_t t = audibleFrame();
    const double p = static_cast<double>(t > a->start ? t - a->start : 0) / rate_;
    return a->total ? std::min(p, static_cast<double>(a->total) / rate_) : p;
}

double FilePlayer::duration() const {
    std::lock_guard<std::mutex> lk(m_);
    const Segment* a = audibleLocked();
    return rate_ && a ? static_cast<double>(a->total) / rate_ : 0.0;
}

media::Info FilePlayer::info() const {
    media::Info m;
    std::lock_guard<std::mutex> lk(m_);
    const State s = state_.load();
    const Segment* a = audibleLocked();
    if (s == State::Idle || !a || !a->dec) return m;
    const uint64_t t = audibleFrame();
    m.available = true;
    m.app = "Aqulify";
    m.title = a->title;
    m.artist = a->dec->tags().artist;
    m.album = a->dec->tags().album;
    m.duration = rate_ ? static_cast<double>(a->total) / rate_ : 0.0;
    m.position = rate_ ? static_cast<double>(t > a->start ? t - a->start : 0) / rate_ : 0.0;
    if (m.duration > 0.0) m.position = std::min(m.position, m.duration);
    m.positionAt = std::chrono::steady_clock::now();
    m.playing = s == State::Playing;
    m.canPlay = !m.playing;
    m.canPause = m.playing;
    m.canSeek = a->total > 0;
    m.artId = a->artId;
    return m;
}

bool FilePlayer::art(const std::string& id, std::string& mime, std::string& bytes) const {
    std::lock_guard<std::mutex> lk(m_);
    if (id.empty()) return false;
    for (const Segment& s : segs_) {
        if (s.dec && id == s.artId) {
            const bool embedded = !s.dec->tags().art.empty();
            mime = embedded ? s.dec->tags().artMime : s.coverMime;
            bytes = embedded ? s.dec->tags().art : s.cover;
            return true;
        }
    }
    return false;
}

bool FilePlayer::appendNextLocked() {
    if (!next_ || segs_.empty()) return false;
    uint64_t after = segs_.back().token;
    for (int i = 0; i < kMaxSkip; ++i) {
        std::string path;
        uint64_t token = 0;
        if (!next_(after, path, token)) return false;
        auto seg = makeSegment(path, nullptr, token);
        if (!seg) { after = token; continue; }   // nem nyitható: a sor következő száma
        if (seg->dec->sampleRate() != rate_) {
            pending_ = std::move(seg);           // eltérő mintavétel: a mostani vége után, új hangfolyamon
            return false;
        }
        seg->start = segs_.back().end;
        seg->end = UINT64_MAX;
        segs_.push_back(std::move(*seg));
        return true;
    }
    return false;
}

bool FilePlayer::fillLocked() {
    while (!eof_.load() && !segs_.empty()) {
        Segment& s = segs_.back();
        const uint64_t w = wr_.load(), r = rd_.load(std::memory_order_acquire);
        const size_t free = ringFrames_ - static_cast<size_t>(w - r);
        if (free < kChunkFrames) return true;
        if (decodeBuf_.size() < kChunkFrames * s.channels) decodeBuf_.assign(kChunkFrames * s.channels, 0.0f);
        const size_t got = s.dec->read(decodeBuf_.data(), kChunkFrames);
        if (got == 0) {
            // a szám vége az idővonalon; azonos mintavételnél a következő szám ugyanebben a pufferben folytatódik
            s.end = base_.load() + w;
            if (!appendNextLocked()) { eof_ = true; break; }
            continue;
        }
        const uint32_t srcCh = s.channels;
        const float* in = decodeBuf_.data();
        for (size_t i = 0; i < got; ++i, in += srcCh) {
            float l, rr;
            if (srcCh == 1) l = rr = in[0];
            else if (srcCh == 2) { l = in[0]; rr = in[1]; }
            else {
                // FL FR [FC LFE] [BL BR …]: elülső + 0,707 · középső + 0,707 · hátsó (az LFE kimarad)
                constexpr float k = 0.70710678f;
                l = in[0]; rr = in[1];
                if (srcCh >= 3) { l += k * in[2]; rr += k * in[2]; }
                if (srcCh >= 6) { l += k * in[4]; rr += k * in[5]; }
                else if (srcCh == 5) { l += k * in[3]; rr += k * in[4]; }
            }
            const size_t idx = static_cast<size_t>((w + i) % ringFrames_) * 2;
            ring_[idx] = l;
            ring_[idx + 1] = rr;
        }
        wr_.store(w + got, std::memory_order_release);
    }
    return false;
}

void FilePlayer::render(float* stereo, size_t frames) {
    if (state_.load() != State::Playing) {
        std::memset(stereo, 0, frames * 2 * sizeof(float));
        return;
    }
    const uint64_t r = rd_.load(), w = wr_.load(std::memory_order_acquire);
    const size_t n = std::min<size_t>(frames, static_cast<size_t>(w - r));
    for (size_t i = 0; i < n; ++i) {
        const size_t idx = static_cast<size_t>((r + i) % ringFrames_) * 2;
        stereo[2 * i] = ring_[idx];
        stereo[2 * i + 1] = ring_[idx + 1];
    }
    if (n < frames) std::memset(stereo + 2 * n, 0, (frames - n) * 2 * sizeof(float));
    rd_.store(r + n, std::memory_order_release);
    played_.fetch_add(n);
    if (n < frames && eof_.load()) {
        finished_ = true;
        cv_.notify_all();
    } else if (!eof_.load() && (w - r - n) < ringFrames_ / 2) {
        cv_.notify_all();   // a puffer félig kiürült: a dekódoló szál ne a következő 20 ms-os ébredésre várjon
    }
}

void FilePlayer::decodeLoop() {
    std::unique_lock<std::mutex> lk(m_);
    while (!quit_) {
        cv_.wait_for(lk, std::chrono::milliseconds(20));
        if (quit_) break;
        if (segs_.empty()) continue;
        // a már elhangzott számok kiesnek (a hallott és a később következők maradnak)
        while (segs_.size() > 1 && audibleFrame() >= segs_.front().end) segs_.pop_front();
        if (finished_.load() && state_.load() == State::Playing) {
            // a szám vége: megvárjuk, amíg a kimenet pufferéből az utolsó minta is elszól (különben a vége, pl.
            // WASAPI-nál kb. 100 ms, levágódna); utána a kimenet leáll, a pozíció a végén marad
            if (audibleFrame() < played_.load()) continue;
            if (pending_) {
                // a sor következő száma más mintavétellel: új hangfolyam (ha nem nyitható, a lejátszás leáll)
                std::unique_ptr<Segment> next = std::move(pending_);
                out_->close();
                if (!startStreamLocked(std::move(next), nullptr)) out_->close();
                continue;
            }
            state_ = State::Ended;
            out_->setActive(false);
            continue;
        }
        if (state_.load() == State::Playing || state_.load() == State::Paused) fillLocked();
    }
}

} // namespace ee_analyzer::player
