#pragma once
/**
 * @file src_convert.hpp
 * @brief Fájlból fájlba mintavétel-átalakítás a beépített lejátszó kódútján (mérésekhez, pl. független SRC-összehasonlítás).
 *
 * Ugyanaz történik, mint lejátszáskor (file_player.cpp, a dekóder-szál):
 *  - a fájlt az openDecoder() bontja ki 32 bites lebegőpontos mintákra, 4096 keretes darabokban;
 *  - a lejátszó sztereóra keveri (mono: bal = jobb), mert a felmintavételező mindig sztereó;
 *  - az UpsampleEngine (libsoxr) a lejátszó beállításaival (AudioOptions) alakít, a fejtérrel együtt;
 *  - a fájl végén a szűrőt lépésenként üríti (flush()), amíg a libsoxr ad mintát, ugyanúgy, mint a lejátszó.
 * A kimenet 32 bites lebegőpontos WAV, a forrás csatornaszámával (mono forrásnál a bal csatorna).
 * Nincs külön, „versenyre hangolt” út: amit ez kiad, azt adja a lejátszó is.
 */
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include <soxr.h>

#include "audio_decoder.hpp"
#include "audio_options.hpp"
#include "upsample_engine.hpp"

namespace ee_analyzer::srcconv {

/** A lejátszó dekóder-szálának darabmérete és a szűrő kiürítésének lépése (file_player.cpp kChunkFrames, kFlushChunkFrames). */
inline constexpr size_t kChunkFrames = 4096;
inline constexpr size_t kFlushFrames = kChunkFrames * 4;
/** A kiürítés lépéseinek felső korlátja (végtelen ciklus ellen; 16384 × 4096 keret bőven elég). */
inline constexpr int kMaxFlushSteps = 4096;

struct Result {
    uint32_t srcRate = 0, dstRate = 0, channels = 0;
    uint32_t outBits = 32;          // kimeneti bitmélység (32: lejátszó mód, 64: fájlátalakító mód)
    uint64_t inFrames = 0, outFrames = 0;
    uint64_t flushFrames = 0;       // a szám végén a szűrőből kiürített keretek
    int flushSteps = 0;             // ennyi flush()-hívás adott mintát (a lejátszó ugyanígy, lépésenként üríti)
    uint64_t leftoverFrames = 0;    // a lépéskorlát után is maradt (nem fordulhat elő; 0 a jó)
    uint64_t clipped = 0;           // a fejtér utáni ±1 fölötti minták
    bool monoMismatch = false;      // mono forrásnál a bal és a jobb kimenet eltért (nem fordulhat elő)
};

/** 32 bites lebegőpontos WAV írása (WAVE_FORMAT_IEEE_FLOAT, fact blokkal). */
inline bool writeFloatWav(const std::string& path, const std::vector<float>& interleaved, uint32_t channels, uint32_t rate,
                          std::string* err = nullptr) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { if (err) *err = "nem írható: " + path; return false; }
    const uint64_t dataBytes64 = static_cast<uint64_t>(interleaved.size()) * 4u;
    if (dataBytes64 > 0xFFFFFF00ull) { std::fclose(f); if (err) *err = "túl nagy a kimenet a WAV-hoz"; return false; }
    const uint32_t dataBytes = static_cast<uint32_t>(dataBytes64);
    const uint32_t frames = channels ? static_cast<uint32_t>(interleaved.size() / channels) : 0;
    auto u32 = [&](uint32_t v) { uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; std::fwrite(b, 1, 4, f); };
    auto u16 = [&](uint16_t v) { uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; std::fwrite(b, 1, 2, f); };
    std::fwrite("RIFF", 1, 4, f); u32(4 + (8 + 18) + (8 + 4) + (8 + dataBytes));
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(18);
    u16(3); u16(static_cast<uint16_t>(channels)); u32(rate); u32(rate * channels * 4); u16(static_cast<uint16_t>(channels * 4)); u16(32); u16(0);
    std::fwrite("fact", 1, 4, f); u32(4); u32(frames);
    std::fwrite("data", 1, 4, f); u32(dataBytes);
    // a minták kis-endián IEEE float formában (minden támogatott gépen ez a natív forma)
    const bool ok = std::fwrite(interleaved.data(), 4, interleaved.size(), f) == interleaved.size();
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed) { if (err) *err = "írási hiba: " + path; return false; }
    return true;
}

