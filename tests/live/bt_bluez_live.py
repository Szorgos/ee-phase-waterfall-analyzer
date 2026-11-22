#!/usr/bin/env python3
"""A Linux Bluetooth RFCOMM szerver (include/bt_bluez.hpp) próbája VALÓDI dbus-daemon ellen.

Mit igazol: a saját D-Bus-kódolásunkat (fejlécmezők, igazítás, a{sv}, SASL EXTERNAL, fájlleíró-átadás) a referencia
démon és a dbus-python fogadja el és értelmezi, típushelyesen. A BlueZ-t egy kis Python-szolgáltatás játssza
(org.bluez, ProfileManager1, Adapter1.Address), amely a regisztrált profilnak NewConnection-t küld egy valódi
socketpár egyik végével. NEM igazol: a valódi BlueZ-t és Bluetooth-rádiót (azt csak hardveres próba mutatja meg).

Használat: python3 -I tests/live/bt_bluez_live.py <build>/test_bt_bluez
Szükséges: dbus-daemon, python3-dbus, python3-gi.
"""
import json
import os
import queue
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

PROFILE_PATH = "/org/aqulify/bluetooth"
UUID = "43ad541b-d816-443a-a652-146cb5191139"


# ------------------------------------------------------------------------------------------------ hamis BlueZ
def fake_bluez():
    import dbus
    import dbus.mainloop.glib
    import dbus.service
    from gi.repository import GLib

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()   # a DBUS_SYSTEM_BUS_ADDRESS-t követi
    state = {"client": None, "path": None, "keep": []}

    def emit(**kw):
        print(json.dumps(kw), flush=True)

    class Manager(dbus.service.Object):
        @dbus.service.method("org.bluez.ProfileManager1", in_signature="osa{sv}", out_signature="",
                             sender_keyword="sender")
        def RegisterProfile(self, profile, uuid, opts, sender=None):
            typed = {}
            for k, v in opts.items():
                typed[str(k)] = [type(v).__name__, (str(v) if isinstance(v, str) else int(v))]
            state["client"] = sender
            state["path"] = str(profile)
            emit(event="register", uuid=str(uuid), path=str(profile), opts=typed)

        @dbus.service.method("org.bluez.ProfileManager1", in_signature="o", out_signature="")
        def UnregisterProfile(self, profile):
            emit(event="unregister", path=str(profile))

    class Adapter(dbus.service.Object):
        @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="ss", out_signature="v")
        def Get(self, iface, prop):
            if iface == "org.bluez.Adapter1" and prop == "Address":
                return dbus.String("AA:BB:CC:DD:EE:01")
            raise dbus.exceptions.DBusException("org.freedesktop.DBus.Error.UnknownProperty", "nincs")

    Manager(bus, "/org/bluez")
    Adapter(bus, "/org/bluez/hci0")
    state["name"] = dbus.service.BusName("org.bluez", bus, do_not_queue=True)   # megtartva, különben a név felszabadul
    loop = GLib.MainLoop()

    def newconn():
        try:
            a, b = socket.socketpair()
            proxy = bus.get_object(state["client"], state["path"], introspect=False)
            iface = dbus.Interface(proxy, "org.bluez.Profile1")
            iface.NewConnection(dbus.ObjectPath("/org/bluez/hci0/dev_11_22_33_44_55_66"),
                                dbus.types.UnixFd(a.fileno()), dbus.Dictionary({}, signature="sv"))
            a.close()
            b.settimeout(3)
            b.sendall(b"hallo\n")
            emit(event="echo", data=b.recv(100).decode())
            state["keep"].append(b)
        except Exception as e:   # noqa: BLE001 - a próba jelentse
            emit(event="error", msg=repr(e))

    def on_stdin(fd, cond):
        line = os.read(fd, 100).decode().strip()
        if line == "newconn":
            newconn()
        elif line == "quit" or not line:
            loop.quit()
            return False
        return True

    GLib.io_add_watch(sys.stdin.fileno(), GLib.IO_IN | GLib.IO_HUP, on_stdin)
    emit(event="ready")
    loop.run()


# ------------------------------------------------------------------------------------------------ vezénylő
class Proc:
    """Egy gyerekfolyamat; a szabványos kimenet sorai egy sorba gyűlnek (szálban olvasva)."""

    def __init__(self, argv, env, stdin=False):
        self.p = subprocess.Popen(argv, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  stdin=subprocess.PIPE if stdin else None, text=True, bufsize=1)
        self.q = queue.Queue()
        self.seen = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            self.q.put(line.rstrip("\n"))
        self.q.put(None)

    def wait_line(self, pred, timeout):
        """A következő illeszkedő sor; a találatig (és azt is) beleértve a sorok elfogynak, így a későbbi
        keresés csak az újabb eseményeket látja."""
        end = time.time() + timeout
        while True:
            for i, l in enumerate(self.seen):
                if pred(l):
                    del self.seen[:i + 1]
                    return l
            if time.time() >= end:
                return None
            try:
                l = self.q.get(timeout=0.1)
            except queue.Empty:
                continue
            if l is None:
                return None
            self.seen.append(l)

    def send(self, text):
        self.p.stdin.write(text + "\n")
        self.p.stdin.flush()


results = {"ok": 0, "fail": 0}


def check(cond, label):
    results["ok" if cond else "fail"] += 1
    print(("  [ok]   " if cond else "  [HIBA] ") + label, flush=True)


def ev(name):
    def pred(l):
        try:
            return json.loads(l).get("event") == name
        except ValueError:
            return False
    return pred


