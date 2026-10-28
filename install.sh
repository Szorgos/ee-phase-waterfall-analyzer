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
        inst.title) f='Aqulify – telepítés Linuxra' ;;
        inst.noRoot) f='Ne rootként futtasd! A saját felhasználóddal indítsd (a csomagokhoz sudo jelszót fog kérni).' ;;
        inst.step1) f='[1/6] Szükséges csomagok telepítése...' ;;
        inst.noOsRelease) f='Nem állapítható meg a disztribúció (/etc/os-release).' ;;
        inst.unknownDistro1) f='Ismeretlen disztribúció (%s). Kell: C/C++20 fordító, cmake, pkg-config, libpipewire-0.3 és fftw3 (float) fejlesztői csomag;' ;;
        inst.unknownDistro2) f='a saját ablakhoz: gtk3 és webkit2gtk-4.1 (vagy 4.0) fejlesztői csomag.' ;;
        inst.noWebkit) f='A WebKitGTK csomag nem települt: a felület böngészőben fog megnyílni.' ;;
        inst.noPwCli) f='A pw-cli nem található. Biztosan PipeWire a hangrendszered?' ;;
        inst.systemPkg) f='Csomagból telepített Aqulify is van a gépen (/usr/bin). Ez a telepítés (~/.local) mostantól elsőbbséget kap; ha a csomagost használnád, utána futtasd az ./uninstall.sh-t.' ;;
        inst.step2) f='[2/6] Fordítás...' ;;
        inst.rnnSystem) f='RNNoise (beszéd zajcsökkentés): a rendszer csomagja.' ;;
        inst.rnnDownload) f='RNNoise modell letöltése (Ubuntu archívum, kb. 19 MB)...' ;;
        inst.rnnOk) f='RNNoise (beszéd zajcsökkentés): beépítve.' ;;
        inst.rnnFail) f='Az RNNoise modell nem tölthető le: a beszéd zajcsökkentése a saját, sávonkénti módszerrel működik.' ;;
        inst.engineFail) f='A motor fordítása sikertelen.' ;;
        inst.engineOk) f='Motor: build/aqulify-engine' ;;
        inst.guiOk) f='Saját ablakos felület: build/aqulify-gui' ;;
        inst.guiFail1) f='A saját ablakos (WebKitGTK) felület nem fordult le (részletek: build/gui-build.log).' ;;
        inst.guiFail2) f='Tartalék: Chromium alapú böngésző alkalmazásablaka, ha nincs ilyen, az alapértelmezett böngésző.' ;;
        inst.guiFail3) f='Javítás: telepítsd a gtk3 és webkit2gtk-4.1 fejlesztői csomagot, majd futtasd újra az ./install.sh-t.' ;;
        inst.step3) f='[3/6] Fájlok másolása (%s)...' ;;
        inst.legacy) f='Korábbi AetherAudio telepítés eltávolítása (a beállítások átkerülnek)...' ;;
        inst.filesOk) f='Fájlok a helyükön.' ;;
        inst.step4) f='[4/6] Automatikus indítás beállítása (systemd felhasználói szolgáltatás)...' ;;
        inst.unitDesc) f='Aqulify DSP motor (virtuális "Aqulify" hangkimenet)' ;;
        inst.serviceOk) f='A motor fut, és bejelentkezéskor magától elindul.' ;;
        inst.serviceFail) f='A systemd felhasználói szolgáltatás nem érhető el. Indítsd kézzel: %s &' ;;
        inst.step5) f='[5/6] Menübejegyzés...' ;;
        inst.desktopGeneric) f='Hangkorrekció és akusztikai vízesés' ;;
        inst.desktopComment) f='Parametrikus EQ, konvolver, crossfeed és true peak limiter a PipeWire-hez, élő 3D vízesés-analizátorral' ;;
        inst.menuOk) f='A menüben és az asztalon: Aqulify' ;;
        inst.step6) f='[6/6] Alapértelmezett hangkimenet' ;;
        inst.defaultExplain) f='Az alkalmazások hangja akkor megy át az Aqulify-n, ha az "Aqulify" a kimeneti eszköz.' ;;
        inst.legacyDefault) f='A korábbi AetherAudio volt az alapértelmezett kimenet: az Aqulify veszi át.' ;;
        inst.askDefault) f='Beállítsam most alapértelmezettnek? [i/N] ' ;;
        inst.defaultOk) f='Az "Aqulify" lett az alapértelmezett kimenet.' ;;
        inst.defaultFail) f='Nem sikerült; állítsd be a Hangbeállításokban (Kimenet: Aqulify).' ;;
        inst.defaultLater) f='Később a Hangbeállításokban választhatod ki (Kimenet: Aqulify).' ;;
        inst.done) f='Kész! Az Aqulify telepítve.' ;;
        inst.sumGui) f='Felület:        menü → Aqulify  (vagy: aqulify-gui)' ;;
        inst.sumState) f='Motor állapota: systemctl --user status aqulify' ;;
        inst.sumLog) f='Napló:          journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='Eltávolítás:    ./uninstall.sh' ;;
        inst.sumHeadlessWeb) f='Web felület:     http://%s:47820  (vagy Aqulify Remote mobilapp)' ;;
        inst.sumHeadlessPin) f='Párosítási PIN:   %s/aqulify-engine --pair-pin' ;;
        inst.sumHeadlessLinger) f='Linger (folyamatos futás kijelentkezéskor is): bekapcsolva (loginctl).' ;;
        esac ;;
    esac
    if [ -z "$f" ]; then
        case "$2" in
        common.errorPrefix) f='[Error]' ;;
        inst.title) f='Aqulify – installation on Linux' ;;
        inst.noRoot) f='Do not run it as root! Start it as your own user (it will ask for the sudo password for the packages).' ;;
        inst.step1) f='[1/6] Installing required packages...' ;;
        inst.noOsRelease) f='Cannot determine the distribution (/etc/os-release).' ;;
        inst.unknownDistro1) f='Unknown distribution (%s). Required: C/C++20 compiler, cmake, pkg-config, libpipewire-0.3 and fftw3 (float) development packages;' ;;
        inst.unknownDistro2) f='for the app window: gtk3 and webkit2gtk-4.1 (or 4.0) development packages.' ;;
        inst.noWebkit) f='The WebKitGTK package was not installed: the interface will open in a browser.' ;;
        inst.noPwCli) f='pw-cli was not found. Is your sound system really PipeWire?' ;;
        inst.systemPkg) f='A packaged Aqulify is also installed (/usr/bin). This installation (~/.local) takes precedence from now on; to use the packaged one instead, run ./uninstall.sh afterwards.' ;;
        inst.step2) f='[2/6] Building...' ;;
        inst.rnnSystem) f='RNNoise (speech noise reduction): system package.' ;;
        inst.rnnDownload) f='Downloading the RNNoise model (Ubuntu archive, about 19 MB)...' ;;
        inst.rnnOk) f='RNNoise (speech noise reduction): built in.' ;;
        inst.rnnFail) f='The RNNoise model could not be downloaded: speech noise reduction uses the built-in band-wise method.' ;;
        inst.engineFail) f='Building the engine failed.' ;;
        inst.engineOk) f='Engine: build/aqulify-engine' ;;
        inst.guiOk) f='App window interface: build/aqulify-gui' ;;
        inst.guiFail1) f='The app window (WebKitGTK) interface did not build (details: build/gui-build.log).' ;;
        inst.guiFail2) f='Fallback: the app window of a Chromium-based browser, or if there is none, the default browser.' ;;
        inst.guiFail3) f='Fix: install the gtk3 and webkit2gtk-4.1 development packages, then run ./install.sh again.' ;;
        inst.step3) f='[3/6] Copying files (%s)...' ;;
        inst.legacy) f='Removing the previous AetherAudio installation (the settings are carried over)...' ;;
        inst.filesOk) f='Files in place.' ;;
        inst.step4) f='[4/6] Setting up automatic start (systemd user service)...' ;;
        inst.unitDesc) f='Aqulify DSP engine (virtual "Aqulify" audio output)' ;;
        inst.serviceOk) f='The engine is running and starts automatically at login.' ;;
        inst.serviceFail) f='The systemd user service is not available. Start it manually: %s &' ;;
        inst.step5) f='[5/6] Menu entry...' ;;
        inst.desktopGeneric) f='Equalizer and acoustic waterfall analyzer' ;;
        inst.desktopComment) f='Parametric EQ, convolver, crossfeed and true peak limiter for PipeWire with a live 3D waterfall analyzer' ;;
        inst.menuOk) f='In the menu and on the desktop: Aqulify' ;;
        inst.step6) f='[6/6] Default audio output' ;;
        inst.defaultExplain) f='Application audio goes through Aqulify when "Aqulify" is the output device.' ;;
        inst.legacyDefault) f='The previous AetherAudio was the default output: Aqulify takes over.' ;;
        inst.askDefault) f='Set it as the default now? [y/N] ' ;;
        inst.defaultOk) f='"Aqulify" is now the default output.' ;;
        inst.defaultFail) f='That did not work; set it in the Sound settings (Output: Aqulify).' ;;
        inst.defaultLater) f='You can select it later in the Sound settings (Output: Aqulify).' ;;
        inst.done) f='Done! Aqulify is installed.' ;;
        inst.sumGui) f='Interface:      menu → Aqulify  (or: aqulify-gui)' ;;
        inst.sumState) f='Engine status:  systemctl --user status aqulify' ;;
        inst.sumLog) f='Log:            journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='Uninstall:      ./uninstall.sh' ;;
        inst.sumHeadlessWeb) f='Web interface:   http://%s:47820  (or Aqulify Remote mobile app)' ;;
        inst.sumHeadlessPin) f='Pairing PIN:     %s/aqulify-engine --pair-pin' ;;
        inst.sumHeadlessLinger) f='Linger (background service without login): enabled (loginctl).' ;;
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
# desktop_l10n MEZŐ KULCS – a .desktop fájl fordított sorai (MEZŐ[nyelv]=...)
desktop_l10n() {
    local l
    for l in hu; do
        printf "%s[%s]=%s\n" "$1" "$l" "$(msg_text "$l" "$2")"
    done
}
# <<< i18n

