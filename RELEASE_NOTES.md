### Újdonságok (1.4.1)
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
- Forrásból: `git clone`, majd `./install.sh` (Ubuntu/Debian, Fedora, Arch, openSUSE). Létrehoz egy „AetherAudio” kimeneti hangeszközt; a motor bejelentkezéskor magától indul.

Licenc: GNU GPL-3.0
