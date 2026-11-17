#!/usr/bin/env python3
"""
Útonkénti EQ élő tesztje valódi PipeWire-gráfon (K14).
Indítás: tests/live/multiout_live.sh <build> python3 -I tests/live/way_eq_live.py
"""
import json, math, os, signal, struct, subprocess, sys, time, wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wsclient import WS      # noqa: E402
from analyze import levels   # noqa: E402

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


def tone(path):
    sr = 48000
    w = wave.open(path, "wb")
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr)
    fr = bytearray()
    for i in range(sr * 6):
        v = 0.25 * math.sin(2 * math.pi * 100 * i / sr) + 0.25 * math.sin(2 * math.pi * 4000 * i / sr)
        s = int(v * 32767)
        fr += struct.pack("<hh", s, s)
    w.writeframes(bytes(fr)); w.close()


def record(name, tone_path):
    a = os.path.join(WORK, name + "_a.wav")
    b = os.path.join(WORK, name + "_b.wav")
    rec = [subprocess.Popen(["pw-record", "--target", t, "-P", "{ stream.capture.sink = true }", "--rate", "48000",
                             "--channels", "2", "--format", "s16", f]) for t, f in (("dac_a", a), ("dac_b", b))]
    time.sleep(0.8)
    subprocess.run(["pw-play", "--target", "aqulify_sink", tone_path], check=True)
    time.sleep(0.3)
    for r in rec:
        r.terminate()
        r.wait()
    la, _ = levels(a, [100, 4000])
    lb, _ = levels(b, [100, 4000])
    return la, lb


def get_status():
    q = WS(PORT)
    q.send({"type": "hello", "client": "way_eq_live_status"})
    q.send({"type": "subscribe", "spectrum": 0})
    q.send({"type": "get_multiout"})
    m = q.wait_type("multiout", limit=5000)
    q.s.close()
    return m["status"] if m else {}


def find_engine_proc():
    for pid_str in os.listdir("/proc"):
        if not pid_str.isdigit():
            continue
        try:
            with open(f"/proc/{pid_str}/cmdline", "rb") as f:
                args = [a.decode("utf-8", "ignore") for a in f.read().split(b"\0") if a]
            if args and "aqulify-engine" in os.path.basename(args[0]) and str(PORT) in args:
                exe = os.readlink(f"/proc/{pid_str}/exe")
                return int(pid_str), exe, args[1:]
        except Exception:
            continue
    return None, None, None


def main():
    t = os.path.join(WORK, "tone.wav")
    tone(t)

    ws = WS(PORT)
    ws.send({"type": "hello", "client": "way_eq_live"})
    ws.send({"type": "subscribe", "spectrum": 0})

    print("[1] Alap keresztváltó 500 Hz (DAC A = mély, DAC B = magas)")
    ws.send({"type": "set_multiout", "enabled": True, "mode": "crossover", "slope": 24, "crossovers": [500],
             "outputs": [{"band": 0}, {"sink": "dac_b", "band": 1}]})
    time.sleep(0.8)
    a0, b0 = record("xo_base", t)
    check(abs(a0[100] + 12) < 0.3 and a0[4000] < -60 and abs(b0[4000] + 12) < 0.3 and b0[100] < -55,
          f"Alap XO: A(100={a0[100]:.1f}, 4k={a0[4000]:.1f}) B(100={b0[100]:.1f}, 4k={b0[4000]:.1f})")

    print("[2] K14: 2. úton Bell +6 dB @ 4 kHz, Q=1.0")
    ws.send({"type": "set_way_eq", "output": 1, "on": True,
             "bands": [{"type": "bell", "freq": 4000, "gain": 6, "q": 1.0}]})
    m = ws.wait_type("multiout", limit=500)
    time.sleep(0.8)
    a1, b1 = record("xo_eq", t)
    print(f'K_METRIC: {{"test":"K14","a_100":{a1[100]:.2f},"a_4k":{a1[4000]:.2f},"b_100":{b1[100]:.2f},"b_4k":{b1[4000]:.2f}}}')
    check(abs(b1[4000] + 6.0) < 0.3 and b1[100] < -55,
          f"K14 DAC B (+6 dB @ 4 kHz): 4 kHz={b1[4000]:.2f} dBFS (-6.0±0.3), 100 Hz={b1[100]:.2f} (<-55)")
    check(abs(a1[100] + 12.0) < 0.3 and a1[4000] < -60,
          f"K14 DAC A változatlan: 100 Hz={a1[100]:.2f} dBFS (-12.0±0.3), 4 kHz={a1[4000]:.2f} (<-60)")

    st = m["status"] if m else get_status()
    has_way = bool(st.get("outputs") and len(st["outputs"]) > 1 and st["outputs"][1].get("way", {}).get("on") is True)
    has_resp = bool(st.get("response") and st["response"].get("freqs") and st["response"].get("ways"))
    check(has_way and has_resp, "K14 állapot JSON tartalmazza a way és response mezőket")
    ws.s.close()

    print("[3] K14: motor újraindítása után az útonkénti EQ megmarad")
    pid, exe, args = find_engine_proc()
    check(pid is not None and exe is not None, f"futó motor megtalálva (pid={pid})")
    if pid and exe:
        os.kill(pid, signal.SIGTERM)
        for _ in range(40):
            try:
                os.kill(pid, 0)
                time.sleep(0.1)
            except ProcessLookupError:
                break
        logf = open(os.path.join(WORK, "engine_restarted.log"), "wb")
        new_proc = subprocess.Popen([exe] + args, stdout=logf, stderr=subprocess.STDOUT)
        try:
            for _ in range(50):
                try:
                    st2 = get_status()
                    if st2:
                        break
                except Exception:
                    time.sleep(0.15)
            time.sleep(1.5)
            a2, b2 = record("xo_eq_after_restart", t)
            print(f'K_METRIC: {{"test":"K14_restart","b_4k":{b2[4000]:.2f},"b_100":{b2[100]:.2f},"a_100":{a2[100]:.2f}}}')
            check(abs(b2[4000] + 6.0) < 0.3 and b2[100] < -55 and abs(a2[100] + 12.0) < 0.3,
                  f"K14 újraindítás után megmaradt: B 4 kHz={b2[4000]:.2f}, B 100 Hz={b2[100]:.2f}, A 100 Hz={a2[100]:.2f}")
        finally:
            new_proc.terminate()
            new_proc.wait()
            logf.close()

    print(f"\n{'SIKERTELEN' if fails else 'MINDEN ÉLŐ WAY EQ TESZT SIKERES'} ({fails} hiba)")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
