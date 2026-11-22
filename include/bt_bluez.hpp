#pragma once
/**
 * @file bt_bluez.hpp
 * @brief Linux Bluetooth RFCOMM szerver a BlueZ-en át (D-Bus: org.bluez.ProfileManager1 / Profile1).
 *
 * Működés: a motor a rendszerbuszon regisztrál egy RFCOMM-profilt (RegisterProfile, a telefon ugyanazzal a UUID-vel
 * keres: btlink::kServiceUuid). A BlueZ a SDP-bejegyzést és a figyelő socketet kezeli; minden bejövő kapcsolatnál
 * meghívja a NewConnection metódust, amelyben egy kész, csatlakozott RFCOMM socket fájlleírója érkezik (SCM_RIGHTS).
 * Ezt a bt_link.hpp RfcommListener::acceptOne() adja a hídnak: onnantól ugyanaz a soros protokoll fut, mint Windowson
 * (első soros token, soronkénti JSON). A rendszerszintű párosítás kötelező (RequireAuthentication), az alkalmazás
 * szintű hitelesítés a token; a BlueZ-os külön engedélykérést (RequireAuthorization) kikapcsoljuk, mert fej nélküli
 * gépen nincs, aki jóváhagyja.
 *
 * Három réteg, hogy Bluetooth és D-Bus nélkül is tesztelhető legyen:
 *  1. dbus_wire.hpp: üzenetkódolás (platformfüggetlen).
 *  2. ProfileSession: a regisztráció és a hívások állapotgépe; üzeneteket kap, és üzeneteket ad vissza (platformfüggetlen).
 *  3. UnixBusIo + BluezListener: a Unix-socketes szállítás és a szál (csak Linux). A szállítás cserélhető (BusIo).
 * Külső könyvtárfüggőség nincs (a D-Bus protokollt a dbus_wire.hpp valósítja meg), ahogy az MPRIS-nél sincs.
 */
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dbus_wire.hpp"

#if defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace ee_analyzer::bluez {

/** A profil objektumútvonala a mi oldalunkon (a BlueZ ide hívja a NewConnection-t). */
constexpr const char* kProfilePath = "/org/aqulify/bluetooth";
/**
 * RFCOMM-csatorna. A BlueZ csak akkor nyit figyelő socketet egyedi UUID-hez, ha a Channel meg van adva. A telefon SDP-n
 * át, UUID alapján találja meg, így a szám nem fontos, csak ne ütközzön a BlueZ beépített profiljaival (1, 3, 6, 7, 9–17).
 */
constexpr uint16_t kRfcommChannel = 22;
constexpr const char* kProfileName = "Aqulify";
/** Legfeljebb ennyi még át nem vett bejövő kapcsolat várakozhat (a felesleg bontva). */
constexpr size_t kMaxQueuedConnections = 8;
/** A BlueZ-hoz tartozó adapterek, amelyek címét megkérdezzük (hci0..hci3). */
constexpr int kMaxAdapterIndex = 3;

enum class State {
    Idle,          ///< még nem indult
    Hello,         ///< a buszra köszönés elküldve
    AddMatch,      ///< a BlueZ eltűnésének figyelése
    Register,      ///< RegisterProfile elküldve
    Adapter,       ///< az adapter (és címének) keresése
    Ready,         ///< regisztrálva; a telefon kapcsolódhat
    WaitingBluez,  ///< a BlueZ elment / elengedett; visszatérésekor újraregisztrál
    Failed         ///< végleges hiba (lásd failure()); a hívó nyissa újra
};

/** A BlueZ/D-Bus hibanevéből ember által olvasható ok (a naplóba és a hibajelzésbe). */
inline std::string describeError(const std::string& name, const std::string& text) {
    if (name == "org.freedesktop.DBus.Error.ServiceUnknown" || name == "org.freedesktop.DBus.Error.NameHasNoOwner")
        return "a BlueZ (bluetoothd) nem fut";
    if (name == "org.freedesktop.DBus.Error.AccessDenied")
        return "a D-Bus nem engedi a BlueZ elérését (a felhasználó nincs a „bluetooth” csoportban?)";
    if (name == "org.bluez.Error.AlreadyExists")
        return "ezt a profilt már regisztrálta egy másik program (fut másik Aqulify?)";
    if (name == "org.freedesktop.DBus.Error.NoReply" || name == "org.freedesktop.DBus.Error.Timeout")
        return "a BlueZ nem válaszolt";
    return name + (text.empty() ? std::string() : ": " + text);
}

