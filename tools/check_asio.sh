#!/usr/bin/env bash
# Csalásbiztos ellenőrző szkript a Windows ASIO-kimenet motorhoz (K1–K13 és 7. fejezet).
# Használat: AQ_CHECK_SEED=12345 bash tools/check_asio.sh [--quick]

set -uo pipefail

QUICK=0
for arg in "$@"; do
    if [ "$arg" = "--quick" ]; then
        QUICK=1
    fi
done

SEED="${AQ_CHECK_SEED:-$(date +%s)}"
export AQ_CHECK_SEED="$SEED"
echo "AQ_CHECK_SEED=$SEED"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

# Git segédfüggvény: normál Linux klónban és Windows git worktree-ben is működik
run_git() {
    if git -C "$ROOT_DIR" rev-parse --git-dir >/dev/null 2>&1; then
        git -C "$ROOT_DIR" "$@"
    else
        git.exe -C "$(wslpath -w "$ROOT_DIR")" "$@" | tr -d '\r'
    fi
}

FROZEN_COMMIT="$(run_git log --format=%H --grep='tesztek és hamis meghajtó (befagyasztva)' -n 1 2>/dev/null || true)"
if [ -n "$FROZEN_COMMIT" ] && run_git rev-parse --verify "${FROZEN_COMMIT}^" >/dev/null 2>&1; then
    BASE_REF="$(run_git rev-parse "${FROZEN_COMMIT}^")"
elif run_git rev-parse --verify origin/master >/dev/null 2>&1; then
    BASE_REF="$(run_git merge-base HEAD origin/master)"
else
    BASE_REF="$(run_git rev-list --max-parents=0 HEAD | tail -n 1)"
fi

WORK_ROOT="$(mktemp -d /tmp/aq_asio_XXXXXX)"

# Windows-oldali futtató és tesztadat mappa (hogy /tmp klónból is natív Windows útvonalat lássanak a .exe tesztek)
WIN_TEMP_RAW="$(cmd.exe /c "echo %TEMP%" 2>/dev/null | tr -d '\r\n')"
WIN_TEMP_WSL="$(wslpath -u "$WIN_TEMP_RAW" 2>/dev/null || echo "/mnt/c/Users/gergo/AppData/Local/Temp")"
WIN_RUN_DIR="$WIN_TEMP_WSL/aq_asio_win_run_$$"
mkdir -p "$WIN_RUN_DIR"
cp -r "$ROOT_DIR/tests/data" "$WIN_RUN_DIR/data"
WIN_DATA_DIR="$(wslpath -m "$WIN_RUN_DIR/data")"

# Biztonsági mentés a mutációs és befagyasztott tesztekhez, hogy bármilyen megszakítás esetén is visszaálljon
ORIG_ASIO_HPP="$WORK_ROOT/player_output_asio.hpp.orig"
ORIG_SDK_H="$WORK_ROOT/asio.h.orig"
ORIG_TEST_CPP="$WORK_ROOT/test_asio.cpp.orig"
cp "$ROOT_DIR/windows/engine/player_output_asio.hpp" "$ORIG_ASIO_HPP"
cp "$ROOT_DIR/third_party/asiosdk/common/asio.h" "$ORIG_SDK_H"
cp "$ROOT_DIR/src/test_asio.cpp" "$ORIG_TEST_CPP"

cleanup() {
    if [ -f "$ORIG_ASIO_HPP" ]; then cp "$ORIG_ASIO_HPP" "$ROOT_DIR/windows/engine/player_output_asio.hpp"; fi
    if [ -f "$ORIG_SDK_H" ]; then cp "$ORIG_SDK_H" "$ROOT_DIR/third_party/asiosdk/common/asio.h"; fi
    if [ -f "$ORIG_TEST_CPP" ]; then cp "$ORIG_TEST_CPP" "$ROOT_DIR/src/test_asio.cpp"; fi
    rm -rf "$WORK_ROOT" "$WIN_RUN_DIR"
}
trap cleanup EXIT

echo "========================================================"
echo " Aqulify ASIO-mag ellenőrzés (AQ_CHECK_SEED=$SEED, quick=$QUICK)"
echo " Alap referencia: $BASE_REF | Munkamappa: $WORK_ROOT"
echo "========================================================"

