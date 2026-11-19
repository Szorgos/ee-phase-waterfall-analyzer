# Tesztek platformonként

Melyik tesztbináris és élő teszt melyik platformon fut, és ha nem fut, miért. A `tools/check_test_lists.py`
ellenőrzi, hogy az alábbi táblázat egyezik-e a tényleges listákkal (CMake, munkafolyamatok, csomagolás): ha egy
CMake-teszt indok nélkül hiányzik egy listából, vagy a táblázat elavult, hibát jelez.

A listák helye:

| Oszlop | Forrás |
|---|---|
| Windows | `.github/workflows/windows-tests.yml` (natív Windows, MSVC-mentes llvm-mingw fordítás) |
| macOS | `.github/workflows/macos.yml` (arm64 natívan és x86_64 Rosettán) |
| Linux deb | `debian/rules` (a kiadás `debian-deb` feladata és a helyi `dpkg-buildpackage`) |
| Fedora rpm | `packaging/fedora/aqulify.spec` (`%check`, COPR) |
| Arch | `packaging/arch/PKGBUILD` (`check()`, a kiadás Arch-konténere) |
| Pi | `debian/rules` a `tools/pi/pi_deb_build.sh`-n át (`pi-deb.yml`: natív arm64). `AQ_EMULALT=1` (QEMU) esetén a `test_dsp`, `test_hires_b` és `test_src_ref` kimarad (valós idejű sebességmérés). |
| live-tests | `.github/workflows/live-tests.yml` (Linux, PipeWire, virtuális hangkártyák) |

A Linux kiadási fordítás (`release.yml`, `linux-package` és a Windows-feladat „Közös DSP tesztek (Linux)” lépése)
külön, rövidebb listát futtat; a Linux-csomag teljes listája a `debian/rules`.

## Tesztbinárisok

Cellák: `igen` = a platform listája futtatja; `nem: <indok>`.