/**
 * A `in` fájl átalakítása `dstRate`-re a lejátszó kódútján; a kimenet a memóriába (`out`, interleaved, a forrás
 * csatornaszámával, de legfeljebb 2). Több mint kétcsatornás forrásnál a lejátszó sztereó keverését adja.
 */
inline bool convert(const std::string& in, uint32_t dstRate, const audioopt::AudioOptions& optsIn, std::vector<float>& out,
                    Result& res, std::string* err = nullptr) {
    using player::AudioDecoder;
    std::string e;
    std::unique_ptr<AudioDecoder> dec = player::openDecoder(in, &e);
    if (!dec) { if (err) *err = "nem nyitható: " + in + (e.empty() ? "" : " (" + e + ")"); return false; }
    const uint32_t srcCh = dec->channels(), srcRate = dec->sampleRate();
    if (srcCh == 0 || srcRate == 0) { if (err) *err = "érvénytelen fájl: " + in; return false; }
    audioopt::AudioOptions opts = optsIn;
    audioopt::sanitize(opts);

    UpsampleEngine ups;
    ups.setThreads(1);
    if (!ups.initWithOptions(srcRate, dstRate, 2, opts)) { if (err) *err = "a libsoxr nem indult"; return false; }

    res = Result{};
    res.srcRate = srcRate; res.dstRate = dstRate; res.channels = srcCh == 1 ? 1u : 2u;
    std::vector<float> decodeBuf(kChunkFrames * srcCh), stereo(kChunkFrames * 2), stage;
    std::vector<float> all;   // sztereó kimenet
    const double ratio = static_cast<double>(dstRate) / srcRate;
    for (;;) {
        const size_t got = dec->read(decodeBuf.data(), kChunkFrames);
        if (got == 0) break;
        res.inFrames += got;
        const float* p = decodeBuf.data();
        for (size_t i = 0; i < got; ++i, p += srcCh) {   // ugyanaz a keverés, mint a file_player.cpp-ben
            float l, r;
            if (srcCh == 1) l = r = p[0];
            else if (srcCh == 2) { l = p[0]; r = p[1]; }
            else {
                constexpr float k = 0.70710678f;
                l = p[0]; r = p[1];
                if (srcCh >= 3) { l += k * p[2]; r += k * p[2]; }
                if (srcCh >= 6) { l += k * p[4]; r += k * p[5]; }
                else if (srcCh == 5) { l += k * p[3]; r += k * p[4]; }
            }
            stereo[2 * i] = l; stereo[2 * i + 1] = r;
        }
        size_t outFrames = 0;
        if (ups.isActive()) {
            const size_t needed = static_cast<size_t>(std::ceil(got * ratio)) + 256;
            if (stage.size() < needed * 2) stage.resize(needed * 2);
            ups.process(stereo.data(), got, stage.data(), stage.size() / 2, outFrames);
            all.insert(all.end(), stage.begin(), stage.begin() + static_cast<std::ptrdiff_t>(outFrames * 2));
        } else {
            all.insert(all.end(), stereo.begin(), stereo.begin() + static_cast<std::ptrdiff_t>(got * 2));
        }
    }
    if (ups.isActive()) {
        if (stage.size() < kFlushFrames * 2) stage.resize(kFlushFrames * 2);
        for (int step = 0;; ++step) {   // mint a lejátszó: lépésenként, amíg a libsoxr ad mintát
            size_t fl = 0;
            ups.flush(stage.data(), kFlushFrames, fl);
            if (fl == 0) break;
            if (step >= kMaxFlushSteps) { res.leftoverFrames += fl; break; }
            res.flushFrames += fl;
            ++res.flushSteps;
            all.insert(all.end(), stage.begin(), stage.begin() + static_cast<std::ptrdiff_t>(fl * 2));
        }
        res.clipped = ups.clippedSamples();
    }
    const size_t frames = all.size() / 2;
    res.outFrames = frames;
    if (res.channels == 1) {
        out.resize(frames);
        for (size_t i = 0; i < frames; ++i) {
            out[i] = all[2 * i];
            if (std::memcmp(&all[2 * i], &all[2 * i + 1], sizeof(float)) != 0) res.monoMismatch = true;
        }
    } else {
        out.swap(all);
    }
    return true;
}

