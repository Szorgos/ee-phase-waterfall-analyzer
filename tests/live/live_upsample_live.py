#!/usr/bin/env python3
"""
Felmintavételezés az élő úton (K14): más program (itt pw-play, a valóságban pl. MPD) szól az Aqulify eszközre.
Indítás: tests/live/multiout_live.sh <build> python3 -I tests/live/live_upsample_live.py

Ellenőrzi (valódi PipeWire-gráfon, a null DAC-nak nincs /proc/asound adata, így a beépített lejátszó listája dönt):
  (a) „forrást követi” + ×2: 44,1 kHz-es külső hangfolyam -> a gráf ténylegesen 88 200 Hz-en fut (pw-dump), az
      állapot liveUpsample.active, srcRate 44100, targetRate 88200, converter "pipewire";
  (b) a szabály kikapcsolásakor a kényszer megszűnik (clock.force-rate 0, active false);
  (c) fix mintavételnél nincs kényszer (reason "rate_mode");
  (d) a külső hangfolyam vége után (3 s türelem) a kényszer megszűnik (reason "no_source").
"""
import os, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import upsample_live as ul  # noqa: E402  (set_opts, get_status, pw_graph_rate, make_sine_wav)

fails = 0


def check(ok, text):
    global fails
    print(("  [OK]   " if ok else "  [HIBA] ") + text)
    if not ok:
        fails += 1


def force_rate():
    out = subprocess.run(["pw-metadata", "-n", "settings", "0", "clock.force-rate"],
                         capture_output=True, text=True, timeout=5).stdout or ""
    tag = "value:'"
    p = out.find(tag)
    if p < 0:
        return 0
    e = out.find("'", p + len(tag))
    try:
        return int(out[p + len(tag):e])
    except ValueError:
        return 0


def live():
    return ul.get_status().get("liveUpsample", {})


def set_output_sink(sink_name):
    ws = ul.WS(ul.PORT)
    ws.send({"type": "hello", "client": "live_upsample"})
    ws.send({"type": "set_output_sink", "sink": sink_name})
    time.sleep(0.3)
    ws.s.close()


def wait_for(pred, seconds):
    end = time.time() + seconds
    while time.time() < end:
        v = pred()
        if v:
            return v
        time.sleep(0.5)
    return pred()


