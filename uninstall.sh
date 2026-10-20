#!/usr/bin/env bash
# Aqulify eltávolítása Linuxról (a mentett beállítások a ~/.config/aqulify mappában maradnak,
# a --purge kapcsolóval azok is törlődnek).
set -u
PREFIX="${HOME}/.local"
UNIT="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/aqulify.service"

# Ha az Aqulify volt az alapértelmezett kimenet, a WirePlumber magától visszavált a hangkártyára
systemctl --user disable --now aqulify.service >/dev/null 2>&1 || true
rm -f "$UNIT"
systemctl --user daemon-reload >/dev/null 2>&1 || true

rm -f "$PREFIX/bin/aqulify-engine" "$PREFIX/bin/aqulify-gui"
rm -rf "$PREFIX/share/aqulify"
rm -f "$PREFIX/share/applications/aqulify.desktop"
rm -f "$PREFIX/share/icons/hicolor/256x256/apps/aqulify.png"
update-desktop-database "$PREFIX/share/applications" >/dev/null 2>&1 || true

if [ "${1:-}" = "--purge" ]; then
    rm -rf "${XDG_CONFIG_HOME:-$HOME/.config}/aqulify"
    echo "Az Aqulify és a mentett beállításai eltávolítva."
else
    echo "Az Aqulify eltávolítva. (A beállítások megmaradtak: ~/.config/aqulify — törlés: ./uninstall.sh --purge)"
fi
