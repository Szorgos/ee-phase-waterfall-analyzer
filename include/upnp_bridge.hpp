#pragma once
/**
 * @file upnp_bridge.hpp
 * @brief UPnP / DLNA lejátszó (MediaRenderer:1) a motorban: a renderer és a beépített lejátszó összekötése, be- és
 *        kikapcsolás az audio.json beállításai szerint, állapot a felületnek. A Linuxos, a Windows-os és a macOS-es
 *        motor közös kódja (mint a radio_bridge.hpp).
 *
 * Részek: upnp_ssdp.hpp (felfedezés), upnp_http.hpp (leírás, SOAP, GENA), upnp_renderer.hpp (állapotgép),
 * http_track_decoder.hpp (a szám letöltése a RAM-ba). A hang a FilePlayer-en át a teljes láncon szól (EQ,
 * felmintavételezés, kizárólagos / bitpontos kimenet, DoP / natív DSD); a szünetmentes váltás a lejátszási sor
 * NextProvider-én át (PlayQueue::setForeignNext). Alapból kikapcsolva; a beállítás: upnp, upnpName, upnpFixedVolume,
 * upnpAllow (audio.json).
 */
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "file_player.hpp"
#include "mdns_announcer.hpp"
#include "play_queue.hpp"
#include "radio_stream.hpp"
#include "upnp_http.hpp"
#include "upnp_renderer.hpp"

namespace ee_analyzer::upnp {

/** A FilePlayer a renderer felé. A hangerő a motor kimeneti hangereje (a „most szól” csúszkája). */
class FilePlayerPort final : public PlayerPort {
public:
    using SetVolume = std::function<void(double level, int mute)>;
    using GetVolume = std::function<bool(double& level, int& muted)>;
    FilePlayerPort(player::FilePlayer& fp, SetVolume sv, GetVolume gv)
        : fp_(fp), setVol_(std::move(sv)), getVol_(std::move(gv)) {}

    bool openFile(const std::string& path, uint64_t token, std::string& err) override {
        return fp_.open(path, &err, token);
    }
    bool openLive(const std::string& url, const radio::NetOptions& net, const std::string& title, uint64_t token,
                  std::string& err, const std::atomic<bool>* cancel) override {
        radio::RadioDecoder::Config c;
        c.url = url;
        c.station = title;
        c.net = net;
        auto dec = radio::RadioDecoder::open(c, err, cancel);
        if (!dec) return false;
        return fp_.openStream(std::move(dec), title, &err, token);
    }
    void play() override { fp_.play(); }
    void pause() override { fp_.pause(); }
    void stop() override { fp_.stop(); }
    void seek(double sec) override { fp_.seek(sec); }
    St state() const override {
        switch (fp_.state()) {
        case player::FilePlayer::State::Playing: return St::Playing;
        case player::FilePlayer::State::Paused: return St::Paused;
        case player::FilePlayer::State::Ended: return St::Ended;
        default: return St::Idle;
        }
    }
    uint64_t currentToken() const override { return fp_.hasTrack() ? fp_.currentToken() : 0; }
    double position() const override { return fp_.position(); }
    double duration() const override { return fp_.duration(); }
    void dropPrefetched() override { fp_.dropPrefetched(); }
    void setVolume(int percent, int mute) override {
        if (setVol_) setVol_(percent < 0 ? -1.0 : percent / 100.0, mute);
    }
    bool getVolume(int& percent, bool& muted) const override {
        double l = 1.0;
        int m = 0;
        if (!getVol_ || !getVol_(l, m)) return false;
        percent = static_cast<int>(std::lround(std::clamp(l, 0.0, 1.0) * 100.0));
        muted = m != 0;
        return true;
    }

private:
    player::FilePlayer& fp_;
    SetVolume setVol_;
    GetVolume getVol_;
};

/** A „vezérlők címe” beállítás (vesszővel / szóközzel elválasztott IPv4-címek) bontása; a hibás elem kimarad. */
inline std::vector<uint32_t> parseAllowList(const std::string& s) {
    std::vector<uint32_t> out;
    std::string cur;
    auto flush = [&] {
        uint32_t a;
        if (!cur.empty() && parseV4(cur, a) && out.size() < 32) out.push_back(a);
        cur.clear();
    };
    for (char c : s) {
        if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\n') flush();
        else cur += c;
    }
    flush();
    return out;
}

/** Az alapnév: „Aqulify (gépnév)”. */
inline std::string defaultRendererName() { return "Aqulify (" + MdnsAnnouncer::computerName() + ")"; }

class UpnpService {
public:
    UpnpService(player::FilePlayer& fp, player::PlayQueue& queue, FilePlayerPort::SetVolume sv, FilePlayerPort::GetVolume gv)
        : queue_(queue), port_(fp, std::move(sv), std::move(gv)), renderer_(port_), server_(renderer_) {
        queue_.setForeignNext([this](uint64_t after, std::string& path, uint64_t& token) {
            return renderer_.nextFor(after, path, token);
        });
    }
    ~UpnpService() {
        queue_.setForeignNext(nullptr);
        apply(false, "", false, "");
    }
    UpnpService(const UpnpService&) = delete;
    UpnpService& operator=(const UpnpService&) = delete;