/**
 * A regisztráció és a bejövő hívások állapotgépe. Nem ismer szálat, socketet vagy órát: üzenetet kap (onMessage),
 * és a kapott SendFn-nel üzenetet küld. A fájlleírók tulajdonjoga az átvétellel a példányé; a lezárásukat a CloseFn végzi.
 */
class ProfileSession {
public:
    using SendFn = std::function<bool(const dbus::Message&)>;
    using CloseFn = std::function<void(int)>;
    using Clock = std::chrono::steady_clock;

    ProfileSession(std::string uuid, SendFn send, CloseFn closeFd)
        : uuid_(std::move(uuid)), send_(std::move(send)), closeFd_(std::move(closeFd)) {}
    ~ProfileSession() { drain(); }
    ProfileSession(const ProfileSession&) = delete;
    ProfileSession& operator=(const ProfileSession&) = delete;

    State state() const { return state_; }
    std::string failure() const { std::lock_guard<std::mutex> lk(mu_); return failure_; }
    std::string address() const { std::lock_guard<std::mutex> lk(mu_); return address_; }
    bool unregistered() const { return unregistered_; }

    /** A busz kapcsolat felépült: a köszönéssel indul a sor. */
    void start() {
        state_ = State::Hello;
        call(Purpose::Hello, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello", "", {});
    }

    /** Új üzenet a buszról. */
    void onMessage(dbus::Message& m) {
        std::vector<int> fds = std::move(m.fds);
        m.fds.clear();
        switch (m.type) {
            case dbus::MsgType::MethodReturn:
            case dbus::MsgType::Error: onReply(m); break;
            case dbus::MsgType::Signal: onSignal(m); break;
            case dbus::MsgType::MethodCall: onCall(m, fds); break;
        }
        for (int fd : fds) closeFd_(fd);   // amit a hívás nem vett át
    }

    /** Időzített teendők (a szál másodpercenként hívja): a BlueZ újraindulása utáni újraregisztráció ismétlése. */
    void tick(Clock::time_point now) {
        if (retryPending_ && now >= retryAt_) {
            retryPending_ = false;
            sendRegister();
        }
    }

    /** Tiszta leállás: a profil törlése (a válasz nem kötelező; a busz lezárása is elég a BlueZ-nak). */
    void shutdown() {
        if (!registered_) return;
        registered_ = false;
        dbus::Writer b;
        b.str(kProfilePath);
        call(Purpose::Unregister, "org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "UnregisterProfile", "o", b.bytes());
    }

    /** Egy átvehető bejövő kapcsolat (a fájlleíró tulajdonjoga a hívóé); false, ha nincs. */
    bool popConnection(int& fd) {
        std::lock_guard<std::mutex> lk(mu_);
        if (queue_.empty()) return false;
        fd = queue_.front();
        queue_.pop_front();
        return true;
    }

    /** A még át nem vett kapcsolatok lezárása. */
    void drain() {
        std::deque<int> q;
        { std::lock_guard<std::mutex> lk(mu_); q.swap(queue_); }
        for (int fd : q) closeFd_(fd);
    }

    /** Hány hívás vár még válaszra (tesztekhez). */
    size_t pendingCalls() const { return pending_.size(); }

private:
    enum class Purpose { Hello, AddMatch, Register, Adapter, Unregister };
    struct Pending { Purpose purpose; };

    void call(Purpose p, const std::string& dest, const std::string& path, const std::string& iface,
              const std::string& member, const std::string& sig, const std::vector<uint8_t>& body) {
        dbus::Message m;
        m.type = dbus::MsgType::MethodCall;
        m.serial = nextSerial_++;
        m.dest = dest;
        m.path = path;
        m.iface = iface;
        m.member = member;
        m.signature = sig;
        m.body = body;
        // A BlueZ-t nem indítjuk el mellékhatásként: ha a rendszeren nem fut, a Bluetooth-csatorna egyszerűen nem érhető el
        if (dest == "org.bluez") m.flags |= dbus::kFlagNoAutoStart;
        pending_[m.serial] = Pending{ p };
        if (!send_(m)) fail("a D-Bus kapcsolatra nem sikerült írni");
    }

    void fail(const std::string& why) {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ == State::Failed) return;
        failure_ = why;
        state_ = State::Failed;
    }

    void sendRegister() {
        state_ = State::Register;
        dbus::Writer b;
        b.str(kProfilePath);
        b.str(uuid_);
        b.dict([&](dbus::Writer& w) {
            w.dictEntryStr("Name", kProfileName);
            w.dictEntryStr("Role", "server");
            w.dictEntryU16("Channel", kRfcommChannel);
            w.dictEntryBool("RequireAuthentication", true);   // csak párosított eszköz (mint Windowson)
            w.dictEntryBool("RequireAuthorization", false);   // fej nélküli gépen nincs, aki jóváhagyná
        });
        call(Purpose::Register, "org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "RegisterProfile", "osa{sv}",
             b.bytes());
    }

    void sendAdapter() {
        dbus::Writer b;
        b.str("org.bluez.Adapter1");
        b.str("Address");
        call(Purpose::Adapter, "org.bluez", "/org/bluez/hci" + std::to_string(adapterIdx_),
             "org.freedesktop.DBus.Properties", "Get", "ss", b.bytes());
    }

    static std::string errorText(const dbus::Message& m) {
        if (m.signature.empty() || m.signature[0] != 's') return {};
        dbus::Reader r(m.body.data(), m.body.size(), m.bigEndian);
        return r.str();
    }

    void onReply(const dbus::Message& m) {
        auto it = pending_.find(m.replySerial);
        if (it == pending_.end()) return;
        const Purpose p = it->second.purpose;
        pending_.erase(it);
        const bool err = m.type == dbus::MsgType::Error;
        switch (p) {
            case Purpose::Hello:
                if (err) { fail("a D-Bus köszönés sikertelen: " + describeError(m.errorName, errorText(m))); return; }
                state_ = State::AddMatch;
                {
                    dbus::Writer b;
                    b.str("type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
                          "member='NameOwnerChanged',arg0='org.bluez'");
                    call(Purpose::AddMatch, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                         "AddMatch", "s", b.bytes());
                }
                break;
            case Purpose::AddMatch:
                // Hiba esetén is megyünk tovább: a BlueZ újraindulását ilyenkor nem vesszük észre (a híd 30 mp-es
                // karbantartása akkor újranyitja a csatornát, mert az isOpen() hamissá válik vagy a kapcsolat bomlik).
                sendRegister();
                break;
            case Purpose::Register:
                if (err) {
                    const std::string why = describeError(m.errorName, errorText(m));
                    if (reregistering_ && m.errorName != "org.freedesktop.DBus.Error.AccessDenied" && ++retries_ <= 5) {
                        // A BlueZ épp elindult, a ProfileManager még nem áll készen: pár másodperc múlva újra
                        state_ = State::WaitingBluez;
                        retryPending_ = true;
                        retryAt_ = Clock::now() + std::chrono::seconds(2);
                        return;
                    }
                    fail("a profil regisztrálása nem sikerült: " + why);
                    return;
                }
                registered_ = true;
                retries_ = 0;
                if (reregistering_) { reregistering_ = false; state_ = State::Ready; return; }
                adapterIdx_ = 0;
                state_ = State::Adapter;
                sendAdapter();
                break;
            case Purpose::Adapter:
                if (err) {
                    if (++adapterIdx_ <= kMaxAdapterIndex) { sendAdapter(); return; }
                    fail("nincs Bluetooth-adapter (a BlueZ nem lát rádiót)");
                    return;
                }
                {
                    dbus::Reader r(m.body.data(), m.body.size(), m.bigEndian);
                    const std::string vs = r.sig();   // változat: a belső típus aláírása
                    const std::string addr = vs == "s" ? r.str() : std::string();
                    if (!r.ok() || addr.empty()) { fail("az adapter címe nem olvasható"); return; }
                    std::lock_guard<std::mutex> lk(mu_);
                    address_ = addr;
                    state_ = State::Ready;
                }
                break;
            case Purpose::Unregister:
                unregistered_ = true;
                break;
        }
    }

    void onSignal(const dbus::Message& m) {
        if (m.iface != "org.freedesktop.DBus" || m.member != "NameOwnerChanged") return;
        dbus::Reader r(m.body.data(), m.body.size(), m.bigEndian);
        const std::string name = r.str();
        const std::string oldOwner = r.str();
        const std::string newOwner = r.str();
        if (!r.ok() || name != "org.bluez") return;
        (void)oldOwner;
        if (newOwner.empty()) {
            // A BlueZ elment: a regisztrációnk vele együtt elveszett
            registered_ = false;
            retryPending_ = false;
            if (state_ == State::Ready || state_ == State::Register || state_ == State::Adapter) state_ = State::WaitingBluez;
        } else if (state_ == State::WaitingBluez) {
            reregistering_ = true;
            retries_ = 0;
            sendRegister();
        }
    }

    void reply(const dbus::Message& req, bool ok, const std::string& errName = {}, const std::string& errMsg = {}) {
        if (req.flags & dbus::kFlagNoReplyExpected) return;
        dbus::Message r;
        r.type = ok ? dbus::MsgType::MethodReturn : dbus::MsgType::Error;
        r.serial = nextSerial_++;
        r.replySerial = req.serial;
        r.dest = req.sender;
        if (!ok) {
            r.errorName = errName;
            r.signature = "s";
            dbus::Writer b;
            b.str(errMsg);
            r.body = b.bytes();
        }
        send_(r);
    }

    void onCall(const dbus::Message& m, std::vector<int>& fds) {
        if (m.iface == "org.freedesktop.DBus.Peer" && m.member == "Ping") { reply(m, true); return; }
        if (m.path != kProfilePath || m.iface != "org.bluez.Profile1") {
            reply(m, false, "org.freedesktop.DBus.Error.UnknownMethod", "ismeretlen objektum vagy felület");
            return;
        }
        if (m.member == "NewConnection") {
            dbus::Reader r(m.body.data(), m.body.size(), m.bigEndian);
            const std::string device = r.str();
            const uint32_t idx = r.u32();
            if (m.signature.compare(0, 2, "oh") != 0 || !r.ok() || idx >= fds.size()) {
                reply(m, false, "org.freedesktop.DBus.Error.InvalidArgs", "NewConnection: hibás argumentumok");
                return;
            }
            const int fd = fds[idx];
            fds[idx] = -1;
            bool queued = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (queue_.size() < kMaxQueuedConnections) { queue_.push_back(fd); queued = true; }
            }
            if (!queued) closeFd_(fd);
            fds.erase(std::remove(fds.begin(), fds.end(), -1), fds.end());
            reply(m, true);
        } else if (m.member == "RequestDisconnection") {
            // A BlueZ kéri a kapcsolat bontását; a kapcsolatot a híd kezeli, ő bontja, ha a telefon elmegy
            reply(m, true);
        } else if (m.member == "Release") {
            // A BlueZ elengedett (leáll); a regisztrációnk megszűnt
            registered_ = false;
            if (state_ == State::Ready) state_ = State::WaitingBluez;
            reply(m, true);
        } else {
            reply(m, false, "org.freedesktop.DBus.Error.UnknownMethod", "ismeretlen metódus");
        }
    }

