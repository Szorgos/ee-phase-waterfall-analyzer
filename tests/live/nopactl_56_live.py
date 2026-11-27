#!/usr/bin/env python3
"""
Pactl és pipewire-pulse nélküli élő PipeWire teszt (#56 regresszió).
Indítás: tests/live/nopactl_56_live.sh <build>

Ellenőrzi:
  1. wpctl alapértelmezett kimenet (aqulify_sink), natív sink-lista, currentSink és wpctl hangerő-olvasás/állítás;
  2. 48 kHz-es korlátozott DAC (dac_burrbrown: audio.allowed-rates=[32000 44100 48000]) + 44,1 kHz WAV + #56 opciók:
     -> filter_only 44 100 Hz (NEM 88 200 Hz), actualDacRate=44100, resampleWarning=False, tiszta 1 kHz jel;
  3. nem-USB fix 48 kHz sink (dac_fix48: SPA_PARAM_EnumFormat rate=48000, audio.allowed-rates nélkül) + 48 kHz WAV ×2:
     -> filter_only 48 000 Hz (NEM 96 000 Hz), actualDacRate=48000, resampleWarning=False, tiszta 1 kHz jel;
  4. korlátozatlan DAC (dac_alt: EnumFormat 1..INT_MAX kiszűrve) + 44,1 kHz WAV ×2:
     -> valódi 88 200 Hz felmintavételezés, tiszta 1 kHz jel.
"""
import os, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import upsample_live as ul  # noqa: E402

PORT = int(os.environ.get("AQ_PORT", "47810"))
WORK = os.environ.get("AQ_WORK", "/tmp")
if len(sys.argv) > 1:
    PORT = int(sys.argv[1])
if len(sys.argv) > 2:
    WORK = sys.argv[2]
ul.PORT = PORT
ul.WORK = WORK

fails = 0


def check(ok, text):
    global fails
    print(("  [OK]   " if ok else "  [HIBA] ") + text)
    if not ok:
        fails += 1


def wait_for(pred, seconds):
    end = time.time() + seconds
    while time.time() < end:
        v = pred()
        if v:
            return v
        time.sleep(0.4)
    return pred()