RED='\033[0;31m'; GREEN='\033[0;32m'; BLUE='\033[0;34m'; YELLOW='\033[1;33m'; NC='\033[0m'
say()  { echo -e "${YELLOW}$*${NC}"; }
ok()   { echo -e "${GREEN}✓ $*${NC}"; }
warn() { echo -e "${YELLOW}! $*${NC}"; }
die()  { echo -e "${RED}$(msg common.errorPrefix) $*${NC}"; exit 1; }

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
echo -e "${GREEN}  $(msg inst.title)${NC}"
echo -e "${BLUE}====================================================================${NC}"

[ "$(id -u)" -eq 0 ] && [ -z "${AQULIFY_ALLOW_ROOT:-}" ] && die "$(msg inst.noRoot)"

HEADLESS=0
for arg in "$@"; do
    case "$arg" in
        --headless) HEADLESS=1 ;;
    esac
done

# ------------------------------------------------------------------ 1. csomagok
say "\n$(msg inst.step1)"
. /etc/os-release 2>/dev/null || die "$(msg inst.noOsRelease)"
ID_ALL="${ID:-} ${ID_LIKE:-}"
GUI_PKGS_OK=1
case "$ID_ALL" in
    *ubuntu*|*debian*)
        sudo apt-get update
        sudo apt-get install -y build-essential cmake pkg-config libpipewire-0.3-dev libfftw3-dev pipewire
        # RNNoise (a Magic Touch beszéd zajcsökkentése): újabb kiadásokban csomagként is van
        sudo apt-get install -y librnnoise-dev 2>/dev/null || true
        if [ "$HEADLESS" -eq 0 ]; then
            sudo apt-get install -y libgtk-3-dev libwebkit2gtk-4.1-dev \
                || sudo apt-get install -y libgtk-3-dev libwebkit2gtk-4.0-dev || GUI_PKGS_OK=0
        else
            GUI_PKGS_OK=0
        fi
        ;;
    *fedora*|*rhel*|*centos*)
        sudo dnf install -y gcc gcc-c++ cmake pkgconf pipewire-devel fftw-devel pipewire
        if [ "$HEADLESS" -eq 0 ]; then
            sudo dnf install -y gtk3-devel webkit2gtk4.1-devel || GUI_PKGS_OK=0
        else
            GUI_PKGS_OK=0
        fi
        ;;
    *arch*|*manjaro*|*endeavouros*)
        sudo pacman -S --needed --noconfirm base-devel cmake pkgconf pipewire fftw
        if [ "$HEADLESS" -eq 0 ]; then
            sudo pacman -S --needed --noconfirm gtk3 webkit2gtk-4.1 || GUI_PKGS_OK=0
        else
            GUI_PKGS_OK=0
        fi
        ;;
    *suse*)
        sudo zypper install -y gcc gcc-c++ cmake pkg-config pipewire-devel fftw3-devel pipewire
        if [ "$HEADLESS" -eq 0 ]; then
            sudo zypper install -y gtk3-devel webkit2gtk3-devel || GUI_PKGS_OK=0
        else
            GUI_PKGS_OK=0
        fi
        ;;
    *)
        warn "$(msg inst.unknownDistro1 "${ID:-?}")"
        [ "$HEADLESS" -eq 0 ] && warn "$(msg inst.unknownDistro2)"
        ;;