FAILS=0
RESULTS_FILE="$WORK_ROOT/steps.tsv"
MUTANTS_FILE="$WORK_ROOT/mutants.tsv"
METRICS_RAW="$WORK_ROOT/metrics_raw.txt"
: > "$RESULTS_FILE"
: > "$MUTANTS_FILE"
: > "$METRICS_RAW"

record_step() {
    local name="$1"
    local status="$2"
    local secs="$3"
    printf "%s\t%s\t%s\n" "$name" "$status" "$secs" >> "$RESULTS_FILE"
    if [ "$status" != "OK" ]; then
        FAILS=$((FAILS + 1))
    fi
}

# Natív Windows .exe futtatása (AQ_CHECK_SEED és AQ_TEST_DATA_DIR átadásával a WSLENV-en keresztül)
run_win_exe() {
    local exe_path="$1"
    shift
    local base_name
    base_name="$(basename "$exe_path")"
    local run_copy="$WIN_RUN_DIR/${RANDOM}_${base_name}"
    cp "$exe_path" "$run_copy"
    chmod +x "$run_copy"
    export AQ_CHECK_SEED="$SEED"
    export AQ_TEST_DATA_DIR="$WIN_DATA_DIR"
    export WSLENV="AQ_CHECK_SEED:AQ_TEST_DATA_DIR"
    "$run_copy" "$@"
    local rc=$?
    rm -f "$run_copy"
    return $rc
}

# CRLF/LF-független, többhunkos unified diff alkalmazó segédfüggvény
apply_patch_file() {
    local patch_file="$1"
    python3 - "$ROOT_DIR" "$patch_file" << 'PY'
import os, sys

root, patch_path = sys.argv[1], sys.argv[2]
lines = open(patch_path, encoding="utf-8").read().splitlines()
target_rel = None
hunks = []
cur_old, cur_new = [], []
in_hunk = False

for line in lines:
    if line.startswith("+++ b/"):
        target_rel = line[6:].strip()
    elif line.startswith("@@ "):
        if in_hunk and cur_old:
            hunks.append((cur_old, cur_new))
        cur_old, cur_new = [], []
        in_hunk = True
    elif in_hunk:
        if line.startswith("-"):
            cur_old.append(line[1:])
        elif line.startswith("+"):
            cur_new.append(line[1:])
        elif line.startswith(" "):
            cur_old.append(line[1:])
            cur_new.append(line[1:])

if in_hunk and cur_old:
    hunks.append((cur_old, cur_new))

if not target_rel or not hunks:
    sys.exit(1)

full_path = os.path.join(root, target_rel)
raw = open(full_path, "rb").read().decode("utf-8")
crlf = "\r\n" in raw
norm = raw.replace("\r\n", "\n")

for old_chunk, new_chunk in hunks:
    old_str = "\n".join(old_chunk)
    new_str = "\n".join(new_chunk)
    if old_str not in norm:
        sys.exit(1)
    norm = norm.replace(old_str, new_str, 1)

patched = norm.replace("\n", "\r\n") if crlf else norm
open(full_path, "wb").write(patched.encode("utf-8"))
PY
}

