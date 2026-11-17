// A 2.x beépített lejátszójának tesztje: dekóderek (FLAC, WAV, MP3, Ogg Vorbis), címkék és borító, tekerés,
// a lejátszó viselkedése (bitpontos kimenet, tekerés, szünet, a szám vége, újraindítás), lekeverés, zenetár,
// lejátszási sor (szünetmentes váltás, eltérő mintavétel, következő / előző, mentés), hibás fájl.
// A tesztfájlok: tests/data/player_* (tools/make_player_test_data.py); a WAV-okat a teszt maga állítja elő.
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_decoder.hpp"
#include "file_player.hpp"
#include "library_bridge.hpp"
#include "music_library.hpp"
#include "play_queue.hpp"
#include "play_stats.hpp"
#include "playlists.hpp"
#include "smart_lists.hpp"
#ifdef _WIN32
#include "../windows/engine/player_output_wasapi.hpp"
#endif

using namespace ee_analyzer;
using namespace ee_analyzer::player;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; std::printf("  [HIBA] " __VA_ARGS__); } else { std::printf("  [OK]   " __VA_ARGS__); } std::printf("\n"); } while (0)
thread_local volatile bool g_rtTrackAlloc = false;
std::atomic<uint64_t> g_rtAllocCount{0};

void* operator new(std::size_t size) {
    if (g_rtTrackAlloc) g_rtAllocCount.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

static constexpr double kPi = 3.14159265358979323846;
// A tesztfájlok mappája: a fordításkor megadott, vagy az AQ_TEST_DATA_DIR környezeti változó (pl. ha a Windows-os
// tesztprogram más gépen fut, mint ahol fordult)
static const std::string kData = std::getenv("AQ_TEST_DATA_DIR") ? std::getenv("AQ_TEST_DATA_DIR") : AQ_TEST_DATA_DIR;

/** Goertzel Hann-ablakkal: az f frekvencia amplitúdója egy csatornán (ch / chans) a [from, from+n) szakaszon. */
static double toneAmp(const std::vector<float>& x, size_t chans, size_t ch, size_t from, size_t n, double f, double sr) {
    std::complex<double> acc(0, 0);
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * kPi * i / (n - 1));
        acc += w * x[(from + i) * chans + ch] * std::polar(1.0, -2 * kPi * f * i / sr);
    }
    return 2.0 * std::abs(acc) / (n * 0.5);
}

static std::vector<float> decodeAll(AudioDecoder& d) {
    std::vector<float> out, buf(4096 * d.channels());
    for (;;) {
        const size_t got = d.read(buf.data(), 4096);
        if (!got) break;
        out.insert(out.end(), buf.begin(), buf.begin() + got * d.channels());
    }
    return out;
}

/** 32 bites lebegőpontos WAV írása (interleaved). */
static void writeWavFloat(const std::string& path, uint32_t rate, uint16_t ch, const std::vector<float>& x) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t data = static_cast<uint32_t>(x.size() * 4);
    f.write("RIFF", 4); u32(36 + data); f.write("WAVEfmt ", 8); u32(16); u16(3); u16(ch); u32(rate);
    u32(rate * ch * 4); u16(static_cast<uint16_t>(ch * 4)); u16(32); f.write("data", 4); u32(data);
    f.write(reinterpret_cast<const char*>(x.data()), data);
}

static std::vector<uint8_t> readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::string tmpPath(const char* name) { return (fs::temp_directory_path() / name).string(); }

/** Próba-kimenet: a render csak a pull() hívására fut, a zár alatt (mint a PipeWire hurok szálán). */
class FakeOutput : public PlayerOutput {
public:
    bool open(uint32_t sr, RenderFn r, std::string*) override {
        std::lock_guard<std::mutex> l(m);
        rate = sr; render = std::move(r); renderDop = nullptr; dopMode = false; opened = true; active = false; ++opens;
        return true;
    }
    bool openDop(uint32_t sr, RenderDopFn r, std::string* err) override {
        if (!allowDop) {
            if (err) *err = dopFailReason;
            return false;
        }
        std::lock_guard<std::mutex> l(m);
        rate = sr; render = nullptr; renderDop = std::move(r); dopMode = true; opened = true; active = false; ++opens;
        return true;
    }
    std::atomic<int> opens{0};   // hányszor nyílt új hangfolyam (szünetmentes váltásnál nem nő)
    void close() override { std::lock_guard<std::mutex> l(m); render = nullptr; renderDop = nullptr; dopMode = false; opened = false; active = false; }
    void setActive(bool a) override { std::lock_guard<std::mutex> l(m); active = a; }
    void lock() override { m.lock(); }
    void unlock() override { m.unlock(); }
    void flush() override { ++flushes; }
    uint64_t consumedFrames() const override { return consumed.load(); }
    /** Kizárólagos módot játszik el (a lejátszó mintavétel-váltáskor csenddel kezd). */
    bool settle = false;
    bool settlesOnRateChange() const override { return settle; }
    bool allowDop = false;
    std::string dopFailReason = "format";
    bool dopMode = false;
    /** A „hangeszköz” által már megszólaltatott keretek (UINT64_MAX: nem ismert, mint a PipeWire-nél). */
    std::atomic<uint64_t> consumed{UINT64_MAX};
    int flushes = 0;
    /** false: a hangfolyam nem fut (szünet / leállítva), ilyenkor a render nem hívódik. */
    bool pull(float* s, size_t n) {
        std::lock_guard<std::mutex> l(m);
        if (!opened || !active || dopMode || !render) return false;
        render(s, n);
        return true;
    }
    bool pullDop(int32_t* s, size_t n) {
        std::lock_guard<std::mutex> l(m);
        if (!opened || !active || !dopMode || !renderDop) return false;
        renderDop(s, n);
        return true;
    }
    std::mutex m;
    uint32_t rate = 0;
    bool opened = false, active = false;
    RenderFn render;
    RenderDopFn renderDop;
};

