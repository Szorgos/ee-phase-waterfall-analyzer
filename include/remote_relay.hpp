#pragma once
/**
 * @file remote_relay.hpp
 * @brief Távoli motorok párosítása, célpont-kezelése és WebSocket átjátszó (relay) réteg.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef _WIN32
#include <netdb.h>
#endif

#include "net_compat.hpp"
#include "mini_json.hpp"
#include "sha1_base64.hpp"

namespace ee_analyzer {

struct RemoteTarget {
    std::string id;       // Egyedi célazonosító
    std::string name;     // Felhasználóbarát név
    std::string host;     // Cím / hosztnév
    int port = 47820;     // Port
    std::string token;    // 64 karakteres titkos token (soha nem kerül kimenő üzenetbe!)
    std::string machine;  // Távoli gépazonosító
    long long last = 0;   // Utolsó sikeres kapcsolat időbélyege (epoch másodperc)
};

namespace relay_detail {

inline std::string urlEncode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else if (c == ' ') {
            out += '+';
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

inline std::string targetsToJson(const std::vector<RemoteTarget>& targets, bool includeToken) {
    std::string j = "{\"type\":\"targets\",\"targets\":[";
    for (size_t i = 0; i < targets.size(); ++i) {
        if (i) j += ",";
        const auto& t = targets[i];
        j += "{\"id\":\"" + media::jsonEscape(t.id) +
             "\",\"name\":\"" + media::jsonEscape(t.name) +
             "\",\"host\":\"" + media::jsonEscape(t.host) +
             "\",\"port\":" + std::to_string(t.port) +
             ",\"machine\":\"" + media::jsonEscape(t.machine) +
             "\",\"last\":" + std::to_string(t.last);
        if (includeToken) {
            j += ",\"token\":\"" + media::jsonEscape(t.token) + "\"";
        }
        j += "}";
    }
    j += "]}";
    return j;
}

inline std::vector<RemoteTarget> targetsFromJson(const std::string& str) {
    std::vector<RemoteTarget> out;
    if (str.empty()) return out;
    json::Value root;
    if (!json::parse(str, root) || root.type != json::Value::Type::Object) return out;
    const json::Value* arr = root.get("targets");
    if (!arr || arr->type != json::Value::Type::Array) return out;
    for (const auto& item : arr->arr) {
        if (item.type != json::Value::Type::Object) continue;
        RemoteTarget t;
        auto getStr = [&](const char* k) -> std::string {
            const json::Value* v = item.get(k);
            return v ? v->stringOr("") : "";
        };
        auto getNum = [&](const char* k, double def) -> double {
            const json::Value* v = item.get(k);
            return v ? v->numberOr(def) : def;
        };
        t.id = getStr("id");
        t.name = getStr("name");
        t.host = getStr("host");
        t.port = static_cast<int>(getNum("port", 47820));
        t.token = getStr("token");
        t.machine = getStr("machine");
        t.last = static_cast<long long>(getNum("last", 0));
        if (!t.id.empty() && !t.host.empty()) out.push_back(std::move(t));
    }
    return out;
}

inline void sendWsTextFrame(net::socket_t s, const std::string& text) {
    std::vector<uint8_t> f;
    f.push_back(0x81); // FIN + Opcode 1 (text)
    if (text.size() <= 125) {
        f.push_back(static_cast<uint8_t>(text.size()));
    } else if (text.size() <= 65535) {
        f.push_back(126);
        f.push_back(static_cast<uint8_t>((text.size() >> 8) & 0xFF));
        f.push_back(static_cast<uint8_t>(text.size() & 0xFF));
    } else {
        f.push_back(127);
        for (int i = 7; i >= 0; --i) f.push_back(static_cast<uint8_t>((text.size() >> (i * 8)) & 0xFF));
    }
    f.insert(f.end(), text.begin(), text.end());
    net::sendSome(s, reinterpret_cast<const char*>(f.data()), f.size());
}

inline net::socket_t connectTcp(const std::string& host, int port, int timeoutMs = 3000) {
    net::socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == net::kInvalidSocket) return net::kInvalidSocket;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        net::closeSocket(s);
        return net::kInvalidSocket;
    }
    std::memcpy(&addr, res->ai_addr, sizeof(addr));
    freeaddrinfo(res);

    net::setNonBlocking(s, true);
    int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    bool inProgress = false;
#ifdef _WIN32
    int lastErr = WSAGetLastError();
    inProgress = (lastErr == WSAEWOULDBLOCK || lastErr == WSAEINPROGRESS);
#else
    inProgress = (errno == EINPROGRESS || errno == EALREADY || errno == EAGAIN || errno == EWOULDBLOCK);
#endif
    if (rc < 0 && inProgress) {
        pollfd pfd{};
        pfd.fd = s;
        pfd.events = POLLOUT;
#ifdef _WIN32
        int pr = WSAPoll(&pfd, 1, timeoutMs);
#else
        int pr = ::poll(&pfd, 1, timeoutMs);
#endif
        if (pr <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            net::closeSocket(s);
            return net::kInvalidSocket;
        }
        int err = 0;
#ifdef _WIN32
        int len = sizeof(err);
#else
        socklen_t len = sizeof(err);
#endif
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) != 0 || err != 0) {
            net::closeSocket(s);
            return net::kInvalidSocket;
        }
    } else if (rc < 0) {
        net::closeSocket(s);
        return net::kInvalidSocket;
    }
    net::setNoDelay(s);
    return s;
}

inline bool readHttpHandshake(net::socket_t s, std::string& response, int timeoutMs = 3000) {
    response.clear();
    auto start = std::chrono::steady_clock::now();
    char buf[1024];
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(timeoutMs)) {
        long long n = net::recvSome(s, buf, sizeof(buf));
        if (n > 0) {
            response.append(buf, static_cast<size_t>(n));
            if (response.find("\r\n\r\n") != std::string::npos) return true;
        } else if (n == 0 || (n < 0 && !net::wouldBlock())) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

inline int parseHttpStatus(const std::string& response) {
    size_t sp1 = response.find(' ');
    if (sp1 == std::string::npos) return 0;
    size_t sp2 = response.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) sp2 = response.find("\r\n", sp1 + 1);
    if (sp2 == std::string::npos) return 0;
    try {
        return std::stoi(response.substr(sp1 + 1, sp2 - sp1 - 1));
    } catch (...) {
        return 0;
    }
}

inline bool readWsFrame(net::socket_t s, std::string& payload, std::string& buf, int timeoutMs = 3000) {
    payload.clear();
    auto start = std::chrono::steady_clock::now();
    char temp[1024];
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(timeoutMs)) {
        if (buf.size() >= 2) {
            uint8_t b1 = static_cast<uint8_t>(buf[1]);
            bool masked = (b1 & 0x80) != 0;
            size_t len = (b1 & 0x7F);
            size_t headSize = 2;
            if (len == 126) {
                if (buf.size() < 4) goto fetch;
                len = (static_cast<uint8_t>(buf[2]) << 8) | static_cast<uint8_t>(buf[3]);
                headSize = 4;
            } else if (len == 127) {
                if (buf.size() < 10) goto fetch;
                headSize = 10;
            }
            size_t maskSize = masked ? 4 : 0;
            size_t total = headSize + maskSize + len;
            if (buf.size() >= total) {
                const uint8_t* maskKey = masked ? reinterpret_cast<const uint8_t*>(buf.data() + headSize) : nullptr;
                const uint8_t* pdata = reinterpret_cast<const uint8_t*>(buf.data() + headSize + maskSize);
                payload.resize(len);
                for (size_t i = 0; i < len; ++i) {
                    payload[i] = static_cast<char>(masked ? (pdata[i] ^ maskKey[i % 4]) : pdata[i]);
                }
                buf.erase(0, total);
                return true;
            }
        }
fetch:
        long long n = net::recvSome(s, temp, sizeof(temp));
        if (n > 0) {
            buf.append(temp, static_cast<size_t>(n));
        } else if (n == 0 || (n < 0 && !net::wouldBlock())) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace relay_detail

class RemoteRelayManager {
public:
    struct RelayBridge {
        net::socket_t localSock = net::kInvalidSocket;
        net::socket_t remoteSock = net::kInvalidSocket;
        std::string localToRemoteBuf;
        std::string remoteToLocalBuf;
        std::string targetId;
        bool dead = false;
    };

    RemoteRelayManager() = default;
    ~RemoteRelayManager() { closeAll(); }

    void setStorage(std::function<std::string()> loadFn, std::function<bool(const std::string&)> saveFn) {
        loadFn_ = std::move(loadFn);
        saveFn_ = std::move(saveFn);
        loadTargets();
    }

    void loadTargets() {
        if (!loadFn_) return;
        const std::string doc = loadFn_();
        std::lock_guard<std::mutex> lk(targetsMtx_);
        targets_ = relay_detail::targetsFromJson(doc);
    }

    bool saveTargets() {
        if (!saveFn_) return false;
        std::string doc;
        {
            std::lock_guard<std::mutex> lk(targetsMtx_);
            doc = relay_detail::targetsToJson(targets_, true);
        }
        return saveFn_(doc);
    }

    std::vector<RemoteTarget> targets() const {
        std::lock_guard<std::mutex> lk(targetsMtx_);
        return targets_;
    }

    bool findTarget(const std::string& id, RemoteTarget& outTarget) const {
        std::lock_guard<std::mutex> lk(targetsMtx_);
        for (const auto& t : targets_) {
            if (t.id == id) {
                outTarget = t;
                return true;
            }
        }
        return false;
    }

    bool forgetTarget(const std::string& id) {
        closeRelaysForTarget(id);
        std::lock_guard<std::mutex> lk(targetsMtx_);
        const auto before = targets_.size();
        targets_.erase(std::remove_if(targets_.begin(), targets_.end(), [&](const RemoteTarget& t) { return t.id == id; }),
                       targets_.end());
        if (targets_.size() != before) {
            if (saveFn_) {
                const std::string doc = relay_detail::targetsToJson(targets_, true);
                saveFn_(doc);
            }
            return true;
        }
        return false;
    }

    /** Célgép párosítása a helyi motorból kliensként. */
    bool pairTarget(const std::string& host, int port, const std::string& code, const std::string& localName,
                    const std::string& localMachineId, RemoteTarget& outTarget, std::string& outError,
                    const std::string& clientName = "") {
        net::socket_t s = relay_detail::connectTcp(host, port);
        if (s == net::kInvalidSocket) {
            outError = "connection_failed";
            return false;
        }

        const std::string remoteReportedName = clientName.empty() ? localName : clientName;
        std::string secKey = "dGhlIHNhbXBsZSBub25jZQ==";
        std::string req = "GET /?pair=" + relay_detail::urlEncode(code) +
                          "&client=aqulify_desktop" +
                          "&name=" + relay_detail::urlEncode(remoteReportedName) +
                          "&app=" + relay_detail::urlEncode(localMachineId) + " HTTP/1.1\r\n" +
                          "Host: " + host + ":" + std::to_string(port) + "\r\n" +
                          "Upgrade: websocket\r\n" +
                          "Connection: Upgrade\r\n" +
                          "Sec-WebSocket-Key: " + secKey + "\r\n" +
                          "Sec-WebSocket-Version: 13\r\n\r\n";

        net::sendSome(s, req.data(), req.size());
        std::string resp;
        if (!relay_detail::readHttpHandshake(s, resp)) {
            net::closeSocket(s);
            outError = "handshake_timeout";
            return false;
        }

        int status = relay_detail::parseHttpStatus(resp);
        if (status == 401) {
            net::closeSocket(s);
            outError = "unauthorized";
            return false;
        }
        if (status == 429) {
            net::closeSocket(s);
            outError = "busy";
            return false;
        }
        if (status != 101) {
            net::closeSocket(s);
            outError = "http_error_" + std::to_string(status);
            return false;
        }

        size_t eoh = resp.find("\r\n\r\n");
        std::string leftovers = (eoh != std::string::npos && eoh + 4 < resp.size()) ? resp.substr(eoh + 4) : "";
        std::string framePayload;
        if (!relay_detail::readWsFrame(s, framePayload, leftovers)) {
            net::closeSocket(s);
            outError = "no_pair_frame";
            return false;
        }

        net::closeSocket(s);

        json::Value root;
        if (!json::parse(framePayload, root) || root.type != json::Value::Type::Object) {
            outError = "invalid_json";
            return false;
        }

        std::string type = root.get("type") ? root.get("type")->stringOr("") : "";
        if (type != "paired") {
            outError = "not_paired";
            return false;
        }

        std::string devId = root.get("id") ? root.get("id")->stringOr("") : "";
        std::string token = root.get("token") ? root.get("token")->stringOr("") : "";
        std::string machine = root.get("machine") ? root.get("machine")->stringOr("") : "";
        std::string computer = root.get("computer") ? root.get("computer")->stringOr("") : "";

        if (token.empty() || token.size() != 64) {
            outError = "invalid_token";
            return false;
        }

        outTarget.id = devId.empty() ? ("t_" + localMachineId.substr(0, 8)) : devId;
        outTarget.name = localName.empty() ? (computer.empty() ? "Távoli gép" : computer) : localName;
        outTarget.host = host;
        outTarget.port = port;
        outTarget.token = token;
        outTarget.machine = machine;
        outTarget.last = static_cast<long long>(std::time(nullptr));

        {
            std::lock_guard<std::mutex> lk(targetsMtx_);
            bool updated = false;
            for (auto& t : targets_) {
                if (t.id == outTarget.id || (!t.machine.empty() && t.machine == outTarget.machine)) {
                    t = outTarget;
                    updated = true;
                    break;
                }
            }
            if (!updated) targets_.push_back(outTarget);
        }
        saveTargets();
        return true;
    }

    /** Kapcsolódás távoli célhoz tokennel átjátszáshoz. */
    net::socket_t connectTarget(const RemoteTarget& target, std::string& leftovers, std::string& outError) {
        net::socket_t s = relay_detail::connectTcp(target.host, target.port);
        if (s == net::kInvalidSocket) {
            outError = "connection_failed";
            return net::kInvalidSocket;
        }

        std::string secKey = "dGhlIHNhbXBsZSBub25jZQ==";
        std::string req = "GET /?token=" + relay_detail::urlEncode(target.token) +
                          "&client=aqulify_desktop HTTP/1.1\r\n" +
                          "Host: " + target.host + ":" + std::to_string(target.port) + "\r\n" +
                          "Upgrade: websocket\r\n" +
                          "Connection: Upgrade\r\n" +
                          "Sec-WebSocket-Key: " + secKey + "\r\n" +
                          "Sec-WebSocket-Version: 13\r\n\r\n";

        net::sendSome(s, req.data(), req.size());
        std::string resp;
        if (!relay_detail::readHttpHandshake(s, resp)) {
            net::closeSocket(s);
            outError = "handshake_timeout";
            return net::kInvalidSocket;
        }

        int status = relay_detail::parseHttpStatus(resp);
        if (status == 401) {
            net::closeSocket(s);
            outError = "unauthorized";
            return net::kInvalidSocket;
        }
        if (status != 101) {
            net::closeSocket(s);
            outError = "http_error_" + std::to_string(status);
            return net::kInvalidSocket;
        }

        size_t eoh = resp.find("\r\n\r\n");
        leftovers = (eoh != std::string::npos && eoh + 4 < resp.size()) ? resp.substr(eoh + 4) : "";
        net::setNonBlocking(s, true);
        return s;
    }

    void addRelayBridge(net::socket_t localSock, net::socket_t remoteSock, const std::string& targetId, const std::string& remoteLeftovers) {
        RelayBridge rb;
        rb.localSock = localSock;
        rb.remoteSock = remoteSock;
        rb.targetId = targetId;
        rb.remoteToLocalBuf = remoteLeftovers;
        net::setNonBlocking(localSock, true);
        net::setNonBlocking(remoteSock, true);
        bridges_.push_back(std::move(rb));
    }

    void pumpRelays(size_t maxQueuedBytes) {
        char buf[8192];
        for (auto& b : bridges_) {
            if (b.dead) continue;

            // 1. Olvasás a helyi felületről -> továbbítás a távoli motornak
            long long n = net::recvSome(b.localSock, buf, sizeof(buf));
            if (n > 0) {
                b.localToRemoteBuf.append(buf, static_cast<size_t>(n));
            } else if (n == 0 || (n < 0 && !net::wouldBlock())) {
                b.dead = true;
            }

            // 2. Olvasás a távoli motorról -> továbbítás a helyi felületnek
            n = net::recvSome(b.remoteSock, buf, sizeof(buf));
            if (n > 0) {
                b.remoteToLocalBuf.append(buf, static_cast<size_t>(n));
            } else if (n == 0 || (n < 0 && !net::wouldBlock())) {
                b.dead = true;
            }

            // 3. Küldés a távoli motor felé
            if (!b.localToRemoteBuf.empty()) {
                long long s = net::sendSome(b.remoteSock, b.localToRemoteBuf.data(), b.localToRemoteBuf.size());
                if (s > 0) {
                    b.localToRemoteBuf.erase(0, static_cast<size_t>(s));
                } else if (s < 0 && !net::wouldBlock()) {
                    b.dead = true;
                }
            }

            // 4. Küldés a helyi felület felé
            if (!b.remoteToLocalBuf.empty()) {
                long long s = net::sendSome(b.localSock, b.remoteToLocalBuf.data(), b.remoteToLocalBuf.size());
                if (s > 0) {
                    b.remoteToLocalBuf.erase(0, static_cast<size_t>(s));
                } else if (s < 0 && !net::wouldBlock()) {
                    b.dead = true;
                }
            }

            if (b.localToRemoteBuf.size() > maxQueuedBytes || b.remoteToLocalBuf.size() > maxQueuedBytes) {
                b.dead = true;
            }
        }

        for (size_t i = 0; i < bridges_.size();) {
            if (bridges_[i].dead) {
                net::closeSocket(bridges_[i].localSock);
                net::closeSocket(bridges_[i].remoteSock);
                bridges_.erase(bridges_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    void closeRelaysForTarget(const std::string& targetId) {
        for (auto& b : bridges_) {
            if (b.targetId == targetId) b.dead = true;
        }
    }

    void closeAll() {
        for (auto& b : bridges_) {
            net::closeSocket(b.localSock);
            net::closeSocket(b.remoteSock);
        }
        bridges_.clear();
    }

    size_t activeRelayCount() const { return bridges_.size(); }

private:
    mutable std::mutex targetsMtx_;
    std::vector<RemoteTarget> targets_;
    std::vector<RelayBridge> bridges_;
    std::function<std::string()> loadFn_;
    std::function<bool(const std::string&)> saveFn_;
};

} // namespace ee_analyzer