def record_1khz(sink_name, sr=44100, dur=1.4):
    raw_path = os.path.join(WORK, f"rec56_{sink_name}_{sr}.raw")
    if os.path.exists(raw_path):
        os.remove(raw_path)
    p = subprocess.Popen(
        ["pw-record", "--target", sink_name, "--rate", str(sr),
         "--channels", "2", "--format", "f32", "-P", "stream.capture.sink=true", raw_path],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    time.sleep(dur)
    p.terminate()
    try:
        p.wait(timeout=3)
    except subprocess.TimeoutExpired:
        p.kill()
    if not os.path.exists(raw_path):
        return None
    data = open(raw_path, "rb").read()
    off = 44 if data[:4] == b"RIFF" else 0
    n_frames = ((len(data) - off) // 4) // 2
    if n_frames < 8192:
        return None
    start = max(0, (n_frames - 8192) // 2)
    left = [struct.unpack_from("<f", data, off + (start + i) * 8)[0] for i in range(8192)]
    return ul.goertzel_db(left, sr, 1000.0)


def main():
    print("========================================================")
    print(" #56 élő teszt (pactl és pipewire-pulse NÉLKÜL)")
    print("========================================================")

    # 1. Alapértelmezett kimenet (wpctl), natív sink-lista, currentSink és wpctl hangerő
    def_inspect = subprocess.run(["wpctl", "inspect", "@DEFAULT_AUDIO_SINK@"],
                                 capture_output=True, text=True, timeout=5).stdout or ""
    check('node.name = "aqulify_sink"' in def_inspect,
          "#56-1a: wpctl alapértelmezett kimenet aqulify_sink")

    st0 = wait_for(lambda: (lambda s: s if s.get("outputSinks") and s.get("currentSink") else None)(ul.get_status()), 6.0) or ul.get_status()
    sink_names = [s.get("name") for s in st0.get("outputSinks", [])]
    vol0 = st0.get("volume", {})
    check("dac_burrbrown" in sink_names and "dac_fix48" in sink_names and "dac_alt" in sink_names,
          f"#56-1b: outputSinks pactl nélkül is tartalmazza az összes fizikai sinket: {sink_names}")
    check(st0.get("currentSink") == "dac_burrbrown",
          f"#56-1c: currentSink = {st0.get('currentSink')} (várt: dac_burrbrown)")
    check(isinstance(vol0.get("level"), (int, float)) and vol0.get("level") >= 0.0,
          f"#56-1d: hangerő kiolvasva wpctl-lel: {vol0}")

    ws_v = ul.WS(PORT)
    ws_v.send({"type": "hello", "client": "nopactl_56"})
    ws_v.send({"type": "set_volume", "level": 0.73, "mute": False})
    time.sleep(0.8)
    ws_v.s.close()
    vol1 = ul.get_status().get("volume", {})
    check(isinstance(vol1.get("level"), (int, float)) and abs(vol1.get("level") - 0.73) <= 0.03 and vol1.get("muted") is False,
          f"#56-1e: set_volume pactl nélkül (wpctl): {vol1}")

    ws_v2 = ul.WS(PORT)
    ws_v2.send({"type": "hello", "client": "nopactl_56"})
    ws_v2.send({"type": "set_volume", "level": 1.0, "mute": False})
    time.sleep(0.5)
    ws_v2.s.close()

    # 2. #56 forgatókönyv: 44,1 kHz WAV + follow + ×2 + #56 szűrőparaméterek a 48 kHz-es dac_burrbrown-on
    wav44 = os.path.join(WORK, "nopactl56_44k.wav")
    wav48 = os.path.join(WORK, "nopactl56_48k.wav")
    ul.make_sine_wav(wav44, sr=44100, freq=1000.0, seconds=12.0, amp=0.5)
    ul.make_sine_wav(wav48, sr=48000, freq=1000.0, seconds=12.0, amp=0.5)

    ul.set_opts(
        rateMode="follow", keepAwake=False, buffer=2048,
        upsampleRule=1, upsampleEngine=0, upsamplePreset=1,
        upsamplePassbandEnd=0.935408, upsampleStopbandBegin=1.088154,
        upsamplePrecision=32, upsamplePhase=18.556479, upsampleHeadroomDb=-3.0,
    )
    time.sleep(0.8)

    ws = ul.WS(PORT)
    ws.send({"type": "hello", "client": "nopactl_56"})
    ws.send({"type": "player_open", "path": wav44})
    res = ws.wait_type("player_result", limit=500)
    check(res is not None and res.get("ok") is True, f"#56-2a: 44,1 kHz WAV megnyitva: {res}")
    time.sleep(2.0)

    u2 = ul.get_status().get("upsample", {})
    dac_sr = ul.pw_node_rate("dac_burrbrown")
    aq_sr = ul.pw_node_rate("aqulify_sink")
    tone2 = record_1khz("dac_burrbrown", sr=44100, dur=1.4)
    check(u2.get("active") is True and u2.get("reason") == "filter_only" and
          u2.get("srcRate") == 44100 and u2.get("targetRate") == 44100,
          f"#56-2b: 48 kHz-es DAC-on filter_only 44100 Hz (NEM 88200 Hz): {u2}")
    check(u2.get("actualDacRate") == 44100 and u2.get("resampleWarning") is False,
          f"#56-2c: actualDacRate={u2.get('actualDacRate')}, resampleWarning={u2.get('resampleWarning')}")
    check(dac_sr == 44100 and aq_sr == 44100,
          f"#56-2d: pw-dump valódi ráta dac_burrbrown={dac_sr} Hz, aqulify_sink={aq_sr} Hz (várt 44100)")
    check(tone2 is not None and tone2 > -20.0,
          f"#56-2e: dac_burrbrown tiszta 1 kHz jel ({tone2:.1f} dBFS)" if tone2 is not None else "#56-2e: felvétel sikertelen")

    ws.send({"type": "player_stop"})
    ws.wait_type("player_result", limit=300)
    time.sleep(0.6)

    # 3. Nem-USB fix 48 kHz sink (SPA_PARAM_EnumFormat rate=48000, audio.allowed-rates nélkül)
    ws.send({"type": "set_output_sink", "sink": "dac_fix48"})
    st_fix = wait_for(lambda: (lambda s: s if s.get("currentSink") == "dac_fix48" else None)(ul.get_status()), 5.0) or ul.get_status()
    time.sleep(0.8)  # pw_node EnumFormat megérkezése
    check(st_fix.get("currentSink") == "dac_fix48",
          f"#56-3a: pactl nélküli kimenetváltás -> currentSink={st_fix.get('currentSink')}")
    ws.send({"type": "player_open", "path": wav48})
    res3 = ws.wait_type("player_result", limit=500)
    check(res3 is not None and res3.get("ok") is True, f"#56-3b: 48 kHz WAV megnyitva dac_fix48-on: {res3}")
    time.sleep(2.0)

    u3 = ul.get_status().get("upsample", {})
    fix_sr = ul.pw_node_rate("dac_fix48")
    tone3 = record_1khz("dac_fix48", sr=48000, dur=1.4)
    check(u3.get("active") is True and u3.get("reason") == "filter_only" and
          u3.get("srcRate") == 48000 and u3.get("targetRate") == 48000 and
          u3.get("actualDacRate") == 48000 and u3.get("resampleWarning") is False,
          f"#56-3c: nem-USB EnumFormat fix 48 kHz -> filter_only 48000 Hz (NEM 96000 Hz): {u3}")
    check(fix_sr == 48000 and tone3 is not None and tone3 > -20.0,
          f"#56-3d: dac_fix48 valódi ráta={fix_sr} Hz, 1 kHz jel={tone3:.1f} dBFS" if tone3 is not None else "#56-3d: felvétel sikertelen")

    ws.send({"type": "player_stop"})
    ws.wait_type("player_result", limit=300)
    time.sleep(0.6)

    # 4. Korlátozatlan DAC (dac_alt): EnumFormat 1..INT_MAX kiszűrve -> valódi 44,1 -> 88,2 kHz
    ws.send({"type": "set_output_sink", "sink": "dac_alt"})
    st_alt = wait_for(lambda: (lambda s: s if s.get("currentSink") == "dac_alt" else None)(ul.get_status()), 5.0) or ul.get_status()
    time.sleep(0.8)
    check(st_alt.get("currentSink") == "dac_alt",
          f"#56-4a: pactl nélküli kimenetváltás -> currentSink={st_alt.get('currentSink')}")
    ws.send({"type": "player_open", "path": wav44})
    res4 = ws.wait_type("player_result", limit=500)
    check(res4 is not None and res4.get("ok") is True, f"#56-4b: 44,1 kHz WAV megnyitva dac_alt-on: {res4}")
    time.sleep(2.0)

    u4 = ul.get_status().get("upsample", {})
    alt_sr = ul.pw_node_rate("dac_alt")
    tone4 = record_1khz("dac_alt", sr=88200, dur=1.4)
    check(u4.get("active") is True and u4.get("reason") == "" and
          u4.get("srcRate") == 44100 and u4.get("targetRate") == 88200 and
          u4.get("actualDacRate") == 88200 and u4.get("resampleWarning") is False,
          f"#56-4c: korlátozatlan DAC-on valódi 44,1 -> 88,2 kHz felmintavételezés: {u4}")
    check(alt_sr == 88200 and tone4 is not None and tone4 > -20.0,
          f"#56-4d: dac_alt valódi ráta={alt_sr} Hz, 1 kHz jel={tone4:.1f} dBFS" if tone4 is not None else "#56-4d: felvétel sikertelen")

    ws.send({"type": "player_stop"})
    ws.wait_type("player_result", limit=300)
    ws.s.close()

    print(f"\n{('SIKERTELEN' if fails else 'MINDEN TESZT SIKERES')} ({fails} hiba)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
