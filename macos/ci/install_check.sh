#!/bin/bash
# CI: a DMG telepíthetősége egy friss macOS gépen, ahogy egy felhasználó telepíti. Lépések:
#   - letöltött (karanténos) DMG → csatolás → Aqulify.app az Alkalmazások mappába
#   - aláírás, Gatekeeper, architektúrák, minimális macOS, függőségek
#   - indítás az engedélyezés után, újraindítás, indítás közvetlenül a DMG-ről
#   - eltávolítás
# Használat: macos/ci/install_check.sh <dmg> <a CMakeLists.txt verziója>
set -uo pipefail
dmg=$1
want_ver=$2
here=$(cd "$(dirname "$0")" && pwd)
app=/Applications/Aqulify.app
log="$HOME/Library/Logs/Aqulify/aqulify.log"
port=47810
fail=0
err() { echo "::error title=Telepítés::$*"; fail=1; }
note() { echo "::notice title=Telepítés::$*"; }
ver_le() { [ "$(printf '%s\n' "$1" "$2" | sort -V | head -n1)" = "$1" ]; }   # $1 <= $2

echo "::group::Rendszer"
sw_vers; uname -m
spctl --status 2>&1 || true
csrutil status 2>&1 || true
echo "::endgroup::"
os_ver=$(sw_vers -productVersion)
os_arch=$(uname -m)

# 1. Letöltés: ellenőrzőösszeg, majd karantén, ahogy a böngésző menti
echo "::group::DMG"
sum="$dmg.sha256"
if [ -f "$sum" ]; then
    (cd "$(dirname "$dmg")" && shasum -a 256 -c "$(basename "$sum")") || err "a DMG ellenőrzőösszege nem egyezik"
else
    err "hiányzik az ellenőrzőösszeg: $sum"
fi
plain="$RUNNER_TEMP/plain.dmg"           # karantén nélküli példány (a DMG-ről indításhoz)
cp "$dmg" "$plain"
xattr -w com.apple.quarantine "0083;$(printf %x "$(date +%s)");Safari;$(uuidgen)" "$dmg"
hdiutil verify "$dmg" || err "hdiutil verify: a DMG sérült"
mnt=$(mktemp -d "$RUNNER_TEMP/mnt.XXXX")
if ! hdiutil attach -nobrowse -readonly -noautoopen -mountpoint "$mnt" "$dmg"; then
    err "a DMG nem csatolható"; exit 1
fi
ls -la "$mnt"
entries=$(ls -A "$mnt" | grep -v -E '^\.(fseventsd|Trashes|DS_Store)$' | sort | tr '\n' ' ')
[ "$entries" = "Applications Aqulify.app " ] || err "a DMG tartalma nem a várt (Aqulify.app és Applications): $entries"
[ "$(readlink "$mnt/Applications")" = /Applications ] || err "az Applications parancsikon nem az /Applications mappára mutat"
echo "::endgroup::"

# 2. Telepítés: húzás az Alkalmazások mappába (a Finder is megtartja a kiterjesztett attribútumokat, a karantént is)
echo "::group::Telepítés"
[ -e "$app" ] && err "$app már létezik a friss gépen"
ditto "$mnt/Aqulify.app" "$app" || err "a másolás az Alkalmazások mappába sikertelen"
hdiutil detach "$mnt" >/dev/null || hdiutil detach -force "$mnt" >/dev/null
q=$(xattr -p com.apple.quarantine "$app" 2>/dev/null || true)
if [ -n "$q" ]; then
    echo "karantén a telepített alkalmazáson (a DMG-ből örökölve): $q"
else
    note "a csatolt DMG nem adta tovább a karantént; az alkalmazásra külön tesszük fel"
    xattr -w com.apple.quarantine "0083;$(printf %x "$(date +%s)");Safari;$(uuidgen)" "$app"
fi
echo "::endgroup::"

# 3. Aláírás és Gatekeeper
echo "::group::Aláírás és Gatekeeper"
codesign --verify --deep --strict --verbose=2 "$app" 2>&1 || err "az aláírás hibás (a macOS sérült alkalmazásnak mutatná)"
codesign -dv "$app" 2>&1 | grep -E "^(Identifier|Format|Signature|TeamIdentifier|CodeDirectory)" || true
ga=$(spctl -a -vv -t exec "$app" 2>&1 || true)
echo "$ga"
case "$ga" in
    *accepted*) note "macOS $os_ver: a Gatekeeper elfogadta" ;;
    *rejected*) echo "Gatekeeper: elutasítva (várható: nincs Apple-hitelesítés; a README első indítás lépései kellenek)" ;;
    *) note "macOS $os_ver: ismeretlen Gatekeeper válasz: $ga" ;;
