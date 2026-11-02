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
run "Motor: DSP-modulok" python3 -I "$HERE/modules_live.py"
run "Motor: haladó hangkimenet" python3 -I "$HERE/audio_opts_live.py"
if command -v node >/dev/null 2>&1 && node -e "require('playwright')" 2>/dev/null; then
    run "Felület: több kimenet" node "$HERE/gui_multiout_live.js"
    run "Felület: kezelőszervek" node "$HERE/gui_controls_live.js"
    run "Felület: hangkimenet és csatornák" node "$HERE/gui_audio_live.js"
    run "Felület: hangerő" node "$HERE/gui_volume_live.js"
else
    echo "Playwright nincs telepítve: a felületi tesztek kimaradnak."
fi
echo
[ $fail -eq 0 ] && echo "AZ ÖSSZES ÉLŐ TESZT SIKERES" || echo "VAN SIKERTELEN ÉLŐ TESZT"
exit $fail
