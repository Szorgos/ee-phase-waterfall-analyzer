// A nagy felbontás B részének tesztjei (DSD512 / DSD1024; terv: aqulify-mobile/docs/plans/terv-claude-hires-2026-11-17.md):
//  [1] K3: DSF és DFF DSD64–DSD1024, mindkét családban (44,1 / 48 kHz): fejléc, hossz, nyers bitfolyam bitre, tekerés
//  [2] K8: DSD512 / DSD1024 → PCM: a kétfokozatú tizedelés jele és zaja a bevált egyfokozatúéval egyezik; THD+N
//  [3] K8: visszahajlás: a köztes mintavétel közelében lévő hang (pl. 700,6 kHz) nem hajlik vissza a hallható sávba
//  [4] Dekóder: DSD512 / DSD1024 fájl tekerés utáni kimenete bitre egyezik a folyamatos olvasással
//  [5] K10: DSD1024 → PCM sebessége (x86_64: ≥ 3× valós idő, aarch64: ≥ 1,2×)
// A fájlírók, a modulátor és a mérés függetlenek a termékkódtól (a teszt saját példányai).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "audio_decoder.hpp"
#include "dsd.hpp"
#if defined(AQ_TEST_ALSA)
#include "player_output_alsa.hpp"
#endif

using namespace ee_analyzer;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; std::printf("  [HIBA] " __VA_ARGS__); } else { std::printf("  [OK]   " __VA_ARGS__); } std::printf("\n"); } while (0)
static constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- független írók
static void le(std::vector<uint8_t>& o, uint64_t v, int n) { for (int i = 0; i < n; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i))); }
static void be(std::vector<uint8_t>& o, uint64_t v, int n) { for (int i = n - 1; i >= 0; --i) o.push_back(static_cast<uint8_t>(v >> (8 * i))); }
static void tag(std::vector<uint8_t>& o, const char* t) { o.insert(o.end(), t, t + 4); }
static uint8_t rev8(uint8_t b) { uint8_t r = 0; for (int i = 0; i < 8; ++i) if (b & (1 << i)) r |= static_cast<uint8_t>(0x80 >> i); return r; }

/** DSF [Sony DSF v1.01]: csatornánként 4096 bájtos blokkok, LSB-first bájtok, little-endian mezők. */
static void writeDsf(const std::string& path, uint32_t rate, const std::vector<std::vector<uint8_t>>& msb) {
    const uint32_t ch = static_cast<uint32_t>(msb.size()), bs = 4096;
    const size_t perCh = msb[0].size(), blocks = (perCh + bs - 1) / bs;
    const uint64_t dataSize = 12 + static_cast<uint64_t>(blocks) * bs * ch;
    std::vector<uint8_t> o;
    tag(o, "DSD "); le(o, 28, 8); le(o, 28 + 52 + dataSize, 8); le(o, 0, 8);
    tag(o, "fmt "); le(o, 52, 8); le(o, 1, 4); le(o, 0, 4); le(o, ch == 1 ? 1 : 2, 4); le(o, ch, 4); le(o, rate, 4); le(o, 1, 4);
    le(o, static_cast<uint64_t>(perCh) * 8, 8); le(o, bs, 4); le(o, 0, 4);
    tag(o, "data"); le(o, dataSize, 8);
    for (size_t b = 0; b < blocks; ++b)
        for (uint32_t c = 0; c < ch; ++c)
            for (uint32_t i = 0; i < bs; ++i) { const size_t k = b * bs + i; o.push_back(k < perCh ? rev8(msb[c][k]) : 0); }
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(o.data()), static_cast<std::streamsize>(o.size()));
}

