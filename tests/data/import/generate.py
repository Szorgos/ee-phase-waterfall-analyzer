#!/usr/bin/env python3
"""Kitalált (szintetikus) mintafájlok az import teszteléséhez (átadó: atadas-masik-mi-2026-11-16d.md).

A fájlok a nyilvánosan leírt formátumokat követik (EQ-konfigurációs szöveg: Preamp / Filter / Channel / Delay /
Convolution / GraphicEQ / Include / Copy sorok; WAV PCM és IEEE float; CamillaDSP YAML; frekvencia–dB táblázat).
Egyik sem egy konkrét program valódi kimenete. A várt eredmény: VART-EREDMENYEK.md.

Futtatás: python generate.py   (a fájlok ebbe a mappába kerülnek; újrafuttatható, ugyanazt állítja elő)
"""
import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
SR = 48000


def w(name, text):
    p = os.path.join(HERE, name)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def wav(name, channels, frames, sr=SR, fmt="float32"):
    """channels: csatornánként egy lista (azonos hossz). fmt: float32 | pcm16 | pcm24 | extensible."""
    n = len(channels)
    ext = fmt == "extensible"
    if fmt in ("float32", "extensible"):
        bits, tag = 32, 3
        data = b"".join(struct.pack("<" + "f" * n, *[c[i] for c in channels]) for i in range(frames))
    elif fmt == "pcm16":
        bits, tag = 16, 1
        data = b"".join(struct.pack("<" + "h" * n, *[max(-32768, min(32767, round(c[i] * 32767))) for c in channels]) for i in range(frames))
    elif fmt == "pcm24":
        bits, tag = 24, 1
        out = bytearray()
        for i in range(frames):
            for c in channels:
                v = max(-8388608, min(8388607, round(c[i] * 8388607)))
                out += struct.pack("<i", v)[:3]
        data = bytes(out)
    else:
        raise ValueError(fmt)
    block = n * bits // 8
    if ext:
        # WAVE_FORMAT_EXTENSIBLE, alformátum: IEEE float
        sub = struct.pack("<HHI", 22, bits, 0) + bytes.fromhex("0300000000001000800000aa00389b71")
        fmt_chunk = struct.pack("<HHIIHH", 0xFFFE, n, sr, sr * block, block, bits) + sub
    else:
        fmt_chunk = struct.pack("<HHIIHH", tag, n, sr, sr * block, block, bits)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt_chunk)) + fmt_chunk + b"data" + struct.pack("<I", len(data)) + data
    p = os.path.join(HERE, name)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)


def dirac(frames, at, amp=1.0):
    x = [0.0] * frames
    x[at] = amp
    return x


N = 256   # IR-hossz (minta)

# --- 1. Egyszerű, egy csatornás parametrikus profil (ma is betölthető; regresszió)
w("01-parametrikus.txt", """Preamp: -6.2 dB
Filter 1: ON PK Fc 65 Hz Gain 5.8 dB Q 1.41
Filter 2: ON PK Fc 120 Hz Gain -8.5 dB Q 4.20
Filter 3: ON LSC Fc 105 Hz Gain 3.0 dB Q 0.70
Filter 4: ON HSC Fc 10000 Hz Gain -2.0 dB Q 0.71
Filter 5: OFF PK Fc 3000 Hz Gain 4.0 dB Q 2.00
""")

# --- 2. Szobakorrekció jellegű export: csatornánként eltérő szűrők, késleltetés, közös rész
w("02-szoba-bal-jobb.txt", """# Szintetikus szobakorrekciós export (bal és jobb hangszóró külön)
Preamp: -5.0 dB
Channel: L
Delay: 0.35 ms
Filter 1: ON PK Fc 42 Hz Gain -7.5 dB Q 6.00
Filter 2: ON PK Fc 87 Hz Gain -4.0 dB Q 3.50
Filter 3: ON PK Fc 210 Hz Gain 2.5 dB Q 2.00
Channel: R
Delay: 0 ms
Filter 1: ON PK Fc 45 Hz Gain -6.0 dB Q 5.50
Filter 2: ON PK Fc 160 Hz Gain -3.5 dB Q 4.00
Channel: L R
Filter 4: ON HSC Fc 8000 Hz Gain -1.5 dB Q 0.71
""")

