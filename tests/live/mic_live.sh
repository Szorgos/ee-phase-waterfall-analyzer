#!/usr/bin/env bash
# A mikrofonfelvevő élő tesztje elszigetelt PipeWire-környezetben (Linux; pipewire, wireplumber, dbus-daemon kell).
# Használat: tests/live/mic_live.sh <build-mappa>
# Két null-kimenet (dac_a: ide szól a mérőjel; dac_b: néma), a felvevő a monitorjukról vesz fel (src/test_mic_live.cpp).
set -u
BUILD=${1:-build}
TEST="$(cd "$BUILD" && pwd)/test_mic_live"
W=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; [ -n "${DBUS_PID:-}" ] && kill $DBUS_PID 2>/dev/null; sleep 0.5; rm -rf "$W" 2>/dev/null' EXIT
export XDG_RUNTIME_DIR="$W/run" XDG_CONFIG_HOME="$W/conf" XDG_STATE_HOME="$W/state" PIPEWIRE_RUNTIME_DIR="$W/run"
unset PULSE_SERVER PULSE_SINK PULSE_SOURCE PULSE_COOKIE PIPEWIRE_REMOTE
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d" "$XDG_STATE_HOME"
chmod 700 "$XDG_RUNTIME_DIR"
cat > "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d/10-null.conf" <<'CONF'
context.properties = { default.clock.rate = 48000 }
context.objects = [
  { factory = adapter args = { factory.name = support.null-audio-sink node.name = "dac_a" node.description = "DAC A"
      media.class = Audio/Sink audio.position = [ MONO ] object.linger = true node.driver = true
      priority.driver = 2000 priority.session = 2000 } }
  { factory = adapter args = { factory.name = support.null-audio-sink node.name = "dac_b" node.description = "DAC B"
      media.class = Audio/Sink audio.position = [ MONO ] object.linger = true node.driver = true
      priority.driver = 1000 priority.session = 1000 } }
]
CONF
eval "$(dbus-daemon --session --fork --print-address=1 --print-pid=1 | { read a; read p; echo "export DBUS_SESSION_BUS_ADDRESS=$a; DBUS_PID=$p"; })"
pipewire > "$W/pipewire.log" 2>&1 &
sleep 1.5
wireplumber > "$W/wireplumber.log" 2>&1 &
sleep 2.5
"$TEST"
rc=$?
[ $rc -ne 0 ] && { echo "--- pipewire napló ---"; tail -n 20 "$W/pipewire.log"; echo "--- wireplumber napló ---"; tail -n 20 "$W/wireplumber.log"; }
exit $rc
