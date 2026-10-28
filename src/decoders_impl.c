/* A beépített lejátszó dekódereinek megvalósítása (egyszer, külön fordítási egységben; third_party/decoders). */
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#include "stb_vorbis.c"

/* Apple Lossless Audio Codec (ALAC, Apache-2.0, third_party/decoders/alac). */
#include "alac/ALACBitUtilities.c"
#include "alac/EndianPortable.c"
#include "alac/ag_dec.c"
#include "alac/dp_dec.c"
#include "alac/matrix_dec.c"
