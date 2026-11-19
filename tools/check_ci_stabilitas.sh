#!/usr/bin/env bash
# A CI-stabilitási kör (2026-11-19) ellenőrzése: friss klónból is fut (Linux / WSL; cmake, ninja, g++, python3).
#
#   S1  apt-újrapróbálás minden munkafolyamatban, a csomaglista bájtra azonos a BASE-zel; RNNoise-letöltési korlátok
#   S2  a Pi-feltöltés megvárja a kiadást: tools/ci/test_wait_release.sh hamis gh-val (azonnal / később / soha)
#   S3  test_multiout 7.3: AQ_CHECK_SEED (megadott vagy véletlen, kiírja) + a CI-ben bukott magok
#       (1795089109, 1795030579) + a helyben talált 1067; teljes módban további magok
#   S4  a test_measure_live busy()-várakozása a helyén (az élő 50-es futás a jelentésben, ide túl hosszú)
#   S5  tools/check_test_lists.py (a docs/tesztek.md táblázat és a listák)
#   tiltott minták a BASE..HEAD különbségben (tools/ci/check_ci_static.py forbidden)
#   mutánsok (X1–X6): a fa egy másolatába beépített szándékos hibát a megfelelő ellenőrzésnek jeleznie kell
#
# Használat: tools/check_ci_stabilitas.sh [--quick]
#   --quick            csak a három ismert mag, további magok nélkül
#   AQ_CHECK_SEED=<n>  a 7.3 véletlen magja (alapból időalapú; mindig kiírja)
#   AQ_BASE=<ref>      az összevetés alapja (alap: origin/master)
#   AQ_EXTRA_SEEDS=<n> teljes módban ennyi további mag (alap 20; a jelentés 500-as futása külön)
# Eredmény: ci_stabilitas_results.json (a tároló gyökerében), kilépési kód 0 = minden zöld, minden mutáns megölve.
set -u
QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BASE="${AQ_BASE:-origin/master}"
SEED="${AQ_CHECK_SEED:-$(date +%s)}"
EXTRA="${AQ_EXTRA_SEEDS:-20}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
OUT="$ROOT/ci_stabilitas_results.json"
results=()
fails=0
t_start=$SECONDS

# git: WSL-ből Windowsos munkapéldányhoz a git.exe kell (a .git fájl Windowsos útvonalra mutat)
GITDIR="$ROOT"
if ! git -C "$ROOT" rev-parse HEAD >/dev/null 2>&1 && command -v git.exe >/dev/null 2>&1; then
  export AQ_GIT="git.exe"; GITDIR="$(wslpath -w "$ROOT")"
fi
G() { ${AQ_GIT:-git} -C "$GITDIR" "$@"; }
G rev-parse --verify --quiet "$BASE^{commit}" >/dev/null || { echo "HIBA: az alap ($BASE) nem található; AQ_BASE=<ref>"; exit 2; }
HEAD_SHA=$(G rev-parse --short HEAD)

