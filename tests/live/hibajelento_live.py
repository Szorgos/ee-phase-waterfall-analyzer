"""Beépített hibajelentés, 1. szakasz – élő próba az elszigetelt motorral (--port 47899, átmeneti XDG_CONFIG_HOME).

A motor tesztfordítás (-DAQ_TEST_HOOKS=ON): az AQ_TEST_STALL_PULL_AFTER=<s> változóval a híd szála szándékosan
beragad a hang lehúzásában (mint a 2026-11-19-i hibánál). Lépések:
  1. beragasztott híd -> az őrszem jelzi (napló, állapotfájl), a SIGTERM nem állítja le -> SIGKILL
  2. újraindítás -> „előző rendellenes futás” helyi jelentés (tisztítva), rendes leállás SIGTERM-re
  3. újraindítás -> nincs jelentés (kikapcsolt beállításnál a régi törlődik), rendes leállás
  4. SIGSEGV -> a jelzéskezelő a crash.txt-be ír; a következő indítás jelentésében benne van
Használat: hibajelento_live.py <aqulify-engine> <munkamappa>   (tests/live/hibajelento_live.sh indítja)
"""
import json, os, signal, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wsclient import WS  # noqa: E402

PORT = 47899
FAIL = 0


def check(ok, text):
    global FAIL
    print(("  [OK]   " if ok else "  [HIBA] ") + text, flush=True)
    if not ok:
        FAIL += 1


