// SRC „Referencia” előbeállítás és a parancssori átalakító (include/src_convert.hpp) tesztje.
// A bemeneti fájlokat és a mérést a teszt maga írja (saját WAV-író, saját egyfrekvenciás vetítés), a vizsgált kódtól
// függetlenül. Követelmények:
//  [1] R1  az előbeállítás értékei (a felület gombjaival azonosak), a sanitize megtartja, a JSON visszaolvasható
//  [2] R2  sávszélesség: 96 → 44,1 kHz és 44,1 → 96 kHz, 21,9 kHz-es hang szintje ±0,02 dB (a Köztes ezt levágja)
//  [3] R3  visszahajlás / tükörkép: ≤ −140 dB (lefelé 22,5 és 24 kHz-es hang, felfelé a 21,5 kHz-es hang tükörképe)
//  [4] R4  időzítés: lineáris fázis, nulla késleltetés (az impulzus a helyén, szimmetrikus), a hossz a várt ±1
//  [5] R5  hézagmentesség: két külön átalakított fél egymás után = az egész, a számhatártól távol
//  [6] R6  a szám végén a szűrő teljesen kiürül, a hossz pontos (Referencia, 44,1 → 96, 384 és 705,6 kHz)
//  [7] R7  mono: bal = jobb bitre; sztereó: a csendes csatorna csendes marad
//  [8] R8  64 bites lebegőpontos bemenet = 32 bites (a lejátszó 32 bites lebegőpontosan dolgozik)
//  [9] R9  a kiírt WAV fejléce és mintái
// [10] R10 fejtér: 0 dB pontosan egységnyi, −1 dB pontosan −1 dB
// [11] R11 sebesség: Referencia, 44,1 → 384 kHz sztereó, legalább 2× valós idő (x86_64), 0,5× (egyéb)
// [12] R12 a lejátszó kimenete = az átalakító kimenete, a szám vége nem vész el
// [13] R13 64 bites WAV olvasó (16/24/32 int, 32/64 float, WAVE_FORMAT_EXTENSIBLE, extra chunkok, csonka fájl)
// [14] R14 64 bites WAV író (fejléc és bitpontos minták)
// [15] R15 64 bites fájlátalakító (convertFile64): bitre egyezik a libsoxr közvetlen SOXR_FLOAT64_I hívásával
// [16] R16 64 bites fájlátalakító: −150 dB-es jel megmarad, visszahajlás és intermoduláció zajszintje ≤ −190 dB
// [17] R17 szélkezelés (V2): kettévágott két-tónusos jel illesztése LPC-vel ≤ −120 dB az egyben átalakítottól
// [18] R18 szélkezelés csendes szélen / impulzuson: bitre azonos a nyers úttal; sztereó csendes csatorna csendes marad
// [19] R19 saját 64 bites FFT (vegyes-radix 2/3/4/5/7 + Bluestein + RealFft): IFFT(FFT(x)) == x ≤ 1e-14
// [20] R20 egylépcsős 64 bites FFT+Kaiser-sinc motor (Fft64): 22 kHz átvitel, ≤ −250 dB visszahajlás/IMD, 0 késleltetés, LPC hézagmentesség
// [21] R21 beépített lejátszó FFT64 motor + választható fázis (0/25/50) + AudioOptions JSON + FilePlayer végigjátszás
// [22] R22 106 bites double-double FFT motor (FftDD): 40 kHz záró sáv ≤ −310 dB, IMD ≤ −300 dB, 22,02 kHz sávszélesség, 0 késleltetés
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <mutex>
#include <thread>

#include "file_player.hpp"
#include "src_convert.hpp"
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace ee_analyzer;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c, msg)                                                         \
    do {                                                                      \
        if (c) std::printf("  [OK]   %s\n", msg);                             \
        else { std::printf("  [HIBA] %s\n", msg); ++g_fail; }                 \
    } while (0)

static const double kPi = 3.14159265358979323846;

static fs::path tmpDir() {
    fs::path d = fs::temp_directory_path() / ("aq_src_ref_" + std::to_string(static_cast<long long>(
#ifdef _WIN32
        _getpid()
#else
        getpid()
#endif
        )));
    fs::create_directories(d);
    return d;
}

// Saját WAV-író (16 bájtos fmt, IEEE float 32 vagy 64 bit)
template <typename T>
static void writeWav(const fs::path& p, const std::vector<double>& x, uint32_t ch, uint32_t rate) {
    FILE* f = std::fopen(p.string().c_str(), "wb");
    const uint32_t bps = sizeof(T), data = static_cast<uint32_t>(x.size() * bps);
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); w32(36 + data); std::fwrite("WAVEfmt ", 1, 8, f); w32(16);
    w16(3); w16(static_cast<uint16_t>(ch)); w32(rate); w32(rate * ch * bps); w16(static_cast<uint16_t>(ch * bps)); w16(bps * 8);
    std::fwrite("data", 1, 4, f); w32(data);
    for (double v : x) { T s = static_cast<T>(v); std::fwrite(&s, bps, 1, f); }
    std::fclose(f);
}

static std::vector<double> tone(double f, double amp, uint32_t rate, size_t n) {
    std::vector<double> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = amp * std::sin(2.0 * kPi * f * static_cast<double>(i) / rate);
    return x;
}

// Egyfrekvenciás amplitúdó (legkisebb négyzetes vetítés szinuszra és koszinuszra) a [start, start+n) szakaszon
static double ampAt(const std::vector<float>& y, size_t ch, size_t c, size_t start, size_t n, double f, double rate) {
    if ((start + n) * ch > y.size()) {   // rövidebb a kimenet a vártnál: hibás mérés (a hívó ellenőrzése elbukik)
        std::printf("         (a kimenet rövidebb a vártnál: %zu < %zu)\n", y.size(), (start + n) * ch);
        return 1e30;
    }
    double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
    for (size_t i = 0; i < n; ++i) {
        const double t = 2.0 * kPi * f * static_cast<double>(start + i) / rate;
        const double s = std::sin(t), co = std::cos(t), v = y[(start + i) * ch + c];
        ss += s * s; cc += co * co; sc += s * co; ys += v * s; yc += v * co;
    }
    const double det = ss * cc - sc * sc;
    const double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
    return std::sqrt(a * a + b * b);
}

// A lejátszó kimenete: a teszt húzza a mintákat (mint a hangkártya)
class PullOut : public player::PlayerOutput {
public:
    bool open(uint32_t sr, RenderFn r, std::string*) override { std::lock_guard<std::mutex> l(m); rate = sr; render = std::move(r); opened = true; return true; }
    void close() override { std::lock_guard<std::mutex> l(m); render = nullptr; opened = false; }
    void setActive(bool a) override { std::lock_guard<std::mutex> l(m); active = a; }
    void lock() override { m.lock(); }
    void unlock() override { m.unlock(); }
    std::vector<uint32_t> supportedRates() const override { return {44100, 48000, 88200, 96000, 176400, 192000}; }
    bool pull(float* s, size_t n) { std::lock_guard<std::mutex> l(m); if (!opened || !active || !render) return false; render(s, n); return true; }
    std::mutex m;
    uint32_t rate = 0;
    bool opened = false, active = false;
    RenderFn render;
};

// Egy hang kivonása a jelből (a legkisebb négyzetes illesztés szerint), hogy a mellette lévő gyenge összetevő mérhető
// legyen: a téglalap-ablakos vetítésbe az erős szomszéd szivárgása (1/(π·Δf·T)) egyébként −80 dB körül belemérne.
static std::vector<float> removeTone(const std::vector<float>& y, size_t start, size_t n, double f, double rate) {
    if (start + n > y.size()) return y;   // rövid kimenet: az ampAt jelzi a hibát
    double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
    for (size_t i = 0; i < n; ++i) {
        const double t = 2.0 * kPi * f * static_cast<double>(start + i) / rate;
        const double s = std::sin(t), co = std::cos(t), v = y[start + i];
        ss += s * s; cc += co * co; sc += s * co; ys += v * s; yc += v * co;
    }
    const double det = ss * cc - sc * sc, a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
    std::vector<float> r(y.size(), 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const double t = 2.0 * kPi * f * static_cast<double>(start + i) / rate;
        r[start + i] = static_cast<float>(y[start + i] - (a * std::sin(t) + b * std::cos(t)));
    }
    return r;
}

static double db(double v) { return 20.0 * std::log10(std::max(v, 1e-30)); }

static bool conv(const fs::path& in, uint32_t rate, int preset, float headroom, std::vector<float>& out, srcconv::Result& r) {
    audioopt::AudioOptions o;
    audioopt::applyUpsamplePreset(o, preset);
    o.upsampleHeadroomDb = headroom;
    std::string err;
    const bool ok = srcconv::convert(in.string(), rate, o, out, r, &err);
    if (!ok) std::printf("         (%s)\n", err.c_str());
    return ok;
}

