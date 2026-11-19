#!/usr/bin/env bash
# Az RNNoise betanított modelljét tartalmazó forráscsomag (rnnoise_0.2+ds.orig.tar.xz) letöltése több tükörről.
# Ugyanaz a fájl az Ubuntu és a Debian archívumában is megvan; bármelyik címről csak a rögzített sha256-tal
# egyező fájlt fogadjuk el, ezért a tükör megválasztása nem befolyásolja, mi kerül a programba.
# Használat: tools/fetch_rnnoise.sh <célfájl>
#
# Elakadás (C1, 2026-11-19): a macOS CI-ben az első tükör nem bukott el, csak csordogált (4 perc 43 mp egy
# 19 MB-os fájlra a szokásos 3–41 mp helyett). A CI ezért két változót állít be (a felhasználói telepítésnél
# – install.sh, Pi, COPR – nincsenek megadva, ott a viselkedés a korábbi, lassú kapcsolaton is működik):
#   AQ_FETCH_MIN_BPS   – ha egy próbálkozás átlagsebessége AQ_FETCH_SPEED_TIME (alap 20) mp-en át ez alatt
#                        marad (bájt/s), a próbálkozás megszakad, és a következő tükör jön;
#   AQ_FETCH_DEADLINE  – a teljes letöltés időkorlátja mp-ben (minden próbálkozással és várakozással együtt).
# Teszteléshez: AQ_RNNOISE_MIRRORS (szóközzel elválasztott címek) a beépített tükörlista helyett;
# AQ_FETCH_PAUSE (alap 15) a körök közti várakozás mp-ben.
set -u
out="${1:?Használat: fetch_rnnoise.sh <célfájl>}"
want=db2718ee4a0b294bfbbe198fe94aabfdc8cd0d77cb9cd95d76c7a818ad805f0b
if [ -n "${AQ_RNNOISE_MIRRORS:-}" ]; then
  read -r -a mirrors <<< "$AQ_RNNOISE_MIRRORS"
else
  mirrors=(
    "http://archive.ubuntu.com/ubuntu/pool/universe/r/rnnoise/rnnoise_0.2+ds.orig.tar.xz"
    "http://azure.archive.ubuntu.com/ubuntu/pool/universe/r/rnnoise/rnnoise_0.2+ds.orig.tar.xz"
    "https://deb.debian.org/debian/pool/main/r/rnnoise/rnnoise_0.2+ds.orig.tar.xz"
  )
fi
min_bps="${AQ_FETCH_MIN_BPS:-}"
speed_time="${AQ_FETCH_SPEED_TIME:-20}"
deadline="${AQ_FETCH_DEADLINE:-}"
pause="${AQ_FETCH_PAUSE:-15}"
start=$SECONDS

sha() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum < "$1" | cut -d' ' -f1
  else shasum -a 256 < "$1" | cut -d' ' -f1; fi
}

# hátralévő idő a teljes időkorlátból (mp); korlát nélkül üres
remaining() {
  [ -n "$deadline" ] || return 0
  echo $(( deadline - (SECONDS - start) ))
}

fetch() {
  local max=300 rem
  rem=$(remaining)
  if [ -n "$rem" ] && [ "$rem" -lt "$max" ]; then max=$rem; fi
  [ "$max" -gt 0 ] || return 1
  if command -v curl >/dev/null 2>&1; then
    local speed=()
    [ -n "$min_bps" ] && speed=(--speed-limit "$min_bps" --speed-time "$speed_time")
    # ${speed[@]+…}: üres tömb `set -u` mellett a macOS bash 3.2-jén is
    curl -fsSL --connect-timeout 20 --max-time "$max" ${speed[@]+"${speed[@]}"} -o "$2" "$1"
  else
    wget -q -T 20 --tries=1 -O "$2" "$1"
  fi
}

for round in 1 2 3; do
  for url in "${mirrors[@]}"; do
    rem=$(remaining)
    if [ -n "$rem" ] && [ "$rem" -le 0 ]; then break 2; fi
    rm -f "$out"
    t0=$SECONDS
    if fetch "$url" "$out" && [ "$(sha "$out")" = "$want" ]; then
      echo "RNNoise: $url ($((SECONDS - t0)) mp)"
      exit 0
    fi
    echo "RNNoise: nem sikerült ($url, $((SECONDS - t0)) mp)" >&2
  done
  if [ "$round" -lt 3 ]; then
    rem=$(remaining)
    if [ -n "$rem" ] && [ "$rem" -le "$pause" ]; then break; fi
    sleep "$pause"
  fi
done
rm -f "$out"
if [ -n "$deadline" ] && [ $((SECONDS - start)) -ge "$deadline" ]; then
  echo "RNNoise: a letöltés nem fejeződött be a megadott ${deadline} mp alatt (AQ_FETCH_DEADLINE)" >&2
else
  echo "RNNoise: egyik tükörről sem sikerült a letöltés (vagy eltért a sha256)" >&2
fi
exit 1
