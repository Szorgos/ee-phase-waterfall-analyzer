#!/usr/bin/env python3
"""A motor teljes állapota (a fő lánc) JSON-ként, egy külön kapcsolaton (a felület tesztjeihez)."""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wsclient import WS  # noqa: E402
ws = WS(int(os.environ.get("AQ_PORT", "47810")))
ws.send({"type": "hello", "client": "get_state"})
ws.send({"type": "subscribe", "spectrum": 0})
m = ws.wait_type("state", limit=5000)
print(json.dumps(m or {}))
