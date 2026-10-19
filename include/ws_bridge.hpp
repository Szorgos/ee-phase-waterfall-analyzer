#pragma once
/**
 * @file ws_bridge.hpp
 * @brief Könnyűsúlyú, platformfüggetlen C++ WebSocket híd (RFC 6455) a hangmotor és a webes GUI között.
 *
 * - Csak a 127.0.0.1 címen figyel (a helyi hálózat felől nem vezérelhető).
 * - A motor hangjából valódi FFT-spektrumot számol és ~60 FPS-sel küldi ("spectrum" üzenet).
 * - A GUI vezérlőüzeneteit (preamp, modulkapcsolók, EQ sávok) ParamData-vá alakítja.
 *
 * Ugyanez a kód fut Linuxon (PipeWire csomópont) és Windowson (AetherAudio.exe).
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net_compat.hpp"
#include "sha1_base64.hpp"
#include "spectrum.hpp"
#include "param_block.hpp"
#include "mini_json.hpp"

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
};

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
        if (!openListener()) {
            net::cleanup();
            return false;
        }
        running_ = true;
        thread_ = std::thread(&WebSocketBridge::run, this);
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
        net::closeSocket(client_);
        client_ = net::kInvalidSocket;
        net::closeSocket(listener_);
        listener_ = net::kInvalidSocket;
        net::cleanup();
    }

    bool hasClient() const { return clientConnected_.load(); }

private:
    // ------------------------------------------------------------------ hálózat
    bool openListener() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == net::kInvalidSocket) {
            std::cerr << "[WS Bridge] Hiba a socket létrehozásakor!\n";
            return false;
        }
#ifndef _WIN32
        int opt = 1;
        setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#else
        BOOL excl = TRUE; // Windowson ne lehessen "ellopni" a portot
        setsockopt(listener_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<uint16_t>(port_));
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            std::cerr << "[WS Bridge] A 127.0.0.1:" << port_ << " port foglalt (fut már egy példány?)\n";
            net::closeSocket(listener_);
            listener_ = net::kInvalidSocket;
            return false;
        }
        if (::listen(listener_, 4) != 0) {
            std::cerr << "[WS Bridge] Listen hiba!\n";
            net::closeSocket(listener_);
            listener_ = net::kInvalidSocket;
            return false;
        }
        net::setNonBlocking(listener_, true);
        std::cout << "[WS Bridge] Élő WebSocket híd: ws://127.0.0.1:" << port_ << "\n";
        return true;
    }

    void run() {
        using clock = std::chrono::steady_clock;
        SpectrumAnalyzer analyzer(4096, 160);
        std::vector<float> chunk(8192 * 2);
        auto lastSpectrum = clock::now();
        auto lastStatus = clock::now();
        bool freshAudio = false;

        while (running_) {
            acceptPending();

            // 1. Hang lehúzása a motorból (akkor is, ha nincs kliens, hogy ne torlódjon fel)
            if (cb_.pullAudio) {
                size_t got;
                while ((got = cb_.pullAudio(chunk.data(), chunk.size() / 2)) > 0) {
                    analyzer.push(chunk.data(), got);
                    freshAudio = true;
                }
            }

            if (client_ != net::kInvalidSocket) {
                const auto now = clock::now();
                if (freshAudio && now - lastSpectrum >= std::chrono::milliseconds(16)) {
                    const double sr = cb_.sampleRate ? cb_.sampleRate() : 48000.0;
                    analyzer.compute(sr > 0 ? sr : 48000.0);
                    sendText(buildSpectrumMessage(analyzer, sr));
                    lastSpectrum = now;
                    freshAudio = false;
                }
                if (now - lastStatus >= std::chrono::milliseconds(500)) {
                    sendText(buildStatusMessage());
                    lastStatus = now;
                }
                pumpIncoming();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    void acceptPending() {
        sockaddr_in ca{};
#ifdef _WIN32
        int len = sizeof(ca);
#else
        socklen_t len = sizeof(ca);
#endif
        net::socket_t s = ::accept(listener_, reinterpret_cast<sockaddr*>(&ca), &len);
        if (s == net::kInvalidSocket) return;

        net::setNonBlocking(s, false);
        if (!performHandshake(s)) {
            net::closeSocket(s);
            return;
        }
        net::setNonBlocking(s, true);
        net::setNoDelay(s);

        // Az új kliens váltja a régit (pl. GUI újratöltésekor)
        if (client_ != net::kInvalidSocket) net::closeSocket(client_);
        client_ = s;
        rxBuf_.clear();
        clientConnected_ = true;
        std::cout << "[WS Bridge] Grafikus felület csatlakozott.\n";
        sendText(buildStateMessage());
    }

    bool performHandshake(net::socket_t sock) {
        // Fejléc beolvasása (max 1 s, max 8 KB)
#ifdef _WIN32
        DWORD tv = 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
        timeval tv{1, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        std::string request;
        char buffer[2048];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
            const long long n = net::recvSome(sock, buffer, sizeof(buffer));
            if (n <= 0) return false;
            request.append(buffer, static_cast<size_t>(n));
        }

        std::string key = headerValue(request, "sec-websocket-key");
        if (key.empty()) {
            const char* resp = "HTTP/1.1 426 Upgrade Required\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            net::sendSome(sock, resp, std::char_traits<char>::length(resp));
            return false;
        }

        const std::string magicGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::vector<uint8_t> digest = Sha1::hash(key + magicGuid);
        std::string acceptKey = Base64::encode(digest);

        std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + acceptKey + "\r\n\r\n";
        return sendAll(sock, response.data(), response.size());
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

    void dropClient(const char* why) {
        if (client_ == net::kInvalidSocket) return;
        net::closeSocket(client_);
        client_ = net::kInvalidSocket;
        clientConnected_ = false;
        rxBuf_.clear();
        std::cout << "[WS Bridge] Grafikus felület lecsatlakozott (" << why << ").\n";
    }

    /** Teljes puffer elküldése nem blokkoló socketen is (rövid várakozással). */
    bool sendAll(net::socket_t s, const char* data, size_t len) {
        size_t sent = 0;
        int spins = 0;
        while (sent < len) {
            const long long n = net::sendSome(s, data + sent, len - sent);
            if (n > 0) { sent += static_cast<size_t>(n); spins = 0; continue; }
            if (n < 0 && net::wouldBlock() && ++spins < 200) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            return false;
        }
        return true;
    }

    void sendFrame(uint8_t opcode, const std::string& payload) {
        if (client_ == net::kInvalidSocket) return;
        std::string frame;
        frame.reserve(payload.size() + 10);
        frame.push_back(static_cast<char>(0x80 | opcode));
        const uint64_t len = payload.size();
        if (len <= 125) {
            frame.push_back(static_cast<char>(len));
        } else if (len <= 0xFFFF) {
            frame.push_back(126);
            frame.push_back(static_cast<char>((len >> 8) & 0xFF));
            frame.push_back(static_cast<char>(len & 0xFF));
        } else {
            frame.push_back(127);
            for (int i = 7; i >= 0; --i) frame.push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
        }
        frame += payload;
        if (!sendAll(client_, frame.data(), frame.size())) dropClient("küldési hiba");
    }

    void sendText(const std::string& text) { sendFrame(0x1, text); }

    /** Beérkező WebSocket keretek feldolgozása (töredezett TCP olvasással is). */
    void pumpIncoming() {
        char buf[4096];
        while (client_ != net::kInvalidSocket) {
            const long long n = net::recvSome(client_, buf, sizeof(buf));
            if (n > 0) {
                rxBuf_.append(buf, static_cast<size_t>(n));
                if (rxBuf_.size() > (1u << 20)) { dropClient("túl nagy üzenet"); return; }
                continue;
            }
            if (n == 0) { dropClient("kapcsolat bontva"); return; }
            if (net::wouldBlock()) break;
            dropClient("olvasási hiba");
            return;
        }

        while (client_ != net::kInvalidSocket) {
            if (rxBuf_.size() < 2) return;
            const uint8_t b0 = static_cast<uint8_t>(rxBuf_[0]);
            const uint8_t b1 = static_cast<uint8_t>(rxBuf_[1]);
            const uint8_t opcode = b0 & 0x0F;
            const bool masked = (b1 & 0x80) != 0;
            uint64_t len = b1 & 0x7F;
            size_t hdr = 2;
            if (len == 126) {
                if (rxBuf_.size() < 4) return;
                len = (static_cast<uint8_t>(rxBuf_[2]) << 8) | static_cast<uint8_t>(rxBuf_[3]);
                hdr = 4;
            } else if (len == 127) {
                if (rxBuf_.size() < 10) return;
                len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<uint8_t>(rxBuf_[2 + i]);
                hdr = 10;
            }
            if (len > (1u << 20)) { dropClient("túl nagy keret"); return; }
            const size_t maskLen = masked ? 4 : 0;
            if (rxBuf_.size() < hdr + maskLen + len) return;

            std::string payload = rxBuf_.substr(hdr + maskLen, static_cast<size_t>(len));
            if (masked) {
                const char* m = rxBuf_.data() + hdr;
                for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= m[i % 4];
            }
            rxBuf_.erase(0, hdr + maskLen + static_cast<size_t>(len));

            switch (opcode) {
                case 0x1: handleMessage(payload); break;       // szöveg
                case 0x8: sendFrame(0x8, ""); dropClient("close keret"); return;
                case 0x9: sendFrame(0xA, payload); break;      // ping -> pong
                default: break;                                 // bináris / folytatás: figyelmen kívül
            }
        }
    }

    // ------------------------------------------------------------------ protokoll
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
        if (!cb_.getParams || !cb_.setParams) return;

        ParamData p = cb_.getParams();
        bool changed = true;

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
            else changed = false;
        } else if (type == "set_crossfeed_preset" && val) {
            p.crossfeedPreset = static_cast<int32_t>(val->numberOr(0.0));
        } else if (type == "set_eq") {
            const json::Value* bands = msg.get("bands");
            if (!bands || bands->type != json::Value::Type::Array) return;
            p.numBands = 0;
            for (const auto& b : bands->arr) {
                if (p.numBands >= kMaxEqBands) break;
                EqBandPod& out = p.bands[p.numBands++];
                const json::Value* t = b.get("type");
                out.type = eqTypeFromName(t ? t->stringOr("peaking").c_str() : "peaking");
                out.freq = static_cast<float>(b.get("freq") ? b.get("freq")->numberOr(1000.0) : 1000.0);
                out.gainDb = static_cast<float>(b.get("gain") ? b.get("gain")->numberOr(0.0) : 0.0);
                out.q = static_cast<float>(b.get("q") ? b.get("q")->numberOr(0.7071) : 0.7071);
            }
        } else {
            changed = false;
        }

        if (changed) {
            p.set(kFlagInitialized, true);
            cb_.setParams(p);
        }
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
        s += "]}";
        return s;
    }

    std::string buildStatusMessage() const {
        std::string s = "{\"type\":\"status\"";
        if (cb_.statusFields) {
            std::string extra = cb_.statusFields();
            if (!extra.empty()) { s += ','; s += extra; }
        }
        s += '}';
        return s;
    }

    std::string buildStateMessage() const {
        ParamData p = cb_.getParams ? cb_.getParams() : ParamData{};
        std::string s = "{\"type\":\"state\",\"initialized\":";
        s += p.has(kFlagInitialized) ? "true" : "false";
        s += ",\"preamp\":";
        appendFloat(s, p.preampDb, 2);
        s += ",\"modules\":{";
        s += "\"eq\":"; s += p.has(kFlagEq) ? "1" : "0";
        s += ",\"crossfeed\":"; s += p.has(kFlagCrossfeed) ? "1" : "0";
        s += ",\"convolver\":"; s += p.has(kFlagConvolver) ? "1" : "0";
        s += ",\"limiter\":"; s += p.has(kFlagLimiter) ? "1" : "0";
        s += ",\"bass\":"; s += p.has(kFlagBass) ? "1" : "0";
        s += "},\"crossfeedPreset\":" + std::to_string(p.crossfeedPreset);
        s += ",\"bands\":[";
        const uint32_t n = std::min(p.numBands, kMaxEqBands);
        for (uint32_t i = 0; i < n; ++i) {
            if (i) s += ',';
            s += "{\"type\":\"";
            s += eqTypeName(p.bands[i].type);
            s += "\",\"freq\":"; appendFloat(s, p.bands[i].freq, 2);
            s += ",\"gain\":"; appendFloat(s, p.bands[i].gainDb, 2);
            s += ",\"q\":"; appendFloat(s, p.bands[i].q, 3);
            s += '}';
        }
        s += "]}";
        return s;
    }

    int port_;
    BridgeCallbacks cb_;
    std::atomic<bool> running_{false};
    std::atomic<bool> clientConnected_{false};
    std::thread thread_;
    net::socket_t listener_ = net::kInvalidSocket;
    net::socket_t client_ = net::kInvalidSocket;
    std::string rxBuf_;
};

} // namespace ee_analyzer