esac
[ "$GUI_PKGS_OK" = 1 ] || [ "$HEADLESS" -eq 1 ] || warn "$(msg inst.noWebkit)"

command -v pw-cli >/dev/null 2>&1 || warn "$(msg inst.noPwCli)"
# Két telepítés egyszerre (T28): a ~/.config/systemd/user szolgáltatás felülírja a csomagét, így a ~/.local motor indul
if [ -x /usr/bin/aqulify-engine ]; then warn "$(msg inst.systemPkg)"; fi

# ------------------------------------------------------------------ 2. fordítás
say "\n$(msg inst.step2)"
# RNNoise: a rendszer csomagja, ha van; különben a betanított modell az Ubuntu forráscsomagjából (ellenőrzött sha256)
RNN_OPT=""
if pkg-config --exists rnnoise 2>/dev/null; then
    ok "$(msg inst.rnnSystem)"
else
    RNN_URL="http://archive.ubuntu.com/ubuntu/pool/universe/r/rnnoise/rnnoise_0.2+ds.orig.tar.xz"
    RNN_SHA="db2718ee4a0b294bfbbe198fe94aabfdc8cd0d77cb9cd95d76c7a818ad805f0b"
    RNN_DIR="$PROJECT_DIR/build/rnnoise-model"
    if [ ! -f "$RNN_DIR/rnnoise_data.c" ]; then
        say "$(msg inst.rnnDownload)"
        mkdir -p "$RNN_DIR"
        RNN_TAR="$RNN_DIR/rnnoise.orig.tar.xz"
        { curl -fsSL -o "$RNN_TAR" "$RNN_URL" 2>/dev/null || wget -q -O "$RNN_TAR" "$RNN_URL" 2>/dev/null; } &&
            echo "$RNN_SHA  $RNN_TAR" | sha256sum -c --status &&
            tar -xJf "$RNN_TAR" -C "$RNN_DIR" --strip-components=2 rnnoise-0.2/src/rnnoise_data.c rnnoise-0.2/src/rnnoise_data.h 2>/dev/null
        rm -f "$RNN_TAR"
    fi
    if [ -f "$RNN_DIR/rnnoise_data.c" ] && [ -f "$RNN_DIR/rnnoise_data.h" ]; then
        RNN_OPT="-DAQULIFY_RNNOISE_MODEL_DIR=$RNN_DIR"
        ok "$(msg inst.rnnOk)"
    else
        warn "$(msg inst.rnnFail)"
    fi