/** DSDIFF [Philips v1.5]: big-endian, FRM8 / FVER / PROP(FS, CHNL, CMPR) / DSD, bájtonként váltakozó csatornák, MSB-first. */
static void writeDff(const std::string& path, uint32_t rate, const std::vector<std::vector<uint8_t>>& msb) {
    const uint32_t ch = static_cast<uint32_t>(msb.size());
    const size_t perCh = msb[0].size();
    std::vector<uint8_t> prop;
    tag(prop, "SND ");
    tag(prop, "FS  "); be(prop, 4, 8); be(prop, rate, 4);
    tag(prop, "CHNL"); be(prop, 2 + 4 * ch, 8); be(prop, ch, 2); for (uint32_t c = 0; c < ch; ++c) tag(prop, c ? "SRGT" : "SLFT");
    tag(prop, "CMPR"); be(prop, 4 + 1 + 14, 8); tag(prop, "DSD "); prop.push_back(14);
    const char* nm = "not compressed"; prop.insert(prop.end(), nm, nm + 14); prop.push_back(0);   // páratlan hossz: kitöltő bájt
    std::vector<uint8_t> body;
    tag(body, "DSD ");
    tag(body, "FVER"); be(body, 4, 8); be(body, 0x01050000u, 4);
    tag(body, "PROP"); be(body, prop.size(), 8); body.insert(body.end(), prop.begin(), prop.end());
    tag(body, "DSD "); be(body, static_cast<uint64_t>(perCh) * ch, 8);
    for (size_t k = 0; k < perCh; ++k) for (uint32_t c = 0; c < ch; ++c) body.push_back(msb[c][k]);
    if ((perCh * ch) & 1) body.push_back(0);
    std::vector<uint8_t> o;
    tag(o, "FRM8"); be(o, body.size(), 8); o.insert(o.end(), body.begin(), body.end());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(o.data()), static_cast<std::streamsize>(o.size()));
}

/** Másodrendű szigma-delta modulátor (MSB-first bájtok); amplitúdó ≤ 0,4. */
static std::vector<uint8_t> sdm(double rate, double seconds, double freq, double amp) {
    const size_t bytes = static_cast<size_t>(rate * seconds / 8.0);
    std::vector<uint8_t> o(bytes);
    double i1 = 0, i2 = 0, y = 0, ph = 0;
    const double dph = 2 * kPi * freq / rate;
    for (size_t b = 0; b < bytes; ++b) {
        uint8_t acc = 0;
        for (int k = 0; k < 8; ++k) {
            const double x = amp * std::sin(ph);
            ph += dph;
            if (ph > 2 * kPi) ph -= 2 * kPi;
            i1 += x - y;
            i2 += i1 - y;
            y = i2 >= 0 ? 1.0 : -1.0;
            acc = static_cast<uint8_t>((acc << 1) | (y > 0 ? 1 : 0));
        }
        o[b] = acc;
    }
    return o;
}

// ---------------------------------------------------------------- mérés (FFT, Blackman–Harris)
static void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const std::complex<double> w = std::polar(1.0, -2 * kPi / static_cast<double>(len));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> u(1, 0);
            for (size_t k = 0; k < len / 2; ++k, u *= w) {
                const auto x = a[i + k], y = a[i + k + len / 2] * u;
                a[i + k] = x + y;
                a[i + k + len / 2] = x - y;
            }
        }
    }
}
struct Spectrum { std::vector<double> p; double binHz = 0; };
/** Teljesítményspektrum (egy csatorna, Blackman–Harris ablak, N = 65536). */
static Spectrum spectrum(const std::vector<float>& lr, size_t from, double sr) {
    const size_t n = 65536;
    std::vector<std::complex<double>> a(n);
    double wsum = 0;
    for (size_t i = 0; i < n; ++i) {
        // periodikus ablak: koherens (binre eső) hangnál a szivárgás pontosan ±3 binre korlátozódik
        const double t = 2 * kPi * static_cast<double>(i) / n;
        const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        wsum += w;
        a[i] = { lr[2 * (from + i)] * w, 0 };
    }
    fft(a);
    Spectrum s;
    s.binHz = sr / n;
    s.p.resize(n / 2);
    for (size_t k = 0; k < n / 2; ++k) s.p[k] = std::norm(a[k]) / (wsum * wsum / 4.0);   // szinusz csúcsa: amplitúdó²
    return s;
}
static double bandPower(const Spectrum& s, double f, int halfBins = 6) {
    const long c = std::lround(f / s.binHz);
    double sum = 0;
    for (long k = std::max(1L, c - halfBins); k <= c + halfBins && k < static_cast<long>(s.p.size()); ++k) sum += s.p[k];
    return sum;
}
/** THD+N 20 Hz – 20 kHz (dB, az alaphanghoz képest). */
static double thdn(const Spectrum& s, double f0) {
    const double sig = bandPower(s, f0);
    double noise = 0;
    const long c = std::lround(f0 / s.binHz);
    for (long k = std::lround(20.0 / s.binHz); k <= std::lround(20000.0 / s.binHz); ++k)
        if (std::abs(k - c) > 6) noise += s.p[k];
    return 10 * std::log10(noise / sig);
}

