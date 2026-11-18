#!/usr/bin/env python3
"""
Nagy felbontás (A rész) élő tesztje valódi PipeWire-gráfon.
Indítás: tests/live/multiout_live.sh <build> python3 -I tests/live/hires_live.py

Ellenőrzi:
  (H1) „forrást követi” módban egy 768 kHz-es, 24 bites WAV: a PipeWire hangkártya-csomópont ténylegesen
       768 000 Hz-en fut (pw-dump), a motor állapota sr == 768000 és loadAdvisor.dspBypassed == True,
       és a hangkártya monitorjáról felvett jel egy hosszú szakaszon BITRE egyezik a fájllal (24 bit).
  (H2) 352,8 kHz-es WAV: a gráf 352 800 Hz-en fut, a DSP nincs megkerülve (dspBypassed == False).
  (H3) felmintavételezés 44,1 -> 705,6 kHz (egyéni táblázat): a gráf 705 600 Hz-en fut, upsample.active,
       targetRate == 705600, és a terhelés-tanácsadó mezői (cpu, threads, cores) értelmesek.
"""
import json, os, random, struct, subprocess, sys, time, wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wsclient import WS  # noqa: E402
from upsample_live import pw_graph_rate, make_sine_wav  # noqa: E402

PORT = int(os.environ.get("AQ_PORT", "47810"))
WORK = os.environ.get("AQ_WORK", "/tmp")
fails = 0


def check(ok, text):
    global fails
    print(("  [OK]   " if ok else "  [HIBA] ") + text)
    if not ok:
        fails += 1


def set_opts(**kw):
    ws = WS(PORT)
    ws.send({"type": "hello", "client": "hires_live"})
    ws.send({"type": "set_audio_opts", "opts": kw})
    m = ws.wait_type("audio_opts", limit=2000)
    ws.s.close()
    return m


def get_status():
    ws = WS(PORT)
    ws.send({"type": "hello", "client": "hires_live"})
    ws.send({"type": "subscribe", "spectrum": 0})
    m = ws.wait_type("status", limit=5000)
    ws.s.close()
    return m or {}


def make_noise_wav24(path, sr, seconds, seed):
    """Álvéletlen 24 bites sztereó jel (bal és jobb eltér), -12 dBFS körüli csúcs: nincs túlvezérlés."""
    rnd = random.Random(seed)
    n = int(sr * seconds)
    vals = []
    frames = bytearray(n * 6)
    for i in range(n):
        l = rnd.randint(-2_000_000, 2_000_000)
        r = rnd.randint(-2_000_000, 2_000_000)
        vals.append((l, r))
        frames[i * 6:i * 6 + 3] = (l & 0xFFFFFF).to_bytes(3, "little")
        frames[i * 6 + 3:i * 6 + 6] = (r & 0xFFFFFF).to_bytes(3, "little")
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(3)
        w.setframerate(sr)
        w.writeframes(bytes(frames))
    return vals