    std::string uuid_;
    SendFn send_;
    CloseFn closeFd_;
    std::atomic<State> state_{ State::Idle };
    mutable std::mutex mu_;
    std::string failure_, address_;
    std::deque<int> queue_;
    std::map<uint32_t, Pending> pending_;
    uint32_t nextSerial_ = 1;
    int adapterIdx_ = 0;
    bool registered_ = false;
    std::atomic<bool> unregistered_{ false };
    bool reregistering_ = false;
    bool retryPending_ = false;
    int retries_ = 0;
    Clock::time_point retryAt_{};
};

/** Egy D-Bus-cím (a DBUS_SYSTEM_BUS_ADDRESS formátuma) egy Unix-socketes végpontja. */
struct BusEndpoint {
    bool abstract = false;
    std::string path;
};

/** "unix:path=/x,guid=…;unix:abstract=y" → végpontok (százalékkódolás feloldva). Más szállítást kihagy. */
inline std::vector<BusEndpoint> parseBusAddress(const std::string& address) {
    std::vector<BusEndpoint> out;
    size_t pos = 0;
    while (pos <= address.size()) {
        size_t semi = address.find(';', pos);
        if (semi == std::string::npos) semi = address.size();
        const std::string one = address.substr(pos, semi - pos);
        pos = semi + 1;
        if (one.compare(0, 5, "unix:") != 0) continue;
        BusEndpoint ep;
        bool have = false;
        size_t p = 5;
        while (p <= one.size()) {
            size_t comma = one.find(',', p);
            if (comma == std::string::npos) comma = one.size();
            const std::string kv = one.substr(p, comma - p);
            p = comma + 1;
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = kv.substr(0, eq);
            if (key != "path" && key != "abstract") continue;
            std::string val;
            for (size_t i = eq + 1; i < kv.size(); ++i) {
                if (kv[i] == '%' && i + 2 < kv.size() && std::isxdigit(static_cast<unsigned char>(kv[i + 1])) &&
                    std::isxdigit(static_cast<unsigned char>(kv[i + 2]))) {
                    val += static_cast<char>(std::strtol(kv.substr(i + 1, 2).c_str(), nullptr, 16));
                    i += 2;
                } else val += kv[i];
            }
            ep.abstract = key == "abstract";
            ep.path = val;
            have = !val.empty();
        }
        if (have) out.push_back(ep);
    }
    return out;
}

