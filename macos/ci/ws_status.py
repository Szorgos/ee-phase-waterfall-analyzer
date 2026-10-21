#!/usr/bin/env python3
"""CI füstteszt: kapcsolódik a futó Aqulify hídjához (ws://127.0.0.1:PORT), és kiírja az első "status" üzenetet.
Kilépési kód: 0, ha az üzenet macOS motortól jött; 1 egyébként. Használat: ws_status.py [port] [időkorlát_mp]"""
import base64
import json
import os
import socket
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 47810
deadline = time.time() + (float(sys.argv[2]) if len(sys.argv) > 2 else 20.0)

s = socket.create_connection(("127.0.0.1", port), timeout=5)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall((f"GET / HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
buf = b""
while b"\r\n\r\n" not in buf:
    chunk = s.recv(4096)
    if not chunk:
        sys.exit("a híd bontotta a kapcsolatot a kézfogás közben")
    buf += chunk
head, buf = buf.split(b"\r\n\r\n", 1)
if b" 101 " not in head.split(b"\r\n")[0]:
    sys.exit("nem WebSocket válasz: " + head.decode(errors="replace"))


def need(n):
    global buf
    while len(buf) < n:
        chunk = s.recv(65536)
        if not chunk:
            sys.exit("a híd bontotta a kapcsolatot")
        buf += chunk
    out, buf = buf[:n], buf[n:]
    return out


while time.time() < deadline:
    b0, b1 = need(2)
    n = b1 & 0x7F
    if n == 126:
        n = int.from_bytes(need(2), "big")
    elif n == 127:
        n = int.from_bytes(need(8), "big")
    payload = need(n)
    if b0 & 0x0F != 1:
        continue
    try:
        msg = json.loads(payload)
    except ValueError:
        continue
    if msg.get("type") == "status":
        print(json.dumps(msg, ensure_ascii=False, indent=1))
        sys.exit(0 if msg.get("platform") == "macos" else 1)
sys.exit("nem jött status üzenet")
