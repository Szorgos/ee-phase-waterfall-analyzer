#!/usr/bin/env bash
# Aqulify eltávolítása Linuxról (a mentett beállítások a ~/.config/aqulify mappában maradnak,
# a --purge kapcsolóval azok is törlődnek).
set -u

# >>> i18n (előállítva: tools/i18n_build.py – ne szerkeszd kézzel; források: i18n/scripts/<nyelv>.json)
# Nyelv: AQULIFY_LANG, a felületen választott nyelv (~/.config/aqulify/language.txt), majd a rendszeré
aq_detect_lang() {
    local l="${AQULIFY_LANG:-}" f="${XDG_CONFIG_HOME:-$HOME/.config}/aqulify/language.txt"
    [ -z "$l" ] && [ -r "$f" ] && l="$(head -n 1 "$f" 2>/dev/null || true)"
    [ -z "$l" ] && l="${LC_ALL:-${LC_MESSAGES:-${LANG:-en}}}"
    l="${l%%[_.@-]*}"
    case "$l" in
        hu|en) printf "%s" "$l" ;;
        *) printf "en" ;;
    esac
}
AQ_LANG="$(aq_detect_lang)"
# msg_text NYELV KULCS – a szöveg (printf formátum) az adott nyelven; ha nincs, angolul
msg_text() {
    local f=""
    case "$1" in
    hu) case "$2" in
        common.errorPrefix) f='[Hiba]' ;;
        uninst.purged) f='Az Aqulify és a mentett beállításai eltávolítva.' ;;
        uninst.removed) f='Az Aqulify eltávolítva. (A beállítások megmaradtak: ~/.config/aqulify — törlés: ./uninstall.sh --purge)' ;;
        esac ;;
    esac
    if [ -z "$f" ]; then
        case "$2" in
        common.errorPrefix) f='[Error]' ;;
        uninst.purged) f='Aqulify and its saved settings have been removed.' ;;
        uninst.removed) f='Aqulify has been removed. (The settings were kept: ~/.config/aqulify — to delete them: ./uninstall.sh --purge)' ;;
        esac
    fi
    printf "%s" "$f"
}
# msg KULCS [argumentumok...] – a választott nyelven (%s helyére az argumentumok)
msg() {
    local f
    f="$(msg_text "$AQ_LANG" "$1")"; shift
    # shellcheck disable=SC2059
    printf "$f" "$@"
}
# <<< i18n

PREFIX="${HOME}/.local"
UNIT="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/aqulify.service"

# Ha az Aqulify volt az alapértelmezett kimenet, a WirePlumber magától visszavált a hangkártyára
systemctl --user disable --now aqulify.service >/dev/null 2>&1 || true
rm -f "$UNIT"
systemctl --user daemon-reload >/dev/null 2>&1 || true

rm -f "$PREFIX/bin/aqulify-engine" "$PREFIX/bin/aqulify-gui" "$PREFIX/bin/aqulify-play"
rm -rf "$PREFIX/share/aqulify"
rm -f "$PREFIX/share/applications/aqulify.desktop"
rm -f "$PREFIX/share/icons/hicolor/256x256/apps/aqulify.png"
update-desktop-database "$PREFIX/share/applications" >/dev/null 2>&1 || true

if [ "${1:-}" = "--purge" ]; then
    rm -rf "${XDG_CONFIG_HOME:-$HOME/.config}/aqulify"
    msg uninst.purged; echo
else
    msg uninst.removed; echo
fi
