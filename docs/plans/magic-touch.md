# Terv: Magic Touch – automatikus, visszafogott hangszépítő

**Állapot:** az 1–4. ütem elkészült (2026-10-21), az 5–6. ütem terv.
**Felvéve:** 2026-10-21

## Cél

Egyetlen szabályzó (Magic Touch, 0–100 %), amely a lejátszott hangot elemzi, és a lehető legkisebb,
legtermészetesebb beavatkozással javít rajta. Nem hangosít, nem tesz rá „V” (mosolygó) görbét, és nem
tömörít. Abból indul ki, mi hiányzik vagy zavaró az adott felvételben, és azt igazítja.

A felhasználó elsődleges vezérlője az intenzitás. A négy belső modul (Restoration, Tone & Air,
Micro-dynamics, Warmth & Space) automatikusan súlyozott réteg. A felhasználó nem állítgatja őket
egyenként, mert akkor már nem „magic touch”, hanem hagyományos mastering plugin lenne.

## Mit mutatnak a kutatások

Az alábbi állítások forrásait ellenőriztem. Ahol a beérkezett anyag pontatlan volt, azt jelölöm.

| Megállapítás | Forrás | Mit jelent a Magic Touchnak |
|---|---|---|
| 130 hallgató, hangerőre kiegyenlített A/B tesztek: a kevesebb jelre alkalmazott, mérsékelt kompressziót szerették jobban. A nehéz limitinget nem szerették, főleg ha a teljes keveréken volt. | Campbell, Paterson, van der Linde (2017), JAES 65(7/8) | A teljes jelet nem tömöríti és nem limitálja erősen. A dinamikai beavatkozás finom és célzott. |
| Hangosságra normalizált mintáknál a nagyobb csúcstényező (crest factor) jobb, a nagyobb hangosság (erősebb tömörítés) rosszabb minőségi értékelést kapott. Az 1,6–6,4 kHz-es sávok spektrális fluxusa negatívan függött össze a minőséggel. A legerősebb negatív tényező a durvaság (roughness) volt. | Wilson, Fazenda (2016), JAES 64(1/2), 63 dal | A dinamikát nem csökkentheti. A 2–6 kHz tartományt óvni kell a túlhajtástól. Torzítást, érdességet nem adhat hozzá. |
| A Harman célgörbét a hallgatók 64 %-a választotta. Külön csoport (21 %) kevesebb basszust, egy másik több basszust szeretett. | Olive (Harman), Acoustics Today 2022 tavasz | Nincs mindenkinek jó egyetlen görbe: kell a Karakter választó. A fejhallgató-korrekció az EQ (AutoEq) dolga, a Magic Touch ezt nem duplázza. |
| A hangminőség észlelt dimenziói: tisztaság, élesség/keménység–lágyság, fényesség–sötétség, teltség–vékonyság, térérzet, közelség, zavaró hangok, hangosság. | Gabrielsson, Sjögren (1979), JASA | A diagnosztika ezekre a dimenziókra képez le. *A beérkezett anyag ezt egy 2024-es tanulmánynak tulajdonította. A 2024-es (Bannister és mtsai, Frontiers in Psychology, hallókészülék-viselők) más dimenziókat talált: tisztaság, élesség, torzítás, tágasság, magas-, közép- és mélyerősség.* |
| A népszerű felvételek hosszú idejű átlagspektruma egy iparági gyakorlat formálta célgörbe felé tart, ami részben a valódi együttesek természetes spektrumára hasonlít. | Pestana és mtsai (2013), AES 135. konvenció | A spektrális egyensúly referenciája ilyen átlagspektrum lehet, nem a lapos (sima) spektrum. |

Pontosítások a beérkezett anyaghoz:
- *„A mikrodinamika pozitívan, a 4 kHz körüli energia negatívan korrelált”:* a forrásban a csúcstényező
  korrelált pozitívan, és az 1,6–6,4 kHz sávok spektrális fluxusa (nem az energiája) negatívan. Az irány
  ettől még ugyanaz.
