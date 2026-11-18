# Nagy felbontás: 352,8–768 kHz a beépített lejátszóban

Az Aqulify beépített lejátszója a 44,1 … 192 kHz-es fájlok mellett a **352,8 / 384 kHz-es** és a **705,6 / 768 kHz-es**
PCM-et is kezeli, bitpontosan. Ez a leírás azt foglalja össze, mi működik ezeken a mintavételeken, és mi nem.

## Mit tud?

| Mintavétel | Fájl lejátszása | DSP-lánc (EQ, szűrők, szobakorrekció) | Felmintavételezés célja |
|---|---|---|---|
| 44,1 … 192 kHz | igen, bitpontosan (DSP nélkül) | igen | igen |
| 352,8 / 384 kHz | igen, bitpontosan (DSP nélkül) | **igen** | igen |
| 705,6 / 768 kHz | igen, bitpontosan | **nem hat** – a jel változatlanul megy át | igen |

- **384 kHz fölött a DSP-lánc nem számol.** A modulok legfeljebb 384 kHz-ig vannak méretezve; e fölött a jel
  érintetlenül, bitpontosan halad tovább. A felület ezt egy sávval jelzi („…mintavételen a DSP-lánc nem hat”).
  Ha EQ-t vagy szobakorrekciót használsz, maradj 384 kHz-en vagy alatta.
- A **forrást követő** módban (Hangkimenet (haladó) → Mintavétel → „A forrást követi”) Linuxon a PipeWire a fájl
  vagy a felmintavételezés céljára vált, egészen 768 kHz-ig – ha a DAC is tudja.
- A rögzített mintavétel listájában is megjelent a 352,8 / 384 / 705,6 / 768 kHz.

## Mi kell hozzá?

- Olyan DAC, amely ezeket a mintavételeket fogadja (a 705,6 / 768 kHz-et kevés eszköz tudja; a gyártói adatlap
  szerint ellenőrizd).
- Linuxon (PipeWire) a forrást követő mód; Windowson a kizárólagos (WASAPI exclusive) mód; macOS-en a kizárólagos
  hozzáférés. Megosztott módban a rendszer keverője a saját mintavételére alakítja a jelet – ezt a felület jelzi.

## Számítási szálak (felmintavételezés)

**Automatikus (ajánlott) / 1 szál / 2 szál.** Két szálnál a bal és a jobb csatorna külön processzormagon számolódik.
Az eredmény **bitre azonos** az egy szálas számítással – hallható különbség nincs, csak a terhelés oszlik meg, így a
nagy célmintavételek (352,8 kHz-től) gyengébb gépen is kijönnek. Automatikus módban 352,8 kHz-es vagy nagyobb célnál,
legalább 4 magos gépen két szál fut.

## Dedikált mag (haladó, alapból ki)

A dekóder-szálat egy kiválasztott processzormagra rögzíti (Linux és Windows; macOS-en nem érhető el). A
hangminőségen **nem változtat**; legfeljebb az ütemezést teheti kiszámíthatóbbá. A legtöbb gépen nincs rá szükség.

## Legnagyobb célmintavétel

A felmintavételezés célja felülről korlátozható (176,4 / 192 / 352,8 / 384 kHz). Hasznos, ha a DAC tudna többet, de a
gép nem bírja, vagy ha a DSP-láncot is használni szeretnéd (384 kHz-ig).

## Terhelés-tanácsadó

A hangmotor méri a saját processzorhasználatát. Ha az **tartósan** (20 másodpercig) a gép erejének 30 %-a fölött van
(Raspberry Pi-n 20 %), vagy **hangkimaradás** (alulcsordulás) történik, a felület felső részén megjelenik egy sáv:

- **Optimalizálás** – egyetlen, előre rögzített lépést alkalmaz, ebben a sorrendben:
  1. két számítási szál (ha legalább 4 mag van; a hang bitre azonos marad),
  2. a szűrő pontossága 24 bitre (kisebb terhelés; a legtöbb DAC-nál nincs gyakorlati különbség),
  3. alacsonyabb célmintavétel (768 → 384 kHz, illetve 384 → 192 kHz),
  4. a felmintavételezés kikapcsolása.
- **Visszavonás** – az optimalizálás után visszaállítja az előző beállítást.
- **Marad így** – amíg a terhelés nem csökken, nem szól újra.
- **Ne kérdezz többet** – kikapcsolja a tanácsadót (visszakapcsolható: Beépített lejátszó → „Szóljon, ha nagy a
  processzorterhelés”).

