#pragma once
/**
 * @file dbus_wire.hpp
 * @brief Minimális D-Bus üzenetkódolás/-értelmezés (a „wire” formátum), külső könyvtár nélkül.
 *
 * Csak annyi, amennyi a BlueZ Profile1 regisztrációhoz kell (bt_bluez.hpp): metódushívás / válasz / hiba / jelzés
 * üzenetek, a fejlécmezők, valamint az o, s, g, u, h, q, b, y típusok és az a{sv} szótár. Platformfüggetlen (tiszta
 * bájtműveletek), ezért egységtesztelhető Bluetooth és D-Bus nélkül is. A kódolás mindig kis-endian; az értelmező a
 * nagy-endian üzeneteket is érti.
 */
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace ee_analyzer::dbus {

enum class MsgType : uint8_t { MethodCall = 1, MethodReturn = 2, Error = 3, Signal = 4 };

constexpr uint8_t kFlagNoReplyExpected = 0x1;
/** A hívás ne indítsa el a célszolgáltatást (D-Bus aktiválás): ha nem fut, ServiceUnknown a válasz. */
constexpr uint8_t kFlagNoAutoStart = 0x2;
/** Egy üzenet legnagyobb elfogadott mérete (a specifikáció 128 MiB-ot enged; nekünk ennyi bőven elég). */
constexpr size_t kMaxMessageBytes = 1u << 20;
constexpr size_t kMaxHeaderFieldsBytes = 1u << 16;

/** Egy D-Bus üzenet (fejlécmezők + a törzs nyers bájtjai). */
struct Message {
    MsgType type = MsgType::MethodCall;
    uint8_t flags = 0;
    uint32_t serial = 0;
    uint32_t replySerial = 0;   ///< csak válasznál / hibánál
    std::string path, iface, member, errorName, dest, sender, signature;
    uint32_t unixFds = 0;       ///< a fejlécben jelzett fájlleíró-szám
    bool bigEndian = false;     ///< fogadott üzenetnél: a törzs nagy-endian-e
    std::vector<uint8_t> body;
    std::vector<int> fds;       ///< fogadáskor: a hozzá tartozó (már átvett) fájlleírók
};

/** Bájtpuffer, a D-Bus igazítási szabályaival. */
class Writer {
public:
    void align(size_t n) { while (buf_.size() % n) buf_.push_back(0); }
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { align(2); put(v, 2); }
    void u32(uint32_t v) { align(4); put(v, 4); }
    void boolean(bool v) { u32(v ? 1u : 0u); }
    void str(const std::string& s) {   // s és o típus
        u32(static_cast<uint32_t>(s.size()));
        buf_.insert(buf_.end(), s.begin(), s.end());
        buf_.push_back(0);
    }
    void sig(const std::string& s) {   // g típus
        u8(static_cast<uint8_t>(s.size()));
        buf_.insert(buf_.end(), s.begin(), s.end());
        buf_.push_back(0);
    }
    /** a{sv} bejegyzés kulcs + változat (variant) értékkel. */
    void dictEntryStr(const std::string& key, const std::string& value) {
        align(8); str(key); sig("s"); str(value);
    }
    void dictEntryU16(const std::string& key, uint16_t value) {
        align(8); str(key); sig("q"); u16(value);
    }
    void dictEntryBool(const std::string& key, bool value) {
        align(8); str(key); sig("b"); boolean(value);
    }
    /** a{sv}: az elemeket a fn írja (dictEntry*), a hossz automatikusan kitöltődik. */
    template <class F> void dict(F fn) {
        u32(0);
        const size_t lenPos = buf_.size() - 4;
        align(8);
        const size_t start = buf_.size();
        fn(*this);
        patch32(lenPos, static_cast<uint32_t>(buf_.size() - start));
    }
    void patch32(size_t pos, uint32_t v) {
        for (int i = 0; i < 4; ++i) buf_[pos + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
    }
    void raw(const std::vector<uint8_t>& b) { buf_.insert(buf_.end(), b.begin(), b.end()); }
    std::vector<uint8_t>& bytes() { return buf_; }
    const std::vector<uint8_t>& bytes() const { return buf_; }
    size_t size() const { return buf_.size(); }

private:
    void put(uint32_t v, int n) { for (int i = 0; i < n; ++i) buf_.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    std::vector<uint8_t> buf_;
};

/** Bájtpuffer olvasása, határellenőrzéssel; bármilyen hiba után ok() hamis, az olvasások 0/üres értéket adnak. */
class Reader {
public:
    Reader(const uint8_t* p, size_t n, bool bigEndian = false) : p_(p), n_(n), big_(bigEndian) {}
    bool ok() const { return ok_; }
    size_t pos() const { return pos_; }
    size_t remaining() const { return ok_ ? n_ - pos_ : 0; }

    void align(size_t a) {
        const size_t np = (pos_ + a - 1) / a * a;
        if (np > n_) { ok_ = false; return; }
        pos_ = np;
    }
    uint8_t u8() { return take(1) ? p_[pos_ - 1] : 0; }
    uint16_t u16() { align(2); return static_cast<uint16_t>(num(2)); }
    uint32_t u32() { align(4); return num(4); }
    bool boolean() { return u32() != 0; }
    std::string str() {   // s, o
        const uint32_t len = u32();
        if (!ok_ || len > n_ - pos_) { ok_ = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p_ + pos_), len);
        pos_ += len;
        if (u8() != 0) ok_ = false;   // záró nulla
        return ok_ ? s : std::string();
    }
    std::string sig() {   // g
        const uint8_t len = u8();
        if (!ok_ || len > n_ - pos_) { ok_ = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p_ + pos_), len);
        pos_ += len;
        if (u8() != 0) ok_ = false;
        return ok_ ? s : std::string();
    }
    void skip(size_t n) { if (take(n)) {} }

private:
    bool take(size_t n) {
        if (!ok_ || n > n_ - pos_) { ok_ = false; return false; }
        pos_ += n;
        return true;
    }
    uint32_t num(int n) {
        if (!take(static_cast<size_t>(n))) return 0;
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            const uint32_t b = p_[pos_ - static_cast<size_t>(n) + static_cast<size_t>(i)];
            v |= big_ ? b << (8 * (n - 1 - i)) : b << (8 * i);
        }
        return v;
    }
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
    bool big_;
    bool ok_ = true;
};

