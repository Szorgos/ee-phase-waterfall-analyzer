### Újdonságok (1.6.2)
- **2D Grafikon Zoom & Pásztázás:** A 2D frekvenciameneti rajzterület mostantól finomhangolható zoommal és pásztázással rendelkezik (a sima egérgörgő funkciója érintetlenül megmaradt a szűrősávok Q-faktorának állítására):
  - **Függőleges dB skála:** Választható `±6 dB`, `±12 dB`, `±24 dB` és `±36 dB` nézet a tized-dB-es finomhangolásokhoz és mastering feladatokhoz.
  - **Vízszintes frekvencia zoom:** `🔍−` és `🔍+` gombok, valamint finom Zoom csúszka (1.0x – 5.0x nagyítás).
  - **Pásztázás (Pan):** Nagyított nézetben az egér bal gombjával üres területen húzva szabadon pásztázható a spektrum.
  - **Alaphelyzet (Reset):** `⟲` gombbal egyetlen kattintással visszaáll a normál teljes nézet.
  - **Ablakméret növelés:** `↕` gomb a grafikon rajzterületének megnövelésére (520 px magas nézet).
- **Független görbe-kapcsolók:** A kék EQ átviteli görbe, a narancssárga csoportkésleltetés és a zöld szaggatott fázisgörbe mostantól egyenként, tetszés szerint ki-be kapcsolható a vezérlősáv gombjaival. A megjelenítés kikapcsolása tisztán grafikai jellegű, a háttérben futó DSP motorra és a hangzásra garantáltan nincs hatással.
- **Finom Donate pulzálás:** A Támogatás (Donate) gomb diszkrét, percenként pontosan háromszor felvillanó halvány és lassú pulzáló animációt kapott.

### Újdonságok (1.6.1)
- **Tiszta indulás:** Telepítés és első indítás után minden modul alapból kikapcsolt (Bypass) állapotban van, a sávlista üres, így a hangzás teljesen érintetlen, nulláról indul.
- **Bővített frekvenciaskála:** A vízesés alatti skála mostantól 10 Hz – 24 kHz közötti tartományt fed le.
- **Gyorsabb frekvenciajelzők:** A zene sávszélességét mutató kék jelzések reakcióideje jelentősen gyorsult, azonnal és dinamikusan követik a lejátszott zenét.
- **Frekvencia-szélsőértékek kijelzése:** A skála két szélén kis diszkrét felirat mutatja az elmúlt 10 másodperc legmélyebb (Hz) és legmagasabb (kHz) mért frekvenciáját.
- **Fakítás csúszka:** A 3D vízesés telítettsége a jobb felső „Fakítás” csúszkával tetszés szerint lágyítható vagy teljesen fekete-fehérré tehető.
- **Fejléc finomhangolás:** A felső sáv háttere egységesen alkalmazkodik az ablak sötét tónusához, és megjeleníti a verziószámot (`v1.6.1`).

### Újdonságok (1.6.0) – az EasyEffects DSP könyvtárai
- **Equalizer:** LSP Parametric Equalizer (mint az EasyEffects-ben): IIR, FIR, FFT és SPM mód; sávonként 12 típus, RLC / BWC / LRX / APO szűrőmód és x1–x4 meredekség; decramp Off, x2, x3, x4, x6, x8 (44,1 kHz-hez viszonyítva). A görbét a motor számolja ugyanazokkal a szűrőkkel. Az AutoEq / APO import APO (DR) sávokat hoz létre; a korábbi profilok sávjai is APO (DR) módban töltődnek be, így a hangolásuk megmarad.
- **Limiter:** LSP Limiter: 12 mód, túlmintavételezés (True Peak is), dither, lookahead, attack, release, stereo link, gain boost, ALR, bemeneti és kimeneti erősítés.
- **Bass Enhancer:** Calf Bass Enhancer (amount, harmonics, scope, floor, blend, listen).
- **Crossfeed:** libbs2b: Bauer bs2b (700 Hz / 4,5 dB), Jan Meier (650 Hz / 9,5 dB), közeltéri stúdió (600 Hz / 3,5 dB).
- **Convolver:** zita-convolver (FFTW3), IR újramintavételezés SpeexDSP-vel, IR szélesség, autogain, dry / wet.
- A korábbi beállítások (params.bin) frissítéskor átkerülnek az új paraméterekre.

