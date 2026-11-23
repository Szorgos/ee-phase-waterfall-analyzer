#pragma once
/**
 * @file http_track_decoder.hpp
 * @brief Véges hosszú hálózati szám (UPnP-renderer) letöltése a memóriába, a rádió szállításán (radio_stream.hpp:
 *        curl / WinHTTP) és címszabályán át.
 *
 * Miért letöltés és nem folyam: a meglévő dekóderek (FLAC, WAV, AIFF, ALAC, MP3, Ogg, Opus, WavPack, DSF, DFF)
 * fájlból olvasnak és tekernek, és a natív DSD (DoP / ALSA DSD / ASIO) is fájlból nyitja a nyers bitfolyamot. Ha a
 * szám a RAM-ba (Linuxon a futásidejű mappába, mint a FilePlayer előbetöltése) kerül, a FilePlayer ugyanúgy nyitja
 * meg, mint egy helyi fájlt: a teljes DSP-lánc, a felmintavételezés, a kizárólagos / bitpontos kimenet, a DoP és a
 * szünetmentes váltás változtatás nélkül működik, a tekerés pedig mintapontos és nem vár a hálózatra.
 *
 * HTTP Range (RFC 7233): ha a kapcsolat a letöltés közben megszakad, a folytatás "Range: bytes=N-" kéréssel onnan
 * indul, ahol abbamaradt (legfeljebb kMaxResumes alkalommal); 206 válasznál a Content-Range kezdete ellenőrzött, 200
 * válasznál (a szerver nem tud Range-et) a letöltés elölről kezdődik. Átirányítást a szállítás nem követ: itt minden
 * lépésnél újra ellenőrződik a cím (legfeljebb 5 lépés).
 *
 * Korlátok: a fájl legfeljebb Config::maxBytes (alapból 2 GiB); ha a futásidejű mappában nincs elég hely, a rendszer
 * ideiglenes mappája; ha ott sincs, "space" hiba. Content-Length nélküli válasz (élő folyam): State::Live, a hívó
 * rádióként játssza le (RadioDecoder), kivéve ha a hívó szerint véges (finite), akkor a vége a kapcsolat vége.
 */
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "radio_stream.hpp"

namespace ee_analyzer::upnp {

class HttpTrack {
public:
    enum class State { Loading, Ready, Live, Failed };

    struct Config {
        std::string url;
        radio::NetOptions net;       // a címszabály (allowAddr) és az időkorlátok
        std::string mimeHint;        // a DIDL-Lite protocolInfo MIME-típusa (üres: ismeretlen)
        std::string title;           // a fájl neve ebből lesz (a lejátszó címke nélkül ezt mutatja)
        bool finite = false;         // Content-Length nélkül is véges (a kapcsolat vége a fájl vége)
        uint64_t maxBytes = 2ull << 30;
    };
    static constexpr int kMaxResumes = 5;

    /** A letöltés indítása saját szálon. */
    static std::shared_ptr<HttpTrack> start(Config c);
    ~HttpTrack();
    HttpTrack(const HttpTrack&) = delete;
    HttpTrack& operator=(const HttpTrack&) = delete;

    State state() const;
    /** A hiba kódja (Failed): "dns", "blocked", "http", "size", "space", "format", "io", "aborted", … */
    std::string error() const;
    /** A kész fájl útvonala (UTF-8; Ready állapotban). */
    std::string path() const;
    const std::string& url() const { return cfg_.url; }
    uint64_t bytesDone() const { return done_.load(); }
    uint64_t bytesTotal() const { return total_.load(); }
    /** Várakozás, amíg a letöltés véget ér (Ready / Live / Failed); igaz, ha véget ért. */
    bool wait(int ms) const;
    /** A letöltés megszakítása (bármely szálról); a fájl a megszűnéskor törlődik. */
    void cancel();

    /** Az ideiglenes mappa (futásidejű / rendszer) régi, előző futásból maradt fájljainak törlése. */
    static void cleanupStale();
    /** A fájlnév a címből: tiltott karakterek nélkül, legfeljebb 80 bájt; üres: "UPnP". */
    static std::string safeFileName(const std::string& title);

private:
    explicit HttpTrack(Config c) : cfg_(std::move(c)) {}
    void run();
    void finish(State s, const std::string& err);

    Config cfg_;
    mutable std::mutex m_;
    mutable std::condition_variable cv_;
    State state_ = State::Loading;
    std::string err_, path_, dir_;
    std::atomic<uint64_t> done_{0}, total_{0};
    std::atomic<bool> cancel_{false};
    radio::HttpStream* conn_ = nullptr;   // a nyitott kapcsolat (a megszakításhoz), m_ alatt
    std::thread th_;
};

} // namespace ee_analyzer::upnp
