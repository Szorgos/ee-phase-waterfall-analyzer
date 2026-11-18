/**
 * @file file_player.cpp
 * @brief A beépített zenelejátszó magja (platformfüggetlen); a hangkimenet a PlayerOutput.
 */
#include "file_player.hpp"

#include <algorithm>

#include "music_library.hpp"
#include "load_meter.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

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

bool isDsdFilePath(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return false;
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return ext == "dsf" || ext == "dff";
}

/** DoP 1.1 csend (kDsdSilenceByte = 0xAA, váltakozó 0x05/0xFA jelölővel, soha nem nulla PCM minta). */
uint8_t fillDopSilenceFrames(int32_t* outStereo, size_t frames, uint8_t startPhase) {
    static constexpr uint8_t kSilenceBytes[4] = {
        dsd::kDsdSilenceByte, dsd::kDsdSilenceByte,
        dsd::kDsdSilenceByte, dsd::kDsdSilenceByte
    };
    uint8_t phase = startPhase & 1u;
    for (size_t i = 0; i < frames; ++i) {
        phase = dsd::packDopFrames(kSilenceBytes, 1, 2, phase, outStereo + 2 * i);
    }
    return phase;
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

FilePlayer::TempCopy::~TempCopy() {
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

namespace {
/** UTF-8 szöveg <-> útvonal (Windowson is helyes; C++20-ban a u8string char8_t). */
std::filesystem::path pathFromUtf8(const std::string& s) {
#if defined(__cpp_char8_t)
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
#else
    return std::filesystem::u8path(s);
#endif
}
std::string pathToUtf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

/** Az előbetöltés helye: Linuxon a RAM-ban lévő futásidejű mappa (XDG_RUNTIME_DIR), máshol a rendszer ideiglenes mappája. */
std::filesystem::path preloadDir() {
    std::error_code ec;
    if (const char* x = std::getenv("XDG_RUNTIME_DIR"); x && *x && std::filesystem::is_directory(x, ec)) return std::filesystem::path(x) / "aqulify-preload";
    return std::filesystem::temp_directory_path(ec) / "aqulify-preload";
}

/** A fájl másolata az előbetöltés mappájába; üres: nem sikerült (vagy túl nagy). */
std::string copyForPreload(const std::string& src, uint64_t maxBytes) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const uint64_t size = fs::file_size(pathFromUtf8(src), ec);
    if (ec || size == 0 || size > maxBytes) return {};
    const fs::path dir = preloadDir();
    fs::create_directories(dir, ec);
    static std::atomic<uint64_t> counter{0};
    const fs::path dst = dir / ("p" + std::to_string(counter.fetch_add(1)) + pathFromUtf8(src).extension().string());
    if (!fs::copy_file(pathFromUtf8(src), dst, fs::copy_options::overwrite_existing, ec) || ec) {
        fs::remove(dst, ec);
        return {};
    }
    return pathToUtf8(dst);
}
} // namespace

float FilePlayer::gainFor(const TrackTags& t) const {
    const int mode = rgMode_.load();
    if (mode == 0) return 1.0f;
    float g = mode == 2 && !std::isnan(t.rgAlbumGain) ? t.rgAlbumGain : t.rgTrackGain;
    float peak = mode == 2 && !std::isnan(t.rgAlbumGain) ? t.rgAlbumPeak : t.rgTrackPeak;
    if (std::isnan(g)) g = mode == 1 && !std::isnan(t.rgAlbumGain) ? t.rgAlbumGain : NAN;
    if (std::isnan(g)) return 1.0f;   // címke nélkül a szám érintetlen
    float lin = std::pow(10.0f, (g + rgPreampDb_.load()) / 20.0f);
    if (rgPreventClip_.load() && !std::isnan(peak) && peak > 0.0f) lin = std::min(lin, 1.0f / peak);
    return lin;
}

void FilePlayer::setReplayGain(int mode, float preampDb, bool preventClip) {
    rgMode_ = std::clamp(mode, 0, 2);
    rgPreampDb_ = std::clamp(preampDb, -15.0f, 15.0f);
    rgPreventClip_ = preventClip;
    std::lock_guard<std::mutex> lk(m_);
    for (Segment& s : segs_) if (s.dec) s.gain = gainFor(s.dec->tags());
    if (pending_ && pending_->dec) pending_->gain = gainFor(pending_->dec->tags());
}

float FilePlayer::currentGainDb() const {
    if (dopActive_.load()) return 0.0f;   // DoP módban a ReplayGain és minden mintamódosítás kimarad
    std::lock_guard<std::mutex> lk(m_);
    const Segment* a = audibleLocked();
    return a && a->gain > 0.0f ? 20.0f * std::log10(a->gain) : 0.0f;
}

std::string FilePlayer::dopFallbackReason() const {
    std::lock_guard<std::mutex> lk(m_);
    return dopFallback_;
}

void FilePlayer::setDsdDop(bool on) {
    if (dsdDop_.exchange(on) == on) return;
    dropPrefetched();
    std::string reopenPath;
    uint64_t reopenToken = 0;
    double reopenPos = 0.0;
    State prevState = State::Idle;
    {
        std::lock_guard<std::mutex> lk(m_);
        prevState = state_.load();
        const Segment* a = audibleLocked();
        if (prevState != State::Idle && a && isDsdFilePath(a->path)) {
            reopenPath = a->path;
            reopenToken = a->token;
            const uint64_t t = audibleFrame();
            reopenPos = rate_ ? static_cast<double>(t > a->start ? t - a->start : 0) / rate_ : 0.0;
        }
    }
    if (!reopenPath.empty()) {
        if (open(reopenPath, nullptr, reopenToken)) {
            if (reopenPos > 0.0) seek(reopenPos);
            if (prevState == State::Paused) pause();
        }
    }
}

std::unique_ptr<FilePlayer::Segment> FilePlayer::makeSegment(const std::string& path, std::string* error, uint64_t token, bool bypassUpsample) {
    std::unique_ptr<TempCopy> temp;
    if (preload_.load()) {
        std::string copy = copyForPreload(path, kPreloadMaxBytes);
        if (!copy.empty()) temp = std::make_unique<TempCopy>(std::move(copy));
    }
    const std::string actualPath = temp ? temp->path : path;
    auto dec = openDecoder(actualPath, error);
    if (!dec) return nullptr;
    auto s = std::make_unique<Segment>();
    s->temp = std::move(temp);
    s->gain = gainFor(dec->tags());
    s->path = path;
    s->channels = dec->channels();
    s->total = dec->totalFrames();
    s->title = dec->tags().title.empty() ? fileStem(path) : dec->tags().title;
    if (!dec->tags().art.empty()) s->artId = media::hashId(dec->tags().art);
    else if (library::folderCover(path, s->coverMime, s->cover)) s->artId = media::hashId(s->cover);   // a mappa képe
    s->token = token;
    s->bypassUpsample = bypassUpsample;
    if (dsdDop_.load() && isDsdFilePath(path)) {
        std::string rawErr;
        auto raw = dsd::openDsdRawFile(actualPath, &rawErr);
        if (raw) {
            s->dsdRate = raw->info().dsdSampleRate;
            if (raw->info().channels >= 1 && raw->info().channels <= 2 && raw->info().dopSampleRate > 0) {
                s->dopRate = raw->info().dopSampleRate;
                s->dopTotal = (raw->totalChannelBytes() + 1ULL) / 2ULL;
                s->dopRaw = std::move(raw);
            } else {
                s->dopFallback = "format";
            }
        } else {
            s->dopFallback = "format";
        }
    }
    s->dec = std::move(dec);
    return s;
}

bool FilePlayer::open(const std::string& path, std::string* error, uint64_t token, bool bypassUpsample) {
    std::string err;
    auto seg = makeSegment(path, &err, token, bypassUpsample);
    if (!seg) {
        if (error) *error = err;
        return false;
    }
    std::lock_guard<std::mutex> lk(m_);
    const State st = state_.load();
    const bool curDop = dopActive_.load();
    const bool nextDop = seg->dopRaw != nullptr;
    const uint32_t nextSrcRate = nextDop ? seg->dopRate : seg->dec->sampleRate();
    const uint32_t nextTargetRate = decideTargetRate(nextSrcRate, seg->bypassUpsample, nextDop || (seg->dsdRate > 0));
    if (st != State::Idle && !segs_.empty() && curDop == nextDop && nextTargetRate == rate_ && out_->settlesOnRateChange() &&
        (!upsampleActive_.load() || nextSrcRate == upsampleSrcRate_.load())) {
        replaceTrackLocked(std::move(seg), st != State::Playing);
        return true;
    }
    const bool modeChanged = (st != State::Idle && curDop != nextDop);
    stopLocked();
    return startStreamLocked(std::move(seg), error, modeChanged);
}

void FilePlayer::replaceTrackLocked(std::unique_ptr<Segment> seg, bool streamStopped) {
    out_->lock();                       // a render most nem fut: a puffer biztonságosan üríthető
    const bool dop = dopActive_.load() && seg->dopRaw != nullptr;
    if (dop) {
        seg->total = seg->dopTotal;
        dopDsdRate_ = seg->dsdRate;
        dopFallback_.clear();
    } else {
        dopDsdRate_ = seg->dsdRate;
        dopFallback_ = seg->dopFallback;
        if (upsampleActive_.load() && seg->total > 0 && upsampleSrcRate_.load() > 0) {
            seg->total = static_cast<uint64_t>(std::round(static_cast<double>(seg->dec->totalFrames()) * rate_ / upsampleSrcRate_.load()));
        }
    }
    seg->start = 0;
    seg->end = UINT64_MAX;
    segs_.clear();
    segs_.push_back(std::move(*seg));
    pending_.reset();
    wr_ = rd_ = played_ = base_ = 0;
    eof_ = finished_ = false;
    out_->flush();                      // az előző szám pufferben maradt része ne szóljon
    // Ha a kimenet állt (szünet / a szám vége), az újrainduláskor a DAC újra szinkronizál: csenddel kezd
    const uint64_t settleFrames = static_cast<uint64_t>(rate_) * kRateSettleMs / 1000;
    leadTotal_ = streamStopped ? (dop ? (settleFrames & ~uint64_t(1)) : settleFrames) : 0;
    leadIn_ = leadTotal_.load();
    if (dop) dopPhase_ = dopRenderPhase_;
    out_->unlock();
    fillLocked();
    state_ = State::Playing;
    out_->setActive(true);
    cv_.notify_all();
}

bool FilePlayer::startStreamLocked(std::unique_ptr<Segment> seg, std::string* error, bool modeChanged) {
    if (seg->dopRaw) {
        std::string dopErr;
        if (out_->openDop(seg->dopRate, [this](int32_t* d, size_t n) { renderDop(d, n); }, &dopErr)) {
            dopActive_ = true;
            dopDsdRate_ = seg->dsdRate;
            dopFallback_.clear();
            rate_ = seg->dopRate;
            seg->total = seg->dopTotal;
            seg->start = 0;
            seg->end = UINT64_MAX;
            segs_.clear();
            segs_.push_back(std::move(*seg));
            pending_.reset();
            ringFrames_ = std::max<size_t>(rate_, kChunkFrames * 2);
            dopRing_.assign(ringFrames_ * 2, 0);
            wr_ = rd_ = played_ = base_ = 0;
            eof_ = finished_ = false;
            dopPhase_ = 0;
            dopRenderPhase_ = 0;
            leadIn_ = 0;
            leadTotal_ = 0;
            if (out_->settlesOnRateChange() || modeChanged) {
                // DoP-indítás és PCM<->DoP váltás: kRateSettleMs DoP-csend (kDsdSilenceByte = 0xAA csomagolva,
                // páros keretszámmal, hogy a csend után az első DoP adatkeret is 0x05 jelölővel induljon).
                leadTotal_ = (static_cast<uint64_t>(rate_) * kRateSettleMs / 1000) & ~uint64_t(1);
                leadIn_ = leadTotal_.load();
            }
            fillLocked();
            state_ = State::Playing;
            out_->setActive(true);
            cv_.notify_all();
            return true;
        }
        // A hangeszköz nem fogadta el a DoP formátumot / mintavételt: visszalépés PCM-átalakításra
        seg->dopRaw.reset();
        seg->dopFallback = dopErr.empty() ? "format" : dopErr;
    }
    dopActive_ = false;
    dopDsdRate_ = seg->dsdRate;
    dopFallback_ = seg->dopFallback;
    const uint32_t srcRate = seg->dec->sampleRate();
    const bool isDsd = (seg->dsdRate > 0);
    const uint32_t targetRate = decideTargetRate(srcRate, seg->bypassUpsample, isDsd);

    if (targetRate > srcRate) {
        upsampleActive_ = true;
        upsampleSrcRate_ = srcRate;
        upsampleTargetRate_ = targetRate;
        // Szálak: 2 = kért; automatikus = 2 szál a nagy célmintavételeknél (≥ 352,8 kHz), ha van legalább 4 mag
        const int thr = upsampleOpts_.upsampleThreads == 2 ? 2
                      : upsampleOpts_.upsampleThreads == 1 ? 1
                      : (targetRate >= 352800 && load::logicalCores() >= 4 ? 2 : 1);
        upsampler_.setThreads(thr);
        upsampleThreadsUsed_ = thr;
        upsampler_.initWithOptions(srcRate, targetRate, 2, upsampleOpts_);
        rate_ = targetRate;
        if (seg->total > 0) {
            seg->total = static_cast<uint64_t>(std::round(static_cast<double>(seg->dec->totalFrames()) * targetRate / srcRate));
        }
    } else {
        upsampleActive_ = false;
        upsampleSrcRate_ = srcRate;
        upsampleTargetRate_ = srcRate;
        upsampler_.reset();
        rate_ = srcRate;
    }

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
    leadIn_ = 0;
    leadTotal_ = 0;
    if (!out_->open(rate_, [this](float* s, size_t n) { render(s, n); }, &err)) {
        segs_.clear();
        state_ = State::Idle;
        if (error) *error = err.empty() ? "A hangkimenet nem nyitható meg." : err;
        return false;
    }
    // Kattanásmentes indulás: kizárólagos módban (és DoP<->PCM váltáskor) minden új hangfolyam rövid csenddel kezd,
    // amíg a DAC az új órajelre és üzemmódra áll és elnémítását feloldja
    if (out_->settlesOnRateChange() || modeChanged) {
        leadTotal_ = static_cast<uint64_t>(rate_) * kRateSettleMs / 1000;
        leadIn_ = leadTotal_.load();
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
    dopActive_ = false;
    dopDsdRate_ = 0;
    dopFallback_.clear();
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
    if (dopActive_.load() && s.dopRaw) {
        if (!s.dopRaw->seekByte(frame * 2ULL)) {
            s.dopRaw->seekByte(0);
            frame = 0;
        }
    } else {
        uint64_t seekFrame = frame;
        if (upsampleActive_.load() && upsampleSrcRate_.load() > 0 && rate_ > 0) {
            seekFrame = static_cast<uint64_t>(std::round(static_cast<double>(frame) * upsampleSrcRate_.load() / rate_));
        }
        if (!s.dec->seek(seekFrame)) {
            s.dec->seek(0);
            frame = 0;
        }
        if (upsampleActive_.load()) {
            upsampler_.clear();
        }
    }
    s.start = 0;
    s.end = UINT64_MAX;
    wr_ = rd_ = 0;
    played_ = base_ = frame;
    eof_ = finished_ = false;
    out_->flush();                      // a kimenet pufferében várakozó régi minták se szóljanak
    leadIn_ = 0;                        // tekeréskor az eleji csend elmarad (a flush a számlálót is nullázza)
    leadTotal_ = 0;
    if (dopActive_.load()) dopPhase_ = dopRenderPhase_;
    out_->unlock();
    fillLocked();
}

uint64_t FilePlayer::audibleFrame() const {
    const uint64_t base = base_.load(), played = played_.load();
    const uint64_t given = played > base ? played - base : 0;
    uint64_t consumed = out_->consumedFrames();
    if (consumed != UINT64_MAX) {   // a hangfolyam eleji csend nem része a számnak
        const uint64_t lead = leadTotal_.load();
        consumed = consumed > lead ? consumed - lead : 0;
    }
    return base + std::min(given, consumed);
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

uint32_t FilePlayer::streamRate() const {
    std::lock_guard<std::mutex> lk(m_);
    return state_.load() == State::Idle ? 0 : rate_;
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
    const bool curDop = dopActive_.load();
    for (int i = 0; i < kMaxSkip; ++i) {
        std::string path;
        uint64_t token = 0;
        if (!next_(after, path, token)) return false;
        auto seg = makeSegment(path, nullptr, token);
        if (!seg) { after = token; continue; }
        const bool nextDop = seg->dopRaw != nullptr;
        const uint32_t nextSrcRate = nextDop ? seg->dopRate : seg->dec->sampleRate();
        const uint32_t nextTargetRate = decideTargetRate(nextSrcRate, seg->bypassUpsample, nextDop || (seg->dsdRate > 0));
        if (nextDop != curDop || nextTargetRate != rate_ || (upsampleActive_.load() && nextSrcRate != upsampleSrcRate_.load())) {
            pending_ = std::move(seg);           // eltérő mód vagy mintavétel: új hangfolyamon
            return false;
        }
        if (nextDop) seg->total = seg->dopTotal;
        else if (upsampleActive_.load() && seg->total > 0 && upsampleSrcRate_.load() > 0) {
            seg->total = static_cast<uint64_t>(std::round(static_cast<double>(seg->dec->totalFrames()) * rate_ / upsampleSrcRate_.load()));
        }
        seg->start = segs_.back().end;
        seg->end = UINT64_MAX;
        segs_.push_back(std::move(*seg));
        return true;
    }
    return false;
}

bool FilePlayer::fillLocked() {
    if (dopActive_.load()) {
        while (!eof_.load() && !segs_.empty()) {
            Segment& s = segs_.back();
            const uint64_t w = wr_.load(), r = rd_.load(std::memory_order_acquire);
            const size_t free = ringFrames_ - static_cast<size_t>(w - r);
            if (free < kChunkFrames) return true;
            const uint32_t srcCh = s.dopRaw ? s.dopRaw->info().channels : 0;
            if (!s.dopRaw || srcCh == 0 || srcCh > 2) {
                s.end = base_.load() + w;
                if (!appendNextLocked()) { eof_ = true; break; }
                continue;
            }
            // 1 DoP keret = 2 DSD bájt csatornánként (MSB-first); ReplayGain és mintamódosítás nélkül
            if (dopRawBuf_.size() < kChunkFrames * 2 * srcCh)
                dopRawBuf_.assign(kChunkFrames * 2 * srcCh, dsd::kDsdSilenceByte);
            const size_t gotBytes = s.dopRaw->read(dopRawBuf_.data(), kChunkFrames * 2);
            if (gotBytes == 0) {
                s.end = base_.load() + w;
                if (!appendNextLocked()) { eof_ = true; break; }
                continue;
            }
            if (gotBytes & 1u) {
                for (uint32_t c = 0; c < srcCh; ++c)
                    dopRawBuf_[gotBytes * srcCh + c] = dsd::kDsdSilenceByte;
            }
            const size_t gotFrames = (gotBytes + 1) / 2;
            // A DsdRawSource bájtonként váltogatja a csatornákat ([L0, R0, L1, R1]); a packDopFrames
            // csatornánként 2 egymást követő DSD bájtot vár ([L0, L1, R0, R1]).
            if (dopPackBuf_.size() < gotFrames * 4) dopPackBuf_.resize(gotFrames * 4);
            for (size_t f = 0; f < gotFrames; ++f) {
                if (srcCh == 1) {
                    const uint8_t b0 = dopRawBuf_[2 * f];
                    const uint8_t b1 = dopRawBuf_[2 * f + 1];
                    dopPackBuf_[4 * f + 0] = b0;
                    dopPackBuf_[4 * f + 1] = b1;
                    dopPackBuf_[4 * f + 2] = b0;
                    dopPackBuf_[4 * f + 3] = b1;
                } else {
                    dopPackBuf_[4 * f + 0] = dopRawBuf_[(2 * f) * 2 + 0];
                    dopPackBuf_[4 * f + 1] = dopRawBuf_[(2 * f + 1) * 2 + 0];
                    dopPackBuf_[4 * f + 2] = dopRawBuf_[(2 * f) * 2 + 1];
                    dopPackBuf_[4 * f + 3] = dopRawBuf_[(2 * f + 1) * 2 + 1];
                }
            }
            if (dopFrameBuf_.size() < gotFrames * 2) dopFrameBuf_.resize(gotFrames * 2);
            dopPhase_ = dsd::packDopFrames(dopPackBuf_.data(), gotFrames, 2, dopPhase_, dopFrameBuf_.data());
            for (size_t i = 0; i < gotFrames; ++i) {
                const size_t idx = static_cast<size_t>((w + i) % ringFrames_) * 2;
                dopRing_[idx] = dopFrameBuf_[2 * i];
                dopRing_[idx + 1] = dopFrameBuf_[2 * i + 1];
            }
            wr_.store(w + gotFrames, std::memory_order_release);
        }
        return false;
    }
    while (!eof_.load() && !segs_.empty()) {
        Segment& s = segs_.back();
        const uint64_t w = wr_.load(), r = rd_.load(std::memory_order_acquire);
        const size_t free = ringFrames_ - static_cast<size_t>(w - r);
        const size_t ratioCeil = upsampleActive_.load()
            ? static_cast<size_t>(std::ceil(static_cast<double>(rate_) / std::max<uint32_t>(1u, upsampleSrcRate_.load())))
            : 1u;
        const size_t minFree = std::max<size_t>(kChunkFrames * ratioCeil + 1024, upsampleActive_.load() ? 16384u : kChunkFrames);
        if (free < minFree) return true;
        if (decodeBuf_.size() < kChunkFrames * s.channels) decodeBuf_.assign(kChunkFrames * s.channels, 0.0f);
        const size_t got = s.dec->read(decodeBuf_.data(), kChunkFrames);
        if (got == 0) {
            // A szám vége az idővonalon: felmintavételezéskor a libsoxr szűrő késleltetése (soxr_delay)
            // még az előző szám mintáit tartja vissza, így a számhatár ennyivel később lép ki a kimeneten (K6)
            const uint64_t delayOut = upsampleActive_.load()
                ? static_cast<uint64_t>(std::llround(std::max(0.0, upsampler_.delayFrames())))
                : 0;
            s.end = base_.load() + w + delayOut;
            if (appendNextLocked()) {
                continue; // szünetmentes átmenet azonos forrással, a szűrő folytonos marad (K7)
            }
            if (upsampleActive_.load()) {
                size_t flFrames = 0;
                if (upsampleOutBuf_.size() < kChunkFrames * 8) upsampleOutBuf_.resize(kChunkFrames * 8);
                upsampler_.flush(upsampleOutBuf_.data(), upsampleOutBuf_.size() / 2, flFrames);
                for (size_t i = 0; i < flFrames; ++i) {
                    const size_t idx = static_cast<size_t>((w + i) % ringFrames_) * 2;
                    ring_[idx] = upsampleOutBuf_[2 * i];
                    ring_[idx + 1] = upsampleOutBuf_[2 * i + 1];
                }
                wr_.store(w + flFrames, std::memory_order_release);
                s.end = base_.load() + wr_.load();
            }
            eof_ = true;
            break;
        }
        const uint32_t srcCh = s.channels;
        const float gain = s.gain;
        if (gain != 1.0f) for (size_t i = 0; i < got * srcCh; ++i) decodeBuf_[i] *= gain;

        if (stereoMixBuf_.size() < got * 2) stereoMixBuf_.resize(got * 2);
        const float* in = decodeBuf_.data();
        for (size_t i = 0; i < got; ++i, in += srcCh) {
            float l, rr;
            if (srcCh == 1) l = rr = in[0];
            else if (srcCh == 2) { l = in[0]; rr = in[1]; }
            else {
                constexpr float k = 0.70710678f;
                l = in[0]; rr = in[1];
                if (srcCh >= 3) { l += k * in[2]; rr += k * in[2]; }
                if (srcCh >= 6) { l += k * in[4]; rr += k * in[5]; }
                else if (srcCh == 5) { l += k * in[3]; rr += k * in[4]; }
            }
            stereoMixBuf_[2 * i] = l;
            stereoMixBuf_[2 * i + 1] = rr;
        }

        if (upsampleActive_.load()) {
            size_t neededOut = static_cast<size_t>(std::ceil(got * (static_cast<double>(rate_) / s.dec->sampleRate()))) + 256;
            if (upsampleOutBuf_.size() < neededOut * 2) upsampleOutBuf_.resize(neededOut * 2);
            size_t outFrames = 0;
            upsampler_.process(stereoMixBuf_.data(), got, upsampleOutBuf_.data(), upsampleOutBuf_.size() / 2, outFrames);
            for (size_t i = 0; i < outFrames; ++i) {
                const size_t idx = static_cast<size_t>((w + i) % ringFrames_) * 2;
                ring_[idx] = upsampleOutBuf_[2 * i];
                ring_[idx + 1] = upsampleOutBuf_[2 * i + 1];
            }
            wr_.store(w + outFrames, std::memory_order_release);
        } else {
            for (size_t i = 0; i < got; ++i) {
                const size_t idx = static_cast<size_t>((w + i) % ringFrames_) * 2;
                ring_[idx] = stereoMixBuf_[2 * i];
                ring_[idx + 1] = stereoMixBuf_[2 * i + 1];
            }
            wr_.store(w + got, std::memory_order_release);
        }
    }
    return false;
}
void FilePlayer::render(float* stereo, size_t frames) {
    if (state_.load() != State::Playing) {
        std::memset(stereo, 0, frames * 2 * sizeof(float));
        return;
    }
    // A hangfolyam eleji csend (kattanásmentes váltás): a DAC ez alatt áll az új órajelre; a pozíciót nem lépteti
    if (const uint64_t lead = leadIn_.load()) {
        const size_t z = static_cast<size_t>(std::min<uint64_t>(frames, lead));
        std::memset(stereo, 0, z * 2 * sizeof(float));
        leadIn_.store(lead - z);
        if (z == frames) return;
        stereo += 2 * z;
        frames -= z;
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
        if (n < frames) underruns_.fetch_add(1, std::memory_order_relaxed);   // hangkimaradás (a terhelés-tanácsadónak)
        cv_.notify_all();   // a puffer félig kiürült: a dekódoló szál ne a következő 20 ms-os ébredésre várjon
    }
}

void FilePlayer::renderDop(int32_t* stereoDop24, size_t frames) {
    if (state_.load() != State::Playing) {
        dopRenderPhase_ = fillDopSilenceFrames(stereoDop24, frames, dopRenderPhase_);
        return;
    }
    // A hangfolyam eleji DoP-csend (kattanásmentes váltás): kDsdSilenceByte (0xAA) DoP-ba csomagolva, soha nem 0!
    if (const uint64_t lead = leadIn_.load()) {
        const size_t z = static_cast<size_t>(std::min<uint64_t>(frames, lead));
        dopRenderPhase_ = fillDopSilenceFrames(stereoDop24, z, dopRenderPhase_);
        leadIn_.store(lead - z);
        if (z == frames) return;
        stereoDop24 += 2 * z;
        frames -= z;
    }
    const uint64_t r = rd_.load(), w = wr_.load(std::memory_order_acquire);
    const size_t n = std::min<size_t>(frames, static_cast<size_t>(w - r));
    // A jelölőt (0x05 / 0xFA) itt, a kimenet saját fázisszámlálója szerint írjuk a szavakba: így a váltakozás akkor sem
    // törik meg, ha előtte páratlan számú csendkeret ment ki (pl. alulcsordulás), és a DAC nem esik ki a DoP-módból
    for (size_t i = 0; i < n; ++i) {
        const size_t idx = static_cast<size_t>((r + i) % ringFrames_) * 2;
        const uint32_t marker = ((dopRenderPhase_ + i) & 1u) ? 0xFAu : 0x05u;
        stereoDop24[2 * i] = static_cast<int32_t>((static_cast<uint32_t>(dopRing_[idx]) & 0xFFFFu) | (marker << 16));
        stereoDop24[2 * i + 1] = static_cast<int32_t>((static_cast<uint32_t>(dopRing_[idx + 1]) & 0xFFFFu) | (marker << 16));
    }
    dopRenderPhase_ = static_cast<uint8_t>((dopRenderPhase_ + n) & 1u);
    if (n < frames) {
        dopRenderPhase_ = fillDopSilenceFrames(stereoDop24 + 2 * n, frames - n, dopRenderPhase_);
    }
    rd_.store(r + n, std::memory_order_release);
    played_.fetch_add(n);
    if (n < frames && eof_.load()) {
        finished_ = true;
        cv_.notify_all();
    } else if (!eof_.load() && (w - r - n) < ringFrames_ / 2) {
        cv_.notify_all();
    }
}

void FilePlayer::decodeLoop() {
    std::unique_lock<std::mutex> lk(m_);
    while (!quit_) {
        cv_.wait_for(lk, std::chrono::milliseconds(20));
        if (quit_) break;
        // Dedikált mag (Linux / Windows): a beállítás változása a következő körben érvényes
        if (const int want = coreWanted_.load(); want != coreApplied_.load()) {
            if (load::pinCurrentThread(want)) coreApplied_ = (want >= 0 ? load::currentThreadAffinitySingle() : -1);
            else coreApplied_ = -1, coreWanted_ = -1;
        }
        if (segs_.empty()) continue;
        // a már elhangzott számok kiesnek (a hallott és a később következők maradnak)
        while (segs_.size() > 1 && audibleFrame() >= segs_.front().end) segs_.pop_front();
        if (finished_.load() && state_.load() == State::Playing) {
            // a szám vége: megvárjuk, amíg a kimenet pufferéből az utolsó minta is elszól (különben a vége, pl.
            // WASAPI-nál kb. 100 ms, levágódna); utána a kimenet leáll, a pozíció a végén marad
            if (audibleFrame() < played_.load()) continue;
            if (pending_) {
                // a sor következő száma más mintavétellel / móddal: új hangfolyam (ha nem nyitható, a lejátszás leáll)
                std::unique_ptr<Segment> next = std::move(pending_);
                const bool modeChanged = (dopActive_.load() != (next->dopRaw != nullptr));
                out_->close();
                dopActive_ = false;
                if (!startStreamLocked(std::move(next), nullptr, modeChanged)) out_->close();
                continue;
            }
            state_ = State::Ended;
            out_->setActive(false);
            continue;
        }
        if (state_.load() == State::Playing || state_.load() == State::Paused) fillLocked();
    }
}

uint32_t FilePlayer::decideTargetRate(uint32_t srcRate, bool bypassUpsample, bool isDsd) const {
    audioopt::UpsampleRule rule = upsampleOpts_.upsampleRule;
    if (rule == audioopt::UpsampleRule::Off && upsampleEnabled_) {
        rule = audioopt::UpsampleRule::Double;
    }
    const std::vector<uint32_t> supported = out_
        ? out_->supportedRates()
        : std::vector<uint32_t>{ 44100, 48000, 88200, 96000, 176400, 192000 };
    return UpsampleEngine::selectTargetRate(srcRate, rule, supported, upsampleOpts_.upsampleCustom,
                                            bypassUpsample, isDsd, &upsampleReason_, upsampleOpts_.upsampleMaxRate);
}

void FilePlayer::setUpsample(bool on) {
    std::lock_guard<std::mutex> lk(m_);
    upsampleEnabled_ = on;
}

void FilePlayer::setUpsampleOptions(const audioopt::AudioOptions& opts) {
    std::lock_guard<std::mutex> lk(m_);
    upsampleOpts_ = opts;
    upsampleEnabled_ = (opts.upsampleRule != audioopt::UpsampleRule::Off);
    coreWanted_ = load::affinitySupported() ? opts.dedicatedCore : -1;
}

uint64_t FilePlayer::upsampleClippedSamples() const {
    std::lock_guard<std::mutex> lk(m_);
    return upsampler_.clippedSamples();
}

uint64_t FilePlayer::upsampleProcessedFrames() const {
    std::lock_guard<std::mutex> lk(m_);
    return upsampler_.processedFrames();
}

double FilePlayer::upsampleDelayMs() const {
    std::lock_guard<std::mutex> lk(m_);
    if (!upsampleActive_.load() || rate_ == 0) return 0.0;
    return 1000.0 * upsampler_.delayFrames() / static_cast<double>(rate_);
}

std::string FilePlayer::upsampleReason() const {
    std::lock_guard<std::mutex> lk(m_);
    return upsampleReason_;
}

} // namespace ee_analyzer::player
