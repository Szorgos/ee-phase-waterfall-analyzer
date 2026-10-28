#pragma once
/**
 * @file audio_decoder.hpp
 * @brief Hangfájlok dekódolása a beépített lejátszóhoz (2.x): FLAC, WAV, MP3, Ogg Vorbis, ALAC (M4A / CAF), Opus, WavPack.
 *
 * A dekóderek a third_party/decoders könyvtárai (dr_flac, dr_wav, dr_mp3, stb_vorbis, Apple ALAC, libopus, WavPack; megengedő
 * licencek). A kimenet 32 bites lebegőpontos, a fájl saját csatornaszámával és mintavételi frekvenciájával; az
 * átalakítást (sztereóra, a kimenet frekvenciájára) a lejátszó és a rendszer végzi.
 */
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace ee_analyzer::player {

/** A fájl címkéi. Az üres mező: nincs ilyen címke (a cím helyett a lejátszó a fájlnevet mutatja). */
struct TrackTags {
    std::string title, artist, album;
    std::string albumArtist;              // ha nincs előadó, az előadó helyén is ez áll (finalizeTags)
    int track = 0, disc = 0, year = 0;    // 0: nincs megadva
    std::string genre;                    // műfaj (az ID3 számkódja szöveggé alakítva); üres: nincs megadva
    std::string artMime;   // a borító típusa (pl. "image/jpeg"); üres: nincs borító
    std::string art;       // a borító bájtjai
};

class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;
    virtual uint32_t sampleRate() const = 0;
    virtual uint32_t channels() const = 0;
    /** A hossz mintakeretben; 0: ismeretlen (ilyenkor a tekerés nem engedélyezett). */
    virtual uint64_t totalFrames() const = 0;
    /** Legfeljebb `frames` keret, csatornánként váltakozva (interleaved); 0: a fájl vége (vagy olvasási hiba). */
    virtual size_t read(float* interleaved, size_t frames) = 0;
    /** Ugrás a megadott keretre; false: nem sikerült (a pozíció ilyenkor határozatlan). */
    virtual bool seek(uint64_t frame) = 0;
    const TrackTags& tags() const { return tags_; }

protected:
    TrackTags tags_;
};

/** A kiterjesztés alapján támogatott-e (.flac, .wav, .mp3, .ogg, .oga, .m4a, .alac, .caf, .opus, .wv); a tartalmat az openDecoder ellenőrzi. */
bool isSupportedFile(const std::string& path);

/**
 * A fájl megnyitása a kiterjesztésnek megfelelő dekóderrel; nullptr: nem nyitható (error: a magyarázat).
 * probe = true: csak a címkék és az adatok kellenek (zenetár): MP3-nál a hosszt csak a Xing / Info fejlécből veszi
 * (különben 0, ismeretlen), a fájlt nem olvassa végig.
 */
std::unique_ptr<AudioDecoder> openDecoder(const std::string& path, std::string* error = nullptr, bool probe = false);

/** Belső segédek (a tesztek is használják). */
namespace detail {
/** ID3v2 címke (a teljes blokk a 10 bájtos fejléccel) feldolgozása: cím, előadó, album, borító. */
void parseId3v2(const uint8_t* data, size_t size, TrackTags& out);
/** FLAC PICTURE blokk (a METADATA_BLOCK_PICTURE Vorbis-megjegyzés tartalma is ez) feldolgozása. */
bool parseFlacPicture(const uint8_t* data, size_t size, std::string& mime, std::string& bytes, uint32_t& type);
/** Vorbis-megjegyzés (pl. "TITLE=Valami") beírása a címkékbe; a METADATA_BLOCK_PICTURE base64 borítót is. */
void applyVorbisComment(const char* comment, size_t len, TrackTags& out);
/** A címkék kiegészítése a feldolgozás végén: az előadó helyén az albumelőadó, ha az előadó hiányzik. */
void finalizeTags(TrackTags& t);
/** Szám az elején ("3/12" -> 3, "2009-05-01" -> 2009); 0, ha nincs. */
int leadingNumber(const std::string& s);
/** Az ID3v1 szabványos műfaja (0–79); üres, ha a szám nem szabványos. */
std::string id3v1Genre(int n);
/** Műfaj szövegként: "(17)" és "17" -> "Rock", "(17)Egyedi" -> "Egyedi", "(RX)" -> "Remix"; a szöveg marad. */
std::string normalizeGenre(const std::string& raw);
}

} // namespace ee_analyzer::player