    /** A beállítás érvényesítése (be / ki, név, fix hangerő, engedélyezett vezérlők); a változatlan beállítás nem indít újra. */
    void apply(bool on, const std::string& name, bool fixedVolume, const std::string& allow) {
        std::lock_guard<std::mutex> lk(m_);
        renderer_.setFixedVolume(fixedVolume);
        const std::string nm = name.empty() ? defaultRendererName() : name;
        if (!on) {
            if (state_ != "off") {
                server_.stop();
                renderer_.reset();
                std::cout << "[UPnP] Leállt.\n";
            }
            state_ = "off";
            error_.clear();
            return;
        }
        if (state_ == "running" && nm == name_ && allow == allow_) return;
        if (state_ == "off") HttpTrack::cleanupStale();
        name_ = nm;
        allow_ = allow;
        UpnpServer::Config c;
        c.name = nm;
        c.uuid = stableUuid(MdnsAnnouncer::computerName());
        c.allow = parseAllowList(allow);
        std::string err;
        if (server_.start(c, err)) {
            state_ = "running";
            error_.clear();
            std::cout << "[UPnP] Fut: \"" << nm << "\", HTTP-port " << server_.port() << ".\n";
        } else {
            state_ = "failed";
            error_ = err;
            std::cout << "[UPnP] Nem indult: " << err << "\n";
        }
    }

    /** Az audio_opts válasz "upnpState" mezője. */
    std::string statusJson() {
        std::lock_guard<std::mutex> lk(m_);
        std::string s = "{\"state\":\"" + state_ + "\"";
        if (state_ != "off") {
            s += ",\"name\":\"" + media::jsonEscape(name_) + "\",\"port\":" + std::to_string(server_.port()) +
                 ",\"ssdp\":" + (server_.ssdpJoined() ? "true" : "false") + ",\"subs\":" + std::to_string(server_.subscriptions());
            std::string addrs;
            for (const auto& f : privateIfaces()) addrs += (addrs.empty() ? "\"" : ",\"") + v4Text(f.ip) + "\"";
            s += ",\"addresses\":[" + addrs + "]";
        }
        if (!error_.empty()) s += ",\"error\":\"" + media::jsonEscape(error_) + "\"";
        return s + ",\"renderer\":" + renderer_.statusJson() + "}";
    }

    /** A „most szól” kiegészítése a DIDL-Lite adataival (ha a renderer száma szól). */
    void patchInfo(media::Info& m) { renderer_.patchInfo(m); }
    Renderer& renderer() { return renderer_; }

private:
    player::PlayQueue& queue_;
    FilePlayerPort port_;
    Renderer renderer_;
    UpnpServer server_;
    std::mutex m_;
    std::string state_ = "off", error_, name_, allow_;
};

} // namespace ee_analyzer::upnp