static std::vector<float> convert(uint32_t dsdRate, uint32_t pcmRate, const std::vector<uint8_t>& monoMsb, bool twoStage, bool* usedTwo = nullptr) {
    dsd::DsdToPcmConverter c;
    c.init(dsdRate, pcmRate, 2, twoStage);
    if (usedTwo) *usedTwo = c.twoStage();
    const size_t frames = monoMsb.size() / c.bytesPerPcmFrame();
    std::vector<uint8_t> inter(monoMsb.size() * 2);
    for (size_t i = 0; i < monoMsb.size(); ++i) inter[2 * i] = inter[2 * i + 1] = monoMsb[i];
    std::vector<float> out(frames * 2);
    c.process(inter.data(), frames, out.data());
    return out;
}

// ---------------------------------------------------------------- [1] K3
static void testFiles() {
    std::printf("[1] K3: DSF és DFF DSD64–DSD1024, 44,1 és 48 kHz család\n");
    for (uint32_t base : { 2822400u, 3072000u }) {
        for (uint32_t mult : { 1u, 2u, 4u, 8u, 16u }) {
            const uint32_t rate = base * mult;
            std::mt19937 rng(rate);
            const size_t perCh = static_cast<size_t>(rate / 8 / 20) + 777;   // ~50 ms, nem kerek blokkszám
            std::vector<std::vector<uint8_t>> msb(2, std::vector<uint8_t>(perCh));
            for (auto& c : msb) for (auto& b : c) b = static_cast<uint8_t>(rng());
            for (const char* kind : { "dsf", "dff" }) {
                const std::string path = (fs::temp_directory_path() / ("aq_b_" + std::to_string(rate) + "." + kind)).string();
                if (std::string(kind) == "dsf") writeDsf(path, rate, msb); else writeDff(path, rate, msb);
                std::string err;
                auto raw = dsd::openDsdRawFile(path, &err);
                bool ok = raw && raw->info().dsdSampleRate == rate && raw->info().channels == 2 &&
                          raw->info().dopSampleRate == rate / 16 && raw->totalChannelBytes() == perCh;
                size_t diff = 0;
                if (ok) {
                    std::vector<uint8_t> buf(perCh * 2);
                    ok = raw->read(buf.data(), perCh) == perCh;
                    for (size_t i = 0; i < perCh; ++i) diff += (buf[2 * i] != msb[0][i]) + (buf[2 * i + 1] != msb[1][i]);
                    const uint64_t mid = perCh / 2 + 3;
                    uint8_t two[2] = {};
                    ok = ok && raw->seekByte(mid) && raw->read(two, 1) == 1 && two[0] == msb[0][mid] && two[1] == msb[1][mid];
                }
                raw.reset();
                auto dec = player::openDecoder(path, &err);
                const uint32_t pcm = (base == 2822400u) ? 88200u : 96000u;
                const uint64_t frames = static_cast<uint64_t>(perCh) * 8 / (rate / pcm);
                const bool decOk = dec && dec->sampleRate() == pcm && dec->channels() == 2 && dec->totalFrames() == frames;
                dec.reset();
                CHECK(ok && diff == 0 && decOk, "%s DSD%u (%u Hz): fejléc, %zu bájt / csatorna bitre (%zu eltérés), tekerés, PCM %u Hz, %llu keret (%s)",
                      kind, 64 * mult, rate, perCh, diff, pcm, static_cast<unsigned long long>(frames), err.c_str());
                std::error_code ec;
                fs::remove(path, ec);
            }
        }
    }
}