# SDK fájlonkénti SHA-256 ellenőrző függvény (K2 és U08 mutáns számára is)
verify_sdk_hashes() {
    local dir="$1"
    python3 - "$dir" << 'PY'
import hashlib, os, sys

root = sys.argv[1]
expected = {
    "third_party/asiosdk/LICENSE.txt": "61e56814f24b32216884b2db0b4dcc317111a5ab354d54d19dd9fc13d71b1c7a",
    "third_party/asiosdk/changes.txt": "9d2cc8a29c86feaad64dcdc359b3083b3d949006ac14aaf3a73c8dc5db3050d8",
    "third_party/asiosdk/common/LICENSE.txt": "61e56814f24b32216884b2db0b4dcc317111a5ab354d54d19dd9fc13d71b1c7a",
    "third_party/asiosdk/common/asio.cpp": "4ecee2bfaebd8b0777db96175d2b2ea8d0cd684dfaf9651480c44b0ebec0fd61",
    "third_party/asiosdk/common/asio.h": "93d2f80d49b4ac9c963514e7c1307ab5111d8104246192233745beb058534ea1",
    "third_party/asiosdk/common/asiodrvr.cpp": "47cf72e30cc04bc37fe609a467f833e85434f76b27b9cd19186358e4c50a6982",
    "third_party/asiosdk/common/asiodrvr.h": "ca2d65191d4914cb405b5c0b9e0af932195b2734e8781cd4d8c94e6526fb8926",
    "third_party/asiosdk/common/asiosys.h": "f44a1ed0fd7c056ff047aeebc4c05a8c185f0585a24d8debc55c70107777dbef",
    "third_party/asiosdk/common/iasiodrv.h": "86ad4788452c74823653cf57d4bd557c48bb6542d7e6eb7fff80102c705a066d",
    "third_party/asiosdk/host/ASIOConvertSamples.h": "f1613470d008aec5e11905c8597c445390950d254e83956673d2b3f3c0b0ae6a",
    "third_party/asiosdk/host/asiodrivers.h": "23f78334ed9aff7f9db4b0d36b228595005c6f6a3ad842e9d0ad33edfb041a8f",
    "third_party/asiosdk/host/ginclude.h": "96db001a91c469702afac88178f289bd50f78dd43ed12c174f898a44a7a56676",
    "third_party/asiosdk/host/ASIOConvertSamples.cpp": "1257966155c3b35719b5454813fad8170e818b9079003f82d3845db61a7dd437",
    "third_party/asiosdk/host/asiodrivers.cpp": "6f5c7529047c482998201df4b9c9e6bcdeee782ed764e6324d49b3bca4340601",
    "third_party/asiosdk/host/pc/asiolist.cpp": "d3f7d83f561eaafed17393c3c95ba077f2ec109e2f7c762dcdf905f0e5c17f02",
    "third_party/asiosdk/host/pc/asiolist.h": "daab5066bcb9ca79552541bc2981cf43cde7ac72bd4572286764097c9cc92010",
}

for rel, exp_hash in expected.items():
    p = os.path.join(root, rel)
    if not os.path.isfile(p):
        print(f"Hiányzó SDK fájl: {rel}", file=sys.stderr)
        sys.exit(1)
    h = hashlib.sha256(open(p, "rb").read()).hexdigest()
    if h != exp_hash:
        print(f"Módosított SDK fájl ({rel}): {h} != {exp_hash}", file=sys.stderr)
        sys.exit(1)

for dirpath, _, filenames in os.walk(os.path.join(root, "third_party/asiosdk")):
    for fn in filenames:
        low = fn.lower()
        if low.endswith((".dll", ".png", ".svg", ".jpg", ".eps")):
            print(f"Tiltott fájl a third_party/asiosdk alatt: {fn}", file=sys.stderr)
            sys.exit(1)

lic_txt = open(os.path.join(root, "third_party/asiosdk/LICENSE.txt"), encoding="utf-8", errors="ignore").read()
if "General Public License (GPL) Version 3" not in lic_txt:
    print("Hiányzik a GPLv3 licencszöveg a third_party/asiosdk/LICENSE.txt-ből", file=sys.stderr)
    sys.exit(1)

readme = open(os.path.join(root, "third_party/README.md"), encoding="utf-8", errors="ignore").read()
for req in ("asiosdk", "2.3.4", "d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca", "GPL-3.0"):
    if req not in readme:
        print(f"Hiányzó adat a third_party/README.md-ből: {req}", file=sys.stderr)
        sys.exit(1)
PY
}

# ---------------------------------------------------------------------------
# 1. K1 (2.4): Tiltott közös / más platformos fájlok érintetlensége
# ---------------------------------------------------------------------------
echo "[1/7] 2.4 Tiltott fájlok érintetlenségének ellenőrzése (K1)..."
T0=$(date +%s)
FORBIDDEN_DIFF="$(run_git diff "$BASE_REF" HEAD -- \
    include/audio_options.hpp include/ws_bridge.hpp include/file_player.hpp src/file_player.cpp \
    windows/engine/aether_engine.cpp windows/engine/player_output_wasapi.hpp \
    gui_integrated_system.html i18n linux macos 2>/dev/null || true)"
