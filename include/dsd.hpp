#pragma once
/**
 * @file dsd.hpp
 * @brief Tiszta, platformfüggetlen DSD-értelmező (.dsf / .dff), DSD→PCM (Kaiser-FIR) átalakító és DoP 1.1 csomagoló.
 *
 * Szabványforrások (minden mezőnél külön is hivatkozva):
 *  - [DSF] Sony Corporation: "DSF File Format Specification" v1.01
 *          (2.1 DSD chunk, 2.2 fmt chunk, 2.3 data chunk, 2.4 Metadata chunk).
 *  - [DFF] Royal Philips Electronics: "DSDIFF File Format Specification" v1.5
 *          (2.1 Chunk structure / even padding / big-endian, 3.1 FRM8, 3.2 FVER, 3.3 PROP/FS/CHNL/CMPR, 3.4 DSD, 3.5 DST).
 *  - [DoP] dCS et al.: "DoP open Standard" v1.1
 *          (24 bites PCM konténer, 0x05/0xFA váltakozó jelölő a felső 8 biten, t0..t15 MSB→LSB az alsó 16 biten).
 *
 * DSD → PCM aluláteresztő FIR szűrő paraméterei (kód által számítva):
 *  - Kimeneti mintavételi frekvencia: 44,1 kHz-es DSD család → 88 200 Hz; 48 kHz-es DSD család → 96 000 Hz.
 *  - Tizedelési arány: D = dsdSampleRate / pcmSampleRate (pl. DSD64 → D = 32, DSD128 → D = 64, DSD256 → D = 128).
 *  - Ablakfüggvény: Kaiser-ablakos sinc, célzott zárósáv-csillapítás A_s = 105 dB (béta = 10,61).
 *  - Áteresztési határ: f_p = 20 000 Hz (áteresztő sávbeli hullámzás < ±0,02 dB).
 *  - Vágási (félmagasságú) frekvencia: f_c = 31 000 Hz; zárósáv kezdete: f_s = 42 000 Hz (< Nyquist: 44 100 Hz).
 *  - Erősítés: DSD bit '1' -> +1, '0' -> -1; a FIR együtthatók összege pontosan 1.0, a kimenet ×2 (+6,02 dB,
 *    kOutputGain): az 50%-os modulációs mélység (0 dB SACD) = 0 dBFS PCM.
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace ee_analyzer::dsd {

enum class ContainerType {
    Dsf,
    Dsdiff
};

/** Fejlécből kiolvasott DSD folyam-adatok. */
struct DsdFileInfo {
    ContainerType container = ContainerType::Dsf;
    uint32_t dsdSampleRate = 0;       // pl. 2822400 (DSD64), 3072000 (DSD64x48)
    uint32_t pcmSampleRate = 0;       // 88200 vagy 96000
    uint32_t dopSampleRate = 0;       // dsdSampleRate / 16 (pl. 176400 DSD64-nél)
    uint32_t channels = 0;            // csatornaszám
    uint32_t bitsPerSample = 1;       // DSF [2.2]: 1 (LSB-first) vagy 8 (MSB-first); DSDIFF [3.4]: 8 (MSB-first)
    uint32_t blockSizePerChannel = 1; // DSF [2.2]: jellemzően 4096; DSDIFF [3.4]: 1 (bájtonként összefésülve)
    bool lsbFirst = false;            // true: bájton belül bit0 a legkorábbi; false: bit7 (MSB) a legkorábbi
    uint64_t sampleCount = 0;         // csatornánkénti 1 bites DSD minták száma
    uint64_t dataOffset = 0;          // a nyers mintaadat kezdőbájtja a fájlban
    uint64_t dataBytes = 0;           // a mintaadat-blokk mérete bájtban (pad-dal együtt)
    uint64_t metadataOffset = 0;      // DSF [2.1]: az ID3v2 blokk helye (0 ha nincs)
    uint64_t metadataMaxBytes = 0;    // metadataOffset és fájlvég közötti bájtmennyiség
};

/** Fájlszelet olvasására szolgáló függvénymutató (tetszőleges I/O réteg fölött). */
using ReadAtFn = bool (*)(void* ctx, uint64_t offset, void* dst, size_t len);

// ---------------------------------------------------------------------------
// Bájtsorrend és bitfordítás segédfüggvények
// ---------------------------------------------------------------------------

inline uint16_t readLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t readLe64(const uint8_t* p) {
    return static_cast<uint64_t>(readLe32(p)) | (static_cast<uint64_t>(readLe32(p + 4)) << 32);
}