def main():
    probe = os.path.abspath(sys.argv[1])
    work = tempfile.mkdtemp(prefix="aq-bt-live-")
    conf = os.path.join(work, "bus.conf")
    with open(conf, "w") as f:
        f.write('<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" '
                '"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">'
                '<busconfig><type>session</type><listen>unix:tmpdir=' + work + '</listen><auth>EXTERNAL</auth>'
                '<policy context="default"><allow send_destination="*" eavesdrop="true"/>'
                '<allow eavesdrop="true"/><allow own="*"/></policy></busconfig>')
    daemon = subprocess.Popen(["dbus-daemon", "--config-file=" + conf, "--print-address=1", "--nofork"],
                              stdout=subprocess.PIPE, text=True)
    address = daemon.stdout.readline().strip()
    print("A privát busz címe: " + address.split(",")[0], flush=True)
    env = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS=address)
    procs = [daemon]
    try:
        # 1. Nincs BlueZ a buszon: az open() hibával tér vissza, érthető okkal
        pr = Proc([probe, "--probe"], env)
        procs.append(pr.p)
        l = pr.wait_line(lambda s: s.startswith("OPEN"), 6)
        check(l is not None and l.startswith("OPEN 0") and "nem fut" in l,
              "BlueZ nélkül: OPEN 0, ok: a BlueZ nem fut (a valódi démon ServiceUnknown-ot ad) -> " + str(l))
        pr.p.send_signal(signal.SIGTERM)
        check(pr.wait_line(lambda s: s == "CLOSED", 5) is not None and pr.p.wait(5) == 0, "SIGTERM: tiszta kilépés BlueZ nélkül is")

        # 2. Van BlueZ: regisztráció, cím, típushelyes opciók
        fake = Proc([sys.executable, "-I", os.path.abspath(__file__), "--fake"], env, stdin=True)
        procs.append(fake.p)
        check(fake.wait_line(ev("ready"), 10) is not None, "a hamis BlueZ (dbus-python) megszerezte az org.bluez nevet")
        pr = Proc([probe, "--probe"], env)
        procs.append(pr.p)
        l = pr.wait_line(lambda s: s.startswith("OPEN"), 6)
        check(l is not None and l.startswith("OPEN 1 AA:BB:CC:DD:EE:01"), "BlueZ-val: OPEN 1 és az adapter címe -> " + str(l))
        reg = fake.wait_line(ev("register"), 3)
        r = json.loads(reg) if reg else {}
        o = r.get("opts", {})
        check(r.get("uuid") == UUID and r.get("path") == PROFILE_PATH, "a valódi démonon át a profil útvonala és UUID-je megérkezett")
        check(o.get("Role") == ["String", "server"] and o.get("Channel") == ["UInt16", 22] and o.get("Name") == ["String", "Aqulify"],
              "a dbus-python típushelyesen látja: Role=s, Channel=q, Name=s -> " + str(o))
        check(o.get("RequireAuthentication") == ["Boolean", 1] and o.get("RequireAuthorization") == ["Boolean", 0],
              "RequireAuthentication=igaz, RequireAuthorization=hamis (b típus)")

        # 3. Bejövő kapcsolat: fájlleíró a démonon át (SCM_RIGHTS), adat oda-vissza
        fake.send("newconn")
        check(pr.wait_line(lambda s: s == "CONN", 4) is not None, "NewConnection: a motor megkapta a socketet")
        e = fake.wait_line(ev("echo"), 5)
        check(e is not None and json.loads(e)["data"] == "echo:hallo\n", "az átadott RFCOMM-socketen adat megy oda-vissza -> " + str(e))

        # 4. A BlueZ elmegy és visszajön (a valódi démon NameOwnerChanged-et küld)
        fake.send("quit")
        fake.p.wait(5)
        check(pr.wait_line(lambda s: s == "STATE 0", 5) is not None, "a BlueZ eltűnése: a motor észleli (NameOwnerChanged), isOpen hamis")
        fake2 = Proc([sys.executable, "-I", os.path.abspath(__file__), "--fake"], env, stdin=True)
        procs.append(fake2.p)
        check(fake2.wait_line(ev("ready"), 10) is not None, "új BlueZ-példány")
        check(fake2.wait_line(ev("register"), 8) is not None, "a motor újranyitás nélkül újraregisztrál az új BlueZ-nál")
        check(pr.wait_line(lambda s: s == "STATE 1", 5) is not None, "isOpen ismét igaz")
        fake2.send("newconn")
        e = fake2.wait_line(ev("echo"), 5)
        check(e is not None and json.loads(e)["data"] == "echo:hallo\n", "újraregisztráció után új kapcsolat is működik")

        # 5. Tiszta leállás: UnregisterProfile a BlueZ-nak
        pr.p.send_signal(signal.SIGTERM)
        check(fake2.wait_line(ev("unregister"), 5) is not None, "leállításkor a BlueZ UnregisterProfile-t kap a mi útvonalunkkal")
        check(pr.wait_line(lambda s: s == "CLOSED", 5) is not None and pr.p.wait(5) == 0, "a motor tisztán kilép (0-s kód)")
    finally:
        for p in procs:
            try:
                p.kill()
            except Exception:   # noqa: BLE001
                pass
    print("\n%d sikeres, %d hibás ellenőrzés." % (results["ok"], results["fail"]))
    return 0 if results["fail"] == 0 else 1


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--fake":
        fake_bluez()
    else:
        sys.exit(main())
