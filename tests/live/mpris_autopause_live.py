#!/usr/bin/env python3
"""
Élő MPRIS auto-pause regressziós teszt elszigetelt D-Bus + PipeWire környezetben.
Indítás: AQ_ENGINE_PORT=47899 tests/live/multiout_live.sh <build> python3 -I tests/live/mpris_autopause_live.py

Ellenőrzi:
  [M1] Hibás/nem létező fájl megnyitása NEM állítja meg a szóló külső lejátszót.
  [M2] Helyi fájl indításakor minden Playing külső MPRIS lejátszó szünetel (a Pause-t figyelmen kívül hagyó
       kliens is a megerősített PlayPause tartalékkal), miközben a már Paused külső lejátszó nem kap parancsot.
  [M3] A beépített lejátszó szünetből történő újraindítása (play) szintén szünetelteti a közben elindított
       külső lejátszót.
"""
import os, sys, threading, time
from gi.repository import Gio, GLib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import upsample_live as ul  # noqa: E402

MPRIS_XML = """
<node>
  <interface name="org.mpris.MediaPlayer2">
    <property name="Identity" type="s" access="read"/>
  </interface>
  <interface name="org.mpris.MediaPlayer2.Player">
    <method name="Pause"/>
    <method name="PlayPause"/>
    <method name="Play"/>
    <property name="PlaybackStatus" type="s" access="read"/>
    <property name="CanPlay" type="b" access="read"/>
    <property name="CanPause" type="b" access="read"/>
    <property name="CanGoNext" type="b" access="read"/>
    <property name="CanGoPrevious" type="b" access="read"/>
    <property name="CanSeek" type="b" access="read"/>
    <property name="Position" type="x" access="read"/>
    <property name="Metadata" type="a{sv}" access="read"/>
  </interface>
</node>
"""

fails = 0


def check(ok, text):
    global fails
    print(("  [OK]   " if ok else "  [HIBA] ") + text)
    if not ok:
        fails += 1


class MockMprisPlayer:
    def __init__(self, bus_name, identity, initial_status, ignore_pause=False):
        self.bus_name = bus_name
        self.identity = identity
        self.status = initial_status
        self.ignore_pause = ignore_pause
        self.pause_calls = 0
        self.playpause_calls = 0
        self.ready = threading.Event()
        self.owner_id = None
        self.reg_ids = []
        self.conn = None

    def register(self):
        node_info = Gio.DBusNodeInfo.new_for_xml(MPRIS_XML)
        # Mindegyik próba lejátszó saját privát D-Bus kapcsolatot nyit, mert ugyanazon a kapcsolaton
        # a /org/mpris/MediaPlayer2 objektumútvonal csak egyszer exportálható.
        addr = os.environ["DBUS_SESSION_BUS_ADDRESS"]
        flags = (
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
            | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION
        )
        self.conn = Gio.DBusConnection.new_for_address_sync(addr, flags, None, None)
        for iface in node_info.interfaces:
            rid = self.conn.register_object(
                "/org/mpris/MediaPlayer2",
                iface,
                self._on_method,
                self._on_get_prop,
                None,
            )
            self.reg_ids.append(rid)

        def on_name_acquired(_conn, _name):
            self.ready.set()

        self.owner_id = Gio.bus_own_name_on_connection(
            self.conn,
            self.bus_name,
            Gio.BusNameOwnerFlags.NONE,
            on_name_acquired,
            None,
        )

    def unregister(self):
        if self.owner_id:
            Gio.bus_unown_name(self.owner_id)
            self.owner_id = None
        if self.conn:
            for rid in self.reg_ids:
                self.conn.unregister_object(rid)
            self.reg_ids.clear()
            try:
                self.conn.close_sync(None)
            except Exception:
                pass
            self.conn = None

    def _on_method(self, _conn, _sender, _path, _iface, method_name, _params, invocation):
        if method_name == "Pause":
            self.pause_calls += 1
            if not self.ignore_pause and self.status == "Playing":
                self.status = "Paused"
        elif method_name == "PlayPause":
            self.playpause_calls += 1
            self.status = "Paused" if self.status == "Playing" else "Playing"
        elif method_name == "Play":
            self.status = "Playing"
        invocation.return_value(None)

    def _on_get_prop(self, _conn, _sender, _path, _iface, prop_name):
        if prop_name == "Identity":
            return GLib.Variant("s", self.identity)
        if prop_name == "PlaybackStatus":
            return GLib.Variant("s", self.status)
        if prop_name in ("CanPlay", "CanPause", "CanGoNext", "CanGoPrevious", "CanSeek"):
            return GLib.Variant("b", True)
        if prop_name == "Position":
            return GLib.Variant("x", 0)
        if prop_name == "Metadata":
            return GLib.Variant("a{sv}", {
                "xesam:title": GLib.Variant("s", "Teszt"),
                "mpris:trackid": GLib.Variant("o", "/org/mpris/MediaPlayer2/Track/1"),
            })
        return None