// Saját PCM WAV-író (16/24/32 bit signed integer) a független teszthez
static void writeWavPcm(const fs::path& p, const std::vector<int32_t>& rawInts, uint16_t bits, uint32_t ch, uint32_t rate,
                        bool extensible = false, bool withExtraChunks = false) {
    FILE* f = std::fopen(p.string().c_str(), "wb");
    const uint32_t bps = bits / 8u;
    const uint32_t data = static_cast<uint32_t>(rawInts.size() * bps);
    const uint32_t fmtSz = extensible ? 40u : 16u;
    const uint32_t junkSz = withExtraChunks ? 5u : 0u;  // szándékosan páratlan méret a padding teszteléséhez
    const uint32_t junkBlock = withExtraChunks ? (8u + junkSz + 1u) : 0u;
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    w32(4u + (8u + fmtSz) + junkBlock + (8u + data));
    std::fwrite("WAVE", 1, 4, f);
    if (withExtraChunks) {
        std::fwrite("JUNK", 1, 4, f);
        w32(junkSz);
        const uint8_t jb[6] = {1, 2, 3, 4, 5, 0};
        std::fwrite(jb, 1, 6, f);
    }
    std::fwrite("fmt ", 1, 4, f);
    w32(fmtSz);
    w16(extensible ? 0xFFFEu : 1u);
    w16(static_cast<uint16_t>(ch));
    w32(rate);
    w32(rate * ch * bps);
    w16(static_cast<uint16_t>(ch * bps));
    w16(bits);
    if (extensible) {
        w16(22);
        w16(bits);
        w32(ch == 1 ? 4u : 3u);
        const uint8_t guid[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        std::fwrite(guid, 1, 16, f);
    }
    std::fwrite("data", 1, 4, f);
    w32(data);
    for (int32_t v : rawInts) {
        if (bits == 16) {
            int16_t s = static_cast<int16_t>(v);
            std::fwrite(&s, 2, 1, f);
        } else if (bits == 24) {
            uint8_t b[3] = {uint8_t(v & 0xFF), uint8_t((v >> 8) & 0xFF), uint8_t((v >> 16) & 0xFF)};
            std::fwrite(b, 1, 3, f);
        } else {
            std::fwrite(&v, 4, 1, f);
        }
    }
    std::fclose(f);
}

// WAVE_FORMAT_EXTENSIBLE IEEE float (64 bit) WAV író
static void writeWavExtFloat64(const fs::path& p, const std::vector<double>& x, uint32_t ch, uint32_t rate) {
    FILE* f = std::fopen(p.string().c_str(), "wb");
    const uint32_t data = static_cast<uint32_t>(x.size() * 8u);
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); w32(4u + 48u + 8u + data); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); w32(40);
    w16(0xFFFE); w16(static_cast<uint16_t>(ch)); w32(rate); w32(rate * ch * 8u); w16(static_cast<uint16_t>(ch * 8u)); w16(64);
    w16(22); w16(64); w32(ch == 1 ? 4u : 3u);
    const uint8_t guid[16] = {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    std::fwrite(guid, 1, 16, f);
    std::fwrite("data", 1, 4, f); w32(data);
    std::fwrite(x.data(), 8, x.size(), f);
    std::fclose(f);
}

// Double-pontosságú egyfrekvenciás amplitúdómérés
static double ampAtD(const std::vector<double>& y, size_t ch, size_t c, size_t start, size_t n, double f, double rate) {
    if ((start + n) * ch > y.size()) return 1e30;
    double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
    for (size_t i = 0; i < n; ++i) {
        const double t = 2.0 * kPi * f * static_cast<double>(start + i) / rate;
        const double s = std::sin(t), co = std::cos(t), v = y[(start + i) * ch + c];
        ss += s * s; cc += co * co; sc += s * co; ys += v * s; yc += v * co;
    }
    const double det = ss * cc - sc * sc;
    const double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
    return std::sqrt(a * a + b * b);
}

// Double-pontosságú egyfrekvenciás tónuskivonás a [start, start+n) szakaszon
static std::vector<double> removeToneD(const std::vector<double>& y, size_t start, size_t n, double f, double rate) {
    if (start + n > y.size()) return y;
    double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
    for (size_t i = 0; i < n; ++i) {
        const double t = 2.0 * kPi * f * static_cast<double>(start + i) / rate;
        const double s = std::sin(t), co = std::cos(t), v = y[start + i];
        ss += s * s; cc += co * co; sc += s * co; ys += v * s; yc += v * co;
    }
    const double det = ss * cc - sc * sc, a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
    std::vector<double> r = y;
    for (size_t i = 0; i < n; ++i) {
        const double t = 2.0 * kPi * f * static_cast<double>(start + i) / rate;
        r[start + i] = y[start + i] - (a * std::sin(t) + b * std::cos(t));
    }
    return r;
}

// Közvetlen libsoxr referencia-átalakítás SOXR_FLOAT64_I-vel (a teszt saját ellenőrzője)
static std::vector<double> directSoxr64(const std::vector<double>& in, uint32_t ch, uint32_t srcRate, uint32_t dstRate,
                                        const audioopt::AudioOptions& o) {
    soxr_quality_spec_t q = soxr_quality_spec(SOXR_32_BITQ, SOXR_DOUBLE_PRECISION | SOXR_HI_PREC_CLOCK);
    q.precision = static_cast<double>(o.upsamplePrecision);
    q.phase_response = o.upsamplePhase;
    q.passband_end = o.upsamplePassbandEnd;
    q.stopband_begin = o.upsampleStopbandBegin;
    soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT64_I, SOXR_FLOAT64_I);
    soxr_runtime_spec_t rt = soxr_runtime_spec(1);
    soxr_error_t err = nullptr;
    soxr_t s = soxr_create(srcRate, dstRate, ch, &err, &io, &q, &rt);
    std::vector<double> out;
    if (err || !s) return out;
    const size_t totalIn = in.size() / ch;
    std::vector<double> buf(32768 * ch);
    size_t pos = 0;
    while (pos < totalIn) {
        size_t idone = 0, odone = 0;
        soxr_process(s, in.data() + pos * ch, std::min<size_t>(4096, totalIn - pos), &idone, buf.data(), 32768, &odone);
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(odone * ch));
        pos += idone;
    }
    for (;;) {
        size_t idone = 0, odone = 0;
        soxr_process(s, nullptr, 0, &idone, buf.data(), 32768, &odone);
        if (odone == 0) break;
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(odone * ch));
    }
    soxr_delete(s);
    return out;
}

