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
rm -f "$TMP/var/lib/aqulify-tune/state"

echo "=== [8] Wi-Fi energiatakarékosság (hamis iw, NetworkManager-mappa) ==="
WIFI="$(mktemp -d /tmp/aq-tune-wifi.XXXXXX)"
trap 'rm -rf "$TMP" "$TMP_NOCPU" "$WIFI"' EXIT
mkdir -p "$WIFI/sys/class/net/wlan0/wireless" "$WIFI/sys/class/net/eth0" "$WIFI/etc/NetworkManager/conf.d" "$WIFI/bin"
echo "on" > "$WIFI/iw_wlan0"
cat > "$WIFI/bin/iw" <<FAKEIW
#!/bin/sh
# hamis iw: dev <csatoló> get power_save | dev <csatoló> set power_save on|off
f="$WIFI/iw_\$2"
case "\$3" in
    get) [ -f "\$f" ] || exit 1; echo "Power save: \$(cat "\$f")" ;;
    set) echo "\$5" > "\$f" ;;
esac
FAKEIW
chmod 755 "$WIFI/bin/iw"
OUT_WIFI="$(AQ_SYSROOT="$WIFI" AQ_IW="$WIFI/bin/iw" bash "$TUNE" enable)"
check_eq "wlan0 energiatakarékosság -> off" "off" "$(cat "$WIFI/iw_wlan0")"
check "kábeles csatolóhoz (eth0) nem nyúlt" test ! -e "$WIFI/iw_eth0"
check "állapotfájlban a wlan0 eredeti értéke (on)" grep -Fxq "wifi:wlan0=on" "$WIFI/var/lib/aqulify-tune/state"
check "NetworkManager tartós beállítás létrejött" grep -Fq "wifi.powersave = 2" "$WIFI/etc/NetworkManager/conf.d/95-aqulify-wifi-powersave.conf"
check "kimenet jelzi a Wi-Fi-t" grep -Fq "Wi-Fi: 1 csatolón" <<<"$OUT_WIFI"
AQ_SYSROOT="$WIFI" AQ_IW="$WIFI/bin/iw" bash "$TUNE" enable --boot >/dev/null
check "kétszeri enable után is az eredeti (on) marad mentve" grep -Fxq "wifi:wlan0=on" "$WIFI/var/lib/aqulify-tune/state"
check "status kiírja a Wi-Fi állapotát" grep -Fq "Wi-Fi (wlan0) energiatakarékosság: off" <<<"$(AQ_SYSROOT="$WIFI" AQ_IW="$WIFI/bin/iw" bash "$TUNE" status)"
AQ_SYSROOT="$WIFI" AQ_IW="$WIFI/bin/iw" bash "$TUNE" disable >/dev/null
check_eq "disable után a wlan0 visszaállt: on" "on" "$(cat "$WIFI/iw_wlan0")"
check "disable törölte a NetworkManager-beállítást" test ! -e "$WIFI/etc/NetworkManager/conf.d/95-aqulify-wifi-powersave.conf"
# iw nélküli rendszeren sem áll le hibával
check "enable iw nélkül is sikeres" env AQ_SYSROOT="$WIFI" AQ_IW="$WIFI/bin/nincs-ilyen-iw" bash "$TUNE" enable >/dev/null
AQ_SYSROOT="$WIFI" AQ_IW="$WIFI/bin/nincs-ilyen-iw" bash "$TUNE" disable >/dev/null

echo "=== [9] Bejelentkezés nélküli futás (linger.sh) ==="
LINGER="$SCRIPT_DIR/linger.sh"
LG="$(mktemp -d /tmp/aq-linger.XXXXXX)"
trap 'rm -rf "$TMP" "$TMP_NOCPU" "$WIFI" "$LG"' EXIT
mkdir -p "$LG/bin"
cat > "$LG/bin/loginctl" <<FAKELC
#!/bin/sh
# hamis loginctl: enable-linger / disable-linger <név> a hamis /var/lib/systemd/linger alatt
echo "\$*" >> "$LG/loginctl.log"
case "\$1" in
    enable-linger) mkdir -p "$LG/var/lib/systemd/linger"; : > "$LG/var/lib/systemd/linger/\$2" ;;
    disable-linger) rm -f "$LG/var/lib/systemd/linger/\$2" ;;
