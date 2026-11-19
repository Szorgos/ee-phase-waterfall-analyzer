#!/usr/bin/env bash
# Aqulify 23 nyelvű fordítás ellenőrző szkript (N1–N5, tiltott minták, L1–L7 mutánsok).
# Friss, normál klónból és munkapéldányból is futtatható.
# Használat:
#   bash tools/check_nyelvek.sh [--quick] [--base <ref>] [--out <json>]

set -euo pipefail
[ -d /usr/bin ] && PATH="$PATH:/usr/bin"

QUICK=0
BASE_REF=""
OUT_JSON="nyelvek_results.json"

while [ $# -gt 0 ]; do
    case "$1" in
        --quick) QUICK=1 ;;
        --base) BASE_REF="${2:-}"; shift ;;
        --out) OUT_JSON="${2:-}"; shift ;;
        -h|--help)
            echo "Használat: bash tools/check_nyelvek.sh [--quick] [--base <ref>] [--out <json>]"
            exit 0
            ;;
        *)
            echo "Ismeretlen kapcsoló: $1" >&2
            exit 2
            ;;
    esac
    shift
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

# Python keresése (Linux/macOS: python3, Windows Git Bash: python)
if command -v python3 >/dev/null 2>&1 && python3 --version >/dev/null 2>&1; then
    PY=python3
elif command -v python >/dev/null 2>&1 && python --version >/dev/null 2>&1; then
    PY=python
else
    echo "HIBA: Python nem található." >&2
    exit 1
fi
export PYTHONIOENCODING=utf-8

# Alap referencia meghatározása a BASE..HEAD ellenőrzéshez
if [ -z "$BASE_REF" ]; then
    if git rev-parse --verify origin/master >/dev/null 2>&1; then
        BASE_REF="origin/master"
    elif git rev-parse --verify master >/dev/null 2>&1; then
        BASE_REF="master"
    else
        BASE_REF=""
    fi
fi

TMPDIR_WORK="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_WORK"' EXIT

echo "=== Aqulify 23 nyelv ellenőrzés (ROOT=$ROOT) ==="

# 1. N1–N4 + L6–L7: check_i18n_pending.py
echo "[1/4] N1–N4, L6–L7 ellenőrzés (tools/check_i18n_pending.py)..."
PENDING_JSON="$TMPDIR_WORK/pending.json"
if "$PY" "$ROOT/tools/check_i18n_pending.py" --root "$ROOT" --json "$PENDING_JSON"; then
    N1_N4_OK=true
else
    N1_N4_OK=false
fi

# 2. N5: tools/i18n_build.py --check
echo "[2/4] N5 ellenőrzés (tools/i18n_build.py --check)..."
if "$PY" "$ROOT/tools/i18n_build.py" --check; then
    N5_OK=true
else
    N5_OK=false
fi

# 3. Tiltott minták ellenőrzése (BASE..HEAD): hu.json / en.json nem változott, modellnév nincs commitban
echo "[3/4] Tiltott minták ellenőrzése (BASE=${BASE_REF:-<nincs>})..."
FORBIDDEN_OK=true
FORBIDDEN_ISSUES=""

check_hu_en_unchanged() {
    local repo_dir="$1"
    local base="$2"
    local changed=""
    if [ -n "$base" ] && git -C "$repo_dir" rev-parse --verify "$base" >/dev/null 2>&1; then
        changed="$(git -C "$repo_dir" diff --name-only "$base"..HEAD; git -C "$repo_dir" diff --name-only; git -C "$repo_dir" diff --cached --name-only)"
    else
        changed="$(git -C "$repo_dir" diff --name-only; git -C "$repo_dir" diff --cached --name-only)"
    fi
    if printf '%s\n' "$changed" | grep -qE '^i18n/gui/(hu|en)\.json$'; then
        return 1
    fi
    return 0
}

