#!/usr/bin/env bash
# Aqulify választható és visszavonható rendszerhangolás (Linux / Raspberry Pi OS):
#   1. CPU frekvenciaskálázás: scaling_governor -> performance (ha elérhető)
#   2. USB-energiatakarékosság kikapcsolása az USB Audio Class (bInterfaceClass = 01) eszközökön: power/control -> on
#   3. Valós idejű prioritás és memóriazárolás az @audio csoportnak: /etc/security/limits.d/95-aqulify-audio.conf
#   4. Systemd oneshot szolgáltatás újraindítás utáni alkalmazáshoz: /etc/systemd/system/aqulify-tune.service
#
# Dokumentációs források (ellenőrizve: 2026-11-15):
# - Linux kernel CPU Performance Scaling (cpufreq sysfs: scaling_governor, scaling_available_governors):
#   https://docs.kernel.org/admin-guide/pm/cpufreq.html
# - Linux kernel Power Management for USB (usb sysfs: power/control = on | auto):
#   https://docs.kernel.org/driver-api/usb/power-management.html
# - USB Class Codes (Audio Interface Class = 0x01):
#   https://www.usb.org/defined-class-codes
# - PAM limits.conf(5) kézikönyv (rtprio, memlock, /etc/security/limits.d/*.conf):
#   https://man7.org/linux/man-pages/man5/limits.conf.5.html
#
# Használat:
#   sudo aqulify-tune.sh enable [--boot]
#   sudo aqulify-tune.sh disable
#   aqulify-tune.sh status
#
# Teszteléshez (root nélkül): AQ_SYSROOT=/ideiglenes/mappa bash aqulify-tune.sh <enable|disable|status>

set -euo pipefail

SYSROOT="${AQ_SYSROOT:-}"
SYSROOT="${SYSROOT%/}"

STATE_DIR="$SYSROOT/var/lib/aqulify-tune"
STATE_FILE="$STATE_DIR/state"
LIMITS_FILE="$SYSROOT/etc/security/limits.d/95-aqulify-audio.conf"
SERVICE_FILE="$SYSROOT/etc/systemd/system/aqulify-tune.service"

usage() {
    cat <<'EOF'
Használat: aqulify-tune.sh <enable [--boot] | disable | status>
  enable   Bekapcsolja a rendszerhangolást (eredeti értékek mentése után).
  disable  Visszaállítja az eredeti értékeket és törli a létrehozott fájlokat.
  status   Kiírja a jelenlegi CPU, USB, limits.d és systemd állapotot.
EOF
}

require_root_if_real() {
    if [ -z "$SYSROOT" ] && [ "$(id -u)" -ne 0 ]; then
        echo "HIBA: ezt a parancsot rootként (sudo-val) kell futtatni." >&2
        exit 1
    fi
}

# Egy kulcs=érték pár mentése az állapotfájlba, ha a kulcs még nem szerepel benne (kétszeri enable védelme).
save_state_once() {
    local key="$1"
    local val="$2"
    mkdir -p "$STATE_DIR"
    touch "$STATE_FILE"
    if ! grep -Fq "${key}=" "$STATE_FILE"; then
        printf '%s=%s\n' "$key" "$val" >> "$STATE_FILE"
    fi
}

# Az összes USB Audio Class (bInterfaceClass == 01) eszközhöz tartozó power/control fájl megkeresése.
find_usb_audio_power_controls() {
    local usb_dir="$SYSROOT/sys/bus/usb/devices"
    [ -d "$usb_dir" ] || return 0
    local iface_file iface_dir cls candidate parent_dir base_dev
    while IFS= read -r iface_file; do
        [ -f "$iface_file" ] || continue
        cls="$(tr -d '[:space:]' < "$iface_file" 2>/dev/null || true)"
        [ "$cls" = "01" ] || continue
        iface_dir="$(dirname "$iface_file")"
        parent_dir="$(dirname "$iface_dir")"
        base_dev="${iface_dir%%:*}"
        for candidate in "$iface_dir/power/control" "$parent_dir/power/control" "$base_dev/power/control"; do
            if [ -f "$candidate" ]; then
                printf '%s\n' "$candidate"
                break
            fi
        done
    done < <(find -L "$usb_dir" -maxdepth 4 -name bInterfaceClass 2>/dev/null | sort) | awk '!seen[$0]++'
}