- *„A 3–5 kHz-re a legérzékenyebb a fül”:* ez helyes (egyenlő hangosságú görbék, ISO 226: a hallásküszöb
  minimuma 3–4 kHz körül van).
- *„< 0,5 % CPU Apple Siliconon”:* nem igazolható. A célt mérés alapján kell meghatározni (lásd lent).

## Hely az Aqulify láncában

```
Magic Touch -> Preamp -> Equalizer (LSP) -> Bass Enhancer -> Crossfeed -> Convolver -> Limiter
```

- **A lánc elején van**, mert a felvételt javítja. A fejhallgató- vagy teremkorrekció (EQ, AutoEq,
  konvolver) utána jön, és változatlanul működik.
- **Közös kód Linuxra és Windowsra** (`include/`, `aether_chain.hpp`), ahogy az EasyEffects-paritás terv
  szabálya előírja. Ugyanaz fut a PipeWire csomópontban és az EQ APO pluginban.
- **A hangfeldolgozás valós idejű és késleltetésmentes** (IIR szűrők, burkológörbék). Az elemzés a lánc
  meglévő háttérszálán fut, így a plugin motor nélkül is működik. A valós idejű szálra csak a kész
  paraméterek kerülnek, simítva.
- **Hangerő-semleges:** a kimenet hangossága ±0,5 LU-n belül egyezik a bemenetével (folyamatos,
  lassú kiegyenlítés). Így a Preamp, az automatikus preamp és a Limiter működése nem változik.
- **0 %-on bitre pontosan átenged** (null teszttel ellenőrizve). Ki- és bekapcsolása kattanásmentes, mint a
  többi modulé.

## Felület

Új „Magic Touch” fül a modulok között:

- **Magic Touch:** 0–100 % (a fő szabályzó).
- **Karakter:** Természetes / Meleg / Fényes / Tágas.
- **Tartalom:** Automatikus / Zene / Beszéd / Archív felvétel.
- **Előtte / utána (A/B):** egy kattintással összehasonlítható az eredetivel, hangerőre kiegyenlítve
  (ITU-R BS.1770 szerint), hogy a puszta hangerő-különbség ne tévessze meg a fület. A váltás rövid,
  nem hallható áttűnéssel történik.
- **Biztonság (opcionális):** a de-esser mélysége és a true peak plafon.
- **Részletek / diagnosztika** (lenyitható): a felismert tartalom, a mért értékek (rövid idejű LUFS, LRA,
  csúcstényező, mély-közép többlet, élesség, sziszegés, zajszint), és hogy melyik belső modul mennyit
  dolgozik (dB-ben). Ehhez a zenehallgatás közben nem kell hozzányúlni.

## Belső modulok

Az építőkövek lehetőleg ugyanazok, mint az EasyEffectsben (LSP, Calf, libebur128). Ahol az EasyEffectsnek
nincs megfelelője, az külön jelölve van.

| Modul | Feladat | Építőkő |
|---|---|---|
| **1. Restoration** | De-hum (50/60 Hz és felharmonikusai, csak ha stabil zümmögés van); kíméletes zajcsökkentés (legfeljebb 4–6 dB); de-click (később). | De-hum: keskeny szűrők (saját). Zajszűrés: beszédhez RNNoise (mint az EasyEffectsben). Zenéhez spektrális kapuzás (saját, EasyEffects-megfelelő nincs). |
| **2. Tone & Air** | Dinamikus vágás 150–300 Hz-en (dobozhang, „sár”), csak ha többlet van. Levegő (air) 10–16 kHz-en legfeljebb +2–3 dB. Automatikus de-esser. A 3–5 kHz-et soha nem emeli, ha élesség vagy sziszegés látszik. | Dinamikus EQ sáv (LSP szűrők + burkoló), Deesser (Calf deesser algoritmus). |
| **3. Micro-dynamics** | Finom felfelé irányuló dinamika (a halk részek emelése), tranziens-kiemelés legfeljebb +10–15 %. Beszédnél EBU R128 szintkiegyenlítés. Zenénél nincs szintkiegyenlítés. | LSP kompresszor felfelé irányuló módban, Autogain (libebur128). A tranziens-alakító saját (az EasyEffects Crystalizere a legközelebbi). |
| **4. Warmth & Space** | Lágy exciter (páros felharmonikusok, legfeljebb 10–15 % keverés). Fokozatos mono basszus. Sztereó tágítás csak 150 Hz felett, mérsékelten. | Exciter (Calf), Stereo Tools (Calf M/S), Linkwitz–Riley keresztváltó. |

