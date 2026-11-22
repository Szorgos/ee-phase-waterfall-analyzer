#pragma once
/**
 * @file radio_stream.hpp
 * @brief Internetrádió a beépített lejátszóban: hálózati folyam pufferrel és újracsatlakozással.
 *
 * Felépítés: a RadioDecoder egy AudioDecoder, így a FilePlayer ugyanúgy kezeli, mint egy fájlt (a teljes EQ/DSP-láncon
 * át szól), csak a hossza ismeretlen és nem tekerhető. Saját szála kapcsolódik, dekódol és egy PCM-pufferbe ír; a
 * FilePlayer read() hívása soha nem vár a hálózatra (a lejátszó zárja alatt fut): ha a puffer kiürült, csendet ad és
 * újrapufferel, 0-t csak a folyam végleges vége (hiba, formátumváltás) után.
 *
 * Szállítás: Linuxon / Pi-n és macOS-en a rendszer curl programja (argumentumtömbbel, héj nélkül, mint a frissítés-
 * ellenőrzés), Windowson a WinHTTP. Átirányítást a szállítás nem követ: a RadioDecoder minden lépésnél újra ellenőrzi
 * a címet (legfeljebb 5 lépés, a lejátszási lista is egy lépés).
 *
 * SSRF-védelem: a cél nevét a motor maga oldja fel, és csak akkor kapcsolódik, ha MINDEN feloldott cím nyilvános
 * (radio_net.hpp: isPublicV4 / isPublicV6); a curl a már ellenőrzött címre kapcsolódik (--resolve), így a névfeloldás
 * a kettő között nem változhat. Windowson a WinHTTP is az ellenőrzött címet kapja (ahol a rendszer ezt támogatja), és
 * a válasz után a ténylegesen használt cím is ellenőrzés alá esik: tiltott címnél a törzs nem olvasódik. Proxy nincs.
 * Időkorlátok: kapcsolódás 10 s, a fejléc 15 s, adat nélkül legfeljebb 20 s. Méretkorlátok: fejléc 16 KB, lejátszási
 * lista 64 KB, ICY-blokk 4080 bájt, PCM-puffer legfeljebb 10 s (a régebbi rész túlcsorduláskor kiesik).
 *
 * Formátumok: MP3, AAC (ADTS; AAC-LC, HE-AAC, HE-AACv2 a FAAD2-vel), Ogg Vorbis, Ogg Opus, FLAC (natív és Ogg; a láncolt
 * Ogg-adásnál számonként új dekóder), ICY metaadat („most szól”), PLS / M3U lista. Nem: HLS (szegmentált folyam, "hls").
 */
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio_decoder.hpp"
#include "radio_net.hpp"