fi
GUI_CMAKE_OPT="-DBUILD_LINUX_GUI=ON"
[ "$HEADLESS" -eq 1 ] && GUI_CMAKE_OPT="-DBUILD_LINUX_GUI=OFF"
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DBUILD_PIPEWIRE_PLUGIN=ON $GUI_CMAKE_OPT $RNN_OPT
cmake --build build -j"$(nproc)" --target aqulify-engine aqulify-play
[ -x build/aqulify-engine ] || die "$(msg inst.engineFail)"
ok "$(msg inst.engineOk)"
HAVE_GUI=0
if [ "$HEADLESS" -eq 0 ]; then
    if cmake --build build -j"$(nproc)" --target aqulify-gui > build/gui-build.log 2>&1 && [ -x build/aqulify-gui ]; then
        HAVE_GUI=1
        ok "$(msg inst.guiOk)"
    else
        tail -n 15 build/gui-build.log 2>/dev/null || true
        warn "$(msg inst.guiFail1)"
        warn "$(msg inst.guiFail2)"
        warn "$(msg inst.guiFail3)"
    fi
fi

# ------------------------------------------------------------------ 3. fájlok
say "\n$(msg inst.step3 "${PREFIX}")"
# Futó motor leállítása frissítéskor
systemctl --user stop aqulify.service >/dev/null 2>&1 || true
# Régi név (AetherAudio, 1.4.x-ig) eltávolítása: szolgáltatás, programfájlok, menü, ikon.
# A beállítások és profilok a ~/.config/aetheraudio mappából az első indításkor átköltöznek.
OLD="aether""audio"
if [ -e "$UNIT_DIR/${OLD}.service" ] || [ -e "$BIN_DIR/${OLD}-engine" ]; then
    say "$(msg inst.legacy)"
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
# 2.x: a beépített lejátszó parancssori indítója (aqulify-play <fájl>)
if [ -x build/aqulify-play ]; then install -m 755 build/aqulify-play "$BIN_DIR/aqulify-play"; fi
install -m 644 gui_integrated_system.html autoeq_index.js gui_i18n.js gui_player.js "$GUI_DIR/"
install -m 644 windows/third_party/three/three.min.js "$GUI_DIR/three.min.js"
install -m 644 windows/third_party/three/LICENSE "$GUI_DIR/three.js-LICENSE.txt"
install -m 644 linux/aqulify.png "$ICON_DIR/aqulify.png"
install -m 644 linux/aqulify.png "$PIXMAP_DIR/aqulify.png"
if [ "$HAVE_GUI" = 1 ]; then
    install -m 755 build/aqulify-gui "$BIN_DIR/aqulify-gui"
