/**
 * @file test_dsd.cpp
 * @brief DSD (.dsf / .dff) fejléc-értelmező, Kaiser-FIR átalakító, ID3v2 címke, tekerés,
 *        DST-elutasítás és DoP 1.1 csomagoló/kibontó önellenőrző teszt.
 */
#include "audio_decoder.hpp"
#include "dsd.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ee_analyzer;

namespace {

int gFail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cerr << "[HIBA] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")\n"; ++gFail; } \
} while (0)

constexpr double kPi = 3.14159265358979323846;

void writeLe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

void writeLe64(std::vector<uint8_t>& out, uint64_t v) {
    writeLe32(out, static_cast<uint32_t>(v & 0xFFFFFFFFULL));
    writeLe32(out, static_cast<uint32_t>(v >> 32));
}

void writeBe16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
}

void writeBe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
}

void writeBe64(std::vector<uint8_t>& out, uint64_t v) {
    writeBe32(out, static_cast<uint32_t>(v >> 32));
    writeBe32(out, static_cast<uint32_t>(v & 0xFFFFFFFFULL));
}

void writeFourCC(std::vector<uint8_t>& out, const char* id4) {
    out.insert(out.end(), id4, id4 + 4);
}

/**
 * 3. rendű, visszacsatolással stabilizált szigma-delta modulátor (DSD64 szimulátor).
 * Kimenet: MSB-first bájtfolyam (1 bit = +1.0, 0 bit = -1.0).
 * -6 dBFS (amp = 0,501187) szinuszra teljesen stabil, és >85 dB sávon belüli SNR-t ad.
 */
std::vector<uint8_t> generateSdmDsd(double freqHz, double ampLinear, uint32_t dsdRate, size_t numBytes) {
    double i1 = 0.0, i2 = 0.0, i3 = 0.0;
    double y = -1.0;
    std::vector<uint8_t> bytes(numBytes, 0);
    const double phaseStep = 2.0 * kPi * freqHz / static_cast<double>(dsdRate);
    double phase = 0.0;

    for (size_t b = 0; b < numBytes; ++b) {
        uint8_t acc = 0;
        for (int bit = 0; bit < 8; ++bit) {
            const double x = ampLinear * std::sin(phase);
            phase += phaseStep;
            // Stabil 3. rendű CIFB hurokszűrő
            const double err = x - y;
            i1 += 0.25 * err;
            i2 += 0.50 * (i1 - y);
            i3 += 0.85 * (i2 - y);
            // Szaturációs védelem az integratorok túlcsordulása ellen
            i1 = std::clamp(i1, -4.0, 4.0);
            i2 = std::clamp(i2, -4.0, 4.0);
            i3 = std::clamp(i3, -4.0, 4.0);
            const uint8_t bitVal = (i3 >= 0.0) ? 1u : 0u;
            y = bitVal ? 1.0 : -1.0;
            acc = static_cast<uint8_t>((acc << 1) | bitVal); // MSB-first
        }
        bytes[b] = acc;
    }
    return bytes;
}

/** Egyszerű ID3v2.3 blokk építése TIT2, TPE1, TALB szövegekkel. */
std::vector<uint8_t> buildId3v2Tag(const std::string& title, const std::string& artist, const std::string& album) {
    std::vector<uint8_t> frames;
    auto addTextFrame = [&](const char* id, const std::string& val) {
        frames.insert(frames.end(), id, id + 4);
        const uint32_t sz = static_cast<uint32_t>(1 + val.size());
        writeBe32(frames, sz);
        writeBe16(frames, 0); // flags
        frames.push_back(3);  // UTF-8 (ID3v2 stílus; parseId3v2 támogatja)
        frames.insert(frames.end(), val.begin(), val.end());
    };
    addTextFrame("TIT2", title);
    addTextFrame("TPE1", artist);
    addTextFrame("TALB", album);

    std::vector<uint8_t> tag;
    tag.insert(tag.end(), {'I', 'D', '3', 3, 0, 0}); // v2.3
    const uint32_t sz = static_cast<uint32_t>(frames.size());
    tag.push_back(static_cast<uint8_t>((sz >> 21) & 0x7Fu));
    tag.push_back(static_cast<uint8_t>((sz >> 14) & 0x7Fu));
    tag.push_back(static_cast<uint8_t>((sz >> 7) & 0x7Fu));
    tag.push_back(static_cast<uint8_t>(sz & 0x7Fu));
    tag.insert(tag.end(), frames.begin(), frames.end());
    return tag;
}