T1=$(date +%s)
if [ -z "$FORBIDDEN_DIFF" ]; then
    echo "  [OK]   K1: A 2.4 fejezetben felsorolt fájlok teljesen érintetlenek (0 sor diff)"
    record_step "k1_untouched_files" "OK" "$((T1 - T0))"
else
    echo "  [HIBA] K1: Tiltott fájl módosult a 2.4 listából!"
    echo "$FORBIDDEN_DIFF" | head -n 30
    record_step "k1_untouched_files" "HIBA" "$((T1 - T0))"
fi

# ---------------------------------------------------------------------------
# 2. K2: ASIO SDK eredete, GPLv3 licenc és fájlonkénti SHA-256
# ---------------------------------------------------------------------------
echo "[2/7] ASIO SDK eredete, GPLv3 licenc és SHA-256 ellenőrzése (K2)..."
T0=$(date +%s)
if verify_sdk_hashes "$ROOT_DIR" && [ -f "$ROOT_DIR/LICENSE" ]; then
    T1=$(date +%s)
    echo "  [OK]   K2: ASIO SDK 2.3.4 (GPLv3) változatlan (16/16 SHA-256 egyezik), README-sor rendben"
    record_step "k2_sdk_license_hash" "OK" "$((T1 - T0))"
else
    T1=$(date +%s)
    echo "  [HIBA] K2: ASIO SDK licenc vagy SHA-256 eltérés!"
    record_step "k2_sdk_license_hash" "HIBA" "$((T1 - T0))"
fi

# ---------------------------------------------------------------------------
# 3. 7.5 és K13: Tiltott minták és dokumentáció ellenőrzése
# ---------------------------------------------------------------------------
echo "[3/7] Tiltott minták (7.5) és dokumentáció (K13) ellenőrzése..."
T0=$(date +%s)
PAT_OK=1
if grep -n -E '\b(TODO|FIXME|stub|fake|simul)\b' "$ROOT_DIR/windows/engine/player_output_asio.hpp" >/dev/null 2>&1; then
    echo "  [HIBA] Tiltott minta (TODO/FIXME/stub/fake/simul) a player_output_asio.hpp-ban!"
    grep -n -E '\b(TODO|FIXME|stub|fake/simul)\b' "$ROOT_DIR/windows/engine/player_output_asio.hpp" || true
    PAT_OK=0
fi
if grep -n -i -E '\b(foobar2000|hqplayer|audirvana|jriver|roon|aimp|winamp)\b' \
    "$ROOT_DIR/windows/engine/player_output_asio.hpp" "$ROOT_DIR/src/test_asio.cpp" "$ROOT_DIR/docs/asio.md" >/dev/null 2>&1; then
    echo "  [HIBA] Más lejátszóprogram neve szerepel az új fájlokban!"
    PAT_OK=0
fi
if grep -n -E '(#if 0|//.*CHECK\()' "$ROOT_DIR/src/test_asio.cpp" >/dev/null 2>&1; then
    echo "  [HIBA] Kikapcsolt ellenőrzés a src/test_asio.cpp-ben!"
    PAT_OK=0
fi
if [ ! -s "$ROOT_DIR/docs/asio.md" ] || ! grep -q "GPLv3" "$ROOT_DIR/docs/asio.md"; then
    echo "  [HIBA] K13: docs/asio.md hiányzik vagy hiányos!"
    PAT_OK=0
fi
T1=$(date +%s)
if [ "$PAT_OK" -eq 1 ]; then
    echo "  [OK]   Tiltott minták (0 találat) és docs/asio.md (K13) rendben"
    record_step "k13_patterns_and_doc" "OK" "$((T1 - T0))"
else
    record_step "k13_patterns_and_doc" "HIBA" "$((T1 - T0))"
fi

# ---------------------------------------------------------------------------
# 4. K1 + K11: Windows llvm-mingw fordítás és natív tesztfuttatás
# ---------------------------------------------------------------------------
echo "[4/7] Windows llvm-mingw fordítás és natív tesztfuttatás (K1, K3–K11, 7.3)..."
LLVM_MINGW="${LLVM_MINGW_ROOT:-/home/gergo/toolchains/llvm-mingw-20250924-ucrt-ubuntu-22.04-x86_64}"
FFTW_DIR="${AQULIFY_FFTW_SOURCE_DIR:-/home/gergo/toolchains/fftw-3.3.10}"
RNNOISE_DIR="${AQULIFY_RNNOISE_MODEL_DIR:-/home/gergo/rnnoise-model}"
BUILD_DIR="$WORK_ROOT/build_win"

