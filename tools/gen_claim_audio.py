#!/usr/bin/env python3
"""
Helyben szintetizált számjegy-hangminták (0-9) generátora a headless Raspberry Pi első beállításhoz.

Forrás és licenc:
  Saját fejlesztésű, tiszta Python formáns-szintetizátor (Klatt-típusú rezonátor-bank),
  külső hangminta és külső TTS-függőség nélkül.
  Licenc: CC0-1.0 / a projekt saját forrásával azonos (szabadon újraelosztható).

Kimenet:
  data/claim_audio/en/0.wav .. 9.wav  (alapértelmezett és fallback minden más nyelvre)
  data/claim_audio/hu/0.wav .. 9.wav  (magyar)
Formátum: 16 kHz, 16 bites előjeles PCM, mono WAV.
"""
import math
import os
import random
import struct
import sys
import wave

SR = 16000
TWO_PI = 2.0 * math.pi


class BiquadBandpass:
    """Másodrendű rezonátor (formáns-szűrő) adott frekvenciára és sávszélességre."""
    def __init__(self):
        self.y1 = 0.0
        self.y2 = 0.0

    def process(self, x: float, freq: float, bw: float) -> float:
        r = math.exp(-math.pi * bw / SR)
        c = -2.0 * r * math.cos(TWO_PI * freq / SR)
        a2 = r * r
        gain = 1.0 - r
        y = gain * x - c * self.y1 - a2 * self.y2
        self.y2 = self.y1
        self.y1 = y
        return y


# Fonéma definíciók: (típus, alap_idő_ms, F1, B1, F2, B2, F3, B3, zaj_szint, zaj_f, zaj_bw)
# típus: 'V' = magánhangzó/zöngés, 'N' = nazális/folyékony, 'F' = réshang, 'S' = zárhang-szünet+felpattanás
PHONEMES = {
    # Magánhangzók
    "i":  ("V", 120, 280,  60, 2250, 100, 3000, 150, 0.00, 4000, 1000),
    "I":  ("V",  95, 390,  70, 1990, 110, 2550, 160, 0.00, 4000, 1000),
    "e":  ("V", 120, 480,  70, 1950, 110, 2600, 160, 0.00, 4000, 1000),
    "E":  ("V", 110, 580,  80, 1780, 110, 2500, 170, 0.00, 4000, 1000),
    "a":  ("V", 125, 730,  90, 1150, 110, 2450, 180, 0.00, 4000, 1000),
    "A":  ("V", 135, 680,  90, 1250, 110, 2500, 180, 0.00, 4000, 1000),
    "o":  ("V", 125, 450,  75,  880,  90, 2400, 170, 0.00, 4000, 1000),
    "O":  ("V", 130, 540,  85,  960, 100, 2400, 170, 0.00, 4000, 1000),
    "u":  ("V", 130, 310,  65,  820,  90, 2250, 170, 0.00, 4000, 1000),
    "y":  ("V", 125, 300,  65, 1750, 110, 2250, 170, 0.00, 4000, 1000),
    "2":  ("V", 130, 430,  75, 1520, 110, 2250, 170, 0.00, 4000, 1000),  # magyar ö/ő
    "@":  ("V",  85, 500,  85, 1450, 110, 2450, 170, 0.00, 4000, 1000),  # schwa
    # Folyékonyak, nazálisok, félhangzók
    "w":  ("N",  75, 320,  70,  700,  90, 2200, 180, 0.00, 3000, 1000),
    "r":  ("N",  70, 420,  80, 1300, 110, 1650, 140, 0.00, 3000, 1000),
    "l":  ("N",  75, 360,  75, 1200, 100, 2700, 180, 0.00, 3000, 1000),
    "n":  ("N",  80, 280,  90, 1450, 150, 2350, 200, 0.00, 3000, 1000),
    "m":  ("N",  80, 260,  90, 1100, 150, 2200, 200, 0.00, 3000, 1000),
    "j":  ("N",  70, 290,  70, 2150, 110, 2950, 170, 0.00, 3000, 1000),
    # Réshangok (zöngés / zöngétlen)
    "z":  ("V",  85, 300, 100, 1600, 150, 2600, 200, 0.45, 5200, 1400),
    "v":  ("V",  75, 320, 100, 1200, 150, 2300, 200, 0.30, 3400, 1600),
    "s":  ("F", 105, 400, 200, 1800, 300, 2800, 300, 0.90, 5600, 1500),
    "f":  ("F",  95, 400, 200, 1500, 300, 2500, 300, 0.55, 3600, 2000),
    "T":  ("F",  85, 400, 200, 1600, 300, 2600, 300, 0.48, 4400, 1900),  # angol th
    "h":  ("F",  75, 550, 160, 1500, 220, 2500, 250, 0.42, 2200, 1400),
    "c":  ("F",  95, 380, 180, 1900, 260, 2700, 280, 0.85, 5400, 1500),  # magyar c (ts)
    # Zárhangok
    "t":  ("S",  65, 400, 180, 1750, 250, 2700, 280, 0.85, 4200, 1500),
    "k":  ("S",  70, 420, 180, 1500, 250, 2300, 260, 0.80, 2600, 1200),
    "d":  ("S",  55, 320, 150, 1700, 220, 2600, 250, 0.45, 3800, 1400),
    "g":  ("S",  55, 320, 150, 1450, 220, 2200, 240, 0.45, 2400, 1200),
}

