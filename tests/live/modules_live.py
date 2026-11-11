#!/usr/bin/env python3
"""
A DSP-modulok élő ellenőrzése valódi PipeWire-gráfon (felület → motor → hang). A multiout_live.sh indítja:
    tests/live/multiout_live.sh <build> python3 -I tests/live/modules_live.py
Minden eset a motornak ugyanazokat az üzeneteket küldi, mint a felület, majd a DAC A felvételén mér (mindkét
csatornán), és a végén mindent kikapcsol. Az elvárt értékek a modulok leírásából és a tervezett működésből jönnek.
"""
import math, os, struct, subprocess, sys, time, wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wsclient import WS  # noqa: E402

PORT = int(os.environ.get("AQ_PORT", "47810"))
WORK = os.environ.get("AQ_WORK", HERE)
SR = 48000
fails = 0


def check(ok, text):
    global fails
    print(("  [OK]   " if ok else "  [HIBA] ") + text)
    fails += 0 if ok else 1


def write_wav(name, left, right):
    p = os.path.join(WORK, name)
    w = wave.open(p, "wb")
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR)
    inter = np.empty(2 * len(left), dtype=np.int16)
    inter[0::2] = np.clip(np.asarray(left) * 32767, -32768, 32767).astype(np.int16)
    inter[1::2] = np.clip(np.asarray(right) * 32767, -32768, 32767).astype(np.int16)
    w.writeframes(inter.tobytes()); w.close()
    return p


def sines(freqs_amps, seconds=6.0):
    t = np.arange(int(SR * seconds)) / SR
    return sum(a * np.sin(2 * np.pi * f * t) for f, a in freqs_amps)


def record(name, src, settle=0.8):
    """A DAC A monitorja (sztereó), közben a forrás az Aqulify eszközre szól."""
    out = os.path.join(WORK, "rec_" + name + ".wav")   # külön név: a forrásfájlt nem írhatja felül
    rec = subprocess.Popen(["pw-record", "--target", "dac_a", "-P", "{ stream.capture.sink = true }", "--rate", str(SR),
                            "--channels", "2", "--format", "s16", out])
    time.sleep(settle)
    subprocess.run(["pw-play", "--target", "aqulify_sink", src], check=True)
    time.sleep(0.3)
    rec.terminate(); rec.wait()
    w = wave.open(out)
    d = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, 2).astype(np.float64) / 32768.0
    w.close()
    return d


def seg(d, start=0.5, length=1.0):
    """A hangos szakasz (a lejátszás eleje után `start` s-tól `length` s) – a csend nélkül."""
    env = np.abs(d).max(axis=1)
    loud = np.where(env > 1e-3)[0]
    if len(loud) == 0:
        return d[: int(SR * length)]
    a = loud[0] + int(SR * start)
    return d[a: a + int(SR * length)]


def level(x, f):
    n = len(x)
    w = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(n) / (n - 1))
    c = np.sum(w * x * np.exp(-2j * np.pi * f * np.arange(n) / SR))
    return 20 * math.log10(max(2 * abs(c) / (n * 0.5), 1e-9))


def peak_db(x):
    return 20 * math.log10(max(np.abs(x).max(), 1e-9))


