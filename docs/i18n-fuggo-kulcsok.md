# Függő fordítások

Jelenleg **nincs** függő kulcs: minden felületi (`i18n/gui/`), telepítő-script (`i18n/scripts/`) és zenetári/rádió (`gui_player.js`, zenetár `STR`) szöveg mind a 23 nyelven le van fordítva (a 21 nem magyar/angol nyelv fordítása 2.9.0 előtt készült el, gépi/AI fordítás, anyanyelvi átnézés nélkül).

Új kulcs felvételekor, ha egyelőre csak magyarul és angolul készül el, sorold fel ide a kulcsokat, a többi nyelvben az angol szöveg álljon helyőrzőként (`python tools/i18n_build.py` lefuttatja az ellenőrzést), és a lista legyen a teendő a fordításig.

Fájlok: `i18n/gui/<nyelv>.json`, `i18n/scripts/<nyelv>.json`, a zenetár/rádió szövegei a `gui_player.js` második `STR` blokkjában; összeállítás: `python tools/i18n_build.py` (a `--check` csak ellenőriz).

Fordításkor ügyelj a változókra (`{rate}`, `{cpu}`, `{n}`, `{name}`, `{q}`, `{e}`), a HTML-jelekre és a márkanév-szabályra: a felületen nincs más cég/termék neve, csak az adatforrás (pl. radio-browser.info) és a kötelező jogi feltüntetés (`ao.asioCredit`: a „Steinberg ASIO SDK” és a védjegy-mondat szerkezete maradjon meg). A telepítő `[y/N]` válasza minden nyelven `y` marad, mert a script `i`/`y` betűt fogad el.
