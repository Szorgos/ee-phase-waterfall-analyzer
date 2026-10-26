#pragma once
/**
 * @file ws_bridge.hpp
 * @brief Könnyűsúlyú, platformfüggetlen C++ WebSocket híd (RFC 6455) a hangmotor és a webes GUI között.
 *
 * - Csak a 127.0.0.1 címen figyel.
 * - Egyszerre több kliens (pl. több felületablak); az egyik változtatását a többi is megkapja ("sync").
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
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "net_compat.hpp"
#include "sha1_base64.hpp"
#include "spectrum.hpp"
#include "param_block.hpp"
#include "mini_json.hpp"
#include "ir_data.hpp"
#include "ee_dsp.hpp"
#include "self_path.hpp"

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
};

/** Az EQ profil-dokumentum maximális mérete (bájt). */
constexpr size_t kBridgeMaxPresetBytes = 2u << 20;

/** A GUI által küldött IR maximális hossza (frame). */
constexpr uint32_t kBridgeMaxIrFrames = 262144;

/** Egyszerre ennyi kliens (pl. több felületablak) csatlakozhat. */
constexpr size_t kBridgeMaxClients = 8;

/** A vezérlési protokoll verziója (a "state" üzenet "protocol" mezője). 2: több kliens, "sync". */
constexpr int kBridgeProtocol = 2;

/** Egy kliens küldési sora legfeljebb ekkora lehet; ha nem olvas, a kapcsolat bomlik (a többi kliens nem vár rá). */
constexpr size_t kBridgeMaxQueuedBytes = 16u << 20;

class WebSocketBridge {
public:
    explicit WebSocketBridge(int port = 8080) : port_(port) {}
    ~WebSocketBridge() { stop(); }

    WebSocketBridge(const WebSocketBridge&) = delete;
    WebSocketBridge& operator=(const WebSocketBridge&) = delete;

    bool start(BridgeCallbacks cb) {
        cb_ = std::move(cb);
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
        net::closeSocket(listener_);
        listener_ = net::kInvalidSocket;
        net::cleanup();
    }

    bool hasClient() const { return clientCount_.load() > 0; }

private:
    using clock = std::chrono::steady_clock;

    struct Client {
        net::socket_t sock = net::kInvalidSocket;
        std::string rx, frag;           // beérkező bájtok, töredezett üzenet
        uint8_t fragOpcode = 0;
        std::string out;                // küldésre váró bájtok (nem blokkoló küldés)
        size_t outPos = 0;
        uint64_t spectrumSeq = 0;       // az utoljára kapott spektrum sorszáma
        bool dead = false;
        const char* why = "";
    };