cmd_enable() {
    local boot_mode=0
    if [ "${1:-}" = "--boot" ]; then
        boot_mode=1
    fi
    require_root_if_real
    mkdir -p "$STATE_DIR"
    touch "$STATE_FILE"

    # 1. CPU scaling_governor -> performance
    local cpu_found=0 cpu_changed=0 gov_file cpu_dir avail_file rel_path orig_gov
    for gov_file in "$SYSROOT"/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
        [ -f "$gov_file" ] || continue
        cpu_found=1
        cpu_dir="$(dirname "$gov_file")"
        avail_file="$cpu_dir/scaling_available_governors"
        if [ -f "$avail_file" ] && ! grep -qw "performance" "$avail_file"; then
            echo "CPU: $(basename "$(dirname "$cpu_dir")") nem támogatja a 'performance' governort, kihagyva."
            continue
        fi
        orig_gov="$(tr -d '[:space:]' < "$gov_file")"
        rel_path="${gov_file#"$SYSROOT"}"
        save_state_once "cpu:${rel_path}" "$orig_gov"
        printf '%s\n' "performance" > "$gov_file"
        cpu_changed=$((cpu_changed + 1))
    done
    if [ "$cpu_found" -eq 0 ]; then
        echo "CPU: cpufreq nem érhető el ezen a rendszeren, kihagyva."
    else
        echo "CPU: $cpu_changed mag 'performance' üzemmódra állítva."
    fi

    # 2. USB-energiatakarékosság kikapcsolása a hangeszközökön (bInterfaceClass = 01 -> power/control = on)
    local usb_count=0 ctrl_file orig_ctrl
    while IFS= read -r ctrl_file; do
        [ -n "$ctrl_file" ] && [ -f "$ctrl_file" ] || continue
        orig_ctrl="$(tr -d '[:space:]' < "$ctrl_file")"
        rel_path="${ctrl_file#"$SYSROOT"}"
        save_state_once "usb:${rel_path}" "$orig_ctrl"
        printf '%s\n' "on" > "$ctrl_file"
        usb_count=$((usb_count + 1))
    done < <(find_usb_audio_power_controls)
    echo "USB: $usb_count hangeszközön az energiatakarékosság kikapcsolva (power/control = on)."

    # 3. Valós idejű prioritás (@audio csoportnak)
    mkdir -p "$(dirname "$LIMITS_FILE")"
    cat > "$LIMITS_FILE" <<'EOF'
# Aqulify – valós idejű prioritás és memóriazárolás az audio csoportnak (limits.conf(5))
@audio - rtprio 95
@audio - memlock unlimited
EOF
    echo "Valós idejű prioritás: $LIMITS_FILE létrehozva (@audio rtprio 95, memlock unlimited)."
    echo "Megjegyzés: a valós idejű prioritáshoz a felhasználónak az 'audio' csoportban kell lennie (ellenőrzés: groups)."

    # 4. Systemd szolgáltatás újraindítás utánra
    mkdir -p "$(dirname "$SERVICE_FILE")"
    cat > "$SERVICE_FILE" <<'EOF'
[Unit]
Description=Aqulify system tuning for low-latency audio
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/usr/bin/aqulify-tune enable --boot
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
    echo "Szolgáltatás: $SERVICE_FILE létrehozva."
    if [ -n "$SYSROOT" ]; then
        echo "[AQ_SYSROOT tesztmód] systemctl kihagyva (ezt hívná: systemctl daemon-reload && systemctl enable aqulify-tune.service)"
    elif [ "$boot_mode" -eq 0 ] && command -v systemctl >/dev/null 2>&1; then
        systemctl daemon-reload
        systemctl enable aqulify-tune.service
    fi
}