if ! check_hu_en_unchanged "$ROOT" "$BASE_REF"; then
    echo "  HIBA: i18n/gui/hu.json vagy i18n/gui/en.json módosult!"
    FORBIDDEN_OK=false
    FORBIDDEN_ISSUES="${FORBIDDEN_ISSUES}hu_or_en_modified;"
else
    echo "  OK: hu.json és en.json változatlan."
fi

if [ -n "$BASE_REF" ] && git rev-parse --verify "$BASE_REF" >/dev/null 2>&1; then
    # Commit üzenetekben ne legyen modellnév
    if git log "$BASE_REF"..HEAD --format=%B | grep -qiE '\b(claude|gemini|gpt-?[0-9]|openai|anthropic|chatgpt|copilot|antigravity)\b'; then
        echo "  HIBA: tiltott modellnév szerepel egy commit-üzenetben!"
        FORBIDDEN_OK=false
        FORBIDDEN_ISSUES="${FORBIDDEN_ISSUES}model_name_in_commit;"
    else
        echo "  OK: nincs modellnév a commit-üzenetekben."
    fi
fi

# 4. Mutánsok (L1–L7)
MUTANTS_JSON="$TMPDIR_WORK/mutants.json"
if [ "$QUICK" -eq 1 ]; then
    echo "[4/4] Mutánsok kihagyva (--quick)."
    echo '{"skipped": true, "all_killed": true, "results": []}' > "$MUTANTS_JSON"
    MUTANTS_OK=true
