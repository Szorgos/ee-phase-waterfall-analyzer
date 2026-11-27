#!/usr/bin/env bash
# Pactl és pipewire-pulse nélküli élő PipeWire teszt (#56: fej nélküli Raspberry Pi deb környezet):
# 1) wpctl alapértelmezett kimenet (aqulify_sink), natív sink-lista, currentSink és wpctl hangerő-olvasás/állítás;
# 2) 48 kHz-es korlátozott DAC (audio.allowed-rates = [32000 44100 48000]) + 44,1 kHz ×2 -> filter_only 44100 Hz;
# 3) nem-USB fix 48 kHz-es sink (SPA_PARAM_EnumFormat rate=48000) + 48 kHz ×2 -> filter_only 48000 Hz;
# 4) szabad DAC (EnumFormat 1..INT_MAX kiszűrve) + 44,1 kHz ×2 -> valódi 88200 Hz, pactl nélküli kimenetváltással.
set -u
EPORT=${AQ_ENGINE_PORT:-47810}
BUILD=${1:-build}
ENGINE="$(cd "$BUILD" && pwd)/aqulify-engine"
HERE="$(cd "$(dirname "$0")" && pwd)"
W=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; [ -n "${DBUS_PID:-}" ] && kill $DBUS_PID 2>/dev/null; sleep 0.5; rm -rf "$W" 2>/dev/null' EXIT
export XDG_RUNTIME_DIR="$W/run" XDG_CONFIG_HOME="$W/conf" XDG_STATE_HOME="$W/state" PIPEWIRE_RUNTIME_DIR="$W/run"
unset PULSE_SERVER PULSE_SINK PULSE_SOURCE PULSE_COOKIE PIPEWIRE_REMOTE
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d" "$XDG_STATE_HOME" "$W/bin"
chmod 700 "$XDG_RUNTIME_DIR"

# Szándékosan pactl nélküli PATH (és pipewire-pulse sem indul), mint fej nélküli Debian/Pi csomagtelepítésnél
cat > "$W/bin/pactl" <<'EOF'
#!/bin/sh
exit 127
EOF
chmod +x "$W/bin/pactl"
export PATH="$W/bin:$PATH"

cat > "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d/10-nopactl.conf" <<'CONF'
context.objects = [
  { factory = adapter args = { factory.name = support.null-audio-sink node.name = "dac_burrbrown"
      node.description = "BurrBrown USB AUDIO DAC" media.class = Audio/Sink audio.position = [ FL FR ]
      object.linger = true node.driver = true priority.driver = 2000 priority.session = 2000
      audio.allowed-rates = [ 32000 44100 48000 ] } }
  { factory = adapter args = { factory.name = support.null-audio-sink node.name = "dac_fix48"
      node.description = "Fixed 48k I2S/HDA DAC" media.class = Audio/Sink audio.position = [ FL FR ]
      object.linger = true node.driver = true priority.driver = 1500 priority.session = 1500
      audio.rate = 48000 } }
  { factory = adapter args = { factory.name = support.null-audio-sink node.name = "dac_alt"
      node.description = "Hi-Res DAC" media.class = Audio/Sink audio.position = [ FL FR ]
      object.linger = true node.driver = true priority.driver = 1000 priority.session = 1000 } }
]
CONF

eval "$(dbus-daemon --session --fork --print-address=1 --print-pid=1 | { read a; read p; echo "export DBUS_SESSION_BUS_ADDRESS=$a; DBUS_PID=$p"; })"
pipewire > "$W/pipewire.log" 2>&1 &
sleep 1.5
wireplumber > "$W/wireplumber.log" 2>&1 &
sleep 2.0
"$ENGINE" --port "$EPORT" > "$W/engine.log" 2>&1 &
ENGINE_PID=$!
sleep 3.0

AQ_PORT=$EPORT AQ_WORK="$W" python3 -I "$HERE/nopactl_56_live.py" "$EPORT" "$W"
rc=$?

if ! kill -0 $ENGINE_PID 2>/dev/null; then
    wait $ENGINE_PID; erc=$?
    echo "  [HIBA] a motor leállt vagy összeomlott a teszt alatt (kilépési kód $erc)"
    rc=1
fi
[ $rc -ne 0 ] && { echo "--- motor napló ---"; tail -n 60 "$W/engine.log"; }
exit $rc