### Az 1.5.3 újdonságai
- **Javítva: crossfeed szint.** A középre kevert mély hangok eddig +3 dB-t kaptak (túlvezérlés, limiter-munka), a középhangok 1 kHz körül ~5,5 dB-t vesztettek. Most Bauer bs2b szerinti szűrés, mono jelre ~0 dB.
- **Frekvenciaskála:** jelek mutatják, milyen tartományt fog át a zene (alsó és felső határ, néhány másodpercig megmaradnak).

### Az 1.5.2 újdonságai
- **Linux:** a felület indításkor elindítja a motort, ha az nem fut (a csomag telepítése után így nem marad „Motor Offline”).
- **Linux:** sötét ablakcímsor a felület színeivel.

### Az 1.5.1 újdonságai
- **Fedora RPM csomag** minden kiadásnál (`aqulify-<verzió>.fc<NN>.x86_64.rpm`): `sudo dnf install ./aqulify-*.rpm`. Rendszerszintű telepítés menüvel, ikonnal, AppStream adatlappal (Discover / GNOME Szoftver) és automatikusan induló motorral.
- Előkészítve a Fedora COPR-tároló (automatikus frissítés `dnf`-fel és a Discoverből).

### Az 1.5.0 újdonságai – az AetherAudio új neve: Aqulify
- **Névváltás:** a program mostantól **Aqulify** (az „AetherAudio” nevet más hangszoftverek is használják). A GitHub-tároló címe nem változik.
- **Automatikus átállás frissítéskor:**
  - *Windows:* az `Aqulify-Setup.exe` eltávolítja a régi AetherAudio-t (futó program, Equalizer APO plugin sora – így nem fut két plugin egymás után –, parancsikonok, automatikus indítás), és átveszi a beállításokat, a konvolver IR-t és az EQ profilokat.
  - *Linux:* az `install.sh` leállítja és törli a régi `aetheraudio` szolgáltatást és programfájlokat; a `~/.config/aetheraudio` mappa első indításkor `~/.config/aqulify` lesz. Ha az AetherAudio volt az alapértelmezett kimenet, az Aqulify veszi át.
- A Windows „Programok és szolgáltatások” listájában most a valódi verziószám látszik (eddig tévesen 1.3.0).
- **Linux saját ablak:** a számmezők léptetőnyilai visszakerültek; a mezők szélesebbek és jobbra igazítottak, így a teljes érték látszik.

### Az 1.4.8 újdonságai
- **Linux saját ablak:** a számmezők (Freq, Gain, Q, Preamp stb.) értéke nem csúszik ki jobbra – a GTK léptetőgombok helyett görgővel és a fel/le nyilakkal lehet léptetni.
- **Nincs hatástalan gomb:** a profil átnevezése/törlése csak akkor látszik, ha van aktív profil; az utolsó EQ-sávnál a Törlés gomb láthatóan tiltott; a crossfeed profilgombok a késleltetést is átadják a motornak, és szólnak, ha a modul ki van kapcsolva.

### Az 1.4.7 újdonságai
- **Frekvenciaskála:** köztes feliratok (30, 300, 3k, 15k) és sűrűbb osztás 10–20 kHz között (12k, 14k, 15k, 16k, 18k).
- **Vízesés fejléc:** a jobb felső beállítások keskeny ablakban több sorba tördelnek, és nem takarják el a bal felső „Top-Strip 3D” feliratot; halvány sötét hátteret kaptak, így felülnézetben is olvashatók.

### Az 1.4.6 újdonságai
- **Frekvenciaskála a 3D vízesés alatt** (20 Hz – 20 kHz): a vízesés alsó éléhez igazodik, így a csúcsok frekvenciája leolvasható; a 3D és a felülnézetben látszik, oldalnézetben (ahol a frekvencia a mélységbe mutat) rejtve.

### Az 1.4.5 újdonságai
- **Javítva: „+ Új Sáv” gomb** – eddig nem csinált semmit. Az új sáv 0 dB-lel a legnagyobb üres frekvenciatartományba kerül (legfeljebb 32 sáv); dupla kattintással a görbén a kattintás helyére is tehetsz új sávot.
- **Javítva: EQ-görbe kirajzolása magas polcszűrőnél (high shelf)** – a felület hibás képlettel számolta, ezért a teljes kék görbe több tíz dB-lel lejjebb, gyakran a képen kívül látszott (a hangot nem érintette, a motor helyesen számolt). Most a görbe pontosan követi a sávok mozgatását.

