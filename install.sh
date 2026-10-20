#!/usr/bin/env bash
# ==============================================================================
# AetherAudio – telepítő Linuxra (PipeWire)
#
# Mit csinál:
#   1. telepíti a fordításhoz szükséges csomagokat (sudo jelszót kérhet),
#   2. lefordítja a motort (és ha lehet, a saját ablakos felületet),
#   3. felmásolja a felhasználó saját mappájába (~/.local), root jog nélkül,
#   4. beállítja, hogy a motor bejelentkezéskor automatikusan induljon (systemd felhasználói szolgáltatás),
#   5. menübejegyzést készít ("AetherAudio"),
#   6. kérésre az "AetherAudio" eszközt teszi meg alapértelmezett hangkimenetnek.
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
SHARE_DIR="${PREFIX}/share/aetheraudio"
GUI_DIR="${SHARE_DIR}/gui"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
APP_DIR="${PREFIX}/share/applications"
ICON_DIR="${PREFIX}/share/icons/hicolor/256x256/apps"

echo -e "${BLUE}====================================================================${NC}"
echo -e "${GREEN}  AetherAudio – telepítés Linuxra${NC}"
echo -e "${BLUE}====================================================================${NC}"

[ "$(id -u)" -eq 0 ] && [ -z "${AETHER_ALLOW_ROOT:-}" ] && die "Ne rootként futtasd! A saját felhasználóddal indítsd (a csomagokhoz sudo jelszót fog kérni)."

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
cmake --build build -j"$(nproc)" --target aetheraudio-engine
[ -x build/aetheraudio-engine ] || die "A motor fordítása sikertelen."
ok "Motor: build/aetheraudio-engine"
HAVE_GUI=0
if cmake --build build -j"$(nproc)" --target aetheraudio-gui 2>/dev/null && [ -x build/aetheraudio-gui ]; then
    HAVE_GUI=1
    ok "Saját ablakos felület: build/aetheraudio-gui"
else
    warn "A saját ablakos felület nem fordult le; a menüpont böngészőben nyitja meg a felületet."
fi

# ------------------------------------------------------------------ 3. fájlok
say "\n[3/6] Fájlok másolása (${PREFIX})..."
# Futó motor leállítása frissítéskor
systemctl --user stop aetheraudio.service >/dev/null 2>&1 || true
PIXMAP_DIR="${PREFIX}/share/pixmaps"
mkdir -p "$BIN_DIR" "$GUI_DIR" "$APP_DIR" "$ICON_DIR" "$UNIT_DIR" "$PIXMAP_DIR"
install -m 755 build/aetheraudio-engine "$BIN_DIR/aetheraudio-engine"
install -m 644 gui_integrated_system.html autoeq_index.js "$GUI_DIR/"
install -m 644 windows/third_party/three/three.min.js "$GUI_DIR/three.min.js"
install -m 644 windows/third_party/three/LICENSE "$GUI_DIR/three.js-LICENSE.txt"
install -m 644 linux/aetheraudio.png "$ICON_DIR/aetheraudio.png"
install -m 644 linux/aetheraudio.png "$PIXMAP_DIR/aetheraudio.png"
if [ "$HAVE_GUI" = 1 ]; then
    install -m 755 build/aetheraudio-gui "$BIN_DIR/aetheraudio-gui"
else
    cat > "$BIN_DIR/aetheraudio-gui" <<EOF
#!/usr/bin/env bash
# AetherAudio felület böngészőben (a saját ablakhoz telepítsd a webkit2gtk fejlesztői csomagot, és futtasd újra az install.sh-t)
exec xdg-open "file://${GUI_DIR}/gui_integrated_system.html"
EOF
    chmod 755 "$BIN_DIR/aetheraudio-gui"
fi
ok "Fájlok a helyükön."