else
    echo "[4/4] Mutánsok futtatása (L1–L7) elszigetelt másolatban..."
    MUT_ROOT="$TMPDIR_WORK/mut_repo"
    mkdir -p "$MUT_ROOT/i18n/gui" "$MUT_ROOT/tools"
    cp "$ROOT"/i18n/gui/*.json "$MUT_ROOT/i18n/gui/"
    cp "$ROOT/tools/check_i18n_pending.py" "$MUT_ROOT/tools/"

    # L5-höz egy minimális git repó a hu.json változás detektálására
    GIT_MUT="$TMPDIR_WORK/git_mut"
    mkdir -p "$GIT_MUT/i18n/gui"
    cp "$ROOT/i18n/gui/hu.json" "$ROOT/i18n/gui/en.json" "$GIT_MUT/i18n/gui/"
    git -C "$GIT_MUT" init -q
    git -C "$GIT_MUT" -c user.name=Test -c user.email=test@example.com add .
    git -C "$GIT_MUT" -c user.name=Test -c user.email=test@example.com commit -q -m "base"

    "$PY" - "$MUT_ROOT" "$GIT_MUT" "$MUTANTS_JSON" <<'PYEOF'
import json
import os
import shutil
import subprocess
import sys

mut_root, git_mut, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
checker = os.path.join(mut_root, "tools", "check_i18n_pending.py")


def mutate_json(lang, key, fn):
    p = os.path.join(mut_root, "i18n", "gui", f"{lang}.json")
    orig = open(p, encoding="utf-8").read()
    d = json.loads(orig)
    d[key] = fn(d[key])
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        json.dump(d, f, ensure_ascii=False, indent=2)
    try:
        r = subprocess.run([sys.executable, checker, "--root", mut_root],
                           capture_output=True, text=True, encoding="utf-8")
        return r.returncode != 0, (r.stdout + r.stderr).strip().splitlines()[-2:]
    finally:
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(orig)


def test_l5():
    p = os.path.join(git_mut, "i18n", "gui", "hu.json")
    orig = open(p, encoding="utf-8").read()
    d = json.loads(orig)
    d["la.title"] = "Módosított magyar szöveg"
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        json.dump(d, f, ensure_ascii=False, indent=2)
    try:
        r = subprocess.run(["git", "-C", git_mut, "diff", "--name-only"],
                           capture_output=True, text=True, encoding="utf-8")
        killed = "i18n/gui/hu.json" in r.stdout.splitlines()
        return killed, ["git diff --name-only -> " + r.stdout.strip()]
    finally:
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(orig)


mutants = [
    ("L1", "de: ao.asioDriver angolul marad", lambda: mutate_json("de", "ao.asioDriver", lambda _: "ASIO driver")),
    ("L2", "ja: hires.dspOff elveszti a {rate} változót", lambda: mutate_json("ja", "hires.dspOff", lambda s: s.replace("{rate}", "768 kHz"))),
    ("L3", "fr: ao.asioHint lezáratlan <strong> címke", lambda: mutate_json("fr", "ao.asioHint", lambda s: s + " <strong>")),
    ("L4", "fi: la.title magyar szöveget kap", lambda: mutate_json("fi", "la.title", lambda _: "Nagy a processzorterhelés")),
    ("L5", "hu.json egy értéke megváltozik", test_l5),
    ("L6", "es: ao.asioCredit elveszti a cégnevet", lambda: mutate_json("es", "ao.asioCredit", lambda s: s.replace("Steinberg Media Technologies GmbH", "Valaki Kft."))),
    ("L7", "de/fr/ja: ups.threadsHint hangminőség-javulást ígér", lambda: (
        all(mutate_json(l, "ups.threadsHint", lambda s, add=add: s + add)[0]
            for l, add in [("de", " Damit es besser klingt."), ("fr", " Pour un meilleur son."), ("ja", " 音質が向上します。")])
        , ["de (besser klingt), fr (meilleur son), ja (音質が向上) mind elkapva"]
    )),
]

results = []
all_killed = True
for mid, desc, fn in mutants:
    killed, detail = fn()
    if not killed:
        all_killed = False
    print(f"  {mid} ({desc}): {'MEGÖLVE' if killed else 'TÚLÉLTE!'}")
    results.append({"id": mid, "description": desc, "killed": killed, "detail": detail})

with open(out_path, "w", encoding="utf-8", newline="\n") as f:
    json.dump({"skipped": False, "all_killed": all_killed, "results": results}, f, ensure_ascii=False, indent=2)

sys.exit(0 if all_killed else 1)
PYEOF
    if [ $? -eq 0 ]; then
        MUTANTS_OK=true
    else
        MUTANTS_OK=false
    fi
fi

# Összesített eredményfájl (nyelvek_results.json)
"$PY" - "$PENDING_JSON" "$MUTANTS_JSON" "$OUT_JSON" "$N1_N4_OK" "$N5_OK" "$FORBIDDEN_OK" "$MUTANTS_OK" "$FORBIDDEN_ISSUES" <<'PYEOF'
import datetime
import json
import sys

pending_p, mutants_p, out_p, n14, n5, forb, mut, forb_iss = sys.argv[1:9]
pending = json.load(open(pending_p, encoding="utf-8")) if pending_p else {}
mutants = json.load(open(mutants_p, encoding="utf-8")) if mutants_p else {}

ok = (n14 == "true") and (n5 == "true") and (forb == "true") and (mut == "true")
res = {
    "timestamp": datetime.datetime.now().isoformat(timespec="seconds"),
    "ok": ok,
    "N1_to_N4_L6_L7": n14 == "true",
    "N5_i18n_build_check": n5 == "true",
    "forbidden_patterns_ok": forb == "true",
    "forbidden_issues": [x for x in forb_iss.split(";") if x],
    "mutants_ok": mut == "true",
    "mutants": mutants,
    "pending_summary": {
        "total_gui_keys": pending.get("total_gui_keys"),
        "pending_keys_count": pending.get("pending_keys_count"),
        "languages_checked": pending.get("languages_checked"),
        "checks": pending.get("checks"),
    },
}
with open(out_p, "w", encoding="utf-8", newline="\n") as f:
    json.dump(res, f, ensure_ascii=False, indent=2)
    f.write("\n")
print(f"Eredményfájl írva: {out_p} (ok={ok})")
sys.exit(0 if ok else 1)
PYEOF