/** Szállítási réteg: a hívó (BluezListener) csak ezt látja; teszteknél hamis megvalósítás is adható. */
class BusIo {
public:
    virtual ~BusIo() = default;
    /** A várakozáshoz (poll) használható fájlleíró. */
    virtual int pollFd() const = 0;
    /**
     * Olvasás. >0: bájtok (a hozzájuk érkezett fájlleírók az fds-be); 0: a másik oldal bontott; -1: most nincs adat;
     * -2: hiba.
     */
    virtual long read(uint8_t* buf, size_t n, std::vector<int>& fds) = 0;
    virtual bool write(const uint8_t* data, size_t n) = 0;
};

#if defined(__linux__)

/** Unix-socketes D-Bus kapcsolat, SASL EXTERNAL hitelesítéssel és fájlleíró-átadással. */
class UnixBusIo : public BusIo {
public:
    ~UnixBusIo() override { if (fd_ >= 0) ::close(fd_); }

    /** Csatlakozás és hitelesítés. Hiba esetén nullptr, és err az ok. */
    static std::unique_ptr<UnixBusIo> connect(const std::string& address, std::string& err, int timeoutMs) {
        const auto eps = parseBusAddress(address);
        if (eps.empty()) { err = "érvénytelen D-Bus cím"; return nullptr; }
        std::string last = "nem sikerült csatlakozni a D-Bus-hoz";
        for (const auto& ep : eps) {
            const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0) continue;
            sockaddr_un sa{};
            sa.sun_family = AF_UNIX;
            socklen_t len;
            if (ep.abstract) {
                if (ep.path.size() + 1 > sizeof(sa.sun_path)) { ::close(fd); continue; }
                std::memcpy(sa.sun_path + 1, ep.path.data(), ep.path.size());
                len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + ep.path.size());
            } else {
                if (ep.path.size() >= sizeof(sa.sun_path)) { ::close(fd); continue; }
                std::memcpy(sa.sun_path, ep.path.c_str(), ep.path.size() + 1);
                len = static_cast<socklen_t>(sizeof(sa));
            }
            if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), len) != 0) {
                last = std::string("a D-Bus nem érhető el (") + std::strerror(errno) + ")";
                ::close(fd);
                continue;
            }
            std::unique_ptr<UnixBusIo> io(new UnixBusIo(fd));
            if (!io->authenticate(timeoutMs, err)) return nullptr;
            return io;
        }
        err = last;
        return nullptr;
    }

    int pollFd() const override { return fd_; }

    long read(uint8_t* buf, size_t n, std::vector<int>& fds) override {
        alignas(cmsghdr) char ctl[CMSG_SPACE(sizeof(int) * 16)];
        iovec iov{ buf, n };
        msghdr mh{};
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = ctl;
        mh.msg_controllen = sizeof(ctl);
        const ssize_t r = ::recvmsg(fd_, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : (errno == EINTR ? -1 : -2);
        for (cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                const size_t cnt = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                for (size_t i = 0; i < cnt; ++i) {
                    int fd;
                    std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
                    fds.push_back(fd);
                }
            }
        }
        return static_cast<long>(r);
    }

    bool write(const uint8_t* data, size_t n) override {
        size_t off = 0;
        while (off < n) {
            const ssize_t w = ::send(fd_, data + off, n - off, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (w > 0) { off += static_cast<size_t>(w); continue; }
            if (w < 0 && errno == EINTR) continue;
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd p{ fd_, POLLOUT, 0 };
                if (::poll(&p, 1, 2000) <= 0) return false;   // 2 mp-ig nem ürül a puffer: a busz megakadt
                continue;
            }
            return false;
        }
        return true;
    }

