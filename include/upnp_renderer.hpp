#pragma once
/**
 * @file upnp_renderer.hpp
 * @brief UPnP MediaRenderer:1 állapotgépe: AVTransport:1, RenderingControl:1 és ConnectionManager:1 műveletei,
 *        leképezve a beépített lejátszóra (FilePlayer + lejátszási sor) egy kis felületen (PlayerPort) át.
 *
 * Források: AVTransport:1 Service Template 1.01 (2.2 állapotváltozók, 2.3 a TransportState átmenetei, 2.4 a
 * műveletek és hibakódjaik), RenderingControl:1 Service Template 1.01 (ListPresets, SelectPreset, Get/SetVolume,
 * Get/SetMute), ConnectionManager:1 Service Template 1.01 (GetProtocolInfo, GetCurrentConnectionIDs,
 * GetCurrentConnectionInfo).
 *
 * Állapotok: NO_MEDIA_PRESENT, STOPPED, PLAYING, PAUSED_PLAYBACK, TRANSITIONING (a szám letöltése / megnyitása alatt).
 *  - SetAVTransportURI: a cím ellenőrzése (upnp_soap.hpp checkMediaUrl: a kérés interfészének alhálózata), a
 *    letöltés azonnal indul (HttpTrack), így a Play már kész fájlt talál. Ha előtte szólt, az új szám is szól.
 *  - Play: a kész fájl a FilePlayer-be kerül (saját jellel, kOwnTokenBase fölött), a teljes DSP-láncon át szól.
 *  - SetNextAVTransportURI: a következő szám is letöltődik; ha kész, a lejátszó a mostani szám végén a NextProvider-en
 *    (nextFor) át szünetmentesen folytatja (azonos mintavételnél mintapontosan). Ha a szám vége előbb jön el, mint a
 *    letöltés vége, a váltás a letöltés után, rövid szünettel történik. A váltás után az AVTransportURI a következő
 *    szám címe lesz (2.2.2 / 2.2.3).
 *  - Seek: REL_TIME / ABS_TIME (egy szám: a kettő ugyanaz) és TRACK_NR=1; más mód 710. Élő folyamnál 710.
 *  - Élő folyam (Content-Length nélkül, vagy upnp:class audioBroadcast): a rádió dekóderével szól, nem tekerhető.
 *  - Ha a lejátszón közben más szól (a felület, a sor, a rádió), a renderer STOPPED-be lép.
 *
 * Hangerő: a motor kimeneti hangereje (a „most szól” sáv csúszkája: kizárólagos módban a lánc végén digitálisan, a
 * ditherrel; megosztott módban a rendszeré). Fix hangerő (bitpontos): a SetVolume / SetMute hatástalan, a vezérlő
 * 100-at és némítatlant lát.
 *
 * Zárak: a lejátszó a következő számot a saját zárja alatt kéri (nextFor), ezért a renderer a saját zárja alatt soha
 * nem hívja a lejátszót (a sorrend: lejátszó, utána a renderer). Az események (LastChange) a renderer szálán,
 * legfeljebb 5-ször másodpercenként (AVTransport:1 2.2.1: moderált esemény, 0,2 s).
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "http_track_decoder.hpp"
#include "media_session.hpp"
#include "upnp_soap.hpp"

namespace ee_analyzer::upnp {

enum class Transport { NoMedia, Stopped, Playing, Paused, Transitioning };

inline const char* transportName(Transport t) {
    switch (t) {
    case Transport::Stopped: return "STOPPED";
    case Transport::Playing: return "PLAYING";
    case Transport::Paused: return "PAUSED_PLAYBACK";
    case Transport::Transitioning: return "TRANSITIONING";
    default: return "NO_MEDIA_PRESENT";
    }
}

/** A lejátszó felé (a motorban a FilePlayer + PlayQueue; a tesztekben hamis). A renderer a saját zárja nélkül hívja. */
class PlayerPort {
public:
    enum class St { Idle, Playing, Paused, Ended };
    virtual ~PlayerPort() = default;
    virtual bool openFile(const std::string& path, uint64_t token, std::string& err) = 0;
    /** Élő folyam (rádió): blokkolhat (kapcsolódás); cancel igazra váltásakor megszakad. */
    virtual bool openLive(const std::string& url, const radio::NetOptions& net, const std::string& title, uint64_t token,
                          std::string& err, const std::atomic<bool>* cancel) = 0;
    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void stop() = 0;
    virtual void seek(double sec) = 0;
    virtual St state() const = 0;
    virtual uint64_t currentToken() const = 0;
    virtual double position() const = 0;
    virtual double duration() const = 0;
    /** A sor változott: a lejátszó a mostani szám végén újra kérdez (FilePlayer::dropPrefetched). */
    virtual void dropPrefetched() = 0;
    /** Hangerő 0–100 (< 0: nem változik), némítás 0 / 1 (< 0: nem változik). */
    virtual void setVolume(int percent, int mute) = 0;
    virtual bool getVolume(int& percent, bool& muted) const = 0;
};

