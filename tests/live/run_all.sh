#!/usr/bin/env bash
# Az összes élő teszt egymás után, mindegyik saját, tiszta PipeWire-környezetben.
# Használat: tests/live/run_all.sh <build-mappa>   (a felületi tesztekhez Node.js és Playwright kell)
set -u
BUILD=${1:-build}
HERE="$(cd "$(dirname "$0")" && pwd)"
fail=0
run() {
    echo; echo "=================== $1"
    shift
    "$HERE/multiout_live.sh" "$BUILD" "$@" || fail=1
}
run "Motor: több kimenet"
run "Motor: útonkénti EQ" python3 -I "$HERE/way_eq_live.py"
run "Motor: DSP-modulok" python3 -I "$HERE/modules_live.py"
run "Motor: haladó hangkimenet" python3 -I "$HERE/audio_opts_live.py"
run "Motor: felmintavételezés" python3 -I "$HERE/upsample_live.py"
run "Motor: felmintavételezés az élő úton" python3 -I "$HERE/live_upsample_live.py"
run "Motor: külső lejátszók automatikus szüneteltetése (MPRIS)" python3 -I "$HERE/mpris_autopause_live.py"
run "Motor: nagy felbontás (352,8–768 kHz)" python3 -I "$HERE/hires_live.py"
NR_DIR=$(mktemp -d)
PATH="$HERE/fake_netrecv:$PATH" AQ_FAKE_NETRECV_DIR="$NR_DIR" run "Motor: hálózati vevő (hamis program)" python3 -I "$HERE/netrecv_live.py"
rm -rf "$NR_DIR"
run "Motor: UPnP / DLNA renderer (felfedezés, SetURI / Play / Seek / SetNext / Stop)" python3 -I "$HERE/upnp_live.py"
echo; echo "=================== Motor: PipeWire újracsatlakozás (AQ-19)"
bash "$HERE/reconnect_live.sh" "$BUILD" || fail=1
echo; echo "=================== Motor: pactl nélküli PipeWire, wpctl és 48 kHz-es DAC (#56)"
bash "$HERE/nopactl_56_live.sh" "$BUILD" || fail=1
if command -v node >/dev/null 2>&1 && node -e "require('playwright')" 2>/dev/null; then
    run "Felület: több kimenet" node "$HERE/gui_multiout_live.js"
    run "Felület: útonkénti EQ" node "$HERE/gui_way_eq_live.js"
    run "Felület: kezelőszervek" node "$HERE/gui_controls_live.js"
    run "Felület: hangkimenet és csatornák" node "$HERE/gui_audio_live.js"
    run "Felület: felmintavételezés" node "$HERE/gui_upsample_live.js"
    run "Felület: hangerő" node "$HERE/gui_volume_live.js"
else
    echo "Playwright nincs telepítve: a felületi tesztek kimaradnak."
fi
echo
[ $fail -eq 0 ] && echo "AZ ÖSSZES ÉLŐ TESZT SIKERES" || echo "VAN SIKERTELEN ÉLŐ TESZT"
exit $fail