// ---------------------------------------------------------------- [2] K8 jel és zaj
static void testQuality() {
    std::printf("[2] K8: DSD512 / DSD1024 → PCM: kétfokozatú a bevált egyfokozatúhoz mérve\n");
    for (uint32_t base : { 2822400u, 3072000u }) {
        for (uint32_t mult : { 8u, 16u }) {
            const uint32_t rate = base * mult, pcm = base == 2822400u ? 88200u : 96000u;
            const double f0 = std::round(1000.0 / (pcm / 65536.0)) * (pcm / 65536.0);   // ~1 kHz, pontosan egy FFT-binen
            const auto bits = sdm(rate, 0.85, f0, 0.4);
            bool two = false;
            const auto a = convert(rate, pcm, bits, true, &two);
            const auto b = convert(rate, pcm, bits, false);
            const size_t from = static_cast<size_t>(pcm * 0.05);
            const Spectrum sa = spectrum(a, from, pcm), sb = spectrum(b, from, pcm);
            const double la = 10 * std::log10(bandPower(sa, f0)), lb = 10 * std::log10(bandPower(sb, f0));
            const double na = thdn(sa, f0), nb = thdn(sb, f0);
            CHECK(two && std::abs(la - lb) < 0.01, "DSD%u (%u Hz): kétfokozatú út, a hang szintje %.3f / %.3f dB (eltérés %.4f)", 64 * mult, rate, la, lb, la - lb);
            // a tűrés 0,5 dB: a kétfokozatú út a mérés szerint 0,1 dB-en belül van; egyszeres pontosságú B-fokozattal 1,5–1,8 dB-lel rosszabb volt
            CHECK(na <= -100.0 && na <= nb + 0.5, "DSD%u (%u Hz): THD+N %.1f dB (egyfokozatú: %.1f dB; elvárt ≤ −100, és legfeljebb 0,5 dB-lel rosszabb)",
                  64 * mult, rate, na, nb);
        }
    }
}

// ---------------------------------------------------------------- [3] K8 visszahajlás
static void testAliasing() {
    std::printf("[3] K8: a köztes mintavétel közelében lévő hang nem hajlik vissza\n");
    for (uint32_t base : { 2822400u, 3072000u }) {
        for (uint32_t mult : { 8u, 16u }) {
            const uint32_t rate = base * mult, pcm = base == 2822400u ? 88200u : 96000u;
            const double ri = 8.0 * pcm, fa = ri - 5000.0;           // az A-fokozat után 5 kHz-re hajlana vissza
            const auto bits = sdm(rate, 0.85, fa, 0.4);
            const auto a = convert(rate, pcm, bits, true);
            const Spectrum s = spectrum(a, static_cast<size_t>(pcm * 0.05), pcm);
            const double rel = 10 * std::log10(bandPower(s, 5000.0) / (0.8 * 0.8));   // a 0,4-es DSD-hang PCM-ben 0,8 lenne
            CHECK(rel <= -100.0, "DSD%u: %.1f kHz-es hang visszahajlása 5 kHz-re %.1f dB (elvárt ≤ −100)", 64 * mult, fa / 1000.0, rel);
        }
    }
}

