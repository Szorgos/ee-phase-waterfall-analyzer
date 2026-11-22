/**
 * @file test_bt_bluez.cpp
 * @brief A Linux Bluetooth RFCOMM szerver (BlueZ Profile1 a D-Bus-on) tesztjei, Bluetooth-rádió nélkül.
 *
 * Rétegenként:
 *  [1] dbus_wire.hpp: kódolás/értelmezés (kézzel számolt bájtokkal), darabolt bemenet, hibás folyam.
 *  [2] bt_bluez.hpp ProfileSession: az állapotgép hamis busszal (üzenetek be, üzenetek ki), minden hibaúttal.
 *  [3] (csak Linux) BluezListener + UnixBusIo hamis „D-Bus démonnal” és „BlueZ-zal” egy Unix-socketen: SASL, szál,
 *      fájlleíró-átadás (SCM_RIGHTS), tiszta leállás, újrakapcsolódás, időtúllépés, fájlleíró-szivárgás.
 *  [4] (csak Linux) a valódi híd (WebSocketBridge) a hamis BlueZ-on át: egy átadott RFCOMM-socketen ugyanaz a protokoll és
 *      ugyanaz a token-hitelesítés fut, mint a Windowsos ágon.
 * Amit ez NEM bizonyít: a valódi BlueZ viselkedését és a telefonnal való kapcsolódást (ahhoz kell BlueZ + rádió).
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "bt_bluez.hpp"
#include "bt_link.hpp"
#include "dbus_wire.hpp"

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <signal.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

#include "ws_bridge.hpp"
#endif

using namespace ee_analyzer;
using namespace ee_analyzer::dbus;
using namespace ee_analyzer::bluez;

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool cond, const std::string& label) {
    if (cond) {
        ++g_pass;
        std::printf("  [ok]   %s\n", label.c_str());
    } else {
        ++g_fail;
        std::printf("  [HIBA] %s\n", label.c_str());
    }
}

// ---------------------------------------------------------------------------------------------------- segédek
Message mkCall(uint32_t serial, const std::string& dest, const std::string& path, const std::string& iface,
               const std::string& member, const std::string& sig = {}, std::vector<uint8_t> body = {}) {
    Message m;
    m.type = MsgType::MethodCall;
    m.serial = serial;
    m.dest = dest; m.path = path; m.iface = iface; m.member = member; m.signature = sig;
    m.body = std::move(body);
    return m;
}

Message mkReturn(uint32_t replySerial, const std::string& sig = {}, std::vector<uint8_t> body = {}) {
    Message m;
    m.type = MsgType::MethodReturn;
    m.serial = 900 + replySerial;
    m.replySerial = replySerial;
    m.signature = sig;
    m.body = std::move(body);
    return m;
}

Message mkError(uint32_t replySerial, const std::string& name, const std::string& text = {}) {
    Message m;
    m.type = MsgType::Error;
    m.serial = 900 + replySerial;
    m.replySerial = replySerial;
    m.errorName = name;
    if (!text.empty()) {
        Writer w;
        w.str(text);
        m.signature = "s";
        m.body = w.bytes();
    }
    return m;
}

std::vector<uint8_t> strBody(const std::string& s) {
    Writer w;
    w.str(s);
    return w.bytes();
}

/** A Properties.Get válasza: v (s) */
std::vector<uint8_t> variantStrBody(const std::string& s) {
    Writer w;
    w.sig("s");
    w.str(s);
    return w.bytes();
}

Message mkNameOwnerChanged(const std::string& name, const std::string& oldO, const std::string& newO) {
    Message m;
    m.type = MsgType::Signal;
    m.serial = 777;
    m.path = "/org/freedesktop/DBus";
    m.iface = "org.freedesktop.DBus";
    m.member = "NameOwnerChanged";
    m.sender = "org.freedesktop.DBus";
    m.signature = "sss";
    Writer w;
    w.str(name); w.str(oldO); w.str(newO);
    m.body = w.bytes();
    return m;
}

Message mkNewConnection(uint32_t serial, const std::string& device, uint32_t fdIndex, size_t nfds, bool noReply = false) {
    Message m = mkCall(serial, ":1.7", kProfilePath, "org.bluez.Profile1", "NewConnection", "oha{sv}");
    m.sender = ":1.9";
    Writer w;
    w.str(device);
    w.u32(fdIndex);
    w.dict([](Writer&) {});
    m.body = w.bytes();
    m.unixFds = static_cast<uint32_t>(nfds);
    if (noReply) m.flags |= kFlagNoReplyExpected;
    return m;
}

/** A RegisterProfile törzsének önálló értelmezése (a Reader-től független elrendezés-ellenőrzés a dict kulcsaira). */
bool parseRegisterBody(const Message& m, std::string& path, std::string& uuid, std::map<std::string, std::string>& props) {
    Reader r(m.body.data(), m.body.size());
    path = r.str();
    uuid = r.str();
    const uint32_t len = r.u32();
    r.align(8);
    const size_t start = r.pos();
    while (r.ok() && r.pos() - start < len) {
        r.align(8);
        const std::string key = r.str();
        const std::string t = r.sig();
        if (t == "s") props[key] = r.str();
        else if (t == "q") props[key] = std::to_string(r.u16());
        else if (t == "b") props[key] = r.boolean() ? "true" : "false";
        else return false;
    }
    return r.ok() && r.pos() - start == len && r.remaining() == 0;
}

/** Hamis busz a munkamenet-teszthez: a kimenő üzeneteket gyűjti. */
struct Fixture {
    std::vector<Message> sent;
    std::vector<int> closed;
    bool sendOk = true;
    ProfileSession s;
    Fixture() : s("43ad541b-d816-443a-a652-146cb5191139",
                  [this](const Message& m) { sent.push_back(m); return sendOk; },
                  [this](int fd) { closed.push_back(fd); }) {}
    const Message& last() const { return sent.back(); }
    /** A legutóbb küldött hívásra válasz (a szerial alapján). */
    void answer(Message m) { m.replySerial = last().serial; s.onMessage(m); }
    /** Köszönés → AddMatch → RegisterProfile → Properties.Get → kész. */
    void bringUp(const std::string& addr = "AA:BB:CC:DD:EE:01") {
        s.start();
        answer(mkReturn(0, "s", strBody(":1.7")));
        answer(mkReturn(0));
        answer(mkReturn(0));
        answer(mkReturn(0, "v", variantStrBody(addr)));
    }
};