namespace ee_analyzer::radio {

struct NetOptions {
    bool allowLoopback = false;   // csak a tesztekhez (helyi próbaszerver); a magánhálózat így is tiltott
    int connectTimeoutS = 10;
    int headTimeoutS = 15;
    int stallTimeoutS = 20;       // ennyi ideig nem jön adat: a kapcsolat bontása (újracsatlakozás)
    std::string userAgent = "Aqulify";
};

/**
 * A cím feloldása: true, ha legalább egy cím van és mindegyik nyilvános (allowLoopback mellett a loopback is
 * elfogadott). ipOut: az első cím szövegként. err: "dns" vagy "blocked".
 */
bool resolvePublic(const RadioUrl& u, bool allowLoopback, std::string& ipOut, std::string& err);

/** Egy GET-kérés, átirányítás követése nélkül. */
class HttpStream {
public:
    HttpStream();
    ~HttpStream();
    HttpStream(const HttpStream&) = delete;
    HttpStream& operator=(const HttpStream&) = delete;
    /** Kapcsolódás és a válasz fejléce; false: err = "dns", "blocked", "connect", "timeout", "tls", "http", "nocurl", "aborted". */
    bool open(const RadioUrl& u, const NetOptions& o, HttpHead& head, std::string& err);
    /** A törzs következő bájtjai (blokkol); 0: vége, hiba, időtúllépés vagy megszakítás. */
    size_t read(uint8_t* buf, size_t n);
    /** Bármely szálról: a folyamatban lévő open / read azonnal visszatér. */
    void abort();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** Az állomáskereső egy találata (a közösségi katalógusból; a mezők tisztítva, a cím ellenőrizve). */
struct CatalogItem {
    std::string name, url, codec, country, tags;
    int bitrate = 0;
};

/** A közösségi állomáskatalógus (radio-browser.info) címe; a DNS a közösség szerverei közül választ. */
constexpr const char* kCatalogBase = "https://all.api.radio-browser.info";

/**
 * Keresés név szerint a katalógusban (legfeljebb 50 találat, a népszerűség szerint; HLS nélkül). Csak olvas: kattintás-
 * számlálót és más adatot nem küld. A kérés a rádió szállításán megy (SSRF-ellenőrzés, időkorlát), a válasz legfeljebb
 * 1 MB. reg: a nyitott kapcsolat bejegyzése a megszakításhoz (nullptr: vége). baseUrl: más katalógus (a tesztekhez).
 * false: err = "query", "json", "size", vagy a szállítás hibakódja.
 */
bool catalogSearch(const std::string& query, const NetOptions& net, std::vector<CatalogItem>& out, std::string& err,
                   const std::function<void(HttpStream*)>& reg = nullptr, const std::string& baseUrl = "");

enum class RadioState { Connecting, Buffering, Playing, Reconnecting, Ended, Failed };
const char* stateName(RadioState s);

struct RadioStatus {
    RadioState state = RadioState::Connecting;
    std::string error;        // a végleges hiba kódja (Failed), illetve az utolsó újracsatlakozás oka
    std::string codec;        // "mp3", "vorbis", "opus", "flac"
    int bitrate = 0;          // kbit/s az icy-br fejlécből (0: ismeretlen)
    uint64_t reconnects = 0;
    uint64_t droppedFrames = 0;
    uint64_t framesIn = 0;       // a kapcsolat(ok)ból dekódolt összes keret (diagnosztika: a szerver üteme)
    std::string icyName;      // a szerver által küldött állomásnév (csak tájékoztató)
    std::string streamTitle;  // a hallott rész „most szól” szövege
};

class RadioDecoder final : public player::AudioDecoder {
public:
    struct Config {
        std::string url;
        std::string station;          // az állomás neve a felhasználó listájából (a „most szól” albuma)
        NetOptions net;
        double prebufferSec = 2.0;    // ennyi hang gyűlik össze indulás / kiürülés után, mielőtt szól
        double maxBufferSec = 10.0;
        int maxFailures = 8;          // egymás utáni sikertelen újracsatlakozás után végleges hiba
        int backoffBaseMs = 1000;     // az újracsatlakozás várakozása: base, 2×base, … legfeljebb 30 s
    };

    /**
     * Kapcsolódás és az első hang pufferelése. Blokkol (legfeljebb kb. 30 s), ezért nem a lejátszó zárja alatt hívandó.
     * cancel: ha igazra vált, a várakozás megszakad (err = "aborted"). nullptr: hiba (err: a kód).
     */
    static std::unique_ptr<RadioDecoder> open(const Config& c, std::string& err, const std::atomic<bool>* cancel = nullptr);
    ~RadioDecoder() override;

    uint32_t sampleRate() const override;
    uint32_t channels() const override;
    uint64_t totalFrames() const override { return 0; }
    size_t read(float* interleaved, size_t frames) override;
    bool seek(uint64_t) override { return false; }
    bool isLive() const override { return true; }
    void liveInfo(std::string& title, std::string& artist, std::string& album) const override;

    RadioStatus status() const;
    /** Az állapot megosztott része: a vezérlő a lejátszó nélkül is olvashatja (a dekóder megszűnése után is). */
    struct Core;
    std::shared_ptr<Core> core() const { return core_; }
    static RadioStatus statusOf(const Core& c);
    static bool closed(const Core& c);

private:
    explicit RadioDecoder(std::shared_ptr<Core> c);
    std::shared_ptr<Core> core_;
    std::thread thread_;
};

} // namespace ee_analyzer::radio
