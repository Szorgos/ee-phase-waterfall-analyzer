### Újdonságok (1.7.2)
- **Bass Enhancer:** a felület is az EasyEffectsét követi: mérő mutatja a keltett felharmonikusok szintjét, a Blend egész lépésekben állítható (−10: 3., +10: 2. felharmonikus). A hangzás és az alapértékek nem változtak, ezek eddig is az EasyEffectséi voltak.

### Javítások (1.7.2)
- **Crossfeed:** az előbeállítások közötti váltás nem kapcsolja be a modult; kikapcsolt modulnál a BE/KI gomb röviden felvillan.

### Újdonságok (1.7.1)
- **macOS (új):** Aqulify.app egy alkalmazásként Apple Silicon és Intel Macre (macOS 12-től), menüsor-ikonnal és saját ablakkal, ugyanazzal a DSP lánccal és felülettel. macOS 14.2-től a rendszerhangot illesztőprogram nélkül, a rendszerhang-rögzítési engedéllyel veszi át; régebbi rendszeren (vagy engedély nélkül) a BlackHole 2ch virtuális eszközzel működik. A kimenet a rendszer alapértelmezettje vagy a felületen választott eszköz. A fordítást és az indítást GitHub macOS gépen ellenőriztük; valódi Macen, hanggal még nem próbáltuk.

### Javítások (1.7.1)
- **Felület:** WebGL nélkül (pl. virtuális gépen vagy letiltott grafikus gyorsítással) a felület eddig betöltés közben leállt, és nem csatlakozott a motorhoz. Most működik; a 3D vízesés helyén ilyenkor üzenet látszik.
- **Windows:** AVX2 nélküli processzoron (pl. sok Pentium és Celeron, régebbi Intel és AMD) a Convolver használatakor leállt a motor, illetve az Equalizer APO-ba töltött plugin: a beépített FFTW könyvtár a teljes kódjában AVX2 utasításokat használt. Most SSE2-vel fordul, ami minden 64 bites processzoron megvan.
- **Felület ↔ motor:** egy épp nem olvasó felület (pl. háttérbe tett ablak) nem akasztja meg a motor kapcsolatait, és nem vesznek el a beállítások; egyszerre több felület is nyitva lehet, és az egyikben tett változtatás a többiben is megjelenik.

### Újdonságok (1.7.0)
- **Magic Touch:** új, automatikus és visszafogott hangszépítő modul a lánc elején, Linuxon és Windowson ugyanazzal a kóddal. Elemzi a lejátszott felvételt, és csak azt igazítja, ami hiányzik vagy zavaró; a kimenet hangossága megegyezik a bemenetével, rendben lévő felvételen a jel bitre érintetlen, 0 %-on a modul kimarad a láncból.
  - *Karakter:* Természetes, Meleg, Fényes, Tágas. *Tartalom:* Automatikus (felismeréssel), Zene, Beszéd, Archív felvétel.
  - *Helyreállítás:* az 50/60 Hz-es zümmögés eltávolítása, kattanásjavítás, zajcsökkentés.
  - Intenzitás csúszka, hangerőre kiegyenlített A/B összehasonlítás, részletes diagnosztika.
- **Új megjelenés:** sötét, „víz és fény” arculat türkiz kiemelőszínnel; beágyazott Inter és IBM Plex Mono betűtípus (hálózat nélkül is); egységes vonalas ikonok az emojik helyett; színesebb vízesés-skála a valódi zene szintjeihez igazítva (mélykék → ibolya → kék → türkiz → zöld → sárga → narancs → korall a csúcsokon); kitöltött csúszkák, új állapotjelvények és modulpontok. Linuxon a címsor, Windows 11-en a címsor színe is a felülethez igazodik, és a felület háttere már betöltés előtt is sötét.
- **Magyar és angol nyelv mindenhol:** a felület (nyelvválasztó a fejlécben, alapból a rendszer nyelve), Windowson a tálcamenü, az üzenetablakok, a telepítő és az eltávolító, Linuxon az `install.sh` / `uninstall.sh`, a menübejegyzés és a PipeWire kimenet neve.
- **3D vízesés:** a frekvenciaskála 0 Hz-től indul (eddig 10 Hz), a mély sávok saját, kb. 3 Hz felbontású elemzésből jönnek, így a „min” jel a zene valódi legmélyebb hangját mutatja, nem ragad a skála aljára; a háló első éle a skálán ül, és a skála a háló két széléig ér; az első sor alatt a sor színeit folytató függöny. Folyásirány: lefelé vagy felfelé, váltáskor a háló összeomlik és az új irányban újraépül. Trapézkorrekció gombokkal, mint a kivetítőnél. Felülnézetben a háló kitölti a sávot (spektrogram).
- **2D EQ grafikon:** függőleges nagyítás (±1 … ±48 dB, folyamatosan Ctrl+Shift+görgővel a kurzor magasságánál) és eltolás (húzás, Alt+görgő); az „Igazít” gomb egy kattintással mindent láthatóvá tesz, és jelzi, ha a görbe vagy egy sáv kilóg; a nézeten kívüli sáv a szélen nyílként látszik.

