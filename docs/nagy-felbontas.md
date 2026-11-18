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

## Ellenőrzés (fejlesztőknek)

- `test_hires`: DSP-korlát (705,6 / 768 kHz bitpontos, 384 kHz-en a DSP hat), késleltetés, beállítások és régi JSON,
  célválasztás, egy / két szál bitazonossága, terhelésmérő, tanácsadó, 384 / 768 kHz-es WAV, mag-rögzítés.
- `tests/live/hires_live.py`: valódi PipeWire-gráfon 768 kHz bitpontos lejátszás (a monitorról felvett jel bitre
  egyezik a fájllal), 352,8 kHz-en működő DSP, 44,1 → 705,6 kHz felmintavételezés két szálon.
- `tools/check_hires_a.sh`: a fentiek friss másolatból, tiltott minták, 10 mutáns (`tools/mutants_hires/`).
