#!/usr/bin/env bash
# Több kimenet élő tesztje elszigetelt PipeWire-környezetben (Linux; pipewire, wireplumber, pipewire-pulse,
# pactl, dbus-daemon kell). Használat: tests/live/multiout_live.sh <build-mappa> [parancs…]
# Parancs nélkül a motor élő tesztje (multiout_live.py) fut; különben a megadott parancs, AQ_PORT és AQ_WORK
# környezeti változóval (pl. a felület böngészős tesztje).
set -u
BUILD=${1:-build}
shift || true
ENGINE="$(cd "$BUILD" && pwd)/aqulify-engine"
HERE="$(cd "$(dirname "$0")" && pwd)"
W=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; [ -n "${DBUS_PID:-}" ] && kill $DBUS_PID 2>/dev/null; sleep 0.5; rm -rf "$W" 2>/dev/null' EXIT
export XDG_RUNTIME_DIR="$W/run" XDG_CONFIG_HOME="$W/conf" XDG_STATE_HOME="$W/state" PIPEWIRE_RUNTIME_DIR="$W/run"
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d" "$XDG_STATE_HOME"
chmod 700 "$XDG_RUNTIME_DIR"
cat > "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d/10-null.conf" <<'CONF'
context.objects = [
  { factory = adapter args = { factory.name = support.null-audio-sink node.name = "dac_a" node.description = "DAC A"
      media.class = Audio/Sink audio.position = [ FL FR ] object.linger = true node.driver = true
      priority.driver = 2000 priority.session = 2000 } }
  { factory = adapter args = { factory.name = support.null-audio-sink node.name = "dac_b" node.description = "DAC B"
      media.class = Audio/Sink audio.position = [ FL FR ] object.linger = true node.driver = true
      priority.driver = 1000 priority.session = 1000 } }
]
CONF
eval "$(dbus-daemon --session --fork --print-address=1 --print-pid=1 | { read a; read p; echo "export DBUS_SESSION_BUS_ADDRESS=$a; DBUS_PID=$p"; })"
pipewire > "$W/pipewire.log" 2>&1 &
sleep 1.5
wireplumber > "$W/wireplumber.log" 2>&1 &
pipewire-pulse > "$W/pulse.log" 2>&1 &
sleep 2.5
"$ENGINE" --port 47810 > "$W/engine.log" 2>&1 &
sleep 3
if [ $# -gt 0 ]; then
    AQ_PORT=47810 AQ_WORK="$W" "$@"
else
    python3 -I "$HERE/multiout_live.py" 47810 "$W"
fi
rc=$?
[ $rc -ne 0 ] && { echo "--- motor napló ---"; tail -n 40 "$W/engine.log"; }
exit $rc
