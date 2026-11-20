#!/bin/sh
# Aqulify headless boot-partíciós visszaállítás (/boot/firmware/aqulify-reset vagy /boot/aqulify-reset).
# Root jogosultsággal fut indításkor (aqulify-reset.service), feloldja a valódi felhasználók
# ~/.config/aqulify/remote.json fájlját (nem hardkódolt /home/pi), kiüríti a párosításokat,
# és hiba esetén is eltávolítja a jelzőfájlt.
set -u

FLAG=""
if [ -n "${AQULIFY_BOOT_DIR:-}" ]; then
    if [ -f "$AQULIFY_BOOT_DIR/aqulify-reset" ]; then
        FLAG="$AQULIFY_BOOT_DIR/aqulify-reset"
    fi
else
    for d in /boot/firmware /boot; do
        if [ -f "$d/aqulify-reset" ]; then
            FLAG="$d/aqulify-reset"
            break
        fi
    done
fi

[ -n "$FLAG" ] || exit 0

ERR=0

reset_user_remote() {
    uid="$1"
    gid="$2"
    home="$3"
    [ -d "$home" ] || return 0
    cfgdir="$home/.config/aqulify"
    cfg="$cfgdir/remote.json"
    # Csak olyan felhasználót érintünk, akinél már van Aqulify config mappa vagy valódi bejelentkező user
    if [ ! -d "$cfgdir" ]; then
        mkdir -p "$cfgdir" 2>/dev/null || { echo "[aqulify-reset] Hiba: nem hozható létre: $cfgdir" >&2; ERR=1; return 1; }
        chown "$uid:$gid" "$home/.config" "$cfgdir" 2>/dev/null || true
    fi
    tmp="$cfg.reset.tmp"
    newdoc='{"enabled":true,"bluetooth":true,"port":47820,"devices":[]}'
    if [ -f "$cfg" ] && command -v python3 >/dev/null 2>&1; then
        parsed=$(python3 - "$cfg" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1], encoding="utf-8"))
    out = {
        "enabled": True,
        "bluetooth": bool(d.get("bluetooth", True)),
        "port": int(d.get("port", 47820)) if 1024 <= int(d.get("port", 47820)) <= 65500 else 47820,
        "devices": [],
    }
    if isinstance(d.get("machine"), str) and d["machine"]:
        out["machine"] = d["machine"]
    print(json.dumps(out, separators=(",", ":")))
except Exception:
    sys.exit(1)
PY
        ) && [ -n "$parsed" ] && newdoc="$parsed"
    fi
    if printf '%s' "$newdoc" > "$tmp" 2>/dev/null; then
        chmod 600 "$tmp" 2>/dev/null || true
        chown "$uid:$gid" "$tmp" 2>/dev/null || true
        if mv -f "$tmp" "$cfg" 2>/dev/null; then
            echo "[aqulify-reset] Párosítások törölve: $cfg"
        else
            rm -f "$tmp" 2>/dev/null || true
            echo "[aqulify-reset] Hiba: nem cserélhető: $cfg" >&2
            ERR=1
        fi
    else
        echo "[aqulify-reset] Hiba: nem írható: $tmp" >&2
        ERR=1
    fi
}

if [ -n "${AQULIFY_TEST_HOMES:-}" ]; then
    # Tesztmód: kettősponttal elválasztott home mappák
    OLD_IFS="$IFS"
    IFS=":"
    for h in $AQULIFY_TEST_HOMES; do
        reset_user_remote "$(id -u)" "$(id -g)" "$h"
    done
    IFS="$OLD_IFS"
else
    # Valódi rendszerfelhasználók feloldása getent passwd-ből (nem hardkódolt /home/pi)
    getent passwd | while IFS=: read -r _user _pw uid gid _gecos home shell; do
        [ -n "$uid" ] || continue
        case "$uid" in *[!0-9]*) continue ;; esac
        [ "$uid" -ge 1000 ] && [ "$uid" -lt 65534 ] || continue
        case "$shell" in */nologin|*/false) [ -f "$home/.config/aqulify/remote.json" ] || continue ;; esac
        reset_user_remote "$uid" "$gid" "$home"
    done
fi

# A reset fájlt akkor is eltávolítjuk, ha a párosítások törlése hibázott, nehogy minden indításkor újrafusson
if ! rm -f "$FLAG" 2>/dev/null; then
    echo "[aqulify-reset] HIBA: az aqulify-reset fájl nem törölhető (valószínűleg írásvédett boot partíció): $FLAG" >&2
    exit 1
fi
echo "[aqulify-reset] Jelzőfájl eltávolítva: $FLAG"
exit "$ERR"
