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

    # --- K14e / K14f: Saját motoros rendszerhang-felmintavételezés (#51 és #52 regresszió)
    wav48 = os.path.join(work, "live_48k.wav")
    ul.make_sine_wav(wav48, sr=48000, freq=1000.0, seconds=30.0, amp=0.3)

    print("[K14e] Saját motor (rendszerhang): follow + ×2, 48 kHz 1 kHz szinusz — nincs 0,5x/2x tempóhiba és őszinte állapot")
    ul.set_opts(rateMode="follow", keepAwake=False, upsampleRule=1, upsampleSystemAudio=True, upsampleEngine=0)
    p48 = play(wav48)
    try:
        time.sleep(4.0)
        lu = live()
        cap_sr = ul.pw_node_rate("aqulify_sink")
        dac_sr = ul.pw_node_rate("dac_a")
        rec_sr = dac_sr if dac_sr > 0 else 48000
        tones = record_tones("dac_a", rec_sr, duration=1.5)
        if tones is not None:
            ok_pitch = (tones["db1000"] > -25.0 and
                        (tones["db1000"] - tones["db500"]) >= 40.0 and
                        (tones["db1000"] - tones["db2000"]) >= 40.0)
            check(ok_pitch,
                  f"K14e: hangmagasság/sebesség pontos (1 kHz={tones['db1000']:.1f} dB, 500 Hz={tones['db500']:.1f} dB, "
                  f"2 kHz={tones['db2000']:.1f} dB, rec_sr={rec_sr}, cap_sr={cap_sr}, dac_sr={dac_sr})")
        else:
            check(False, f"K14e: felvétel sikertelen a dac_a nyelőről (rec_sr={rec_sr})")
        if lu.get("active") is True and lu.get("ownEngine") is True:
            honest = (cap_sr == 48000 and dac_sr == 96000 and
                      lu.get("srcRate") == 48000 and lu.get("targetRate") == 96000)
        else:
            honest = (lu.get("active") is False and lu.get("ownEngine") is False and
                      lu.get("reason") == "rate_mismatch" and lu.get("converter") == "none")
        check(honest, f"K14e: őszinte állapot (cap_sr={cap_sr}, dac_sr={dac_sr}, liveUpsample={lu})")
        # Ping-pong ellenőrzés: 2,5 s múlva sem kapcsol vissza hibásan és a hang 1 kHz marad
        time.sleep(2.5)
        lu2 = live()
        if lu.get("active") is False:
            check(lu2.get("active") is False and lu2.get("ownEngine") is False and lu2.get("reason") == "rate_mismatch",
                  f"K14e: visszalépés után nincs ping-pong (liveUpsample={lu2})")
    finally:
        p48.terminate()
        p48.wait(timeout=5)
    time.sleep(0.8)

    print("[K14f] Saját motor (rendszerhang): 48 kHz-re zárt kimenet (system mód) — tiszta visszalépés, nincs 0,5x/dupla konverzió")
    ul.set_opts(rateMode="system", keepAwake=False, upsampleRule=1, upsampleSystemAudio=True, upsampleEngine=0)
    p48b = play(wav48)
    try:
        time.sleep(4.0)
        lu = live()
        cap_sr = ul.pw_node_rate("aqulify_sink")
        dac_sr = ul.pw_node_rate("dac_a")
        rec_sr = dac_sr if dac_sr > 0 else 48000
        tones = record_tones("dac_a", rec_sr, duration=1.5)
        if tones is not None:
            ok_pitch = (tones["db1000"] > -25.0 and
                        (tones["db1000"] - tones["db500"]) >= 40.0 and
                        (tones["db1000"] - tones["db2000"]) >= 40.0)
            check(ok_pitch,
                  f"K14f: 48 kHz-re zárt kimeneten 1 kHz pontos (1 kHz={tones['db1000']:.1f} dB, 500 Hz={tones['db500']:.1f} dB, "
                  f"2 kHz={tones['db2000']:.1f} dB, dac_sr={dac_sr})")
        else:
            check(False, f"K14f: felvétel sikertelen (rec_sr={rec_sr})")
        check(dac_sr == 48000 and lu.get("active") is False and lu.get("ownEngine") is False and
              lu.get("reason") == "rate_mismatch" and lu.get("converter") == "none",
              f"K14f: 48 kHz-es kimenetnél őszinte rate_mismatch állapot (dac_sr={dac_sr}, cap_sr={cap_sr}, liveUpsample={lu})")
    finally:
        p48b.terminate()
        p48b.wait(timeout=5)

    ul.set_opts(rateMode="system", upsampleRule=0, upsampleSystemAudio=False)
    print(f"\n{('SIKERTELEN' if fails else 'MINDEN TESZT SIKERES')} ({fails} hiba)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