// ---------------------------------------------------------------------------------------------------- [1] kódolás
void testWire() {
    std::printf("[1] dbus_wire.hpp: kódolás és értelmezés\n");

    // 1a. A Hello hívás bájtról bájtra, kézzel kiszámolva a specifikáció szerint
    {
        Message m = mkCall(1, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello");
        const auto b = encode(m);
        // fejléc: 'l', hívás, nincs jelző, 1-es verzió, törzs 0, serial 1, mezők hossza 109
        const uint8_t head[16] = { 'l', 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 109, 0, 0, 0 };
        check(b.size() == 128 && std::memcmp(b.data(), head, 16) == 0, "Hello: 16 bájtos fejléc, 109 bájtos mezőtömb, 128 bájtra igazítva");
        // 1. mező: PATH (1), 'o', hossz 21, "/org/freedesktop/DBus\0"
        const uint8_t f1[8] = { 1, 1, 'o', 0, 21, 0, 0, 0 };
        check(std::memcmp(b.data() + 16, f1, 8) == 0 && std::memcmp(b.data() + 24, "/org/freedesktop/DBus", 22) == 0,
              "Hello: az útvonal-mező kódja, típusa, hossza és szövege");
        // 2. mező 8 bájtra igazítva: a 16+26=42 → 48-nál kezdődik: INTERFACE (2), 's'
        check(b[48] == 2 && b[49] == 1 && b[50] == 's' && b[51] == 0, "Hello: a következő mező 8 bájtra igazítva kezdődik");
        for (size_t i = 125; i < 128; ++i) if (b[i] != 0) { check(false, "Hello: a kitöltés nulla bájtok"); break; }
        // visszaolvasás
        Decoder d; d.feed(b.data(), b.size());
        Message o;
        check(d.next(o) == Decoder::Result::Got && o.type == MsgType::MethodCall && o.serial == 1 && o.member == "Hello" &&
                  o.path == "/org/freedesktop/DBus" && o.dest == "org.freedesktop.DBus" && o.iface == "org.freedesktop.DBus",
              "Hello: kódolás → értelmezés körbe ér");
    }
    // 1b. Törzs: a{sv} elrendezése és hossza
    {
        Writer w;
        w.str("/p");                                   // 4+2+1 = 7 bájt
        w.dict([](Writer& d) { d.dictEntryU16("Channel", 22); });
        const auto& b = w.bytes();
        // a tömbhossz mező 8-ra igazítva, az adat is 8-ra: 8..11 hossz, 12..15 kitöltés, 16-tól a bejegyzés:
        // "Channel": 4+7+1 = 12 → 28; "q" aláírás: 3 bájt → 31; u16 2-re igazítva: 32..33 → 34 bájt; adat = 34-16 = 18
        check(b.size() == 34 && b[8] == 18 && b[9] == 0 && b[16] == 7 && b[32] == 22 && b[33] == 0,
              "a{sv}: hossz 18, a bejegyzés 8 bájtra igazítva, q típus 2 bájton");
        Writer e;
        e.str("/p");
        e.dict([](Writer&) {});
        check(e.size() == 16 && e.bytes()[8] == 0, "üres a{sv}: nulla hossz, a (nulla elemű) tömb is 8 bájtra igazított");
    }
    // 1c. Logikai érték 4 bájt
    {
        Writer w;
        w.dict([](Writer& d) { d.dictEntryBool("RequireAuthentication", true); });
        Reader r(w.bytes().data(), w.size());
        r.u32(); r.align(8); r.str(); const std::string t = r.sig();
        check(t == "b" && r.boolean() && r.ok(), "logikai érték: 'b' aláírás, 4 bájtos 1-es");
    }
    // 1d. Nagy-endian üzenet értelmezése (kézzel épített MethodReturn, reply_serial = 5, törzs: egy s)
    {
        std::vector<uint8_t> b = {
            'B', 2, 0, 1,                      // nagy-endian, MethodReturn
            0, 0, 0, 8,                        // törzs: 8 bájt (hossz 3 + "abc" + nul = 8)
            0, 0, 0, 9,                        // serial 9
            0, 0, 0, 24,                       // mezők hossza: REPLY_SERIAL (8+4=12→) + SIGNATURE ...
        };
        // 5. mező REPLY_SERIAL: kód 5, 'u' aláírás (1,'u',0), pad → u32 a 4-es határon: 5,1,'u',0, 0,0,0,5 = 8 bájt
        const uint8_t f5[8] = { 5, 1, 'u', 0, 0, 0, 0, 5 };
        // 8. mező SIGNATURE: 8, 'g'-aláírás (1,'g',0), hossz 1, 's', nul = 4+2+... : 8,1,'g',0, 1,'s',0 = 7 bájt, 8-ra igazítva nem kell a végén
        const uint8_t f8[7] = { 8, 1, 'g', 0, 1, 's', 0 };
        // összesen 8 + (igazítás 8-ra: 0) + 7 = 15, nem 24: javítjuk a hosszt
        b[15] = 15;
        b.insert(b.end(), f5, f5 + 8);
        b.insert(b.end(), f8, f8 + 7);
        while (b.size() % 8) b.push_back(0);     // a fejléc 8-ra igazított vége
        const uint8_t body[8] = { 0, 0, 0, 3, 'a', 'b', 'c', 0 };
        b.insert(b.end(), body, body + 8);
        Decoder d; d.feed(b.data(), b.size());
        Message o;
        const auto r = d.next(o);
        Reader br(o.body.data(), o.body.size(), o.bigEndian);
        check(r == Decoder::Result::Got && o.replySerial == 5 && o.serial == 9 && o.signature == "s" && o.bigEndian && br.str() == "abc",
              "nagy-endian válasz: serial, reply_serial, aláírás és a törzs helyesen olvasható");
    }
    // 1e. Darabolt bemenet, két üzenet egymás után, fájlleírók hozzárendelése
    {
        Message a = mkCall(2, "x", "/a", "i.f", "A");
        Message b = mkCall(3, "x", "/b", "i.f", "B");
        b.unixFds = 1;
        const auto ba = encode(a), bb = encode(b);
        std::vector<uint8_t> all = ba;
        all.insert(all.end(), bb.begin(), bb.end());
        Decoder d;
        Message o;
        int got = 0;
        std::vector<Message> msgs;
        for (size_t i = 0; i < all.size(); ++i) {
            d.feed(&all[i], 1, i == ba.size() + 3 ? std::vector<int>{ 42 } : std::vector<int>{});   // a fd a 2. üzenet közben érkezik
            while (d.next(o) == Decoder::Result::Got) { ++got; msgs.push_back(o); }
        }
        check(got == 2 && msgs[0].member == "A" && msgs[1].member == "B", "bájtonként érkező folyam: mindkét üzenet kijön, sorrendben");
        check(msgs[0].fds.empty() && msgs[1].fds == std::vector<int>{ 42 }, "a fájlleíró a jelzett (2.) üzenethez kerül");
    }
    // 1f. Hibás folyamok
    {
        auto bad = [](std::vector<uint8_t> v, const std::vector<int>& fds, const char* what) {
            Decoder d; d.feed(v.data(), v.size(), fds);
            Message o;
            check(d.next(o) == Decoder::Result::Bad, what);
        };
        Message m = mkCall(1, "x", "/a", "i.f", "A");
        auto good = encode(m);
        auto v = good; v[0] = 'X';           bad(v, {}, "ismeretlen bájtsorrend-jel: hibás folyam");
        v = good; v[3] = 2;                  bad(v, {}, "ismeretlen protokollverzió: hibás folyam");
        v = good; v[1] = 9;                  bad(v, {}, "ismeretlen üzenettípus: hibás folyam");
        v = good; v[8] = v[9] = v[10] = v[11] = 0; bad(v, {}, "nulla serial: hibás folyam");
        v = good; v[7] = 0x7f;               bad(v, {}, "óriási törzshossz: hibás folyam (nem várunk rá)");
        v = good; v[15] = 0x7f;              bad(v, {}, "óriási mezőhossz: hibás folyam");
        Message nm = mkCall(1, "x", "", "i.f", "A");     // útvonal nélküli metódushívás
        bad(encode(nm), {}, "útvonal nélküli hívás: hibás folyam");
        Message fm = good.empty() ? m : m; fm.unixFds = 2;
        bad(encode(fm), { 5 }, "több fájlleírót jelez, mint amennyi megérkezett: hibás folyam");
        Message rm = mkReturn(0);
        bad(encode(rm), {}, "reply_serial nélküli válasz: hibás folyam");
        // levágott fejléc nem hiba, csak több adat kell
        Decoder d; d.feed(good.data(), 10);
        Message o;
        check(d.next(o) == Decoder::Result::NeedMore, "félig érkezett üzenet: több adat kell (nem hiba)");
        // Utána jön a fennmaradó rész
        d.feed(good.data() + 10, good.size() - 10);
        check(d.next(o) == Decoder::Result::Got, "…és a maradék megérkezése után kész");
        // Hibás után a folyam használhatatlan
        Decoder e; auto g2 = good; g2[0] = 'X'; e.feed(g2.data(), g2.size());
        check(e.next(o) == Decoder::Result::Bad && !e.feed(good.data(), good.size()) && e.next(o) == Decoder::Result::Bad,
              "hiba után további bemenet sem fogadható el");
    }
    // 1g. Olvasó: határellenőrzés
    {
        const uint8_t s[] = { 0, 0, 0, 200, 'a' };
        Reader r(s, sizeof(s));
        check(r.str().empty() && !r.ok(), "olvasó: a túlnyúló szöveghossz hiba, nem túlolvasás");
        const uint8_t t[] = { 0, 0, 0, 1, 'a', 'x' };   // a záró nulla helyén 'x'
        Reader r2(t, sizeof(t));
        check(r2.str().empty() && !r2.ok(), "olvasó: hiányzó záró nulla hiba");
    }
}

// ---------------------------------------------------------------------------------------------------- [2] állapotgép
void testCaps() {
    std::printf("[2a] D-Bus-cím és hibaszövegek\n");
    auto eps = parseBusAddress("unix:path=/var/run/dbus/system_bus_socket");
    check(eps.size() == 1 && !eps[0].abstract && eps[0].path == "/var/run/dbus/system_bus_socket", "unix:path");
    eps = parseBusAddress("unix:abstract=/tmp/dbus-X,guid=abc");
    check(eps.size() == 1 && eps[0].abstract && eps[0].path == "/tmp/dbus-X", "unix:abstract, guid figyelmen kívül");
    eps = parseBusAddress("tcp:host=1.2.3.4,port=5;unix:path=/a%20b");
    check(eps.size() == 1 && eps[0].path == "/a b", "tcp kihagyva, a százalékkódolás feloldva");
    check(parseBusAddress("").empty() && parseBusAddress("tcp:host=x").empty() && parseBusAddress("unix:guid=x").empty(),
          "üres / nem Unix / útvonal nélküli cím: nincs végpont");
    check(describeError("org.freedesktop.DBus.Error.ServiceUnknown", "").find("nem fut") != std::string::npos &&
              describeError("org.freedesktop.DBus.Error.AccessDenied", "").find("bluetooth") != std::string::npos &&
              describeError("org.bluez.Error.AlreadyExists", "").find("másik") != std::string::npos &&
              describeError("org.x.Y", "szöveg") == "org.x.Y: szöveg",
          "hibaszövegek: BlueZ nem fut / jogosultság / már regisztrálva / ismeretlen");
}

void testSession() {
    std::printf("[2] ProfileSession: a regisztráció állapotgépe hamis busszal\n");

    // 2a. A teljes jó út, a kimenő üzenetek tartalmával
    {
        Fixture f;
        f.s.start();
        check(f.sent.size() == 1 && f.last().member == "Hello" && f.last().dest == "org.freedesktop.DBus" && f.s.state() == State::Hello,
              "indulás: a Hello megy ki elsőnek");
        f.answer(mkReturn(0, "s", strBody(":1.7")));
        check(f.last().member == "AddMatch" && f.s.state() == State::AddMatch, "Hello után: AddMatch");
        Reader mr(f.last().body.data(), f.last().body.size());
        const std::string rule = mr.str();
        check(rule.find("NameOwnerChanged") != std::string::npos && rule.find("arg0='org.bluez'") != std::string::npos,
              "az AddMatch a BlueZ névváltására szűr");
        f.answer(mkReturn(0));
        check(f.last().member == "RegisterProfile" && f.last().dest == "org.bluez" && f.last().path == "/org/bluez" &&
                  f.last().iface == "org.bluez.ProfileManager1" && f.last().signature == "osa{sv}",
              "AddMatch után: RegisterProfile az org.bluez-nek, osa{sv} aláírással");
        check((f.last().flags & kFlagNoAutoStart) != 0 && (f.sent[0].flags & kFlagNoAutoStart) == 0,
              "a BlueZ-hívások NO_AUTO_START jelzővel mennek (nem indítjuk el a bluetoothd-t), a busz-hívások nem");
        std::string path, uuid;
        std::map<std::string, std::string> props;
        const bool parsed = parseRegisterBody(f.last(), path, uuid, props);
        check(parsed && path == kProfilePath && uuid == "43ad541b-d816-443a-a652-146cb5191139", "a profil útvonala és UUID-je (azonos a telefonéval)");
        check(props["Role"] == "server" && props["Channel"] == "22" && props["Name"] == "Aqulify",
              "szerver szerep, rögzített RFCOMM-csatorna (BlueZ enélkül nem nyit figyelőt), név");
        check(props["RequireAuthentication"] == "true" && props["RequireAuthorization"] == "false" && props.size() == 5,
              "párosítás kötelező (mint Windowson), BlueZ-os külön engedélykérés kikapcsolva");
        f.answer(mkReturn(0));
        check(f.last().member == "Get" && f.last().path == "/org/bluez/hci0" && f.last().iface == "org.freedesktop.DBus.Properties" &&
                  f.s.state() == State::Adapter,
              "regisztráció után: az adapter címének lekérdezése (hci0)");
        f.answer(mkReturn(0, "v", variantStrBody("AA:BB:CC:DD:EE:01")));
        check(f.s.state() == State::Ready && f.s.address() == "AA:BB:CC:DD:EE:01", "kész: a gép Bluetooth-címe megvan");
    }
    // 2b. Az első adapter nincs: hci1 jön
    {
        Fixture f;
        f.s.start();
        f.answer(mkReturn(0, "s", strBody(":1.7"))); f.answer(mkReturn(0)); f.answer(mkReturn(0));
        f.answer(mkError(0, "org.freedesktop.DBus.Error.UnknownObject"));
        check(f.last().path == "/org/bluez/hci1" && f.s.state() == State::Adapter, "hci0 nincs: hci1-et kérdezi");
        f.answer(mkReturn(0, "v", variantStrBody("11:22:33:44:55:66")));
        check(f.s.state() == State::Ready && f.s.address() == "11:22:33:44:55:66", "hci1 címe lesz a gép címe");
    }
    // 2c. Hibaágak
    {
        auto failsWith = [](const char* errName, const char* needle, int upTo, const char* what) {
            Fixture f;
            f.s.start();
            if (upTo >= 1) f.answer(mkReturn(0, "s", strBody(":1.7")));
            if (upTo >= 2) f.answer(mkReturn(0));
            f.answer(mkError(0, errName, "szöveg"));
            check(f.s.state() == State::Failed && f.s.failure().find(needle) != std::string::npos, what);
        };
        failsWith("org.freedesktop.DBus.Error.ServiceUnknown", "nem fut", 2, "nincs BlueZ: Failed, „nem fut”");
        failsWith("org.freedesktop.DBus.Error.AccessDenied", "bluetooth", 2, "a D-Bus-szabály tiltja: Failed, útmutatás a csoportról");
        failsWith("org.bluez.Error.AlreadyExists", "másik", 2, "már regisztrált profil: Failed");
        failsWith("org.freedesktop.DBus.Error.AccessDenied", "köszönés", 0, "a Hello hibája: Failed");
        Fixture f;
        f.s.start();
        f.answer(mkReturn(0, "s", strBody(":1.7"))); f.answer(mkReturn(0)); f.answer(mkReturn(0));
        for (int i = 0; i <= kMaxAdapterIndex; ++i) f.answer(mkError(0, "org.freedesktop.DBus.Error.UnknownObject"));
        check(f.s.state() == State::Failed && f.s.failure().find("adapter") != std::string::npos && f.sent.back().path == "/org/bluez/hci3",
              "nincs adapter (hci0–hci3 mind hibás): Failed, „nincs Bluetooth-adapter”");
        Fixture g;
        g.s.start();
        g.answer(mkReturn(0, "s", strBody(":1.7"))); g.answer(mkReturn(0)); g.answer(mkReturn(0));
        g.answer(mkReturn(0, "v", [] { Writer w; w.sig("u"); w.u32(1); return w.bytes(); }()));
        check(g.s.state() == State::Failed, "az adapter címe nem szöveg: Failed (nem Ready üres címmel)");
        Fixture h;
        h.sendOk = false;
        h.s.start();
        check(h.s.state() == State::Failed && h.s.failure().find("írni") != std::string::npos, "az írás sikertelen: Failed");
        // AddMatch hiba nem végzetes
        Fixture k;
        k.s.start();
        k.answer(mkReturn(0, "s", strBody(":1.7")));
        k.answer(mkError(0, "org.freedesktop.DBus.Error.AccessDenied"));
        check(k.s.state() == State::Register && k.last().member == "RegisterProfile", "az AddMatch hibája után is regisztrál");
        // Ismeretlen válasz-serial figyelmen kívül
        Message stray = mkReturn(9999);
        k.s.onMessage(stray);
        check(k.s.state() == State::Register, "idegen válasz nem zavarja az állapotot");
    }
    // 2d. Bejövő hívások
    {
        Fixture f;
        f.bringUp();
        f.sent.clear();
        Message nc = mkNewConnection(41, "/org/bluez/hci0/dev_11", 0, 1);
        nc.fds = { 100 };
        f.s.onMessage(nc);
        int fd = -1;
        check(f.s.popConnection(fd) && fd == 100 && f.closed.empty(), "NewConnection: a fájlleíró a várólistára kerül");
        check(f.sent.size() == 1 && f.last().type == MsgType::MethodReturn && f.last().replySerial == 41 && f.last().dest == ":1.9",
              "NewConnection: üres válasz a hívónak (a hívás serialjával)");
        check(!f.s.popConnection(fd), "a várólista kiürült");

        // Hibás argumentumok: a fd bezárva, hibaválasz
        f.sent.clear();
        Message bad = mkNewConnection(42, "/d", 3, 1);
        bad.fds = { 101 };
        f.s.onMessage(bad);
        check(f.closed == std::vector<int>{ 101 } && f.last().type == MsgType::Error && !f.s.popConnection(fd),
              "NewConnection hibás fd-indexszel: hibaválasz, a kapott fd bezárva");
        f.closed.clear();
        Message nofd = mkNewConnection(43, "/d", 0, 0);
        f.s.onMessage(nofd);
        check(f.last().type == MsgType::Error && f.last().errorName == "org.freedesktop.DBus.Error.InvalidArgs",
              "NewConnection fájlleíró nélkül: InvalidArgs");
        // Telítés
        f.sent.clear();
        for (int i = 0; i < 10; ++i) {
            Message m = mkNewConnection(50 + static_cast<uint32_t>(i), "/d", 0, 1);
            m.fds = { 200 + i };
            f.s.onMessage(m);
        }
        size_t n = 0;
        while (f.s.popConnection(fd)) ++n;
        check(n == kMaxQueuedConnections && f.closed == std::vector<int>({ 208, 209 }),
              "legfeljebb 8 átvétlen kapcsolat várakozik, a többi (208, 209) azonnal bezárva");
        // Felesleges fd egy más hívásnál bezárul
        f.closed.clear();
        Message ping = mkCall(60, ":1.7", "/x", "org.freedesktop.DBus.Peer", "Ping");
        ping.sender = ":1.3"; ping.fds = { 300 };
        f.s.onMessage(ping);
        check(f.closed == std::vector<int>{ 300 } && f.last().type == MsgType::MethodReturn, "Peer.Ping válasz; a nem várt fd bezárva");
        // Ismeretlen metódus / objektum
        Message unk = mkCall(61, ":1.7", kProfilePath, "org.bluez.Profile1", "Valami");
        unk.sender = ":1.9";
        f.s.onMessage(unk);
        check(f.last().type == MsgType::Error && f.last().errorName == "org.freedesktop.DBus.Error.UnknownMethod", "ismeretlen metódus: UnknownMethod");
        Message unp = mkCall(62, ":1.7", "/mas/ut", "org.bluez.Profile1", "NewConnection");
        unp.sender = ":1.9";
        f.s.onMessage(unp);
        check(f.last().type == MsgType::Error, "idegen objektumútvonal: hiba");
        // RequestDisconnection: üres válasz
        Message rd = mkCall(63, ":1.7", kProfilePath, "org.bluez.Profile1", "RequestDisconnection", "o", strBody("/d"));
        rd.sender = ":1.9";
        f.s.onMessage(rd);
        check(f.last().type == MsgType::MethodReturn && f.s.state() == State::Ready, "RequestDisconnection: válasz, a regisztráció marad");
        // Várakozó fd-k lezárása
        Message m = mkNewConnection(70, "/d", 0, 1); m.fds = { 400 };
        f.s.onMessage(m);
        f.closed.clear();
        f.s.drain();
        check(f.closed == std::vector<int>{ 400 } && !f.s.popConnection(fd), "drain(): az átvétlen fd-k bezárva");
    }
    // 2e. Release (válasz nélkül kérve), BlueZ elmegy / visszajön, újrapróbálás
    {
        Fixture f;
        f.bringUp();
        f.sent.clear();
        Message rel = mkCall(80, ":1.7", kProfilePath, "org.bluez.Profile1", "Release");
        rel.sender = ":1.9"; rel.flags = kFlagNoReplyExpected;
        f.s.onMessage(rel);
        check(f.sent.empty() && f.s.state() == State::WaitingBluez, "Release (noreply): nincs válasz, a regisztráció elveszett → WaitingBluez");

        Fixture g;
        g.bringUp();
        g.s.onMessage(*std::make_unique<Message>(mkNameOwnerChanged("org.bluez", ":1.9", "")));
        check(g.s.state() == State::WaitingBluez, "a BlueZ eltűnt (NameOwnerChanged, új tulajdonos nélkül): WaitingBluez");
        // Más nevek jelzése nem számít
        Fixture h;
        h.bringUp();
        h.s.onMessage(*std::make_unique<Message>(mkNameOwnerChanged("org.mas", ":1.9", "")));
        check(h.s.state() == State::Ready, "más név eltűnése nem érinti a regisztrációt");

        g.sent.clear();
        g.s.onMessage(*std::make_unique<Message>(mkNameOwnerChanged("org.bluez", "", ":1.11")));
        check(g.last().member == "RegisterProfile" && g.s.state() == State::Register, "a BlueZ visszatért: újraregisztrál");
        // A BlueZ még nem áll készen: hiba → újrapróbálás 2 mp múlva
        g.answer(mkError(0, "org.freedesktop.DBus.Error.UnknownObject"));
        check(g.s.state() == State::WaitingBluez, "az újraregisztráció első hibája nem végleges");
        const size_t before = g.sent.size();
        g.s.tick(ProfileSession::Clock::now());
        check(g.sent.size() == before, "az újrapróbálás előtt (2 mp) nem küld semmit");
        g.s.tick(ProfileSession::Clock::now() + std::chrono::seconds(3));
        check(g.sent.size() == before + 1 && g.last().member == "RegisterProfile", "2 mp múlva újra próbálja");
        g.answer(mkReturn(0));
        check(g.s.state() == State::Ready && g.s.address() == "AA:BB:CC:DD:EE:01" && g.last().member == "RegisterProfile",
              "sikeres újraregisztráció: Ready, az adapter címe megmarad (nem kérdezi újra)");

        // Tartós hiba: 5 próbálkozás után Failed
        Fixture k;
        k.bringUp();
        k.s.onMessage(*std::make_unique<Message>(mkNameOwnerChanged("org.bluez", ":1.9", "")));
        k.s.onMessage(*std::make_unique<Message>(mkNameOwnerChanged("org.bluez", "", ":1.12")));
        for (int i = 0; i < 6; ++i) {
            k.answer(mkError(0, "org.freedesktop.DBus.Error.UnknownObject"));
            k.s.tick(ProfileSession::Clock::now() + std::chrono::seconds(10 * (i + 1)));
        }
        check(k.s.state() == State::Failed, "tartósan hibás újraregisztráció: 5 próba után Failed (a híd újranyitja)");
        // Jogosultsági hiba újrapróbálás nélkül azonnal végleges
        Fixture q;
        q.bringUp();
        q.s.onMessage(*std::make_unique<Message>(mkNameOwnerChanged("org.bluez", ":1.9", "")));
        q.s.onMessage(*std::make_unique<Message>(mkNameOwnerChanged("org.bluez", "", ":1.12")));
        q.answer(mkError(0, "org.freedesktop.DBus.Error.AccessDenied"));
        check(q.s.state() == State::Failed, "újraregisztrációnál a jogosultsági hiba azonnal végleges");
    }
    // 2f. Tiszta leállás
    {
        Fixture f;
        f.bringUp();
        f.sent.clear();
        f.s.shutdown();
        check(f.sent.size() == 1 && f.last().member == "UnregisterProfile" && f.last().signature == "o", "shutdown: UnregisterProfile megy ki");
        Reader r(f.last().body.data(), f.last().body.size());
        check(r.str() == kProfilePath, "…a mi profilunk útvonalával");
        check(!f.s.unregistered(), "a válasz előtt még nincs megerősítve");
        f.answer(mkReturn(0));
        check(f.s.unregistered(), "a válasz után megerősítve");
        f.sent.clear();
        f.s.shutdown();
        check(f.sent.empty(), "kétszeri shutdown nem küld újra");
        Fixture n;
        n.s.start();
        n.sent.clear();
        n.s.shutdown();
        check(n.sent.empty(), "regisztráció nélkül nincs mit törölni");
    }
}

#if defined(__linux__)
// ---------------------------------------------------------------------------------------------------- [3] hamis démon
struct FakeDaemon {
    std::string path;
    int listenFd = -1;
    int wake[2] = { -1, -1 };
    std::thread th;
    std::atomic<bool> stop{ false };

    // beállítások (a teszt a start előtt / közben állítja; a szál atomikusan olvassa)
    std::atomic<bool> rejectAuth{ false };
    std::atomic<bool> silentHello{ false };
    std::atomic<int> registerFailures{ 0 };               // ennyi RegisterProfile-ra hiba a válasz
    std::string registerErrorName = "org.freedesktop.DBus.Error.UnknownObject";
    std::atomic<int> adapterMissing{ 0 };                 // az első ennyi Get-re hiba (UnknownObject)
    std::atomic<bool> noAuthNegotiate{ false };

    std::mutex mu;
    std::vector<std::string> log;                         // a kapott hívások tagjai sorrendben
    std::string authLine;
    std::map<std::string, std::string> regProps;
    std::string regUuid;
    std::deque<std::pair<std::vector<uint8_t>, int>> outbox;   // a kliensnek küldendő bájtok (+ fd vagy -1)
    std::atomic<int> dropNow{ 0 };
    std::atomic<int> connections{ 0 };
    std::atomic<bool> sawEofAfterUnregister{ false };
    std::atomic<bool> unregisterSeen{ false };

    bool start() {
        static std::atomic<int> counter{ 0 };
        path = "/tmp/aqulify-bt-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter++) + ".sock";
        ::unlink(path.c_str());
        listenFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        std::strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
        if (listenFd < 0 || ::bind(listenFd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 || ::listen(listenFd, 4) != 0) return false;
        if (::pipe2(wake, O_NONBLOCK | O_CLOEXEC) != 0) return false;
        th = std::thread([this] { run(); });
        return true;
    }
    ~FakeDaemon() {
        stop = true;
        if (wake[1] >= 0) { const char b = 1; const ssize_t w = ::write(wake[1], &b, 1); (void)w; }
        if (th.joinable()) th.join();
        for (auto& o : outbox) if (o.second >= 0) ::close(o.second);   // még el nem küldött fájlleírók
        if (listenFd >= 0) ::close(listenFd);
        for (int w : wake) if (w >= 0) ::close(w);
        ::unlink(path.c_str());
    }
    std::string address() const { return "unix:path=" + path; }

    void kick() { const char b = 1; const ssize_t w = ::write(wake[1], &b, 1); (void)w; }
    void queue(const Message& m, int fd = -1) {
        { std::lock_guard<std::mutex> lk(mu); outbox.emplace_back(encode(m), fd); }
        kick();
    }
    bool outboxEmpty() {
        std::lock_guard<std::mutex> lk(mu);
        return outbox.empty();
    }
    int logCount(const std::string& member) {
        std::lock_guard<std::mutex> lk(mu);
        return static_cast<int>(std::count(log.begin(), log.end(), member));
    }
    bool waitLog(const std::string& member, int count, int ms) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until) {
            if (logCount(member) >= count) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

private:
    static bool sendBytes(int fd, const std::vector<uint8_t>& b, int passFd) {
        iovec iov{ const_cast<uint8_t*>(b.data()), b.size() };
        msghdr mh{};
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        alignas(cmsghdr) char ctl[CMSG_SPACE(sizeof(int))];
        if (passFd >= 0) {
            std::memset(ctl, 0, sizeof(ctl));
            mh.msg_control = ctl;
            mh.msg_controllen = sizeof(ctl);
            cmsghdr* c = CMSG_FIRSTHDR(&mh);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(c), &passFd, sizeof(int));
        }
        return ::sendmsg(fd, &mh, MSG_NOSIGNAL) == static_cast<ssize_t>(b.size());
    }
    static bool sendStr(int fd, const std::string& s) { return ::send(fd, s.data(), s.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(s.size()); }

    void reply(int fd, const Message& req, const Message& r0) {
        Message r = r0;
        r.replySerial = req.serial;
        r.dest = ":1.7";
        r.sender = req.dest == "org.bluez" ? ":1.9" : "org.freedesktop.DBus";
        sendBytes(fd, encode(r), -1);
    }

    void handleConn(int c) {
        ++connections;
        // SASL
        std::string buf;
        bool authed = false;
        auto line = [&](std::string& out) -> bool {
            for (;;) {
                const size_t nl = buf.find('\n');
                if (nl != std::string::npos) { out = buf.substr(0, nl); buf.erase(0, nl + 1); if (!out.empty() && out.back() == '\r') out.pop_back(); return true; }
                pollfd p{ c, POLLIN, 0 };
                if (::poll(&p, 1, 2000) <= 0) return false;
                char t[256];
                const ssize_t n = ::recv(c, t, sizeof(t), 0);
                if (n <= 0) return false;
                buf.append(t, static_cast<size_t>(n));
            }
        };
        std::string l;
        if (!line(l)) return;
        if (!l.empty() && l[0] == '\0') l.erase(0, 1);
        { std::lock_guard<std::mutex> lk(mu); authLine = l; }
        if (rejectAuth) { sendStr(c, "REJECTED EXTERNAL\r\n"); return; }
        sendStr(c, "OK 0123456789abcdef0123456789abcdef\r\n");
        if (!line(l) || l != "NEGOTIATE_UNIX_FD") return;
        sendStr(c, noAuthNegotiate ? "ERROR\r\n" : "AGREE_UNIX_FD\r\n");
        if (noAuthNegotiate) return;
        if (!line(l) || l != "BEGIN") return;
        authed = true;
        (void)authed;
        // Üzenetek
        Decoder dec;
        auto handleMessages = [&] {
            Message m;
            while (dec.next(m) == Decoder::Result::Got) {
                { std::lock_guard<std::mutex> lk(mu); log.push_back(m.member); }
                if (m.member == "Hello") {
                    if (!silentHello) reply(c, m, mkReturn(0, "s", strBody(":1.7")));
                } else if (m.member == "AddMatch") {
                    reply(c, m, mkReturn(0));
                } else if (m.member == "RegisterProfile") {
                    std::string pth, uuid;
                    std::map<std::string, std::string> props;
                    parseRegisterBody(m, pth, uuid, props);
                    { std::lock_guard<std::mutex> lk(mu); regProps = props; regUuid = uuid; }
                    if (registerFailures.load() > 0) { --registerFailures; reply(c, m, mkError(0, registerErrorName)); }
                    else reply(c, m, mkReturn(0));
                } else if (m.member == "Get") {
                    if (adapterMissing.load() > 0) { --adapterMissing; reply(c, m, mkError(0, "org.freedesktop.DBus.Error.UnknownObject")); }
                    else reply(c, m, mkReturn(0, "v", variantStrBody("AA:BB:CC:DD:EE:01")));
                } else if (m.member == "UnregisterProfile") {
                    unregisterSeen = true;
                    reply(c, m, mkReturn(0));
                }
            }
        };
        // A BEGIN-nel egy csomagban érkezett üzenetbájtok is feldolgozandók (a kliens rögtön a Hello-t küldi)
        if (!buf.empty()) { dec.feed(reinterpret_cast<const uint8_t*>(buf.data()), buf.size()); handleMessages(); }
        for (;;) {
            if (stop) return;
            if (dropNow.exchange(0)) return;   // a kapcsolat eldobása (a c-t a hívó zárja)
            // kimenő sor
            for (;;) {
                std::pair<std::vector<uint8_t>, int> o;
                { std::lock_guard<std::mutex> lk(mu); if (outbox.empty()) break; o = std::move(outbox.front()); outbox.pop_front(); }
                sendBytes(c, o.first, o.second);
                if (o.second >= 0) ::close(o.second);
            }
            pollfd p[2] = { { c, POLLIN, 0 }, { wake[0], POLLIN, 0 } };
            if (::poll(p, 2, 200) <= 0) continue;
            if (p[1].revents & POLLIN) { char t[64]; while (::read(wake[0], t, sizeof(t)) > 0) {} }
            if (p[0].revents & (POLLIN | POLLHUP)) {
                uint8_t t[4096];
                const ssize_t n = ::recv(c, t, sizeof(t), 0);
                if (n <= 0) { if (unregisterSeen) sawEofAfterUnregister = true; return; }
                dec.feed(t, static_cast<size_t>(n));
                handleMessages();
            }
        }
    }

    void run() {
        while (!stop) {
            pollfd p[2] = { { listenFd, POLLIN, 0 }, { wake[0], POLLIN, 0 } };
            if (::poll(p, 2, 200) <= 0) continue;
            if (p[1].revents & POLLIN) { char t[64]; while (::read(wake[0], t, sizeof(t)) > 0) {} }
            if (p[0].revents & POLLIN) {
                const int c = ::accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
                if (c >= 0) { handleConn(c); ::close(c); }
            }
        }
    }
};

size_t openFdCount() {
    size_t n = 0;
    if (DIR* d = ::opendir("/proc/self/fd")) {
        while (dirent* e = ::readdir(d)) if (e->d_name[0] != '.') ++n;
        ::closedir(d);
    }
    return n;
}

BluezListener::Connector connectorFor(FakeDaemon& d) {
    const std::string addr = d.address();
    return [addr](std::string& err, int timeoutMs) { return std::unique_ptr<BusIo>(UnixBusIo::connect(addr, err, timeoutMs)); };
}

bool waitFor(const std::function<bool()>& cond, int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cond();
}

long long msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count();
}

std::string readSome(int fd, int ms) {
    std::string out;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        pollfd p{ fd, POLLIN, 0 };
        if (::poll(&p, 1, 50) > 0) {
            char b[512];
            const ssize_t n = ::recv(fd, b, sizeof(b), MSG_DONTWAIT);
            if (n <= 0) break;
            out.append(b, static_cast<size_t>(n));
            if (out.find('\n') != std::string::npos) break;
        }
    }
    return out;
}

