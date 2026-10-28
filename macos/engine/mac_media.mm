/**
 * @file mac_media.mm
 * @brief MediaRemote: a „most szól” adatok lekérdezése másodpercenként, és a lejátszásvezérlő parancsok.
 */
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <dlfcn.h>

#include <mutex>

#include "mac_media.hpp"

namespace ee_analyzer::media {

namespace {

typedef void (*MRGetInfoFn)(dispatch_queue_t, void (^)(CFDictionaryRef));
typedef void (*MRGetPidFn)(dispatch_queue_t, void (^)(int));
typedef void (*MRGetIsPlayingFn)(dispatch_queue_t, void (^)(Boolean));
typedef Boolean (*MRSendCommandFn)(int, CFDictionaryRef);
typedef void (*MRSetElapsedFn)(double);

// MRMediaRemoteCommand
constexpr int kMRPlay = 0, kMRPause = 1, kMRTogglePlayPause = 2, kMRNextTrack = 4, kMRPreviousTrack = 5;

struct Api {
    MRGetInfoFn getInfo = nullptr;
    MRGetPidFn getPid = nullptr;
    MRGetIsPlayingFn getIsPlaying = nullptr;
    MRSendCommandFn sendCommand = nullptr;
    MRSetElapsedFn setElapsed = nullptr;
    CFStringRef kTitle = nullptr, kArtist = nullptr, kAlbum = nullptr, kDuration = nullptr, kElapsed = nullptr,
                kTimestamp = nullptr, kRate = nullptr, kArtData = nullptr, kArtMime = nullptr;

    bool load() {
        void* h = dlopen("/System/Library/PrivateFrameworks/MediaRemote.framework/MediaRemote", RTLD_LAZY);
        if (!h) return false;
        getInfo = reinterpret_cast<MRGetInfoFn>(dlsym(h, "MRMediaRemoteGetNowPlayingInfo"));
        getPid = reinterpret_cast<MRGetPidFn>(dlsym(h, "MRMediaRemoteGetNowPlayingApplicationPID"));
        getIsPlaying = reinterpret_cast<MRGetIsPlayingFn>(dlsym(h, "MRMediaRemoteGetNowPlayingApplicationIsPlaying"));
        sendCommand = reinterpret_cast<MRSendCommandFn>(dlsym(h, "MRMediaRemoteSendCommand"));
        setElapsed = reinterpret_cast<MRSetElapsedFn>(dlsym(h, "MRMediaRemoteSetElapsedTime"));
        auto key = [h](const char* name) -> CFStringRef {
            auto* p = reinterpret_cast<CFStringRef*>(dlsym(h, name));
            return p ? *p : nullptr;
        };
        kTitle = key("kMRMediaRemoteNowPlayingInfoTitle");
        kArtist = key("kMRMediaRemoteNowPlayingInfoArtist");
        kAlbum = key("kMRMediaRemoteNowPlayingInfoAlbum");
        kDuration = key("kMRMediaRemoteNowPlayingInfoDuration");
        kElapsed = key("kMRMediaRemoteNowPlayingInfoElapsedTime");
        kTimestamp = key("kMRMediaRemoteNowPlayingInfoTimestamp");
        kRate = key("kMRMediaRemoteNowPlayingInfoPlaybackRate");
        kArtData = key("kMRMediaRemoteNowPlayingInfoArtworkData");
        kArtMime = key("kMRMediaRemoteNowPlayingInfoArtworkMIMEType");
        return getInfo && sendCommand && kTitle;
    }
};

std::string str(NSDictionary* d, CFStringRef key) {
    if (!key) return {};
    id v = d[(__bridge NSString*)key];
    return [v isKindOfClass:[NSString class]] ? std::string([(NSString*)v UTF8String] ?: "") : std::string();
}

double num(NSDictionary* d, CFStringRef key, double def) {
    if (!key) return def;
    id v = d[(__bridge NSString*)key];
    return [v isKindOfClass:[NSNumber class]] ? [(NSNumber*)v doubleValue] : def;
}

} // namespace

struct MacMediaSession::Impl {
    Api api;
    bool loaded = false;
    dispatch_queue_t queue = nullptr;
    dispatch_source_t timer = nullptr;
    mutable std::mutex mutex;
    Info info;
    std::string artBytes, artMime, artKey;

    void refresh() {
        // A válaszok a sorban érkeznek: előbb a „most szól” adatok, utána a lejátszó folyamatazonosítója
        Impl* impl = this;
        api.getInfo(queue, ^(CFDictionaryRef d) {
            NSDictionary* dict = d ? [(__bridge NSDictionary*)d copy] : nil;
            if (!impl->api.getPid) {
                impl->apply(dict, 0);
                return;
            }
            impl->api.getPid(impl->queue, ^(int pid) { impl->apply(dict, pid); });
        });
    }

