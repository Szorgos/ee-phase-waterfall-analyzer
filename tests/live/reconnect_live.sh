#!/usr/bin/env bash
# PipeWire lejátszó stream automatikus újracsatlakozásának élő tesztje (AQ-19):
# 1) A motor úgy indul, hogy még egyetlen kimeneti sink sincs (no target node available hiba),
#    majd megjelenik a test_dac (module-null-sink) -> magától újracsatlakozik és szól.
# 2) Futás közben a test_dac eltávolítása, majd újbóli létrehozása -> motor-újraindítás nélkül újra szól.
set -u
BUILD=${1:-build}
ENGINE="$(cd "$BUILD" && pwd)/aqulify-engine"
HERE="$(cd "$(dirname "$0")" && pwd)"
W=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; [ -n "${DBUS_PID:-}" ] && kill $DBUS_PID 2>/dev/null; sleep 0.5; rm -rf "$W" 2>/dev/null' EXIT
export XDG_RUNTIME_DIR="$W/run" XDG_CONFIG_HOME="$W/conf" XDG_STATE_HOME="$W/state" PIPEWIRE_RUNTIME_DIR="$W/run"
unset PULSE_SERVER PULSE_SINK PULSE_SOURCE PULSE_COOKIE PIPEWIRE_REMOTE
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME" "$XDG_STATE_HOME"
chmod 700 "$XDG_RUNTIME_DIR"

eval "$(dbus-daemon --session --fork --print-address=1 --print-pid=1 | { read a; read p; echo "export DBUS_SESSION_BUS_ADDRESS=$a; DBUS_PID=$p"; })"
pipewire > "$W/pipewire.log" 2>&1 &
sleep 1.5
wireplumber > "$W/wireplumber.log" 2>&1 &
pipewire-pulse > "$W/pulse.log" 2>&1 &
sleep 2.5

# A motor kimeneti eszköz nélkül indul (valódi "no target node available" helyzet, mint AQ-19)
"$ENGINE" --port 47810 > "$W/engine.log" 2>&1 &
ENGINE_PID=$!
sleep 2.5

AQ_PORT=47810 AQ_WORK="$W" python3 -I "$HERE/reconnect_live.py" 47810 "$W"
rc=$?

if ! kill -0 $ENGINE_PID 2>/dev/null; then
    wait $ENGINE_PID; erc=$?
    echo "  [HIBA] a motor leállt vagy összeomlott a teszt alatt (kilépési kód $erc)"
    rc=1
fi
[ $rc -ne 0 ] && { echo "--- motor napló ---"; tail -n 60 "$W/engine.log"; }
exit $rc
