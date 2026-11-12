#pragma once
/**
 * @file bt_link.hpp
 * @brief A távirányító Bluetooth-csatornája (RFCOMM): ugyanazok a JSON-üzenetek, mint a WebSocketen, soronként egy.
 *
 * - Keretezés: UTF-8, minden üzenet egy sor, "\n" zárja; egy sor legfeljebb kMaxLineBytes bájt.
 * - Hitelesítés: az első sor {"type":"hello","token":"<64 hex>",...}; a token ugyanaz, amit a telefon a Wi-Fi-s
 *   párosításkor kapott. Bluetoothon párosítani nem lehet.
 * - A szerver (RfcommListener) most csak Windowson van megvalósítva (Winsock AF_BTH, SDP-bejegyzés a kServiceUuid
 *   szolgáltatás-azonosítóval). Linuxon és macOS-en az open() false-t ad: ott nincs Bluetooth-csatorna.
 */
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "net_compat.hpp"
#include "mini_json.hpp"

#ifdef _WIN32
#include <ws2bth.h>
#include <bluetoothapis.h>
#endif

namespace ee_analyzer::btlink {

/** Egy beérkező sor legnagyobb mérete (bájt); hosszabb sornál a kapcsolat bomlik. */
constexpr size_t kMaxLineBytes = 1u << 20;
/** Ennyi idő alatt kell megérkeznie az első (hitelesítő) sornak. */
constexpr int kHelloTimeoutSeconds = 5;
/** Ennyi hibás token után kAuthLockSeconds másodpercig minden Bluetooth-kapcsolat azonnal bomlik. */
constexpr int kMaxAuthFails = 5;
constexpr int kAuthLockSeconds = 60;
/** A szolgáltatás azonosítója (SDP); a telefon ezzel kapcsolódik (createRfcommSocketToServiceRecord). */
constexpr const char* kServiceUuid = "43ad541b-d816-443a-a652-146cb5191139";

/** Bájtfolyam → sorok. A sorvégi "\r" lekerül, az üres sorok kimaradnak. */
class LineSplitter {
public:
    /** A kész sorok az out végére kerülnek. false: túl hosszú sor jött (a kapcsolatot bontani kell). */
    bool push(const char* data, size_t n, std::vector<std::string>& out) {
        size_t pos = 0;
        while (pos < n) {
            const void* nl = std::memchr(data + pos, '\n', n - pos);
            const size_t end = nl ? static_cast<size_t>(static_cast<const char*>(nl) - data) : n;
            if (buf_.size() + (end - pos) > kMaxLineBytes) return false;
            buf_.append(data + pos, end - pos);
            if (!nl) break;
            if (!buf_.empty() && buf_.back() == '\r') buf_.pop_back();
            if (!buf_.empty()) out.push_back(std::move(buf_));
            buf_.clear();
            pos = end + 1;
        }
        return true;
    }

private:
    std::string buf_;
};

/**
 * Egy kimenő üzenet sorrá alakítva. A JSON-ban a szövegen belüli sortörés mindig escape-elve van, így a nyers "\r" és
 * "\n" csak elemek közti szóköz lehet: szóközre cserélve az üzenet ugyanaz marad, és nem törik ketté.
 */
inline std::string toLine(const std::string& payload) {
    std::string s;
    s.reserve(payload.size() + 1);
    for (char ch : payload) s += (ch == '\n' || ch == '\r') ? ' ' : ch;
    s += '\n';
    return s;
}

/** Az első sor értelmezése. ok: "hello" üzenet, nem üres tokennel (a token érvényességét a híd ellenőrzi). */
struct Hello {
    bool ok = false;
    std::string token, app;
};

inline Hello parseHello(const std::string& line) {
    Hello h;
    json::Value v;
    if (!json::parse(line, v) || v.type != json::Value::Type::Object) return h;
    const json::Value* t = v.get("type");
    if (!t || t->stringOr("") != "hello") return h;
    if (const json::Value* tok = v.get("token")) h.token = tok->stringOr("");
    if (const json::Value* a = v.get("app")) h.app = a->stringOr("");
    h.ok = !h.token.empty();
    return h;
}

/** Tokenpróbálgatás elleni zár: kMaxAuthFails hibás próbálkozás után kAuthLockSeconds másodpercig zárva. */
class AuthGate {
public:
    using time_point = std::chrono::steady_clock::time_point;
    bool locked(time_point now) const { return now < lockUntil_; }
    /** Hibás próbálkozás; true, ha ezzel kezdődött a zárolás. */
    bool fail(time_point now) {
        if (++fails_ < kMaxAuthFails) return false;
        fails_ = 0;
        lockUntil_ = now + std::chrono::seconds(kAuthLockSeconds);
        return true;
    }
    void success() { fails_ = 0; }

private:
    int fails_ = 0;
    time_point lockUntil_{};
};

/** Bluetooth-cím szövegként: "AA:BB:CC:DD:EE:FF" (a 48 bites cím, a legfelső bájttal kezdve). */
inline std::string formatAddress(uint64_t a) {
    char s[18];
    std::snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X", static_cast<unsigned>((a >> 40) & 0xFF),
                  static_cast<unsigned>((a >> 32) & 0xFF), static_cast<unsigned>((a >> 24) & 0xFF),
                  static_cast<unsigned>((a >> 16) & 0xFF), static_cast<unsigned>((a >> 8) & 0xFF),
                  static_cast<unsigned>(a & 0xFF));
    return s;
}