def record(rate, seconds):
    # WAV-fájlba vesz fel (a régebbi pw-record, pl. a PipeWire 1.0, nem ismeri a --raw kapcsolót)
    raw = os.path.join(WORK, f"hires_rec_{rate}.wav")
    if os.path.exists(raw):
        os.remove(raw)
    p = subprocess.Popen(["pw-record", "--target", "dac_a", "--rate", str(rate), "--channels", "2",
                          "--format", "s32", "-P", "stream.capture.sink=true", raw],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(seconds)
    p.terminate()
    try:
        p.wait(timeout=3)
    except subprocess.TimeoutExpired:
        p.kill()
    if not os.path.exists(raw):
        return []
    data = open(raw, "rb").read()
    diag = os.environ.get("AQ_DIAG")
    if diag:
        os.makedirs(diag, exist_ok=True)
        open(os.path.join(diag, os.path.basename(raw)), "wb").write(data)
    off = 0
    if data[:4] == b"RIFF":   # a "data" blokk keresése (a fejléc hossza verziónként eltér)
        pos = 12
        off = -1
        while pos + 8 <= len(data):
            cid, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
            if cid == b"data":
                off = pos + 8
                break
            pos += 8 + size + (size & 1)
        if off < 0:
            return []
    n = (len(data) - off) // 8
    out = []
    for i in range(n):
        l, r = struct.unpack_from("<ii", data, off + i * 8)
        out.append((l >> 8, r >> 8))   # s32-ből 24 bit (a felső 24 bit)
    return out


def bit_compare(src, rec, need=100_000):
    """A felvételben megkeresi a forrás egy szakaszát, és megszámolja az eltérő mintákat."""
    # a felvétel első nem néma keretétől 4096 kerettel beljebb (a megnyitás utáni átmenet kimarad) vett horgony
    nz = next((j for j in range(len(rec)) if rec[j] != (0, 0)), -1)
    if nz < 0 or nz + 4096 + need > len(rec):
        return 0, -1, -1, nz
    j = nz + 4096
    anchor = rec[j:j + 3]
    for i in range(len(src) - 3):   # a teljes forrásban keres (a lejátszás bárhol tarthat)
        if src[i] == anchor[0] and src[i + 1] == anchor[1] and src[i + 2] == anchor[2]:
            n = min(len(src) - i, len(rec) - j, need)
            diff = sum(1 for k in range(n) if src[i + k] != rec[j + k])
            return n, diff, i, j
    return 0, -1, -1, j


def open_file(path):
    ws = WS(PORT)
    ws.send({"type": "hello", "client": "hires_live"})
    ws.send({"type": "player_open", "path": path})
    res = ws.wait_type("player_result", limit=500)
    return ws, res


def stop(ws):
    ws.send({"type": "player_stop"})
    ws.wait_type("player_result", limit=300)
    ws.s.close()
    time.sleep(1.0)


def main():
    print("========================================================")
    print(" Nagy felbontás (A rész) élő PipeWire teszt")
    print("========================================================")
    seed = int(os.environ.get("AQ_CHECK_SEED", "4242"))

    # ---- H1: 768 kHz, bitpontos
    print("[H1] 768 kHz-es 24 bites WAV, forrást követő mód, felmintavételezés ki")
    p768 = os.path.join(WORK, "hires_768k.wav")
    src = make_noise_wav24(p768, 768000, 4.0, seed)
    m = set_opts(rateMode="follow", keepAwake=False, upsampleRule=0, replayGain="off")
    check(m is not None and m.get("opts", {}).get("rateMode") == "follow", "H1: forrást követő mód beállítva")
    time.sleep(0.8)
    ws, res = open_file(p768)
    check(res is not None and res.get("ok") is True, f"H1: 768 kHz WAV megnyitva ({res})")
    time.sleep(1.2)
    pw = pw_graph_rate()
    st = get_status()
    la = st.get("loadAdvisor", {})
    check(pw == 768000, f"H1: a PipeWire csomópont ténylegesen 768000 Hz-en fut (pw-dump: {pw} Hz)")
    check(st.get("sr") == 768000, f"H1: motor sr = {st.get('sr')}")
    check(la.get("dspBypassed") is True, f"H1: loadAdvisor.dspBypassed = {la.get('dspBypassed')}")
    rec = record(768000, 1.5)
    n, diff, i, j = bit_compare(src, rec)
    check(n >= 100_000 and diff == 0,
          f"H1: bitpontos: {n} keret összevetve, {diff} eltérés (forrás@{i}, felvétel@{j}, felvett keret: {len(rec)})")
    print(f'K_METRIC: {{"test":"H1_768k","pwRate":{pw},"compared":{n},"diff":{diff}}}')
    stop(ws)

    # ---- H2: 352,8 kHz, a DSP működik
    print("[H2] 352,8 kHz-es WAV: a DSP-lánc nincs megkerülve")
    p352 = os.path.join(WORK, "hires_352k.wav")
    make_sine_wav(p352, sr=352800, freq=1000.0, seconds=4.0, amp=0.5)
    ws, res = open_file(p352)
    check(res is not None and res.get("ok") is True, "H2: 352,8 kHz WAV megnyitva")
    time.sleep(1.5)
    pw = pw_graph_rate()
    st = get_status()
    la = st.get("loadAdvisor", {})
    check(pw == 352800, f"H2: a PipeWire csomópont 352800 Hz-en fut (pw-dump: {pw} Hz)")
    check(la.get("dspBypassed") is False, f"H2: loadAdvisor.dspBypassed = {la.get('dspBypassed')}")
    stop(ws)

    # ---- H3: felmintavételezés 44,1 -> 705,6 kHz
    print("[H3] Felmintavételezés 44,1 -> 705,6 kHz (egyéni táblázat)")
    p44 = os.path.join(WORK, "hires_44k.wav")
    make_sine_wav(p44, sr=44100, freq=1000.0, seconds=6.0, amp=0.5)
    m = set_opts(rateMode="follow", upsampleRule=4, upsampleCustom={"44100": 705600}, upsampleThreads=0,
                 upsampleMaxRate=0, upsampleHeadroomDb=-1.0)
    o = (m or {}).get("opts", {})
    check(o.get("upsampleRule") == 4 and o.get("upsampleCustom", {}).get("44100") == 705600,
          f"H3: egyéni táblázat visszaigazolva ({o.get('upsampleCustom')})")
    time.sleep(0.8)
    ws, res = open_file(p44)
    check(res is not None and res.get("ok") is True, "H3: 44,1 kHz WAV megnyitva")
    time.sleep(3.0)
    pw = pw_graph_rate()
    st = get_status()
    u = st.get("upsample", {})
    la = st.get("loadAdvisor", {})
    check(pw == 705600, f"H3: a PipeWire csomópont 705600 Hz-en fut (pw-dump: {pw} Hz)")
    check(u.get("active") is True and u.get("targetRate") == 705600, f"H3: upsample állapot: {u}")
    cores = la.get("cores", 0)
    want_threads = 2 if cores >= 4 else 1
    check(la.get("threads") == want_threads, f"H3: automatikus szálszám {la.get('threads')} (magok: {cores}, várt: {want_threads})")
    check(isinstance(la.get("cpu"), (int, float)) and 0.0 <= la.get("cpu") <= 100.0, f"H3: CPU-mérés: {la.get('cpu')} %")
    check(la.get("dspBypassed") is True, "H3: 705,6 kHz-en a DSP megkerülve")
    print(f'K_METRIC: {{"test":"H3_upsample705","pwRate":{pw},"cpu":{la.get("cpu")},"threads":{la.get("threads")},"cores":{cores}}}')
    stop(ws)

    set_opts(rateMode="system", upsampleRule=0, upsampleCustom={})
    print(f"\n{('SIKERTELEN' if fails else 'MINDEN TESZT SIKERES')} ({fails} hiba)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