/** Sony DSF v1.01 fájl összeállítása MSB-first csatornánkénti bájtokból + opcionális ID3v2-vel. */
bool writeDsfFile(const fs::path& p, uint32_t dsdRate, uint32_t channels,
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

    // [DSF Section 2.1] DSD chunk (28 bájt)
    writeFourCC(out, "DSD ");
    writeLe64(out, 28);
    writeLe64(out, totalFileSize);
    writeLe64(out, metaOffset);

    // [DSF Section 2.2] fmt chunk (52 bájt)
    writeFourCC(out, "fmt ");
    writeLe64(out, 52);
    writeLe32(out, 1);                          // format version = 1
    writeLe32(out, 0);                          // format id = 0 (DSD raw)
    writeLe32(out, channels == 1 ? 1u : 2u);    // channel type (1=mono, 2=stereo)
    writeLe32(out, channels);                   // channel num
    writeLe32(out, dsdRate);                    // sampling freq
    writeLe32(out, 1);                          // bits per sample = 1 (LSB-first)
    writeLe64(out, sampleCount);                // sample count per channel
    writeLe32(out, kBlockSize);                 // block size = 4096
    writeLe32(out, 0);                          // reserved = 0

    // [DSF Section 2.3] data chunk
    writeFourCC(out, "data");
    writeLe64(out, dataChunkSize);
    for (size_t blk = 0; blk < numBlocks; ++blk) {
        for (uint32_t c = 0; c < channels; ++c) {
            for (uint32_t i = 0; i < kBlockSize; ++i) {
                const size_t idx = blk * kBlockSize + i;
                // DSF bitsPerSample == 1: LSB-first per byte -> bitfordítás az MSB-first bemenetről
                uint8_t b = (idx < rawPerCh) ? dsd::reverseBits8(chBytesMsb[c][idx]) : 0x00u;
                out.push_back(b);
            }
        }
    }

    // [DSF Section 2.4] Metadata chunk (ID3v2)
    out.insert(out.end(), id3Tag.begin(), id3Tag.end());

    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return f.good();
}

/** Philips DSDIFF v1.5 fájl összeállítása MSB-first csatornánkénti bájtokból. */
bool writeDsdiffFile(const fs::path& p, uint32_t dsdRate, uint32_t channels,
                     const std::vector<std::vector<uint8_t>>& chBytesMsb, bool fakeDst = false) {
    std::vector<uint8_t> propSub;
    // FS
    writeFourCC(propSub, "FS  ");
    writeBe64(propSub, 4);
    writeBe32(propSub, dsdRate);
    // CHNL
    writeFourCC(propSub, "CHNL");
    writeBe64(propSub, 2 + static_cast<uint64_t>(channels) * 4);
    writeBe16(propSub, static_cast<uint16_t>(channels));
    for (uint32_t c = 0; c < channels; ++c) writeFourCC(propSub, c == 0 ? "SLFT" : "SRGT");
    // CMPR
    const std::string cmprName = fakeDst ? "DST" : "not compressed";
    const uint64_t cmprSize = 4 + 1 + cmprName.size();
    writeFourCC(propSub, "CMPR");
    writeBe64(propSub, cmprSize);
    writeFourCC(propSub, fakeDst ? "DST " : "DSD ");
    propSub.push_back(static_cast<uint8_t>(cmprName.size()));
    propSub.insert(propSub.end(), cmprName.begin(), cmprName.end());
    if (cmprSize & 1ULL) propSub.push_back(0); // [DFF 2.1] páros igazítás

    std::vector<uint8_t> fverChunk;
    writeFourCC(fverChunk, "FVER");
    writeBe64(fverChunk, 4);
    writeBe32(fverChunk, 0x01050000u); // v1.5

    std::vector<uint8_t> propChunk;
    writeFourCC(propChunk, "PROP");
    writeBe64(propChunk, 4 + propSub.size());
    writeFourCC(propChunk, "SND ");
    propChunk.insert(propChunk.end(), propSub.begin(), propSub.end());

    const size_t rawPerCh = chBytesMsb.empty() ? 0 : chBytesMsb[0].size();
    const uint64_t dataPayload = fakeDst ? 16ULL : (static_cast<uint64_t>(rawPerCh) * channels);
    std::vector<uint8_t> dataChunk;
    writeFourCC(dataChunk, fakeDst ? "DST " : "DSD ");
    writeBe64(dataChunk, dataPayload);
    if (fakeDst) {
        dataChunk.resize(dataChunk.size() + 16, 0);
    } else {
        for (size_t i = 0; i < rawPerCh; ++i) {
            for (uint32_t c = 0; c < channels; ++c) {
                dataChunk.push_back(chBytesMsb[c][i]); // MSB-first, 1 bájt/csatorna összefésülve
            }
        }
    }
    if (dataPayload & 1ULL) dataChunk.push_back(0);

    std::vector<uint8_t> out;
    writeFourCC(out, "FRM8");
    writeBe64(out, 4 + fverChunk.size() + propChunk.size() + dataChunk.size());
    writeFourCC(out, "DSD ");
    out.insert(out.end(), fverChunk.begin(), fverChunk.end());
    out.insert(out.end(), propChunk.begin(), propChunk.end());
    out.insert(out.end(), dataChunk.begin(), dataChunk.end());

    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return f.good();
}

