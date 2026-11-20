# Beépített hibajelentés – motor, küldő és felület

Állapot: **a motor helyi része, a küldő (`include/report_sender.hpp`, `AQ_REPORT_URL` beállítva) és a felület (`gui_integrated_system.html`, hu + en; a többi nyelv angol helyettesítővel) elkészült**. A küldés alapértelmezésben ki van kapcsolva: csak a felhasználó kifejezett hozzájárulásával (`errorReports = true` vagy egyszeri kézi küldés) indul. A telepítő (Inno Setup) jelölőnégyzete és a 23 nyelv végleges fordítása külön lépés.

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

## Küldés (motor)

- **Fejléc:** `include/report_sender.hpp` (`aq::report::Sender`, `aq::report::Service`), mindhárom motorba bekötve (`src/pipewire_plugin.cpp`, `windows/engine/aether_engine.cpp`, `macos/app/main.mm`).
- **Végpont:** fordítási állandó `AQ_REPORT_URL` (CMake-változó `-DAQ_REPORT_URL="https://..."`, alapérték: üres → nincs küldés). Token és kulcs nincs a programban.
- **Szállítás:** Linux/Pi és macOS: a rendszer `curl` programja (`--proto =https`, `--max-time 15`, a törzs ideiglenes fájlból `--data-binary @...`, soha nem parancssori paraméterként); Windows: WinHTTP (`win::postJson`, HTTPS, 15 s időkorlát). A küldés külön háttérszálon fut, a leállítás nem vár hálózati időkorlátra.
- **Méretkorlát és válaszkezelés:** a törzs legfeljebb 65 536 bájt (túllépésnél a `log` legrégebbi sorai elhagyva). `201` → siker (`AQ-<id>`, automatikus küldésnél `previous-run.json/.txt` törölve); `429` → `Retry-After` tiszteletben tartása; `413` → rövidítés és egyszeri újrapróbálás; `400`/`415` → nincs újrapróbálás; hálózati hiba / `5xx` → legfeljebb 3 próba indításonként, a jelentés helyben marad.
- **Hozzájárulás:** kikapcsolt `errorReports` és kézi jóváhagyás nélkül 0 hálózati kérés.
- **Hídüzenetek:** `error_report_preview` (előnézet), `error_report_send` (egyszeri kézi küldés), `error_report_save` (mentés a `diag/` mappába új néven `report-ÉÉÉÉHHNN-ÓÓPPMP.json`, soha nem ír felül, a felület csak a fájlnevet kapja meg), `error_report_dismiss_previous`, `error_log` (a motor naplójának utolsó sorai, alapból 500, legfeljebb 1000, ugyanúgy tisztítva, mint a jelentés; válasz: `{"type":"error_log","available","lines","text"}`). A felhasználói megjegyzés (`note`, max. 400 karakter) tisztítva, `[Felhasználó] …` előtaggal kerül a `log` utolsó sorába (a `format 1` séma változatlan). Az előnézet és a napló szövegében a sortörés megmarad, a hibás UTF-8 bájt `?` lesz (`jsonEscapeText`).
- **Távirányítóról (minden motoron: Linux/Pi, Windows, macOS):** a fenti üzeneteket a párosított eszköz is küldheti, ha rendszergazda (a fej nélküli gép első, kóddal párosított telefonja tulajdonos és rendszergazda). Jog nélkül a válasz `{"type":"error","error":"remote","request":"<üzenet>"}`, a napló nem olvasódik. A küldés mindig a vezérelt gépről megy a szerverre.

## Felület

- **Beállítás:** „Névtelen hibajelentés” kapcsoló (`errorReports`, alapból ki), rövid adatvédelmi tájékoztatóval és „Mit küld?” előnézettel.
- **Első indítás:** egyszeri kérdés a motor által tárolt `errorReportsAsked` alapján (nem `localStorage`): `[Nem]` (alapértelmezett) `[Igen]` `[Mit küld?]`.
- **Rendellenes előző futás:** `diag/previous-run.json` esetén sáv induláskor: „Legutóbb az Aqulify nem állt le rendesen. Elküldjük, mi történt?” `[Megnézem és elküldöm]` `[Nem]`.
- **„Hibát jelentek” ablak:** 6 tünetgomb (`noSound`, `crackle`, `buttonNoEffect`, `freeze`, `badSound`, `other`), opcionális megjegyzés, **kötelező előnézet küldés előtt**, `[Küldés]` (egyszeri hozzájárulás; üres `AQ_REPORT_URL`-nél tiltott, őszinte üzenettel), `[Mentés fájlba]`, `[Mégse]`. Siker esetén másolható `AQ-<id>` azonosító.
- **„Napló” gomb:** a motor tisztított naplója (utolsó 500 sor), másolható. Távoli gép vezérlésekor (relay) az a gép naplója és jelentése látszik; ilyenkor a „Mentés fájlba” rejtve van, mert a távoli gép mappájába írna.
- **Telefonos alkalmazás** (külön tároló, Kimenetek fül alja): automatikus jelentés kapcsolója, „Hibát jelentek” (tünet, megjegyzés, előnézet, küldés, `AQ-<id>`), „Napló”, és az előző rendellenes futás sávja. Fej nélküli gépnél (Raspberry Pi) ez az egyetlen felület hozzá.

## Mit nem küld, és hogyan olvassa a tulajdonos a jelentéseket

- **Nem kerül a jelentésbe:** felhasználónév, teljes név, gépnév, otthoni mappa és fájlútvonalak (`<path>`), párosított eszközök felhasználói nevei (`<name>`), IP- (`<ip>`), MAC- (`<mac>`) és e-mail-cím (`<email>`). A teljes fájlútvonal a felületre sem kerül ki.
- **Olvasás és 90 napos törlés:** a privát Cloudflare D1 adatbázisból a tulajdonos a `cloudflare/hibajelento-worker/README.md` **„A jelentések olvasása”** szakaszában szereplő `wrangler d1 execute` parancsokkal olvassa és törli a jelentéseket.

## Nincs benne (még)

- Élő Cloudflare Worker élesítés és `AQ_REPORT_URL` beállítása (a tulajdonos végzi).
- macOS és Windows élő futtatás, valamint valódi Raspberry Pi próba (a CI fordítja és futtatja az egységteszteket).
- Telepítő (Inno Setup) jelölőnégyzete, a maradék 21 nyelv emberi fordítása, hídtól független `aqulify-gui` jelentésküldő.
