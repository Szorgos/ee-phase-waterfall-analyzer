# Ellenőrzés: 23 nyelv, telefonnézet (2026-11-09)

Ez a dokumentum a 18 új nyelv (de, it, nl, pl, ru, pt, ro, el, tr, fi, da, nb, sv, he, ja, ko, cs, uk) és a
kapcsolódó felület ellenőrzését rögzíti a `szabalyok/szabalyok-mi.md` 4. pontja szerint.

## 1. Mit csinált a lépés

- Ellenőriztem a fordítási fájlokat (gui, native, scripts), a generátort, a Linux- és Windows-fordítást, a shell-szkripteket,
  a felület nyelvváltását és a telefonszéles nézetet mind a 23 nyelven.
- Javítottam a `gui_player.js` (2.x lejátszósáv szótárai) három hibáját (lásd 4. pont).

## 2. Hogyan ellenőriztem

| Mit | Hogyan | Eredmény |
|---|---|---|
| Fordítási kulcsok | `tools/i18n_build.py --check` | rendben, 23 nyelv |
| Kulcsfedettség | Python: minden nyelv kulcsait a `hu` kulcsaival vetettem össze | gui 648, native 110, scripts 46 kulcs, 0 hiányzó, 0 üres, 0 fölösleges |
| Maradt magyar szöveg | Python: a `hu` értékkel azonos, nem latin írású nyelvek értékei | csak termék- és rövidítésnevek (Crossfeed, Magic Touch, TPDF, USB, HDMI, számok) |
| Mintaszöveg-minőség | a nyolc modulfül, kártyacím, tálcamenü, támogatás-gomb, „Kész” és ablakcím kiolvasása mind a 23 nyelven | kézi áttekintés, lásd 5. pont |
| Shell-szkriptek | `bash -n install.sh`, `bash -n uninstall.sh` (WSL) | mindkettő hibátlan |
| `.desktop` fájl | nincs `desktop-file-validate` a WSL-ben | nem ellenőriztem |
| C++ fordítás Linuxon | CMake + Ninja, `~/aq-i18n-check` build mappa a forrásfán kívül | fordítás hibátlan (467/467) |
| Linux tesztek | `test_ringbuffer`, `test_core`, `test_dsp`, `test_matrix`, `test_player` | mind 0 hibával (exit 0) |
| C++ keresztfordítás Windowsra | llvm-mingw, `~/aq-i18n-win` build mappa | fordítás hibátlan (1102/1102) |
| Windows tesztek | nem futtattam | nincs eredmény |
| JavaScript-szintaxis | `node --check gui_player.js` (WSL Node 22) | javítás után hibátlan |
| Szótárak egyezése | `player_keys.js`: a két `STR` szótár nyelvenként a angol kulcsaival vetve | javítás után: 0 hiányzó kulcs, csak 2 fölösleges kulcs (`o_before`, `o_after`) |
| Felület nyelvváltása, PC-méret | böngészőben (beépített), `setLanguage(l)` minden nyelvre, JS-ellenőrzés | 0 hiányzó / nyers kulcs / `undefined` felirat |
| Felület nyelvváltása, telefonméret | 375×812, ugyanaz az ellenőrzés, plusz vízszintes túlcsordulás mérése | 0 hiba feliratban, nincs oldalszintű vízszintes görgetés |
| Konzolhibák | `read_console_messages` telefonméretben | csak `three.min.js` 404: a fejlesztői szerverről hiányzik, az install a telepítéskor másolja (`install.sh` 1239. sor), a felület CDN-tartalékot is tartalmaz |
| Sötét téma | telefonméret, sötét sémával, képernyőkép | rendben |

## 3. Telefonnézet megfigyelései

- Telefonméretben a modulfülek nem egy sorban vannak, hanem több sorba törnek (németben öt sor, a fülek
  `getBoundingClientRect` szerint). A fix lejátszósáv az alján van, ezért az alsó fülsorok görgetéssel érhetők el.
  A hosszabb feliratú nyelveknél a sorok száma nő; a túlcsordulást nem találtam (a fülek nem vágódnak le).