/**
 * RFCOMM-szerver: a gép Bluetooth-rádióján figyel, és a szolgáltatást kServiceUuid azonosítóval bejegyzi (SDP), hogy a
 * telefon megtalálja. Csak rendszerszinten párosított (hitelesített) eszköz kapcsolódhat. A kapott socket nem blokkoló.
 */
class RfcommListener {
public:
    RfcommListener() = default;
    ~RfcommListener() { close(); }
    RfcommListener(const RfcommListener&) = delete;
    RfcommListener& operator=(const RfcommListener&) = delete;

    bool isOpen() const { return sock_ != net::kInvalidSocket; }
    /** A gép Bluetooth-címe ("AA:BB:…"); üres, ha nem sikerült megállapítani. */
    const std::string& address() const { return addr_; }

#ifdef _WIN32
    bool open() {
        close();
        net::socket_t s = ::socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
        if (s == net::kInvalidSocket) return false;   // nincs Bluetooth-rádió (vagy ki van kapcsolva)
        ULONG on = TRUE;
        if (setsockopt(s, SOL_RFCOMM, SO_BTH_AUTHENTICATE, reinterpret_cast<const char*>(&on), sizeof(on)) != 0)
            std::fprintf(stderr, "[BT] Figyelem: a hitelesítés kérése nem sikerült (%d).\n", WSAGetLastError());
        if (setsockopt(s, SOL_RFCOMM, SO_BTH_ENCRYPT, reinterpret_cast<const char*>(&on), sizeof(on)) != 0)
            std::fprintf(stderr, "[BT] Figyelem: a titkosítás kérése nem sikerült (%d).\n", WSAGetLastError());
        SOCKADDR_BTH sa{};
        sa.addressFamily = AF_BTH;
        sa.port = BT_PORT_ANY;
        if (::bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 || ::listen(s, 2) != 0) {
            net::closeSocket(s);
            return false;
        }
        int len = sizeof(local_);
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&local_), &len) != 0) {
            net::closeSocket(s);
            return false;
        }
        if (!setService(RNRSERVICE_REGISTER)) {
            std::fprintf(stderr, "[BT] A szolgáltatás bejegyzése (SDP) nem sikerült (%d).\n", WSAGetLastError());
            net::closeSocket(s);
            return false;
        }
        registered_ = true;
        net::setNonBlocking(s, true);
        sock_ = s;
        uint64_t a = local_.btAddr;
        if (a == 0) a = radioAddress();
        addr_ = a ? formatAddress(a) : std::string();
        return true;
    }

    void close() {
        if (registered_) setService(RNRSERVICE_DELETE);
        registered_ = false;
        broken_ = false;
        net::closeSocket(sock_);
        sock_ = net::kInvalidSocket;
        addr_.clear();
    }

    /** Egy várakozó kapcsolat (nem blokkoló); kInvalidSocket, ha nincs. Valódi hibánál a broken() igaz lesz. */
    net::socket_t acceptOne() {
        if (!isOpen()) return net::kInvalidSocket;
        SOCKADDR_BTH ca{};
        int len = sizeof(ca);
        net::socket_t c = ::accept(sock_, reinterpret_cast<sockaddr*>(&ca), &len);
        if (c != net::kInvalidSocket) net::setNonBlocking(c, true);
        else if (!net::wouldBlock()) broken_ = true;   // pl. a Bluetooth-verem újraindult (alvás után)
        return c;
    }

    /**
     * Az SDP-bejegyzés megújítása (törlés + új bejegyzés). A Windows a gép alvása után elfelejtheti a bejegyzést,
     * miközben a socket látszólag tovább figyel: a telefon ilyenkor nem találja a szolgáltatást. false, ha nem sikerült
     * (ilyenkor a hívó nyissa újra az egészet).
     */
    bool refresh() {
        if (!isOpen()) return false;
        if (registered_) setService(RNRSERVICE_DELETE);
        registered_ = setService(RNRSERVICE_REGISTER);
        return registered_;
    }