# --- 3. Bal és jobb blokk azonos (egyszer kell alkalmazni)
w("03-azonos-bal-jobb.txt", """Channel: L
Filter 1: ON PK Fc 60 Hz Gain -6.0 dB Q 4.00
Filter 2: ON LS 6dB Fc 120 Hz Gain 2.0 dB
Channel: R
Filter 1: ON PK Fc 60 Hz Gain -6.0 dB Q 4.00
Filter 2: ON LS 6dB Fc 120 Hz Gain 2.0 dB
""")

# --- 4. Meredekséggel megadott szűrők (kaszkád)
w("04-meredekseg.txt", """Filter 1: ON HP Fc 25 Hz
Filter 2: ON LPQ Fc 18000 Hz Q 0.71
Filter 3: ON LS 12dB Fc 80 Hz Gain 3.0 dB
Filter 4: ON HS 6dB Fc 6000 Hz Gain -2.0 dB
Filter 5: ON HP 24 dB Fc 30 Hz
""")

# --- 5. Grafikus EQ (sok pont → illesztés parametrikus sávokra)
pts = []
f = 20.0
while f <= 20000.0:
    g = -4.0 * math.exp(-((math.log2(f / 50.0)) ** 2) / 0.3) + 2.0 * math.exp(-((math.log2(f / 3000.0)) ** 2) / 0.8) - 1.5 * (f > 10000)
    pts.append(f"{f:.0f} {g:.1f}")
    f *= 2 ** (1 / 6)
w("05-grafikus.txt", "GraphicEQ: " + "; ".join(pts) + "\n")

# --- 6. Include: fő fájl + alfájlok (jó, kilépő, körkörös)
w("06-include/fo.txt", """Preamp: -3 dB
Include: reszek/basszus.txt
Include: reszek/magas.txt
""")
w("06-include/reszek/basszus.txt", "Filter 1: ON PK Fc 50 Hz Gain -5 dB Q 3\n")
w("06-include/reszek/magas.txt", "Filter 2: ON HSC Fc 9000 Hz Gain -2 dB Q 0.71\n")
w("06-include/kilepo.txt", "Include: ../../titkos.txt\nInclude: C:\\Windows\\win.ini\nInclude: /etc/passwd\n")
w("06-include/kor-a.txt", "Include: kor-b.txt\nFilter 1: ON PK Fc 100 Hz Gain 1 dB Q 1\n")
w("06-include/kor-b.txt", "Include: kor-a.txt\n")

# --- 7. Konvolúció csatornánként (IR-fájlok mellette)
w("07-konvolucio/eq.txt", """Preamp: -2 dB
Channel: L
Convolution: ir-bal.wav
Channel: R
Convolution: ir-jobb.wav
""")
wav("07-konvolucio/ir-bal.wav", [dirac(N, 0, 0.5)], N)            # bal: azonnal, 0,5-szeres
wav("07-konvolucio/ir-jobb.wav", [dirac(N, 48, 1.0)], N)          # jobb: 48 minta (1 ms) késés
w("07-konvolucio/hianyzo.txt", "Convolution: nincs-ilyen.wav\n")

