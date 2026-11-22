#!/usr/bin/env python3
"""Hálózati vevő élő tesztje (Linux): a valódi motor a hamis shairport-sync-et (tests/live/fake_netrecv) indítja.

Indítás (run_all.sh): PATH="tests/live/fake_netrecv:$PATH" AQ_FAKE_NETRECV_DIR=<mappa> \
    tests/live/multiout_live.sh <build> python3 -I tests/live/netrecv_live.py
Ellenőrzi: alapból ki; bekapcsolás → a vevő elindul az aqulify_sink célra, a beállítófájl a -h szerinti modult
írja; az állapot a felületnek; névváltás → újraindul; összeomlás → újraindul; kikapcsolás → leáll; az audio.json-ban
megmarad.
"""
import json, os, signal, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wsclient import WS  # noqa: E402

PORT = int(os.environ.get("AQ_PORT", "47810"))
WORK = os.environ.get("AQ_WORK", "/tmp")
FAKE = os.environ.get("AQ_FAKE_NETRECV_DIR", "")
CONF_DIR = os.path.join(os.environ.get("XDG_CONFIG_HOME", os.path.join(WORK, "conf")), "aqulify")
fails = 0


def check(ok, msg):
    global fails
    print(("  [OK]   " if ok else "  [HIBA] ") + msg)
    if not ok:
        fails += 1


def opts(**kw):
    ws = WS(PORT)
    ws.send({"type": "hello", "client": "netrecv_live"})
    ws.send({"type": "set_audio_opts" if kw else "get_audio_opts", "opts": kw})
    m = ws.wait_type("audio_opts", limit=2000)
    ws.s.close()
    return m or {}


def read(name):
    try:
        with open(os.path.join(FAKE, name)) as f:
            return f.read().strip()
    except OSError:
        return ""


def alive(pid):
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    try:
        with open(f"/proc/{pid}/stat") as f:
            return f.read().rsplit(")", 1)[1].split()[0] != "Z"
    except OSError:
        return False


def wait(cond, sec=6.0):
    end = time.time() + sec
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.1)
    return cond()


def pid():
    s = read("netrecv.pid")
    return int(s) if s.isdigit() else -1


print("Hálózati vevő (hamis program)")
if not FAKE or not os.path.isdir(FAKE):
    print("  [HIBA] AQ_FAKE_NETRECV_DIR nincs megadva")
    sys.exit(1)

m = opts()
check(m.get("caps", {}).get("netRecv") is True, "a képesség megvan (caps.netRecv)")
check(m.get("opts", {}).get("netRecv") is False, "alapból ki")
check(m.get("netRecvState", {}).get("state") == "off", f"állapot: off ({m.get('netRecvState')})")
check(pid() == -1, "nem fut vevő")

m = opts(netRecv=True, netRecvName="Teszt Nappali")
check(m.get("opts", {}).get("netRecv") is True and m.get("opts", {}).get("netRecvName") == "Teszt Nappali",
      "bekapcsolva, a név visszajön")
check(wait(lambda: pid() > 0), "a motor elindította a vevőt")
p1 = pid()
args = read("netrecv.args")
check("-a Teszt Nappali" in args and "-c " in args, f"parancssor: {args}")
check(read("netrecv.env") == "aqulify_sink|aqulify_sink", f"hangcél: {read('netrecv.env')}")
conf = ""
try:
    with open(os.path.join(CONF_DIR, "netrecv.conf")) as f:
        conf = f.read()
except OSError:
    pass
check('output_backend = "pipewire";' in conf and 'sink_target = "aqulify_sink";' in conf,
      "beállítófájl: PipeWire-modul, az Aqulify eszköz a cél")
check(wait(lambda: opts().get("netRecvState", {}).get("state") == "running"), "állapot: running")
st = opts().get("netRecvState", {})
check(st.get("name") == "Teszt Nappali" and st.get("backend") == "pipewire", f"állapot részletei: {st}")
try:
    with open(os.path.join(CONF_DIR, "audio.json")) as f:
        saved = json.load(f)
    check(saved.get("netRecv") is True and saved.get("netRecvName") == "Teszt Nappali", "az audio.json-ban megmarad")
except (OSError, ValueError) as e:
    check(False, f"audio.json olvasása: {e}")

m = opts(netRecvName="Konyha")
check(wait(lambda: pid() > 0 and pid() != p1), "névváltás: új példány")
check(not alive(p1) and "-a Konyha" in read("netrecv.args"), "a régi leállt, az új az új névvel fut")

p2 = pid()
os.kill(p2, signal.SIGKILL)
check(wait(lambda: pid() > 0 and pid() != p2, 8.0), "összeomlás után újraindult")
p3 = pid()

m = opts(netRecv=False)
check(m.get("opts", {}).get("netRecv") is False, "kikapcsolva")
check(wait(lambda: not alive(p3)), "kikapcsolás után a vevő leállt")
check(wait(lambda: opts().get("netRecvState", {}).get("state") == "off"), "állapot: off")

print("SIKERES" if fails == 0 else f"SIKERTELEN ({fails} hiba)")
sys.exit(1 if fails else 0)
