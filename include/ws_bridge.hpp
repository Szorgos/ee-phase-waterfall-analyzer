#pragma once
/**
 * @file ws_bridge.hpp
 * @brief Könnyűsúlyú, platformfüggetlen C++ WebSocket híd (RFC 6455) a hangmotor és a webes GUI között.
 *
 * - Alapból csak a 127.0.0.1 címen figyel. A hálózati elérés (távirányító) külön kapcsolható be: ekkor a helyi
 *   hálózatról csak párosított eszköz csatlakozhat (rövid kóddal kapott, hosszú véletlen tokennel).
 * - Egyszerre több kliens (felület, távirányító); az egyik változtatását a többi is megkapja ("sync").
 * - A motor hangjából valódi FFT-spektrumot számol és ~60 FPS-sel küldi ("spectrum" üzenet).
 * - A GUI vezérlőüzeneteit (preamp, modulkapcsolók, EQ sávok) ParamData-vá alakítja.
 *
 * Ugyanez a kód fut Linuxon (PipeWire csomópont) és Windowson (Aqulify.exe).
 */
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "net_compat.hpp"
#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif
#include "sha1_base64.hpp"
#include "spectrum.hpp"
#include "param_block.hpp"
#include "mini_json.hpp"
#include "ir_data.hpp"
#include "ee_dsp.hpp"
#include "self_path.hpp"
#include "media_session.hpp"
#include "mdns_announcer.hpp"
#include "bluetooth_sink.hpp"
#include <fstream>

namespace ee_analyzer {

struct BridgeCallbacks {
    /** Interleaved sztereó minták lehúzása a motorból (nem blokkolhat). */
    std::function<size_t(float* lr, size_t maxFrames)> pullAudio;
    /** A forrásjel aktuális mintavételi frekvenciája. */
    std::function<double()> sampleRate;
    /** Aktuális DSP beállítások. */
    std::function<ParamData()> getParams;
    /** Új DSP beállítások a GUI-ból. */
    std::function<void(const ParamData&)> setParams;
    /** Extra állapotmezők JSON-töredékként, pl. "\"source\":\"WASAPI\"" (opcionális). */
    std::function<std::string()> statusFields;
    /** Aktuális limiter csillapítás dB-ben (opcionális, a spektrum üzenethez). */
    std::function<double()> gainReduction;
    /** A Bass Enhancer felharmonikus-szintje dB-ben (opcionális, a spektrum üzenethez). */
    std::function<double()> bassHarmonics;
    /** Új konvolver impulzusválasz a GUI-ból (üres IrData = törlés). */
    std::function<void(IrData&&)> setIr;
    /** A betöltött IR adatai (név, mintavétel, előnézet) az állapotüzenethez. */
    std::function<IrData()> getIr;
    /**
     * IR-tár (az IR-profilok impulzusválaszai azonosítóval, az ir.bin formátumában) olvasása / mentése / törlése
     * (opcionális). Az azonosítót a híd ellenőrzi (isValidIrId), így fájlnévként használható.
     */
    std::function<std::string(const std::string& id)> loadLibraryIr;
    std::function<bool(const std::string& id, const std::string& bytes)> saveLibraryIr;
    std::function<bool(const std::string& id)> removeLibraryIr;
    /** Kimeneti hangeszköz váltása (pl. Speaker / USB Audio). */
    std::function<void(const std::string&)> setOutputSink;
    /** EQ profilok (a felület által kezelt JSON dokumentum) olvasása / mentése (opcionális). */
    std::function<std::string()> getPresets;
    std::function<void(const std::string&)> setPresets;
    /** A felületen választott nyelv (pl. "de"; üres: még nem választott) – a tálcamenü és a rendszerüzenetek is ezt használják. */
    std::function<std::string()> getLanguage;
    std::function<void(const std::string&)> setLanguage;
    /** A Magic Touch mérései JSON objektumként (magic::diagToJson); üres, ha nincs adat (opcionális). */
    std::function<std::string()> magicDiagJson;
    /** A hálózati elérés beállításai és a párosított eszközök JSON-ban (opcionális; nélküle a beállítás nem marad meg). */
    std::function<std::string()> loadRemote;
    std::function<bool(const std::string&)> saveRemote;
    /** A gépen szóló lejátszó ("most szól") JSON objektumként (media::toJson); üres: nincs ilyen szolgáltatás (opcionális). */
    std::function<std::string()> mediaJson;
    /** Lejátszásvezérlés: play / pause / toggle / next / previous / seek (value: pozíció másodpercben). */
    std::function<void(const std::string& action, double value)> mediaCommand;
    /** A borító: bájtok és típus, vagy egy https cím; false: nincs (vagy már más szól). */
    std::function<bool(const std::string& id, std::string& mime, std::string& bytes, std::string& url)> mediaArt;
    /** Beépített lejátszó (2.x): fájl megnyitása és lejátszása (csak a saját gépről); false: hiba (error). */
    std::function<bool(const std::string& path, std::string& error)> playerOpen;
    /** A beépített lejátszó leállítása (a „most szól” utána ismét a gépen futó lejátszót mutatja). */
    std::function<void()> playerStop;
    /**
     * Zenetár (2.x): a "library" és "library_*" üzenetek; a válasz JSON szöveg (üres: nincs válasz). remote: a
     * hálózatról jött (ilyenkor a mappák nem állíthatók, és útvonal nem adható ki). Ha be van kötve, a "state" üzenet
     * "caps" mezőjében "library" áll.
     */
    std::function<std::string(const std::string& type, const json::Value& msg, bool remote)> libraryRequest;
    /** Zenetár-borító album, mappa vagy szám azonosítójához. */
    std::function<bool(const std::string& id, std::string& mime, std::string& bytes)> libraryArt;
    /**
     * Lejátszási sor és listák (2.x, 3b / 3c): a "queue_*" és "playlist_*" üzenetek; a válasz JSON szöveg (üres:
     * nincs válasz). Ha be van kötve, a "caps" mezőben "queue" és "playlists" is áll; az állapotüzenet "queue" és
     * "playlists" mezője a queueStatus, illetve a playlistsStatus (üres: kimarad).
     */
    std::function<std::string(const std::string& type, const json::Value& msg, bool remote)> queueRequest;
    std::function<std::string()> queueStatus;
    std::function<std::string()> playlistsStatus;
    /** Telefonos zeneátvitel (4b): "phone_track_*" üzenetek és bináris AEPF hangfolyam-keretek. */
    std::function<std::string(const std::string& type, const json::Value& msg, bool remote)> phoneTrackRequest;
    std::function<std::string(const std::string& data)> phoneTrackBinary;
    /** Üzenet szórása a csatlakozott klienseknek (pl. phone_track_demand). */
    std::function<void(const std::string& text)> broadcast;
    /** Van-e csatlakozott távoli (telefonos) kliens (4d). */
    std::function<bool()> hasRemoteClient;
    /** Headless üzemmód (Raspberry Pi / szerver) PIN mentése (opcionális). */
    std::function<bool(const std::string& pin)> savePairPin;
};

/**
 * Az EQ profil-dokumentum maximális mérete (bájt). Egy 10 sávos AutoEq-profil kb. 830 bájt, így ez kb. 15 000 profil
 * (egy teljes AutoEq-mappa importja); 2 MB-nál (kb. 2500 profil) a nagyobb dokumentumot a motor nem mentette el.
 * A felület (PRESET_DOC_MAX_BYTES) ugyanezt a határt tartja. A JSON-ba ágyazott szöveg a kódolással kb. 1,2-szeres:
 * így is belefér a 20 MB-os WebSocket-keretbe.
 */
constexpr size_t kBridgeMaxPresetBytes = 12u << 20;

/** A borító legnagyobb mérete, amit a híd továbbad (bájt). */
constexpr size_t kBridgeMaxArtBytes = 2u << 20;

/** A GUI által küldött IR maximális hossza (frame). */
constexpr uint32_t kBridgeMaxIrFrames = 262144;

/** Egyszerre ennyi kliens (felület, távirányító) csatlakozhat. */
constexpr size_t kBridgeMaxClients = 8;

/** A vezérlési protokoll verziója (a "state" üzenet "protocol" mezője). 2: több kliens, "sync", távoli elérés. */
constexpr int kBridgeProtocol = 2;

/** Hálózati (LAN) elérés: alapból kikapcsolva; bekapcsolva ezen a porton (foglaltság esetén a következőkön) figyel. */
constexpr int kRemotePortFirst = 47820;
constexpr int kRemotePortCount = 5;

/** A párosító kód ennyi ideig használható a megjelenítése (a hálózati elérés bekapcsolása vagy új kód kérése) után. */
constexpr int kPairWindowSeconds = 300;

/** Egy kliens küldési sora legfeljebb ekkora lehet; ha nem olvas, a kapcsolat bomlik (a többi kliens nem vár rá). */
constexpr size_t kBridgeMaxQueuedBytes = 16u << 20;

namespace bridge_detail {

inline std::string toHex(const uint8_t* p, size_t n) {
    static const char* hx = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += hx[p[i] >> 4];
        s += hx[p[i] & 15];
    }
    return s;
}

/** Kriptográfiailag erős véletlen bájtok hexadecimálisan (std::random_device: /dev/urandom, arc4random, rand_s). */
inline std::string randomHex(size_t bytes) {
    std::random_device rd;
    std::vector<uint8_t> b(bytes);
    for (size_t i = 0; i < bytes; i += 4) {
        uint32_t v = rd();
        for (size_t k = 0; k < 4 && i + k < bytes; ++k, v >>= 8) b[i + k] = static_cast<uint8_t>(v & 0xFF);
    }
    return toHex(b.data(), b.size());
}

inline std::string randomDigits(int n) {
    std::random_device rd;
    std::string s;
    for (int i = 0; i < n; ++i) s += static_cast<char>('0' + rd() % 10);
    return s;
}

inline std::string sha1Hex(const std::string& s) {
    const std::vector<uint8_t> d = Sha1::hash(s);
    return toHex(d.data(), d.size());
}

/** Időzítéstől független összehasonlítás (token-kivonatokhoz). */
inline bool sameSecret(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

inline bool isHex(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

inline std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') out += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                 std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else out += s[i];
    }
    return out;
}

/** A kérés sorából (GET /?a=1&b=2 HTTP/1.1) egy lekérdezési paraméter. */
inline std::string queryParam(const std::string& request, const char* name) {
    const size_t eol = request.find("\r\n");
    const std::string line = request.substr(0, eol);
    const size_t q = line.find('?');
    if (q == std::string::npos) return {};
    const size_t end = line.find(' ', q);
    const std::string query = line.substr(q + 1, end == std::string::npos ? std::string::npos : end - q - 1);
    const std::string key = std::string(name) + "=";
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::string part = query.substr(pos, amp - pos);
        if (part.rfind(key, 0) == 0) return urlDecode(part.substr(key.size()));
        pos = amp + 1;
    }
    return {};
}

/** Eszköznév a felületnek: vezérlőkarakterek nélkül, legfeljebb 40 bájt, egész UTF-8 karakterekkel. */
inline std::string cleanName(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size();) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + len > in.size()) break;
        if (len == 1 && c < 0x20) { ++i; continue; }
        if (out.size() + len > 40) break;
        out.append(in, i, len);
        i += len;
    }
    return out;
}

