# Terv: Magic Touch – automatikus, visszafogott hangszépítő

**Állapot:** terv, a megvalósítás ez után következik.
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

## Nyitott kérdések

- Az első verzió terjedelme (1. ütem önmagában, vagy rögtön az 1–2. ütem).
- RNNoise és a zajszűrés: mikor, és milyen csomagolással Windowsra.
- A macOS változat ütemezése.

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