<!-- tesztlista:kezdet -->
| Teszt | Windows | macOS | Linux deb | Fedora rpm | Arch | Pi | live-tests |
|---|---|---|---|---|---|---|---|
| `test_alsa` | nem: ALSA csak Linuxon van, a CMake csak ott építi | nem: ALSA csak Linuxon van, a CMake csak ott építi | nem: ALSA-eszközt nyit, a csomagépítő környezetben (chroot, konténer) nem garantált; a live-tests és a release.yml linux-package futtatja | nem: ALSA-eszközt nyit, a csomagépítő környezetben (mock) nem garantált; a live-tests futtatja | nem: ALSA-eszközt nyit, a csomagépítő környezetben nem garantált; a live-tests futtatja | nem: ALSA-eszközt nyit, a Pi-csomag konténerében nem garantált; a live-tests futtatja | igen |
| `test_asio` | igen | nem: ASIO csak Windowson van, a CMake csak ott építi | nem: ASIO csak Windowson van, a CMake csak ott építi | nem: ASIO csak Windowson van, a CMake csak ott építi | nem: ASIO csak Windowson van, a CMake csak ott építi | nem: ASIO csak Windowson van, a CMake csak ott építi | nem: ASIO csak Windowson van, a CMake csak ott építi |
| `test_core` | igen | igen | igen | igen | igen | igen | nem: egységteszt hang nélkül; a live-tests csak a PipeWire-t igénylő teszteket futtatja |
| `test_dsd` | igen | igen | igen | nem: a Fedora-csomag csak kiadáskor épül (COPR), előtte nem ellenőrizhető; felvétele a tulajdonos döntése | nem: az Arch-csomag csak kiadáskor épül, előtte nem ellenőrizhető; felvétele a tulajdonos döntése | igen | nem: egységteszt hang nélkül; a live-tests csak a PipeWire-t igénylő teszteket futtatja |
| `test_dsp` | igen | igen | igen | igen | igen | igen | nem: egységteszt hang nélkül; a live-tests csak a PipeWire-t igénylő teszteket futtatja |
| `test_hires` | igen | igen | igen | nem: a Fedora-csomag csak kiadáskor épül (COPR), előtte nem ellenőrizhető; felvétele a tulajdonos döntése | nem: az Arch-csomag csak kiadáskor épül, előtte nem ellenőrizhető; felvétele a tulajdonos döntése | igen | nem: egységteszt hang nélkül; a live-tests csak a PipeWire-t igénylő teszteket futtatja |
| `test_hires_b` | igen | igen | igen | nem: a Fedora-csomag csak kiadáskor épül (COPR), előtte nem ellenőrizhető; felvétele a tulajdonos döntése | nem: az Arch-csomag csak kiadáskor épül, előtte nem ellenőrizhető; felvétele a tulajdonos döntése | igen | nem: egységteszt hang nélkül; a live-tests csak a PipeWire-t igénylő teszteket futtatja |
| `test_keepawake` | igen | nem: a Windows-os híd ébren tartását méri, más rendszeren a teszt üres | nem: a Windows-os híd ébren tartását méri, más rendszeren a teszt üres | nem: a Windows-os híd ébren tartását méri, más rendszeren a teszt üres | nem: a Windows-os híd ébren tartását méri, más rendszeren a teszt üres | nem: a Windows-os híd ébren tartását méri, más rendszeren a teszt üres | nem: a Windows-os híd ébren tartását méri, más rendszeren a teszt üres |
| `test_matrix` | igen | igen | igen | nem: a Fedora-csomag csak kiadáskor épül (COPR), előtte nem ellenőrizhető; felvétele a tulajdonos döntése | igen | igen | nem: egységteszt hang nélkül; a live-tests csak a PipeWire-t igénylő teszteket futtatja |
| `test_measure_live` | nem: PipeWire-gráfot és virtuális hangkártyát igényel (csak Linux) | nem: PipeWire-gráfot és virtuális hangkártyát igényel (csak Linux) | nem: PipeWire-gráfot igényel, a csomagépítés alatt nincs | nem: PipeWire-gráfot igényel, a csomagépítés alatt nincs | nem: PipeWire-gráfot igényel, a csomagépítés alatt nincs | nem: PipeWire-gráfot igényel, a Pi-csomag konténerében nincs | igen |
| `test_mic_basic` | igen | igen | nem: Linuxon nincs ilyen cél (a CMake csak Windowson és macOS-en építi); Linuxon a test_mic_live méri a felvételt | nem: Linuxon nincs ilyen cél (a CMake csak Windowson és macOS-en építi) | nem: Linuxon nincs ilyen cél (a CMake csak Windowson és macOS-en építi) | nem: Linuxon nincs ilyen cél (a CMake csak Windowson és macOS-en építi) | nem: Linuxon nincs ilyen cél; helyette a test_mic_live fut |
| `test_mic_live` | nem: PipeWire-gráfot és virtuális hangkártyát igényel (csak Linux) | nem: PipeWire-gráfot és virtuális hangkártyát igényel (csak Linux) | nem: PipeWire-gráfot igényel, a csomagépítés alatt nincs | nem: PipeWire-gráfot igényel, a csomagépítés alatt nincs | nem: PipeWire-gráfot igényel, a csomagépítés alatt nincs | nem: PipeWire-gráfot igényel, a Pi-csomag konténerében nincs | igen |
| `test_multiout` | igen | igen | igen | nem: a Fedora-csomag csak kiadáskor épül (COPR), előtte nem ellenőrizhető; felvétele a tulajdonos döntése | nem: az Arch-csomag csak kiadáskor épül, előtte nem ellenőrizhető; felvétele a tulajdonos döntése | igen | nem: egységteszt hang nélkül; a live-tests csak a PipeWire-t igénylő teszteket futtatja |
| `test_player` | igen | igen | igen | nem: a Fedora-csomag csak kiadáskor épül (COPR), előtte nem ellenőrizhető; felvétele a tulajdonos döntése | igen | igen | nem: egységteszt hang nélkül; a live-tests csak a PipeWire-t igénylő teszteket futtatja |
| `test_ringbuffer` | igen | igen | igen | igen | igen | igen | nem: egységteszt hang nélkül; a live-tests csak a PipeWire-t igénylő teszteket futtatja |
| `test_room` | igen | igen | igen | nem: a Fedora-csomag csak kiadáskor épül (COPR), előtte nem ellenőrizhető; felvétele a tulajdonos döntése | nem: az Arch-csomag csak kiadáskor épül, előtte nem ellenőrizhető; felvétele a tulajdonos döntése | igen | igen |
| `test_src_ref` | igen | igen | igen | nem: a Fedora-csomag csak kiadáskor épül (COPR), előtte nem ellenőrizhető; felvétele a tulajdonos döntése | nem: az Arch-csomag csak kiadáskor épül, előtte nem ellenőrizhető; felvétele a tulajdonos döntése | igen | nem: egységteszt hang nélkül; a live-tests csak a PipeWire-t igénylő teszteket futtatja |
| `test_upsample` | igen | igen | igen | igen | igen | igen | nem: egységteszt hang nélkül; a live-tests csak a PipeWire-t igénylő teszteket futtatja |
| `test_winout` | igen | nem: a Windows-os kimenetváltót (WASAPI / ASIO) teszteli, a CMake csak Windowson építi | nem: a Windows-os kimenetváltót teszteli, a CMake csak Windowson építi | nem: a Windows-os kimenetváltót teszteli, a CMake csak Windowson építi | nem: a Windows-os kimenetváltót teszteli, a CMake csak Windowson építi | nem: a Windows-os kimenetváltót teszteli, a CMake csak Windowson építi | nem: a Windows-os kimenetváltót teszteli, a CMake csak Windowson építi |
<!-- tesztlista:vége -->

## Élő tesztek (szkriptek)

Csak a live-tests futtatja (Linux, elszigetelt PipeWire, virtuális hangkártyák, headless Chromium); a többi
platform CI-jében nincs ilyen hangkörnyezet.

| Élő teszt | Mit vizsgál |
|---|---|
| `tests/live/mic_live.sh` (`test_mic_live`) | mikrofonfelvétel a null-kimenet monitorjáról |
| `tests/live/measure_live.sh` (`test_measure_live`) | a teljes szobamérés: a motor lejátszója + felvevő + kiértékelés |
| `tests/live/run_all.sh` | több kimenet (`multiout_live.sh`), útonkénti EQ, DSP-modulok (`modules_live.py`), haladó hangkimenet, felmintavételezés, nagy felbontás, a felület kezelőszervei |
| `tools/pi/test-aqulify-tune.sh` | a választható rendszerhangoló szkript |