- **Felső sáv, telefonméret (375 px): nem fér el.** A „Kimenet”, „Burn-in” és „Live – EQ APO” elemek a képernyőn kívül
  vannak (például angolban a „Live – EQ APO plugin” jobb széle kb. 910 px), a „Bibliothek / Bibliothèque / Библиотека”
  gomb a szélen csúszik le. Ez nyelvfüggetlen elrendezési hiba, de a hosszabb feliratoknál rosszabb. Nem javítottam, mert
  elrendezési döntést igényel (görgethető sáv vagy több sorba törés).
- **Hebrew (he):** a felület nem vált RTL elrendezésre (`document.documentElement.dir` marad `ltr`). A szöveg jó, de az
  elrendezés nem tükröződik. Ehhez a `setLanguage` függvényben a `dir` beállítása és RTL CSS kell.

## 4. Javított hibák a `gui_player.js`-ben

1. **Szintaktikai hiba (2 hely):** `},,` állt: az első szótár koreai blokkja után, és a második szótár cseh blokkja
   előtt. Ez JavaScript-hiba, ami miatt a fájl nem futhat le.
2. **Szintaktikai hiba (7 sor):** az olasz, holland szövegekben a `"{name}"` / `"{title}"` helyőrzők nyitó-záró
   idézőjelei nem voltak escape-elve (például `"Eliminare la playlist "{name}"?"`). Escape-eltem.
3. **Hiányzó fordítások cseh és ukrán nyelven:** a második szótárban 34-34 kulcs hiányzott (például `s_added`,
   `optCopyAlways`, `removeFolderConfirm`), így a felület a nyers kulcsot mutatta volna. Lefordítottam (lásd 5. pont).

## 5. Fordítási minőség: amit ellenőriztem és amit nem

- Ellenőriztem: a tálca- és ablakszövegeket, a modulneveket, a „Kész”/„Finished” típusú állapotüzeneteket és a
  támogatás-gombot mind a 23 nyelven, valamint a lejátszósáv és a zenetár szótárait kulcsszinten.
- Nem ellenőriztem: anyanyelvi lektorálást. A fordítások gépi megfontolással készültek; a jogi szöveg (`cf.desc`,
  licencek) és a telepítő hibaüzenetei külön átnézést igényelnek.
- A cseh és ukrán kiegészítések az én fordításaim (ez az ellenőrzés részeként készült), ezért ezeket is át kell nézni.

## 5a. Újraellenőrzés (a javítások után)

- `tools/i18n_build.py --check`: rendben, 23 nyelv.
- `node --check gui_player.js`: hibátlan.
- `gui_player.js` szótárak (`player_keys`/`verify_player.js`): 23 nyelv, mind a két szótárban 0 hiányzó, 0 üres,
  0 helyőrző-eltérés az angolhoz képest.
- Böngésző: a könyvtár-nézetet mind a 23 nyelven megnyitottam, JavaScript-hiba nem keletkezett, nyers kulcs nem látszott.
  A `cs` és `uk` könyvtárfeliratok élő megjelenítését (telefonos kapcsolat kell hozzá) nem próbáltam, csak a
  szótár-ellenőrzés fedi le.
- Fülcímek és kártyacímek mind a 23 nyelven a várt szöveget mutatják.
- A konzolban csak a `three.min.js` 404 jelenik meg (lásd 2. pont).

## 6. Nem ellenőrizve

- Windows-on futtatott tesztek (csak a keresztfordítást néztem).
- `.desktop` fájl validálása.
- Telefon-app (`www/`, privát 2.x tároló) — ez a dokumentum a PC-s felületet tesztelte telefonméretben.
- Valódi telefon és valódi hangkimenet.
- A felső sáv javítása és a Hebrew RTL — lásd 3. pont.
- Anyanyelvi fordításellenőrzés.
- Commit és push: nincs, a változások a munkafában vannak.
