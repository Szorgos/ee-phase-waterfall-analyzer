# Átadás: új nyelv hozzáadása az Aqulify-hoz

Dátum: 2026-11-09. Ez a leírás egy másik MI-nek szól, aki további nyelveket visz be a programba.

> **Kötelező, minden más előtt:** `szabalyok/szabalyok-mi.md` (a munkaszabályok: mikor „KÉSZ” valami, mi tilos,
> mit kell tesztelni és dokumentálni), és a tulajdonos szabályai a `szabalyok/atadas.md` 2. pontjában.
> A magyar a mérvadó nyelv; a dokumentáció, a commit-üzenet és a válasz magyarul készül.

---

## 1. Mi van már kész

- A felület (`gui_integrated_system.html`) öt nyelvet támogat: **hu** (forrás), **en**, **es**, **fr**, **zh**.
- A szövegek a `i18n/gui/<nyelv>.json` fájlokban vannak. Ezekből készül a `gui_i18n.js` (generált, nem szerkeszthető).
- A modulfülek és a Bass Enhancer / Magic Touch kártyák címei a `mod.*` kulcsokat használják (2026-11-09-i javítás).
  Nyelvváltáskor ezek is átállnak.

## 2. Hol vannak a nyelvi fájlok

| Fájl | Mire való | Megléte |
|---|---|---|
| `i18n/gui/<nyelv>.json` | a felület szövegei | van, öt nyelv |
| `i18n/native/<nyelv>.json` | Windows tálcamenü, üzenetablakok, telepítő; Linux ablak üzenetei | van |
| `i18n/scripts/<nyelv>.json` | `install.sh` / `uninstall.sh` üzenetei | van |
| `whatsnew.json` | rövid változásnapló verziónként, minden nyelven | van |

A generátor: `tools/i18n_build.py`. A `LANGS` lista (a fájl elején) az érvényes nyelvek listája, a `SOURCE = "hu"`.

## 3. Lépések egy új nyelvhez (példa: `xx`)

1. **Szövegfájlok.** Másold a `hu.json`-t mindhárom mappába `xx.json` néven (`i18n/gui`, `i18n/native`,
   `i18n/scripts`), és fordítsd le az értékeket. A kulcsok **pontosan** egyezzenek a `hu.json` kulcsaival: a generátor
   hiányzó, felesleges vagy üres kulcsnál hibával leáll. Ugyanannyi `{változó}`, ugyanazok a HTML-címkék (`<strong>`
   stb.) és ugyanazok a `%s` / `%d` jelek legyenek, mint a magyarban.
2. **Változásnapló.** A `whatsnew.json` minden bejegyzésébe (`notes`) add hozzá az `xx` tömböt, pontosan annyi
   ponttal, mint a `hu` tömb.
3. **Generátor.** A `tools/i18n_build.py` elején a `LANGS` listához add hozzá: `"xx"` (a sorrend a felülettel egyezzen).
4. **Felületi nyelvválasztó.** A `gui_integrated_system.html`-ben:
   - `AQ_LANGS` (kb. 2833. sor): `["xx", "Saját név"]` (a nyelv saját nevével, pl. „Deutsch”);
   - `AQ_LOCALES` (kb. 2834. sor): `xx: "xx-XX"` (a dátum- és számformátumhoz).
5. **Windows és Linux natív kód.** Az `include/aq_i18n.hpp` 107. sora környékén a nyelvkódokat a Windows-nyelvazonosítóra
   képezi (`case LANG_CHINESE: return "zh";`). Új nyelvnél ide is kell egy `case`, különben a rendszernyelv nem ismeri
   fel. Ellenőrizd a többi `LANG_*` ágat is.
6. **Telefonos / kisebb felület.** A `gui_player.js` a saját nyelvi szótárait **nem** a generátorból kapja: két helyen
   (kb. 85. és 1024. sor) `hu`, `en`, `es`, `fr`, `zh` blokkok vannak. Ezeket kézzel kell bővíteni, és a kulcsaiknak
   a `gui_player.js` saját logikájával kell egyezniük. Az `i18n_build.py` ezt a fájlt nem kezeli; a 2.x rendszer
   szerint a `tools/sync-player.mjs` állíthatja elő, ezt a 1.x tárolóban nem ellenőriztük. Ha a telefonos másolat (`www/gui`) is kell, a privát 2.x tárolóban a `tools/sync-gui.mjs` futtatandó.
7. **Generálás és ellenőrzés.** Az `ee-phase-waterfall-analyzer` mappában:
   ```bash
   python tools/i18n_build.py
   python tools/i18n_build.py --check
   ```
   Mindkettőnek „rendben” kimenettel és 0-s kilépési kóddal kell lefutnia. A generátor a `gui_i18n.js`-t, a
   `include/aq_i18n_strings.hpp`-t, az `install.sh` / `uninstall.sh` i18n blokkját és a `.desktop` fájl fordításait írja.
   Ezeket **ne** szerkeszd kézzel.