### Javítások (1.7.0)
- **Linux felület:** bezáráskor nem omlik össze a WebKit tartalomfolyamata (nincs core dump), és nem akad meg; a vízesés követi az ablak átméretezését, rejtett ablakban nem rajzol.
- **Windows:** a Feladatkezelőben helyes a felület neve (eddig „Aqulify felÃ¼let”).
- **EQ profilok:** az EQ mód váltása (IIR / FIR / FFT / SPM) nem hoz létre új profilt (eddig AutoEq profilnál „– módosított”, profil nélkül „Saját profil” készült); a választott módot az aktív profil megjegyzi.
- **Crossfeed:** előbeállítás választásakor a modul be is kapcsol (eddig csak értesítés kérte); a kiválasztott előbeállítás jelölése nem hasonlít a bekapcsolt állapotra. Kikapcsolt modul beállításakor a modul BE/KI gombja röviden felvillan.
- **EQ grafikon:** nagyításkor a késleltetés-görbe a rajzterületen belül marad, a nézeten kívül futó EQ-görbe a szélen szaggatott vonallal látszik, a nagyítás-felirat a görbék fölött is olvasható.

### Előkészítve (1.7.0)
- **Raspberry Pi (64 bites ARM):** a motor és a tesztek keresztfordítással lefordulnak (`linux/toolchain-aarch64-linux-gnu.cmake`), a tesztek QEMU alatt sikeresek; az így talált Convolver-hiba (100 %-os IR-szélességnél nem volt pontosan érintetlen) javítva. Valódi Pi-n még nem futott.

### Javítások (1.6.3)
- **Felület ↔ motor:** megszűnt a felület és a motor közötti folyamatos üzenetváltás (tétlen felületnél is kb. 23 EQ-üzenet másodpercenként). Ennek következményei is megszűntek: a kézi módosítás ismét mentődik az aktív profilba, AutoEq-profil módosításakor létrejön a „– módosított” profil, a motor pedig nyitott felület mellett is elmenti a beállításokat (`params.bin`).
- **Automatikus preamp:** az új sáverősítés és a hozzá tartozó preamp egyszerre lép életbe, profilváltáskor is (nincs rövid, előerősítés nélküli kiugrás).
- **EQ Bypass:** a preamp az EQ része, kikapcsolt EQ-nál nem hat – a Bypass az eredeti, érintetlen jelet adja.
- **Kattanásmentes váltás:** modul be/ki kapcsolásakor, EQ-mód, decramp, limiter túlmintavételezés és előretekintés váltásakor rövid le- és felúsztatás; a preamp változása 5 ms alatt történik. Egy másik modul kapcsolásakor a változatlanul futó (pl. FIR módú) EQ nem némul el.
- **Sávok:** sáv nélkül nincs hatástalan szerkesztőmező, csak útmutatás; az utolsó sáv is törölhető (ekkor az EQ kikapcsol); üres profil betöltése a sávlistát is üríti; a tartományon kívül beírt érték a mező tartományára áll be.
- **Import:** a formátumfelismerés és az importálás ugyanazt a feldolgozót használja; új típusok: NO, AP, BP és BW Oct megadás; a kikapcsolt (OFF) sorokat kihagyja, és kiírja, hány sort és miért hagyott ki.
- **Kijelzés:** motor nélkül a Limiter valós (kikapcsolt) állapotát mutatja; a Convolver nem ír ki nem létező IR-t; a Bass Enhancer „Alsó határ frekvencia” csúszkája csak bekapcsolt alsó határnál látszik; a preamp jelvény a mentett értéket mutatja; a késleltetés-tengely feliratai a görbéhez igazodnak; az előcsengés-jelzés minden EQ-bekapcsolásnál frissül; a limiter szintmérő csak 0 dBFS közelében piros; a Limiter fülön a választott túlmintavételezés látszik.
- **Vízesés és skála:** a vízesés másodpercenként 60 sort lép a monitor frissítésétől függetlenül; 44,1 kHz-en sem csúszik el a frekvenciaskála; a skála szélein a 10 másodperces szélsőértékek „min” / „max” jelölést kaptak.
- **Verzió:** a motor elküldi a verziószámát; ha eltér a felületétől (pl. félbemaradt frissítés), a fejléc mindkettőt mutatja.
- **Elrendezés:** szélesebb számmezők (Q, Gain, Preamp); 1000–1200 px széles ablakban a fejléc nem csúszik össze; alacsony ablakban (pl. 1000×640) a sávszerkesztő görgetés nélkül látszik.

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
