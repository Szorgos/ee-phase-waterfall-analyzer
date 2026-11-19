#!/usr/bin/env bash
# A pi-deb.yml „upload” feladata a feltöltés előtt megvárja, hogy a kiadás (GitHub Release) létezzen (C2).
# Ok (2026-11-18, v2.6.1): a kiadás piszkozatként jött létre (21:52), a Pi-csomag 21:58-kor készült el, a
# `gh release upload` „release not found” hibával bukott, a kiadás csak 22:13-kor lett közzétéve. A piszkozatot
# a címke szerinti keresés nem látja, ezért a közzétételig kell várni.
#
# Használat: tools/ci/wait_release.sh <címke> [owner/tároló]
#   AQ_WAIT_MAX   – legfeljebb ennyi mp-ig vár (alap 1200 = 20 perc), utána érthető hibaüzenettel kilép (1)
#   AQ_WAIT_STEP  – ennyi mp-enként próbálja újra (alap 30)
# Ha a kiadás azonnal létezik, rögtön 0-val tér vissza (a viselkedés a korábbival azonos).
set -u
tag="${1:?Használat: wait_release.sh <címke> [owner/tároló]}"
repo="${2:-${GITHUB_REPOSITORY:-}}"
max="${AQ_WAIT_MAX:-1200}"
step="${AQ_WAIT_STEP:-30}"
case "$max$step" in *[!0-9]*) echo "::error title=Kiadás::AQ_WAIT_MAX és AQ_WAIT_STEP egész mp legyen" >&2; exit 2 ;; esac
[ "$step" -gt 0 ] || step=1
args=(release view "$tag" --json tagName)
[ -n "$repo" ] && args+=(-R "$repo")

start=$SECONDS
last=""
while :; do
  if last=$(gh "${args[@]}" 2>&1 >/dev/null); then
    echo "Kiadás: $tag létezik ($((SECONDS - start)) mp várakozás után)"
    exit 0
  fi
  waited=$((SECONDS - start))
  if [ "$waited" -ge "$max" ]; then
    echo "::error title=Kiadás::a(z) $tag kiadás $max mp után sem létezik (vagy még piszkozat). Tedd közzé a kiadást, majd futtasd újra ezt a feladatot. Utolsó válasz: ${last:-–}" >&2
    exit 1
  fi
  echo "Kiadás: $tag még nincs ($waited/$max mp; ${last:-–}); újra $step mp múlva"
  rem=$((max - waited))
  sleep $(( step < rem ? step : rem ))
done
