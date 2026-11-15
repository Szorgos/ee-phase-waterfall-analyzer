#pragma once
/**
 * @file pair_cli.hpp
 * @brief Párosítás kijelző nélkül: `aqulify-engine --pair` (Linux) / `Aqulify.exe --pair` (Windows).
 *
 * A parancs a gépen már futó motorhoz kapcsolódik a helyi vezérlőcsatornán (WebSocket, 127.0.0.1), és:
 *  1. lekéri a távoli elérés állapotát (`get_remote`);
 *  2. ha a hálózati elérés ki van kapcsolva, bekapcsolja (`set_remote` enabled:1) – ezt ki is írja;
 *  3. új párosító kódot kér (`remote_new_code`), és kiírja a kódot, az érvényességét és a gép címeit.
 * A kódot a telefonos appban kell beírni (Új gép párosítása → Párosítás kóddal). Így SSH-n, kijelző nélkül is
 * párosítható.
 * A párosító kódot a motor csak helyi (127.0.0.1) kapcsolatnak adja ki.
 */
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#include "mini_json.hpp"
#include "net_compat.hpp"

namespace ee_analyzer::paircli {

/** Maszkolt szöveges WebSocket-keret (a kliens minden keretét maszkolja, RFC 6455 5.3). */
inline std::string encodeClientText(const std::string& text, const uint8_t mask[4]) {
    std::string f;
    f.push_back(static_cast<char>(0x81));
    const size_t n = text.size();
    if (n < 126) {
        f.push_back(static_cast<char>(0x80 | n));
    } else if (n <= 0xFFFF) {
        f.push_back(static_cast<char>(0x80 | 126));
        f.push_back(static_cast<char>((n >> 8) & 0xFF));
        f.push_back(static_cast<char>(n & 0xFF));
    } else {
        f.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i) f.push_back(static_cast<char>((static_cast<uint64_t>(n) >> (8 * i)) & 0xFF));
    }
    for (int i = 0; i < 4; ++i) f.push_back(static_cast<char>(mask[i]));
    for (size_t i = 0; i < n; ++i) f.push_back(static_cast<char>(text[i] ^ mask[i % 4]));
    return f;
}

/**
 * A szerver kereteinek bontása (maszkolatlan). push()-sal érkeznek a bájtok, next() adja a következő teljes
 * szöveges üzenetet (a nem szöveges kereteket kihagyja). Darabolt (folytatásos) üzenetet a motor nem küld.
 */
class FrameReader {
public:
    void push(const char* data, size_t n) { buf_.append(data, n); }

    /** true: egy teljes szöveges üzenet került az out-ba; false: még nincs elég adat. closed: záró keret jött. */
    bool next(std::string& out, bool* closed = nullptr) {
        for (;;) {
            if (buf_.size() < 2) return false;
            const int op = static_cast<unsigned char>(buf_[0]) & 0x0F;
            uint64_t len = static_cast<unsigned char>(buf_[1]) & 0x7F;
            size_t off = 2;
            if (len == 126) {
                if (buf_.size() < 4) return false;
                len = (static_cast<uint64_t>(static_cast<unsigned char>(buf_[2])) << 8) | static_cast<unsigned char>(buf_[3]);
                off = 4;
            } else if (len == 127) {
                if (buf_.size() < 10) return false;
                len = 0;
                for (int i = 2; i < 10; ++i) len = (len << 8) | static_cast<unsigned char>(buf_[i]);
                off = 10;
            }
            if (buf_.size() < off + len) return false;
            std::string payload = buf_.substr(off, static_cast<size_t>(len));
            buf_.erase(0, off + static_cast<size_t>(len));
            if (op == 0x8) { if (closed) *closed = true; return false; }
            if (op == 0x1) { out = std::move(payload); return true; }
            // bináris, ping, pong: kihagyjuk
        }
    }

private:
    std::string buf_;
};

/** A párosítás eredménye a motor "remote" üzenetéből. */
struct PairInfo {
    bool enabled = false, listening = false;
    int port = 0;
    std::string code;
    int codeSeconds = 0;
    std::vector<std::string> addresses;
};