private:
    explicit UnixBusIo(int fd) : fd_(fd) {}

    bool sendRaw(const std::string& s) {
        size_t off = 0;
        while (off < s.size()) {
            const ssize_t w = ::send(fd_, s.data() + off, s.size() - off, MSG_NOSIGNAL);
            if (w <= 0) { if (w < 0 && errno == EINTR) continue; return false; }
            off += static_cast<size_t>(w);
        }
        return true;
    }

    /** Egy sor (\r\n-ig) a hitelesítési párbeszédből, bájtonként, hogy a későbbi üzenetbájtokat ne nyeljük el. */
    bool readLine(std::string& line, int timeoutMs) {
        line.clear();
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (line.size() < 512) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
            if (left <= 0) return false;
            pollfd p{ fd_, POLLIN, 0 };
            if (::poll(&p, 1, static_cast<int>(left)) <= 0) continue;
            char c;
            const ssize_t r = ::recv(fd_, &c, 1, 0);
            if (r <= 0) return false;
            line += c;
            if (c == '\n') {
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                return true;
            }
        }
        return false;
    }

    bool authenticate(int timeoutMs, std::string& err) {
        std::string hex;
        for (char c : std::to_string(static_cast<unsigned long>(::getuid()))) {
            char b[3];
            std::snprintf(b, sizeof(b), "%02x", static_cast<unsigned>(static_cast<unsigned char>(c)));
            hex += b;
        }
        std::string line;
        if (!sendRaw(std::string(1, '\0') + "AUTH EXTERNAL " + hex + "\r\n") || !readLine(line, timeoutMs) ||
            line.compare(0, 2, "OK") != 0) {
            err = "a D-Bus hitelesítés nem sikerült";
            return false;
        }
        if (!sendRaw("NEGOTIATE_UNIX_FD\r\n") || !readLine(line, timeoutMs) || line != "AGREE_UNIX_FD") {
            err = "a D-Bus nem enged fájlleírót átadni";
            return false;
        }
        if (!sendRaw("BEGIN\r\n")) { err = "a D-Bus kapcsolat megszakadt"; return false; }
        return true;
    }

    int fd_;
};

