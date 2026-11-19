# Függő fordítások

**Állapot (2026-11-19):** Nincs függő fordítás.

A nagy felbontás (A, B, C rész: terhelés-tanácsadó, DSP-korlát, felmintavételezés, ASIO-kimenet, natív DSD és DoP, Referencia előbeállítás), a telefonos app 1.2.0 Hangkimenet képernyő és a névtelen hibajelentés (`er.*`) összes felületi szövege (115 fordítandó kulcs + 2 nemzetközi kulcs: `ao.asioOn`, `ao.replayGain`) lefordításra került mind a 21 további nyelvre (`es, fr, zh, de, it, nl, pl, ru, pt, ro, el, tr, fi, da, nb, sv, he, ja, ko, cs, uk`), és a `gui_i18n.js` újra lett generálva (`python tools/i18n_build.py --check`).

Új felületi szöveg hozzáadásakor ide kerülnek a még lefordítatlan kulcsok.
Forrás: `i18n/gui/<nyelv>.json`, összeállítás: `python tools/i18n_build.py`.