# Számjegyek fonémasorozatai (rövid, tiszta artikuláció)
WORDS = {
    "en": {
        "0": ["z", "i", "r", "o", "u"],
        "1": ["w", "a", "n"],
        "2": ["t", "u", "u"],
        "3": ["T", "r", "i", "i"],
        "4": ["f", "O", "r"],
        "5": ["f", "a", "i", "v"],
        "6": ["s", "I", "k", "s"],
        "7": ["s", "E", "v", "@", "n"],
        "8": ["e", "i", "t"],
        "9": ["n", "a", "i", "n"],
    },
    "hu": {
        "0": ["n", "u", "l", "l", "a"],
        "1": ["E", "d", "j"],
        "2": ["k", "E", "t", "2"],
        "3": ["h", "A", "r", "o", "m"],
        "4": ["n", "e", "d", "j"],
        "5": ["2", "t"],
        "6": ["h", "a", "t"],
        "7": ["h", "e", "t"],
        "8": ["n", "j", "o", "l", "c"],
        "9": ["k", "i", "l", "E", "n", "c"],
    },
}


def synth_word(phoneme_list, seed: int = 42):
    rng = random.Random(seed)
    f1_filt = BiquadBandpass()
    f2_filt = BiquadBandpass()
    f3_filt = BiquadBandpass()
    nz_filt = BiquadBandpass()

    # Teljes hossz kiszámítása
    segs = [PHONEMES[p] for p in phoneme_list]
    total_samples = sum(int(SR * seg[1] / 1000.0) for seg in segs)
    out = []

    phase = 0.0
    sample_idx = 0

    for si, seg in enumerate(segs):
        kind, dur_ms, f1, b1, f2, b2, f3, b3, nz_lvl, nz_f, nz_bw = seg
        n = int(SR * dur_ms / 1000.0)
        next_seg = segs[si + 1] if si + 1 < len(segs) else seg

        for i in range(n):
            t_global = sample_idx / max(1, total_samples)
            t_local = i / max(1, n)

            # Természetes ereszkedő alapfrekvencia (F0: 155 Hz -> 120 Hz)
            f0 = 152.0 - 32.0 * t_global + 4.0 * math.sin(math.pi * t_global)
            phase += f0 / SR
            if phase >= 1.0:
                phase -= 1.0

            # Rosenberg-szerű glottális impulzus (lágy, természetes felharmonikus-eloszlás)
            if phase < 0.40:
                glottal = 0.5 * (1.0 - math.cos(math.pi * phase / 0.40))
            elif phase < 0.56:
                glottal = math.cos(0.5 * math.pi * (phase - 0.40) / 0.16)
            else:
                glottal = 0.0
            # Differenciálás az akusztikai sugárzáshoz
            glottal = (glottal - 0.35) * 1.4

            # Koartikuláció: a szegmens utolsó 25%-ában sima átmenet a következő formánsok felé
            blend = max(0.0, (t_local - 0.75) / 0.25) if si + 1 < len(segs) else 0.0
            cf1 = f1 + (next_seg[2] - f1) * blend
            cf2 = f2 + (next_seg[4] - f2) * blend
            cf3 = f3 + (next_seg[6] - f3) * blend

            white = rng.uniform(-1.0, 1.0)
            y = 0.0

            if kind in ("V", "N"):
                amp = 1.0 if kind == "V" else 0.65
                env = min(1.0, i / (SR * 0.012)) * min(1.0, (n - i) / (SR * 0.012))
                v = (
                    0.55 * f1_filt.process(glottal, cf1, b1)
                    + 0.32 * f2_filt.process(glottal, cf2, b2)
                    + 0.15 * f3_filt.process(glottal, cf3, b3)
                )
                y += amp * env * v
                if nz_lvl > 0.0:
                    y += nz_lvl * env * nz_filt.process(white, nz_f, nz_bw) * 0.5
            elif kind == "F":
                env = min(1.0, i / (SR * 0.015)) * min(1.0, (n - i) / (SR * 0.015))
                y += nz_lvl * env * nz_filt.process(white, nz_f, nz_bw) * 0.65
            elif kind == "S":
                # Zárhang: rövid csend (első 55%), majd éles felpattanás (utolsó 45%)
                if t_local < 0.55:
                    y = 0.0
                else:
                    bt = (t_local - 0.55) / 0.45
                    benv = math.exp(-bt * 4.5)
                    y = nz_lvl * benv * nz_filt.process(white, nz_f, nz_bw) * 0.9

            out.append(y)
            sample_idx += 1

    # Rövid be- és kifutási csend (20 ms) + normalizálás -3 dBFS-re
    peak = max((abs(v) for v in out), default=1.0)
    scale = (0.70 * 32767.0) / peak if peak > 1e-6 else 0.0
    pad = [0] * int(SR * 0.02)
    pcm = pad + [max(-32767, min(32767, int(v * scale))) for v in out] + pad
    return pcm


def write_wav(path: str, pcm):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(SR)
        wf.writeframes(struct.pack("<" + "h" * len(pcm), *pcm))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_base = os.path.join(root, "data", "claim_audio")
    count = 0
    for lang, digits in WORDS.items():
        for digit, ph in digits.items():
            pcm = synth_word(ph, seed=100 + int(digit) * 17 + (0 if lang == "en" else 500))
            out_path = os.path.join(out_base, lang, f"{digit}.wav")
            write_wav(out_path, pcm)
            count += 1
    print(f"Rendben: {count} db 16 kHz mono PCM16 WAV számjegy-hangminta elkészült -> {out_base}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
