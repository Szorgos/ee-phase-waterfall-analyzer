#!/usr/bin/env python3
"""
Aqulify 23 nyelvű felületi fordítás ellenőrző (N1–N4, L1–L7).

Ellenőrzi:
  N1: A 117 függő kulcs egyikében sem maradt angollal betűre azonos érték a 21 nyelven
      (kivéve a nyelvenként megindokolt kivétellistát).
  N2: Változó ({...}), HTML-címke (nyitó/záró egyensúly is) és printf-jel paritás
      minden kulcsra és nyelvre a magyarhoz (hu) képest.
  N3: Nincs magyar szöveg (ő/ű vagy magyar szavak) más nyelvben, és nincs nyers kulcs.
  N4: Az összes kulcsra riportolja a 21 nyelvben az angollal betűre azonos értékeket.
  L6: Az ao.asioCredit minden nyelven tartalmazza a Steinberg Media Technologies GmbH cégnevet.
  L7: Az ups.threads* és ups.core* kulcsokban nincs hangminőség-javulást ígérő szöveg.

Használat:
  python tools/check_i18n_pending.py [--root <mappa>] [--json <fájl>] [--verbose]
"""
import argparse
import json
import os
import re
import sys
from html.parser import HTMLParser

LANGS = [
    "hu", "en", "es", "fr", "zh", "de", "it", "nl", "pl", "ru", "pt", "ro",
    "el", "tr", "fi", "da", "nb", "sv", "he", "ja", "ko", "cs", "uk",
]
OTHER_LANGS = [l for l in LANGS if l not in ("hu", "en")]

# A 117 függő kulcs (63 eredeti + 2 módosult + 9 telefonos 1.2.0 + 42 hibajelentés + 1 ao.asioOn)
PENDING_KEYS = [
    # Nagy felbontás A–C + ASIO + SRC Referencia (63 + 2 módosult + ao.asioOn = 66)
    "ao.asio", "ao.asioCredit", "ao.asioDriver", "ao.asioDsdDop", "ao.asioDsdNative",
    "ao.asioErr_buffer", "ao.asioErr_busy", "ao.asioErr_channels",
    "ao.asioErr_com_create_failed", "ao.asioErr_create_buffers", "ao.asioErr_device_lost",
    "ao.asioErr_driver_not_found", "ao.asioErr_format", "ao.asioErr_hw_malfunction",
    "ao.asioErr_init_failed", "ao.asioErr_invalid_clsid", "ao.asioErr_no_driver",
    "ao.asioErr_no_dsp", "ao.asioErr_not_open", "ao.asioErr_open_failed",
    "ao.asioErr_other", "ao.asioErr_rate", "ao.asioErr_reset", "ao.asioErr_start_failed",
    "ao.asioFallback", "ao.asioFirst", "ao.asioHint", "ao.asioMissing", "ao.asioNone",
    "ao.asioOn", "ao.dsdDop", "ao.dsdDopHint", "ao.dsdNativeOn", "ao.exclRate",
    "hires.dspOff",
    "la.doneText", "la.doneTitle", "la.keep", "la.never", "la.optimize", "la.setting",
    "la.settingHint", "la.step.lower_target", "la.step.none", "la.step.precision24",
    "la.step.two_threads", "la.step.upsample_off", "la.title", "la.undo",
    "la.whyCpu", "la.whyXrun",
    "ups.core", "ups.coreHint", "ups.coreN", "ups.coreOff",
    "ups.maxRate", "ups.maxRateNone", "ups.presetReference", "ups.presetReferenceHint",
    "ups.statusCore", "ups.statusThreads",
    "ups.threads", "ups.threads1", "ups.threads2", "ups.threadsAuto", "ups.threadsHint",
    # Telefonos app 1.2.0, Hangkimenet képernyő (9)
    "ao.adminRequired", "ao.dsdFallbackPcm", "ao.dsdPathLabel", "ao.enableBtn",
    "ao.readOnlyTitle", "ao.replayGain", "ao.statusTitle",
    "ups.errPassbandRange", "ups.errStopbandRange",
    # Névtelen hibajelentés (42)
    "er.cancelBtn", "er.copied", "er.copyId",
    "er.err_network", "er.err_not_configured", "er.err_other",
    "er.err_rate_limited", "er.err_too_large",
    "er.firstRunText", "er.modalTitle", "er.no", "er.notConfigured",
    "er.noteLabel", "er.notePh", "er.oneTimeHint", "er.pickSymptom",
    "er.prevBannerText", "er.prevIntro", "er.prevReviewBtn",
    "er.previewBytes", "er.previewLoading", "er.previewRefresh", "er.previewTitle",
    "er.privacyShort", "er.reportBtn", "er.saveBtn", "er.saveFailed", "er.savedOk",
    "er.section", "er.sendBtn", "er.sendFailed", "er.sending", "er.sentOk", "er.setting",
    "er.sym_badSound", "er.sym_buttonNoEffect", "er.sym_crackle", "er.sym_freeze",
    "er.sym_noSound", "er.sym_other", "er.whatSends", "er.yes",
]

