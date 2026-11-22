#!/usr/bin/env bash
# A Linux Bluetooth RFCOMM szerver próbája valódi dbus-daemon ellen (hamis BlueZ-zal), lásd bt_bluez_live.py.
# Használat: tests/live/bt_bluez_live.sh <build-mappa>
set -u
BUILD=${1:-build}
HERE="$(cd "$(dirname "$0")" && pwd)"
exec python3 -I "$HERE/bt_bluez_live.py" "$BUILD/test_bt_bluez"