void testListener() {
    std::printf("[3] BluezListener + UnixBusIo hamis D-Bus démonnal (SASL, szál, SCM_RIGHTS, leállás)\n");

    // 3a. Jó út: nyitás, hitelesítés, regisztráció, kapcsolat átvétele
    {
        FakeDaemon d;
        check(d.start(), "a hamis démon elindul");
        BluezListener l(connectorFor(d));
        check(l.open(), "open(): sikerül");
        check(l.isOpen() && l.address() == "AA:BB:CC:DD:EE:01" && !l.broken() && l.state() == State::Ready,
              "nyitva, a gép Bluetooth-címe az adapterből");
        {
            std::lock_guard<std::mutex> lk(d.mu);
            check(d.authLine == "AUTH EXTERNAL " + [] { std::string h; for (char c : std::to_string(::getuid())) { char b[3]; std::snprintf(b, 3, "%02x", c); h += b; } return h; }(),
                  "SASL EXTERNAL az uid hexa számjegyeivel");
            check(d.log == std::vector<std::string>({ "Hello", "AddMatch", "RegisterProfile", "Get" }), "a hívások sorrendje: Hello, AddMatch, RegisterProfile, Get");
            check(d.regUuid == "43ad541b-d816-443a-a652-146cb5191139" && d.regProps["RequireAuthentication"] == "true" &&
                      d.regProps["RequireAuthorization"] == "false" && d.regProps["Channel"] == "22",
                  "a démon a valódi RegisterProfile-t kapta (UUID, hitelesítés, csatorna)");
        }
        check(l.acceptOne() == -1, "kapcsolat nélkül nincs mit átvenni");

        int sp[2];
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp);
        d.queue(mkNewConnection(5, "/org/bluez/hci0/dev_X", 0, 1), sp[0]);
        int got = -1;
        check(waitFor([&] { got = l.acceptOne(); return got >= 0; }, 1500), "NewConnection: az átadott fájlleíró átvehető");
        if (got >= 0) {
            const int fl = ::fcntl(got, F_GETFL, 0);
            check((fl & O_NONBLOCK) != 0, "az átvett socket nem blokkoló");
            check(::send(sp[1], "ping\n", 5, 0) == 5 && readSome(got, 500) == "ping\n", "adat megy át a kapott socketen (ugyanaz a kapcsolat)");
            check(::send(got, "pong\n", 5, MSG_NOSIGNAL) == 5 && readSome(sp[1], 500) == "pong\n", "és vissza");
            ::close(got);
        }
        ::close(sp[1]);
        // Tiszta leállás
        const auto t0 = std::chrono::steady_clock::now();
        l.close();
        check(msSince(t0) < 1500, "close(): gyorsan leáll (a szál nem akad)");
        check(d.unregisterSeen.load() || waitFor([&] { return d.unregisterSeen.load(); }, 500), "close(): a démon megkapta az UnregisterProfile-t");
        check(waitFor([&] { return d.sawEofAfterUnregister.load(); }, 1000), "…és csak utána zárult a busz kapcsolata");
        check(!l.isOpen() && l.address().empty() && l.acceptOne() == -1, "lezárva: nem nyitott, nincs cím, nincs kapcsolat");
        l.close();
        check(true, "a kétszeri close() ártalmatlan");
        // Újranyitás ugyanazon a példányon
        const bool reopened = l.open() && l.isOpen();
        check(reopened, "újranyitható " + l.lastError());
        l.close();
    }
    // 3b. Az át nem vett kapcsolat leállításkor bezárul
    {
        FakeDaemon d; d.start();
        BluezListener l(connectorFor(d));
        l.open();
        int sp[2];
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp);
        d.queue(mkNewConnection(5, "/d", 0, 1), sp[0]);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        l.close();
        char b[4];
        pollfd p{ sp[1], POLLIN, 0 };
        const bool eof = ::poll(&p, 1, 1000) > 0 && ::recv(sp[1], b, sizeof(b), 0) == 0;
        check(eof, "az át nem vett kapcsolat a leállításkor bezárul (a telefon bontást lát)");
        ::close(sp[1]);
    }
    // 3c. A démon eldobja a kapcsolatot: broken()
    {
        FakeDaemon d; d.start();
        BluezListener l(connectorFor(d));
        l.open();
        d.dropNow = 1;
        d.kick();
        check(waitFor([&] { return l.broken(); }, 2500) && !l.isOpen(), "a busz kapcsolata megszakadt: broken(), nem nyitott");
        l.close();
        check(!l.broken() && !l.isOpen(), "close() után nincs hiba-állapot");
        check(l.open() && l.isOpen(), "a híd újranyithatja a démon visszatérte után");
        l.close();
    }
    // 3d. A BlueZ újraindul: a szál észleli és újraregisztrál (hibás első próbával)
    {
        FakeDaemon d; d.start();
        BluezListener l(connectorFor(d));
        l.open();
        check(d.logCount("RegisterProfile") == 1, "alapállapot: egy regisztráció");
        d.queue(mkNameOwnerChanged("org.bluez", ":1.9", ""));
        check(waitFor([&] { return !l.isOpen(); }, 1500), "a BlueZ eltűnése: isOpen() hamis");
        d.registerFailures = 1;
        d.queue(mkNameOwnerChanged("org.bluez", "", ":1.12"));
        check(d.waitLog("RegisterProfile", 2, 1500), "a BlueZ visszatérése: újraregisztráció");
        check(!l.isOpen(), "…az első próbálkozás hibás: még nem nyitott");
        check(d.waitLog("RegisterProfile", 3, 4500) && waitFor([&] { return l.isOpen(); }, 1500),
              "pár mp múlva újra próbálja, és sikerül: isOpen() ismét igaz, újranyitás nélkül");
        int sp[2];
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp);
        d.queue(mkNewConnection(9, "/d", 0, 1), sp[0]);
        int got = -1;
        check(waitFor([&] { got = l.acceptOne(); return got >= 0; }, 1500), "az újraregisztráció után új kapcsolat is jön");
        if (got >= 0) ::close(got);
        ::close(sp[1]);
        l.close();
    }
    // 3e. Hibás nyitások: okok és idők
    {
        BluezListener none([](std::string& err, int) { err = "x"; return std::unique_ptr<BusIo>(); });
        check(!none.open() && none.lastError() == "x", "a kapcsolat nem épül fel: false és az ok");

        std::string gone;
        { FakeDaemon g; g.start(); gone = g.address(); }   // a démon megszűnt: a socket-fájl sincs meg
        BluezListener nosock([gone](std::string& err, int t) { return std::unique_ptr<BusIo>(UnixBusIo::connect(gone, err, t)); });
        check(!nosock.open(500) && nosock.lastError().find("nem érhető el") != std::string::npos, "nincs rendszerbusz: false, „nem érhető el”");

        FakeDaemon d; d.start();
        d.rejectAuth = true;
        BluezListener l(connectorFor(d));
        check(!l.open(800) && l.lastError().find("hitelesítés") != std::string::npos && !l.isOpen(), "elutasított hitelesítés: false");
        d.rejectAuth = false;
        d.noAuthNegotiate = true;
        check(!l.open(800) && l.lastError().find("fájlleírót") != std::string::npos, "ha nincs fájlleíró-átadás: false (nélküle nem megy)");
        d.noAuthNegotiate = false;
        d.silentHello = true;
        const auto t0 = std::chrono::steady_clock::now();
        check(!l.open(400) && l.lastError().find("nem válaszolt") != std::string::npos && msSince(t0) < 2000, "a démon nem válaszol: időtúllépés ~400 ms után");
        d.silentHello = false;
        d.registerErrorName = "org.freedesktop.DBus.Error.ServiceUnknown";
        d.registerFailures = 1;
        check(!l.open(800) && l.lastError().find("nem fut") != std::string::npos, "nincs BlueZ: a hiba oka „nem fut”");
        d.registerErrorName = "org.freedesktop.DBus.Error.AccessDenied";
        d.registerFailures = 1;
        check(!l.open(800) && l.lastError().find("bluetooth") != std::string::npos, "tiltó D-Bus-szabály: útmutatás a csoportról");
        d.adapterMissing = 10;
        check(!l.open(800) && l.lastError().find("adapter") != std::string::npos, "nincs adapter: false, „nincs Bluetooth-adapter”");
        d.adapterMissing = 0;
        check(l.open(800), "a hibák után ugyanaz a példány újra sikeresen nyit");
        l.close();
    }
    // 3f. Fájlleíró-szivárgás: sok nyitás/zárás, kapcsolatokkal
    {
        FakeDaemon d; d.start();
        BluezListener l(connectorFor(d));
        l.open(); l.close();   // bemelegítés
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const size_t before = openFdCount();
        for (int i = 0; i < 25; ++i) {
            l.open();
            int sp[2];
            ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp);
            d.queue(mkNewConnection(100 + static_cast<uint32_t>(i), "/d", 0, 1), sp[0]);
            waitFor([&] { return d.outboxEmpty(); }, 1000);   // a démon elküldte (és bezárta a saját példányát)
            if (i % 2 == 0) { waitFor([&] { int g = l.acceptOne(); if (g >= 0) ::close(g); return g >= 0; }, 800); }
            ::close(sp[1]);
            l.close();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const size_t after = openFdCount();
        check(after == before, "25 nyitás/zárás kapcsolatokkal: nincs fájlleíró-szivárgás (" + std::to_string(before) + " → " + std::to_string(after) + ")");
    }
}