def wait_port(port, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
            return True
        except OSError:
            time.sleep(0.2)
    return False


def wait_port_closed(port, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
            time.sleep(0.2)
        except OSError:
            return True
    return False


def read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def load_json(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def start(engine, env, logname):
    out = open(logname, "w", encoding="utf-8")
    p = subprocess.Popen([engine, "--port", str(PORT)], stdout=out, stderr=subprocess.STDOUT, env=env)
    return p, out


def main():
    engine, work = sys.argv[1], sys.argv[2]
    conf = os.path.join(work, "conf-hj")
    os.makedirs(conf, exist_ok=True)
    diag = os.path.join(conf, "aqulify", "diag")
    env = dict(os.environ)
    env["XDG_CONFIG_HOME"] = conf
    env["AQULIFY_NO_UPDATE_CHECK"] = "1"
    home = os.environ.get("HOME", "")
    user = os.environ.get("USER", "")
    host = socket.gethostname()

    print("[L1] Beragasztott híd: az őrszem jelez, a jelentés a hídtól függetlenül fájlban van")
    env1 = dict(env, AQ_TEST_STALL_PULL_AFTER="3")
    p, out = start(engine, env1, os.path.join(work, "hj1.out"))
    check(wait_port(PORT, 15), "a motor elindul (127.0.0.1:%d)" % PORT)
    ok_status = False
    try:
        ws = WS(PORT)
        ok_status = ws.wait_type("status", 400) is not None
        ws.s.close()
    except Exception as e:  # noqa: BLE001
        print("    ws:", e)
    check(ok_status, "a beragadás előtt a híd állapotot küld")
    t0 = time.time()
    stall = None
    while time.time() - t0 < 20:
        st = load_json(os.path.join(diag, "state.json"))
        if st:
            for s in st.get("stalls", []):
                if s.get("thread") == "bridge":
                    stall = s
        if stall:
            break
        time.sleep(0.25)
    took = time.time() - t0
    check(stall is not None, "az állapotfájlban: a „bridge” szál beragadt (%.1f s a figyelés kezdetétől; beragasztás 3 s-nál, küszöb 5 s)" % took)
    check(stall is not None and stall.get("label") == "bridge: pullAudio", "utolsó címke: %s" % (stall or {}).get("label"))
    st = load_json(os.path.join(diag, "state.json")) or {}
    threads = {h.get("thread") for h in st.get("heartbeats", [])}
    print("    szívverések:", sorted(t for t in threads if t))
    check({"bridge", "player-decoder", "dsp-worker", "pw-output"} <= threads, "regisztrált szálak: híd, dekóder, DSP, kimenet")
    others = [s.get("thread") for s in st.get("stalls", []) if s.get("thread") != "bridge"]
    check(not others, "más szál nem jelez (%s)" % others)
    eng = (st.get("sections") or {}).get("engine") or {}
    check("path" in eng and "audio_opts" in eng, "az állapotban a lejátszási út és az audio_opts: %s" % eng.get("path"))
    check("system" in (st.get("sections") or {}) and "sound_cards" in st, "rendszer és hangkártyák szakasz")
    check(st.get("platform") == "linux" and st.get("version"), "platform, verzió: %s %s" % (st.get("platform"), st.get("version")))
    check(st.get("reports_enabled") is False, "a beállítás alapból ki")
    log = read(os.path.join(diag, "engine.log"))
    check("[Őrszem]" in log and "bridge: pullAudio" in log, "a motor naplójában (diag/engine.log) az őrszem sora")
    check("Aqulify motor indul" in log, "a napló tükrében az indulássor")
    stdout = read(os.path.join(work, "hj1.out"))
    check("[Őrszem]" in stdout, "a szabványos kimeneten (journald) is")
    # a beragadt híd miatt a SIGTERM nem állítja le (a 2026-11-19-i tünet); erőszakos leállítás
    p.send_signal(signal.SIGTERM)
    try:
        p.wait(4)
        check(False, "SIGTERM után kilépett (a beragasztott híd mellett ez nem várt)")
    except subprocess.TimeoutExpired:
        check(True, "a beragasztott híd mellett a SIGTERM nem állítja le (a hiba tünete)")
        p.kill()
        p.wait(5)
    out.close()
    check(os.path.exists(os.path.join(diag, "running")), "a „fut” jel megmaradt (nincs „rendben leállt”)")
    wait_port_closed(PORT, 5)

    print("[L2] Újraindítás: „előző rendellenes futás” helyi jelentés")
    p, out = start(engine, env, os.path.join(work, "hj2.out"))
    check(wait_port(PORT, 15), "a motor újraindul")
    time.sleep(1.0)
    stdout = read(os.path.join(work, "hj2.out"))
    check("Az előző futás nem állt le rendesen" in stdout, "a naplóban: az előző futás nem állt le rendesen")
    rep = read(os.path.join(diag, "previous-run.json"))
    repj = load_json(os.path.join(diag, "previous-run.json"))
    check(repj is not None and repj.get("kind") == "previous_abnormal", "previous-run.json érvényes, kind = previous_abnormal")
    check("bridge: pullAudio" in rep and "szándékosan beragad" in rep, "a jelentésben a beragadt szál és a napló utolsó sorai")
    check(not any(x and len(x) >= 3 and x in rep for x in (home, user, host)), "a jelentésben nincs otthoni mappa, felhasználó- és gépnév")
    check(os.path.exists(os.path.join(diag, "previous-run.txt")), "olvasható előnézet (previous-run.txt)")
    sample = os.environ.get("AQ_HJ_SAMPLE_DIR")
    if sample:   # mintának a jelentéshez (a tisztított szöveg)
        os.makedirs(sample, exist_ok=True)
        for n in ("previous-run.txt", "previous-run.json"):
            with open(os.path.join(sample, n), "w", encoding="utf-8") as f:
                f.write(read(os.path.join(diag, n)))
    p.send_signal(signal.SIGTERM)
    try:
        rc = p.wait(15)
    except subprocess.TimeoutExpired:
        p.kill(); p.wait(5); rc = -999
    out.close()
    check(rc == 0, "rendes leállás SIGTERM-re (kód: %s)" % rc)
    st = load_json(os.path.join(diag, "state.json")) or {}
    check(st.get("clean_exit") is True and not os.path.exists(os.path.join(diag, "running")), "clean_exit = true, a „fut” jel törölve")
    wait_port_closed(PORT, 5)

    print("[L3] Rendes leállás után: nincs jelentés, a régi (kikapcsolt beállítás) törlődik")
    p, out = start(engine, env, os.path.join(work, "hj3.out"))
    check(wait_port(PORT, 15), "a motor elindul")
    time.sleep(1.0)
    check("Az előző futás nem állt le rendesen" not in read(os.path.join(work, "hj3.out")), "nincs „előző rendellenes futás”")
    check(not os.path.exists(os.path.join(diag, "previous-run.json")), "a régi jelentés törölve (beállítás: ki)")

    print("[L4] Összeomlás (SIGSEGV): a jelzéskezelő a crash.txt-be ír")
    time.sleep(2.5)   # az őrszem legalább egy állapotot írjon, a szálak jelezzenek
    p.send_signal(signal.SIGSEGV)
    try:
        rc = p.wait(15)
    except subprocess.TimeoutExpired:
        p.kill(); p.wait(5); rc = -999
    out.close()
    check(rc == -signal.SIGSEGV, "a motor a SIGSEGV-vel áll le (kód: %s)" % rc)
    crash = read(os.path.join(diag, "crash.txt"))
    check("AQ_CRASH signal=11 (SIGSEGV)" in crash and "thread=bridge" in crash, "crash.txt: jelzés és a szálak utolsó címkéje")
    wait_port_closed(PORT, 5)
    p, out = start(engine, env, os.path.join(work, "hj5.out"))
    check(wait_port(PORT, 15), "a motor újraindul")
    time.sleep(1.0)
    rep = read(os.path.join(diag, "previous-run.json"))
    check("AQ_CRASH signal=11" in rep, "a következő indítás jelentésében az összeomlás jele")
    p.send_signal(signal.SIGTERM)
    try:
        rc = p.wait(15)
    except subprocess.TimeoutExpired:
        p.kill(); p.wait(5); rc = -999
    out.close()
    check(rc == 0, "rendes leállás (kód: %s)" % rc)

    print("\nÉLŐ PRÓBA: %s (%d hiba)" % ("SIKERES" if FAIL == 0 else "SIKERTELEN", FAIL))
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
