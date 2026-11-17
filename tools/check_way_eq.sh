#!/usr/bin/env bash
# Útonkénti EQ teljes, csalásbiztos ellenőrző szkript (K17, K18, 7.1–7.6).
# Használat:
#   AQ_CHECK_SEED=12345 bash tools/check_way_eq.sh          (teljes futtatás)
#   bash tools/check_way_eq.sh --quick                      (gyors mód: mutánsok és élő tesztek nélkül)
set -u

QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
export AQ_CHECK_SEED="${AQ_CHECK_SEED:-$(date +%s)}"

BASE_REF="v2.5.0"
if ! git -C "$REPO" rev-parse --verify --quiet "$BASE_REF" >/dev/null 2>&1; then
    BASE_REF="992bee2"
fi

if [ -z "${NODE_PATH:-}" ] && command -v node >/dev/null 2>&1; then
    if ! node -e "require('playwright')" >/dev/null 2>&1 && [ -d /home/gergo/aq-pw/node_modules ]; then
        export NODE_PATH=/home/gergo/aq-pw/node_modules
    fi
fi

WORK="$(mktemp -d /tmp/aq_way_eq_XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

SRC="$WORK/src"
BUILD="$WORK/build"
MUT_SRC="$WORK/mut_src"
MUT_BUILD="$WORK/mut_build"
RESULTS_JSON="$REPO/way_eq_results.json"
METRICS_LOG="$WORK/metrics.jsonl"
SUMMARY_LOG="$WORK/summary.tsv"
MUTANTS_LOG="$WORK/mutants.tsv"
: > "$METRICS_LOG"
: > "$SUMMARY_LOG"
: > "$MUTANTS_LOG"

FAIL=0

echo "========================================================"
echo " Aqulify útonkénti EQ ellenőrzés (AQ_CHECK_SEED=$AQ_CHECK_SEED, quick=$QUICK)"
echo " Alap referencia: $BASE_REF | Munkamappa: $WORK"
echo "========================================================"

# 1. Tiszta munkamásolat és fordítás átmeneti mappába
mkdir -p "$SRC"
tar -C "$REPO" --exclude=.git --exclude='build*' -cf - . | tar -C "$SRC" -xf -
find "$SRC" -name '*.sh' -exec sed -i 's/\r$//' {} +

t0=$(date +%s)
echo "[1/6] Tiszta CMake + Ninja fordítás..."
if cmake -S "$SRC" -B "$BUILD" -G Ninja >/dev/null && \
   cmake --build "$BUILD" --target test_core test_player test_alsa test_dsd test_dsp test_multiout test_matrix test_room aqulify-engine >/dev/null 2>&1; then
    t1=$(date +%s)
    printf "build\tOK\t%d\n" "$((t1 - t0))" >> "$SUMMARY_LOG"
    echo "  [OK]   Tiszta fordítás ($((t1 - t0)) s)"
else
    cmake -S "$SRC" -B "$BUILD" -G Ninja
    cmake --build "$BUILD" --target test_core test_player test_alsa test_dsd test_dsp test_multiout test_matrix test_room aqulify-engine
    t1=$(date +%s)
    printf "build\tHIBA\t%d\n" "$((t1 - t0))" >> "$SUMMARY_LOG"
    echo "  [HIBA] A tiszta fordítás sikertelen"
    exit 1
fi

run_timed() {
    local name="$1"
    shift
    local s_t e_t rc out
    s_t=$(date +%s)
    out="$WORK/out_${name//[^a-zA-Z0-9_]/_}.log"
    "$@" > "$out" 2>&1
    rc=$?
    e_t=$(date +%s)
    grep '^K_METRIC:' "$out" >> "$METRICS_LOG" || true
    if [ $rc -eq 0 ]; then
        printf "%s\tOK\t%d\n" "$name" "$((e_t - s_t))" >> "$SUMMARY_LOG"
        echo "  [OK]   $name ($((e_t - s_t)) s)"
    else
        printf "%s\tHIBA\t%d\n" "$name" "$((e_t - s_t))" >> "$SUMMARY_LOG"
        echo "  [HIBA] $name ($((e_t - s_t)) s) – részletek:"
        tail -n 30 "$out"
        FAIL=1
    fi
}

