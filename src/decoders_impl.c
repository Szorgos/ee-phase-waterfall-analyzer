/* A beépített lejátszó dekódereinek megvalósítása (egyszer, külön fordítási egységben; third_party/decoders). */
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_WAV_IMPLEMENTATION
/* A WAV / AIFF dekóder alapból 384 kHz-ig fogad el fájlt; a nagy felbontás 705,6 / 768 kHz-ig */
#define DRWAV_MAX_SAMPLE_RATE 768000
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
