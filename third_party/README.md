# Külső DSP könyvtárak

Az Aqulify ugyanazokat a DSP könyvtárakat használja, mint az EasyEffects. A források változatlanok,
kivéve ahol jelölve van; mindegyik a saját licencével.

| Mappa | Forrás | Verzió | Licenc |
|---|---|---|---|
| `lsp/lsp-common-lib` | https://github.com/lsp-plugins/lsp-common-lib | 1.0.49 | LGPL-3.0-or-later |
| `lsp/lsp-dsp-lib` | https://github.com/lsp-plugins/lsp-dsp-lib | 1.1.1 | LGPL-3.0-or-later |
| `lsp/lsp-lltl-lib` | https://github.com/lsp-plugins/lsp-lltl-lib | 1.0.33 | LGPL-3.0-or-later |
| `lsp/lsp-runtime-lib` | https://github.com/lsp-plugins/lsp-runtime-lib | 1.0.36 | LGPL-3.0-or-later |
| `lsp/lsp-dsp-units` | https://github.com/lsp-plugins/lsp-dsp-units | 1.0.38 | LGPL-3.0-or-later |
| `calf` | https://github.com/calf-studio-gear/calf (a1f87a4): `biquad.h`, `primitives.h`; a Bass Enhancer kódja `calf_bass_enhancer.h`-ban (LV2 port- és mérőkód nélkül) | | LGPL-2.1-or-later |
| `libbs2b` | https://github.com/alexmarsev/libbs2b | 3.1.0 | MIT |
| `speexdsp` | https://github.com/xiph/speexdsp (7a15878): resampler; `speexdsp_config_types.h` a configure kimenete helyett | | BSD-3-Clause |
| `zita-convolver` | Fons Adriaensen zita-convolvere az Ardour hordozható változatában (https://github.com/Ardour/ardour, libs/zita-convolver, 1e41d33) | 4.0.0 | GPL-3.0-or-later |
| `rnnoise` | https://gitlab.xiph.org/xiph/rnnoise (a 0.2-es kiadás kódja a 372f7b4 javítással: „Fix compilation errors.”, a Debian `fix-compilation-errors.patch` foltja); a fordításhoz szükséges fájlok, a betanított modell nélkül | 0.2 | BSD-3-Clause |

Az RNNoise betanított modellje (`src/rnnoise_data.c`, 29 MB, a `model_version` szerinti 0b50c45) sincs a tárolóban.
Forrása az Ubuntu `rnnoise_0.2+ds.orig.tar.xz` csomagja
(http://archive.ubuntu.com/ubuntu/pool/universe/r/rnnoise/, sha256
`db2718ee4a0b294bfbbe198fe94aabfdc8cd0d77cb9cd95d76c7a818ad805f0b`): a CI, a COPR és az `install.sh` innen veszi,
és a `-DAQULIFY_RNNOISE_MODEL_DIR=<mappa>` kapcsolóval kerül a fordításba. Ennek hiányában Linuxon a rendszer
`librnnoise` csomagja épül be, ha van; ha egyik sincs, a Magic Touch a beszédnél is a saját, sávonkénti
zajcsökkentést használja.

A felület betűtípusai (`fonts`: csak a licencek; a betűkészletek a felület karakterkészletére szűkítve, WOFF2
formában a `gui_integrated_system.html`-be ágyazva, mert a felület hálózat nélkül is fut):

| Betűtípus | Forrás | Verzió | Licenc |
|---|---|---|---|
| Inter (változó súly) | https://github.com/rsms/inter, a `@fontsource-variable/inter` 5.1.0 csomagból | 4.001 | OFL-1.1 (`fonts/Inter-OFL.txt`) |
| IBM Plex Mono (400, 500) | https://github.com/IBM/plex, a `@fontsource/ibm-plex-mono` 5.1.0 csomagból | 2.3 | OFL-1.1 (`fonts/IBMPlexMono-OFL.txt`) |

Az FFTW3 (GPL-2.0-or-later) nincs a tárolóban: Linuxon a rendszer `fftw3f` csomagja, Windowson a
https://www.fftw.org/fftw-3.3.10.tar.gz forrás épül be (`-DAQULIFY_FFTW_SOURCE_DIR=...`).

Az LSP pluginok (para_equalizer 1.0.40, limiter 1.0.37) beállítás-feldolgozó kódjának átvett részei:
`src/ee_dsp_lsp.cpp`.
