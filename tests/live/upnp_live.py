#!/usr/bin/env python3
"""UPnP / DLNA renderer élő tesztje (Linux): egy kis Python-vezérlő a valódi motor ellen.

Indítás (run_all.sh): tests/live/multiout_live.sh <build> python3 -I tests/live/upnp_live.py
A gépnek kell egy magánhálózati (RFC 1918) IPv4-címe: a renderer csak ilyen interfészen fogad vezérlést, és a
médiacím is csak a kérés alhálózatából jöhet. A próbaszerver (Range-dzsel) és a GENA-visszahívás ezen a címen fut.

Lépések: alapból ki → bekapcsolás (audio_opts) → SSDP-felfedezés (M-SEARCH, MediaRenderer:1) → leírás és SCPD →
GENA-feliratkozás → SetAVTransportURI + Play → „most szól” a motor állapotában → Seek → SetNextAVTransportURI
(a következő szám a mostani vége után szól, az AVTransportURI átvált) → hangerő és fix hangerő → hibakódok (tiltott
cím 716, rossz InstanceID 718) → Stop → kikapcsolás (a szolgáltatás eltűnik).
Környezet: AQ_UPNP_NO_MULTICAST=1 – a felfedezés helyett a motor által jelentett címet használja (multicast nélküli
gépen); AQ_UPNP_EVENTS_OPTIONAL=1 – a GENA-értesítés hiánya csak figyelmeztetés (pl. alkalmazásonkénti tűzfal).
"""
import http.server
import json
import math
import os
import socket
import struct
import sys
import threading
import time
import urllib.request
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wsclient import WS  # noqa: E402

PORT = int(os.environ.get("AQ_PORT", "47810"))
NAME = "Aqulify élő teszt"
AVT = "urn:schemas-upnp-org:service:AVTransport:1"
RCS = "urn:schemas-upnp-org:service:RenderingControl:1"
CM = "urn:schemas-upnp-org:service:ConnectionManager:1"
fails = 0


def check(ok, msg):
    global fails
    print(("  [OK]   " if ok else "  [HIBA] ") + msg, flush=True)
    if not ok:
        fails += 1


def warn(msg):
    print("  [FIGY] " + msg, flush=True)


def wait(cond, sec=8.0, step=0.1):
    end = time.time() + sec
    while time.time() < end:
        v = cond()
        if v:
            return v
        time.sleep(step)
    return cond()


def opts(**kw):
    ws = WS(PORT)
    ws.send({"type": "hello", "client": "upnp_live"})
    ws.send({"type": "set_audio_opts" if kw else "get_audio_opts", "opts": kw})
    m = ws.wait_type("audio_opts", limit=2000)
    ws.s.close()
    return m or {}


def status():
    ws = WS(PORT)
    ws.send({"type": "hello", "client": "upnp_live"})
    m = ws.wait_type("status", limit=4000)
    ws.s.close()
    return m or {}


def wav(seconds, freq, rate=44100):
    """16 bites sztereó WAV (szinusz), címke nélkül."""
    n = int(seconds * rate)
    frames = bytearray()
    for i in range(n):
        v = int(8000 * math.sin(2 * math.pi * freq * i / rate))
        frames += struct.pack("<hh", v, v)
    hdr = b"RIFF" + struct.pack("<I", 36 + len(frames)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, rate, rate * 4, 4, 16)
    return hdr + b"data" + struct.pack("<I", len(frames)) + bytes(frames)


# ------------------------------------------------------------------------------------------------ próbaszerver

FILES = {}
REQUESTS = []


