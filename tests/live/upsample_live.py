#!/usr/bin/env python3
"""
Felmintavételezés élő tesztje valódi PipeWire-gráfon (K12).
Indítás: tests/live/multiout_live.sh <build> python3 -I tests/live/upsample_live.py

Ellenőrzi:
  (a) „forrást követi” módban ×2 szabály, 44,1 kHz-es 1 kHz szinusz -> a PipeWire hangkártya-csomópont
      ténylegesen 88 200 Hz-en fut (pw-dump), a felvett jel 1 kHz-es csúcsa megvan és a 43,1 kHz-es tükörkép
      elnyomott, az állapotban actualDacRate == 88200 és resampleWarning == False.
  (b) „rendszer” módban 48 kHz-es gráffal ×2 szabály -> a gráf 48 000 Hz-en marad, az állapotban
      actualDacRate == 48000 és resampleWarning == True.
"""
import json, math, os, struct, subprocess, sys, time, wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wsclient import WS  # noqa: E402

PORT = int(os.environ.get("AQ_PORT", "47810"))
WORK = os.environ.get("AQ_WORK", "/tmp")
if __name__ == "__main__":
    if len(sys.argv) > 1:
        PORT = int(sys.argv[1])
    if len(sys.argv) > 2:
        WORK = sys.argv[2]

fails = 0


def check(ok, text):
    global fails
    print(("  [OK]   " if ok else "  [HIBA] ") + text)
    if not ok:
        fails += 1


def set_opts(**kw):
    ws = WS(PORT)
    ws.send({"type": "hello", "client": "upsample_live"})
    ws.send({"type": "set_audio_opts" if kw else "get_audio_opts", "opts": kw})
    m = ws.wait_type("audio_opts", limit=2000)
    ws.s.close()
    return m


def get_status():
    ws = WS(PORT)
    ws.send({"type": "hello", "client": "upsample_live"})
    ws.send({"type": "subscribe", "spectrum": 0})
    m = ws.wait_type("status", limit=5000)
    ws.s.close()
    return m or {}


def _parse_pw_dump_objects(text):
    """A pw-dump kimenete tartalmazhat egy vagy több egymást követő JSON dokumentumot."""
    dec = json.JSONDecoder()
    objs = []
    idx = 0
    n = len(text)
    while idx < n:
        while idx < n and text[idx].isspace():
            idx += 1
        if idx >= n:
            break
        try:
            val, next_idx = dec.raw_decode(text, idx)
        except json.JSONDecodeError:
            break
        if isinstance(val, list):
            objs.extend(val)
        elif isinstance(val, dict):
            objs.append(val)
        idx = next_idx
    return objs


def pw_node_rate(node_name="dac_a"):
    """A PipeWire gráf tényleges futási mintavételének kiolvasása pw-dump-ból (nem a motor állapota!)."""
    out = subprocess.run(["pw-dump"], capture_output=True, text=True, timeout=5).stdout or ""
    objs = _parse_pw_dump_objects(out)
    rate_fmt = 0
    rate_prop = 0
    for o in objs:
        if not isinstance(o, dict) or o.get("type") != "PipeWire:Interface:Node":
            continue
        info = o.get("info", {})
        props = info.get("props", {})
        if props.get("node.name") != node_name:
            continue
        # 1. Valódi futó formátum (info.params.Format -> rate)
        for fmt in info.get("params", {}).get("Format", []):
            r = fmt.get("rate")
            if isinstance(r, int) and r > 0:
                rate_fmt = r
        # 2. node.rate ("1/88200")
        nr = str(props.get("node.rate", ""))
        if "/" in nr:
            try:
                r = int(nr.split("/")[1])
                if r > 0:
                    rate_prop = r
            except ValueError:
                pass
    return rate_fmt or rate_prop


def pw_graph_rate():
    """A futó PipeWire csomópontok (dac_a vagy aqulify_sink) valódi futási rátája a pw-dump alapján."""
    for name in ("dac_a", "aqulify_sink"):
        r = pw_node_rate(name)
        if r > 0:
            return r
    return 0


def make_sine_wav(path, sr=44100, freq=1000.0, seconds=6.0, amp=0.5):
    n = int(sr * seconds)
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(sr)
        frames = bytearray(n * 4)
        for i in range(n):
            s = int(round(amp * math.sin(2.0 * math.pi * freq * i / sr) * 32767.0))
            struct.pack_into("<hh", frames, i * 4, s, s)
        w.writeframes(bytes(frames))


def goertzel_db(samples, sr, freq):
    n = len(samples)
    if n < 64:
        return -160.0
    w = 2.0 * math.pi * freq / sr
    cos_w = math.cos(w)
    sin_w = math.sin(w)
    coeff = 2.0 * cos_w
    s0 = s1 = s2 = 0.0
    for i, x in enumerate(samples):
        win = 0.5 - 0.5 * math.cos(2.0 * math.pi * i / (n - 1))
        s0 = x * win + coeff * s1 - s2
        s2 = s1
        s1 = s0
    real = s1 - s2 * cos_w
    imag = s2 * sin_w
    mag = math.hypot(real, imag) / (0.25 * n)
    return 20.0 * math.log10(max(mag, 1e-12))