def ws_call(msg, wait_s=0.6):
    ws = ul.WS(ul.PORT)
    ws.send({"type": "hello", "client": "mpris_autopause_live"})
    ws.send(msg)
    time.sleep(wait_s)
    ws.s.close()


def main():
    work = os.environ.get("AQ_WORK", "/tmp")
    ul.PORT = int(os.environ.get("AQ_PORT", "47810"))
    print("========================================================")
    print(" MPRIS külső lejátszó automatikus szüneteltetése (élő)")
    print("========================================================")

    loop = GLib.MainLoop()
    m_play = MockMprisPlayer("org.mpris.MediaPlayer2.mock_sone", "MockPlayer1", "Playing", ignore_pause=False)
    m_paused = MockMprisPlayer("org.mpris.MediaPlayer2.mock_paused", "MockPlayer2", "Paused", ignore_pause=False)
    m_stubborn = MockMprisPlayer("org.mpris.MediaPlayer2.mock_stubborn", "MockPlayer3", "Playing", ignore_pause=True)

    m_play.register()
    m_paused.register()
    m_stubborn.register()

    th = threading.Thread(target=loop.run, daemon=True)
    th.start()
    try:
        ready = m_play.ready.wait(5.0) and m_paused.ready.wait(5.0) and m_stubborn.ready.wait(5.0)
        check(ready, "mindhárom próba MPRIS szolgáltatás felcsatlakozott az elszigetelt D-Bus buszra")
        if not ready:
            return 1

        wav = os.path.join(work, "local_track.wav")
        ul.make_sine_wav(wav, sr=44100, freq=440.0, seconds=10.0, amp=0.25)

        print("[M1] Hibás fájl megnyitása nem szünetelteti a külső lejátszót")
        ws_call({"type": "player_open", "path": os.path.join(work, "nincs_ilyen.wav")}, wait_s=0.5)
        check(
            m_play.status == "Playing" and m_play.pause_calls == 0 and m_stubborn.status == "Playing",
            f"M1: hibás fájlnál Playing maradt (play={m_play.status}, calls={m_play.pause_calls})",
        )

        print("[M2] Helyi fájl indítása szünetelteti az összes Playing külső lejátszót, a Paused érintetlen")
        ws_call({"type": "player_open", "path": wav}, wait_s=1.0)
        st = ul.get_status()
        check(
            m_play.status == "Paused" and m_play.pause_calls == 1 and m_play.playpause_calls == 0,
            f"M2: normál Playing lejátszó Pause-zal megállt (status={m_play.status}, pause={m_play.pause_calls}, toggle={m_play.playpause_calls})",
        )
        check(
            m_stubborn.status == "Paused" and m_stubborn.pause_calls == 1 and m_stubborn.playpause_calls == 1,
            f"M2: Pause-t ignoráló lejátszó az egyszeri PlayPause tartalékkal megállt (status={m_stubborn.status}, pause={m_stubborn.pause_calls}, toggle={m_stubborn.playpause_calls})",
        )
        check(
            m_paused.status == "Paused" and m_paused.pause_calls == 0 and m_paused.playpause_calls == 0,
            f"M2: már Paused lejátszó nem kapott parancsot (status={m_paused.status}, pause={m_paused.pause_calls}, toggle={m_paused.playpause_calls})",
        )
        check(
            st.get("path", {}).get("playerRate", 0) > 0,
            f"M2: a beépített lejátszó elindult (playerRate={st.get('path', {}).get('playerRate')})",
        )

        print("[M3] Szüneteltetett helyi fájl újraindítása (play) ismét szünetelteti a közben elindított külsőt")
        ws_call({"type": "media", "action": "pause"}, wait_s=0.4)
        m_play.status = "Playing"
        ws_call({"type": "media", "action": "play"}, wait_s=0.8)
        check(
            m_play.status == "Paused" and m_play.pause_calls == 2,
            f"M3: play() folytatás újra megállította a külső lejátszót (status={m_play.status}, pause={m_play.pause_calls})",
        )

        ws_call({"type": "player_stop"}, wait_s=0.3)
    finally:
        m_play.unregister()
        m_paused.unregister()
        m_stubborn.unregister()
        loop.quit()
        th.join(timeout=2.0)

    print(f"\n{('SIKERTELEN' if fails else 'MINDEN MPRIS AUTO-PAUSE TESZT SIKERES')} ({fails} hiba)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