elif [ "$HEADLESS" -eq 1 ]; then
    cat > "$BIN_DIR/aqulify-gui" <<EOF
#!/usr/bin/env bash
# Aqulify Headless környezet
echo "Az Aqulify Headless üzemmódban fut."
echo "Nyisd meg böngészőben: http://localhost:47820 (vagy a gép IP-címén: http://\$(hostname -I 2>/dev/null | awk '{print \$1}'):47820)"
echo "Vagy csatlakozz az Aqulify Remote mobilalkalmazással!"
EOF
    chmod 755 "$BIN_DIR/aqulify-gui"
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
ok "$(msg inst.filesOk)"

# ------------------------------------------------------------------ 4. automatikus indítás
say "\n$(msg inst.step4)"
if [ "$HEADLESS" -eq 1 ]; then
    EXEC_CMD="${BIN_DIR}/aqulify-engine --headless --listen lan"
else
    EXEC_CMD="${BIN_DIR}/aqulify-engine"
fi
cat > "$UNIT_DIR/aqulify.service" <<EOF
[Unit]
Description=$(msg inst.unitDesc)
After=pipewire.service wireplumber.service
Wants=pipewire.service

[Service]
ExecStart=${EXEC_CMD}
Restart=on-failure
RestartSec=2

[Install]
WantedBy=default.target
EOF
if systemctl --user daemon-reload 2>/dev/null && systemctl --user enable --now aqulify.service; then
    ok "$(msg inst.serviceOk)"
    if [ "$HEADLESS" -eq 1 ] && command -v loginctl >/dev/null 2>&1; then
        loginctl enable-linger "$USER" 2>/dev/null && ok "$(msg inst.sumHeadlessLinger)" || true
    fi
else
    warn "$(msg inst.serviceFail "${EXEC_CMD}")"
fi

# ------------------------------------------------------------------ 5. menübejegyzés
if [ "$HEADLESS" -eq 0 ]; then
    say "\n$(msg inst.step5)"
    cat > "$APP_DIR/aqulify.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Aqulify
GenericName=$(msg_text en inst.desktopGeneric)
$(desktop_l10n GenericName inst.desktopGeneric)
Comment=$(msg_text en inst.desktopComment)
$(desktop_l10n Comment inst.desktopComment)
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
    ok "$(msg inst.menuOk)"
fi

# ------------------------------------------------------------------ 6. alapértelmezett kimenet
say "\n$(msg inst.step6)"
msg inst.defaultExplain; echo
if [ "${WAS_DEFAULT:-0}" = 1 ] || [ "$HEADLESS" -eq 1 ]; then
    ans="y"
    [ "${WAS_DEFAULT:-0}" = 1 ] && { msg inst.legacyDefault; echo; }
else
    read -r -p "$(msg inst.askDefault)" ans || ans=""
fi
# Igen: i (igen) vagy y (yes)
case "$ans" in
    [iIyY]*) ans="y" ;;
    *) ans="" ;;
esac
if [ "$ans" = "y" ]; then
    sleep 1
    if pw-metadata 0 default.configured.audio.sink '{ "name": "aqulify_sink" }' Spa:String:JSON >/dev/null 2>&1; then
        ok "$(msg inst.defaultOk)"
    else
        warn "$(msg inst.defaultFail)"
    fi
else
    msg inst.defaultLater; echo
fi

echo -e "\n${BLUE}====================================================================${NC}"
echo -e "${GREEN}  $(msg inst.done)${NC}"
echo -e "${BLUE}====================================================================${NC}"
if [ "$HEADLESS" -eq 1 ]; then
    LOCAL_IP=$(hostname -I 2>/dev/null | awk '{print $1}')
    [ -z "$LOCAL_IP" ] && LOCAL_IP="<IP-cím>"
    echo "  $(msg inst.sumHeadlessWeb "$LOCAL_IP")"
    echo "  $(msg inst.sumHeadlessPin "$BIN_DIR")"
    echo "  $(msg inst.sumState)"
    echo "  $(msg inst.sumLog)"
    echo "  $(msg inst.sumUninstall)"
else
    for k in inst.sumGui inst.sumState inst.sumLog inst.sumUninstall; do echo "  $(msg "$k")"; done
fi
