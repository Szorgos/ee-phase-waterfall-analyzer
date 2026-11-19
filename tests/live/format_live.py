#!/usr/bin/env python3
"""
A „most szól” forrás-formátuma (media.fmt) élő tesztje valódi motorral.
Indítás: tests/live/multiout_live.sh <build> python3 -I tests/live/format_live.py

Ellenőrzi: egy 24 bites / 96 kHz-es és egy 16 bites / 44,1 kHz-es WAV lejátszásakor az állapotüzenet media.fmt
mezője a fájl saját bitmélységét és mintavételét adja (nem a kimenetét), leállítás után nincs media.fmt.
"""
import math, os, struct, sys, time, wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wsclient import WS  # noqa: E402

PORT = int(os.environ.get("AQ_PORT", "47810"))
WORK = os.environ.get("AQ_WORK", "/tmp")
fails = 0


def check(ok, text):
    global fails
    print(("  [OK]   " if ok else "  [HIBA] ") + text)
    if not ok:
        fails += 1


def make_wav(path, sr, width, seconds=6):
    n = int(sr * seconds)
    amp = (1 << (8 * width - 1)) - 1
    buf = bytearray()
    for i in range(n):
        v = int(0.2 * amp * math.sin(2 * math.pi * 440 * i / sr))
        b = v.to_bytes(width, "little", signed=True)
        buf += b + b
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(width)
        w.setframerate(sr)
        w.writeframes(bytes(buf))


def status_with_media(ws, want_available, tries=40):
    for _ in range(tries):
        m = ws.wait_type("status", limit=3000)
        media = (m or {}).get("media") or {}
        if bool(media.get("available")) == want_available:
            return media
    return {}


def play_and_read(path):
    ws = WS(PORT)
    ws.send({"type": "hello", "client": "format_live"})
    ws.send({"type": "subscribe", "spectrum": 0})
    ws.send({"type": "player_open", "path": path})
    media = status_with_media(ws, True)
    ws.send({"type": "player_stop"})
    after = status_with_media(ws, False)
    ws.s.close()
    return media, after


for sr, width, label in ((96000, 3, "24 bit / 96 kHz"), (44100, 2, "16 bit / 44,1 kHz")):
    p = os.path.join(WORK, f"aq_format_{sr}_{width * 8}.wav")
    make_wav(p, sr, width)
    media, after = play_and_read(p)
    fmt = media.get("fmt") or {}
    check(fmt.get("bits") == width * 8 and fmt.get("rate") == sr and fmt.get("dsd") == 0,
          f"{label}: media.fmt = {fmt}")
    check("fmt" not in after, f"{label}: leállítás után nincs media.fmt")

print("MINDEN OK" if fails == 0 else f"{fails} HIBA")
sys.exit(1 if fails else 0)