/** A "remote" üzenet értelmezése; false, ha nem ilyen üzenet. */
inline bool parseRemote(const std::string& text, PairInfo& out) {
    json::Value v;
    if (!json::parse(text, v) || v.type != json::Value::Type::Object) return false;
    const json::Value* t = v.get("type");
    if (!t || t->stringOr("") != "remote") return false;
    auto flag = [&](const char* k) { const json::Value* x = v.get(k); return x && x->numberOr(0.0) != 0.0; };
    out.enabled = flag("enabled");
    out.listening = flag("listening");
    if (const json::Value* p = v.get("port")) out.port = static_cast<int>(p->numberOr(0.0));
    if (const json::Value* c = v.get("code")) out.code = c->stringOr("");
    if (const json::Value* s = v.get("codeSeconds")) out.codeSeconds = static_cast<int>(s->numberOr(0.0));
    out.addresses.clear();
    if (const json::Value* a = v.get("addresses"); a && a->type == json::Value::Type::Array)
        for (const auto& e : a->arr) if (e.type == json::Value::Type::String && !e.str.empty()) out.addresses.push_back(e.str);
    return true;
}

/** A kiírandó szöveg (a felhasználónak, magyarul). */
inline std::string formatPairInfo(const PairInfo& p, bool enabledNow) {
    std::string s;
    if (enabledNow) s += "A hálózati elérés ki volt kapcsolva: most bekapcsoltam (enélkül a telefon nem éri el a gépet).\n";
    s += "Párosító kód: " + p.code + "\n";
    if (p.codeSeconds > 0) s += "Érvényes még: " + std::to_string((p.codeSeconds + 59) / 60) + " percig.\n";
    if (!p.addresses.empty()) {
        // A jellemző otthoni tartományok elöl, a virtuális hálózati kártyák (172.16–31.) hátul – mint a felületen
        auto rank = [](const std::string& a) {
            if (a.rfind("192.168.", 0) == 0) return 0;
            if (a.rfind("10.", 0) == 0) return 1;
            if (a.rfind("172.", 0) == 0) { const int b = std::atoi(a.c_str() + 4); if (b >= 16 && b <= 31) return 3; }
            return 2;
        };
        std::vector<std::string> addrs = p.addresses;
        std::stable_sort(addrs.begin(), addrs.end(), [&](const std::string& x, const std::string& y) { return rank(x) < rank(y); });
        s += "A gép címe:";
        for (size_t i = 0; i < addrs.size() && i < 4; ++i) s += (i ? ", " : " ") + addrs[i] + ":" + std::to_string(p.port);
        s += "\n";
    }
    s += "A telefonon: Aqulify Remote → Új gép párosítása: a gép címe, a port, a 6 jegyű kód → Párosítás kóddal.\n";
    return s;
}

/** Kapcsolat a helyi motorhoz (127.0.0.1) a vezérlőcsatornán. */
class LocalClient {
public:
    ~LocalClient() { close(); }

    bool connect(int port, int timeoutMs = 2000) {
        close();
        s_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s_ == net::kInvalidSocket) return false;
        setTimeout(timeoutMs);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<uint16_t>(port));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(s_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { close(); return false; }
        const std::string req = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                "Sec-WebSocket-Key: QXF1bGlmeVBhaXJDTEkwMQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
        if (!sendAll(req)) { close(); return false; }
        std::string resp;
        char tmp[1024];
        while (resp.find("\r\n\r\n") == std::string::npos) {
            const long long n = ::recv(s_, tmp, sizeof(tmp), 0);
            if (n <= 0 || resp.size() > 8192) { close(); return false; }
            resp.append(tmp, static_cast<size_t>(n));
        }
        if (resp.rfind("HTTP/1.1 101", 0) != 0) { close(); return false; }
        const size_t end = resp.find("\r\n\r\n") + 4;
        if (end < resp.size()) reader_.push(resp.data() + end, resp.size() - end);
        return true;
    }

    bool sendText(const std::string& text) {
        const uint8_t mask[4] = { 0x5A, 0x3C, 0x96, 0xE1 };
        return sendAll(encodeClientText(text, mask));
    }

    /** A következő szöveges üzenet, legfeljebb a határidőig; false: nem jött (vagy bontás). */
    bool readText(std::string& out, std::chrono::steady_clock::time_point deadline) {
        for (;;) {
            bool closed = false;
            if (reader_.next(out, &closed)) return true;
            if (closed || std::chrono::steady_clock::now() >= deadline) return false;
            char tmp[16384];
            const long long n = ::recv(s_, tmp, sizeof(tmp), 0);
            if (n > 0) reader_.push(tmp, static_cast<size_t>(n));
            else if (n == 0 || !net::wouldBlock()) return false;
        }
    }

    void close() { if (s_ != net::kInvalidSocket) net::closeSocket(s_); s_ = net::kInvalidSocket; }

