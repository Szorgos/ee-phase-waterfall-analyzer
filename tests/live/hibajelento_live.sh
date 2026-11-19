#!/usr/bin/env bash
# Beépített hibajelentés, 1. szakasz – élő próba elszigetelt PipeWire-környezetben (Linux; pipewire, wireplumber,
# pipewire-pulse, pactl, dbus-daemon kell). A motort a próba maga indítja (--port 47899, saját XDG_CONFIG_HOME).
# Használat: tests/live/hibajelento_live.sh <build-mappa, -DAQ_TEST_HOOKS=ON fordítással>
set -u
BUILD=${1:-build}
ENGINE="$(cd "$BUILD" && pwd)/aqulify-engine"
HERE="$(cd "$(dirname "$0")" && pwd)"
W=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; [ -n "${DBUS_PID:-}" ] && kill $DBUS_PID 2>/dev/null; sleep 0.5; rm -rf "$W" 2>/dev/null' EXIT
export XDG_RUNTIME_DIR="$W/run" XDG_CONFIG_HOME="$W/conf" XDG_STATE_HOME="$W/state" PIPEWIRE_RUNTIME_DIR="$W/run"
unset PULSE_SERVER PULSE_SINK PULSE_SOURCE PULSE_COOKIE PIPEWIRE_REMOTE
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d" "$XDG_STATE_HOME"
chmod 700 "$XDG_RUNTIME_DIR"
cat > "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d/10-null.conf" <<'CONF'
context.objects = [
  { factory = adapter args = { factory.name = support.null-audio-sink node.name = "dac_a" node.description = "DAC A"
      media.class = Audio/Sink audio.position = [ FL FR ] object.linger = true node.driver = true
      priority.driver = 2000 priority.session = 2000 } }
]
CONF
eval "$(dbus-daemon --session --fork --print-address=1 --print-pid=1 | { read a; read p; echo "export DBUS_SESSION_BUS_ADDRESS=$a; DBUS_PID=$p"; })"
pipewire > "$W/pipewire.log" 2>&1 &
sleep 1.5
wireplumber > "$W/wireplumber.log" 2>&1 &
pipewire-pulse > "$W/pulse.log" 2>&1 &
sleep 2.5
python3 -I "$HERE/hibajelento_live.py" "$ENGINE" "$W"
rc=$?
if [ $rc -ne 0 ]; then
    for f in "$W"/hj*.out; do echo "--- $(basename "$f") ---"; tail -n 25 "$f"; done
fi
exit $rc
