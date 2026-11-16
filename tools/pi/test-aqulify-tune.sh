#!/usr/bin/env bash
# Az aqulify-tune.sh automatizált egységtesztje hamis sys/etc fával (root jogosultság nélkül).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TUNE="$SCRIPT_DIR/aqulify-tune.sh"

fail=0
pass=0

check() {
    local desc="$1"
    shift
    if "$@"; then
        echo "  [OK]   $desc"
        pass=$((pass + 1))
    else
        echo "  [HIBA] $desc"
        fail=$((fail + 1))
    fi
}

check_eq() {
    local desc="$1"
    local expected="$2"
    local actual="$3"
    if [ "$expected" = "$actual" ]; then
        echo "  [OK]   $desc ($actual)"
        pass=$((pass + 1))
    else
        echo "  [HIBA] $desc (várt: '$expected', kapott: '$actual')"
        fail=$((fail + 1))
    fi
}

TMP="$(mktemp -d /tmp/aq-tune-test.XXXXXX)"
TMP_NOCPU="$(mktemp -d /tmp/aq-tune-nocpu.XXXXXX)"
trap 'rm -rf "$TMP" "$TMP_NOCPU"' EXIT

echo "=== [1] Hamis sys/etc fa felépítése (4 CPU, 1 USB Audio + 1 USB HID eszköz) ==="
for c in 0 1 2; do
    mkdir -p "$TMP/sys/devices/system/cpu/cpu$c/cpufreq"
    echo "ondemand powersave performance schedutil" > "$TMP/sys/devices/system/cpu/cpu$c/cpufreq/scaling_available_governors"
done
echo "ondemand" > "$TMP/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
echo "ondemand" > "$TMP/sys/devices/system/cpu/cpu1/cpufreq/scaling_governor"
echo "schedutil" > "$TMP/sys/devices/system/cpu/cpu2/cpufreq/scaling_governor"

# cpu3: nem támogatja a performance governort
mkdir -p "$TMP/sys/devices/system/cpu/cpu3/cpufreq"
echo "powersave ondemand" > "$TMP/sys/devices/system/cpu/cpu3/cpufreq/scaling_available_governors"
echo "powersave" > "$TMP/sys/devices/system/cpu/cpu3/cpufreq/scaling_governor"

# USB 1-1: hangeszköz (bInterfaceClass = 01)
mkdir -p "$TMP/sys/bus/usb/devices/1-1/power" "$TMP/sys/bus/usb/devices/1-1/1-1:1.0" "$TMP/sys/bus/usb/devices/1-1/1-1:1.1"
echo "auto" > "$TMP/sys/bus/usb/devices/1-1/power/control"
echo "01" > "$TMP/sys/bus/usb/devices/1-1/1-1:1.0/bInterfaceClass"
echo "01" > "$TMP/sys/bus/usb/devices/1-1/1-1:1.1/bInterfaceClass"

# USB 1-2: nem hang osztályú eszköz (HID, bInterfaceClass = 03)
mkdir -p "$TMP/sys/bus/usb/devices/1-2/power" "$TMP/sys/bus/usb/devices/1-2/1-2:1.0"
echo "auto" > "$TMP/sys/bus/usb/devices/1-2/power/control"
echo "03" > "$TMP/sys/bus/usb/devices/1-2/1-2:1.0/bInterfaceClass"

# Nem a /sys/bus/usb/devices alatt lévő bInterfaceClass = 01 (nem szabad hozzányúlnia):
mkdir -p "$TMP/sys/devices/platform/fake_usb/power" "$TMP/sys/devices/platform/fake_usb/fake:1.0"
echo "auto" > "$TMP/sys/devices/platform/fake_usb/power/control"
echo "01" > "$TMP/sys/devices/platform/fake_usb/fake:1.0/bInterfaceClass"

echo "=== [2] enable futtatása és ellenőrzés ==="
OUT_ENABLE="$(AQ_SYSROOT="$TMP" bash "$TUNE" enable)"
echo "$OUT_ENABLE"