### Az 1.4.4 újdonságai
- **Automatikus preamp:** induláskor 0 dB. Ha az EQ-görbe klippelne, a preamp magától a görbe csúcsához igazodik (visszavett kiemelésnél vissza is megy, legfeljebb 0 dB-ig), és a limiter bekapcsol. Importnál a fájlban megadott preamp pontosan megmarad; ha a fájlban nincs preamp, az automatika állítja be. A kézzel beírt vagy profilban mentett preamp addig marad, amíg az EQ nem klippel.

### Az 1.4.3 újdonságai
- **Linux telepítő csomag a kiadások között:** `AetherAudio-Linux-<verzió>.tar.gz` (kicsomagolás után `./install.sh`). Minden kiadás előtt a GitHub lefordítja és teszteli a Linuxos motort és a saját ablakos felületet.
- Előkészítve a Windows kódaláírás (SignPath Foundation); az aláírt kiadások a jóváhagyás után érkeznek.

### Az 1.4.2 újdonságai
- **Windows: saját ablak:** a felület saját WebView2 adatmappát használ (`%LOCALAPPDATA%\AetherAudio\WebView2`), és hiba esetén friss mappával újra megpróbálja. Így nem esik vissza böngészőre, ha a régi adatmappa nem írható. Ha a WebView2 mégsem indul, az Edge alkalmazásablaka nyílik meg (fülek és címsor nélkül), és az ok a `%LOCALAPPDATA%\AetherAudio\gui.log` fájlba kerül.
- **Linux:** ha a WebKitGTK ablak nem érhető el, Chromium alapú böngésző alkalmazásablaka nyílik; az `install.sh` kiírja a felület fordítási hibáját.

### Az 1.4.1 újdonságai
- **Limiter automatikusan:** ha egy importált (AutoEq, kézi) vagy betöltött EQ profilban negatív a preamp, a true peak limiter magától bekapcsol. Importkor a plafon -1,0 dBTP, a túlmintavétel 4x lesz. A preamp továbbra is pontosan a profilban megadott érték marad.

### Az 1.4.0 újdonságai
- **EQ profilok:** AutoEq importkor és kézi beállításkor automatikusan profil készül; az aktív profil neve a fejlécben látszik, a korábbi profilok újraindítás után is választhatók, átnevezhetők, törölhetők. Az importált AutoEq profil érintetlen marad, a kézi módosítás új profilba kerül.
- **Javítva:** az AutoEq import néha „nincs érvényes EQ sáv” hibát adott; betöltéskor az EQ most bekapcsol.
- **Javítva:** zenelejátszás közben a beállítások néha visszaugrottak egy korábbi állapotra (a felület és a motor közötti kapcsolat megszakadásakor). Újracsatlakozáskor a felület beállításai maradnak érvényben.
- Diszkrétebb előcsengés-jelzés FIR módban; nem blokkoló értesítések.
- Biztonság: a motort csak a saját felület vezérelheti; Linuxon biztonságos kimeneti eszközválasztás.
- **Támogatás:** az AetherAudio donateware – a fejléc „♥ Támogatás” gombjával önkéntesen támogathatod a fejlesztést (Revolut: @gergoqhg6).

### Windows 11
- **AetherAudio-Setup-*.exe**: telepítő (motor, Equalizer APO plugin, felület saját ablakban). Ha nincs Equalizer APO, felajánlja a letöltését; hangfeldolgozás csak vele működik, nélküle a program csak mér és megjelenít.
- A telepítő nincs digitálisan aláírva: a SmartScreen első indításkor figyelmeztethet (*További információ → Futtatás mindenképp*). Az ellenőrzőösszeg a `.sha256` fájlban.

### Linux (PipeWire)
- **AetherAudio-Linux-*.tar.gz**: kicsomagolás után `./install.sh` (Ubuntu/Debian, Fedora, Arch, openSUSE). Létrehoz egy „AetherAudio” kimeneti hangeszközt; a motor bejelentkezéskor magától indul, a felület saját ablakban (WebKitGTK) nyílik. Frissítéskor az új csomaggal futtasd újra az `./install.sh`-t.

Licenc: GNU GPL-3.0
