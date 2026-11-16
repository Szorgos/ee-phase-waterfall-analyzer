#!/usr/bin/env python3
"""A kiadás aqulify-versions.json fájlja (a motor és az appok verzióellenőrzése: include/version_check.hpp).

Használat: python3 tools/versions_json.py --desktop 2.3.0 --out aqulify-versions.json [--android 1.0.1]

- desktop: a kiadás verziója (a Pi-csomag ugyanezt követi; a csomagrevízió nem számít).
- protocol / min_client_protocol: az include/ws_bridge.hpp kBridgeProtocol / kBridgeMinClientProtocol értéke.
- android: ha nincs megadva, a tároló kiadásai közül a legmagasabb Aqulify-Remote-X.Y.Z.apk fájlnév verziója
  (GitHub API; GITHUB_TOKEN, ha van). A „latest/download” itt nem használható: a most készülő kiadás már a legújabb,
  és még nincs benne ilyen fájl.
"""
import argparse
import datetime
import json
import os
import re
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VER = re.compile(r"^\d{1,4}\.\d{1,4}\.\d{1,4}$")
APK = re.compile(r"^Aqulify-Remote-(\d{1,4}\.\d{1,4}\.\d{1,4})\.apk$")


def bridge_const(name):
    text = open(os.path.join(ROOT, "include", "ws_bridge.hpp"), encoding="utf-8").read()
    m = re.search(r"constexpr int " + name + r" = (\d+);", text)
    if not m:
        sys.exit(f"HIBA: {name} nem található az include/ws_bridge.hpp-ban")
    return int(m.group(1))


def key(v):
    return tuple(int(x) for x in v.split("."))


def latest_android(repo):
    req = urllib.request.Request(f"https://api.github.com/repos/{repo}/releases?per_page=30",
                                 headers={"Accept": "application/vnd.github+json", "User-Agent": "aqulify-release"})
    tok = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if tok:
        req.add_header("Authorization", f"Bearer {tok}")
    with urllib.request.urlopen(req, timeout=30) as r:
        releases = json.load(r)
    found = [m.group(1) for rel in releases for a in rel.get("assets", []) if (m := APK.match(a.get("name", "")))]
    return max(found, key=key) if found else ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--desktop", required=True)
    ap.add_argument("--android", default="")
    ap.add_argument("--repo", default="Szorgos/ee-phase-waterfall-analyzer")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    if not VER.match(a.desktop):
        sys.exit(f"HIBA: érvénytelen asztali verzió: {a.desktop}")
    android = a.android or latest_android(a.repo)
    if android and not VER.match(android):
        sys.exit(f"HIBA: érvénytelen android-verzió: {android}")
    doc = {
        "desktop": a.desktop,
        "android": android,
        "protocol": bridge_const("kBridgeProtocol"),
        "min_client_protocol": bridge_const("kBridgeMinClientProtocol"),
        "date": datetime.date.today().isoformat(),
    }
    if not android:
        del doc["android"]
        print("FIGYELEM: nem található Aqulify-Remote-*.apk a kiadásokban; az android mező kimarad", file=sys.stderr)
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False)
        f.write("\n")
    print(json.dumps(doc, ensure_ascii=False))


if __name__ == "__main__":
    main()