T0=$(date +%s)
if cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/windows/toolchain-llvm-mingw.cmake" \
    -DLLVM_MINGW_ROOT="$LLVM_MINGW" \
    -DAQULIFY_FFTW_SOURCE_DIR="$FFTW_DIR" \
    -DAQULIFY_RNNOISE_MODEL_DIR="$RNNOISE_DIR" > "$WORK_ROOT/cmake.log" 2>&1 && \
   cmake --build "$BUILD_DIR" -j"$(nproc)" --target \
    test_asio test_ringbuffer test_core test_dsp test_matrix test_multiout \
    test_keepawake test_dsd test_player test_upsample test_mic_basic Aqulify > "$WORK_ROOT/build.log" 2>&1; then
    T1=$(date +%s)
    echo "  [OK]   Windows llvm-mingw tiszta fordítás ($((T1 - T0)) s)"
    record_step "build_windows" "OK" "$((T1 - T0))"
else
    T1=$(date +%s)
    echo "  [HIBA] Windows llvm-mingw fordítás sikertelen ($((T1 - T0)) s)"
    tail -n 30 "$WORK_ROOT/build.log" || true
    record_step "build_windows" "HIBA" "$((T1 - T0))"
fi

# test_asio.exe natív futtatása
T0=$(date +%s)
ASIO_LOG="$WORK_ROOT/test_asio.log"
if run_win_exe "$BUILD_DIR/test_asio.exe" > "$ASIO_LOG" 2>&1; then
    T1=$(date +%s)
    echo "  [OK]   test_asio.exe natív futtatás ($((T1 - T0)) s)"
    grep -E '^K_METRIC:' "$ASIO_LOG" >> "$METRICS_RAW" || true
    record_step "test_asio" "OK" "$((T1 - T0))"
else
    T1=$(date +%s)
    echo "  [HIBA] test_asio.exe sikertelen ($((T1 - T0)) s):"
    cat "$ASIO_LOG"
    record_step "test_asio" "HIBA" "$((T1 - T0))"
fi

# Meglévő Windows tesztek futtatása (K1)
if [ "$QUICK" -eq 0 ]; then
    for t in test_ringbuffer test_core test_dsp test_matrix test_multiout test_keepawake test_dsd test_player test_upsample test_mic_basic; do
        T0=$(date +%s)
        TLOG="$WORK_ROOT/${t}.log"
        if run_win_exe "$BUILD_DIR/${t}.exe" > "$TLOG" 2>&1; then
            T1=$(date +%s)
            echo "  [OK]   ${t}.exe ($((T1 - T0)) s)"
            record_step "$t" "OK" "$((T1 - T0))"
        else
            T1=$(date +%s)
            echo "  [HIBA] ${t}.exe ($((T1 - T0)) s)"
            tail -n 20 "$TLOG" || true
            record_step "$t" "HIBA" "$((T1 - T0))"
        fi
    done
fi

