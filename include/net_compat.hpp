#pragma once
/**
 * @file net_compat.hpp
 * @brief Vékony socket-réteg: ugyanaz a kód fordul Linuxon (BSD socket) és Windowson (Winsock2).
 */
#include <cstdint>
#include <cstddef>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <unistd.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <poll.h>
  #include <cerrno>
#endif

namespace ee_analyzer::net {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

/** Winsock indítása (Linuxon üres). Folyamatonként egyszer hívandó. */
inline bool startup() {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

inline void cleanup() {
#ifdef _WIN32
    WSACleanup();
#endif
}

inline void closeSocket(socket_t s) {
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

inline bool setNonBlocking(socket_t s, bool on) {
#ifdef _WIN32
    u_long mode = on ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(s, F_SETFL, flags) == 0;
#endif
}

inline void setNoDelay(socket_t s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

/** Igaz, ha az utolsó hiba "próbáld újra" (nem blokkoló socket, nincs adat / tele puffer). */
inline bool wouldBlock() {
#ifdef _WIN32
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

/** Írható-e most a socket (azonnali, nem blokkoló ellenőrzés). */
inline bool writableNow(socket_t s) {
#ifdef _WIN32
    fd_set w;
    FD_ZERO(&w);
    FD_SET(s, &w);
    timeval tv{0, 0};
    return ::select(0, nullptr, &w, nullptr, &tv) > 0;
#else
    pollfd p{ s, POLLOUT, 0 };
    return ::poll(&p, 1, 0) > 0 && (p.revents & POLLOUT);
#endif
}

/** recv, ami mindkét platformon ptrdiff_t-t ad vissza. */
inline long long recvSome(socket_t s, char* buf, size_t len) {
    return static_cast<long long>(::recv(s, buf, static_cast<int>(len), 0));
}

inline long long sendSome(socket_t s, const char* buf, size_t len) {
#ifdef _WIN32
    return static_cast<long long>(::send(s, buf, static_cast<int>(len), 0));
#else
    return static_cast<long long>(::send(s, buf, len, MSG_NOSIGNAL));
#endif
}

} // namespace ee_analyzer::net
