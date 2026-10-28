#!/usr/bin/env bash
# ==============================================================================
# Aqulify 2.x – Raspberry Pi Headless Audiophile Streamer Beállító Szkript
# ==============================================================================
# Ez a szkript a Raspberry Pi 4 / 5 (Raspberry Pi OS Lite / Debian Bookworm/Trixie)
# miniszámítógépet dedikált, fejhallgatós és hangsugárzós audiophile streamerré
# alakítja Aqulify 2.x DSP motorral, Bluetooth A2DP vevővel, mDNS felderítéssel
# és távvezérelhető Web / Mobil felülettel.
# ==============================================================================
set -euo pipefail

AQ_LANG="en"
case "${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}" in
    hu*|HU*) AQ_LANG="hu" ;;
esac

msg_text() {
    local l="$1" k="$2" f=""
    case "$l" in
    hu)
        case "$k" in
        rpi.title)       f='Aqulify – Raspberry Pi Audiophile Streamer Telepítő' ;;
        rpi.noRoot)      f='Ne rootként futtasd! A saját felhasználóddal indítsd (a csomagokhoz és a Bluetooth-hoz sudo jelszót fog kérni).' ;;
        rpi.step1)       f='[1/5] Hangrendszer és rendszerfüggőségek telepítése (PipeWire, BlueZ, Avahi)...' ;;
        rpi.step2)       f='[2/5] Linger és háttérszolgáltatások beállítása (bootolás bejelentkezés nélkül)...' ;;
        rpi.step3)       f='[3/5] Bluetooth A2DP Sink és WirePlumber hangfolyam-kezelés konfigurálása...' ;;
        rpi.step4)       f='[4/5] Aqulify 2.x Headless DSP motor fordítása és telepítése...' ;;
        rpi.step5)       f='[5/5] Rendszerellenőrzés és állapot...' ;;
        rpi.btOk)        f='Bluetooth A2DP vevő konfigurálva (SBC-XQ, AAC, LDAC, aptX támogatás).' ;;
        rpi.pipewireOk)  f='PipeWire és WirePlumber felhasználói szolgáltatások aktívak.' ;;
        rpi.lingerOk)    f='Linger aktív: a hangrendszer és az Aqulify újraindítás után azonnal elindul, bejelentkezés nélkül.' ;;
        rpi.stepAirplay) f='AirPlay vevő konfigurálása (shairport-sync -> aqulify_sink)...' ;;
        rpi.airplayOk)   f='AirPlay vevő aktív: Apple eszközökről közvetlenül átjátszható a zene.' ;;
        rpi.stepSpotify) f='Spotify Connect konfigurálása (librespot -> aqulify_sink)...' ;;
        rpi.spotifyOk)   f='Spotify Connect aktív: a Spotify appból kiválasztható az "Aqulify Streamer".' ;;
        rpi.askAirplay)  f='Telepítsem és beállítsam az AirPlay vevőt (iPhone / Mac átjátszáshoz)? [i/N] ' ;;
        rpi.askSpotify)  f='Telepítsem és beállítsam a Spotify Connect vevőt (Spotify átjátszáshoz)? [i/N] ' ;;
        rpi.done)        f='Sikeres telepítés! A Raspberry Pi készen áll az audio streamingre.' ;;
        rpi.infoWeb)     f='Vezérlőfelület:         http://%s:47820  (vagy nyisd meg az Aqulify Remote appot)' ;;
        rpi.infoPin)     f='Első párosítás PIN:     aqulify-engine --pair-pin' ;;
        rpi.infoBt)      f='Bluetooth párosítás:    A telefonodon keresd a "%s" eszközt' ;;
        rpi.infoAirplay) f='AirPlay átjátszás:      Keresd az "Aqulify Streamer" eszközt az Apple eszközödön' ;;
        rpi.infoSpotify) f='Spotify Connect:        Keresd az "Aqulify Streamer" eszközt a Spotify appban' ;;
        rpi.infoService) f='Szolgáltatás állapota:  systemctl --user status aqulify' ;;
        rpi.infoLogs)    f='Élő DSP napló:          journalctl --user -u aqulify -f' ;;
        rpi.dacTip)      f='Tipp: I2S HAT (pl. HiFiBerry, IQaudIO) vagy USB DAC esetén az eszköz a webes felület "Eszközök" lapján vagy a pw-cli segítségével állítható be kimenetként.' ;;
        esac ;;
    *)
        case "$k" in
        rpi.title)       f='Aqulify – Raspberry Pi Audiophile Streamer Setup' ;;
        rpi.noRoot)      f='Do not run as root! Run as your normal user (it will ask for sudo password for packages and Bluetooth).' ;;
        rpi.step1)       f='[1/5] Installing audio stack and dependencies (PipeWire, BlueZ, Avahi)...' ;;
        rpi.step2)       f='[2/5] Enabling linger and background services (boot without login)...' ;;
        rpi.step3)       f='[3/5] Configuring Bluetooth A2DP Sink and WirePlumber routing...' ;;
        rpi.step4)       f='[4/5] Building and installing Aqulify 2.x Headless DSP engine...' ;;
        rpi.step5)       f='[5/5] Verifying system status...' ;;
        rpi.btOk)        f='Bluetooth A2DP sink configured (SBC-XQ, AAC, LDAC, aptX support).' ;;
        rpi.pipewireOk)  f='PipeWire and WirePlumber user services are active.' ;;
        rpi.lingerOk)    f='Linger active: audio stack and Aqulify start automatically at boot without login.' ;;
        rpi.stepAirplay) f='Configuring AirPlay receiver (shairport-sync -> aqulify_sink)...' ;;
        rpi.airplayOk)   f='AirPlay receiver active: stream directly from Apple devices.' ;;
        rpi.stepSpotify) f='Configuring Spotify Connect (librespot -> aqulify_sink)...' ;;
        rpi.spotifyOk)   f='Spotify Connect active: "Aqulify Streamer" is available in the Spotify app.' ;;
        rpi.askAirplay)  f='Install and configure AirPlay receiver (for iPhone / Mac streaming)? [y/N] ' ;;
        rpi.askSpotify)  f='Install and configure Spotify Connect (for Spotify streaming)? [y/N] ' ;;
        rpi.done)        f='Setup completed! Your Raspberry Pi is ready for audiophile streaming.' ;;
        rpi.infoWeb)     f='Control interface:      http://%s:47820  (or open Aqulify Remote app)' ;;
        rpi.infoPin)     f='First pairing PIN:      aqulify-engine --pair-pin' ;;
        rpi.infoBt)      f='Bluetooth pairing:      Search for "%s" on your phone' ;;
        rpi.infoAirplay) f='AirPlay streaming:      Select "Aqulify Streamer" on your Apple device' ;;
        rpi.infoSpotify) f='Spotify Connect:        Select "Aqulify Streamer" in the Spotify app' ;;
        rpi.infoService) f='Service status:         systemctl --user status aqulify' ;;
        rpi.infoLogs)    f='Live DSP logs:          journalctl --user -u aqulify -f' ;;
        rpi.dacTip)      f='Tip: For I2S HATs (e.g. HiFiBerry, IQaudIO) or USB DACs, select the hardware output on the "Devices" tab of the web interface or via pw-cli.' ;;
        esac ;;
    esac
    printf "%s" "$f"
}