check_eq "cpu0 governor -> performance" "performance" "$(cat "$TMP/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")"
check_eq "cpu1 governor -> performance" "performance" "$(cat "$TMP/sys/devices/system/cpu/cpu1/cpufreq/scaling_governor")"
check_eq "cpu2 governor -> performance" "performance" "$(cat "$TMP/sys/devices/system/cpu/cpu2/cpufreq/scaling_governor")"
check_eq "cpu3 governor érintetlen (nincs performance)" "powersave" "$(cat "$TMP/sys/devices/system/cpu/cpu3/cpufreq/scaling_governor")"
check_eq "USB hangeszköz (1-1) power/control -> on" "on" "$(cat "$TMP/sys/bus/usb/devices/1-1/power/control")"
check_eq "USB nem-hang eszköz (1-2) power/control érintetlen (auto)" "auto" "$(cat "$TMP/sys/bus/usb/devices/1-2/power/control")"
check_eq "/sys/bus/usb/devices-en kívüli power/control érintetlen (auto)" "auto" "$(cat "$TMP/sys/devices/platform/fake_usb/power/control")"

check "limits.d fájl létrejött" test -f "$TMP/etc/security/limits.d/95-aqulify-audio.conf"
check "limits.d tartalmazza: @audio - rtprio 95" grep -Fq "@audio - rtprio 95" "$TMP/etc/security/limits.d/95-aqulify-audio.conf"
check "limits.d tartalmazza: @audio - memlock unlimited" grep -Fq "@audio - memlock unlimited" "$TMP/etc/security/limits.d/95-aqulify-audio.conf"
check "systemd service fájl létrejött" test -f "$TMP/etc/systemd/system/aqulify-tune.service"
check "systemd service Type=oneshot" grep -Fq "Type=oneshot" "$TMP/etc/systemd/system/aqulify-tune.service"
check "systemd service ExecStart tartalmazza: enable --boot" grep -Fq "enable --boot" "$TMP/etc/systemd/system/aqulify-tune.service"
check "kimenet figyelmeztet az audio csoportra" grep -Fq "audio" <<<"$OUT_ENABLE"
check "kimenet jelzi a systemctl kihagyását tesztmódban" grep -Fq "systemctl kihagyva" <<<"$OUT_ENABLE"

echo "=== [3] Kétszeri enable nem rontja el a mentett eredeti állapotot ==="
AQ_SYSROOT="$TMP" bash "$TUNE" enable >/dev/null
AQ_SYSROOT="$TMP" bash "$TUNE" enable --boot >/dev/null
check "állapotfájlban cpu0 eredeti értéke ondemand maradt" grep -Fq "cpu:/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor=ondemand" "$TMP/var/lib/aqulify-tune/state"
check "állapotfájlban cpu2 eredeti értéke schedutil maradt" grep -Fq "cpu:/sys/devices/system/cpu/cpu2/cpufreq/scaling_governor=schedutil" "$TMP/var/lib/aqulify-tune/state"
check "állapotfájlban USB 1-1 eredeti értéke auto maradt" grep -Fq "usb:/sys/bus/usb/devices/1-1/power/control=auto" "$TMP/var/lib/aqulify-tune/state"

echo "=== [4] status parancs ellenőrzése ==="
OUT_STATUS="$(AQ_SYSROOT="$TMP" bash "$TUNE" status)"
echo "$OUT_STATUS"
check "status jelzi az aktív állapotfájlt" grep -Fq "aktív" <<<"$OUT_STATUS"

echo "=== [5] disable futtatása és teljes visszaállítás ellenőrzése ==="
OUT_DISABLE="$(AQ_SYSROOT="$TMP" bash "$TUNE" disable)"
echo "$OUT_DISABLE"

check_eq "cpu0 governor visszaállt: ondemand" "ondemand" "$(cat "$TMP/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")"
check_eq "cpu1 governor visszaállt: ondemand" "ondemand" "$(cat "$TMP/sys/devices/system/cpu/cpu1/cpufreq/scaling_governor")"
check_eq "cpu2 governor visszaállt: schedutil" "schedutil" "$(cat "$TMP/sys/devices/system/cpu/cpu2/cpufreq/scaling_governor")"
check_eq "cpu3 governor maradt: powersave" "powersave" "$(cat "$TMP/sys/devices/system/cpu/cpu3/cpufreq/scaling_governor")"
check_eq "USB hangeszköz (1-1) power/control visszaállt: auto" "auto" "$(cat "$TMP/sys/bus/usb/devices/1-1/power/control")"
check_eq "USB nem-hang eszköz (1-2) power/control maradt: auto" "auto" "$(cat "$TMP/sys/bus/usb/devices/1-2/power/control")"
check "limits.d fájl törölve" test ! -e "$TMP/etc/security/limits.d/95-aqulify-audio.conf"
check "systemd service fájl törölve" test ! -e "$TMP/etc/systemd/system/aqulify-tune.service"
check "állapotfájl törölve" test ! -e "$TMP/var/lib/aqulify-tune/state"

