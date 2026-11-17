# Aktív keresztváltó és útonkénti EQ – felhasználói útmutató

Az **Aktív keresztváltó** mód (a **Kimenetek** fülön) lehetővé teszi, hogy 2–4 hangkártyán keresztül külön hangszóró-utakat (például mélysugárzót és szatelit hangfalakat, vagy mély-, közép- és magassugárzót) hajts meg Linkwitz–Riley szűrőkkel (12 / 24 / 48 dB/oktáv).

## Mire való az útonkénti EQ?

A közös EQ (a fő láncban) még a sávfelosztás **előtt** minden hangszóróra együtt hat. Ezzel szemben az **útonkénti EQ** közvetlenül a keresztváltó szűrői **után**, külön-külön csak az adott hangszóróra dolgozik, nulla hozzáadott késleltetéssel.

Tipikus felhasználások:
- **Mélysugárzó (subwoofer):** egyetlen erős szobai állóhullám („dübörgés”) célzott csillapítása (pl. −6 dB, 55 Hz, Q = 3–4), miközben a fő hangfalak hangzása érintetlen marad.
- **Magassugárzó / középsugárzó:** egy adott hangszóró saját hangszínének finomhangolása vagy sávhatár környéki egyenetlenségének simítása (Bell, Mélypolc / Low shelf, Magaspolc / High shelf).

## Hol találod és hogyan használd?

1. Nyisd meg a **Kimenetek** fület, kapcsold be a több kimenetet, majd állítsd az üzemmódot **Aktív keresztváltó**-ra.
2. Minden kimenet sorában megjelenik egy **EQ** gomb. Kattints rá a szerkesztő ablak megnyitásához.
3. A **Bekapcsolva (A/B)** kapcsolóval összehasonlíthatod a nyers és a hangolt utat; a **+ Sáv hozzáadása** gombbal adhatsz hozzá új szűrőt (legfeljebb 10 db), az **Alaphelyzet** gombbal pedig mindent visszaállíthatsz.
4. A grafikon a motor tényleges, élő átviteli görbéjét mutatja az aktuális útra (valamint halványan az összes engedélyezett út összegét).

*(Független zónák módban az útonkénti EQ gomb rejtett és nem szól bele a hangzásba, de a korábban beállított görbe megmarad, és aktív keresztváltóra visszaváltva újra él.)*

## Fontos határértékek és hangszóróvédelem

- **Frekvencia:** 20 Hz – 20 000 Hz
- **Erősítés:** −18 dB … **+6 dB**
- **Meredekség / szélesség (Q):** 0,3 … 10
- **Sávok száma:** legfeljebb 10 sáv útonként

**Mire figyelj?**
- **A magassugárzóra ne adj mélyhangot:** az útonkénti EQ nem váltja ki a keresztváltót; alul- és felüláteresztő szűrő helyett mindig a keresztváltó vágási frekvenciája védi a hangszórót.
- **+6 dB-nél nagyobb emelés nincs:** a hangszóró tekercsének túlterhelése és a torzítás elkerülése érdekében a rendszer szándékosan korlátozza az emelést. Szobai dübörgésnél és rezonanciáknál mindig a **csökkentés (vágás)** ad tisztább, biztonságosabb eredményt.
- **Kimenetenként független beállítás:** egy út EQ-ja semmilyen körülmények között nem szivárog át a többi kimenetre vagy a zóna mód láncaiba.
- **Távvezérlési jogosultság:** az aktív keresztváltó (`set_multiout`) és az útonkénti EQ (`set_way_eq`) beállításait a helyi gépen túl távoli távirányítóról kizárólag adminisztrátori joggal rendelkező, jóváhagyott eszköz módosíthatja.
