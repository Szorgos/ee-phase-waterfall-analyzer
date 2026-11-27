/**
 * @file audio_decoder.cpp
 * @brief A beépített lejátszó dekóderei (FLAC, WAV, MP3, Ogg Vorbis) és a címkék (Vorbis-megjegyzés, ID3v2, FLAC
 *        PICTURE) feldolgozása. A könyvtárak megvalósítását a src/decoders_impl.c fordítja.
 */
#include "audio_decoder.hpp"
#include "dsd.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#include "alac/ALACDecoder.h"
#include "alac/ALACBitUtilities.h"
#include "neaacdec.h"
#include "opusfile.h"
#include "wavpack.h"

namespace ee_analyzer::player {

namespace {

constexpr size_t kMaxArtBytes = 8u << 20;   // ennél nagyobb borítót nem tartunk meg

#ifdef _WIN32
/** UTF-8 útvonal Windows-os széles karakteres alakja (a char* fájlmegnyitás az ékezetes nevet nem kezeli). */
std::wstring widePath(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    w.resize(static_cast<size_t>(n - 1));
    return w;
}
#endif

std::string lowerExt(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string e = path.substr(dot + 1);
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]); }
uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint64_t be64(const uint8_t* p) { return (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4); }
uint32_t synchsafe(const uint8_t* p) { return (uint32_t(p[0] & 0x7f) << 21) | (uint32_t(p[1] & 0x7f) << 14) | (uint32_t(p[2] & 0x7f) << 7) | (p[3] & 0x7f); }

bool fileSeek64(FILE* f, uint64_t off, int whence) {
#ifdef _WIN32
    return _fseeki64(f, static_cast<__int64>(off), whence) == 0;
#else
    return fseeko(f, static_cast<off_t>(off), whence) == 0;
#endif
}

uint64_t fileTell64(FILE* f) {
#ifdef _WIN32
    const __int64 p = _ftelli64(f);
#else
    const off_t p = ftello(f);
#endif
    return p > 0 ? static_cast<uint64_t>(p) : 0;
}

