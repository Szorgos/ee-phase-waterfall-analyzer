/**
 * @file aqulify_play.cpp
 * @brief A beépített lejátszó indítója a parancssorból (2.x, Linux / macOS):
 *
 *   aqulify-play <fájl>        a fájl lejátszása a motorban (a hang a DSP-láncon át szól)
 *   aqulify-play play | pause | toggle | stop
 *   aqulify-play seek <másodperc>
 *
 * A futó motor WebSocket-hídjához csatlakozik a saját gépen (127.0.0.1, ugyanazok a portok, mint a felületé), és a
 * motor protokolljának üzeneteit küldi (player_open, player_stop, media). Csak a saját gépről működik: a hálózatról
 * fájl nem nyitható meg. Windowson is (aqulify-play.exe, Winsock, Unicode útvonal).
 */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdlib.h>
using sock_t = SOCKET;
#define MSG_NOSIGNAL 0
static int sockClose(sock_t s) { return closesocket(s); }
static int pollOne(sock_t s, int timeoutMs) { WSAPOLLFD pf{ s, POLLRDNORM, 0 }; return WSAPoll(&pf, 1, timeoutMs); }
static const sock_t kBadSock = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static int sockClose(sock_t s) { return ::close(s); }
static int pollOne(sock_t s, int timeoutMs) { pollfd pf{ s, POLLIN, 0 }; return ::poll(&pf, 1, timeoutMs); }
static const sock_t kBadSock = -1;
#endif

#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

const int kPorts[] = { 47810, 47811, 47812, 47813, 47814, 8080 };

std::string jsonEscape(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += static_cast<char>(c);
    }
    return out;
}

std::string base64(const uint8_t* p, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t(p[i]) << 16) | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += i + 1 < n ? t[(v >> 6) & 63] : '=';
        out += i + 2 < n ? t[v & 63] : '=';
    }
    return out;
}