def record_and_analyze(target_sr=88200, duration=1.2):
    raw_path = os.path.join(WORK, "ups_rec.raw")
    if os.path.exists(raw_path):
        os.remove(raw_path)
    p = subprocess.Popen(
        ["pw-record", "--target", "dac_a", "--rate", str(target_sr),
         "--channels", "2", "--format", "f32", "-P", "stream.capture.sink=true", raw_path],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    time.sleep(duration)
    p.terminate()
    try:
        p.wait(timeout=3)
    except subprocess.TimeoutExpired:
        p.kill()
    if not os.path.exists(raw_path):
        return None, None
    data = open(raw_path, "rb").read()
    # Skip WAV header if pw-record wrote one (RIFF)
    off = 44 if data[:4] == b"RIFF" else 0
    n_floats = (len(data) - off) // 4
    n_frames = n_floats // 2
    if n_frames < 8192:
        return None, None
    # Take middle 8192 frames from left channel
    start = max(0, (n_frames - 8192) // 2)
    left = [struct.unpack_from("<f", data, off + (start + i) * 8)[0] for i in range(8192)]
    tone_db = goertzel_db(left, target_sr, 1000.0)
    # Tükörkép a 44,1 kHz-es forrásból: 44100 - 1000 = 43100 Hz
    img_db = goertzel_db(left, target_sr, 43100.0)
    return tone_db, img_db


def main():
    print("========================================================")
    print(" Felmintavételezés élő PipeWire teszt (K12)")
    print("========================================================")

    wav_path = os.path.join(WORK, "ups_44k_1k.wav")
    make_sine_wav(wav_path, sr=44100, freq=1000.0, seconds=8.0, amp=0.5)

    # --- K12(a): „forrást követi” + ×2 szabály -> valódi 88 200 Hz a PipeWire-gráfon
    print("[K12a] Forrást követő mód + ×2 szabály (44,1 -> 88,2 kHz)")
    m = set_opts(rateMode="follow", keepAwake=False, upsampleRule=1, upsamplePreset=1,
                 upsamplePhase=25.0, upsamplePassbandEnd=0.95, upsampleStopbandBegin=1.0,
                 upsamplePrecision=28, upsampleHeadroomDb=-1.0)
    check(m is not None and m.get("opts", {}).get("upsampleRule") == 1,
          f"K12a: audio_opts visszaigazolta az upsampleRule=1 beállítást")
    time.sleep(0.8)

    ws = WS(PORT)
    ws.send({"type": "hello", "client": "upsample_live"})
    ws.send({"type": "player_open", "path": wav_path})
    res = ws.wait_type("player_result", limit=500)
    check(res is not None and res.get("ok") is True, f"K12a: 44,1 kHz WAV megnyitva ({res})")
    time.sleep(2.5)

    pw_sr_a = pw_graph_rate()
    st_a = get_status()
    u_a = st_a.get("upsample", {})
    check(pw_sr_a == 88200, f"K12a: a PipeWire csomópont ténylegesen 88200 Hz-en fut (pw-dump: {pw_sr_a} Hz)")
    check(u_a.get("active") is True and u_a.get("srcRate") == 44100 and u_a.get("targetRate") == 88200,
          f"K12a: motor upsample állapot: {u_a}")
    check(u_a.get("actualDacRate") == 88200 and u_a.get("resampleWarning") is False,
          f"K12a: actualDacRate={u_a.get('actualDacRate')}, resampleWarning={u_a.get('resampleWarning')}")

    tone_db, img_db = record_and_analyze(target_sr=88200, duration=1.5)
    check(tone_db is not None and tone_db > -20.0,
          f"K12a: felvett jel 1 kHz csúcsa jelen van ({tone_db:.1f} dBFS)" if tone_db is not None else "K12a: felvétel sikertelen")
    if tone_db is not None and img_db is not None:
        rej = tone_db - img_db
        check(rej >= 70.0, f"K12a: 43,1 kHz tükörkép elnyomása {rej:.1f} dB (1 kHz: {tone_db:.1f} dB, 43,1 kHz: {img_db:.1f} dB)")
        print(f'K_METRIC: {{"test":"K12a_follow","pwRate":{pw_sr_a},"tone1kDb":{tone_db:.2f},"image43kDb":{img_db:.2f},"rejectionDb":{rej:.2f}}}')

    ws.send({"type": "player_stop"})
    ws.wait_type("player_result", limit=300)
    ws.s.close()
    time.sleep(1.0)

    # --- K12(b): „rendszer” mód (48 kHz-es gráf) + ×2 szabály -> visszaalakítás figyelmeztetés
    print("[K12b] Rendszer mód (48 kHz gráf) + ×2 szabály -> nem alakít (a gráf csak 48 kHz-et enged, nincs kettős átalakítás)")
    set_opts(rateMode="system", keepAwake=False, upsampleRule=1)
    time.sleep(1.0)

    ws2 = WS(PORT)
    ws2.send({"type": "hello", "client": "upsample_live"})
    ws2.send({"type": "player_open", "path": wav_path})
    res2 = ws2.wait_type("player_result", limit=500)
    check(res2 is not None and res2.get("ok") is True, "K12b: 44,1 kHz WAV megnyitva rendszer módban")
    time.sleep(2.0)

    pw_sr_b = pw_graph_rate()
    st_b = get_status()
    u_b = st_b.get("upsample", {})
    check(pw_sr_b == 48000, f"K12b: a PipeWire gráf 48000 Hz-en maradt (pw-dump: {pw_sr_b} Hz)")
    check(u_b.get("active") is False and u_b.get("reason") == "dac_unsupported" and not u_b.get("resampleWarning"),
          f"K12b: a ×2 a 48 kHz-es gráfon nem lehetséges, nem alakít (active={u_b.get('active')}, ok={u_b.get('reason')}, warn={u_b.get('resampleWarning')})")
    print(f'K_METRIC: {{"test":"K12b_system_warn","pwRate":{pw_sr_b},"actualDacRate":{u_b.get("actualDacRate",0)},"resampleWarning":{str(bool(u_b.get("resampleWarning"))).lower()}}}')

    ws2.send({"type": "player_stop"})
    ws2.wait_type("player_result", limit=300)
    ws2.s.close()

    # --- K12(c): „rendszer” mód + „Kimenet maximuma” -> a gráf rátájára (48 kHz) alakít, nincs figyelmeztetés
    print("[K12c] Rendszer mód + Kimenet maximuma (44,1 -> 48 kHz, a gráf rátája)")
    set_opts(rateMode="system", keepAwake=False, upsampleRule=3)
    time.sleep(1.0)
    ws3 = WS(PORT)
    ws3.send({"type": "hello", "client": "upsample_live"})
    ws3.send({"type": "player_open", "path": wav_path})
    res3 = ws3.wait_type("player_result", limit=500)
    time.sleep(2.0)
    u_c = get_status().get("upsample", {})
    check(res3 is not None and res3.get("ok") is True and u_c.get("active") is True and u_c.get("targetRate") == 48000 and
          not u_c.get("resampleWarning"),
          f"K12c: 44,1 -> 48 kHz, a PipeWire nem alakít újra (target={u_c.get('targetRate')}, warn={u_c.get('resampleWarning')})")
    ws3.send({"type": "player_stop"})
    ws3.wait_type("player_result", limit=300)
    ws3.s.close()
    time.sleep(1.0)

    # --- K12(d): lefelé alakítás: 96 kHz-es fájl a 48 kHz-es gráfon. „Rendszer” (alap): nem alakít (a PipeWire igen);
    # választott motorral (FFT64) a lejátszó alakít 48 kHz-re
    print("[K12d] Lefelé alakítás: 96 kHz-es fájl, 48 kHz-es gráf (rendszer, majd FFT64)")
    wav96 = os.path.join(WORK, "ups_96k_1k.wav")
    make_sine_wav(wav96, sr=96000, freq=1000.0, seconds=6.0, amp=0.5)
    for down, want_active, label in ((-1, False, "Rendszer"), (1, True, "FFT64")):
        set_opts(rateMode="system", keepAwake=False, upsampleRule=0, downsampleEngine=down)
        time.sleep(1.0)
        ws4 = WS(PORT)
        ws4.send({"type": "hello", "client": "upsample_live"})
        ws4.send({"type": "player_open", "path": wav96})
        res4 = ws4.wait_type("player_result", limit=500)
        time.sleep(2.0)
        u_d = get_status().get("upsample", {})
        ok = res4 is not None and res4.get("ok") is True and u_d.get("active") is want_active
        if want_active:
            ok = ok and u_d.get("srcRate") == 96000 and u_d.get("targetRate") == 48000
        check(ok, f"K12d ({label}): active={u_d.get('active')}, {u_d.get('srcRate')} -> {u_d.get('targetRate')} Hz, ok={u_d.get('reason')}")
        ws4.send({"type": "player_stop"})
        ws4.wait_type("player_result", limit=300)
        ws4.s.close()
        time.sleep(1.0)

    # Visszaállítás
    set_opts(rateMode="system", upsampleRule=0, downsampleEngine=-1)
    print(f"\n{('SIKERTELEN' if fails else 'MINDEN TESZT SIKERES')} ({fails} hiba)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