/** A teljes üzenet bájtjai (kis-endian). A fds nem kerül bele (azt a szállítás viszi, SCM_RIGHTS-szel). */
inline std::vector<uint8_t> encode(const Message& m) {
    Writer f;   // a fejlécmezők tömbje (az elemek 8 bájtra igazítottak, a kezdet a tömb adatának eleje)
    auto fieldStr = [&](uint8_t code, char t, const std::string& v) {
        f.align(8); f.u8(code);
        f.sig(std::string(1, t));
        if (t == 'g') f.sig(v); else f.str(v);
    };
    auto fieldU32 = [&](uint8_t code, uint32_t v) {
        f.align(8); f.u8(code); f.sig("u"); f.u32(v);
    };
    if (!m.path.empty()) fieldStr(1, 'o', m.path);
    if (!m.iface.empty()) fieldStr(2, 's', m.iface);
    if (!m.member.empty()) fieldStr(3, 's', m.member);
    if (!m.errorName.empty()) fieldStr(4, 's', m.errorName);
    if (m.type == MsgType::MethodReturn || m.type == MsgType::Error) fieldU32(5, m.replySerial);
    if (!m.dest.empty()) fieldStr(6, 's', m.dest);
    if (!m.sender.empty()) fieldStr(7, 's', m.sender);
    if (!m.signature.empty()) fieldStr(8, 'g', m.signature);
    if (m.unixFds) fieldU32(9, m.unixFds);

    Writer w;
    w.u8('l');
    w.u8(static_cast<uint8_t>(m.type));
    w.u8(m.flags);
    w.u8(1);   // protokollverzió
    w.u32(static_cast<uint32_t>(m.body.size()));
    w.u32(m.serial);
    w.u32(static_cast<uint32_t>(f.size()));
    // A fejlécmező-tömb első eleme 8 bájtra igazított (a fejléc 16 bájtnál tart: már igazított).
    w.raw(f.bytes());
    w.align(8);
    w.raw(m.body);
    return w.bytes();
}

/**
 * Bájtfolyam → üzenetek. A fogadott fájlleírókat a szállítás a feed()-nek adja át (érkezési sorrendben), és a
 * fejlécben jelzett számú leíró az épp kész üzenethez kerül.
 */
