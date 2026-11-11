#!/usr/bin/env bash
# Az RNNoise betanított modelljét tartalmazó forráscsomag (rnnoise_0.2+ds.orig.tar.xz) letöltése több tükörről.
# Ugyanaz a fájl az Ubuntu és a Debian archívumában is megvan; bármelyik címről csak a rögzített sha256-tal
# egyező fájlt fogadjuk el, ezért a tükör megválasztása nem befolyásolja, mi kerül a programba.
# Használat: tools/fetch_rnnoise.sh <célfájl>
set -u
out="${1:?Használat: fetch_rnnoise.sh <célfájl>}"
want=db2718ee4a0b294bfbbe198fe94aabfdc8cd0d77cb9cd95d76c7a818ad805f0b
mirrors=(
  "http://archive.ubuntu.com/ubuntu/pool/universe/r/rnnoise/rnnoise_0.2+ds.orig.tar.xz"
  "http://azure.archive.ubuntu.com/ubuntu/pool/universe/r/rnnoise/rnnoise_0.2+ds.orig.tar.xz"
  "https://deb.debian.org/debian/pool/main/r/rnnoise/rnnoise_0.2+ds.orig.tar.xz"
)

sha() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum < "$1" | cut -d' ' -f1
  else shasum -a 256 < "$1" | cut -d' ' -f1; fi
}

fetch() {
  if command -v curl >/dev/null 2>&1; then curl -fsSL --connect-timeout 20 --max-time 300 -o "$2" "$1"
  else wget -q -T 20 -O "$2" "$1"; fi
}

for round in 1 2 3; do
  for url in "${mirrors[@]}"; do
    rm -f "$out"
    if fetch "$url" "$out" && [ "$(sha "$out")" = "$want" ]; then
      echo "RNNoise: $url"
      exit 0
    fi
    echo "RNNoise: nem sikerült ($url)" >&2
  done
  [ "$round" -lt 3 ] && sleep 15
done
rm -f "$out"
echo "RNNoise: egyik tükörről sem sikerült a letöltés (vagy eltért a sha256)" >&2
exit 1
