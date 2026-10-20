#!/usr/bin/env bash
# AetherAudio eltávolítása Linuxról (a mentett beállítások a ~/.config/aetheraudio mappában maradnak,
# a --purge kapcsolóval azok is törlődnek).
set -u
PREFIX="${HOME}/.local"
UNIT="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/aetheraudio.service"

# Ha az AetherAudio volt az alapértelmezett kimenet, a WirePlumber magától visszavált a hangkártyára
systemctl --user disable --now aetheraudio.service >/dev/null 2>&1 || true
rm -f "$UNIT"
systemctl --user daemon-reload >/dev/null 2>&1 || true

rm -f "$PREFIX/bin/aetheraudio-engine" "$PREFIX/bin/aetheraudio-gui"
rm -rf "$PREFIX/share/aetheraudio"
rm -f "$PREFIX/share/applications/aetheraudio.desktop"
rm -f "$PREFIX/share/icons/hicolor/256x256/apps/aetheraudio.png"
update-desktop-database "$PREFIX/share/applications" >/dev/null 2>&1 || true

if [ "${1:-}" = "--purge" ]; then
    rm -rf "${XDG_CONFIG_HOME:-$HOME/.config}/aetheraudio"
    echo "Az AetherAudio és a mentett beállításai eltávolítva."
else
    echo "Az AetherAudio eltávolítva. (A beállítások megmaradtak: ~/.config/aetheraudio — törlés: ./uninstall.sh --purge)"
fi