/** A gép IPv4 címei a helyi hálózaton (a párosításhoz: ezen érhető el a motor). */
inline std::vector<std::string> localAddresses() {
    std::vector<std::string> out;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        r = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
    }
    if (r != NO_ERROR) return out;
    for (auto* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            char ip[INET_ADDRSTRLEN] = {};
            if (sin->sin_family == AF_INET && inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) out.emplace_back(ip);
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        if (!(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
        char ip[INET_ADDRSTRLEN] = {};
        const auto* sin = reinterpret_cast<const sockaddr_in*>(a->ifa_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) out.emplace_back(ip);
    }
    freeifaddrs(list);
#endif
    return out;
}

} // namespace bridge_detail

class WebSocketBridge {
public:
    explicit WebSocketBridge(int port = 8080) : port_(port) {}
    ~WebSocketBridge() { stop(); }

    WebSocketBridge(const WebSocketBridge&) = delete;
    WebSocketBridge& operator=(const WebSocketBridge&) = delete;

    bool start(BridgeCallbacks cb) {
        cb_ = std::move(cb);
        if (!cb_.broadcast) cb_.broadcast = [this](const std::string& text) { broadcast(text); };
        if (!cb_.hasRemoteClient) cb_.hasRemoteClient = [this]() -> bool { return hasRemoteClient(); };
        if (!net::startup()) {
            std::cerr << "[WS Bridge] Socket alrendszer indítása sikertelen!\n";
            return false;
        }
        listener_ = openListener(INADDR_LOOPBACK, port_);
        if (listener_ == net::kInvalidSocket) {
            std::cerr << "[WS Bridge] A 127.0.0.1:" << port_ << " port foglalt (fut már egy példány?)\n";
            net::cleanup();
            return false;
        }
        std::cout << "[WS Bridge] Élő WebSocket híd: ws://127.0.0.1:" << port_ << "\n";
        loadRemoteConfig();
        running_ = true;
        thread_ = std::thread(&WebSocketBridge::run, this);
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
        for (auto& c : clients_) net::closeSocket(c->sock);
        clients_.clear();
        for (auto& p : pending_) net::closeSocket(p.sock);
        pending_.clear();
        closeRemoteListener(true);
        net::closeSocket(listener_);
        listener_ = net::kInvalidSocket;
        net::cleanup();
    }

    bool hasClient() const { return clientCount_.load() > 0; }
    bool hasRemoteClient() const { return remoteClientCount_.load() > 0; }

    void setHeadless(bool h) {
        headless_ = h;
        if (headless_) {
            setRemoteEnabled(true);
        }
    }
    bool isHeadless() const { return headless_; }

    void setWebRootDir(const std::string& dir) {
        webRootDir_ = dir;
        while (!webRootDir_.empty() && (webRootDir_.back() == '/' || webRootDir_.back() == '\\')) {
            webRootDir_.pop_back();
        }
    }
    const std::string& webRootDir() const { return webRootDir_; }

    void setRemoteEnabled(bool on) {
        if (remoteEnabled_ == on && (remoteListener_ != net::kInvalidSocket) == on) return;
        remoteEnabled_ = on;
        if (on) {
            if (openRemoteListener()) newPairCode();
        } else {
            closeRemoteListener();
            pairCode_.clear();
        }
        saveRemoteConfig();
        broadcastRemoteState();
    }
    bool remoteEnabled() const { return remoteEnabled_; }
    std::string currentPairCode() const { return pairCode_; }

private:
    using clock = std::chrono::steady_clock;

    struct Client {
        net::socket_t sock = net::kInvalidSocket;
        bool remote = false;            // a helyi hálózatról, párosított eszköz tokenjével
        std::string deviceId, deviceName;
        std::string rx, frag;           // beérkező bájtok, töredezett üzenet
        uint8_t fragOpcode = 0;
        std::string out;                // küldésre váró bájtok (nem blokkoló küldés)
        size_t outPos = 0;
        bool spectrum = true;           // kér-e spektrumot, és milyen gyakran
        int spectrumMs = 16;
        clock::time_point lastSpectrum{};
        uint64_t spectrumSeq = 0;
        bool dead = false;
        const char* why = "";
    };

    /** Kézfogásra váró kapcsolat (a fejléc nem blokkolva, legfeljebb néhány másodpercig érkezik). */
    struct Pending {
        net::socket_t sock = net::kInvalidSocket;
        bool remote = false;
        std::string buf;
        clock::time_point deadline;
        std::string clientIp;
    };

    /** Jóváhagyásra váró párosítási kérelem a telefonról (push prompt). */
    struct PendingPairRequest {
        std::string requestId;
        std::string deviceName;
        std::string clientIp;
        std::string clientType;
        std::string tier;
        net::socket_t sock = net::kInvalidSocket;
        std::string secWebSocketKey;
        clock::time_point deadline;
    };

    struct RemoteDevice {
        std::string id, name, hash;     // a tokennek csak a kivonatát tároljuk
        long long created = 0, last = 0;
    };