**Fokozatos basszus-sztereó** (nem merev 150 Hz-es mono):
- 80 Hz alatt közel mono (feszes mély, nincs fáziskioltás).
- 80–150 Hz között fokozatos átmenet: a szintetizátorok és basszusgitárok tere megmarad.
- 150 Hz felett normál sztereó.
- Csak akkor avatkozik be, ha a mély sávban a két csatorna korrelációja alacsony vagy negatív
  (fázisproblémás basszus). Rendben lévő felvételen nem nyúl hozzá.
- Bekapcsolt Crossfeed mellett nincs sztereó tágítás, mert a kettő egymás ellen dolgozna.

## Diagnosztika és döntések

Az elemzés 3 másodperces csúszó ablakokon fut (az LRA-hoz kb. 60 másodpercnyi előzmény kell).

| Mérés | Hogyan |
|---|---|
| Hangosság, LRA | ITU-R BS.1770 (K-súlyozás), EBU Tech 3342 |
| Csúcstényező | csúcs/RMS, ablakonként |
| Spektrális egyensúly | 1/3 oktávos sávenergia egy átlagspektrum-referenciához képest |
| Mély-közép többlet | 150–300 Hz a szomszédos sávokhoz és a referenciához képest |
| Élesség | 2–6 kHz relatív szint és a spektrális fluxus ebben a sávban |
| Sziszegés | 5–9 kHz-es rövid kitörések a teljes sávhoz képest |
| Zajszint | minimum-statisztika a halk részeken; sziszegés = magas, a jeltől független zajpadló |
| Zümmögés | stabil csúcsok 50/60 Hz-en és felharmonikusain, halk részeken is |
| Sávszélesség | a spektrum letörése (pl. MP3 16 kHz, kazetta 12–14 kHz) → archív anyag jele |
| Sztereó kép | sávonkénti korreláció (mély fázisprobléma, keskeny keverés) |
| Tartalom | beszéd/zene: 4 Hz-es moduláció, spektrális fluxus, harmonikus szerkezet |

A modulok nem egyetlen összegzett „Repair” számból kapják az erősítést, hanem egy szabályrendszer
(állapotgép) dönt:

```
Ha magas a zaj:
    ├── Denoise: növelése (max. 4–6 dB)
    ├── Air Lift: csökkentése (ne erősítse a susogást)
    └── Exciter: csökkentése / kikapcsolása
Ha élesség / sziszegés látszik:
    ├── De-esser: bekapcsolása
    ├── 3–5 kHz jelenlét-emelés: letiltása
    └── Szaturáció: visszafogása
Ha alacsony az LRA (lapos dinamika):
    ├── Finom felfelé irányuló dinamika (halk részek megemelése)
    ├── Tranziens attack: enyhe kiemelés (+10–15 %)
    └── További kompresszió és limitálás: nincs
Ha dobozhang / 150–300 Hz többlet:
    └── Dinamikus vágás 200 Hz körül (jelenlét-emelés nélkül)
```

A döntések lassan változnak (másodperces időállandók, hiszterézis, legfeljebb kb. 1 dB/s
paraméterváltozás), hogy ne legyen pumpálás. A Tartalom „Automatikus” módban csak akkor vált
profilt, ha az új tartalom 5–10 másodpercig egyértelmű.

## Tartalmi profilok

