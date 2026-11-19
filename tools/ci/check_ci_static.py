#!/usr/bin/env python3
"""A CI-stabilitási kör (2026-11-19) statikus ellenőrzései; a tools/check_ci_stabilitas.sh hívja.

  apt        minden `apt-get update|install` a munkafolyamatokban `$APT_NET`-tel fut, és a munkafolyamat definiálja
             (Acquire::Retries); a telepített csomaglista bájtra azonos a BASE-zel (C1, S1)
  rnnoise    az RNNoise-letöltés minden munkafolyamatban kap sebességhatárt és teljes időkorlátot (C1)
  pi_wait    a pi-deb.yml upload feladata a feltöltés előtt a tools/ci/wait_release.sh-t hívja (C2)
  c3         a 7.3 határai változatlanok (0,05 dB, −90 dB, 50 eset, időalapú mag), és a mérés a lecsengésből
             számolt hosszal és a mély pontokon szinuszos gerjesztéssel fut (C3)
  c4         a test_measure_live minden mérés-/szintpróba-indítás előtt megvárja a munkamenet szabad állapotát (C4/T4)
  forbidden  a BASE..HEAD különbség hozzáadott soraiban nincs continue-on-error, `|| true` tesztlépésben, rögzített
             AQ_CHECK_SEED, és a motor forrása (include/, src/ a tesztek kivételével) nem változott (2. fejezet)

Használat: check_ci_static.py --root <fájlok> --git <tároló> --base <ref> [csak ezek a nevek…]
Kimenet: soronként `OK|HIBA <név>: …`; kilépési kód 0, ha minden kért ellenőrzés OK.
"""
import argparse
import glob
import os
import re
import subprocess
import shlex
import sys

WF = ".github/workflows"
# WSL-ből egy Windowsos munkapéldányhoz a git.exe kell (AQ_GIT="git.exe"); egyébként a rendszer git-je
GIT = shlex.split(os.environ.get("AQ_GIT", "git"))


def read(root, rel):
    with open(os.path.join(root, rel), encoding="utf-8") as f:
        return f.read()


def git_show(gitdir, base, rel):
    r = subprocess.run([*GIT, "-C", gitdir, "show", f"{base}:{rel}"], capture_output=True, text=True, encoding="utf-8")
    return r.stdout if r.returncode == 0 else None


def apt_install_lists(text):
    """Az `apt-get … install` parancsok csomaglistái (sorfolytatással), a hálózati kapcsolók nélkül."""
    out = []
    joined = re.sub(r"\\\n\s*", " ", text)
    for m in re.finditer(r"apt-get\s+(?:\$APT_NET\s+)?install\s+([^\n]*)", joined):
        out.append(" ".join(t for t in m.group(1).split()))
    return out


def check_apt(root, gitdir, base):
    errs = []
    for p in sorted(glob.glob(os.path.join(root, WF, "*.yml"))):
        rel = os.path.relpath(p, root).replace(os.sep, "/")
        t = read(root, rel)
        lines = [l for l in t.splitlines() if re.search(r"apt-get\s+(\$APT_NET\s+)?(update|install)", l) and not l.lstrip().startswith("#")]
        if not lines:
            continue
        if not re.search(r"^\s*APT_NET:\s*\"[^\"]*Acquire::Retries=\d+", t, re.M):
            errs.append(f"{rel}: apt-get van, de az APT_NET (Acquire::Retries) nincs definiálva")
        for l in lines:
            if "$APT_NET" not in l:
                errs.append(f"{rel}: újrapróbálás nélküli apt-get: {l.strip()[:90]}")
        old = git_show(gitdir, base, rel)
        if old is not None and apt_install_lists(old) != apt_install_lists(t):
            errs.append(f"{rel}: a telepített csomaglista eltér a {base}-tól")
    return errs


def check_rnnoise(root, gitdir, base):
    errs = []
    for p in sorted(glob.glob(os.path.join(root, WF, "*.yml"))):
        rel = os.path.relpath(p, root).replace(os.sep, "/")
        t = read(root, rel)
        if "fetch_rnnoise.sh" not in t:
            continue
        for k in ("AQ_FETCH_MIN_BPS", "AQ_FETCH_DEADLINE"):
            if not re.search(rf"^\s*{k}:\s*\"\d+\"", t, re.M):
                errs.append(f"{rel}: RNNoise-letöltés {k} nélkül")
    f = read(root, "tools/fetch_rnnoise.sh")
    if "--speed-limit" not in f or "AQ_FETCH_DEADLINE" not in f:
        errs.append("tools/fetch_rnnoise.sh: nincs sebességhatár / teljes időkorlát")
    return errs


