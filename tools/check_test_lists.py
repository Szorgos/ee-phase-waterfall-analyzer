#!/usr/bin/env python3
"""A tesztlisták egy helyen (C5): a CMake-ben definiált tesztbinárisok és a platformok tesztlistái egyeznek-e a
docs/tesztek.md táblázatával.

Minden CMake-beli `add_executable(test_…)` legyen a táblázat egy sora. Minden cella vagy „igen” (a platform listája
tartalmazza a tesztet), vagy „nem: <indok>” (a lista nem tartalmazza, és az indok le van írva). Eltérés = hiba:
  - a lista tartalmazza, de a táblázat „nem”-et ír (a táblázat elavult);
  - a lista nem tartalmazza, de a táblázat „igen”-t ír (a teszt indok nélkül kimaradt a listából);
  - „nem” indok nélkül; ismeretlen teszt a táblázatban; CMake-teszt, amely hiányzik a táblázatból.

A listák forrása (csak olvasás):
  Windows      .github/workflows/windows-tests.yml   a `for t in …; do` ciklus
  macOS        .github/workflows/macos.yml           a `for t in …; do` ciklus
  Linux deb    debian/rules                          a `for t in …; do` ciklus
  Fedora rpm   packaging/fedora/aqulify.spec         a `for t in …; do` ciklus
  Arch         packaging/arch/PKGBUILD               a `for t in …; do` ciklus
  Pi           debian/rules (a tools/pi/pi_deb_build.sh azzal épít; ezt is ellenőrzi)
  live-tests   .github/workflows/live-tests.yml      `./build/test_…` és a meghívott tests/live/*.sh szkriptek

Használat: tools/check_test_lists.py [--root <tároló>] [--print]
Kilépési kód: 0 = egyezik, 1 = eltérés, 2 = a forrás nem olvasható / nem értelmezhető.
"""
import argparse
import os
import re
import sys

PLATFORMS = ["Windows", "macOS", "Linux deb", "Fedora rpm", "Arch", "Pi", "live-tests"]
BEGIN = "<!-- tesztlista:kezdet -->"
END = "<!-- tesztlista:vége -->"
FOR_RE = re.compile(r"for t in ((?:test_\w+\s*)+);\s*do")


def read(root, rel):
    with open(os.path.join(root, rel), encoding="utf-8") as f:
        return f.read()


def for_list(root, rel):
    m = FOR_RE.search(read(root, rel))
    if not m:
        raise ValueError(f"{rel}: nincs `for t in test_…; do` ciklus")
    return set(m.group(1).split())


def cmake_tests(root):
    return set(re.findall(r"add_executable\((test_\w+)", read(root, "CMakeLists.txt")))


def live_list(root):
    wf = read(root, ".github/workflows/live-tests.yml")
    found = set(re.findall(r"\./build/(test_\w+)", wf))
    for script in set(re.findall(r"bash (tests/live/\w+\.sh)", wf)):
        found |= set(re.findall(r"/(test_\w+)\"", read(root, script)))
    return found


def platform_lists(root):
    deb = for_list(root, "debian/rules")
    pi_src = read(root, "tools/pi/pi_deb_build.sh")
    if "dpkg-buildpackage" not in pi_src:
        raise ValueError("tools/pi/pi_deb_build.sh: nem a debian/rules-szal (dpkg-buildpackage) épít – a Pi-oszlop forrása kérdéses")
    return {
        "Windows": for_list(root, ".github/workflows/windows-tests.yml"),
        "macOS": for_list(root, ".github/workflows/macos.yml"),
        "Linux deb": deb,
        "Fedora rpm": for_list(root, "packaging/fedora/aqulify.spec"),
        "Arch": for_list(root, "packaging/arch/PKGBUILD"),
        "Pi": set(deb),
        "live-tests": live_list(root),
    }


def parse_table(root):
    text = read(root, "docs/tesztek.md")
    if BEGIN not in text or END not in text:
        raise ValueError(f"docs/tesztek.md: hiányzik a {BEGIN} … {END} jelölés")
    body = text.split(BEGIN, 1)[1].split(END, 1)[0]
    rows = [r.strip() for r in body.splitlines() if r.strip().startswith("|")]
    header = [c.strip() for c in rows[0].strip("|").split("|")]
    if header[1:] != PLATFORMS:
        raise ValueError(f"docs/tesztek.md: a fejléc oszlopai {header[1:]}, várt {PLATFORMS}")
    table = {}
    for r in rows[2:]:
        cells = [c.strip() for c in r.strip("|").split("|")]
        name = cells[0].strip("`")
        if len(cells) != len(PLATFORMS) + 1:
            raise ValueError(f"docs/tesztek.md: {name}: {len(cells) - 1} cella, várt {len(PLATFORMS)}")
        table[name] = dict(zip(PLATFORMS, cells[1:]))
    return table


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap.add_argument("--print", action="store_true", help="a listák szerinti mátrix kiírása (a táblázat írásához)")
    a = ap.parse_args()
    try:
        tests = cmake_tests(a.root)
        lists = platform_lists(a.root)
        if a.print:
            for t in sorted(tests):
                print(t.ljust(18), " ".join(("igen" if t in lists[p] else "-").ljust(len(p)) for p in PLATFORMS))
            return 0
        table = parse_table(a.root)
    except (OSError, ValueError, IndexError) as e:
        print(f"HIBA: {e}")
        return 2

    errors = []
    for t in sorted(tests - table.keys()):
        errors.append(f"{t}: a CMake-ben van, a docs/tesztek.md táblázatából hiányzik")
    for t in sorted(table.keys() - tests):
        errors.append(f"{t}: a táblázatban van, a CMake-ben nincs ilyen tesztbináris")
    for p in PLATFORMS:
        for t in sorted(lists[p] - tests):
            errors.append(f"{p}: a lista ismeretlen tesztet futtat: {t}")
    for t in sorted(tests & table.keys()):
        for p in PLATFORMS:
            cell = table[t][p]
            listed = t in lists[p]
            if cell == "igen":
                if not listed:
                    errors.append(f"{p}: {t} a táblázat szerint fut, de a listából indok nélkül hiányzik")
            elif cell.startswith("nem:"):
                if len(cell[4:].strip()) < 10:
                    errors.append(f"{p}: {t}: „nem” indok nélkül (vagy túl rövid indok): {cell!r}")
                if listed:
                    errors.append(f"{p}: {t} a listában szerepel, a táblázat mégis „nem”-et ír")
            else:
                errors.append(f"{p}: {t}: érvénytelen cella {cell!r} (igen | nem: <indok>)")

    runs = sum(1 for t in tests for p in PLATFORMS if t in lists[p])
    print(f"Tesztlisták: {len(tests)} tesztbináris × {len(PLATFORMS)} platform; {runs} futás, "
          f"{len(tests) * len(PLATFORMS) - runs} indokolt kihagyás")
    for e in errors:
        print(f"  [HIBA] {e}")
    print("EGYEZIK" if not errors else f"ELTÉRÉS ({len(errors)} hiba)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
