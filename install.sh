#!/usr/bin/env bash
# ==============================================================================
# EasyEffects Valós Idejű Fázis- és Akusztikai Vízesés Analizátor
# Univerzális Linux Telepítő és Build Szkript
# Támogatott disztribúciók: Ubuntu, Debian, Fedora, Arch Linux, Manjaro, openSUSE
# ==============================================================================

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${BLUE}====================================================================${NC}"
echo -e "${GREEN}  EasyEffects Phase & Acoustic Waterfall Analyzer Telepítő${NC}"
echo -e "${BLUE}====================================================================${NC}"

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

# 1. Linux Disztribúció Felismerése
echo -e "\n${YELLOW}[1/5] Linux disztribúció és csomagkezelő detektálása...${NC}"

if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS=$ID
    LIKE=$ID_LIKE
else
    echo -e "${RED}[Hiba] Nem sikerült meghatározni a Linux disztribúciót!${NC}"
    exit 1
fi

echo -e "Észlelt rendszer: ${GREEN}${NAME} (${OS})${NC}"

# 2. Szükséges Csomagok Telepítése
echo -e "\n${YELLOW}[2/5] Rendszerfüggőségek telepítése (C++20, CMake, PipeWire)...${NC}"

install_dependencies() {
    case "$OS" in
        ubuntu|debian|linuxmint|pop)
            echo "Ubuntu/Debian alapú rendszer észlelve, apt használata..."
            sudo apt-get update
            sudo apt-get install -y build-essential cmake libpipewire-0.3-dev pkg-config pipewire pipewire-bin
            ;;
        fedora|rhel|centos)
            echo "Fedora/RHEL alapú rendszer észlelve, dnf használata..."
            sudo dnf install -y gcc-c++ cmake pipewire-devel pkgconf pipewire
            ;;
        arch|manjaro|endeavouros)
            echo "Arch Linux alapú rendszer észlelve, pacman használata..."
            sudo pacman -S --needed --noconfirm base-devel cmake pipewire pkgconf
            ;;
        opensuse*|suse)
            echo "openSUSE alapú rendszer észlelve, zypper használata..."
            sudo zypper install -y gcc-c++ cmake pipewire-devel pkg-config pipewire
            ;;
        *)
            if [[ "$LIKE" == *"debian"* || "$LIKE" == *"ubuntu"* ]]; then
                sudo apt-get update && sudo apt-get install -y build-essential cmake libpipewire-0.3-dev pkg-config pipewire
            elif [[ "$LIKE" == *"arch"* ]]; then
                sudo pacman -S --needed --noconfirm base-devel cmake pipewire pkgconf
            elif [[ "$LIKE" == *"fedora"* ]]; then
                sudo dnf install -y gcc-c++ cmake pipewire-devel pkgconf pipewire
            else
                echo -e "${YELLOW}[Figyelem] Ismeretlen disztribúció. Kérlek győződj meg róla, hogy a libpipewire-0.3 és cmake telepítve van!${NC}"
            fi
            ;;
    esac
}

install_dependencies

# 3. C++ Motor Fordítása CMake-kel
echo -e "\n${YELLOW}[3/5] Natív C++ DSP és PipeWire motor fordítása...${NC}"
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DBUILD_PIPEWIRE_PLUGIN=ON
cmake --build build -j"$(nproc)"

if [ -f "build/pipewire_plugin" ]; then
    echo -e "${GREEN}✓ C++ bináris sikeresen lefordult: build/pipewire_plugin${NC}"
else
    echo -e "${RED}[Hiba] A fordítás sikertelen volt!${NC}"
    exit 1
fi

# 4. Kényelmi Indítószkript (run.sh) Létrehozása
echo -e "\n${YELLOW}[4/5] Kényelmi futtató környezet előkészítése...${NC}"
cat << 'EOF' > run.sh
#!/usr/bin/env bash
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

echo "=== EasyEffects Phase & Waterfall Analyzer Indítása ==="

# Háttér webszerver indítása a GUI-hoz (ha szükséges)
if command -v python3 &>/dev/null; then
    python3 -m http.server 3000 --directory "$PROJECT_DIR" &>/dev/null &
    HTTP_PID=$!
    echo "[OK] Helyi HTTP szerver elindítva (PID: $HTTP_PID)"
fi

# Böngésző megnyitása a grafikus felülettel
URL="http://localhost:3000/gui_integrated_system.html"
if command -v xdg-open &>/dev/null; then
    xdg-open "$URL" &>/dev/null &
elif command -v sensible-browser &>/dev/null; then
    sensible-browser "$URL" &>/dev/null &
fi

# PipeWire C++ DSP motor futtatása az előtérben
echo "[OK] PipeWire DSP szűrő csatlakoztatása..."
./build/pipewire_plugin

# Kilépéskor leállítjuk a webszervert is
if [ -n "$HTTP_PID" ]; then
    kill "$HTTP_PID" 2>/dev/null || true
fi
EOF
chmod +x run.sh

# 5. Linux Asztali Ikon és Alkalmazásmenü (.desktop) Regisztráció
echo -e "\n${YELLOW}[5/5] Linux asztali menü bejegyzés létrehozása...${NC}"
DESKTOP_DIR="${HOME}/.local/share/applications"
mkdir -p "$DESKTOP_DIR"

cat << EOF > "${DESKTOP_DIR}/ee-phase-waterfall.desktop"
[Desktop Entry]
Name=EasyEffects Phase & Waterfall Analyzer
Comment=Real-time Phase, Group Delay & Acoustic Waterfall DSP Analyzer
Exec=${PROJECT_DIR}/run.sh
Icon=audio-card
Terminal=true
Type=Application
Categories=AudioVideo;Audio;AudioVideoEditing;
EOF

chmod +x "${DESKTOP_DIR}/ee-phase-waterfall.desktop"

echo -e "\n${BLUE}====================================================================${NC}"
echo -e "${GREEN}  SIKERES TELEPÍTÉS! A rendszer készen áll a használatra.${NC}"
echo -e "${BLUE}====================================================================${NC}"
echo -e "Indítás a terminálból:"
echo -e "  ${YELLOW}./run.sh${NC}"
echo -e "\nVagy keresd a Linux alkalmazásmenüdben:"
echo -e "  ${GREEN}EasyEffects Phase & Waterfall Analyzer${NC}\n"