# ---------------------------------------------------------------------------
# 5. 7.1: Befagyasztott tesztek ellenőrzése
# ---------------------------------------------------------------------------
echo "[5/7] Befagyasztott tesztek ellenőrzése (7.1)..."
if [ "$QUICK" -eq 0 ] && [ -n "$FROZEN_COMMIT" ]; then
    T0=$(date +%s)
    echo "  Befagyasztott tesztcommit: $FROZEN_COMMIT"
    FROZEN_OK=1

    # Ellenőrizzük, hogy a src/test_asio.cpp csak Teszt-javítás: commitban változhatott
    NON_FIX_COMMITS="$(run_git log "${FROZEN_COMMIT}..HEAD" --invert-grep --grep='^Teszt-javítás:' --format=%H -- src/test_asio.cpp 2>/dev/null || true)"
    if [ -n "$NON_FIX_COMMITS" ]; then
        echo "  [HIBA] A src/test_asio.cpp nem 'Teszt-javítás:' commitban módosult: $NON_FIX_COMMITS"
        FROZEN_OK=0
    fi

    # A befagyasztott állapot visszaállítása (player_output_asio.hpp nélkül) és fordítása ugyanabban a BUILD_DIR-ben
    rm -f "$ROOT_DIR/windows/engine/player_output_asio.hpp"
    run_git show "${FROZEN_COMMIT}:src/test_asio.cpp" > "$ROOT_DIR/src/test_asio.cpp"
    rm -f "$BUILD_DIR/CMakeFiles/test_asio.dir/src/test_asio.cpp.obj"

    if cmake --build "$BUILD_DIR" -j"$(nproc)" --target test_asio > "$WORK_ROOT/frozen_build.log" 2>&1; then
        run_win_exe "$BUILD_DIR/test_asio.exe" > "$WORK_ROOT/frozen_run.log" 2>&1
        FRC=$?
        if [ "$FRC" -eq 0 ]; then
            echo "  [HIBA] A befagyasztott test_asio.exe implementáció nélkül is 0-val tért vissza!"
            FROZEN_OK=0
        else
            echo "  [OK]   A befagyasztott test_asio.exe implementáció nélkül valódi hívásokkal elbukott (rc=$FRC)"
        fi
    else
        echo "  [HIBA] A befagyasztott tesztcommit nem fordult le!"
        tail -n 20 "$WORK_ROOT/frozen_build.log" || true
        FROZEN_OK=0
    fi

    # Visszaállítjuk a termékkódot és a tesztfájlt
    cp "$ORIG_ASIO_HPP" "$ROOT_DIR/windows/engine/player_output_asio.hpp"
    cp "$ORIG_TEST_CPP" "$ROOT_DIR/src/test_asio.cpp"
    rm -f "$BUILD_DIR/CMakeFiles/test_asio.dir/src/test_asio.cpp.obj"
    cmake --build "$BUILD_DIR" -j"$(nproc)" --target test_asio >/dev/null 2>&1

    T1=$(date +%s)
    if [ "$FROZEN_OK" -eq 1 ]; then
        record_step "frozen_tests" "OK" "$((T1 - T0))"
    else
        record_step "frozen_tests" "HIBA" "$((T1 - T0))"
    fi
else
    echo "  [KIHAGYVA] Befagyasztott tesztek (--quick)"
fi

# ---------------------------------------------------------------------------
# 6. 7.2: Mutánsok futtatása (10 mutáns)
# ---------------------------------------------------------------------------
echo "[6/7] Mutánsok futtatása (7.2, 10 mutáns)..."
if [ "$QUICK" -eq 0 ]; then
    MUT_COUNT=0
    for patch in "$ROOT_DIR"/tools/mutants_asio/U*.patch; do
        MUT_COUNT=$((MUT_COUNT + 1))
        mname="$(basename "$patch" .patch)"
        MT0=$(date +%s)
        cp "$ORIG_ASIO_HPP" "$ROOT_DIR/windows/engine/player_output_asio.hpp"
        cp "$ORIG_SDK_H" "$ROOT_DIR/third_party/asiosdk/common/asio.h"

        if ! apply_patch_file "$patch"; then
            echo "  [HIBA] $mname: a patch nem alkalmazható!"
            printf "%s\tAPPLY_ERR\tnone\n" "$mname" >> "$MUTANTS_FILE"
            FAILS=$((FAILS + 1))
            continue
        fi

        KILLED_BY=""
        if ! verify_sdk_hashes "$ROOT_DIR" >/dev/null 2>&1; then
            KILLED_BY="k2_sdk_hash"
        else
            rm -f "$BUILD_DIR/CMakeFiles/test_asio.dir/src/test_asio.cpp.obj"
            if ! cmake --build "$BUILD_DIR" -j"$(nproc)" --target test_asio >/dev/null 2>&1; then
                KILLED_BY="build"
            elif ! run_win_exe "$BUILD_DIR/test_asio.exe" >/dev/null 2>&1; then
                KILLED_BY="test_asio"
            fi
        fi

        cp "$ORIG_ASIO_HPP" "$ROOT_DIR/windows/engine/player_output_asio.hpp"
        cp "$ORIG_SDK_H" "$ROOT_DIR/third_party/asiosdk/common/asio.h"

        MT1=$(date +%s)
        if [ -n "$KILLED_BY" ]; then
            echo "  [OK]   $mname -> megölte: $KILLED_BY ($((MT1 - MT0)) s)"
            printf "%s\tKILLED\t%s\n" "$mname" "$KILLED_BY" >> "$MUTANTS_FILE"
        else
            echo "  [HIBA] $mname -> TÚLÉLTE! ($((MT1 - MT0)) s)"
            printf "%s\tSURVIVED\tnone\n" "$mname" >> "$MUTANTS_FILE"
            FAILS=$((FAILS + 1))
        fi
    done
    if [ "$MUT_COUNT" -lt 10 ]; then
        echo "  [HIBA] Kevesebb mint 10 mutáns található ($MUT_COUNT db)!"
        FAILS=$((FAILS + 1))
    fi
    rm -f "$BUILD_DIR/CMakeFiles/test_asio.dir/src/test_asio.dir/src/test_asio.cpp.obj" "$BUILD_DIR/CMakeFiles/test_asio.dir/src/test_asio.cpp.obj"
    cmake --build "$BUILD_DIR" -j"$(nproc)" --target test_asio >/dev/null 2>&1