/** 64 bites lebegőpontos WAV beolvasásának eredménye (interleaved double minták). */
struct WavDoubleData {
    std::vector<double> samples;
    uint32_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t srcBits = 0;
    uint16_t srcFmtTag = 0;  // 1 = PCM, 3 = IEEE float (EXTENSIBLE esetén is feloldva)
};

/** 64 bites pozícionálás (Linux/macOS: fseeko/ftello, Windows: _fseeki64/_ftelli64). */
inline bool seek64(FILE* f, int64_t off, int origin) {
#ifdef _WIN32
    return _fseeki64(f, off, origin) == 0;
#else
    return fseeko(f, static_cast<off_t>(off), origin) == 0;
#endif
}

inline int64_t tell64(FILE* f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return static_cast<int64_t>(ftello(f));
#endif
}

/**
 * WAV fájl közvetlen beolvasása 64 bites lebegőpontos (double) tömbbe.
 * Támogatja: 16/24/32 bites egész PCM, 32/64 bites IEEE float és WAVE_FORMAT_EXTENSIBLE (PCM/IEEE float).
 */
inline bool readWavDouble(const std::string& path, WavDoubleData& out, std::string* err = nullptr) {
    out = WavDoubleData{};
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { if (err) *err = "nem nyitható: " + path; return false; }

    auto fail = [&](const std::string& msg) {
        std::fclose(f);
        if (err) *err = msg;
        return false;
    };

    if (!seek64(f, 0, SEEK_END)) return fail("fájlméret nem olvasható: " + path);
    const int64_t fileSize = tell64(f);
    if (fileSize < 44 || !seek64(f, 0, SEEK_SET)) return fail("érvénytelen vagy túl rövid WAV: " + path);

    uint8_t riff[12];
    if (std::fread(riff, 1, 12, f) != 12) return fail("WAV fejléc olvasási hiba: " + path);
    if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0)
        return fail("nem RIFF/WAVE fájl: " + path);

    auto u16le = [](const uint8_t* p) -> uint16_t {
        return static_cast<uint16_t>(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
    };
    auto u32le = [](const uint8_t* p) -> uint32_t {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    };

    bool hasFmt = false, hasData = false;
    uint16_t fmtTag = 0, channels = 0, blockAlign = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0;
    int64_t dataOffset = 0;
    uint64_t dataSize = 0;

    static constexpr uint8_t kGuidTail[14] = {
        0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
    };

    int64_t pos = 12;
    while (pos + 8 <= fileSize) {
        if (!seek64(f, pos, SEEK_SET)) return fail("chunk pozícionálási hiba: " + path);
        uint8_t hdr[8];
        if (std::fread(hdr, 1, 8, f) != 8) return fail("chunk fejléc olvasási hiba: " + path);
        const uint32_t sz = u32le(hdr + 4);
        const int64_t bodyPos = pos + 8;
        if (static_cast<int64_t>(sz) > fileSize - bodyPos) return fail("csonka WAV chunk: " + path);

        if (std::memcmp(hdr, "fmt ", 4) == 0) {
            if (sz < 16) return fail("túl rövid fmt chunk: " + path);
            const size_t readLen = std::min<size_t>(sz, 64);
            uint8_t fb[64] = {};
            if (std::fread(fb, 1, readLen, f) != readLen) return fail("fmt olvasási hiba: " + path);
            fmtTag = u16le(fb + 0);
            channels = u16le(fb + 2);
            sampleRate = u32le(fb + 4);
            blockAlign = u16le(fb + 12);
            bitsPerSample = u16le(fb + 14);
            if (fmtTag == 0xFFFE) {
                if (sz < 40) return fail("csonka WAVE_FORMAT_EXTENSIBLE fmt chunk: " + path);
                const uint16_t validBits = u16le(fb + 18);
                if (validBits == 0 || validBits > bitsPerSample)
                    return fail("érvénytelen validBitsPerSample: " + path);
                if (std::memcmp(fb + 26, kGuidTail, 14) != 0)
                    return fail("nem támogatott WAVE_FORMAT_EXTENSIBLE SubFormat GUID: " + path);
                fmtTag = u16le(fb + 24);
            }
            hasFmt = true;
        } else if (std::memcmp(hdr, "data", 4) == 0) {
            dataOffset = bodyPos;
            dataSize = sz;
            hasData = true;
        }
        pos = bodyPos + static_cast<int64_t>(sz) + static_cast<int64_t>(sz & 1u);
    }

    if (!hasFmt || !hasData) return fail("hiányzó fmt vagy data chunk: " + path);
    if (channels == 0 || channels > 8 || sampleRate < 1000 || sampleRate > 6144000)
        return fail("érvénytelen csatornaszám vagy mintavétel: " + path);

    const bool validPcm = (fmtTag == 1) && (bitsPerSample == 16 || bitsPerSample == 24 || bitsPerSample == 32);
    const bool validFlt = (fmtTag == 3) && (bitsPerSample == 32 || bitsPerSample == 64);
    if (!validPcm && !validFlt) return fail("nem támogatott WAV mintaformátum: " + path);

    const uint32_t bytesPerSample = bitsPerSample / 8u;
    const uint32_t expectedAlign = channels * bytesPerSample;
    if (blockAlign != expectedAlign) return fail("eltérő blockAlign a WAV fejlécben: " + path);
    if (dataSize % expectedAlign != 0) return fail("a data chunk mérete nem egész számú keret: " + path);

    const uint64_t totalFrames = dataSize / expectedAlign;
    const uint64_t totalSamples = totalFrames * channels;
    out.samples.resize(static_cast<size_t>(totalSamples));
    out.channels = channels;
    out.sampleRate = sampleRate;
    out.srcBits = bitsPerSample;
    out.srcFmtTag = fmtTag;

    if (!seek64(f, dataOffset, SEEK_SET)) return fail("data chunk pozícionálási hiba: " + path);

    constexpr size_t kReadBlockSamples = 65536;
    std::vector<uint8_t> raw(kReadBlockSamples * bytesPerSample);
    size_t doneSamples = 0;
    while (doneSamples < totalSamples) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(kReadBlockSamples, totalSamples - doneSamples));
        const size_t wantBytes = want * bytesPerSample;
        if (std::fread(raw.data(), 1, wantBytes, f) != wantBytes) return fail("WAV mintaolvasási hiba: " + path);
        double* dst = out.samples.data() + doneSamples;
        const uint8_t* p = raw.data();
        if (fmtTag == 3 && bitsPerSample == 64) {
            std::memcpy(dst, p, wantBytes);
        } else if (fmtTag == 3 && bitsPerSample == 32) {
            for (size_t i = 0; i < want; ++i, p += 4) {
                float v;
                std::memcpy(&v, p, 4);
                dst[i] = static_cast<double>(v);
            }
        } else if (fmtTag == 1 && bitsPerSample == 16) {
            for (size_t i = 0; i < want; ++i, p += 2) {
                const int16_t v = static_cast<int16_t>(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
                dst[i] = static_cast<double>(v) / 32768.0;
            }
        } else if (fmtTag == 1 && bitsPerSample == 24) {
            for (size_t i = 0; i < want; ++i, p += 3) {
                int32_t v = static_cast<int32_t>(p[0]) | (static_cast<int32_t>(p[1]) << 8) |
                            (static_cast<int32_t>(p[2]) << 16);
                if (v & 0x00800000) v |= static_cast<int32_t>(0xFF000000u);
                dst[i] = static_cast<double>(v) / 8388608.0;
            }
        } else if (fmtTag == 1 && bitsPerSample == 32) {
            for (size_t i = 0; i < want; ++i, p += 4) {
                const int32_t v = static_cast<int32_t>(u32le(p));
                dst[i] = static_cast<double>(v) / 2147483648.0;
            }
        }
        doneSamples += want;
    }

    std::fclose(f);
    return true;
}

