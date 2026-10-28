/**
 * @file audio_decoder.cpp
 * @brief A beépített lejátszó dekóderei (FLAC, WAV, MP3, Ogg Vorbis) és a címkék (Vorbis-megjegyzés, ID3v2, FLAC
 *        PICTURE) feldolgozása. A könyvtárak megvalósítását a src/decoders_impl.c fordítja.
 */
#include "audio_decoder.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
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

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint64_t be64(const uint8_t* p) { return (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4); }
uint32_t synchsafe(const uint8_t* p) { return (uint32_t(p[0] & 0x7f) << 21) | (uint32_t(p[1] & 0x7f) << 14) | (uint32_t(p[2] & 0x7f) << 7) | (p[3] & 0x7f); }

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

class AlacDecoder : public AudioDecoder {
public:
    ~AlacDecoder() override {
        if (file_) std::fclose(file_);
    }

    bool open(const std::string& path, bool probe = false) {
#ifdef _WIN32
        file_ = _wfopen(widePath(path).c_str(), L"rb");
#else
        file_ = std::fopen(path.c_str(), "rb");
#endif
        if (!file_) return false;

        std::fseek(file_, 0, SEEK_END);
        const long sz = std::ftell(file_);
        if (sz <= 8) return false;
        const uint64_t fileSize = static_cast<uint64_t>(sz);
        std::fseek(file_, 0, SEEK_SET);

        uint8_t magic[4] = {};
        if (std::fread(magic, 1, 4, file_) != 4) return false;
        std::fseek(file_, 0, SEEK_SET);

        bool ok = false;
        if (std::memcmp(magic, "caff", 4) == 0) {
            ok = parseCaf(fileSize);
        } else {
            ok = parseMp4(fileSize);
        }
        if (!ok || packets_.empty() || cookie_.empty() || channels_ == 0 || sampleRate_ == 0) return false;

        detail::finalizeTags(tags_);
        if (probe) {
            return true;
        }

        if (dec_.Init(cookie_.data(), static_cast<uint32_t>(cookie_.size())) != 0) return false;

        packetBuf_.resize(4096 * channels_);
        pcmRaw_.resize(4096 * channels_ * 4);
        return seek(0);
    }

    uint32_t sampleRate() const override { return sampleRate_; }
    uint32_t channels() const override { return channels_; }
    uint64_t totalFrames() const override { return totalFrames_; }

