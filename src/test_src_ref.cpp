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

    std::error_code ec;
    fs::remove_all(dir, ec);
    if (g_fail == 0) std::printf("\nMINDEN TESZT SIKERES (0 hiba)\n");
    else std::printf("\n%d HIBA: SIKERTELEN\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
