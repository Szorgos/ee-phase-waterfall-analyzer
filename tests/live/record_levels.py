#!/usr/bin/env python3
"""Felvétel a két hangkártyáról teszthanggal (multiout_live.record), a szintek JSON-ként. Használat: record_levels.py <név>"""
import json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import multiout_live as ml   # noqa: E402
ml.WORK = os.environ.get("AQ_WORK", HERE)
t = os.path.join(ml.WORK, "tone.wav")
if not os.path.exists(t):
    ml.tone(t)
a, b = ml.record(sys.argv[1], t)
print(json.dumps({"a": {str(k): v for k, v in a.items()}, "b": {str(k): v for k, v in b.items()}}))
