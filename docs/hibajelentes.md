# Beépített hibajelentés – 1. szakasz: a motor helyi része

Állapot: **a motor része elkészült (Linux, Windows)**; a felület (2. szakasz), a küldés (3. szakasz) és a telepítő
kérdése (4. szakasz) még nincs. **Hálózatra semmi nem megy**: minden csak helyben, a motor saját mappájában van.

## Mit csinál

- **Őrszem.** A fontos szálak (a híd, a lejátszó dekódere, a DSP háttérszála, a kimenet) rendszeresen jelzik, hogy
  élnek, egy rövid „most itt tartok” címkével (pl. `bridge: pullAudio`). Egy külön őrszál figyeli őket: ha egy szál
  5 másodpercnél tovább nem jelez (`AQULIFY_WATCHDOG_SECONDS` változóval állítható, legalább 1), a motor naplójába
  ír (melyik szál, mióta, mit csinált utoljára), és frissíti az állapotfájlt. Az őrszem csak figyel és jelent: nem
  állít le és nem indít újra semmit.
- **Állapotfájl** (`diag/state.json`, 2 másodpercenként és változáskor): verzió, platform, csomagtípus, a lejátszás
  útja (pl. `native DSD, DSD256, exclusive ALSA`), a hangbeállítások, a hangkártyák gyári típusneve és formátumai,
  a szálak szívverése, a beragadások, a processzor- és memóriahasználat.
- **„Rendben leállt” jel** (`diag/running`): indításkor létrejön, rendes leálláskor törlődik. Ha induláskor megvan,
  az előző futás rendellenesen ért véget (beragadás, erőszakos leállítás, összeomlás): az előző állapotból, az
  összeomlás jeléből és a napló utolsó kb. 200 sorából **helyi** jelentés készül (`diag/previous-run.json` és az
  olvasható `diag/previous-run.txt`).
- **Összeomláskor** (Linux: jelzéskezelő; Windows: kezeletlen kivétel, `abort`) a motor egy rövid sort ír a
  `diag/crash.txt`-be (a jelzés vagy kivétel kódja, a szálak utolsó címkéje). Memóriakép nem készül.
- **Tisztítás.** A jelentés a kész szövegen tisztul: fájlútvonalak helyén `<path>`; a felhasználónév, a gépnév, a
  párosított eszközök neve, az IP-, MAC- és e-mail-címek kitörölve. A hangeszközök gyári típusneve (pl. egy DAC
  típusa) megmarad, mert nélküle nem lehet hibát keresni.

## Hol vannak a fájlok

| Rendszer | Mappa | Napló |
|---|---|---|
| Linux, Raspberry Pi | `~/.config/aqulify/diag/` (vagy `$XDG_CONFIG_HOME/aqulify/diag/`) | `diag/engine.log` (a motor kimenetének tükre) |
| Windows | `%LOCALAPPDATA%\Aqulify\diag\` | `%LOCALAPPDATA%\Aqulify\engine.log` (a meglévő napló) |

## A beállítás

`errorReports` a hangbeállításokban (`audio.json`), **alapból ki**. Kikapcsolva:

- az előző futás jelentése csak a következő rendes indításig marad meg, utána törlődik;
- Linuxon a napló tükre indításkor csak az előző futás sorait tartja meg.

Bekapcsolva a jelentés a küldésig megmarad (a küldés a 3. szakasz). A beállítás hozzájárulás, ezért az indításkori
visszaállítás módjától függetlenül érvényes.

## Ellenőrzés

- `test_watchdog`: őrszem, a 2026-11-19-i beragadás mint teszt, tisztítás, rendellenes vég, a beállítás.
- `tests/live/hibajelento_live.sh <build>`: élő próba elszigetelt motorral (tesztfordítás: `-DAQ_TEST_HOOKS=ON`).
- `tools/check_hibajelento_1.sh`: a teljes ellenőrzés (egységtesztek, élő próba, mutánsok, tiltott minták).
- `tools/check_signal_safe.py`: a jelzéskezelőben csak aszinkron-jelzésbiztos művelet van.

## Nincs benne (még)

- macOS: a közös kódon át a szálak jeleznek, de az őrszál és az állapotfájl a macOS-motorban nincs elindítva.
- Windows: az ASIO-kimenet szála és a rendszerhang-figyelő (loopback) szál nem jelez.
- A felület, a „Hibát jelentek” gomb, a küldés, a telepítő kérdése: 2–4. szakasz.