| Tartalom | Prioritás | Aktív |
|---|---|---|
| Beszéd / podcast / hangoskönyv | Érthetőség, zajmentesség | Zajcsökkentés (RNNoise), de-hum, mélyvágás 80 Hz alatt, enyhe jelenlét (2–3 kHz) csak ha nincs élesség, stabil szint (−16 LUFS körül) |
| Archív felvétel (kazetta, régi rádió, rossz MP3) | Sávszélesség és élet visszaadása zajkiemelés nélkül | De-click, finom sziszegés-kapuzás, óvatos high-shelf vagy exciter, tranziens-helyreállítás |
| Túltömörített modern zene | Kevésbé fárasztó, frissebb, térbelibb hang | 150–300 Hz dinamikus vágás, a túltolt sziszegés lágyítása, finom sztereó frissítés, nagyon visszafogott tranziens-alakítás; **nincs** további kompresszió és erős exciter |
| Akusztikus / élő zene | Természetesség, átlátszóság | Minimális beavatkozás: csak finom levegő 12 kHz felett |
| Egyéb zene | Egyensúly | A szabályrendszer szerint |

A túltömörített anyagoknál nem ígérünk dinamika-visszaállítást: egy végigtömörített mastert nem lehet
visszaalakítani. A reális cél a fáradtság csökkentése és a térérzet frissítése.

## Kemény korlátok

A kódban rögzített határok, 100 %-os intenzitáson is:

| Mi | Korlát |
|---|---|
| Zajcsökkentés | legfeljebb 4–6 dB csillapítás |
| Levegő (air) emelés | legfeljebb +2–3 dB |
| Exciter | legfeljebb 10–15 % keverés |
| Tranziens-kiemelés | legfeljebb +10–15 % |
| Sztereó tágítás | csak 150 Hz felett, mérsékelten |
| Limiter (a lánc végén, a meglévő) | lehetőleg 0–3 dB erősítéscsökkentés |
| Bármely sáv hangszín-módosítása | legfeljebb ±3 dB |
| Hangosság | ±0,5 LU a bemenethez képest |

## Erőforrás és késleltetés

- A célokat mérés alapján rögzítjük: egy sztereó stream, 48 kHz, 256–512 mintás puffer mellett a teljes
  Magic Touch néhány százalék CPU egy mai processzoron (RNNoise-zal együtt is).
- Plusz késleltetés: a hangútban IIR szűrők és burkolók vannak, ezért gyakorlatilag 0. A de-esser
  előretekintése legfeljebb 5 ms, a teljes cél 10 ms alatt.
- A Windows EQ APO plugin az audiodg.exe valós idejű szálán fut: az elemzés ott is a háttérszálon marad.

## Ütemezés

1. **Elemzés és diagnosztika** (a hang még nem változik): a mérések és a tartalomfelismerés
   ellenőrizhetők valódi zenén, mielőtt bármit javítanánk.
2. **Tone & Air, fokozatos mono basszus, tér, hangerő-kiegyenlített A/B, intenzitás, kemény korlátok.**
3. **Micro-dynamics és exciter, Karakter választó.**
4. **Automatikus tartalomfelismerés, beszéd szintkiegyenlítés.**
5. **Restoration:** de-hum, zajcsökkentés (RNNoise beszédhez, spektrális kapuzás zenéhez), de-click.
6. **Később:** tanulás a felhasználó A/B választásaiból (külön profilok: fejhallgató, hangszóró, autó,
   beszéd, zene).

## Tesztelés

- **Objektív:** 0 %-on null teszt; hangerő-semlegesség (LUFS); nincs klippelés és true peak túllépés;
  CPU és késleltetés mérése; minden szabályhoz szintetikus jelek (zümmögés, sziszegés, ellenfázisú
  basszus, túltömörített jel, beszéd).
- **Hallgatói:** hangerő-kiegyenlített A/B teszt egy válogatott lejátszási listán (modern pop,
  akusztikus, régi felvétel, podcast).

## macOS

Az Aqulify most Linuxon és Windowson fut. A macOS változat külön terv lesz:
1. változat: BlackHole virtuális hurok, felhasználói beállítási útmutatóval;
2. változat: Core Audio Tap, jobb kezelhetőséggel;
mindkettőnél világos hibaüzenettel, ha a rendszerhang nem fogható el.

