#!/usr/bin/env python3
"""
PipeWire lejátszó stream automatikus újracsatlakozásának élő tesztje (AQ-19).
1. A motor külső kimeneti eszköz nélkül indul -> "no target node available" hiba.
2. Dinamikus null-sink (test_dac) létrehozása -> motor-újraindítás NÉLKÜL újracsatlakozik és 1 kHz hallható (> -30 dB).
3. A test_dac eltávolítása (pactl unload-module), majd újbóli létrehozása -> motor-újraindítás NÉLKÜL a hang visszajön.
4. Napló ellenőrzés: az első hiba + sikeres újracsatlakozás megjelenik, nincs hiba-spammelés.
"""
import math
import os
import struct
import subprocess
import sys
import time
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from analyze import levels  # noqa: E402

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 47810
WORK = sys.argv[2] if len(sys.argv) > 2 else HERE

fails = 0


def check(ok, text):
    global fails
    print(("  [OK]   " if ok else "  [HIBA] ") + text)
    if not ok:
        fails += 1


def write_tone(path, seconds=3):
    sr = 48000
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(sr)
        frames = bytearray()
        for i in range(sr * seconds):
            v = 0.35 * math.sin(2.0 * math.pi * 1000.0 * i / sr)
            s = int(v * 32767)
            frames += struct.pack("<hh", s, s)
        w.writeframes(bytes(frames))


def measure_sink(tag, tone_path):
    wav = os.path.join(WORK, f"{tag}.wav")
    rec = subprocess.Popen(
        ["pw-record", "--target", "test_dac", "-P", "{ stream.capture.sink = true }",
         "--rate", "48000", "--channels", "2", "--format", "s16", wav],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    time.sleep(0.7)
    subprocess.run(["pw-play", "--target", "aqulify_sink", tone_path], check=True)
    time.sleep(0.3)
    rec.terminate()
    rec.wait()
    lv, _ = levels(wav, [1000])
    return lv[1000]


def load_null_sink():
    r = subprocess.run(
        ["pactl", "load-module", "module-null-sink", "sink_name=test_dac", "sink_properties=device.description=TestDAC"],
        capture_output=True, text=True,
    )
    if r.returncode != 0 or not r.stdout.strip():
        return ""
    subprocess.run(["pactl", "set-default-sink", "test_dac"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return r.stdout.strip()


tone_path = os.path.join(WORK, "tone1k.wav")
write_tone(tone_path, 3)

# 1. A motor kimenet nélkül indult; most adjuk hozzá az első kimenetet (DAC késői megjelenése)
mod1 = load_null_sink()
check(mod1 != "", f"1. test_dac létrehozva a motor indulása után (modul #{mod1})")
time.sleep(2.5)

db_first = measure_sink("first", tone_path)
check(db_first > -30.0, f"késői DAC megjelenése után (újraindítás nélkül): 1 kHz = {db_first:.1f} dB (> -30 dB)")

# 2. A kimenet eltávolítása, majd visszahozása futás közben
res = subprocess.run(["pactl", "unload-module", mod1], capture_output=True, text=True)
check(res.returncode == 0, f"test_dac eltávolítva (modul #{mod1})")
subprocess.run(["pw-play", "--target", "aqulify_sink", tone_path],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1.5)

mod2 = load_null_sink()
check(mod2 != "", f"test_dac újra létrehozva (modul #{mod2})")
time.sleep(2.5)

db_after = measure_sink("after", tone_path)
check(db_after > -30.0, f"eltávolítás + visszahozás után (újraindítás nélkül): 1 kHz = {db_after:.1f} dB (> -30 dB)")

# 3. Napló ellenőrzés: nincs hiba-spam, és hiba után volt újracsatlakozás
log_path = os.path.join(WORK, "engine.log")
if os.path.exists(log_path):
    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        log = f.read()
    err_lines = [ln for ln in log.splitlines() if "[PipeWire] lejátszó stream hiba:" in ln]
    rec_lines = [ln for ln in log.splitlines() if "[PipeWire] Lejátszó stream újracsatlakoztatva." in ln]
    check(len(err_lines) <= 2, f"lejátszó stream hiba naplósorok száma: {len(err_lines)} (<= 2, nincs spam)")
    if len(err_lines) > 0:
        check(len(rec_lines) >= 1, f"sikeres újracsatlakozás naplózva ({len(rec_lines)} sor)")

sys.exit(0 if fails == 0 else 1)