def play(path):
    return subprocess.Popen(["pw-play", "--target", "aqulify_sink", path],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def record_tones(target_sink, rec_sr, duration=1.5):
    work = os.environ.get("AQ_WORK", "/tmp")
    raw_path = os.path.join(work, f"live_rec_{target_sink}_{rec_sr}.raw")
    if os.path.exists(raw_path):
        os.remove(raw_path)
    p = subprocess.Popen(
        ["pw-record", "--target", target_sink, "--rate", str(rec_sr),
         "--channels", "2", "--format", "f32", "-P", "stream.capture.sink=true", raw_path],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    time.sleep(duration)
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
    win = 16384 if rec_sr >= 88200 else 8192
    if n_frames < win:
        return None
    start = max(0, (n_frames - win) // 2)
    left = [struct.unpack_from("<f", data, off + (start + i) * 8)[0] for i in range(win)]
    return {
        "db1000": ul.goertzel_db(left, rec_sr, 1000.0),
        "db500": ul.goertzel_db(left, rec_sr, 500.0),
        "db2000": ul.goertzel_db(left, rec_sr, 2000.0),
        "dbImg": ul.goertzel_db(left, rec_sr, 47000.0) if rec_sr >= 96000 else None,
    }


def main():
    work = os.environ.get("AQ_WORK", "/tmp")
    print("========================================================")
    print(" Felmintavételezés az élő úton (K14)")
    print("========================================================")
    wav = os.path.join(work, "live_44k.wav")
    ul.make_sine_wav(wav, sr=44100, freq=1000.0, seconds=30.0, amp=0.3)

    print("[K14a] Forrást követő mód + ×2, külső 44,1 kHz-es hangfolyam az Aqulify eszközre")
    ul.set_opts(rateMode="follow", keepAwake=False, upsampleRule=1)
    p = play(wav)
    try:
        lu = wait_for(lambda: (lambda x: x if x.get("active") else None)(live()), 8.0) or live()
        check(lu.get("active") is True and lu.get("srcRate") == 44100 and lu.get("targetRate") == 88200
              and lu.get("converter") == "pipewire", f"K14a: liveUpsample állapot: {lu}")
        check(force_rate() == 88200, f"K14a: clock.force-rate = {force_rate()}")
        rate = wait_for(lambda: ul.pw_graph_rate() == 88200 and 88200, 5.0) or ul.pw_graph_rate()
        check(rate == 88200, f"K14a: a DAC-csomópont ténylegesen 88200 Hz-en fut (pw-dump: {rate} Hz)")

        print("[K14b] A szabály kikapcsolása lejátszás közben")
        ul.set_opts(upsampleRule=0)
        lu = wait_for(lambda: (lambda x: x if not x.get("active") else None)(live()), 4.0) or live()
        check(lu.get("active") is False and lu.get("reason") == "off" and force_rate() == 0,
              f"K14b: a kényszer megszűnt (force-rate {force_rate()}, {lu})")

        print("[K14c] Fix mintavétel: nincs kényszer")
        ul.set_opts(rateMode="fixed", fixedRate=48000, upsampleRule=1)
        time.sleep(2.5)
        lu = live()
        check(lu.get("active") is False and lu.get("reason") == "rate_mode" and force_rate() == 48000,
              f"K14c: fix 48 kHz marad (force-rate {force_rate()}, {lu})")

        print("[K14d] Vissza forrás-követésre, majd a külső hangfolyam vége")
        ul.set_opts(rateMode="follow", upsampleRule=1)
        lu = wait_for(lambda: (lambda x: x if x.get("active") else None)(live()), 6.0) or live()
        check(lu.get("active") is True and force_rate() == 88200, f"K14d: újra aktív ({lu})")
    finally:
        p.terminate()
        p.wait(timeout=5)
    lu = wait_for(lambda: (lambda x: x if not x.get("active") else None)(live()), 8.0) or live()
    check(lu.get("active") is False and lu.get("reason") == "no_source" and force_rate() == 0,
          f"K14d: a hangfolyam vége után a kényszer megszűnt (force-rate {force_rate()}, {lu})")

    # --- K14e / K14g / K14f: Saját motoros rendszerhang-felmintavételezés (#51 és #52 regresszió, 2. fázis)
    wav48 = os.path.join(work, "live_48k.wav")
    ul.make_sine_wav(wav48, sr=48000, freq=1000.0, seconds=60.0, amp=0.3)

    print("[K14e] Saját motor (rendszerhang): follow + ×2, 48 kHz -> 96 kHz — valódi szétválasztás, 1 kHz pontos, nincs 500 Hz")
    ul.set_opts(rateMode="follow", keepAwake=False, upsampleRule=1, upsampleSystemAudio=True, upsampleEngine=0)
    p48 = play(wav48)
    try:
        lu = wait_for(lambda: (lambda x: x if x.get("active") and x.get("ownEngine") else None)(live()), 7.0) or live()
        time.sleep(1.0)
        lu = live()
        cap_sr = ul.pw_node_rate("aqulify_sink")
        dac_sr = ul.pw_node_rate("dac_a")
        rec_sr = dac_sr if dac_sr > 0 else 48000
        tones = record_tones("dac_a", rec_sr, duration=1.5)
        check(lu.get("active") is True and lu.get("ownEngine") is True and
              lu.get("converter") == "libsoxr" and
              lu.get("srcRate") == 48000 and lu.get("targetRate") == 96000 and
              cap_sr == 48000 and dac_sr == 96000,
              f"K14e (libsoxr): valódi szétválasztott mód (cap_sr={cap_sr}, dac_sr={dac_sr}, liveUpsample={lu})")
        if tones is not None:
            ok_pitch = (tones["db1000"] > -25.0 and
                        (tones["db1000"] - tones["db500"]) >= 40.0 and
                        (tones["db1000"] - tones["db2000"]) >= 40.0)
            check(ok_pitch,
                  f"K14e (libsoxr): 1 kHz pontos, nincs 500 Hz/2 kHz (1 kHz={tones['db1000']:.1f} dB, "
                  f"500 Hz={tones['db500']:.1f} dB, 2 kHz={tones['db2000']:.1f} dB)")
            if tones["dbImg"] is not None:
                rej = tones["db1000"] - tones["dbImg"]
                check(rej >= 70.0,
                      f"K14e (libsoxr): 47 kHz tükörkép elnyomása {rej:.1f} dB (1 kHz={tones['db1000']:.1f}, 47 kHz={tones['dbImg']:.1f})")
        else:
            check(False, f"K14e (libsoxr): felvétel sikertelen a dac_a nyelőről (rec_sr={rec_sr})")

        # Motorváltás futás közben Aqulify FFT64-re
        ul.set_opts(upsampleEngine=1)
        lu_fft = wait_for(lambda: (lambda x: x if x.get("active") and x.get("converter") == "fft64" else None)(live()), 6.0) or live()
        time.sleep(1.0)
        cap_sr = ul.pw_node_rate("aqulify_sink")
        dac_sr = ul.pw_node_rate("dac_a")
        tones_fft = record_tones("dac_a", dac_sr if dac_sr > 0 else 48000, duration=1.5)
        check(lu_fft.get("active") is True and lu_fft.get("ownEngine") is True and
              lu_fft.get("converter") == "fft64" and cap_sr == 48000 and dac_sr == 96000,
              f"K14e (FFT64): futás közbeni motorváltás aktív (cap_sr={cap_sr}, dac_sr={dac_sr}, liveUpsample={lu_fft})")
        if tones_fft is not None:
            ok_fft = (tones_fft["db1000"] > -25.0 and
                      (tones_fft["db1000"] - tones_fft["db500"]) >= 40.0 and
                      (tones_fft["db1000"] - tones_fft["db2000"]) >= 40.0)
            check(ok_fft,
                  f"K14e (FFT64): 1 kHz pontos (1 kHz={tones_fft['db1000']:.1f} dB, 500 Hz={tones_fft['db500']:.1f} dB)")
        else:
            check(False, "K14e (FFT64): felvétel sikertelen")

        # --- K14g: Élő kimenetváltás (dac_a -> dac_b -> dac_a) felmintavételezés közben (node.dont-reconnect=true mellett)
        print("[K14g] Kimenetváltás aktív rendszerhang-felmintavételezés közben (dac_a -> dac_b -> dac_a)")
        ul.set_opts(upsampleEngine=0)
        time.sleep(1.0)
        set_output_sink("dac_b")
        st_b = wait_for(lambda: (lambda s: s if s.get("currentSink") == "dac_b" and
                                 s.get("liveUpsample", {}).get("active") and
                                 s.get("liveUpsample", {}).get("ownEngine") else None)(ul.get_status()), 8.0) or ul.get_status()
        time.sleep(1.0)
        lu_b = st_b.get("liveUpsample", {})
        cap_b = ul.pw_node_rate("aqulify_sink")
        dac_b = ul.pw_node_rate("dac_b")
        tones_b = record_tones("dac_b", dac_b if dac_b > 0 else 48000, duration=1.5)
        check(st_b.get("currentSink") == "dac_b" and lu_b.get("active") is True and
              lu_b.get("ownEngine") is True and cap_b == 48000 and dac_b == 96000,
              f"K14g (dac_b): átváltás sikerült (currentSink={st_b.get('currentSink')}, cap_sr={cap_b}, dac_b={dac_b}, lu={lu_b})")
        if tones_b is not None:
            ok_b = (tones_b["db1000"] > -25.0 and
                    (tones_b["db1000"] - tones_b["db500"]) >= 40.0 and
                    (tones_b["db1000"] - tones_b["db2000"]) >= 40.0)
            check(ok_b,
                  f"K14g (dac_b): átváltás után 1 kHz pontos, nincs 500 Hz (1 kHz={tones_b['db1000']:.1f} dB, "
                  f"500 Hz={tones_b['db500']:.1f} dB, 2 kHz={tones_b['db2000']:.1f} dB)")
        else:
            check(False, f"K14g (dac_b): felvétel sikertelen a dac_b nyelőről (dac_b={dac_b})")

        # Visszaváltás dac_a-ra
        set_output_sink("dac_a")
        st_a2 = wait_for(lambda: (lambda s: s if s.get("currentSink") == "dac_a" and
                                  s.get("liveUpsample", {}).get("active") and
                                  s.get("liveUpsample", {}).get("ownEngine") else None)(ul.get_status()), 8.0) or ul.get_status()
        time.sleep(1.0)
        cap_a2 = ul.pw_node_rate("aqulify_sink")
        dac_a2 = ul.pw_node_rate("dac_a")
        tones_a2 = record_tones("dac_a", dac_a2 if dac_a2 > 0 else 48000, duration=1.5)
        check(st_a2.get("currentSink") == "dac_a" and cap_a2 == 48000 and dac_a2 == 96000 and
              tones_a2 is not None and tones_a2["db1000"] > -25.0 and (tones_a2["db1000"] - tones_a2["db500"]) >= 40.0,
              f"K14g (vissza dac_a): visszaváltás után 1 kHz pontos (cap_sr={cap_a2}, dac_a={dac_a2}, "
              f"1 kHz={tones_a2['db1000'] if tones_a2 else None}, 500 Hz={tones_a2['db500'] if tones_a2 else None})")

        # --- K14f: 48 kHz-re zárt kimenet (sof-hda-dsp szimuláció: audio.allowed-rates=[48000], node.lock-rate=true)
        print("[K14f] Csak 48 kHz-et tudó kimenet (dac_48k_only) — őszinte dac_unsupported visszalépés, 1 kHz marad")
        pw_create = subprocess.run(
            ["pw-cli", "create-node", "adapter",
             'factory.name=support.null-audio-sink node.name=dac_48k_only node.description="48k Only Speaker" '
             'media.class=Audio/Sink audio.position="[ FL FR ]" object.linger=true node.driver=true '
             'priority.driver=1500 priority.session=1500 audio.rate=48000 audio.allowed-rates="[ 48000 ]" '
             'node.lock-rate=true'],
            capture_output=True, text=True, timeout=5,
        )
        time.sleep(1.0)
        set_output_sink("dac_48k_only")
        time.sleep(3.5)
        st_48 = ul.get_status()
        lu_48 = st_48.get("liveUpsample", {})
        dac_48 = ul.pw_node_rate("dac_48k_only")
        tones_48 = record_tones("dac_48k_only", 48000, duration=1.5)
        check(pw_create.returncode == 0 and st_48.get("currentSink") == "dac_48k_only" and dac_48 == 48000 and
              lu_48.get("active") is False and lu_48.get("ownEngine") is False and
              lu_48.get("reason") == "dac_unsupported" and lu_48.get("converter") == "none",
              f"K14f: csak 48 kHz-es kimenetnél őszinte dac_unsupported (sink={st_48.get('currentSink')}, dac_48={dac_48}, lu={lu_48})")
        if tones_48 is not None:
            check(tones_48["db1000"] > -25.0 and (tones_48["db1000"] - tones_48["db500"]) >= 40.0,
                  f"K14f: 48 kHz-es kimeneten 1 kHz pontos (1 kHz={tones_48['db1000']:.1f} dB, 500 Hz={tones_48['db500']:.1f} dB)")
        else:
            check(False, "K14f: felvétel sikertelen a dac_48k_only nyelőről")

        # Vissza dac_a-ra: a dac_unsupported tiltás feloldódik, a saját motor újra 96 kHz-en aktív!
        set_output_sink("dac_a")
        st_back = wait_for(lambda: (lambda s: s if s.get("currentSink") == "dac_a" and
                                    s.get("liveUpsample", {}).get("active") and
                                    s.get("liveUpsample", {}).get("ownEngine") else None)(ul.get_status()), 8.0) or ul.get_status()
        lu_back = st_back.get("liveUpsample", {})
        check(st_back.get("currentSink") == "dac_a" and lu_back.get("active") is True and
              lu_back.get("ownEngine") is True and ul.pw_node_rate("dac_a") == 96000,
              f"K14f: dac_a-ra visszaváltva a saját motor újra aktív 96 kHz-en (lu={lu_back})")
    finally:
        p48.terminate()
        p48.wait(timeout=5)

    # --- K14h (#46, #53): follow + keepAwake=True + 48->96 kHz: nincs 48<->96 oszcilláció, nincs keepAwake naplózaj,
    #     és a külső forrás leállása után a keepAlive visszakapcsol
    print("[K14h] Forrást követő + ébren tartás + 48->96 kHz: nincs 48<->96 oszcilláció, nincs naplózaj, utána keepAlive visszatér")
    engine_log_path = os.path.join(os.environ.get("AQ_WORK", "/tmp"), "engine.log")
    log_pos_h = os.path.getsize(engine_log_path) if os.path.exists(engine_log_path) else 0
    ul.set_opts(rateMode="follow", keepAwake=True, upsampleRule=1, upsampleSystemAudio=True, upsampleEngine=0)
    ka_init = wait_for(lambda: True if ul.get_status().get("path", {}).get("keepAwake") is True else None, 6.0)
    check(ka_init is True, f"K14h: külső forrás előtt a keepAlive aktív (keepAwake={ka_init})")
    p48_h = subprocess.Popen(["pw-play", "--target", "aqulify_sink", wav48],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        st_h = wait_for(lambda: (lambda s: s if s.get("liveUpsample", {}).get("active") and
                                 s.get("liveUpsample", {}).get("ownEngine") else None)(ul.get_status()), 8.0) or ul.get_status()
        # 4,5 másodpercen át figyeljük: a keepAlive nem tér vissza közben és a DAC végig 96 kHz marad (nincs 48<->96)
        rates_seen = []
        ka_seen_during = []
        for _ in range(9):
            time.sleep(0.5)
            rates_seen.append(ul.pw_node_rate("dac_a"))
            ka_seen_during.append(ul.get_status().get("path", {}).get("keepAwake"))
        check(all(r == 96000 for r in rates_seen),
              f"K14h: 4,5 s alatt a DAC végig 96000 Hz, nincs 48<->96 oszcilláció (mért={rates_seen})")
        check(all(k is False for k in ka_seen_during),
              f"K14h: aktív felmintavételezés közben a keepAlive végig szünetel (mért={ka_seen_during})")
    finally:
        p48_h.terminate()
        p48_h.wait(timeout=5)
    # A külső forrás leállása után a saját motor (3 s no_source) leáll, majd a keepAlive visszakapcsol
    ka_resumed = wait_for(lambda: True if ul.get_status().get("path", {}).get("keepAwake") is True else None, 10.0)
    check(ka_resumed is True, f"K14h: a külső forrás leállása után a keepAlive visszakapcsolt (keepAwake={ka_resumed})")
    ul.set_opts(rateMode="system", keepAwake=False, upsampleRule=0, upsampleSystemAudio=False)
    time.sleep(1.0)
    if os.path.exists(engine_log_path):
        with open(engine_log_path, "r", encoding="utf-8", errors="replace") as lf:
            lf.seek(log_pos_h)
            delta_h = lf.read()
        ka_on_h = delta_h.count("[PipeWire] Hangkártya ébren tartása: be")
        ka_off_h = delta_h.count("[PipeWire] Hangkártya ébren tartása: ki")
        osc_back = delta_h.count("Mintavétel-követés: 96000 → 48000")
        osc_up = delta_h.count("Mintavétel-követés: 48000 → 96000")
        check(ka_on_h == 1 and ka_off_h == 1,
              f"K14h: ébren tartás csak felhasználói kapcsoláskor naplózott (be={ka_on_h}, ki={ka_off_h}, várt 1/1)")
        check(osc_back <= 1 and osc_up <= 1,
              f"K14h: nincs 48000 <-> 96000 napló-oszcilláció (48->96={osc_up}, 96->48={osc_back})")

    # ---- K14i: 44,1 -> 96 kHz Referencia előbeállítás (32 bit / 99,7%, libsoxr): 0 alulcsordulás, nyugodt drift ----
    print("\n[K14i] 44,1 -> 96 kHz Referencia (32 bit / 99,7%, libsoxr) élő teszt")
    set_output_sink("dac_a")
    ul.set_opts(
        rateMode="follow",
        keepAwake=False,
        upsampleRule=3,
        upsampleMaxRate=96000,
        upsampleEngine=0,
        upsampleSystemAudio=True,
        upsamplePreset=4,
        upsamplePassbandEnd=0.997,
        upsampleStopbandBegin=1.0,
        upsamplePrecision=32,
        upsamplePhase=50.0,
    )
    pa_ref = play(wav)
    try:
        lu_ref = wait_for(
            lambda: (lambda x: x if (x.get("active") and x.get("ownEngine") and x.get("targetRate") == 96000) else None)(live()),
            10.0,
        ) or live()
        time.sleep(3.0)
        rec_ref = record_tones("dac_a", 96000, duration=2.0)
        lu_ref2 = live()
        check(
            lu_ref2.get("active") is True and lu_ref2.get("ownEngine") is True
            and lu_ref2.get("srcRate") == 44100 and lu_ref2.get("targetRate") == 96000,
            f"K14i: 44,1 -> 96 kHz Referencia aktív maradt (lu={lu_ref2})",
        )
        check(
            lu_ref2.get("underruns", 999) == 0,
            f"K14i: Referencia 44,1 -> 96 kHz 0 alulcsordulással fut (underruns={lu_ref2.get('underruns')})",
        )
        check(
            abs(lu_ref2.get("driftPpm", 999.0)) < 200.0,
            f"K14i: 44,1 -> 96 kHz drift nem ütközik ±500 ppm-re (driftPpm={lu_ref2.get('driftPpm')})",
        )
        if rec_ref:
            check(rec_ref["db1000"] > -20.0 and rec_ref["db500"] < rec_ref["db1000"] - 40.0,
                  f"K14i: tiszta 1 kHz kimenet Referencia előbeállítással (1k={rec_ref['db1000']:.1f} dB, 500Hz={rec_ref['db500']:.1f} dB)")
        else:
            check(False, "K14i: nem sikerült felvételt készíteni a dac_a kimenetről")
    finally:
        pa_ref.terminate()
        try:
            pa_ref.wait(timeout=3)
        except Exception:
            pa_ref.kill()
        ul.set_opts(rateMode="system", keepAwake=False, upsampleRule=0, upsampleMaxRate=0, upsampleSystemAudio=False,
                    upsamplePreset=1, upsamplePassbandEnd=0.95, upsamplePrecision=28, upsamplePhase=25.0)
        time.sleep(0.5)

    print(f"\n{('SIKERTELEN' if fails else 'MINDEN TESZT SIKERES')} ({fails} hiba)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