msg() {
    local f
    f="$(msg_text "$AQ_LANG" "$1")"; shift
    # shellcheck disable=SC2059
    printf "$f" "$@"
}

RED='\033[0;31m'; GREEN='\033[0;32m'; BLUE='\033[0;34m'; YELLOW='\033[1;33m'; NC='\033[0m'
say()  { echo -e "${YELLOW}$*${NC}"; }
ok()   { echo -e "${GREEN}✓ $*${NC}"; }
warn() { echo -e "${YELLOW}! $*${NC}"; }
die()  { echo -e "${RED}[Error] $*${NC}"; exit 1; }

echo -e "\n${BLUE}====================================================================${NC}"
echo -e "${BLUE}  $(msg rpi.title)${NC}"
echo -e "${BLUE}====================================================================${NC}"

[ "$(id -u)" -eq 0 ] && die "$(msg rpi.noRoot)"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

WITH_AIRPLAY=0
WITH_SPOTIFY=0
for arg in "$@"; do
    case "$arg" in
        --with-airplay) WITH_AIRPLAY=1 ;;
        --with-spotify) WITH_SPOTIFY=1 ;;
        --all-streamers) WITH_AIRPLAY=1; WITH_SPOTIFY=1 ;;
    esac
done

if [ -t 0 ] && [ "$WITH_AIRPLAY" -eq 0 ] && [ "$WITH_SPOTIFY" -eq 0 ]; then
    read -r -p "$(msg rpi.askAirplay)" ans_ap || ans_ap=""
    case "$ans_ap" in [iIyY]*) WITH_AIRPLAY=1 ;; esac
    read -r -p "$(msg rpi.askSpotify)" ans_sp || ans_sp=""
    case "$ans_sp" in [iIyY]*) WITH_SPOTIFY=1 ;; esac
fi