## Kapcsolódó terv

Az [EasyEffects plugin-paritás](easyeffects-plugins.md) több plugint is tartalmaz, amelyeket a Magic Touch
belül használ (Autogain, Deesser, Exciter, Stereo Tools, Compressor, Crystalizer, Noise Reduction).
A DSP kód közös, így ezek később önálló modulként is megjelenhetnek.

## Döntések (2026-10-21)

- Az első verzió az 1–2. ütem: mérés és diagnosztika, Tone & Air, fokozatos mono basszus és tér,
  hangerőre kiegyenlített A/B, intenzitás.
- A zajszűrés (Restoration, RNNoise) később, külön lépésben jön.
- A macOS változat külön terv lesz, később.

## Megvalósítás (1–2. ütem)

Kód: `include/magic_touch.hpp` (a lánc: `aether_chain.hpp`), tesztek: `src/test_dsp.cpp` („Magic Touch”).

- **Felület:** „Magic Touch” fül: be/ki, intenzitás (0–100 %, alapból 50 %), A/B gomb (csak bekapcsolva
  látszik), „Részletek / diagnosztika” a motor méréseivel. A Karakter és a Tartalom választó a 3–4. ütemmel
  jön, addig nem jelenik meg (nincs hatástalan vezérlő).
- **Hangút sorrendje:** mély-közép vágás → levegő → de-esser → hangerő-kiegyenlítés → oldaljel (mono basszus,
  tágítás) → A/B. A hangerő-kiegyenlítés csak a hangszínváltozást egyenlíti ki: az ellenfázisú mély
  eltávolítása után nem emeli vissza a szintet.
- **Érintetlenség:** a fokozatok csak akkor futnak, ha van mit tenniük; rendben lévő felvételen a kimenet bitre
  egyezik a bemenettel (a csúcsok sem nőnek). 0 %-on a lánc teljesen kikerüli.
- **Fokozatos mono basszus:** két mély polcszűrő az oldaljelen (75 Hz, teljes erővel 2 × −12 dB): 50 Hz alatt
  kb. −20 dB, 80 Hz −11 dB, 120 Hz −4 dB, 150 Hz −2 dB, 200 Hz felett 1 dB alatt. A középjel nem változik.
  Csak 0,4 alatti mély sávi korrelációnál lép be (természetes, szétszórt mikrofonos felvételeknél 0,5–0,6 a
  szokásos érték), teljes erővel −0,2-nél.
- **Tágítás:** magas polc az oldaljelen (200 Hz), legfeljebb +1,5 dB, csak −14 dB alatti (de nem mono)
  S/M aránynál, és bekapcsolt Crossfeed mellett soha.
- **Levegő:** legfeljebb +2 dB (10,5 kHz-es magas polc); sziszegésnél, élességnél és sávkorlátos
  (pl. 8 kHz-ig terjedő) felvételnél nem, illetve kevésbé emel.
- **De-esser:** legfeljebb 4 dB, csak a felvételre jellemzőnél jóval erősebb 4,5 kHz feletti kitörésekre
  (a medián fölött 12 dB-lel és a 95. percentilis fölött), hogy a cintányérokat ne tompítsa.
- **Lassúság:** a hangszín-mérés 20 s-os átlag (a refrén és a vers között ne ingadozzon), a beavatkozások
  legfeljebb 0,5 dB/s sebességgel változnak; új számnál (1,5 s csend után) a mérés újraindul.
- **Mérések a valóságban:** a hangútban 48 kHz-en egy mag 0,1–0,2 %-a, az elemzés kb. 0,7 % (Xeon, 2,1 GHz),
  plusz késleltetés nincs. A hangosság a bemenetéhez képest ±0,1 LU-n belül marad.
- **Hangolás:** szabad licencű zenei mintákon (klasszikus, elektronikus, drum and bass, pop, ragtime, beszéd)
  és ezek mesterségesen rontott változatain (dobozhang, tompa magasak, sziszegés, ellenfázisú mély, keskeny
  keverés). A hallgatói A/B teszt valódi lejátszási listán még hátravan.

