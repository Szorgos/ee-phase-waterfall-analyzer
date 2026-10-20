#!/usr/bin/env bash
# ==============================================================================
# Aqulify – telepítő Linuxra (PipeWire)
#
# Mit csinál:
#   1. telepíti a fordításhoz szükséges csomagokat (sudo jelszót kérhet),
#   2. lefordítja a motort (és ha lehet, a saját ablakos felületet),
#   3. felmásolja a felhasználó saját mappájába (~/.local), root jog nélkül,
#   4. beállítja, hogy a motor bejelentkezéskor automatikusan induljon (systemd felhasználói szolgáltatás),
#   5. menübejegyzést készít ("Aqulify"),
#   6. kérésre az "Aqulify" eszközt teszi meg alapértelmezett hangkimenetnek.
#
# Eltávolítás: ./uninstall.sh
# Támogatott: Ubuntu / Debian / Mint / Pop!_OS, Fedora, Arch / Manjaro / EndeavourOS, openSUSE
# ==============================================================================
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; BLUE='\033[0;34m'; YELLOW='\033[1;33m'; NC='\033[0m'
say()  { echo -e "${YELLOW}$*${NC}"; }
ok()   { echo -e "${GREEN}✓ $*${NC}"; }
warn() { echo -e "${YELLOW}! $*${NC}"; }
die()  { echo -e "${RED}[Hiba] $*${NC}"; exit 1; }

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

PREFIX="${HOME}/.local"
BIN_DIR="${PREFIX}/bin"
SHARE_DIR="${PREFIX}/share/aqulify"
GUI_DIR="${SHARE_DIR}/gui"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
APP_DIR="${PREFIX}/share/applications"
ICON_DIR="${PREFIX}/share/icons/hicolor/256x256/apps"

echo -e "${BLUE}====================================================================${NC}"
echo -e "${GREEN}  Aqulify – telepítés Linuxra${NC}"
echo -e "${BLUE}====================================================================${NC}"

[ "$(id -u)" -eq 0 ] && [ -z "${AQULIFY_ALLOW_ROOT:-}" ] && die "Ne rootként futtasd! A saját felhasználóddal indítsd (a csomagokhoz sudo jelszót fog kérni)."

# ------------------------------------------------------------------ 1. csomagok
say "\n[1/6] Szükséges csomagok telepítése..."
. /etc/os-release 2>/dev/null || die "Nem állapítható meg a disztribúció (/etc/os-release)."
ID_ALL="${ID:-} ${ID_LIKE:-}"
GUI_PKGS_OK=1
case "$ID_ALL" in
    *ubuntu*|*debian*)
        sudo apt-get update
        sudo apt-get install -y build-essential cmake pkg-config libpipewire-0.3-dev pipewire
        sudo apt-get install -y libgtk-3-dev libwebkit2gtk-4.1-dev \
            || sudo apt-get install -y libgtk-3-dev libwebkit2gtk-4.0-dev || GUI_PKGS_OK=0
        ;;
    *fedora*|*rhel*|*centos*)
        sudo dnf install -y gcc-c++ cmake pkgconf pipewire-devel pipewire
        sudo dnf install -y gtk3-devel webkit2gtk4.1-devel || GUI_PKGS_OK=0
        ;;
    *arch*|*manjaro*|*endeavouros*)
        sudo pacman -S --needed --noconfirm base-devel cmake pkgconf pipewire
        sudo pacman -S --needed --noconfirm gtk3 webkit2gtk-4.1 || GUI_PKGS_OK=0
        ;;
    *suse*)
        sudo zypper install -y gcc-c++ cmake pkg-config pipewire-devel pipewire
        sudo zypper install -y gtk3-devel webkit2gtk3-devel || GUI_PKGS_OK=0
        ;;
    *)
        warn "Ismeretlen disztribúció ($ID). Kell: C++20 fordító, cmake, pkg-config, libpipewire-0.3 fejlesztői csomag;"
        warn "a saját ablakhoz: gtk3 és webkit2gtk-4.1 (vagy 4.0) fejlesztői csomag."
        ;;
esac
[ "$GUI_PKGS_OK" = 1 ] || warn "A WebKitGTK csomag nem települt: a felület böngészőben fog megnyílni."

command -v pw-cli >/dev/null 2>&1 || warn "A pw-cli nem található. Biztosan PipeWire a hangrendszered?"