// ---------------------------------------------------------------------------------------------------- [4] híd
net::socket_t adopt(int fd) { return static_cast<net::socket_t>(fd); }

std::vector<std::string> readLines(int fd, int ms, bool* closed = nullptr) {
    std::vector<std::string> out;
    btlink::LineSplitter sp;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        pollfd p{ fd, POLLIN, 0 };
        if (::poll(&p, 1, 50) <= 0) continue;
        char b[4096];
        const ssize_t n = ::recv(fd, b, sizeof(b), 0);
        if (n <= 0) { if (closed) *closed = true; break; }
        sp.push(b, static_cast<size_t>(n), out);
    }
    return out;
}

bool hasType(const std::vector<std::string>& lines, const char* type) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) {
        json::Value v;
        return json::parse(l, v) && v.get("type") && v.get("type")->stringOr("") == type;
    });
}

void testBridge() {
    std::printf("[4] a valódi híd a hamis BlueZ-on át: ugyanaz a protokoll és token-hitelesítés, mint Windowson\n");
    FakeDaemon d;
    check(d.start(), "a hamis démon elindul");
    ::setenv("DBUS_SYSTEM_BUS_ADDRESS", d.address().c_str(), 1);

    const std::string token = bridge_detail::randomHex(32);
    const std::string remoteDoc =
        "{\"enabled\":true,\"port\":47830,\"bluetooth\":1,\"devices\":[{\"id\":\"0b0c0d0e\",\"name\":\"BT-teszt\",\"hash\":\"" +
        bridge_detail::sha1Hex(token) + "\",\"created\":0,\"last\":0}]}";
    BridgeCallbacks cb;
    cb.pullAudio = [](float*, size_t) -> size_t { return 0; };
    cb.sampleRate = [] { return 48000.0; };
    cb.getParams = [] { return transparentParams(); };
    cb.setParams = [](const ParamData&) {};
    cb.loadRemote = [&] { return remoteDoc; };
    cb.saveRemote = [](const std::string&) { return true; };
    WebSocketBridge bridge(47831);
    const bool started = bridge.start(cb);
    check(started, "a híd elindul");
    if (started) {
        check(d.waitLog("RegisterProfile", 1, 4000), "a híd a Bluetooth-csatornához regisztrálja a profilt a BlueZ-nál");

        // Jó token egy átadott RFCOMM-socketen (itt: socketpár)
        int a[2];
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, a);
        d.queue(mkNewConnection(21, "/org/bluez/hci0/dev_AA", 0, 1), a[0]);
        const std::string hello = "{\"type\":\"hello\",\"client\":\"aqulify-remote\",\"token\":\"" + token + "\"}\n{\"type\":\"get_bt_info\"}\n";
        ::send(a[1], hello.data(), hello.size(), MSG_NOSIGNAL);
        const auto lines = readLines(a[1], 2500);
        check(hasType(lines, "machine") && hasType(lines, "state"), "jó token: megjön a machine és a state üzenet (mint a Windowsos ágon)");
        const auto info = std::find_if(lines.begin(), lines.end(), [](const std::string& l) {
            json::Value v; return json::parse(l, v) && v.get("type") && v.get("type")->stringOr("") == "bt_info"; });
        json::Value iv;
        check(info != lines.end() && json::parse(*info, iv) && iv.get("available") && iv.get("available")->b &&
                  iv.get("addr") && iv.get("addr")->stringOr("") == "AA:BB:CC:DD:EE:01",
              "get_bt_info: available és az adapter címe (a telefon ezt használja)");

        // Rossz token: elutasítás és bontás
        int b[2];
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, b);
        d.queue(mkNewConnection(22, "/org/bluez/hci0/dev_BB", 0, 1), b[0]);
        const std::string bad = "{\"type\":\"hello\",\"token\":\"" + std::string(64, 'f') + "\"}\n";
        ::send(b[1], bad.data(), bad.size(), MSG_NOSIGNAL);
        bool closed = false;
        const auto bl = readLines(b[1], 2500, &closed);
        check(bl.size() == 1 && bl[0].find("\"ok\":false") != std::string::npos && closed, "rossz token: {\"auth\":{\"ok\":false}}, majd bontás");

        // Nem hello első sor
        int c[2];
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, c);
        d.queue(mkNewConnection(23, "/org/bluez/hci0/dev_CC", 0, 1), c[0]);
        const std::string nh = "{\"type\":\"get_state\"}\n";
        ::send(c[1], nh.data(), nh.size(), MSG_NOSIGNAL);
        closed = false;
        const auto cl = readLines(c[1], 2500, &closed);
        check(cl.size() == 1 && cl[0].find("\"ok\":false") != std::string::npos && closed, "nem hello az első sor: elutasítva");

        // A jó kapcsolat közben is él (a rossz nem rántotta le)
        const std::string ping = "{\"type\":\"ping\",\"t\":3}\n";
        ::send(a[1], ping.data(), ping.size(), MSG_NOSIGNAL);
        check(hasType(readLines(a[1], 1500), "pong"), "a jó kapcsolat a rosszak mellett is működik (pong)");

        // Párosítás visszavonása: a Bluetoothon csatlakozott telefont is bontja — a kliens a bridge-en át nem érhető itt, ezért
        // a kapcsolat leállítással zárul: a híd leállítása bontja a soros klienst és tisztán lezárja a profilt
        const auto t0 = std::chrono::steady_clock::now();
        bridge.stop();
        check(msSince(t0) < 4000, "a híd leállítása gyors (a Bluetooth-csatorna lezárása nem akad)");
        check(d.unregisterSeen.load(), "leállításkor a profil törlődik a BlueZ-ban (UnregisterProfile)");
        bool eof = false;
        readLines(a[1], 1500, &eof);
        check(eof, "leálláskor a csatlakozott telefon kapcsolata bomlik");
        ::close(a[1]); ::close(b[1]); ::close(c[1]);
    }
    ::unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
    (void)adopt;
}

