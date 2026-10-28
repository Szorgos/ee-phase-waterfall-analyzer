# Hangfájl-dekóderek (a 2.x beépített lejátszójához)

| Fájl | Forrás | Commit | Licenc |
|---|---|---|---|
| dr_flac.h, dr_wav.h, dr_mp3.h | https://github.com/mackron/dr_libs | dfe8377631000664666519fdb83da193fd8037f4 (2026-08-31) | public domain / MIT-0 (dr_libs-LICENSE) |
| stb_vorbis.c | https://github.com/nothings/stb | 2c980bb59875b0d32144a71867fbdebb2f77cd20 (2026-08-02) | MIT / public domain (stb-LICENSE) |
| alac/* | https://github.com/macosforge/alac | master | Apache-2.0 (alac/alac-LICENSE) |
| opus/* | https://gitlab.xiph.org/xiph (opus, ogg, opusfile) | master | BSD-3-Clause (opus/opus-LICENSE) |
| wavpack/* | https://github.com/dbry/WavPack | 5.9.0 (2026) | BSD-3-Clause (wavpack/wavpack-LICENSE) |

Változtatás nélkül, egy kivétellel; a megvalósítást a src/decoders_impl.c és a CMake fordítja (egyszer).

Kivétel: `opus/opus/CMakeLists.txt`, az ARM-os futásidejű CPU-felismerés ága (`dnn_sources_arm_rtcd`) az `OPUS_DNN` feltétel mögé
került, ahogy az x86-os ág és az ARM NEON ág is. A beágyazott fában nincs `dnn/` mappa, ezért ARM-on (aarch64) a fordítás
a hiányzó `dnn/arm/arm_dnn_map.c` miatt leállt (COPR, Fedora aarch64).