FILE* openBinaryFile(const std::string& path) {
#ifdef _WIN32
    return _wfopen(widePath(path).c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

/** Érvényes UTF-8-e, és van-e benne nem ASCII karakter (a „Latin-1”-nek jelölt, de UTF-8 címkék felismeréséhez). */
bool looksUtf8(const uint8_t* p, size_t n) {
    bool multi = false;
    for (size_t i = 0; i < n && p[i];) {
        const uint8_t c = p[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (len == 0 || i + len > n) return false;
        for (size_t k = 1; k < len; ++k)
            if ((p[i + k] & 0xC0) != 0x80) return false;
        multi |= len > 1;
        i += len;
    }
    return multi;
}

/**
 * ID3 szöveg (kódolás: 0 Latin-1, 1 UTF-16 BOM-mal, 2 UTF-16BE, 3 UTF-8) UTF-8-ra, az első NUL-ig. Sok program
 * (pl. a libsndfile / LAME) UTF-8 bájtokat ír a Latin-1-nek jelölt mezőbe: ha a bájtok érvényes UTF-8-at adnak, úgy
 * olvassuk (valódi Latin-1 ékezetes szövegnél ez gyakorlatilag nem fordul elő).
 */
std::string id3Text(uint8_t enc, const uint8_t* p, size_t n) {
    std::string out;
    if (enc == 0 && !looksUtf8(p, n)) {
        for (size_t i = 0; i < n && p[i]; ++i) appendUtf8(out, p[i]);
    } else if (enc == 0 || enc == 3) {
        size_t len = 0;
        while (len < n && p[len]) ++len;
        out.assign(reinterpret_cast<const char*>(p), len);
    } else if (enc == 1 || enc == 2) {
        bool bigEndian = enc == 2;
        size_t i = 0;
        if (enc == 1 && n >= 2) {
            if (p[0] == 0xFE && p[1] == 0xFF) { bigEndian = true; i = 2; }
            else if (p[0] == 0xFF && p[1] == 0xFE) { bigEndian = false; i = 2; }
        }
        for (; i + 1 < n; i += 2) {
            uint32_t u = bigEndian ? (uint32_t(p[i]) << 8 | p[i + 1]) : (uint32_t(p[i + 1]) << 8 | p[i]);
            if (u == 0) break;
            if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {     // pótlólagos pár (pl. emoji)
                const uint32_t lo = bigEndian ? (uint32_t(p[i + 2]) << 8 | p[i + 3]) : (uint32_t(p[i + 3]) << 8 | p[i + 2]);
                if (lo >= 0xDC00 && lo < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
            }
            appendUtf8(out, u);
        }
    }
    while (!out.empty() && (out.back() == ' ')) out.pop_back();
    return out;
}

/** A NUL-lal lezárt mező hossza a kódolás szerint (UTF-16-nál két nulla bájt, páros helyen), a lezáróval együtt. */
size_t id3FieldLen(uint8_t enc, const uint8_t* p, size_t n) {
    if (enc == 1 || enc == 2) {
        for (size_t i = 0; i + 1 < n; i += 2)
            if (p[i] == 0 && p[i + 1] == 0) return i + 2;
        return n;
    }
    for (size_t i = 0; i < n; ++i)
        if (p[i] == 0) return i + 1;
    return n;
}

/** Borító megtartása: az első, vagy ha az új az előlap (3), az előlap. */
void offerArt(TrackTags& t, uint32_t& keptType, uint32_t type, std::string mime, const uint8_t* data, size_t size) {
    if (size == 0 || size > kMaxArtBytes) return;
    if (!t.art.empty() && !(type == 3 && keptType != 3)) return;
    if (mime.empty() || mime == "-->") return;                     // a "-->" link, nem kép
    for (char& c : mime) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (mime == "jpg") mime = "image/jpeg";
    else if (mime == "png") mime = "image/png";
    else if (mime.find('/') == std::string::npos) mime = "image/" + mime;
    t.artMime = mime;
    t.art.assign(reinterpret_cast<const char*>(data), size);
    keptType = type;
}

std::vector<uint8_t> base64Decode(const char* s, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else continue;   // = és szóköz
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(static_cast<uint8_t>(acc >> bits)); }
    }
    return out;
}

bool startsWithNoCase(const char* s, size_t len, const char* key) {
    const size_t k = std::strlen(key);
    if (len < k) return false;
    for (size_t i = 0; i < k; ++i)
        if (std::toupper(static_cast<unsigned char>(s[i])) != key[i]) return false;
    return true;
}

// ---------------------------------------------------------------- dekóderek

class WavDecoder : public AudioDecoder {
public:
    ~WavDecoder() override { if (ok_) drwav_uninit(&wav_); }
    bool open(const std::string& path) {
#ifdef _WIN32
        ok_ = drwav_init_file_w(&wav_, widePath(path).c_str(), nullptr);
#else
        ok_ = drwav_init_file(&wav_, path.c_str(), nullptr);
#endif
        return ok_ && wav_.channels > 0;
    }
    uint32_t sampleRate() const override { return wav_.sampleRate; }
    uint32_t channels() const override { return wav_.channels; }
    uint32_t bitsPerSample() const override { return wav_.bitsPerSample; }
    uint64_t totalFrames() const override { return wav_.totalPCMFrameCount; }
    size_t read(float* out, size_t frames) override { return static_cast<size_t>(drwav_read_pcm_frames_f32(&wav_, frames, out)); }
    bool seek(uint64_t frame) override { return drwav_seek_to_pcm_frame(&wav_, frame); }

private:
    drwav wav_{};
    bool ok_ = false;
};

class FlacDecoder : public AudioDecoder {
public:
    ~FlacDecoder() override { if (flac_) drflac_close(flac_); }
    bool open(const std::string& path) {
#ifdef _WIN32
        flac_ = drflac_open_file_with_metadata_w(widePath(path).c_str(), &FlacDecoder::onMeta, this, nullptr);
#else
        flac_ = drflac_open_file_with_metadata(path.c_str(), &FlacDecoder::onMeta, this, nullptr);
#endif
        detail::finalizeTags(tags_);
        return flac_ && flac_->channels > 0;
    }
    uint32_t sampleRate() const override { return flac_->sampleRate; }
    uint32_t channels() const override { return flac_->channels; }
    uint32_t bitsPerSample() const override { return flac_->bitsPerSample; }
    uint64_t totalFrames() const override { return flac_->totalPCMFrameCount; }
    size_t read(float* out, size_t frames) override { return static_cast<size_t>(drflac_read_pcm_frames_f32(flac_, frames, out)); }
    bool seek(uint64_t frame) override { return drflac_seek_to_pcm_frame(flac_, frame); }

private:
    static void onMeta(void* user, drflac_metadata* m) {
        auto* self = static_cast<FlacDecoder*>(user);
        if (m->type == DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) {
            drflac_vorbis_comment_iterator it;
            drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount, m->data.vorbis_comment.pComments);
            drflac_uint32 len = 0;
            while (const char* c = drflac_next_vorbis_comment(&it, &len)) detail::applyVorbisComment(c, len, self->tags_);
        } else if (m->type == DRFLAC_METADATA_BLOCK_TYPE_PICTURE && m->data.picture.pPictureData) {
            offerArt(self->tags_, self->artType_, m->data.picture.type,
                     std::string(m->data.picture.mime, m->data.picture.mimeLength), m->data.picture.pPictureData,
                     m->data.picture.pictureDataSize);
        }
    }
    drflac* flac_ = nullptr;
    uint32_t artType_ = 0;
};

class Mp3Decoder : public AudioDecoder {
public:
    ~Mp3Decoder() override { if (ok_) drmp3_uninit(&mp3_); }
    bool open(const std::string& path, bool probe) {
#ifdef _WIN32
        ok_ = drmp3_init_file_with_metadata_w(&mp3_, widePath(path).c_str(), &Mp3Decoder::onMeta, this, nullptr);
#else
        ok_ = drmp3_init_file_with_metadata(&mp3_, path.c_str(), &Mp3Decoder::onMeta, this, nullptr);
#endif
        if (!ok_ || mp3_.channels == 0) return false;
        // az ID3v1 (a fájl végén, a dr_mp3 ezt jelzi elsőként) csak tartalék: az ID3v2 pontosabb (Unicode, hosszabb)
        if (tags_.title.empty()) tags_.title = v1_.title;
        if (tags_.artist.empty()) tags_.artist = v1_.artist;
        if (tags_.album.empty()) tags_.album = v1_.album;
        if (tags_.genre.empty()) tags_.genre = v1_.genre;
        detail::finalizeTags(tags_);
        if (probe) {
            // zenetár: a hossz csak a Xing / Info fejlécből (a LAME mindig írja); különben ismeretlen, a fájl
            // végigolvasása nélkül
            // (ismert hossznál a drmp3_get_pcm_frame_count nem olvas, csak levonja a kódoló elő- és utótöltését)
            total_ = mp3_.totalPCMFrameCount != DRMP3_UINT64_MAX ? drmp3_get_pcm_frame_count(&mp3_) : 0;
            return true;
        }
        total_ = drmp3_get_pcm_frame_count(&mp3_);   // Xing / Info nélkül a fájl végigolvasása (csak a keretfejlécek)
        drmp3_seek_to_pcm_frame(&mp3_, 0);
        return true;
    }
    uint32_t sampleRate() const override { return mp3_.sampleRate; }
    uint32_t channels() const override { return mp3_.channels; }
    uint64_t totalFrames() const override { return total_; }
    size_t read(float* out, size_t frames) override { return static_cast<size_t>(drmp3_read_pcm_frames_f32(&mp3_, frames, out)); }
    bool seek(uint64_t frame) override { return drmp3_seek_to_pcm_frame(&mp3_, frame); }

private:
    static void onMeta(void* user, const drmp3_metadata* m) {
        auto* self = static_cast<Mp3Decoder*>(user);
        if (m->type == DRMP3_METADATA_TYPE_ID3V2) {
            detail::parseId3v2(static_cast<const uint8_t*>(m->pRawData), m->rawDataSize, self->tags_);
        } else if (m->type == DRMP3_METADATA_TYPE_ID3V1 && m->rawDataSize >= 128) {
            // ID3v1 (Latin-1, rögzített hosszú mezők): tartalék, az open() csak az üres mezőkbe teszi
            const auto* p = static_cast<const uint8_t*>(m->pRawData);
            self->v1_.title = id3Text(0, p + 3, 30);
            self->v1_.artist = id3Text(0, p + 33, 30);
            self->v1_.album = id3Text(0, p + 63, 30);
            self->v1_.genre = detail::id3v1Genre(p[127]);
        }
    }
    drmp3 mp3_{};
    TrackTags v1_;
    bool ok_ = false;
    uint64_t total_ = 0;
};

class VorbisDecoder : public AudioDecoder {
public:
    ~VorbisDecoder() override { if (v_) stb_vorbis_close(v_); }
    bool open(const std::string& path) {
        int err = 0;
#ifdef _WIN32
        FILE* f = _wfopen(widePath(path).c_str(), L"rb");
        v_ = f ? stb_vorbis_open_file(f, 1, &err, nullptr) : nullptr;   // 1: a fájlt a stb_vorbis_close zárja
        if (!v_ && f) std::fclose(f);
#else
        v_ = stb_vorbis_open_filename(path.c_str(), &err, nullptr);
#endif
        if (!v_) return false;
        const stb_vorbis_info info = stb_vorbis_get_info(v_);
        rate_ = info.sample_rate;
        ch_ = static_cast<uint32_t>(info.channels);
        total_ = stb_vorbis_stream_length_in_samples(v_);
        const stb_vorbis_comment c = stb_vorbis_get_comment(v_);
        for (int i = 0; i < c.comment_list_length; ++i)
            if (c.comment_list[i]) detail::applyVorbisComment(c.comment_list[i], std::strlen(c.comment_list[i]), tags_);
        detail::finalizeTags(tags_);
        return ch_ > 0 && rate_ > 0;
    }
    uint32_t sampleRate() const override { return rate_; }
    uint32_t channels() const override { return ch_; }
    uint64_t totalFrames() const override { return total_; }
    size_t read(float* out, size_t frames) override {
        const int n = stb_vorbis_get_samples_float_interleaved(v_, static_cast<int>(ch_), out, static_cast<int>(frames * ch_));
        return n > 0 ? static_cast<size_t>(n) : 0;
    }
    bool seek(uint64_t frame) override { return stb_vorbis_seek(v_, static_cast<unsigned int>(frame)) != 0; }

private:
    stb_vorbis* v_ = nullptr;
    uint32_t rate_ = 0, ch_ = 0;
    uint64_t total_ = 0;
};

/**
 * MP4 / M4A / CAF konténer alapú dekóder: támogatja az ALAC ('alac' MP4 és CAF) és az AAC ('mp4a' + 'esds', FAAD2)
 * hangsávokat, külön trak-feldolgozással (fejezet- és borító-sávok nem zavarják), ilst címkékkel és borítóval.
 */
class Mp4AudioDecoder : public AudioDecoder {
public:
    ~Mp4AudioDecoder() override { closeHandles(); }

    bool open(const std::string& path, bool probe = false) {
        file_ = openBinaryFile(path);
        if (!file_) return false;

        if (!fileSeek64(file_, 0, SEEK_END)) return false;
        const uint64_t fileSize = fileTell64(file_);
        if (fileSize <= 8) return false;
        if (!fileSeek64(file_, 0, SEEK_SET)) return false;

        uint8_t magic[4] = {};
        if (std::fread(magic, 1, 4, file_) != 4) return false;
        fileSeek64(file_, 0, SEEK_SET);

        bool ok = false;
        if (std::memcmp(magic, "caff", 4) == 0) {
            ok = parseCaf(fileSize);
        } else {
            ok = parseMp4(fileSize);
        }
        if (!ok || codec_ == Codec::None || packets_.empty() || cookie_.empty()) return false;

        detail::finalizeTags(tags_);

        if (codec_ == Codec::Alac) {
            if (channels_ == 0 || sampleRate_ == 0) return false;
            if (probe) {
                closeHandles();
                return true;
            }
            if (alac_.Init(cookie_.data(), static_cast<uint32_t>(cookie_.size())) != 0) return false;
            packetBuf_.resize(4096 * channels_);
            pcmRaw_.resize(4096 * channels_ * 4);
            return seek(0);
        }

        // MP4 AAC (FAAD2)
        if (!initAac()) return false;

        // Az első nem üres csomag dekódolása megadja a valós kimeneti rátát, csatornaszámot és SBR-skálát
        uint32_t decodedSr = 0, decodedCh = 0;
        size_t firstFrames = 0;
        size_t firstIdx = 0;
        const size_t probeLimit = std::min<size_t>(packets_.size(), 8);
        for (size_t i = 0; i < probeLimit; ++i) {
            uint32_t sr = 0, ch = 0;
            size_t outF = 0;
            if (!decodeAacPacket(i, &sr, &ch, &outF)) continue;
            if (outF > 0 && sr > 0 && ch > 0) {
                decodedSr = sr;
                decodedCh = ch;
                firstFrames = outF;
                firstIdx = i;
                break;
            }
        }
        if (decodedSr == 0 || decodedCh == 0 || firstFrames == 0) return false;

        sampleRate_ = decodedSr;
        channels_ = decodedCh;
        bitDepth_ = 0;   // veszteséges (AAC)
        aacPrimePackets_ = firstIdx;   // a FAAD2 első frame <= 1 csomagja(i) 0 mintát adnak ki (MDCT priming)

        // SBR (HE-AAC) vagy eltérő timescale esetén a csomagok kimeneti keretszámának igazítása,
        // a priming csomagok 0 kimeneti kerettel szerepelnek, így a startFrame egzaktul a dekódolt PCM pozíció
        const uint32_t origDelta = packets_[firstIdx].frameCount;
        uint32_t scaleNum = 1, scaleDen = 1;
        if (origDelta > 0 && firstFrames != origDelta) {
            if (firstFrames == static_cast<size_t>(origDelta) * 2) {
                scaleNum = 2;
            } else if (trackTimescale_ > 0 && trackTimescale_ != sampleRate_) {
                scaleNum = sampleRate_;
                scaleDen = trackTimescale_;
            }
        }
        {
            uint64_t cur = 0;
            for (size_t i = 0; i < packets_.size(); ++i) {
                auto& p = packets_[i];
                if (i < aacPrimePackets_) {
                    p.frameCount = 0;
                    p.startFrame = 0;
                    continue;
                }
                if (scaleNum != 1 || scaleDen != 1) {
                    p.frameCount = static_cast<uint32_t>((static_cast<uint64_t>(p.frameCount) * scaleNum + scaleDen / 2) / scaleDen);
                    if (p.frameCount == 0) p.frameCount = static_cast<uint32_t>(firstFrames);
                }
                p.startFrame = cur;
                cur += p.frameCount;
            }
            rawTotalFrames_ = cur;
        }

        // Gapless vágás (iTunSMPB), ha érvényes és beleesik a nyers tartományba
        skipStart_ = 0;
        totalFrames_ = rawTotalFrames_;
        if (aacValidFrames_ > 0 && scaleNum == 1 && scaleDen == 1 &&
            aacDelay_ < 16384 && aacDelay_ + aacValidFrames_ <= rawTotalFrames_) {
            skipStart_ = aacDelay_;
            totalFrames_ = aacValidFrames_;
        }

        if (probe) {
            closeHandles();
            return true;
        }
        return seek(0);
    }

    uint32_t sampleRate() const override { return sampleRate_; }
    uint32_t channels() const override { return channels_; }
    uint32_t bitsPerSample() const override { return codec_ == Codec::Alac ? bitDepth_ : 0; }
    uint64_t totalFrames() const override { return totalFrames_; }

    size_t read(float* out, size_t frames) override {
        if (!file_ || currentFrame_ >= totalFrames_) return 0;
        const size_t limit = static_cast<size_t>(std::min<uint64_t>(frames, totalFrames_ - currentFrame_));
        size_t framesRead = 0;
        while (framesRead < limit) {
            if (bufOffset_ < bufFrames_) {
                const size_t toCopy = std::min(limit - framesRead, bufFrames_ - bufOffset_);
                std::memcpy(out + framesRead * channels_,
                            packetBuf_.data() + bufOffset_ * channels_,
                            toCopy * channels_ * sizeof(float));
                framesRead += toCopy;
                bufOffset_ += toCopy;
                currentFrame_ += toCopy;
            } else {
                if (currentPkt_ >= packets_.size()) break;
                if (!decodePacket(currentPkt_++)) break;
                bufOffset_ = 0;
            }
        }
        return framesRead;
    }

    bool seek(uint64_t frame) override {
        if (!file_ || packets_.empty()) return false;
        if (frame > totalFrames_) frame = totalFrames_;
        if (frame == totalFrames_) {
            currentPkt_ = packets_.size();
            bufOffset_ = 0;
            bufFrames_ = 0;
            currentFrame_ = frame;
            return true;
        }
        const uint64_t rawTarget = frame + skipStart_;
        auto it = std::upper_bound(packets_.begin(), packets_.end(), rawTarget,
            [](uint64_t f, const Packet& p) { return f < p.startFrame + p.frameCount; });
        if (it == packets_.end()) it = packets_.end() - 1;
        const size_t targetPkt = static_cast<size_t>(std::distance(packets_.begin(), it));

        if (codec_ == Codec::Aac) {
            // 2 csomagos elődekódolás (MDCT overlap + esetleges SBR QMF bemelegítés)
            const size_t preStart = (targetPkt > aacPrimePackets_ + 2) ? (targetPkt - 2) : 0;
            if (preStart == 0) {
                if (!initAac()) return false;
            } else {
                NeAACDecPostSeekReset(aac_, static_cast<long>(preStart + 2));
            }
            for (size_t p = preStart; p < targetPkt; ++p) {
                decodeAacPacket(p);
            }
            if (!decodeAacPacket(targetPkt)) return false;
            size_t nextPkt = targetPkt + 1;
            while (bufFrames_ == 0 && nextPkt < packets_.size()) {
                if (!decodeAacPacket(nextPkt++)) break;
            }
            const uint64_t pktStart = packets_[targetPkt].startFrame;
            const uint64_t rel = rawTarget > pktStart ? (rawTarget - pktStart) : 0;
            bufOffset_ = static_cast<size_t>(std::min<uint64_t>(rel, bufFrames_));
            currentPkt_ = nextPkt;
            currentFrame_ = frame;
            return true;
        }

        if (!decodeAlacPacket(targetPkt)) return false;
        const uint64_t pktStart = packets_[targetPkt].startFrame;
        const uint64_t rel = rawTarget > pktStart ? (rawTarget - pktStart) : 0;
        bufOffset_ = static_cast<size_t>(std::min<uint64_t>(rel, bufFrames_));
        currentPkt_ = targetPkt + 1;
        currentFrame_ = frame;
        return true;
    }

private:
    enum class Codec { None, Alac, Aac };

    struct Packet {
        uint64_t fileOffset = 0;
        uint32_t byteSize = 0;
        uint32_t frameCount = 0;
        uint64_t startFrame = 0;
    };

    struct SttsEntry { uint32_t count = 0, delta = 0; };
    struct StscEntry { uint32_t firstChunk = 0, samplesPerChunk = 0, descIdx = 0; };

    struct Mp4Track {
        Codec codec = Codec::None;
        uint32_t timescale = 0;
        uint32_t sampleRate = 0, channels = 0, bitDepth = 0;
        std::vector<uint8_t> cookie;
        std::vector<uint32_t> sampleSizes;
        std::vector<SttsEntry> stts;
        std::vector<StscEntry> stsc;
        std::vector<uint64_t> chunkOffsets;
    };

    void closeHandles() {
        if (aac_) { NeAACDecClose(aac_); aac_ = nullptr; }
        if (file_) { std::fclose(file_); file_ = nullptr; }
    }

    bool initAac() {
        if (aac_) { NeAACDecClose(aac_); aac_ = nullptr; }
        aac_ = NeAACDecOpen();
        if (!aac_) return false;
        NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(aac_);
        cfg->outputFormat = FAAD_FMT_FLOAT;
        cfg->downMatrix = 1;
        NeAACDecSetConfiguration(aac_, cfg);
        unsigned long sr = 0;
        unsigned char ch = 0;
        if (NeAACDecInit2(aac_, cookie_.data(), static_cast<unsigned long>(cookie_.size()), &sr, &ch) < 0) {
            NeAACDecClose(aac_);
            aac_ = nullptr;
            return false;
        }
        return true;
    }

    bool decodePacket(size_t idx) {
        return codec_ == Codec::Aac ? decodeAacPacket(idx) : decodeAlacPacket(idx);
    }

    bool decodeAacPacket(size_t idx, uint32_t* outSr = nullptr, uint32_t* outCh = nullptr, size_t* outFrames = nullptr) {
        if (idx >= packets_.size() || !file_ || !aac_) return false;
        const auto& pkt = packets_[idx];
        if (pkt.byteSize == 0 || pkt.byteSize > (1u << 20)) return false;
        if (!fileSeek64(file_, pkt.fileOffset, SEEK_SET)) return false;
        inBuf_.resize(pkt.byteSize);
        if (std::fread(inBuf_.data(), 1, pkt.byteSize, file_) != pkt.byteSize) return false;

        NeAACDecFrameInfo fi{};
        void* pcm = NeAACDecDecode(aac_, &fi, inBuf_.data(), static_cast<unsigned long>(inBuf_.size()));
        if (fi.error != 0) return false;
        if (!pcm || fi.samples == 0 || fi.channels == 0) {
            bufFrames_ = 0;
            if (outFrames) *outFrames = 0;
            return true;
        }
        const size_t nFrames = fi.samples / fi.channels;
        if (channels_ > 0 && fi.channels != channels_) {
            // Ritka csatornaváltás: igazítjuk a puffert
            packetBuf_.resize(nFrames * channels_, 0.0f);
            const float* src = static_cast<const float*>(pcm);
            const size_t copyCh = std::min<size_t>(fi.channels, channels_);
            for (size_t f = 0; f < nFrames; ++f) {
                for (size_t c = 0; c < copyCh; ++c) packetBuf_[f * channels_ + c] = src[f * fi.channels + c];
                if (fi.channels == 1 && channels_ == 2) packetBuf_[f * 2 + 1] = src[f];
            }
        } else {
            const float* src = static_cast<const float*>(pcm);
            packetBuf_.assign(src, src + fi.samples);
        }
        bufFrames_ = nFrames;
        if (outSr) *outSr = static_cast<uint32_t>(fi.samplerate);
        if (outCh) *outCh = fi.channels;
        if (outFrames) *outFrames = nFrames;
        return true;
    }

    bool decodeAlacPacket(size_t idx) {
        if (idx >= packets_.size() || !file_) return false;
        const auto& pkt = packets_[idx];
        if (!fileSeek64(file_, pkt.fileOffset, SEEK_SET)) return false;
        inBuf_.resize(pkt.byteSize);
        if (std::fread(inBuf_.data(), 1, pkt.byteSize, file_) != pkt.byteSize) return false;
        BitBuffer bits;
        BitBufferInit(&bits, inBuf_.data(), pkt.byteSize);
        uint32_t outFrames = 0;
        if (alac_.Decode(&bits, pcmRaw_.data(), 4096, channels_, &outFrames) != 0) return false;
        bufFrames_ = outFrames;
        if (bitDepth_ == 16) {
            const int16_t* s16 = reinterpret_cast<const int16_t*>(pcmRaw_.data());
            for (size_t i = 0; i < outFrames * channels_; ++i) packetBuf_[i] = s16[i] / 32768.0f;
        } else if (bitDepth_ == 20 || bitDepth_ == 24) {
            const uint8_t* p = pcmRaw_.data();
            for (size_t i = 0; i < outFrames * channels_; ++i, p += 3) {
                const int32_t s = (static_cast<int32_t>(static_cast<int8_t>(p[2])) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[0];
                packetBuf_[i] = s / 8388608.0f;
            }
        } else if (bitDepth_ == 32) {
            const int32_t* s32 = reinterpret_cast<const int32_t*>(pcmRaw_.data());
            for (size_t i = 0; i < outFrames * channels_; ++i) packetBuf_[i] = s32[i] / 2147483648.0f;
        }
        return true;
    }

    /** MPEG-4 descriptor hossz olvasása (1..4 bájt, 7 bit/bájt). */
    static bool readDescLen(const uint8_t* p, size_t n, size_t& pos, size_t& lenOut) {
        lenOut = 0;
        for (int i = 0; i < 4 && pos < n; ++i) {
            const uint8_t b = p[pos++];
            lenOut = (lenOut << 7) | (b & 0x7Fu);
            if ((b & 0x80u) == 0) return true;
        }
        return false;
    }

    /** AudioSpecificConfig kinyerése az MP4 esds doboz payloadjából (4 bájtos FullBox fejléc után). */
    static bool parseEsds(const uint8_t* p, size_t n, std::vector<uint8_t>& ascOut) {
        if (n < 4) return false;
        size_t pos = 4;   // version (1) + flags (3)
        auto parseDecoderConfig = [&](size_t start, size_t end) -> bool {
            size_t q = start;
            if (q + 13 > end) return false;
            const uint8_t objType = p[q];
            // 0x40 = MPEG-4 Audio (AAC), 0x66..0x68 = MPEG-2 AAC; MP3 (0x69/0x6B) stb. elutasítva
            if (objType != 0x40 && objType != 0x66 && objType != 0x67 && objType != 0x68) return false;
            q += 13;
            while (q + 2 <= end) {
                const uint8_t tag = p[q++];
                size_t len = 0;
                if (!readDescLen(p, end, q, len) || q + len > end) break;
                if (tag == 0x05 && len >= 2 && len <= 64) {
                    ascOut.assign(p + q, p + q + len);
                    return true;
                }
                q += len;
            }
            return false;
        };

        while (pos + 2 <= n) {
            const uint8_t tag = p[pos++];
            size_t len = 0;
            if (!readDescLen(p, n, pos, len)) break;
            const size_t end = std::min(n, pos + len);
            if (tag == 0x03) {   // ES_DescrTag
                if (pos + 3 > end) break;
                pos += 2;   // ES_ID
                const uint8_t flags = p[pos++];
                if (flags & 0x80u) { if (pos + 2 > end) break; pos += 2; }
                if (flags & 0x40u) { if (pos + 1 > end) break; const uint8_t ulen = p[pos++]; if (pos + ulen > end) break; pos += ulen; }
                if (flags & 0x20u) { if (pos + 2 > end) break; pos += 2; }
                while (pos + 2 <= end) {
                    const uint8_t subTag = p[pos++];
                    size_t subLen = 0;
                    if (!readDescLen(p, end, pos, subLen)) break;
                    const size_t subEnd = std::min(end, pos + subLen);
                    if (subTag == 0x04 && parseDecoderConfig(pos, subEnd)) return true;
                    pos = subEnd;
                }
                return false;
            } else if (tag == 0x04) {
                if (parseDecoderConfig(pos, end)) return true;
            }
            pos = end;
        }
        return false;
    }

    /** Gyermek dobozok keresése az stsd hangbejegyzésében (QuickTime 'wave' burkolón belül is). */
    static void scanSampleEntryChildren(const uint8_t* data, size_t start, size_t end, uint32_t fmt, Mp4Track& trk) {
        size_t pos = start;
        while (pos + 8 <= end) {
            const uint32_t chSz = be32(data + pos);
            const uint32_t chType = be32(data + pos + 4);
            if (chSz < 8 || pos + chSz > end) break;
            if (fmt == 0x616c6163 && chType == 0x616c6163 && chSz >= 12) {
                const size_t cookieLen = chSz - 12;
                trk.cookie.assign(data + pos + 12, data + pos + 12 + cookieLen);
                if (trk.cookie.size() >= 24) {
                    trk.codec = Codec::Alac;
                    trk.bitDepth = trk.cookie[5];
                    trk.channels = trk.cookie[9];
                    trk.sampleRate = be32(trk.cookie.data() + 20);
                }
                return;
            } else if (fmt == 0x6d703461 && chType == 0x65736473 && chSz >= 12) {
                if (parseEsds(data + pos + 8, chSz - 8, trk.cookie)) {
                    trk.codec = Codec::Aac;
                    return;
                }
            } else if (chType == 0x77617665) {   // QuickTime 'wave' konténer
                scanSampleEntryChildren(data, pos + 8, pos + chSz, fmt, trk);
                if (trk.codec != Codec::None) return;
            }
            pos += chSz;
        }
    }

    bool parseCaf(uint64_t fileSize) {
        if (fileSize < 12) return false;
        uint8_t hdr[8];
        if (std::fread(hdr, 1, 8, file_) != 8) return false;
        std::vector<uint32_t> paktSizes;
        uint64_t dataOffset = 0;
        while (fileTell64(file_) + 12 <= fileSize) {
            uint8_t chdr[12];
            if (std::fread(chdr, 1, 12, file_) != 12) break;
            const uint32_t ctype = be32(chdr);
            const int64_t csize = static_cast<int64_t>(be64(chdr + 4));
            const uint64_t curPos = fileTell64(file_);
            const uint64_t nextPos = (csize < 0) ? fileSize : std::min<uint64_t>(fileSize, curPos + static_cast<uint64_t>(csize));

            if (ctype == 0x64657363 && csize >= 32) { // "desc"
                uint8_t desc[32];
                if (std::fread(desc, 1, 32, file_) == 32) {
                    double sr = 0.0;
                    uint64_t srBits = be64(desc);
                    std::memcpy(&sr, &srBits, sizeof(double));
                    sampleRate_ = static_cast<uint32_t>(sr);
                    const uint32_t fmt = be32(desc + 8);
                    if (fmt != 0x616c6163) return false;
                    channels_ = be32(desc + 24);
                    bitDepth_ = be32(desc + 28);
                }
            } else if (ctype == 0x6b756b69 && csize >= 24 && csize <= (1 << 20)) { // "kuki"
                cookie_.resize(static_cast<size_t>(csize));
                if (std::fread(cookie_.data(), 1, cookie_.size(), file_) == cookie_.size()) {
                    if (cookie_.size() >= 24) {
                        bitDepth_ = cookie_[5];
                        channels_ = cookie_[9];
                        sampleRate_ = be32(cookie_.data() + 20);
                    }
                }
            } else if (ctype == 0x70616b74 && csize >= 24 && csize <= (32 << 20)) { // "pakt"
                std::vector<uint8_t> pdata(static_cast<size_t>(csize));
                if (std::fread(pdata.data(), 1, pdata.size(), file_) == pdata.size()) {
                    const int64_t numPkts = static_cast<int64_t>(be64(pdata.data()));
                    const int64_t validFrames = static_cast<int64_t>(be64(pdata.data() + 8));
                    totalFrames_ = validFrames > 0 ? static_cast<uint64_t>(validFrames) : 0;
                    size_t pos = 24;
                    while (pos < pdata.size() && static_cast<int64_t>(paktSizes.size()) < numPkts) {
                        uint32_t val = 0;
                        while (pos < pdata.size()) {
                            const uint8_t b = pdata[pos++];
                            val = (val << 7) | (b & 0x7f);
                            if ((b & 0x80) == 0) break;
                        }
                        paktSizes.push_back(val);
                    }
                }
            } else if (ctype == 0x64617461) { // "data"
                dataOffset = fileTell64(file_) + 4;
            }
            if (csize < 0) break;
            fileSeek64(file_, nextPos, SEEK_SET);
        }

        if (paktSizes.empty() || dataOffset == 0) return false;
        codec_ = Codec::Alac;
        uint64_t curOff = dataOffset;
        uint64_t curFrame = 0;
        uint64_t rem = totalFrames_;
        for (size_t i = 0; i < paktSizes.size(); ++i) {
            Packet p;
            p.fileOffset = curOff;
            p.byteSize = paktSizes[i];
            p.frameCount = static_cast<uint32_t>(std::min<uint64_t>(4096, rem > 0 ? rem : 4096));
            p.startFrame = curFrame;
            curFrame += p.frameCount;
            if (rem > p.frameCount) rem -= p.frameCount; else rem = 0;
            curOff += p.byteSize;
            packets_.push_back(p);
        }
        if (totalFrames_ == 0) totalFrames_ = curFrame;
        return true;
    }

    void parseIlst(uint64_t endPos) {
        while (fileTell64(file_) + 8 <= endPos) {
            const uint64_t boxStart = fileTell64(file_);
            uint8_t hdr[8];
            if (std::fread(hdr, 1, 8, file_) != 8) break;
            const uint32_t boxSize = be32(hdr);
            if (boxSize < 8) break;
            const uint32_t tagCode = be32(hdr + 4);
            const uint64_t nextBox = std::min(endPos, boxStart + static_cast<uint64_t>(boxSize));

            if (tagCode == 0x2d2d2d2d) { // "----" freeform atom (iTunSMPB, ReplayGain)
                std::string name, val;
                while (fileTell64(file_) + 8 <= nextBox) {
                    const uint64_t sStart = fileTell64(file_);
                    uint8_t shdr[8];
                    if (std::fread(shdr, 1, 8, file_) != 8) break;
                    const uint32_t ssz = be32(shdr);
                    const uint32_t stype = be32(shdr + 4);
                    if (ssz < 8 || sStart + ssz > nextBox) break;
                    if (stype == 0x6e616d65 && ssz > 12 && ssz < 1024) { // "name"
                        fileSeek64(file_, sStart + 12, SEEK_SET);
                        name.resize(ssz - 12);
                        if (std::fread(name.data(), 1, name.size(), file_) != name.size()) name.clear();
                    } else if (stype == 0x64617461 && ssz > 16 && ssz < 4096) { // "data"
                        fileSeek64(file_, sStart + 16, SEEK_SET);
                        val.resize(ssz - 16);
                        if (std::fread(val.data(), 1, val.size(), file_) != val.size()) val.clear();
                    }
                    fileSeek64(file_, sStart + ssz, SEEK_SET);
                }
                if (name == "iTunSMPB" && !val.empty()) {
                    unsigned long long z = 0, d = 0, pad = 0, valid = 0;
                    if (std::sscanf(val.c_str(), " %llx %llx %llx %llx", &z, &d, &pad, &valid) == 4 && valid > 0) {
                        aacDelay_ = static_cast<uint64_t>(d);
                        aacValidFrames_ = static_cast<uint64_t>(valid);
                    }
                } else if (!name.empty() && !val.empty()) {
                    detail::applyReplayGain(name, val, tags_);
                }
                fileSeek64(file_, nextBox, SEEK_SET);
                continue;
            }

            while (fileTell64(file_) + 8 <= nextBox) {
                const uint64_t dataStart = fileTell64(file_);
                uint8_t dhdr[8];
                if (std::fread(dhdr, 1, 8, file_) != 8) break;
                const uint32_t dsz = be32(dhdr);
                if (dsz < 16 || dataStart + dsz > nextBox) break;
                const uint32_t dtype = be32(dhdr + 4);
                if (dtype == 0x64617461) { // "data"
                    uint8_t flags[8];
                    if (std::fread(flags, 1, 8, file_) == 8) {
                        const uint32_t type = be32(flags);
                        const size_t psize = dsz - 16;
                        if (psize <= kMaxArtBytes) {
                            std::vector<uint8_t> pbuf(psize);
                            if (std::fread(pbuf.data(), 1, psize, file_) == psize) {
                                if (type == 1) { // UTF-8 text
                                    const std::string str(pbuf.begin(), pbuf.end());
                                    if (tagCode == 0xa96e616d && tags_.title.empty()) tags_.title = str;
                                    else if (tagCode == 0xa9415254 && tags_.artist.empty()) tags_.artist = str;
                                    else if (tagCode == 0x61415254 && tags_.albumArtist.empty()) tags_.albumArtist = str;
                                    else if (tagCode == 0xa9616c62 && tags_.album.empty()) tags_.album = str;
                                    else if (tagCode == 0xa9646179 && tags_.year == 0) tags_.year = detail::leadingNumber(str);
                                    else if (tagCode == 0xa967656e && tags_.genre.empty()) tags_.genre = detail::normalizeGenre(str);
                                } else if (tagCode == 0x74726b6e && psize >= 6) { // trkn
                                    if (!tags_.track) tags_.track = (pbuf[2] << 8) | pbuf[3];
                                } else if (tagCode == 0x6469736b && psize >= 6) { // disk
                                    if (!tags_.disc) tags_.disc = (pbuf[2] << 8) | pbuf[3];
                                } else if (tagCode == 0x676e7265 && psize >= 2) { // gnre
                                    const int n = ((pbuf[0] << 8) | pbuf[1]) - 1;
                                    if (tags_.genre.empty()) tags_.genre = detail::id3v1Genre(n);
                                } else if (tagCode == 0x636f7672 && psize > 0) { // covr
                                    const std::string mime = (type == 14) ? "image/png" : "image/jpeg";
                                    offerArt(tags_, artType_, 3, mime, pbuf.data(), psize);
                                }
                            }
                        }
                    }
                }
                fileSeek64(file_, dataStart + dsz, SEEK_SET);
            }
            fileSeek64(file_, nextBox, SEEK_SET);
        }
    }

    void walkMp4(uint64_t endPos, std::vector<Mp4Track>& tracks, Mp4Track* cur) {
        constexpr uint64_t kMaxBoxRead = 32u << 20;
        while (fileTell64(file_) + 8 <= endPos) {
            const uint64_t boxStart = fileTell64(file_);
            uint8_t hdr[8];
            if (std::fread(hdr, 1, 8, file_) != 8) break;
            const uint32_t sz32 = be32(hdr);
            uint64_t boxSize = sz32;
            uint32_t hdrLen = 8;
            if (sz32 == 1) {
                uint8_t hdr64[8];
                if (std::fread(hdr64, 1, 8, file_) != 8) break;
                boxSize = be64(hdr64);
                hdrLen = 16;
            } else if (sz32 == 0) {
                boxSize = endPos - boxStart;
            }
            if (boxSize < hdrLen) break;
            const uint32_t type = be32(hdr + 4);
            const uint64_t nextBox = std::min(endPos, boxStart + boxSize);
            const uint64_t payloadSize = nextBox - (boxStart + hdrLen);

            if (type == 0x7472616b) { // "trak": külön sáv (fejezet / borító sáv nem keveredik a hanggal)
                tracks.emplace_back();
                walkMp4(nextBox, tracks, &tracks.back());
            } else if (type == 0x6d6f6f76 || type == 0x6d646961 ||
                       type == 0x6d696e66 || type == 0x75647461 || type == 0x7374626c) {
                walkMp4(nextBox, tracks, cur);
            } else if (type == 0x6d657461) { // "meta": ISO FullBox (+4 bájt) vagy ritka QuickTime konténer (+0)
                if (payloadSize >= 8) {
                    const uint64_t metaBody = fileTell64(file_);
                    uint8_t peek[8] = {};
                    if (std::fread(peek, 1, 8, file_) == 8) {
                        const uint32_t t0 = be32(peek + 4);
                        const bool nonFull = (t0 == 0x68646c72 || t0 == 0x696c7374 || t0 == 0x6b657973);
                        fileSeek64(file_, metaBody + (nonFull ? 0 : 4), SEEK_SET);
                        walkMp4(nextBox, tracks, cur);
                    }
                }
            } else if (type == 0x6d646864 && cur && payloadSize >= 16 && payloadSize <= 1024) { // "mdhd"
                std::vector<uint8_t> buf(static_cast<size_t>(payloadSize));
                if (std::fread(buf.data(), 1, buf.size(), file_) == buf.size()) {
                    const uint8_t ver = buf[0];
                    if (ver == 0 && buf.size() >= 16) cur->timescale = be32(buf.data() + 12);
                    else if (ver == 1 && buf.size() >= 24) cur->timescale = be32(buf.data() + 20);
                }
            } else if (type == 0x73747364 && cur && payloadSize >= 8 && payloadSize <= kMaxBoxRead) { // "stsd"
                std::vector<uint8_t> buf(static_cast<size_t>(payloadSize));
                if (std::fread(buf.data(), 1, buf.size(), file_) == buf.size()) {
                    const uint32_t count = be32(buf.data() + 4);
                    size_t pos = 8;
                    for (uint32_t e = 0; e < count && pos + 8 <= buf.size(); ++e) {
                        const uint32_t entrySize = be32(buf.data() + pos);
                        const uint32_t fmt = be32(buf.data() + pos + 4);
                        if (entrySize < 8) break;
                        const size_t entryEnd = std::min(buf.size(), pos + static_cast<size_t>(entrySize));
                        if ((fmt == 0x616c6163 || fmt == 0x6d703461) && pos + 36 <= entryEnd) {
                            const uint16_t ver = be16(buf.data() + pos + 16);
                            const size_t hdrOff = (ver == 1) ? 52 : (ver == 2) ? 72 : 36;
                            if (fmt == 0x6d703461) {
                                cur->channels = be16(buf.data() + pos + 24);
                                cur->sampleRate = be32(buf.data() + pos + 32) >> 16;
                            }
                            if (pos + hdrOff < entryEnd) {
                                scanSampleEntryChildren(buf.data(), pos + hdrOff, entryEnd, fmt, *cur);
                            }
                            if (cur->codec == Codec::None && hdrOff != 36 && pos + 36 < entryEnd) {
                                scanSampleEntryChildren(buf.data(), pos + 36, entryEnd, fmt, *cur);
                            }
                        }
                        pos = entryEnd;
                    }
                }
            } else if (type == 0x73747473 && cur && payloadSize >= 8 && payloadSize <= kMaxBoxRead) { // "stts"
                std::vector<uint8_t> buf(static_cast<size_t>(payloadSize));
                if (std::fread(buf.data(), 1, buf.size(), file_) == buf.size()) {
                    const uint32_t count = be32(buf.data() + 4);
                    size_t pos = 8;
                    for (uint32_t i = 0; i < count && pos + 8 <= buf.size(); ++i, pos += 8) {
                        cur->stts.push_back({ be32(buf.data() + pos), be32(buf.data() + pos + 4) });
                    }
                }
            } else if (type == 0x73747363 && cur && payloadSize >= 8 && payloadSize <= kMaxBoxRead) { // "stsc"
                std::vector<uint8_t> buf(static_cast<size_t>(payloadSize));
                if (std::fread(buf.data(), 1, buf.size(), file_) == buf.size()) {
                    const uint32_t count = be32(buf.data() + 4);
                    size_t pos = 8;
                    for (uint32_t i = 0; i < count && pos + 12 <= buf.size(); ++i, pos += 12) {
                        cur->stsc.push_back({ be32(buf.data() + pos), be32(buf.data() + pos + 4), be32(buf.data() + pos + 8) });
                    }
                }
            } else if (type == 0x7374737a && cur && payloadSize >= 12 && payloadSize <= kMaxBoxRead) { // "stsz"
                std::vector<uint8_t> buf(static_cast<size_t>(payloadSize));
                if (std::fread(buf.data(), 1, buf.size(), file_) == buf.size()) {
                    const uint32_t uniformSize = be32(buf.data() + 4);
                    const uint32_t count = be32(buf.data() + 8);
                    if (uniformSize != 0) {
                        if (count <= 20000000u) cur->sampleSizes.assign(count, uniformSize);
                    } else {
                        size_t pos = 12;
                        for (uint32_t i = 0; i < count && pos + 4 <= buf.size(); ++i, pos += 4) {
                            cur->sampleSizes.push_back(be32(buf.data() + pos));
                        }
                    }
                }
            } else if (type == 0x7374636f && cur && payloadSize >= 8 && payloadSize <= kMaxBoxRead) { // "stco"
                std::vector<uint8_t> buf(static_cast<size_t>(payloadSize));
                if (std::fread(buf.data(), 1, buf.size(), file_) == buf.size()) {
                    const uint32_t count = be32(buf.data() + 4);
                    size_t pos = 8;
                    for (uint32_t i = 0; i < count && pos + 4 <= buf.size(); ++i, pos += 4) {
                        cur->chunkOffsets.push_back(be32(buf.data() + pos));
                    }
                }
            } else if (type == 0x636f3634 && cur && payloadSize >= 8 && payloadSize <= kMaxBoxRead) { // "co64"
                std::vector<uint8_t> buf(static_cast<size_t>(payloadSize));
                if (std::fread(buf.data(), 1, buf.size(), file_) == buf.size()) {
                    const uint32_t count = be32(buf.data() + 4);
                    size_t pos = 8;
                    for (uint32_t i = 0; i < count && pos + 8 <= buf.size(); ++i, pos += 8) {
                        cur->chunkOffsets.push_back(be64(buf.data() + pos));
                    }
                }
            } else if (type == 0x696c7374) {
                parseIlst(nextBox);
            }
            fileSeek64(file_, nextBox, SEEK_SET);
        }
    }

    bool parseMp4(uint64_t fileSize) {
        std::vector<Mp4Track> tracks;
        walkMp4(fileSize, tracks, nullptr);

        // Az első érvényes hang-sáv kiválasztása (ALAC vagy AAC)
        const Mp4Track* chosen = nullptr;
        for (const auto& trk : tracks) {
            if (trk.codec != Codec::None && !trk.cookie.empty() &&
                !trk.sampleSizes.empty() && !trk.chunkOffsets.empty() && !trk.stsc.empty()) {
                chosen = &trk;
                break;
            }
        }
        if (!chosen) return false;

        codec_ = chosen->codec;
        trackTimescale_ = chosen->timescale;
        sampleRate_ = chosen->sampleRate;
        channels_ = chosen->channels;
        bitDepth_ = chosen->bitDepth;
        cookie_ = chosen->cookie;

        const auto& sampleSizes = chosen->sampleSizes;
        const auto& sttsTable = chosen->stts;
        const auto& stscTable = chosen->stsc;
        const auto& chunkOffsets = chosen->chunkOffsets;

        std::vector<uint32_t> samplesPerChunk(chunkOffsets.size(), 0);
        for (size_t i = 0; i < stscTable.size(); ++i) {
            const uint32_t first = stscTable[i].firstChunk > 0 ? stscTable[i].firstChunk - 1 : 0;
            const uint32_t last = (i + 1 < stscTable.size() && stscTable[i + 1].firstChunk > 0)
                ? (stscTable[i + 1].firstChunk - 1)
                : static_cast<uint32_t>(chunkOffsets.size());
            for (uint32_t c = first; c < last && c < chunkOffsets.size(); ++c) {
                samplesPerChunk[c] = stscTable[i].samplesPerChunk;
            }
        }

        std::vector<uint32_t> packetFrames;
        for (const auto& e : sttsTable) {
            if (packetFrames.size() + e.count > sampleSizes.size() + 64) break;
            packetFrames.insert(packetFrames.end(), e.count, e.delta);
        }

        const uint32_t defaultDelta = (codec_ == Codec::Aac) ? 1024u : 4096u;
        size_t pktIdx = 0;
        uint64_t currentFrame = 0;
        packets_.reserve(sampleSizes.size());
        for (size_t c = 0; c < chunkOffsets.size(); ++c) {
            uint64_t off = chunkOffsets[c];
            const uint32_t count = samplesPerChunk[c];
            for (uint32_t s = 0; s < count && pktIdx < sampleSizes.size(); ++s, ++pktIdx) {
                Packet p;
                p.fileOffset = off;
                p.byteSize = sampleSizes[pktIdx];
                p.frameCount = (pktIdx < packetFrames.size() && packetFrames[pktIdx] > 0) ? packetFrames[pktIdx] : defaultDelta;
                p.startFrame = currentFrame;
                currentFrame += p.frameCount;
                off += p.byteSize;
                packets_.push_back(p);
            }
        }
        totalFrames_ = currentFrame;
        return !packets_.empty();
    }

    FILE* file_ = nullptr;
    Codec codec_ = Codec::None;
    ALACDecoder alac_;
    NeAACDecHandle aac_ = nullptr;
    uint32_t trackTimescale_ = 0;
    uint32_t sampleRate_ = 0, channels_ = 0, bitDepth_ = 0;
    uint64_t totalFrames_ = 0, rawTotalFrames_ = 0;
    uint64_t aacDelay_ = 0, aacValidFrames_ = 0, skipStart_ = 0;
    size_t aacPrimePackets_ = 0;
    std::vector<Packet> packets_;
    std::vector<uint8_t> cookie_;
    std::vector<uint8_t> inBuf_, pcmRaw_;
    std::vector<float> packetBuf_;
    size_t currentPkt_ = 0;
    size_t bufOffset_ = 0;
    size_t bufFrames_ = 0;
    uint64_t currentFrame_ = 0;
    uint32_t artType_ = 0;
};

/**
 * Nyers ADTS AAC (.aac) fájlok dekódere FAAD2-vel (AAC-LC, HE-AAC / SBR, HE-AACv2 / PS), ID3v2 címke- és
 * borítóolvasással, valamint csomagalapú tekeréssel.
 */
class AdtsAacDecoder : public AudioDecoder {
public:
    ~AdtsAacDecoder() override { closeHandles(); }

    bool open(const std::string& path, bool probe = false) {
        file_ = openBinaryFile(path);
        if (!file_) return false;
        if (!fileSeek64(file_, 0, SEEK_END)) return false;
        const uint64_t fileSize = fileTell64(file_);
        if (fileSize < 7) return false;
        fileSeek64(file_, 0, SEEK_SET);

        // Opcionális ID3v2 címke a fájl elején
        uint64_t audioStart = 0;
        uint8_t id3Hdr[10] = {};
        if (fileSize >= 10 && std::fread(id3Hdr, 1, 10, file_) == 10 && std::memcmp(id3Hdr, "ID3", 3) == 0) {
            const uint32_t bodyLen = synchsafe(id3Hdr + 6);
            const uint32_t footer = (id3Hdr[5] & 0x10u) ? 10u : 0u;
            const uint64_t totalTag = 10ull + bodyLen + footer;
            if (totalTag <= fileSize && totalTag <= (16u << 20)) {
                std::vector<uint8_t> tagBuf(static_cast<size_t>(10ull + bodyLen));
                std::memcpy(tagBuf.data(), id3Hdr, 10);
                if (bodyLen == 0 || std::fread(tagBuf.data() + 10, 1, bodyLen, file_) == bodyLen) {
                    detail::parseId3v2(tagBuf.data(), tagBuf.size(), tags_);
                }
                audioStart = totalTag;
            }
        }
        detail::finalizeTags(tags_);

        // ADTS keretek indexelése (csak a fejlécek beolvasása, teljes dekódolás nélkül)
        if (!scanAdtsFrames(audioStart, fileSize)) return false;
        if (!initAac()) return false;

        // Az első nem üres csomagból a valós mintavételi frekvencia, csatornaszám és kerethossz (SBR-nél 2048)
        uint32_t sr = 0, ch = 0;
        size_t pktFrames = 0;
        size_t firstIdx = 0;
        const size_t probeLim = std::min<size_t>(packets_.size(), 8);
        for (size_t i = 0; i < probeLim; ++i) {
            if (decodePacket(i, &sr, &ch, &pktFrames) && pktFrames > 0 && sr > 0 && ch > 0) {
                firstIdx = i;
                break;
            }
        }
        if (sr == 0 || ch == 0 || pktFrames == 0) return false;

        sampleRate_ = sr;
        channels_ = ch;
        primePackets_ = firstIdx;
        uint64_t cur = 0;
        for (size_t i = 0; i < packets_.size(); ++i) {
            auto& p = packets_[i];
            if (i < primePackets_) {
                p.frameCount = 0;
                p.startFrame = 0;
                continue;
            }
            p.frameCount = static_cast<uint32_t>(pktFrames);
            p.startFrame = cur;
            cur += p.frameCount;
        }
        totalFrames_ = cur;

        if (probe) {
            closeHandles();
            return true;
        }
        return seek(0);
    }

    uint32_t sampleRate() const override { return sampleRate_; }
    uint32_t channels() const override { return channels_; }
    uint64_t totalFrames() const override { return totalFrames_; }

    size_t read(float* out, size_t frames) override {
        if (!file_ || currentFrame_ >= totalFrames_) return 0;
        const size_t limit = static_cast<size_t>(std::min<uint64_t>(frames, totalFrames_ - currentFrame_));
        size_t framesRead = 0;
        while (framesRead < limit) {
            if (bufOffset_ < bufFrames_) {
                const size_t toCopy = std::min(limit - framesRead, bufFrames_ - bufOffset_);
                std::memcpy(out + framesRead * channels_,
                            packetBuf_.data() + bufOffset_ * channels_,
                            toCopy * channels_ * sizeof(float));
                framesRead += toCopy;
                bufOffset_ += toCopy;
                currentFrame_ += toCopy;
            } else {
                if (currentPkt_ >= packets_.size()) break;
                if (!decodePacket(currentPkt_++)) break;
                bufOffset_ = 0;
            }
        }
        return framesRead;
    }

    bool seek(uint64_t frame) override {
        if (!file_ || packets_.empty()) return false;
        if (frame > totalFrames_) frame = totalFrames_;
        if (frame == totalFrames_) {
            currentPkt_ = packets_.size();
            bufOffset_ = 0;
            bufFrames_ = 0;
            currentFrame_ = frame;
            return true;
        }
        auto it = std::upper_bound(packets_.begin(), packets_.end(), frame,
            [](uint64_t f, const Packet& p) { return f < p.startFrame + p.frameCount; });
        if (it == packets_.end()) it = packets_.end() - 1;
        const size_t targetPkt = static_cast<size_t>(std::distance(packets_.begin(), it));

        const size_t preStart = (targetPkt > primePackets_ + 2) ? (targetPkt - 2) : 0;
        if (preStart == 0) {
            if (!initAac()) return false;
        } else {
            NeAACDecPostSeekReset(aac_, static_cast<long>(preStart + 2));
        }
        for (size_t p = preStart; p < targetPkt; ++p) {
            decodePacket(p);
        }
        if (!decodePacket(targetPkt)) return false;
        size_t nextPkt = targetPkt + 1;
        while (bufFrames_ == 0 && nextPkt < packets_.size()) {
            if (!decodePacket(nextPkt++)) break;
        }
        const uint64_t rel = frame > packets_[targetPkt].startFrame ? (frame - packets_[targetPkt].startFrame) : 0;
        bufOffset_ = static_cast<size_t>(std::min<uint64_t>(rel, bufFrames_));
        currentPkt_ = nextPkt;
        currentFrame_ = frame;
        return true;
    }

private:
    struct Packet {
        uint64_t fileOffset = 0;
        uint32_t byteSize = 0;
        uint32_t frameCount = 0;
        uint64_t startFrame = 0;
    };

    void closeHandles() {
        if (aac_) { NeAACDecClose(aac_); aac_ = nullptr; }
        if (file_) { std::fclose(file_); file_ = nullptr; }
    }

    static bool validAdtsHeader(const uint8_t h[7], uint32_t& frameLenOut) {
        if (h[0] != 0xFF || (h[1] & 0xF6u) != 0xF0u) return false;
        const uint8_t srIdx = (h[2] >> 2) & 0x0Fu;
        if (srIdx >= 13) return false;
        const uint32_t hdrLen = (h[1] & 0x01u) ? 7u : 9u;
        const uint32_t flen = (static_cast<uint32_t>(h[3] & 0x03u) << 11) |
                              (static_cast<uint32_t>(h[4]) << 3) |
                              (static_cast<uint32_t>(h[5] & 0xE0u) >> 5);
        if (flen < hdrLen) return false;
        frameLenOut = flen;
        return true;
    }

    bool scanAdtsFrames(uint64_t startOff, uint64_t fileSize) {
        // Első érvényes ADTS szinkron megkeresése (max 64 KB átugrása)
        uint64_t pos = startOff;
        const uint64_t searchLim = std::min<uint64_t>(fileSize, startOff + 65536ull);
        if (!fileSeek64(file_, pos, SEEK_SET)) return false;
        std::vector<uint8_t> probeBuf(static_cast<size_t>(searchLim - pos));
        if (probeBuf.size() < 7 || std::fread(probeBuf.data(), 1, probeBuf.size(), file_) != probeBuf.size()) return false;

        bool found = false;
        for (size_t i = 0; i + 7 <= probeBuf.size(); ++i) {
            uint32_t flen = 0;
            if (!validAdtsHeader(probeBuf.data() + i, flen)) continue;
            if (pos + i + flen > fileSize) continue;
            if (pos + i + flen == fileSize) {
                pos += i;
                found = true;
                break;
            }
            // Következő keret szinkronjának igazolása téves találat ellen
            if (i + flen + 7 <= probeBuf.size()) {
                uint32_t flen2 = 0;
                if (validAdtsHeader(probeBuf.data() + i + flen, flen2)) {
                    pos += i;
                    found = true;
                    break;
                }
            } else {
                uint8_t nextH[7] = {};
                if (fileSeek64(file_, pos + i + flen, SEEK_SET) && std::fread(nextH, 1, 7, file_) == 7) {
                    uint32_t flen2 = 0;
                    if (validAdtsHeader(nextH, flen2)) {
                        pos += i;
                        found = true;
                        break;
                    }
                }
            }
        }
        if (!found) return false;

        // Végigugrálás az ADTS kereteken
        packets_.clear();
        while (pos + 7 <= fileSize) {
            uint8_t h[7] = {};
            if (!fileSeek64(file_, pos, SEEK_SET) || std::fread(h, 1, 7, file_) != 7) break;
            uint32_t flen = 0;
            if (!validAdtsHeader(h, flen) || pos + flen > fileSize) break;
            packets_.push_back({ pos, flen, 1024, 0 });
            pos += flen;
            if (packets_.size() >= 2000000u) break;   // védelem
        }
        return !packets_.empty();
    }

    bool initAac() {
        if (aac_) { NeAACDecClose(aac_); aac_ = nullptr; }
        aac_ = NeAACDecOpen();
        if (!aac_) return false;
        NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(aac_);
        cfg->outputFormat = FAAD_FMT_FLOAT;
        cfg->downMatrix = 1;
        NeAACDecSetConfiguration(aac_, cfg);

        std::vector<uint8_t> initBuf;
        for (size_t i = 0; i < packets_.size() && initBuf.size() < 16384; ++i) {
            const auto& p = packets_[i];
            const size_t old = initBuf.size();
            initBuf.resize(old + p.byteSize);
            if (!fileSeek64(file_, p.fileOffset, SEEK_SET) ||
                std::fread(initBuf.data() + old, 1, p.byteSize, file_) != p.byteSize) {
                initBuf.resize(old);
                break;
            }
        }
        if (initBuf.size() < 7) return false;
        unsigned long sr = 0;
        unsigned char ch = 0;
        if (NeAACDecInit(aac_, initBuf.data(), static_cast<unsigned long>(initBuf.size()), &sr, &ch) < 0) {
            NeAACDecClose(aac_);
            aac_ = nullptr;
            return false;
        }
        return true;
    }

    bool decodePacket(size_t idx, uint32_t* outSr = nullptr, uint32_t* outCh = nullptr, size_t* outFrames = nullptr) {
        if (idx >= packets_.size() || !file_ || !aac_) return false;
        const auto& pkt = packets_[idx];
        if (!fileSeek64(file_, pkt.fileOffset, SEEK_SET)) return false;
        inBuf_.resize(pkt.byteSize);
        if (std::fread(inBuf_.data(), 1, pkt.byteSize, file_) != pkt.byteSize) return false;

        NeAACDecFrameInfo fi{};
        void* pcm = NeAACDecDecode(aac_, &fi, inBuf_.data(), static_cast<unsigned long>(inBuf_.size()));
        if (fi.error != 0) return false;
        if (!pcm || fi.samples == 0 || fi.channels == 0) {
            bufFrames_ = 0;
            if (outFrames) *outFrames = 0;
            return true;
        }
        const size_t nFrames = fi.samples / fi.channels;
        if (channels_ > 0 && fi.channels != channels_) {
            packetBuf_.resize(nFrames * channels_, 0.0f);
            const float* src = static_cast<const float*>(pcm);
            const size_t copyCh = std::min<size_t>(fi.channels, channels_);
            for (size_t f = 0; f < nFrames; ++f) {
                for (size_t c = 0; c < copyCh; ++c) packetBuf_[f * channels_ + c] = src[f * fi.channels + c];
                if (fi.channels == 1 && channels_ == 2) packetBuf_[f * 2 + 1] = src[f];
            }
        } else {
            const float* src = static_cast<const float*>(pcm);
            packetBuf_.assign(src, src + fi.samples);
        }
        bufFrames_ = nFrames;
        if (outSr) *outSr = static_cast<uint32_t>(fi.samplerate);
        if (outCh) *outCh = fi.channels;
        if (outFrames) *outFrames = nFrames;
        return true;
    }

    FILE* file_ = nullptr;
    NeAACDecHandle aac_ = nullptr;
    uint32_t sampleRate_ = 0, channels_ = 0;
    uint64_t totalFrames_ = 0;
    size_t primePackets_ = 0;
    std::vector<Packet> packets_;
    std::vector<uint8_t> inBuf_;
    std::vector<float> packetBuf_;
    size_t currentPkt_ = 0;
    size_t bufOffset_ = 0;
    size_t bufFrames_ = 0;
    uint64_t currentFrame_ = 0;
};

class OpusDecoder : public AudioDecoder {
public:
    ~OpusDecoder() override {
        if (of_) op_free(of_);
    }

    bool open(const std::string& path) {
        int err = 0;
        of_ = op_open_file(path.c_str(), &err);
        if (!of_) return false;
        const OpusHead* head = op_head(of_, -1);
        if (!head || head->channel_count <= 0) return false;
        ch_ = static_cast<uint32_t>(head->channel_count);
        rate_ = 48000;
        const ogg_int64_t total = op_pcm_total(of_, -1);
        total_ = total > 0 ? static_cast<uint64_t>(total) : 0;

        const OpusTags* tags = op_tags(of_, -1);
        if (tags) {
            for (int i = 0; i < tags->comments; ++i) {
                if (tags->user_comments[i]) {
                    const int len = tags->comment_lengths[i];
                    detail::applyVorbisComment(tags->user_comments[i], len > 0 ? static_cast<size_t>(len) : std::strlen(tags->user_comments[i]), tags_);
                }
            }
        }
        detail::finalizeTags(tags_);
        return true;
    }

    uint32_t sampleRate() const override { return rate_; }
    uint32_t channels() const override { return ch_; }
    uint64_t totalFrames() const override { return total_; }

    size_t read(float* out, size_t frames) override {
        if (!of_ || frames == 0) return 0;
        size_t framesRead = 0;
        while (framesRead < frames) {
            const int want = static_cast<int>((frames - framesRead) * ch_);
            const int ret = op_read_float(of_, out + framesRead * ch_, want, nullptr);
            if (ret <= 0) break;
            framesRead += static_cast<size_t>(ret);
        }
        return framesRead;
    }

    bool seek(uint64_t frame) override {
        if (!of_) return false;
        return op_pcm_seek(of_, static_cast<ogg_int64_t>(frame)) == 0;
    }

private:
    OggOpusFile* of_ = nullptr;
    uint32_t rate_ = 48000, ch_ = 0;
    uint64_t total_ = 0;
};

class WavpackDecoder : public AudioDecoder {
public:
    WavpackDecoder() = default;
    ~WavpackDecoder() override {
        if (wpc_) WavpackCloseFile(wpc_);
    }

    bool open(const std::string& path, bool probe = false) {
        (void)probe;
        char error[80] = {0};
        wpc_ = WavpackOpenFileInput(path.c_str(), error, OPEN_TAGS, 0);
        if (!wpc_) return false;

        ch_ = static_cast<uint32_t>(WavpackGetNumChannels(wpc_));
        rate_ = WavpackGetSampleRate(wpc_);
        const int64_t total = WavpackGetNumSamples64(wpc_);
        total_ = total > 0 ? static_cast<uint64_t>(total) : 0;
        bytesPerSample_ = WavpackGetBytesPerSample(wpc_);
        bitsPerSample_ = WavpackGetBitsPerSample(wpc_);
        isFloat_ = (WavpackGetMode(wpc_) & MODE_FLOAT) != 0;

        readTags();
        return ch_ > 0 && rate_ > 0;
    }

    uint32_t sampleRate() const override { return rate_; }
    uint32_t channels() const override { return ch_; }
    uint64_t totalFrames() const override { return total_; }

    size_t read(float* interleaved, size_t frames) override {
        if (!wpc_ || frames == 0) return 0;
        rawBuf_.resize(frames * ch_);
        const uint32_t unpacked = WavpackUnpackSamples(wpc_, rawBuf_.data(), static_cast<uint32_t>(frames));
        if (unpacked == 0) return 0;

        const size_t totalSamples = unpacked * ch_;
        if (isFloat_) {
            for (size_t i = 0; i < totalSamples; ++i) {
                int32_t v = rawBuf_[i];
                float f;
                std::memcpy(&f, &v, sizeof(float));
                interleaved[i] = f;
            }
        } else {
            float scale = 1.0f;
            if (bytesPerSample_ == 1) scale = 1.0f / 128.0f;
            else if (bytesPerSample_ == 2) scale = 1.0f / 32768.0f;
            else if (bytesPerSample_ == 3) scale = 1.0f / 8388608.0f;
            else if (bytesPerSample_ == 4) scale = 1.0f / 2147483648.0f;
            else if (bitsPerSample_ > 0 && bitsPerSample_ <= 32) scale = 1.0f / static_cast<float>(1ULL << (bitsPerSample_ - 1));

            for (size_t i = 0; i < totalSamples; ++i) {
                interleaved[i] = static_cast<float>(rawBuf_[i]) * scale;
            }
        }
        return unpacked;
    }

    bool seek(uint64_t frame) override {
        if (!wpc_) return false;
        return WavpackSeekSample64(wpc_, static_cast<int64_t>(frame)) != 0;
    }

private:
    void readTags() {
        if (!wpc_) return;
        char buf[1024];
        auto getStr = [&](const char* item, std::string& out) {
            if (WavpackGetTagItem(wpc_, item, buf, sizeof(buf))) {
                out = buf;
            }
        };
        getStr("Title", tags_.title);
        getStr("Artist", tags_.artist);
        getStr("Album", tags_.album);
        getStr("AlbumArtist", tags_.albumArtist);
        if (tags_.albumArtist.empty()) getStr("Album Artist", tags_.albumArtist);

        std::string s;
        getStr("Track", s);
        if (!s.empty()) tags_.track = detail::leadingNumber(s);
        s.clear();
        getStr("Disc", s);
        if (!s.empty()) tags_.disc = detail::leadingNumber(s);
        s.clear();
        getStr("Year", s);
        if (s.empty()) getStr("Date", s);
        if (!s.empty()) tags_.year = detail::leadingNumber(s);
        s.clear();
        getStr("Genre", s);
        if (!s.empty()) tags_.genre = detail::normalizeGenre(s);
        for (const char* k : { "REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_TRACK_PEAK", "REPLAYGAIN_ALBUM_GAIN", "REPLAYGAIN_ALBUM_PEAK" }) {
            s.clear();
            getStr(k, s);
            if (!s.empty()) detail::applyReplayGain(k, s, tags_);
        }

        // Binary cover art (APEv2 "Cover Art (Front)")
        int binSize = WavpackGetBinaryTagItem(wpc_, "Cover Art (Front)", nullptr, 0);
        if (binSize > 0) {
            std::vector<char> binData(binSize);
            if (WavpackGetBinaryTagItem(wpc_, "Cover Art (Front)", binData.data(), binSize) == binSize) {
                int imgOffset = 0;
                for (int i = 0; i < binSize; ++i) {
                    if (binData[i] == 0) {
                        imgOffset = i + 1;
                        break;
                    }
                }
                if (imgOffset > 0 && imgOffset < binSize) {
                    tags_.art.assign(binData.data() + imgOffset, binSize - imgOffset);
                    if (tags_.art.size() >= 4 && reinterpret_cast<const uint8_t*>(tags_.art.data())[0] == 0xFF && reinterpret_cast<const uint8_t*>(tags_.art.data())[1] == 0xD8) {
                        tags_.artMime = "image/jpeg";
                    } else if (tags_.art.size() >= 8 && std::memcmp(tags_.art.data(), "\x89PNG\r\n\x1a\n", 8) == 0) {
                        tags_.artMime = "image/png";
                    } else {
                        tags_.artMime = "image/jpeg";
                    }
                }
            }
        }

        detail::finalizeTags(tags_);
    }

    WavpackContext* wpc_ = nullptr;
    uint32_t rate_ = 0, ch_ = 0;
    uint64_t total_ = 0;
    int bytesPerSample_ = 0;
public:
    uint32_t bitsPerSample() const override { return bitsPerSample_ > 0 ? static_cast<uint32_t>(bitsPerSample_) : 0u; }

private:
    int bitsPerSample_ = 0;
    bool isFloat_ = false;
    std::vector<int32_t> rawBuf_;
};

// ---------------------------------------------------------------- DSD (.dsf / .dff)

class FileDsdRawSource final : public dsd::DsdRawSource {
public:
    ~FileDsdRawSource() override {
        if (fp_) std::fclose(fp_);
    }

    bool open(const std::string& path, std::string& error) {
#ifdef _WIN32
        fp_ = _wfopen(widePath(path).c_str(), L"rb");
#else
        fp_ = std::fopen(path.c_str(), "rb");
#endif
        if (!fp_) {
            error = "A DSD fájl nem nyitható meg.";
            return false;
        }
        if (!seekAbs(0) || seekEnd() < 0) {
            error = "A DSD fájlméret nem kérdezhető le.";
            return false;
        }
        const uint64_t fsize = fileSize_;
        const std::string ext = lowerExt(path);
        dsd::DsdFileInfo info{};
        const bool ok = (ext == "dff")
                            ? dsd::parseDsdiffHeader(&FileDsdRawSource::readAtCb, this, fsize, info, error)
                            : dsd::parseDsfHeader(&FileDsdRawSource::readAtCb, this, fsize, info, error);
        if (!ok) return false;
        if (!stream_.init(info, &FileDsdRawSource::readAtCb, this)) {
            error = "A DSD folyam nem inicializálható.";
            return false;
        }
        return true;
    }

    bool readBytesAt(uint64_t offset, void* dst, size_t len) {
        if (!fp_) return false;
        if (!seekAbs(offset)) return false;
        return len == 0 || std::fread(dst, 1, len, fp_) == len;
    }

    const dsd::DsdFileInfo& info() const override { return stream_.info(); }
    uint64_t totalChannelBytes() const override { return stream_.totalChannelBytes(); }
    size_t read(uint8_t* dstInterleavedMsb, size_t channelBytes) override {
        return stream_.readChannelBytes(dstInterleavedMsb, channelBytes);
    }
    bool seekByte(uint64_t chByteOffset) override { return stream_.seekByte(chByteOffset); }

private:
    static bool readAtCb(void* ctx, uint64_t offset, void* dst, size_t len) {
        return static_cast<FileDsdRawSource*>(ctx)->readBytesAt(offset, dst, len);
    }

    bool seekAbs(uint64_t off) {
#ifdef _WIN32
        return _fseeki64(fp_, static_cast<__int64>(off), SEEK_SET) == 0;
#else
        return fseeko(fp_, static_cast<off_t>(off), SEEK_SET) == 0;
#endif
    }

    int64_t seekEnd() {
#ifdef _WIN32
        if (_fseeki64(fp_, 0, SEEK_END) != 0) return -1;
        const __int64 sz = _ftelli64(fp_);
        if (sz < 0) return -1;
        fileSize_ = static_cast<uint64_t>(sz);
        _fseeki64(fp_, 0, SEEK_SET);
        return static_cast<int64_t>(sz);
#else
        if (fseeko(fp_, 0, SEEK_END) != 0) return -1;
        const off_t sz = ftello(fp_);
        if (sz < 0) return -1;
        fileSize_ = static_cast<uint64_t>(sz);
        fseeko(fp_, 0, SEEK_SET);
        return static_cast<int64_t>(sz);
#endif
    }

    FILE* fp_ = nullptr;
    uint64_t fileSize_ = 0;
    dsd::DsdRawStream stream_;
};

} // namespace

namespace detail {
void parseId3v2(const uint8_t* data, size_t size, TrackTags& out);
void finalizeTags(TrackTags& t);
} // namespace detail

namespace {

class DsdDecoder final : public AudioDecoder {
public:
    bool open(const std::string& path, std::string& error, bool probe = false) {
        auto src = std::make_unique<FileDsdRawSource>();
        if (!src->open(path, error)) return false;

        const dsd::DsdFileInfo info = src->info();
        if (info.channels == 0 || info.pcmSampleRate == 0 || info.dsdSampleRate < info.pcmSampleRate) {
            error = "Érvénytelen DSD folyamparaméterek.";
            return false;
        }
        const uint32_t decim = info.dsdSampleRate / info.pcmSampleRate;
        totalFrames_ = info.sampleCount / decim;

        // DSF-nél a fájl végi ID3v2 blokk beolvasása a meglévő parseId3v2 segéddel [DSF Section 2.4]
        if (info.container == dsd::ContainerType::Dsf && info.metadataOffset > 0 && info.metadataMaxBytes >= 10) {
            const size_t metaLen = static_cast<size_t>(std::min<uint64_t>(info.metadataMaxBytes, 16u << 20));
            std::vector<uint8_t> id3(metaLen);
            if (src->readBytesAt(info.metadataOffset, id3.data(), metaLen)) {
                detail::parseId3v2(id3.data(), id3.size(), tags_);
                detail::finalizeTags(tags_);
            }
        }

        raw_ = std::move(src);
        if (!probe) {
            if (!conv_.init(info.dsdSampleRate, info.pcmSampleRate, info.channels)) {
                error = "A DSD→PCM szűrő inicializálása sikertelen.";
                return false;
            }
            seek(0);
        }
        return true;
    }

    uint32_t sampleRate() const override { return raw_ ? raw_->info().pcmSampleRate : 0; }
    uint32_t channels() const override { return raw_ ? raw_->info().channels : 0; }
    uint32_t dsdSampleRate() const override { return raw_ ? raw_->info().dsdSampleRate : 0; }
    uint64_t totalFrames() const override { return totalFrames_; }

    size_t read(float* interleaved, size_t frames) override {
        if (!raw_ || pcmPos_ >= totalFrames_ || frames == 0) return 0;
        const uint32_t ch = raw_->info().channels;
        const size_t step = conv_.bytesPerPcmFrame();
        const uint64_t totalRaw = raw_->totalChannelBytes();
        const size_t todo = static_cast<size_t>(std::min<uint64_t>(frames, totalFrames_ - pcmPos_));

        size_t done = 0;
        while (done < todo) {
            const size_t chunk = std::min<size_t>(todo - done, 2048u);
            const size_t needBytes = chunk * step;
            tmpBytes_.resize(needBytes * ch);
            size_t got = 0;
            if (rawReadPos_ < totalRaw) {
                const size_t canRead = static_cast<size_t>(std::min<uint64_t>(needBytes, totalRaw - rawReadPos_));
                got = raw_->read(tmpBytes_.data(), canRead);
            }
            if (got < needBytes) {
                std::memset(tmpBytes_.data() + got * ch, dsd::kDsdSilenceByte, (needBytes - got) * ch);
            }
            rawReadPos_ += needBytes;
            conv_.process(tmpBytes_.data(), chunk, interleaved + done * ch);
            done += chunk;
            pcmPos_ += chunk;
        }
        return done;
    }

    bool seek(uint64_t frame) override {
        if (!raw_ || frame > totalFrames_) return false;
        const uint32_t ch = raw_->info().channels;
        const uint32_t step = conv_.bytesPerPcmFrame();
        const uint32_t half = conv_.halfFilterBytes(); // 14 * step (előnézés = utónézés)
        const uint64_t totalRaw = raw_->totalChannelBytes();

        conv_.reset();
        const int64_t centerByte = static_cast<int64_t>(frame) * static_cast<int64_t>(step);
        const int64_t winStart = centerByte - static_cast<int64_t>(half);
        const int64_t winEnd = centerByte + static_cast<int64_t>(half);

        if (winStart < 0) {
            conv_.pushSilenceBytes(static_cast<uint32_t>(-winStart));
        }
        const uint64_t fileStart = (winStart > 0) ? static_cast<uint64_t>(winStart) : 0ULL;
        const uint64_t fileWantEnd = static_cast<uint64_t>(winEnd);
        const uint64_t availEnd = std::min(fileWantEnd, totalRaw);

        if (availEnd > fileStart) {
            const size_t toRead = static_cast<size_t>(availEnd - fileStart);
            tmpBytes_.resize(toRead * ch);
            raw_->seekByte(fileStart);
            const size_t got = raw_->read(tmpBytes_.data(), toRead);
            if (got > 0) conv_.pushBytesOnly(tmpBytes_.data(), got);
            if (got < toRead) conv_.pushSilenceBytes(static_cast<uint32_t>(toRead - got));
        }
        if (fileWantEnd > availEnd) {
            conv_.pushSilenceBytes(static_cast<uint32_t>(fileWantEnd - availEnd));
        }

        raw_->seekByte(std::min(fileWantEnd, totalRaw));
        rawReadPos_ = fileWantEnd;
        pcmPos_ = frame;
        return true;
    }

private:
    std::unique_ptr<FileDsdRawSource> raw_;
    dsd::DsdToPcmConverter conv_;
    uint64_t totalFrames_ = 0;
    uint64_t pcmPos_ = 0;
    uint64_t rawReadPos_ = 0;
    std::vector<uint8_t> tmpBytes_;
};

} // namespace

namespace detail {

bool applyReplayGain(const std::string& key, const std::string& value, TrackTags& t) {
    std::string k = key;
    for (char& ch : k) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    const char* v = value.c_str();
    char* end = nullptr;
    const double x = std::strtod(v, &end);   // "-6.54 dB" -> -6.54; „0.988” -> 0.988
    if (end == v || !std::isfinite(x)) return k.rfind("REPLAYGAIN_", 0) == 0 || k.rfind("R128_", 0) == 0;
    if (k == "REPLAYGAIN_TRACK_GAIN") t.rgTrackGain = static_cast<float>(x);
    else if (k == "REPLAYGAIN_TRACK_PEAK") t.rgTrackPeak = static_cast<float>(x);
    else if (k == "REPLAYGAIN_ALBUM_GAIN") t.rgAlbumGain = static_cast<float>(x);
    else if (k == "REPLAYGAIN_ALBUM_PEAK") t.rgAlbumPeak = static_cast<float>(x);
    // Opus (RFC 7845): Q7.8 egész, a -23 LUFS-hoz; a ReplayGain referenciája -18 LUFS, ezért +5 dB.
    // A REPLAYGAIN_* címke elsőbbséget kap, ha mindkettő megvan.
    else if (k == "R128_TRACK_GAIN") { if (std::isnan(t.rgTrackGain)) t.rgTrackGain = static_cast<float>(x / 256.0 + 5.0); }
    else if (k == "R128_ALBUM_GAIN") { if (std::isnan(t.rgAlbumGain)) t.rgAlbumGain = static_cast<float>(x / 256.0 + 5.0); }
    else return false;
    return true;
}

void applyVorbisComment(const char* c, size_t len, TrackTags& t) {
    const char* eq = static_cast<const char*>(std::memchr(c, '=', len));
    if (!eq) return;
    const size_t keyLen = static_cast<size_t>(eq - c);
    const std::string value(eq + 1, len - keyLen - 1);
    auto is = [&](const char* key) { return keyLen == std::strlen(key) && startsWithNoCase(c, len, key); };
    if (is("TITLE") && t.title.empty()) t.title = value;
    else if (is("ARTIST") && t.artist.empty()) t.artist = value;
    else if ((is("ALBUMARTIST") || is("ALBUM ARTIST")) && t.albumArtist.empty()) t.albumArtist = value;
    else if (is("ALBUM") && t.album.empty()) t.album = value;
    else if (is("TRACKNUMBER") && t.track == 0) t.track = leadingNumber(value);
    else if (is("DISCNUMBER") && t.disc == 0) t.disc = leadingNumber(value);
    else if ((is("DATE") || is("YEAR")) && t.year == 0) t.year = leadingNumber(value);
    else if (is("GENRE") && t.genre.empty()) t.genre = normalizeGenre(value);
    else if (applyReplayGain(std::string(c, keyLen), value, t)) {}
    else if (is("METADATA_BLOCK_PICTURE")) {
        const std::vector<uint8_t> raw = base64Decode(value.data(), value.size());
        std::string mime, bytes;
        uint32_t type = 0, kept = t.art.empty() ? 0 : 3;   // a már meglévő borítót csak előlap írhatja felül
        if (parseFlacPicture(raw.data(), raw.size(), mime, bytes, type))
            offerArt(t, kept, type, mime, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    }
}

void finalizeTags(TrackTags& t) {
    if (t.artist.empty()) t.artist = t.albumArtist;
}

std::string id3v1Genre(int n) {
    // Az ID3v1 szabvány 80 műfaja (0–79); a többi szám nem szabványos
    static const char* const kGenres[80] = {
        "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal",
        "New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial",
        "Alternative", "Ska", "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal",
        "Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel",
        "Noise", "AlternRock", "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock",
        "Ethnic", "Gothic", "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream",
        "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle",
        "Native American", "Cabaret", "New Wave", "Psychadelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal",
        "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock" };
    return n >= 0 && n < 80 ? kGenres[n] : std::string();
}

std::string normalizeGenre(const std::string& raw) {
    // ID3v2: "(17)", "(17)Rock" (a zárójel utáni szöveg a pontosítás), "17", "(RX)" / "(CR)"; Vorbis: szöveg
    std::string s = raw;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    size_t a = 0;
    while (a < s.size() && s[a] == ' ') ++a;
    s.erase(0, a);
    auto numeric = [](const std::string& x) {
        return !x.empty() && x.size() <= 3 && std::all_of(x.begin(), x.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    if (!s.empty() && s[0] == '(') {
        const size_t close = s.find(')');
        if (close != std::string::npos) {
            const std::string inner = s.substr(1, close - 1), rest = s.substr(close + 1);
            if (!rest.empty() && rest[0] != '(') return rest;
            if (numeric(inner)) return id3v1Genre(std::stoi(inner));
            if (inner == "RX") return "Remix";
            if (inner == "CR") return "Cover";
        }
    }
    if (numeric(s)) return id3v1Genre(std::stoi(s));
    return s;
}

int leadingNumber(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    int v = 0, digits = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9' && digits < 9; ++i, ++digits) v = v * 10 + (s[i] - '0');
    return v;
}

bool parseFlacPicture(const uint8_t* p, size_t n, std::string& mime, std::string& bytes, uint32_t& type) {
    size_t pos = 0;
    auto u32 = [&](uint32_t& v) { if (pos + 4 > n) return false; v = be32(p + pos); pos += 4; return true; };
    uint32_t mimeLen = 0, descLen = 0, w, h, d, cols, dataLen = 0;
    if (!u32(type) || !u32(mimeLen) || pos + mimeLen > n) return false;
    mime.assign(reinterpret_cast<const char*>(p + pos), mimeLen);
    pos += mimeLen;
    if (!u32(descLen) || pos + descLen > n) return false;
    pos += descLen;
    if (!u32(w) || !u32(h) || !u32(d) || !u32(cols) || !u32(dataLen) || pos + dataLen > n) return false;
    bytes.assign(reinterpret_cast<const char*>(p + pos), dataLen);
    return true;
}

void parseId3v2(const uint8_t* data, size_t size, TrackTags& t) {
    if (size < 10 || std::memcmp(data, "ID3", 3) != 0) return;
    const uint8_t ver = data[3], flags = data[5];
    if (ver < 2 || ver > 4) return;
    size_t end = std::min<size_t>(size, 10 + synchsafe(data + 6));
    // Az egész címkére kiterjedő „unsynchronisation”: a 0xFF után beszúrt 0x00 bájtok eltávolítása
    std::vector<uint8_t> buf;
    const uint8_t* p = data;
    if (flags & 0x80) {
        buf.reserve(end);
        for (size_t i = 0; i < end; ++i) {
            buf.push_back(data[i]);
            if (data[i] == 0xFF && i + 1 < end && data[i + 1] == 0x00) ++i;
        }
        p = buf.data();
        end = buf.size();
    }
    size_t pos = 10;
    if ((flags & 0x40) && ver >= 3 && pos + 4 <= end)   // kiterjesztett fejléc
        pos += ver == 4 ? synchsafe(p + pos) : be32(p + pos) + 4;
    uint32_t artType = 0;
    const size_t hdr = ver == 2 ? 6 : 10;
    while (pos + hdr <= end) {
        char id[5] = {};
        size_t fsize;
        uint16_t fflags = 0;
        if (ver == 2) {
            std::memcpy(id, p + pos, 3);
            fsize = (size_t(p[pos + 3]) << 16) | (size_t(p[pos + 4]) << 8) | p[pos + 5];
        } else {
            std::memcpy(id, p + pos, 4);
            fsize = ver == 4 ? synchsafe(p + pos + 4) : be32(p + pos + 4);
            fflags = static_cast<uint16_t>((p[pos + 8] << 8) | p[pos + 9]);
        }
        if (id[0] == 0) break;   // kitöltés
        pos += hdr;
        if (fsize == 0 || pos + fsize > end) break;
        const uint8_t* f = p + pos;
        size_t fn = fsize;
        pos += fsize;
        // tömörített / titkosított keret: kihagyjuk; adathossz-jelző (v2.4) és csoportazonosító (v2.3) átugrása
        if (ver == 4) {
            if (fflags & 0x000C) continue;
            if (fflags & 0x0040) { if (fn < 1) continue; ++f; --fn; }
            if (fflags & 0x0001) { if (fn < 4) continue; f += 4; fn -= 4; }
        } else if (ver == 3) {
            if (fflags & 0x00C0) continue;
            if (fflags & 0x0020) { if (fn < 1) continue; ++f; --fn; }
        }
        const std::string sid = id;
        if (fn < 1) continue;
        if (sid == "TIT2" || sid == "TT2") { if (t.title.empty()) t.title = id3Text(f[0], f + 1, fn - 1); }
        else if (sid == "TPE1" || sid == "TP1") { if (t.artist.empty()) t.artist = id3Text(f[0], f + 1, fn - 1); }
        else if (sid == "TPE2" || sid == "TP2") { if (t.albumArtist.empty()) t.albumArtist = id3Text(f[0], f + 1, fn - 1); }
        else if (sid == "TALB" || sid == "TAL") { if (t.album.empty()) t.album = id3Text(f[0], f + 1, fn - 1); }
        else if (sid == "TRCK" || sid == "TRK") { if (!t.track) t.track = leadingNumber(id3Text(f[0], f + 1, fn - 1)); }
        else if (sid == "TPOS" || sid == "TPA") { if (!t.disc) t.disc = leadingNumber(id3Text(f[0], f + 1, fn - 1)); }
        else if (sid == "TYER" || sid == "TDRC" || sid == "TYE") { if (!t.year) t.year = leadingNumber(id3Text(f[0], f + 1, fn - 1)); }
        else if (sid == "TCON" || sid == "TCO") { if (t.genre.empty()) t.genre = normalizeGenre(id3Text(f[0], f + 1, fn - 1)); }
        else if (sid == "TXXX" || sid == "TXX") {
            // felhasználói szöveg: kódolás, leírás (pl. REPLAYGAIN_TRACK_GAIN), érték
            const uint8_t enc = f[0];
            const size_t dl = id3FieldLen(enc, f + 1, fn - 1);
            if (1 + dl > fn) continue;
            applyReplayGain(id3Text(enc, f + 1, dl), id3Text(enc, f + 1 + dl, fn - 1 - dl), t);
        }
        else if (sid == "APIC" || sid == "PIC") {
            const uint8_t enc = f[0];
            size_t q = 1;
            std::string mime;
            if (sid == "PIC") {                       // v2.2: 3 betűs formátum (JPG, PNG)
                if (q + 3 > fn) continue;
                mime.assign(reinterpret_cast<const char*>(f + q), 3);
                q += 3;
            } else {
                const size_t ml = id3FieldLen(0, f + q, fn - q);
                mime.assign(reinterpret_cast<const char*>(f + q), ml > 0 && f[q + ml - 1] == 0 ? ml - 1 : ml);
                q += ml;
            }
            if (q + 1 > fn) continue;
            const uint32_t ptype = f[q++];
            q += id3FieldLen(enc, f + q, fn - q);    // leírás
            if (q < fn) offerArt(t, artType, ptype, mime, f + q, fn - q);
        }
    }
}

} // namespace detail

bool isSupportedFile(const std::string& path) {
    const std::string e = lowerExt(path);
    return e == "flac" || e == "wav" || e == "mp3" || e == "ogg" || e == "oga" ||
           e == "m4a" || e == "aac" || e == "alac" || e == "caf" || e == "opus" || e == "wv" ||
           e == "aif" || e == "aiff" || e == "aifc" || e == "dsf" || e == "dff";
}

std::unique_ptr<AudioDecoder> openDecoder(const std::string& path, std::string* error, bool probe) {
    const std::string e = lowerExt(path);
    auto fail = [&](const char* why) -> std::unique_ptr<AudioDecoder> { if (error) *error = why; return nullptr; };
    if (e == "flac") { auto d = std::make_unique<FlacDecoder>(); if (d->open(path)) return d; return fail("A FLAC fájl nem nyitható meg."); }
    if (e == "wav") { auto d = std::make_unique<WavDecoder>(); if (d->open(path)) return d; return fail("A WAV fájl nem nyitható meg."); }
    // AIFF / AIFF-C: ugyanaz a dekóder (dr_wav; a tárolót a fájl fejlécéből ismeri fel), 768 kHz-ig
    if (e == "aif" || e == "aiff" || e == "aifc") { auto d = std::make_unique<WavDecoder>(); if (d->open(path)) return d; return fail("Az AIFF fájl nem nyitható meg."); }
    if (e == "mp3") { auto d = std::make_unique<Mp3Decoder>(); if (d->open(path, probe)) return d; return fail("Az MP3 fájl nem nyitható meg."); }
    if (e == "ogg" || e == "oga") {
        auto d = std::make_unique<VorbisDecoder>();
        if (d->open(path)) return d;
        auto dopus = std::make_unique<OpusDecoder>();
        if (dopus->open(path)) return dopus;
        return fail("Az Ogg fájl nem nyitható meg (nem Vorbis vagy Opus adatfolyam).");
    }
    if (e == "m4a" || e == "alac" || e == "caf") {
        auto d = std::make_unique<Mp4AudioDecoder>();
        if (d->open(path, probe)) return d;
        return fail("Az M4A / ALAC / AAC fájl nem nyitható meg.");
    }
    if (e == "aac") {
        auto d = std::make_unique<AdtsAacDecoder>();
        if (d->open(path, probe)) return d;
        auto mp4 = std::make_unique<Mp4AudioDecoder>();
        if (mp4->open(path, probe)) return mp4;
        return fail("Az AAC fájl nem nyitható meg.");
    }
    if (e == "opus") { auto d = std::make_unique<OpusDecoder>(); if (d->open(path)) return d; return fail("Az Opus fájl nem nyitható meg."); }
    if (e == "wv") { auto d = std::make_unique<WavpackDecoder>(); if (d->open(path, probe)) return d; return fail("A WavPack (.wv) fájl nem nyitható meg."); }
    if (e == "dsf" || e == "dff") {
        auto d = std::make_unique<DsdDecoder>();
        std::string dsdErr;
        if (d->open(path, dsdErr, probe)) return d;
        return fail(dsdErr.empty() ? "A DSD fájl nem nyitható meg." : dsdErr.c_str());
    }
    return fail("Nem támogatott formátum (FLAC, WAV, AIFF, MP3, Ogg Vorbis, ALAC, AAC, Opus, WavPack, DSF, DFF).");
}

} // namespace ee_analyzer::player

namespace ee_analyzer::dsd {

std::unique_ptr<DsdRawSource> openDsdRawFile(const std::string& path, std::string* error) {
    auto src = std::make_unique<ee_analyzer::player::FileDsdRawSource>();
    std::string err;
    if (!src->open(path, err)) {
        if (error) *error = err;
        return nullptr;
    }
    return src;
}

} // namespace ee_analyzer::dsd