inline uint16_t readBe16(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

inline uint32_t readBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline uint64_t readBe64(const uint8_t* p) {
    return (static_cast<uint64_t>(readBe32(p)) << 32) | static_cast<uint64_t>(readBe32(p + 4));
}

inline bool tagEq(const uint8_t* p, const char* id4) {
    return p[0] == static_cast<uint8_t>(id4[0]) && p[1] == static_cast<uint8_t>(id4[1]) &&
           p[2] == static_cast<uint8_t>(id4[2]) && p[3] == static_cast<uint8_t>(id4[3]);
}

/** 8 bites bitfordító tábla (LSB-first DSF bájtok <-> MSB-first bájtok). */
inline uint8_t reverseBits8(uint8_t b) {
    static constexpr std::array<uint8_t, 256> kRev = [] {
        std::array<uint8_t, 256> t{};
        for (int i = 0; i < 256; ++i) {
            uint8_t v = static_cast<uint8_t>(i), r = 0;
            for (int bit = 0; bit < 8; ++bit) {
                r = static_cast<uint8_t>((r << 1) | (v & 1u));
                v >>= 1;
            }
            t[static_cast<size_t>(i)] = r;
        }
        return t;
    }();
    return kRev[b];
}

/**
 * DSD mintavételi frekvencia ellenőrzése és cél PCM ráta meghatározása:
 * 44,1 kHz család -> 88 200 Hz; 48 kHz család -> 96 000 Hz.
 * Feltétel: dsdRate / pcmOut osztható legyen 8-cal (egész bájtszámú keretenkénti tizedelés).
 */
inline bool resolveDsdRates(uint32_t dsdRate, uint32_t& outPcmRate, uint32_t& outDopRate) {
    if (dsdRate < 2822400u) return false;
    // 44,1 kHz család (pl. 2822400, 5644800, 11289600, 22579200)
    if (dsdRate % (8u * 88200u) == 0) {
        outPcmRate = 88200u;
        outDopRate = dsdRate / 16u;
        return true;
    }
    // 48 kHz család (pl. 3072000, 6144000, 12288000, 24576000)
    if (dsdRate % (8u * 96000u) == 0) {
        outPcmRate = 96000u;
        outDopRate = dsdRate / 16u;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 1. Sony DSF v1.01 fejléc-értelmező [DSF 2.1 - 2.4]
// ---------------------------------------------------------------------------

/**
 * Sony DSF fájl fejlécének értelmezése. Minden numerikus mező little-endian [DSF 2].
 * Csatornánkénti adatelrendezés: blockSizePerChannel (jellemzően 4096) bájtnyi blokkokban [DSF 2.2-2.3].
 */
inline bool parseDsfHeader(ReadAtFn readAt, void* ctx, uint64_t fileSize, DsdFileInfo& out, std::string& error) {
    if (!readAt || fileSize < 92) {
        error = "Érvénytelen vagy csonka DSF fájl (túl kicsi fejléc).";
        return false;
    }

    // [DSF Section 2.1] DSD chunk (28 bájt)
    uint8_t dsdChunk[28]{};
    if (!readAt(ctx, 0, dsdChunk, sizeof(dsdChunk))) {
        error = "DSF fejléc olvasási hiba.";
        return false;
    }
    if (!tagEq(dsdChunk, "DSD ")) {
        error = "Nem DSF fájl (hiányzó 'DSD ' azonosító).";
        return false;
    }
    const uint64_t dsdChunkSize = readLe64(dsdChunk + 4);      // [DSF 2.1] Chunk size (28)
    const uint64_t declaredTotalSize = readLe64(dsdChunk + 12); // [DSF 2.1] Total file size
    const uint64_t metadataPtr = readLe64(dsdChunk + 20);       // [DSF 2.1] Pointer to Metadata chunk
    if (dsdChunkSize < 28 || dsdChunkSize > fileSize) {
        error = "Érvénytelen DSF DSD-chunk méret.";
        return false;
    }
    (void)declaredTotalSize;

    // [DSF Section 2.2] fmt chunk (legalább 52 bájt a dsdChunkSize ofszeten)
    const uint64_t fmtOff = dsdChunkSize;
    if (fmtOff + 52 > fileSize) {
        error = "Hiányzó vagy csonka DSF fmt chunk.";
        return false;
    }
    uint8_t fmtChunk[52]{};
    if (!readAt(ctx, fmtOff, fmtChunk, sizeof(fmtChunk))) {
        error = "DSF fmt chunk olvasási hiba.";
        return false;
    }
    if (!tagEq(fmtChunk, "fmt ")) {
        error = "Érvénytelen DSF formátum (hiányzó 'fmt ' chunk).";
        return false;
    }
    const uint64_t fmtSize = readLe64(fmtChunk + 4);           // [DSF 2.2] Chunk size (általában 52)
    const uint32_t fmtVersion = readLe32(fmtChunk + 12);       // [DSF 2.2] Format version (1)
    const uint32_t fmtId = readLe32(fmtChunk + 16);            // [DSF 2.2] Format ID (0 = DSD raw)
    const uint32_t channelType = readLe32(fmtChunk + 20);      // [DSF 2.2] Channel type (1..7)
    const uint32_t channelNum = readLe32(fmtChunk + 24);       // [DSF 2.2] Channel num
    const uint32_t sampleFreq = readLe32(fmtChunk + 28);       // [DSF 2.2] Sampling frequency
    const uint32_t bitsPerSample = readLe32(fmtChunk + 32);    // [DSF 2.2] Bits per sample (1 vagy 8)
    const uint64_t sampleCount = readLe64(fmtChunk + 36);      // [DSF 2.2] Sample count per channel
    const uint32_t blockSize = readLe32(fmtChunk + 44);        // [DSF 2.2] Block size per channel (4096)
    (void)channelType;

    if (fmtSize < 52 || fmtOff + fmtSize + 12 > fileSize) {
        error = "Érvénytelen DSF fmt chunk méret.";
        return false;
    }
    if (fmtVersion != 1 || fmtId != 0) {
        error = "Nem támogatott DSF verzió vagy tömörítés.";
        return false;
    }
    if (channelNum == 0 || channelNum > 8) {
        error = "Érvénytelen csatornaszám a DSF fájlban.";
        return false;
    }
    if (bitsPerSample != 1 && bitsPerSample != 8) {
        error = "Érvénytelen mintamélység a DSF fájlban (csak 1 vagy 8 bit támogatott).";
        return false;
    }
    if (blockSize == 0 || blockSize > (1u << 20)) {
        error = "Érvénytelen blokkméret a DSF fájlban.";
        return false;
    }
    uint32_t pcmRate = 0, dopRate = 0;
    if (!resolveDsdRates(sampleFreq, pcmRate, dopRate)) {
        error = "Nem támogatott DSD mintavételi frekvencia a DSF fájlban.";
        return false;
    }

    // [DSF Section 2.3] data chunk (12 bájtos fejléc + mintaadat)
    const uint64_t dataHdrOff = fmtOff + fmtSize;
    uint8_t dataHdr[12]{};
    if (!readAt(ctx, dataHdrOff, dataHdr, sizeof(dataHdr))) {
        error = "DSF data chunk olvasási hiba.";
        return false;
    }
    if (!tagEq(dataHdr, "data")) {
        error = "Érvénytelen DSF formátum (hiányzó 'data' chunk).";
        return false;
    }
    const uint64_t dataChunkSize = readLe64(dataHdr + 4); // [DSF 2.3] 12 + mintaadat méret
    if (dataChunkSize < 12) {
        error = "Érvénytelen DSF data chunk méret.";
        return false;
    }
    const uint64_t payloadOffset = dataHdrOff + 12;
    const uint64_t availPayload = fileSize - payloadOffset;
    const uint64_t payloadBytes = std::min<uint64_t>(dataChunkSize - 12, availPayload);

    // [DSF Section 2.2 & 2.3] Tényleges mintaszám lefedése a fizikai adatblokkból
    const uint64_t fullBlocks = payloadBytes / (static_cast<uint64_t>(blockSize) * channelNum);
    const uint64_t maxSamplesFromData = fullBlocks * blockSize * 8ULL;
    const uint64_t validSamples = std::min<uint64_t>(sampleCount, maxSamplesFromData);

    out = {};
    out.container = ContainerType::Dsf;
    out.dsdSampleRate = sampleFreq;
    out.pcmSampleRate = pcmRate;
    out.dopSampleRate = dopRate;
    out.channels = channelNum;
    out.bitsPerSample = bitsPerSample;
    out.blockSizePerChannel = blockSize;
    out.lsbFirst = (bitsPerSample == 1); // [DSF 2.2-2.3] 1: LSB-first per byte; 8: MSB-first
    out.sampleCount = validSamples;
    out.dataOffset = payloadOffset;
    out.dataBytes = payloadBytes;

    // [DSF Section 2.4] Metadata chunk (fájl végi ID3v2 blokk)
    if (metadataPtr >= payloadOffset + payloadBytes && metadataPtr < fileSize) {
        out.metadataOffset = metadataPtr;
        out.metadataMaxBytes = fileSize - metadataPtr;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 2. Philips DSDIFF v1.5 fejléc-értelmező [DFF 2.1, 3.1 - 3.5]
// ---------------------------------------------------------------------------

/**
 * Philips DSDIFF (.dff) fejlécének értelmezése. Minden szám big-endian, a chunkok
 * hossza fájlpozícióban páros bájtra igazított [DFF Section 2.1].
 * Csak a tömörítetlen 'DSD ' támogatott; 'DST ' tömörítésre érthető hibát ad.
 */
inline bool parseDsdiffHeader(ReadAtFn readAt, void* ctx, uint64_t fileSize, DsdFileInfo& out, std::string& error) {
    if (!readAt || fileSize < 16) {
        error = "Érvénytelen vagy csonka DSDIFF (.dff) fájl.";
        return false;
    }

    // [DFF Section 3.1] Form DSD Chunk ('FRM8', 8 bájtos méret, formType = 'DSD ')
    uint8_t frm[16]{};
    if (!readAt(ctx, 0, frm, sizeof(frm))) {
        error = "DSDIFF fejléc olvasási hiba.";
        return false;
    }
    if (!tagEq(frm, "FRM8") || !tagEq(frm + 12, "DSD ")) {
        error = "Nem DSDIFF fájl (hiányzó 'FRM8'/'DSD ' fejléc).";
        return false;
    }
    const uint64_t frmDataSize = readBe64(frm + 4);
    const uint64_t formEnd = std::min<uint64_t>(fileSize, (frmDataSize > fileSize ? fileSize : 12 + frmDataSize));

    bool hasProp = false, hasRate = false, hasCh = false, hasCmpr = false, hasData = false;
    uint32_t sampleRate = 0;
    uint16_t numChannels = 0;
    bool isDst = false;
    std::string cmprId;
    uint64_t dataOffset = 0, dataBytes = 0;

    // Alchunkok bejárása FRM8-on belül (offset 16-tól formEnd-ig)
    uint64_t pos = 16;
    while (pos + 12 <= formEnd) {
        uint8_t chHdr[12]{};
        if (!readAt(ctx, pos, chHdr, sizeof(chHdr))) break;
        const uint64_t ckSize = readBe64(chHdr + 4); // [DFF 2.1] ckDataSize (uint64 BE)
        const uint64_t ckDataOff = pos + 12;
        if (ckSize > formEnd - ckDataOff) break;
        const uint64_t ckNext = ckDataOff + ckSize + (ckSize & 1ULL); // [DFF 2.1] páros igazítás

        if (tagEq(chHdr, "PROP") && ckSize >= 4) {
            // [DFF Section 3.3] Property Chunk (propType == 'SND ')
            uint8_t propType[4]{};
            if (readAt(ctx, ckDataOff, propType, 4) && tagEq(propType, "SND ")) {
                hasProp = true;
                uint64_t ppos = ckDataOff + 4;
                const uint64_t propEnd = ckDataOff + ckSize;
                while (ppos + 12 <= propEnd) {
                    uint8_t subHdr[12]{};
                    if (!readAt(ctx, ppos, subHdr, sizeof(subHdr))) break;
                    const uint64_t subSize = readBe64(subHdr + 4);
                    const uint64_t subOff = ppos + 12;
                    if (subSize > propEnd - subOff) break;
                    const uint64_t subNext = subOff + subSize + (subSize & 1ULL);

                    if (tagEq(subHdr, "FS  ") && subSize >= 4) {
                        // [DFF Section 3.3.1] Sample Rate Chunk (uint32 BE)
                        uint8_t buf[4]{};
                        if (readAt(ctx, subOff, buf, 4)) {
                            sampleRate = readBe32(buf);
                            hasRate = true;
                        }
                    } else if (tagEq(subHdr, "CHNL") && subSize >= 2) {
                        // [DFF Section 3.3.2] Channels Chunk (uint16 BE numChannels + 4*numChannels char ID)
                        uint8_t buf[2]{};
                        if (readAt(ctx, subOff, buf, 2)) {
                            numChannels = readBe16(buf);
                            hasCh = true;
                        }
                    } else if (tagEq(subHdr, "CMPR") && subSize >= 4) {
                        // [DFF Section 3.3.3] Compression Type Chunk ('DSD ' vagy 'DST ' + pstring)
                        uint8_t buf[4]{};
                        if (readAt(ctx, subOff, buf, 4)) {
                            cmprId.assign(reinterpret_cast<const char*>(buf), 4);
                            hasCmpr = true;
                            if (tagEq(buf, "DST ")) isDst = true;
                        }
                    }
                    ppos = subNext;
                }
            }
        } else if (tagEq(chHdr, "DSD ")) {
            // [DFF Section 3.4] DSD Sound Data Chunk (tömörítetlen, 1 bájt/csatorna összefésülve, MSB-first)
            if (!hasData) {
                hasData = true;
                dataOffset = ckDataOff;
                dataBytes = ckSize;
            }
        } else if (tagEq(chHdr, "DST ")) {
            // [DFF Section 3.5] DST Sound Data Chunk (veszteségmentes DST tömörítés)
            isDst = true;
        }

        if (ckNext <= pos) break;
        pos = ckNext;
    }

    if (isDst) {
        error = "A DST-tömörített DSDIFF (.dff) fájlok nem támogatottak (csak tömörítetlen DSDIFF játszható le).";
        return false;
    }
    if (!hasProp || !hasRate || !hasCh) {
        error = "Hiányos DSDIFF fejléc (hiányzó PROP/FS/CHNL tulajdonságok).";
        return false;
    }
    if (hasCmpr && cmprId != "DSD ") {
        error = "Nem támogatott tömörítés a DSDIFF fájlban: '" + cmprId + "'.";
        return false;
    }
    if (!hasData || dataBytes == 0) {
        error = "Hiányzó vagy üres DSD adatblokk a DSDIFF fájlban.";
        return false;
    }
    if (numChannels == 0 || numChannels > 8) {
        error = "Érvénytelen csatornaszám a DSDIFF fájlban.";
        return false;
    }
    uint32_t pcmRate = 0, dopRate = 0;
    if (!resolveDsdRates(sampleRate, pcmRate, dopRate)) {
        error = "Nem támogatott DSD mintavételi frekvencia a DSDIFF fájlban.";
        return false;
    }

    const uint64_t bytesPerCh = dataBytes / numChannels;
    out = {};
    out.container = ContainerType::Dsdiff;
    out.dsdSampleRate = sampleRate;
    out.pcmSampleRate = pcmRate;
    out.dopSampleRate = dopRate;
    out.channels = numChannels;
    out.bitsPerSample = 8;        // [DFF 3.4] bájtonként 8 bit, MSB-first
    out.blockSizePerChannel = 1;  // [DFF 3.4] 1 bájt/csatorna felváltva
    out.lsbFirst = false;
    out.sampleCount = bytesPerCh * 8ULL;
    out.dataOffset = dataOffset;
    out.dataBytes = dataBytes;
    return true;
}

// ---------------------------------------------------------------------------
// 3. DoP v1.1 csomagoló és kibontó [DoP Section 2.1 - 2.2]
// ---------------------------------------------------------------------------

/**
 * Egy DoP 1.1 24 bites PCM szó összeállítása két időrendben egymást követő MSB-first DSD bájtból
 * (b0 tartalmazza t0..t7 biteket, b0 bit7 = t0; b1 tartalmazza t8..t15 biteket, b1 bit0 = t15).
 * Az eredmény 32 bites egészben az alsó 24 biten áll:
 *   bits [23:16] = marker (0x05 vagy 0xFA, keretenként váltakozva, minden csatornán azonos)
 *   bits [15:8]  = b0 (időben első 8 DSD bit, t0 a 15. biten)
 *   bits [7:0]   = b1 (következő 8 DSD bit, t15 a 0. biten)
 */
inline int32_t packDopSample(uint8_t b0Msb, uint8_t b1Msb, uint8_t marker) {
    return static_cast<int32_t>((static_cast<uint32_t>(marker) << 16) |
                                (static_cast<uint32_t>(b0Msb) << 8) |
                                static_cast<uint32_t>(b1Msb));
}

/** DoP 24 bites PCM szó kibontása (teszteléshez és ellenőrzéshez). */
inline void unpackDopSample(int32_t pcm24, uint8_t& outMarker, uint8_t& outB0Msb, uint8_t& outB1Msb) {
    const uint32_t u = static_cast<uint32_t>(pcm24);
    outMarker = static_cast<uint8_t>((u >> 16) & 0xFFu);
    outB0Msb = static_cast<uint8_t>((u >> 8) & 0xFFu);
    outB1Msb = static_cast<uint8_t>(u & 0xFFu);
}

/**
 * Többcsatornás, csatornánként bájtonként váltakozó MSB-first DSD puffer csomagolása DoP 1.1 keretekbe.
 * @param dsdInterleavedMsb  bemenet: `frames * channels * 2` bájt (keretenként 2 DSD bájt / csatorna)
 * @param frames             DoP keretek száma (1 DoP keret = 16 DSD bit / csatorna)
 * @param channels           csatornaszám
 * @param startPhase         0 -> első keret jelölője 0x05; 1 -> 0xFA (majd váltakozik)
 * @param dopPcmOut          kimenet: `frames * channels` db 32 bites egészben tárolt 24 bites DoP minta
 * @return                   a következő kerethez tartozó fázis (0 vagy 1)
 */
inline uint8_t packDopFrames(const uint8_t* dsdInterleavedMsb, size_t frames, uint32_t channels,
                             uint8_t startPhase, int32_t* dopPcmOut) {
    uint8_t phase = startPhase & 1u;
    for (size_t f = 0; f < frames; ++f) {
        const uint8_t marker = (phase == 0) ? 0x05u : 0xFAu;
        const size_t base = f * static_cast<size_t>(channels);
        for (uint32_t c = 0; c < channels; ++c) {
            const uint8_t b0 = dsdInterleavedMsb[(base + c) * 2u];
            const uint8_t b1 = dsdInterleavedMsb[(base + c) * 2u + 1u];
            dopPcmOut[base + c] = packDopSample(b0, b1, marker);
        }
        phase ^= 1u;
    }
    return phase;
}

// ---------------------------------------------------------------------------
// 4. Nyers DSD bájtforrás (egységes DSF/DSDIFF olvasó, DoP és FIR számára egyaránt)
// ---------------------------------------------------------------------------

/** DSD csendbájt (váltakozó +1/-1 bit -> 0 DC, Nyquist frekvencián). */
inline constexpr uint8_t kDsdSilenceByte = 0xAAu;

/**
 * Összefüggő (csatornánként felbontott) DSD bájtok olvasására alkalmas általános adatfolyam-kezelő.
 * Minden kiadott bájt MSB-first formátumú (bit 7 a legkorábbi időbeli DSD minta).
 */
class DsdRawStream {
public:
    bool init(const DsdFileInfo& info, ReadAtFn readAt, void* ctx) {
        info_ = info;
        readAt_ = readAt;
        ctx_ = ctx;
        totalChannelBytes_ = (info_.sampleCount + 7ULL) / 8ULL;
        bytePos_ = 0;
        if (info_.container == ContainerType::Dsf) {
            blockBuf_.resize(static_cast<size_t>(info_.blockSizePerChannel) * info_.channels);
        } else {
            // DSDIFF esetén több keretet olvasunk egyszerre az I/O csökkentésére
            interleaveChunkBytes_ = 4096u * info_.channels;
            blockBuf_.resize(interleaveChunkBytes_);
        }
        cachedBlock_ = UINT64_MAX;
        return info_.channels > 0 && readAt_ != nullptr;
    }

    const DsdFileInfo& info() const { return info_; }
    uint64_t totalChannelBytes() const { return totalChannelBytes_; }
    uint64_t bytePosition() const { return bytePos_; }

    bool seekByte(uint64_t chByteOffset) {
        bytePos_ = std::min(chByteOffset, totalChannelBytes_);
        return true;
    }

    /**
     * Legfeljebb `requestedBytes` csatornánkénti DSD bájtot olvas.
     * Kimenet: `chBytesOut[ch][i]` (MSB-first, többletbiteknél a fájl végén `kDsdSilenceByte` kiegészítés nélkül —
     * a visszatérési érték a ténylegesen beolvasott bájt/csatorna mennyiség).
     */
    size_t readChannelBytes(uint8_t* dstInterleavedMsb, size_t requestedBytes) {
        if (bytePos_ >= totalChannelBytes_ || requestedBytes == 0) return 0;
        const size_t avail = static_cast<size_t>(std::min<uint64_t>(requestedBytes, totalChannelBytes_ - bytePos_));
        const uint32_t ch = info_.channels;

        if (info_.container == ContainerType::Dsf) {
            const uint32_t bsz = info_.blockSizePerChannel;
            size_t done = 0;
            while (done < avail) {
                const uint64_t cur = bytePos_ + done;
                const uint64_t blkIdx = cur / bsz;
                const size_t inBlk = static_cast<size_t>(cur % bsz);
                const size_t take = std::min(avail - done, static_cast<size_t>(bsz - inBlk));
                if (cachedBlock_ != blkIdx) {
                    const uint64_t fileOff = info_.dataOffset + blkIdx * static_cast<uint64_t>(bsz) * ch;
                    if (!readAt_(ctx_, fileOff, blockBuf_.data(), blockBuf_.size())) break;
                    cachedBlock_ = blkIdx;
                }
                for (size_t i = 0; i < take; ++i) {
                    uint8_t* dstFrame = dstInterleavedMsb + (done + i) * ch;
                    for (uint32_t c = 0; c < ch; ++c) {
                        uint8_t raw = blockBuf_[static_cast<size_t>(c) * bsz + inBlk + i];
                        dstFrame[c] = info_.lsbFirst ? reverseBits8(raw) : raw;
                    }
                }
                done += take;
            }
            // Utolsó részleges bájt maszkmentes átadása: ha sampleCount nem osztható 8-cal, a végső bitek 0/pad helyett kDsdSilenceByte-ra cserélhetők
            bytePos_ += done;
            return done;
        }

        // DSDIFF: 1 bájt / csatorna felváltva, már eleve MSB-first
        size_t done = 0;
        const size_t bytesPerStep = 4096u;
        while (done < avail) {
            const uint64_t cur = bytePos_ + done;
            const uint64_t blkIdx = cur / bytesPerStep;
            const size_t inBlk = static_cast<size_t>(cur % bytesPerStep);
            const size_t take = std::min(avail - done, bytesPerStep - inBlk);
            if (cachedBlock_ != blkIdx) {
                const uint64_t fileOff = info_.dataOffset + blkIdx * bytesPerStep * ch;
                const uint64_t remInData = (info_.dataOffset + info_.dataBytes > fileOff)
                                               ? (info_.dataOffset + info_.dataBytes - fileOff)
                                               : 0;
                const size_t toRead = static_cast<size_t>(std::min<uint64_t>(blockBuf_.size(), remInData));
                std::fill(blockBuf_.begin(), blockBuf_.end(), kDsdSilenceByte);
                if (toRead > 0 && !readAt_(ctx_, fileOff, blockBuf_.data(), toRead)) break;
                cachedBlock_ = blkIdx;
            }
            std::memcpy(dstInterleavedMsb + done * ch, blockBuf_.data() + inBlk * ch, take * ch);
            done += take;
        }
        bytePos_ += done;
        return done;
    }

private:
    DsdFileInfo info_{};
    ReadAtFn readAt_ = nullptr;
    void* ctx_ = nullptr;
    uint64_t totalChannelBytes_ = 0;
    uint64_t bytePos_ = 0;
    uint64_t cachedBlock_ = UINT64_MAX;
    size_t interleaveChunkBytes_ = 0;
    std::vector<uint8_t> blockBuf_;
};

// ---------------------------------------------------------------------------
// 5. DSD → PCM Kaiser-ablakos sinc FIR szűrő és 8 bites LUT tizedelő
// ---------------------------------------------------------------------------

namespace detail {
inline double besselI0(double x) {
    double sum = 1.0;
    double term = 1.0;
    const double x2 = 0.25 * x * x;
    for (int k = 1; k <= 40; ++k) {
        term *= x2 / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-15) break;
    }
    return sum;
}
} // namespace detail

/**
 * DSD → PCM valós idejű tizedelő FIR szűrő (8 bites bájt-LUT gyorsítással).
 *
 * Matematikai modell:
 *  - A szűrő együtthatóit a kód számolja: Kaiser-ablakos ideális aluláteresztő sinc
 *    (f_c = 31 kHz, átmenet 20..42 kHz, célcsillapítás 105 dB -> béta = 10,61).
 *  - A szűrő hossza T_bytes * 8 DSD minta; egy PCM minta előállításához D / 8 új bájt lép be.
 *  - Minden 8 bitnyi csoporthoz (minden szűrőbájthoz) 256 előre számolt összeget tárolunk (±1.0 súlyokkal),
 *    így a tizedeléshez sem bitművelet, sem szorzás nem kell futás közben.
 */
class DsdToPcmConverter {
public:
    static constexpr double kCutoffHz = 31000.0;
    static constexpr double kKaiserBeta = 10.61; // ~105 dB zárósáv-csillapítás
    /**
     * Kimeneti erősítés: ×2 (+6,02 dB). A SACD-szabvány szerinti referenciaszint (0 dB SACD) az 50%-os modulációs
     * mélység; ×2-vel ez épp a PCM teljes skálája (0 dBFS) lesz – a DSD→PCM átalakítók szokásos beállítása.
     * A kimenet lebegőpontos: az 50% fölötti (a szabvány szerint megengedett) csúcsok 1,0 fölé mehetnek, itt nincs
     * vágás; a további lánc (hangerő, limiter) kezeli. A FIR maga egységnyi erősítésű (a coeffs_ összege 1,0).
     */
    static constexpr double kOutputGain = 2.0;

    bool init(uint32_t dsdSampleRate, uint32_t pcmSampleRate, uint32_t channels) {
        if (channels == 0 || pcmSampleRate == 0 || dsdSampleRate % (8u * pcmSampleRate) != 0) return false;
        dsdRate_ = dsdSampleRate;
        pcmRate_ = pcmSampleRate;
        channels_ = channels;
        decimFactor_ = dsdSampleRate / pcmSampleRate;
        bytesPerPcmFrame_ = decimFactor_ / 8u; // pl. DSD64 -> 32 / 8 = 4 bájt

        // Szimmetrikus FIR hossza: Kaiser képlet szerint Δω = 2π·22000 / dsdRate -> ~867 * (dsdRate / 2822400).
        // 29 PCM keret (DSD64-nél 29 * 32 = 928 tap, 116 bájt) biztosítja a páratlan keretszámú, nulla fázistolású
        // középpontozást (14 keret előnézés + 1 középső keret + 14 keret utónézés).
        constexpr uint32_t kFilterPcmSpan = 29u;
        filterBytes_ = kFilterPcmSpan * bytesPerPcmFrame_;
        halfFilterBytes_ = ((kFilterPcmSpan - 1u) / 2u) * bytesPerPcmFrame_;
        const size_t taps = static_cast<size_t>(filterBytes_) * 8u;

        coeffs_.assign(taps, 0.0);
        const double fcNorm = kCutoffHz / static_cast<double>(dsdSampleRate); // [0..0.5]
        const double center = 0.5 * static_cast<double>(taps - 1);
        const double denom = detail::besselI0(kKaiserBeta);
        constexpr double kPi = 3.14159265358979323846;

        double sum = 0.0;
        for (size_t n = 0; n < taps; ++n) {
            const double d = static_cast<double>(n) - center;
            const double sinc = (std::abs(d) < 1e-12) ? (2.0 * fcNorm) : (std::sin(2.0 * kPi * fcNorm * d) / (kPi * d));
            const double r = d / center;
            const double w = detail::besselI0(kKaiserBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / denom;
            const double h = sinc * w;
            coeffs_[n] = h;
            sum += h;
        }
        // Egységnyi DC erősítésű FIR (a +6 dB-es kimeneti erősítés a LUT-ban: kOutputGain)
        for (double& h : coeffs_) h /= sum;

        // 8 bites LUT feltöltése (MSB-first bájtsorrendre: bit 7 = legkorábbi minta az ablak adott bájtjában)
        lut_.assign(static_cast<size_t>(filterBytes_) * 256u, 0.0f);
        for (uint32_t b = 0; b < filterBytes_; ++b) {
            const double* hb = &coeffs_[static_cast<size_t>(b) * 8u];
            float* table = &lut_[static_cast<size_t>(b) * 256u];
            for (int val = 0; val < 256; ++val) {
                double acc = 0.0;
                for (int bit = 0; bit < 8; ++bit) {
                    const bool one = ((val >> (7 - bit)) & 1) != 0;
                    acc += one ? hb[bit] : -hb[bit];
                }
                table[val] = static_cast<float>(acc * kOutputGain);
            }
        }

        history_.assign(static_cast<size_t>(channels_) * filterBytes_, kDsdSilenceByte);
        histHead_ = 0;
        return true;
    }

    uint32_t bytesPerPcmFrame() const { return bytesPerPcmFrame_; }
    uint32_t filterBytes() const { return filterBytes_; }
    uint32_t halfFilterBytes() const { return halfFilterBytes_; }
    const std::vector<double>& coeffs() const { return coeffs_; }

    /** Szűrőállapot alaphelyzetbe állítása DSD csenddel (tekerés előtt vagy fájl elején). */
    void reset() {
        std::fill(history_.begin(), history_.end(), kDsdSilenceByte);
        histHead_ = 0;
    }

    /** Egy `bytesPerPcmFrame()` bájt/csatorna előzetes betöltése a gyűrűpufferbe (kimenet nélkül, pre-rollhoz). */
    void pushSilenceBytes(uint32_t chBytesCount) {
        for (uint32_t i = 0; i < chBytesCount; ++i) {
            for (uint32_t c = 0; c < channels_; ++c) {
                history_[static_cast<size_t>(c) * filterBytes_ + histHead_] = kDsdSilenceByte;
            }
            histHead_ = (histHead_ + 1u) % filterBytes_;
        }
    }

    /** `chBytesCount` bájtnyi (interleaved MSB-first) adat betöltése kimenet generálása nélkül. */
    void pushBytesOnly(const uint8_t* interleavedMsb, size_t chBytesCount) {
        for (size_t i = 0; i < chBytesCount; ++i) {
            const uint8_t* src = interleavedMsb + i * channels_;
            for (uint32_t c = 0; c < channels_; ++c) {
                history_[static_cast<size_t>(c) * filterBytes_ + histHead_] = src[c];
            }
            histHead_ = (histHead_ + 1u) % filterBytes_;
        }
    }

    /**
     * `pcmFrames * bytesPerPcmFrame()` csatornánkénti DSD bájt feldolgozása -> `pcmFrames` PCM minta/csatorna.
     * @param dsdInterleavedMsb  bemenet: `pcmFrames * bytesPerPcmFrame() * channels` bájt (MSB-first)
     * @param pcmFrames          legyártandó PCM keretek száma
     * @param pcmOut             kimenet: `pcmFrames * channels` float (-1.0 .. +1.0)
     */
    void process(const uint8_t* dsdInterleavedMsb, size_t pcmFrames, float* pcmOut) {
        const size_t step = bytesPerPcmFrame_;
        for (size_t f = 0; f < pcmFrames; ++f) {
            // 1. Új keret (`step` bájt/csatorna) betolása a gyűrűpufferbe
            for (size_t s = 0; s < step; ++s) {
                const uint8_t* src = dsdInterleavedMsb + (f * step + s) * channels_;
                for (uint32_t c = 0; c < channels_; ++c) {
                    history_[static_cast<size_t>(c) * filterBytes_ + histHead_] = src[c];
                }
                histHead_ = (histHead_ + 1u) % filterBytes_;
            }

            // 2. Konvolúció kiértékelése a gyűrűpufferen a 8 bites LUT-tal
            // Mivel a LUT sorai h[b*8 .. b*8+7] időrendben szimmetrikusak, az `histHead_` az ablak végét (legújabb)
            // jelöli, így `idx = (histHead_ + b) % filterBytes_` a legrégebbi bájttól a legújabbig halad.
            float* outFrame = pcmOut + f * channels_;
            for (uint32_t c = 0; c < channels_; ++c) {
                const uint8_t* chHist = &history_[static_cast<size_t>(c) * filterBytes_];
                double acc = 0.0;
                uint32_t idx = histHead_;
                for (uint32_t b = 0; b < filterBytes_; ++b) {
                    acc += lut_[static_cast<size_t>(b) * 256u + chHist[idx]];
                    if (++idx == filterBytes_) idx = 0;
                }
                outFrame[c] = static_cast<float>(acc);
            }
        }
    }

private:
    uint32_t dsdRate_ = 0;
    uint32_t pcmRate_ = 0;
    uint32_t channels_ = 0;
    uint32_t decimFactor_ = 0;
    uint32_t bytesPerPcmFrame_ = 0;
    uint32_t filterBytes_ = 0;
    uint32_t halfFilterBytes_ = 0;
    uint32_t histHead_ = 0;
    std::vector<double> coeffs_;
    std::vector<float> lut_;
    std::vector<uint8_t> history_;
};

/**
 * Nyers MSB-first DSD bájtforrás fájl alapú megnyitáshoz (DoP kimenet számára; a lejátszóba még nincs bekötve).
 * Implementáció: src/audio_decoder.cpp.
 */
class DsdRawSource {
public:
    virtual ~DsdRawSource() = default;
    virtual const DsdFileInfo& info() const = 0;
    virtual uint64_t totalChannelBytes() const = 0;
    /** Csatornánként legfeljebb `channelBytes` bájt olvasása MSB-first sorrendben (interleaved: ch0, ch1, ...). */
    virtual size_t read(uint8_t* dstInterleavedMsb, size_t channelBytes) = 0;
    virtual bool seekByte(uint64_t chByteOffset) = 0;
};

/**
 * DSD (.dsf / .dff) fájl megnyitása nyers DSD bájt-olvasásra (DoP-csomagoláshoz).
 * Hibás, csonka vagy DST-tömörített fájl esetén nullptr-t ad vissza és kitölti az `error`-t.
 */
std::unique_ptr<DsdRawSource> openDsdRawFile(const std::string& path, std::string* error = nullptr);

} // namespace ee_analyzer::dsd
