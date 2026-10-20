### Újdonságok (1.4.5)
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