# ------------------------------------------------------------------ 1. Csomagok
say "\n$(msg rpi.step1)"
if command -v apt-get >/dev/null 2>&1; then
    sudo apt-get update -y
    sudo apt-get install -y \
        build-essential cmake pkg-config libpipewire-0.3-dev libfftw3-dev \
        pipewire pipewire-pulse wireplumber libspa-0.2-bluetooth \
        bluez bluez-tools avahi-daemon
    # Opcionális kiváló minőségű kodekek és zajszűrés, ha elérhető
    sudo apt-get install -y librnnoise-dev 2>/dev/null || true
    sudo apt-get install -y libspa-0.2-extra 2>/dev/null || true
else
    warn "apt-get nem található; ellenőrizd manuálisan a PipeWire és BlueZ csomagokat."
fi

# ------------------------------------------------------------------ 2. Linger & User Services
say "\n$(msg rpi.step2)"
if command -v loginctl >/dev/null 2>&1; then
    loginctl enable-linger "$USER" 2>/dev/null || true
    ok "$(msg rpi.lingerOk)"
fi

# PipeWire és WirePlumber engedélyezése felhasználói szinten
systemctl --user enable --now pipewire.service pipewire-pulse.service wireplumber.service 2>/dev/null || true
ok "$(msg rpi.pipewireOk)"

# Avahi (mDNS / Zeroconf) engedélyezése a hálózati automatikus felderítéshez
sudo systemctl enable --now avahi-daemon 2>/dev/null || true

# ------------------------------------------------------------------ 3. Bluetooth A2DP Sink & WirePlumber
say "\n$(msg rpi.step3)"

# 3.1 BlueZ konfiguráció (Audio Sink / Hangszóró osztály)
if [ -f /etc/bluetooth/main.conf ]; then
    sudo sed -i 's/^#*Class *=.*/Class = 0x200414/' /etc/bluetooth/main.conf 2>/dev/null || true
    sudo sed -i 's/^#*DiscoverableTimeout *=.*/DiscoverableTimeout = 0/' /etc/bluetooth/main.conf 2>/dev/null || true
fi

# 3.2 WirePlumber Bluetooth konfiguráció
# Debian 12 (Bookworm) wireplumber 0.4.x vs Debian 13 (Trixie) wireplumber 0.5.x kompatibilitás
WP_VER="$(wireplumber --version 2>/dev/null | awk '{print $NF}' || echo '0.4.0')"
WP_MAJOR="$(echo "$WP_VER" | cut -d. -f1)"
WP_MINOR="$(echo "$WP_VER" | cut -d. -f2)"

if [ "${WP_MAJOR:-0}" -ge 1 ] || [ "${WP_MINOR:-0}" -ge 5 ]; then
    # WirePlumber 0.5+ SPA-JSON konfiguráció
    WP_CONF_DIR="$HOME/.config/wireplumber/wireplumber.conf.d"
    mkdir -p "$WP_CONF_DIR"
    cat > "$WP_CONF_DIR/50-bluez.conf" <<'EOF'
monitor.bluez.properties = {
  bluez5.enable-sbc-xq = true
  bluez5.enable-msbc = true
  bluez5.enable-hw-volume = true
  bluez5.codecs = [ sbc sbc_xq aac ldac aptx aptx_hd ]
}
monitor.bluez.rules = [
  {
    matches = [
      {
        node.name = "~bluez_input.*"
      }
    ]
    actions = {
      update-props = {
        node.pause-on-idle = false
      }
    }
  }
]
EOF
else
    # WirePlumber 0.4.x Lua konfiguráció
    WP_LUA_DIR="$HOME/.config/wireplumber/bluetooth.lua.d"
    mkdir -p "$WP_LUA_DIR"
    cat > "$WP_LUA_DIR/50-bluez-config.lua" <<'EOF'
bluez_monitor.properties = {
  ["bluez5.enable-sbc-xq"] = true,
  ["bluez5.enable-msbc"] = true,
  ["bluez5.enable-hw-volume"] = true,
  ["bluez5.headset-roles"] = "[ hsp_hs hsp_ag hfp_hf hfp_ag ]",
  ["bluez5.codecs"] = "[ sbc sbc_xq aac ldac aptx aptx_hd ]",
}
EOF
fi

# WirePlumber és Bluetooth újraindítása a módosítások érvényesítéséhez
sudo systemctl restart bluetooth 2>/dev/null || true
systemctl --user restart wireplumber 2>/dev/null || true
ok "$(msg rpi.btOk)"

# ------------------------------------------------------------------ 4. Aqulify 2.x Headless Motor
say "\n$(msg rpi.step4)"
cd "$ROOT_DIR"
./install.sh --headless

