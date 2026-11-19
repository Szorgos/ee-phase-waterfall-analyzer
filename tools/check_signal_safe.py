#!/usr/bin/env python3
"""Statikus ellenőrzés: a jelzés- / kivételkezelő kód (include/crash_handler.hpp, az AQ_SIGNAL_SAFE_BEGIN és
AQ_SIGNAL_SAFE_END jelölés közötti rész) csak aszinkron-jelzésbiztos műveletet használhat: nincs foglalás (malloc,
new, std::string, std::vector…), nincs stdio / iostream (printf, fwrite, cout…), nincs zár és kivétel.
Használat: check_signal_safe.py [fájl …]   (alap: include/crash_handler.hpp a tároló gyökeréből)
Kilépési kód: 0 = rendben, 1 = tiltott hívás, 2 = a jelölt rész hiányzik vagy túl rövid.
"""
import os, re, sys

FORBIDDEN = [
    r"\bmalloc\b", r"\bcalloc\b", r"\brealloc\b", r"\bfree\s*\(", r"\bstrdup\b", r"\bnew\b", r"\bdelete\b",
    r"\bprintf\b", r"\bfprintf\b", r"\bsprintf\b", r"\bsnprintf\b", r"\bputs\b", r"\bfputs\b", r"\bfwrite\b",
    r"\bfopen\b", r"\bfflush\b", r"\bfclose\b", r"\bsyslog\b", r"(?<![\w_])exit\s*\(",
    r"std::string\b", r"std::wstring\b", r"std::vector\b", r"std::map\b", r"std::function\b", r"std::to_string\b",
    r"std::cout\b", r"std::cerr\b", r"std::clog\b", r"\bostringstream\b", r"\bstringstream\b",
    r"std::mutex\b", r"std::lock_guard\b", r"std::unique_lock\b", r"\.lock\s*\(", r"\bthrow\b",
    r"\blocaltime\b", r"\bstrftime\b", r"\bgetenv\b",
]


def strip(code):
    code = re.sub(r"/\*.*?\*/", " ", code, flags=re.S)
    code = re.sub(r"//[^\n]*", " ", code)
    code = re.sub(r'"(\\.|[^"\\])*"', '""', code)
    code = re.sub(r"'(\\.|[^'\\])'", "''", code)
    return code


def check(path):
    text = open(path, encoding="utf-8").read()
    b = text.find("// AQ_SIGNAL_SAFE_BEGIN")
    e = text.find("// AQ_SIGNAL_SAFE_END")
    if b < 0 or e < 0 or e <= b:
        print(f"  [HIBA] {path}: a jelölt rész (AQ_SIGNAL_SAFE_BEGIN / END) hiányzik")
        return 2
    region = text[b:e]
    lines = region.count("\n")
    if lines < 20 or "onSignal" not in region:
        print(f"  [HIBA] {path}: a jelölt rész túl rövid ({lines} sor) vagy nincs benne a kezelő")
        return 2
    first_line = text[:b].count("\n") + 1
    bad = []
    for i, raw in enumerate(region.split("\n")):
        code = strip(raw)
        for pat in FORBIDDEN:
            if re.search(pat, code):
                bad.append((first_line + i, pat, raw.strip()))
    for ln, pat, src in bad:
        print(f"  [HIBA] {path}:{ln}: tiltott a jelzéskezelőben ({pat}): {src}")
    if not bad:
        print(f"  [OK]   {path}: a jelzéskezelő része ({lines} sor) aszinkron-jelzésbiztos")
    return 1 if bad else 0


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    files = sys.argv[1:] or [os.path.join(root, "include", "crash_handler.hpp")]
    rc = 0
    for f in files:
        rc = max(rc, check(f))
    return rc


if __name__ == "__main__":
    sys.exit(main())
