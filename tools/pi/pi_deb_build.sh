#!/bin/bash
# Aqulify arm64 .deb csomag készítése Raspberry Pi OS-re (Debian 13 „trixie” alapú), a Pi-n magán
# (vagy az emulált arm64-es környezetben).
#
# Használat (a Pi-n, a tároló gyökeréből):
#   sudo apt install --no-install-recommends build-essential cmake ninja-build pkg-config xz-utils dpkg-dev \
#        fakeroot git python3 curl ca-certificates libpipewire-0.3-dev libfftw3-dev libsystemd-dev libasound2-dev \
#        libgtk-3-dev libwebkit2gtk-4.1-dev
#   bash tools/pi/pi_deb_build.sh [a motor forrásmappája, alap: a tároló gyökere] [git-ref, alap: HEAD] [kimeneti mappa]
#
# Mit csinál: a megadott ref tiszta másolatából (git archive) a meglévő debian/ csomagolással épít
# (dpkg-buildpackage -b: fordítás, a közös tesztek, telepítés, .deb). A beszéd-zajcsökkentés betanított
# RNNoise-modellje nincs a tárolóban: a szkript a tools/fetch_rnnoise.sh-val tölti le (rögzített sha256), mint az
# asztali csomagok. Ha a gépen nincs curl / wget, AQ_RNNOISE_TAR=<előre letöltött rnnoise_0.2+ds.orig.tar.xz>;
# a modell nélküli csomag csak AQ_NO_RNNOISE=1 megadásával épül.
# Csomagrevízió (ugyanannak a verziónak az újraépítése, hogy az apt frissítésnek lássa): AQ_PI_REV=2 → 2.3.0-2~pi1.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SRC="${1:-$REPO_ROOT}"
REF="${2:-HEAD}"
OUT="${3:-$PWD/pi-deb}"
WORK="$(mktemp -d /tmp/aq-pi-build.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

[ "$(dpkg --print-architecture)" = "arm64" ] || echo "FIGYELEM: nem arm64-es rendszer ($(dpkg --print-architecture))" >&2
git -C "$SRC" archive --format=tar "$REF" | tar -x -C "$WORK"
echo "Forrás: $(git -C "$SRC" rev-parse --short "$REF") -> $WORK"
cd "$WORK"
# RNNoise-modell (a debian/rules a debian/rnnoise_0.2+ds.orig.tar.xz-ből bontja ki)
RNN_TAR="debian/rnnoise_0.2+ds.orig.tar.xz"
if [ "${AQ_NO_RNNOISE:-0}" = "1" ]; then
  echo "FIGYELEM: AQ_NO_RNNOISE=1 – a csomag RNNoise-modell (beszéd-zajcsökkentés) nélkül épül"
elif [ -n "${AQ_RNNOISE_TAR:-}" ]; then
  [ "$(sha256sum < "$AQ_RNNOISE_TAR" | cut -d' ' -f1)" = "db2718ee4a0b294bfbbe198fe94aabfdc8cd0d77cb9cd95d76c7a818ad805f0b" ] \
    || { echo "HIBA: az AQ_RNNOISE_TAR sha256-ja nem a várt"; exit 1; }
  cp "$AQ_RNNOISE_TAR" "$RNN_TAR"
  echo "RNNoise: $AQ_RNNOISE_TAR"
else
  bash "$SCRIPT_DIR/../fetch_rnnoise.sh" "$RNN_TAR" \
    || { echo "HIBA: az RNNoise-modell nem tölthető le (AQ_RNNOISE_TAR=<fájl> vagy AQ_NO_RNNOISE=1)"; exit 1; }
