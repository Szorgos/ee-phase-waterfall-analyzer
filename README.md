# AetherAudio • Valós Idejű Fázis, Spektrum & Akusztikai Vízesés DSP Csomag
**Projekt:** `AetherAudio`  
**Cél:** Professzionális, zéró késleltetésű C++20 DSP motor és valós idejű 3D akusztikai vízesés / 2D fázis- és csoportkésleltetés-analizátor a Linux PipeWire és audiofil zenehallgató rendszerekhez.

---

### ✨ A Név Eredete és Filozófiája

> *„Az éter nem más, mint a tökéletes transzparencia: amikor a technika láthatatlanná válik, és csak a zene marad a térben lebegve.”*

Az **Aether** (magyarul *éter*, ógörögül *αἰθήρ – aithér*, a szikrázó, tiszta ragyogásból ered) a klasszikus mitológiában és filozófiában az **ötödik elem**, a kvintesszencia: a mennyei, romolhatatlan szféra, amit a csillagok között az istenek lélegeznek be. A 19. századi fizika úgy tartotta, hogy az éter az a mindent átható, láthatatlan és súlytalan rugalmas közeg, amelyben a fény és az elektromágneses hullámok akadálytalanul terjednek a kozmoszban.

Az audiofil világban az éteriség (*airiness*) a szent grál: az a pillanat, amikor megszűnik a hangsugárzók és fejhallgatók fizikai jelenléte. A hangok nem a hangszórók membránjából szólnak, hanem testet öltenek a térben, kristálytiszta lebegéssel, mikrodinamikával és holografikus színpadképpel. 

Az **AetherAudio** ezt a filozófiát egyesíti a modern digitális jelfeldolgozással:
* **Analitikus transzparencia:** Zéró késleltetésű C++20 algoritmusok, minimálfázisú és lineáris fázisú szűrők fázistorzulás nélküli átvitele.
* **Akusztikai látásmód:** A rejtett rezonanciák és FIR előcsengések (pre-ringing) leleplezése a 3D vízesés-spektrogramon keresztül.
* **Akusztikai tisztaság:** Teljes kontroll a fázis, a térhatás és a csúcsértékek felett, hogy a zene pontosan úgy szólaljon meg, ahogyan megszületett.

---

> 💡 **Ennyit a bullshitről – most pedig lássuk a kőkemény technikai valóságot és a szoftver tudását!**

---

## 🎧 Főbb Képességek és Funkciók

1. **Élő 3D Akusztikai Vízesés & Spektrogram (Top-Strip WebGL Engine):**
   * Valós idejű, 60 FPS gördülés állítható időszeletekkel (60 – 240 lépcső).
   * **FIR Pre-ringing automatikus észlelés:** Ragyogó fehér lecsengési foltokkal és HUD jelvénnyel mutatja ki a lineáris fázisú szűrők által keltett előcsengést.
   * Állítható hőtérkép és nézőpontok (3D Izometrikus, Felülnézeti 2D Hőtérkép, Szembőli Spektrum).

2. **2D Analitikus Fázis- és Csoportkésleltetés ($-\frac{d\phi}{d\omega}$):**
   * Zárt matematikai képletű Biquad IIR kalkuláció (0% CPU terhelés).
   * Interaktív, Pro-Q stílusú közvetlen pont-húzás a görbén és egérgörgős Q-faktor állítás.
   * Kétirányú numerikus beviteli mezők ezredpontos értékekkel.

3. **8 850+ Fejhallgató AutoEq Profil Kereső & Kézi Importálás:**
   * **Valós AutoEq Adatbázis Integráció:** A Jaakko Pasanen-féle hivatalos AutoEq tároló több mint 8 850 fül- és fejhallgatójának (oratory1990, Crinacle, Rtings stb.) élő keresése és profil-letöltése közvetlenül a GitHubról egyetlen kattintással.
   * **Univerzális Kézi Importáló (Squiglink / REW / EqualizerAPO):** A Squiglinkről vagy más mérésekből exportált CSV és TXT görbék, illetve REW / APO szűrők közvetlen beillesztése és automatikus felismerése manuális módban.
   * **Intelligens Dupla-Preamp Védelem:** Ha az importált profil már tartalmaz negatív preampot, azt használja; ha nem, az Auto-Guard állítja be a pozitív kiemelések kivédésére.

4. **Kézi Mesterkapcsolók Minden Modulhoz (Ki/Bekapcsolható lánc):**
   * **Parametrikus EQ:** Szabadon állítható sávok minimálfázisú (IIR) és lineáris fázisú (FIR) móddal.
   * **Crossfeed Szimulátor:** Bauer bs2b, Jan Meier Corda és Studio Nearfield térhatás-profilok a fejhallgatós sztereó izoláció megszüntetésére.
   * **Convolver (Partícionált FIR):** Zéró késleltetésű szobaimpulzus (WAV/IRS) betöltés és előnézet.
   * **True Peak Limiter & Soft-Clipper:** ITU-R BS.1770-4 szabványú -1.0 dBTP plafon 4x Lanczos túlmintavételezéssel.
   * **Bass Enhancer:** Pszichoakusztikai hiányzó alaphang szintézis membránkímélő 30 Hz-es szub-vágással.