# Kivétellista a függő kulcsok között (nyelv -> {kulcs: indok})
# Olyan esetek, ahol az idegen nyelvű szöveg természetes módon betűre azonos az angollal.
GLOBAL_EXEMPTIONS = {
    "ao.asioOn": "Márkanév és változók (ASIO: {driver}, {format}) minden nyelven azonos",
    "ao.replayGain": "Nemzetközi szabvány/márkanév (ReplayGain) minden nyelven fordítás nélkül",
}

LANG_EXEMPTIONS = {
    "es": {
        "er.no": "Spanyolul a 'Nem' betűre 'No'",
    },
    "fr": {
        "ups.threads1": "Francia műszaki szókincsben a 'thread' átvett kifejezés ('1 thread')",
        "ups.threads2": "Francia műszaki szókincsben a 'threads' átvett kifejezés ('2 threads')",
    },
    "it": {
        "er.no": "Olaszul a 'Nem' betűre 'No'",
        "ups.coreN": "Olasz műszaki szókincsben a 'Core' átvett kifejezés ('Core {n}')",
        "ups.threads1": "Olasz műszaki szókincsben a 'thread' átvett kifejezés ('1 thread')",
    },
    "nl": {
        "ups.coreN": "Holland műszaki szókincsben a 'Core' átvett kifejezés ('Core {n}')",
        "ups.threads1": "Holland műszaki szókincsben a 'thread' átvett kifejezés ('1 thread')",
        "ups.threads2": "Holland műszaki szókincsben a 'threads' átvett kifejezés ('2 threads')",
    },
    "pt": {
        "ups.threads1": "Portugál műszaki szókincsben a 'thread' átvett kifejezés ('1 thread')",
        "ups.threads2": "Portugál műszaki szókincsben a 'threads' átvett kifejezés ('2 threads')",
    },
    "da": {
        "er.sendBtn": "Dánul a 'Küldés' felszólító módja betűre 'Send'",
        "ups.presetReference": "Dánul a 'Referencia' betűre 'Reference'",
    },
    "nb": {
        "er.sendBtn": "Norvég bokmålban a 'Küldés' felszólító módja betűre 'Send'",
    },
}

# Magyar specifikus szavak (kisbetűsen, szóhatárral), amelyek más nyelvben nem fordulhatnak elő
HU_WORD_RE = re.compile(
    r"\b("
    r"felmintavételezés\w*|processzorterhelés\w*|hibajelentés\w*|kizárólagos\w*|"
    r"visszavonás\w*|beállítás\w*|meghajtó\w*|hangmotor\w*|hangkimaradás\w*|"
    r"hangminőség\w*|számítási|dedikált|előnézet\w*|azonosító\w*|mentés\w*|"
    r"küldés\w*|optimalizálás\w*|ajánlott|nincs|kikapcsolva|bekapcsolás\w*"
    r")\b",
    re.IGNORECASE,
)
# Magyar hosszú ö/ü (ő, ű) egyetlen másik támogatott nyelv ábécéjében sem létezik
HU_CHAR_RE = re.compile(r"[őűŐŰ]")

# Nyers kulcs minta az értékekben
RAW_KEY_RE = re.compile(r"\b(?:ao|er|la|ups|hires)\.[a-zA-Z0-9_.]+\b")

# Hangminőség-javulást ígérő tiltott minták az ups.threads* és ups.core* kulcsokban (L7)
QUALITY_CLAIM_PATTERNS = [
    r"besser\w*\s+kling",
    r"bessere\s+klangqualit",
    r"verbessert\w*\s+(?:den\s+)?klang",
    r"meilleur\w*\s+son",
    r"meilleure\s+qualit[ée]\s+sonore",
    r"am[ée]liore\s+la\s+qualit[ée]",
    r"better\s+sound",
    r"improv\w*\s+sound\s+quality",
    r"cleaner\s+sound",
    r"音質が向上",
    r"音質向上",
    r"音が良く",
    r"音质提升",
    r"改善音质",
    r"음질\s*향상",
    r"더\s*좋은\s*음질",
    r"mejor\w*\s+(?:el\s+)?sonido",
    r"suono\s+migliore",
    r"migliora\s+la\s+qualit[àa]",
    r"melhor\w*\s+(?:o\s+)?som",
    r"betere\s+geluidskwaliteit",
    r"lepsz\w*\s+d[źz]wi[ęe]k",
    r"улучшает\s+(?:качество\s+)?звук",
    r"покращує\s+(?:якість\s+)?звук",
    r"parempi\s+äänenlaatu",
    r"bedre\s+lyd",
    r"bättre\s+ljud",
    r"daha\s+iyi\s+ses",
    r"καλύτερ\w*\s+ήχ",
    r"lep[šs][íi]\s+zvuk",
    r"sunet\s+mai\s+bun",
    r"איכות\s+שמע\s+טובה\s+יותר",
    r"משפר\s+את\s+איכות",
    r"jobb\s+hangminőség",
]
QUALITY_CLAIM_RE = re.compile("|".join(QUALITY_CLAIM_PATTERNS), re.IGNORECASE)