/** 64 bites lebegőpontos WAV írása (WAVE_FORMAT_IEEE_FLOAT, fact blokkal). */
inline bool writeDoubleWav(const std::string& path, const std::vector<double>& interleaved, uint32_t channels, uint32_t rate,
                           std::string* err = nullptr) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { if (err) *err = "nem írható: " + path; return false; }
    const uint64_t dataBytes64 = static_cast<uint64_t>(interleaved.size()) * 8u;
    if (dataBytes64 > 0xFFFFFF00ull) { std::fclose(f); if (err) *err = "túl nagy a kimenet a WAV-hoz"; return false; }
    const uint32_t dataBytes = static_cast<uint32_t>(dataBytes64);
    const uint32_t frames = channels ? static_cast<uint32_t>(interleaved.size() / channels) : 0;
    auto u32 = [&](uint32_t v) { uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; std::fwrite(b, 1, 4, f); };
    auto u16 = [&](uint16_t v) { uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; std::fwrite(b, 1, 2, f); };
    std::fwrite("RIFF", 1, 4, f); u32(4 + (8 + 18) + (8 + 4) + (8 + dataBytes));
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(18);
    u16(3); u16(static_cast<uint16_t>(channels)); u32(rate); u32(rate * channels * 8u); u16(static_cast<uint16_t>(channels * 8u)); u16(64); u16(0);
    std::fwrite("fact", 1, 4, f); u32(4); u32(frames);
    std::fwrite("data", 1, 4, f); u32(dataBytes);
    // a minták kis-endián IEEE 754 double formában (minden támogatott architektúrán natív)
    const bool ok = std::fwrite(interleaved.data(), 8, interleaved.size(), f) == interleaved.size();
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed) { if (err) *err = "írási hiba: " + path; return false; }
    return true;
}