class Renderer {
public:
    using Args = std::vector<std::pair<std::string, std::string>>;
    /** service: 0 = AVTransport, 1 = RenderingControl; xml: a LastChange értéke (escape nélkül). */
    using EventSink = std::function<void(int service, const std::string& xml)>;
    using Fetch = std::function<std::shared_ptr<HttpTrack>(const HttpTrack::Config&)>;

    /** A renderer jelei a lejátszóban: ettől fölfelé (a lejátszási sor jelei kicsik, 1-től számolnak). */
    static constexpr uint64_t kOwnTokenBase = 1ull << 62;
    static bool isOwnToken(uint64_t t) { return t >= kOwnTokenBase; }

    explicit Renderer(PlayerPort& p, Fetch fetch = nullptr) : p_(p), fetch_(std::move(fetch)) {
        if (!fetch_) fetch_ = [](const HttpTrack::Config& c) { return HttpTrack::start(c); };
        int v = 100;
        bool mu = false;
        if (p_.getVolume(v, mu)) { volume_ = std::clamp(v, 0, 100); mute_ = mu; }
        th_ = std::thread([this] { loop(); });
    }
    ~Renderer() {
        {
            std::lock_guard<std::mutex> lk(m_);
            quit_ = true;
            cancelOpen_ = true;
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void setEventSink(EventSink s) { std::lock_guard<std::mutex> lk(m_); sink_ = std::move(s); }
    void setFixedVolume(bool on) { std::lock_guard<std::mutex> lk(m_); fixed_ = on; }
    bool fixedVolume() const { std::lock_guard<std::mutex> lk(m_); return fixed_; }

    /** A lejátszás leállítása (ha a renderer szól) és az adathordozó elengedése (a szolgáltatás kikapcsolásakor). */
    void reset() {
        bool stopPlayer;
        std::shared_ptr<HttpTrack> a, b;
        {
            std::lock_guard<std::mutex> lk(m_);
            stopPlayer = opened_;
            ++gen_;
            cancelOpen_ = true;
            a = cur_.dl;
            b = next_.dl;
            cur_ = Slot{};
            next_ = Slot{};
            retired_.clear();
            opened_ = wantPlay_ = false;
            ts_ = Transport::NoMedia;
            status_ = "OK";
            pendingSeek_ = -1;
        }
        if (stopPlayer && isOwnToken(p_.currentToken())) p_.stop();
        if (a) a->cancel();
        if (b) b->cancel();
    }

    // ------------------------------------------------------------------ SOAP

    /**
     * Egy SOAP-művelet végrehajtása; 0: siker (out: a kimenő argumentumok), különben a UPnPError kódja.
     * via: az interfész, amelyen a kérés jött (a címszabályhoz); peer: a vezérlő címe (a felületnek).
     */
    int handle(const SoapRequest& rq, const Iface& via, const std::string& peer, Args& out) {
        const std::string& a = rq.action;
        {
            std::lock_guard<std::mutex> lk(m_);
            lastPeer_ = peer;
            lastAction_ = a;
        }
        if (rq.serviceType.rfind("urn:schemas-upnp-org:service:AVTransport:", 0) == 0) return avt(rq, via, out);
        if (rq.serviceType.rfind("urn:schemas-upnp-org:service:RenderingControl:", 0) == 0) return rcs(rq, out);
        if (rq.serviceType.rfind("urn:schemas-upnp-org:service:ConnectionManager:", 0) == 0) return cm(rq, out);
        return 401;
    }

    /** A lejátszó kérdése a szám végén (a lejátszó zárja alatt): a kész következő szám útvonala és jele. */
    bool nextFor(uint64_t after, std::string& path, uint64_t& token) {
        std::lock_guard<std::mutex> lk(m_);
        if (!isOwnToken(after) || after != cur_.token || next_.uri.empty() || next_.live || !next_.dl) return false;
        if (next_.dl->state() != HttpTrack::State::Ready) return false;
        path = next_.dl->path();
        token = next_.token;
        next_.offered = true;
        return true;
    }

    /** A teljes LastChange (új feliratkozónak): service 0 = AVTransport, 1 = RenderingControl. */
    std::string fullLastChange(int service) {
        std::lock_guard<std::mutex> lk(m_);
        return service == 0 ? lastChangeXml(kAvtEventNs, avtVarsLocked()) : lastChangeXml(kRcsEventNs, rcsVarsLocked());
    }

    /** A „most szól” kiegészítése a DIDL-Lite adataival, ha a renderer száma szól és a fájlban nincs ilyen címke. */
    void patchInfo(media::Info& m) {
        const uint64_t tok = p_.currentToken();
        std::lock_guard<std::mutex> lk(m_);
        if (!isOwnToken(tok) || tok != cur_.token || !m.available) return;
        if (!cur_.didl.title.empty() && (m.title.empty() || m.live)) m.title = cur_.didl.title;
        if (m.artist.empty()) m.artist = cur_.didl.artist;
        if (m.album.empty()) m.album = cur_.didl.album;
        m.app = "UPnP";
    }

    /** A felület állapotmezője: {state, title, artist, peer, action, error, loaded, total}. */
    std::string statusJson() const {
        std::lock_guard<std::mutex> lk(m_);
        std::string s = std::string("{\"state\":\"") + transportName(ts_) + "\"";
        if (!cur_.uri.empty()) {
            s += ",\"title\":\"" + media::jsonEscape(cur_.didl.title) + "\",\"artist\":\"" + media::jsonEscape(cur_.didl.artist) + "\"";
            if (cur_.dl && ts_ == Transport::Transitioning)
                s += ",\"loaded\":" + std::to_string(cur_.dl->bytesDone()) + ",\"total\":" + std::to_string(cur_.dl->bytesTotal());
            if (cur_.live) s += ",\"live\":true";
        }
        if (!lastPeer_.empty()) s += ",\"peer\":\"" + media::jsonEscape(lastPeer_) + "\",\"action\":\"" + media::jsonEscape(lastAction_) + "\"";
        if (!lastError_.empty()) s += ",\"error\":\"" + media::jsonEscape(lastError_) + "\"";
        return s + "}";
    }

    Transport transport() const { std::lock_guard<std::mutex> lk(m_); return ts_; }

private:
    struct Slot {
        std::string uri, metaXml, mime;
        DidlItem didl;
        std::shared_ptr<HttpTrack> dl;
        uint64_t token = 0;
        bool live = false;       // élő folyam (rádióként szól)
        bool offered = false;    // a lejátszó már megkapta következőként
        Iface via;
        double didlDuration() const {
            const DidlRes* r = didlResFor(didl, uri);
            return r ? r->duration : 0.0;
        }
    };
    enum class Op { None, Play, Pause, Stop, Seek, Drop };

    static bool validInstance(const SoapRequest& rq, int& err) {
        if (!rq.has("InstanceID")) { err = 402; return false; }
        if (trim(rq.arg("InstanceID")) != "0") { err = 718; return false; }
        return true;
    }

    radio::NetOptions netFor(const Iface& via) const {
        radio::NetOptions n;
        n.userAgent = "Aqulify UPnP";
        n.allowAddr = [via](int fam, const uint8_t* addr) { return mediaAddrAllowed(fam, addr, via); };
        return n;
    }

    /** Új adathordozó a címből (ellenőrzés, a letöltés indítása); 0: rendben, különben a hibakód. */
    int makeSlot(const std::string& uri, const std::string& meta, const Iface& via, Slot& s) {
        radio::RadioUrl u;
        const std::string e = checkMediaUrl(uri, via, u);
        if (!e.empty()) {
            std::lock_guard<std::mutex> lk(m_);
            lastError_ = "url:" + e;
            return 716;
        }
        s.uri = uri;
        s.metaXml = meta.size() <= kMaxArgBytes ? meta : std::string();
        s.via = via;
        parseDidl(s.metaXml, s.didl);   // hiányzó vagy hibás metaadat: üres adatok (nem hiba)
        if (const DidlRes* r = didlResFor(s.didl, uri)) s.mime = protocolMime(r->protocolInfo);
        const std::string ext = extForMime(s.mime);
        if (ext == "-" || (!s.mime.empty() && ext.empty() && s.mime.rfind("audio/", 0) != 0 && s.mime != "application/octet-stream")) {
            std::lock_guard<std::mutex> lk(m_);
            lastError_ = "mime:" + s.mime;
            return 714;
        }
        s.live = s.didl.broadcast();
        {
            std::lock_guard<std::mutex> lk(m_);
            s.token = kOwnTokenBase + (++tokenSeq_);
        }
        if (!s.live) {
            HttpTrack::Config c;
            c.url = uri;
            c.net = netFor(via);
            c.mimeHint = s.mime;
            c.title = s.didl.title.empty() ? std::string("UPnP") : s.didl.title;
            c.finite = s.didlDuration() > 0.0;
            s.dl = fetch_(c);
        }
        return 0;
    }

    void retireLocked(Slot&& s) {
        if (s.dl) {
            // az előző szám fájlja egy ideig megmarad: a lejátszó még olvashatja (Windowson a nyitott fájl nem törölhető)
            retired_.push_back(std::move(s.dl));
            while (retired_.size() > 1) retired_.pop_front();
        }
    }

    /** A lejátszó állapota (a renderer zárja nélkül olvasva). */
    struct Snap {
        uint64_t token = 0;
        PlayerPort::St st = PlayerPort::St::Idle;
        double pos = 0, dur = 0;
    };
    Snap snap() const { return Snap{ p_.currentToken(), p_.state(), p_.position(), p_.duration() }; }

    void runOp(Op op, double v) {
        switch (op) {
        case Op::Play: p_.play(); break;
        case Op::Pause: p_.pause(); break;
        case Op::Stop: p_.stop(); break;
        case Op::Seek: p_.seek(v); break;
        case Op::Drop: p_.dropPrefetched(); break;
        default: break;
        }
        cv_.notify_all();
    }

    std::string actionsLocked() const {
        const bool seek = !cur_.live;
        const bool nx = !next_.uri.empty();
        switch (ts_) {
        case Transport::Stopped: return std::string("Play") + (seek ? ",Seek" : "") + (nx ? ",Next" : "");
        case Transport::Playing: return std::string("Pause,Stop") + (seek ? ",Seek,Previous" : "") + (nx ? ",Next" : "");
        case Transport::Paused: return std::string("Play,Stop") + (seek ? ",Seek,Previous" : "") + (nx ? ",Next" : "");
        case Transport::Transitioning: return "Stop";
        default: return "";
        }
    }

    double trackDurationLocked(const Snap& s) const {
        if (opened_ && s.token == cur_.token && s.dur > 0) return s.dur;
        return cur_.didlDuration();
    }

    // ------------------------------------------------------------------ AVTransport

    int avt(const SoapRequest& rq, const Iface& via, Args& out) {
        const std::string& a = rq.action;
        int err = 0;
        if (!validInstance(rq, err)) return err;
        if (a == "SetAVTransportURI" || a == "SetNextAVTransportURI") {
            const bool isNext = a == "SetNextAVTransportURI";
            const char* uk = isNext ? "NextURI" : "CurrentURI";
            const char* mk = isNext ? "NextURIMetaData" : "CurrentURIMetaData";
            if (!rq.has(uk)) return 402;
            const std::string uri = trim(rq.arg(uk));
            Slot s;
            if (!uri.empty()) {
                const int e = makeSlot(uri, rq.arg(mk), via, s);
                if (e) return e;
            }
            Op op = Op::None;
            std::shared_ptr<HttpTrack> cancelDl;
            {
                std::lock_guard<std::mutex> lk(m_);
                if (isNext) {
                    if (next_.dl) cancelDl = next_.dl;
                    retireLocked(std::move(next_));
                    next_ = std::move(s);
                    if (opened_) op = Op::Drop;   // a régi előre betöltött következő szám kiesik
                } else {
                    const bool wasPlaying = ts_ == Transport::Playing || ts_ == Transport::Transitioning;
                    if (opened_) op = Op::Stop;
                    ++gen_;
                    cancelOpen_ = true;
                    if (cur_.dl) cancelDl = cur_.dl;
                    retireLocked(std::move(cur_));
                    retireLocked(std::move(next_));
                    next_ = Slot{};
                    cur_ = std::move(s);
                    opened_ = false;
                    pendingSeek_ = -1;
                    status_ = "OK";
                    lastError_.clear();
                    if (cur_.uri.empty()) { ts_ = Transport::NoMedia; wantPlay_ = false; }
                    else { wantPlay_ = wasPlaying; ts_ = wasPlaying ? Transport::Transitioning : Transport::Stopped; }
                }
            }
            if (cancelDl) cancelDl->cancel();
            runOp(op, 0);
            return 0;
        }
        if (a == "Play") {
            const std::string speed = rq.has("Speed") ? trim(rq.arg("Speed")) : "1";
            if (speed != "1") return 717;
            Op op = Op::None;
            {
                std::lock_guard<std::mutex> lk(m_);
                if (ts_ == Transport::NoMedia) return 701;
                if (ts_ == Transport::Paused && opened_) { op = Op::Play; ts_ = Transport::Playing; }
                else if (ts_ == Transport::Stopped) { wantPlay_ = true; ts_ = Transport::Transitioning; status_ = "OK"; }
            }
            runOp(op, 0);
            return 0;
        }
        if (a == "Pause") {
            Op op = Op::None;
            {
                std::lock_guard<std::mutex> lk(m_);
                if (ts_ == Transport::Playing && opened_) { op = Op::Pause; ts_ = Transport::Paused; }
                else if (ts_ != Transport::Paused) return 701;
            }
            runOp(op, 0);
            return 0;
        }
        if (a == "Stop") {
            Op op = Op::None;
            {
                std::lock_guard<std::mutex> lk(m_);
                if (ts_ == Transport::NoMedia) return 0;
                if (opened_) op = Op::Stop;
                ++gen_;
                cancelOpen_ = true;
                opened_ = wantPlay_ = false;
                pendingSeek_ = -1;
                ts_ = Transport::Stopped;
            }
            runOp(op, 0);
            return 0;
        }
        if (a == "Seek") {
            const std::string unit = trim(rq.arg("Unit")), target = trim(rq.arg("Target"));
            if (!rq.has("Unit") || !rq.has("Target")) return 402;
            double sec = 0;
            if (unit == "REL_TIME" || unit == "ABS_TIME") {
                if (!parseTime(target, sec)) return 711;
            } else if (unit == "TRACK_NR") {
                if (target != "1") return 711;
            } else {
                return 710;
            }
            const Snap sn = snap();
            Op op = Op::None;
            {
                std::lock_guard<std::mutex> lk(m_);
                if (ts_ == Transport::NoMedia) return 701;
                if (cur_.live) return 710;
                const double dur = trackDurationLocked(sn);
                if (dur > 0 && sec > dur) return 711;
                if (opened_ && sn.token == cur_.token) op = Op::Seek;
                else pendingSeek_ = sec;
            }
            runOp(op, sec);
            return 0;
        }
        if (a == "Next") {
            Op op = Op::None;
            std::shared_ptr<HttpTrack> cancelDl;
            {
                std::lock_guard<std::mutex> lk(m_);
                if (next_.uri.empty()) return 711;
                const bool wasPlaying = ts_ == Transport::Playing || ts_ == Transport::Transitioning;
                if (opened_) op = Op::Stop;
                ++gen_;
                cancelOpen_ = true;
                if (cur_.dl) cancelDl = cur_.dl;
                retireLocked(std::move(cur_));
                cur_ = std::move(next_);
                next_ = Slot{};
                opened_ = false;
                pendingSeek_ = -1;
                wantPlay_ = wasPlaying;
                ts_ = wasPlaying ? Transport::Transitioning : Transport::Stopped;
            }
            if (cancelDl) cancelDl->cancel();
            runOp(op, 0);
            return 0;
        }
        if (a == "Previous") {
            const Snap sn = snap();
            Op op = Op::None;
            {
                std::lock_guard<std::mutex> lk(m_);
                if (ts_ == Transport::NoMedia) return 701;
                if (cur_.live) return 701;
                if (opened_ && sn.token == cur_.token) op = Op::Seek;
                else pendingSeek_ = 0;
            }
            runOp(op, 0);
            return 0;
        }
        if (a == "SetPlayMode") {
            return trim(rq.arg("NewPlayMode")) == "NORMAL" ? 0 : 712;
        }
        if (a == "GetTransportInfo") {
            std::lock_guard<std::mutex> lk(m_);
            out = { { "CurrentTransportState", transportName(ts_) }, { "CurrentTransportStatus", status_ }, { "CurrentSpeed", "1" } };
            return 0;
        }
        if (a == "GetPositionInfo") {
            const Snap sn = snap();
            std::lock_guard<std::mutex> lk(m_);
            const bool has = !cur_.uri.empty();
            double pos = 0;
            if (opened_ && sn.token == cur_.token) pos = sn.pos;
            else if (pendingSeek_ > 0) pos = pendingSeek_;
            const double dur = trackDurationLocked(sn);
            const std::string rel = formatTime(pos);
            out = { { "Track", has ? "1" : "0" }, { "TrackDuration", formatTime(dur) }, { "TrackMetaData", cur_.metaXml },
                    { "TrackURI", cur_.uri }, { "RelTime", rel }, { "AbsTime", rel },
                    { "RelCount", "2147483647" }, { "AbsCount", "2147483647" } };
            return 0;
        }
        if (a == "GetMediaInfo") {
            const Snap sn = snap();
            std::lock_guard<std::mutex> lk(m_);
            const bool has = !cur_.uri.empty();
            out = { { "NrTracks", has ? "1" : "0" }, { "MediaDuration", formatTime(trackDurationLocked(sn)) },
                    { "CurrentURI", cur_.uri }, { "CurrentURIMetaData", cur_.metaXml },
                    { "NextURI", next_.uri }, { "NextURIMetaData", next_.metaXml },
                    { "PlayMedium", has ? "NETWORK" : "NONE" }, { "RecordMedium", "NOT_IMPLEMENTED" },
                    { "WriteStatus", "NOT_IMPLEMENTED" } };
            return 0;
        }
        if (a == "GetDeviceCapabilities") {
            out = { { "PlayMedia", "NETWORK" }, { "RecMedia", "NOT_IMPLEMENTED" }, { "RecQualityModes", "NOT_IMPLEMENTED" } };
            return 0;
        }
        if (a == "GetTransportSettings") {
            out = { { "PlayMode", "NORMAL" }, { "RecQualityMode", "NOT_IMPLEMENTED" } };
            return 0;
        }
        if (a == "GetCurrentTransportActions") {
            std::lock_guard<std::mutex> lk(m_);
            out = { { "Actions", actionsLocked() } };
            return 0;
        }
        return 401;
    }

    // ------------------------------------------------------------------ RenderingControl

    static bool parseBool(const std::string& v, bool& out) {
        const std::string s = trim(v);
        if (s == "1" || s == "true" || s == "yes") { out = true; return true; }
        if (s == "0" || s == "false" || s == "no") { out = false; return true; }
        return false;
    }

    int rcs(const SoapRequest& rq, Args& out) {
        const std::string& a = rq.action;
        int err = 0;
        if (!validInstance(rq, err)) return err == 718 ? 702 : err;   // RenderingControl:1: 702 Invalid InstanceID
        if (a == "ListPresets") { out = { { "CurrentPresetNameList", "FactoryDefaults" } }; return 0; }
        if (a == "SelectPreset") return trim(rq.arg("PresetName")) == "FactoryDefaults" ? 0 : 701;
        const bool chanOk = trim(rq.arg("Channel")) == "Master";
        if (a == "GetVolume" || a == "GetMute") {
            if (!rq.has("Channel")) return 402;
            if (!chanOk) return 600;
            std::lock_guard<std::mutex> lk(m_);
            if (a == "GetVolume") out = { { "CurrentVolume", std::to_string(fixed_ ? 100 : volume_) } };
            else out = { { "CurrentMute", (!fixed_ && mute_) ? "1" : "0" } };
            return 0;
        }
        if (a == "SetVolume" || a == "SetMute") {
            if (!rq.has("Channel")) return 402;
            if (!chanOk) return 600;
            int vol = -1, mu = -1;
            if (a == "SetVolume") {
                const std::string v = trim(rq.arg("DesiredVolume"));
                if (v.empty() || v.size() > 3 || v.find_first_not_of("0123456789") != std::string::npos) return 600;
                vol = std::stoi(v);
                if (vol > 100) return 601;
            } else {
                bool b;
                if (!parseBool(rq.arg("DesiredMute"), b)) return 600;
                mu = b ? 1 : 0;
            }
            {
                std::lock_guard<std::mutex> lk(m_);
                if (fixed_) return 0;   // fix hangerő (bitpontos): a lánc érintetlen
                if (vol >= 0) volume_ = vol;
                if (mu >= 0) mute_ = mu == 1;
            }
            p_.setVolume(vol, mu);
            cv_.notify_all();
            return 0;
        }
        return 401;
    }

    // ------------------------------------------------------------------ ConnectionManager

    int cm(const SoapRequest& rq, Args& out) {
        const std::string& a = rq.action;
        if (a == "GetProtocolInfo") { out = { { "Source", "" }, { "Sink", sinkProtocolInfo() } }; return 0; }
        if (a == "GetCurrentConnectionIDs") { out = { { "ConnectionIDs", "0" } }; return 0; }
        if (a == "GetCurrentConnectionInfo") {
            if (!rq.has("ConnectionID")) return 402;
            if (trim(rq.arg("ConnectionID")) != "0") return 706;
            std::lock_guard<std::mutex> lk(m_);
            out = { { "RcsID", "0" }, { "AVTransportID", "0" },
                    { "ProtocolInfo", cur_.mime.empty() ? std::string() : "http-get:*:" + cur_.mime + ":*" },
                    { "PeerConnectionManager", "" }, { "PeerConnectionID", "-1" }, { "Direction", "Input" }, { "Status", "OK" } };
            return 0;
        }
        return 401;
    }

    // ------------------------------------------------------------------ események

    std::vector<EventVar> avtVarsLocked() const {
        const bool has = !cur_.uri.empty();
        const double dur = opened_ && lastSnap_.token == cur_.token && lastSnap_.dur > 0 ? lastSnap_.dur : cur_.didlDuration();
        return { { "TransportState", transportName(ts_), "" },
                 { "TransportStatus", status_, "" },
                 { "PlaybackStorageMedium", has ? "NETWORK" : "NONE", "" },
                 { "PossiblePlaybackStorageMedia", "NETWORK", "" },
                 { "CurrentPlayMode", "NORMAL", "" },
                 { "TransportPlaySpeed", "1", "" },
                 { "NumberOfTracks", has ? "1" : "0", "" },
                 { "CurrentTrack", has ? "1" : "0", "" },
                 { "CurrentTrackDuration", formatTime(dur), "" },
                 { "CurrentMediaDuration", formatTime(dur), "" },
                 { "CurrentTrackURI", cur_.uri, "" },
                 { "CurrentTrackMetaData", cur_.metaXml, "" },
                 { "AVTransportURI", cur_.uri, "" },
                 { "AVTransportURIMetaData", cur_.metaXml, "" },
                 { "NextAVTransportURI", next_.uri, "" },
                 { "NextAVTransportURIMetaData", next_.metaXml, "" },
                 { "CurrentTransportActions", actionsLocked(), "" } };
    }
    std::vector<EventVar> rcsVarsLocked() const {
        return { { "PresetNameList", "FactoryDefaults", "" },
                 { "Volume", std::to_string(fixed_ ? 100 : volume_), "Master" },
                 { "Mute", (!fixed_ && mute_) ? "1" : "0", "Master" } };
    }

    /** A változott változók (az előző küldéshez képest); prev frissül. */
    static std::vector<EventVar> diff(const std::vector<EventVar>& now, std::map<std::string, std::string>& prev) {
        std::vector<EventVar> out;
        for (const auto& v : now) {
            const std::string k = v.name + "|" + v.channel;
            auto it = prev.find(k);
            if (it == prev.end() || it->second != v.value) {
                out.push_back(v);
                prev[k] = v.value;
            }
        }
        return out;
    }

    // ------------------------------------------------------------------ a renderer szála

    void loop() {
        std::unique_lock<std::mutex> lk(m_);
        while (!quit_) {
            cv_.wait_for(lk, std::chrono::milliseconds(200));
            if (quit_) break;
            if (opened_) {   // a lejátszót csak akkor kérdezzük, ha a renderer száma szól
                lk.unlock();
                const Snap sn = snap();
                lk.lock();
                lastSnap_ = sn;
            }
            const Snap sn = lastSnap_;
            reconcileLocked(sn, lk);
            if (quit_) break;
            startOpenLocked(lk);
            if (quit_) break;
            // a következő szám letöltése most készült el: a lejátszó újra kérdez (szünetmentes váltás)
            Op op = Op::None;
            if (opened_ && !next_.uri.empty() && !next_.offered && !next_.live && next_.dl &&
                next_.dl->state() == HttpTrack::State::Ready && !nextReadyNotified_) {
                nextReadyNotified_ = true;
                op = Op::Drop;
            }
            if (next_.uri.empty() || next_.offered) nextReadyNotified_ = false;
            // moderált LastChange
            auto av = diff(avtVarsLocked(), avtPrev_);
            auto rc = diff(rcsVarsLocked(), rcsPrev_);
            EventSink sink = sink_;
            lk.unlock();
            if (op == Op::Drop) p_.dropPrefetched();
            if (sink) {
                if (!av.empty()) sink(0, lastChangeXml(kAvtEventNs, av));
                if (!rc.empty()) sink(1, lastChangeXml(kRcsEventNs, rc));
            }
            lk.lock();
        }
    }

    /** A lejátszó állapotának átvétele (szám vége, szünetmentes váltás, más vette át a lejátszót). */
    void reconcileLocked(const Snap& sn, std::unique_lock<std::mutex>&) {
        if (!opened_) return;
        if (!next_.uri.empty() && sn.token == next_.token) {
            // a lejátszó szünetmentesen átlépett a következő számra
            retireLocked(std::move(cur_));
            cur_ = std::move(next_);
            next_ = Slot{};
            ts_ = sn.st == PlayerPort::St::Paused ? Transport::Paused : Transport::Playing;
            return;
        }
        if (sn.token != cur_.token) {
            // más szól a lejátszón (a felület, a sor, a rádió): a renderer leáll
            opened_ = wantPlay_ = false;
            ts_ = cur_.uri.empty() ? Transport::NoMedia : Transport::Stopped;
            return;
        }
        switch (sn.st) {
        case PlayerPort::St::Playing: ts_ = Transport::Playing; break;
        case PlayerPort::St::Paused: ts_ = Transport::Paused; break;
        case PlayerPort::St::Ended:
        case PlayerPort::St::Idle:
            opened_ = false;
            if (sn.st == PlayerPort::St::Ended && !next_.uri.empty()) {
                // a szám véget ért, a következő még nem volt kész (vagy élő folyam): most indul
                retireLocked(std::move(cur_));
                cur_ = std::move(next_);
                next_ = Slot{};
                wantPlay_ = true;
                ts_ = Transport::Transitioning;
            } else {
                wantPlay_ = false;
                ts_ = Transport::Stopped;
            }
            break;
        }
    }

    /** Ha lejátszást kértek és a szám kész: megnyitás a lejátszóban (a zár nélkül). */
    void startOpenLocked(std::unique_lock<std::mutex>& lk) {
        if (!wantPlay_ || opened_ || cur_.uri.empty()) return;
        std::string path;
        bool live = cur_.live;
        if (!live) {
            if (!cur_.dl) return;
            const HttpTrack::State st = cur_.dl->state();
            if (st == HttpTrack::State::Loading) return;
            if (st == HttpTrack::State::Failed) {
                lastError_ = "load:" + cur_.dl->error();
                status_ = "ERROR_OCCURRED";
                wantPlay_ = false;
                ts_ = Transport::Stopped;
                return;
            }
            if (st == HttpTrack::State::Live) { cur_.live = live = true; }
            else path = cur_.dl->path();
        }
        const uint64_t gen = ++gen_;
        cancelOpen_ = false;
        const uint64_t token = cur_.token;
        const std::string uri = cur_.uri;
        const std::string title = cur_.didl.title.empty() ? std::string("UPnP") : cur_.didl.title;
        const radio::NetOptions net = netFor(cur_.via);
        const double seekTo = pendingSeek_;
        pendingSeek_ = -1;
        lk.unlock();
        std::string err;
        bool ok = live ? p_.openLive(uri, net, title, token, err, &cancelOpen_) : p_.openFile(path, token, err);
        if (ok && seekTo > 0 && !live) p_.seek(seekTo);
        lk.lock();
        if (gen != gen_ || quit_) {
            // közben új kérés jött (Stop, új cím): ha a mi számunk szól, leáll
            if (ok) {
                lk.unlock();
                if (p_.currentToken() == token) p_.stop();
                lk.lock();
            }
            return;
        }
        if (!ok) {
            lastError_ = "open:" + err;
            status_ = "ERROR_OCCURRED";
            wantPlay_ = false;
            ts_ = Transport::Stopped;
            return;
        }
        opened_ = true;
        status_ = "OK";
        lastError_.clear();
        ts_ = Transport::Playing;
    }

    PlayerPort& p_;
    Fetch fetch_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::thread th_;
    bool quit_ = false;
    EventSink sink_;

    Slot cur_, next_;
    std::deque<std::shared_ptr<HttpTrack>> retired_;
    Transport ts_ = Transport::NoMedia;
    std::string status_ = "OK";
    bool wantPlay_ = false, opened_ = false;
    bool nextReadyNotified_ = false;
    double pendingSeek_ = -1;
    uint64_t gen_ = 0;
    std::atomic<bool> cancelOpen_{ false };
    uint64_t tokenSeq_ = 0;
    int volume_ = 100;
    bool mute_ = false;
    bool fixed_ = false;
    std::string lastPeer_, lastAction_, lastError_;
    Snap lastSnap_;
    std::map<std::string, std::string> avtPrev_, rcsPrev_;
};

} // namespace ee_analyzer::upnp