VOID_TAGS = {"br", "hr", "img", "input", "meta", "link"}


class TagBalanceChecker(HTMLParser):
    """Ellenőrzi, hogy minden megnyitott HTML-címke megfelelően le van-e zárva."""
    def __init__(self):
        super().__init__()
        self.stack = []
        self.tag_sequence = []
        self.unbalanced = []

    def handle_starttag(self, tag, attrs):
        t = tag.lower()
        self.tag_sequence.append(f"<{t}>")
        if t not in VOID_TAGS:
            self.stack.append(t)

    def handle_endtag(self, tag):
        t = tag.lower()
        self.tag_sequence.append(f"</{t}>")
        if self.stack and self.stack[-1] == t:
            self.stack.pop()
        else:
            self.unbalanced.append(f"váratlan záró címke </{t}>")

    def handle_startendtag(self, tag, attrs):
        t = tag.lower()
        self.tag_sequence.append(f"<{t}/>")


def check_html(s):
    parser = TagBalanceChecker()
    parser.feed(s)
    parser.close()
    issues = list(parser.unbalanced)
    for open_tag in parser.stack:
        issues.append(f"lezáratlan címke <{open_tag}>")
    return sorted(parser.tag_sequence), issues


def placeholders(s):
    return sorted(re.findall(r"\{(\w+)\}", s))


def printf_marks(s):
    return re.findall(r"%[sdu]|%%", s)


def is_exempt(lang, key):
    if key in GLOBAL_EXEMPTIONS:
        return GLOBAL_EXEMPTIONS[key]
    return LANG_EXEMPTIONS.get(lang, {}).get(key)