5. **Zármentes (Lock-free) C++20 Ringbuffer és PipeWire SPA Integráció:**
   * Zéró hangkimaradás (xrun-free): a grafikai terhelés soha nem akaszthatja meg a zenelejátszást.
   * Kétirányú, valós idejű WebSocket híd (`ws://127.0.0.1:8080`) a Linuxos hangmotor és a felület között.

---

## 🚀 Telepítés Linuxon (Egyetlen Parancsból)

A telepítő szkript automatikusan felismeri a disztribúciót, felrakja a szükséges csomagokat, lefordítja a C++ motort és regisztrálja az asztali alkalmazásikont.

### 1. Klónozás és Telepítés:
Nyiss meg egy terminált, és futtasd:

```bash
git clone https://github.com/<felhasznalonev>/ee-phase-waterfall-analyzer.git
cd ee-phase-waterfall-analyzer
chmod +x install.sh
./install.sh
```

### 2. Támogatott Linux Disztribúciók:
* **Ubuntu / Debian / Linux Mint / Pop!_OS:** `apt-get` csomagkezelővel
* **Fedora / RHEL / CentOS:** `dnf` csomagkezelővel
* **Arch Linux / Manjaro / EndeavourOS:** `pacman` csomagkezelővel
* **openSUSE / SUSE:** `zypper` csomagkezelővel

---

## 🎮 Indítás és Használat

A telepítés után kétféleképpen is elindíthatod a rendszert:

1. **A Terminálból:**
   ```bash
   ./run.sh
   ```
   Ez azonnal elindítja a PipeWire C++ szűrőcsomópontot, és megnyitja a grafikus kezelőfelületet az alapértelmezett böngésződben.

2. **A Linux Alkalmazásmenüből:**
   Keresd meg a rendszer menüjében a **`EasyEffects Phase & Waterfall Analyzer`** ikont, és kattints rá!

3. **Csatlakozási állapot:**
   A fejlécben lévő zöld jelvény jelzi az élő kapcsolatot:
   * **`● PipeWire Live (Csatlakoztatva)`**: A valós rendszerhang szűrése és 3D megjelenítése aktív.
   * **`○ PipeWire Offline (Szimuláció)`**: Ha a PipeWire démon nem elérhető, a felület automatikusan a beépített tesztgenerátorra vált.

---

## 🛠️ Kézi Fordítás és Fejlesztői Környezet

Ha kézzel szeretnéd lefordítani vagy módosítani a C++ motort:

```bash
# Függőségek (Ubuntu/Debian példa):
sudo apt install build-essential cmake libpipewire-0.3-dev pkg-config

# Fordítás:
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DBUILD_PIPEWIRE_PLUGIN=ON
cmake --build build -j$(nproc)

# Tesztek futtatása:
./build/test_math         # Zárt képletű csoportkésleltetés teszt
./build/test_pipeline     # DSP lánc és limiter csillapítás teszt
./build/test_ringbuffer   # 96 000 mintás többszálú zármentes átviteli teszt
```

---

## 📂 Projektstruktúra

```
├── CMakeLists.txt              # C++20 szabványos építőrendszer (Linux & Windows)
├── install.sh                  # Univerzális Linux disztró-felismerő és telepítő szkript
├── run.sh                      # Kényelmi futtató szkript (Web + PipeWire indítás)
├── gui_integrated_system.html  # Teljes EasyEffects Adwaita grafikus felület és 3D WebGL
├── autoeq_index.js             # 8 850+ modellt tartalmazó AutoEq fülhallgató adatbázis
├── include/
│   ├── biquad_analyzer.hpp     # Analitikus IIR szűrőmatematika és csoportkésleltetés
│   ├── crossfeed.hpp           # Bauer bs2b és Jan Meier fejhallgatós térhatás motor
│   ├── convolver.hpp           # Partícionált FIR impulzusválasz konvolúció
│   ├── limiter.hpp             # True Peak limiter és analóg soft-clipper védelem
│   ├── dsp_pipeline.hpp        # Integrált feldolgozási lánc (Preamp -> EQ -> CF -> Conv -> Limiter)
│   ├── ringbuffer.hpp          # Zármentes SPSC körpuffer valós idejű hangátvitelhez
│   ├── pipewire_filter_node.hpp# Natív Linux pw_filter audio csomópont
│   ├── ws_bridge.hpp           # RFC 6455 élő WebSocket szerver a GUI összekötéséhez
│   └── sha1_base64.hpp         # Független SHA-1 és Base64 modul a WS kézfogáshoz
└── src/
    ├── pipewire_plugin.cpp     # A PipeWire háttérszolgáltatás fő belépési pontja
    ├── test_math.cpp           # Matematikai ellenőrző alkalmazás
    ├── test_pipeline.cpp       # DSP integrációs tesztprogram
    └── test_ringbuffer.cpp     # Többszálú valós idejű stabilitási tesztprogram
```

---

## 📄 Licenc
* A projekt forráskódja nyílt forráskódú (GPLv3 / MIT kompatibilis).
* Az AutoEq index és profilok forrása az [AutoEq GitHub projekt](https://github.com/jaakkopasanen/AutoEq) (MIT Licenc).