rec() { # név, ok(0/1), részlet
  local d=${3//\\/\\\\}; d=${d//\"/\\\"}
  results+=("{\"name\":\"$1\",\"ok\":$([ "$2" = 0 ] && echo true || echo false),\"detail\":\"$d\"}")
  if [ "$2" = 0 ]; then echo "  [OK]   $1: $3"; else echo "  [HIBA] $1: $3"; fails=$((fails+1)); fi
}

echo "CI-stabilitás ellenőrzése: HEAD $HEAD_SHA, alap $BASE, $([ $QUICK = 1 ] && echo gyors || echo teljes) mód"
echo "AQ_CHECK_SEED=$SEED"

# ---------------------------------------------------------------- fordítás (csak a test_multiout)
build_multiout() { # forrás, build-mappa
  cmake -B "$2" -S "$1" -G Ninja -DCMAKE_BUILD_TYPE=Release > "$2.conf.log" 2>&1 &&
  cmake --build "$2" -j"$(nproc)" --target test_multiout > "$2.build.log" 2>&1
}
echo "[fordítás] test_multiout"
build_multiout "$ROOT" "$WORK/build" || { rec build 1 "a test_multiout nem fordul (napló: $WORK/build.build.log)"; tail -20 "$WORK/build.build.log"; }

# ---------------------------------------------------------------- statikus ellenőrzések (S1, S2 helye, C3/C4 jelenlét, tiltott minták)
echo "[statikus] tools/ci/check_ci_static.py"
static_out=$(python3 -I "$ROOT/tools/ci/check_ci_static.py" --root "$ROOT" --git "$GITDIR" --base "$BASE")
while IFS= read -r l; do
  n=$(echo "$l" | awk '{print $2}' | tr -d ':'); st=$(echo "$l" | awk '{print $1}')
  rec "static_$n" $([ "$st" = OK ] && echo 0 || echo 1) "${l#* }"
done <<< "$static_out"

# ---------------------------------------------------------------- S2: várakozás a kiadásra, hamis gh-val
echo "[S2] tools/ci/test_wait_release.sh"
s2=$(bash "$ROOT/tools/ci/test_wait_release.sh" 2>&1); rc=$?
echo "$s2" | sed 's/^/       /'
rec S2_wait_release $rc "$(echo "$s2" | grep -c '^OK') OK sor, kód $rc"

# ---------------------------------------------------------------- S5: tesztlisták
echo "[S5] tools/check_test_lists.py"
s5=$(python3 -I "$ROOT/tools/check_test_lists.py" --root "$ROOT" 2>&1); rc=$?
echo "$s5" | sed 's/^/       /'
rec S5_test_lists $rc "$(echo "$s5" | head -1)"

# ---------------------------------------------------------------- S3: 7.3 magokkal
seed_run() { # bináris, mag -> "passed max_err rc"
  local o; o=$(AQ_CHECK_SEED=$2 "$1" 2>&1); local rc=$?
  local m; m=$(echo "$o" | grep -o '"seed":[0-9]*,"passed":[0-9]*,"max_err_db":[0-9.]*' | head -1)
  echo "$(echo "$m" | sed 's/.*"passed":\([0-9]*\).*/\1/') $(echo "$m" | sed 's/.*"max_err_db":\([0-9.]*\).*/\1/') $rc"
}
if [ -x "$WORK/build/test_multiout" ]; then
  seeds="$SEED 1795089109 1795030579 1067"
  [ $QUICK = 0 ] && for i in $(seq 1 "$EXTRA"); do seeds="$seeds $((SEED + i * 7919))"; done
  echo "[S3] test_multiout 7.3, magok: $seeds"
  worst=0; bad=""; cnt=0
  for s in $seeds; do
    read -r p e rc <<< "$(seed_run "$WORK/build/test_multiout" "$s")"
    cnt=$((cnt+1))
    echo "       mag $s: $p/50, max $e dB, kód $rc"
    [ "$p" = 50 ] && [ "$rc" = 0 ] || bad="$bad $s"
    worst=$(python3 -c "print(max($worst, ${e:-99}))")
  done
  rec S3_seeds $([ -z "$bad" ] && echo 0 || echo 1) "$cnt mag, legrosszabb eltérés $worst dB (határ 0,05)${bad:+; bukott:$bad}"
fi

# ---------------------------------------------------------------- mutánsok
echo "[mutánsok] a HEAD másolatán (munkapéldány-változások nélkül: commitolt állapot)"
mut_tree() { # név -> a fa másolata $WORK/m_<név>
  local d="$WORK/m_$1"; mkdir -p "$d"
  G archive HEAD | tar -x -C "$d"
  echo "$d"
}
kill_rec() { # mutáns, megölve(0/1), részlet
  rec "mutant_$1" "$2" "$3"
}
static_on() { # fa, ellenőrzés -> kód
  python3 -I "$ROOT/tools/ci/check_ci_static.py" --root "$1" --git "$GITDIR" --base "$BASE" "$2" > "$1.static.log" 2>&1
}

# X1: a 7.3 határa 0,1 dB -> a statikus c3 ellenőrzésnek tiltott változásként kell jeleznie
d=$(mut_tree X1); sed -i 's/if (err > 0.05) caseOk = false;/if (err > 0.1) caseOk = false;/; s/worstGlobalErr <= 0.05/worstGlobalErr <= 0.1/' "$d/src/test_multiout.cpp"
static_on "$d" c3; rc=$?; kill_rec X1_hatar_0_1dB $([ $rc -ne 0 ] && echo 0 || echo 1) "$(tail -1 "$d.static.log" | cut -c1-120)"

# X2: a C3 javítása visszaállítva (fix 1,5 s, csak impulzus) -> a bukott maggal piros
d=$(mut_tree X2)
python3 -I - "$d/src/test_multiout.cpp" <<'PY'
import sys
p = sys.argv[1]; t = open(p, encoding="utf-8").read()
t = t.replace("const size_t len = impulseLenForDecay(cEq, targetWay, sr, 200.0, 1.5, 10.0);", "const size_t len = static_cast<size_t>(sr * 1.5);")
t = t.replace("deep ? toneGainDb(cEq, targetWay, sr, f, len) : db(", "false ? 0.0 : db(")
open(p, "w", encoding="utf-8").write(t)
PY
if build_multiout "$d" "$WORK/build_X2"; then
  read -r p e rc <<< "$(seed_run "$WORK/build_X2/test_multiout" 1795089109)"
  kill_rec X2_c3_visszaallitva $([ "$rc" != 0 ] && echo 0 || echo 1) "mag 1795089109: $p/50, max $e dB, kód $rc"
else
  kill_rec X2_c3_visszaallitva 1 "a mutáns nem fordult (nem ölhető így)"
fi

# X3: a T4-javítás (busy()-várakozás) kivéve a test_measure_live-ból -> a statikus c4 ellenőrzés jelzi.
# (A konvolver-teszt [T5] okát nem sikerült bizonyítani, ott nincs javítás, amit vissza lehetne állítani.)
d=$(mut_tree X3); sed -i 's/const double waitedMs = waitNotBusy(s, 10.0);/const double waitedMs = 0.0;/' "$d/src/test_measure_live.cpp"
static_on "$d" c4; rc=$?; kill_rec X3_t4_varakozas_kiveve $([ $rc -ne 0 ] && echo 0 || echo 1) "$(tail -1 "$d.static.log" | cut -c1-120)"

# X4: a C2 várakozás végtelen (nincs időkorlát) -> a hamis gh-s próba „soha” esete bukik
d=$(mut_tree X4); sed -i 's/if \[ "$waited" -ge "$max" \]; then/if false; then/; s/sleep $(( step < rem ? step : rem ))/sleep "$step"/' "$d/tools/ci/wait_release.sh"
bash "$ROOT/tools/ci/test_wait_release.sh" "$d/tools/ci/wait_release.sh" > "$d.s2.log" 2>&1; rc=$?
kill_rec X4_c2_vegtelen $([ $rc -ne 0 ] && echo 0 || echo 1) "$(grep -m1 'HIBA' "$d.s2.log" | cut -c1-120)"

# X5: a debian/rules listájából indok nélkül kimarad egy teszt -> check_test_lists.py jelzi
d=$(mut_tree X5); sed -i 's/ test_room test_hires_b/ test_hires_b/' "$d/debian/rules"
python3 -I "$ROOT/tools/check_test_lists.py" --root "$d" > "$d.s5.log" 2>&1; rc=$?
kill_rec X5_debian_lista $([ $rc -ne 0 ] && echo 0 || echo 1) "$(grep -m1 'HIBA' "$d.s5.log" | cut -c1-120)"

# X6: egy apt-get helyről kimarad az újrapróbálás -> a statikus apt ellenőrzés jelzi
d=$(mut_tree X6); sed -i '0,/sudo apt-get \$APT_NET update/s//sudo apt-get update/' "$d/.github/workflows/release.yml"
static_on "$d" apt; rc=$?; kill_rec X6_apt_ujraprobalas $([ $rc -ne 0 ] && echo 0 || echo 1) "$(tail -1 "$d.static.log" | cut -c1-120)"

# X7: az RNNoise-letöltés CI-korlátja kimarad a macos.yml-ből -> a statikus rnnoise ellenőrzés jelzi
d=$(mut_tree X7); sed -i '/AQ_FETCH_MIN_BPS:/d' "$d/.github/workflows/macos.yml"
static_on "$d" rnnoise; rc=$?; kill_rec X7_rnnoise_korlat $([ $rc -ne 0 ] && echo 0 || echo 1) "$(tail -1 "$d.static.log" | cut -c1-120)"

# ---------------------------------------------------------------- eredmény
dur=$((SECONDS - t_start))
{
  printf '{"head":"%s","base":"%s","seed":%s,"quick":%s,"seconds":%s,"failed":%s,"checks":[' \
    "$HEAD_SHA" "$BASE" "$SEED" "$([ $QUICK = 1 ] && echo true || echo false)" "$dur" "$fails"
  (IFS=,; printf '%s' "${results[*]}")
  printf ']}\n'
} > "$OUT"
python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$OUT" || { echo "HIBA: az eredményfájl nem érvényes JSON"; fails=$((fails+1)); }
echo
echo "Eredmény: $OUT ($dur s)"
if [ $fails -eq 0 ]; then echo "MINDEN ELLENŐRZÉS ZÖLD, MINDEN MUTÁNS MEGÖLVE"; else echo "SIKERTELEN: $fails tétel"; fi
exit $([ $fails -eq 0 ] && echo 0 || echo 1)
