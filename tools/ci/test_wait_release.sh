#!/usr/bin/env bash
# A tools/ci/wait_release.sh próbája hamis `gh`-val (C2 / S2): a kiadás (1) azonnal létezik, (2) a 3. kérdezésre
# jelenik meg, (3) soha nem jelenik meg. Kimenet: OK/HIBA soronként; kilépési kód 0, ha mind átment.
# Használat: tools/ci/test_wait_release.sh [wait_release.sh útvonala]
set -u
here="$(cd "$(dirname "$0")" && pwd)"
script="${1:-$here/wait_release.sh}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin"
# hamis gh: a kérdezések számát számolja; a FAKE_APPEAR_AT-adik kérdezéstől „létezik” (0: soha)
cat > "$tmp/bin/gh" <<'EOF'
#!/usr/bin/env bash
n=$(( $(cat "$FAKE_COUNT" 2>/dev/null || echo 0) + 1 )); echo "$n" > "$FAKE_COUNT"
[ "$1 $2" = "release view" ] || { echo "váratlan hívás: $*" >&2; exit 3; }
if [ "$FAKE_APPEAR_AT" -gt 0 ] && [ "$n" -ge "$FAKE_APPEAR_AT" ]; then echo '{"tagName":"'"$3"'"}'; exit 0; fi
echo "release not found" >&2; exit 1
EOF
chmod +x "$tmp/bin/gh"
fail=0
run_case() { # név, megjelenés, várt kód, várt kérdezésszám-tartomány (min max), max mp
  local name=$1 appear=$2 want_rc=$3 cmin=$4 cmax=$5 max=$6
  rm -f "$tmp/count"
  local s=$SECONDS out rc
  out=$(PATH="$tmp/bin:$PATH" FAKE_COUNT="$tmp/count" FAKE_APPEAR_AT=$appear AQ_WAIT_MAX=$max AQ_WAIT_STEP=1 \
        timeout 30 bash "$script" v9.9.9 o/r 2>&1); rc=$?
  local el=$((SECONDS - s)) c; c=$(cat "$tmp/count" 2>/dev/null || echo 0)
  if [ "$rc" = "$want_rc" ] && [ "$c" -ge "$cmin" ] && [ "$c" -le "$cmax" ] && [ "$el" -le $((max + 2)) ]; then
    echo "OK   $name (kód $rc, $c kérdezés, $el mp)"
  else
    echo "HIBA $name (kód $rc, várt $want_rc; $c kérdezés, várt $cmin..$cmax; $el mp)"; echo "$out" | sed 's/^/     /'; fail=1
  fi
  [ "$name" = "soha" ] && echo "$out" | grep -q "::error title=Kiadás::" && echo "OK   soha: érthető hibaüzenet" \
    || { [ "$name" = "soha" ] && { echo "HIBA soha: nincs ::error üzenet"; fail=1; }; }
}
run_case "azonnal"        1 0 1 1 5
run_case "3. kérdezésre"  3 0 3 3 5
run_case "soha"           0 1 2 8 4
exit $fail