# --- 8. IR-változatok a konvolverhez
wav("08-ir/sztereo-float.wav", [dirac(N, 0), dirac(N, 10, -1.0)], N)                     # R: 10 minta, fordított
wav("08-ir/sztereo-pcm16.wav", [dirac(N, 0, 0.5), dirac(N, 0, 0.5)], N, fmt="pcm16")
wav("08-ir/sztereo-pcm24.wav", [dirac(N, 5, 0.25), dirac(N, 5, 0.25)], N, fmt="pcm24")
wav("08-ir/sztereo-extensible.wav", [dirac(N, 0), dirac(N, 0)], N, fmt="extensible")
wav("08-ir/sztereo-44k1.wav", [dirac(N, 0), dirac(N, 0)], N, sr=44100)                   # újramintavételezés
wav("08-ir/true-stereo-4ch.wav", [dirac(N, 0), dirac(N, 20, 0.3), dirac(N, 20, 0.3), dirac(N, 0)], N)  # LL LR RL RR
wav("08-ir/mono-bal.wav", [dirac(N, 0)], N)
wav("08-ir/mono-jobb.wav", [dirac(N, 24)], N)
wav("08-ir/harom-csatorna.wav", [dirac(N, 0), dirac(N, 0), dirac(N, 0)], N)              # nem támogatott
with open(os.path.join(HERE, "08-ir/serult.wav"), "wb") as f:
    f.write(b"RIFF\x10\x00\x00\x00WAVEfmt ")                                             # csonka fejléc

# --- 9. Copy és egyéb sorok
w("09-copy.txt", """Copy: L=R R=L
Filter 1: ON PK Fc 100 Hz Gain -2 dB Q 2
""")
w("09-copy-bonyolult.txt", "Copy: L=0.5*L+0.5*R R=R\nFilter 1: ON PK Fc 100 Hz Gain -2 dB Q 2\n")
w("09-egyeb-sorok.txt", """Device: Hangszórók
Stage: post-mix
Eval: x = 1
Filter 1: ON PK Fc 1000 Hz Gain 1 dB Q 1
Filter 2: ON XYZ Fc 1000 Hz Gain 1 dB Q 1
Ez egy értelmezhetetlen sor
""")

# --- 10. Frekvencia–dB táblázatok: mérés (fázissal), mikrofon-kalibráció, célgörbe
rows = []
f = 20.0
while f <= 20000.0:
    spl = 75 + 6 * math.exp(-((math.log2(f / 45.0)) ** 2) / 0.2) - 3 * (f > 12000)
    rows.append(f"{f:.2f},{spl:.2f},{(-90 * math.log10(f / 20)):.1f}")
    f *= 2 ** (1 / 24)
w("10-meres.csv", "Freq(Hz),SPL(dB),Phase(degrees)\n" + "\n".join(rows) + "\n")
cal = ['"Sens Factor =-38.2dB, SERNO: 0000001"']
f = 20.0
while f <= 20000.0:
    cal.append(f"{f:.1f}\t{(0.8 if f < 30 else 0.0) + (-1.2 if f > 15000 else 0.0):.2f}")
    f *= 2 ** (1 / 3)
w("10-mikrofon-kalibracio.txt", "\n".join(cal) + "\n")
w("10-celgorbe.txt", "* Célgörbe: enyhén eső, basszusemeléssel\n20 4.0\n60 3.0\n200 0.5\n1000 0\n5000 -1.5\n20000 -4.0\n")

# --- 11. CamillaDSP-szerű YAML (szűrők, csatornánkénti lánc)
w("11-camilladsp.yml", """filters:
  bass_l:
    type: Biquad
    parameters: {type: Peaking, freq: 44, gain: -7.0, q: 5.0}
  bass_r:
    type: Biquad
    parameters: {type: Peaking, freq: 47, gain: -6.0, q: 5.0}
  shelf:
    type: Biquad
    parameters: {type: Highshelf, freq: 8000, gain: -1.5, q: 0.707}
  hp:
    type: Biquad
    parameters: {type: Highpass, freq: 25, q: 0.707}
  delay_l:
    type: Delay
    parameters: {delay: 0.35, unit: ms}
  vol:
    type: Gain
    parameters: {gain: -5.0}
  room_l:
    type: Conv
    parameters: {type: Wav, filename: 07-konvolucio/ir-bal.wav}
  ismeretlen:
    type: DiffEq
    parameters: {a: [1.0], b: [1.0]}
pipeline:
  - type: Filter
    channels: [0]
    names: [vol, hp, bass_l, delay_l, shelf]
  - type: Filter
    channels: [1]
    names: [vol, hp, bass_r, shelf]
""")

print("kész:", HERE)