else
    echo "  [KIHAGYVA] Mutánsok (--quick)"
fi

# ---------------------------------------------------------------------------
# 7. 7.4: Összefoglaló táblázat és asio_results.json
# ---------------------------------------------------------------------------
echo "[7/7] asio_results.json és összefoglaló táblázat előállítása (7.4)..."
JSON_OUT="$ROOT_DIR/asio_results.json"
python3 - "$SEED" "$QUICK" "$FAILS" "$RESULTS_FILE" "$MUTANTS_FILE" "$METRICS_RAW" "$JSON_OUT" << 'PY'
import json, sys

seed = int(sys.argv[1])
quick = bool(int(sys.argv[2]))
fails = int(sys.argv[3])
steps_file, mut_file, metrics_file, out_file = sys.argv[4:8]

steps = []
for line in open(steps_file, encoding="utf-8"):
    parts = line.rstrip("\n").split("\t")
    if len(parts) == 3:
        steps.append({"name": parts[0], "status": parts[1], "seconds": int(parts[2])})

mutants = []
for line in open(mut_file, encoding="utf-8"):
    parts = line.rstrip("\n").split("\t")
    if len(parts) == 3:
        mutants.append({"name": parts[0], "status": parts[1], "killer": parts[2]})

metrics = {}
for line in open(metrics_file, encoding="utf-8"):
    line = line.strip()
    if line.startswith("K_METRIC:"):
        try:
            obj = json.loads(line.split("K_METRIC:", 1)[1].strip())
            t = obj.pop("test", "unknown")
            metrics[t] = obj
        except Exception:
            pass

data = {
    "seed": seed,
    "quick": quick,
    "passed": (fails == 0),
    "sdk": {
        "version": "2.3.4",
        "date": "2025-10-15",
        "license": "GPL-3.0",
        "archive_sha256": "d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca",
    },
    "tests": steps,
    "metrics": metrics,
    "mutants": mutants,
}
with open(out_file, "w", encoding="utf-8") as f:
    json.dump(data, f, indent=2, ensure_ascii=False)
PY

echo ""
echo "===================== ÖSSZEFOGLALÓ TÁBLÁZAT ====================="
printf "%-28s | %-8s | %s\n" "Teszt" "Állapot" "Idő (s)"
echo "--------------------------------------------------"
while IFS=$'\t' read -r name status secs; do
    printf "%-28s | %-8s | %7s\n" "$name" "$status" "$secs"
done < "$RESULTS_FILE"

if [ -s "$MUTANTS_FILE" ]; then
    echo ""
    echo "Mutánsok ($(wc -l < "$MUTANTS_FILE") db):"
    while IFS=$'\t' read -r mname mstatus mkiller; do
        printf "  %-40s -> %s (%s)\n" "$mname" "$mstatus" "$mkiller"
    done < "$MUTANTS_FILE"
fi

echo ""
echo "Eredményfájl: $JSON_OUT"
echo ""
if [ "$FAILS" -eq 0 ]; then
    echo "MINDEN ELLENŐRZÉS SIKERES (0 hiba)"
    exit 0
else
    echo "ELLENŐRZÉS SIKERTELEN ($FAILS hiba)"
    exit 1
fi