/** Szélkezelési mód a fájlátalakítóban (--mode file): a fájl elején és végén lévő lépés (Gibbs-hullámzás) ellen. */
enum class EdgeMode {
    None,  // nincs kiterjesztés (nyers libsoxr)
    Odd,   // páratlan tükrözés (2*x[szél] - x[szél-k]), ablakolt lecsengéssel
    Lpc,   // Burg-módszerű lineáris előrejelzéses (AR) kiterjesztés, ablakolt lecsengéssel
};

/** Fájlátalakító mód (64 bites lebegőpontos út) kiegészítő beállításai. */
struct FileOptions {
    double precision = 0.0;      // 0 = az AudioOptions pontossága (pl. 32 bit); [15..33] között felülírható
    double passbandEnd = 0.0;    // 0 = az AudioOptions sávszélessége; (0.5..0.997] között felülírható
    double stopbandBegin = 0.0;  // 0 = az AudioOptions záró sávja; [1.0..1.5] között felülírható
    double phase = -1.0;         // <0 = az AudioOptions fázisa; [0..100] között felülírható
    EdgeMode edge = EdgeMode::None;
};

namespace detail {

/** Burg-módszer: garantáltan stabil AR(p) együtthatók számítása (a[0..p-1], ahol x[n] ≈ -sum a[k]*x[n-1-k]). */
inline std::vector<double> burgLpc(const std::vector<double>& x, size_t maxOrder) {
    const size_t n = x.size();
    const size_t order = std::min(maxOrder, n > 1 ? n - 1 : 0);
    if (order == 0) return {};

    double e0 = 0.0;
    for (double v : x) e0 += v * v;
    if (e0 <= 1e-60) return {};

    std::vector<double> ef = x, eb = x;
    std::vector<double> a(order + 1, 0.0), aNext(order + 1, 0.0);
    a[0] = 1.0;
    size_t usedOrder = 0;

    for (size_t m = 1; m <= order; ++m) {
        double num = 0.0, den = 0.0;
        for (size_t i = m; i < n; ++i) {
            const double f = ef[i], b = eb[i - 1];
            num += f * b;
            den += f * f + b * b;
        }
        // Ha a maradékenergia már a jelenergia 1e-24-e alá esett (egzakt harmonikus jel), ne illesszünk zajra
        if (den <= e0 * 1e-24 || den <= 1e-60) break;
        double k = -2.0 * num / den;
        k = std::clamp(k, -0.999999, 0.999999);

        aNext[0] = 1.0;
        for (size_t j = 1; j < m; ++j) aNext[j] = a[j] + k * a[m - j];
        aNext[m] = k;
        a.swap(aNext);
        usedOrder = m;

        // Visszafelé frissítés, hogy eb[i - 1] még a régi értéket adja
        for (size_t i = n - 1; i >= m; --i) {
            const double f = ef[i], b = eb[i - 1];
            ef[i] = f + k * b;
            eb[i] = b + k * f;
        }
    }
    return std::vector<double>(a.begin() + 1, a.begin() + 1 + static_cast<std::ptrdiff_t>(usedOrder));
}

/** Egy csatorna végének (tail) előrefelé történő kiterjesztése padLen mintával. */
inline std::vector<double> extrapolateTail(const std::vector<double>& tail, size_t padLen, EdgeMode mode) {
    std::vector<double> ext(padLen, 0.0);
    const size_t n = tail.size();
    if (padLen == 0 || n < 2 || mode == EdgeMode::None) return ext;

    // Csend-ellenőrzés a szél közvetlen környezetében (utolsó legfeljebb 64 minta): ha csend, 0 marad
    const size_t checkN = std::min<size_t>(n, 64);
    double nearPeak = 0.0;
    for (size_t i = n - checkN; i < n; ++i) nearPeak = std::max(nearPeak, std::abs(tail[i]));
    if (nearPeak <= 1e-15) return ext;

    double winPeak = nearPeak;
    for (double v : tail) winPeak = std::max(winPeak, std::abs(v));

    if (mode == EdgeMode::Odd) {
        const double edge = tail[n - 1];
        const size_t avail = n - 1;
        for (size_t i = 0; i < padLen; ++i) {
            const size_t k = (i % avail) + 1;
            ext[i] = 2.0 * edge - tail[n - 1 - k];
        }
        const size_t keep = std::min(avail / 2, padLen / 4);
        const size_t fade = padLen - keep;
        if (fade > 1) {
            constexpr double kPi = 3.14159265358979323846;
            for (size_t i = 0; i < fade; ++i) {
                const double w = 0.5 * (1.0 + std::cos(kPi * static_cast<double>(i) / static_cast<double>(fade - 1)));
                ext[keep + i] *= w;
            }
        }
        return ext;
    }

    // EdgeMode::Lpc: Burg-módszeres autoregresszív kiterjesztés
    const std::vector<double> a = burgLpc(tail, 16);
    const size_t p = a.size();
    if (p == 0) return ext;

    std::vector<double> buf(p + padLen, 0.0);
    for (size_t i = 0; i < p; ++i) buf[i] = tail[n - p + i];
    const double peakLim = std::max(winPeak * 4.0, 1e-30);

    for (size_t i = 0; i < padLen; ++i) {
        double val = 0.0;
        for (size_t k = 0; k < p; ++k) val -= a[k] * buf[p + i - 1 - k];
        if (!std::isfinite(val) || std::abs(val) > peakLim) {
            // Ha divergens lenne, ettől a ponttól lágyan nullázódik
            break;
        }
        buf[p + i] = val;
    }
    for (size_t i = 0; i < padLen; ++i) ext[i] = buf[p + i];

    // Az első fél tiszta előrejelzés, a második fél koszinusz-ablakkal simán 0-ba fut ki
    const size_t keep = padLen / 2;
    const size_t fade = padLen - keep;
    if (fade > 1) {
        constexpr double kPi = 3.14159265358979323846;
        for (size_t i = 0; i < fade; ++i) {
            const double w = 0.5 * (1.0 + std::cos(kPi * static_cast<double>(i) / static_cast<double>(fade - 1)));
            ext[keep + i] *= w;
        }
    }
    return ext;
}

/** Egzakt egész kimeneti mintaszámot adó bemeneti kiterjesztés-hossz (padIn * dstRate / srcRate == padOut). */
inline void computeExactEdgePad(uint32_t srcRate, uint32_t dstRate, size_t minPadIn, size_t& padIn, size_t& padOut) {
    padIn = 0;
    padOut = 0;
    if (srcRate == 0 || dstRate == 0 || srcRate == dstRate) return;
    const uint32_t g = std::gcd(srcRate, dstRate);
    const size_t stepIn = srcRate / g;
    const size_t stepOut = dstRate / g;
    if (stepIn == 0 || stepIn > 32768) return;
    const size_t k = (minPadIn + stepIn - 1) / stepIn;
    padIn = k * stepIn;
    padOut = k * stepOut;
}

}  // namespace detail