bool sendAll(sock_t fd, const void* data, size_t n) {
    const char* p = static_cast<const char*>(data);
    while (n > 0) {
        const auto w = ::send(fd, p, static_cast<int>(n), MSG_NOSIGNAL);
        if (w <= 0) return false;
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

/** Olvasás időkorláttal; false: időtúllépés vagy hiba. */
bool recvAll(sock_t fd, void* data, size_t n, int timeoutMs) {
    char* p = static_cast<char*>(data);
    while (n > 0) {
        if (pollOne(fd, timeoutMs) <= 0) return false;
        const auto r = ::recv(fd, p, static_cast<int>(n), 0);
        if (r <= 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

/** Csatlakozás és WebSocket-kézfogás egy porton; kBadSock: nem sikerült. */
sock_t connectPort(int port) {
    const sock_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kBadSock) return kBadSock;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { sockClose(fd); return kBadSock; }
    std::random_device rd;
    uint8_t key[16];
    for (auto& k : key) k = static_cast<uint8_t>(rd());
    const std::string req = "GET / HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + base64(key, 16) +
                            "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    if (!sendAll(fd, req.data(), req.size())) { sockClose(fd); return kBadSock; }
    std::string resp;
    char c;
    while (resp.size() < 4096 && resp.find("\r\n\r\n") == std::string::npos) {
        if (!recvAll(fd, &c, 1, 1500)) { sockClose(fd); return kBadSock; }
        resp += c;
    }
    if (resp.compare(0, 12, "HTTP/1.1 101") != 0) { sockClose(fd); return kBadSock; }
    return fd;
}

bool sendText(sock_t fd, const std::string& text) {
    std::string f;
    f += static_cast<char>(0x81);
    const size_t n = text.size();
    if (n < 126) f += static_cast<char>(0x80 | n);
    else if (n < 65536) { f += static_cast<char>(0x80 | 126); f += static_cast<char>(n >> 8); f += static_cast<char>(n & 0xff); }
    else { f += static_cast<char>(0x80 | 127); for (int i = 7; i >= 0; --i) f += static_cast<char>((uint64_t(n) >> (8 * i)) & 0xff); }
    std::random_device rd;
    uint8_t mask[4];
    for (auto& m : mask) m = static_cast<uint8_t>(rd());
    f.append(reinterpret_cast<const char*>(mask), 4);
    for (size_t i = 0; i < n; ++i) f += static_cast<char>(text[i] ^ mask[i & 3]);
    return sendAll(fd, f.data(), f.size());
}

/** A következő szöveges üzenet (a ping és más vezérlőkeretek kimaradnak); false: időtúllépés vagy lezárás. */
bool readText(sock_t fd, std::string& out, int timeoutMs) {
    for (;;) {
        uint8_t h[2];
        if (!recvAll(fd, h, 2, timeoutMs)) return false;
        uint64_t len = h[1] & 0x7f;
        if (len == 126) { uint8_t e[2]; if (!recvAll(fd, e, 2, timeoutMs)) return false; len = (e[0] << 8) | e[1]; }
        else if (len == 127) { uint8_t e[8]; if (!recvAll(fd, e, 8, timeoutMs)) return false; len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | e[i]; }
        if (len > (64u << 20)) return false;
        uint8_t mask[4] = {};
        if ((h[1] & 0x80) && !recvAll(fd, mask, 4, timeoutMs)) return false;
        std::string payload(len, '\0');
        if (len && !recvAll(fd, payload.data(), len, timeoutMs)) return false;
        if (h[1] & 0x80) for (size_t i = 0; i < len; ++i) payload[i] ^= static_cast<char>(mask[i & 3]);
        const int op = h[0] & 0x0f;
        if (op == 8) return false;
        if (op == 1) { out = std::move(payload); return true; }
    }
}

int usage() {
    std::fprintf(stderr,
                 "Használat: aqulify-play <fájl>            lejátszás a motorban (FLAC, WAV, MP3, Ogg Vorbis)\n"
                 "           aqulify-play play | pause | toggle | stop\n"
                 "           aqulify-play seek <másodperc>\n");
    return 2;
}

/** A teljes útvonal (UTF-8); üres: a fájl nem található. */
std::string absolutePath(const char* arg) {
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, arg, -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, arg, -1, w.data(), n);
    static wchar_t full[32768];   // hosszú (\\?\) útvonal is elfér
    if (!_wfullpath(full, w.c_str(), 32768) || GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) return {};
    const int m = WideCharToMultiByte(CP_UTF8, 0, full, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(m > 0 ? m : 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, full, -1, out.data(), m, nullptr, nullptr);
    out.resize(out.size() - 1);
    return out;
#else
    char abs[PATH_MAX];
    return ::realpath(arg, abs) ? std::string(abs) : std::string();
#endif
}

int run(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    std::string msg;
    bool expectResult = false;
    if (cmd == "play" || cmd == "pause" || cmd == "toggle") {
        msg = "{\"type\":\"media\",\"action\":\"" + cmd + "\"}";
    } else if (cmd == "seek") {
        if (argc < 3) return usage();
        msg = "{\"type\":\"media\",\"action\":\"seek\",\"position\":" + std::to_string(std::atof(argv[2])) + "}";
    } else if (cmd == "stop") {
        msg = "{\"type\":\"player_stop\"}";
        expectResult = true;
    } else if (cmd == "-h" || cmd == "--help") {
        return usage();
    } else {
        const std::string abs = absolutePath(argv[1]);
        if (abs.empty()) {
            std::fprintf(stderr, "Nem található: %s\n", argv[1]);
            return 1;
        }
        msg = "{\"type\":\"player_open\",\"path\":\"" + jsonEscape(abs) + "\"}";
        expectResult = true;
    }

    sock_t fd = kBadSock;
    for (int p : kPorts)
        if ((fd = connectPort(p)) != kBadSock) break;
    if (fd == kBadSock) {
        std::fprintf(stderr, "Nem fut az Aqulify motor (vagy nem érhető el a 127.0.0.1-en).\n");
        return 1;
    }
    if (!sendText(fd, msg)) { sockClose(fd); std::fprintf(stderr, "A parancs nem küldhető el.\n"); return 1; }
    int rc = 0;
    if (expectResult) {
        rc = 1;
        std::string text;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline && readText(fd, text, 5000)) {
            if (text.find("\"player_result\"") == std::string::npos) continue;
            if (text.find("\"ok\":true") != std::string::npos) rc = 0;
            else {
                const size_t e = text.find("\"error\":\"");
                std::fprintf(stderr, "A motor nem tudta lejátszani: %s\n",
                             e == std::string::npos ? text.c_str() : text.substr(e + 9, text.find('"', e + 9) - e - 9).c_str());
            }
            break;
        }
        if (rc != 0 && text.find("\"player_result\"") == std::string::npos)
            std::fprintf(stderr, "Nincs válasz: a futó motor nem 2.x-es (a beépített lejátszó a 2.x motorban van).\n");
    }
    sockClose(fd);
    return rc;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** wargv) {
    SetConsoleOutputCP(CP_UTF8);
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string a(static_cast<size_t>(n > 0 ? n : 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, a.data(), n, nullptr, nullptr);
        a.resize(a.size() - 1);
        args.push_back(std::move(a));
    }
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    const int rc = run(argc, argv.data());
    WSACleanup();
    return rc;
}
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