int main() {
    const fs::path dir = tmpDir();
    constexpr int REF = audioopt::kUpsamplePresetReference;

    std::printf("[1] R1 – a Referencia előbeállítás értékei\n");
    {
        audioopt::AudioOptions o;
        audioopt::applyUpsamplePreset(o, REF);
        CHECK(o.upsamplePreset == 4 && std::abs(o.upsamplePassbandEnd - 0.997) < 1e-12 && o.upsampleStopbandBegin == 1.0 &&
                  o.upsamplePrecision == 32 && o.upsamplePhase == 50.0,
              "Referencia: 99,7%, záró sáv 100%, 32 bit, lineáris fázis (50)");
        audioopt::AudioOptions s = o;
        audioopt::sanitize(s);
        CHECK(s == o, "a sanitize nem módosítja (99,7% és a 4-es előbeállítás érvényes)");
        json::Value v;
        CHECK(json::parse(audioopt::toJson(o), v) && audioopt::fromJson(v) == o, "JSON-ba írva és visszaolvasva azonos");
        audioopt::AudioOptions big = o;
        big.upsamplePassbandEnd = 0.999; big.upsamplePreset = 9;
        audioopt::sanitize(big);
        CHECK(std::abs(big.upsamplePassbandEnd - 0.997) < 1e-12 && big.upsamplePreset == 4, "a 99,7% és a 4 a felső határ");
        audioopt::AudioOptions c = o;
        c.upsampleHeadroomDb = -3.0f;
        audioopt::applyUpsamplePreset(c, 3);
        CHECK(c == ([&] { audioopt::AudioOptions t = o; t.upsampleHeadroomDb = -3.0f; return t; })(),
              "az Egyéni (3) nem változtat; a fejtér a felhasználóé");
        audioopt::AudioOptions l, m, mi;
        audioopt::applyUpsamplePreset(l, 0); audioopt::applyUpsamplePreset(m, 1); audioopt::applyUpsamplePreset(mi, 2);
        CHECK(l.upsamplePhase == 50.0 && m.upsamplePhase == 25.0 && mi.upsamplePhase == 0.0 && l.upsamplePrecision == 28 &&
                  std::abs(m.upsamplePassbandEnd - 0.95) < 1e-12,
              "a régi előbeállítások értékei változatlanok (95%, 28 bit, 50 / 25 / 0)");
    }

    std::printf("[2] R2 – sávszélesség\n");
    {
        const fs::path p = dir / "pb96.wav";
        writeWav<float>(p, tone(21900.0, 0.5, 96000, 192000), 1, 96000);
        std::vector<float> y; srcconv::Result r;
        CHECK(conv(p, 44100, REF, 0.0f, y, r), "96 → 44,1 kHz átalakítás");
        const double a = ampAt(y, 1, 0, 20000, 44100, 21900.0, 44100.0);
        std::printf("         21,9 kHz: %+.4f dB\n", db(a / 0.5));
        CHECK(std::abs(db(a / 0.5)) <= 0.02, "lefelé: a 21,9 kHz-es hang szintje ±0,02 dB");
        std::vector<float> yi; srcconv::Result ri;
        conv(p, 44100, 1, 0.0f, yi, ri);
        const double ai = ampAt(yi, 1, 0, 20000, 44100, 21900.0, 44100.0);
        std::printf("         Köztes (95%%): %+.1f dB\n", db(ai / 0.5));
        CHECK(db(ai / 0.5) < -3.0, "ugyanez a Köztes előbeállítással legalább 3 dB-lel halkabb (a beállítás érvényesül)");

        const fs::path q = dir / "pb44.wav";
        writeWav<float>(q, tone(21900.0, 0.5, 44100, 88200), 1, 44100);
        std::vector<float> u; srcconv::Result ru;
        CHECK(conv(q, 96000, REF, 0.0f, u, ru), "44,1 → 96 kHz átalakítás");
        const double au = ampAt(u, 1, 0, 40000, 96000, 21900.0, 96000.0);
        std::printf("         felfelé 21,9 kHz: %+.4f dB\n", db(au / 0.5));
        CHECK(std::abs(db(au / 0.5)) <= 0.02, "felfelé: a 21,9 kHz-es hang szintje ±0,02 dB");
    }

    std::printf("[3] R3 – visszahajlás és tükörkép\n");
    {
        for (double f : {22500.0, 24000.0}) {
            const fs::path p = dir / "al.wav";
            writeWav<float>(p, tone(f, 0.5, 96000, 384000), 1, 96000);
            std::vector<float> y; srcconv::Result r;
            conv(p, 44100, REF, 0.0f, y, r);
            const double al = ampAt(y, 1, 0, 40000, 131072, 44100.0 - f, 44100.0);
            char m[160];
            std::snprintf(m, sizeof m, "lefelé %.1f kHz → %.2f kHz visszahajlás: %.1f dB (≤ −140)", f / 1000, (44100 - f) / 1000, db(al));
            CHECK(db(al) <= -140.0, m);
        }
        const fs::path q = dir / "im.wav";
        writeWav<float>(q, tone(21500.0, 0.5, 44100, 176400), 1, 44100);
        std::vector<float> u; srcconv::Result ru;
        conv(q, 96000, REF, 0.0f, u, ru);
        const std::vector<float> res = removeTone(u, 60000, 262144, 21500.0, 96000.0);
        const double im = ampAt(res, 1, 0, 60000, 262144, 44100.0 - 21500.0, 96000.0);
        char m[160];
        std::snprintf(m, sizeof m, "felfelé 21,5 kHz → 22,6 kHz tükörkép: %.1f dB (≤ −140)", db(im));
        CHECK(db(im) <= -140.0, m);
    }

    std::printf("[4] R4 – időzítés (lineáris fázis, nulla késleltetés)\n");
    {
        std::vector<double> x(96000, 0.0);
        x[48000] = 0.5;
        const fs::path p = dir / "imp.wav";
        writeWav<float>(p, x, 1, 96000);
        std::vector<float> y; srcconv::Result r;
        conv(p, 44100, REF, 0.0f, y, r);
        size_t pk = 0;
        for (size_t i = 0; i < y.size(); ++i) if (std::abs(y[i]) > std::abs(y[pk])) pk = i;
        std::printf("         csúcs: %zu (elvárt 22050), hossz %zu (elvárt 44100)\n", pk, y.size());
        CHECK(pk == 22050, "az impulzus csúcsa a helyén (nincs késleltetés)");
        CHECK(y.size() >= 44099 && y.size() <= 44101, "a kimenet hossza a várt ±1");
        double asym = 0, peak = std::abs(y[pk]);
        for (size_t k = 1; k < 4000 && k <= pk && pk + k < y.size(); ++k) asym = std::max(asym, std::abs(double(y[pk - k]) - y[pk + k]));
        std::printf("         aszimmetria: %.1f dB a csúcshoz\n", db(asym / peak));
        CHECK(db(asym / peak) <= -100.0, "szimmetrikus impulzusválasz (lineáris fázis)");
    }

    std::printf("[5] R5 – hézagmentesség\n");
    {
        const auto all = tone(1000.0, 0.5, 96000, 96000);
        const std::vector<double> a(all.begin(), all.begin() + 48000), b(all.begin() + 48000, all.end());
        writeWav<float>(dir / "g_all.wav", all, 1, 96000);
        writeWav<float>(dir / "g_a.wav", a, 1, 96000);
        writeWav<float>(dir / "g_b.wav", b, 1, 96000);
        std::vector<float> ya, yb, yw; srcconv::Result r1, r2, r3;
        conv(dir / "g_all.wav", 44100, REF, 0.0f, yw, r1);
        conv(dir / "g_a.wav", 44100, REF, 0.0f, ya, r2);
        conv(dir / "g_b.wav", 44100, REF, 0.0f, yb, r3);
        std::printf("         hosszak: %zu + %zu = %zu (egész: %zu)\n", ya.size(), yb.size(), ya.size() + yb.size(), yw.size());
        CHECK(ya.size() == 22050 && yb.size() == 22050 && yw.size() == 44100, "a két fél pontosan 22050 + 22050 keret");
        double d = 0;
        for (size_t i = 8000; i + 8000 < ya.size() && i < yw.size(); ++i) d = std::max(d, std::abs(double(ya[i]) - yw[i]));
        for (size_t i = 8000; i + 8000 < yb.size() && 22050 + i < yw.size(); ++i)
            d = std::max(d, std::abs(double(yb[i]) - yw[22050 + i]));
        std::printf("         eltérés a határtól távol: %.1f dB\n", db(d));
        CHECK(db(d) <= -120.0, "a határtól távol az egymás utáni felek = az egész (folytonos fázis)");
    }

    std::printf("[6] R6 – a szám végén a szűrő teljesen kiürül\n");
    {
        const fs::path p = dir / "fl.wav";
        std::vector<double> st(44100 * 2);
        for (size_t i = 0; i < 44100; ++i) { st[2 * i] = 0.3 * std::sin(2 * kPi * 1000.0 * i / 44100); st[2 * i + 1] = -st[2 * i]; }
        writeWav<float>(p, st, 2, 44100);
        for (uint32_t rate : {96000u, 384000u, 705600u}) {
            std::vector<float> y; srcconv::Result r;
            conv(p, rate, REF, 0.0f, y, r);
            char m[200];
            std::snprintf(m, sizeof m, "44,1 → %.1f kHz: kiürítve %llu keret %d lépésben, maradék %llu, hossz %llu (elvárt %u)",
                          rate / 1000.0, (unsigned long long)r.flushFrames, r.flushSteps, (unsigned long long)r.leftoverFrames,
                          (unsigned long long)r.outFrames, rate);
            CHECK(r.leftoverFrames == 0 && r.outFrames + 1 >= rate && r.outFrames <= rate + 1, m);
        }
    }

    std::printf("[7] R7 – csatornák\n");
    {
        std::vector<float> y; srcconv::Result r;
        conv(dir / "pb96.wav", 44100, REF, 0.0f, y, r);
        CHECK(r.channels == 1 && !r.monoMismatch, "mono: a lejátszó bal és jobb kimenete bitre azonos");
        std::vector<double> st(96000 * 2, 0.0);
        for (size_t i = 0; i < 96000; ++i) st[2 * i] = 0.5 * std::sin(2 * kPi * 3000.0 * i / 96000);
        writeWav<float>(dir / "st.wav", st, 2, 96000);
        std::vector<float> ys; srcconv::Result rs;
        conv(dir / "st.wav", 44100, REF, 0.0f, ys, rs);
        bool silent = rs.channels == 2;
        for (size_t i = 1; i < ys.size(); i += 2) if (ys[i] != 0.0f) silent = false;
        CHECK(silent, "sztereó: a csendes jobb csatorna pontosan csendes marad");
    }

    std::printf("[8] R8 – 64 bites lebegőpontos bemenet\n");
    {
        const auto x = tone(5000.0, 0.5, 96000, 96000);
        writeWav<float>(dir / "f32.wav", x, 1, 96000);
        writeWav<double>(dir / "f64.wav", x, 1, 96000);
        std::vector<float> a, b; srcconv::Result ra, rb;
        const bool ok = conv(dir / "f32.wav", 44100, REF, 0.0f, a, ra) && conv(dir / "f64.wav", 44100, REF, 0.0f, b, rb);
        CHECK(ok && a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * 4) == 0,
              "a 64 bites bemenet kimenete bitre azonos a 32 bitesével");
    }

    std::printf("[9] R9 – a kiírt WAV\n");
    {
        std::vector<float> d{0.25f, -0.5f, 1.0e-7f, 0.75f, -1.0f, 0.125f};
        const fs::path p = dir / "out.wav";
        std::string err;
        CHECK(srcconv::writeFloatWav(p.string(), d, 2, 44100, &err), "írás");
        std::vector<uint8_t> b(static_cast<size_t>(fs::file_size(p)));
        FILE* f = std::fopen(p.string().c_str(), "rb");
        std::fread(b.data(), 1, b.size(), f);
        std::fclose(f);
        auto u32 = [&](size_t o) { return uint32_t(b[o]) | uint32_t(b[o + 1]) << 8 | uint32_t(b[o + 2]) << 16 | uint32_t(b[o + 3]) << 24; };
        auto u16 = [&](size_t o) { return uint16_t(b[o] | b[o + 1] << 8); };
        bool ok = std::memcmp(b.data(), "RIFF", 4) == 0 && u32(4) == b.size() - 8 && std::memcmp(&b[8], "WAVE", 4) == 0;
        size_t i = 12, dataOff = 0; uint32_t dataLen = 0, fmtTag = 0, ch = 0, rate = 0, bits = 0, fact = 0;
        while (i + 8 <= b.size()) {
            const uint32_t sz = u32(i + 4);
            if (!std::memcmp(&b[i], "fmt ", 4)) { fmtTag = u16(i + 8); ch = u16(i + 10); rate = u32(i + 12); bits = u16(i + 22); }
            else if (!std::memcmp(&b[i], "fact", 4)) fact = u32(i + 8);
            else if (!std::memcmp(&b[i], "data", 4)) { dataOff = i + 8; dataLen = sz; }
            i += 8 + sz + (sz & 1);
        }
        ok = ok && fmtTag == 3 && ch == 2 && rate == 44100 && bits == 32 && fact == 3 && dataLen == 24 && dataOff + 24 == b.size();
        ok = ok && std::memcmp(&b[dataOff], d.data(), 24) == 0;
        CHECK(ok, "fejléc (IEEE float, 2 csat., 44100 Hz, 32 bit, fact = 3 keret) és a minták bitre");
    }

    std::printf("[10] R10 – fejtér\n");
    {
        const fs::path p = dir / "hr.wav";
        writeWav<float>(p, tone(1000.0, 0.5, 96000, 96000), 1, 96000);
        std::vector<float> a, b; srcconv::Result ra, rb;
        conv(p, 44100, REF, 0.0f, a, ra);
        conv(p, 44100, REF, -1.0f, b, rb);
        const double la = ampAt(a, 1, 0, 8000, 22050, 1000.0, 44100.0), lb = ampAt(b, 1, 0, 8000, 22050, 1000.0, 44100.0);
        std::printf("         0 dB: %+.5f dB, −1 dB: %+.5f dB\n", db(la / 0.5), db(lb / 0.5));
        CHECK(std::abs(db(la / 0.5)) <= 0.001, "0 dB fejtér: egységnyi erősítés");
        CHECK(std::abs(db(lb / la) + 1.0) <= 0.001, "−1 dB fejtér: pontosan −1 dB");
    }

    std::printf("[11] R11 – sebesség\n");
    {
        std::vector<double> st(44100 * 2 * 10);
        for (size_t i = 0; i < st.size() / 2; ++i) { st[2 * i] = 0.3 * std::sin(2 * kPi * 997.0 * i / 44100); st[2 * i + 1] = 0.3 * std::sin(2 * kPi * 1499.0 * i / 44100); }
        writeWav<float>(dir / "sp.wav", st, 2, 44100);
#if defined(__x86_64__) || defined(_M_X64)
        const double need = 2.0;
#else
        const double need = 0.5;
#endif
        for (uint32_t rate : {96000u, 384000u}) {
            std::vector<float> y; srcconv::Result r;
            const auto t0 = std::chrono::steady_clock::now();
            conv(dir / "sp.wav", rate, REF, 0.0f, y, r);
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            char m[160];
            std::snprintf(m, sizeof m, "Referencia, 44,1 → %.0f kHz sztereó: %.1f× valós idő (elvárt ≥ %.1f×)", rate / 1000.0, 10.0 / s, need);
            CHECK(10.0 / s >= need, m);
        }
    }

    std::printf("[12] R12 – a lejátszó kimenete = az átalakító kimenete, a szám vége nem vész el (Referencia, ×2)\n");
    {
        std::vector<double> st(44100 * 2);
        for (size_t i = 0; i < 44100; ++i) {
            st[2 * i] = 0.4 * std::sin(2 * kPi * 440.0 * i / 44100);
            st[2 * i + 1] = 0.4 * std::sin(2 * kPi * 21000.0 * i / 44100);
        }
        // a szám vége hangos: ha a lejátszó levágja, az eltérés biztosan látszik
        writeWav<float>(dir / "pl.wav", st, 2, 44100);
        audioopt::AudioOptions o;
        audioopt::applyUpsamplePreset(o, REF);
        o.upsampleHeadroomDb = 0.0f;
        o.upsampleRule = audioopt::UpsampleRule::Double;
        std::vector<float> ref; srcconv::Result rr;
        srcconv::convert((dir / "pl.wav").string(), 88200, o, ref, rr);

        auto outp = std::make_unique<PullOut>();
        PullOut* po = outp.get();
        player::FilePlayer p(std::move(outp));
        p.setUpsampleOptions(o);
        p.setUpsample(true);
        std::string err;
        const bool opened = p.open((dir / "pl.wav").string(), &err);
        for (int i = 0; i < 3000 && p.state() != player::FilePlayer::State::Playing; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::vector<float> got, blk(2048 * 2);
        for (int guard = 0; guard < 20000; ++guard) {
            for (int i = 0; i < 3000 && p.bufferedFrames() < 2048 && !p.decodeAtEnd(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (p.decodeAtEnd() && p.bufferedFrames() == 0) break;
            const size_t k = std::min<size_t>(2048, std::max<size_t>(1, p.bufferedFrames()));
            if (!po->pull(blk.data(), k)) break;
            got.insert(got.end(), blk.begin(), blk.begin() + static_cast<std::ptrdiff_t>(k * 2));
        }
        p.stop();
        auto firstLoud = [](const std::vector<float>& v) { size_t i = 0; while (i < v.size() && std::abs(v[i]) < 1e-3f) ++i; return i; };
        const size_t fg = firstLoud(got), fr = firstLoud(ref);
        bool same = opened && po->rate == 88200 && fg >= fr && ref.size() == 88200 * 2;
        const size_t off = fg - fr;
        if (same) same = got.size() >= off + ref.size() && std::memcmp(&got[off], ref.data(), ref.size() * 4) == 0;
        std::printf("         kimenet %u Hz, a lejátszótól %zu, az átalakítótól %zu keret (eltolás %zu)\n", po->rate, got.size() / 2,
                    ref.size() / 2, off / 2);
        CHECK(same, "a lejátszó a teljes számot adja, bitre azonosan a parancssori átalakítóval (88200 keret)");
    }

    std::printf("[13] R13 – 64 bites WAV olvasó (16/24/32 int, 32/64 float, WAVE_FORMAT_EXTENSIBLE)\n");
    {
        // 16 bit PCM (extra páratlan JUNK chunkkal)
        const std::vector<int32_t> i16{-32768, -16384, 0, 16384, 32767};
        writeWavPcm(dir / "r13_16.wav", i16, 16, 1, 96000, false, true);
        srcconv::WavDoubleData w16; std::string err;
        bool ok16 = srcconv::readWavDouble((dir / "r13_16.wav").string(), w16, &err) && w16.srcBits == 16 &&
                    w16.srcFmtTag == 1 && w16.channels == 1 && w16.sampleRate == 96000 && w16.samples.size() == i16.size();
        for (size_t i = 0; ok16 && i < i16.size(); ++i)
            if (std::abs(w16.samples[i] - static_cast<double>(i16[i]) / 32768.0) > 0.0) ok16 = false;
        CHECK(ok16, "16 bites PCM WAV (extra páratlan JUNK chunkkal) pontosan olvasva");

        // 24 bit PCM + WAVE_FORMAT_EXTENSIBLE
        const std::vector<int32_t> i24{-8388608, -1, 0, 1, 8388607, 123456};
        writeWavPcm(dir / "r13_24ext.wav", i24, 24, 2, 96000, true, true);
        srcconv::WavDoubleData w24;
        bool ok24 = srcconv::readWavDouble((dir / "r13_24ext.wav").string(), w24, &err) && w24.srcBits == 24 &&
                    w24.srcFmtTag == 1 && w24.channels == 2 && w24.samples.size() == i24.size();
        for (size_t i = 0; ok24 && i < i24.size(); ++i)
            if (std::abs(w24.samples[i] - static_cast<double>(i24[i]) / 8388608.0) > 1e-15) ok24 = false;
        CHECK(ok24, "24 bites WAVE_FORMAT_EXTENSIBLE PCM WAV pontosan olvasva");

        // 32 bit PCM
        const std::vector<int32_t> i32{INT32_MIN, -1000000000, 0, 1, INT32_MAX};
        writeWavPcm(dir / "r13_32i.wav", i32, 32, 1, 96000, false, false);
        srcconv::WavDoubleData w32;
        bool ok32 = srcconv::readWavDouble((dir / "r13_32i.wav").string(), w32, &err) && w32.srcBits == 32 &&
                    w32.srcFmtTag == 1 && w32.samples.size() == i32.size();
        for (size_t i = 0; ok32 && i < i32.size(); ++i)
            if (std::abs(w32.samples[i] - static_cast<double>(i32[i]) / 2147483648.0) > 1e-15) ok32 = false;
        CHECK(ok32, "32 bites PCM WAV pontosan olvasva (LSB pontossággal)");

        // 32 bit float + 64 bit float + 64 bit WAVE_FORMAT_EXTENSIBLE float
        const std::vector<double> dvals{0.12345678901234567, -5.623413251903491e-12, 1.0 - 1e-15, -1.0e-14, 0.0, 0.5};
        writeWav<float>(dir / "r13_32f.wav", dvals, 2, 96000);
        writeWav<double>(dir / "r13_64f.wav", dvals, 2, 96000);
        writeWavExtFloat64(dir / "r13_64ext.wav", dvals, 2, 96000);
        srcconv::WavDoubleData w32f, w64f, w64e;
        bool ok32f = srcconv::readWavDouble((dir / "r13_32f.wav").string(), w32f, &err) && w32f.srcBits == 32 &&
                     w32f.srcFmtTag == 3 && w32f.samples.size() == dvals.size();
        for (size_t i = 0; ok32f && i < dvals.size(); ++i)
            if (w32f.samples[i] != static_cast<double>(static_cast<float>(dvals[i]))) ok32f = false;
        const bool ok64f = srcconv::readWavDouble((dir / "r13_64f.wav").string(), w64f, &err) && w64f.srcBits == 64 &&
                           w64f.srcFmtTag == 3 && w64f.samples.size() == dvals.size() &&
                           std::memcmp(w64f.samples.data(), dvals.data(), dvals.size() * 8) == 0;
        const bool ok64e = srcconv::readWavDouble((dir / "r13_64ext.wav").string(), w64e, &err) && w64e.srcBits == 64 &&
                           w64e.srcFmtTag == 3 && w64e.samples.size() == dvals.size() &&
                           std::memcmp(w64e.samples.data(), dvals.data(), dvals.size() * 8) == 0;
        CHECK(ok32f && ok64f && ok64e, "32 bites float, 64 bites float és 64 bites EXTENSIBLE bitre pontosan olvasva");

        // Csonka / érvénytelen fájl elutasítása
        const fs::path bad = dir / "r13_bad.wav";
        { FILE* f = std::fopen(bad.string().c_str(), "wb"); std::fwrite("RIFF\x10\0\0\0WAVEfmt ", 1, 16, f); std::fclose(f); }
        srcconv::WavDoubleData wbad;
        CHECK(!srcconv::readWavDouble(bad.string(), wbad, &err), "csonka WAV hibával elutasítva");
    }

    std::printf("[14] R14 – 64 bites WAV író (fejléc és bitpontos minták)\n");
    {
        const std::vector<double> d{0.12345678901234567, -5.623413251903491e-12, 1.0e-15, 0.75, -1.0, 0.3333333333333333};
        const fs::path p = dir / "out64.wav";
        std::string err;
        CHECK(srcconv::writeDoubleWav(p.string(), d, 2, 44100, &err), "64 bites WAV írása");
        std::vector<uint8_t> b(static_cast<size_t>(fs::file_size(p)));
        FILE* f = std::fopen(p.string().c_str(), "rb");
        std::fread(b.data(), 1, b.size(), f);
        std::fclose(f);
        auto u32 = [&](size_t o) { return uint32_t(b[o]) | uint32_t(b[o + 1]) << 8 | uint32_t(b[o + 2]) << 16 | uint32_t(b[o + 3]) << 24; };
        auto u16 = [&](size_t o) { return uint16_t(b[o] | b[o + 1] << 8); };
        bool ok = std::memcmp(b.data(), "RIFF", 4) == 0 && u32(4) == b.size() - 8 && std::memcmp(&b[8], "WAVE", 4) == 0;
        size_t i = 12, dataOff = 0; uint32_t dataLen = 0, fmtTag = 0, ch = 0, rate = 0, align = 0, bits = 0, fact = 0;
        while (i + 8 <= b.size()) {
            const uint32_t sz = u32(i + 4);
            if (!std::memcmp(&b[i], "fmt ", 4)) {
                fmtTag = u16(i + 8); ch = u16(i + 10); rate = u32(i + 12); align = u16(i + 20); bits = u16(i + 22);
            } else if (!std::memcmp(&b[i], "fact", 4)) fact = u32(i + 8);
            else if (!std::memcmp(&b[i], "data", 4)) { dataOff = i + 8; dataLen = sz; }
            i += 8 + sz + (sz & 1);
        }
        ok = ok && fmtTag == 3 && ch == 2 && rate == 44100 && align == 16 && bits == 64 && fact == 3 &&
             dataLen == 48 && dataOff + 48 == b.size() && std::memcmp(&b[dataOff], d.data(), 48) == 0;
        CHECK(ok, "fejléc (IEEE float, 2 csat., 44100 Hz, 64 bit, fact = 3 keret) és 64 bites minták bitre");
    }

    std::printf("[15] R15 – 64 bites fájlátalakító (convertFile64) bitre egyezik a libsoxr közvetlen hívásával\n");
    {
        audioopt::AudioOptions o;
        audioopt::applyUpsamplePreset(o, REF);
        o.upsampleHeadroomDb = 0.0f;

        const size_t N = 24000;
        std::vector<int32_t> v16(N), v24(N), v32(N);
        std::vector<double> d16(N), d24(N), d32(N), d32f(N), d64f(N), d64tiny(N);
        for (size_t i = 0; i < N; ++i) {
            const double s = std::sin(2.0 * kPi * 1234.5 * static_cast<double>(i) / 96000.0);
            v16[i] = static_cast<int32_t>(std::llround(s * 20000.0));
            d16[i] = static_cast<double>(v16[i]) / 32768.0;
            v24[i] = static_cast<int32_t>(std::llround(s * 4000000.0));
            d24[i] = static_cast<double>(v24[i]) / 8388608.0;
            v32[i] = static_cast<int32_t>(std::llround(s * 1000000000.0));
            d32[i] = static_cast<double>(v32[i]) / 2147483648.0;
            d32f[i] = static_cast<double>(static_cast<float>(0.5 * s));
            d64f[i] = 0.5 * s + 1.0e-12 * std::sin(2.0 * kPi * 5432.1 * static_cast<double>(i) / 96000.0);
            d64tiny[i] = 5.6e-12 * s;  // −225 dBFS nagyságrendű 64 bites tesztjel
        }
        writeWavPcm(dir / "bd16.wav", v16, 16, 1, 96000);
        writeWavPcm(dir / "bd24.wav", v24, 24, 1, 96000);
        writeWavPcm(dir / "bd32i.wav", v32, 32, 1, 96000);
        writeWav<float>(dir / "bd32f.wav", d32f, 1, 96000);
        writeWav<double>(dir / "bd64f.wav", d64f, 1, 96000);
        writeWav<double>(dir / "bd64tiny.wav", d64tiny, 1, 96000);

        struct Case { const char* name; fs::path path; const std::vector<double>* raw; };
        const Case cases[] = {
            {"16 bit int", dir / "bd16.wav", &d16},
            {"24 bit int", dir / "bd24.wav", &d24},
            {"32 bit int", dir / "bd32i.wav", &d32},
            {"32 bit float", dir / "bd32f.wav", &d32f},
            {"64 bit float", dir / "bd64f.wav", &d64f},
            {"64 bit float (−225 dBFS)", dir / "bd64tiny.wav", &d64tiny},
        };
        for (const auto& c : cases) {
            std::vector<double> got;
            srcconv::Result res;
            std::string err;
            const bool ok = srcconv::convertFile64(c.path.string(), 44100, o, got, res, &err);
            const auto ref = directSoxr64(*c.raw, 1, 96000, 44100, o);
            const bool match = ok && res.outBits == 64 && res.leftoverFrames == 0 && got.size() == ref.size() &&
                               !got.empty() && std::memcmp(got.data(), ref.data(), got.size() * sizeof(double)) == 0;
            char msg[160];
            std::snprintf(msg, sizeof msg, "%s: bitre egyezik a közvetlen libsoxr SOXR_FLOAT64_I kimenettel (%zu keret)",
                          c.name, got.size());
            CHECK(match, msg);
        }
    }

    std::printf("[16] R16 – 64 bites fájlátalakító: visszahajlás és intermoduláció zajszintje ≤ −190 dB\n");
    {
        audioopt::AudioOptions o;
        audioopt::applyUpsamplePreset(o, REF);
        o.upsampleHeadroomDb = 0.0f;

        // 16a. Visszahajlás: 23 kHz (−4 dB) + 1 kHz (−150 dB) 96 → 44,1 kHz
        const size_t N = 96000 * 4;
        const double amp23k = std::pow(10.0, -4.0 / 20.0);
        const double amp1k = std::pow(10.0, -150.0 / 20.0);
        std::vector<double> alIn(N);
        for (size_t i = 0; i < N; ++i) {
            const double t = static_cast<double>(i) / 96000.0;
            alIn[i] = amp23k * std::sin(2.0 * kPi * 23000.0 * t) + amp1k * std::sin(2.0 * kPi * 1000.0 * t);
        }
        const fs::path pAl = dir / "r16_al.wav";
        writeWav<double>(pAl, alIn, 1, 96000);
        std::vector<double> alOut; srcconv::Result rAl; std::string err;
        CHECK(srcconv::convertFile64(pAl.string(), 44100, o, alOut, rAl, &err), "visszahajlás-teszt átalakítás (64 bit)");
        const size_t start = 40000, len = 90000;
        const double got1k = ampAtD(alOut, 1, 0, start, len, 1000.0, 44100.0);
        const double err1kDb = std::abs(db(got1k / amp1k));
        const auto alRem = removeToneD(alOut, start, len, 1000.0, 44100.0);
        const double alias21k = ampAtD(alRem, 1, 0, start, len, 21100.0, 44100.0);
        double alSumSq = 0.0;
        for (size_t i = 0; i < len; ++i) alSumSq += alRem[start + i] * alRem[start + i];
        const double alNoiseRmsDb = db(std::sqrt(alSumSq / static_cast<double>(len)));
        std::printf("         −150 dB-es 1 kHz eltérés: %.4f dB; 21,1 kHz visszahajlás: %.1f dB; maradék zaj RMS: %.1f dB\n",
                    err1kDb, db(alias21k), alNoiseRmsDb);
        CHECK(err1kDb <= 0.05, "a −150 dB-es 1 kHz-es jel sértetlenül megmarad (±0,05 dB) a −4 dB-es 23 kHz mellett");
        CHECK(db(alias21k) <= -190.0 && alNoiseRmsDb <= -190.0, "visszahajlás (21,1 kHz) és maradék zajpadló ≤ −190 dB");

        // 16b. Intermoduláció: 64,6 Hz (−6 dB) + 6998,2 Hz (−18 dB) 96 → 44,1 kHz
        const double f1 = 64.6, f2 = 6998.2;
        const double a1 = std::pow(10.0, -6.0 / 20.0), a2 = std::pow(10.0, -18.0 / 20.0);
        std::vector<double> imIn(N);
        for (size_t i = 0; i < N; ++i) {
            const double t = static_cast<double>(i) / 96000.0;
            imIn[i] = a1 * std::sin(2.0 * kPi * f1 * t) + a2 * std::sin(2.0 * kPi * f2 * t);
        }
        const fs::path pIm = dir / "r16_im.wav";
        writeWav<double>(pIm, imIn, 1, 96000);
        std::vector<double> imOut; srcconv::Result rIm;
        CHECK(srcconv::convertFile64(pIm.string(), 44100, o, imOut, rIm, &err), "intermoduláció-teszt átalakítás (64 bit)");
        // Két nem-egész-periódusú alaphang együttes kivonása (alternáló vetítés: 4 körben < −300 dB keresztszivárgás)
        auto imRem = imOut;
        for (int it = 0; it < 4; ++it) {
            imRem = removeToneD(imRem, start, len, f1, 44100.0);
            imRem = removeToneD(imRem, start, len, f2, 44100.0);
        }
        double imSumSq = 0.0;
        for (size_t i = 0; i < len; ++i) imSumSq += imRem[start + i] * imRem[start + i];
        const double imNoiseRmsDb = db(std::sqrt(imSumSq / static_cast<double>(len)));
        const double imdDiffDb = db(ampAtD(imRem, 1, 0, start, len, f2 - f1, 44100.0));
        const double imdSumDb = db(ampAtD(imRem, 1, 0, start, len, f2 + f1, 44100.0));
        std::printf("         IMD maradék RMS: %.1f dB, f2−f1: %.1f dB, f2+f1: %.1f dB\n", imNoiseRmsDb, imdDiffDb, imdSumDb);
        CHECK(imNoiseRmsDb <= -190.0 && imdDiffDb <= -190.0 && imdSumDb <= -190.0,
              "intermoduláció: a két alaphang kivonása után a zajszint és az IMD termékek ≤ −190 dB");
    }

    std::printf("[17] R17 – szélkezelés (V2): hézagmentesség a fájlhatáron\n");
    {
        audioopt::AudioOptions o;
        audioopt::applyUpsamplePreset(o, REF);
        o.upsampleHeadroomDb = 0.0f;

        // Két szinusz összege (236 Hz + 994 Hz), a vágáspontnál nagy amplitúdóval
        const size_t N = 96000;
        const size_t split = 48000;  // 48000 * 441 / 960 = 22050 egész kimeneti keret
        std::vector<double> full(N);
        for (size_t i = 0; i < N; ++i) {
            const double t = static_cast<double>(i) / 96000.0;
            full[i] = 0.55 * std::sin(2.0 * kPi * 236.0 * t + 0.7) + 0.38 * std::sin(2.0 * kPi * 994.0 * t + 1.1);
        }
        const std::vector<double> partA(full.begin(), full.begin() + split);
        const std::vector<double> partB(full.begin() + split, full.end());
        writeWav<double>(dir / "v2_full.wav", full, 1, 96000);
        writeWav<double>(dir / "v2_a.wav", partA, 1, 96000);
        writeWav<double>(dir / "v2_b.wav", partB, 1, 96000);

        srcconv::FileOptions foLpc; foLpc.edge = srcconv::EdgeMode::Lpc;
        srcconv::FileOptions foOdd; foOdd.edge = srcconv::EdgeMode::Odd;
        srcconv::FileOptions foNone; foNone.edge = srcconv::EdgeMode::None;

        std::vector<double> yRef, yNoneA, yNoneB, yOddA, yOddB, yLpcA, yLpcB;
        srcconv::Result r; std::string err;
        CHECK(srcconv::convertFile64((dir / "v2_full.wav").string(), 44100, o, yRef, r, &err, foLpc), "egész referencia átalakítása");
        srcconv::convertFile64((dir / "v2_a.wav").string(), 44100, o, yNoneA, r, &err, foNone);
        srcconv::convertFile64((dir / "v2_b.wav").string(), 44100, o, yNoneB, r, &err, foNone);
        srcconv::convertFile64((dir / "v2_a.wav").string(), 44100, o, yOddA, r, &err, foOdd);
        srcconv::convertFile64((dir / "v2_b.wav").string(), 44100, o, yOddB, r, &err, foOdd);
        srcconv::convertFile64((dir / "v2_a.wav").string(), 44100, o, yLpcA, r, &err, foLpc);
        srcconv::convertFile64((dir / "v2_b.wav").string(), 44100, o, yLpcB, r, &err, foLpc);

        auto seamErr = [&](const std::vector<double>& a, const std::vector<double>& b) -> double {
            if (a.size() != 22050 || b.size() != 22050 || yRef.size() != 44100) return 1e9;
            double mx = 0.0;
            for (size_t i = 22050 - 50; i < 22050; ++i) mx = std::max(mx, std::abs(a[i] - yRef[i]));
            for (size_t i = 0; i < 50; ++i) mx = std::max(mx, std::abs(b[i] - yRef[22050 + i]));
            return mx;
        };
        const double eNone = seamErr(yNoneA, yNoneB);
        const double eOdd = seamErr(yOddA, yOddB);
        const double eLpc = seamErr(yLpcA, yLpcB);
        std::printf("         illesztés ±50 minta max eltérés: none=%.1f dB, odd=%.1f dB, lpc=%.1f dB\n",
                    db(eNone), db(eOdd), db(eLpc));
        CHECK(db(eOdd) <= -40.0 && eOdd < eNone * 0.1, "páratlan tükrözés (odd): jelentősen csökkenti a határhibát (≤ −40 dB)");
        CHECK(db(eLpc) <= -120.0, "Burg-LPC szélkezelés: az illesztés ±50 mintáján ≤ −120 dB az ideális egyben átalakítottól");

        // Nem-egész vágás (3287 + 2409 minta, mint a gaplesstest_s/_m): fázisigazítás ellenőrzése
        const std::vector<double> smS(full.begin(), full.begin() + 3287);
        const std::vector<double> smM(full.begin() + 3287, full.begin() + 3287 + 2409);
        const std::vector<double> smAll(full.begin(), full.begin() + 3287 + 2409);
        writeWav<double>(dir / "v2_sm_all.wav", smAll, 1, 96000);
        writeWav<double>(dir / "v2_sm_s.wav", smS, 1, 96000);
        writeWav<double>(dir / "v2_sm_m.wav", smM, 1, 96000);
        std::vector<double> ySmAll, ySmS, ySmM;
        srcconv::convertFile64((dir / "v2_sm_all.wav").string(), 44100, o, ySmAll, r, &err, foLpc);
        srcconv::convertFile64((dir / "v2_sm_s.wav").string(), 44100, o, ySmS, r, &err, foLpc);
        srcconv::convertFile64((dir / "v2_sm_m.wav").string(), 44100, o, ySmM, r, &err, foLpc);
        double sEndErr = 0.0;
        for (size_t i = ySmS.size() - 50; i < ySmS.size(); ++i) sEndErr = std::max(sEndErr, std::abs(ySmS[i] - ySmAll[i]));
        // A második fájl első mintája a bemenet saját t = 0 pontjához (smM[0]) igazodik
        const double mStartErr = std::abs(ySmM[0] - smM[0]);
        std::printf("         3287+2409 próba: s_vége eltérés=%.1f dB, m[0]−bemenet[0]=%.1f dB (hossz %zu+%zu)\n",
                    db(sEndErr), db(mStartErr), ySmS.size(), ySmM.size());
        CHECK(ySmS.size() == 1510 && ySmM.size() == 1107 && db(sEndErr) <= -120.0 && db(mStartErr) <= -80.0,
              "nem-egész vágáspontnál s vége ≤ −120 dB, m eleje pontosan t=0-hoz igazodik");
    }

    std::printf("[18] R18 – szélkezelés csendes szélen és sztereó csatorna-izoláció\n");
    {
        audioopt::AudioOptions o;
        audioopt::applyUpsamplePreset(o, REF);
        o.upsampleHeadroomDb = 0.0f;

        // Csenddel kezdődő és végződő impulzus: LPC kimenete bitre azonos a nyers úttal
        std::vector<double> imp(96000, 0.0);
        imp[48000] = 0.5;
        writeWav<double>(dir / "v2_imp.wav", imp, 1, 96000);
        srcconv::FileOptions foNone, foLpc;
        foNone.edge = srcconv::EdgeMode::None;
        foLpc.edge = srcconv::EdgeMode::Lpc;
        std::vector<double> yImp0, yImp1; srcconv::Result r; std::string err;
        srcconv::convertFile64((dir / "v2_imp.wav").string(), 44100, o, yImp0, r, &err, foNone);
        srcconv::convertFile64((dir / "v2_imp.wav").string(), 44100, o, yImp1, r, &err, foLpc);
        CHECK(yImp0.size() == yImp1.size() && std::memcmp(yImp0.data(), yImp1.data(), yImp0.size() * sizeof(double)) == 0,
              "csendes szélű impulzuson az LPC szélkezelés bitre azonos a nyers úttal");

        // Sztereó: bal csatorna hangos a szélen, jobb csatorna csendes → jobb csatorna pontosan 0.0 marad
        std::vector<double> st(48000 * 2, 0.0);
        for (size_t i = 0; i < 48000; ++i)
            st[2 * i] = 0.7 * std::cos(2.0 * kPi * 500.0 * static_cast<double>(i) / 96000.0);
        writeWav<double>(dir / "v2_st.wav", st, 2, 96000);
        std::vector<double> ySt;
        srcconv::convertFile64((dir / "v2_st.wav").string(), 44100, o, ySt, r, &err, foLpc);
        bool rSilent = (r.channels == 2 && !ySt.empty());
        for (size_t i = 1; i < ySt.size(); i += 2) if (ySt[i] != 0.0) rSilent = false;
        CHECK(rSilent, "sztereó LPC: a csendes jobb csatorna pontosan 0.0 marad");
    }

    std::printf("[19] R19 – saját 64 bites FFT (vegyes-radix 2/3/4/5/7 + Bluestein + RealFft)\n");
    {
        // Komplex FFT: vegyes-radix (147, 320, 512, 1920) és Bluestein (101, 222) oda-vissza egyezése és Parseval
        bool cOk = true;
        double worstC = 0.0;
        for (size_t n : {147u, 320u, 512u, 1920u, 101u, 222u}) {
            srcconv::fft64::ComplexFft fft(n);
            std::vector<srcconv::fft64::C64> orig(n), buf(n);
            double eTime = 0.0;
            for (size_t i = 0; i < n; ++i) {
                orig[i] = {std::sin(0.37 * static_cast<double>(i) + 0.2), std::cos(0.91 * static_cast<double>(i) - 0.5)};
                eTime += orig[i].r * orig[i].r + orig[i].i * orig[i].i;
            }
            buf = orig;
            fft.transform(buf.data(), false);
            double eFreq = 0.0;
            for (const auto& z : buf) eFreq += z.r * z.r + z.i * z.i;
            const double parsevalErr = std::abs(eFreq / static_cast<double>(n) - eTime) / std::max(eTime, 1.0);
            fft.transform(buf.data(), true);
            for (size_t i = 0; i < n; ++i) {
                worstC = std::max(worstC, std::hypot(buf[i].r - orig[i].r, buf[i].i - orig[i].i));
            }
            if (parsevalErr > 1e-13) cOk = false;
        }
        std::printf("         ComplexFft max oda-vissza hiba: %.2e\n", worstC);
        CHECK(cOk && worstC <= 1e-13, "ComplexFft (vegyes-radix + Bluestein): IFFT(FFT(x)) == x és Parseval ≤ 1e-13");

        // RealFft: páros méretek (320, 2940, 1024, 150) oda-vissza egyezése
        double worstR = 0.0;
        for (size_t n : {150u, 320u, 1024u, 2940u}) {
            srcconv::fft64::RealFft rfft(n);
            std::vector<double> x(n), y(n);
            std::vector<srcconv::fft64::C64> X(n / 2 + 1);
            for (size_t i = 0; i < n; ++i) x[i] = std::sin(0.53 * static_cast<double>(i) + 0.1) + 0.3 * std::cos(1.7 * static_cast<double>(i));
            rfft.forward(x.data(), X.data());
            rfft.inverse(X.data(), y.data());
            for (size_t i = 0; i < n; ++i) worstR = std::max(worstR, std::abs(y[i] - x[i]));
        }
        std::printf("         RealFft max oda-vissza hiba: %.2e\n", worstR);
        CHECK(worstR <= 1e-13, "RealFft: IRFFT(RFFT(x)) == x ≤ 1e-13");
    }

    std::printf("[20] R20 – egylépcsős 64 bites FFT+Kaiser-sinc motor (FileEngine::Fft64)\n");
    {
        audioopt::AudioOptions o;
        audioopt::applyUpsamplePreset(o, REF);
        o.upsampleHeadroomDb = 0.0f;
        srcconv::FileOptions fo;
        fo.engine = srcconv::FileEngine::Fft64;
        fo.edge = srcconv::EdgeMode::Lpc;

        // 20a. Sávszélesség (22,0 kHz átvitel 96 -> 44,1 kHz-en ±0,01 dB) és felfelé (44,1 -> 96 kHz, 21,9 kHz)
        const fs::path pPb = dir / "f64_pb.wav";
        writeWav<double>(pPb, tone(22000.0, 0.5, 96000, 96000 * 3), 1, 96000);
        std::vector<double> yPb; srcconv::Result r; std::string err;
        CHECK(srcconv::convertFile64(pPb.string(), 44100, o, yPb, r, &err, fo), "Fft64 96 -> 44,1 kHz átalakítás");
        const double a22 = ampAtD(yPb, 1, 0, 35000, 60000, 22000.0, 44100.0);
        std::printf("         22,0 kHz átvitel: %+.5f dB\n", db(a22 / 0.5));
        CHECK(std::abs(db(a22 / 0.5)) <= 0.01, "Fft64 sávszélesség: 22,0 kHz szintje ±0,01 dB");

        const fs::path pUp = dir / "f64_up.wav";
        writeWav<double>(pUp, tone(21900.0, 0.5, 44100, 44100 * 3), 1, 44100);
        std::vector<double> yUp;
        CHECK(srcconv::convertFile64(pUp.string(), 96000, o, yUp, r, &err, fo), "Fft64 44,1 -> 96 kHz átalakítás");
        const double aUp = ampAtD(yUp, 1, 0, 70000, 120000, 21900.0, 96000.0);
        std::printf("         felfelé 21,9 kHz átvitel: %+.5f dB\n", db(aUp / 0.5));
        CHECK(std::abs(db(aUp / 0.5)) <= 0.01, "Fft64 felfelé: 21,9 kHz szintje ±0,01 dB");

        // 20b. Visszahajlás + -150 dB jel és Intermoduláció ≤ -250 dB
        const size_t N = 96000 * 4;
        const double amp23k = std::pow(10.0, -4.0 / 20.0), amp1k = std::pow(10.0, -150.0 / 20.0);
        std::vector<double> alIn(N);
        for (size_t i = 0; i < N; ++i) {
            const double t = static_cast<double>(i) / 96000.0;
            alIn[i] = amp23k * std::sin(2.0 * kPi * 23000.0 * t) + amp1k * std::sin(2.0 * kPi * 1000.0 * t);
        }
        writeWav<double>(dir / "f64_al.wav", alIn, 1, 96000);
        std::vector<double> alOut;
        srcconv::convertFile64((dir / "f64_al.wav").string(), 44100, o, alOut, r, &err, fo);
        const size_t st = 40000, ln = 90000;
        const double err1k = std::abs(db(ampAtD(alOut, 1, 0, st, ln, 1000.0, 44100.0) / amp1k));
        const auto alRem = removeToneD(alOut, st, ln, 1000.0, 44100.0);
        const double alias21k = db(ampAtD(alRem, 1, 0, st, ln, 21100.0, 44100.0));
        double alSq = 0.0;
        for (size_t i = 0; i < ln; ++i) alSq += alRem[st + i] * alRem[st + i];
        const double alRms = db(std::sqrt(alSq / static_cast<double>(ln)));
        std::printf("         Fft64 visszahajlás: −150 dB jel hiba=%.4f dB, 21,1 kHz=%.1f dB, maradék RMS=%.1f dB\n",
                    err1k, alias21k, alRms);
        CHECK(err1k <= 0.01 && alias21k <= -260.0 && alRms <= -220.0,
              "Fft64 visszahajlás: −150 dB jel megmarad, 21,1 kHz ≤ −260 dB és maradék RMS ≤ −220 dB");

        const double f1 = 64.6, f2 = 6998.2;
        const double a1 = std::pow(10.0, -6.0 / 20.0), a2 = std::pow(10.0, -18.0 / 20.0);
        std::vector<double> imIn(N);
        for (size_t i = 0; i < N; ++i) {
            const double t = static_cast<double>(i) / 96000.0;
            imIn[i] = a1 * std::sin(2.0 * kPi * f1 * t) + a2 * std::sin(2.0 * kPi * f2 * t);
        }
        writeWav<double>(dir / "f64_im.wav", imIn, 1, 96000);
        std::vector<double> imOut;
        srcconv::convertFile64((dir / "f64_im.wav").string(), 44100, o, imOut, r, &err, fo);
        auto imRem = imOut;
        for (int it = 0; it < 4; ++it) {
            imRem = removeToneD(imRem, st, ln, f1, 44100.0);
            imRem = removeToneD(imRem, st, ln, f2, 44100.0);
        }
        double imSq = 0.0;
        for (size_t i = 0; i < ln; ++i) imSq += imRem[st + i] * imRem[st + i];
        const double imRms = db(std::sqrt(imSq / static_cast<double>(ln)));
        const double imdDiff = db(ampAtD(imRem, 1, 0, st, ln, f2 - f1, 44100.0));
        const double imdSum = db(ampAtD(imRem, 1, 0, st, ln, f2 + f1, 44100.0));
        std::printf("         Fft64 IMD maradék RMS: %.1f dB, f2−f1: %.1f dB, f2+f1: %.1f dB\n", imRms, imdDiff, imdSum);
        CHECK(imRms <= -235.0 && imdDiff <= -260.0 && imdSum <= -260.0,
              "Fft64 intermoduláció: maradék RMS ≤ −235 dB és f2±f1 ≤ −260 dB");

        // 20c. Impulzus: 0 késleltetés, szimmetrikus lineáris fázis
        std::vector<double> imp(96000 * 2, 0.0);
        imp[96000] = 0.5;
        writeWav<double>(dir / "f64_imp.wav", imp, 1, 96000);
        std::vector<double> yImp;
        srcconv::convertFile64((dir / "f64_imp.wav").string(), 44100, o, yImp, r, &err, fo);
        size_t pk = 0;
        for (size_t i = 0; i < yImp.size(); ++i) if (std::abs(yImp[i]) > std::abs(yImp[pk])) pk = i;
        double asym = 0.0;
        for (size_t k = 1; k < 4000 && k <= pk && pk + k < yImp.size(); ++k)
            asym = std::max(asym, std::abs(yImp[pk - k] - yImp[pk + k]));
        std::printf("         Fft64 impulzus csúcs: %zu (elvárt 44100), aszimmetria: %.1f dB\n", pk, db(asym / std::abs(yImp[pk])));
        CHECK(pk == 44100 && db(asym / std::abs(yImp[pk])) <= -250.0,
              "Fft64 impulzus: nulla késleltetés és szimmetrikus lineáris fázis (≤ −250 dB)");

        // 20d. Hézagmentesség (LPC + Fft64) és sztereó csatorna-izoláció
        std::vector<double> yFull, yA, yB;
        srcconv::convertFile64((dir / "v2_full.wav").string(), 44100, o, yFull, r, &err, fo);
        srcconv::convertFile64((dir / "v2_a.wav").string(), 44100, o, yA, r, &err, fo);
        srcconv::convertFile64((dir / "v2_b.wav").string(), 44100, o, yB, r, &err, fo);
        double gErr = 0.0;
        for (size_t i = 22050 - 50; i < 22050; ++i) gErr = std::max(gErr, std::abs(yA[i] - yFull[i]));
        for (size_t i = 0; i < 50; ++i) gErr = std::max(gErr, std::abs(yB[i] - yFull[22050 + i]));
        std::printf("         Fft64 + LPC illesztés ±50 minta max eltérés: %.1f dB\n", db(gErr));
        CHECK(yA.size() == 22050 && yB.size() == 22050 && db(gErr) <= -150.0,
              "Fft64 + LPC hézagmentesség: illesztés ±50 mintán ≤ −150 dB");

        std::vector<double> ySt;
        srcconv::convertFile64((dir / "v2_st.wav").string(), 44100, o, ySt, r, &err, fo);
        bool rZero = (r.channels == 2 && !ySt.empty());
        for (size_t i = 1; i < ySt.size(); i += 2) if (ySt[i] != 0.0) rZero = false;
        CHECK(rZero, "Fft64 sztereó: a csendes jobb csatorna pontosan 0.0 marad");
    }

    std::printf("[21] R21 – beépített lejátszó FFT64 motor + választható fázis (0/25/50) + FilePlayer\n");
    {
        // 21a. AudioOptions upsampleEngine JSON oda-vissza és sanitize (0=libsoxr, 1=FFT64, 2=FFT-DD)
        audioopt::AudioOptions o;
        o.upsampleEngine = 1;
        json::Value jv;
        const bool parsed = json::parse(audioopt::toJson(o), jv);
        audioopt::AudioOptions o2 = audioopt::fromJson(jv);
        CHECK(parsed && o2.upsampleEngine == 1, "AudioOptions::upsampleEngine=1 JSON oda-vissza");
        o.upsampleEngine = 2;
        json::Value jvDD;
        const bool parsedDD = json::parse(audioopt::toJson(o), jvDD);
        audioopt::AudioOptions oDD = audioopt::fromJson(jvDD);
        audioopt::sanitize(oDD);
        CHECK(parsedDD && oDD.upsampleEngine == 2, "AudioOptions::upsampleEngine=2 (106 bites FFT-DD) JSON oda-vissza és sanitize");
        o2.upsampleEngine = 99;
        audioopt::sanitize(o2);
        CHECK(o2.upsampleEngine == 0, "AudioOptions::upsampleEngine érvénytelen érték visszaáll 0-ra");

        // 21a-dd. UpsampleEngine streaming 106 bites FFT-DD (44,1 -> 88,2 kHz)
        {
            UpsampleEngine engDD;
            const bool initDD = engDD.init(44100, 88200, 1, 32, 50.0, 0.9966, 1.00, 0.0f,
                                           static_cast<int>(audioopt::UpsampleEngineType::FftDD));
            CHECK(initDD && engDD.isActive() && engDD.isFft64(), "UpsampleEngine 106 bites FFT-DD init");
            std::vector<float> inDD(44100);
            for (size_t i = 0; i < inDD.size(); ++i)
                inDD[i] = 0.5f * std::sin(2.0f * static_cast<float>(kPi) * 10000.0f * i / 44100.0f);
            std::vector<float> outDD, chunkDD(8192);
            size_t posDD = 0;
            while (posDD < inDD.size()) {
                const size_t n = std::min<size_t>(2048, inDD.size() - posDD);
                size_t got = 0;
                engDD.process(inDD.data() + posDD, n, chunkDD.data(), chunkDD.size(), got);
                outDD.insert(outDD.end(), chunkDD.begin(), chunkDD.begin() + static_cast<std::ptrdiff_t>(got));
                posDD += n;
            }
            for (int fstep = 0; fstep < 100; ++fstep) {
                size_t got = 0;
                engDD.flush(chunkDD.data(), chunkDD.size(), got);
                if (got == 0) break;
                outDD.insert(outDD.end(), chunkDD.begin(), chunkDD.begin() + static_cast<std::ptrdiff_t>(got));
            }
            const double a10DD = ampAt(outDD, 1, 0, 22050, 44100, 10000.0, 88200.0);
            const double imgDD = ampAt(outDD, 1, 0, 22050, 44100, 34100.0, 88200.0);
            CHECK(outDD.size() == 88200 && std::abs(db(a10DD / 0.5)) <= 0.01 && db(imgDD / 0.5) <= -135.0,
                  "UpsampleEngine 106 bites FFT-DD streaming: egzakt hossz, 10 kHz átvitel ±0,01 dB és tükörkép ≤ −135 dB");
        }

        // 21b. UpsampleEngine streaming FFT64 mindhárom fázissal (50=lineáris, 25=köztes, 0=minimum)
        for (double ph : {50.0, 25.0, 0.0}) {
            UpsampleEngine eng;
            const bool initOk = eng.init(44100, 96000, 1, 32, ph, 0.95, 1.00, 0.0f,
                                         static_cast<int>(audioopt::UpsampleEngineType::Fft64));
            CHECK(initOk && eng.isActive() && eng.isFft64(), "UpsampleEngine FFT64 init és isFft64()");

            // 1 másodperces 10 kHz szinusz + impulzus ellenőrzés
            std::vector<float> in(44100);
            for (size_t i = 0; i < in.size(); ++i) in[i] = 0.5f * std::sin(2.0f * static_cast<float>(kPi) * 10000.0f * i / 44100.0f);
            std::vector<float> out, chunk(8192);
            size_t pos = 0;
            while (pos < in.size()) {
                const size_t n = std::min<size_t>(2048, in.size() - pos);
                size_t got = 0;
                eng.process(in.data() + pos, n, chunk.data(), chunk.size(), got);
                out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
                pos += n;
            }
            for (int fstep = 0; fstep < 100; ++fstep) {
                size_t got = 0;
                eng.flush(chunk.data(), chunk.size(), got);
                if (got == 0) break;
                out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
            }
            const double a10 = ampAt(out, 1, 0, 24000, 48000, 10000.0, 96000.0);
            const double img = ampAt(out, 1, 0, 24000, 48000, 34100.0, 96000.0);
            std::printf("         FFT64 stream (fázis %.0f): hossz=%zu (elvárt 96000), 10 kHz=%.4f dB, tükörkép 34,1 kHz=%.1f dB\n",
                        ph, out.size(), db(a10 / 0.5), db(img / 0.5));
            CHECK(out.size() == 96000 && std::abs(db(a10 / 0.5)) <= 0.02 && db(img / 0.5) <= -120.0,
                  "FFT64 streaming: egzakt hossz, 10 kHz átvitel ±0,02 dB és tükörkép ≤ −120 dB");

            // Impulzus előrezgés: minimumfázisnál (ph=0) a csúcs előtti régió elhanyagolható (< -90 dB)
            if (ph == 0.0) {
                eng.clear();
                std::vector<float> imp(44100, 0.0f);
                imp[22050] = 0.8f;
                std::vector<float> iout;
                size_t ip = 0;
                while (ip < imp.size()) {
                    const size_t n = std::min<size_t>(2048, imp.size() - ip);
                    size_t got = 0;
                    eng.process(imp.data() + ip, n, chunk.data(), chunk.size(), got);
                    iout.insert(iout.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
                    ip += n;
                }
                for (int fstep = 0; fstep < 100; ++fstep) {
                    size_t got = 0;
                    eng.flush(chunk.data(), chunk.size(), got);
                    if (got == 0) break;
                    iout.insert(iout.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
                }
                size_t pk = 0;
                for (size_t i = 0; i < iout.size(); ++i) if (std::abs(iout[i]) > std::abs(iout[pk])) pk = i;
                // t0 = 22050 * 96000 / 44100 = 48000: minimumfázisnál t < t0 (az impulzus előtti régió) zéró!
                const size_t t0 = 48000;
                double preMax = 0.0;
                for (size_t i = (t0 > 2000 ? t0 - 2000 : 0); i < t0; ++i) preMax = std::max<double>(preMax, std::abs(iout[i]));
                std::printf("         FFT64 minimumfázis impulzus: t0=%zu, csúcs=%zu, t<t0 előrezgés=%.1f dB\n",
                            t0, pk, db(preMax / std::abs(iout[pk])));
                CHECK(pk >= t0 && db(preMax / std::abs(iout[pk])) <= -110.0,
                      "FFT64 minimumfázis: kauzális (csúcs ≥ t0) és t < t0 előrezgés ≤ −110 dB");
            }
        }

        // 21c. FilePlayer végigjátszás FFT64 motorral (44,1 -> 88,2 kHz, Double szabály)
        audioopt::AudioOptions poOpt;
        audioopt::applyUpsamplePreset(poOpt, 1);  // köztes fázis (25)
        poOpt.upsampleEngine = static_cast<int>(audioopt::UpsampleEngineType::Fft64);
        poOpt.upsampleHeadroomDb = 0.0f;
        poOpt.upsampleRule = audioopt::UpsampleRule::Double;

        auto outp = std::make_unique<PullOut>();
        PullOut* po = outp.get();
        player::FilePlayer fp(std::move(outp));
        fp.setUpsampleOptions(poOpt);
        fp.setUpsample(true);
        std::string err;
        const bool opened = fp.open((dir / "pl.wav").string(), &err);
        for (int i = 0; i < 3000 && fp.state() != player::FilePlayer::State::Playing; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::vector<float> got, blk(2048 * 2);
        for (int guard = 0; guard < 20000; ++guard) {
            for (int i = 0; i < 3000 && fp.bufferedFrames() < 2048 && !fp.decodeAtEnd(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (fp.decodeAtEnd() && fp.bufferedFrames() == 0) break;
            const size_t k = std::min<size_t>(2048, std::max<size_t>(1, fp.bufferedFrames()));
            if (!po->pull(blk.data(), k)) break;
            got.insert(got.end(), blk.begin(), blk.begin() + static_cast<std::ptrdiff_t>(k * 2));
        }
        fp.stop();
        std::printf("         FilePlayer FFT64: kimenet %u Hz, %zu keret\n", po->rate, got.size() / 2);
        CHECK(opened && po->rate == 88200 && got.size() >= 88200 * 2,
              "FilePlayer FFT64 (köztes fázis): 88,2 kHz kimenet és teljes számhossz (≥ 88200 keret)");
    }

    std::printf("[22] R22 – 106 bites double-double FFT motor (FileEngine::FftDD)\n");
    {
        audioopt::AudioOptions o;
        audioopt::applyUpsamplePreset(o, REF);
        o.upsampleHeadroomDb = 0.0f;
        srcconv::FileOptions fo;
        fo.engine = srcconv::FileEngine::FftDD;
        fo.edge = srcconv::EdgeMode::Lpc;

        // 22a. 40 kHz tiszta szinusz (záró sáv, fáziszaj-mentes bemenet): FftDD ≤ −310 dB RMS!
        const size_t n40 = 96000 * 2;
        std::vector<double> x40(n40);
        for (size_t i = 0; i < n40; ++i) {
            const uint64_t num = (40000ull * i) % 96000ull;
            srcconv::fft64::DD sn, cs;
            srcconv::fft64::realSinCosRatio<srcconv::fft64::DD>(static_cast<int64_t>(2 * num), 96000ull, sn, cs);
            x40[i] = sn.hi;
        }
        writeWav<double>(dir / "dd_40k.wav", x40, 1, 96000);
        std::vector<double> y40; srcconv::Result r; std::string err;
        CHECK(srcconv::convertFile64((dir / "dd_40k.wav").string(), 44100, o, y40, r, &err, fo), "FftDD 40 kHz átalakítás");
        double sq = 0.0, pk40 = 0.0;
        const size_t st40 = 15000, ln40 = 55000;
        for (size_t i = st40; i < st40 + ln40; ++i) {
            sq += y40[i] * y40[i];
            pk40 = std::max(pk40, std::abs(y40[i]));
        }
        const double rms40 = db(std::sqrt(sq / static_cast<double>(ln40)));
        std::printf("         FftDD 40 kHz záró sáv: RMS=%.1f dB, csúcs=%.1f dB\n", rms40, db(pk40));
        CHECK(rms40 <= -310.0 && db(pk40) <= -295.0, "FftDD 40 kHz záró sáv: RMS ≤ −310 dB és csúcs ≤ −295 dB");

        // 22b. 21,95 kHz áteresztés (±0,005 dB), 22,0 kHz (≥ −0,35 dB) és impulzus zéró késleltetés
        std::vector<double> x2195(96000 * 2);
        for (size_t i = 0; i < x2195.size(); ++i) x2195[i] = 0.5 * std::sin(2.0 * kPi * 21950.0 * static_cast<double>(i) / 96000.0);
        writeWav<double>(dir / "dd_2195.wav", x2195, 1, 96000);
        std::vector<double> y2195, yPb, yImp;
        CHECK(srcconv::convertFile64((dir / "dd_2195.wav").string(), 44100, o, y2195, r, &err, fo), "FftDD 21,95 kHz átalakítás");
        const double a2195 = ampAtD(y2195, 1, 0, 20000, 48000, 21950.0, 44100.0);
        CHECK(srcconv::convertFile64((dir / "f64_pb.wav").string(), 44100, o, yPb, r, &err, fo), "FftDD 22 kHz átalakítás");
        const double a22 = ampAtD(yPb, 1, 0, 20000, 48000, 22000.0, 44100.0);
        std::printf("         FftDD 21,95 kHz: %.5f dB, 22,0 kHz: %.3f dB\n", db(a2195 / 0.5), db(a22 / 0.5));
        CHECK(std::abs(db(a2195 / 0.5)) <= 0.005 && db(a22 / 0.5) >= -0.35, "FftDD sávszélesség: 21,95 kHz ±0,005 dB és 22,0 kHz ≥ −0,35 dB");

        CHECK(srcconv::convertFile64((dir / "f64_imp.wav").string(), 44100, o, yImp, r, &err, fo), "FftDD impulzus átalakítás");
        size_t pk = 0;
        for (size_t i = 0; i < yImp.size(); ++i) if (std::abs(yImp[i]) > std::abs(yImp[pk])) pk = i;
        double asym = 0.0;
        for (size_t k = 1; k < 4000 && k <= pk && pk + k < yImp.size(); ++k)
            asym = std::max(asym, std::abs(yImp[pk - k] - yImp[pk + k]));
        std::printf("         FftDD impulzus csúcs: %zu (elvárt 44100), aszimmetria: %.1f dB\n", pk, db(asym / std::abs(yImp[pk])));
        CHECK(pk == 44100 && db(asym / std::abs(yImp[pk])) <= -300.0, "FftDD impulzus: 0 késleltetés és ≤ −300 dB szimmetria");
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    if (g_fail == 0) std::printf("\nMINDEN TESZT SIKERES (0 hiba)\n");
    else std::printf("\n%d HIBA: SIKERTELEN\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