esac
if command -v syspolicy_check >/dev/null; then syspolicy_check distribution "$app" 2>&1 | head -n 30 || true; fi
echo "::endgroup::"

# 4. Info.plist
echo "::group::Info.plist"
plist="$app/Contents/Info.plist"
pl() { /usr/libexec/PlistBuddy -c "Print :$1" "$plist" 2>/dev/null; }
plutil -lint "$plist" || err "az Info.plist hibás"
[ "$(pl CFBundleIdentifier)" = io.github.szorgos.Aqulify ] || err "CFBundleIdentifier: $(pl CFBundleIdentifier)"
[ "$(pl CFBundlePackageType)" = APPL ] || err "CFBundlePackageType: $(pl CFBundlePackageType)"
[ "$(pl CFBundleShortVersionString)" = "$want_ver" ] || err "CFBundleShortVersionString: $(pl CFBundleShortVersionString), a CMakeLists.txt szerint $want_ver"
dmg_ver=$(basename "$dmg" | sed -n 's/^Aqulify-\([0-9][0-9.]*\)-macOS\.dmg$/\1/p')
[ -z "$dmg_ver" ] || [ "$dmg_ver" = "$want_ver" ] || err "a DMG neve $dmg_ver verziót mond, az alkalmazás $want_ver"
min=$(pl LSMinimumSystemVersion)
[ -n "$min" ] || err "hiányzik az LSMinimumSystemVersion"
echo "LSMinimumSystemVersion: $min"
for k in NSAudioCaptureUsageDescription NSMicrophoneUsageDescription; do
    [ -n "$(pl "$k")" ] || err "hiányzik: $k (enélkül a macOS nem kér engedélyt, és a hangrögzítés csendben sikertelen)"
done
exe="$app/Contents/MacOS/$(pl CFBundleExecutable)"
[ -x "$exe" ] || err "a futtatható fájl hiányzik: $exe"
[ -f "$app/Contents/Resources/$(pl CFBundleIconFile).icns" ] || err "hiányzik az ikon"
[ -f "$app/Contents/Resources/gui/gui_integrated_system.html" ] || err "hiányzik a felület (Resources/gui)"
echo "::endgroup::"

# 5. Bináris fájlok: architektúrák, minimális macOS, függőségek (egy tiszta Macen nincs Homebrew)
echo "::group::Bináris fájlok"
n=0
while IFS= read -r f; do
    file -b "$f" | grep -q "Mach-O" || continue
    n=$((n + 1))
    rel=${f#"$app"/}
    archs=$(lipo -archs "$f" 2>/dev/null)
    echo "$rel: $archs"
    for a in arm64 x86_64; do
        case " $archs " in *" $a "*) ;; *) err "$rel: hiányzik a(z) $a változat"; continue ;; esac
        minos=$(otool -l -arch "$a" "$f" | awk '/LC_BUILD_VERSION/{b=1} b&&/minos/{print $2; exit}')
        [ -n "$minos" ] || minos=$(otool -l -arch "$a" "$f" | awk '/LC_VERSION_MIN_MACOSX/{b=1} b&&/version/{print $2; exit}')
        echo "  $a: minos $minos"
        if [ -z "$minos" ] || ! ver_le "$minos" "$min"; then
            err "$rel ($a): a bináris macOS $minos-tól fut, az Info.plist $min-t ígér"
        fi
        otool -L -arch "$a" "$f" | tail -n +2 | awk '{print $1}' | while IFS= read -r dep; do
            case "$dep" in
                /System/Library/*|/usr/lib/*) ;;
                @rpath/*|@executable_path/*|@loader_path/*)
                    find "$app/Contents" -name "$(basename "$dep")" | grep -q . ||
                        echo "::error title=Telepítés::$rel ($a): a függőség nincs az alkalmazásban: $dep" ;;
                *) echo "::error title=Telepítés::$rel ($a): külső függőség, egy tiszta Macen nincs meg: $dep" ;;
            esac
        done | tee "$RUNNER_TEMP/deps.err"
        [ -s "$RUNNER_TEMP/deps.err" ] && fail=1
    done
done < <(find "$app/Contents" -type f)
[ "$n" -ge 1 ] || err "nincs futtatható bináris az alkalmazásban"
# A macOS 12-nél újabb rendszerfüggvények gyenge kötéssel kellenek, különben a program régebbi macOS-en el sem indul
for a in arm64 x86_64; do
    nm -m -arch "$a" "$exe" | grep "(undefined)" > "$RUNNER_TEMP/undef.txt"
    for s in _AudioHardwareCreateProcessTap _AudioHardwareDestroyProcessTap '_OBJC_CLASS_$_CATapDescription' '_OBJC_CLASS_$_SMAppService'; do
        line=$(grep -F " $s " "$RUNNER_TEMP/undef.txt" | head -n1)
        [ -n "$line" ] || continue
        echo "  $a: $line"
        case "$line" in *"weak external"*) ;; *) err "$a: $s nem gyenge kötésű (macOS $min alatt nem indulna)" ;; esac
    done
    # Az összes erősen kötött Objective-C osztály (az SDK nem mindegyiknél jelzi, melyik macOS-től van)
    echo "  $a: erősen kötött osztályok:" $(grep -v "weak external" "$RUNNER_TEMP/undef.txt" |
        sed -n 's/.*_OBJC_CLASS_\$_\([A-Za-z0-9_]*\) (from \([A-Za-z0-9]*\)).*/\1(\2)/p' | sort -u)