/** A rendszerbusz (ahol a BlueZ él) címe: a DBUS_SYSTEM_BUS_ADDRESS, különben az alapértelmezett socket. */
inline std::string systemBusAddress() {
    if (const char* e = std::getenv("DBUS_SYSTEM_BUS_ADDRESS"); e && *e) return e;
    return "unix:path=/var/run/dbus/system_bus_socket";
}

inline std::unique_ptr<BusIo> connectSystemBus(std::string& err, int timeoutMs) {
    return UnixBusIo::connect(systemBusAddress(), err, timeoutMs);
}

/**
 * A BlueZ-os RFCOMM figyelő: a kapcsolat felépítése és a regisztráció a hívó szálán (korlátos ideig), utána egy szál
 * dolgozza fel a hívásokat (NewConnection, a BlueZ újraindulása). Az acceptOne() a kész kapcsolatokat adja.
 */
class BluezListener {
public:
    using Connector = std::function<std::unique_ptr<BusIo>(std::string& err, int timeoutMs)>;

    explicit BluezListener(Connector c = &connectSystemBus, std::string uuid = "43ad541b-d816-443a-a652-146cb5191139")
        : connect_(std::move(c)), uuid_(std::move(uuid)) {}
    ~BluezListener() { close(); }
    BluezListener(const BluezListener&) = delete;
    BluezListener& operator=(const BluezListener&) = delete;

    /** Megnyitás: busz + regisztráció + adapter, legfeljebb timeoutMs alatt (összesen). false: lásd lastError(). */
    bool open(int timeoutMs = 3000) {
        close();
        lastError_.clear();
        std::string err;
        io_ = connect_(err, timeoutMs);
        if (!io_) { lastError_ = err; return false; }
        if (::pipe2(wake_, O_NONBLOCK | O_CLOEXEC) != 0) { lastError_ = "belső hiba (pipe)"; io_.reset(); return false; }
        dec_ = dbus::Decoder();
        session_ = std::make_unique<ProfileSession>(
            uuid_,
            [this](const dbus::Message& m) {
                const std::vector<uint8_t> bytes = dbus::encode(m);
                return io_ && io_->write(bytes.data(), bytes.size());
            },
            [](int fd) { if (fd >= 0) ::close(fd); });
        session_->start();
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (session_->state() != State::Ready) {
            if (session_->state() == State::Failed) { lastError_ = session_->failure(); teardown(); return false; }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
            if (left <= 0) { lastError_ = "a BlueZ nem válaszolt időben"; teardown(); return false; }
            pollfd p{ io_->pollFd(), POLLIN, 0 };
            if (::poll(&p, 1, static_cast<int>(left)) < 0 && errno != EINTR) { lastError_ = "belső hiba (poll)"; teardown(); return false; }
            if (!pump()) { lastError_ = "a D-Bus kapcsolat megszakadt"; teardown(); return false; }
        }
        addr_ = session_->address();
        broken_ = false;
        stop_ = false;
        thread_ = std::thread([this] { run(); });
        return true;
    }