## Megvalósítás (3–4. ütem)

Kód: `include/magic_touch.hpp`, tesztek: `src/test_dsp.cpp` („Magic Touch” 8–12.), `src/test_core.cpp` (paraméterfájl).

- **Felület:** a Magic Touch fülön megjelent a Karakter (Természetes / Meleg / Fényes / Tágas) és a Tartalom
  (Automatikus / Zene / Beszéd / Archív felvétel) választó. A „Részletek / diagnosztika” új sorai: a használt
  tartalom (felismert vagy kézi, beszéd-jelleg, sávszélesség), mikrodinamika, exciter / melegség, beszéd.
  Paraméterfájl v6 (a v5 és a v4 átvehető), közös memória v6.
- **Hangút sorrendje:** beszéd: mélyvágás → mély-közép vágás → Meleg: mély polc → beszéd: jelenlét → levegő →
  de-esser → exciter → mikrodinamika → hangerő-kiegyenlítés (beszédnél a szintkiegyenlítéssel) → oldaljel → A/B.
  Ezek a fokozatok is csak akkor futnak, ha van mit tenniük; a rendben lévő felvétel továbbra is bitre érintetlen.
- **Mikrodinamika:** tranziens-kiemelés legfeljebb +1,2 dB (+15 %) a gyors (2 ms) és a lassú (40 ms)
  teljesítmény-burkoló arányából, csak lapos dinamikánál (a csúcstényező 12 dB, az LRA 6 LU alatt). A halk részek
  emelése legfeljebb +2 dB, csak a 3 s-os szintnél 18 dB-lel halkabb részeken, a csendet és a zajpadlót nem emeli.
  A csúcs a mikrodinamika miatt nem kerül −1 dBFS fölé. További kompresszió és limitálás nincs.
- **Exciter:** a 3–9 kHz sáv páros felharmonikusai 6 kHz felett (a 3–5 kHz-et nem emeli), legfeljebb 12 % keverés.
  Ott dolgozik, ahol a magas hiányzik (tompa vagy sávkorlátos felvétel); zajnál, élességnél és sziszegésnél vissza.
- **Karakter** (a szabályok súlyai, a kemény korlátokon belül):
  - Természetes: az 1–2. ütem szabályai.
  - Meleg: mély polc 150 Hz alatt legfeljebb +2 dB, a 120–600 Hz sáv páros felharmonikusai (legfeljebb 8 %),
    a levegő-polc legfeljebb −1 dB, kisebb mély-közép vágás. A már dobozos felvételre nem tesz melegséget.
  - Fényes: a levegő hamarabb és legfeljebb +2,5 dB-lel emel, enyhe exciter. Élességnél, sziszegésnél, zajnál
    ugyanúgy visszafogja magát.
  - Tágas: a tágítás már −8 dB-es S/M aránynál is belép (a Természetesnél −14 dB), bekapcsolt Crossfeed mellett
    továbbra sincs tágítás.
- **Tartalomfelismerés (Automatikus):** beszéd-jelleg a 10 ms-os burkolóból (az 1 s-os átlag felénél halkabb
  keretek aránya, a 2,5–6,5 Hz-es moduláció) és a kevés mélyből (40–80 Hz a 200 Hz – 2 kHz sávhoz képest).
  Archív: 14,5 kHz alatt meredeken letört spektrum, vagy közel mono, zajos felvétel. A profil csak akkor vált, ha
  az új tartalom 8 másodpercig egyértelmű (a beszédet kb. 12–15 s után ismeri fel). A zenei alprofil a
  csúcstényezőből és az LRA-ból jön: túltömörített (csúcstényező 10 dB, LRA 5 LU alatt) vagy akusztikus / élő
  (csúcstényező 15 dB és LRA 6 LU felett, vagy LRA 12 LU felett).
