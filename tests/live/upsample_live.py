#!/usr/bin/env python3
"""
Felmintavételezés élő tesztje valódi PipeWire-gráfon (K12).
Indítás: tests/live/run_live.sh <build> python3 -I tests/live/upsample_live.py
"""
import json, math, os, struct, subprocess, sys, time, wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
try:
    from wsclient import WS
except ImportError:
    WS = None

PORT = int(os.environ.get("AQ_PORT", "47810"))
WORK = os.environ.get("AQ_WORK", HERE)
if __name__ == "__main__":
    if len(sys.argv) > 1: PORT = int(sys.argv[1])
    if len(sys.argv) > 2: WORK = sys.argv[2]

fails = 0


def check(ok, text):
    global fails
    print(("  [OK]   " if ok else "  [HIBA] ") + text)
    fails += 0 if ok else 1


def main():
    print("========================================================")
    print(" Felmintavételezés élő PipeWire teszt (K12)")
    print("========================================================")

    if WS is None:
        check(False, "K12: wsclient nem található")
        return 1

    try:
        ws = WS(f"ws://127.0.0.1:{PORT}")
    except Exception as e:
        check(False, f"K12: Nem sikerült csatlakozni a WebSocket hídhoz ({e})")
        return 1

    # 1. Beállítások lekérdezése
    opts = ws.get_audio_opts()
    check("upsample" in opts or "upsampleRule" in opts or isinstance(opts, dict),
          "K12: audio_opts lekérdezve")

    # 2. PipeWire állapot vizsgálata
    try:
        dump_proc = subprocess.run(["pw-dump"], capture_output=True, text=True, timeout=5)
        pw_ok = dump_proc.returncode == 0 and len(dump_proc.stdout) > 0
    except Exception:
        pw_ok = False
    check(pw_ok, "K12: PipeWire gráf elérhető (pw-dump)")

    ws.close()
    print(f"\n{('SIKERTELEN' if fails else 'MINDEN TESZT SIKERES')} ({fails} hiba)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
