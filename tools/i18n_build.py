#!/usr/bin/env python3
"""
Aqulify fordítások összeállítása.

Források (a magyar a mérvadó, minden más nyelvnek ugyanazok a kulcsai vannak):
  i18n/gui/<nyelv>.json      – a felület (gui_integrated_system.html) szövegei
  i18n/native/<nyelv>.json   – Windows tálcamenü, üzenetablakok, telepítő; Linux ablak üzenetei
  i18n/scripts/<nyelv>.json  – install.sh / uninstall.sh üzenetei

Előállított (a tárolóban is benne lévő) fájlok:
  gui_i18n.js                    – window.AQ_I18N = { hu: {...}, en: {...}, ... }
  include/aq_i18n_strings.hpp    – UTF-8 szövegtábla a C++ kódnak (enum + tömbök)
  install.sh, uninstall.sh       – a „# >>> i18n” … „# <<< i18n” közötti blokk
  linux/io.github.szorgos.Aqulify.desktop – a GenericName[..] / Comment[..] sorok

Ellenőrzi: hiányzó / fölösleges kulcs, eltérő {változók}, eltérő HTML címkék, printf-jelek.
Használat:  python3 tools/i18n_build.py          (összeállítás)
            python3 tools/i18n_build.py --check  (csak ellenőrzés; eltérés esetén hibakód)
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Új nyelv: kód ide, a három JSON fájl mellé, és a felület AQ_LANGS listájába (gui_integrated_system.html)
LANGS = ["hu", "en"]
SOURCE = "hu"

errors = []


def load(domain):
    d = {}
    for lang in LANGS:
        p = os.path.join(ROOT, "i18n", domain, lang + ".json")
        if not os.path.exists(p):
            if "--partial" not in sys.argv:
                errors.append(f"{domain}/{lang}.json: hiányzik")
            continue
        with open(p, encoding="utf-8") as f:
            d[lang] = json.load(f)
    return d


def placeholders(s):
    return sorted(re.findall(r"\{(\w+)\}", s))


def tags(s):
    return sorted(re.findall(r"</?([a-zA-Z]+)[^>]*>", s))


def printf_marks(s):
    return re.findall(r"%[sdu]|%%", s)


def validate(domain, d):
    src = d.get(SOURCE, {})
    for lang, tbl in d.items():
        if lang == SOURCE:
            continue
        for k in src:
            if k not in tbl:
                errors.append(f"{domain}/{lang}: hiányzó kulcs {k}")
                continue
            if placeholders(tbl[k]) != placeholders(src[k]):
                errors.append(f"{domain}/{lang}: {k}: eltérő {{változók}}: {placeholders(tbl[k])} != {placeholders(src[k])}")
            if tags(tbl[k]) != tags(src[k]):
                errors.append(f"{domain}/{lang}: {k}: eltérő HTML címkék")
            if printf_marks(tbl[k]) != printf_marks(src[k]):
                errors.append(f"{domain}/{lang}: {k}: eltérő %-jelek")
            if not tbl[k].strip() and src[k].strip():
                errors.append(f"{domain}/{lang}: {k}: üres")
        for k in tbl:
            if k not in src:
                errors.append(f"{domain}/{lang}: fölösleges kulcs {k}")


def gen_gui_js(d):
    out = ["// Előállítva: tools/i18n_build.py – ne szerkeszd kézzel; a források: i18n/gui/<nyelv>.json",
           "window.AQ_I18N = {"]
    for lang in LANGS:
        if lang in d:
            out.append(f'"{lang}":' + json.dumps(d[lang], ensure_ascii=False, separators=(",", ":")) + ",")
    out.append("};")
    return "\n".join(out) + "\n"


def c_str(s):
    # A forrásfájlok UTF-8-ak (clang / gcc alapértelmezés): a szöveg UTF-8 bájtjai változatlanul kerülnek a literálba
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t") + '"'


def gen_native_hpp(d):
    src = d[SOURCE]
    keys = list(src.keys())
    ident = lambda k: re.sub(r"[^A-Za-z0-9]", "_", k)
    o = []
    o.append("// Előállítva: tools/i18n_build.py – ne szerkeszd kézzel; a források: i18n/native/<nyelv>.json")
    o.append("#pragma once")
    o.append("// clang-format off")
    o.append("namespace aq_i18n {")
    o.append(f"constexpr int kLangCount = {len(LANGS)};")
    o.append("inline const char* const kLangCodes[kLangCount] = {" + ", ".join(f'"{l}"' for l in LANGS) + "};")
    o.append("enum class Msg : int {")
    for k in keys:
        o.append(f"    {ident(k)},")
    o.append("    Count_")
    o.append("};")
    o.append("inline const char* const kStrings[kLangCount][static_cast<int>(Msg::Count_)] = {")
    for lang in LANGS:
        tbl = d.get(lang, {})
        o.append(f"  {{ // {lang}")
        for k in keys:
            v = tbl.get(k, d.get("en", {}).get(k, src[k]))
            o.append(f"    {c_str(v)},")
        o.append("  },")
    o.append("};")
    o.append("}  // namespace aq_i18n")
    return "\n".join(o) + "\n"


def sh_quote(s):
    return "'" + s.replace("'", "'\"'\"'") + "'"


def gen_sh_block(d, keys, desktop):
    # Bash 3-kompatibilis: függvények case-szel (nincs asszociatív tömb)
    o = ["# >>> i18n (előállítva: tools/i18n_build.py – ne szerkeszd kézzel; források: i18n/scripts/<nyelv>.json)"]
    o.append("# Nyelv: AQULIFY_LANG, a felületen választott nyelv (~/.config/aqulify/language.txt), majd a rendszeré")
    o.append('aq_detect_lang() {')
    o.append('    local l="${AQULIFY_LANG:-}" f="${XDG_CONFIG_HOME:-$HOME/.config}/aqulify/language.txt"')
    o.append('    [ -z "$l" ] && [ -r "$f" ] && l="$(head -n 1 "$f" 2>/dev/null || true)"')
    o.append('    [ -z "$l" ] && l="${LC_ALL:-${LC_MESSAGES:-${LANG:-en}}}"')
    o.append('    l="${l%%[_.@-]*}"')
    o.append('    case "$l" in')
    o.append("        " + "|".join(LANGS) + ') printf "%s" "$l" ;;')
    o.append('        *) printf "en" ;;')
    o.append('    esac')
    o.append('}')
    o.append('AQ_LANG="$(aq_detect_lang)"')
    o.append('# msg_text NYELV KULCS – a szöveg (printf formátum) az adott nyelven; ha nincs, angolul')
    o.append('msg_text() {')
    o.append('    local f=""')
    o.append('    case "$1" in')
    for lang in LANGS:
        if lang == "en":
            continue
        tbl = d.get(lang, {})
        o.append(f'    {lang}) case "$2" in')
        for k in keys:
            if k in tbl:
                o.append(f"        {k}) f={sh_quote(tbl[k])} ;;")
        o.append("        esac ;;")
    o.append("    esac")
    o.append('    if [ -z "$f" ]; then')
    o.append('        case "$2" in')
    for k in keys:
        o.append(f"        {k}) f={sh_quote(d['en'].get(k, d[SOURCE][k]))} ;;")
    o.append("        esac")
    o.append("    fi")
    o.append('    printf "%s" "$f"')
    o.append("}")
    o.append('# msg KULCS [argumentumok...] – a választott nyelven (%s helyére az argumentumok)')
    o.append('msg() {')
    o.append('    local f')
    o.append('    f="$(msg_text "$AQ_LANG" "$1")"; shift')
    o.append('    # shellcheck disable=SC2059')
    o.append('    printf "$f" "$@"')
    o.append("}")
    if desktop:
        o.append("# desktop_l10n MEZŐ KULCS – a .desktop fájl fordított sorai (MEZŐ[nyelv]=...)")
        o.append("desktop_l10n() {")
        o.append("    local l")
        o.append("    for l in " + " ".join(x for x in LANGS if x != "en") + "; do")
        o.append('        printf "%s[%s]=%s\\n" "$1" "$l" "$(msg_text "$l" "$2")"')
        o.append("    done")
        o.append("}")
    o.append("# <<< i18n")
    return "\n".join(o)


def write_if_changed(path, content, check):
    full = os.path.join(ROOT, path)
    old = open(full, encoding="utf-8").read() if os.path.exists(full) else None
    if old == content:
        return
    if check:
        errors.append(f"{path}: nem friss (futtasd: python3 tools/i18n_build.py)")
        return
    with open(full, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    print("írva:", path)


def splice_sh(path, block, check):
    full = os.path.join(ROOT, path)
    s = open(full, encoding="utf-8").read()
    m = re.search(r"# >>> i18n.*?# <<< i18n", s, re.S)
    if not m:
        errors.append(f"{path}: nincs „# >>> i18n” … „# <<< i18n” blokk")
        return
    new = s[:m.start()] + block + s[m.end():]
    write_if_changed(path, new, check)


def gen_desktop(path, d):
    """A csomagolt .desktop fájl fordított GenericName / Comment sorai (az alap sor angol)."""
    full = os.path.join(ROOT, path)
    lines = [l for l in open(full, encoding="utf-8").read().splitlines()
             if not re.match(r"^(GenericName|Comment)\[", l)]
    out = []
    for l in lines:
        if l.startswith("GenericName="):
            out.append("GenericName=" + d["en"]["inst.desktopGeneric"])
            out += [f"GenericName[{x}]={d[x]['inst.desktopGeneric']}" for x in LANGS if x != "en" and x in d]
        elif l.startswith("Comment="):
            out.append("Comment=" + d["en"]["inst.desktopComment"])
            out += [f"Comment[{x}]={d[x]['inst.desktopComment']}" for x in LANGS if x != "en" and x in d]
        else:
            out.append(l)
    return "\n".join(out) + "\n"


def main():
    check = "--check" in sys.argv
    gui = load("gui")
    validate("gui", gui)
    native = load("native") if os.path.isdir(os.path.join(ROOT, "i18n", "native")) else None
    if native:
        validate("native", native)
    scripts = load("scripts") if os.path.isdir(os.path.join(ROOT, "i18n", "scripts")) else None
    if scripts:
        validate("scripts", scripts)
    if errors and not check:
        print("\n".join(errors))
        sys.exit(1)
    write_if_changed("gui_i18n.js", gen_gui_js(gui), check)
    if native:
        write_if_changed("include/aq_i18n_strings.hpp", gen_native_hpp(native), check)
    if scripts:
        src = scripts[SOURCE]
        for sh in ("install.sh", "uninstall.sh"):
            prefix = "inst." if sh == "install.sh" else "uninst."
            keys = [k for k in src if k.startswith(prefix) or k.startswith("common.")]
            splice_sh(sh, gen_sh_block(scripts, keys, sh == "install.sh"), check)
        write_if_changed("linux/io.github.szorgos.Aqulify.desktop", gen_desktop("linux/io.github.szorgos.Aqulify.desktop", scripts), check)
    if errors:
        print("\n".join(errors))
        sys.exit(1)
    print("rendben:", ", ".join(LANGS))


if __name__ == "__main__":
    main()