done
echo "::endgroup::"

# 6. Indítás. Az indítást figyeli: fut-e a folyamat, él-e a híd, és csatlakozik-e az ablak felülete.
gui_diag() {   # ha az ablak felülete nem csatlakozik: a WebKit folyamatai, hibajelentései és rendszernaplója
    echo "::group::$1: a felület hibakeresése"
    ps ax -o pid,stat,etime,command | grep -i -E "webkit|aqulify" | grep -v grep
    ls -lt ~/Library/Logs/DiagnosticReports 2>/dev/null | head -n 10
    log show --last 2m --style compact \
        --predicate 'process == "Aqulify" OR process BEGINSWITH "com.apple.WebKit"' 2>/dev/null |
        grep -i -E "error|fail|crash|denied|webgl|gpu|sandbox|terminat" | tail -n 80
    screencapture -x "$RUNNER_TEMP/screenshot.png" 2>/dev/null || true
    echo "::endgroup::"
}
launch() {   # $1: az alkalmazás, $2: leírás, $3: a folyamat várt útvonala
    local path=$1 what=$2 want_exe=$3 start ok=1 pid i
    start=0
    [ -f "$log" ] && start=$(wc -l < "$log" | tr -d ' ')
    open "$path" &
    opener=$!
    for i in $(seq 1 30); do nc -z 127.0.0.1 $port 2>/dev/null && break; sleep 1; done
    nc -z 127.0.0.1 $port 2>/dev/null || { err "$what: a motor hídja nem indult el ($port)"; ok=0; }
    for i in $(seq 1 30); do
        tail -n +"$((start + 1))" "$log" 2>/dev/null | grep -q "Grafikus felület csatlakozott" && break; sleep 1
    done
    tail -n +"$((start + 1))" "$log" 2>/dev/null | grep -q "Grafikus felület csatlakozott" ||
        { err "$what: az ablak felülete nem csatlakozott a motorhoz"; ok=0; gui_diag "$what"; }
    [ $ok = 1 ] && { python3 "$here/ws_status.py" $port 20 || { err "$what: a híd nem küld állapotot"; ok=0; }; }
    pid=$(pgrep -x Aqulify | head -n1)
    if [ -z "$pid" ]; then
        err "$what: az alkalmazás nem fut (összeomlott?)"; ok=0
    else
        exe_now=$(ps -o comm= -p "$pid")
        echo "$what: fut, pid $pid, $exe_now"
        case "$exe_now" in
            *AppTranslocation*) err "$what: a macOS áthelyezve futtatja (App Translocation), nem a helyéről" ;;
            "$want_exe") ;;
            *) err "$what: nem a várt helyről fut: $exe_now" ;;
        esac
    fi
    if kill -0 $opener 2>/dev/null; then err "$what: az open nem tért vissza"; kill $opener; ok=0
    elif ! wait $opener; then err "$what: az open hibát jelzett"; ok=0; fi
    echo "::group::$what: napló"; tail -n +"$((start + 1))" "$log" 2>/dev/null; echo "::endgroup::"
    return $((1 - ok))
}
quit_app() {   # $1: leírás
    killall -TERM Aqulify 2>/dev/null || true
    for i in 1 2 3 4 5 6 7 8 9 10; do pgrep -x Aqulify >/dev/null || break; sleep 1; done
    if pgrep -x Aqulify >/dev/null; then err "$1: kilépéskor nem állt le"; killall -KILL Aqulify; fi
    for i in 1 2 3 4 5; do nc -z 127.0.0.1 $port 2>/dev/null || break; sleep 1; done
}

