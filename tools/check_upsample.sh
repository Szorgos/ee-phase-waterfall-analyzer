#!/usr/bin/env bash
# Felmintavételezés (libsoxr) teljes, csalásbiztos ellenőrző szkript (K1–K20, 8.1–8.5).
# Használat:
#   AQ_CHECK_SEED=12345 bash tools/check_upsample.sh        (teljes futtatás)
#   bash tools/check_upsample.sh --quick                    (gyors mód: befagyasztott/élő/mutánsok nélkül)
set -u

QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
export AQ_CHECK_SEED="${AQ_CHECK_SEED:-$(date +%s)}"

echo "AQ_CHECK_SEED=$AQ_CHECK_SEED"

if git -C "$REPO" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    repo_git() { git -C "$REPO" "$@"; }
elif [ -f "$REPO/.git" ] && command -v wslpath >/dev/null 2>&1; then
    WIN_GITDIR="$(sed 's/^gitdir:[[:space:]]*//; s/\r$//' "$REPO/.git")"
    UNIX_GITDIR="$(wslpath -u "$WIN_GITDIR")"
    repo_git() { GIT_DIR="$UNIX_GITDIR" GIT_WORK_TREE="$REPO" git "$@"; }
else
    repo_git() { git -C "$REPO" "$@"; }
fi

FROZEN_COMMIT="$(repo_git log --format='%H %s' HEAD | grep 'Felmintavételezés: tesztek (befagyasztva)' | head -n 1 | awk '{print $1}')"
if [ -n "$FROZEN_COMMIT" ] && repo_git rev-parse --verify --quiet "${FROZEN_COMMIT}^" >/dev/null 2>&1; then
    BASE_REF="$(repo_git rev-parse "${FROZEN_COMMIT}^")"
else
    BASE_REF="origin/master"
    if ! repo_git rev-parse --verify --quiet "$BASE_REF" >/dev/null 2>&1; then
        BASE_REF="bac20b9"
    fi
fi

if [ -z "${NODE_PATH:-}" ] && command -v node >/dev/null 2>&1; then
    if ! node -e "require('playwright')" >/dev/null 2>&1 && [ -d /home/gergo/aq-pw/node_modules ]; then
        export NODE_PATH=/home/gergo/aq-pw/node_modules
    fi
fi

WORK="$(mktemp -d /tmp/aq_upsample_XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

SRC="$WORK/src"
BUILD="$WORK/build"
MUT_SRC="$WORK/mut_src"
MUT_BUILD="$WORK/mut_build"
RESULTS_JSON="$REPO/upsample_results.json"
METRICS_LOG="$WORK/metrics.jsonl"
SUMMARY_LOG="$WORK/summary.tsv"
MUTANTS_LOG="$WORK/mutants.tsv"
: > "$METRICS_LOG"
: > "$SUMMARY_LOG"
: > "$MUTANTS_LOG"

FAIL=0

echo "========================================================"
echo " Aqulify felmintavételezés ellenőrzés (AQ_CHECK_SEED=$AQ_CHECK_SEED, quick=$QUICK)"
echo " Alap referencia: $BASE_REF | Munkamappa: $WORK"
echo "========================================================"

# 1. Tiszta munkamásolat és fordítás átmeneti mappába
mkdir -p "$SRC"
tar -C "$REPO" --exclude=.git --exclude='build*' -cf - . | tar -C "$SRC" -xf -
find "$SRC" -name '*.sh' -exec sed -i 's/\r$//' {} +

t0=$(date +%s)
echo "[1/7] Tiszta CMake + Ninja fordítás..."
if cmake -S "$SRC" -B "$BUILD" -G Ninja >/dev/null 2>&1 && \
   cmake --build "$BUILD" --target test_core test_player test_alsa test_dsd test_dsp test_multiout test_matrix test_room test_upsample test_mic_live test_measure_live aqulify-engine >/dev/null 2>&1; then
    t1=$(date +%s)
    printf "build\tOK\t%d\n" "$((t1 - t0))" >> "$SUMMARY_LOG"
    echo "  [OK]   Tiszta fordítás ($((t1 - t0)) s)"
else
    cmake -S "$SRC" -B "$BUILD" -G Ninja
    cmake --build "$BUILD" --target test_core test_player test_alsa test_dsd test_dsp test_multiout test_matrix test_room test_upsample test_mic_live test_measure_live aqulify-engine
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

# 2. Gyors C++ / JS / i18n tesztek (K1–K11, K16, K17, K19)
echo "[2/7] Gyors egység- és integrációs tesztek..."
for tbin in test_upsample test_core test_player test_alsa test_dsd test_dsp test_multiout test_matrix test_room; do
    run_timed "$tbin" "$BUILD/$tbin"
done
run_timed "test_import.js" node "$SRC/tests/test_import.js"
run_timed "i18n_check" python3 "$SRC/tools/i18n_build.py" --check

# 3. Licenc és csomagolás ellenőrzés (K18)
echo "[3/7] Licenc és csomagolás ellenőrzése (K18)..."
t0=$(date +%s)
k18_ok=1
for f in third_party/soxr/LICENCE third_party/README.md debian/copyright debian/rules packaging/fedora/aqulify.spec packaging/arch/PKGBUILD install.sh CMakeLists.txt; do
    if [ ! -f "$SRC/$f" ]; then
        echo "  [HIBA] K18: hiányzó fájl: $f"
        k18_ok=0
    fi
done
grep -q "b111c15fdc8c029989330ff559184198c161100a59312f5dc19ddeb9b5a15889" "$SRC/third_party/README.md" || { echo "  [HIBA] K18: third_party/README.md SHA-256 hiányzik"; k18_ok=0; }
grep -qi "soxr" "$SRC/debian/copyright" || { echo "  [HIBA] K18: debian/copyright nem említi a libsoxr-t"; k18_ok=0; }
grep -q "third_party/soxr/LICENCE" "$SRC/debian/rules" || { echo "  [HIBA] K18: debian/rules nem csomagolja a libsoxr licencét"; k18_ok=0; }
grep -q "third_party/soxr/LICENCE" "$SRC/packaging/fedora/aqulify.spec" || { echo "  [HIBA] K18: aqulify.spec nem csomagolja a libsoxr licencét"; k18_ok=0; }
grep -q "third_party/soxr/LICENCE" "$SRC/packaging/arch/PKGBUILD" || { echo "  [HIBA] K18: PKGBUILD nem csomagolja a libsoxr licencét"; k18_ok=0; }
grep -q "third_party/soxr/LICENCE" "$SRC/install.sh" || { echo "  [HIBA] K18: install.sh nem másolja a libsoxr licencét"; k18_ok=0; }
grep -q "soxr/LICENCE" "$SRC/CMakeLists.txt" || { echo "  [HIBA] K18: CMakeLists.txt nem másolja a libsoxr licencét Windows/macOS csomagba"; k18_ok=0; }
repo_git archive HEAD | tar -t | grep -q "third_party/soxr/LICENCE" || { echo "  [HIBA] K18: git archive (Linux tar.gz) nem tartalmazza third_party/soxr/LICENCE"; k18_ok=0; }
t1=$(date +%s)
if [ $k18_ok -eq 1 ]; then
    printf "k18_licenses\tOK\t%d\n" "$((t1 - t0))" >> "$SUMMARY_LOG"
    echo "  [OK]   K18: libsoxr licenc a tárolóban, README-ben és minden telepítőben rendben"
else
    printf "k18_licenses\tHIBA\t%d\n" "$((t1 - t0))" >> "$SUMMARY_LOG"
    FAIL=1
fi

# 4. Tiltott minták ellenőrzése (8.5)
echo "[4/7] Tiltott minták ellenőrzése (8.5)..."
forbidden_ok=1
t0=$(date +%s)
PROD_DIFF="$WORK/prod_diff.txt"
repo_git diff -U0 "$BASE_REF"..HEAD -- include/ src/file_player.cpp src/pipewire_plugin.cpp windows/engine/ macos/ gui_integrated_system.html | grep '^+' | grep -v '^+++' > "$PROD_DIFF" || true
if grep -E '\b(Math\.random|TODO|FIXME|stub|fake|simul)\b' "$PROD_DIFF" >/dev/null 2>&1; then
    echo "  [HIBA] Tiltott minta a termékkódban (Math.random/TODO/FIXME/stub/fake/simul):"
    grep -nE '\b(Math\.random|TODO|FIXME|stub|fake|simul)\b' "$PROD_DIFF" | head -n 10
    forbidden_ok=0
fi

TEST_DIFF="$WORK/test_diff.txt"
{
    repo_git diff -U0 "$BASE_REF"..HEAD -- src/test_upsample.cpp src/test_player.cpp src/test_core.cpp src/test_alsa.cpp src/test_measure_live.cpp | grep '^+' | grep -v '^+++' || true
    cat "$SRC/tests/live/upsample_live.py" "$SRC/tests/live/gui_upsample_live.js"
} > "$TEST_DIFF"
if grep -E '\bskip\b|\bSKIP\b|#if 0|if \(false\)|// *CHECK' "$TEST_DIFF" >/dev/null 2>&1; then
    echo "  [HIBA] Tiltott minta a tesztekben (skip/SKIP/#if 0/if (false)/kikommentezett CHECK):"
    grep -nE '\bskip\b|\bSKIP\b|#if 0|if \(false\)|// *CHECK' "$TEST_DIFF" | head -n 10
    forbidden_ok=0
fi

GUI_DIFF="$WORK/gui_diff.txt"
repo_git diff -U0 "$BASE_REF"..HEAD -- gui_integrated_system.html i18n/gui/ docs/felmintavetelezes.md | grep '^+' | grep -v '^+++' > "$GUI_DIFF" || true
if grep -iE '\b(foobar2000|hqplayer|audirvana|roon|jriver|mpd|strawberry|deadbeef|clementine|aimp|winamp)\b' "$GUI_DIFF" >/dev/null 2>&1; then
    echo "  [HIBA] Más lejátszóprogram neve szerepel a felületen / fordításban / dokumentációban:"
    grep -inE '\b(foobar2000|hqplayer|audirvana|roon|jriver|mpd|strawberry|deadbeef|clementine|aimp|winamp)\b' "$GUI_DIFF" | head -n 10
    forbidden_ok=0
fi

GUI_HTML_DIFF="$WORK/gui_html_diff.txt"
repo_git diff -U0 "$BASE_REF"..HEAD -- gui_integrated_system.html | grep '^+' | grep -v '^+++' > "$GUI_HTML_DIFF" || true
if python3 - "$GUI_HTML_DIFF" <<'PY'
import re, sys
bad = []
for i, raw in enumerate(open(sys.argv[1], encoding="utf-8"), 1):
    line = raw[1:].strip()
    if not line or line.startswith("//") or line.startswith("<!--") or line.startswith("*"):
        continue
    if "data-i18n" in line or 't("' in line or "t('" in line:
        continue
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
    # 5. Befagyasztott tesztek ellenőrzése (8.1)
    echo "[5/7] Befagyasztott tesztek ellenőrzése (8.1)..."
    t0=$(date +%s)
    if [ -z "$FROZEN_COMMIT" ]; then
        echo "  [HIBA] Nem található első (befagyasztott) tesztcommit!"
        printf "frozen_tests\tHIBA\t0\n" >> "$SUMMARY_LOG"
        FAIL=1
    else
        echo "  Befagyasztott tesztcommit: $FROZEN_COMMIT"
        FROZEN_DIR="$WORK/frozen"
        mkdir -p "$FROZEN_DIR/src" "$FROZEN_DIR/build"
        repo_git archive "$FROZEN_COMMIT" | tar -C "$FROZEN_DIR/src" -xf -
        cmake -S "$FROZEN_DIR/src" -B "$FROZEN_DIR/build" -G Ninja >/dev/null 2>&1
        cmake --build "$FROZEN_DIR/build" --target test_core test_player test_alsa >/dev/null 2>&1
        set +e
        "$FROZEN_DIR/build/test_core" >/dev/null 2>&1; rc_co=$?
        "$FROZEN_DIR/build/test_alsa" >/dev/null 2>&1; rc_al=$?
        "$FROZEN_DIR/build/test_player" > "$WORK/frozen_player.log" 2>&1; rc_pl=$?
        set -e
        k1_ok_cnt="$(grep -c '\[OK\].*K1 ' "$WORK/frozen_player.log" || true)"
        if [ $rc_co -ne 0 ] && [ $rc_al -ne 0 ] && [ $rc_pl -ne 0 ] && [ "$k1_ok_cnt" -eq 14 ]; then
            echo "  [OK]   Befagyasztott tesztek implementáció nélkül elbuktak (core=$rc_co, alsa=$rc_al, player=$rc_pl), K1 14/14 zöld!"
            printf "frozen_tests\tOK\t%d\n" "$(($(date +%s) - t0))" >> "$SUMMARY_LOG"
        else
            echo "  [HIBA] Befagyasztott tesztek nem a várt állapotot adták (core=$rc_co, alsa=$rc_al, player=$rc_pl, K1_ok=$k1_ok_cnt)!"
            printf "frozen_tests\tHIBA\t%d\n" "$(($(date +%s) - t0))" >> "$SUMMARY_LOG"
            FAIL=1
        fi
    fi

    # 6. Élő PipeWire és Playwright tesztek (K12, K13, K14, K15, K19)
    echo "[6/7] Élő PipeWire és felületi tesztek (K12, K13, K14, K15, K19)..."
    run_timed "live_run_all" bash "$SRC/tests/live/run_all.sh" "$BUILD"
    run_timed "live_mic" bash "$SRC/tests/live/mic_live.sh" "$BUILD"
    run_timed "live_measure" bash "$SRC/tests/live/measure_live.sh" "$BUILD"

    # 7. Mutánsok ellenőrzése (8.2)
    echo "[7/7] Mutánsok futtatása (8.2, 18 mutáns)..."
    mkdir -p "$MUT_SRC"
    tar -C "$SRC" -cf - . | tar -C "$MUT_SRC" -xf -
    find "$MUT_SRC/include" "$MUT_SRC/src" -type f -exec sed -i 's/\r$//' {} +
    find "$SRC/include" "$SRC/src" -type f -exec sed -i 's/\r$//' {} +
    cmake -S "$MUT_SRC" -B "$MUT_BUILD" -G Ninja >/dev/null 2>&1
    cmake --build "$MUT_BUILD" --target test_upsample test_player test_core aqulify-engine >/dev/null 2>&1

    for patch in "$REPO"/tools/mutants_upsample/U*.patch; do
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
        if [[ "$mname" == U15_* ]]; then
            if ! cmake --build "$MUT_BUILD" --target aqulify-engine >/dev/null 2>&1; then
                killer="BUILD_ERROR"
                FAIL=1
            elif ! bash "$MUT_SRC/tests/live/multiout_live.sh" "$MUT_BUILD" python3 -I "$MUT_SRC/tests/live/upsample_live.py" >/dev/null 2>&1; then
                killer="upsample_live.py"
            fi
        elif [[ "$mname" == U16_* ]]; then
            if ! cmake --build "$MUT_BUILD" --target test_core >/dev/null 2>&1; then
                killer="BUILD_ERROR"
                FAIL=1
            elif ! "$MUT_BUILD/test_core" >/dev/null 2>&1; then
                killer="test_core"
            fi
        elif [[ "$mname" == U02_* || "$mname" == U03_* || "$mname" == U04_* || "$mname" == U05_* || "$mname" == U06* || "$mname" == U07_* || "$mname" == U08_* || "$mname" == U09_* ]]; then
            if ! cmake --build "$MUT_BUILD" --target test_upsample >/dev/null 2>&1; then
                killer="BUILD_ERROR"
                FAIL=1
            elif ! "$MUT_BUILD/test_upsample" >/dev/null 2>&1; then
                killer="test_upsample"
            fi
        else
            if ! cmake --build "$MUT_BUILD" --target test_player >/dev/null 2>&1; then
                killer="BUILD_ERROR"
                FAIL=1
            elif ! "$MUT_BUILD/test_player" >/dev/null 2>&1; then
                killer="test_player"
            fi
        fi
        # Visszaállítás
        if ! (cd "$MUT_SRC" && patch -R -p1 --silent < "$patch" >/dev/null 2>&1); then
            tar -C "$SRC" -cf - include src | tar -C "$MUT_SRC" -xf -
            find "$MUT_SRC/include" "$MUT_SRC/src" -type f -exec sed -i 's/\r$//' {} +
        fi
        mt1=$(date +%s)
        if [ -n "$killer" ] && [ "$killer" != "BUILD_ERROR" ]; then
            echo "  [OK]   $mname -> megölte: $killer ($((mt1 - mt0)) s)"
            printf "%s\tKILLED\t%s\n" "$mname" "$killer" >> "$MUTANTS_LOG"
        else
            echo "  [HIBA] $mname TÚLÉLT ($killer)!"
            printf "%s\tSURVIVED\t%s\n" "$mname" "${killer:-NONE}" >> "$MUTANTS_LOG"
            FAIL=1
        fi
    done
    if [ "$(wc -l < "$MUTANTS_LOG")" -lt 14 ]; then
        echo "  [HIBA] Kevesebb mint 14 mutáns futott!"
        FAIL=1
    fi
fi

# Gépi JSON eredményfájl (8.4) és összegző táblázat
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
                if isinstance(metrics[t], list):
                    metrics[t].append(obj)
                else:
                    metrics[t] = [metrics[t], obj]
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
    print(f"\nMutánsok ({len(mutants)} db):")
    for m in mutants:
        print(f"  {m['name']:<38} -> {m['status']} ({m['killer']})")
print(f"\nEredményfájl: {out_f}")
PY

echo
[ $FAIL -eq 0 ] && echo "MINDEN ELLENŐRZÉS SIKERES (0 hiba)" || echo "ELLENŐRZÉS SIKERTELEN"
exit $FAIL