def check_pi_wait(root, gitdir, base):
    t = read(root, f"{WF}/pi-deb.yml")
    up = t.split("\n  upload:", 1)[1] if "\n  upload:" in t else ""
    a, b = up.find("wait_release.sh"), up.find("gh release upload")
    if a < 0 or b < 0 or a > b:
        return ["pi-deb.yml: az upload feladat nem várja meg a kiadást a feltöltés előtt"]
    return []


def check_c3(root, gitdir, base):
    t = read(root, "src/test_multiout.cpp")
    need = {
        "if (err > 0.05) caseOk = false;": "a 7.3 pontonkénti határa 0,05 dB",
        "worstGlobalErr <= 0.05": "a 7.3 összesített határa 0,05 dB",
        "expDb > -90.0": "a −90 dB-es mérési küszöb",
        "for (int tc = 0; tc < 50; ++tc)": "50 eset",
        "unsigned seed = static_cast<unsigned>(std::time(nullptr));": "időalapú mag (AQ_CHECK_SEED felülírhatja)",
        "impulseLenForDecay(cEq, targetWay, sr, 200.0, 1.5, 10.0)": "C3/1: a mérés hossza a lecsengésből",
        "deep ? toneGainDb(": "C3/2: mély pontokon szinuszos mérés",
    }
    return [f"src/test_multiout.cpp: hiányzik vagy megváltozott – {why}: `{s}`" for s, why in need.items() if s not in t]


def check_c4(root, gitdir, base):
    t = read(root, "src/test_measure_live.cpp")
    errs = []
    body = t.split("int main()", 1)[1] if "int main()" in t else ""
    # minden indítás (a szándékosan hibás mikrofonos [3] kivételével, amely egy leállítás és busy-várakozás után jön)
    for m in re.finditer(r"s\.(startMeasurement|startLevelCheck)\(cfg, \"dac_st\"|s\.startLevelCheck\(\"dac_st\"", body):
        before = body[max(0, m.start() - 700): m.start()]
        if "waitNotBusy(s" not in before and "s.busy()" not in before:
            line = body[: m.start()].count("\n")
            errs.append(f"src/test_measure_live.cpp: a main() {line}. sora körüli indítás előtt nincs busy()-várakozás")
    return errs


def check_forbidden(root, gitdir, base):
    errs = []
    r = subprocess.run([*GIT, "-C", gitdir, "diff", "--name-only", f"{base}...HEAD"], capture_output=True, text=True)
    names = [n for n in r.stdout.split() if n]
    # a munkapéldány (nem commitolt) változásai is számítanak
    r2 = subprocess.run([*GIT, "-C", gitdir, "diff", "--name-only", "HEAD"], capture_output=True, text=True)
    names = sorted(set(names) | {n for n in r2.stdout.split() if n})
    for n in names:
        if (n.startswith("include/") or (n.startswith("src/") and not os.path.basename(n).startswith("test_"))):
            errs.append(f"motorforrás változott (jóváhagyás nélkül tilos): {n}")
    for n in names:
        if not (n.startswith(".github/") or n.startswith("tests/") or n.startswith("debian/") or n.startswith("packaging/")
                or n.startswith("tools/")):
            continue
        old = git_show(gitdir, base, n) or ""
        try:
            new = read(root, n)
        except OSError:
            continue
        added = [l for l in new.splitlines() if l not in set(old.splitlines())]
        for l in added:
            s = l.strip()
            if s.startswith("#"):
                continue
            if "continue-on-error" in s:
                errs.append(f"{n}: új continue-on-error: {s[:80]}")
            if n.startswith(".github/") and re.search(r"\|\|\s*true", s) and re.search(r"test_|run_all|_live", s):
                errs.append(f"{n}: `|| true` tesztlépésben: {s[:80]}")
            if re.search(r"AQ_CHECK_SEED\s*[:=]\s*\d", s):
                errs.append(f"{n}: rögzített AQ_CHECK_SEED: {s[:80]}")
    return errs


CHECKS = {"apt": check_apt, "rnnoise": check_rnnoise, "pi_wait": check_pi_wait, "c3": check_c3, "c4": check_c4,
          "forbidden": check_forbidden}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--git", required=True)
    ap.add_argument("--base", default="origin/master")
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()
    bad = 0
    for name in a.names or list(CHECKS):
        try:
            errs = CHECKS[name](a.root, a.git, a.base)
        except (OSError, KeyError, IndexError) as e:
            errs = [f"nem futtatható: {e}"]
        print(f"{'OK  ' if not errs else 'HIBA'} {name}" + ("" if not errs else ": " + " | ".join(errs)))
        bad += bool(errs)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