A tanácsadó **magától soha nem változtat semmit.**

## Fájlformátumok

A WAV, a FLAC és az **AIFF / AIFF-C** (`.aif`, `.aiff`, `.aifc`) 352,8–768 kHz-en is bitpontosan szól, tekeréssel
együtt. A 32 bites egész fájlok a lejátszó lebegőpontos útja miatt 24 bites pontossággal szólnak (ez a legtöbb DAC
felbontásánál nem számít).

A spektrum, a vízesés és a csúcsmérő 192 kHz fölött tizedelt jelből dolgozik. Ugyanazt a felbontást mutatja, mint
176,4 / 192 kHz-en, és a csúcsmérő minden mintát lát.

## DSD512 és DSD1024

A DSF és a DFF (tömörítetlen) fájlokat DSD64-től DSD1024-ig olvassa, mindkét családban (44,1 és 48 kHz).

| Kimenet | Hogyan szól |
|---|---|
| Linux / Pi, közvetlen ALSA (kizárólagos), „DSD natívan” bekapcsolva | **natív DSD** (`DSD_U32_BE`, `U32_LE`, `U16_BE`, `U16_LE`, `U8` – amit a DAC tud), bitre változatlanul; ha a DAC nem tud natív DSD-t: DoP |
| Windows, ASIO, „DSD natívan” bekapcsolva | natív DSD, ha az ASIO-meghajtó tudja; különben DoP PCM-keretekben |
| Windows, kizárólagos WASAPI | DoP, az eszköz határáig |
| macOS, és minden más esetben | DSD → PCM (88,2 / 96 kHz), a DSP-lánccal (macOS-en a DoP / natív DSD még nincs megvalósítva) |

- **A DoP-nak van határa.** DSD512 DoP-ban 1,4112 MHz-es, DSD1024 DoP-ban 2,8224 MHz-es PCM kellene; ez utóbbit egy
  DAC sem fogadja. Ilyenkor a jelút kiírja az okot, és a lejátszó PCM-re alakít.
- **DSD-bitfolyamon nincs DSP.** Natív DSD-nél és DoP-nál a DSP-lánc, a ReplayGain és a digitális hangerő kimarad. Ha
  EQ vagy szobakorrekció kell, kapcsold ki a „DSD natívan” beállítást: ekkor a DSD → PCM út szól, a lánccal.
- **DSD → PCM DSD512 / DSD1024-en kétfokozatú tizedeléssel.** A zaj és a hang szintje 0,1 dB-en belül ugyanaz, mint
  az egyfokozatú úton, de kb. kétszer gyorsabb. DSD1024 → 88,2 kHz egy laptopmagon ~5–7× valós idő; a Pi-n a valós
  idejű lejátszáshoz elég, de ezt valódi Pi-n még meg kell mérni.

## Ellenőrzés (fejlesztőknek)

- `test_hires_b`: DSF / DFF DSD64–DSD1024 mindkét családban (fejléc, hossz, nyers bitfolyam bitre, tekerés);
  DSD512 / DSD1024 → PCM: a kétfokozatú út jele és THD+N-je a bevált egyfokozatúéval egyezik (≤ −115 dB); a köztes
  mintavétel közeli hang visszahajlása ≤ −105 dB; tekerés bitre; sebesség; natív ALSA-csomagolás mind az 5
  formátumban, több perióduson át (DSD_U8 páratlan periódussal is); végponttól végpontig a lejátszón és az ALSA
  „null” eszközén át, natív és DoP módban, bitre.
- `tools/check_hires_b.sh`: a fentiek friss másolatból, tiltott minták, 8 mutáns (`tools/mutants_hires_b/`).
- `test_hires`: DSP-korlát (705,6 / 768 kHz bitpontos, 384 kHz-en a DSP hat), késleltetés, beállítások és régi JSON,
  célválasztás, egy / két szál bitazonossága, terhelésmérő, tanácsadó, 384 / 768 kHz-es WAV, mag-rögzítés.
- `tests/live/hires_live.py`: valódi PipeWire-gráfon 768 kHz bitpontos lejátszás (a monitorról felvett jel bitre
  egyezik a fájllal), 352,8 kHz-en működő DSP, 44,1 → 705,6 kHz felmintavételezés két szálon.
- `tools/check_hires_a.sh`: a fentiek friss másolatból, tiltott minták, 10 mutáns (`tools/mutants_hires/`).