class Media(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def do_GET(self):
        body = FILES.get(self.path)
        REQUESTS.append((self.path, self.headers.get("Range", "")))
        if body is None:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        start = 0
        rng = self.headers.get("Range", "")
        if rng.startswith("bytes=") and rng[6:].split("-")[0].isdigit():
            start = int(rng[6:].split("-")[0])
        if start:
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{len(body) - 1}/{len(body)}")
        else:
            self.send_response(200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(len(body) - start))
        self.end_headers()
        self.wfile.write(body[start:])


EVENTS = []


class Events(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def do_NOTIFY(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n).decode("utf-8", "replace")
        EVENTS.append((self.path, self.headers.get("SID", ""), self.headers.get("SEQ", ""), body))
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()


def serve(handler, ip):
    srv = http.server.ThreadingHTTPServer((ip, 0), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


# ------------------------------------------------------------------------------------------------ UPnP-vezérlő

def discover(ip, timeout=4.0):
    """M-SEARCH a MediaRenderer:1-re a megadott interfészen; a válaszok LOCATION-jei."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.bind((ip, 0))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(ip))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
    msg = ("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\n"
           "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n\r\n").encode()
    s.sendto(msg, ("239.255.255.250", 1900))
    s.settimeout(0.3)
    locs, end, resent = set(), time.time() + timeout, False
    while time.time() < end:
        if not resent and time.time() > end - timeout / 2:
            s.sendto(msg, ("239.255.255.250", 1900))   # az UDP elveszhet
            resent = True
        try:
            data, _ = s.recvfrom(4096)
        except socket.timeout:
            continue
        text = data.decode("utf-8", "replace")
        if not text.startswith("HTTP/1.1 200"):
            continue
        hdr = {k.strip().upper(): v.strip() for k, _, v in (l.partition(":") for l in text.split("\r\n")[1:]) if k}
        if hdr.get("ST") == "urn:schemas-upnp-org:device:MediaRenderer:1" and "LOCATION" in hdr:
            locs.add(hdr["LOCATION"])
    s.close()
    return locs


def local(tag):
    return tag.rsplit("}", 1)[-1]


def soap(base, key, stype, action, **args):
    """(http-állapot, {kimenő argumentum: érték} vagy {"errorCode": n})"""
    body = (f'<?xml version="1.0" encoding="utf-8"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            f's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body><u:{action} xmlns:u="{stype}">' +
            "".join(f"<{k}>{escape(str(v))}</{k}>" for k, v in args.items()) + f"</u:{action}></s:Body></s:Envelope>")
    req = urllib.request.Request(f"{base}/ctl/{key}", data=body.encode(), method="POST",
                                 headers={"Content-Type": 'text/xml; charset="utf-8"', "SOAPACTION": f'"{stype}#{action}"'})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            code, text = r.status, r.read()
    except urllib.error.HTTPError as e:
        code, text = e.code, e.read()
    root = ET.fromstring(text)
    out = {}
    for el in root.iter():
        if local(el.tag) == "errorCode":
            return code, {"errorCode": int(el.text)}
        if local(el.tag) == action + "Response":
            for c in el:
                out[local(c.tag)] = c.text or ""
    return code, out


def gena(base, method, key, headers):
    req = urllib.request.Request(f"{base}/evt/{key}", method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers)


def didl(title, url, mime="audio/wav", dur=""):
    d = f' duration="{dur}"' if dur else ""
    return ('<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" xmlns:dc="http://purl.org/dc/elements/1.1/" '
            'xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/"><item id="1" parentID="0" restricted="1">'
            f'<dc:title>{escape(title)}</dc:title><upnp:artist>Teszt előadó</upnp:artist><upnp:album>Teszt album</upnp:album>'
            '<upnp:class>object.item.audioItem.musicTrack</upnp:class>'
            f'<res protocolInfo="http-get:*:{mime}:*"{d}>{escape(url)}</res></item></DIDL-Lite>')


def tsec(t):
    h, m, s = t.split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


# ------------------------------------------------------------------------------------------------ a teszt

print("UPnP / DLNA renderer")
m = opts()
check(m.get("caps", {}).get("upnp") is True, "a képesség megvan (caps.upnp)")
check(m.get("opts", {}).get("upnp") is False, "alapból ki")
check(m.get("upnpState", {}).get("state") == "off", f"állapot: off ({m.get('upnpState')})")

m = opts(upnp=True, upnpName=NAME)
st = m.get("upnpState", {})
check(st.get("state") == "running" and st.get("name") == NAME, f"bekapcsolva, fut ({st.get('state')}, {st.get('name')})")
addrs = st.get("addresses", [])
if not addrs:
    print("  [HIBA] a gépnek nincs magánhálózati címe: a renderer nem érhető el (a teszt itt véget ér)")
    opts(upnp=False)
    sys.exit(1)
ip, hport = addrs[0], st.get("port")
print(f"  (cím: {ip}, HTTP-port: {hport})")

if os.environ.get("AQ_UPNP_NO_MULTICAST") == "1":
    base = f"http://{ip}:{hport}"
    warn("felfedezés kihagyva (AQ_UPNP_NO_MULTICAST=1)")
else:
    locs = discover(ip)
    mine = [l for l in locs if f"//{ip}:{hport}/" in l]
    check(bool(mine), f"SSDP: M-SEARCH-re válaszolt ({sorted(locs)})")
    base = f"http://{ip}:{hport}"
with urllib.request.urlopen(f"{base}/description.xml", timeout=5) as r:
    desc = ET.fromstring(r.read())
fn = [e.text for e in desc.iter() if local(e.tag) == "friendlyName"]
dt = [e.text for e in desc.iter() if local(e.tag) == "deviceType"]
svcs = [e.text for e in desc.iter() if local(e.tag) == "serviceType"]
check(fn == [NAME] and dt == ["urn:schemas-upnp-org:device:MediaRenderer:1"], f"leírás: {fn}, {dt}")
check(set(svcs) == {AVT, RCS, CM}, "három szolgáltatás (AVTransport, RenderingControl, ConnectionManager)")
with urllib.request.urlopen(f"{base}/AVTransport.xml", timeout=5) as r:
    scpd = r.read().decode()
check("<name>SetNextAVTransportURI</name>" in scpd and "<name>Seek</name>" in scpd, "AVTransport SCPD")
code, out = soap(base, "cm", CM, "GetProtocolInfo")
check(code == 200 and "http-get:*:audio/flac:*" in out.get("Sink", ""), "GetProtocolInfo: FLAC a Sink-ben")

# próbaszerver és visszahívás a gép magánhálózati címén
FILES["/egy.wav"] = wav(5.0, 440)
FILES["/ketto.wav"] = wav(2.0, 660)
msrv = serve(Media, ip)
mbase = f"http://{ip}:{msrv.server_address[1]}"
esrv = serve(Events, ip)
code, hdrs = gena(base, "SUBSCRIBE", "avt", {"CALLBACK": f"<http://{ip}:{esrv.server_address[1]}/avt>", "NT": "upnp:event",
                                             "TIMEOUT": "Second-300"})
sid = hdrs.get("SID", "")
check(code == 200 and sid.startswith("uuid:") and hdrs.get("TIMEOUT") == "Second-300", f"GENA SUBSCRIBE ({code}, {sid})")
events_ok = wait(lambda: any(e[1] == sid and e[2] == "0" for e in EVENTS), 5)
if events_ok or os.environ.get("AQ_UPNP_EVENTS_OPTIONAL") != "1":
    check(bool(events_ok), "kezdő esemény (SEQ 0)")
else:
    warn("nem jött kezdő esemény (AQ_UPNP_EVENTS_OPTIONAL=1: a motor kimenő kapcsolatát tűzfal tilthatja)")
if events_ok:
    lc = [e[3] for e in EVENTS if e[1] == sid and e[2] == "0"][0]
    check("TransportState val=&quot;NO_MEDIA_PRESENT&quot;" in lc, "a kezdő LastChange a teljes állapot (NO_MEDIA_PRESENT)")

# hibakódok
code, out = soap(base, "avt", AVT, "Play", InstanceID=1, Speed=1)
check(code == 500 and out.get("errorCode") == 718, f"rossz InstanceID: 718 ({out})")
code, out = soap(base, "avt", AVT, "Play", InstanceID=0, Speed=1)
check(out.get("errorCode") == 701, f"Play adathordozó nélkül: 701 ({out})")
code, out = soap(base, "avt", AVT, "SetAVTransportURI", InstanceID=0, CurrentURI="http://127.0.0.1:9/x.wav", CurrentURIMetaData="")
check(out.get("errorCode") == 716, f"loopback médiacím: 716 ({out})")
code, out = soap(base, "avt", AVT, "SetAVTransportURI", InstanceID=0, CurrentURI="http://169.254.169.254/latest/", CurrentURIMetaData="")
check(out.get("errorCode") == 716, f"link-local médiacím: 716 ({out})")

# SetURI + Play
u1, u2 = f"{mbase}/egy.wav", f"{mbase}/ketto.wav"
code, out = soap(base, "avt", AVT, "SetAVTransportURI", InstanceID=0, CurrentURI=u1, CurrentURIMetaData=didl("Első szám", u1, dur="0:00:05"))
check(code == 200, "SetAVTransportURI")
code, out = soap(base, "avt", AVT, "GetMediaInfo", InstanceID=0)
check(out.get("CurrentURI") == u1 and out.get("MediaDuration") == "0:00:05", f"GetMediaInfo ({out.get('CurrentURI')}, {out.get('MediaDuration')})")
code, out = soap(base, "avt", AVT, "Play", InstanceID=0, Speed=1)
check(code == 200, "Play")
ok = wait(lambda: soap(base, "avt", AVT, "GetTransportInfo", InstanceID=0)[1].get("CurrentTransportState") == "PLAYING", 10)
check(bool(ok), "PLAYING")
check(any(p == "/egy.wav" for p, _ in REQUESTS), "a motor letöltötte a számot a próbaszerverről")
med = (status().get("media") or {})
check(med.get("title") == "Első szám" and med.get("artist") == "Teszt előadó" and med.get("app") == "UPnP",
      f"„most szól” a motor állapotában ({med.get('app')}: {med.get('title')} / {med.get('artist')})")
check((med.get("fmt") or {}).get("rate") == 44100 and (med.get("fmt") or {}).get("bits") == 16, f"formátum: {med.get('fmt')}")
if events_ok:
    check(wait(lambda: any(e[1] == sid and "TransportState val=&quot;PLAYING&quot;" in e[3] for e in EVENTS), 3),
          "LastChange-esemény: PLAYING")

# Seek
code, out = soap(base, "avt", AVT, "Seek", InstanceID=0, Unit="REL_TIME", Target="0:00:02")
check(code == 200, "Seek REL_TIME 0:00:02")
code, out = soap(base, "avt", AVT, "GetPositionInfo", InstanceID=0)
rel = tsec(out.get("RelTime", "0:00:00"))
check(2.0 <= rel < 4.5 and out.get("TrackURI") == u1, f"GetPositionInfo a tekerés után: {out.get('RelTime')}")
code, out = soap(base, "avt", AVT, "Seek", InstanceID=0, Unit="X_DLNA_REL_BYTE", Target="100")
check(out.get("errorCode") == 710, f"ismeretlen Seek-mód: 710 ({out})")

# SetNext: a következő szám a mostani után
code, out = soap(base, "avt", AVT, "SetNextAVTransportURI", InstanceID=0, NextURI=u2, NextURIMetaData=didl("Második szám", u2))
check(code == 200, "SetNextAVTransportURI")
check(wait(lambda: any(p == "/ketto.wav" for p, _ in REQUESTS), 5), "a következő szám letöltése azonnal indul")
code, out = soap(base, "avt", AVT, "GetMediaInfo", InstanceID=0)
check(out.get("NextURI") == u2, "GetMediaInfo: NextURI")
ok = wait(lambda: soap(base, "avt", AVT, "GetMediaInfo", InstanceID=0)[1].get("CurrentURI") == u2, 10)
check(bool(ok), "a szám végén a következő szól (AVTransportURI = NextURI)")
code, out = soap(base, "avt", AVT, "GetTransportInfo", InstanceID=0)
check(out.get("CurrentTransportState") == "PLAYING", f"továbbra is PLAYING ({out.get('CurrentTransportState')})")
med = (status().get("media") or {})
check(med.get("title") == "Második szám", f"„most szól”: {med.get('title')}")
code, out = soap(base, "avt", AVT, "GetMediaInfo", InstanceID=0)
check(out.get("NextURI", "") == "", "a NextURI kiürült")

# hangerő
code, out = soap(base, "rcs", RCS, "SetVolume", InstanceID=0, Channel="Master", DesiredVolume=40)
code2, out2 = soap(base, "rcs", RCS, "GetVolume", InstanceID=0, Channel="Master")
check(code == 200 and out2.get("CurrentVolume") == "40", f"SetVolume / GetVolume ({out2})")
opts(upnpFixedVolume=True)
code, out = soap(base, "rcs", RCS, "GetVolume", InstanceID=0, Channel="Master")
check(out.get("CurrentVolume") == "100", f"fix hangerő: a vezérlő 100-at lát ({out})")
opts(upnpFixedVolume=False)

# a szám vége, Stop
ok = wait(lambda: soap(base, "avt", AVT, "GetTransportInfo", InstanceID=0)[1].get("CurrentTransportState") == "STOPPED", 8)
check(bool(ok), "az utolsó szám végén STOPPED")
code, out = soap(base, "avt", AVT, "Play", InstanceID=0, Speed=1)
ok = wait(lambda: soap(base, "avt", AVT, "GetTransportInfo", InstanceID=0)[1].get("CurrentTransportState") == "PLAYING", 8)
check(code == 200 and bool(ok), "Play újra (a már letöltött számmal)")
code, out = soap(base, "avt", AVT, "Stop", InstanceID=0)
code2, out2 = soap(base, "avt", AVT, "GetTransportInfo", InstanceID=0)
check(code == 200 and out2.get("CurrentTransportState") == "STOPPED", "Stop")
check(not (status().get("media") or {}).get("playing"), "a motor lejátszója nem szól")

code, _ = gena(base, "UNSUBSCRIBE", "avt", {"SID": sid})
check(code == 200, "GENA UNSUBSCRIBE")
st = opts().get("upnpState", {})
check(st.get("renderer", {}).get("peer") == ip, f"az utolsó vezérlő címe a felületnek ({st.get('renderer', {}).get('peer')})")

# kikapcsolás
m = opts(upnp=False)
check(m.get("upnpState", {}).get("state") == "off", "kikapcsolva")
try:
    urllib.request.urlopen(f"{base}/description.xml", timeout=2)
    gone = False
except Exception:
    gone = True
check(gone, "kikapcsolás után a leírás nem érhető el")
conf = os.path.join(os.environ.get("AQ_WORK", "/tmp"), "conf", "aqulify", "audio.json")
try:
    saved = json.load(open(conf))
    check(saved.get("upnp") is False and saved.get("upnpName") == NAME, "a beállítás az audio.json-ban")
except OSError:
    warn(f"az audio.json nem olvasható ({conf})")
msrv.shutdown()
esrv.shutdown()
print("UPnP élő teszt: " + ("SIKERES" if fails == 0 else f"{fails} HIBA"))
sys.exit(1 if fails else 0)