# ------------------------------------------------------------------ 2. fordítás
say "\n[2/6] Fordítás..."
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DBUILD_PIPEWIRE_PLUGIN=ON -DBUILD_LINUX_GUI=ON
cmake --build build -j"$(nproc)" --target aqulify-engine
[ -x build/aqulify-engine ] || die "A motor fordítása sikertelen."
ok "Motor: build/aqulify-engine"
HAVE_GUI=0
if cmake --build build -j"$(nproc)" --target aqulify-gui > build/gui-build.log 2>&1 && [ -x build/aqulify-gui ]; then
    HAVE_GUI=1
    ok "Saját ablakos felület: build/aqulify-gui"
else
    tail -n 15 build/gui-build.log 2>/dev/null || true
    warn "A saját ablakos (WebKitGTK) felület nem fordult le (részletek: build/gui-build.log)."
    warn "Tartalék: Chromium alapú böngésző alkalmazásablaka, ha nincs ilyen, az alapértelmezett böngésző."
    warn "Javítás: telepítsd a gtk3 és webkit2gtk-4.1 fejlesztői csomagot, majd futtasd újra az ./install.sh-t."
fi

# ------------------------------------------------------------------ 3. fájlok
say "\n[3/6] Fájlok másolása (${PREFIX})..."
# Futó motor leállítása frissítéskor
systemctl --user stop aqulify.service >/dev/null 2>&1 || true
# Régi név (AetherAudio, 1.4.x-ig) eltávolítása: szolgáltatás, programfájlok, menü, ikon.
# A beállítások és profilok a ~/.config/aetheraudio mappából az első indításkor átköltöznek.
OLD="aether""audio"
if [ -e "$UNIT_DIR/${OLD}.service" ] || [ -e "$BIN_DIR/${OLD}-engine" ]; then
    say "Korábbi AetherAudio telepítés eltávolítása (a beállítások átkerülnek)..."
    WAS_DEFAULT=0
    pw-metadata 0 default.configured.audio.sink 2>/dev/null | grep -q "\"${OLD}_sink\"" && WAS_DEFAULT=1
    systemctl --user disable --now "${OLD}.service" >/dev/null 2>&1 || true
    rm -f "$UNIT_DIR/${OLD}.service" "$BIN_DIR/${OLD}-engine" "$BIN_DIR/${OLD}-gui"
    rm -rf "${PREFIX}/share/${OLD}"
    rm -f "$APP_DIR/${OLD}.desktop" "$ICON_DIR/${OLD}.png" "${PREFIX}/share/pixmaps/${OLD}.png"
    for d in "$HOME/Asztal" "$HOME/Desktop"; do rm -f "$d/${OLD}.desktop"; done
    systemctl --user daemon-reload >/dev/null 2>&1 || true
fi
PIXMAP_DIR="${PREFIX}/share/pixmaps"
mkdir -p "$BIN_DIR" "$GUI_DIR" "$APP_DIR" "$ICON_DIR" "$UNIT_DIR" "$PIXMAP_DIR"
install -m 755 build/aqulify-engine "$BIN_DIR/aqulify-engine"
install -m 644 gui_integrated_system.html autoeq_index.js "$GUI_DIR/"
install -m 644 windows/third_party/three/three.min.js "$GUI_DIR/three.min.js"
install -m 644 windows/third_party/three/LICENSE "$GUI_DIR/three.js-LICENSE.txt"
install -m 644 linux/aqulify.png "$ICON_DIR/aqulify.png"
install -m 644 linux/aqulify.png "$PIXMAP_DIR/aqulify.png"
if [ "$HAVE_GUI" = 1 ]; then
    install -m 755 build/aqulify-gui "$BIN_DIR/aqulify-gui"
else
    cat > "$BIN_DIR/aqulify-gui" <<EOF
#!/usr/bin/env bash
# Aqulify felület WebKitGTK nélkül (a beépített saját ablakhoz telepítsd a webkit2gtk fejlesztői csomagot,
# és futtasd újra az install.sh-t). Chromium alapú böngésző alkalmazásablaka (--app), végső esetben xdg-open.
URL="file://${GUI_DIR}/gui_integrated_system.html"
for b in chromium chromium-browser google-chrome google-chrome-stable brave-browser microsoft-edge vivaldi; do
    if command -v "\$b" >/dev/null 2>&1; then
        exec "\$b" --app="\$URL" --class=Aqulify --window-size=1480,940
    fi
