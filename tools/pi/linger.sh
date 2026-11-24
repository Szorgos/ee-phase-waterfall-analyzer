#!/bin/sh
# Bejelentkezés nélküli futás (systemd „lingering”) a Pi-csomaghoz.
#
# Az aqulify.service felhasználói (--user) szolgáltatás. Képernyő nélküli Pi-n nincs bejelentkezett munkamenet, ezért
# lingering nélkül a systemd az utolsó SSH-munkamenet bezárásakor leállítja a felhasználó szolgáltatásait (a PipeWire-t
# és az Aqulify-t is), és a gép a következő bejelentkezésig elérhetetlen (felhasználói visszajelzés, 2026-11-24).
#
# A csomag telepítésekor (postinst configure) bekapcsolja annak a felhasználónak, aki telepít (sudo apt …: SUDO_USER),
# vagy ha ez nem ismert, az 1000-es azonosítójú (a Pi OS első) felhasználónak. Csak akkor nyúl hozzá, ha még ki volt
# kapcsolva, és ezt megjegyzi: eltávolításkor (prerm remove) csak azt kapcsolja vissza, amit maga kapcsolt be.
#
# Használat (a csomag szkriptjeiből, rootként):  linger.sh enable | disable
# Teszteléshez: AQ_SYSROOT=<mappa> (a /var/lib alá ír), AQ_LOGINCTL=<parancs> (a loginctl helyett), AQ_UID1000=<név>.

set -e
ROOT="${AQ_SYSROOT:-}"
ROOT="${ROOT%/}"
LINGER_DIR="$ROOT/var/lib/systemd/linger"
MARK="$ROOT/var/lib/aqulify/linger-user"
LOGINCTL="${AQ_LOGINCTL:-loginctl}"

pick_user() {
    u="${SUDO_USER:-}"
    if [ -z "$u" ] || [ "$u" = "root" ]; then
        if [ -n "${AQ_UID1000+x}" ]; then
            u="$AQ_UID1000"
        else
            u="$(getent passwd 1000 2>/dev/null | cut -d: -f1)"
        fi
    fi
    [ "$u" = "root" ] && u=""
    printf '%s' "$u"
}

cmd_enable() {
    user="$(pick_user)"
    if [ -z "$user" ]; then
        echo "Aqulify: nem található felhasználó a bejelentkezés nélküli futáshoz; kézzel: sudo loginctl enable-linger <név>"
        return 0
    fi
    if [ -e "$LINGER_DIR/$user" ]; then
        return 0   # már be van kapcsolva (akár a felhasználó kapcsolta be): nem jegyezzük meg, nem vonjuk vissza
    fi
    # a loginctl azonnal elindítja a felhasználói példányt is; systemd nélkül (pl. chroot) a fájl ugyanazt jelenti
    if ! "$LOGINCTL" enable-linger "$user" >/dev/null 2>&1; then
        mkdir -p "$LINGER_DIR" && : > "$LINGER_DIR/$user"
    fi
    mkdir -p "$(dirname "$MARK")"
    printf '%s\n' "$user" > "$MARK"
    echo "Aqulify: a(z) $user felhasználó szolgáltatásai mostantól bejelentkezés nélkül is futnak (loginctl enable-linger)."
}

cmd_disable() {
    [ -f "$MARK" ] || return 0
    user="$(head -n 1 "$MARK")"
    if [ -n "$user" ]; then
        if ! "$LOGINCTL" disable-linger "$user" >/dev/null 2>&1; then
            rm -f "$LINGER_DIR/$user"
        fi
    fi
    rm -f "$MARK"
    rmdir "$(dirname "$MARK")" 2>/dev/null || true
}

case "${1:-}" in
    enable) cmd_enable ;;
    disable) cmd_disable ;;
    *) echo "Használat: linger.sh enable|disable" >&2; exit 1 ;;
esac
exit 0
