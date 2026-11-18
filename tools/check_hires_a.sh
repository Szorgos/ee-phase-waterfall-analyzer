#!/usr/bin/env bash
# Nagy felbontás, A rész (352,8–768 kHz, DSP-korlát, két szálas felmintavételezés, terhelés-tanácsadó,
# dedikált mag) – ellenőrző szkript. Friss, normál klónból is futtatható (Linux / WSL, PipeWire kell az élő részhez).
# Használat:
#   AQ_CHECK_SEED=12345 bash tools/check_hires_a.sh     (teljes: egységtesztek, élő teszt, mutánsok)
#   bash tools/check_hires_a.sh --quick                  (élő teszt és mutánsok nélkül)
set -u
QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "${AQ_REPO:-$SCRIPT_DIR/..}" && pwd)"
# védelem: csak az Aqulify tároló gyökerében fut (máshonnan indítva nem másol semmit)
if [ ! -f "$REPO/CMakeLists.txt" ] || [ ! -f "$REPO/tools/check_hires_a.sh" ] || [ ! -f "$REPO/include/audio_options.hpp" ]; then
    echo "HIBA: a tároló gyökere nem található ($REPO). Indítsd a tárolóból, vagy add meg: AQ_REPO=<tároló>"; exit 2
fi
export AQ_CHECK_SEED="${AQ_CHECK_SEED:-$(date +%s)}"

# git a WSL-ből Windowsos munkafára is (a .git ott fájl, Windowsos útvonallal)
if git -C "$REPO" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    repo_git() { git -C "$REPO" "$@"; }
elif [ -f "$REPO/.git" ] && command -v wslpath >/dev/null 2>&1; then
    UNIX_GITDIR="$(wslpath -u "$(sed 's/^gitdir:[[:space:]]*//; s/\r$//' "$REPO/.git")")"
    repo_git() { GIT_DIR="$UNIX_GITDIR" GIT_WORK_TREE="$REPO" git "$@"; }
else
    repo_git() { git -C "$REPO" "$@"; }
fi
# Alap: a felmintavételezés beolvasztása (az A rész erre épül)
BASE_REF="$(repo_git merge-base HEAD origin/master 2>/dev/null || echo 5587bd0)"

WORK="$(mktemp -d /tmp/aq_hires_a_XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
SRC="$WORK/src"; BUILD="$WORK/build"; MUT_SRC="$WORK/mut_src"; MUT_BUILD="$WORK/mut_build"
RESULTS_JSON="$REPO/hires_a_results.json"
SUMMARY="$WORK/summary.tsv"; METRICS="$WORK/metrics.jsonl"; MUTLOG="$WORK/mutants.tsv"
: > "$SUMMARY"; : > "$METRICS"; : > "$MUTLOG"
FAIL=0
UNIT_TESTS="test_hires test_upsample test_core test_player test_dsp test_dsd test_alsa test_multiout test_matrix test_room"

echo "========================================================"
echo " Nagy felbontás A rész ellenőrzése (AQ_CHECK_SEED=$AQ_CHECK_SEED, quick=$QUICK)"
echo " Alap: $BASE_REF | Munkamappa: $WORK"
echo "========================================================"

step() { local name="$1"; shift; local t0 t1 out rc; t0=$(date +%s); out="$WORK/out_${name//[^a-zA-Z0-9_]/_}.log"
    "$@" > "$out" 2>&1; rc=$?; t1=$(date +%s)
    grep '^K_METRIC:' "$out" >> "$METRICS" || true
    if [ $rc -eq 0 ]; then printf "%s\tOK\t%d\n" "$name" $((t1-t0)) >> "$SUMMARY"; echo "  [OK]   $name ($((t1-t0)) s)"
    else printf "%s\tHIBA\t%d\n" "$name" $((t1-t0)) >> "$SUMMARY"; echo "  [HIBA] $name – részletek:"; tail -n 30 "$out"; FAIL=1; fi; }