esac
FAKELC
chmod 755 "$LG/bin/loginctl"
lg() { env AQ_SYSROOT="$LG" AQ_LOGINCTL="$LG/bin/loginctl" "$@"; }
# 9a: sudo apt-tal telepít a „bala” felhasználó: bekapcsolja és megjegyzi
lg SUDO_USER=bala AQ_UID1000=pi sh "$LINGER" enable >/dev/null
check "SUDO_USER lingeringje bekapcsolva" test -e "$LG/var/lib/systemd/linger/bala"
check "nem a uid 1000-es felhasználóé" test ! -e "$LG/var/lib/systemd/linger/pi"
check_eq "megjegyezte, kinek kapcsolta be" "bala" "$(cat "$LG/var/lib/aqulify/linger-user")"
# 9b: eltávolításkor visszavonja
lg sh "$LINGER" disable
check "disable visszavonta" test ! -e "$LG/var/lib/systemd/linger/bala"
check "a megjegyzés törölve" test ! -e "$LG/var/lib/aqulify/linger-user"
# 9c: a felhasználó már maga bekapcsolta: nem jegyzi meg, és eltávolításkor sem vonja vissza
mkdir -p "$LG/var/lib/systemd/linger"; : > "$LG/var/lib/systemd/linger/bala"
rm -f "$LG/loginctl.log"
lg SUDO_USER=bala sh "$LINGER" enable >/dev/null
check "már bekapcsolt lingeringhez nem nyúlt" test ! -e "$LG/loginctl.log"
lg sh "$LINGER" disable
check "a felhasználó saját beállítása megmaradt" test -e "$LG/var/lib/systemd/linger/bala"
rm -f "$LG/var/lib/systemd/linger/bala"
# 9d: sudo nélkül (root): a uid 1000-es felhasználó; root sosem
lg SUDO_USER=root AQ_UID1000=pi sh "$LINGER" enable >/dev/null
check "root telepítésnél a uid 1000-es felhasználó kapja" test -e "$LG/var/lib/systemd/linger/pi"
check "rootnak nem kapcsolja be" test ! -e "$LG/var/lib/systemd/linger/root"
lg sh "$LINGER" disable
# 9e: nincs felhasználó: nem hiba, nem ír semmit
check "felhasználó nélkül is 0-val tér vissza" lg SUDO_USER= AQ_UID1000= sh "$LINGER" enable
check "felhasználó nélkül nincs megjegyzés" test ! -e "$LG/var/lib/aqulify/linger-user"
# 9f: a loginctl nem működik (pl. systemd nélküli chroot): a linger-fájlt maga hozza létre
lg AQ_LOGINCTL=false SUDO_USER=bala sh "$LINGER" enable >/dev/null
check "loginctl nélkül a linger-fájl létrejött" test -e "$LG/var/lib/systemd/linger/bala"
lg AQ_LOGINCTL=false sh "$LINGER" disable
check "loginctl nélkül a disable is törölte" test ! -e "$LG/var/lib/systemd/linger/bala"

echo "=== [10] A Pi-csomag postinst és prerm hívja a lingeringet ==="
check "pi_deb_build: a postinst kiegészítés tartalmazza: aqulify-linger enable" grep -Fq "/usr/lib/aqulify/aqulify-linger enable" "$SCRIPT_DIR/pi_deb_build.sh"
check "a prerm (remove) tartalmazza: aqulify-linger disable" grep -Fq "aqulify-linger disable" "$PRERM"
check "a linger.sh telepítése a debian/rules-ba kerül" grep -Fq "debian/aqulify-linger.sh \$(D)/usr/lib/aqulify/aqulify-linger" "$SCRIPT_DIR/pi_deb_build.sh"

echo ""
if [ "$fail" -eq 0 ]; then
    echo "MINDEN TESZT SIKERES ($pass teszt, 0 hiba)"
    exit 0
else
    echo "TESZT SIKERTELEN ($pass sikeres, $fail hiba)"
    exit 1
fi