// ---------------------------------------------------------------- [4] tekerés
static void testSeek() {
    std::printf("[4] Dekóder: tekerés utáni kimenet bitre a folyamatos olvasással (DSD512 / DSD1024)\n");
    for (uint32_t mult : { 8u, 16u }) {
        const uint32_t rate = 2822400u * mult;
        std::vector<std::vector<uint8_t>> msb = { sdm(rate, 0.3, 1000.0, 0.3), sdm(rate, 0.3, 3000.0, 0.3) };
        const std::string path = (fs::temp_directory_path() / ("aq_b_seek_" + std::to_string(rate) + ".dsf")).string();
        writeDsf(path, rate, msb);
        std::string err;
        auto d = player::openDecoder(path, &err);
        bool ok = d != nullptr;
        double worst = 0;
        if (d) {
            std::vector<float> all(d->totalFrames() * 2);
            const size_t got = d->read(all.data(), d->totalFrames());
            const uint64_t at = d->totalFrames() / 2 + 13;
            std::vector<float> part(1024 * 2);
            ok = got == d->totalFrames() && d->seek(at) && d->read(part.data(), 1024) == 1024;
            for (size_t i = 0; ok && i < part.size(); ++i) worst = std::max(worst, std::abs(double(part[i]) - all[at * 2 + i]));
        }
        d.reset();
        CHECK(ok && worst == 0.0, "DSD%u: tekerés után 1024 keret, legnagyobb eltérés %.3g (%s)", 64 * mult, worst, err.c_str());
        std::error_code ec;
        fs::remove(path, ec);
    }
}

// ---------------------------------------------------------------- [5] K10 sebesség
static void testSpeed() {
    std::printf("[5] K10: DSD1024 → PCM sebessége\n");
    const uint32_t rate = 45158400u, pcm = 88200u;
    dsd::DsdToPcmConverter c;
    c.init(rate, pcm, 2);
    const double seconds = 2.0;
    const size_t frames = static_cast<size_t>(pcm * seconds);
    std::vector<uint8_t> in(frames * c.bytesPerPcmFrame() * 2);
    std::mt19937 r(7);
    for (auto& b : in) b = static_cast<uint8_t>(r());
    std::vector<float> out(frames * 2);
    const auto t0 = std::chrono::steady_clock::now();
    c.process(in.data(), frames, out.data());
    const double x = seconds / std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
#if defined(__aarch64__) || defined(_M_ARM64) || (defined(__APPLE__) && defined(__x86_64__))
    const double need = 1.2;   // ARM (Pi-osztály), illetve macOS-en az x86_64-es változat Rosettán (emulálva) fut
#else
    const double need = 3.0;
#endif
    std::printf("K_METRIC: {\"test\":\"K10_dsd1024\",\"realtime\":%.2f}\n", x);
    CHECK(x >= need, "DSD1024 → 88,2 kHz: %.1f× valós idő (elvárt ≥ %.1f×)", x, need);
}