echo "=== [6] Hiányzó cpufreq (pl. emulált környezet) esetén nem áll le hibával ==="
mkdir -p "$TMP_NOCPU/sys/devices/system/cpu/cpu0" "$TMP_NOCPU/sys/bus/usb/devices"
check "enable hiányzó cpufreq mellett sikeres" env AQ_SYSROOT="$TMP_NOCPU" bash "$TUNE" enable >/dev/null
check "status hiányzó cpufreq mellett sikeres" env AQ_SYSROOT="$TMP_NOCPU" bash "$TUNE" status >/dev/null
check "disable hiányzó cpufreq mellett sikeres" env AQ_SYSROOT="$TMP_NOCPU" bash "$TUNE" disable >/dev/null

echo "=== [7] debian/prerm ellenőrzése (állapotfájl nélkül, állapotfájllal és hibával) ==="
PRERM="$TMP/prerm"
sed -n '/^cat > debian\/prerm/,/^EOF$/{ /^cat /d; /^EOF$/d; p; }' "$SCRIPT_DIR/pi_deb_build.sh" > "$PRERM"
chmod 755 "$PRERM"
mkdir -p "$TMP/bin"
cat > "$TMP/bin/aqulify-tune" <<WRAP
#!/usr/bin/env bash
echo "\$*" >> "$TMP/prerm_calls.log"
exec bash "$TUNE" "\$@"
WRAP
chmod 755 "$TMP/bin/aqulify-tune"

# 7a: állapotfájl nem létezik -> prerm nem hívja az aqulify-tune-t
rm -f "$TMP/prerm_calls.log" "$TMP/var/lib/aqulify-tune/state"
check "prerm remove állapotfájl nélkül sikeres" env AQ_SYSROOT="$TMP" PATH="$TMP/bin:$PATH" sh "$PRERM" remove
check "prerm állapotfájl nélkül nem hívott aqulify-tune-t" test ! -e "$TMP/prerm_calls.log"

# 7b: állapotfájl létezik -> prerm meghívja az aqulify-tune disable-t és visszaállítja az állapotot
AQ_SYSROOT="$TMP" bash "$TUNE" enable >/dev/null
check "prerm remove állapotfájllal sikeres" env AQ_SYSROOT="$TMP" PATH="$TMP/bin:$PATH" sh "$PRERM" remove
check_eq "prerm az 'aqulify-tune disable' parancsot hívta" "disable" "$(cat "$TMP/prerm_calls.log")"
check "prerm után az állapotfájl törölve" test ! -e "$TMP/var/lib/aqulify-tune/state"
check_eq "prerm után cpu0 governor visszaállt: ondemand" "ondemand" "$(cat "$TMP/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")"

# 7c: ha az aqulify-tune disable hibával tér vissza, a prerm akkor sem áll le hibával
mkdir -p "$TMP/var/lib/aqulify-tune"
touch "$TMP/var/lib/aqulify-tune/state"
cat > "$TMP/bin/aqulify-tune" <<'FAILWRAP'
#!/bin/sh
exit 1
FAILWRAP
chmod 755 "$TMP/bin/aqulify-tune"
check "prerm remove hibás aqulify-tune mellett is 0-val tér vissza" env AQ_SYSROOT="$TMP" PATH="$TMP/bin:$PATH" sh "$PRERM" remove

echo ""
if [ "$fail" -eq 0 ]; then
    echo "MINDEN TESZT SIKERES ($pass teszt, 0 hiba)"
    exit 0
else
    echo "TESZT SIKERTELEN ($pass sikeres, $fail hiba)"
    exit 1
fi