# 1. Tiszta másolat és fordítás
echo "[1/6] Tiszta fordítás..."
mkdir -p "$SRC"
tar -C "$REPO" --exclude=.git --exclude='build*' -cf - . | tar -C "$SRC" -xf -
find "$SRC" -type f \( -name '*.sh' -o -name '*.py' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.c' -o -name '*.h' \) -exec sed -i 's/\r$//' {} +
t0=$(date +%s)
if cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release > "$WORK/build.log" 2>&1 && \
   nice -n 5 cmake --build "$BUILD" -j6 --target $UNIT_TESTS aqulify-engine >> "$WORK/build.log" 2>&1; then
    printf "build\tOK\t%d\n" $(($(date +%s)-t0)) >> "$SUMMARY"; echo "  [OK]   fordítás ($(($(date +%s)-t0)) s)"
else
    tail -n 30 "$WORK/build.log"; echo "  [HIBA] a fordítás sikertelen"; exit 1
fi

# 2. Egységtesztek (az összes meglévő csomag: a régi viselkedés változatlan)
echo "[2/6] Egységtesztek..."
for t in $UNIT_TESTS; do step "$t" "$BUILD/$t"; done
step "i18n_check" python3 "$SRC/tools/i18n_build.py" --check

# 3. Tiltott minták a változásban
echo "[3/6] Tiltott minták..."
ok=1
repo_git diff -U0 "$BASE_REF"..HEAD -- include/ src/ windows/ macos/ gui_integrated_system.html \
    ':!src/test_hires.cpp' | grep '^+' | grep -v '^+++' > "$WORK/prod.diff" || true
if grep -nE '\b(TODO|FIXME|stub|fake|Math\.random)\b' "$WORK/prod.diff"; then echo "  [HIBA] tiltott minta a termékkódban"; ok=0; fi
repo_git diff -U0 "$BASE_REF"..HEAD -- src/test_hires.cpp tests/live/hires_live.py | grep '^+' | grep -v '^+++' > "$WORK/test.diff" || true
if grep -nE '\bskip\b|\bSKIP\b|#if 0|if \(false\)|// *CHECK' "$WORK/test.diff"; then echo "  [HIBA] kikapcsolt teszt"; ok=0; fi
repo_git diff -U0 "$BASE_REF"..HEAD -- gui_integrated_system.html i18n/gui/ docs/ | grep '^+' | grep -v '^+++' > "$WORK/gui.diff" || true
if grep -inE '\b(foobar2000|hqplayer|audirvana|roon|jriver|moode|volumio|deadbeef|clementine|aimp|winamp)\b' "$WORK/gui.diff"; then
    echo "  [HIBA] más lejátszóprogram neve a felületen / dokumentációban"; ok=0; fi
# magyar ékezetes szöveg i18n nélkül a felület kódjában
repo_git diff -U0 "$BASE_REF"..HEAD -- gui_integrated_system.html | grep '^+' | grep -v '^+++' > "$WORK/html.diff" || true
if python3 - "$WORK/html.diff" <<'PY'
import re, sys
bad = []
for raw in open(sys.argv[1], encoding="utf-8"):
    line = raw[1:].strip()
    if not line or line.startswith(("//", "/*", "<!--", "*")) or "data-i18n" in line or 't("' in line:
        continue
    code = line.split("//")[0]
    if re.search(r"[áéíóöőúüűÁÉÍÓÖŐÚÜŰ]", code):
        bad.append(code[:100])
if bad:
    print("  [HIBA] i18n nélküli magyar szöveg:", bad[:5]); sys.exit(1)
PY
then :; else ok=0; fi
if [ $ok -eq 1 ]; then printf "forbidden\tOK\t0\n" >> "$SUMMARY"; echo "  [OK]   tiltott minták: 0 találat"
else printf "forbidden\tHIBA\t0\n" >> "$SUMMARY"; FAIL=1; fi

if [ $QUICK -eq 0 ]; then
    # 4. Élő PipeWire-teszt (a jel a mag szerinti álvéletlen zaj) + a felmintavételezés élő tesztje (regresszió)
    echo "[4/6] Élő PipeWire-tesztek..."
    step "live_hires" bash "$SRC/tests/live/multiout_live.sh" "$BUILD" python3 -I "$SRC/tests/live/hires_live.py"
    step "live_upsample" bash "$SRC/tests/live/multiout_live.sh" "$BUILD" python3 -I "$SRC/tests/live/upsample_live.py"

    # 5. Mutánsok: mindegyiket meg kell ölnie a fájlnévben megnevezett tesztnek
    echo "[5/6] Mutánsok..."
    mkdir -p "$MUT_SRC"; tar -C "$SRC" -cf - . | tar -C "$MUT_SRC" -xf -
    cmake -S "$MUT_SRC" -B "$MUT_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
    for patch in "$SRC"/tools/mutants_hires/H*.patch; do
        m="$(basename "$patch" .patch)"; killer="${m#*_}"; killer="${killer%%__*}"
        if ! (cd "$MUT_SRC" && patch -p1 --silent < "$patch"); then
            echo "  [HIBA] $m nem alkalmazható"; printf "%s\tAPPLY_ERROR\t-\n" "$m" >> "$MUTLOG"; FAIL=1; continue; fi
        res="SURVIVED"
        if [ "$killer" = "live" ]; then
            if ! nice -n 5 cmake --build "$MUT_BUILD" -j6 --target aqulify-engine > "$WORK/mut.log" 2>&1; then res="BUILD_ERROR"
            elif ! bash "$MUT_SRC/tests/live/multiout_live.sh" "$MUT_BUILD" python3 -I "$MUT_SRC/tests/live/hires_live.py" > "$WORK/mut_run.log" 2>&1; then res="KILLED"; fi
        else
            if ! nice -n 5 cmake --build "$MUT_BUILD" -j6 --target "$killer" > "$WORK/mut.log" 2>&1; then res="BUILD_ERROR"
            elif ! timeout 300 "$MUT_BUILD/$killer" > "$WORK/mut_run.log" 2>&1; then res="KILLED"; fi
        fi
        (cd "$MUT_SRC" && patch -R -p1 --silent < "$patch") || { echo "  [HIBA] $m visszaállítása sikertelen"; FAIL=1; }
        printf "%s\t%s\t%s\n" "$m" "$res" "$killer" >> "$MUTLOG"
        if [ "$res" = "KILLED" ]; then echo "  [OK]   $m -> megölte: $killer"
        else echo "  [HIBA] $m: $res ($killer)"; FAIL=1; fi
    done
    [ "$(wc -l < "$MUTLOG")" -ge 8 ] || { echo "  [HIBA] 8-nál kevesebb mutáns futott"; FAIL=1; }
fi

# 6. Eredményfájl és összegzés
echo "[6/6] Összegzés..."
python3 - "$SUMMARY" "$METRICS" "$MUTLOG" "$RESULTS_JSON" "$AQ_CHECK_SEED" "$QUICK" "$FAIL" "$(repo_git rev-parse HEAD 2>/dev/null)" <<'PY'
import json, sys
s, met, mut, out, seed, quick, fail, head = sys.argv[1:9]
tests = [dict(zip(("name", "status", "seconds"), l.rstrip("\n").split("\t"))) for l in open(s, encoding="utf-8") if l.strip()]
metrics = {}
for l in open(met, encoding="utf-8"):
    if l.startswith("K_METRIC:"):
        try:
            o = json.loads(l[9:]); metrics.setdefault(o.pop("test", "?"), []).append(o)
        except Exception:
            pass
mutants = [dict(zip(("name", "status", "killer"), l.rstrip("\n").split("\t"))) for l in open(mut, encoding="utf-8") if l.strip()]
json.dump({"seed": seed, "head": head, "quick": quick == "1", "passed": fail == "0", "tests": tests,
           "metrics": metrics, "mutants": mutants}, open(out, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
print(f"{'Teszt':<22} | Állapot | Idő (s)")
for t in tests:
    print(f"{t['name']:<22} | {t['status']:<7} | {t['seconds']}")
for m in mutants:
    print(f"  {m['name']:<52} {m['status']} ({m['killer']})")
print("Eredményfájl:", out)
PY
echo
[ $FAIL -eq 0 ] && echo "MINDEN ELLENŐRZÉS SIKERES" || echo "ELLENŐRZÉS SIKERTELEN"
exit $FAIL
