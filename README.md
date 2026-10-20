# AetherAudio • Valós Idejű Fázis, Spektrum & Akusztikai Vízesés DSP Csomag

[![GitHub Release](https://img.shields.io/github/v/release/Szorgos/ee-phase-waterfall-analyzer?style=flat-square&color=blue)](https://github.com/Szorgos/ee-phase-waterfall-analyzer/releases/latest)
[![Windows Download](https://img.shields.io/badge/Windows%2011-Telep%C3%ADt%C5%91%20(.exe)-0078D6?style=flat-square&logo=windows)](https://github.com/Szorgos/ee-phase-waterfall-analyzer/releases/latest/download/AetherAudio-Setup.exe)
[![Linux PipeWire](https://img.shields.io/badge/Linux-PipeWire%20SPA-FCC624?style=flat-square&logo=linux&logoColor=black)](https://github.com/Szorgos/ee-phase-waterfall-analyzer#telep%C3%ADt%C3%A9s-linuxon-egyetlen-parancsb%C3%B3l)
[![Támogatás](https://img.shields.io/badge/T%C3%A1mogat%C3%A1s-Revolut%20%40gergoqhg6-ff5c8a?style=flat-square&logo=revolut)](#-támogatás)
[![License: GPL-3.0](https://img.shields.io/badge/License-GPLv3-green.svg?style=flat-square)](https://www.gnu.org/licenses/gpl-3.0)

> ### 📦 **Gyors Letöltés Windows 11-re:**
> 👉 **[AetherAudio-Setup.exe letöltése – mindig a legújabb kiadás](https://github.com/Szorgos/ee-phase-waterfall-analyzer/releases/latest/download/AetherAudio-Setup.exe)**  
> *(Telepítő: AetherAudio motor tálcaikonnal, saját ablakos felület és az Equalizer APO plugin automatikus bekötése. Korábbi verziók és ellenőrzőösszegek: [Kiadások](https://github.com/Szorgos/ee-phase-waterfall-analyzer/releases).)*

---
**Projekt:** `AetherAudio`  
**Cél:** Professzionális, zéró késleltetésű C++20 DSP motor és valós idejű 3D akusztikai vízesés / 2D fázis- és csoportkésleltetés-analizátor a Linux PipeWire és audiofil zenehallgató rendszerekhez.

---

### ✨ A Név Eredete és Filozófiája

> *„Az éter nem más, mint a tökéletes transzparencia: amikor a technika láthatatlanná válik, és csak a zene marad a térben lebegve.”*

Az **Aether** (magyarul *éter*, ógörögül *αἰθήρ – aithér*, a szikrázó, tiszta ragyogásból ered) a klasszikus mitológiában és filozófiában az **ötödik elem**, a kvintesszencia: a mennyei, romolhatatlan szféra, amit a csillagok között az istenek lélegeznek be. A 19. századi fizika úgy tartotta, hogy az éter az a mindent átható, láthatatlan és súlytalan rugalmas közeg, amelyben a fény és az elektromágneses hullámok akadálytalanul terjednek a kozmoszban.

Az audiofil világban az éteriség (*airiness*) a szent grál: az a pillanat, amikor megszűnik a hangsugárzók és fejhallgatók fizikai jelenléte. A hangok nem a hangszórók membránjából szólnak, hanem testet öltenek a térben, kristálytiszta lebegéssel, mikrodinamikával és holografikus színpadképpel. 

Az **AetherAudio** ezt a filozófiát egyesíti a modern digitális jelfeldolgozással:
* **Analitikus transzparencia:** Alacsony késleltetésű C++20 algoritmusok; választható minimálfázisú (IIR vagy FIR), köztes és lineáris fázisú szűrők.
* **Akusztikai látásmód:** A rejtett rezonanciák és a lineáris fázisú szűrők előcsengésének (pre-ringing) megjelenítése a 3D vízesés-spektrogramon.
* **Akusztikai tisztaság:** Teljes kontroll a fázis, a térhatás és a csúcsértékek felett, hogy a zene pontosan úgy szólaljon meg, ahogyan megszületett.

---

> 💡 **Ennyit a bullshitről – most pedig lássuk a kőkemény technikai valóságot és a szoftver tudását!**

---

## 🎧 Főbb Képességek és Funkciók

1. **Élő 3D Akusztikai Vízesés & Spektrogram (Top-Strip WebGL Engine):**
   * Valós idejű, 60 FPS gördülés állítható időszeletekkel (60 – 240 lépcső).
   * **Előcsengés (pre-ringing) jelzése:** Lineáris / köztes fázisú FIR módban egy diszkrét felirat jelzi, hogy előcsengés lehetséges. Mértéke általában olyan kicsi, hogy a vízesésen alig látszik; a program nem nagyítja fel.
   * Állítható hőtérkép és nézőpontok (3D Izometrikus, Felülnézeti 2D Hőtérkép, Szembőli Spektrum).

2. **2D Analitikus Fázis- és Csoportkésleltetés ($-\frac{d\phi}{d\omega}$):**
   * Zárt matematikai képletű Biquad IIR kalkuláció (0% CPU terhelés).
   * Interaktív, Pro-Q stílusú közvetlen pont-húzás a görbén és egérgörgős Q-faktor állítás.
   * Kétirányú numerikus beviteli mezők ezredpontos értékekkel.

3. **8 850+ Fejhallgató AutoEq Profil Kereső & Kézi Importálás:**
   * **Valós AutoEq Adatbázis Integráció:** A Jaakko Pasanen-féle hivatalos AutoEq tároló több mint 8 850 fül- és fejhallgatójának (oratory1990, Crinacle, Rtings stb.) élő keresése és profil-letöltése közvetlenül a GitHubról egyetlen kattintással.
   * **Univerzális Kézi Importáló (Squiglink / REW / EqualizerAPO):** A Squiglinkről vagy más mérésekből exportált CSV és TXT görbék, illetve REW / APO szűrők közvetlen beillesztése és automatikus felismerése manuális módban.
   * **EQ profilok:** AutoEq importkor és kézi beállításkor automatikusan profil készül; az aktív profil neve a fejlécben látszik, és a korábbi profilok újraindítás után is választhatók (tárolás: Linuxon `~/.config/aetheraudio/presets.json`, Windowson `%APPDATA%\AetherAudio\presets.json`). Az importált AutoEq profil érintetlen marad: kézi módosításkor „– módosított” nevű saját profil készül.
   * **Intelligens Dupla-Preamp Védelem:** Ha az importált profil már tartalmaz negatív preampot, azt használja; ha nem, az Auto-Guard állítja be a pozitív kiemelések kivédésére.

4. **Kézi Mesterkapcsolók Minden Modulhoz (Ki/Bekapcsolható lánc):**
   * **Parametrikus EQ:** Szabadon állítható sávok minimálfázisú (IIR) és lineáris fázisú (FIR) móddal.
   * **Crossfeed Szimulátor:** Bauer bs2b, Jan Meier Corda és Studio Nearfield térhatás-profilok a fejhallgatós sztereó izoláció megszüntetésére.
   * **Convolver (Partícionált FIR):** IR (WAV/IRS) betöltés és előnézet; a feldolgozási késleltetés nem nő az IR hosszával (az IR saját késleltetése megmarad). Szobakorrekcióhoz mérésből tervezett korrekciós szűrőt tölts be (pl. REW), ne a szoba nyers impulzusválaszát.
   * **True Peak Limiter & Soft-Clipper:** BS.1770-szerű, túlmintavételezett (1x–8x) mintaközi csúcsbecslés, alapértelmezett -1,0 dBTP plafon. A túlvezérlés ellen elsősorban a teljes EQ-görbéhez igazított előerősítés véd; a limiter biztonsági háló (negatív preampú profil importjakor vagy betöltésekor automatikusan bekapcsol), nem hallásvédelem.
   * **Bass Enhancer:** Pszichoakusztikai hiányzó alaphang szintézis membránkímélő 30 Hz-es szub-vágással.

5. **Zármentes (Lock-free) C++20 Ringbuffer és PipeWire SPA Integráció:**
   * Zéró hangkimaradás (xrun-free): a grafikai terhelés soha nem akaszthatja meg a zenelejátszást.
   * Kétirányú, valós idejű WebSocket híd (`ws://127.0.0.1:8080`) a hangmotor és a felület között: a motor valódi FFT spektrumot küld, a GUI pedig a preampot, a modulkapcsolókat és az EQ sávokat.

---

## 🚀 Telepítés Linuxon (Egyetlen Parancsból)

A telepítő szkript automatikusan felismeri a disztribúciót, felrakja a szükséges csomagokat, lefordítja a C++ motort és regisztrálja az asztali alkalmazásikont.

### 1. Letöltés és Telepítés:
A [legújabb kiadásnál](https://github.com/Szorgos/ee-phase-waterfall-analyzer/releases/latest) töltsd le az `AetherAudio-Linux-<verzió>.tar.gz` csomagot, majd:

```bash
tar xzf AetherAudio-Linux-*.tar.gz
cd AetherAudio-Linux-*/
./install.sh
```

Vagy a fejlesztői változat a GitHubról (frissítés: `git pull`, majd újra `./install.sh`):

```bash
git clone https://github.com/Szorgos/ee-phase-waterfall-analyzer.git
cd ee-phase-waterfall-analyzer
./install.sh
```

### 2. Támogatott Linux Disztribúciók:
* **Ubuntu / Debian / Linux Mint / Pop!_OS:** `apt-get` csomagkezelővel
* **Fedora / RHEL / CentOS:** `dnf` csomagkezelővel
* **Arch Linux / Manjaro / EndeavourOS:** `pacman` csomagkezelővel
* **openSUSE / SUSE:** `zypper` csomagkezelővel

---

## 🪟 Windows 11 verzió

Egyetlen telepítő: **`AetherAudio-Setup.exe`**. Felismeri (vagy letölti) az **Equalizer APO**-t, a plugint automatikusan a lánc végére illeszti, parancsikonokat és eltávolítót hoz létre. A motor tálcaikonos háttéralkalmazásként fut, a GUI ugyanaz, mint Linuxon. Részletek: **[windows/README_WINDOWS.md](windows/README_WINDOWS.md)**.

A Linuxos és a Windowsos ág ugyanazt a C++ DSP láncot futtatja (`include/aether_chain.hpp`): IIR vagy FIR EQ (lineáris / köztes / minimálfázis), Bass Enhancer, crossfeed, partícionált FFT konvolver és limiter. A drága műveletek háttérszálon futnak, a csere kattanásmentes.

---

## 🎮 Indítás és Használat (Linux)

Az `install.sh` mindent beállít:

* **Motor:** `aetheraudio-engine` systemd felhasználói szolgáltatásként fut, és bejelentkezéskor magától indul. Létrehoz egy **„AetherAudio”** nevű kimeneti hangeszközt; ami ide szól, az a DSP láncon át a valódi hangkártyára / DAC-ra megy (a WirePlumber köti be automatikusan).
* **Használat:** a *Hangbeállításokban* válaszd kimenetnek az **AetherAudio**-t (vagy a telepítő végén kérd, hogy ez legyen az alapértelmezett).
* **Felület:** menü → **AetherAudio**. Saját ablakban nyílik (WebKitGTK); ha a WebKitGTK fejlesztői csomag nem telepíthető, böngészőben.
* **Beállítások:** a `~/.config/aetheraudio/` mappába mentődnek (EQ, modulok, betöltött impulzusválasz), újraindítás után is megmaradnak.
* **Állapot / napló:** `systemctl --user status aetheraudio`, `journalctl --user -u aetheraudio -f`
* **Eltávolítás:** `./uninstall.sh` (a `--purge` a beállításokat is törli)

A fejléc jelvénye: **„● Élő – PipeWire – AetherAudio • 48.0 kHz”** = a motor fut és szűr; **„○ Motor Offline (Szimuláció)”** = a motor nem fut, a felület a beépített tesztjelet mutatja.

> EasyEffects mellett is használható: ilyenkor a kettő közül csak az egyiket válaszd kimenetnek, vagy láncold őket (pl. qpwgraph-fal), különben a hang kétszer menne át korrekción.

---

## 🛠️ Kézi Fordítás és Fejlesztői Környezet

Ha kézzel szeretnéd lefordítani vagy módosítani a C++ motort:

```bash
# Függőségek (Ubuntu/Debian példa):
sudo apt install build-essential cmake libpipewire-0.3-dev pkg-config

# Fordítás (a saját ablakhoz: libgtk-3-dev libwebkit2gtk-4.1-dev):
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DBUILD_PIPEWIRE_PLUGIN=ON
cmake --build build -j$(nproc)
./build/aetheraudio-engine        # motor kézi indítása (Ctrl+C: leállítás)

# Tesztek futtatása:
./build/test_math         # Zárt képletű csoportkésleltetés teszt
./build/test_pipeline     # DSP lánc és limiter csillapítás teszt
./build/test_ringbuffer   # 96 000 mintás többszálú zármentes átviteli teszt
./build/test_core         # FFT spektrum, seqlock, tap körpuffer, JSON, átlátszóság
./build/test_dsp          # Konvolver, FIR EQ, Bass Enhancer, limiter, teljes lánc
```

---

## 📂 Projektstruktúra

```
├── CMakeLists.txt              # C++20 szabványos építőrendszer (Linux & Windows)
├── install.sh                  # Univerzális Linux disztró-felismerő és telepítő szkript
├── uninstall.sh                # Linux eltávolító
├── gui_integrated_system.html  # Teljes EasyEffects Adwaita grafikus felület és 3D WebGL
├── autoeq_index.js             # 8 850+ modellt tartalmazó AutoEq fülhallgató adatbázis
├── include/
│   ├── aether_chain.hpp        # Teljes DSP lánc háttérszállal (Linux + Windows közös mag)
│   ├── partitioned_convolver.hpp # Zéró latency partícionált FFT konvolver
│   ├── fir_design.hpp          # FIR EQ tervezés, IR újramintavételezés, minimálfázis
│   ├── bass_enhancer.hpp       # Pszichoakusztikai mélyhang-kiemelő
│   ├── biquad_analyzer.hpp     # Analitikus IIR szűrőmatematika és csoportkésleltetés
│   ├── crossfeed.hpp           # Bauer bs2b és Jan Meier fejhallgatós térhatás motor
│   ├── convolver.hpp           # Partícionált FIR impulzusválasz konvolúció
│   ├── limiter.hpp             # True Peak limiter és analóg soft-clipper védelem
│   ├── dsp_pipeline.hpp        # Integrált feldolgozási lánc (Preamp -> EQ -> CF -> Conv -> Limiter)
│   ├── ringbuffer.hpp          # Zármentes SPSC körpuffer valós idejű hangátvitelhez
│   ├── pipewire_sink_node.hpp  # Linux: „AetherAudio” virtuális kimenet (két pw_stream, filter-chain mintára)
│   ├── linux_store.hpp         # Linux: beállítások és IR mentése (~/.config/aetheraudio)
│   ├── ws_bridge.hpp           # RFC 6455 WebSocket híd (Linux + Windows), valódi FFT spektrummal
│   ├── net_compat.hpp          # BSD socket / Winsock2 közös réteg
│   ├── spectrum.hpp            # Hann-ablakos FFT, 160 logaritmikus sáv + L/R fázis
│   ├── param_block.hpp         # Zármentes (seqlock) paraméterblokk GUI -> audio szál
│   ├── param_apply.hpp         # Paraméterek alkalmazása a DSP láncra, foglalásmentesen
│   ├── tap_ring.hpp            # Felülíró sztereó tap körpuffer (közös memóriában is)
│   ├── shared_state.hpp        # Plugin <-> motor közös memória elrendezés (Windows)
│   ├── mini_json.hpp           # Függőségmentes JSON olvasó a vezérlőüzenetekhez
│   └── sha1_base64.hpp         # Független SHA-1 és Base64 modul a WS kézfogáshoz
├── src/
│   ├── pipewire_plugin.cpp     # Linux motor (aetheraudio-engine) belépési pontja
│   ├── aether_gui_linux.cpp    # Linux felület saját ablakban (WebKitGTK)
│   ├── test_core.cpp           # Közös mag teszt (FFT, seqlock, tap, JSON, átlátszóság)
│   ├── test_dsp.cpp            # Konvolver, FIR EQ, Bass Enhancer, teljes lánc teszt
│   ├── test_math.cpp           # Matematikai ellenőrző alkalmazás
│   ├── test_pipeline.cpp       # DSP integrációs tesztprogram
│   └── test_ringbuffer.cpp     # Többszálú valós idejű stabilitási tesztprogram
└── windows/
    ├── README_WINDOWS.md       # Windows telepítés, használat, fordítás
    ├── engine/                 # AetherAudio.exe (tálcás motor, WASAPI loopback, plugin híd)
    ├── plugin/                 # AetherAudio.dll Equalizer APO plugin (VST 2.4 ABI)
    ├── installer/              # AetherAudio-Setup.exe telepítő és eltávolító
    ├── tools/eqapo_host_sim.cpp# Teszt-hoszt: az EQ APO betöltési sorrendjét utánozza
    ├── common/win_shared.hpp   # Közös memória, beállítás- és IR-fájl (ProgramData)
    ├── resources/              # Ikon, manifestek, verzióinformáció
    ├── third_party/three/      # three.js r128 (MIT) az offline GUI-hoz
    └── toolchain-llvm-mingw.cmake # Keresztfordítás Linuxról
```

---

## 🔏 Kódaláírási szabályzat (Code signing policy)

A Windows kiadásokat (`AetherAudio-Setup.exe`, valamint a benne lévő `AetherAudio.exe`, `AetherAudioGui.exe` és `AetherAudio.dll`) aláírva tesszük közzé.

Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by [SignPath Foundation](https://signpath.org/).

- **Szerepek:** fejlesztő (committer), ellenőrző (reviewer) és jóváhagyó (approver): [Szorgos](https://github.com/Szorgos) (Sass Gergő).
- **Mit írunk alá:** csak az ebben a tárolóban lévő forrásból, a GitHub Actions `Release` workflow-ján fordított fájlokat. Minden aláírási kérést a jóváhagyó egyenként hagy jóvá.
- **Adatvédelem:** az AetherAudio nem gyűjt és nem küld személyes adatot. Hálózatot csak ezekre használ: a felület a GitHubról tölti le az AutoEq fejhallgató-listát és a kiválasztott profilt, a „Támogatás” gomb pedig a Revolut oldalát nyitja meg a böngészőben.

Részletek a beállításhoz: [docs/code-signing.md](docs/code-signing.md).

## 📄 Licenc

Ez a projekt a **GNU General Public License v3.0 (GPL-3.0)** alatt érhető el.
* Copyright (C) 2026 **Szorgos** (<https://github.com/Szorgos>)
* A részletes licencfeltételeket a [LICENSE](LICENSE) fájl tartalmazza.
* Beépített külső összetevők: [three.js](https://threejs.org/) (MIT), [webview/webview](https://github.com/webview/webview) (MIT), Microsoft WebView2 SDK fejlécek (BSD-3-Clause).
* Az AutoEq index és profilok forrása az [AutoEq GitHub projekt](https://github.com/jaakkopasanen/AutoEq) (MIT Licenc).

---

## ♥ Támogatás

Az AetherAudio **ingyenes és nyílt forrású (donateware)**: minden funkció korlátozás nélkül használható. Ha hasznosnak találod, egy önkéntes támogatással segítheted a további fejlesztést. Köszönöm!

<p align="center">
  <a href="https://revolut.me/gergoqhg6"><img src="docs/revolut-qr.png" alt="Revolut QR-kód: @gergoqhg6" width="220"></a><br>
  <b>Revolut: <a href="https://revolut.me/gergoqhg6">@gergoqhg6</a></b>
</p>

A programban a fejléc **♥ Támogatás** gombja ugyanezt mutatja.