private:
    static GUID serviceGuid() {
        return GUID{ 0x43ad541b, 0xd816, 0x443a, { 0xa6, 0x52, 0x14, 0x6c, 0xb5, 0x19, 0x11, 0x39 } };
    }

    bool setService(WSAESETSERVICEOP op) {
        GUID g = serviceGuid();
        wchar_t name[] = L"Aqulify";
        CSADDR_INFO csa{};
        csa.LocalAddr.lpSockaddr = reinterpret_cast<sockaddr*>(&local_);
        csa.LocalAddr.iSockaddrLength = sizeof(local_);
        csa.iSocketType = SOCK_STREAM;
        csa.iProtocol = BTHPROTO_RFCOMM;
        WSAQUERYSETW qs{};
        qs.dwSize = sizeof(qs);
        qs.lpszServiceInstanceName = name;
        qs.lpServiceClassId = &g;
        qs.dwNameSpace = NS_BTH;
        qs.dwNumberOfCsAddrs = 1;
        qs.lpcsaBuffer = &csa;
        return WSASetServiceW(&qs, op, 0) == 0;
    }

    /** A rádió címe a rendszer Bluetooth-könyvtárából (dinamikusan betöltve), ha a socket címe nem adta meg. */
    static uint64_t radioAddress() {
        HMODULE h = LoadLibraryA("bthprops.cpl");
        if (!h) return 0;
        using FindFirst = HBLUETOOTH_RADIO_FIND(WINAPI*)(const BLUETOOTH_FIND_RADIO_PARAMS*, HANDLE*);
        using FindClose = BOOL(WINAPI*)(HBLUETOOTH_RADIO_FIND);
        using GetInfo = DWORD(WINAPI*)(HANDLE, PBLUETOOTH_RADIO_INFO);
        auto ff = reinterpret_cast<FindFirst>(reinterpret_cast<void*>(GetProcAddress(h, "BluetoothFindFirstRadio")));
        auto fc = reinterpret_cast<FindClose>(reinterpret_cast<void*>(GetProcAddress(h, "BluetoothFindRadioClose")));
        auto gi = reinterpret_cast<GetInfo>(reinterpret_cast<void*>(GetProcAddress(h, "BluetoothGetRadioInfo")));
        uint64_t a = 0;
        if (ff && fc && gi) {
            BLUETOOTH_FIND_RADIO_PARAMS p{ sizeof(BLUETOOTH_FIND_RADIO_PARAMS) };
            HANDLE radio = nullptr;
            if (HBLUETOOTH_RADIO_FIND f = ff(&p, &radio)) {
                BLUETOOTH_RADIO_INFO info{};
                info.dwSize = sizeof(info);
                if (gi(radio, &info) == ERROR_SUCCESS) a = info.address.ullLong;
                CloseHandle(radio);
                fc(f);
            }
        }
        FreeLibrary(h);
        return a;
    }

    SOCKADDR_BTH local_{};
    bool registered_ = false;
#else
    /** NINCS MEGVALÓSÍTVA ezen a platformon (Linux: a terv 2. lépése; macOS: nincs tervben). */
    bool open() { return false; }
    void close() {}
    net::socket_t acceptOne() { return net::kInvalidSocket; }
    bool refresh() { return false; }
#endif

public:
    /** A figyelő socket hibát adott (nem csak „nincs várakozó kapcsolat”): újra kell nyitni. */
    bool broken() const { return broken_; }

private:
    bool broken_ = false;
    net::socket_t sock_ = net::kInvalidSocket;
    std::string addr_;
};

} // namespace ee_analyzer::btlink