    /** Kézfogásra váró kapcsolat (a fejléc nem blokkolva, legfeljebb néhány másodpercig érkezik). */
    struct Pending {
        net::socket_t sock = net::kInvalidSocket;
        std::string buf;
        clock::time_point deadline;
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
            acceptNew(listener_);
            progressPending();

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
                if (freshAudio && now - lastSpectrum >= std::chrono::milliseconds(16)) {
                    const double sr = cb_.sampleRate ? cb_.sampleRate() : 48000.0;
                    analyzer.compute(sr > 0 ? sr : 48000.0);
                    spectrumMsg = buildSpectrumMessage(analyzer, sr);
                    ++spectrumSeq;
                    lastSpectrum = now;
                    freshAudio = false;
                }
                // Minden kliens egyszer kapja meg; ha még az előzőt sem olvasta, kimarad
                for (auto& c : clients_) {
                    if (c->spectrumSeq == spectrumSeq || spectrumMsg.empty()) continue;
                    queueText(*c, spectrumMsg, true);
                    c->spectrumSeq = spectrumSeq;
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

    void acceptNew(net::socket_t listener) {
        for (int k = 0; k < 4; ++k) {
            sockaddr_in ca{};
#ifdef _WIN32
            int len = sizeof(ca);
#else
            socklen_t len = sizeof(ca);
#endif
            net::socket_t s = ::accept(listener, reinterpret_cast<sockaddr*>(&ca), &len);
            if (s == net::kInvalidSocket) return;
            if (pending_.size() >= 8) { net::closeSocket(s); continue; }
            net::setNonBlocking(s, true);
            pending_.push_back({ s, {}, clock::now() + std::chrono::seconds(2) });
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
                if (!ok) net::closeSocket(p.sock);
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

    /** A kézfogás befejezése; true: a kapcsolat kliens lett (a socket hozzá került). */
    bool finishHandshake(Pending& p) {
        const std::string& request = p.buf;
        const std::string key = headerValue(request, "sec-websocket-key");
        if (key.empty()) { reply(p.sock, "426 Upgrade Required"); return false; }

        if (!originAllowed(headerValue(request, "origin"))) {
            // Csak a saját felület csatlakozhat: egy tetszőleges weboldal a böngészőből ne vezérelhesse a motort
            reply(p.sock, "403 Forbidden");
            return false;
        }

        if (clients_.size() >= kBridgeMaxClients) {
            // Az új felületnek helyet csinálunk (a legrégebbi kapcsolat bomlik, pl. egy újratöltött ablaké)
            kill(*clients_.front(), "új felület csatlakozott");
            removeDead();
        }

        const std::string magicGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        const std::string acceptKey = Base64::encode(Sha1::hash(key + magicGuid));
        auto c = std::make_unique<Client>();
        c->sock = p.sock;
        c->out = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: " + acceptKey + "\r\n\r\n";
        net::setNoDelay(c->sock);
        Client& cl = *c;
        clients_.push_back(std::move(c));
        clientCount_ = clients_.size();
        std::cout << "[WS Bridge] Grafikus felület csatlakozott.\n";

        cur_ = &cl;
        sendText(buildStateMessage());
        cur_ = nullptr;
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
        for (auto it = clients_.begin(); it != clients_.end();) {
            Client& c = **it;
            if (!c.dead) { ++it; continue; }
            net::closeSocket(c.sock);
            std::cout << "[WS Bridge] Grafikus felület lecsatlakozott (" << c.why << ").\n";
            it = clients_.erase(it);
        }
        clientCount_ = clients_.size();
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
     *        párbeszédablak, háttérbe tett ablak), kihagyjuk, és NEM bontjuk a kapcsolatot.
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
     * állapotot ("sync"), így több felületablak egyszerre is használható.
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

    // ------------------------------------------------------------------ protokoll
    void handleBinary(const std::string& data) {
        IrData ir;
        if (!parseIrMessage(data, ir, kBridgeMaxIrFrames)) {
            std::cout << "[WS Bridge] Érvénytelen IR üzenet (" << data.size() << " bájt).\n";
            return;
        }
        std::cout << "[WS Bridge] Impulzusválasz érkezett: " << ir.name << " (" << ir.frames() << " minta, "
                  << ir.sampleRate << " Hz, " << ir.channels() << " csatorna)\n";
        if (cb_.setIr) cb_.setIr(std::move(ir));
        broadcast(buildStateMessage("ir"));   // mindenki (a küldő is) az új IR-t látja
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
        if (type == "set_language") {
            // Csak rövid nyelvkód (a–z): a motor fájlba írja, és a tálcamenü is erre vált
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
            if (cb_.setIr) cb_.setIr(IrData{});
            broadcast(buildStateMessage("ir"));
            return;
        } else if (type == "get_eq_response") {
            sendText(buildEqResponseMessage(p, reqSeq));
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
        s += '}';
        return s;
    }

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
        if (cb_.getIr) {
            IrData ir = cb_.getIr();
            s += ",\"ir\":";
            if (ir.empty()) {
                s += "null";
            } else {
                s += "{\"name\":\"" + jsonEscape(ir.name) + "\",\"sr\":" + std::to_string(ir.sampleRate) +
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

    int port_;
    BridgeCallbacks cb_;
    std::atomic<bool> running_{false};
    std::atomic<size_t> clientCount_{0};
    std::thread thread_;
    net::socket_t listener_ = net::kInvalidSocket;
    std::vector<std::unique_ptr<Client>> clients_;
    std::vector<Pending> pending_;
    Client* cur_ = nullptr;             // akinek az üzenetét éppen feldolgozzuk
    bool syncPending_ = false, syncPresets_ = false;
    const Client* syncSource_ = nullptr;
    clock::time_point lastChange_{};
};

} // namespace ee_analyzer