// ---------------------------------------------------------------- [6] K7 natív DSD-csomagolás (Linux, ALSA)
#if defined(AQ_TEST_ALSA)
static void testNativePacking() {
    std::printf("[6] K7: natív ALSA DSD-keretek a DoP-szavakból (minden formátum, több periódus, U8 páratlan periódussal)\n");
    for (const auto& fmt : player::kAlsaDsdFormats) {
        // független forrás: a mintaméret és a bájtsorrend az ALSA saját formátumleírásából (nem a mi táblázatunkból)
        const int alsaBytes = snd_pcm_format_physical_width(fmt.fmt) / 8;
        const bool alsaBe = alsaBytes == 1 || snd_pcm_format_big_endian(fmt.fmt) == 1;
        CHECK(fmt.bytes == alsaBytes && fmt.bigEndian == alsaBe, "%s: a táblázat mintamérete %d bájt, bájtsorrendje %s (ALSA: %d bájt, %s)", fmt.name,
              fmt.bytes, fmt.bigEndian ? "BE" : "LE", alsaBytes, alsaBe ? "BE" : "LE");
        std::mt19937 rng(static_cast<uint32_t>(fmt.fmt) * 31u + 7u);
        const std::vector<size_t> periods = { 37, 64, 1, 255, 128, 3 };   // páratlan és páros, változó hosszú periódusok
        size_t totalFrames = 0;
        for (size_t p : periods) totalFrames += p;
        const size_t perCh = totalFrames * static_cast<size_t>(fmt.bytes) + 2;
        // a forrás DSD-bájtfolyam csatornánként (időrendben, MSB-first); a DoP-szavak a szabvány szerint
        std::vector<uint8_t> L(perCh), R(perCh);
        for (auto& b : L) b = static_cast<uint8_t>(rng());
        for (auto& b : R) b = static_cast<uint8_t>(rng());
        player::NativeDsdPacker pk;
        pk.setup(fmt.bytes, fmt.bigEndian, 512);
        std::vector<uint8_t> got;
        size_t used = 0;   // ennyi DSD-bájt / csatorna ment már DoP-szóként a csomagolónak
        for (size_t p : periods) {
            const size_t n = pk.dopFramesFor(p);
            std::vector<int32_t> dop(n * 2);
            for (size_t i = 0; i < n; ++i) {
                const uint8_t marker = ((used / 2 + i) & 1) ? 0xFA : 0x05;
                dop[2 * i] = static_cast<int32_t>((uint32_t(marker) << 16) | (uint32_t(L[used + 2 * i]) << 8) | L[used + 2 * i + 1]);
                dop[2 * i + 1] = static_cast<int32_t>((uint32_t(marker) << 16) | (uint32_t(R[used + 2 * i]) << 8) | R[used + 2 * i + 1]);
            }
            used += 2 * n;
            std::vector<uint8_t> out(p * 2 * static_cast<size_t>(fmt.bytes));
            pk.pack(dop.data(), p, out.data());
            got.insert(got.end(), out.begin(), out.end());
        }
        // független elvárás: a k. ALSA-keret c. mintája a csatorna [k·b, k·b + b) bájtjai, _LE-nél fordított sorrendben
        size_t diff = 0;
        for (size_t k = 0; k < totalFrames && got.size() == totalFrames * 2 * static_cast<size_t>(alsaBytes); ++k)
            for (int c = 0; c < 2; ++c)
                for (int q = 0; q < alsaBytes; ++q) {
                    const uint8_t want = (c ? R : L)[k * alsaBytes + q];
                    const size_t pos = (k * 2 + c) * alsaBytes + (alsaBe ? q : alsaBytes - 1 - q);
                    if (got[pos] != want) ++diff;
                }
        CHECK(diff == 0 && got.size() == totalFrames * 2 * static_cast<size_t>(alsaBytes),
              "%s: %zu keret, %zu eltérő bájt (6 periódus, a bitfolyam folytonos)", fmt.name, totalFrames, diff);
    }
}