    size_t read(float* out, size_t frames) override {
        size_t framesRead = 0;
        while (framesRead < frames) {
            if (bufOffset_ < bufFrames_) {
                const size_t toCopy = std::min(frames - framesRead, bufFrames_ - bufOffset_);
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
        currentPkt_ = static_cast<size_t>(std::distance(packets_.begin(), it));
        if (!decodePacket(currentPkt_)) return false;
        bufOffset_ = static_cast<size_t>(frame - packets_[currentPkt_].startFrame);
        currentFrame_ = frame;
        currentPkt_++;
        return true;
    }

private:
    struct Packet {
        uint64_t fileOffset = 0;
        uint32_t byteSize = 0;
        uint32_t frameCount = 0;
        uint64_t startFrame = 0;
    };

    struct SttsEntry { uint32_t count = 0, delta = 0; };
    struct StscEntry { uint32_t firstChunk = 0, samplesPerChunk = 0, descIdx = 0; };

    bool decodePacket(size_t idx) {
        if (idx >= packets_.size() || !file_) return false;
        const auto& pkt = packets_[idx];
        std::fseek(file_, static_cast<long>(pkt.fileOffset), SEEK_SET);
        inBuf_.resize(pkt.byteSize);
        if (std::fread(inBuf_.data(), 1, pkt.byteSize, file_) != pkt.byteSize) return false;
        BitBuffer bits;
        BitBufferInit(&bits, inBuf_.data(), pkt.byteSize);
        uint32_t outFrames = 0;
        if (dec_.Decode(&bits, pcmRaw_.data(), 4096, channels_, &outFrames) != 0) return false;
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

    bool parseCaf(uint64_t fileSize) {
        if (fileSize < 12) return false;
        uint8_t hdr[8];
        if (std::fread(hdr, 1, 8, file_) != 8) return false;
        std::vector<uint32_t> paktSizes;
        uint64_t dataOffset = 0;
        while (static_cast<uint64_t>(std::ftell(file_)) + 12 <= fileSize) {
            uint8_t chdr[12];
            if (std::fread(chdr, 1, 12, file_) != 12) break;
            const uint32_t ctype = be32(chdr);
            const int64_t csize = static_cast<int64_t>(be64(chdr + 4));
            const long nextPos = std::ftell(file_) + static_cast<long>(csize < 0 ? 0 : csize);

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
            } else if (ctype == 0x6b756b69 && csize >= 24) { // "kuki"
                cookie_.resize(static_cast<size_t>(csize));
                if (std::fread(cookie_.data(), 1, cookie_.size(), file_) == cookie_.size()) {
                    if (cookie_.size() >= 24) {
                        bitDepth_ = cookie_[5];
                        channels_ = cookie_[9];
                        sampleRate_ = be32(cookie_.data() + 20);
                    }
                }
            } else if (ctype == 0x70616b74 && csize >= 24) { // "pakt"
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
                dataOffset = static_cast<uint64_t>(std::ftell(file_) + 4);
            }
            if (csize < 0) break;
            std::fseek(file_, nextPos, SEEK_SET);
        }

        if (paktSizes.empty() || dataOffset == 0) return false;
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
        while (static_cast<uint64_t>(std::ftell(file_)) + 8 <= endPos) {
            const long boxStart = std::ftell(file_);
            uint8_t hdr[8];
            if (std::fread(hdr, 1, 8, file_) != 8) break;
            const uint32_t boxSize = be32(hdr);
            if (boxSize < 8) break;
            const uint32_t tagCode = be32(hdr + 4);
            const long nextBox = boxStart + static_cast<long>(boxSize);

            while (static_cast<uint64_t>(std::ftell(file_)) + 8 <= static_cast<uint64_t>(nextBox)) {
                const long dataStart = std::ftell(file_);
                uint8_t dhdr[8];
                if (std::fread(dhdr, 1, 8, file_) != 8) break;
                const uint32_t dsz = be32(dhdr);
                if (dsz < 16) break;
                const uint32_t dtype = be32(dhdr + 4);
                if (dtype == 0x64617461) { // "data"
                    uint8_t flags[8];
                    if (std::fread(flags, 1, 8, file_) == 8) {
                        const uint32_t type = be32(flags);
                        const size_t psize = dsz - 16;
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
                std::fseek(file_, dataStart + dsz, SEEK_SET);
            }
            std::fseek(file_, nextBox, SEEK_SET);
        }
    }

    void walkMp4(uint64_t endPos, std::vector<uint32_t>& sampleSizes,
                 std::vector<SttsEntry>& sttsTable, std::vector<StscEntry>& stscTable,
                 std::vector<uint64_t>& chunkOffsets) {
        while (static_cast<uint64_t>(std::ftell(file_)) + 8 <= endPos) {
            const long boxStart = std::ftell(file_);
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
                boxSize = endPos - static_cast<uint64_t>(boxStart);
            }
            if (boxSize < hdrLen) break;
            const uint32_t type = be32(hdr + 4);
            const uint64_t payloadSize = boxSize - hdrLen;
            const long nextBox = boxStart + static_cast<long>(boxSize);

            if (type == 0x6d6f6f76 || type == 0x7472616b || type == 0x6d646961 ||
                type == 0x6d696e66 || type == 0x75647461) {
                walkMp4(static_cast<uint64_t>(nextBox), sampleSizes, sttsTable, stscTable, chunkOffsets);
            } else if (type == 0x6d657461) {
                std::fseek(file_, 4, SEEK_CUR);
                walkMp4(static_cast<uint64_t>(nextBox), sampleSizes, sttsTable, stscTable, chunkOffsets);
            } else if (type == 0x7374626c) {
                walkMp4(static_cast<uint64_t>(nextBox), sampleSizes, sttsTable, stscTable, chunkOffsets);
            } else if (type == 0x73747364) {
                std::vector<uint8_t> buf(static_cast<size_t>(payloadSize));
                if (std::fread(buf.data(), 1, buf.size(), file_) == buf.size() && buf.size() >= 8) {
                    const uint32_t count = be32(buf.data() + 4);
                    size_t pos = 8;
                    for (uint32_t e = 0; e < count && pos + 8 <= buf.size(); ++e) {
                        const uint32_t entrySize = be32(buf.data() + pos);
                        const uint32_t fmt = be32(buf.data() + pos + 4);
                        if (fmt == 0x616c6163 && pos + 36 <= buf.size()) {
                            size_t childPos = pos + 36;
                            while (childPos + 8 <= pos + entrySize && childPos + 8 <= buf.size()) {
                                const uint32_t chSz = be32(buf.data() + childPos);
                                const uint32_t chType = be32(buf.data() + childPos + 4);
                                if (chType == 0x616c6163 && chSz >= 12) {
                                    const size_t cookieLen = chSz - 12;
                                    cookie_.assign(buf.data() + childPos + 12, buf.data() + childPos + 12 + cookieLen);
                                    if (cookie_.size() >= 24) {
                                        bitDepth_ = cookie_[5];
                                        channels_ = cookie_[9];
                                        sampleRate_ = be32(cookie_.data() + 20);
                                    }
                                    break;
                                }
                                if (chSz < 8) break;
                                childPos += chSz;
                            }
                        }
                        if (entrySize < 8) break;
                        pos += entrySize;
                    }
                }
            } else if (type == 0x73747473) {
                std::vector<uint8_t> buf(static_cast<size_t>(payloadSize));
                if (std::fread(buf.data(), 1, buf.size(), file_) == buf.size() && buf.size() >= 8) {
                    const uint32_t count = be32(buf.data() + 4);
                    size_t pos = 8;
                    for (uint32_t i = 0; i < count && pos + 8 <= buf.size(); ++i, pos += 8) {
                        sttsTable.push_back({ be32(buf.data() + pos), be32(buf.data() + pos + 4) });
                    }
                }
            } else if (type == 0x73747363) {
                std::vector<uint8_t> buf(static_cast<size_t>(payloadSize));
                if (std::fread(buf.data(), 1, buf.size(), file_) == buf.size() && buf.size() >= 8) {
                    const uint32_t count = be32(buf.data() + 4);
                    size_t pos = 8;
                    for (uint32_t i = 0; i < count && pos + 12 <= buf.size(); ++i, pos += 12) {
                        stscTable.push_back({ be32(buf.data() + pos), be32(buf.data() + pos + 4), be32(buf.data() + pos + 8) });
                    }
                }
            } else if (type == 0x7374737a) {
                std::vector<uint8_t> buf(static_cast<size_t>(payloadSize));
                if (std::fread(buf.data(), 1, buf.size(), file_) == buf.size() && buf.size() >= 12) {
                    const uint32_t uniformSize = be32(buf.data() + 4);
                    const uint32_t count = be32(buf.data() + 8);
                    if (uniformSize != 0) {
                        sampleSizes.assign(count, uniformSize);
                    } else {
                        size_t pos = 12;
                        for (uint32_t i = 0; i < count && pos + 4 <= buf.size(); ++i, pos += 4) {
                            sampleSizes.push_back(be32(buf.data() + pos));
                        }
                    }
                }
            } else if (type == 0x7374636f) {
                std::vector<uint8_t> buf(static_cast<size_t>(payloadSize));
                if (std::fread(buf.data(), 1, buf.size(), file_) == buf.size() && buf.size() >= 8) {
                    const uint32_t count = be32(buf.data() + 4);
                    size_t pos = 8;
                    for (uint32_t i = 0; i < count && pos + 4 <= buf.size(); ++i, pos += 4) {
                        chunkOffsets.push_back(be32(buf.data() + pos));
                    }
                }
            } else if (type == 0x636f3634) {
                std::vector<uint8_t> buf(static_cast<size_t>(payloadSize));
                if (std::fread(buf.data(), 1, buf.size(), file_) == buf.size() && buf.size() >= 8) {
                    const uint32_t count = be32(buf.data() + 4);
                    size_t pos = 8;
                    for (uint32_t i = 0; i < count && pos + 8 <= buf.size(); ++i, pos += 8) {
                        chunkOffsets.push_back(be64(buf.data() + pos));
                    }
                }
            } else if (type == 0x696c7374) {
                parseIlst(static_cast<uint64_t>(nextBox));
            }
            std::fseek(file_, nextBox, SEEK_SET);
        }
    }

    bool parseMp4(uint64_t fileSize) {
        std::vector<uint32_t> sampleSizes;
        std::vector<SttsEntry> sttsTable;
        std::vector<StscEntry> stscTable;
        std::vector<uint64_t> chunkOffsets;

        walkMp4(fileSize, sampleSizes, sttsTable, stscTable, chunkOffsets);

        if (sampleSizes.empty() || chunkOffsets.empty() || stscTable.empty()) return false;

        std::vector<uint32_t> samplesPerChunk(chunkOffsets.size(), 0);
        for (size_t i = 0; i < stscTable.size(); ++i) {
            const uint32_t first = stscTable[i].firstChunk > 0 ? stscTable[i].firstChunk - 1 : 0;
            const uint32_t last = (i + 1 < stscTable.size() && stscTable[i+1].firstChunk > 0)
                ? (stscTable[i+1].firstChunk - 1)
                : static_cast<uint32_t>(chunkOffsets.size());
            for (uint32_t c = first; c < last && c < chunkOffsets.size(); ++c) {
                samplesPerChunk[c] = stscTable[i].samplesPerChunk;
            }
        }

        std::vector<uint32_t> packetFrames;
        for (const auto& e : sttsTable) {
            for (uint32_t i = 0; i < e.count; ++i) packetFrames.push_back(e.delta);
        }

        size_t pktIdx = 0;
        uint64_t currentFrame = 0;
        for (size_t c = 0; c < chunkOffsets.size(); ++c) {
            uint64_t off = chunkOffsets[c];
            const uint32_t count = samplesPerChunk[c];
            for (uint32_t s = 0; s < count && pktIdx < sampleSizes.size(); ++s, ++pktIdx) {
                Packet p;
                p.fileOffset = off;
                p.byteSize = sampleSizes[pktIdx];
                p.frameCount = (pktIdx < packetFrames.size()) ? packetFrames[pktIdx] : 4096;
                p.startFrame = currentFrame;
                currentFrame += p.frameCount;
                off += p.byteSize;
                packets_.push_back(p);
            }
        }
        totalFrames_ = currentFrame;
        return true;
    }

    FILE* file_ = nullptr;
    ALACDecoder dec_;
    uint32_t sampleRate_ = 0, channels_ = 0, bitDepth_ = 0;
    uint64_t totalFrames_ = 0;
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
    int bitsPerSample_ = 0;
    bool isFloat_ = false;
    std::vector<int32_t> rawBuf_;
};

} // namespace

namespace detail {

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
           e == "m4a" || e == "alac" || e == "caf" || e == "opus" || e == "wv";
}

std::unique_ptr<AudioDecoder> openDecoder(const std::string& path, std::string* error, bool probe) {
    const std::string e = lowerExt(path);
    auto fail = [&](const char* why) -> std::unique_ptr<AudioDecoder> { if (error) *error = why; return nullptr; };
    if (e == "flac") { auto d = std::make_unique<FlacDecoder>(); if (d->open(path)) return d; return fail("A FLAC fájl nem nyitható meg."); }
    if (e == "wav") { auto d = std::make_unique<WavDecoder>(); if (d->open(path)) return d; return fail("A WAV fájl nem nyitható meg."); }
    if (e == "mp3") { auto d = std::make_unique<Mp3Decoder>(); if (d->open(path, probe)) return d; return fail("Az MP3 fájl nem nyitható meg."); }
    if (e == "ogg" || e == "oga") {
        auto d = std::make_unique<VorbisDecoder>();
        if (d->open(path)) return d;
        auto dopus = std::make_unique<OpusDecoder>();
        if (dopus->open(path)) return dopus;
        return fail("Az Ogg fájl nem nyitható meg (nem Vorbis vagy Opus adatfolyam).");
    }
    if (e == "m4a" || e == "alac" || e == "caf") { auto d = std::make_unique<AlacDecoder>(); if (d->open(path, probe)) return d; return fail("Az ALAC (M4A) fájl nem nyitható meg."); }
    if (e == "opus") { auto d = std::make_unique<OpusDecoder>(); if (d->open(path)) return d; return fail("Az Opus fájl nem nyitható meg."); }
    if (e == "wv") { auto d = std::make_unique<WavpackDecoder>(); if (d->open(path, probe)) return d; return fail("A WavPack (.wv) fájl nem nyitható meg."); }
    return fail("Nem támogatott formátum (FLAC, WAV, MP3, Ogg Vorbis, ALAC, Opus, WavPack).");
}

} // namespace ee_analyzer::player