    void apply(NSDictionary* d, int pid) {
        Info m;
        const std::string title = d ? str(d, api.kTitle) : std::string();
        if (d && (!title.empty() || pid > 0)) {
            m.available = true;
            m.title = title;
            m.artist = str(d, api.kArtist);
            m.album = str(d, api.kAlbum);
            m.duration = num(d, api.kDuration, 0.0);
            const double rate = num(d, api.kRate, 0.0);
            m.playing = rate > 0.0;
            double pos = num(d, api.kElapsed, 0.0);
            id ts = api.kTimestamp ? d[(__bridge NSString*)api.kTimestamp] : nil;
            if (m.playing && [ts isKindOfClass:[NSDate class]]) pos += -[(NSDate*)ts timeIntervalSinceNow] * rate;
            m.position = pos;
            m.positionAt = std::chrono::steady_clock::now();
            m.canPlay = m.canPause = m.canNext = m.canPrevious = true;
            m.canSeek = m.duration > 0.0 && api.setElapsed != nullptr;
            if (pid > 0) {
                NSRunningApplication* app = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
                const std::string bundle = app.bundleIdentifier ? app.bundleIdentifier.UTF8String : "";
                m.app = knownApp(bundle);
                if (m.app.empty() && app.localizedName) m.app = app.localizedName.UTF8String;
            }
            id art = api.kArtData ? d[(__bridge NSString*)api.kArtData] : nil;
            std::string bytes, mime;
            if ([art isKindOfClass:[NSData class]] && [(NSData*)art length] > 0 && [(NSData*)art length] <= (2u << 20)) {
                NSData* data = art;
                bytes.assign(static_cast<const char*>(data.bytes), data.length);
                mime = str(d, api.kArtMime);
                if (mime.empty()) mime = "image/jpeg";
                m.artId = hashId(m.app + "\n" + m.title + "\n" + m.artist + "\n" + m.album + "\n" + std::to_string(bytes.size()));
            }
            std::lock_guard<std::mutex> lk(mutex);
            info = m;
            artBytes = std::move(bytes);
            artMime = mime;
            return;
        }
        std::lock_guard<std::mutex> lk(mutex);
        info = Info{};
        artBytes.clear();
    }
};

MacMediaSession::MacMediaSession() : p_(std::make_unique<Impl>()) {}

MacMediaSession::~MacMediaSession() { stop(); }

void MacMediaSession::start() {
    Impl* p = p_.get();
    if (p->timer) return;
    p->loaded = p->api.load();
    if (!p->loaded) {
        NSLog(@"[Média] A MediaRemote nem érhető el, a „most szól” nem működik.");
        return;
    }
    p->queue = dispatch_queue_create("io.github.szorgos.aqulify.media", DISPATCH_QUEUE_SERIAL);
    p->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, p->queue);
    dispatch_source_set_timer(p->timer, dispatch_time(DISPATCH_TIME_NOW, 0), NSEC_PER_SEC, NSEC_PER_SEC / 10);
    dispatch_source_set_event_handler(p->timer, ^{ p->refresh(); });
    dispatch_resume(p->timer);
}

void MacMediaSession::stop() {
    Impl* p = p_.get();
    if (!p->timer) return;
    dispatch_source_cancel(p->timer);
    p->timer = nullptr;
    dispatch_sync(p->queue, ^{});   // a futó lekérdezés befejeződik
}

Info MacMediaSession::snapshot() const {
    std::lock_guard<std::mutex> lk(p_->mutex);
    return p_->info;
}

void MacMediaSession::command(const std::string& action, double value) {
    Impl* p = p_.get();
    if (!p->loaded || !p->queue) return;
    const std::string a = action;
    dispatch_async(p->queue, ^{
        if (a == "play") p->api.sendCommand(kMRPlay, nullptr);
        else if (a == "pause") p->api.sendCommand(kMRPause, nullptr);
        else if (a == "toggle") p->api.sendCommand(kMRTogglePlayPause, nullptr);
        else if (a == "next") p->api.sendCommand(kMRNextTrack, nullptr);
        else if (a == "previous") p->api.sendCommand(kMRPreviousTrack, nullptr);
        else if (a == "seek" && p->api.setElapsed) p->api.setElapsed(value > 0.0 ? value : 0.0);
        p->refresh();   // a felület gyorsan lássa a változást
    });
}

bool MacMediaSession::art(const std::string& id, std::string& mime, std::string& bytes, std::string& /*url*/) const {
    std::lock_guard<std::mutex> lk(p_->mutex);
    if (id.empty() || id != p_->info.artId || p_->artBytes.empty()) return false;
    bytes = p_->artBytes;
    mime = p_->artMime;
    return true;
}

} // namespace ee_analyzer::media