void testListenerPlatform() {
    std::printf("[5] RfcommListener: platform-jelző és a bridge-hez illeszkedés\n");
    check(btlink::RfcommListener::isSupported(), "Linuxon a Bluetooth RFCOMM a BlueZ-os ággal támogatott");
    // Rendszerbusz nélküli környezet: az open() hibával tér vissza, nem lóg és nem dob
    ::setenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nem/letezo/aqulify-bus", 1);
    btlink::RfcommListener l;
    const auto t0 = std::chrono::steady_clock::now();
    check(!l.open() && !l.isOpen() && !l.broken() && !l.refresh() && l.acceptOne() == net::kInvalidSocket && msSince(t0) < 2000,
          "nincs busz: open() false gyorsan, refresh()/acceptOne()/broken() ártalmatlan");
    check(!l.lastError().empty() && l.address().empty(), "az ok megvan, a cím üres");
    ::unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
}

// ---------------------------------------------------------------------------------------------------- --probe
/**
 * Segédmód a valódi dbus-daemon elleni próbához (tests/live/bt_bluez_live.py): a DBUS_SYSTEM_BUS_ADDRESS buszon nyit egy
 * RfcommListenert, a kapott kapcsolatokon minden sorra „echo:<sor>”-t válaszol, SIGTERM-re tisztán leáll. Eseményei a
 * szabványos kimeneten, soronként. A híd karbantartását (30 mp) nem utánozza: csak broken() esetén nyit újra, így a
 * BlueZ-újraindulás utáni belső újraregisztráció látszik.
 */