cmd_disable() {
    require_root_if_real
    if [ -f "$STATE_FILE" ]; then
        local line key val rel_path target_file
        while IFS= read -r line || [ -n "$line" ]; do
            [ -n "$line" ] || continue
            key="${line%%=*}"
            val="${line#*=}"
            case "$key" in
                cpu:*|usb:*)
                    rel_path="${key#*:}"
                    target_file="${SYSROOT}${rel_path}"
                    if [ -f "$target_file" ]; then
                        printf '%s\n' "$val" > "$target_file"
                    fi
                    ;;
            esac
        done < "$STATE_FILE"
        rm -f "$STATE_FILE"
        rmdir "$STATE_DIR" 2>/dev/null || true
        echo "Állapot visszaállítva az állapotfájl alapján."
    else
        echo "Nincs mentett állapotfájl ($STATE_FILE)."
    fi

    if [ -f "$LIMITS_FILE" ]; then
        rm -f "$LIMITS_FILE"
        echo "Törölve: $LIMITS_FILE"
    fi
    if [ -f "$SERVICE_FILE" ]; then
        rm -f "$SERVICE_FILE"
        echo "Törölve: $SERVICE_FILE"
    fi

    if [ -n "$SYSROOT" ]; then
        echo "[AQ_SYSROOT tesztmód] systemctl kihagyva (ezt hívná: systemctl disable aqulify-tune.service && systemctl daemon-reload)"
    elif command -v systemctl >/dev/null 2>&1; then
        systemctl disable aqulify-tune.service >/dev/null 2>&1 || true
        systemctl daemon-reload >/dev/null 2>&1 || true
    fi
}

cmd_status() {
    echo "=== Aqulify rendszerhangolás állapota (SYSROOT: ${SYSROOT:-/}) ==="
    if [ -f "$STATE_FILE" ]; then
        echo "Állapotfájl: aktív ($STATE_FILE)"
    else
        echo "Állapotfájl: nincs (kikapcsolva)"
    fi

    local cpu_found=0 gov_file cpu_name
    for gov_file in "$SYSROOT"/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
        [ -f "$gov_file" ] || continue
        cpu_found=1
        cpu_name="$(basename "$(dirname "$(dirname "$gov_file")")")"
        echo "CPU $cpu_name governor: $(tr -d '[:space:]' < "$gov_file")"
    done
    if [ "$cpu_found" -eq 0 ]; then
        echo "CPU cpufreq: nem érhető el"
    fi

    local usb_found=0 ctrl_file dev_name
    while IFS= read -r ctrl_file; do
        [ -n "$ctrl_file" ] && [ -f "$ctrl_file" ] || continue
        usb_found=1
        dev_name="$(basename "$(dirname "$(dirname "$ctrl_file")")")"
        echo "USB hangeszköz ($dev_name) power/control: $(tr -d '[:space:]' < "$ctrl_file")"
    done < <(find_usb_audio_power_controls)
    if [ "$usb_found" -eq 0 ]; then
        echo "USB hangeszköz: nem található"
    fi

    if [ -f "$LIMITS_FILE" ]; then
        echo "limits.d: megvan ($LIMITS_FILE)"
    else
        echo "limits.d: nincs"
    fi

    if [ -f "$SERVICE_FILE" ]; then
        echo "systemd egység: megvan ($SERVICE_FILE)"
    else
        echo "systemd egység: nincs"
    fi
}

case "${1:-}" in
    enable)
        shift
        cmd_enable "${1:-}"
        ;;
    disable)
        cmd_disable
        ;;
    status)
        cmd_status
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac
