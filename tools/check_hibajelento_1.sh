#!/usr/bin/env bash
# Beépített hibajelentés, 1. szakasz (őrszem, állapotfájl, helyi jelentés, tisztítás, rendellenes vég) – ellenőrző
# szkript. Friss, normál klónból is futtatható (Linux / WSL; PipeWire kell az élő részhez).
# Használat:
#   AQ_CHECK_SEED=12345 bash tools/check_hibajelento_1.sh     (teljes: egységtesztek, élő próba, mutánsok)
#   bash tools/check_hibajelento_1.sh --quick                  (élő próba és mutánsok nélkül)
# Eredmény: hibajelento_1_results.json a tároló gyökerében.
set -u
QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "${AQ_REPO:-$SCRIPT_DIR/..}" && pwd)"
# védelem: csak az Aqulify tároló gyökerében fut (máshonnan indítva nem másol semmit)
if [ ! -f "$REPO/CMakeLists.txt" ] || [ ! -f "$REPO/tools/check_hibajelento_1.sh" ] || [ ! -f "$REPO/include/health_monitor.hpp" ]; then
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
# Alap: a master, amelyből az ág indult
BASE_REF="$(repo_git merge-base HEAD origin/master 2>/dev/null || echo 521590e)"

WORK="$(mktemp -d /tmp/aq_hibajelento_1_XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
SRC="$WORK/src"; BUILD="$WORK/build"; MUT_SRC="$WORK/mut_src"; MUT_BUILD="$WORK/mut_build"
RESULTS_JSON="$REPO/hibajelento_1_results.json"
SUMMARY="$WORK/summary.tsv"; METRICS="$WORK/metrics.jsonl"; MUTLOG="$WORK/mutants.tsv"
: > "$SUMMARY"; : > "$METRICS"; : > "$MUTLOG"
FAIL=0
# A változás érinti: a híd (test_core), a lejátszó (test_player, test_upsample, test_hires, test_hires_b, test_dsd),
# a DSP-lánc (test_dsp, test_matrix, test_multiout), a közvetlen ALSA-kimenet (test_alsa), a hangbeállítás (audio_options)
UNIT_TESTS="test_watchdog test_core test_player test_dsp test_matrix test_multiout test_alsa test_upsample test_hires test_hires_b test_dsd"

echo "========================================================"
echo " Hibajelentés 1. szakasz ellenőrzése (AQ_CHECK_SEED=$AQ_CHECK_SEED, quick=$QUICK)"
echo " Alap: $BASE_REF | HEAD: $(repo_git rev-parse --short HEAD 2>/dev/null) | Munkamappa: $WORK"
echo "========================================================"

step() { local name="$1"; shift; local t0 t1 out rc; t0=$(date +%s); out="$WORK/out_${name//[^a-zA-Z0-9_]/_}.log"
    "$@" > "$out" 2>&1; rc=$?; t1=$(date +%s)
    grep '^K_METRIC:' "$out" >> "$METRICS" || true
    if [ $rc -eq 0 ]; then printf "%s\tOK\t%d\n" "$name" $((t1-t0)) >> "$SUMMARY"; echo "  [OK]   $name ($((t1-t0)) s)"
    else printf "%s\tHIBA\t%d\n" "$name" $((t1-t0)) >> "$SUMMARY"; echo "  [HIBA] $name – részletek:"; tail -n 30 "$out"; FAIL=1; fi; }

# 1. Tiszta másolat és fordítás (a kiadási motor és egy tesztfordítású motor az élő próbához)
echo "[1/6] Tiszta fordítás..."
mkdir -p "$SRC"
tar -C "$REPO" --exclude=.git --exclude='build*' -cf - . | tar -C "$SRC" -xf -
find "$SRC" -type f \( -name '*.sh' -o -name '*.py' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.patch' \) -exec sed -i 's/\r$//' {} +
t0=$(date +%s)
if cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DAQ_TEST_HOOKS=ON > "$WORK/build.log" 2>&1 && \
   nice -n 5 cmake --build "$BUILD" -j6 --target $UNIT_TESTS aqulify-engine >> "$WORK/build.log" 2>&1; then
    printf "build\tOK\t%d\n" $(($(date +%s)-t0)) >> "$SUMMARY"; echo "  [OK]   fordítás ($(($(date +%s)-t0)) s)"
else
    tail -n 30 "$WORK/build.log"; echo "  [HIBA] a fordítás sikertelen"; exit 1
fi
# a hibajelentés új fájljai figyelmeztetés nélkül fordulnak
if grep -E "(watchdog|health_monitor|error_report|crash_handler|device_format|test_watchdog)\.(hpp|cpp):[0-9]+:[0-9]+: warning" "$WORK/build.log"; then
    echo "  [HIBA] fordítási figyelmeztetés az új fájlokban"; printf "warnings\tHIBA\t0\n" >> "$SUMMARY"; FAIL=1
else
    printf "warnings\tOK\t0\n" >> "$SUMMARY"; echo "  [OK]   nincs fordítási figyelmeztetés az új fájlokban"
fi

# 2. Egységtesztek (az érintett meglévő csomagok is: a régi viselkedés változatlan)
echo "[2/6] Egységtesztek..."
for t in $UNIT_TESTS; do step "$t" "$BUILD/$t"; done
step "signal_safe" python3 "$SRC/tools/check_signal_safe.py" "$SRC/include/crash_handler.hpp"

# 3. Tiltott minták a változásban
echo "[3/6] Tiltott minták..."
ok=1
repo_git diff -U0 "$BASE_REF"..HEAD -- include/ src/ windows/ macos/ gui_integrated_system.html \
    ':!src/test_*.cpp' | grep '^+' | grep -v '^+++' > "$WORK/prod.diff" || true
if grep -nE '\b(TODO|FIXME|stub|fake|Math\.random)\b' "$WORK/prod.diff"; then echo "  [HIBA] tiltott minta a termékkódban"; ok=0; fi
repo_git diff -U0 "$BASE_REF"..HEAD -- 'src/test_*.cpp' tests/live/hibajelento_live.py | grep '^+' | grep -v '^+++' > "$WORK/test.diff" || true
if grep -nE '\bskip\b|\bSKIP\b|#if 0|if \(false\)|// *CHECK' "$WORK/test.diff"; then echo "  [HIBA] kikapcsolt teszt"; ok=0; fi
# hálózati küldés nincs (3. szakasz): az új fájlokban nincs socket, HTTP, curl
for f in include/watchdog.hpp include/health_monitor.hpp include/error_report.hpp include/crash_handler.hpp include/device_format.hpp; do
    if repo_git show "HEAD:$f" 2>/dev/null | grep -nE '\b(socket|connect|sendto|curl|WinHttp|InternetOpen|getaddrinfo)\b|https?://'; then
        echo "  [HIBA] hálózati hívás: $f"; ok=0; fi
done
# token, kulcs nincs a programban
if grep -inE '(api[_-]?key|token|secret|password)[[:space:]]*[=:]' "$WORK/prod.diff"; then echo "  [HIBA] kulcs / token a változásban"; ok=0; fi
# a meglévő tesztek nem lazultak: a módosított régi tesztfájlok (az új test_watchdog kivételével) nincsenek; a
# test_asio és a test_winout csak új csoportot kaphatott (1b: az őrszem a Windows-szálakon), törölt vagy átírt sort nem
if repo_git diff --name-only "$BASE_REF"..HEAD -- src/test_*.cpp tests/ |
    grep -vE '^(src/test_watchdog\.cpp|src/test_asio\.cpp|src/test_winout\.cpp|tests/live/hibajelento_live\.(py|sh))$'; then
    echo "  [HIBA] meglévő teszt módosult"; ok=0; fi
if repo_git diff -U0 "$BASE_REF"..HEAD -- src/test_asio.cpp src/test_winout.cpp | grep '^-' | grep -v '^---'; then
    echo "  [HIBA] a test_asio / test_winout meglévő sora törlődött vagy módosult"; ok=0; fi
repo_git diff -U0 "$BASE_REF"..HEAD -- gui_integrated_system.html i18n/gui/ docs/ | grep '^+' | grep -v '^+++' > "$WORK/gui.diff" || true
if grep -inE '\b(foobar2000|hqplayer|audirvana|roon|jriver|moode|volumio|deadbeef|clementine|aimp|winamp|spotify|tidal|apple)\b' "$WORK/gui.diff"; then
    echo "  [HIBA] más termék neve a felületen / dokumentációban"; ok=0; fi
# Windowsos sorvég (CR) nem kerülhet olyan fájlba, amelyben az alapon nem volt
for f in $(repo_git diff --name-only "$BASE_REF"..HEAD); do
    repo_git cat-file -e "HEAD:$f" 2>/dev/null || continue
    now=$(repo_git show "HEAD:$f" | grep -c $'\r')
    old=$(repo_git show "$BASE_REF:$f" 2>/dev/null | grep -c $'\r')
    if [ "$now" -gt 0 ] && [ "${old:-0}" -eq 0 ]; then echo "  [HIBA] CRLF sorvég került ide: $f ($now sor)"; ok=0; fi
done
if [ $ok -eq 1 ]; then printf "forbidden\tOK\t0\n" >> "$SUMMARY"; echo "  [OK]   tiltott minták: 0 találat"
else printf "forbidden\tHIBA\t0\n" >> "$SUMMARY"; FAIL=1; fi

if [ $QUICK -eq 0 ]; then
    # 4. Élő próba: elszigetelt motor (--port 47899), szándékosan beragasztott híd, napló és állapotfájl
    echo "[4/6] Élő próba (PipeWire, tesztfordítású motor)..."
    step "live_hibajelento" bash "$SRC/tests/live/hibajelento_live.sh" "$BUILD"

    # 5. Mutánsok: mindegyiket meg kell ölnie a fájlnévben megnevezett ellenőrzésnek
    echo "[5/6] Mutánsok..."
    mkdir -p "$MUT_SRC"; tar -C "$SRC" -cf - . | tar -C "$MUT_SRC" -xf -
    cmake -S "$MUT_SRC" -B "$MUT_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DAQ_TEST_HOOKS=ON > /dev/null 2>&1
    # a mutánsok sorrendje a seed szerint (a sorrendtől egyik sem függhet)
    PATCHES=$(ls "$SRC"/tools/mutants_hibajelento_1/W*.patch | python3 -c "import random,sys; l=sys.stdin.read().split(); random.Random(int('$AQ_CHECK_SEED')).shuffle(l); print(' '.join(l))")
    for patch in $PATCHES; do
        m="$(basename "$patch" .patch)"; killer="${m#*_}"; killer="${killer%%__*}"
        if ! (cd "$MUT_SRC" && patch -p1 --silent < "$patch"); then
            echo "  [HIBA] $m nem alkalmazható"; printf "%s\tAPPLY_ERROR\t-\n" "$m" >> "$MUTLOG"; FAIL=1; continue; fi
        res="SURVIVED"
        if [ "${killer#win-}" != "$killer" ]; then
            # Windows-only ölő (test_asio, test_winout): itt nem fut; a natív Windows-futás igazolja (a jelentésben)
            (cd "$MUT_SRC" && patch -R -p1 --silent < "$patch") || { echo "  [HIBA] $m visszaállítása sikertelen"; FAIL=1; }
            printf "%s\t%s\t%s\n" "$m" "WINDOWS_NATIVE" "$killer" >> "$MUTLOG"
            echo "  [--]   $m -> natív Windows-futás kell (${killer#win-}); a patch alkalmazható"
            continue
        elif [ "$killer" = "static" ]; then
            if ! python3 "$MUT_SRC/tools/check_signal_safe.py" "$MUT_SRC/include/crash_handler.hpp" > "$WORK/mut_run.log" 2>&1; then res="KILLED"; fi
        elif [ "$killer" = "live" ]; then
            if ! nice -n 5 cmake --build "$MUT_BUILD" -j6 --target aqulify-engine > "$WORK/mut.log" 2>&1; then res="BUILD_ERROR"
            elif ! timeout 300 bash "$MUT_SRC/tests/live/hibajelento_live.sh" "$MUT_BUILD" > "$WORK/mut_run.log" 2>&1; then res="KILLED"; fi
        else
            if ! nice -n 5 cmake --build "$MUT_BUILD" -j6 --target "$killer" > "$WORK/mut.log" 2>&1; then res="BUILD_ERROR"
            elif ! timeout 300 "$MUT_BUILD/$killer" > "$WORK/mut_run.log" 2>&1; then res="KILLED"; fi
        fi
        (cd "$MUT_SRC" && patch -R -p1 --silent < "$patch") || { echo "  [HIBA] $m visszaállítása sikertelen"; FAIL=1; }
        printf "%s\t%s\t%s\n" "$m" "$res" "$killer" >> "$MUTLOG"
        if [ "$res" = "KILLED" ]; then echo "  [OK]   $m -> megölte: $killer"
        else echo "  [HIBA] $m: $res ($killer)"; tail -n 15 "$WORK/mut_run.log" "$WORK/mut.log" 2>/dev/null; FAIL=1; fi
    done
    [ "$(wc -l < "$MUTLOG")" -ge 17 ] || { echo "  [HIBA] 17-nél kevesebb mutáns futott"; FAIL=1; }
    [ "$(grep -c $'\tKILLED\t' "$MUTLOG")" -ge 13 ] || { echo "  [HIBA] 13-nál kevesebb mutánst ölt meg a Linux-futás"; FAIL=1; }
fi

# 6. Eredményfájl és összegzés
echo "[6/6] Összegzés..."
python3 - "$SUMMARY" "$METRICS" "$MUTLOG" "$RESULTS_JSON" "$AQ_CHECK_SEED" "$QUICK" "$FAIL" "$(repo_git rev-parse HEAD 2>/dev/null)" "$BASE_REF" <<'PY'
import json, sys
s, met, mut, out, seed, quick, fail, head, base = sys.argv[1:10]
tests = [dict(zip(("name", "status", "seconds"), l.rstrip("\n").split("\t"))) for l in open(s, encoding="utf-8") if l.strip()]
metrics = {}
for l in open(met, encoding="utf-8"):
    if l.startswith("K_METRIC:"):
        try:
            o = json.loads(l[9:]); metrics.setdefault(o.pop("test", "?"), []).append(o)
        except Exception:
            pass
mutants = [dict(zip(("name", "status", "killer"), l.rstrip("\n").split("\t"))) for l in open(mut, encoding="utf-8") if l.strip()]
json.dump({"seed": seed, "head": head, "base": base, "quick": quick == "1", "passed": fail == "0", "tests": tests,
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