    // ------------------------------------------------------------------ hálózat
    static net::socket_t openListener(uint32_t hostAddr, int port) {
        net::socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == net::kInvalidSocket) return s;
#ifndef _WIN32
        int opt = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#else
        BOOL excl = TRUE; // Windowson ne lehessen "ellopni" a portot
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(hostAddr);
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(s, 8) != 0) {
            net::closeSocket(s);
            return net::kInvalidSocket;
        }
        net::setNonBlocking(s, true);
        return s;
    }

    void run() {
        SpectrumAnalyzer analyzer(4096, 160);
        std::vector<float> chunk(8192 * 2);
        auto lastSpectrum = clock::now();
        auto lastStatus = clock::now();
        bool freshAudio = false;
        std::string spectrumMsg;
        uint64_t spectrumSeq = 0;

        while (running_) {
            acceptNew(listener_, false);
            if (remoteListener_ != net::kInvalidSocket) acceptNew(remoteListener_, true);
            progressPending();
            progressPendingPairRequests();

            // 1. Hang lehúzása a motorból (akkor is, ha nincs kliens, hogy ne torlódjon fel)
            if (cb_.pullAudio) {
                const double inSr = cb_.sampleRate ? cb_.sampleRate() : 48000.0;
                analyzer.setSampleRate(inSr > 0 ? inSr : 48000.0);   // a mély sávok decimálásához
                size_t got;
                while ((got = cb_.pullAudio(chunk.data(), chunk.size() / 2)) > 0) {
                    analyzer.push(chunk.data(), got);
                    freshAudio = true;
                }
            }

            if (!clients_.empty()) {
                const auto now = clock::now();
                const bool wanted = std::any_of(clients_.begin(), clients_.end(), [](const auto& c) { return c->spectrum; });
                if (wanted && freshAudio && now - lastSpectrum >= std::chrono::milliseconds(16)) {
                    const double sr = cb_.sampleRate ? cb_.sampleRate() : 48000.0;
                    analyzer.compute(sr > 0 ? sr : 48000.0);
                    spectrumMsg = buildSpectrumMessage(analyzer, sr);
                    ++spectrumSeq;
                    lastSpectrum = now;
                    freshAudio = false;
                }
                // Mindenki a saját ütemében kapja (pl. a telefon ritkábban): ha még az előzőt sem olvasta, kimarad
                for (auto& c : clients_) {
                    if (!c->spectrum || c->spectrumSeq == spectrumSeq || spectrumMsg.empty() ||
                        now - c->lastSpectrum < std::chrono::milliseconds(c->spectrumMs))
                        continue;
                    queueText(*c, spectrumMsg, true);
                    c->spectrumSeq = spectrumSeq;
                    c->lastSpectrum = now;
                }
                if (now - lastStatus >= std::chrono::milliseconds(500)) {
                    const std::string status = buildStatusMessage();
                    for (auto& c : clients_) queueText(*c, status, true);
                    lastStatus = now;
                }
                for (auto& c : clients_) {
                    if (c->dead) continue;
                    pumpIncoming(*c);
                    flush(*c);
                }
                flushSync();
                removeDead();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    void acceptNew(net::socket_t listener, bool remote) {
        for (int k = 0; k < 4; ++k) {
            sockaddr_in ca{};
#ifdef _WIN32
            int len = sizeof(ca);
#else
            socklen_t len = sizeof(ca);
#endif
            net::socket_t s = ::accept(listener, reinterpret_cast<sockaddr*>(&ca), &len);
            if (s == net::kInvalidSocket) return;
            // Külön keret a helyi és a hálózati kapcsolatoknak: a hálózatról nem lehet kiszorítani a helyi felületet
            const auto waiting = std::count_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return p.remote == remote; });
            if (waiting >= (remote ? 4 : 8)) { net::closeSocket(s); continue; }
            net::setNonBlocking(s, true);
            char ipBuf[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &ca.sin_addr, ipBuf, sizeof(ipBuf));
            pending_.push_back({ s, remote, {}, clock::now() + std::chrono::seconds(remote ? 5 : 2), std::string(ipBuf) });
        }
    }

    void progressPending() {
        const auto now = clock::now();
        for (size_t i = 0; i < pending_.size();) {
            Pending& p = pending_[i];
            bool done = false, ok = false;
            char buf[2048];
            while (!done) {
                const long long n = net::recvSome(p.sock, buf, sizeof(buf));
                if (n > 0) {
                    p.buf.append(buf, static_cast<size_t>(n));
                    if (p.buf.size() > 8192) done = true;   // túl hosszú fejléc
                    continue;
                }
                if (n == 0 || !net::wouldBlock()) done = true;
                break;
            }
            if (!done && p.buf.find("\r\n\r\n") != std::string::npos) {
                ok = finishHandshake(p);
                done = true;
            } else if (!done && now > p.deadline) {
                done = true;
            }
            if (done) {
                if (!ok && p.sock != net::kInvalidSocket) net::closeSocket(p.sock);
                pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    static void reply(net::socket_t s, const char* status) {
        const std::string r = std::string("HTTP/1.1 ") + status + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        net::sendSome(s, r.data(), r.size());
    }

    static void replyHttp(net::socket_t s, int code, const char* status, const std::string& mime, const std::string& body) {
        std::string r = "HTTP/1.1 " + std::to_string(code) + " " + status + "\r\n";
        r += "Content-Type: " + mime + "\r\n";
        r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        r += "Connection: close\r\n";
        r += "Access-Control-Allow-Origin: *\r\n\r\n";
        r += body;
        net::sendSome(s, r.data(), r.size());
    }

    bool handleHttpRequest(Pending& p) {
        const std::string& req = p.buf;
        if (req.rfind("GET ", 0) != 0 && req.rfind("HEAD ", 0) != 0) return false;

        const size_t endLine = req.find("\r\n");
        if (endLine == std::string::npos) return false;
        const std::string line = req.substr(0, endLine);
        const size_t sp1 = line.find(' ');
        const size_t sp2 = line.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) return false;

        const std::string rawPath = line.substr(sp1 + 1, sp2 - (sp1 + 1));
        const size_t q = rawPath.find('?');
        std::string path = (q != std::string::npos) ? rawPath.substr(0, q) : rawPath;

        if (path == "/" || path.empty()) path = "/gui_integrated_system.html";

        // Biztonsági ellenőrzés: könyvtárelhagyás (directory traversal) kizárása
        if (path.find("..") != std::string::npos || path.find('\\') != std::string::npos) {
            replyHttp(p.sock, 403, "Forbidden", "text/plain", "Forbidden");
            return true;
        }

        if (path == "/api/status") {
            std::string st = "{\"status\":\"ok\",\"headless\":";
            st += headless_ ? "true" : "false";
            st += ",\"pairingOpen\":";
            st += pairingOpen() ? "true" : "false";
            st += ",\"clients\":" + std::to_string(clients_.size()) + "}";
            replyHttp(p.sock, 200, "OK", "application/json; charset=utf-8", st);
            return true;
        }

        if (webRootDir_.empty()) {
            replyHttp(p.sock, 404, "Not Found", "text/plain", "Web root not configured");
            return true;
        }

        std::string ext;
        const size_t dot = path.rfind('.');
        if (dot != std::string::npos) ext = path.substr(dot);
        std::string mime = "application/octet-stream";
        if (ext == ".html" || ext == ".htm") mime = "text/html; charset=utf-8";
        else if (ext == ".js") mime = "application/javascript; charset=utf-8";
        else if (ext == ".css") mime = "text/css; charset=utf-8";
        else if (ext == ".json") mime = "application/json; charset=utf-8";
        else if (ext == ".png") mime = "image/png";
        else if (ext == ".svg") mime = "image/svg+xml";
        else if (ext == ".txt") mime = "text/plain; charset=utf-8";
        else {
            replyHttp(p.sock, 403, "Forbidden", "text/plain", "File type not supported");
            return true;
        }

        const std::string fullPath = webRootDir_ + path;
        std::ifstream f(fullPath, std::ios::binary);
        if (!f.is_open()) {
            replyHttp(p.sock, 404, "Not Found", "text/plain", "File not found: " + path);
            return true;
        }
        const std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        replyHttp(p.sock, 200, "OK", mime, body);
        return true;
    }

    bool approveDirectHeadless(Pending& p, const std::string& key, const std::string& request) {
        if (clock::now() < pairLockUntil_) {
            reply(p.sock, "429 Too Many Requests");
            return false;
        }
        if (clients_.size() >= kBridgeMaxClients) {
            reply(p.sock, "503 Service Unavailable");
            return false;
        }

        const std::string rawName = bridge_detail::queryParam(request, "name");
        std::string name = bridge_detail::cleanName(rawName);
        if (name.empty()) name = "Távoli eszköz";

        const std::string devId = bridge_detail::randomHex(4);
        const std::string token = bridge_detail::randomHex(32);
        RemoteDevice d;
        d.id = devId;
        d.name = name;
        d.hash = bridge_detail::sha1Hex(token);
        d.created = d.last = static_cast<long long>(std::time(nullptr));
        devices_.push_back(d);
        if (devices_.size() > 32) devices_.erase(devices_.begin());
        saveRemoteConfig();

        const std::string magicGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        const std::string acceptKey = Base64::encode(Sha1::hash(key + magicGuid));
        auto c = std::make_unique<Client>();
        c->sock = p.sock;
        c->remote = true;
        c->deviceId = devId;
        c->deviceName = d.name;
        c->out = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: " + acceptKey + "\r\n\r\n";
        net::setNoDelay(c->sock);
        p.sock = net::kInvalidSocket;
        Client& cl = *c;
        clients_.push_back(std::move(c));
        updateClientCountsLocked();
        std::cout << "[WS Bridge] Headless közvetlen párosítás jóváhagyva: " << cl.deviceName << " (" << devId << ")\n";

        cur_ = &cl;
        sendText("{\"type\":\"paired\",\"id\":\"" + cl.deviceId + "\",\"token\":\"" + token + "\"}");
        sendText(buildStateMessage());
        cur_ = nullptr;
        broadcastRemoteState();
        return true;
    }

    bool handlePushPairRequest(Pending& p, const std::string& key, const std::string& request) {
        // Harmadik féltől származó távirányítók kizárása: csak a hitelesített Aqulify Remote kliens párosítható
        const std::string clientType = bridge_detail::cleanName(bridge_detail::queryParam(request, "client"));
        if (clientType != "aqulify_remote") {
            reply(p.sock, "403 Forbidden");
            return false;
        }

        const bool hasLocal = std::any_of(clients_.begin(), clients_.end(), [](const auto& c) { return !c->remote && !c->dead; });
        if (!hasLocal) {
            if (headless_ && pairingOpen()) {
                return approveDirectHeadless(p, key, request);
            }
            reply(p.sock, "503 Service Unavailable");
            return false;
        }
        if (clock::now() < pairLockUntil_) {
            reply(p.sock, "429 Too Many Requests");
            return false;
        }
        if (pendingPairRequests_.size() >= kMaxPendingPairRequests) {
            reply(p.sock, "429 Too Many Requests");
            return false;
        }

        const std::string rawName = bridge_detail::queryParam(request, "name");
        std::string name = bridge_detail::cleanName(rawName);
        if (name.empty()) name = "Távoli eszköz";
        const std::string tier = bridge_detail::cleanName(bridge_detail::queryParam(request, "tier"));

        const std::string reqId = "req_" + bridge_detail::randomHex(4);

        PendingPairRequest pr;
        pr.requestId = reqId;
        pr.deviceName = name;
        pr.clientIp = p.clientIp;
        pr.clientType = clientType;
        pr.tier = tier;
        pr.sock = p.sock;
        pr.secWebSocketKey = key;
        pr.deadline = clock::now() + std::chrono::seconds(kPairRequestTimeoutSeconds);

        p.sock = net::kInvalidSocket;
        pendingPairRequests_.push_back(std::move(pr));

        std::string notice = "{\"type\":\"pair_request\",\"requestId\":\"" + reqId +
                             "\",\"name\":\"" + media::jsonEscape(name) +
                             "\",\"ip\":\"" + media::jsonEscape(p.clientIp) + "\"";
        if (!clientType.empty()) notice += ",\"client\":\"" + media::jsonEscape(clientType) + "\"";
        if (!tier.empty()) notice += ",\"tier\":\"" + media::jsonEscape(tier) + "\"";
        notice += "}";

        for (auto& c : clients_) {
            if (!c->remote && !c->dead) queueText(*c, notice);
        }
        std::cout << "[WS Bridge] Párosítási kérelem: " << name << " (" << p.clientIp << ", " << reqId << ")\n";
        return true;
    }

    void progressPendingPairRequests() {
        const auto now = clock::now();
        for (size_t i = 0; i < pendingPairRequests_.size();) {
            auto& pr = pendingPairRequests_[i];
            const bool expired = (now > pr.deadline);
            char peekBuf[1];
            const long long n = net::recvSome(pr.sock, peekBuf, sizeof(peekBuf));
            const bool disconnected = (n == 0 || (n < 0 && !net::wouldBlock()));

            if (expired || disconnected) {
                if (expired) reply(pr.sock, "408 Request Timeout");
                net::closeSocket(pr.sock);
                const std::string cancelMsg = "{\"type\":\"pair_cancel\",\"requestId\":\"" + pr.requestId + "\"}";
                for (auto& c : clients_)
                    if (!c->remote && !c->dead) queueText(*c, cancelMsg);
                pendingPairRequests_.erase(pendingPairRequests_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    /** A kézfogás befejezése; true: a kapcsolat kliens lett (a socket hozzá került). */
    bool finishHandshake(Pending& p) {
        const std::string& request = p.buf;
        const std::string key = headerValue(request, "sec-websocket-key");
        if (key.empty()) {
            if (handleHttpRequest(p)) return false;
            reply(p.sock, "426 Upgrade Required");
            return false;
        }

        std::string deviceId, issuedToken;
        if (p.remote && remoteListener_ == net::kInvalidSocket) {   // közben kikapcsolták
            reply(p.sock, "503 Service Unavailable");
            return false;
        }
        if (p.remote) {
            // A helyi hálózatról csak párosított eszköz (token) vagy érvényes párosító kód fogadható el
            const std::string token = bridge_detail::queryParam(request, "token");
            const std::string code = bridge_detail::queryParam(request, "pair");
            if (!token.empty()) {
                deviceId = deviceForToken(token);
                if (deviceId.empty()) { reply(p.sock, "401 Unauthorized"); return false; }
            } else if (!code.empty()) {
                // Harmadik féltől származó távirányítók kizárása: csak a hitelesített Aqulify Remote kliens párosítható
                const std::string clientType = bridge_detail::cleanName(bridge_detail::queryParam(request, "client"));
                if (clientType != "aqulify_remote") {
                    reply(p.sock, "403 Forbidden");
                    return false;
                }
                if (code == "request") {
                    return handlePushPairRequest(p, key, request);
                }
                const int r = tryPair(code, bridge_detail::queryParam(request, "name"), deviceId, issuedToken);
                if (r != 0) { reply(p.sock, r == 429 ? "429 Too Many Requests" : "401 Unauthorized"); return false; }
            } else {
                reply(p.sock, "401 Unauthorized");
                return false;
            }
        } else if (!originAllowed(headerValue(request, "origin"))) {
            // Csak a saját felület csatlakozhat: egy tetszőleges weboldal a böngészőből ne vezérelhesse a motort
            reply(p.sock, "403 Forbidden");
            return false;
        }

        if (clients_.size() >= kBridgeMaxClients) {
            // Helyi felületnek helyet csinálunk (a legrégebbi helyi kapcsolat bomlik, pl. egy újratöltött ablaké)
            auto oldest = std::find_if(clients_.begin(), clients_.end(), [](const auto& c) { return !c->remote; });
            if (p.remote || oldest == clients_.end()) { reply(p.sock, "503 Service Unavailable"); return false; }
            kill(**oldest, "új felület csatlakozott");
            removeDead();
        }

        const std::string magicGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        const std::string acceptKey = Base64::encode(Sha1::hash(key + magicGuid));
        auto c = std::make_unique<Client>();
        c->sock = p.sock;
        c->remote = p.remote;
        c->deviceId = deviceId;
        c->deviceName = deviceName(deviceId);
        c->out = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: " + acceptKey + "\r\n\r\n";
        net::setNoDelay(c->sock);
        Client& cl = *c;
        clients_.push_back(std::move(c));
        updateClientCountsLocked();
        if (cl.remote) std::cout << "[WS Bridge] Távoli eszköz csatlakozott: " << cl.deviceName << "\n";
        else std::cout << "[WS Bridge] Grafikus felület csatlakozott.\n";

        cur_ = &cl;
        if (!issuedToken.empty())
            sendText("{\"type\":\"paired\",\"id\":\"" + cl.deviceId + "\",\"token\":\"" + issuedToken + "\"}");
        sendText(buildStateMessage());
        cur_ = nullptr;
        if (cl.remote) broadcastRemoteState();   // a helyi felület látja, hogy az eszköz csatlakozott
        return true;
    }

    /** Engedélyezett Origin: nincs (nem böngésző), helyi fájl, a Windows ablak virtuális gépneve, vagy localhost. */
    static bool originAllowed(std::string o) {
        for (auto& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (o.empty() || o == "null" || o.rfind("file://", 0) == 0) return true;
        for (const char* ok : { "https://aqulify.local", "http://aqulify.local" })
            if (o == ok) return true;
        for (const char* host : { "http://127.0.0.1", "http://localhost", "http://[::1]" }) {
            const std::string h = host;
            if (o == h || (o.rfind(h + ":", 0) == 0)) return true;
        }
        return false;
    }

    static std::string headerValue(const std::string& req, const std::string& nameLower) {
        size_t pos = 0;
        while (pos < req.size()) {
            size_t eol = req.find("\r\n", pos);
            if (eol == std::string::npos) eol = req.size();
            std::string line = req.substr(pos, eol - pos);
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name = line.substr(0, colon);
                for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (name == nameLower) {
                    std::string v = line.substr(colon + 1);
                    const size_t a = v.find_first_not_of(" \t");
                    const size_t b = v.find_last_not_of(" \t");
                    return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
                }
            }
            pos = eol + 2;
        }
        return {};
    }

    /** A kapcsolat bontásra jelölése (a ciklus végén záródik; addig a kliens nem kap és nem küld semmit). */
    void kill(Client& c, const char* why) {
        if (c.dead) return;
        c.dead = true;
        c.why = why;
        if (syncSource_ == &c) syncSource_ = nullptr;
    }

    void removeDead() {
        bool remoteGone = false;
        for (auto it = clients_.begin(); it != clients_.end();) {
            Client& c = **it;
            if (!c.dead) { ++it; continue; }
            net::closeSocket(c.sock);
            if (c.remote) {
                std::cout << "[WS Bridge] Távoli eszköz lecsatlakozott: " << c.deviceName << " (" << c.why << ").\n";
                remoteGone = true;
            } else {
                std::cout << "[WS Bridge] Grafikus felület lecsatlakozott (" << c.why << ").\n";
            }
            it = clients_.erase(it);
        }
        updateClientCountsLocked();
        if (remoteGone) broadcastRemoteState();
    }

    /** Kiküldés a sorból, amennyi most blokkolás nélkül elfér (a többit a következő körben). */
    void flush(Client& c) {
        while (!c.dead && c.outPos < c.out.size()) {
            const long long n = net::sendSome(c.sock, c.out.data() + c.outPos, c.out.size() - c.outPos);
            if (n > 0) { c.outPos += static_cast<size_t>(n); continue; }
            if (n < 0 && net::wouldBlock()) return;
            kill(c, "küldési hiba");
            return;
        }
        c.out.clear();
        c.outPos = 0;
    }

    /**
     * @param droppable true = eldobható (spektrum/státusz) keret: ha a kliens épp nem olvas (pl. nyitott
     *        párbeszédablak, háttérbe tett ablak, lassú hálózat), kihagyjuk, és NEM bontjuk a kapcsolatot.
     */
    void queueFrame(Client& c, uint8_t opcode, const std::string& payload, bool droppable = false) {
        if (c.dead) return;
        if (droppable && c.outPos < c.out.size()) return;
        std::string& f = c.out;
        f.push_back(static_cast<char>(0x80 | opcode));
        const uint64_t len = payload.size();
        if (len <= 125) {
            f.push_back(static_cast<char>(len));
        } else if (len <= 0xFFFF) {
            f.push_back(126);
            f.push_back(static_cast<char>((len >> 8) & 0xFF));
            f.push_back(static_cast<char>(len & 0xFF));
        } else {
            f.push_back(127);
            for (int i = 7; i >= 0; --i) f.push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
        }
        f += payload;
        if (f.size() - c.outPos > kBridgeMaxQueuedBytes) { kill(c, "a kliens nem olvas"); return; }
        flush(c);
    }

    void queueText(Client& c, const std::string& text, bool droppable = false) { queueFrame(c, 0x1, text, droppable); }

    /** Válasz annak a kliensnek, akinek az üzenetét éppen feldolgozzuk. */
    void sendFrame(uint8_t opcode, const std::string& payload, bool droppable = false) {
        if (cur_) queueFrame(*cur_, opcode, payload, droppable);
    }
    void sendText(const std::string& text, bool droppable = false) { sendFrame(0x1, text, droppable); }

    /** Mindenkinek (except: kivéve ezt a klienst). */
    void broadcast(const std::string& text, const Client* except = nullptr) {
        for (auto& c : clients_)
            if (c.get() != except) queueText(*c, text);
    }

    /**
     * Egy kliens módosította a beállításokat: a többiek rövid késleltetéssel (összevonva) megkapják az új
     * állapotot ("sync"), így a felület és a távirányító egyszerre is használható.
     */
    void markSync(bool presets = false) {
        if (clients_.size() < 2) return;
        if (!syncPending_) {
            syncPending_ = true;
            syncSource_ = cur_;
        } else if (syncSource_ != cur_) {
            syncSource_ = nullptr;   // több kliens is módosított: mindenki megkapja
        }
        syncPresets_ = syncPresets_ || presets;
        lastChange_ = clock::now();
    }

    void flushSync() {
        if (!syncPending_ || clock::now() - lastChange_ < std::chrono::milliseconds(120)) return;
        syncPending_ = false;
        broadcast(buildStateMessage("sync", syncPresets_), syncSource_);
        syncPresets_ = false;
        syncSource_ = nullptr;
    }

    /** Beérkező WebSocket keretek feldolgozása (töredezett TCP olvasással is). */
    void pumpIncoming(Client& c) {
        char buf[4096];
        while (!c.dead) {
            const long long n = net::recvSome(c.sock, buf, sizeof(buf));
            if (n > 0) {
                c.rx.append(buf, static_cast<size_t>(n));
                if (c.rx.size() > (24u << 20)) { kill(c, "túl nagy üzenet"); return; }
                continue;
            }
            if (n == 0) { kill(c, "kapcsolat bontva"); return; }
            if (net::wouldBlock()) break;
            kill(c, "olvasási hiba");
            return;
        }

        cur_ = &c;
        while (!c.dead) {
            if (c.rx.size() < 2) break;
            const uint8_t b0 = static_cast<uint8_t>(c.rx[0]);
            const uint8_t b1 = static_cast<uint8_t>(c.rx[1]);
            const uint8_t opcode = b0 & 0x0F;
            const bool fin = (b0 & 0x80) != 0;
            const bool masked = (b1 & 0x80) != 0;
            uint64_t len = b1 & 0x7F;
            size_t hdr = 2;
            if (len == 126) {
                if (c.rx.size() < 4) break;
                len = (static_cast<uint8_t>(c.rx[2]) << 8) | static_cast<uint8_t>(c.rx[3]);
                hdr = 4;
            } else if (len == 127) {
                if (c.rx.size() < 10) break;
                len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<uint8_t>(c.rx[2 + i]);
                hdr = 10;
            }
            if (len > (20u << 20)) { kill(c, "túl nagy keret"); break; }
            const size_t maskLen = masked ? 4 : 0;
            if (c.rx.size() < hdr + maskLen + len) break;

            std::string payload = c.rx.substr(hdr + maskLen, static_cast<size_t>(len));
            if (masked) {
                const char* m = c.rx.data() + hdr;
                for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= m[i % 4];
            }
            c.rx.erase(0, hdr + maskLen + static_cast<size_t>(len));

            // Töredezett üzenetek (a böngésző a nagy bináris üzeneteket több keretben küldheti)
            if ((opcode == 0x1 || opcode == 0x2) && !fin) {
                c.fragOpcode = opcode;
                c.frag = std::move(payload);
                continue;
            }
            if (opcode == 0x0) {
                if (c.fragOpcode == 0) continue; // váratlan folytatás: eldobjuk
                c.frag += payload;
                if (c.frag.size() > (24u << 20)) { kill(c, "túl nagy üzenet"); break; }
                if (!fin) continue;
                const uint8_t op = c.fragOpcode;
                c.fragOpcode = 0;
                std::string full = std::move(c.frag);
                c.frag.clear();
                if (op == 0x1) handleMessage(full);
                else handleBinary(full);
                continue;
            }
            switch (opcode) {
                case 0x1: handleMessage(payload); break;       // szöveg
                case 0x2: handleBinary(payload); break;        // bináris (IR feltöltés)
                case 0x8: sendFrame(0x8, ""); flush(c); kill(c, "close keret"); break;
                case 0x9: sendFrame(0xA, payload); break;      // ping -> pong
                default: break;                                 // bináris / folytatás: figyelmen kívül
            }
        }
        cur_ = nullptr;
    }

    // ------------------------------------------------------------------ távoli elérés (párosítás)
    std::string deviceName(const std::string& id) const {
        for (const auto& d : devices_)
            if (d.id == id) return d.name.empty() ? id : d.name;
        return id;
    }

    std::string deviceForToken(const std::string& token) {
        if (token.size() != 64 || !bridge_detail::isHex(token)) return {};
        const std::string h = bridge_detail::sha1Hex(token);
        for (auto& d : devices_) {
            if (!bridge_detail::sameSecret(d.hash, h)) continue;
            d.last = static_cast<long long>(std::time(nullptr));
            saveRemoteConfig();
            return d.id;
        }
        return {};
    }

    /** Párosítás a motor által mutatott kóddal: 0 = siker, 401 = hibás vagy lejárt kód, 429 = túl sok próbálkozás. */
    int tryPair(const std::string& code, const std::string& name, std::string& outId, std::string& outToken) {
        const auto now = clock::now();
        if (now < pairLockUntil_) return 429;
        if (now >= pairUntil_) return 401;   // párosítani csak a kód megjelenítése után néhány percig lehet
        if (pairCode_.empty() || !bridge_detail::sameSecret(code, pairCode_)) {
            if (++pairFails_ >= 5) {
                // Kódpróbálgatás ellen: új kód, és egy percig nincs párosítás
                pairFails_ = 0;
                pairLockUntil_ = now + std::chrono::seconds(60);
                pairCode_ = bridge_detail::randomDigits(6);   // új kód, de a párosítási idő nem hosszabbodik
                std::cout << "[WS Bridge] Új párosító kód: " << pairCode_ << "\n";
                std::cout << "[WS Bridge] Túl sok hibás párosító kód: a párosítás egy percig szünetel.\n";
                broadcastRemoteState();
            }
            return 401;
        }
        RemoteDevice d;
        d.id = bridge_detail::randomHex(4);
        d.name = bridge_detail::cleanName(name);
        outToken = bridge_detail::randomHex(32);
        d.hash = bridge_detail::sha1Hex(outToken);
        d.created = d.last = static_cast<long long>(std::time(nullptr));
        devices_.push_back(d);
        if (devices_.size() > 32) devices_.erase(devices_.begin());
        outId = d.id;
        pairFails_ = 0;
        pairCode_.clear();   // egy kód egy párosításra jó; a következőhöz újat kell kérni
        pairUntil_ = now;
        saveRemoteConfig();
        std::cout << "[WS Bridge] Új eszköz párosítva: " << deviceName(d.id) << "\n";
        return 0;
    }

    /** Új párosító kód, amely a következő néhány percben használható. */
    void newPairCode() {
        pairCode_ = bridge_detail::randomDigits(6);
        const int winSec = headless_ ? 600 : kPairWindowSeconds;
        pairUntil_ = clock::now() + std::chrono::seconds(winSec);
        if (headless_) {
            std::cout << "[WS Bridge] Headless párosító PIN kód: " << pairCode_ << " (" << winSec / 60 << " percig érvényes)\n";
            if (cb_.savePairPin) cb_.savePairPin(pairCode_);
        } else {
            std::cout << "[WS Bridge] Párosító kód: " << pairCode_ << " (" << winSec / 60 << " percig érvényes)\n";
        }
    }

    bool pairingOpen() const { return remoteListener_ != net::kInvalidSocket && !pairCode_.empty() && clock::now() < pairUntil_; }

    bool openRemoteListener() {
        closeRemoteListener();
        for (int i = 0; i < kRemotePortCount; ++i) {
            const int port = remotePort_ + i;
            remoteListener_ = openListener(INADDR_ANY, port);
            if (remoteListener_ == net::kInvalidSocket) continue;
            remoteListenPort_ = port;
            std::cout << "[WS Bridge] Hálózati elérés bekapcsolva: 0.0.0.0:" << port << "\n";
            mdnsAnnouncer_.start(port);
            return true;
        }
        std::cerr << "[WS Bridge] A hálózati elérés portjai (" << remotePort_ << "–" << remotePort_ + kRemotePortCount - 1
                  << ") foglaltak.\n";
        return false;
    }

    void closeRemoteListener(bool quiet = false) {
        mdnsAnnouncer_.stop();
        for (auto& pr : pendingPairRequests_) {
            reply(pr.sock, "503 Service Unavailable");
            net::closeSocket(pr.sock);
        }
        pendingPairRequests_.clear();
        if (remoteListener_ == net::kInvalidSocket) return;
        net::closeSocket(remoteListener_);
        remoteListener_ = net::kInvalidSocket;
        remoteListenPort_ = 0;
        for (auto& c : clients_)
            if (c->remote) kill(*c, "hálózati elérés kikapcsolva");
        if (!quiet) std::cout << "[WS Bridge] Hálózati elérés kikapcsolva.\n";
    }

    void loadRemoteConfig() {
        if (!cb_.loadRemote) return;
        json::Value v;
        const std::string doc = cb_.loadRemote();
        if (doc.empty() || !json::parse(doc, v) || v.type != json::Value::Type::Object) return;
        remoteEnabled_ = num(v, "enabled", 0.0) != 0.0;
        remotePort_ = std::clamp(static_cast<int>(num(v, "port", kRemotePortFirst)), 1024, 65535 - kRemotePortCount);
        if (const json::Value* list = v.get("devices"); list && list->type == json::Value::Type::Array) {
            for (const auto& e : list->arr) {
                RemoteDevice d;
                d.id = e.get("id") ? e.get("id")->stringOr("") : "";
                d.name = bridge_detail::cleanName(e.get("name") ? e.get("name")->stringOr("") : "");
                d.hash = e.get("hash") ? e.get("hash")->stringOr("") : "";
                d.created = static_cast<long long>(num(e, "created", 0.0));
                d.last = static_cast<long long>(num(e, "last", 0.0));
                if (bridge_detail::isHex(d.id) && d.hash.size() == 40 && bridge_detail::isHex(d.hash)) devices_.push_back(d);
            }
        }
        if (remoteEnabled_) openRemoteListener();
    }

    void saveRemoteConfig() {
        if (!cb_.saveRemote) return;
        std::string s = "{\"enabled\":";
        s += remoteEnabled_ ? "true" : "false";
        s += ",\"port\":" + std::to_string(remotePort_) + ",\"devices\":[";
        for (size_t i = 0; i < devices_.size(); ++i) {
            const auto& d = devices_[i];
            if (i) s += ',';
            s += "{\"id\":\"" + d.id + "\",\"name\":\"" + jsonEscape(d.name) + "\",\"hash\":\"" + d.hash +
                 "\",\"created\":" + std::to_string(d.created) + ",\"last\":" + std::to_string(d.last) + "}";
        }
        s += "]}";
        if (!cb_.saveRemote(s)) std::cerr << "[WS Bridge] Figyelem: a távoli elérés beállításai nem menthetők.\n";
    }

    /** A távoli elérés állapota a helyi felületnek (a párosító kódot csak a helyi kliensek kapják meg). */
    std::string buildRemoteMessage() const {
        std::string s = "{\"type\":\"remote\",\"enabled\":";
        s += remoteEnabled_ ? "true" : "false";
        s += ",\"listening\":";
        s += remoteListener_ != net::kInvalidSocket ? "true" : "false";
        s += ",\"port\":" + std::to_string(remoteListenPort_ ? remoteListenPort_ : remotePort_);
        s += ",\"protocol\":" + std::to_string(kBridgeProtocol);
        const auto now = clock::now();
        const bool open = pairingOpen();
        s += ",\"code\":\"" + (open ? pairCode_ : std::string()) + "\"";
        s += ",\"codeSeconds\":" +
             std::to_string(open ? std::chrono::duration_cast<std::chrono::seconds>(pairUntil_ - now).count() : 0);
        s += ",\"locked\":";
        s += now < pairLockUntil_ ? "true" : "false";
        s += ",\"addresses\":[";
        const auto addrs = bridge_detail::localAddresses();
        for (size_t i = 0; i < addrs.size(); ++i) s += (i ? ",\"" : "\"") + addrs[i] + "\"";
        s += "],\"devices\":[";
        for (size_t i = 0; i < devices_.size(); ++i) {
            const auto& d = devices_[i];
            const bool online = std::any_of(clients_.begin(), clients_.end(),
                                            [&](const auto& c) { return !c->dead && c->remote && c->deviceId == d.id; });
            if (i) s += ',';
            s += "{\"id\":\"" + d.id + "\",\"name\":\"" + jsonEscape(d.name) + "\",\"created\":" + std::to_string(d.created) +
                 ",\"last\":" + std::to_string(d.last) + ",\"online\":" + (online ? "true" : "false") + "}";
        }
        s += "]}";
        return s;
    }

    void broadcastRemoteState() {
        bool any = false;
        for (auto& c : clients_) any = any || (!c->remote && !c->dead);
        if (!any) return;
        const std::string msg = buildRemoteMessage();
        for (auto& c : clients_)
            if (!c->remote) queueText(*c, msg);
    }

    /** A távoli elérés vezérlése: csak a saját gépről (helyi felület vagy segédprogram) érhető el. */
    bool handleRemoteControl(const std::string& type, const json::Value& msg) {
        if (type == "get_remote") {
            sendText(buildRemoteMessage());
        } else if (type == "set_remote") {
            const bool on = num(msg, "enabled", remoteEnabled_ ? 1.0 : 0.0) != 0.0;
            if (const json::Value* p = msg.get("port"))
                remotePort_ = std::clamp(static_cast<int>(p->numberOr(remotePort_)), 1024, 65535 - kRemotePortCount);
            remoteEnabled_ = on;
            if (on) {
                if (openRemoteListener()) newPairCode();   // bekapcsoláskor rögtön párosítható
            } else {
                closeRemoteListener();
                pairCode_.clear();
            }
            saveRemoteConfig();
            broadcastRemoteState();
        } else if (type == "remote_new_code") {
            if (remoteListener_ != net::kInvalidSocket) newPairCode();
            broadcastRemoteState();
        } else if (type == "remote_revoke") {
            const std::string id = msg.get("id") ? msg.get("id")->stringOr("") : "";
            const auto before = devices_.size();
            devices_.erase(std::remove_if(devices_.begin(), devices_.end(), [&](const RemoteDevice& d) { return d.id == id; }),
                           devices_.end());
            if (devices_.size() != before) {
                for (auto& c : clients_)
                    if (c->remote && c->deviceId == id) kill(*c, "párosítás visszavonva");
                saveRemoteConfig();
                std::cout << "[WS Bridge] Párosítás visszavonva: " << id << ".\n";
            }
            broadcastRemoteState();
        } else {
            return false;
        }
        return true;
    }

    void handlePairApprove(const json::Value& msg) {
        const std::string reqId = msg.get("requestId") ? msg.get("requestId")->stringOr("") : "";
        const bool approve = msg.get("approve") ? (msg.get("approve")->type == json::Value::Type::Bool ? msg.get("approve")->b : false) : false;

        auto it = std::find_if(pendingPairRequests_.begin(), pendingPairRequests_.end(),
                               [&](const PendingPairRequest& r) { return r.requestId == reqId; });
        if (it == pendingPairRequests_.end()) return;

        PendingPairRequest pr = std::move(*it);
        pendingPairRequests_.erase(it);

        if (!approve) {
            reply(pr.sock, "403 Forbidden");
            net::closeSocket(pr.sock);
            std::cout << "[WS Bridge] Párosítás elutasítva: " << reqId << "\n";
            return;
        }

        if (clients_.size() >= kBridgeMaxClients) {
            reply(pr.sock, "503 Service Unavailable");
            net::closeSocket(pr.sock);
            return;
        }

        std::string devId = bridge_detail::randomHex(4);
        std::string token = bridge_detail::randomHex(32);
        RemoteDevice d;
        d.id = devId;
        d.name = pr.deviceName.empty() ? ("Eszköz (" + devId + ")") : pr.deviceName;
        d.hash = bridge_detail::sha1Hex(token);
        d.created = d.last = static_cast<long long>(std::time(nullptr));
        devices_.push_back(d);
        if (devices_.size() > 32) devices_.erase(devices_.begin());
        saveRemoteConfig();

        const std::string magicGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        const std::string acceptKey = Base64::encode(Sha1::hash(pr.secWebSocketKey + magicGuid));
        auto c = std::make_unique<Client>();
        c->sock = pr.sock;
        c->remote = true;
        c->deviceId = devId;
        c->deviceName = d.name;
        c->out = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: " + acceptKey + "\r\n\r\n";
        net::setNoDelay(c->sock);
        Client& cl = *c;
        clients_.push_back(std::move(c));
        updateClientCountsLocked();
        std::cout << "[WS Bridge] Távoli eszköz jóváhagyva és csatlakozott: " << cl.deviceName << "\n";

        cur_ = &cl;
        sendText("{\"type\":\"paired\",\"id\":\"" + cl.deviceId + "\",\"token\":\"" + token + "\"}");
        sendText(buildStateMessage());
        cur_ = nullptr;
        broadcastRemoteState();
    }

    // ------------------------------------------------------------------ protokoll
    void handleBinary(const std::string& data) {
        if (data.size() >= 4 && std::memcmp(data.data(), "AEPF", 4) == 0) {
            // Telefonos zeneátvitel (4b)
            if (cb_.phoneTrackBinary) {
                const std::string reply = cb_.phoneTrackBinary(data);
                if (!reply.empty()) sendText(reply);
            }
            return;
        }
        IrData ir;
        if (data.size() >= 4 && std::memcmp(data.data(), "AEIS", 4) == 0) {
            // IR-profil: csak a tárba kerül, a betöltés külön "ir_select" üzenettel történik
            std::string id;
            bool ok = parseIrStoreMessage(data, id, ir, kBridgeMaxIrFrames) && cb_.saveLibraryIr &&
                      cb_.saveLibraryIr(id, encodeIrFile(ir));
            if (!ok) std::cout << "[WS Bridge] Az IR nem menthető a tárba (" << data.size() << " bájt).\n";
            sendText("{\"type\":\"ir_stored\",\"id\":\"" + jsonEscape(isValidIrId(id) ? id : "") + "\",\"ok\":" +
                     (ok ? "true" : "false") + "}");
            return;
        }
        if (!parseIrMessage(data, ir, kBridgeMaxIrFrames)) {
            std::cout << "[WS Bridge] Érvénytelen IR üzenet (" << data.size() << " bájt).\n";
            return;
        }
        std::cout << "[WS Bridge] Impulzusválasz érkezett: " << ir.name << " (" << ir.frames() << " minta, "
                  << ir.sampleRate << " Hz, " << ir.channels() << " csatorna)\n";
        activeIrId_.clear();
        if (cb_.setIr) cb_.setIr(std::move(ir));
        broadcast(buildStateMessage("ir"));   // mindenki (a küldő is) az új IR-t látja
    }

    /** IR-profil megnyitása: a tárban lévő IR lesz a konvolveré. @return false, ha nincs ilyen (vagy hibás). */
    bool selectLibraryIr(const std::string& id) {
        if (!isValidIrId(id) || !cb_.loadLibraryIr || !cb_.setIr) return false;
        IrData ir;
        if (!decodeIrFile(cb_.loadLibraryIr(id), ir, kBridgeMaxIrFrames)) return false;
        std::cout << "[WS Bridge] IR-profil: " << ir.name << " (" << id << ")\n";
        activeIrId_ = id;
        cb_.setIr(std::move(ir));
        broadcast(buildStateMessage("ir"));
        return true;
    }

    static double num(const json::Value& msg, const char* key, double def) {
        const json::Value* v = msg.get(key);
        return v ? v->numberOr(def) : def;
    }

    void handleMessage(const std::string& text) {
        json::Value msg;
        if (!json::parse(text, msg)) return;
        const json::Value* typeV = msg.get("type");
        if (!typeV) return;
        const std::string type = typeV->stringOr("");
        const json::Value* val = msg.get("val");
        const bool remote = cur_ && cur_->remote;

        // A távoli elérés vezérlése csak a saját gépről
        if (type == "get_remote" || type == "set_remote" || type.rfind("remote_", 0) == 0) {
            if (!remote) handleRemoteControl(type, msg);
            return;
        }
        if (type == "pair_approve") {
            if (!remote) handlePairApprove(msg);
            return;
        }
        if (type == "subscribe" && cur_) {
            // A kliens maga kéri, kap-e spektrumot, és legfeljebb milyen gyakran (pl. a telefon kímélete)
            cur_->spectrum = num(msg, "spectrum", 1.0) != 0.0;
            cur_->spectrumMs = 1000 / std::clamp(static_cast<int>(num(msg, "fps", 60.0)), 1, 60);
            return;
        }
        if (type == "media") {
            // A gépen szóló lejátszó vezérlése (a felületről és a távirányítóról is)
            const std::string action = msg.get("action") ? msg.get("action")->stringOr("") : "";
            if (media::validAction(action) && cb_.mediaCommand) cb_.mediaCommand(action, num(msg, "position", 0.0));
            return;
        }
        if (type == "player_open" || type == "player_stop") {
            // A beépített lejátszó: fájlt csak a saját gépről lehet megnyitni (a hálózatról tetszőleges útvonalat nem);
            // a lejátszás vezérlése a "media" paranccsal a távirányítóról is megy
            if (remote) {
                sendText("{\"type\":\"player_result\",\"ok\":false,\"error\":\"remote\"}");
                return;
            }
            if (type == "player_stop") {
                if (cb_.playerStop) cb_.playerStop();
                sendText("{\"type\":\"player_result\",\"ok\":true}");
                return;
            }
            const std::string path = msg.get("path") ? msg.get("path")->stringOr("") : "";
            std::string err = "unsupported";
            const bool ok = !path.empty() && path.size() < 4096 && cb_.playerOpen && cb_.playerOpen(path, err);
            sendText(ok ? std::string("{\"type\":\"player_result\",\"ok\":true}")
                        : "{\"type\":\"player_result\",\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}");
            return;
        }
        if (type == "library_art") {
            const std::string id = msg.get("id") ? msg.get("id")->stringOr("") : "";
            std::string mime, bytes;
            if (!id.empty() && id.size() <= 64 && cb_.libraryArt && cb_.libraryArt(id, mime, bytes) && bytes.size() <= kBridgeMaxArtBytes) {
                const std::vector<uint8_t> raw(bytes.begin(), bytes.end());
                sendText("{\"type\":\"library_art\",\"id\":\"" + jsonEscape(id) + "\",\"mime\":\"" + jsonEscape(mime) +
                         "\",\"data\":\"" + Base64::encode(raw) + "\"}");
            } else {
                sendText("{\"type\":\"library_art\",\"id\":\"" + jsonEscape(id) + "\"}");
            }
            return;
        }
        if ((type == "library" || type.rfind("library_", 0) == 0) && cb_.libraryRequest) {
            const std::string reply = cb_.libraryRequest(type, msg, remote);
            if (!reply.empty()) sendText(reply);
            return;
        }
        if ((type.rfind("queue_", 0) == 0 || type.rfind("playlist_", 0) == 0) && cb_.queueRequest) {
            const std::string reply = cb_.queueRequest(type, msg, remote);
            if (!reply.empty()) sendText(reply);
            return;
        }
        if ((type.rfind("phone_track", 0) == 0) && cb_.phoneTrackRequest) {
            const std::string reply = cb_.phoneTrackRequest(type, msg, remote);
            if (!reply.empty()) sendText(reply);
            return;
        }
        if (type == "get_media_art") {
            const std::string id = msg.get("id") ? msg.get("id")->stringOr("") : "";
            std::string mime, bytes, url;
            if (id.empty() || id.size() > 64 || !cb_.mediaArt || !cb_.mediaArt(id, mime, bytes, url)) {
                sendText("{\"type\":\"media_art\",\"id\":\"" + jsonEscape(id) + "\"}");
            } else if (!url.empty()) {
                sendText("{\"type\":\"media_art\",\"id\":\"" + jsonEscape(id) + "\",\"url\":\"" + jsonEscape(url) + "\"}");
            } else if (!bytes.empty() && bytes.size() <= kBridgeMaxArtBytes) {
                const std::vector<uint8_t> raw(bytes.begin(), bytes.end());
                sendText("{\"type\":\"media_art\",\"id\":\"" + jsonEscape(id) + "\",\"mime\":\"" + jsonEscape(mime) +
                         "\",\"data\":\"" + Base64::encode(raw) + "\"}");
            } else {
                sendText("{\"type\":\"media_art\",\"id\":\"" + jsonEscape(id) + "\"}");
            }
            return;
        }
        if (type == "hello" || type == "get_state") {
            sendText(buildStateMessage());
            return;
        }
        if (type == "save_presets") {
            // A felület a teljes profil-dokumentumot küldi JSON szövegként; csak érvényes JSON-t tárolunk
            const json::Value* d = msg.get("data");
            const std::string doc = d ? d->stringOr("") : "";
            json::Value check;
            if (!doc.empty() && doc.size() <= kBridgeMaxPresetBytes && json::parse(doc, check) &&
                check.type == json::Value::Type::Object && cb_.setPresets) {
                cb_.setPresets(doc);
                markSync(true);
            }
            return;
        }
        if (type == "ir_select" || type == "ir_delete") {
            const json::Value* idV = msg.get("id");
            const std::string id = idV ? idV->stringOr("") : "";
            if (type == "ir_delete") {
                // A betöltött IR marad (a konvolver tovább szól vele), csak a tárból törlődik
                if (isValidIrId(id) && cb_.removeLibraryIr) cb_.removeLibraryIr(id);
            } else if (!selectLibraryIr(id)) {
                sendText("{\"type\":\"ir_missing\",\"id\":\"" + jsonEscape(isValidIrId(id) ? id : "") + "\"}");
            }
            return;
        }
        if (type == "set_language") {
            // Csak rövid nyelvkód (a–z): a motor fájlba írja, és a tálcamenü is erre vált (távirányítóról nem)
            if (remote) return;
            const std::string l = val ? val->stringOr("") : "";
            const bool valid = l.size() >= 2 && l.size() <= 8 &&
                               std::all_of(l.begin(), l.end(), [](char c) { return c >= 'a' && c <= 'z'; });
            if (valid && cb_.setLanguage) cb_.setLanguage(l);
            return;
        }
        if (type == "set_output_sink") {
            const std::string s = val ? val->stringOr("") : msg.get("sink") ? msg.get("sink")->stringOr("") : "";
            if (!s.empty() && cb_.setOutputSink) cb_.setOutputSink(s);
            return;
        }
        if (!cb_.getParams || !cb_.setParams) return;

        ParamData p = cb_.getParams();
        const ParamData before = p;
        bool changed = true;
        // A felület a set_eq / get_eq_response üzenethez sorszámot ad; a válasz visszaküldi,
        // így a felület tudja, melyik sávállapothoz tartozik a görbe.
        const double reqSeq = num(msg, "seq", -1.0);

        bool eqChanged = false;
        if (type == "set_preamp" && val) {
            p.preampDb = static_cast<float>(val->numberOr(0.0));
        } else if (type.rfind("set_module_", 0) == 0 && val) {
            const std::string mod = type.substr(11);
            const bool on = val->numberOr(0.0) != 0.0;
            if (mod == "eq") p.set(kFlagEq, on);
            else if (mod == "crossfeed") p.set(kFlagCrossfeed, on);
            else if (mod == "convolver") p.set(kFlagConvolver, on);
            else if (mod == "limiter") p.set(kFlagLimiter, on);
            else if (mod == "bass") p.set(kFlagBass, on);
            else if (mod == "magic") p.set(kFlagMagic, on);
            else if (mod == "mono") p.set(kFlagMono, on);
            else if (mod == "autogain") p.set(kFlagAutogain, on);
            else changed = false;
            // Az EQ és a Konvolver egymást kizárja: a másik kikapcsolódik
            if (on && mod == "eq") p.set(kFlagConvolver, false);
            if (on && mod == "convolver") p.set(kFlagEq, false);
            if (!on && mod == "eq") p.eqCompare = 0;              // kikapcsolt modulnál az A/B is kikapcsol
            if (!on && mod == "convolver") p.convCompare = 0;
            if (!on && mod == "bass") p.bassCompare = 0;
            if (!on && mod == "mono") p.monoCompare = 0;
        } else if (type == "set_eq_compare") {
            p.eqCompare = num(msg, "compare", p.eqCompare) != 0.0 ? 1 : 0;
        } else if (type == "set_conv_compare") {
            p.convCompare = num(msg, "compare", p.convCompare) != 0.0 ? 1 : 0;
        } else if (type == "set_bass_compare") {
            p.bassCompare = num(msg, "compare", p.bassCompare) != 0.0 ? 1 : 0;
        } else if (type == "set_mono_compare") {
            p.monoCompare = num(msg, "compare", p.monoCompare) != 0.0 ? 1 : 0;
        } else if (type == "set_mono") {
            p.subsonicHz = static_cast<int32_t>(num(msg, "subsonic", p.subsonicHz));
            p.monoAmount = static_cast<float>(num(msg, "amount", p.monoAmount));
            p.monoFreqHz = static_cast<float>(num(msg, "freq", p.monoFreqHz));
        } else if (type == "set_autogain") {
            p.agcTargetLufs = static_cast<float>(num(msg, "target", p.agcTargetLufs));
            p.agcMaxGainDb = static_cast<float>(num(msg, "maxGain", p.agcMaxGainDb));
        } else if (type == "set_crossfeed") {
            p.crossfeedFcut = static_cast<float>(num(msg, "fcut", p.crossfeedFcut));
            p.crossfeedFeedDb = static_cast<float>(num(msg, "feed", p.crossfeedFeedDb));
            p.crossfeedPreset = static_cast<int32_t>(num(msg, "preset", p.crossfeedPreset));
        } else if (type == "set_eq_mode" && val) {
            const std::string m = val->stringOr("");
            const int mode = m == "iir" ? 0 : m == "fir" ? 1 : m == "fft" ? 2 : m == "spm" ? 3
                           : static_cast<int>(val->numberOr(0.0));
            p.eqMode = std::clamp(mode, 0, 3);
            eqChanged = true;
        } else if (type == "set_decramp" && val) {
            p.eqDecramp = std::clamp(static_cast<int32_t>(val->numberOr(0.0)), 0, 3);
            eqChanged = true;
        } else if (type == "set_limiter") {
            p.limMode = static_cast<int32_t>(num(msg, "mode", p.limMode));
            p.limOversampling = static_cast<int32_t>(num(msg, "oversampling", p.limOversampling));
            p.limDither = static_cast<int32_t>(num(msg, "dither", p.limDither));
            p.limThresholdDb = static_cast<float>(num(msg, "threshold", p.limThresholdDb));
            p.limLookaheadMs = static_cast<float>(num(msg, "lookahead", p.limLookaheadMs));
            p.limAttackMs = static_cast<float>(num(msg, "attack", p.limAttackMs));
            p.limReleaseMs = static_cast<float>(num(msg, "release", p.limReleaseMs));
            p.limGainBoost = num(msg, "boost", p.limGainBoost) != 0.0 ? 1 : 0;
            p.limStereoLink = static_cast<float>(num(msg, "stereoLink", p.limStereoLink));
            p.limAlr = num(msg, "alr", p.limAlr) != 0.0 ? 1 : 0;
            p.limAlrAttackMs = static_cast<float>(num(msg, "alrAttack", p.limAlrAttackMs));
            p.limAlrReleaseMs = static_cast<float>(num(msg, "alrRelease", p.limAlrReleaseMs));
            p.limAlrKneeDb = static_cast<float>(num(msg, "alrKnee", p.limAlrKneeDb));
            p.limAlrKneeSmoothDb = static_cast<float>(num(msg, "alrKneeSmooth", p.limAlrKneeSmoothDb));
            p.limInputGainDb = static_cast<float>(num(msg, "inputGain", p.limInputGainDb));
            p.limOutputGainDb = static_cast<float>(num(msg, "outputGain", p.limOutputGainDb));
        } else if (type == "set_bass") {
            p.bassAmountDb = static_cast<float>(num(msg, "amount", p.bassAmountDb));
            p.bassHarmonics = static_cast<float>(num(msg, "harmonics", p.bassHarmonics));
            p.bassScopeHz = static_cast<float>(num(msg, "scope", p.bassScopeHz));
            p.bassFloorHz = static_cast<float>(num(msg, "floor", p.bassFloorHz));
            p.bassBlend = static_cast<float>(num(msg, "blend", p.bassBlend));
            p.bassFloorActive = num(msg, "floorActive", p.bassFloorActive) != 0.0 ? 1 : 0;
            p.bassListen = num(msg, "listen", p.bassListen) != 0.0 ? 1 : 0;
            p.bassInputGainDb = static_cast<float>(num(msg, "inputGain", p.bassInputGainDb));
            p.bassOutputGainDb = static_cast<float>(num(msg, "outputGain", p.bassOutputGainDb));
        } else if (type == "set_magic") {
            p.magicAmount = static_cast<float>(num(msg, "amount", p.magicAmount));
            p.magicCompare = num(msg, "compare", p.magicCompare) != 0.0 ? 1 : 0;
            p.magicCharacter = static_cast<int32_t>(num(msg, "character", p.magicCharacter));
            p.magicContent = static_cast<int32_t>(num(msg, "content", p.magicContent));
        } else if (type == "set_convolver") {
            p.convIrWidth = static_cast<int32_t>(num(msg, "irWidth", p.convIrWidth));
            p.convAutogain = num(msg, "autogain", p.convAutogain) != 0.0 ? 1 : 0;
            p.convDryDb = static_cast<float>(num(msg, "dry", p.convDryDb));
            p.convWetDb = static_cast<float>(num(msg, "wet", p.convWetDb));
            p.convInputGainDb = static_cast<float>(num(msg, "inputGain", p.convInputGainDb));
            p.convOutputGainDb = static_cast<float>(num(msg, "outputGain", p.convOutputGainDb));
        } else if (type == "clear_ir") {
            activeIrId_.clear();
            if (cb_.setIr) cb_.setIr(IrData{});
            broadcast(buildStateMessage("ir"));
            return;
        } else if (type == "get_eq_response") {
            sendText(buildEqResponseMessage(p, reqSeq));
            return;
        } else if (type == "ping") {
            std::string pong = "{\"type\":\"pong\"";
            const json::Value* tVal = msg.get("t");
            if (tVal) {
                if (tVal->type == json::Value::Type::Number) {
                    pong += ",\"t\":" + std::to_string(static_cast<int64_t>(tVal->numberOr(0.0)));
                } else if (tVal->type == json::Value::Type::String) {
                    pong += ",\"t\":\"" + jsonEscape(tVal->str) + "\"";
                }
            }
            pong += "}";
            sendText(pong);
            return;
        } else if (type == "get_bluetooth_status") {
            sendText(aqulify::BluetoothSinkManager::instance().toJson());
            return;
        } else if (type == "set_bluetooth_sink") {
            bool en = num(msg, "enabled", 1.0) != 0.0;
            aqulify::BluetoothSinkManager::instance().setEnabled(en);
            sendText(aqulify::BluetoothSinkManager::instance().toJson());
            return;
        } else if (type == "set_smart_handover") {
            bool en = num(msg, "enabled", 1.0) != 0.0;
            aqulify::BluetoothSinkManager::instance().setSmartHandover(en);
            sendText(aqulify::BluetoothSinkManager::instance().toJson());
            return;
        } else if (type == "set_eq") {
            const json::Value* bands = msg.get("bands");
            if (!bands || bands->type != json::Value::Type::Array) return;
            // A sávokhoz tartozó előerősítés ugyanabban az üzenetben: a kettő egyszerre lép életbe
            if (const json::Value* pre = msg.get("preamp")) p.preampDb = static_cast<float>(pre->numberOr(p.preampDb));
            p.numBands = 0;
            for (const auto& b : bands->arr) {
                if (p.numBands >= kMaxEqBands) break;
                EqBandPod& out = p.bands[p.numBands++];
                out = EqBandPod{};
                out.type = static_cast<int32_t>(num(b, "type", 1.0));
                out.mode = static_cast<int32_t>(num(b, "mode", 0.0));
                out.slope = static_cast<int32_t>(num(b, "slope", 0.0));
                out.freq = static_cast<float>(num(b, "freq", 1000.0));
                out.gainDb = static_cast<float>(num(b, "gain", 0.0));
                out.q = static_cast<float>(num(b, "q", 4.36));
                out.width = static_cast<float>(num(b, "width", 4.0));
            }
            eqChanged = true;
        } else {
            changed = false;
        }

        if (changed) {
            p.set(kFlagInitialized, true);
            sanitizeParams(p);
            // Változatlan beállításnál nincs újraalkalmazás és nincs mentés (a mentés csak valódi változás után fut)
            if (std::memcmp(&p, &before, sizeof(ParamData)) != 0) {
                cb_.setParams(p);
                markSync();
            }
            if (eqChanged) sendText(buildEqResponseMessage(p, reqSeq));
        }
    }

    static std::string jsonEscape(const std::string& in) {
        std::string out;
        for (char c : in) {
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
            else out += c;
        }
        return out;
    }

    static void appendFloat(std::string& s, double v, int decimals) {
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), "%.*f", decimals, v);
        s += tmp;
    }

    std::string buildSpectrumMessage(const SpectrumAnalyzer& a, double sr) const {
        std::string s;
        s.reserve(a.numBands() * 14 + 64);
        s += "{\"type\":\"spectrum\",\"sr\":";
        appendFloat(s, sr, 0);
        // a sávok tengelye: ln(1 + f / f0) szerint egyenközű, 0 Hz – 24 kHz (régi motor: f0 nélkül, log 10 Hz – 24 kHz)
        s += ",\"f0\":";
        appendFloat(s, SpectrumAnalyzer::kWarpHz, 0);
        s += ",\"levels\":[";
        const auto& lv = a.levels();
        for (size_t i = 0; i < lv.size(); ++i) {
            if (i) s += ',';
            appendFloat(s, lv[i], 3);
        }
        s += "],\"phase\":[";
        const auto& ph = a.phaseDegrees();
        for (size_t i = 0; i < ph.size(); ++i) {
            if (i) s += ',';
            appendFloat(s, ph[i], 1);
        }
        s += "],\"peak\":";
        appendFloat(s, a.peakDb(), 1);
        if (cb_.gainReduction) {
            s += ",\"gr\":";
            appendFloat(s, cb_.gainReduction(), 2);
        }
        if (cb_.bassHarmonics) {
            s += ",\"bh\":";
            appendFloat(s, cb_.bassHarmonics(), 1);
        }
        s += "}";
        return s;
    }

    /** Az EQ eredő frekvenciamenete az LSP szűrőkkel (a felület görbéje ebből rajzol). */
    std::string buildEqResponseMessage(const ParamData& p, double seq = -1.0) const {
        constexpr size_t kPoints = 256;
        double sr = cb_.sampleRate ? cb_.sampleRate() : 48000.0;
        if (!(sr >= 8000.0)) sr = 48000.0;
        float f[kPoints], mag[kPoints], ph[kPoints];
        for (size_t i = 0; i < kPoints; ++i)
            f[i] = static_cast<float>(10.0 * std::pow(2200.0, static_cast<double>(i) / (kPoints - 1))); // 10 Hz .. 22 kHz
        size_t latency = 0;
        ee::Equalizer::response(p, static_cast<uint32_t>(sr), f, kPoints, mag, ph, &latency);
        std::string s = "{\"type\":\"eq_response\",\"sr\":";
        appendFloat(s, sr, 0);
        s += ",\"mode\":" + std::to_string(p.eqMode);
        s += ",\"latency\":" + std::to_string(latency);
        if (seq >= 0.0) { s += ",\"seq\":"; appendFloat(s, seq, 0); }
        s += ",\"freq\":[";
        for (size_t i = 0; i < kPoints; ++i) { if (i) s += ','; appendFloat(s, f[i], 2); }
        s += "],\"mag\":[";
        for (size_t i = 0; i < kPoints; ++i) { if (i) s += ','; appendFloat(s, mag[i], 3); }
        s += "],\"phase\":[";
        for (size_t i = 0; i < kPoints; ++i) { if (i) s += ','; appendFloat(s, ph[i], 2); }
        s += "]}";
        return s;
    }

    std::string buildStatusMessage() const {
        std::string s = "{\"type\":\"status\"";
#ifdef AQULIFY_VERSION
        s += ",\"version\":\"" AQULIFY_VERSION "\"";   // a felület ebből látja, ha a motor más verziójú
#endif
        // A motor elérési útja: verzióeltérésnél a felület megnevezi a futó (régi) motort
        static const std::string exe = selfExePath();
        if (!exe.empty()) s += ",\"exe\":\"" + jsonEscape(exe) + "\"";
        if (cb_.statusFields) {
            std::string extra = cb_.statusFields();
            if (!extra.empty()) { s += ','; s += extra; }
        }
        if (cb_.magicDiagJson) {
            const std::string m = cb_.magicDiagJson();
            if (!m.empty()) { s += ",\"magic\":"; s += m; }
        }
        if (cb_.mediaJson) {
            const std::string m = cb_.mediaJson();
            if (!m.empty()) { s += ",\"media\":"; s += m; }
        }
        if (cb_.queueStatus) {
            const std::string q = cb_.queueStatus();
            if (!q.empty()) { s += ",\"queue\":"; s += q; }
        }
        if (cb_.playlistsStatus) {
            const std::string p = cb_.playlistsStatus();
            if (!p.empty()) { s += ",\"playlists\":"; s += p; }
        }
        s += '}';
        return s;
    }

public:
    /**
     * @param reason "full" = teljes állapot (csatlakozáskor), "ir" = csak az IR változott,
     *               "sync" = egy másik kliens módosított (a felület átveszi, nem küldi vissza)
     * @param presets a profil-dokumentum is (teljes állapotnál mindig)
     */
    std::string buildStateMessage(const char* reason = "full", bool presets = false) const {
        ParamData p = cb_.getParams ? cb_.getParams() : ParamData{};
        std::string s = "{\"type\":\"state\",\"reason\":\"";
        s += reason;
        s += "\",\"protocol\":" + std::to_string(kBridgeProtocol);
        // 2.x: zenetár (a telefon Zenetár füle), lejátszási sor, telefonos zeneátvitel és Bluetooth Smart Connect
        std::string caps = "[";
        bool hasCap = false;
        if (cb_.libraryRequest) {
            caps += "\"library\"";
            hasCap = true;
            if (cb_.queueRequest) caps += ",\"queue\",\"playlists\"";
            if (cb_.phoneTrackRequest) caps += ",\"phone_track\"";
        }
        if (aqulify::BluetoothSinkManager::instance().isSupported()) {
            if (hasCap) caps += ",";
            caps += "\"bluetooth\"";
            hasCap = true;
        }
        caps += "]";
        if (hasCap) {
            s += ",\"caps\":" + caps;
        }
        if (headless_) {
            s += ",\"headless\":true";
        }
        s += ",\"initialized\":";
        s += p.has(kFlagInitialized) ? "true" : "false";
        s += ",\"preamp\":";
        appendFloat(s, p.preampDb, 2);
        s += ",\"modules\":{";
        s += "\"eq\":"; s += p.has(kFlagEq) ? "1" : "0";
        s += ",\"crossfeed\":"; s += p.has(kFlagCrossfeed) ? "1" : "0";
        s += ",\"convolver\":"; s += p.has(kFlagConvolver) ? "1" : "0";
        s += ",\"limiter\":"; s += p.has(kFlagLimiter) ? "1" : "0";
        s += ",\"bass\":"; s += p.has(kFlagBass) ? "1" : "0";
        s += ",\"magic\":"; s += p.has(kFlagMagic) ? "1" : "0";
        s += ",\"mono\":"; s += p.has(kFlagMono) ? "1" : "0";
        s += ",\"autogain\":"; s += p.has(kFlagAutogain) ? "1" : "0";
        s += "},\"crossfeedPreset\":" + std::to_string(p.crossfeedPreset);
        s += ",\"eqMode\":" + std::to_string(p.eqMode);
        s += ",\"decramp\":" + std::to_string(p.eqDecramp);
        s += ",\"crossfeed\":{\"fcut\":"; appendFloat(s, p.crossfeedFcut, 1);
        s += ",\"feed\":"; appendFloat(s, p.crossfeedFeedDb, 2); s += '}';
        s += ",\"limiter\":{\"mode\":" + std::to_string(p.limMode);
        s += ",\"oversampling\":" + std::to_string(p.limOversampling);
        s += ",\"dither\":" + std::to_string(p.limDither);
        s += ",\"threshold\":"; appendFloat(s, p.limThresholdDb, 2);
        s += ",\"lookahead\":"; appendFloat(s, p.limLookaheadMs, 3);
        s += ",\"attack\":"; appendFloat(s, p.limAttackMs, 3);
        s += ",\"release\":"; appendFloat(s, p.limReleaseMs, 3);
        s += ",\"boost\":" + std::to_string(p.limGainBoost);
        s += ",\"stereoLink\":"; appendFloat(s, p.limStereoLink, 1);
        s += ",\"alr\":" + std::to_string(p.limAlr);
        s += ",\"alrAttack\":"; appendFloat(s, p.limAlrAttackMs, 2);
        s += ",\"alrRelease\":"; appendFloat(s, p.limAlrReleaseMs, 1);
        s += ",\"alrKnee\":"; appendFloat(s, p.limAlrKneeDb, 2);
        s += ",\"alrKneeSmooth\":"; appendFloat(s, p.limAlrKneeSmoothDb, 2);
        s += ",\"inputGain\":"; appendFloat(s, p.limInputGainDb, 2);
        s += ",\"outputGain\":"; appendFloat(s, p.limOutputGainDb, 2); s += '}';
        s += ",\"bass\":{\"amount\":"; appendFloat(s, p.bassAmountDb, 2);
        s += ",\"harmonics\":"; appendFloat(s, p.bassHarmonics, 2);
        s += ",\"scope\":"; appendFloat(s, p.bassScopeHz, 1);
        s += ",\"floor\":"; appendFloat(s, p.bassFloorHz, 1);
        s += ",\"blend\":"; appendFloat(s, p.bassBlend, 2);
        s += ",\"floorActive\":" + std::to_string(p.bassFloorActive);
        s += ",\"listen\":" + std::to_string(p.bassListen);
        s += ",\"inputGain\":"; appendFloat(s, p.bassInputGainDb, 2);
        s += ",\"outputGain\":"; appendFloat(s, p.bassOutputGainDb, 2); s += '}';
        s += ",\"convolver\":{\"irWidth\":" + std::to_string(p.convIrWidth);
        s += ",\"autogain\":" + std::to_string(p.convAutogain);
        s += ",\"dry\":"; appendFloat(s, p.convDryDb, 2);
        s += ",\"wet\":"; appendFloat(s, p.convWetDb, 2);
        s += ",\"inputGain\":"; appendFloat(s, p.convInputGainDb, 2);
        s += ",\"outputGain\":"; appendFloat(s, p.convOutputGainDb, 2); s += '}';
        s += ",\"magic\":{\"amount\":"; appendFloat(s, p.magicAmount, 1);
        s += ",\"compare\":" + std::to_string(p.magicCompare);
        s += ",\"character\":" + std::to_string(p.magicCharacter);
        s += ",\"content\":" + std::to_string(p.magicContent) + '}';
        s += ",\"ab\":{\"eq\":" + std::to_string(p.eqCompare) + ",\"conv\":" + std::to_string(p.convCompare) +
             ",\"bass\":" + std::to_string(p.bassCompare) + ",\"mono\":" + std::to_string(p.monoCompare) + '}';
        s += ",\"monoBass\":{\"subsonicHz\":" + std::to_string(p.subsonicHz);
        s += ",\"amount\":"; appendFloat(s, p.monoAmount, 1);
        s += ",\"freqHz\":"; appendFloat(s, p.monoFreqHz, 0); s += '}';
        s += ",\"autogain\":{\"target\":"; appendFloat(s, p.agcTargetLufs, 1);
        s += ",\"maxGain\":"; appendFloat(s, p.agcMaxGainDb, 1); s += '}';
        // Van-e IR-tár (régebbi motornál nincs: a felület ilyenkor nem készít IR-profilt)
        if (cb_.loadLibraryIr && cb_.saveLibraryIr) s += ",\"irLibrary\":true";
        if (cb_.getIr) {
            IrData ir = cb_.getIr();
            s += ",\"ir\":";
            if (ir.empty()) {
                s += "null";
            } else {
                s += "{\"name\":\"" + jsonEscape(ir.name) + "\",\"id\":\"" + jsonEscape(activeIrId_) + "\",\"sr\":" + std::to_string(ir.sampleRate) +
                     ",\"channels\":" + std::to_string(ir.channels()) + ",\"frames\":" + std::to_string(ir.frames()) +
                     ",\"preview\":[";
                const auto pv = ir.preview(400);
                for (size_t i = 0; i < pv.size(); ++i) { if (i) s += ','; appendFloat(s, pv[i], 3); }
                s += "]}";
            }
        }
        s += ",\"bands\":[";
        const uint32_t n = std::min(p.numBands, kMaxEqBands);
        for (uint32_t i = 0; i < n; ++i) {
            if (i) s += ',';
            const EqBandPod& b = p.bands[i];
            s += "{\"type\":" + std::to_string(b.type) + ",\"mode\":" + std::to_string(b.mode) +
                 ",\"slope\":" + std::to_string(b.slope);
            s += ",\"freq\":"; appendFloat(s, b.freq, 2);
            s += ",\"gain\":"; appendFloat(s, b.gainDb, 2);
            s += ",\"q\":"; appendFloat(s, b.q, 3);
            s += ",\"width\":"; appendFloat(s, b.width, 3);
            s += '}';
        }
        s += "]}";
        if (cb_.statusFields) {
            std::string extra = cb_.statusFields();
            if (!extra.empty()) {
                s.pop_back(); // remove '}'
                s += ',';
                s += extra;
                s += '}';
            }
        }
        if (cb_.getLanguage) {
            const std::string lang = cb_.getLanguage();
            if (!lang.empty()) {
                s.pop_back();
                s += ",\"language\":\"" + jsonEscape(lang) + "\"}";
            }
        }
        if ((presets || std::strcmp(reason, "full") == 0) && cb_.getPresets) {
            // Mentett EQ profilok (érvényes JSON objektum, változatlanul beágyazva)
            const std::string doc = cb_.getPresets();
            json::Value check;
            if (!doc.empty() && doc.size() <= kBridgeMaxPresetBytes && json::parse(doc, check) &&
                check.type == json::Value::Type::Object) {
                s.pop_back();
                s += ",\"presets\":";
                s += doc;
                s += '}';
            }
        }
        return s;
    }

private:
    void updateClientCountsLocked() {
        clientCount_ = clients_.size();
        size_t rem = 0;
        for (const auto& c : clients_) if (c->remote) ++rem;
        remoteClientCount_ = rem;
    }

    int port_;
    BridgeCallbacks cb_;
    std::atomic<bool> running_{false};
    std::atomic<size_t> clientCount_{0};
    std::atomic<size_t> remoteClientCount_{0};
    std::thread thread_;
    net::socket_t listener_ = net::kInvalidSocket;
    std::vector<std::unique_ptr<Client>> clients_;
    std::vector<Pending> pending_;
    Client* cur_ = nullptr;             // akinek az üzenetét éppen feldolgozzuk
    bool syncPending_ = false, syncPresets_ = false;
    const Client* syncSource_ = nullptr;
    std::string activeIrId_;            // a betöltött IR tárbeli azonosítója (üres: nem IR-profilból jött)
    clock::time_point lastChange_{};
    // headless üzemmód (Raspberry Pi / szerver) és beépített HTTP szerver
    bool headless_ = false;
    std::string webRootDir_;
    // hálózati elérés
    bool remoteEnabled_ = false;
    int remotePort_ = kRemotePortFirst;
    int remoteListenPort_ = 0;
    net::socket_t remoteListener_ = net::kInvalidSocket;
    MdnsAnnouncer mdnsAnnouncer_;
    static constexpr size_t kMaxPendingPairRequests = 4;
    static constexpr int kPairRequestTimeoutSeconds = 45;
    std::vector<PendingPairRequest> pendingPairRequests_;
    std::vector<RemoteDevice> devices_;
    std::string pairCode_;
    clock::time_point pairUntil_{};     // eddig párosítható
    int pairFails_ = 0;
    clock::time_point pairLockUntil_{};
};

} // namespace ee_analyzer
