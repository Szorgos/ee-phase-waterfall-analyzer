#!/usr/bin/env python3
"""
Több kimenet élő tesztje valódi PipeWire-gráfon (Linux). A multiout_live.sh indítja egy saját, elszigetelt
PipeWire-környezetben két virtuális hangkártyával (DAC A, DAC B); a motor a megadott porton fut.

Esetek: független zónák (−6 dB a 2. kimeneten), a 2. zóna saját EQ-ja, LR4 keresztváltó 500 Hz-en, kikapcsolás,
eltűnő céleszköz (a stream nem kötődhet át máshová). Mérés: pw-record a két hangkártya monitorjáról.
"""
import json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wsclient import WS          # noqa: E402
from analyze import levels       # noqa: E402

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 47890
WORK = sys.argv[2] if len(sys.argv) > 2 else HERE
fails = 0


def check(ok, text):
    global fails
    print(("  [OK]   " if ok else "  [HIBA] ") + text)
    fails += 0 if ok else 1


def record(name, seconds_tone):
    """Felvétel mindkét hangkártyáról, közben a teszthang az Aqulify eszközre szól."""
    a = os.path.join(WORK, name + "_a.wav")
    b = os.path.join(WORK, name + "_b.wav")
    rec = [subprocess.Popen(["pw-record", "--target", t, "-P", "{ stream.capture.sink = true }", "--rate", "48000",
                             "--channels", "2", "--format", "s16", f]) for t, f in (("dac_a", a), ("dac_b", b))]
    time.sleep(0.8)
    subprocess.run(["pw-play", "--target", "aqulify_sink", seconds_tone], check=True)
    time.sleep(0.3)
    for r in rec:
        r.terminate()
        r.wait()
    la, _ = levels(a, [100, 4000])
    lb, _ = levels(b, [100, 4000])
    return la, lb


def tone(path):
    import math, struct, wave
    sr = 48000
    w = wave.open(path, "wb")
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr)
    fr = bytearray()
    for i in range(sr * 6):
        v = 0.25 * math.sin(2 * math.pi * 100 * i / sr) + 0.25 * math.sin(2 * math.pi * 4000 * i / sr)
        s = int(v * 32767)
        fr += struct.pack("<hh", s, s)
    w.writeframes(bytes(fr)); w.close()


def fresh_status(ws):
    """Állapot egy külön, friss kapcsolaton (a fő kapcsolat sorában korábbi válaszok állhatnak)."""
    q = WS(PORT)
    q.send({"type": "hello", "client": "multiout_live_status"})
    q.send({"type": "subscribe", "spectrum": 0})
    q.send({"type": "get_multiout"})
    m = q.wait_type("multiout", limit=5000)
    q.s.close()
    return m["status"] if m else {}