# 2. Gyors C++ / JS / i18n tesztek (K1–K13, K16 i18n, K17)
echo "[2/6] Gyors egység- és integrációs tesztek..."
for tbin in test_core test_player test_alsa test_dsd test_dsp test_multiout test_matrix test_room; do
    run_timed "$tbin" "$BUILD/$tbin"
done
run_timed "test_import.js" node "$SRC/tests/test_import.js"
run_timed "i18n_check" python3 "$SRC/tools/i18n_build.py" --check

# 3. Tiltott minták (7.5)
echo "[3/6] Tiltott minták ellenőrzése (7.5)..."
forbidden_ok=1
t0=$(date +%s)
PROD_DIFF="$WORK/prod_diff.txt"
git -C "$REPO" diff -U0 "$BASE_REF"..HEAD -- include/ src/pipewire_plugin.cpp macos/ gui_integrated_system.html | grep '^+' | grep -v '^+++' > "$PROD_DIFF" || true
if grep -E '\b(Math\.random|TODO|FIXME|stub|fake|simul)\b' "$PROD_DIFF" >/dev/null 2>&1; then
    echo "  [HIBA] Tiltott minta a termékkódban (Math.random/TODO/FIXME/stub/fake/simul):"
    grep -nE '\b(Math\.random|TODO|FIXME|stub|fake|simul)\b' "$PROD_DIFF" | head -n 10
    forbidden_ok=0
fi

TEST_DIFF="$WORK/test_diff.txt"
{
    git -C "$REPO" diff -U0 "$BASE_REF"..HEAD -- src/test_multiout.cpp src/test_core.cpp | grep '^+' | grep -v '^+++' || true
    cat "$SRC/tests/live/way_eq_live.py" "$SRC/tests/live/gui_way_eq_live.js"
} > "$TEST_DIFF"
if grep -E '\bskip\b|\bSKIP\b|#if 0|if \(false\)|// *CHECK' "$TEST_DIFF" >/dev/null 2>&1; then
    echo "  [HIBA] Tiltott minta a tesztekben (skip/SKIP/#if 0/if (false)/kikommentezett CHECK):"
    grep -nE '\bskip\b|\bSKIP\b|#if 0|if \(false\)|// *CHECK' "$TEST_DIFF" | head -n 10
    forbidden_ok=0
fi

GUI_DIFF="$WORK/gui_diff.txt"
git -C "$REPO" diff -U0 "$BASE_REF"..HEAD -- gui_integrated_system.html | grep '^+' | grep -v '^+++' > "$GUI_DIFF" || true
if python3 - "$GUI_DIFF" <<'PY'
import re, sys
bad = []
for i, raw in enumerate(open(sys.argv[1], encoding="utf-8"), 1):
    line = raw[1:].strip()
    if not line or line.startswith("//") or line.startswith("<!--") or line.startswith("*"):
        continue
    if "data-i18n" in line or "t(\"" in line or "t('" in line:
        continue
    # HTML/JS kodkomment utani reszek levagasa
    code_part = line.split("//")[0]
    if re.search(r"[áéíóöőúüűÁÉÍÓÖŐÚÜŰ]", code_part):
        bad.append((i, code_part[:100]))
if bad:
    print("  [HIBA] Magyar ékezetes szöveg i18n nélkül a felület diffjében:", bad[:5])
    sys.exit(1)
sys.exit(0)
PY
then
    :
else
    forbidden_ok=0
fi
t1=$(date +%s)
if [ $forbidden_ok -eq 1 ]; then
    printf "forbidden_patterns\tOK\t%d\n" "$((t1 - t0))" >> "$SUMMARY_LOG"
    echo "  [OK]   Tiltott minták ellenőrzése (0 találat)"
else
    printf "forbidden_patterns\tHIBA\t%d\n" "$((t1 - t0))" >> "$SUMMARY_LOG"
    FAIL=1
fi