# 6a. Első indítási kísérlet karanténnal: a Gatekeeper megállítja (nincs Apple-hitelesítés). Ilyenkor a folyamat
# már létezik, de áll, és az open a figyelmeztető ablak válaszára vár; elindulni csak a híd megnyílásából látszik.
echo "::group::Indítás karanténnal"
open "$app" &
opener=$!
for i in $(seq 1 20); do nc -z 127.0.0.1 $port 2>/dev/null && break; sleep 1; done
if nc -z 127.0.0.1 $port 2>/dev/null; then
    note "macOS $os_ver: a karanténos alkalmazás engedélyezés nélkül elindult (a CI gépen a Gatekeeper engedékenyebb lehet)"
    quit_app "karanténos indítás"
else
    held=nem; pgrep -x Aqulify >/dev/null && held=igen
    waiting=nem; kill -0 $opener 2>/dev/null && waiting=igen
    echo "a Gatekeeper megállította az első indítást (várható; a README első indítás lépései kellenek)"
    echo "  a folyamat létrejött, de áll: $held; az open a figyelmeztető ablakra vár: $waiting"
fi
kill $opener 2>/dev/null; wait $opener 2>/dev/null
killall -KILL Aqulify 2>/dev/null
killall CoreServicesUIAgent 2>/dev/null || true    # a figyelmeztető ablak (a rendszer szükség esetén újraindítja)
sleep 2
echo "::endgroup::"

# 6b. Engedélyezés után (Megnyitás mindenképp; itt: a karantén levétele), majd újraindítás a mentett beállításokkal
xattr -dr com.apple.quarantine "$app"
launch "$app" "Indítás az Alkalmazások mappából" "$exe" && screencapture -x "$RUNNER_TEMP/screenshot.png" 2>/dev/null
quit_app "első indítás"
[ -d "$HOME/Library/Application Support/Aqulify" ] || err "nem jött létre a beállítások mappája"
launch "$app" "Újraindítás" "$exe"
quit_app "újraindítás"

# 6c. Indítás közvetlenül a DMG-ről (csak olvasható kötet; az alkalmazás nem írhat a saját csomagjába)
mnt2=$(mktemp -d "$RUNNER_TEMP/mnt.XXXX")
if hdiutil attach -nobrowse -readonly -noautoopen -mountpoint "$mnt2" "$plain" >/dev/null; then
    launch "$mnt2/Aqulify.app" "Indítás a DMG-ről" "$mnt2/Aqulify.app/Contents/MacOS/$(basename "$exe")"
    quit_app "DMG-ről indítás"
    sleep 2
    hdiutil detach "$mnt2" >/dev/null || hdiutil detach -force "$mnt2" >/dev/null
else
    err "a karantén nélküli DMG nem csatolható"
fi
ls ~/Library/Logs/DiagnosticReports 2>/dev/null | grep -i aqulify && err "összeomlási jelentés készült"

# 7. Eltávolítás: az alkalmazás törlése után nem maradhat háttérszolgáltatás, illesztőprogram vagy bővítmény
echo "::group::Eltávolítás"
rm -rf "$app"
for d in ~/Library/LaunchAgents /Library/LaunchAgents /Library/LaunchDaemons /Library/Audio/Plug-Ins/HAL \
         /Library/Extensions /Library/SystemExtensions; do
    ls "$d" 2>/dev/null | grep -i -E "aqulify|szorgos" && err "maradvány: $d"
done
# A saját mappában maradó fájlok (a README eltávolítási parancsa ezeket törli)
readme="$here/../../README.md"
find ~/Library -maxdepth 3 \( -iname "*aqulify*" -o -iname "io.github.szorgos*" \) 2>/dev/null |
    sed "s|^$HOME|~|" | cut -d/ -f1-4 | sort -u > "$RUNNER_TEMP/left.txt"   # pl. ~/Library/Caches/io.github.szorgos.Aqulify
cat "$RUNNER_TEMP/left.txt"
while IFS= read -r top; do
    esc=$(printf '%s' "$top" | sed 's/ /\\ /g')        # a README-ben: ~/Library/Application\ Support/...
    grep -q -F -- "$esc" "$readme" || note "a README eltávolítási parancsa nem törli: $top"
done < "$RUNNER_TEMP/left.txt"
echo "::endgroup::"

[ $fail = 0 ] && echo "macOS $os_ver ($os_arch): a telepítés ellenőrzése rendben"
exit $fail