/** FIR frekvenciaátvitel |H(f)| mérése dB-ben a szűrő tényleges együtthatóiból. */
double measureFirMagnitudeDb(const std::vector<double>& h, double freqHz, double dsdRate) {
    double re = 0.0, im = 0.0;
    const double w = 2.0 * kPi * freqHz / dsdRate;
    for (size_t n = 0; n < h.size(); ++n) {
        re += h[n] * std::cos(w * static_cast<double>(n));
        im -= h[n] * std::sin(w * static_cast<double>(n));
    }
    const double mag = std::hypot(re, im);
    return 20.0 * std::log10(std::max(mag, 1e-12));
}

/** Hann-ablakolt spektrumanalízis a dekódolt PCM csatornán: csúcsfrekvencia, jelszint és 20 Hz–20 kHz sávzaj. */
struct SpectrumStats {
    double peakFreqHz = 0.0;
    double peakLevelDbfs = -200.0;
    double noise20_20kDbfs = -200.0;
};

SpectrumStats analyzePcmChannel(const std::vector<float>& interleaved, uint32_t channels, uint32_t chIdx,
                                uint32_t sampleRate, size_t startFrame, size_t N) {
    // Blackman-Harris ablak az igen alacsony oldalnyalábokért (>-92 dB), hogy az 1 kHz-es alaphang
    // szivárgása ne takarja el a 20 Hz-20 kHz zajszintet.
    std::vector<double> x(N);
    double wSum = 0.0;
    for (size_t n = 0; n < N; ++n) {
        const double t = 2.0 * kPi * static_cast<double>(n) / static_cast<double>(N - 1);
        const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2.0 * t) - 0.01168 * std::cos(3.0 * t);
        x[n] = static_cast<double>(interleaved[(startFrame + n) * channels + chIdx]) * w;
        wSum += w;
    }

    const size_t minBin = std::max<size_t>(1, static_cast<size_t>(std::ceil(20.0 * N / sampleRate)));
    const size_t maxBin = std::min<size_t>(N / 2 - 1, static_cast<size_t>(std::floor(20000.0 * N / sampleRate)));

    std::vector<double> mag2(maxBin + 1, 0.0);
    size_t bestBin = minBin;
    double bestMag2 = -1.0;

    for (size_t k = minBin; k <= maxBin; ++k) {
        double re = 0.0, im = 0.0;
        const double w = 2.0 * kPi * static_cast<double>(k) / static_cast<double>(N);
        for (size_t n = 0; n < N; ++n) {
            re += x[n] * std::cos(w * static_cast<double>(n));
            im -= x[n] * std::sin(w * static_cast<double>(n));
        }
        mag2[k] = re * re + im * im;
        if (mag2[k] > bestMag2) {
            bestMag2 = mag2[k];
            bestBin = k;
        }
    }

    // Szinusz amplitúdó: koherens nyereség wSum / 2 -> 0 dBFS szinusz csúcsértéke 1.0
    const double peakAmp = 2.0 * std::sqrt(bestMag2) / wSum;

    // 20 Hz..20 kHz sávon belüli összzaj (a csúcs ±4 bin ablakának és egyenáramú komponensnek kizárásával,
    // ablak ekvivalens zaj-sávszélességével ENBW korrekcióval).
    double w2Sum = 0.0;
    for (size_t n = 0; n < N; ++n) {
        const double t = 2.0 * kPi * static_cast<double>(n) / static_cast<double>(N - 1);
        const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2.0 * t) - 0.01168 * std::cos(3.0 * t);
        w2Sum += w * w;
    }
    const double enbw = static_cast<double>(N) * w2Sum / (wSum * wSum); // ~2.004 Blackman-Harrisra

    double noisePower = 0.0;
    for (size_t k = minBin; k <= maxBin; ++k) {
        if (k + 4 >= bestBin && k <= bestBin + 4) continue;
        // Kétoldali spektrum konvertálása egyoldali RMS négyzetre: 2 * |X[k]|^2 / (N * w2Sum) * (enbw korrekció)
        noisePower += 2.0 * mag2[k] / (static_cast<double>(N) * w2Sum);
    }
    noisePower /= enbw;

    // dBFS-hez képest: teljes skálás szinusz RMS = 1 / sqrt(2) -> P_fs = 0.5
    SpectrumStats st;
    st.peakFreqHz = static_cast<double>(bestBin) * static_cast<double>(sampleRate) / static_cast<double>(N);
    st.peakLevelDbfs = 20.0 * std::log10(std::max(peakAmp, 1e-12));
    st.noise20_20kDbfs = 10.0 * std::log10(std::max(noisePower / 0.5, 1e-15));
    return st;
}

} // namespace