- **Profilok:** beszéd: mélyvágás 80 Hz alatt, jelenlét 2,2 kHz-en legfeljebb +1,5 dB (élességnél és
  sziszegésnél nincs), kevés levegő, nincs exciter, tranziens-kiemelés és tágítás. Archív: exciter teljes
  súllyal, tranziens-helyreállítás, fél levegő. Túltömörített zene: gyenge exciter, visszafogott tranziens.
  Akusztikus / élő: fél mély-közép vágás, nincs exciter, mikrodinamika és tágítás.
- **Beszéd szintkiegyenlítés:** a 3 s-os hangosság (BS.1770) a felvétel saját, hosszú távú (τ ≈ 50 s, kapuzott)
  átlagához igazodik, legfeljebb ±6 dB-lel és 1 dB/s sebességgel; szünetben áll. Egy 8–10 dB-es szintugrásból
  kb. 4 dB marad.
- **Javítás az élességmérésben:** sávkorlátos felvételnél a letörés előtti lejtő nem számít élességnek (korábban az
  archív anyagot „élesnek” mérte, és emiatt a levegőt is visszafogta).
- **Mérések a valóságban:** a hangútban 48 kHz-en egy mag 0,2–0,35 %-a, az elemzés kb. 0,4 % (Xeon, 2,8 GHz),
  plusz késleltetés nincs. A hangosság 100 %-on, bármely Karakterrel ±0,3 LU-n belül marad (a terv határa ±0,5 LU).
- **Hangolás:** szabad licencű zenekari felvételeken (Battle for Wesnoth zenék), ezek mesterségesen rontott
  változatain (erősen tömörített master, 10 kHz-en letört mono felvétel), espeak-ng beszéden (magyar és angol)
  és szintetikus jeleken. A zenét egyik felvételen sem nézte beszédnek (beszéd-jelleg legfeljebb 0,41).
  A hallgatói A/B teszt valódi lejátszási listán még hátravan.

### Döntések (3–4. ütem)

- **Beszéd szintje:** a terv „−16 LUFS körül” értéket ír. A szintkiegyenlítés ehelyett a felvétel saját hosszú
  távú hangosságát tartja, mert egy rögzített célszint a halk podcastot akár 8 dB-lel is hangosítaná, ami ellentmond
  a „nem hangosít” elvnek, és a Preamp / Limiter működését is megváltoztatná.
- **Archív jel:** csak a 14,5 kHz alatti letörés számít archívnak. A 16 kHz-en levágott MP3 (128 kbit/s) gyakori
  modern anyagnál is, ezért az nem kapcsolja be az archív profilt.
- **Az archív profil helyreállító része** (de-click, sziszegés-kapuzás) az 5. ütemmel (Restoration) jön.

## Források

- Campbell, W., Paterson, J., van der Linde, I. (2017): Listener preferences for alternative
  dynamic-range-compressed audio configurations. JAES 65(7/8), 540–551.
  https://repository.uwl.ac.uk/id/eprint/3324/
- Wilson, A., Fazenda, B. M. (2016): Perception of audio quality in productions of popular music.
  JAES 64(1/2). https://salford-repository.worktribe.com/output/1407562/perception-of-audio-quality-in-productions-of-popular-music
- Olive, S.: Harman célgörbe és preferencia-csoportok. Acoustics Today, 2022 tavasz.
  https://acousticstoday.org/issues/2022AT/Spring2022/files/basic-html/page66.html
- Gabrielsson, A., Sjögren, H. (1979): Perceived sound quality of sound-reproducing systems. JASA.
  https://labs.sonicfield.org/library/perceived-sound-quality-of-sound-reproducing-systems
- Bannister, S. és mtsai (2024): hallókészüléket viselők zenei hangminőség-dimenziói.
  Frontiers in Psychology. https://public-pages-files-2025.frontiersin.org/journals/psychology/articles/10.3389/fpsyg.2024.1310176/text
- Pestana, P. D. és mtsai (2013): Spectral characteristics of popular commercial recordings 1950–2010.
  AES 135. konvenció. https://ciencia.ucp.pt/en/publications/spectral-characteristics-of-popular-commercial-recordings-1950-20/