private:
    void setTimeout(int ms) {
#ifdef _WIN32
        DWORD tv = static_cast<DWORD>(ms);
#else
        timeval tv{ ms / 1000, (ms % 1000) * 1000 };
#endif
        setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        setsockopt(s_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    }
    bool sendAll(const std::string& d) {
        size_t off = 0;
        while (off < d.size()) {
            const long long n = net::sendSome(s_, d.data() + off, d.size() - off);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }
    net::socket_t s_ = net::kInvalidSocket;
    FrameReader reader_;
};

/** Az első "remote" üzenet a határidőig, amelyre a feltétel igaz. */
template <typename Pred>
inline bool waitRemote(LocalClient& c, PairInfo& info, int ms, Pred ok) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    std::string msg;
    while (c.readText(msg, deadline)) {
        PairInfo p;
        if (parseRemote(msg, p) && ok(p)) { info = p; return true; }
    }
    return false;
}

/**
 * A `--pair` parancs. Visszatérés: 0 kész (a kód kiírva), 1 a motor nem fut / nem érhető el, 2 a hálózati elérés
 * nem indult el (pl. foglalt port), 3 a motor nem adott kódot. A kimenet a megadott fájlba (alapból stdout).
 */
inline int run(const std::vector<int>& ports, const char* startHint, std::FILE* out = stdout) {
    if (!net::startup()) { std::fprintf(out, "A hálózat nem indítható.\n"); return 1; }
    LocalClient c;
    bool ok = false;
    for (int p : ports) if ((ok = c.connect(p))) break;
    if (!ok) {
        std::fprintf(out, "A motor nem fut ezen a gépen (vagy nem érhető el). %s\n", startHint ? startHint : "");
        net::cleanup();
        return 1;
    }
    c.sendText("{\"type\":\"subscribe\",\"spectrum\":0}");
    c.sendText("{\"type\":\"get_remote\"}");
    PairInfo info;
    if (!waitRemote(c, info, 5000, [](const PairInfo&) { return true; })) {
        std::fprintf(out, "A motor nem válaszolt (régebbi változat?).\n");
        net::cleanup();
        return 1;
    }
    bool enabledNow = false;
    if (!info.enabled) {
        c.sendText("{\"type\":\"set_remote\",\"enabled\":1}");
        enabledNow = true;
        if (!waitRemote(c, info, 5000, [](const PairInfo& p) { return p.enabled; })) {
            std::fprintf(out, "A hálózati elérést nem sikerült bekapcsolni.\n");
            net::cleanup();
            return 2;
        }
    }
    if (!info.listening) {
        std::fprintf(out, "A hálózati elérés be van kapcsolva, de a port (%d) nem nyitható meg.\n", info.port);
        net::cleanup();
        return 2;
    }
    c.sendText("{\"type\":\"remote_new_code\"}");
    if (!waitRemote(c, info, 5000, [](const PairInfo& p) { return !p.code.empty(); })) {
        std::fprintf(out, "A motor nem adott párosító kódot.\n");
        net::cleanup();
        return 3;
    }
    const std::string text = formatPairInfo(info, enabledNow);
    std::fputs(text.c_str(), out);
    std::fflush(out);
    c.close();
    net::cleanup();
    return 0;
}

} // namespace ee_analyzer::paircli
