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
import os, subprocess, sys, time

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

    ul.set_opts(rateMode="system", upsampleRule=0)
    print(f"\n{('SIKERTELEN' if fails else 'MINDEN TESZT SIKERES')} ({fails} hiba)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
