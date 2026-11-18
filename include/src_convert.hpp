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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

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

}  // namespace ee_analyzer::srcconv
