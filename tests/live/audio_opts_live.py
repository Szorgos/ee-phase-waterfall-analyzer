#!/usr/bin/env python3
"""Haladó hangkimenet élő tesztje (Linux): pufferméret, ébren tartás, mintavétel, újramintavételezés, jelút.

Indítás: tests/live/multiout_live.sh <build> python3 -I tests/live/audio_opts_live.py
(a környezet: elszigetelt PipeWire, két null-nyelő: dac_a, dac_b; AQ_PORT, AQ_WORK).
"""
import json, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wsclient import WS  # noqa: E402

PORT = int(os.environ.get("AQ_PORT", "47810"))
WORK = os.environ.get("AQ_WORK", "/tmp")
fails = 0


def check(ok, msg):
    global fails
    print(("  [OK]   " if ok else "  [HIBA] ") + msg)
    if not ok:
        fails += 1


def opts(**kw):
    """Beállítás küldése (üres: csak lekérdezés); a válasz az aktuális beállítás és a képességek."""
    ws = WS(PORT)
    ws.send({"type": "hello", "client": "audio_opts_live"})
    ws.send({"type": "set_audio_opts" if kw else "get_audio_opts", "opts": kw})
    m = ws.wait_type("audio_opts", limit=2000)
    ws.s.close()
    return m


def status():
    ws = WS(PORT)
    ws.send({"type": "hello", "client": "audio_opts_live"})
    ws.send({"type": "subscribe", "spectrum": 0})
    m = ws.wait_type("status", limit=5000)
    ws.s.close()
    return (m or {}).get("path", {})


def nodes():
    out = subprocess.run(["pw-dump"], capture_output=True, text=True).stdout
    res = {}
    for o in json.loads(out or "[]"):
        if o.get("type") != "PipeWire:Interface:Node":
            continue
        info = o.get("info", {})
        props = info.get("props", {})
        res[props.get("node.name", "")] = {"state": info.get("state"), "props": props}
    return res


def setting(key):
    out = subprocess.run(["pw-metadata", "-n", "settings", "0"], capture_output=True, text=True).stdout
    tag = f"key:'{key}' value:'"
    p = out.find(tag)
    return out[p + len(tag):out.find("'", p + len(tag))] if p >= 0 else None


def tone(path, seconds=4):
    import math, struct, wave
    sr = 48000
    w = wave.open(path, "wb")
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr)
    fr = bytearray()
    for i in range(sr * seconds):
        s = int(0.2 * math.sin(2 * math.pi * 440 * i / sr) * 32767)
        fr += struct.pack("<hh", s, s)
    w.writeframes(bytes(fr)); w.close()


print("Haladó hangkimenet")
m = opts()
check(m is not None and m.get("opts", {}).get("buffer") == 0 and m.get("caps", {}).get("buffer") is True,
      f"alapállapot: automatikus puffer, a képességek megvannak ({json.dumps(m and m.get('opts'))})")
orig_allowed, orig_force = setting("clock.allowed-rates"), setting("clock.force-rate")

# --- pufferméret
m = opts(buffer=256)
time.sleep(0.8)
n = nodes()
lat = str(n.get("aqulify_sink", {}).get("props", {}).get("node.latency"))
check(m["opts"]["buffer"] == 256 and lat == "256/48000", f"puffer 256: node.latency = {lat}")
t = os.path.join(WORK, "ao_tone.wav")
tone(t)
p = subprocess.Popen(["pw-play", "--target", "aqulify_sink", t])
time.sleep(2.0)
st = status()
check(0 < st.get("quantum", 0) <= 256, f"lejátszás közben a quantum ≤ 256: {st.get('quantum')} ({st.get('bufMs')} ms)")
check(st.get("transparent") is True and st.get("load", -1) >= 0,
      f"jelút: bitpontos = {st.get('transparent')}, terhelés {st.get('load')} %, túllépés {st.get('overruns')}")
p.wait()
m = opts(buffer=1000)
check(m["opts"]["buffer"] == 1024, f"ismeretlen érték a legközelebbire kerekítve: 1000 -> {m['opts']['buffer']}")
m = opts(buffer=0)
time.sleep(0.8)
lat = nodes().get("aqulify_sink", {}).get("props", {}).get("node.latency")
check(m["opts"]["buffer"] == 0 and lat in (None, f"{setting('clock.quantum')}/48000"),
      f"automatikus puffer: a rendszer alapértéke ({lat})")

# --- ébren tartás
m = opts(keepAwake=True)
time.sleep(2.0)
n = nodes()
ka = n.get("aqulify_sink_keepalive")
check(ka is not None and ka["state"] == "running", f"ébren tartás: a csendes hangfolyam fut ({ka and ka['state']})")
check(n.get("dac_a", {}).get("state") == "running", f"ébren tartás: a hangkártya fut, ha senki nem játszik ({n.get('dac_a', {}).get('state')})")
check(status().get("keepAwake") is True, "jelút: ébren tartás jelezve")
opts(keepAwake=False)
time.sleep(1.5)
n = nodes()
check("aqulify_sink_keepalive" not in n, "ébren tartás ki: a hangfolyam megszűnt")
check(n.get("dac_a", {}).get("state") != "running", f"ébren tartás ki: a hangkártya nem fut ({n.get('dac_a', {}).get('state')})")

# --- mintavétel
opts(rateMode="follow")
time.sleep(0.5)
check(setting("clock.allowed-rates") == "[ 44100 48000 88200 96000 176400 192000 ]" and setting("clock.force-rate") == "0",
      f"forrást követő: allowed-rates = {setting('clock.allowed-rates')}")
opts(rateMode="fixed", fixedRate=96000)
time.sleep(0.5)
check(setting("clock.force-rate") == "96000", f"rögzített 96 kHz: force-rate = {setting('clock.force-rate')}")
opts(rateMode="system")
time.sleep(0.5)
check(setting("clock.allowed-rates") == orig_allowed and setting("clock.force-rate") == orig_force,
      f"rendszer: az eredeti visszaállt ({setting('clock.allowed-rates')}, {setting('clock.force-rate')})")

# --- újramintavételezés a további kimeneteken
ws = WS(PORT)
ws.send({"type": "hello", "client": "audio_opts_live"})
ws.send({"type": "set_multiout", "enabled": True, "mode": "zones", "outputs": [{}, {"sink": "dac_b"}]})
ws.wait_type("multiout", limit=2000)
ws.s.close()
opts(resampleQuality=10)
time.sleep(1.5)
z = nodes().get("aqulify_sink_zone2_output")
check(z is not None and str(z["props"].get("resample.quality")) == "10",
      f"újramintavételezés minősége a 2. kimeneten: {z and z['props'].get('resample.quality')}")

# --- megmarad és leálláskor a rendszer visszaáll
opts(rateMode="follow")
time.sleep(0.3)
saved = open(os.path.join(os.environ.get("XDG_CONFIG_HOME", ""), "aqulify", "audio.json")).read()
check('"rateMode":"follow"' in saved and '"resampleQuality":10' in saved, f"mentve: {saved}")
ws = WS(PORT)
ws.send({"type": "hello", "client": "audio_opts_live"})
ws.send({"type": "engine_quit"})
time.sleep(2.0)
check(setting("clock.allowed-rates") == orig_allowed, f"leálláskor a mintavétel visszaállt ({setting('clock.allowed-rates')})")

print("SIKERES" if fails == 0 else f"{fails} HIBA")
sys.exit(1 if fails else 0)