done
exec xdg-open "\$URL"
EOF
    chmod 755 "$BIN_DIR/aqulify-gui"
fi
ok "Fájlok a helyükön."

# ------------------------------------------------------------------ 4. automatikus indítás
say "\n[4/6] Automatikus indítás beállítása (systemd felhasználói szolgáltatás)..."
cat > "$UNIT_DIR/aqulify.service" <<EOF
[Unit]
Description=Aqulify DSP motor (virtuális "Aqulify" hangkimenet)
After=pipewire.service wireplumber.service
Wants=pipewire.service

[Service]
ExecStart=${BIN_DIR}/aqulify-engine
Restart=on-failure
RestartSec=2

[Install]
WantedBy=default.target
EOF
if systemctl --user daemon-reload 2>/dev/null && systemctl --user enable --now aqulify.service; then
    ok "A motor fut, és bejelentkezéskor magától elindul."
else
    warn "A systemd felhasználói szolgáltatás nem érhető el. Indítsd kézzel: ${BIN_DIR}/aqulify-engine &"
fi

# ------------------------------------------------------------------ 5. menübejegyzés
say "\n[5/6] Menübejegyzés..."
cat > "$APP_DIR/aqulify.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Aqulify
GenericName=Hangkorrekció és akusztikai vízesés
Comment=Parametrikus EQ, konvolver, crossfeed és true peak limiter a PipeWire-hez, élő 3D vízesés-analizátorral
Exec=${BIN_DIR}/aqulify-gui
Icon=${ICON_DIR}/aqulify.png
Terminal=false
Categories=AudioVideo;Audio;
StartupWMClass=Aqulify
EOF
chmod +x "$APP_DIR/aqulify.desktop"

# Asztali ikon létrehozása (ha létezik Asztal vagy Desktop mappa)
for d in "$HOME/Asztal" "$HOME/Desktop"; do
    if [ -d "$d" ]; then
        cp "$APP_DIR/aqulify.desktop" "$d/aqulify.desktop"
        chmod +x "$d/aqulify.desktop"
        gio set "$d/aqulify.desktop" metadata::trusted true >/dev/null 2>&1 || true
    fi
done

update-desktop-database "$APP_DIR" >/dev/null 2>&1 || true
gtk-update-icon-cache -q "${PREFIX}/share/icons/hicolor" >/dev/null 2>&1 || true
kbuildsycoca6 >/dev/null 2>&1 || kbuildsycoca5 >/dev/null 2>&1 || true
# régi (0.x) telepítés maradványa
rm -f "$PROJECT_DIR/run.sh" 2>/dev/null || true
ok "A menüben és az asztalon: Aqulify"

# ------------------------------------------------------------------ 6. alapértelmezett kimenet
say "\n[6/6] Alapértelmezett hangkimenet"
echo "Az alkalmazások hangja akkor megy át az Aqulify-n, ha az \"Aqulify\" a kimeneti eszköz."
if [ "${WAS_DEFAULT:-0}" = 1 ]; then
    ans="i"; echo "A korábbi AetherAudio volt az alapértelmezett kimenet: az Aqulify veszi át."
else
    read -r -p "Beállítsam most alapértelmezettnek? [i/N] " ans || ans=""
fi
if [[ "$ans" =~ ^[iIyY]$ ]]; then
    sleep 1
    if pw-metadata 0 default.configured.audio.sink '{ "name": "aqulify_sink" }' Spa:String:JSON >/dev/null 2>&1; then
        ok "Az \"Aqulify\" lett az alapértelmezett kimenet."
    else
        warn "Nem sikerült; állítsd be a Hangbeállításokban (Kimenet: Aqulify)."
    fi
else
    echo "Később a Hangbeállításokban választhatod ki (Kimenet: Aqulify)."
fi

echo -e "\n${BLUE}====================================================================${NC}"
echo -e "${GREEN}  Kész! Az Aqulify telepítve.${NC}"
echo -e "${BLUE}====================================================================${NC}"
echo "  Felület:        menü → Aqulify  (vagy: aqulify-gui)"
echo "  Motor állapota: systemctl --user status aqulify"
echo "  Napló:          journalctl --user -u aqulify -f"
echo "  Eltávolítás:    ./uninstall.sh"