class Decoder {
public:
    /** Új adat (és a vele érkezett fájlleírók). false: érvénytelen folyam (a kapcsolatot el kell dobni). */
    bool feed(const uint8_t* data, size_t n, const std::vector<int>& fds = {}) {
        if (bad_) return false;
        buf_.insert(buf_.end(), data, data + n);
        fds_.insert(fds_.end(), fds.begin(), fds.end());
        return true;
    }

    enum class Result { NeedMore, Got, Bad };

    /** A következő kész üzenet. Got esetén m kitöltve; Bad után a folyam használhatatlan. */
    Result next(Message& m) {
        if (bad_) return Result::Bad;
        if (buf_.size() < 16) return Result::NeedMore;
        const bool big = buf_[0] == 'B';
        if (!big && buf_[0] != 'l') return fail();
        Reader hr(buf_.data(), 16, big);
        hr.skip(4);
        const uint32_t bodyLen = hr.u32();
        const uint32_t serial = hr.u32();
        const uint32_t fieldsLen = hr.u32();
        if (buf_[3] != 1 || fieldsLen > kMaxHeaderFieldsBytes || bodyLen > kMaxMessageBytes) return fail();
        const size_t hdrEnd = (16 + static_cast<size_t>(fieldsLen) + 7) / 8 * 8;
        const size_t total = hdrEnd + bodyLen;
        if (buf_.size() < total) return Result::NeedMore;

        Message out;
        const uint8_t t = buf_[1];
        if (t < 1 || t > 4) return fail();
        out.type = static_cast<MsgType>(t);
        out.flags = buf_[2];
        out.serial = serial;
        if (serial == 0) return fail();
        Reader r(buf_.data(), 16 + fieldsLen, big);
        r.skip(16);
        while (r.ok() && r.remaining() > 0) {
            r.align(8);
            if (!r.ok() || r.remaining() == 0) break;
            const uint8_t code = r.u8();
            const std::string vs = r.sig();
            if (vs.size() != 1) return fail();
            std::string sv;
            uint32_t uv = 0;
            if (vs[0] == 'o' || vs[0] == 's') sv = r.str();
            else if (vs[0] == 'g') sv = r.sig();
            else if (vs[0] == 'u') uv = r.u32();
            else return fail();
            if (!r.ok()) return fail();
            switch (code) {
                case 1: out.path = sv; break;
                case 2: out.iface = sv; break;
                case 3: out.member = sv; break;
                case 4: out.errorName = sv; break;
                case 5: out.replySerial = uv; break;
                case 6: out.dest = sv; break;
                case 7: out.sender = sv; break;
                case 8: out.signature = sv; break;
                case 9: out.unixFds = uv; break;
                default: break;   // ismeretlen mező: figyelmen kívül
            }
        }
        if (!r.ok()) return fail();
        // Kötelező mezők (a specifikáció szerint)
        if (out.type == MsgType::MethodCall && (out.path.empty() || out.member.empty())) return fail();
        if (out.type == MsgType::Signal && (out.path.empty() || out.iface.empty() || out.member.empty())) return fail();
        if (out.type == MsgType::Error && out.errorName.empty()) return fail();
        if ((out.type == MsgType::MethodReturn || out.type == MsgType::Error) && out.replySerial == 0) return fail();
        if (out.unixFds > fds_.size()) return fail();   // a jelzett leírók nem érkeztek meg
        out.bigEndian = big;
        out.body.assign(buf_.begin() + static_cast<std::ptrdiff_t>(hdrEnd), buf_.begin() + static_cast<std::ptrdiff_t>(total));
        out.fds.assign(fds_.begin(), fds_.begin() + out.unixFds);
        fds_.erase(fds_.begin(), fds_.begin() + out.unixFds);
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(total));
        m = std::move(out);
        return Result::Got;
    }

    /** A még át nem adott (üzenethez nem rendelt) fájlleírók: a hívó zárja be őket, ha a folyam véget ér. */
    std::vector<int> takeStrayFds() { return std::exchange(fds_, {}); }

private:
    Result fail() { bad_ = true; return Result::Bad; }
    std::vector<uint8_t> buf_;
    std::vector<int> fds_;
    bool bad_ = false;
};

} // namespace ee_analyzer::dbus