/** Megvárja, amíg legalább n keret van a pufferben (vagy a fájl végét). */
static void waitBuffered(const FilePlayer& p, size_t n) {
    for (int i = 0; i < 2000 && p.bufferedFrames() < n && !p.decodeAtEnd(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

static bool waitState(const FilePlayer& p, FilePlayer::State s) {
    for (int i = 0; i < 1000 && p.state() != s; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return p.state() == s;
}

/** n keret kivétele 512-es blokkokban (sztereó). */
static std::vector<float> pullFrames(FilePlayer& p, FakeOutput& o, size_t n) {
    std::vector<float> out;
    std::vector<float> blk(512 * 2);
    while (out.size() < n * 2) {
        const size_t k = std::min<size_t>(512, n - out.size() / 2);
        waitBuffered(p, k);
        if (!o.pull(blk.data(), k)) break;
        out.insert(out.end(), blk.begin(), blk.begin() + k * 2);
    }
    return out;
}

/** n DoP-keret kivétele 512-es blokkokban (sztereó 24 bites szavak). */
static std::vector<int32_t> pullDopFrames(FilePlayer& p, FakeOutput& o, size_t n) {
    std::vector<int32_t> out;
    std::vector<int32_t> blk(512 * 2);
    while (out.size() < n * 2) {
        const size_t k = std::min<size_t>(512, n - out.size() / 2);
        waitBuffered(p, k);
        if (!o.pullDop(blk.data(), k)) break;
        out.insert(out.end(), blk.begin(), blk.begin() + k * 2);
    }
    return out;
}

static void testDecoders() {
    std::printf("[1] Dekóderek: szint, frekvencia, hossz, címkék (bal: 440 Hz 0,5; jobb: 1000 Hz 0,25)\n");
    struct Case { const char* file; uint32_t rate, ch; uint64_t frames; uint64_t frameTol; double tolDb; const char* title; };
    const Case cases[] = {
        { "player_stereo44.flac", 44100, 2, 44100, 0, 0.01, "Sztereó FLAC" },
        { "player_mono48.flac", 48000, 1, 24000, 0, 0.01, "Monó FLAC 24 bit" },
        { "player_stereo44.ogg", 44100, 2, 44100, 0, 0.3, "Sztereó Ogg" },
        { "player_stereo44.mp3", 44100, 2, 44100, 1152, 0.3, "Sztereó MP3" },
        { "player_stereo44.m4a", 44100, 2, 44100, 0, 0.01, "Sztereó ALAC" },
        { "player_stereo48.opus", 48000, 2, 48000, 384, 0.5, "Sztereó Opus" },
        { "player_stereo44.wv", 44100, 2, 44100, 0, 0.01, "Sztereó WavPack" },
    };
    for (const Case& c : cases) {
        std::string err;
        auto d = openDecoder(kData + "/" + c.file, &err);
        if (!d) { CHECK(false, "%s: nem nyitható (%s)", c.file, err.c_str()); continue; }
        const std::vector<float> x = decodeAll(*d);
        const uint64_t got = x.size() / c.ch;
        const auto diff = [](uint64_t a, uint64_t b) { return a > b ? a - b : b - a; };
        CHECK(d->sampleRate() == c.rate && d->channels() == c.ch && diff(d->totalFrames(), c.frames) <= c.frameTol &&
              diff(got, d->totalFrames()) <= c.frameTol,
              "%s: %u Hz, %u csatorna, hossz %llu keret (jelzett %llu, várt %llu)", c.file, d->sampleRate(), d->channels(),
              static_cast<unsigned long long>(got), static_cast<unsigned long long>(d->totalFrames()),
              static_cast<unsigned long long>(c.frames));
        const size_t from = static_cast<size_t>(c.rate / 4), n = static_cast<size_t>(c.rate / 4);
        const double l440 = 20 * std::log10(toneAmp(x, c.ch, 0, from, n, 440, c.rate) / 0.5);
        const double lLeak = 20 * std::log10(toneAmp(x, c.ch, 0, from, n, 1000, c.rate) / 0.5 + 1e-12);
        if (c.ch == 2) {
            const double r1k = 20 * std::log10(toneAmp(x, c.ch, 1, from, n, 1000, c.rate) / 0.25);
            const double rLeak = 20 * std::log10(toneAmp(x, c.ch, 1, from, n, 440, c.rate) / 0.25 + 1e-12);
            CHECK(std::abs(l440) < c.tolDb && std::abs(r1k) < c.tolDb && lLeak < -40 && rLeak < -40,
                  "%s: bal 440 Hz %+.3f dB, jobb 1 kHz %+.3f dB, áthallás %.0f / %.0f dB", c.file, l440, r1k, lLeak, rLeak);
        } else {
            CHECK(std::abs(l440) < c.tolDb, "%s: 440 Hz %+.3f dB", c.file, l440);
        }
        const TrackTags& t = d->tags();
        CHECK(t.title == c.title && t.artist == "Aqulify Teszt" && t.album == "Szinuszok",
              "%s: címkék: \"%s\" / \"%s\" / \"%s\"", c.file, t.title.c_str(), t.artist.c_str(), t.album.c_str());
    }
}

static void testSeek() {
    std::printf("[2] Tekerés: a megadott keretnél folytatódik (a teljes dekódolással összevetve)\n");
    const char* files[] = { "player_stereo44.flac", "player_stereo44.ogg", "player_stereo44.mp3", "player_stereo44.m4a", "player_stereo48.opus", "player_stereo44.wv" };
    for (const char* f : files) {
        auto full = openDecoder(kData + "/" + f);
        auto d = openDecoder(kData + "/" + f);
        if (!full || !d) { CHECK(false, "%s: nem nyitható", f); continue; }
        const std::vector<float> all = decodeAll(*full);
        const uint64_t at = (std::string(f).find("48") != std::string::npos) ? 24000 : 22050;
        std::vector<float> part(2048 * 2);
        const bool ok = d->seek(at);
        const size_t got = ok ? d->read(part.data(), 2048) : 0;
        double maxDiff = 0;
        for (size_t i = 0; i < got * 2 && at * 2 + i < all.size(); ++i)
            maxDiff = std::max(maxDiff, static_cast<double>(std::abs(part[i] - all[at * 2 + i])));
        const double tol = (std::string(f).find("opus") != std::string::npos) ? 0.15 : 1e-4;
        CHECK(ok && got == 2048 && maxDiff < tol, "%s: tekerés a %llu. keretre, legnagyobb eltérés %.2e (tűrés %.2e)", f,
              static_cast<unsigned long long>(at), maxDiff, tol);
    }
}

// ---------------------------------------------------------------- szintetikus címkék

static void be32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 3; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }

static std::vector<uint8_t> id3Frame23(const char* id, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> f(id, id + 4);
    be32(f, static_cast<uint32_t>(body.size()));
    f.push_back(0); f.push_back(0);
    f.insert(f.end(), body.begin(), body.end());
    return f;
}

static std::vector<uint8_t> id3Tag23(const std::vector<std::vector<uint8_t>>& frames) {
    std::vector<uint8_t> body;
    for (const auto& f : frames) body.insert(body.end(), f.begin(), f.end());
    std::vector<uint8_t> tag = { 'I', 'D', '3', 3, 0, 0 };
    const uint32_t n = static_cast<uint32_t>(body.size());
    tag.push_back((n >> 21) & 0x7f); tag.push_back((n >> 14) & 0x7f); tag.push_back((n >> 7) & 0x7f); tag.push_back(n & 0x7f);
    tag.insert(tag.end(), body.begin(), body.end());
    return tag;
}

static std::vector<uint8_t> apic(uint8_t type, const std::string& data) {
    std::vector<uint8_t> b = { 0 };                                   // Latin-1
    const std::string mime = "image/jpeg";
    b.insert(b.end(), mime.begin(), mime.end()); b.push_back(0);
    b.push_back(type);
    b.push_back('x'); b.push_back(0);                                  // leírás
    b.insert(b.end(), data.begin(), data.end());
    return b;
}

static void testTags() {
    std::printf("[3] Címkék és borító: ID3v2 (UTF-16, Latin-1, előlap), FLAC PICTURE, Vorbis METADATA_BLOCK_PICTURE\n");
    // TIT2: UTF-16 BOM-mal „Árvíztűrő”; TPE1: Latin-1 „Teszt é”; két kép: előbb egy „egyéb” (0), aztán az előlap (3)
    std::vector<uint8_t> title = { 1, 0xFF, 0xFE };
    for (char16_t c : std::u16string(u"Árvíztűrő")) { title.push_back(c & 0xff); title.push_back(c >> 8); }
    const std::vector<uint8_t> artist = { 0, 'T', 'e', 's', 'z', 't', ' ', 0xE9 };
    const std::string otherPic = "\xFF\xD8\xFF" "egyeb", frontPic = "\xFF\xD8\xFF" "eloap-kep";
    const std::vector<uint8_t> tag = id3Tag23({ id3Frame23("TIT2", title), id3Frame23("TPE1", artist),
                                                id3Frame23("APIC", apic(0, otherPic)), id3Frame23("APIC", apic(3, frontPic)) });
    TrackTags t;
    detail::parseId3v2(tag.data(), tag.size(), t);
    CHECK(t.title == "Árvíztűrő" && t.artist == "Teszt é" && t.artMime == "image/jpeg" && t.art == frontPic,
          "ID3v2.3: cím \"%s\", előadó \"%s\", borító %zu bájt (%s, az előlap)", t.title.c_str(), t.artist.c_str(),
          t.art.size(), t.artMime.c_str());

    // Az MP3 elé illesztett címke: a dekóder is ezt látja, a lejátszó a borítót azonosítóval adja
    std::vector<uint8_t> mp3 = readFile(kData + "/player_stereo44.mp3");
    // a fájl saját (libsndfile) ID3v2 címkéje helyett a fenti
    if (mp3.size() > 10 && std::memcmp(mp3.data(), "ID3", 3) == 0) {
        const size_t old = 10 + ((size_t(mp3[6]) << 21) | (size_t(mp3[7]) << 14) | (size_t(mp3[8]) << 7) | mp3[9]);
        mp3.erase(mp3.begin(), mp3.begin() + static_cast<long>(std::min(old, mp3.size())));
    }
    mp3.insert(mp3.begin(), tag.begin(), tag.end());
    const std::string path = tmpPath("aq_player_tag.mp3");
    { std::ofstream f(path, std::ios::binary); f.write(reinterpret_cast<const char*>(mp3.data()), static_cast<long>(mp3.size())); }
    auto d = openDecoder(path);
    CHECK(d && d->tags().title == "Árvíztűrő" && d->tags().art == frontPic && d->totalFrames() > 40000,
          "MP3 a címkével: cím és borító a dekóderből, a hang ettől függetlenül dekódolható");
    auto* out = new FakeOutput;
    FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
    p.open(path);
    const media::Info mi = p.info();
    std::string mime, bytes;
    CHECK(mi.available && mi.app == "Aqulify" && mi.title == "Árvíztűrő" && !mi.artId.empty() &&
          p.art(mi.artId, mime, bytes) && bytes == frontPic && !p.art("mas", mime, bytes),
          "lejátszó: „most szól” Aqulify, a borító az azonosítójával kérhető (%s)", mi.artId.c_str());
    p.stop();
    d.reset();                 // Windowson a nyitott fájl nem törölhető
    std::error_code ec;
    fs::remove(path, ec);

    // FLAC PICTURE blokk és ugyanez base64-ben Vorbis-megjegyzésként (Ogg / Opus borító)
    std::vector<uint8_t> pic;
    be32(pic, 3); be32(pic, 9); for (char c : std::string("image/png")) pic.push_back(static_cast<uint8_t>(c));
    be32(pic, 0); be32(pic, 1); be32(pic, 1); be32(pic, 24); be32(pic, 0); be32(pic, 4);
    for (char c : std::string("\x89PNG")) pic.push_back(static_cast<uint8_t>(c));
    std::string m2, b2;
    uint32_t type = 0;
    CHECK(detail::parseFlacPicture(pic.data(), pic.size(), m2, b2, type) && m2 == "image/png" && b2 == "\x89PNG" && type == 3,
          "FLAC PICTURE: %s, %zu bájt, típus %u", m2.c_str(), b2.size(), type);
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string enc;
    for (size_t i = 0; i < pic.size(); i += 3) {
        const uint32_t v = (uint32_t(pic[i]) << 16) | (i + 1 < pic.size() ? uint32_t(pic[i + 1]) << 8 : 0) | (i + 2 < pic.size() ? pic[i + 2] : 0);
        enc += b64[(v >> 18) & 63]; enc += b64[(v >> 12) & 63];
        enc += i + 1 < pic.size() ? b64[(v >> 6) & 63] : '='; enc += i + 2 < pic.size() ? b64[v & 63] : '=';
    }
    TrackTags vt;
    const std::string c1 = "title=Kis betűs kulcs", c2 = "METADATA_BLOCK_PICTURE=" + enc, c3 = "ALBUMARTIST=Album Előadó";
    detail::applyVorbisComment(c1.data(), c1.size(), vt);
    detail::applyVorbisComment(c2.data(), c2.size(), vt);
    detail::applyVorbisComment(c3.data(), c3.size(), vt);
    const std::string c4 = "TRACKNUMBER=3/12", c5 = "DISCNUMBER=2", c6 = "DATE=1999-05-01";
    detail::applyVorbisComment(c4.data(), c4.size(), vt);
    detail::applyVorbisComment(c5.data(), c5.size(), vt);
    detail::applyVorbisComment(c6.data(), c6.size(), vt);
    detail::finalizeTags(vt);
    CHECK(vt.title == "Kis betűs kulcs" && vt.albumArtist == "Album Előadó" && vt.artist == "Album Előadó" &&
          vt.art == "\x89PNG" && vt.artMime == "image/png" && vt.track == 3 && vt.disc == 2 && vt.year == 1999,
          "Vorbis-megjegyzés: kis betűs kulcs, ALBUMARTIST (előadó nélkül az előadó helyén), sorszám 3/12 -> %d, "
          "lemez %d, év %d, base64 borító", vt.track, vt.disc, vt.year);

    auto dm4a = openDecoder(kData + "/player_stereo44.m4a");
    CHECK(dm4a && dm4a->tags().title == "Sztereó ALAC" && dm4a->tags().artist == "Aqulify Teszt" &&
          dm4a->tags().album == "Szinuszok" && dm4a->tags().year == 2026 && dm4a->tags().track == 1 &&
          dm4a->tags().genre == "Acoustic",
          "ALAC / M4A címkék: \"%s\" / \"%s\" / \"%s\" (2026, %s, #%d)",
          dm4a ? dm4a->tags().title.c_str() : "", dm4a ? dm4a->tags().artist.c_str() : "",
          dm4a ? dm4a->tags().album.c_str() : "", dm4a ? dm4a->tags().genre.c_str() : "",
          dm4a ? dm4a->tags().track : 0);
}

static void testPlayer() {
    std::printf("[4] Lejátszó: bitpontos kimenet, pozíció, tekerés, szünet, a szám vége, újraindítás\n");
    const uint32_t sr = 48000;
    const size_t n = 2 * sr;
    std::vector<float> src(n * 2);
    for (size_t i = 0; i < n; ++i) {
        src[2 * i] = static_cast<float>(0.5 * std::sin(2 * kPi * 440 * i / sr));
        src[2 * i + 1] = static_cast<float>(0.25 * std::sin(2 * kPi * 1000 * i / sr));
    }
    const std::string path = tmpPath("aq_player_test.wav");
    writeWavFloat(path, sr, 2, src);

    auto* out = new FakeOutput;
    FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
    std::string err;
    CHECK(p.open(path, &err) && out->rate == sr && out->active && p.state() == FilePlayer::State::Playing,
          "megnyitás: a kimenet %u Hz-en, lejátszás", out->rate);

    // fél másodperc: bitpontosan a forrás
    std::vector<float> a = pullFrames(p, *out, sr / 2);
    bool exact = a.size() == sr;
    for (size_t i = 0; exact && i < a.size(); ++i) exact = a[i] == src[i];
    CHECK(exact && std::abs(p.position() - 0.5) < 1e-9, "az első 0,5 s bitpontosan a fájl tartalma, pozíció %.3f s", p.position());

    // tekerés 1,5 s-ra lejátszás közben: a következő minta pontosan onnan jön
    p.seek(1.5);
    std::vector<float> b = pullFrames(p, *out, 1024);
    exact = b.size() == 2048;
    for (size_t i = 0; exact && i < b.size(); ++i) exact = b[i] == src[static_cast<size_t>(1.5 * sr) * 2 + i];
    CHECK(exact, "tekerés 1,5 s-ra: a kimenet a 72000. kerettől folytatódik, bitpontosan");

    // szünet: a hangfolyam áll, a pozíció nem változik
    const double before = p.position();
    p.pause();
    float tmp[64];
    const bool pulled = out->pull(tmp, 32);
    const media::Info paused = p.info();
    CHECK(!pulled && !paused.playing && paused.canPlay && std::abs(p.position() - before) < 1e-9,
          "szünet: a hangfolyam inaktív, a pozíció %.3f s marad", p.position());
    p.play();

    // a vége: a maradék, utána csend, majd vége-állapot; a pozíció a hossz. A vége után a lejátszó bármikor
    // leállíthatja a hangfolyamot (lassú gépen már a 600 keret csend előtt), ezért csak a maradék hossza kötelező.
    const size_t rest = n - static_cast<size_t>(p.position() * sr + 0.5);
    std::vector<float> c = pullFrames(p, *out, rest + 600);
    bool tailSilent = c.size() >= rest * 2;
    for (size_t i = rest * 2; tailSilent && i < c.size(); ++i) tailSilent = c[i] == 0.0f;
    const bool ended = waitState(p, FilePlayer::State::Ended);
    const media::Info e = p.info();
    CHECK(tailSilent && ended && !out->active && !e.playing && std::abs(e.position - 2.0) < 1e-9 && e.duration == 2.0,
          "a fájl végén csend, majd vége-állapot (a hangfolyam leáll), pozíció %.3f / %.3f s", e.position, e.duration);

    // lejátszás a vége után: elölről
    p.play();
    std::vector<float> d = pullFrames(p, *out, 256);
    exact = d.size() == 512;
    for (size_t i = 0; exact && i < d.size(); ++i) exact = d[i] == src[i];
    CHECK(exact && p.state() == FilePlayer::State::Playing, "lejátszás a vége után: elölről indul");

    p.stop();
    CHECK(!p.hasTrack() && !out->opened && !p.info().available, "leállítás: nincs betöltött szám, a hangfolyam bezárult");
    std::error_code ec;
    fs::remove(path, ec);
}

static void testDownmix() {
    std::printf("[5] Csatornák: monó -> két azonos csatorna, 5.1 -> sztereó lekeverés (elülső + 0,707 · középső / hátsó)\n");
    const uint32_t sr = 44100;
    const std::string mono = tmpPath("aq_player_mono.wav"), surround = tmpPath("aq_player_51.wav");
    std::vector<float> m(sr / 10);
    for (size_t i = 0; i < m.size(); ++i) m[i] = static_cast<float>(0.3 * std::sin(2 * kPi * 300 * i / sr));
    writeWavFloat(mono, sr, 1, m);
    std::vector<float> s;
    for (size_t i = 0; i < sr / 10; ++i) { const float fr[6] = { 0.1f, 0.2f, 0.3f, 0.9f, 0.05f, 0.07f }; s.insert(s.end(), fr, fr + 6); }
    writeWavFloat(surround, sr, 6, s);

    auto* o1 = new FakeOutput;
    FilePlayer p1{ std::unique_ptr<PlayerOutput>(o1) };
    p1.open(mono);
    const std::vector<float> a = pullFrames(p1, *o1, 1000);
    bool same = a.size() == 2000;
    for (size_t i = 0; same && i < 1000; ++i) same = a[2 * i] == m[i] && a[2 * i + 1] == m[i];
    CHECK(same, "monó: a bal és a jobb csatorna a forrással azonos");

    auto* o2 = new FakeOutput;
    FilePlayer p2{ std::unique_ptr<PlayerOutput>(o2) };
    p2.open(surround);
    const std::vector<float> b = pullFrames(p2, *o2, 100);
    const float k = 0.70710678f;
    const float wantL = 0.1f + k * 0.3f + k * 0.05f, wantR = 0.2f + k * 0.3f + k * 0.07f;
    CHECK(b.size() == 200 && std::abs(b[0] - wantL) < 1e-6f && std::abs(b[1] - wantR) < 1e-6f,
          "5.1: bal %.4f (várt %.4f), jobb %.4f (várt %.4f), az LFE kimarad", b[0], wantL, b[1], wantR);
    p1.stop();
    p2.stop();
    std::error_code ec;
    fs::remove(mono, ec);
    fs::remove(surround, ec);
}

static void testOutputLatency() {
    std::printf("[7] Kimeneti puffer: a pozíció a hallott helyet mutatja, a vége csak az utolsó minta után jön\n");
    const uint32_t sr = 48000;
    std::vector<float> src(sr / 5 * 2, 0.1f);                 // 0,2 s
    const std::string path = tmpPath("aq_player_latency.wav");
    writeWavFloat(path, sr, 2, src);
    auto* out = new FakeOutput;
    FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
    p.open(path);
    out->consumed = 0;                                         // az eszköz még semmit nem szólaltatott meg
    pullFrames(p, *out, sr / 5 + 1024);                        // minden adat a kimenetnél, utána csend
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const bool stillPlaying = p.state() == FilePlayer::State::Playing;
    const double posWaiting = p.position();
    out->consumed = sr / 10;                                   // a fele elszólt
    const double posHalf = p.position();
    out->consumed = sr / 5;                                    // az utolsó minta is elszólt
    const bool ended = waitState(p, FilePlayer::State::Ended);
    CHECK(stillPlaying && posWaiting == 0.0 && std::abs(posHalf - 0.1) < 1e-9 && ended && std::abs(p.position() - 0.2) < 1e-9,
          "a kimenet pufferében várakozó résznél a lejátszás tart (pozíció %.3f, majd %.3f s), az utolsó minta után vége",
          posWaiting, posHalf);
    p.play();                                                  // elölről: tekerés 0-ra, a kimenet pufferét üríti
    CHECK(out->flushes >= 1 && std::abs(p.position()) < 1e-9, "újraindításkor a kimenet puffere ürül (%d), pozíció 0", out->flushes);
    p.stop();
    std::error_code ec;
    fs::remove(path, ec);
}

static void testLibrary() {
    std::printf("[8] Zenetár: mappák és almappák, albumok, borító, azonosítók, a híd üzenetei\n");
    const fs::path root = fs::temp_directory_path() / "aq_library_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    // root/Szinuszok/{01 sztereó.flac, cover.jpg}, root/Szinuszok/CD2/{a.ogg, b.mp3}, root/Más/Mélyebb/monó.flac,
    // root/Mappa album/címke nélkül.wav, root/Üres/, root/.rejtett/x.flac, root/jegyzet.txt
    fs::create_directories(root / u8"Szinuszok" / "CD2");
    fs::create_directories(root / u8"Más" / u8"Mélyebb");
    fs::create_directories(root / u8"Mappa album");
    fs::create_directories(root / u8"Üres");
    fs::create_directories(root / ".rejtett");
    fs::copy_file(kData + "/player_stereo44.flac", root / u8"Szinuszok" / u8"01 sztereó.flac");
    fs::copy_file(kData + "/player_stereo44.ogg", root / u8"Szinuszok" / "CD2" / "a.ogg");
    fs::copy_file(kData + "/player_stereo44.mp3", root / u8"Szinuszok" / "CD2" / "b.mp3");
    fs::copy_file(kData + "/player_mono48.flac", root / u8"Más" / u8"Mélyebb" / u8"monó.flac");
    fs::copy_file(kData + "/player_mono48.flac", root / ".rejtett" / "x.flac");
    { std::ofstream f(root / u8"Szinuszok" / "cover.jpg", std::ios::binary); f << "\xFF\xD8\xFF" "borito"; }
    { std::ofstream f(root / "jegyzet.txt"); f << "nem zene"; }
    writeWavFloat((root / u8"Mappa album" / u8"címke nélkül.wav").string(), 44100, 2, std::vector<float>(44100 * 2, 0.0f));
    const auto u8s = [](const fs::path& p) { const auto s = p.u8string(); return std::string(s.begin(), s.end()); };

    library::MusicLibrary lib;
    lib.setRoots({ u8s(root) });
    const bool done = lib.waitIdle(20000);
    auto idx = lib.snapshot();
    size_t emptyShown = 0, hiddenShown = 0;
    for (const auto& f : idx->folders) {
        if (f.name == u8s(fs::path(u8"Üres")) && !f.tracks.size()) {
            for (const auto& p : idx->folders)
                for (size_t c : p.folders) if (idx->folders[c].id == f.id) ++emptyShown;
        }
        if (f.name == ".rejtett") ++hiddenShown;
    }
    CHECK(done && idx->tracks.size() == 5 && idx->roots.size() == 1 && emptyShown == 0 && hiddenShown == 0,
          "bejárás: %zu szám (várt 5: a rejtett mappa, a szöveges fájl kimarad), az üres mappa nem látszik", idx->tracks.size());

    // albumok: a négy címkés szám egy album („Szinuszok”, mappától függetlenül), a címke nélküli WAV a mappájáé
    const library::Album* tagged = nullptr;
    const library::Album* byFolder = nullptr;
    for (const auto& a : idx->albums) {
        if (a.title == "Szinuszok") tagged = &a;
        if (a.title == u8s(fs::path(u8"Mappa album"))) byFolder = &a;
    }
    CHECK(idx->albums.size() == 2 && tagged && tagged->tracks.size() == 4 && tagged->artist == "Aqulify Teszt" && byFolder &&
          byFolder->tracks.size() == 1, "albumok: %zu (a címkés négy szám egy album, a címke nélküli a mappáé)", idx->albums.size());
    std::string mp3Dur;
    for (const auto& t : idx->tracks) if (t.file == "b.mp3") { char b[32]; std::snprintf(b, sizeof(b), "%.3f", t.duration); mp3Dur = b; }
    bool durOk = true;
    for (const auto& t : idx->tracks)
        durOk = durOk && std::abs(t.duration - (t.rate == 48000 ? 0.5 : 1.0)) < 1e-6;
    CHECK(durOk, "hosszak a fejlécekből, a fájlok végigolvasása nélkül (MP3: %s s a Xing / Info fejlécből, a kódoló "
          "elő- és utótöltése nélkül)", mp3Dur.c_str());

    // borító: az albumhoz a mappa cover.jpg-je; ismeretlen azonosítóhoz nincs
    std::string mime, bytes;
    const bool albumArt = tagged && lib.art(tagged->id, mime, bytes) && bytes == "\xFF\xD8\xFF" "borito" && mime == "image/jpeg";
    std::string m2, b2;
    CHECK(albumArt && !lib.art("a0000", m2, b2), "albumborító a mappa képéből (%zu bájt, %s)", bytes.size(), mime.c_str());

    // azonosítók: újrabejárás után ugyanazok; a szám útvonala csak indexelt számhoz
    const std::string firstId = idx->tracks.front().id;
    lib.rescan();
    lib.waitIdle(20000);
    std::string path, nope;
    CHECK(lib.snapshot()->trackById.count(firstId) == 1 && lib.trackPath(firstId, path) && !path.empty() &&
          !lib.trackPath("t0123456789abcdef", nope), "az azonosítók újrabejárás után is ugyanazok; ismeretlen azonosítóhoz nincs útvonal");

    // mappanézet: a gyökér a beállított mappa, benne a nem üres almappák névsorrendben
    const std::string rootJson = lib.folderJson("");
    const std::string firstFolder = lib.folderJson(idx->folders[idx->roots[0]].id);
    CHECK(rootJson.find("\"library_folder\"") != std::string::npos && firstFolder.find(u8s(fs::path(u8"Szinuszok"))) != std::string::npos &&
          firstFolder.find(u8s(fs::path(u8"Üres"))) == std::string::npos, "mappanézet: a gyökér és az almappák (az üres nélkül)");

    // a híd üzenetei: a hálózatról a mappák nem állíthatók, a lejátszás azonosítóval megy
    auto* out = new FakeOutput;
    player::FilePlayer fp{ std::unique_ptr<PlayerOutput>(out) };
    player::PlayQueue pq(fp, [&](const std::string& id, std::string& p) { return lib.trackPath(id, p); });
    std::string savedLists;
    player::PlaylistStore pls([&](const std::string& doc) { savedLists = doc; });
    player::PlayStats pstats;
    BridgeCallbacks cb;
    std::string saved;
    library::attachLibrary(cb, lib, pq, pls, pstats, [&](const std::string& doc) { saved = doc; });
    json::Value m;
    json::parse("{\"type\":\"library_set_roots\",\"roots\":[\"/etc\"]}", m);
    const std::string remoteSet = cb.libraryRequest("library_set_roots", m, true);
    const std::string remoteInfo = cb.libraryRequest("library_info", m, true);
    json::Value playMsg;
    json::parse("{\"type\":\"library_play\",\"id\":\"" + firstId + "\"}", playMsg);
    const std::string playRes = cb.libraryRequest("library_play", playMsg, true);
    CHECK(remoteSet.find("\"remote\"") != std::string::npos && saved.empty() && remoteInfo.find("\"roots\"") == std::string::npos &&
          playRes.find("\"ok\":true") != std::string::npos && fp.hasTrack(),
          "híd: a hálózatról mappa nem állítható és a mappák listája nem látszik; lejátszás azonosítóval (a telefonról is)");
    fp.stop();
    const std::string localInfo = cb.libraryRequest("library_info", m, false);
    CHECK(localInfo.find("\"roots\"") != std::string::npos && cb.libraryRequest("library_album", playMsg, false).find("library_error") != std::string::npos,
          "híd: helyi kliensnek a mappák listája is; ismeretlen albumazonosítónál library_error");

    // lejátszási sor a hídon: az album a második számától, a hálózatról is (csak azonosítók utaznak)
    if (tagged && tagged->tracks.size() == 4) {
        const std::string second = idx->tracks[tagged->tracks[1]].id;
        json::Value qm;
        json::parse("{\"type\":\"queue_play\",\"source\":\"album\",\"id\":\"" + tagged->id + "\",\"start\":\"" + second + "\"}", qm);
        const std::string qr = cb.queueRequest("queue_play", qm, true);
        const std::string st = cb.queueStatus();
        json::Value bad;
        json::parse("{\"type\":\"queue_play\",\"source\":\"album\",\"id\":\"a0000\"}", bad);
        const std::string badR = cb.queueRequest("queue_play", bad, true);
        CHECK(qr.find("\"type\":\"queue\"") != std::string::npos && qr.find("\"index\":1") != std::string::npos &&
              pq.size() == 4 && st.find("\"count\":4") != std::string::npos && fp.hasTrack() &&
              badR.find("\"ok\":false") != std::string::npos,
              "híd: queue_play az album második számától (4 tétel, index 1), ismeretlen albumnál hiba");
        fp.stop();
        json::Value qLoad;
        json::parse("{\"type\":\"queue_load\",\"source\":\"album\",\"id\":\"" + tagged->id + "\"}", qLoad);
        const std::string qLoadRes = cb.queueRequest("queue_load", qLoad, true);
        CHECK(qLoadRes.find("\"type\":\"queue\"") != std::string::npos && !fp.hasTrack() && pq.size() == 4,
              "híd: queue_load beállítja a sort, de nem indít lejátszást");

        // listák a hídon: létrehozás az albumból, hozzáadás az elejére, hiányzó szám jelzése, lejátszás listából
        auto req = [&](const std::string& j) {
            json::Value v;
            json::parse(j, v);
            return cb.queueRequest(v.get("type")->stringOr(""), v, true);
        };
        const std::string created = req("{\"type\":\"playlist_create\",\"name\":\"  Kedvencek  \",\"source\":\"album\",\"id\":\"" + tagged->id + "\"}");
        json::Value cv;
        json::parse(created, cv);
        const std::string plId = cv.get("id") ? cv.get("id")->stringOr("") : "";
        const std::string added = req("{\"type\":\"playlist_add\",\"id\":\"" + plId + "\",\"source\":\"tracks\",\"ids\":[\"" + second + "\"],\"position\":0}");
        pls.add(plId, { player::QueueItem{ "t0000000000000000", "Hiányzó", "", "", 1.0 } });
        const std::string got = req("{\"type\":\"playlist_get\",\"id\":\"" + plId + "\"}");
        const std::string listAll = req("{\"type\":\"playlist_list\"}");
        const std::string playL = req("{\"type\":\"queue_play\",\"source\":\"playlist\",\"id\":\"" + plId + "\",\"start\":\"" + second + "\"}");
        const std::string badName = req("{\"type\":\"playlist_create\",\"name\":\"   \"}");
        const std::string unknown = req("{\"type\":\"playlist_rename\",\"id\":\"pnincs\",\"name\":\"x\"}");
        CHECK(created.find("\"name\":\"Kedvencek\"") != std::string::npos && created.find("\"available\":true") != std::string::npos &&
              added.find("\"items\":[{\"id\":\"" + second + "\"") != std::string::npos &&
              got.find("\"available\":false") != std::string::npos && listAll.find("\"count\":6") != std::string::npos &&
              playL.find("\"index\":0") != std::string::npos && pq.size() == 6 && fp.hasTrack() &&
              badName.find("\"error\":\"name\"") != std::string::npos && unknown.find("\"error\":\"unknown\"") != std::string::npos &&
              savedLists.find("Kedvencek") != std::string::npos,
              "híd: lista az albumból (név tisztítva), hozzáadás az elejére, hiányzó szám jelölve, lejátszás listából, hibák");
        const std::string del = req("{\"type\":\"playlist_delete\",\"id\":\"" + plId + "\"}");
        CHECK(del.find("\"type\":\"playlists\"") != std::string::npos && pls.count() == 0, "híd: lista törlése");
        fp.stop();

        // okos lista a hídon: az album szerint (kisbetűvel is), a tartalom lekéréskor számolódik; kézzel nem bővíthető
        const std::string smart = req("{\"type\":\"playlist_create\",\"name\":\"Szinuszok mind\",\"rules\":{\"conditions\":"
                                      "[{\"field\":\"album\",\"op\":\"is\",\"value\":\"szinuszok\"}],\"sort\":\"title\"}}");
        json::Value sv;
        json::parse(smart, sv);
        const std::string sId = sv.get("id") ? sv.get("id")->stringOr("") : "";
        const size_t smartCount = sv.get("items") ? sv.get("items")->arr.size() : 0;
        const std::string smartAdd = req("{\"type\":\"playlist_add\",\"id\":\"" + sId + "\",\"source\":\"tracks\",\"ids\":[\"" + firstId + "\"]}");
        const std::string upd = req("{\"type\":\"playlist_update\",\"id\":\"" + sId + "\",\"rules\":{\"conditions\":"
                                    "[{\"field\":\"album\",\"op\":\"is\",\"value\":\"szinuszok\"}],\"sort\":\"title\",\"limit\":1}}");
        json::Value uv;
        json::parse(upd, uv);
        const std::string badRules = req("{\"type\":\"playlist_create\",\"name\":\"x\",\"rules\":{\"conditions\":[{\"field\":\"added\",\"op\":\"never\"}]}}");
        std::string plain;
        pls.create("Sima", {}, plain);
        const std::string notSmart = req("{\"type\":\"playlist_update\",\"id\":\"" + plain + "\",\"rules\":{}}");
        const std::string playSmart = req("{\"type\":\"queue_play\",\"source\":\"playlist\",\"id\":\"" + sId + "\"}");
        const std::string lists = req("{\"type\":\"playlist_list\"}");
        CHECK(smartCount == 4 && smart.find("\"smart\":true") != std::string::npos && smart.find("\"rules\":{\"match\":\"all\"") != std::string::npos &&
              smartAdd.find("\"error\":\"smart\"") != std::string::npos && uv.get("items") && uv.get("items")->arr.size() == 1 &&
              badRules.find("\"error\":\"rules:condition\"") != std::string::npos && notSmart.find("\"error\":\"notSmart\"") != std::string::npos &&
              playSmart.find("\"type\":\"queue\"") != std::string::npos && pq.size() == 1 &&
              lists.find("\"smart\":true,\"count\":1") != std::string::npos,
              "híd: okos lista (album szerint, 4 szám), kézzel nem bővíthető, a szabály módosítása (1 szám), hibás szabály, "
              "lejátszás okos listából");
        fp.stop();
    } else {
        CHECK(false, "híd: queue_play (az album nem található)");
    }

    // nem létező mappa: üres index, nem hiba
    library::MusicLibrary empty;
    empty.setRoots({ u8s(root / "nincs-ilyen") });
    empty.waitIdle(5000);
    CHECK(empty.snapshot()->tracks.empty() && empty.folderJson("").find("\"folders\":[]") != std::string::npos,
          "nem létező mappa: üres zenetár");
    fs::remove_all(root, ec);
}


static void testLibraryBrowse() {
    std::printf("[8b] Zenetár tallózás (library_browse): mappák, fájlok és szimlinkek szűrése, path traversal védelem\n");
    const fs::path root = fs::temp_directory_path() / "aq_browse_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Mappa1" / "Almappa1");
    fs::create_directories(root / "Mappa2");
    fs::create_directories(root / "Live... 1999");
    fs::create_directories(root / ".rejtett_mappa");
    { std::ofstream f(root / "zene.flac"); f << "test"; }
    { std::ofstream f(root / "Mappa1" / "teszt.mp3"); f << "test"; }
    bool symlinkCreated = false;
    try {
        fs::create_directory_symlink(root / "Mappa2", root / "SzimlinkMappa", ec);
        if (!ec) symlinkCreated = true;
    } catch (...) {}

    library::MusicLibrary lib;
    player::FilePlayer fp{ std::unique_ptr<PlayerOutput>(new FakeOutput) };
    player::PlayQueue pq(fp, [&](const std::string&, std::string&) { return false; });
    player::PlaylistStore pls([](const std::string&) {});
    player::PlayStats pstats;
    BridgeCallbacks cb;
    library::attachLibrary(cb, lib, pq, pls, pstats, [](const std::string&) {});

    const auto u8s = [](const fs::path& p) { const auto s = p.u8string(); return std::string(s.begin(), s.end()); };

    // 1. Üres útvonal (gyökér listázása)
    json::Value reqEmpty;
    json::parse("{\"type\":\"library_browse\",\"path\":\"\"}", reqEmpty);
    std::string resEmpty = cb.libraryRequest("library_browse", reqEmpty, false);
    CHECK(resEmpty.find("\"type\":\"library_browse\"") != std::string::npos &&
          resEmpty.find("\"folders\":[") != std::string::npos,
          "library_browse üres útvonalra visszaadja a gyökér elérési pontokat");

    // 2. Távoli (nem admin) kliens: err("remote")
    std::string resRemote = cb.libraryRequest("library_browse", reqEmpty, true);
    CHECK(resRemote.find("\"error\":\"remote\"") != std::string::npos,
          "library_browse távoli (nem admin) klienstől elutasítva");

    // 3. Valódi mappa tallózása
    json::Value reqRoot;
    json::parse("{\"type\":\"library_browse\",\"path\":\"" + media::jsonEscape(u8s(root)) + "\"}", reqRoot);
    std::string resRoot = cb.libraryRequest("library_browse", reqRoot, false);
    CHECK(resRoot.find("\"name\":\"Mappa1\"") != std::string::npos &&
          resRoot.find("\"name\":\"Mappa2\"") != std::string::npos &&
          resRoot.find("\"name\":\"Live... 1999\"") != std::string::npos &&
          resRoot.find("zene.flac") == std::string::npos &&
          resRoot.find(".rejtett_mappa") == std::string::npos &&
          resRoot.find("Almappa1") == std::string::npos,
          "library_browse: csak közvetlen mappák jelennek meg, fájlok és rejtett mappa kiszűrve, mélyebb almappa nem szerepel");
    if (symlinkCreated) {
        CHECK(resRoot.find("SzimlinkMappa") == std::string::npos,
              "library_browse: a szimlink mappa kiszűrve");
    }

    // 4. Path traversal: '..' és '%2e' elutasítva, de a 'Live... 1999' engedélyezve
    json::Value reqLive;
    json::parse("{\"type\":\"library_browse\",\"path\":\"" + media::jsonEscape(u8s(root / "Live... 1999")) + "\"}", reqLive);
    std::string resLive = cb.libraryRequest("library_browse", reqLive, false);
    CHECK(resLive.find("\"error\":\"denied\"") == std::string::npos && resLive.find("\"type\":\"library_browse\"") != std::string::npos,
          "library_browse: 'Live... 1999' név nem traversal, megnyitható (nem denied)");

    json::Value reqDotDot;
    json::parse("{\"type\":\"library_browse\",\"path\":\"" + media::jsonEscape(u8s(root / "Mappa1" / "..")) + "\"}", reqDotDot);
    std::string resDotDot = cb.libraryRequest("library_browse", reqDotDot, false);
    CHECK(resDotDot.find("\"error\":\"denied\"") != std::string::npos,
          "library_browse: '..' útvonal elutasítva (denied)");

    json::Value reqUrlDot;
    json::parse("{\"type\":\"library_browse\",\"path\":\"" + media::jsonEscape(u8s(root / "Mappa1") + "/%2e%2e/Mappa2") + "\"}", reqUrlDot);
    std::string resUrlDot = cb.libraryRequest("library_browse", reqUrlDot, false);
    CHECK(resUrlDot.find("\"error\":\"denied\"") != std::string::npos,
          "library_browse: '%%2e' útvonal elutasítva (denied)");

    // 5. Nem létező mappa
    json::Value reqNotFound;
    json::parse("{\"type\":\"library_browse\",\"path\":\"" + media::jsonEscape(u8s(root / "nemletezo")) + "\"}", reqNotFound);
    std::string resNotFound = cb.libraryRequest("library_browse", reqNotFound, false);
    CHECK(resNotFound.find("\"error\":\"notfound\"") != std::string::npos,
          "library_browse: nem létező útvonal hiba (notfound)");

    fs::remove_all(root, ec);
}

static bool waitUntil(const std::function<bool()>& pred, int ms = 2000) {
    for (int i = 0; i < ms && !pred(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return pred();
}

/** A sor tesztfájljai: id -> útvonal (az "tx" ismeretlen, a sor átugorja). */
struct QueueFiles {
    std::map<std::string, std::string> path;
    std::map<std::string, std::vector<float>> stereo;   // a várt kimenet (sztereó, a monó két azonos csatornával)
    player::PlayQueue::Resolve resolve() {
        return [this](const std::string& id, std::string& p) {
            auto it = path.find(id);
            if (it == path.end()) return false;
            p = it->second;
            return true;
        };
    }
    void add(const std::string& id, uint32_t rate, uint16_t ch, size_t frames, float seed) {
        std::vector<float> x(frames * ch), st(frames * 2);
        for (size_t i = 0; i < frames; ++i) {
            for (uint16_t c = 0; c < ch; ++c) x[i * ch + c] = static_cast<float>((i * 7 + c * 3) % 1000) / 1000.0f * seed - 0.25f;
            st[2 * i] = x[i * ch];
            st[2 * i + 1] = x[i * ch + (ch > 1 ? 1 : 0)];
        }
        const std::string p = tmpPath(("aq_queue_" + id + ".wav").c_str());
        writeWavFloat(p, rate, ch, x);
        path[id] = p;
        stereo[id] = std::move(st);
    }
    std::vector<player::QueueItem> items(std::initializer_list<const char*> ids) {
        std::vector<player::QueueItem> v;
        for (const char* id : ids) { player::QueueItem it; it.id = id; it.title = id; v.push_back(it); }
        return v;
    }
    ~QueueFiles() {
        std::error_code ec;
        for (auto& [id, p] : path) fs::remove(p, ec);
    }
};

static void testQueue() {
    std::printf("[9] Lejátszási sor: szünetmentes váltás, eltérő mintavétel, következő / előző, átugrás, mentés\n");
    QueueFiles f;
    f.add("ta", 48000, 2, 24000, 0.5f);    // 0,5 s sztereó
    f.add("tb", 48000, 1, 14400, 0.4f);    // 0,3 s monó, ugyanazon a mintavételen
    f.add("tc", 44100, 2, 8820, 0.3f);     // 0,2 s, más mintavétel
    f.add("td", 48000, 2, 9600, 0.6f);     // 0,2 s
    f.add("te", 48000, 2, 4 * 48000, 0.2f);   // 4 s (az „előző” 3 másodperces szabályához)
    const size_t nA = 24000, nB = 14400, nC = 8820, nD = 9600;

    {   // szünetmentes váltás: A után B ugyanabban a hangfolyamban, mintapontosan
        auto* out = new FakeOutput;
        FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
        PlayQueue q(p, f.resolve());
        const bool started = q.play(f.items({ "ta", "tb" }), 0);
        std::vector<float> got = pullFrames(p, *out, nA + 1000);
        const media::Info mid = q.mediaInfo();
        const int midIdx = q.currentIndex();
        const std::vector<float> rest = pullFrames(p, *out, nB - 1000);
        got.insert(got.end(), rest.begin(), rest.end());
        std::vector<float> want = f.stereo["ta"];
        want.insert(want.end(), f.stereo["tb"].begin(), f.stereo["tb"].end());
        bool exact = got.size() == want.size();
        size_t firstDiff = 0;
        for (size_t i = 0; exact && i < got.size(); ++i) if (got[i] != want[i]) { exact = false; firstDiff = i / 2; }
        CHECK(started && exact && out->opens == 1, "A -> B szünetmentesen: %zu keret bitpontosan a két fájl egymás után%s, "
              "egy hangfolyam (%d megnyitás)", got.size() / 2, exact ? "" : (" (eltérés: " + std::to_string(firstDiff) + ")").c_str(),
              out->opens.load());
        CHECK(midIdx == 1 && mid.title == "aq_queue_tb" && std::abs(mid.position - 1000.0 / 48000) < 1e-9 && !mid.canNext &&
              mid.canPrevious, "a váltás a hallott ponton: 1000 kerettel a vége után a B szól (index %d, „%s”, %.4f s)",
              midIdx, mid.title.c_str(), mid.position);
        float tmp[1024];
        out->pull(tmp, 512);
        const bool ended = waitState(p, FilePlayer::State::Ended);
        CHECK(ended && q.currentIndex() == 1 && !q.mediaInfo().canNext, "a sor végén vége-állapot, az utolsó tételnél marad");
    }

    {   // eltérő mintavétel: D (48 kHz) után C (44,1 kHz) új hangfolyamon, a C bitpontosan
        auto* out = new FakeOutput;
        FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
        PlayQueue q(p, f.resolve());
        q.play(f.items({ "td", "tc" }), 0);
        pullFrames(p, *out, nD);
        float tmp[1024];
        out->pull(tmp, 512);                                  // a D vége után: a lejátszó új hangfolyamot nyit
        const bool reopened = waitUntil([&] { return out->rate == 44100 && out->active && q.currentIndex() == 1; });
        const std::vector<float> c = pullFrames(p, *out, nC);
        CHECK(reopened && out->opens == 2 && c == f.stereo["tc"] && p.info().title == "aq_queue_tc",
              "D (48 kHz) -> C (44,1 kHz): új hangfolyam %u Hz-en (%d megnyitás), a C bitpontosan", out->rate, out->opens.load());
    }

    {   // kattanásmentes váltás kizárólagos módban: a hangfolyam elején csend, amíg a DAC az új órajelre áll
        auto* out = new FakeOutput;
        out->settle = true;
        FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
        PlayQueue q(p, f.resolve());
        q.play(f.items({ "td", "tc" }), 0);
        const size_t leadD = 48000u * FilePlayer::kRateSettleMs / 1000, leadC = 44100u * FilePlayer::kRateSettleMs / 1000;
        const double posInLead = p.position();
        const std::vector<float> first = pullFrames(p, *out, leadD + nD);
        const bool zerosD = first.size() == (leadD + nD) * 2 &&
                            std::all_of(first.begin(), first.begin() + leadD * 2, [](float v) { return v == 0.0f; });
        const bool exactD = zerosD && std::equal(first.begin() + leadD * 2, first.end(), f.stereo["td"].begin());
        CHECK(zerosD && exactD && posInLead == 0.0,
              "kizárólagos mód, első hangfolyam: %zu keret (%u ms) csend, utána a D bitpontosan; a pozíció a csend alatt 0",
              leadD, FilePlayer::kRateSettleMs);
        float tmp[1024];
        out->pull(tmp, 512);
        const bool reopened = waitUntil([&] { return out->rate == 44100 && out->active && q.currentIndex() == 1; });
        const uint64_t leadLeft = p.leadInFrames();
        const std::vector<float> second = pullFrames(p, *out, leadC + nC);
        const bool zerosC = second.size() == (leadC + nC) * 2 &&
                            std::all_of(second.begin(), second.begin() + leadC * 2, [](float v) { return v == 0.0f; });
        const bool exactC = zerosC && std::equal(second.begin() + leadC * 2, second.end(), f.stereo["tc"].begin());
        CHECK(reopened && leadLeft == leadC && zerosC && exactC,
              "eltérő mintavétel (48 -> 44,1 kHz): az új hangfolyam %zu keret csenddel kezd, utána a C bitpontosan", leadC);
    }
    {   // azonos mintavétel kizárólagos módban: a szünetmentes váltásnál nincs csend (csak a legelső indításkor)
        auto* out = new FakeOutput;
        out->settle = true;
        FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
        PlayQueue q(p, f.resolve());
        q.play(f.items({ "ta", "tb" }), 0);
        const size_t lead = 48000u * FilePlayer::kRateSettleMs / 1000;
        const std::vector<float> got = pullFrames(p, *out, lead + nA + nB);
        std::vector<float> want(lead * 2, 0.0f);
        want.insert(want.end(), f.stereo["ta"].begin(), f.stereo["ta"].end());
        want.insert(want.end(), f.stereo["tb"].begin(), f.stereo["tb"].end());
        CHECK(got == want && out->opens == 1, "azonos mintavétel: csak az indításkor csend, A -> B között nincs (egy hangfolyam)");
    }
    {   // kizárólagos mód, kézi számváltás azonos mintavételen: a hangfolyam marad (nincs újranyitás)
        auto* out = new FakeOutput;
        out->settle = true;
        FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
        const size_t lead = 48000u * FilePlayer::kRateSettleMs / 1000;
        p.open(f.path["ta"]);
        pullFrames(p, *out, lead + 1000);                     // az indítási csend és a szám eleje elszól
        p.open(f.path["td"]);                                // szóló zene közben: azonnal, csend nélkül
        const uint64_t leadPlaying = p.leadInFrames();
        const std::vector<float> d = pullFrames(p, *out, nD);
        CHECK(out->opens == 1 && leadPlaying == 0 && d == f.stereo["td"],
              "szóló zene közbeni számváltás azonos mintavételen: ugyanaz a hangfolyam (%d megnyitás), csend nélkül, a D "
              "bitpontosan", out->opens.load());
        float tmp[1024];
        out->pull(tmp, 512);
        const bool ended = waitState(p, FilePlayer::State::Ended);
        p.open(f.path["tb"]);                                // a leállt kimenet újraindul: csenddel
        const uint64_t leadAfterEnd = p.leadInFrames();
        const std::vector<float> b = pullFrames(p, *out, lead + nB);
        const bool zeros = b.size() == (lead + nB) * 2 &&
                           std::all_of(b.begin(), b.begin() + lead * 2, [](float v) { return v == 0.0f; });
        CHECK(ended && out->opens == 1 && leadAfterEnd == lead && zeros &&
              std::equal(b.begin() + lead * 2, b.end(), f.stereo["tb"].begin()),
              "a szám vége után új szám azonos mintavételen: a hangfolyam marad, az újrainduláskor %zu keret csend", lead);
        p.open(f.path["tc"]);                                // eltérő mintavétel: új hangfolyam, csenddel
        CHECK(out->opens == 2 && out->rate == 44100 && p.leadInFrames() == 44100u * FilePlayer::kRateSettleMs / 1000,
              "eltérő mintavételű kézi váltás: új hangfolyam (%d megnyitás), csenddel", out->opens.load());
    }
    {   // megosztott mód (nem kizárólagos): soha nincs csend
        auto* out = new FakeOutput;
        FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
        PlayQueue q(p, f.resolve());
        q.play(f.items({ "td" }), 0);
        CHECK(p.leadInFrames() == 0, "megosztott módban nincs eleji csend");
    }

    std::string savedDoc;
    {   // következő / előző, az „előző” 3 másodperces szabálya
        auto* out = new FakeOutput;
        FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
        PlayQueue q(p, f.resolve(), [&](const std::string& d) { savedDoc = d; });
        q.play(f.items({ "ta", "te", "td" }), 0);
        const bool n1 = q.next() && q.currentIndex() == 1;
        p.seek(3.5);
        const bool restart = q.previous() && q.currentIndex() == 1 && p.position() < 0.01;
        const bool back = q.previous() && q.currentIndex() == 0;
        const bool first = q.previous() && q.currentIndex() == 0 && p.position() < 0.01;
        q.jump(2);
        const media::Info last = q.mediaInfo();
        const bool noNext = !q.next() && q.currentIndex() == 2;
        q.jump(1);
        CHECK(n1 && restart && back && first && !last.canNext && last.canPrevious && noNext,
              "következő; az „előző” 3 s után a szám elejére, előtte az előző számra, az elsőnél az elejére; az utolsónál "
              "nincs következő");
    }

    {   // a mentett sor visszaáll: a tételek és a sorszám, lejátszás nélkül; utána onnan indítható
        auto* out = new FakeOutput;
        FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
        PlayQueue q(p, f.resolve());
        const bool ok = q.restore(savedDoc);
        const bool idle = !p.hasTrack() && !out->opened;
        const int idx = q.currentIndex();
        const bool resumed = q.jump(static_cast<size_t>(idx)) && p.info().title == "aq_queue_te";
        CHECK(ok && idle && q.size() == 3 && idx == 1 && resumed && q.json().find("\"id\":\"td\"") != std::string::npos,
              "mentés: a sor (3 tétel) és a sorszám (%d) visszaáll, a lejátszó nem nyílik meg; onnan indítható", idx);
        CHECK(!q.restore("") && !q.restore("{\"items\":[]}") && q.size() == 3, "hibás vagy üres mentés: a sor nem változik");
    }

    {   // borító nélküli fájl: a „most szól” a mappa képét mutatja (cover.png); kép nélküli mappában nincs borító
        const fs::path dir = fs::temp_directory_path() / "aq_cover_test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir / "kep");
        fs::create_directories(dir / "ures");
        writeWavFloat((dir / "kep" / "a.wav").string(), 48000, 2, std::vector<float>(9600, 0.0f));
        writeWavFloat((dir / "ures" / "b.wav").string(), 48000, 2, std::vector<float>(9600, 0.0f));
        { std::ofstream c(dir / "kep" / "Cover.PNG", std::ios::binary); c << "\x89PNG-borito"; }
        auto* out = new FakeOutput;
        FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
        p.open((dir / "kep" / "a.wav").string());
        const media::Info withCover = p.info();
        std::string mime, bytes;
        const bool got = !withCover.artId.empty() && p.art(withCover.artId, mime, bytes);
        p.open((dir / "ures" / "b.wav").string());
        const media::Info noCover = p.info();
        p.stop();
        CHECK(got && bytes == "\x89PNG-borito" && mime == "image/png" && noCover.artId.empty(),
              "borító: beágyazott kép nélkül a mappa képe (Cover.PNG, %s, %zu bájt); kép nélküli mappában nincs", mime.c_str(), bytes.size());
        fs::remove_all(dir, ec);
    }

    {   // a sorban ismeretlen tétel: a szünetmentes lánc átugorja
        auto* out = new FakeOutput;
        FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
        PlayQueue q(p, f.resolve());
        q.play(f.items({ "ta", "tx", "tb" }), 0);
        pullFrames(p, *out, nA + 500);
        CHECK(q.currentIndex() == 2 && out->opens == 1 && p.info().title == "aq_queue_tb",
              "ismeretlen tétel a sorban: átugorja, a következő szünetmentesen szól (index %d)", q.currentIndex());
    }
}

static void testQueueEdits() {
    std::printf("[10] Sor szerkesztése és listák: beszúrás / törlés / átrendezés előre betöltött következő számmal, "
                "ismétlés, keverés, mentés\n");
    QueueFiles f;
    f.add("ta", 48000, 2, 24000, 0.5f);
    f.add("tb", 48000, 1, 14400, 0.4f);
    f.add("td", 48000, 2, 9600, 0.6f);
    f.add("te", 48000, 2, 4800, 0.2f);
    auto cat = [&](std::initializer_list<const char*> ids) {
        std::vector<float> v;
        for (const char* id : ids) v.insert(v.end(), f.stereo[id].begin(), f.stereo[id].end());
        return v;
    };
    auto frames = [&](std::initializer_list<const char*> ids) {
        size_t n = 0;
        for (const char* id : ids) n += f.stereo[id].size() / 2;
        return n;
    };
    // A sor elindul, a lejátszó 1000 keretet kiad (a rövid A után a következő szám már a pufferben van), utána
    // a szerkesztés; a kimenet a szerkesztett sor szerint, bitpontosan
    auto run = [&](std::initializer_list<const char*> start, const std::function<void(PlayQueue&)>& edit,
                   std::initializer_list<const char*> expect, int& opens) {
        auto* out = new FakeOutput;
        FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
        PlayQueue q(p, f.resolve());
        q.play(f.items(start), 0);
        std::vector<float> got = pullFrames(p, *out, 1000);
        // a következő szám is előre betöltődött (vagy a dekóder a sor végéig ért)
        waitUntil([&] { return p.decodeAtEnd() || p.bufferedFrames() > 30000; });
        edit(q);
        const std::vector<float> rest = pullFrames(p, *out, frames(expect) - 1000);
        got.insert(got.end(), rest.begin(), rest.end());
        opens = out->opens.load();
        return got == cat(expect);
    };
    int o1 = 0, o2 = 0, o3 = 0;
    const bool insNext = run({ "ta", "tb" }, [&](PlayQueue& q) { q.add(f.items({ "td" }), true); }, { "ta", "td", "tb" }, o1);
    const bool remNext = run({ "ta", "tb", "td" }, [&](PlayQueue& q) { q.remove(1); }, { "ta", "td" }, o2);
    const bool moved = run({ "ta", "tb", "td" }, [&](PlayQueue& q) { q.move(2, 1); }, { "ta", "td", "tb" }, o3);
    CHECK(insNext && remNext && moved && o1 == 1 && o2 == 1 && o3 == 1,
          "az előre betöltött következő szám után: „következőként” beszúrás, a következő törlése, átrendezés – a "
          "kimenet a szerkesztett sor szerint, bitpontosan, egy hangfolyamon");

    {   // a hallott tétel törlése: a helyére lépő szól; az ürítés leállítja a lejátszást
        auto* out = new FakeOutput;
        FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
        PlayQueue q(p, f.resolve());
        q.play(f.items({ "ta", "tb" }), 0);
        q.remove(0);
        const bool nowB = q.currentIndex() == 0 && p.info().title == "aq_queue_tb" && q.size() == 1;
        q.clear();
        CHECK(nowB && !p.hasTrack() && q.size() == 0, "a hallott tétel törlése: a következő szól; ürítés: a lejátszás leáll");
    }

    {   // ismétlés: egy szám (szünetmentesen újra), az egész sor (az utolsó után az első)
        int oOne = 0, oAll = 0;
        const bool one = run({ "td" }, [&](PlayQueue& q) { q.setRepeat(PlayQueue::Repeat::One); }, { "td", "td", "td" }, oOne);
        const bool all = run({ "td", "te" }, [&](PlayQueue& q) { q.setRepeat(PlayQueue::Repeat::All); }, { "td", "te", "td", "te" }, oAll);
        auto* out = new FakeOutput;
        FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
        PlayQueue q(p, f.resolve());
        q.play(f.items({ "td", "te" }), 1);
        const bool lastNoNext = !q.mediaInfo().canNext;
        q.setRepeat(PlayQueue::Repeat::All);
        const bool wraps = q.mediaInfo().canNext && q.next() && q.currentIndex() == 0;
        CHECK(one && all && oOne == 1 && oAll == 1 && lastNoNext && wraps,
              "ismétlés: egy szám háromszor egymás után, az egész sor körbe, szünet nélkül; az utolsónál a „következő” az első");
    }

    {   // keverés: a hallott tétel a helyén, a többi ugyanaz más sorrendben; kikapcsoláskor az eredeti sorrend,
        // a keverés közben hozzáadott a végén; mentés és visszaállítás az ismétléssel együtt
        std::vector<player::QueueItem> many;
        for (int i = 0; i < 40; ++i) { player::QueueItem it; it.id = "ta"; it.title = "t" + std::to_string(i); many.push_back(it); }
        auto* out = new FakeOutput;
        FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
        std::string saved;
        PlayQueue q(p, f.resolve(), [&](const std::string& d) { saved = d; });
        q.play(many, 0);
        q.setShuffle(true);
        auto titles = [&](const std::vector<player::QueueItem>& v) { std::vector<std::string> t; for (auto& i : v) t.push_back(i.title); return t; };
        const auto shuffled = titles(q.items());
        auto sorted = shuffled;
        std::sort(sorted.begin(), sorted.end());
        auto want = titles(many);
        std::sort(want.begin(), want.end());
        const bool sameSet = sorted == want && shuffled[0] == "t0" && shuffled != titles(many);
        player::QueueItem extra; extra.id = "tb"; extra.title = "uj";
        q.add({ extra }, false);
        q.setRepeat(PlayQueue::Repeat::All);
        // mentés keverve: visszaállítás után ugyanaz a sorrend, az ismétlés és a keverés megmarad
        auto* out2 = new FakeOutput;
        FilePlayer p2{ std::unique_ptr<PlayerOutput>(out2) };
        PlayQueue r(p2, f.resolve());
        const bool restored = r.restore(saved) && titles(r.items()) == titles(q.items()) && r.shuffle() &&
                              r.repeat() == PlayQueue::Repeat::All;
        r.setShuffle(false);
        q.setShuffle(false);
        auto back = titles(many);
        back.push_back("uj");
        CHECK(sameSet && titles(q.items()) == back && restored && titles(r.items()) == back,
              "keverés: a hallott tétel elöl, a többi más sorrendben; kikapcsolva az eredeti sorrend (a közben "
              "hozzáadott a végén); mentés és visszaállítás keverve, ismétléssel");
    }

    {   // a listák tárolója: műveletek, hibák, mentés és visszatöltés, a név tisztítása
        std::string saved;
        player::PlaylistStore s([&](const std::string& d) { saved = d; });
        std::string a, b;
        const bool c1 = s.create("Első", f.items({ "ta", "tb" }), a).empty() && s.create("Második", {}, b).empty() && a != b;
        const bool errs = s.create(" \t ", {}, b) == "name" && s.rename("pnincs", "x") == "unknown" &&
                          s.add(a, {}, -1) == "empty" && s.move(a, 0, 5) == "index" && s.removeItems(a, { 9 }) == "index";
        s.add(a, f.items({ "td" }), 1);                           // ta, td, tb
        s.move(a, 2, 0);                                          // tb, ta, td
        s.removeItems(a, { 1, 1, 7 });                            // tb, td
        s.rename(b, "Átnevezett");
        player::Playlist pa;
        s.get(a, pa);
        const bool ops = pa.items.size() == 2 && pa.items[0].id == "tb" && pa.items[1].id == "td";
        player::PlaylistStore t;
        const bool loaded = t.load(saved) && t.count() == 2 && t.doc() == s.doc() && !t.load("{\"x\":1}") && t.count() == 2;
        std::string longName;
        for (int i = 0; i < 150; ++i) longName += "\xC3\xA9";        // „é” UTF-8-ban, összesen 300 bájt
        const std::string cut = player::PlaylistStore::cleanName(longName);
        CHECK(c1 && errs && ops && loaded && cut.size() == 200 && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80 &&
              player::PlaylistStore::cleanName("  a\tb  ") == "a b",
              "listák: létrehozás, hozzáadás, átrendezés, törlés, átnevezés, hibakódok, mentés és visszatöltés, a név "
              "tisztítása (200 bájt, karakterhatáron)");
    }
}

static void testSmart() {
    std::printf("[11] Okos listák: műfaj, kisbetűsítés, hallgatásmérő, napló, szabályok, kiértékelés\n");
    using player::detail::normalizeGenre;
    TrackTags tg;
    player::detail::applyVorbisComment("GENRE=Klasszikus", 16, tg);
    TrackTags t3;
    std::vector<uint8_t> tcon = { 0 };
    for (char c : std::string("(32)")) tcon.push_back(static_cast<uint8_t>(c));
    const auto tag = id3Tag23({ id3Frame23("TCON", tcon) });
    player::detail::parseId3v2(tag.data(), tag.size(), t3);
    CHECK(normalizeGenre("(17)") == "Rock" && normalizeGenre("17") == "Rock" && normalizeGenre("(17)Saját") == "Saját" &&
          normalizeGenre("(RX)") == "Remix" && normalizeGenre(" Jazz ") == "Jazz" && normalizeGenre("(200)").empty() &&
          tg.genre == "Klasszikus" && t3.genre == "Classical",
          "műfaj: ID3 számkód és szöveg, Vorbis GENRE, ID3v2 TCON (\"%s\", \"%s\")", tg.genre.c_str(), t3.genre.c_str());
    CHECK(library::foldCase("\xC5\x90R\xC3\x9CLT \xC3\x89nek ABC") == "\xC5\x91r\xC3\xBClt \xC3\xA9nek abc" &&
          library::foldCase("\xC3\x81\xC3\x89\xC3\x8D\xC3\x93\xC3\x96\xC3\x9A\xC5\xB0") == "\xC3\xA1\xC3\xA9\xC3\xAD\xC3\xB3\xC3\xB6\xC3\xBA\xC5\xB1",
          "kisbetűsítés: „ŐRÜLT Ének” -> „őrült ének”, ÁÉÍÓÖÚŰ -> áéíóöúű");

    {   // hallgatásmérő: 30 s hallott idő (rövid számnál a hossz fele), az előre tekerés nem számít, újrakezdés új lejátszás
        player::ListenTracker t;
        int counted = 0;
        double pos = 0;
        t.update(7, 0, 200, true, 0.25);
        for (int i = 0; i < 200; ++i) { pos += 0.25; counted += t.update(7, pos, 200, true, 0.25); }   // 50 s
        const bool once = counted == 1;
        player::ListenTracker s;
        int seekCount = 0;
        s.update(8, 0, 200, true, 0.25);
        seekCount += s.update(8, 100, 200, true, 0.25);          // tekerés 100 s-ra: nem hallott idő
        for (int i = 0; i < 40; ++i) seekCount += s.update(8, 100 + 0.25 * (i + 1), 200, true, 0.25);   // 10 s
        const bool seekNot = seekCount == 0;
        int again = 0;
        pos = 0;
        s.update(9, 0, 20, true, 0.25);                            // új tétel: 20 s-os szám (a fele: 10 s)
        for (int i = 0; i < 44; ++i) { pos += 0.25; again += s.update(9, pos, 20, true, 0.25); }    // 11 s
        for (int i = 0; i < 44; ++i) again += s.update(9, 0.25 * (i + 1), 20, true, 0.25);          // újrakezdés, 11 s
        int paused = 0;
        player::ListenTracker p;
        p.update(10, 0, 200, false, 0.25);
        for (int i = 0; i < 200; ++i) paused += p.update(10, 0, 200, false, 0.25);
        CHECK(once && seekNot && again == 2 && paused == 0,
              "hallgatásmérő: 30 s után egyszer; az előre tekerés nem számít; rövid számnál a fele; újrakezdés új "
              "lejátszás; szünetben nem számol");
    }

    {   // napló: feljegyzés, mentés és visszatöltés
        std::string saved;
        player::PlayStats st([&](const std::string& d) { saved = d; });
        st.record("t1", 1000);
        st.record("t1", 2000);
        st.record("t2", 1500);
        player::PlayStats back;
        const bool ok = back.load(saved) && back.get("t1").plays == 2 && back.get("t1").last == 2000 &&
                        back.get("t2").plays == 1 && back.get("tx").plays == 0 && !back.load("[1]");
        CHECK(ok && st.rev() == 3, "napló: lejátszásszám és utolsó lejátszás, mentés és visszatöltés");
    }

    // szabályok: ellenőrzés és egységes alak
    auto parse = [](const std::string& j, library::SmartRules& r, std::string& e) {
        json::Value v;
        return json::parse(j, v) && library::parseSmartRules(v, r, e);
    };
    library::SmartRules r;
    std::string e;
    const bool bad = !parse("{\"conditions\":[{\"field\":\"color\",\"op\":\"is\",\"value\":\"x\"}]}", r, e) &&
                     !parse("{\"conditions\":[{\"field\":\"year\",\"op\":\"contains\",\"value\":1}]}", r, e) &&
                     !parse("{\"conditions\":[{\"field\":\"added\",\"op\":\"never\"}]}", r, e) &&
                     !parse("{\"conditions\":[{\"field\":\"artist\",\"op\":\"is\"}]}", r, e) &&
                     !parse("{\"limit\":20000}", r, e) && !parse("{\"sort\":\"color\"}", r, e) &&
                     !parse("{\"match\":\"some\"}", r, e) && !parse("[]", r, e);
    library::SmartRules r2;
    const bool canon = parse("{\"match\":\"any\",\"conditions\":[{\"field\":\"genre\",\"op\":\"is\",\"value\":\"Jazz\"},"
                             "{\"field\":\"lastPlayed\",\"op\":\"never\"}],\"sort\":\"title\",\"limit\":5}", r, e) &&
                       parse(library::smartRulesJson(r), r2, e) && library::smartRulesJson(r) == library::smartRulesJson(r2) &&
                       r.any && !r.desc && r.limit == 5;
    bool defaultsOk = true;
    for (const auto& [name, rules] : library::defaultSmartLists(true)) defaultsOk = defaultsOk && parse(rules, r2, e) && !name.empty();
    CHECK(bad && canon && defaultsOk && library::defaultSmartLists(true)[0].first == "Legutóbb hozzáadott" &&
          library::defaultSmartLists(false)[0].first == "Recently added",
          "szabályok: hibás mező, művelet, érték, korlát, rendezés elutasítva; egységes alak oda-vissza; a négy alaplista "
          "érvényes (magyar és angol névvel)");

    // kiértékelés egy ismert zenetáron
    library::Index idx;
    const int64_t now = 2000000000;
    auto add = [&](const char* id, const char* title, const char* artist, const char* genre, int year, double dur, int64_t addedDaysAgo) {
        library::Track t;
        t.id = id; t.title = title; t.artist = artist; t.album = "A"; t.genre = genre; t.year = year; t.duration = dur;
        t.added = now - addedDaysAgo * 86400;
        idx.trackById[id] = idx.tracks.size();
        idx.tracks.push_back(t);
    };
    add("t1", "Egy", "\xC5\x90rült Banda", "Rock", 1965, 180, 2);
    add("t2", "Kettő", "Jazz Trió", "Jazz", 2005, 300, 10);
    add("t3", "Három", "Rock Kör", "rock", 2010, 150, 40);
    add("t4", "Négy", "Más", "Pop", 1999, 210, 400);
    library::Album al;
    al.title = "A";
    for (size_t i = 0; i < idx.tracks.size(); ++i) al.tracks.push_back(i);
    idx.albums.push_back(al);
    std::unordered_map<std::string, player::TrackStats> st = { { "t1", { 5, now - 3600 } }, { "t2", { 1, now - 20 * 86400 } },
                                                               { "t3", { 3, now - 2 * 86400 } } };
    auto ids = [&](const std::string& rules) {
        library::SmartRules rr;
        std::string err;
        std::string out;
        if (!parse(rules, rr, err)) return std::string("HIBA:") + err;
        for (const auto& it : library::evaluateSmart(rr, idx, st, now)) out += (out.empty() ? "" : ",") + it.id;
        return out;
    };
    const std::string genre = ids("{\"conditions\":[{\"field\":\"genre\",\"op\":\"is\",\"value\":\"ROCK\"}],\"sort\":\"title\"}");
    const std::string fold = ids("{\"conditions\":[{\"field\":\"artist\",\"op\":\"contains\",\"value\":\"\xC5\x91r\xC3\xBClt\"}]}");
    const std::string all = ids("{\"conditions\":[{\"field\":\"year\",\"op\":\"ge\",\"value\":2000},{\"field\":\"duration\",\"op\":\"lt\",\"value\":200}]}");
    const std::string any = ids("{\"match\":\"any\",\"conditions\":[{\"field\":\"genre\",\"op\":\"is\",\"value\":\"jazz\"},{\"field\":\"year\",\"op\":\"lt\",\"value\":1970}],\"sort\":\"year\",\"desc\":false}");
    const std::string recent = ids("{\"conditions\":[{\"field\":\"added\",\"op\":\"inLast\",\"value\":30}]}");
    const std::string old = ids("{\"conditions\":[{\"field\":\"added\",\"op\":\"notInLast\",\"value\":30}],\"sort\":\"added\",\"desc\":false}");
    const std::string most = ids("{\"conditions\":[{\"field\":\"plays\",\"op\":\"gt\",\"value\":0}],\"sort\":\"plays\",\"limit\":2}");
    const std::string never = ids("{\"conditions\":[{\"field\":\"lastPlayed\",\"op\":\"never\"}]}");
    const std::string week = ids("{\"conditions\":[{\"field\":\"lastPlayed\",\"op\":\"inLast\",\"value\":7}],\"sort\":\"lastPlayed\"}");
    const std::string notWeek = ids("{\"conditions\":[{\"field\":\"lastPlayed\",\"op\":\"notInLast\",\"value\":7}],\"sort\":\"title\"}");
    const std::string everything = ids("{\"sort\":\"title\",\"limit\":0}");
    CHECK(genre == "t1,t3" && fold == "t1" && all == "t3" && any == "t1,t2" && recent == "t1,t2" && old == "t4,t3" &&
          most == "t1,t3" && never == "t4" && week == "t1,t3" && notWeek == "t2,t4" && everything == "t1,t3,t2,t4",
          "kiértékelés: műfaj (kis- és nagybetű), ékezetes előadó, „mind” és „bármelyik”, felkerülés, lejátszásszám, "
          "soha / nemrég / régen játszott, rendezés és korlát (%s | %s | %s | %s | %s)",
          genre.c_str(), any.c_str(), old.c_str(), most.c_str(), notWeek.c_str());

    {   // a lejátszó és a sor mérője: valós idejű lejátszásnál a napló feljegyzi a számot (a fele, 2 s után)
        QueueFiles f;
        f.add("te", 48000, 2, 4 * 48000, 0.2f);
        player::PlayStats stats;
        auto* out = new FakeOutput;
        FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
        PlayQueue q(p, f.resolve());
        q.setPlayedCallback([&](const std::string& id) { stats.record(id); });
        q.play(f.items({ "te" }), 0);
        for (int i = 0; i < 12 && stats.get("te").plays == 0; ++i) {   // 0,25 s-onként 0,25 s hang, legfeljebb 3 s
            pullFrames(p, *out, 12000);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        const uint32_t plays = stats.get("te").plays;
        std::this_thread::sleep_for(std::chrono::milliseconds(600));   // ugyanaz a lejátszás nem számít újra
        CHECK(plays == 1 && stats.get("te").plays == 1, "a sor mérője: valós idejű lejátszásnál egy feljegyzés a napló számára");
    }
}

static void testLibraryCache() {
    std::printf("[12] Zenetár gyorsítótár és változásfigyelés (5.2)\n");
    const fs::path root = fs::temp_directory_path() / "aq_library_cache_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / u8"Mappa1");
    fs::create_directories(root / u8"Mappa2");
    fs::copy_file(kData + "/player_stereo44.flac", root / u8"Mappa1" / "song1.flac");
    fs::copy_file(kData + "/player_stereo44.ogg", root / u8"Mappa1" / "song2.ogg");
    fs::copy_file(kData + "/player_mono48.flac", root / u8"Mappa2" / "song3.flac");

    const auto u8s = [](const fs::path& p) { const auto s = p.u8string(); return std::string(s.begin(), s.end()); };
    std::string cacheStore;
    auto loadFn = [&cacheStore] { return cacheStore; };
    auto saveFn = [&cacheStore](const std::string& doc) { cacheStore = doc; };

    // 1. Első bejárás gyorsítótár nélkül: lefut, és elmenti a gyorsítótárat
    {
        library::MusicLibrary lib;
        lib.setCacheStorage(loadFn, saveFn);
        lib.setRoots({ u8s(root) });
        CHECK(lib.waitIdle(10000), "első bejárás lefut");
        CHECK(!lib.cached(), "bejárás után nem a gyorsítótárból van");
        CHECK(!cacheStore.empty(), "gyorsítótár elmentve a memóriába (library-cache.json)");
        auto snap = lib.snapshot();
        CHECK(snap->tracks.size() == 3, "3 szám beindexelve");
    }

    // 2. Második indítás: azonnal a gyorsítótárból van az index!
    std::string id1, id2, id3;
    {
        library::MusicLibrary lib;
        lib.setCacheStorage(loadFn, saveFn);
        lib.setRoots({ u8s(root) });
        // Azonnal a setRoots után ellenőrizzük: azonnal betöltődött a gyorsítótárból!
        auto snap = lib.snapshot();
        CHECK(snap->tracks.size() == 3, "azonnali indulás: a gyorsítótárból 3 szám azonnal elérhető");
        CHECK(lib.cached(), "indikáció: cached = true az induláskor");
        CHECK(lib.infoJson(false).find("\"cached\":true") != std::string::npos, "infoJson-ban cached: true");
        id1 = snap->tracks[0].id;
        id2 = snap->tracks[1].id;
        id3 = snap->tracks[2].id;

        // Megvárjuk a háttérszálat (gyors stat-ellenőrzés)
        CHECK(lib.waitIdle(10000), "háttérellenőrzés lefut");
        CHECK(!lib.cached(), "háttérellenőrzés után cached = false");
        auto snap2 = lib.snapshot();
        CHECK(snap2->tracks.size() == 3, "a friss indexben is 3 szám van");
        CHECK(snap2->tracks[0].id == id1 && snap2->tracks[1].id == id2 && snap2->tracks[2].id == id3,
              "az azonosítók (t...) a gyorsítótárból és a lemezről bájtra megegyeznek");
    }

    // 3. Fájl módosítása: újranyitás és gyorsítótár frissül
    {
        fs::copy_file(kData + "/player_mono48.flac", root / u8"Mappa1" / "song1.flac", fs::copy_options::overwrite_existing);

        library::MusicLibrary lib;
        lib.setCacheStorage(loadFn, saveFn);
        lib.setRoots({ u8s(root) });
        CHECK(lib.waitIdle(10000), "módosítás utáni bejárás lefut");
        auto snap = lib.snapshot();
        CHECK(snap->tracks.size() == 3, "módosítás után is 3 szám");
        bool foundMono = false;
        for (const auto& t : snap->tracks) {
            if (t.file == "song1.flac" && t.title == "Monó FLAC 24 bit") foundMono = true;
        }
        CHECK(foundMono, "módosult fájl felismerve és újracímkézve");
    }

    // 4. Fájl törlése: kiesik az indexből és a mentett gyorsítótárból
    {
        fs::remove(root / u8"Mappa1" / "song1.flac", ec);

        library::MusicLibrary lib;
        lib.setCacheStorage(loadFn, saveFn);
        lib.setRoots({ u8s(root) });
        CHECK(lib.waitIdle(10000), "törlés utáni bejárás lefut");
        auto snap = lib.snapshot();
        CHECK(snap->tracks.size() == 2, "törölt fájl kiesett az indexből (maradt 2)");
    }

    // 5. Sérült gyorsítótár: eldobja, nem omlik össze, friss bejárást végez
    {
        cacheStore = "{ nem érvényes json ...";
        library::MusicLibrary lib;
        lib.setCacheStorage(loadFn, saveFn);
        lib.setRoots({ u8s(root) });
        CHECK(!lib.cached(), "sérült gyorsítótár esetén nem cached állapot");
        CHECK(lib.waitIdle(10000), "sérült gyorsítótár esetén tiszta bejárás lefut");
        auto snap = lib.snapshot();
        CHECK(snap->tracks.size() == 2, "tiszta bejárás megtalálta a 2 meglévő fájlt");
        CHECK(cacheStore.rfind("{\"version\":1", 0) == 0, "új, érvényes gyorsítótár kiírva");
    }

    // 6. Eltérő verziószámú gyorsítótár: eldobja
    {
        cacheStore = "{\"version\":999,\"roots\":[\"" + u8s(root) + "\"],\"tracks\":[]}";
        library::MusicLibrary lib;
        lib.setCacheStorage(loadFn, saveFn);
        lib.setRoots({ u8s(root) });
        CHECK(!lib.cached(), "eltérő verziójú gyorsítótár eldobva");
        CHECK(lib.waitIdle(10000), "bejárás lefut");
        auto snap = lib.snapshot();
        CHECK(snap->tracks.size() == 2, "index felépült");
    }

    // 7. Kézi rescan kérés
    {
        library::MusicLibrary lib;
        lib.setCacheStorage(loadFn, saveFn);
        lib.setRoots({ u8s(root) });
        CHECK(lib.waitIdle(10000), "indítás befejezve");
        fs::copy_file(kData + "/player_stereo44.flac", root / u8"Mappa1" / "song_uj.flac");
        lib.rescan();
        CHECK(lib.waitIdle(10000), "kézi rescan befejezve");
        auto snap = lib.snapshot();
        CHECK(snap->tracks.size() == 3, "kézi rescan megtalálta az új fájlt (összesen 3)");
    }

    fs::remove_all(root, ec);
}

static void testPhoneStream() {
    std::printf("[13] Telefonos zeneátvitel (4b)\n");
    std::error_code ec;
    const fs::path cacheDir = player::fromUtf8Path(tmpPath("aq_phone_cache"));
    fs::remove_all(cacheDir, ec);

    player::PhoneCache cache(player::toUtf8Path(cacheDir));

    // 1. Azonosító és kiterjesztés ellenőrzése
    CHECK(player::PhoneCache::sanitizeId("p_1234abcd") && player::PhoneCache::sanitizeId("track-99_OK") &&
          !player::PhoneCache::sanitizeId("") && !player::PhoneCache::sanitizeId("../evil") &&
          !player::PhoneCache::sanitizeId("id with spaces") && !player::PhoneCache::sanitizeId(std::string(70, 'a')),
          "PhoneCache: érvényes és érvénytelen azonosítók szűrése");
    CHECK(player::PhoneCache::sanitizeExt("flac") && player::PhoneCache::sanitizeExt("mp3") &&
          player::PhoneCache::sanitizeExt("wav") && player::PhoneCache::sanitizeExt("wv") && !player::PhoneCache::sanitizeExt("exe") &&
          !player::PhoneCache::sanitizeExt("sh"), "PhoneCache: engedélyezett kiterjesztések (flac, wav, mp3, ogg, m4a, aac, opus, wv)");

    // 2. AEPF keret csomagolása és olvasása
    {
        const std::string dummy = "AQULIFY_AUDIO_RAW_DATA_TEST_1234567890";
        const std::string frame = player::buildAepfFrame(0x01, "p_test_01", "flac", 100, 200, dummy.data(), dummy.size());
        player::AepfHeader hdr;
        CHECK(player::parseAepfFrame(frame, hdr), "AEPF keret sikeresen kibontva");
        CHECK(hdr.version == 1 && hdr.flags == 0x01 && hdr.eof() && hdr.id == "p_test_01" && hdr.ext == "flac" &&
              hdr.offset == 100 && hdr.totalSize == 200 && hdr.dataLen == dummy.size() &&
              std::memcmp(hdr.data, dummy.data(), dummy.size()) == 0,
              "AEPF mezők pontos egyezése (verzió, flagek, offset, totalSize, azonosító, adat)");

        // Hibás keret
        player::AepfHeader badHdr;
        std::string badFrame = frame;
        badFrame[0] = 'X';
        CHECK(!player::parseAepfFrame(badFrame, badHdr), "hibás magic elutasítva");
        CHECK(!player::parseAepfFrame("rövid", badHdr), "túl rövid keret elutasítva");
    }

    // 3. Darabolt írás a gyorsítótárba
    {
        // 1. darab: 10 bájt
        const std::string part1 = "1234567890";
        auto res1 = cache.writeChunk("p_track1", "flac", 0, 25, part1.data(), part1.size(), false);
        CHECK(res1.ok && !res1.completed, "1. darab sikeresen kiírva, még nincs kész");
        CHECK(!cache.hasTrack("p_track1", "flac", 25), "még nem létezik a végleges fájl");

        // 2. darab: 15 bájt, EOF
        const std::string part2 = "ABCDEFGHIJKLMNO";
        auto res2 = cache.writeChunk("p_track1", "flac", 10, 25, part2.data(), part2.size(), true);
        CHECK(res2.ok && res2.completed && !res2.finalPath.empty(), "2. darab kiírva, EOF -> végleges fájl elkészült");
        CHECK(cache.hasTrack("p_track1", "flac", 25), "hasTrack megtalálja az elkészült fájlt");

        std::string resolved;
        CHECK(cache.resolve("p_track1", resolved) && resolved == res2.finalPath, "resolve megtalálja a számot");

        // Tartalom ellenőrzése
        std::ifstream in(res2.finalPath, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(content == "1234567890ABCDEFGHIJKLMNO", "a végleges fájl tartalma pontosan megegyezik a darabokkal");
    }

    // 4. Okos párosítás és híd integráció
    {
        library::MusicLibrary lib;
        const std::string root = tmpPath("aq_phone_lib_root");
        fs::create_directories(player::fromUtf8Path(root), ec);
        fs::copy_file(kData + "/player_stereo44.flac", player::fromUtf8Path(root) / "local_song.flac", fs::copy_options::overwrite_existing, ec);
        lib.setRoots({ root });
        CHECK(lib.waitIdle(10000), "helyi könyvtár bejárva");

        auto snap = lib.snapshot();
        CHECK(!snap->tracks.empty(), "helyi szám beindexelve");
        const auto& localTr = snap->tracks[0];

        // Okos párosítás teszt
        std::string matchedId;
        CHECK(lib.matchLocalTrack(localTr.title, localTr.artist, localTr.duration, matchedId) && matchedId == localTr.id,
              "matchLocalTrack megtalálta a helyi számot cím, előadó és időtartam alapján");
        std::string noMatchId;
        CHECK(!lib.matchLocalTrack("Ismeretlen szám 999", "Bárki", 100.0, noMatchId),
              "matchLocalTrack helyesen nem talál ismeretlen számot");

        // Híd bekötése
        auto* out = new FakeOutput;
        player::FilePlayer fp{ std::unique_ptr<PlayerOutput>(out) };
        player::PlayQueue pq(fp, [&](const std::string& id, std::string& p) {
            if (id.rfind("p_", 0) == 0) return cache.resolve(id, p);
            return lib.trackPath(id, p);
        });
        player::PlaylistStore pls([](const std::string&) {});
        player::PlayStats pstats;
        BridgeCallbacks cb;
        library::attachLibrary(cb, lib, pq, pls, pstats, [](const std::string&) {}, &cache);

        // a) phone_track_offer egyező helyi számmal -> phone_track_have
        json::Value offerMatch;
        json::parse("{\"type\":\"phone_track_offer\",\"id\":\"p_local\",\"title\":\"" + localTr.title +
                   "\",\"artist\":\"" + localTr.artist + "\",\"duration\":" + std::to_string(localTr.duration) +
                   ",\"size\":50000,\"ext\":\"flac\"}", offerMatch);
        const std::string replyMatch = cb.phoneTrackRequest("phone_track_offer", offerMatch, true);
        CHECK(replyMatch.find("\"type\":\"phone_track_have\"") != std::string::npos &&
              replyMatch.find("\"local_id\":\"" + localTr.id + "\"") != std::string::npos,
              "híd: helyben meglévő számra phone_track_have válasz érkezik a helyi azonosítóval");

        // c) Bináris átvitel: a valódi FLAC tesztfájl átküldése AEPF keretben
        std::ifstream realFlac(kData + "/player_stereo44.flac", std::ios::binary);
        std::string flacBytes((std::istreambuf_iterator<char>(realFlac)), std::istreambuf_iterator<char>());
        CHECK(!flacBytes.empty(), "teszt FLAC beolvasva");

        // b) phone_track_offer új számmal -> phone_track_send
        json::Value offerNew;
        json::parse("{\"type\":\"phone_track_offer\",\"id\":\"p_stream_flac\",\"title\":\"Új telefonos szám\","
                    "\"artist\":\"Előadó\",\"duration\":1.5,\"size\":" + std::to_string(flacBytes.size()) + ",\"ext\":\"flac\"}", offerNew);
        const std::string replySend = cb.phoneTrackRequest("phone_track_offer", offerNew, true);
        CHECK(replySend.find("\"type\":\"phone_track_send\"") != std::string::npos &&
              replySend.find("\"offset\":0") != std::string::npos,
              "híd: hiányzó számra phone_track_send válasz jön darabkéréssel");

        const std::string aepfData = player::buildAepfFrame(0x01, "p_stream_flac", "flac", 0, flacBytes.size(),
                                                            flacBytes.data(), flacBytes.size());
        const std::string binReply = cb.phoneTrackBinary(aepfData);
        CHECK(binReply.find("\"type\":\"phone_track_ready\"") != std::string::npos &&
              binReply.find("\"status\":\"ready\"") != std::string::npos,
              "híd: AEPF keret fogadása után phone_track_ready érkezik");

        // d) Újbóli offer már cached állapotot jelez
        const std::string replyCached = cb.phoneTrackRequest("phone_track_offer", offerNew, true);
        CHECK(replyCached.find("\"type\":\"phone_track_ready\"") != std::string::npos &&
              replyCached.find("\"status\":\"cached\"") != std::string::npos,
              "híd: meglévő számra azonnali cached válasz");

        // e) Lejátszás indítása a telefonos számmal (library_play)
        json::Value playPhone;
        json::parse("{\"type\":\"library_play\",\"id\":\"p_stream_flac\",\"title\":\"Új telefonos szám\"}", playPhone);
        const std::string playRes = cb.libraryRequest("library_play", playPhone, true);
        CHECK(playRes.find("\"ok\":true") != std::string::npos && fp.hasTrack(),
              "híd: library_play p_ azonosítóval elindítja a lejátszást a gyorsítótárazott fájlból");
        fp.stop();

        // f) Borító kinyerése címkézett MP3-ból
        const std::string taggedPath = tmpPath("aq_player_tag.mp3");
        std::ifstream tagIn(taggedPath, std::ios::binary);
        if (tagIn.is_open()) {
            std::string tagBytes((std::istreambuf_iterator<char>(tagIn)), std::istreambuf_iterator<char>());
            const std::string aepfTag = player::buildAepfFrame(0x01, "p_stream_mp3", "mp3", 0, tagBytes.size(),
                                                               tagBytes.data(), tagBytes.size());
            cb.phoneTrackBinary(aepfTag);
            std::string mime, artBytes;
            CHECK(cb.libraryArt("p_stream_mp3", mime, artBytes) && !mime.empty() && !artBytes.empty(),
                  "híd: beágyazott borító sikeresen kinyerve a telefonról streamelt számból");
        } else {
            CHECK(true, "címkézett mp3 nem elérhető, átugorva");
        }

        // g) 4c: Másolás a PC zenei könyvtárába (phone_track_import)
        json::Value importReq;
        json::parse("{\"type\":\"phone_track_import\",\"id\":\"p_stream_flac\",\"ext\":\"flac\","
                    "\"title\":\"Mentett Szám\",\"artist\":\"Telefon Előadó\",\"album\":\"Telefon Album\"}", importReq);
        const std::string importRes = cb.phoneTrackRequest("phone_track_import", importReq, true);
        CHECK(importRes.find("\"type\":\"phone_track_imported\"") != std::string::npos &&
              importRes.find("\"ok\":true") != std::string::npos &&
              importRes.find("Aqulify a telefon") != std::string::npos,
              "híd: phone_track_import átmásolja a számot a PC zenei könyvtárába (Aqulify a telefonról)");

        CHECK(lib.waitIdle(10000), "importálás utáni bejárás lefut");
        auto snapAfter = lib.snapshot();
        bool foundImported = false;
        for (const auto& tr : snapAfter->tracks) {
            if (tr.path.find("Aqulify a telefon") != std::string::npos) foundImported = true;
        }
        CHECK(foundImported, "az importált szám bekerült a PC állandó zenetárába és indexébe");

        // h) 4d: Eszközök közötti listák (cross-device playlists)
        // 1. Alias és waitForTrack teszt PhoneCache-ben
        cache.setAlias("p_alias_test", kData + "/player_stereo44.flac");
        std::string aliasResolved;
        CHECK(cache.resolve("p_alias_test", aliasResolved) && aliasResolved == kData + "/player_stereo44.flac",
              "PhoneCache: setAlias és resolve feloldja az aliast a helyi fájlra");

        std::string waitResolved;
        CHECK(cache.waitForTrack("p_alias_test", std::chrono::milliseconds(50), waitResolved),
              "PhoneCache: waitForTrack azonnal sikeres meglévő aliasnál");
        CHECK(!cache.waitForTrack("p_nonexistent", std::chrono::milliseconds(50), waitResolved),
              "PhoneCache: waitForTrack időtúllépéssel elutasítja a nem létező számot");

        // 2. Telefonos szám hozzáadása lejátszási listához (playlist_create source: phone_track)
        json::Value plCreateReq;
        json::parse("{\"type\":\"playlist_create\",\"name\":\"Telefon Listám\","
                    "\"source\":\"phone_track\",\"id\":\"p_match_track\",\"title\":\"" + localTr.title +
                    "\",\"artist\":\"" + localTr.artist + "\",\"duration\":" + std::to_string(localTr.duration) + "}", plCreateReq);
        const std::string plCreateRes = cb.queueRequest("playlist_create", plCreateReq, true);
        CHECK(plCreateRes.find("\"type\":\"playlist\"") != std::string::npos &&
              plCreateRes.find("Telefon List") != std::string::npos,
              "híd: playlist_create sikeresen létrehozza a listát telefonos számmal");

        json::Value plCreatedVal;
        json::parse(plCreateRes, plCreatedVal);
        const std::string plId = plCreatedVal.get("id") ? plCreatedVal.get("id")->stringOr("") : "";
        CHECK(!plId.empty(), "létrejött lista azonosító érvényes");

        // 3. Több telefonos szám hozzáadása (source: phone_tracks)
        json::Value plAddReq;
        json::parse("{\"type\":\"playlist_add\",\"id\":\"" + plId + "\",\"source\":\"phone_tracks\",\"items\":["
                    "{\"id\":\"p_cached_track\",\"title\":\"Mentett Szám\",\"duration\":1.5},"
                    "{\"id\":\"p_missing_track\",\"title\":\"Hiányzó Szám\",\"duration\":3.0}"
                    "]}", plAddReq);
        const std::string plAddRes = cb.queueRequest("playlist_add", plAddReq, true);
        CHECK(plAddRes.find("\"type\":\"playlist\"") != std::string::npos,
              "híd: playlist_add (phone_tracks) több számot sikeresen hozzáad a listához");

        // 4. Lista lekérése (playlist_get): elérhetőség és forrásjelölések ellenőrzése
        json::Value plGetReq;
        json::parse("{\"type\":\"playlist_get\",\"id\":\"" + plId + "\"}", plGetReq);
        const std::string plGetRes = cb.queueRequest("playlist_get", plGetReq, true);
        CHECK(plGetRes.find("\"source\":\"phone\"") != std::string::npos, "a tételeknél megjelenik a source: phone");
        CHECK(plGetRes.find("\"id\":\"p_match_track\"") != std::string::npos &&
              plGetRes.find("\"matched\":true") != std::string::npos,
              "okos egyeztetés a listában: a PC-n meglévő telefonos szám matched: true és available: true");
        CHECK(plGetRes.find("\"id\":\"p_missing_track\"") != std::string::npos &&
              plGetRes.find("\"available\":false") != std::string::npos,
              "hiányzó telefonos számnál kapcsolat nélkül available: false");

        // 5. Lista lejátszása (queue_play): az egyeztetett szám lefut a sorban a helyi fájlból
        json::Value qPlayReq;
        json::parse("{\"type\":\"queue_play\",\"source\":\"playlist\",\"id\":\"" + plId + "\",\"start\":\"p_match_track\"}", qPlayReq);
        const std::string qPlayRes = cb.queueRequest("queue_play", qPlayReq, true);
        CHECK(qPlayRes.find("\"type\":\"queue\"") != std::string::npos && fp.hasTrack(),
              "híd: queue_play elindítja a lejátszást a listából az okosan egyeztetett helyi másolatból");
        fp.stop();

        fs::remove_all(player::fromUtf8Path(root), ec);
    }

    fs::remove_all(cacheDir, ec);
}

static void testErrors() {
    std::printf("[6] Hibás és nem támogatott fájl\n");
    const std::string bad = tmpPath("aq_player_bad.flac");
    { std::ofstream f(bad); f << "nem hangfájl"; }
    std::string err1, err2, err3;
    const bool badOpen = openDecoder(bad, &err1) == nullptr;
    const bool aacOpen = openDecoder("valami.aac", &err2) == nullptr;
    auto* out = new FakeOutput;
    FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
    const bool playerFails = !p.open(bad, &err3);
    CHECK(badOpen && !err1.empty() && aacOpen && !err2.empty() && playerFails && !p.hasTrack() && !out->opened,
          "hibás FLAC: \"%s\"; .aac: \"%s\"; a lejátszó nem nyit hangfolyamot", err1.c_str(), err2.c_str());
    CHECK(isSupportedFile("a/B.FLAC") && isSupportedFile("x.Mp3") && isSupportedFile("y.ogg") &&
          isSupportedFile("m.M4A") && isSupportedFile("a.alac") && isSupportedFile("c.CAF") &&
          isSupportedFile("s.opus") && isSupportedFile("T.OPUS") &&
          isSupportedFile("song.wv") && isSupportedFile("TRACK.WV") &&
          !isSupportedFile("z.aac") &&
          !isSupportedFile("mappa.flac/fajl"), "kiterjesztés-felismerés (kis- és nagybetű, mappanév nem számít)");
    std::error_code ec;
    fs::remove(bad, ec);
}

/** TXXX keret (Latin-1): leírás + érték. */
static std::vector<uint8_t> txxx(const std::string& desc, const std::string& value) {
    std::vector<uint8_t> b = { 0 };
    b.insert(b.end(), desc.begin(), desc.end()); b.push_back(0);
    b.insert(b.end(), value.begin(), value.end());
    return b;
}

static void testReplayGain() {
    std::printf("[RG] ReplayGain: címkék (Vorbis, Opus R128, ID3 TXXX), erősítés a lejátszóban, előbetöltés\n");
    TrackTags t;
    const std::string c1 = "REPLAYGAIN_TRACK_GAIN=-6.50 dB", c2 = "replaygain_album_peak=0.988", c3 = "R128_ALBUM_GAIN=-2560";
    detail::applyVorbisComment(c1.data(), c1.size(), t);
    detail::applyVorbisComment(c2.data(), c2.size(), t);
    detail::applyVorbisComment(c3.data(), c3.size(), t);
    CHECK(std::abs(t.rgTrackGain + 6.5f) < 1e-4f && std::abs(t.rgAlbumPeak - 0.988f) < 1e-4f && std::abs(t.rgAlbumGain + 5.0f) < 1e-4f,
          "Vorbis: szám %.2f dB, album csúcs %.3f, R128 album -2560 -> %.2f dB (a -18 LUFS-os referenciához)",
          t.rgTrackGain, t.rgAlbumPeak, t.rgAlbumGain);
    const std::vector<uint8_t> tag = id3Tag23({ id3Frame23("TXXX", txxx("REPLAYGAIN_TRACK_GAIN", "-6.02 dB")),
                                                id3Frame23("TXXX", txxx("REPLAYGAIN_TRACK_PEAK", "0.400000")) });
    TrackTags t2;
    detail::parseId3v2(tag.data(), tag.size(), t2);
    CHECK(std::abs(t2.rgTrackGain + 6.02f) < 1e-4f && std::abs(t2.rgTrackPeak - 0.4f) < 1e-4f, "ID3 TXXX: %.2f dB, csúcs %.3f",
          t2.rgTrackGain, t2.rgTrackPeak);

    // MP3 a fenti ID3 címkével: -6 dB szám szerint, a csúcs-korlát (1 / 0,4) nem szól bele
    std::vector<uint8_t> mp3 = readFile(kData + "/player_stereo44.mp3");
    std::vector<uint8_t> tagged = tag;
    tagged.insert(tagged.end(), mp3.begin(), mp3.end());
    const std::string path = tmpPath("aq_rg_test.mp3");
    { std::ofstream f(path, std::ios::binary); f.write(reinterpret_cast<const char*>(tagged.data()), static_cast<std::streamsize>(tagged.size())); }
    auto rms = [](const std::vector<float>& x) { double e = 0; for (float v : x) e += double(v) * v; return std::sqrt(e / std::max<size_t>(1, x.size())); };
    double level[3] = {};
    for (int mode = 0; mode < 3; ++mode) {
        auto* out = new FakeOutput;
        FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
        p.setReplayGain(mode == 0 ? 0 : 1, mode == 2 ? 3.0f : 0.0f, true);
        std::string err;
        if (!p.open(path, &err)) { CHECK(false, "a címkézett MP3 nem nyitható: %s", err.c_str()); return; }
        level[mode] = rms(pullFrames(p, *out, 22050));
    }
    const double dB = 20 * std::log10(level[1] / level[0]), dBpre = 20 * std::log10(level[2] / level[0]);
    CHECK(std::abs(dB + 6.02) < 0.05, "szám szerinti ReplayGain: %.2f dB (várt -6,02)", dB);
    CHECK(std::abs(dBpre + 3.02) < 0.05, "+3 dB előerősítéssel: %.2f dB (várt -3,02)", dBpre);

    // címke nélküli fájl ReplayGainnel: érintetlen; előbetöltéssel is bitpontos, és a másolat a végén törlődik
    const uint32_t sr = 48000;
    std::vector<float> src(sr * 2);
    for (size_t i = 0; i < sr; ++i) { src[2 * i] = static_cast<float>(0.3 * std::sin(0.01 * i)); src[2 * i + 1] = -src[2 * i]; }
    const std::string wav = tmpPath("aq_rg_plain.wav");
    writeWavFloat(wav, sr, 2, src);
    {
        auto* out = new FakeOutput;
        FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
        p.setReplayGain(2, 6.0f, true);
        p.setPreload(true);
        std::string err;
        CHECK(p.open(wav, &err), "előbetöltéssel megnyitva");
        size_t copies = 0;
        std::error_code ec;
        const char* x = std::getenv("XDG_RUNTIME_DIR");
        const fs::path dir = (x && *x && fs::is_directory(x, ec)) ? fs::path(x) / "aqulify-preload" : fs::temp_directory_path() / "aqulify-preload";
        for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); ++it) ++copies;
        const std::vector<float> a = pullFrames(p, *out, sr / 2);
        CHECK(a.size() == sr && std::equal(a.begin(), a.end(), src.begin()), "címke nélkül, előbetöltve: bitpontos (%zu másolat a memóriában)", copies);
        CHECK(copies >= 1, "a fájl másolata a memóriában (%s)", dir.string().c_str());
        p.stop();
        size_t left = 0;
        for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); ++it) ++left;
        CHECK(left == 0, "leállítás után a másolat törlődött (%zu maradt)", left);
    }
    std::error_code ec;
    fs::remove(path, ec);
    fs::remove(wav, ec);
}

/** Sony DSF v1.01 fájl összeállítása MSB-first csatornánkénti bájtokból + opcionális ID3v2 címkével. */
static void writeDsfTestFile(const std::string& path, uint32_t dsdRate, uint32_t channels,
                             const std::vector<std::vector<uint8_t>>& chBytesMsb,
                             const std::vector<uint8_t>& id3Tag) {
    constexpr uint32_t kBlockSize = 4096;
    const size_t rawPerCh = chBytesMsb[0].size();
    const uint64_t sampleCount = static_cast<uint64_t>(rawPerCh) * 8ULL;
    const size_t numBlocks = (rawPerCh + kBlockSize - 1) / kBlockSize;
    const uint64_t dataPayloadBytes = static_cast<uint64_t>(numBlocks) * kBlockSize * channels;
    const uint64_t dataChunkSize = 12ULL + dataPayloadBytes;
    const uint64_t metaOffset = id3Tag.empty() ? 0ULL : (28ULL + 52ULL + dataChunkSize);
    const uint64_t totalFileSize = 28ULL + 52ULL + dataChunkSize + id3Tag.size();

    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(totalFileSize));
    auto w4 = [&](const char* id4) { out.insert(out.end(), id4, id4 + 4); };
    auto le32 = [&](uint32_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    };
    auto le64 = [&](uint64_t v) {
        le32(static_cast<uint32_t>(v & 0xFFFFFFFFULL));
        le32(static_cast<uint32_t>(v >> 32));
    };
    w4("DSD "); le64(28); le64(totalFileSize); le64(metaOffset);
    w4("fmt "); le64(52); le32(1); le32(0); le32(channels == 1 ? 1u : 2u); le32(channels);
    le32(dsdRate); le32(1); le64(sampleCount); le32(kBlockSize); le32(0);
    w4("data"); le64(dataChunkSize);
    for (size_t blk = 0; blk < numBlocks; ++blk) {
        for (uint32_t c = 0; c < channels; ++c) {
            for (uint32_t i = 0; i < kBlockSize; ++i) {
                const size_t idx = blk * kBlockSize + i;
                const uint8_t b = (idx < rawPerCh) ? dsd::reverseBits8(chBytesMsb[c][idx]) : 0x00u;
                out.push_back(b);
            }
        }
    }
    out.insert(out.end(), id3Tag.begin(), id3Tag.end());
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

static void testDsdDop() {
    std::printf("[DoP] Natív DSD-lejátszás (DoP 1.1): 24 bites szavak, marker, bájtsorrend, csend a váltáskor, ReplayGain kimarad, PCM visszalépés\n");
    constexpr uint32_t kDsd64Rate = 2822400;
    constexpr uint32_t kDopRate = 176400;
    constexpr size_t kDopFrames = 4096;
    constexpr size_t kBytesPerCh = kDopFrames * 2;

    std::vector<uint8_t> leftMsb(kBytesPerCh), rightMsb(kBytesPerCh);
    for (size_t i = 0; i < kBytesPerCh; ++i) {
        leftMsb[i] = static_cast<uint8_t>((i * 37u + 0x12u) & 0xFFu);
        rightMsb[i] = static_cast<uint8_t>((i * 73u + 0xA5u) & 0xFFu);
    }
    const std::vector<uint8_t> rgTag = id3Tag23({
        id3Frame23("TXXX", txxx("REPLAYGAIN_TRACK_GAIN", "-9.50 dB")),
        id3Frame23("TXXX", txxx("REPLAYGAIN_TRACK_PEAK", "0.950000"))
    });
    const std::string dsfPath = tmpPath("aq_dop_test.dsf");
    writeDsfTestFile(dsfPath, kDsd64Rate, 2, { leftMsb, rightMsb }, rgTag);

    const std::string wavPath = tmpPath("aq_dop_pcm48.wav");
    std::vector<float> wavSrc(4800 * 2, 0.25f);
    writeWavFloat(wavPath, 48000, 2, wavSrc);

    // 1. DoP engedélyezve, kizárólagos kimenet (settle = true, allowDop = true), ReplayGain bekapcsolva (-9,5 dB + -6 dB előerősítés):
    //    a kimenetre pontosan a várt 24 bites DoP-szavak jutnak, a ReplayGain nem módosít rajtuk, a váltási csend 0xAA (nem 0).
    {
        auto* out = new FakeOutput;
        out->settle = true;
        out->allowDop = true;
        FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
        p.setDsdDop(true);
        p.setReplayGain(1, -6.0f, true);

        std::string err;
        const bool opened = p.open(dsfPath, &err);
        CHECK(opened && p.isDopActive() && out->dopMode && out->rate == kDopRate &&
              p.dopDsdRate() == kDsd64Rate && p.currentGainDb() == 0.0f && p.dopFallbackReason().empty(),
              "DoP megnyitás: %u Hz DoP (DSD %u Hz), ReplayGain erősítés %.1f dB (kimarad)",
              out->rate, p.dopDsdRate(), p.currentGainDb());

        const size_t lead = static_cast<size_t>(p.leadInFrames());
        const size_t expectedLead = ((kDopRate * FilePlayer::kRateSettleMs / 1000) + 1u) & ~uint64_t{1};
        CHECK(lead == expectedLead && (lead % 2) == 0,
              "DoP indítási csend: %zu keret (%u ms, páros keretszám a markerfázishoz)", lead, FilePlayer::kRateSettleMs);

        const std::vector<int32_t> all = pullDopFrames(p, *out, lead + kDopFrames);
        CHECK(all.size() == (lead + kDopFrames) * 2, "DoP keretek hiánytalanul kiolvasva (%zu minta)", all.size());

        bool silenceOk = all.size() >= lead * 2;
        bool nonZeroSilence = true;
        for (size_t f = 0; silenceOk && f < lead; ++f) {
            const uint8_t wantMarker = (f % 2 == 0) ? 0x05u : 0xFAu;
            const int32_t wantWord = dsd::packDopSample(dsd::kDsdSilenceByte, dsd::kDsdSilenceByte, wantMarker);
            const int32_t wl = all[f * 2], wr = all[f * 2 + 1];
            if (wl == 0 || wr == 0) nonZeroSilence = false;
            if (wl != wantWord || wr != wantWord) silenceOk = false;
        }
        CHECK(silenceOk && nonZeroSilence,
              "DoP indítási csend: minden keret csomagolt 0x%02X (0x05AAAA / 0xFAAAAA), soha nem nulla PCM",
              dsd::kDsdSilenceByte);

        bool payloadOk = all.size() == (lead + kDopFrames) * 2;
        for (size_t f = 0; payloadOk && f < kDopFrames; ++f) {
            const size_t idx = (lead + f) * 2;
            const int32_t wl = all[idx], wr = all[idx + 1];
            const uint8_t wantMarker = ((lead + f) % 2 == 0) ? 0x05u : 0xFAu;
            uint8_t l0 = 0, l1 = 0, r0 = 0, r1 = 0, ml = 0, mr = 0;
            dsd::unpackDopSample(wl, ml, l0, l1);
            dsd::unpackDopSample(wr, mr, r0, r1);
            if (ml != wantMarker || mr != wantMarker ||
                l0 != leftMsb[f * 2] || l1 != leftMsb[f * 2 + 1] ||
                r0 != rightMsb[f * 2] || r1 != rightMsb[f * 2 + 1]) {
                payloadOk = false;
            }
        }
        CHECK(payloadOk, "DoP szavak: váltakozó 0x05/0xFA marker mindkét csatornán, pontos MSB-first bájtsorrend, ReplayGain nem változtat rajtuk");

        // Tekerés DoP módban az 1024. DoP-keretre (2048. DSD-bájtra)
        p.seek(1024.0 / static_cast<double>(kDopRate));
        const std::vector<int32_t> afterSeek = pullDopFrames(p, *out, 256);
        bool seekOk = afterSeek.size() == 256 * 2;
        uint8_t prevMarker = 0;
        for (size_t f = 0; seekOk && f < 256; ++f) {
            uint8_t l0 = 0, l1 = 0, r0 = 0, r1 = 0, ml = 0, mr = 0;
            dsd::unpackDopSample(afterSeek[f * 2], ml, l0, l1);
            dsd::unpackDopSample(afterSeek[f * 2 + 1], mr, r0, r1);
            if ((ml != 0x05u && ml != 0xFAu) || ml != mr || (f > 0 && ml == prevMarker)) seekOk = false;
            prevMarker = ml;
            const size_t bIdx = (1024 + f) * 2;
            if (l0 != leftMsb[bIdx] || l1 != leftMsb[bIdx + 1] ||
                r0 != rightMsb[bIdx] || r1 != rightMsb[bIdx + 1]) {
                seekOk = false;
            }
        }
        CHECK(seekOk, "DoP tekerés az 1024. keretre: a 2048. DSD-bájttól folytatódik bitpontosan, folyamatos markerrel");

        // Páratlan számú csendkeret a hangfolyamban (mint egy alulcsordulásnál): utána is folytonos a jelölő-váltakozás,
        // különben a DAC egy pillanatra kiesne a DoP-módból (hallható reccsenés)
        p.pause();
        std::vector<int32_t> gap(3 * 2);
        {
            std::lock_guard<std::mutex> l(out->m);
            out->renderDop(gap.data(), 3);
        }
        p.play();
        const std::vector<int32_t> afterGap = pullDopFrames(p, *out, 64);
        bool gapOk = afterGap.size() == 64 * 2;
        uint8_t prev = static_cast<uint8_t>((static_cast<uint32_t>(gap[4]) >> 16) & 0xFFu);
        for (size_t f = 0; gapOk && f < 64; ++f) {
            uint8_t l0 = 0, l1 = 0, r0 = 0, r1 = 0, ml = 0, mr = 0;
            dsd::unpackDopSample(afterGap[f * 2], ml, l0, l1);
            dsd::unpackDopSample(afterGap[f * 2 + 1], mr, r0, r1);
            if ((ml != 0x05u && ml != 0xFAu) || ml != mr || ml == prev) gapOk = false;
            prev = ml;
            const size_t bIdx = (1024 + 256 + f) * 2;
            if (l0 != leftMsb[bIdx] || l1 != leftMsb[bIdx + 1] || r0 != rightMsb[bIdx] || r1 != rightMsb[bIdx + 1])
                gapOk = false;
        }
        CHECK(gapOk, "DoP: 3 csendkeret (alulcsordulás) után is folytonos a 0x05/0xFA váltakozás, az adat bitpontos");

        // DoP -> PCM váltás: új PCM hangfolyam nyílik csenddel
        CHECK(p.open(wavPath, &err) && !p.isDopActive() && !out->dopMode && out->rate == 48000,
              "DoP -> PCM váltás: 48 kHz-es PCM kimenetre vált");
        const size_t pcmLead = static_cast<size_t>(p.leadInFrames());
        const std::vector<float> pcmGot = pullFrames(p, *out, pcmLead + 256);
        const bool pcmZeros = pcmGot.size() == (pcmLead + 256) * 2 &&
                              std::all_of(pcmGot.begin(), pcmGot.begin() + pcmLead * 2, [](float v) { return v == 0.0f; });
        const bool pcmExact = pcmZeros && std::all_of(pcmGot.begin() + pcmLead * 2, pcmGot.end(), [](float v) { return v == 0.25f; });
        CHECK(pcmLead == 48000u * FilePlayer::kRateSettleMs / 1000 && pcmExact,
              "DoP -> PCM váltás: %zu keret PCM-csend, utána a PCM minta", pcmLead);

        // PCM -> DoP váltás: újra DoP-csenddel (0xAA) indul
        CHECK(p.open(dsfPath, &err) && p.isDopActive() && out->dopMode && out->rate == kDopRate && p.leadInFrames() == expectedLead,
              "PCM -> DoP váltás: újra %u Hz DoP kimenet %zu keret DoP-csenddel", out->rate, expectedLead);
        p.stop();
    }

    // 2. Visszalépés PCM-re, ha a kimenet nem fogadja el a DoP-ot (allowDop = false):
    {
        auto* out = new FakeOutput;
        out->settle = true;
        out->allowDop = false;
        out->dopFailReason = "format";
        FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
        p.setDsdDop(true);

        std::string err;
        const bool opened = p.open(dsfPath, &err);
        CHECK(opened && !p.isDopActive() && !out->dopMode && out->rate == 88200 && p.dopFallbackReason() == "format",
              "DoP elutasítva (\"%s\"): automatikus visszalépés %u Hz PCM-re", p.dopFallbackReason().c_str(), out->rate);
        const size_t pcmLead = static_cast<size_t>(p.leadInFrames());
        const std::vector<float> pcmFallback = pullFrames(p, *out, pcmLead + 512);
        CHECK(pcmFallback.size() == (pcmLead + 512) * 2,
              "PCM visszalépés: %zu csendkeret + 512 dekódolt PCM-keret", pcmLead);
        p.stop();
    }

#ifdef _WIN32
    // 3. WASAPI 24-in-32 (WAVEFORMATEXTENSIBLE wBitsPerSample=32, wValidBitsPerSample=24) és 24/24 bitpontos átadás:
    {
        const int32_t dopWords[2] = { 0x0005A1B2, 0x00FAA3B4 };
        uint8_t buf32[8] = {};
        const WasapiFormat fmt24in32{ 32, 24, false, 0, "24/32 bit DoP" };
        WasapiPlayerOutput::convertDop(dopWords, buf32, 2, fmt24in32);
        uint32_t w0 = 0, w1 = 0;
        std::memcpy(&w0, buf32, 4);
        std::memcpy(&w1, buf32 + 4, 4);
        CHECK(w0 == 0x05A1B200u && w1 == 0xFAA3B400u && buf32[0] == 0x00 && buf32[3] == 0x05 && buf32[7] == 0xFA,
              "WASAPI 24/32 bit DoP: MSB-igazított (<< 8) 32 bites keret (0x%08X, 0x%08X)", w0, w1);

        uint8_t buf24[6] = {};
        const WasapiFormat fmt24{ 24, 24, false, 0, "24 bit DoP" };
        WasapiPlayerOutput::convertDop(dopWords, buf24, 2, fmt24);
        CHECK(buf24[0] == 0xB2 && buf24[1] == 0xA1 && buf24[2] == 0x05 &&
              buf24[3] == 0xB4 && buf24[4] == 0xA3 && buf24[5] == 0xFA,
              "WASAPI 24 bit DoP: 3 bájtos kis-endián csomagolás");
    }
#endif

    std::error_code ec;
    fs::remove(dsfPath, ec);
    fs::remove(wavPath, ec);
}

// =========================================================================
// Felmintavételezés (libsoxr) tesztek: K1, K6, K7, K8, K16
// =========================================================================

template <class P = FilePlayer>
concept HasUpsamplePlayer = requires(P& p) {
    p.setUpsample(true);
};

static uint64_t fnv1a64(const float* data, size_t count) {
    uint64_t h = 14695981039346656037ull;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
    const size_t bytes = count * sizeof(float);
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static void writeWavPcm16(const std::string& path, uint32_t rate, uint16_t ch, const std::vector<int16_t>& x) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t data = static_cast<uint32_t>(x.size() * 2);
    f.write("RIFF", 4); u32(36 + data); f.write("WAVEfmt ", 8); u32(16);
    u16(1); u16(ch); u32(rate); u32(rate * ch * 2);
    u16(static_cast<uint16_t>(ch * 2)); u16(16);
    f.write("data", 4); u32(data);
    f.write(reinterpret_cast<const char*>(x.data()), data);
}

static void writeWavPcm24(const std::string& path, uint32_t rate, uint16_t ch, const std::vector<int32_t>& x) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t data = static_cast<uint32_t>(x.size() * 3);
    f.write("RIFF", 4); u32(36 + data); f.write("WAVEfmt ", 8); u32(16);
    u16(1); u16(ch); u32(rate); u32(rate * ch * 3);
    u16(static_cast<uint16_t>(ch * 3)); u16(24);
    f.write("data", 4); u32(data);
    for (int32_t s : x) {
        uint32_t u = static_cast<uint32_t>(s);
        uint8_t b[3] = { static_cast<uint8_t>(u & 0xFF),
                         static_cast<uint8_t>((u >> 8) & 0xFF),
                         static_cast<uint8_t>((u >> 16) & 0xFF) };
        f.write(reinterpret_cast<const char*>(b), 3);
    }
}

static void testUpsamplePlayerK1K6K7K8K16() {
    std::printf("\n========================================================\n");
    std::printf(" Felmintavételezés lejátszó tesztek: K1, K6, K7, K8, K16\n");
    std::printf("========================================================\n");

    // K1: Kikapcsolt felmintavételezésnél a lejátszó kimenete bitre azonos a funkció előtti referenciával
    std::printf("[K1] Bitre azonos lejátszási kimenet kikapcsolt felmintavételezésnél (WAV 16/24 bit, FLAC)\n");
    const std::map<uint32_t, std::pair<uint64_t, uint64_t>> refHashes = {
        { 44100,  { 0xC8C58243735BA5B5ull, 0x59F5BC6EC5538975ull } },
        { 48000,  { 0xD28851B0CBBC0A45ull, 0x655344004C5B4FE5ull } },
        { 88200,  { 0xE2C952DDBC789FE5ull, 0x16191E34344414A5ull } },
        { 96000,  { 0x8EF2BE5784FE5DC5ull, 0x8B335B2A33F86025ull } },
        { 176400, { 0xF5B0D5D6618E68A5ull, 0xB1EC017DD9479BE5ull } },
        { 192000, { 0x7E8E6348C3F39705ull, 0x7C9D9CF3B7FA6DE5ull } }
    };

    for (const auto& [r, hashes] : refHashes) {
        const size_t frames = r / 10; // 100 ms
        // 16 bit
        std::vector<int16_t> pcm16(frames * 2);
        for (size_t i = 0; i < frames; ++i) {
            float l = std::sin(2.0 * kPi * 440.0 * i / r) * 0.7071f;
            float right = std::sin(2.0 * kPi * 1000.0 * i / r) * 0.7071f;
            pcm16[2 * i] = static_cast<int16_t>(std::clamp(std::round(l * 32767.0f), -32768.0f, 32767.0f));
            pcm16[2 * i + 1] = static_cast<int16_t>(std::clamp(std::round(right * 32767.0f), -32768.0f, 32767.0f));
        }
        const std::string path16 = tmpPath(("aq_k1_" + std::to_string(r) + "_16.wav").c_str());
        writeWavPcm16(path16, r, 2, pcm16);

        auto* out16 = new FakeOutput;
        FilePlayer p16{ std::unique_ptr<PlayerOutput>(out16) };
        std::string err;
        p16.open(path16, &err);
        const std::vector<float> pulled16 = pullFrames(p16, *out16, frames);
        p16.stop();
        const uint64_t h16 = fnv1a64(pulled16.data(), pulled16.size());
        std::error_code ec;
        fs::remove(path16, ec);
        CHECK(h16 == hashes.first, "K1 WAV 16 bit %u Hz: bitpontos referencia egyezik (0x%016llX)", r, (unsigned long long)h16);

        // 24 bit
        std::vector<int32_t> pcm24(frames * 2);
        for (size_t i = 0; i < frames; ++i) {
            float l = std::sin(2.0 * kPi * 440.0 * i / r) * 0.7071f;
            float right = std::sin(2.0 * kPi * 1000.0 * i / r) * 0.7071f;
            pcm24[2 * i] = static_cast<int32_t>(std::clamp(std::round(l * 8388607.0f), -8388608.0f, 8388607.0f));
            pcm24[2 * i + 1] = static_cast<int32_t>(std::clamp(std::round(right * 8388607.0f), -8388608.0f, 8388607.0f));
        }
        const std::string path24 = tmpPath(("aq_k1_" + std::to_string(r) + "_24.wav").c_str());
        writeWavPcm24(path24, r, 2, pcm24);

        auto* out24 = new FakeOutput;
        FilePlayer p24{ std::unique_ptr<PlayerOutput>(out24) };
        p24.open(path24, &err);
        const std::vector<float> pulled24 = pullFrames(p24, *out24, frames);
        p24.stop();
        const uint64_t h24 = fnv1a64(pulled24.data(), pulled24.size());
        fs::remove(path24, ec);
        CHECK(h24 == hashes.second, "K1 WAV 24 bit %u Hz: bitpontos referencia egyezik (0x%016llX)", r, (unsigned long long)h24);
    }

    // FLAC 44.1 kHz 16 bit
    {
        auto* out = new FakeOutput;
        FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
        std::string err;
        p.open(kData + "/player_stereo44.flac", &err);
        const std::vector<float> pulled = pullFrames(p, *out, 44100);
        p.stop();
        const uint64_t h = fnv1a64(pulled.data(), pulled.size());
        CHECK(h == 0x6661B47F2AA13415ull, "K1 FLAC 16 bit 44,1 kHz: bitpontos referencia egyezik (0x%016llX)", (unsigned long long)h);
    }

    // FLAC 48 kHz 24 bit
    {
        auto* out = new FakeOutput;
        FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
        std::string err;
        p.open(kData + "/player_mono48.flac", &err);
        const std::vector<float> pulled = pullFrames(p, *out, 24000);
        p.stop();
        const uint64_t h = fnv1a64(pulled.data(), pulled.size());
        CHECK(h == 0x9EB8E6E762841965ull, "K1 FLAC 24 bit 48 kHz: bitpontos referencia egyezik (0x%016llX)", (unsigned long long)h);
    }

    if constexpr (HasUpsamplePlayer<FilePlayer>) {
        std::printf("[K6/K7/K8/K16] Upsample player tesztek futtatása...\n");

        // K6: Késleltetés és pozíció (±1 ms pontosság)
        {
            const uint32_t sr = 44100;
            const size_t n = 2 * sr;
            std::vector<float> src(n * 2);
            for (size_t i = 0; i < n; ++i) {
                src[2 * i] = static_cast<float>(0.5 * std::sin(2 * kPi * 440 * i / sr));
                src[2 * i + 1] = static_cast<float>(0.25 * std::sin(2 * kPi * 1000 * i / sr));
            }
            const std::string path = tmpPath("aq_k6_pos.wav");
            writeWavFloat(path, sr, 2, src);

            auto* out = new FakeOutput;
            FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
            audioopt::AudioOptions opts;
            opts.upsampleRule = audioopt::UpsampleRule::Double;
            p.setUpsampleOptions(opts);
            p.setUpsample(true);
            std::string err;
            CHECK(p.open(path, &err) && out->rate == 88200 && out->active, "K6: megnyitás 44.1k -> 88.2k sikeres");

            // 0.5 s lejátszása (44100 kimeneti keret 88.2 kHz-en)
            std::vector<float> pulled = pullFrames(p, *out, sr);
            CHECK(pulled.size() == sr * 2, "K6: 0.5 s kimenet lehúzva (%zu minta)", pulled.size());
            CHECK(std::abs(p.position() - 0.5) <= 0.001, "K6: pozíció 0.5 s-nél %.4f s (hiba <= 1 ms)", p.position());

            // Tekerés 1.2 s-ra
            p.seek(1.2);
            CHECK(std::abs(p.position() - 1.2) <= 0.001, "K6: tekerés 1.2 s-ra: pozíció %.4f s (hiba <= 1 ms)", p.position());
            std::vector<float> afterSeek = pullFrames(p, *out, 8820);
            CHECK(std::abs(p.position() - 1.3) <= 0.001, "K6: tekerés utáni lejátszás: pozíció %.4f s (hiba <= 1 ms)", p.position());
            p.stop();
        }

        // K7: Szünetmentes folytonosság (azonos forrásmintavételű két szám vágásánál d2 <= 2x referencia)
        {
            QueueFiles qf;
            const uint32_t sr = 44100;
            const size_t nA = 22050; // 0.5s
            const size_t nB = 22050; // 0.5s
            std::vector<float> xA(nA * 2), xB(nB * 2), full((nA + nB) * 2);
            for (size_t i = 0; i < nA; ++i) {
                float v = static_cast<float>(0.5 * std::sin(2.0 * kPi * 1000.0 * i / sr));
                xA[2 * i] = xA[2 * i + 1] = v;
                full[2 * i] = full[2 * i + 1] = v;
            }
            for (size_t i = 0; i < nB; ++i) {
                float v = static_cast<float>(0.5 * std::sin(2.0 * kPi * 1000.0 * (nA + i) / sr));
                xB[2 * i] = xB[2 * i + 1] = v;
                full[2 * (nA + i)] = full[2 * (nA + i) + 1] = v;
            }
            const std::string pathA = tmpPath("aq_k7_a.wav");
            const std::string pathB = tmpPath("aq_k7_b.wav");
            writeWavFloat(pathA, sr, 2, xA);
            writeWavFloat(pathB, sr, 2, xB);
            qf.path["k7a"] = pathA;
            qf.path["k7b"] = pathB;

            // Referencia folyamatos szinusz felmintavételezve
            UpsampleEngine refEngine;
            audioopt::AudioOptions defaultOpts;
            defaultOpts.upsampleRule = audioopt::UpsampleRule::Double;
            refEngine.initWithOptions(sr, sr * 2, 2, defaultOpts);
            std::vector<float> refOut((nA + nB) * 4 + 2048);
            size_t refOutFrames = 0;
            refEngine.process(full.data(), nA + nB, refOut.data(), refOut.size() / 2, refOutFrames);
            size_t flFrames = 0;
            refEngine.flush(refOut.data() + refOutFrames * 2, (refOut.size() - refOutFrames * 2) / 2, flFrames);
            refOutFrames += flFrames;

            float maxD2Ref = 0.0f;
            const size_t refStart = std::min<size_t>(4000, refOutFrames);
            const size_t refEnd = refOutFrames > 4000 ? refOutFrames - 4000 : refOutFrames;
            for (size_t f = refStart; f < refEnd; ++f) {
                float d2 = std::abs(refOut[2 * f] - 2.0f * refOut[2 * (f - 1)] + refOut[2 * (f - 2)]);
                if (d2 > maxD2Ref) maxD2Ref = d2;
            }

            auto* out = new FakeOutput;
            FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
            p.setUpsampleOptions(defaultOpts);
            p.setUpsample(true);
            PlayQueue q(p, qf.resolve());
            const bool started = q.play(qf.items({ "k7a", "k7b" }), 0);
            CHECK(started && out->rate == 88200, "K7: lejátszás elindult 88200 Hz-en");

            // K6 (szűrő-késleltetés a számhatáron): 0.6 s lehúzása (0.5 s A + 0.1 s B) -> B pozíciója 0.100 s ±1 ms
            const size_t part1Frames = nA * 2 + 8820; // 0.5 s + 0.1 s = 0.6 s
            std::vector<float> gotOut = pullFrames(p, *out, part1Frames);
            const double posInB = p.position();
            CHECK(std::abs(posInB - 0.1) <= 0.001,
                  "K6: szünetmentes váltás utáni pozíció a B számban %.4f s (várt 0.1000 s, hiba <= 1 ms)", posInB);
            const size_t totalOutFrames = (nA + nB) * 2;
            std::vector<float> restOut = pullFrames(p, *out, totalOutFrames - part1Frames);
            gotOut.insert(gotOut.end(), restOut.begin(), restOut.end());
            p.stop();

            CHECK(out->opens == 1, "K7: szünetmentes váltás, 1etlen hangfolyam nyílt a két számhoz");

            // A teljes középső szakaszon vizsgáljuk a második differenciát (a szűrő késleltetése előtti és
            // utáni határon is, hogy a szűrőállapot nullázása miatti kattanás biztosan lebukjon - K7 / U11)
            float maxD2Cut = 0.0f;
            const size_t totalFrames = gotOut.size() / 2;
            const size_t checkStart = std::min<size_t>(4000, totalFrames);
            const size_t checkEnd = totalFrames > 4000 ? totalFrames - 4000 : totalFrames;
            for (size_t f = checkStart; f < checkEnd; ++f) {
                float d2 = std::abs(gotOut[2 * f] - 2.0f * gotOut[2 * (f - 1)] + gotOut[2 * (f - 2)]);
                if (d2 > maxD2Cut) maxD2Cut = d2;
            }
            CHECK(maxD2Cut <= 2.0f * maxD2Ref,
                  "K7 szünetmentes folytonosság: vágásnál max d2 = %.6f <= 2x referencia (%.6f)",
                  maxD2Cut, 2.0f * maxD2Ref);
        }

        // K8: Valós idejű foglalásmentesség a render szálon
        {
            auto* out = new FakeOutput;
            FilePlayer p{ std::unique_ptr<PlayerOutput>(out) };
            audioopt::AudioOptions defaultOpts;
            defaultOpts.upsampleRule = audioopt::UpsampleRule::Double;
            p.setUpsampleOptions(defaultOpts);
            p.setUpsample(true);
            std::string err;
            p.open(tmpPath("aq_k7_a.wav"), &err);
            pullFrames(p, *out, 1024);

            g_rtAllocCount.store(0, std::memory_order_seq_cst);
            g_rtTrackAlloc = true;
            for (int i = 0; i < 20; ++i) {
                float buf[512 * 2];
                out->pull(buf, 512);
            }
            g_rtTrackAlloc = false;
            p.stop();

            CHECK(g_rtAllocCount.load(std::memory_order_seq_cst) == 0, "K8: valós idejű foglalásmentesség (render hívások alatt 0 újrafoglalás, mért: %llu)",
                  (unsigned long long)g_rtAllocCount.load());
        }

        // K16: DSD/DoP és > 192 kHz kihagyása
        {
            // 1. > 192 kHz forrás (pl. 352.8 kHz)
            const uint32_t highRate = 352800;
            std::vector<float> highPcm(highRate / 10 * 2, 0.1f);
            const std::string highPath = tmpPath("aq_k16_352k.wav");
            writeWavFloat(highPath, highRate, 2, highPcm);

            auto* out1 = new FakeOutput;
            FilePlayer p1{ std::unique_ptr<PlayerOutput>(out1) };
            audioopt::AudioOptions opts;
            opts.upsampleRule = audioopt::UpsampleRule::Double;
            p1.setUpsampleOptions(opts);
            p1.setUpsample(true);
            std::string err;
            CHECK(p1.open(highPath, &err), "K16: 352.8 kHz fájl megnyitva");
            CHECK(out1->rate == highRate, "K16: kimeneti frekvencia maradt 352.8 kHz (nem duplázódott)");
            CHECK(p1.upsampleReason() == "source_rate", "K16: indoklás 'source_rate' (%s)", p1.upsampleReason().c_str());
            p1.stop();

            // 2. DoP / DSD esetén
            auto* out2 = new FakeOutput;
            FilePlayer p2{ std::unique_ptr<PlayerOutput>(out2) };
            p2.setUpsampleOptions(opts);
            p2.setUpsample(true);
            CHECK(p2.decideTargetRate(176400, false, true) == 176400, "K16: DoP / DSD esetén decideTargetRate változatlan");
            CHECK(p2.upsampleReason() == "dsd", "K16: DoP / DSD esetén indoklás 'dsd'");
            // K15 egység-ellenőrzés: bypassUpsample=true megkerüli a felmintavételezőt
            CHECK(p2.decideTargetRate(44100, true, false) == 44100 && p2.upsampleReason() == "bypass",
                  "K15: bypassUpsample esetén nincs átalakítás (ok: bypass)");
        }
    } else {
        CHECK(false, "K6/K7/K8/K16: FilePlayer felmintavételezés nincs megvalósítva");
    }
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // hiba esetén se vesszen el a kimenet (pl. CI, átirányítás)
    std::printf("========================================================\n");
    std::printf(" Aqulify 2.x – beépített lejátszó\n");
    std::printf("========================================================\n");
    testDecoders();
    testSeek();
    testTags();
    testReplayGain();
    testPlayer();
    testDownmix();
    testOutputLatency();
    testLibrary();
    testLibraryBrowse();
    testLibraryCache();
    testQueue();
    testQueueEdits();
    testSmart();
    testPhoneStream();
    testErrors();
    testDsdDop();
    testUpsamplePlayerK1K6K7K8K16();
    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