if [ $QUICK -eq 0 ]; then
    # 4. Befagyasztott tesztek ellenőrzése (7.1)
    echo "[4/6] Befagyasztott tesztek ellenőrzése (7.1)..."
    t0=$(date +%s)
    FROZEN_COMMIT="$(git -C "$REPO" log --format='%H %s' "$BASE_REF"..HEAD | grep 'Útonkénti EQ: tesztek (befagyasztva)' | head -n 1 | awk '{print $1}')"
    if [ -z "$FROZEN_COMMIT" ]; then
        echo "  [HIBA] Nem található 'Útonkénti EQ: tesztek (befagyasztva)' commit!"
        printf "frozen_tests\tHIBA\t0\n" >> "$SUMMARY_LOG"
        FAIL=1
    else
        echo "  Befagyasztott tesztcommit: $FROZEN_COMMIT"
        FROZEN_DIR="$WORK/frozen"
        mkdir -p "$FROZEN_DIR/src" "$FROZEN_DIR/build"
        git -C "$REPO" archive "$FROZEN_COMMIT" | tar -C "$FROZEN_DIR/src" -xf -
        cmake -S "$FROZEN_DIR/src" -B "$FROZEN_DIR/build" -G Ninja >/dev/null
        cmake --build "$FROZEN_DIR/build" --target test_multiout test_core >/dev/null 2>&1
        set +e
        "$FROZEN_DIR/build/test_multiout" >/dev/null 2>&1; rc_mo=$?
        "$FROZEN_DIR/build/test_core" >/dev/null 2>&1; rc_co=$?
        set -e
        if [ $rc_mo -ne 0 ] && [ $rc_co -ne 0 ]; then
            echo "  [OK]   Befagyasztott tesztek implementáció nélkül elbuktak (test_multiout rc=$rc_mo, test_core rc=$rc_co)"
        else
            echo "  [HIBA] A befagyasztott tesztek nem buktak el implementáció nélkül (rc_mo=$rc_mo, rc_co=$rc_co)!"
            FAIL=1
        fi
        BAD_TEST_COMMITS="$(git -C "$REPO" log --format='%H %s' "$FROZEN_COMMIT"..HEAD -- src/test_multiout.cpp src/test_core.cpp tests/live/way_eq_live.py tests/live/gui_way_eq_live.js | grep -v 'Teszt-javítás:' || true)"
        echo "  Teszt-javítás commitok a befagyasztás óta:"
        git -C "$REPO" log --oneline "$FROZEN_COMMIT"..HEAD -- src/test_multiout.cpp src/test_core.cpp tests/live/way_eq_live.py tests/live/gui_way_eq_live.js || true
        if [ -n "$BAD_TEST_COMMITS" ]; then
            echo "  [HIBA] Nem 'Teszt-javítás:' commit módosította a fagyasztott teszteket:"
            echo "$BAD_TEST_COMMITS"
            printf "frozen_tests\tHIBA\t%d\n" "$(($(date +%s) - t0))" >> "$SUMMARY_LOG"
            FAIL=1
        else
            printf "frozen_tests\tOK\t%d\n" "$(($(date +%s) - t0))" >> "$SUMMARY_LOG"
        fi
    fi

    # 5. Élő PipeWire és Playwright tesztek (K14, K15, K16, K17)
    echo "[5/6] Élő PipeWire és felületi tesztek (K14, K15, K16, K17)..."
    run_timed "live_run_all" bash "$SRC/tests/live/run_all.sh" "$BUILD"
    run_timed "live_mic" bash "$SRC/tests/live/mic_live.sh" "$BUILD"
    run_timed "live_measure" bash "$SRC/tests/live/measure_live.sh" "$BUILD"

    # 6. Mutánsok ellenőrzése (7.2)
    echo "[6/6] Mutánsok futtatása (7.2)..."
    mkdir -p "$MUT_SRC"
    tar -C "$SRC" -cf - . | tar -C "$MUT_SRC" -xf -
    find "$MUT_SRC/include" "$MUT_SRC/src" -type f -exec sed -i 's/\r$//' {} +
    find "$SRC/include" "$SRC/src" -type f -exec sed -i 's/\r$//' {} +
    cmake -S "$MUT_SRC" -B "$MUT_BUILD" -G Ninja >/dev/null
    cmake --build "$MUT_BUILD" --target test_multiout test_core >/dev/null 2>&1

    for patch in "$REPO"/tools/mutants/*.patch; do
        [ -f "$patch" ] || continue
        mname="$(basename "$patch" .patch)"
        mt0=$(date +%s)
        if ! (cd "$MUT_SRC" && patch -p1 --silent < "$patch"); then
            echo "  [HIBA] $mname nem alkalmazható!"
            printf "%s\tHIBA (apply)\t-\n" "$mname" >> "$MUTANTS_LOG"
            FAIL=1
            continue
        fi
        killer=""
        if ! cmake --build "$MUT_BUILD" --target test_multiout test_core >/dev/null 2>&1; then
            echo "  [HIBA] $mname fordítási hibát okoz (viselkedési hiba kellene)!"
            killer="BUILD_ERROR"
            FAIL=1
        else
            if ! "$MUT_BUILD/test_multiout" >/dev/null 2>&1; then
                killer="test_multiout"
            elif ! "$MUT_BUILD/test_core" >/dev/null 2>&1; then
                killer="test_core"
            fi
        fi
        # Visszaállítás az eredeti $SRC-ből
        (cd "$MUT_SRC" && patch -R -p1 --silent < "$patch" >/dev/null 2>&1) || \
            tar -C "$SRC" -cf - include src | tar -C "$MUT_SRC" -xf - && find "$MUT_SRC/include" "$MUT_SRC/src" -type f -exec sed -i 's/\r$//' {} +
        mt1=$(date +%s)
        if [ -n "$killer" ] && [ "$killer" != "BUILD_ERROR" ]; then
            echo "  [OK]   $mname -> megölte: $killer ($((mt1 - mt0)) s)"
            printf "%s\tKILLED\t%s\n" "$mname" "$killer" >> "$MUTANTS_LOG"
        else
            echo "  [HIBA] $mname TÚLÉLT!"
            printf "%s\tSURVIVED\t%s\n" "$mname" "${killer:-NONE}" >> "$MUTANTS_LOG"
            FAIL=1
        fi
    done
    if [ "$(wc -l < "$MUTANTS_LOG")" -lt 12 ]; then
        echo "  [HIBA] Kevesebb mint 12 mutáns!"
        FAIL=1
    fi
fi

# Gépi JSON eredményfájl és összegző táblázat
python3 - "$SUMMARY_LOG" "$METRICS_LOG" "$MUTANTS_LOG" "$RESULTS_JSON" "$AQ_CHECK_SEED" "$QUICK" "$FAIL" <<'PY'
import json, sys

sum_f, met_f, mut_f, out_f, seed, quick, fail = sys.argv[1:8]
tests = []
for line in open(sum_f, encoding="utf-8"):
    parts = line.rstrip("\n").split("\t")
    if len(parts) == 3:
        tests.append({"name": parts[0], "status": parts[1], "seconds": int(parts[2])})

metrics = {}
for line in open(met_f, encoding="utf-8"):
    line = line.strip()
    if line.startswith("K_METRIC:"):
        try:
            obj = json.loads(line[len("K_METRIC:"):].strip())
            t = obj.pop("test", "unknown")
            if t in metrics:
                if isinstance(metrics[t], list): metrics[t].append(obj)
                else: metrics[t] = [metrics[t], obj]
            else:
                metrics[t] = obj
        except Exception:
            pass

mutants = []
for line in open(mut_f, encoding="utf-8"):
    parts = line.rstrip("\n").split("\t")
    if len(parts) == 3:
        mutants.append({"name": parts[0], "status": parts[1], "killer": parts[2]})

res = {
    "seed": int(seed) if seed.isdigit() else seed,
    "quick": quick == "1",
    "passed": fail == "0",
    "tests": tests,
    "metrics": metrics,
    "mutants": mutants
}
with open(out_f, "w", encoding="utf-8") as f:
    json.dump(res, f, ensure_ascii=False, indent=2)
    f.write("\n")

print("\n===================== ÖSSZEFOGLALÓ TÁBLÁZAT =====================")
print(f"{'Teszt':<28} | {'Állapot':<8} | {'Idő (s)':>7}")
print("-" * 50)
for t in tests:
    print(f"{t['name']:<28} | {t['status']:<8} | {t['seconds']:>7}")
if mutants:
    print("\nMutánsok:")
    for m in mutants:
        print(f"  {m['name']:<34} -> {m['status']} ({m['killer']})")
print(f"\nEredményfájl: {out_f}")
PY

echo
[ $FAIL -eq 0 ] && echo "MINDEN ELLENŐRZÉS SIKERES (0 hiba)" || echo "ELLENŐRZÉS SIKERTELEN"
exit $FAIL
