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

Az FFTW3 (GPL-2.0-or-later) nincs a tárolóban: Linuxon a rendszer `fftw3f` csomagja, Windowson a
https://www.fftw.org/fftw-3.3.10.tar.gz forrás épül be (`-DAQULIFY_FFTW_SOURCE_DIR=...`).

Az LSP pluginok (para_equalizer 1.0.40, limiter 1.0.37) beállítás-feldolgozó kódjának átvett részei:
`src/ee_dsp_lsp.cpp`.
