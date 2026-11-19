# Beépített hibajelentés – 1. szakasz: a motor helyi része

Állapot: **a motor része Linuxon és Windowson elkészült; macOS-en be van kötve, de csak a CI fordítja, élőben nem
próbált**. A felület (2. szakasz), a küldés (3. szakasz) és a telepítő kérdése (4. szakasz) még nincs. **Hálózatra
semmi nem megy**: minden csak helyben, a motor saját mappájában van.

## Mit csinál

- **Őrszem.** A fontos szálak (a híd, a lejátszó dekódere, a DSP háttérszála, a kimenet) rendszeresen jelzik, hogy
  élnek, egy rövid „most itt tartok” címkével (pl. `bridge: pullAudio`). Egy külön őrszál figyeli őket: ha egy szál
  5 másodpercnél tovább nem jelez (`AQULIFY_WATCHDOG_SECONDS` változóval állítható, legalább 1), a motor naplójába
  ír (melyik szál, mióta, mit csinált utoljára), és frissíti az állapotfájlt. Az őrszem csak figyel és jelent: nem
  állít le és nem indít újra semmit.
  - Figyelt szálak: `bridge`, `player-decoder`, `dsp-worker`; Linuxon `pw-output`, `player-alsa`; Windowson
    `player-wasapi`, `player-asio` (az ASIO vezérlőszála), `player-asio-cb` (a meghajtó visszahívása, csak amíg a
    meghajtó fut), `loopback` (a rendszerhang-figyelő); macOS-en `mac-output` (a motor IOProc-ja, csak feldolgozás
    közben), `player-coreaudio`, `player-coreaudio-hog`.
  - A kimenetek visszahívása (valós idejű szál) csak időbélyeget ír; tőlük csak akkor várunk jelzést, amikor a
    kimenet fut.
- **Állapotfájl** (`diag/state.json`, 2 másodpercenként és változáskor): verzió, platform, csomagtípus, a lejátszás
  útja (pl. `native DSD, DSD256, exclusive ALSA`), a hangbeállítások, a hangkártyák gyári típusneve és formátumai,
  a szálak szívverése, a beragadások, a processzor- és memóriahasználat.
  - Windowson a hangkártyáknál a meghajtó gyári neve és a végpont alapformátuma (pl. `48000 Hz, 24 bit (32-bit
    container), 2 ch, PCM`, a regisztrációs adatbázisból, COM nélkül). A felhasználó által átnevezhető végpontnév
    nem kerül bele.
  - macOS-en a hangkártyák listája még nincs benne; a lejátszás útja és a beállítások igen.
- **„Rendben leállt” jel** (`diag/running`): indításkor létrejön, rendes leálláskor törlődik. Ha induláskor megvan,
  az előző futás rendellenesen ért véget (beragadás, erőszakos leállítás, összeomlás): az előző állapotból, az
  összeomlás jeléből és a napló utolsó kb. 200 sorából **helyi** jelentés készül (`diag/previous-run.json` és az
  olvasható `diag/previous-run.txt`).
- **Összeomláskor** (Linux, macOS: jelzéskezelő; Windows: kezeletlen kivétel, `abort`) a motor egy rövid sort ír a
  `diag/crash.txt`-be (a jelzés vagy kivétel kódja, a szálak utolsó címkéje). Memóriakép nem készül.
- **Tisztítás.** A jelentés a kész szövegen tisztul: fájlútvonalak helyén `<path>`; a felhasználónév, a gépnév, a
  párosított eszközök neve, az IP-, MAC- és e-mail-címek kitörölve. A hangeszközök gyári típusneve (pl. egy DAC
  típusa) megmarad, mert nélküle nem lehet hibát keresni.
  - Ami nem személyes, megmarad: a négyrészes kernelverzió (`6.6.87.2-microsoft-standard-WSL2`: a négyes után
    `-szó-` jön; egy címhez fűzött rag, pl. `192.168.1.23-hoz`, továbbra is `<ip>`), és a helyi visszacsatolási cím
    (`ws://127.0.0.1:47899`; az utána következő útvonal `<path>`).

## Hol vannak a fájlok

| Rendszer | Mappa | Napló |
|---|---|---|
| Linux, Raspberry Pi | `~/.config/aqulify/diag/` (vagy `$XDG_CONFIG_HOME/aqulify/diag/`) | `diag/engine.log` (a motor kimenetének tükre) |
| Windows | `%LOCALAPPDATA%\Aqulify\diag\` | `%LOCALAPPDATA%\Aqulify\engine.log` (a meglévő napló) |
| macOS | `~/Library/Application Support/Aqulify/diag/` | `diag/engine.log` (a kimenet tükre; a meglévő `~/Library/Logs/Aqulify/aqulify.log` is megmarad) |

## A beállítás

`errorReports` a hangbeállításokban (`audio.json`), **alapból ki**. Kikapcsolva:

- az előző futás jelentése csak a következő rendes indításig marad meg, utána törlődik;
- Linuxon és macOS-en a napló tükre indításkor csak az előző futás sorait tartja meg.

Bekapcsolva a jelentés a küldésig megmarad (a küldés a 3. szakasz). A beállítás hozzájárulás, ezért az indításkori
visszaállítás módjától függetlenül érvényes.

## Ellenőrzés

- `test_watchdog`: őrszem, a 2026-11-19-i beragadás mint teszt, tisztítás (a kernelverzióval és a helyi címmel is),
  rendellenes vég, a beállítás, az eszközformátum szöveggé alakítása.
- `test_asio` K11 és `test_winout` [11]–[12] (csak Windows): az ASIO-szál és -visszahívás, a loopback szál
  szívverése, az alapformátum a regisztrációs adatbázisból.
- `tests/live/hibajelento_live.sh <build>`: élő próba elszigetelt motorral (tesztfordítás: `-DAQ_TEST_HOOKS=ON`).
- `tools/check_hibajelento_1.sh`: a teljes ellenőrzés (egységtesztek, élő próba, mutánsok, tiltott minták). A
  Windows-only mutánsokat (`W*_win-*.patch`) csak alkalmazhatóságra nézi; ezeket natív Windows-futás igazolja.
- `tools/check_signal_safe.py`: a jelzéskezelőben csak aszinkron-jelzésbiztos művelet van.

## Nincs benne (még)

- macOS: élő próba nem volt (csak a CI fordítja, és ott a `test_watchdog` fut); a hangkártyák listája nincs az
  állapotfájlban.
- A felület, a „Hibát jelentek” gomb, a küldés, a telepítő kérdése: 2–4. szakasz.
- A fogadó oldal (Cloudflare Worker, privát D1) kódja és a végpont szerződése: `cloudflare/hibajelento-worker/README.md`.
  Még nincs élesítve (a tulajdonos telepíti), és a motor még nem küld.