fi
# A nyilvános debian/changelog lemaradhat a motor verziójától (a debian/rules ellenőrzi az egyezést):
# a munkamásolatban a csomag verziója a CMakeLists.txt verziója lesz, „~pi1” utótaggal, a mostani szerzőcímmel.
AUTHOR="Szorgos <291709182+Szorgos@users.noreply.github.com>"
CMV="$(sed -n '1,10s/^ *VERSION \([0-9][0-9.]*\)$/\1/p' CMakeLists.txt | head -1)"
[ -n "$CMV" ] || { echo "HIBA: a CMakeLists.txt verziója nem olvasható"; exit 1; }
REV="${AQ_PI_REV:-1}"
[[ "$REV" =~ ^[1-9][0-9]*$ ]] || { echo "HIBA: az AQ_PI_REV pozitív egész legyen"; exit 1; }
{ printf 'aqulify (%s-%s~pi1) unstable; urgency=medium\n\n  * Raspberry Pi (arm64) csomag a %s forrásból.\n\n -- %s  %s\n\n' \
    "$CMV" "$REV" "$(git -C "$SRC" rev-parse --short "$REF")" "$AUTHOR" "$(date -R)"; cat debian/changelog; } > debian/changelog.new
mv debian/changelog.new debian/changelog
sed -i "s|^Maintainer: .*|Maintainer: $AUTHOR|" debian/control
# Emulált (QEMU) környezetben a test_dsp valós idejű CPU-mérése értelmetlen (az emuláció sokszor lassabb):
# AQ_EMULALT=1 esetén a csomagkészítés ezt az egy tesztet kihagyja. A valódi Pi-n ne add meg: ott a mérés
# mutatja meg, mit bír a Pi.
if [ "${AQ_EMULALT:-0}" = "1" ]; then
  sed -i 's/test_core test_dsp test_matrix/test_core test_matrix/; s/ test_hires_b;/;/' debian/rules
  grep -q 'test_core test_matrix' debian/rules || { echo "HIBA: a debian/rules tesztlistája nem a várt"; exit 1; }
  echo "FIGYELEM: emulált környezet – a test_dsp és a test_hires_b (valós idejű sebességmérés) kihagyva"
fi
# Választható, visszavonható rendszerhangoló szkript (aqulify-tune) és eltávolításkori visszaállítás (prerm):
cp "$SCRIPT_DIR/aqulify-tune.sh" debian/aqulify-tune.sh
cat > debian/prerm <<'EOF'
#!/bin/sh
set -e
case "$1" in
    remove|deconfigure)
        if [ -f "${AQ_SYSROOT:-}/var/lib/aqulify-tune/state" ] && command -v aqulify-tune >/dev/null 2>&1; then
            aqulify-tune disable >/dev/null 2>&1 || true
        fi
        ;;
esac
exit 0
EOF
chmod 755 debian/aqulify-tune.sh debian/prerm
sed -i 's|\(\tinstall -m 755 debian/postrm \$(D)/DEBIAN/postrm\)|\1\n\tinstall -m 755 debian/aqulify-tune.sh $(D)/usr/bin/aqulify-tune\n\tinstall -m 755 debian/prerm $(D)/DEBIAN/prerm|' debian/rules
grep -Fq $'\tinstall -m 755 debian/aqulify-tune.sh $(D)/usr/bin/aqulify-tune' debian/rules || { echo "HIBA: az aqulify-tune telepítése nem került be a debian/rules fájlba"; exit 1; }
grep -Fq $'\tinstall -m 755 debian/prerm $(D)/DEBIAN/prerm' debian/rules || { echo "HIBA: a debian/prerm telepítése nem került be a debian/rules fájlba"; exit 1; }
start=$(date +%s)
mkdir -p "$OUT"
rc=0
dpkg-buildpackage -us -uc -b > "$OUT/build.log" 2>&1 || rc=$?
grep -E "FIGYELEM|MINDEN TESZT|sikertelen|HIBA|error:" "$OUT/build.log" | tail -20 || true
[ "$rc" -eq 0 ] || { echo "HIBA: a csomagkészítés nem sikerült (kód: $rc, napló: $OUT/build.log)"; exit 1; }
if [ "${AQ_NO_RNNOISE:-0}" != "1" ] && grep -q "^FIGYELEM: nincs RNNoise-modell" "$OUT/build.log"; then
  echo "HIBA: a modell megvolt, mégis RNNoise nélkül épült (napló: $OUT/build.log)"; exit 1
fi
# a dpkg-deb a munkamappa szülőjébe teszi a csomagot
VER="$(dpkg-parsechangelog -S Version)"
mv "$(dirname "$WORK")/aqulify_${VER}_arm64.deb" "$OUT/"
echo "Kész ($(( $(date +%s) - start )) mp): $(ls "$OUT"/*.deb)"