def main():
    ws = WS(PORT)
    ws.send({"type": "hello", "client": "modules_live"})
    ws.send({"type": "subscribe", "spectrum": 0})
    send = ws.send

    def all_off():
        for m in ("eq", "crossfeed", "convolver", "limiter", "bass", "mono", "autogain", "magic", "loudness", "stereo"):
            send({"type": "set_module_" + m, "val": 0})
        send({"type": "set_preamp", "val": 0})
        time.sleep(0.6)

    tone = write_wav("tone.wav", sines([(100, 0.25), (4000, 0.25)]), sines([(100, 0.25), (4000, 0.25)]))
    left_only = write_wav("left.wav", sines([(1000, 0.3), (60, 0.3)]), np.zeros(SR * 6))
    loud = write_wav("loud.wav", sines([(1000, 0.9)]), sines([(1000, 0.9)]))
    bass = write_wav("bass.wav", sines([(60, 0.3)]), sines([(60, 0.3)]))
    sub = write_wav("sub.wav", sines([(20, 0.25), (1000, 0.25)]), sines([(20, 0.25), (1000, 0.25)]))
    quiet = write_wav("quiet.wav", sines([(1000, 0.02)], 10.0), sines([(1000, 0.02)], 10.0))
    all_off()

    print("[1] Alap: minden modul ki – a hang érintetlen")
    s = seg(record("base", tone))
    l100, l4k = level(s[:, 0], 100), level(s[:, 0], 4000)
    check(abs(l100 + 12) < 0.2 and abs(l4k + 12) < 0.2, f"100 Hz {l100:.2f}, 4 kHz {l4k:.2f} dBFS (elvárt −12,0)")

    print("[2] Előerősítés −6 dB (EQ bekapcsolva, sáv nélkül)")
    send({"type": "set_module_eq", "val": 1}); send({"type": "set_eq", "bands": [], "preamp": -6})
    time.sleep(0.6)
    s = seg(record("preamp", tone))
    check(abs(level(s[:, 0], 1000 if False else 100) + 18) < 0.3, f"100 Hz {level(s[:, 0], 100):.2f} dBFS (elvárt −18,0)")
    all_off()

    print("[3] EQ: +6 dB harang 4 kHz-en, mind a négy módban (IIR, FIR, FFT, SPM)")
    send({"type": "set_module_eq", "val": 1})
    send({"type": "set_eq", "bands": [{"type": 1, "mode": 0, "freq": 4000, "gain": 6, "q": 1.0}], "preamp": 0})
    for mode, name in ((0, "IIR"), (1, "FIR"), (2, "FFT"), (3, "SPM")):
        send({"type": "set_eq_mode", "mode": mode})
        time.sleep(1.2)
        s = seg(record("eq_" + name, tone), start=0.8)
        a, b = level(s[:, 0], 100), level(s[:, 0], 4000)
        check(abs(b + 6) < 0.5 and abs(a + 12) < 0.4, f"{name}: 4 kHz {b:.2f} dBFS (elvárt −6), 100 Hz {a:.2f} dBFS (elvárt −12)")
    send({"type": "set_eq_mode", "mode": 0})

    print("[4] EQ kikapcsolva (Bypass): az eredeti jel")
    # Közvetlenül az SPM -> IIR módváltás után: ugyanannyi idő kell, mint a [3]-ban a módváltásnak,
    # különben a lassú CI-gépen a felvétel eleje még a váltás csendjébe eshet
    send({"type": "set_module_eq", "val": 0})
    time.sleep(1.2)
    s = seg(record("eq_off", tone), start=0.8)
    check(abs(level(s[:, 0], 4000) + 12) < 0.2, f"4 kHz {level(s[:, 0], 4000):.2f} dBFS (elvárt −12,0)")
    all_off()

    print("[5] Crossfeed: csak a bal csatornán szóló hang átszivárog a jobbra (mélyebben erősebben)")
    s = seg(record("cf_off", left_only))
    r_off = level(s[:, 1], 60)
    send({"type": "set_module_crossfeed", "val": 1}); send({"type": "set_crossfeed", "preset": 0})
    time.sleep(0.6)
    s = seg(record("cf_on", left_only))
    r60, r1k = level(s[:, 1], 60), level(s[:, 1], 1000)
    check(r_off < -80 and r60 > -40 and r60 > r1k + 3, f"jobb csatorna: ki {r_off:.1f}, be 60 Hz {r60:.1f}, 1 kHz {r1k:.1f} dBFS")
    all_off()

    print("[6] Konvolver: 0,5-ös impulzus (−6 dB), autogain nélkül")
    name = b"teszt"
    ir = b"AEIR" + struct.pack("<5I", 1, SR, 1, 256, len(name)) + name + struct.pack("<f", 0.5) + b"\0" * (255 * 4)
    send({"type": "set_convolver", "autogain": 0, "dry": -100, "wet": 0})
    send({"type": "set_module_convolver", "val": 1})
    ws.send_binary(ir)
    time.sleep(1.0)
    s = seg(record("conv", tone))
    c100 = level(s[:, 0], 100)
    check(abs(c100 + 18) < 0.4, f"100 Hz {c100:.2f} dBFS (elvárt −18)")
    send({"type": "clear_ir"})
    all_off()

    print("[7] Limiter: −0,9 dBFS-es jel, −6 dB küszöb")
    send({"type": "set_module_limiter", "val": 1}); send({"type": "set_limiter", "threshold": -6})
    time.sleep(0.8)
    s = seg(record("lim", loud), start=1.0)
    pk = peak_db(s[:, 0])
    check(pk < -5.5 and pk > -8.0, f"csúcs {pk:.2f} dBFS (elvárt ≈ −6)")
    all_off()

    print("[8] Bass Enhancer: 60 Hz-es hanghoz felharmonikusok (120 / 180 Hz)")
    s = seg(record("bass_off", bass))
    h_off = max(level(s[:, 0], 120), level(s[:, 0], 180))
    send({"type": "set_module_bass", "val": 1}); send({"type": "set_bass", "amount": 6, "harmonics": 8.5})
    time.sleep(0.8)
    s = seg(record("bass_on", bass))
    h_on = max(level(s[:, 0], 120), level(s[:, 0], 180))
    check(h_on > h_off + 20, f"felharmonikus: ki {h_off:.1f}, be {h_on:.1f} dBFS")
    all_off()

    print("[9] Mono és mélyvágás")
    send({"type": "set_module_mono", "val": 1}); send({"type": "set_mono", "subsonic": 35, "amount": 0})
    time.sleep(0.8)
    rec = record("sub", sub)
    s = seg(rec, start=1.0)
    a20, a1k = level(s[:, 0], 20), level(s[:, 0], 1000)
    check(a20 < -12 - 15 and abs(a1k + 12) < 0.3,
          f"35 Hz-es vágás: 20 Hz {a20:.1f} dBFS (≥ 15 dB csillapítás), 1 kHz {a1k:.2f} (a felvétel csúcsa {peak_db(rec[:, 0]):.1f}, hossza {len(rec) / SR:.1f} s)")
    send({"type": "set_mono", "subsonic": 0, "amount": 100, "freq": 100})
    time.sleep(0.8)
    s = seg(record("mono", left_only), start=1.0)
    l60, r60, l1k, r1k = level(s[:, 0], 60), level(s[:, 1], 60), level(s[:, 0], 1000), level(s[:, 1], 1000)
    # 1 kHz-en az oldalsó jel érintetlen, csak a felüláteresztő kis fázisfordítása szivárog át (a felület jelzi)
    check(abs(l60 - r60) < 1.0 and r1k < l1k - 15,
          f"mono 100 % 100 Hz alatt: 60 Hz bal {l60:.1f} / jobb {r60:.1f} dBFS; 1 kHz bal {l1k:.1f} / jobb {r1k:.1f} (fázismaradék)")
    all_off()

    print("[10] Autogain: halk (−34 dBFS) hang lassú felhúzása (legfeljebb 1 dB/s, pumpálás nélkül)")
    send({"type": "set_module_autogain", "val": 1}); send({"type": "set_autogain", "target": -14, "maxGain": 24})
    time.sleep(0.8)
    rec = record("agc", quiet)
    g1, g2 = level(seg(rec, start=2.0, length=1.0)[:, 0], 1000), level(seg(rec, start=8.0, length=1.0)[:, 0], 1000)
    rate = (g2 - g1) / 6.0
    check(g2 > -34 + 3 and 0.3 < rate <= 1.15, f"1 kHz: 2 s-nál {g1:.1f}, 8 s-nál {g2:.1f} dBFS (emelkedés {rate:.2f} dB/s; elvárt ≤ 1 dB/s)")
    all_off()

    print("[11] Magic Touch: tiszta szinuszon nem változtat a szinten")
    send({"type": "set_module_magic", "val": 1}); send({"type": "set_magic", "amount": 100})
    time.sleep(1.0)
    s = seg(record("magic", tone), start=1.0)
    m100, m4k = level(s[:, 0], 100), level(s[:, 0], 4000)
    check(abs(m100 + 12) < 1.0 and abs(m4k + 12) < 1.0, f"100 Hz {m100:.2f}, 4 kHz {m4k:.2f} dBFS (±1 dB)")
    all_off()

    print("[12] A/B: az EQ előtte / utána összehasonlítása")
    send({"type": "set_module_eq", "val": 1})
    send({"type": "set_eq", "bands": [{"type": 1, "mode": 0, "freq": 4000, "gain": 6, "q": 1.0}], "preamp": 0})
    send({"type": "set_eq_compare", "compare": 1})
    time.sleep(1.0)
    s = seg(record("ab", tone), start=1.0)
    d = level(s[:, 0], 4000) - level(s[:, 0], 100)
    check(abs(d) < 1.0, f"„előtte” állásban a 4 kHz és a 100 Hz különbsége {d:.2f} dB (EQ nélkül 0)")
    send({"type": "set_eq_compare", "compare": 0})
    all_off()

    print("[13] Loudness: ISO 226 görbe kiemelése alacsony hangerőn (-30 dB)")
    send({"type": "set_module_loudness", "val": 1})
    send({"type": "set_loudness", "volume": -30.0, "inputGain": 0.0, "outputGain": 0.0, "std": 1, "mode": 0, "hclip": 0, "hclipRange": 6.0, "approx": 2})
    time.sleep(1.0)
    s = seg(record("loudness", tone), start=1.0)
    l100, l4k = level(s[:, 0], 100), level(s[:, 0], 4000)
    check(l100 > l4k, f"Loudness bekapcsolva: 100 Hz ({l100:.2f} dBFS) > 4 kHz ({l4k:.2f} dBFS)")
    all_off()

    print("[14] Stereo Tools: -6 dB kimeneti szint")
    send({"type": "set_module_stereo", "val": 1})
    send({"type": "set_stereo", "inputGain": 0.0, "outputGain": -6.0, "balanceIn": 0.0, "balanceOut": 0.0, "softclip": 0, "mutel": 0, "muter": 0, "phasel": 0, "phaser": 0, "mode": 0, "slev": 0.0, "sbal": 0.0, "mlev": 0.0, "mpan": 0.0, "stereoBase": 0.0, "delay": 0.0, "scLevel": 1.0, "stereoPhase": 0.0, "dry": -100.0, "wet": 0.0})
    time.sleep(0.8)
    s = seg(record("stereo", tone), start=0.8)
    l100 = level(s[:, 0], 100)
    check(abs(l100 + 18) < 0.5, f"Stereo Tools -6 dB kimenet: 100 Hz {l100:.2f} dBFS (elvárt -18)")
    all_off()

    print("\n%s (%d hiba)" % ("SIKERTELEN" if fails else "MINDEN MODULTESZT SIKERES", fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