# ------------------------------------------------------------------ 4. automatikus indítás
say "\n[4/6] Automatikus indítás beállítása (systemd felhasználói szolgáltatás)..."
cat > "$UNIT_DIR/aetheraudio.service" <<EOF
[Unit]
Description=AetherAudio DSP motor (virtuális "AetherAudio" hangkimenet)
After=pipewire.service wireplumber.service
Wants=pipewire.service

[Service]
ExecStart=${BIN_DIR}/aetheraudio-engine
Restart=on-failure
RestartSec=2

[Install]
WantedBy=default.target
EOF
if systemctl --user daemon-reload 2>/dev/null && systemctl --user enable --now aetheraudio.service; then
    ok "A motor fut, és bejelentkezéskor magától elindul."
else
    warn "A systemd felhasználói szolgáltatás nem érhető el. Indítsd kézzel: ${BIN_DIR}/aetheraudio-engine &"
fi

# ------------------------------------------------------------------ 5. menübejegyzés
say "\n[5/6] Menübejegyzés..."
cat > "$APP_DIR/aetheraudio.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=AetherAudio
GenericName=Hangkorrekció és akusztikai vízesés
Comment=Parametrikus EQ, konvolver, crossfeed és true peak limiter a PipeWire-hez, élő 3D vízesés-analizátorral
Exec=${BIN_DIR}/aetheraudio-gui
Icon=${ICON_DIR}/aetheraudio.png
Terminal=false
Categories=AudioVideo;Audio;
StartupWMClass=AetherAudio
EOF
chmod +x "$APP_DIR/aetheraudio.desktop"

# Asztali ikon létrehozása (ha létezik Asztal vagy Desktop mappa)
for d in "$HOME/Asztal" "$HOME/Desktop"; do
    if [ -d "$d" ]; then
        cp "$APP_DIR/aetheraudio.desktop" "$d/aetheraudio.desktop"
        chmod +x "$d/aetheraudio.desktop"
        gio set "$d/aetheraudio.desktop" metadata::trusted true >/dev/null 2>&1 || true
    fi
done

update-desktop-database "$APP_DIR" >/dev/null 2>&1 || true
gtk-update-icon-cache -q "${PREFIX}/share/icons/hicolor" >/dev/null 2>&1 || true
kbuildsycoca6 >/dev/null 2>&1 || kbuildsycoca5 >/dev/null 2>&1 || true
# régi (0.x) telepítés maradványa
rm -f "$PROJECT_DIR/run.sh" 2>/dev/null || true
ok "A menüben és az asztalon: AetherAudio"

# ------------------------------------------------------------------ 6. alapértelmezett kimenet
say "\n[6/6] Alapértelmezett hangkimenet"
echo "Az alkalmazások hangja akkor megy át az AetherAudio-n, ha az \"AetherAudio\" a kimeneti eszköz."
read -r -p "Beállítsam most alapértelmezettnek? [i/N] " ans || ans=""
if [[ "$ans" =~ ^[iIyY]$ ]]; then
    sleep 1
    if pw-metadata 0 default.configured.audio.sink '{ "name": "aetheraudio_sink" }' Spa:String:JSON >/dev/null 2>&1; then
        ok "Az \"AetherAudio\" lett az alapértelmezett kimenet."
    else
        warn "Nem sikerült; állítsd be a Hangbeállításokban (Kimenet: AetherAudio)."
    fi
else
    echo "Később a Hangbeállításokban választhatod ki (Kimenet: AetherAudio)."
fi

echo -e "\n${BLUE}====================================================================${NC}"
echo -e "${GREEN}  Kész! Az AetherAudio telepítve.${NC}"
echo -e "${BLUE}====================================================================${NC}"
echo "  Felület:        menü → AetherAudio  (vagy: aetheraudio-gui)"
echo "  Motor állapota: systemctl --user status aetheraudio"
echo "  Napló:          journalctl --user -u aetheraudio -f"
echo "  Eltávolítás:    ./uninstall.sh"