/** [7] Végponttól végpontig: DSD512-es fájl → lejátszó → ALSA „null” eszköz, natív és DoP módban; a kimeneti bájtok. */
static void testAlsaEndToEnd() {
    std::printf("[7] K7: DSD512 fájl a lejátszón és az ALSA-kimeneten át (null eszköz): natív és DoP\n");
    const uint32_t rate = 22579200u;
    std::mt19937 rng(512);
    const size_t perCh = rate / 8 / 5;   // 0,2 s
    std::vector<std::vector<uint8_t>> msb(2, std::vector<uint8_t>(perCh));
    for (auto& c : msb) for (auto& b : c) b = static_cast<uint8_t>(rng());
    const std::string path = (fs::temp_directory_path() / "aq_b_e2e_512.dsf").string();
    writeDsf(path, rate, msb);
    for (bool native : { true, false }) {
        auto out = std::make_unique<player::AlsaPlayerOutput>("null");
        player::AlsaPlayerOutput* o = out.get();
        o->setPreferNativeDsd(native);
        std::mutex m;
        std::vector<uint8_t> raw;
        int fmtBytes = 0;
        o->setRawTap([&](const uint8_t* p, size_t n, const player::AlsaFormat& f) {
            std::lock_guard<std::mutex> lk(m);
            raw.insert(raw.end(), p, p + n);
            fmtBytes = f.bits / 8;
        });
        player::FilePlayer fp(std::move(out));
        fp.setDsdDop(true);
        std::string err;
        const bool opened = fp.open(path, &err);
        // a lejátszó a DAC beállásáig 0,8 s csenddel indul (kRateSettleMs): a csend és a fájl eleje is beférjen;
        // DoP-ban egy DSD-bájt / csatorna 4 kimeneti bájt, natívban 2 – a határ mindkettőre bőven elég
        for (int i = 0; i < 5000 && fp.state() != player::FilePlayer::State::Ended; ++i) {
            { std::lock_guard<std::mutex> lk(m); if (raw.size() > 24 * perCh) break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const bool isNative = o->isNativeDsd();
        const std::string nativeName = o->nativeDsdFormat();
        fp.stop();
        std::lock_guard<std::mutex> lk(m);
        // a „null” eszköz az első natív formátumot (DSD_U32_BE) fogadja: 4 bájt / minta az ALSA szerint
        const int u32Bytes = snd_pcm_format_physical_width(SND_PCM_FORMAT_DSD_U32_BE) / 8;
        if (isNative) CHECK(nativeName == "DSD_U32_BE" && fmtBytes == u32Bytes, "natív formátum: %s, %d bájt / minta (ALSA: %d)",
                            nativeName.c_str(), fmtBytes, u32Bytes);
        if (isNative) fmtBytes = u32Bytes;
        // a forrás DSD-bitfolyamának első 4096 bájtja / csatorna megtalálható-e a kimenetben (a csend után)
        std::vector<uint8_t> chL, chR;
        if (isNative) {
            for (size_t k = 0; k + 2 * fmtBytes <= raw.size(); k += 2 * fmtBytes)
                for (int q = 0; q < fmtBytes; ++q) { chL.push_back(raw[k + q]); chR.push_back(raw[k + fmtBytes + q]); }   // U32_BE
        } else {
            for (size_t k = 0; k + 8 <= raw.size(); k += 8) {   // S32_LE DoP: [x, b1, b0, marker]
                chL.push_back(raw[k + 2]); chL.push_back(raw[k + 1]);
                chR.push_back(raw[k + 6]); chR.push_back(raw[k + 5]);
            }
        }
        auto it = std::search(chL.begin(), chL.end(), msb[0].begin(), msb[0].begin() + 4096);
        const bool foundL = it != chL.end();
        const size_t off = foundL ? static_cast<size_t>(it - chL.begin()) : 0;
        const bool rightOk = foundL && chR.size() >= off + 4096 && std::equal(msb[1].begin(), msb[1].begin() + 4096, chR.begin() + off);
        CHECK(opened && isNative == native && foundL && rightOk,
              "%s: megnyitva (%s), a kimenet %s, a forrás bitfolyama bitre benne van (bal / jobb, eltolás %zu bájt)",
              native ? "natív" : "DoP", err.c_str(), isNative ? "natív DSD_U32_BE" : "DoP 24/32", off);
    }
    std::error_code ec;
    fs::remove(path, ec);
}
#endif

int main() {
    std::printf("Aqulify – nagy felbontás, B rész (DSD512 / DSD1024)\n");
    testFiles();
    testQuality();
    testAliasing();
    testSeek();
    testSpeed();
#if defined(AQ_TEST_ALSA)
    testNativePacking();
    testAlsaEndToEnd();
#endif
    std::printf(g_fail ? "\nHIBA: %d ellenőrzés nem sikerült\n" : "\nMINDEN TESZT SIKERES (0 hiba)\n", g_fail);
    return g_fail ? 1 : 0;
}