std::atomic<bool> g_term{ false };
void onTerm(int) { g_term = true; }

int runProbe() {
    ::signal(SIGTERM, onTerm);
    ::signal(SIGINT, onTerm);
    btlink::RfcommListener l;
    auto openIt = [&] {
        const bool ok = l.open();
        std::printf("OPEN %d %s %s\n", ok ? 1 : 0, l.address().c_str(), l.lastError().c_str());
        std::fflush(stdout);
    };
    openIt();
    std::vector<int> conns;
    int lastState = -1;
    while (!g_term) {
        if (l.broken()) { l.close(); openIt(); }
        const net::socket_t s = l.acceptOne();
        if (s != net::kInvalidSocket) {
            conns.push_back(static_cast<int>(s));
            std::printf("CONN\n");
            std::fflush(stdout);
        }
        for (int fd : conns) {
            pollfd p{ fd, POLLIN, 0 };
            if (::poll(&p, 1, 0) > 0) {
                char b[256];
                const ssize_t n = ::recv(fd, b, sizeof(b), 0);
                if (n > 0) {
                    const std::string out = "echo:" + std::string(b, static_cast<size_t>(n));
                    ::send(fd, out.data(), out.size(), MSG_NOSIGNAL);
                }
            }
        }
        const int st = l.isOpen() ? 1 : 0;
        if (st != lastState) {
            lastState = st;
            std::printf("STATE %d\n", st);
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    l.close();
    for (int fd : conns) ::close(fd);
    std::printf("CLOSED\n");
    std::fflush(stdout);
    return 0;
}
#endif // __linux__

} // namespace

int main(int argc, char** argv) {
#if defined(__linux__)
    if (argc > 1 && std::string(argv[1]) == "--probe") return runProbe();
#else
    (void)argc; (void)argv;
#endif
    testWire();
    testCaps();
    testSession();
#if defined(__linux__)
    testListener();
    testBridge();
    testListenerPlatform();
#else
    std::printf("[3-5] (csak Linuxon fut: Unix-socketes szállítás, szál, híd)\n");
#endif
    std::printf("\n%d sikeres, %d hibás ellenőrzés.\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