int main() {
    std::cout << "=== TEST_DSD INDÍTÁS ===\n";

    // 1. Szűrő tervezésének és paramétereinek ellenőrzése (44.1 kHz és 48 kHz DSD család)
    {
        dsd::DsdToPcmConverter conv64;
        CHECK(conv64.init(2822400, 88200, 2), "DSD64->88200 FIR init sikertelen");
        double coeffSum = 0.0;
        for (double c : conv64.coeffs()) coeffSum += c;
        CHECK(std::abs(coeffSum - 1.0) < 1e-6, "FIR együtthatók összege != 1.0 (0 dB erősítés)");

        const double h1k = measureFirMagnitudeDb(conv64.coeffs(), 1000.0, 2822400.0);
        const double h20k = measureFirMagnitudeDb(conv64.coeffs(), 20000.0, 2822400.0);
        const double h42k = measureFirMagnitudeDb(conv64.coeffs(), 42000.0, 2822400.0);
        const double h68k = measureFirMagnitudeDb(conv64.coeffs(), 68200.0, 2822400.0);
        const double h500k = measureFirMagnitudeDb(conv64.coeffs(), 500000.0, 2822400.0);
        std::cout << "[FIR] taps=" << conv64.coeffs().size()
                  << " H(1kHz)=" << h1k << " dB, H(20kHz)=" << h20k
                  << " dB, H(42kHz)=" << h42k << " dB, H(68.2kHz)=" << h68k
                  << " dB, H(500kHz)=" << h500k << " dB\n";
        CHECK(std::abs(h1k) < 0.05, "FIR átvitel 1 kHz-en kívül esik a +-0.05 dB-en");
        CHECK(std::abs(h20k) < 0.10, "FIR átvitel 20 kHz-en túl sokat esik");
        CHECK(h42k < -95.0 && h68k < -95.0 && h500k < -95.0, "FIR zárósáv csillapítás < 95 dB");

        uint32_t pcm48 = 0, dop48 = 0;
        CHECK(dsd::resolveDsdRates(3072000, pcm48, dop48) && pcm48 == 96000 && dop48 == 192000,
              "48 kHz család DSD64 -> 96000 Hz feloldás hibás");
    }

    // 2. DSD64 (0,5 s = 1 411 200 minta = 176 400 bájt / csatorna = 44 100 PCM keret @ 88 200 Hz)
    // Bal csatorna: 1000 Hz, -6.0 dBFS; Jobb csatorna: 2000 Hz, -6.0 dBFS
    constexpr uint32_t kDsdRate = 2822400;
    constexpr uint32_t kPcmRate = 88200;
    constexpr uint32_t kChannels = 2;
    constexpr size_t kDsdBytesPerCh = 176400; // 43 * 4096 + 272 -> nem osztható 4096-tal, így a DSF blokkhatárt és paddinget is teszteli!
    constexpr uint64_t kExpectedPcmFrames = (static_cast<uint64_t>(kDsdBytesPerCh) * 8ULL) / 32ULL; // 44100
    const double ampMinus6Db = std::pow(10.0, -6.0 / 20.0); // 0.501187

    std::vector<std::vector<uint8_t>> dsdChannels(2);
    dsdChannels[0] = generateSdmDsd(1000.0, ampMinus6Db, kDsdRate, kDsdBytesPerCh);
    dsdChannels[1] = generateSdmDsd(2000.0, ampMinus6Db, kDsdRate, kDsdBytesPerCh);

    const fs::path tmpDir = fs::temp_directory_path() / "aqulify_dsd_test";
    fs::create_directories(tmpDir);
    const fs::path dsfPath = tmpDir / "sinus_1k_2k.dsf";
    const fs::path dffPath = tmpDir / "sinus_1k_2k.dff";
    const fs::path dstDffPath = tmpDir / "compressed_dst.dff";

    const auto id3 = buildId3v2Tag("DSD Szinusz Teszt", "Aqulify SDM", "DSD64 Referencia");
    CHECK(writeDsfFile(dsfPath, kDsdRate, kChannels, dsdChannels, id3), "DSF fájl írása sikertelen");
    CHECK(writeDsdiffFile(dffPath, kDsdRate, kChannels, dsdChannels, false), "DFF fájl írása sikertelen");
    CHECK(writeDsdiffFile(dstDffPath, kDsdRate, kChannels, dsdChannels, true), "DST DFF fájl írása sikertelen");

    CHECK(player::isSupportedFile(dsfPath.string()), "isSupportedFile false .dsf-re");
    CHECK(player::isSupportedFile(dffPath.string()), "isSupportedFile false .dff-re");

    // 3. DSF és DFF dekódolás, fejléc, ID3v2, PCM szint/frekvencia/zaj, tekerés ellenőrzése
    auto verifyPcmDecoder = [&](const fs::path& p, bool expectTags, const char* label) {
        std::string err;
        auto dec = player::openDecoder(p.string(), &err, false);
        CHECK(dec != nullptr, std::string(label) + " openDecoder sikertelen: " + err);
        if (!dec) return;

        CHECK(dec->sampleRate() == kPcmRate, std::string(label) + " sampleRate != 88200");
        CHECK(dec->channels() == kChannels, std::string(label) + " channels != 2");
        CHECK(dec->totalFrames() == kExpectedPcmFrames, std::string(label) + " totalFrames != 44100");

        if (expectTags) {
            CHECK(dec->tags().title == "DSD Szinusz Teszt", "DSF ID3v2 TIT2 hibás");
            CHECK(dec->tags().artist == "Aqulify SDM", "DSF ID3v2 TPE1 hibás");
            CHECK(dec->tags().album == "DSD64 Referencia", "DSF ID3v2 TALB hibás");
        }

        std::vector<float> pcm(kExpectedPcmFrames * kChannels, 0.0f);
        size_t totalRead = 0;
        while (totalRead < kExpectedPcmFrames) {
            const size_t got = dec->read(pcm.data() + totalRead * kChannels,
                                         std::min<size_t>(4096, kExpectedPcmFrames - totalRead));
            if (got == 0) break;
            totalRead += got;
        }
        CHECK(totalRead == kExpectedPcmFrames, std::string(label) + " tényleges olvasott hossz != totalFrames");
        // Fájlvég után 0-t ad vissza
        float extra[4]{};
        CHECK(dec->read(extra, 2) == 0, std::string(label) + " EOF után még adott adatot");

        // 88200 Hz-en 8820 minta = pontosan 0.1 s (bin szélesség = 10 Hz -> 1000 Hz pontosan bin 100, 2000 Hz pontosan bin 200!)
        constexpr size_t kStart = 8820;
        constexpr size_t kAnaN = 8820;
        const auto stL = analyzePcmChannel(pcm, kChannels, 0, kPcmRate, kStart, kAnaN);
        const auto stR = analyzePcmChannel(pcm, kChannels, 1, kPcmRate, kStart, kAnaN);
        std::cout << "[" << label << "] L: f=" << stL.peakFreqHz << " Hz, szint=" << stL.peakLevelDbfs
                  << " dBFS, zaj(20Hz-20kHz)=" << stL.noise20_20kDbfs << " dBFS\n";
        std::cout << "[" << label << "] R: f=" << stR.peakFreqHz << " Hz, szint=" << stR.peakLevelDbfs
                  << " dBFS, zaj(20Hz-20kHz)=" << stR.noise20_20kDbfs << " dBFS\n";

        CHECK(std::abs(stL.peakFreqHz - 1000.0) < 1.0, std::string(label) + " L csúcsfrekvencia != 1000 Hz");
        CHECK(std::abs(stR.peakFreqHz - 2000.0) < 1.0, std::string(label) + " R csúcsfrekvencia != 2000 Hz");
        // A -6 dBFS-es (≈50%-os modulációs mélységű, azaz 0 dB SACD) bemenet a ×2 kimeneti erősítéssel (+6,02 dB,
        // DsdToPcmConverter::kOutputGain) ≈ 0 dBFS lesz: várt szint -6,0 + 6,0206 = +0,02 dBFS
        const double expectedDb = -6.0 + 20.0 * std::log10(dsd::DsdToPcmConverter::kOutputGain);
        CHECK(std::abs(stL.peakLevelDbfs - expectedDb) <= 0.2, std::string(label) + " L jelszint kívül a várt (+0,02 dBFS) ±0,2 dB-en");
        CHECK(std::abs(stR.peakLevelDbfs - expectedDb) <= 0.2, std::string(label) + " R jelszint kívül a várt (+0,02 dBFS) ±0,2 dB-en");
        // Indokolt zajküszöb: a 3. rendű SDM kvantálási zaja 20 Hz-20 kHz között (OSR=70,5) és a
        // 105 dB-es Kaiser-sinc FIR visszaverődési padlója együtt ~ -90 dBFS körül mérhető;
        // konzervatív küszöb: -80.0 dBFS (a tényleges mért értéknél >10 dB tartalékkal szigorú).
        CHECK(stL.noise20_20kDbfs < -80.0 && stR.noise20_20kDbfs < -80.0,
              std::string(label) + " 20 Hz-20 kHz sávzaj >= -80 dBFS");

        // Tekerés tesztje több 4096-os DSF blokkon túli pozícióra: frame 25000 (DSD bájt 100 000 -> ~24.4 blokk)
        constexpr uint64_t kSeekFrame = 25000;
        CHECK(dec->seek(kSeekFrame), std::string(label) + " seek(25000) sikertelen");
        std::vector<float> seeked(1024 * kChannels);
        const size_t gotSeek = dec->read(seeked.data(), 1024);
        CHECK(gotSeek == 1024, std::string(label) + " seek utáni read hossza != 1024");
        double maxSeekDiff = 0.0;
        for (size_t i = 0; i < 1024 * kChannels; ++i) {
            maxSeekDiff = std::max<double>(maxSeekDiff, std::abs(seeked[i] - pcm[kSeekFrame * kChannels + i]));
        }
        std::cout << "[" << label << "] seek(25000) max eltérés = " << maxSeekDiff << "\n";
        CHECK(maxSeekDiff < 1e-6, std::string(label) + " seek utáni PCM eltér a folyamatos olvasástól");
    };

    verifyPcmDecoder(dsfPath, true, "DSF");
    verifyPcmDecoder(dffPath, false, "DFF");

    // 4. DST-s DFF elutasítása érthető hibaüzenettel
    {
        std::string err;
        auto dec = player::openDecoder(dstDffPath.string(), &err, false);
        CHECK(dec == nullptr, "DST-tömörített DFF-et el kellett volna utasítani");
        CHECK(err.find("DST") != std::string::npos, "DST DFF hibaüzenet nem tartalmazza a 'DST' okot: " + err);
        std::cout << "[DST] elutasítási hibaüzenet: " << err << "\n";
    }

    // 5. Nyers DSD bájt olvasó + DoP 1.1 bitre pontos oda-vissza átalakítás és jelölőváltakozás
    {
        std::string err1, err2;
        auto rawDsf = dsd::openDsdRawFile(dsfPath.string(), &err1);
        auto rawDff = dsd::openDsdRawFile(dffPath.string(), &err2);
        CHECK(rawDsf != nullptr && rawDff != nullptr, "openDsdRawFile sikertelen");
        if (rawDsf && rawDff) {
            CHECK(rawDsf->info().dopSampleRate == 176400, "DoP mintafr != 176400");
            CHECK(rawDsf->totalChannelBytes() == kDsdBytesPerCh, "DSF nyers bájtszám hibás");
            CHECK(rawDff->totalChannelBytes() == kDsdBytesPerCh, "DFF nyers bájtszám hibás");

            // 1024 bájt/csatorna beolvasása (= 512 DoP keret @ 2 bájt/csatorna/keret)
            constexpr size_t kDopFrames = 512;
            constexpr size_t kRawBytes = kDopFrames * 2;
            std::vector<uint8_t> bufDsf(kRawBytes * kChannels), bufDff(kRawBytes * kChannels);
            CHECK(rawDsf->read(bufDsf.data(), kRawBytes) == kRawBytes, "rawDsf read hibás");
            CHECK(rawDff->read(bufDff.data(), kRawBytes) == kRawBytes, "rawDff read hibás");
            CHECK(bufDsf == bufDff, "DSF és DFF MSB-first nyers DSD bájtfolyama nem egyezik");

            std::vector<int32_t> dopOut(kDopFrames * kChannels);
            const uint8_t nextPhase = dsd::packDopFrames(bufDsf.data(), kDopFrames, kChannels, 0, dopOut.data());
            CHECK(nextPhase == (kDopFrames & 1u), "packDopFrames zárófázis hibás");

            std::vector<uint8_t> unpacked(kRawBytes * kChannels);
            for (size_t f = 0; f < kDopFrames; ++f) {
                const uint8_t expectedMarker = (f % 2 == 0) ? 0x05u : 0xFAu;
                for (uint32_t c = 0; c < kChannels; ++c) {
                    uint8_t m = 0, b0 = 0, b1 = 0;
                    dsd::unpackDopSample(dopOut[f * kChannels + c], m, b0, b1);
                    CHECK(m == expectedMarker, "DoP marker eltér a várt 0x05/0xFA váltakozástól");
                    unpacked[(f * kChannels + c) * 2u] = b0;
                    unpacked[(f * kChannels + c) * 2u + 1u] = b1;
                }
            }
            CHECK(unpacked == bufDsf, "DoP oda-vissza kicsomagolás nem bitpontos");
            std::cout << "[DoP] 512 keret (176400 Hz, 0x05/0xFA jelölőváltakozás, L/R azonos marker) bitre pontos.\n";
        }
    }

    // Ideiglenes tesztfájlok takarítása
    std::error_code ec;
    fs::remove_all(tmpDir, ec);

    if (gFail == 0) {
        std::cout << "=== TEST_DSD SIKERES (0 HIBA) ===\n";
        return 0;
    }
    std::cerr << "=== TEST_DSD SIKERTELEN (" << gFail << " HIBA) ===\n";
    return 1;
}
