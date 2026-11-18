# ASIO-kimenet Windowson (tervezet)

## 1. Mi ez és mi kell hozzá?

Az **ASIO** (Audio Stream Input/Output) a Windows-os hangkártyák és külső DAC-ok közvetlen, kizárólagos meghajtó-felülete. A Windows beépített hangkeverőjét teljes egészében megkerüli, és közvetlenül a hardver gyártói meghajtójának adja át a hangmintákat.

**Használatához szükséges:**
- Külső USB/PCIe DAC vagy hangkártya, amelyhez a gyártó telepíthető Windows ASIO-meghajtót (COM DLL-t, regisztrálva a `HKLM\SOFTWARE\ASIO` alatt) biztosít.
- Az Aqulify maga **nem** tartalmaz és nem telepít gyártói ASIO-meghajtót: a gépen már telepített meghajtókat futás közben sorolja fel és tölti be.

## 2. Mit tud az ASIO-kimenet?

- **Csak a beépített zenelejátszóhoz (`FilePlayer`)**: a lejátszott zenefájlokat küldi ki közvetlenül a választott ASIO-meghajtón.
- **PCM lejátszás 44,1 kHz-től 768 kHz-ig**: a meghajtó által támogatott mintavételi frekvenciákon (`canSampleRate`), automatikus órajel-váltással.
- **Támogatott PCM mintatípusok**:
  - `ASIOSTInt32LSB` (32 bites előjeles egész, kis-endián – 24 bites forrás a felső 24 biten bitpontosan),
  - `ASIOSTInt24LSB` (24 bites tömörített egész, 3 bájt, kis-endián – 24 bites forrás bitpontosan),
  - `ASIOSTInt16LSB` (16 bites egész, kis-endián),
  - `ASIOSTFloat32LSB` (32 bites IEEE 754 lebegőpontos),
  - `ASIOSTFloat64LSB` (64 bites IEEE 754 lebegőpontos).
- **Natív DSD lejátszás (DSD64 … DSD1024)**:
  - Ha a meghajtó támogatja az ASIO DSD módot (`kAsioSetIoFormat` → `kASIODSDFormat`) és a DSD bitrátát (`2,8224 MHz` … `49,152 MHz`), a lejátszó a DSD-bitfolyamot közvetlenül, DoP-jelölők nélkül adja át (`ASIOSTDSDInt8MSB1` és `ASIOSTDSDInt8LSB1` bitsorrend támogatással).
- **DoP 1.1 tartalék és PCM visszalépés**:
  - Ha a meghajtó nem támogatja a natív DSD módot, de PCM-ben fogadja a DoP keretrátát (pl. `176,4 kHz` DSD64-hez) 24 vagy 32 bites egész formátumban, a kimenet DoP 1.1 PCM-ként küldi ki a DSD-t.
  - Ha egyik sem támogatott, a lejátszó automatikusan a belső DSD→PCM átalakításra lép vissza.
- **Valós idejű biztonság és hibakezelés**:
  - A meghajtó `bufferSwitch` / `bufferSwitchTimeInfo` visszahívása előre lefoglalt pufferekkel, memória-foglalás és blokkoló művelet nélkül fut.
  - Kezeli a meghajtó újraindítási kérését (`kAsioResetRequest`) és az eszköz menet közbeni leválasztását.

## 3. Mit nem csinál (korlátok)?

- **Csak Windows**: Linuxon a PipeWire és a közvetlen ALSA-kimenet, macOS-en a CoreAudio kizárólagos mód tölti be ugyanezt a szerepet.
- **Kizárólagos használat**: amíg az ASIO-kimenet aktív, a hangkártyát más alkalmazás általában nem tudja egyszerre használni, és egyszerre egy ASIO-meghajtó lehet nyitva a lejátszóban.
- **Rendszerhangok**: a böngésző és más Windows-alkalmazások hangja nem az ASIO-kimeneten, hanem a normál Windows hangeszközön (Equalizer APO / WASAPI) szól.
- **Natív DSD alatt nincs DSP**: natív DSD és DoP lejátszáskor a bitfolyam módosítás nélkül megy a DAC-ra, így az EQ, a konvolver és a hangerőszabályzás ilyenkor nem hat.

## 4. Licenc és jogi megjegyzés

- Az Aqulify a Steinberg hivatalos **ASIO SDK 2.3.4** (`2025-10-15`) nyílt forráskódú választási lehetőségét használja a **GNU General Public License Version 3 (GPLv3)** licenc alatt (`third_party/asiosdk/LICENSE.txt`, `LICENSE`).
- Az ASIO a Steinberg Media Technologies GmbH bejegyzett védjegye.