def main():
    t = os.path.join(WORK, "tone.wav")
    tone(t)
    ws = WS(PORT)
    ws.send({"type": "hello", "client": "multiout_live"})
    ws.send({"type": "subscribe", "spectrum": 0})   # a spektrumkeretek nem kellenek (ne töltsék a sort)
    fmt = lambda a, b: f"A 100 Hz {a[100]:.1f} / 4 kHz {a[4000]:.1f} dBFS | B 100 Hz {b[100]:.1f} / 4 kHz {b[4000]:.1f} dBFS"

    print("[1] Független zónák: a 2. kimenet a DAC B, −6 dB")
    ws.send({"type": "set_multiout", "enabled": True, "mode": "zones", "outputs": [{}, {"sink": "dac_b", "gain": -6}]})
    time.sleep(0.8)
    a, b = record("zones", t)
    check(abs(a[100] + 12) < 0.3 and abs(a[4000] + 12) < 0.3 and abs(b[100] + 18) < 0.3 and abs(b[4000] + 18) < 0.3, fmt(a, b))

    print("[2] A 2. zóna saját EQ-ja (+6 dB 4 kHz-en); az 1. kimenet érintetlen")
    ws.send({"type": "set_multiout", "enabled": True, "mode": "zones", "outputs": [{}, {"sink": "dac_b"}]})
    ws.send({"type": "set_module_eq", "val": 1, "zone": 1})
    ws.send({"type": "set_eq", "zone": 1, "bands": [{"type": 1, "mode": 0, "freq": 4000, "gain": 6, "q": 1.0}]})
    time.sleep(0.8)
    a, b = record("zeq", t)
    check(abs(a[4000] + 12) < 0.3 and abs(b[4000] + 6) < 0.5 and abs(b[100] + 12) < 0.3, fmt(a, b))
    ws.send({"type": "set_module_eq", "val": 0, "zone": 1})

    print("[3] Keresztváltó 500 Hz, LR4: DAC A a mély, DAC B a magas sáv")
    ws.send({"type": "set_multiout", "enabled": True, "mode": "crossover", "slope": 24, "crossovers": [500],
             "outputs": [{"band": 0}, {"sink": "dac_b", "band": 1}]})
    time.sleep(0.8)
    a, b = record("xo", t)
    check(abs(a[100] + 12) < 0.3 and a[4000] < -60 and abs(b[4000] + 12) < 0.3 and b[100] < -55, fmt(a, b))
    st = fresh_status(ws)
    z = (st.get("zones") or [{}])[0]
    check(z.get("underruns", 1) == 0, f"alulcsordulás a 2. kimeneten: {z.get('underruns')}")

    print("[4] Kikapcsolás: a DAC B néma, a DAC A a régi úton szól")
    ws.send({"type": "set_multiout", "enabled": False})
    time.sleep(0.8)
    a, b = record("off", t)
    check(abs(a[100] + 12) < 0.3 and abs(a[4000] + 12) < 0.3 and b[100] < -80 and b[4000] < -80, fmt(a, b))

    print("[5] Eltűnő céleszköz: a 3. kimenet streamje nem kötődik át máshová")
    mod = subprocess.run(["pactl", "load-module", "module-null-sink", "sink_name=dac_c"], capture_output=True, text=True).stdout.strip()
    time.sleep(1.0)
    ws.send({"type": "set_multiout", "enabled": True, "mode": "zones", "outputs": [{}, {"sink": "dac_b"}, {"sink": "dac_c"}]})
    time.sleep(1.0)
    links = subprocess.run(["pw-link", "-l"], capture_output=True, text=True).stdout
    check("|-> dac_c:playback_FL" in links, "a 3. kimenet a DAC C-re kötődött")
    subprocess.run(["pactl", "unload-module", mod])
    time.sleep(1.5)
    links = subprocess.run(["pw-link", "-l"], capture_output=True, text=True).stdout
    check(not any("zone3" in l for l in links.splitlines()), "a DAC C eltávolítása után a 3. kimenetnek nincs kapcsolata")
    a, b = record("gone", t)
    check(abs(a[100] + 12) < 0.3 and abs(a[4000] + 12) < 0.3, "a DAC A-n nincs többletjel: " + fmt(a, b))

    print("[6] Ismeretlen céleszköz: elutasítva")
    ws.send({"type": "set_multiout", "enabled": True, "mode": "zones", "outputs": [{}, {"sink": "nincs_ilyen"}]})
    time.sleep(0.5)
    st = fresh_status(ws)
    check(((st.get("outputs") or [{}, {}])[1]).get("sink", "x") == "", "az ismeretlen nyelő nem lesz céleszköz")
    ws.send({"type": "set_multiout", "enabled": False})

    print("[7] Időzítés: a kimenetek egyszerre szólnak (zajjel, kereszt-korreláció)")
    noise = os.path.join(WORK, "noise.wav")
    import random, struct, wave
    rnd = random.Random(1)
    w = wave.open(noise, "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
    fr = bytearray()
    for _ in range(48000 * 5):
        v = int(rnd.uniform(-0.3, 0.3) * 32767); fr += struct.pack("<hh", v, v)
    w.writeframes(bytes(fr)); w.close()
    ws.send({"type": "set_multiout", "enabled": True, "mode": "zones", "outputs": [{}, {"sink": "dac_b"}]})
    time.sleep(0.8)
    # egyetlen felvevő stream: bal = DAC A monitor, jobb = DAC B monitor (közös óra, mintapontos összevetés)
    lagwav = os.path.join(WORK, "lag.wav")
    rec = subprocess.Popen(["pw-record", "--target", "0", "-P", "{ node.name = aq_lagrec }", "--rate", "48000",
                            "--channels", "2", "--format", "s16", lagwav])
    time.sleep(1.0)
    for src, dst in (("dac_a:monitor_FL", "aq_lagrec:input_FL"), ("dac_b:monitor_FL", "aq_lagrec:input_FR")):
        subprocess.run(["pw-link", src, dst])
    time.sleep(0.5)
    subprocess.run(["pw-play", "--target", "aqulify_sink", noise], check=True)
    time.sleep(0.3)
    rec.terminate(); rec.wait()
    out = subprocess.run([sys.executable, "-I", os.path.join(HERE, "lag_probe.py"), lagwav, lagwav, "stereo"],
                         capture_output=True, text=True).stdout.split()
    lag = int(out[0]) if out else 99999
    peak = float(out[2]) if len(out) > 2 else 0.0
    check(abs(lag) <= 2 and peak > 20, f"a DAC B a DAC A-hoz képest {lag} mintával ({lag / 48.0:.2f} ms) késik (korrelációs csúcs {peak:g}×)")
    ws.send({"type": "set_multiout", "enabled": False})

    print("\n%s (%d hiba)" % ("SIKERTELEN" if fails else "MINDEN ÉLŐ TESZT SIKERES", fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