    /** Tiszta leállás: a szál leáll, a profil törlődik (UnregisterProfile), a busz lezárul, a várakozó kapcsolatok bomlanak. */
    void close() {
        if (thread_.joinable()) {
            stop_ = true;
            const char b = 1;
            if (wake_[1] >= 0) { const ssize_t w = ::write(wake_[1], &b, 1); (void)w; }
            thread_.join();
        }
        if (io_ && session_ && !broken_) {
            session_->shutdown();
            // A válaszra legfeljebb 300 ms-ig várunk (a busz lezárása a BlueZ-nak amúgy is elég)
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
            while (!session_->unregistered() && std::chrono::steady_clock::now() < until) {
                pollfd p{ io_->pollFd(), POLLIN, 0 };
                if (::poll(&p, 1, 50) > 0 && !pump()) break;
            }
        }
        teardown();
        broken_ = false;
    }

    /** Egy átvehető bejövő kapcsolat (nem blokkoló socket); -1, ha nincs. */
    int acceptOne() {
        int fd = -1;
        if (!session_ || !session_->popConnection(fd)) return -1;
        const int fl = ::fcntl(fd, F_GETFL, 0);
        if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        return fd;
    }

    /** Fut a regisztráció (a profil él a BlueZ-ban). */
    bool isOpen() const { return io_ && session_ && !broken_ && session_->state() == State::Ready; }
    /** A busz kapcsolata megszakadt: újra kell nyitni. */
    bool broken() const { return broken_; }
    const std::string& address() const { return addr_; }
    const std::string& lastError() const { return lastError_; }
    State state() const { return session_ ? session_->state() : State::Idle; }

private:
    /** Elérhető bájtok beolvasása és feldolgozása; false: a kapcsolat vége / hibás folyam. */
    bool pump() {
        uint8_t buf[4096];
        for (;;) {
            std::vector<int> fds;
            const long n = io_->read(buf, sizeof(buf), fds);
            if (n == -1) return true;
            if (n <= 0) { for (int fd : fds) ::close(fd); return false; }
            dec_.feed(buf, static_cast<size_t>(n), fds);
            dbus::Message m;
            for (;;) {
                const auto r = dec_.next(m);
                if (r == dbus::Decoder::Result::NeedMore) break;
                if (r == dbus::Decoder::Result::Bad) { for (int fd : dec_.takeStrayFds()) ::close(fd); return false; }
                session_->onMessage(m);
            }
        }
    }

    void run() {
        while (!stop_) {
            pollfd p[2] = { { io_->pollFd(), POLLIN, 0 }, { wake_[0], POLLIN, 0 } };
            const int r = ::poll(p, 2, 1000);
            if (r < 0 && errno != EINTR) { broken_ = true; return; }
            if (stop_) return;
            session_->tick(std::chrono::steady_clock::now());
            if (r > 0 && (p[0].revents & (POLLIN | POLLHUP | POLLERR))) {
                if (!pump()) { if (!stop_) broken_ = true; return; }
            }
        }
    }

    void teardown() {
        if (session_) session_->drain();
        session_.reset();
        io_.reset();
        for (int& w : wake_) { if (w >= 0) ::close(w); w = -1; }
        addr_.clear();
    }

    Connector connect_;
    std::string uuid_;
    std::unique_ptr<BusIo> io_;
    std::unique_ptr<ProfileSession> session_;
    dbus::Decoder dec_;
    std::thread thread_;
    std::atomic<bool> stop_{ false };
    std::atomic<bool> broken_{ false };
    int wake_[2] = { -1, -1 };
    std::string addr_, lastError_;
};

#endif // __linux__

} // namespace ee_analyzer::bluez