# ------------------------------------------------------------------ AirPlay (shairport-sync)
if [ "$WITH_AIRPLAY" -eq 1 ]; then
    say "\n$(msg rpi.stepAirplay)"
    if command -v apt-get >/dev/null 2>&1; then
        sudo apt-get install -y shairport-sync 2>/dev/null || warn "shairport-sync csomag nem található apt-ban"
    fi
    if command -v shairport-sync >/dev/null 2>&1; then
        sudo mkdir -p /etc/shairport-sync.conf.d 2>/dev/null || true
        cat <<'EOF' | sudo tee /etc/shairport-sync.conf.d/aqulify.conf >/dev/null
general = {
  name = "Aqulify Streamer";
  output_backend = "pw";
};
EOF
        sudo systemctl enable --now shairport-sync 2>/dev/null || true
        ok "$(msg rpi.airplayOk)"
    fi
fi

# ------------------------------------------------------------------ Spotify Connect (librespot)
if [ "$WITH_SPOTIFY" -eq 1 ]; then
    say "\n$(msg rpi.stepSpotify)"
    LIBRESPOT_BIN="${HOME}/.local/bin/librespot"
    if ! command -v librespot >/dev/null 2>&1 && [ ! -x "$LIBRESPOT_BIN" ]; then
        if ! sudo apt-get install -y librespot 2>/dev/null; then
            ARCH="$(uname -m)"
            case "$ARCH" in
                aarch64|arm64) SPOT_ARCH="linux-arm64" ;;
                armv7l|armhf)  SPOT_ARCH="linux-armhf" ;;
                x86_64)        SPOT_ARCH="linux-x86_64" ;;
                *)             SPOT_ARCH="" ;;
            esac
            if [ -n "$SPOT_ARCH" ]; then
                say "librespot (${SPOT_ARCH}) letöltése..."
                mkdir -p "${HOME}/.local/bin"
                SPOT_URL="https://github.com/librespot-org/librespot/releases/latest/download/librespot-${SPOT_ARCH}.tar.gz"
                if curl -fsSL "$SPOT_URL" 2>/dev/null | tar -xz -C "${HOME}/.local/bin" 2>/dev/null; then
                    chmod +x "$LIBRESPOT_BIN" 2>/dev/null || true
                fi
            fi
        else
            LIBRESPOT_BIN="$(command -v librespot)"
        fi
    fi
    if command -v librespot >/dev/null 2>&1 || [ -x "$LIBRESPOT_BIN" ]; then
        [ -x "$LIBRESPOT_BIN" ] || LIBRESPOT_BIN="$(command -v librespot)"
        mkdir -p "$HOME/.config/systemd/user"
        cat > "$HOME/.config/systemd/user/aqulify-spotify.service" <<EOF
[Unit]
Description=Aqulify Spotify Connect (librespot)
After=pipewire.service aqulify.service
Wants=aqulify.service

[Service]
ExecStart=${LIBRESPOT_BIN} --name "Aqulify Streamer" --backend pipewire --device aqulify_sink --bitrate 320 --enable-volume-normalisation
Restart=on-failure
RestartSec=3

[Install]
WantedBy=default.target
EOF
        systemctl --user daemon-reload 2>/dev/null || true
        systemctl --user enable --now aqulify-spotify.service 2>/dev/null || true
        ok "$(msg rpi.spotifyOk)"
    fi
fi

# ------------------------------------------------------------------ 5. Ellenőrzés és összegzés
say "\n$(msg rpi.step5)"
LOCAL_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
[ -z "$LOCAL_IP" ] && LOCAL_IP="127.0.0.1"
HOSTNAME_STR="$(hostname 2>/dev/null || echo 'raspberrypi')"

echo -e "\n${BLUE}====================================================================${NC}"
echo -e "${GREEN}  $(msg rpi.done)${NC}"
echo -e "${BLUE}====================================================================${NC}"
echo "  $(msg rpi.infoWeb "$LOCAL_IP")"
echo "  $(msg rpi.infoPin)"
echo "  $(msg rpi.infoBt "$HOSTNAME_STR")"
if [ "$WITH_AIRPLAY" -eq 1 ]; then
    echo "  $(msg rpi.infoAirplay)"
fi
if [ "$WITH_SPOTIFY" -eq 1 ]; then
    echo "  $(msg rpi.infoSpotify)"
fi
echo "  $(msg rpi.infoService)"
echo "  $(msg rpi.infoLogs)"
echo -e "\n${YELLOW}  $(msg rpi.dacTip)${NC}\n"