/**
 * 64 bites lebegőpontos fájlból fájlba átalakítás (libsoxr SOXR_FLOAT64_I, double belső számítás).
 * A beépített lejátszó 32 bites kódútját nem érinti; kizárólag a fájlátalakító mód (--mode file) használja.
 */
inline bool convertFile64(const std::string& in, uint32_t dstRate, const audioopt::AudioOptions& optsIn,
                          std::vector<double>& out, Result& res, std::string* err = nullptr,
                          const FileOptions& fileOpts = {}) {
    WavDoubleData wav;
    if (!readWavDouble(in, wav, err)) return false;

    audioopt::AudioOptions opts = optsIn;
    audioopt::sanitize(opts);

    const uint32_t ch = wav.channels;
    const uint32_t srcRate = wav.sampleRate;
    const size_t totalInFrames = ch ? (wav.samples.size() / ch) : 0;

    res = Result{};
    res.srcRate = srcRate;
    res.dstRate = dstRate;
    res.channels = ch;
    res.outBits = 64;
    res.inFrames = totalInFrames;

    const bool applyGain = (opts.upsampleHeadroomDb != 0.0f);
    const double gain = applyGain ? std::pow(10.0, static_cast<double>(opts.upsampleHeadroomDb) / 20.0) : 1.0;

    if (srcRate == dstRate) {
        out = std::move(wav.samples);
        for (double& s : out) {
            if (applyGain) s *= gain;
            if (s > 1.0 || s < -1.0) ++res.clipped;
        }
        res.outFrames = totalInFrames;
        return true;
    }

    const double prec = (fileOpts.precision > 0.0)
                            ? std::clamp(fileOpts.precision, 15.0, 33.0)
                            : static_cast<double>(opts.upsamplePrecision);
    const double pb = (fileOpts.passbandEnd > 0.0)
                          ? std::clamp(fileOpts.passbandEnd, 0.5, 0.997)
                          : opts.upsamplePassbandEnd;
    const double sb = (fileOpts.stopbandBegin > 0.0)
                          ? std::clamp(fileOpts.stopbandBegin, 1.0, 1.5)
                          : opts.upsampleStopbandBegin;
    const double ph = (fileOpts.phase >= 0.0)
                          ? std::clamp(fileOpts.phase, 0.0, 100.0)
                          : std::clamp(opts.upsamplePhase, 0.0, 100.0);

    soxr_quality_spec_t q_spec = soxr_quality_spec(SOXR_32_BITQ, SOXR_DOUBLE_PRECISION | SOXR_HI_PREC_CLOCK);
    q_spec.precision = prec;
    q_spec.phase_response = ph;
    q_spec.passband_end = pb;
    q_spec.stopband_begin = sb;

    soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT64_I, SOXR_FLOAT64_I);
    soxr_runtime_spec_t rt_spec = soxr_runtime_spec(1);

    soxr_error_t serr = nullptr;
    soxr_t soxr = soxr_create(static_cast<double>(srcRate), static_cast<double>(dstRate), ch, &serr, &io_spec, &q_spec, &rt_spec);
    if (serr != nullptr || soxr == nullptr) {
        if (soxr) soxr_delete(soxr);
        if (err) *err = std::string("a libsoxr (64 bit) nem indult") + (serr ? std::string(": ") + serr : "");
        return false;
    }

    // Szélkezelés előkészítése (csak akkor kér paddinget, ha aktív és legalább az egyik szél nem csendes)
    size_t padIn = 0, padOut = 0;
    std::vector<double> leftPad, rightPad;
    if (fileOpts.edge != EdgeMode::None && totalInFrames >= 4) {
        const size_t checkN = std::min<size_t>(totalInFrames, 64);
        double edgePeak = 0.0;
        for (size_t i = 0; i < checkN; ++i)
            for (uint32_t c = 0; c < ch; ++c) {
                edgePeak = std::max(edgePeak, std::abs(wav.samples[i * ch + c]));
                edgePeak = std::max(edgePeak, std::abs(wav.samples[(totalInFrames - 1 - i) * ch + c]));
            }
        if (edgePeak > 1e-15) {
            detail::computeExactEdgePad(srcRate, dstRate, 10240, padIn, padOut);
            if (padIn > 0) {
                const size_t winLen = std::min<size_t>(totalInFrames, 512);
                leftPad.assign(padIn * ch, 0.0);
                rightPad.assign(padIn * ch, 0.0);
                std::vector<double> tail(winLen);
                for (uint32_t c = 0; c < ch; ++c) {
                    // Bal szél: az első winLen minta fordított sorrendben előrejelezve, majd visszafordítva
                    for (size_t i = 0; i < winLen; ++i) tail[i] = wav.samples[(winLen - 1 - i) * ch + c];
                    const auto extL = detail::extrapolateTail(tail, padIn, fileOpts.edge);
                    for (size_t i = 0; i < padIn; ++i) leftPad[(padIn - 1 - i) * ch + c] = extL[i];

                    // Jobb szél: az utolsó winLen minta előrejelezve
                    for (size_t i = 0; i < winLen; ++i) tail[i] = wav.samples[(totalInFrames - winLen + i) * ch + c];
                    const auto extR = detail::extrapolateTail(tail, padIn, fileOpts.edge);
                    for (size_t i = 0; i < padIn; ++i) rightPad[i * ch + c] = extR[i];
                }
            }
        }
    }

    const double ratio = static_cast<double>(dstRate) / static_cast<double>(srcRate);
    std::vector<double> fullOut;
    fullOut.reserve(static_cast<size_t>(std::ceil((totalInFrames + 2 * padIn) * ratio) + 1024) * ch);

    const size_t stageFrames = static_cast<size_t>(std::ceil(kChunkFrames * ratio)) + 1024;
    std::vector<double> stage(std::max(stageFrames, kFlushFrames) * ch);

    auto feedBlock = [&](const double* ptr, size_t nFrames) -> bool {
        size_t pos = 0;
        while (pos < nFrames) {
            const size_t chunk = std::min(kChunkFrames, nFrames - pos);
            size_t consumed = 0;
            while (consumed < chunk) {
                size_t idone = 0, odone = 0;
                serr = soxr_process(soxr, ptr + (pos + consumed) * ch, chunk - consumed, &idone,
                                    stage.data(), stage.size() / ch, &odone);
                if (serr != nullptr) {
                    if (err) *err = std::string("libsoxr feldolgozási hiba: ") + serr;
                    return false;
                }
                if (odone > 0) {
                    const size_t ns = odone * ch;
                    for (size_t i = 0; i < ns; ++i) {
                        double v = stage[i];
                        if (applyGain) v *= gain;
                        fullOut.push_back(v);
                    }
                }
                if (idone == 0 && odone == 0) {
                    if (err) *err = "libsoxr elakadt (0 bemeneti és 0 kimeneti minta)";
                    return false;
                }
                consumed += idone;
            }
            pos += chunk;
        }
        return true;
    };

    bool ok = true;
    if (padIn > 0) ok = feedBlock(leftPad.data(), padIn);
    if (ok) ok = feedBlock(wav.samples.data(), totalInFrames);
    if (ok && padIn > 0) ok = feedBlock(rightPad.data(), padIn);
    if (!ok) {
        soxr_delete(soxr);
        return false;
    }

    for (int step = 0;; ++step) {
        size_t idone = 0, odone = 0;
        serr = soxr_process(soxr, nullptr, 0, &idone, stage.data(), kFlushFrames, &odone);
        if (serr != nullptr) {
            soxr_delete(soxr);
            if (err) *err = std::string("libsoxr kiürítési hiba: ") + serr;
            return false;
        }
        if (odone == 0) break;
        if (step >= kMaxFlushSteps) { res.leftoverFrames += odone; break; }
        res.flushFrames += odone;
        ++res.flushSteps;
        const size_t ns = odone * ch;
        for (size_t i = 0; i < ns; ++i) {
            double v = stage[i];
            if (applyGain) v *= gain;
            fullOut.push_back(v);
        }
    }

    soxr_delete(soxr);

    if (padIn > 0) {
        // Egzakt levágás: padOut keret az elejéről, a kimeneti hossz a libsoxr saját kerekítése (llround)
        const size_t targetOut = static_cast<size_t>(std::llround(static_cast<double>(totalInFrames) * ratio));
        const size_t availFrames = ch ? (fullOut.size() / ch) : 0;
        const size_t startFrame = std::min(padOut, availFrames);
        const size_t keepFrames = std::min(targetOut, availFrames - startFrame);
        out.assign(fullOut.begin() + static_cast<std::ptrdiff_t>(startFrame * ch),
                   fullOut.begin() + static_cast<std::ptrdiff_t>((startFrame + keepFrames) * ch));
    } else {
        out.swap(fullOut);
    }

    for (double v : out)
        if (v > 1.0 || v < -1.0) ++res.clipped;
    res.outFrames = ch ? (out.size() / ch) : 0;
    return true;
}

}  // namespace ee_analyzer::srcconv