## 4. Ellenőrzés (kötelező a „KÉSZ” előtt)

A szabályok szerint a kód lefordulása nem elég. Legalább ezt kell megtenni:

1. **Helyi szerver:** `python -m http.server 8790 --bind 127.0.0.1` az `ee-phase-waterfall-analyzer` mappából,
   és a beépített böngészőben a `http://127.0.0.1:8790/gui_integrated_system.html` megnyitása. **Ne** a 47810-es
   porthoz kapcsolódj (az a telepített program motorja).
2. **Nyelvváltás és feliratok JavaScripttel:** a böngészőben futtatott kódban `setLanguage('xx', false)`, majd a
   `.plugin-tabs .tab-btn` elemek `textContent` értékeinek kiolvasása. Minden fül és a Bass Enhancer / Magic Touch
   kártyacím a nyelvnek megfelelően jelenjen meg. Utána `setLanguage('hu', false)`.
3. **Konzol:** `read_console_messages` — a nyelvváltáshoz nem tartozó hibák (például a motor hiánya) rendben vannak,
   de a nyelvi kódból származó hiba nem.
4. **Képernyő:** PC-méretben és telefonméretben (375 px) is nézd meg a fülsort: ha a hosszabb felirat elcsúsztatja
   vagy kettétöri a sort, az hiba.
5. **Tesztek:** a meglévő teszteket (`test_core`, `test_dsp`, `test_matrix`, `test_player`) futtasd, ha a kódban más
   is változott. Ha csak szöveg változott, a generátor `--check` a fő ellenőrzés.

## 5. Fordítási elvek (a meglévő nyelveknél már így vannak)

- **Termék- és algoritmusnevek:** a „Magic Touch”, „Crossfeed”, „Bass Enhancer” nevek a felületen angolul maradnak
  (az es/fr/zh fájlokban is). Ha egy nyelvben másként döntesz, az egész nyelven egységesen legyen, és írd le a
  döntést ebbe a dokumentumba.
- **Modulnevek:** a `mod.*` kulcsok értékei (felület, fülek és kártyacímek) a `cv.title`, `lim.title`, `agc.title`
  stb. már lefordított címeivel egyezzenek. Ez a táblázat a jelenlegi értékeket mutatja:

| Kulcs | hu | en | es | fr | zh |
|---|---|---|---|---|---|
| `mod.eq` | Equalizer | Equalizer | Ecualizador | Égaliseur | 均衡器 |
| `mod.crossfeed` | Crossfeed | Crossfeed | Crossfeed | Crossfeed | Crossfeed |
| `mod.convolver` | Convolver | Convolver | Convolucionador | Convolueur | 卷积器 |
| `mod.limiter` | Limiter | Limiter | Limitador | Limiteur | 限幅器 |
| `mod.bass` | Bass Enhancer | Bass Enhancer | Bass Enhancer | Bass Enhancer | Bass Enhancer |
| `mod.mono` | Mono | Mono | Mono | Mono | 单声道 |
| `mod.autogain` | Autogain | Autogain | Autogain | Gain automatique | 自动增益 |
| `mod.magic` | Magic Touch | Magic Touch | Magic Touch | Magic Touch | Magic Touch |

- **Fordítandó szöveg a HTML-ben:** csak az kerüljön a `data-i18n` attribútumos elembe, aminek a szövege a felhasználónak
  szól. A szöveget **`<span data-i18n="kulcs">…</span>`** formában csomagold, ne magára az elemre tedd, és a fülek
  `<span class="module-dot">` eleme maradjon a span **előtt**: az `applyI18nDom` az elem `textContent`-jét cseréli.

## 6. Ismert hiányosságok (ezt nem javítottuk)

- A HTML-ben még lehetnek fordítatlan, beégetett angol vagy magyar szövegek. A modulfülek és a két kártyacím
  javítva van; a teljes fájlt **nem** vizsgáltuk végig. Ellenőrzés: `Grep` a HTML szöveges csomópontjaira `data-i18n`
  nélkül (pl. `>[A-Z][a-z]+<` típusú minták, a `<button>` és `<h3>` elemekben).
- A `gui_player.js` szótárait az `i18n_build.py` nem kezeli (lásd 3.6). Nem ellenőriztük, hogy a `sync-player.mjs`
  ezt a fájlt az 1.x tárolóban felülírja-e.
- A telefonos másolat (`www/gui`, privát 2.x tároló) frissítésre vár.
- Valódi nyelvi tesztet (fordító- vagy anyanyelvi átnézést) nem végeztünk; a fordítások gépi megfontolás alapján
  készültek, és át kell nézni őket.

## 7. Commit és push

Csak a tulajdonos jóváhagyásával. Commit-üzenet magyarul, modellnév nélkül; a szerző
`Szorgos <noreply@anthropic.com>` (`git -c user.name=Szorgos -c user.email=noreply@anthropic.com commit ...`).