def run_checks(root):
    gui = {}
    errors = []
    for lang in LANGS:
        p = os.path.join(root, "i18n", "gui", f"{lang}.json")
        if not os.path.exists(p):
            errors.append(f"Hiányzó fájl: {p}")
            continue
        with open(p, encoding="utf-8") as f:
            gui[lang] = json.load(f)

    if errors:
        return {"ok": False, "errors": errors}

    hu = gui["hu"]
    en = gui["en"]

    # Ellenőrzés: minden PENDING_KEYS létezik hu-ban
    for k in PENDING_KEYS:
        if k not in hu:
            errors.append(f"PENDING_KEYS hiba: {k} nincs a hu.json fájlban")

    n1_failures = []
    n2_failures = []
    n3_failures = []
    l6_failures = []
    l7_failures = []
    n4_all_same_as_en = {}  # lang -> {"exempt_pending": [...], "other_keys": [...]}

    for lang in OTHER_LANGS:
        tbl = gui[lang]
        # Kulcs-paritás
        missing = [k for k in hu if k not in tbl]
        extra = [k for k in tbl if k not in hu]
        for k in missing:
            n2_failures.append(f"{lang}: hiányzó kulcs {k}")
        for k in extra:
            n2_failures.append(f"{lang}: fölösleges kulcs {k}")

        exempt_hits = []
        other_en_hits = []

        for k, hu_val in hu.items():
            if k not in tbl:
                continue
            val = tbl[k]
            en_val = en.get(k, "")

            if not isinstance(val, str) or (hu_val.strip() and not val.strip()):
                n2_failures.append(f"{lang}/{k}: üres érték")
                continue

            # N2: változók, HTML-címkék (egyensúly is!), printf jelek
            if placeholders(val) != placeholders(hu_val):
                n2_failures.append(
                    f"{lang}/{k}: változó-eltérés {placeholders(val)} != {placeholders(hu_val)}"
                )
            hu_tags, hu_html_issues = check_html(hu_val)
            val_tags, val_html_issues = check_html(val)
            if val_html_issues:
                n2_failures.append(f"{lang}/{k}: hibás HTML: {', '.join(val_html_issues)}")
            elif val_tags != hu_tags:
                n2_failures.append(f"{lang}/{k}: HTML-címke eltérés {val_tags} != {hu_tags}")
            if printf_marks(val) != printf_marks(hu_val):
                n2_failures.append(
                    f"{lang}/{k}: printf eltérés {printf_marks(val)} != {printf_marks(hu_val)}"
                )

            # N3: magyar szöveg és nyers kulcs
            if HU_CHAR_RE.search(val):
                n3_failures.append(f"{lang}/{k}: magyar ékezetes karakter (ő/ű): {val!r}")
            elif HU_WORD_RE.search(val):
                m = HU_WORD_RE.search(val)
                n3_failures.append(f"{lang}/{k}: magyar szó ({m.group(0)!r}): {val!r}")
            if RAW_KEY_RE.search(val):
                m = RAW_KEY_RE.search(val)
                n3_failures.append(f"{lang}/{k}: nyers kulcs ({m.group(0)!r}): {val!r}")

            # N1 és N4: angollal betűre azonos értékek
            if val == en_val:
                if k in PENDING_KEYS:
                    reason = is_exempt(lang, k)
                    if reason:
                        exempt_hits.append({"key": k, "value": val, "reason": reason})
                    else:
                        n1_failures.append(f"{lang}/{k}: angolul maradt: {val!r}")
                else:
                    other_en_hits.append(k)

            # L6: ao.asioCredit védjegy-ellenőrzés
            if k == "ao.asioCredit":
                for req in ("Steinberg Media Technologies GmbH", "Steinberg ASIO SDK 2.3.4", "GPLv3", "ASIO"):
                    if req not in val:
                        l6_failures.append(f"{lang}/ao.asioCredit: hiányzik a kötelező elem: {req!r}")

            # L7: ups.threads* és ups.core* hangminőség-ígéret tilalma
            if k.startswith(("ups.threads", "ups.core")):
                m = QUALITY_CLAIM_RE.search(val)
                if m:
                    l7_failures.append(
                        f"{lang}/{k}: tiltott hangminőség-ígéret ({m.group(0)!r}): {val!r}"
                    )

        n4_all_same_as_en[lang] = {
            "exempt_pending_count": len(exempt_hits),
            "exempt_pending": exempt_hits,
            "other_keys_count": len(other_en_hits),
            "other_keys": other_en_hits,
        }

    # N2 + L6 + L7 ellenőrzés hu és en fájlokra is (HTML egyensúly, védjegy)
    for base_lang in ("hu", "en"):
        for k, val in gui[base_lang].items():
            _, issues = check_html(val)
            if issues:
                n2_failures.append(f"{base_lang}/{k}: hibás HTML: {', '.join(issues)}")
            if k == "ao.asioCredit" and "Steinberg Media Technologies GmbH" not in val:
                l6_failures.append(f"{base_lang}/ao.asioCredit: hiányzik a cégnév")

    all_errors = errors + n1_failures + n2_failures + n3_failures + l6_failures + l7_failures
    return {
        "ok": len(all_errors) == 0,
        "total_gui_keys": len(hu),
        "pending_keys_count": len(PENDING_KEYS),
        "languages_checked": len(OTHER_LANGS),
        "checks": {
            "N1_pending_translated": {"ok": len(n1_failures) == 0, "failures": n1_failures},
            "N2_parity_html_printf": {"ok": len(n2_failures) == 0, "failures": n2_failures},
            "N3_no_hungarian_or_raw_key": {"ok": len(n3_failures) == 0, "failures": n3_failures},
            "L6_asio_credit_trademark": {"ok": len(l6_failures) == 0, "failures": l6_failures},
            "L7_no_audio_quality_claim": {"ok": len(l7_failures) == 0, "failures": l7_failures},
        },
        "N4_same_as_en_report": n4_all_same_as_en,
        "errors": all_errors,
    }


def main():
    ap = argparse.ArgumentParser(description="Aqulify 23 nyelvű i18n ellenőrző")
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    help="A tároló gyökérkönyvtára")
    ap.add_argument("--json", dest="json_out", help="JSON eredményfájl útvonala")
    ap.add_argument("--verbose", action="store_true", help="Részletes N4 kimenet")
    args = ap.parse_args()

    res = run_checks(args.root)
    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8", newline="\n") as f:
            json.dump(res, f, ensure_ascii=False, indent=2)
            f.write("\n")

    print(f"Összes GUI kulcs: {res.get('total_gui_keys')}, ellenőrzött függő kulcs: {res.get('pending_keys_count')}, nyelvek: {res.get('languages_checked')}")
    for name, info in res.get("checks", {}).items():
        status = "OK" if info["ok"] else f"HIBA ({len(info['failures'])})"
        print(f"  {name}: {status}")
        for err in info["failures"][:10]:
            print(f"    - {err}")

    if args.verbose and "N4_same_as_en_report" in res:
        print("N4 – angollal betűre azonos értékek (összes kulcs):")
        for lang, info in res["N4_same_as_en_report"].items():
            print(f"  {lang}: kivétel a függőkből={info['exempt_pending_count']}, egyéb régi kulcs={info['other_keys_count']}")

    if not res["ok"]:
        sys.exit(1)
    print("Minden ellenőrzés rendben (N1–N4, L6–L7).")


if __name__ == "__main__":
    main()
