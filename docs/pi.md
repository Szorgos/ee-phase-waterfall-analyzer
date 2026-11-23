# Aqulify – Raspberry Pi (arm64) csomagkészítési és telepítési útmutató

Ez az útmutató a 64 bites (`arm64`) **Raspberry Pi OS** (Debian-alapú) rendszerre készített Aqulify `.deb` csomag előállítását, a meglévő rendszer (és PipeWire) mellé történő telepítését, valamint a szolgáltatás működését írja le.

---

## 1. A `.deb` csomag elkészítése forrásból (a Pi-n)

A csomagot a `tools/pi/pi_deb_build.sh` szkript állítja elő a motor forrásából, a meglévő `debian/` csomagolás (`dpkg-buildpackage -b`) használatával.

### Szükséges csomagok telepítése

A `tools/pi/pi_deb_build.sh` fejléce és a `debian/control` (`Build-Depends`) alapján:

```bash
sudo apt install --no-install-recommends build-essential cmake ninja-build pkg-config xz-utils dpkg-dev \
     fakeroot git python3 curl ca-certificates libpipewire-0.3-dev libfftw3-dev libsystemd-dev libasound2-dev \
     libgtk-3-dev libwebkit2gtk-4.1-dev
```

### A csomag felépítése

```bash
bash tools/pi/pi_deb_build.sh [a motor forrásmappája, alap: a tároló gyökere] [git-ref, alap: HEAD] [kimeneti mappa, alap: ./pi-deb]
```

- A szkript a megadott git-hivatkozás tiszta másolatából (`git archive`) dolgozik, a csomag verzióját a `CMakeLists.txt` verziójához igazítja `-1~pi1` utótaggal (pl. `aqulify_2.3.0-1~pi1_arm64.deb`), lefordítja a programot, és lefuttatja az egységteszteket (`debian/rules`).
- Az elkészült `.deb` csomag és a `build.log` az `[kimeneti mappa]` könyvtárba (alapértelmezetten `./pi-deb`) kerül.
- A beszéd-zajcsökkentés betanított RNNoise-modelljét (nincs a tárolóban) a szkript a `tools/fetch_rnnoise.sh`-val tölti le, rögzített sha256-tal, ugyanúgy, mint az asztali csomagoknál. Ha a gépen nincs `curl` / `wget`: `AQ_RNNOISE_TAR=<előre letöltött rnnoise_0.2+ds.orig.tar.xz>`. Modell nélkül csak `AQ_NO_RNNOISE=1` megadásával épül (akkor a beszéd-zajcsökkentés egyszerűbb módszerrel működik).
- Ugyanannak a verziónak az újraépítése új csomagrevízióval (hogy az `apt` frissítésnek lássa): `AQ_PI_REV=2` → `aqulify_2.3.0-2~pi1_arm64.deb`.
- **Csak emulált (QEMU) környezetben:** `AQ_EMULALT=1 bash tools/pi/pi_deb_build.sh ...` megadásával a valós idejű CPU-terhelést mérő `test_dsp` kihagyható. Valódi Pi-n ezt **ne** add meg, hogy a teszt lemérje a hardvert.

---

## 2. Telepítés a kész `.deb` csomagból

A kész csomagot a Debian csomagkezelőjével (`apt`) telepítsd, hogy a futásidejű függőségeket (`pipewire`, `hicolor-icon-theme` és a szükséges megosztott könyvtárak – `debian/control` 12. sor) automatikusan feloldja:

```bash
sudo apt install ./aqulify_2.9.3-1~pi1_arm64.deb
```

### Telepített fájlok (`debian/rules`)

- `/usr/bin/aqulify-engine` – az Aqulify DSP-motor
- `/usr/bin/aqulify-gui` – asztali grafikus felület (GTK3 + WebKitGTK 4.1)
- `/usr/bin/aqulify-play` – parancssori lejátszó
- `/usr/bin/aqulify-tune` – választható, visszavonható rendszerhangoló szkript (`tools/pi/aqulify-tune.sh`)
- `/usr/lib/systemd/user/aqulify.service` – felhasználói systemd-szolgáltatás (`linux/aqulify.service.in`)
- `/usr/lib/systemd/user-preset/80-aqulify.preset` – systemd előbeállítás (`enable aqulify.service`)
- `/usr/share/applications/io.github.szorgos.Aqulify.desktop` – asztali menübejegyzés

---

## 3. A motor mint felhasználói szolgáltatás és a felület

- **Felhasználói szolgáltatás (`aqulify.service`):** a csomag telepítésekor a `debian/postinst` lefuttatja a `systemctl --global preset aqulify.service` parancsot, így a `80-aqulify.preset` alapján a szolgáltatás minden felhasználónál engedélyezett, és a grafikus/felhasználói munkamenetbe való bejelentkezéskor a PipeWire után (`After=pipewire.service wireplumber.service`, `WantedBy=default.target`) indul.
- **Kézi indítás vagy újraindítás bejelentkezett felhasználóként** (`debian/postinst` 4. sor):
  ```bash
  systemctl --user restart aqulify
  systemctl --user status aqulify
  ```
- **Grafikus felület (HDMI-monitoron):** az alkalmazásmenüből vagy terminálból az `aqulify-gui` paranccsal indítható.

---

## 4. Telefonos távirányító és első beállítás (képernyő nélkül is)

- **Első beállítás (képernyő nélkül, SSH nélkül):** tiszta telepítés után a Pi gazdátlan módban indul, és a helyi hálózaton megjelenik az **Aqulify Remote** telefonos alkalmazásban (*„Új Aqulify eszköz – beállítás”*). A **Beállítás** gombra nyomva a Pi a csatlakoztatott hangkimeneten (DAC) felmond egy 4 jegyű kódot; a kódot az alkalmazásba beírva a telefon lesz a tulajdonos.
- **Új telefon hozzáadása:** további telefonokat a már párosított tulajdonos-telefonon megjelenő jóváhagyó ablakban (Igen/Nem) lehet engedélyezni, valamint ugyanott kezelhető az eszközlista és a távoli admin jogosultság.
- **Elveszett vagy lecserélt telefon:** ha a tulajdonos-telefon már nem érhető el, vedd ki a Raspberry Pi SD-kártyáját, és a számítógépen hozz létre a boot partíción egy üres `aqulify-reset` nevű fájlt. A következő indításkor az Aqulify törli a régi párosításokat és a jelzőfájlt, majd újra beállítható állapotba lép.
- **Hangeszköz nélküli tartalék (csak fizikai hozzáféréssel):** ha a beállítás idején nincs csatlakoztatott hangkimenet, a boot partícióra tett üres `aqulify-claim-no-audio` fájl az indítástól számított 10 percig engedélyezi az első beállítást kód nélkül (erről az alkalmazás figyelmeztet).
- **Asztali felületen (HDMI-monitorral):** az `aqulify-gui` ablakában a **Távirányító** gombbal kezelhetők a párosított eszközök.
- *(Fejlesztői / hibakereső opció: helyi terminálból az `aqulify-engine --pair` parancs továbbra is elérhető.)*
- **Hálózati elérés:** Linuxon és Raspberry Pi OS-en a telefonos távirányító és a Pi között helyi Wi-Fi / Ethernet hálózat használható.
- **Bluetooth-tartalék (Wi-Fi nélkül is vezérelhető):** ha a hálózati elérés be van kapcsolva, a motor a BlueZ-nál (D-Bus rendszerbusz) regisztrál egy RFCOMM-szolgáltatást (UUID: `43ad541b-d816-443a-a652-146cb5191139`, RFCOMM-csatorna: 22). A telefon ugyanazzal a tokennel hitelesít, mint a Wi-Fi-n; a Bluetooth nem párosít alkalmazásszinten (a 4 jegyű kódos párosítás és a tulajdonos/admin jogosultság változatlan). Feltételek:
  - A telefont egyszer **rendszerszinten** párosítani kell a Pi-vel (pl. `bluetoothctl`: `scan on`, `pair <cím>`, `trust <cím>`). Az Aqulify nem regisztrál Bluetooth-párosító ügynököt, ezért fej nélküli gépen ezt a lépést a rendszerben kell elvégezni.
  - Fusson a `bluetooth.service`, és legyen Bluetooth-adapter (`bluetoothctl list`).
  - Az Aqulify-t futtató felhasználónak el kell érnie a BlueZ-t a rendszerbuszon (Raspberry Pi OS-en az alapértelmezett felhasználó a `bluetooth` csoport tagja; egyéb esetben: `sudo usermod -aG bluetooth <felhasználó>`).
  - A naplóban („Bluetooth-csatorna bekapcsolva” vagy „nem érhető el ezen a gépen (ok)”) látszik, miért nem indult el. A zene átküldéséhez továbbra is Wi-Fi kell.

---

## 5. Választható, visszavonható rendszerhangolás (`aqulify-tune`) / Optional, reversible system tuning (`aqulify-tune`)

**Magyar:** A csomag telepíti a `/usr/bin/aqulify-tune` szkriptet, de a telepítés **nem** kapcsolja be magától. A `sudo aqulify-tune enable` parancs elmenti az eredeti rendszerértékeket (`/var/lib/aqulify-tune/state`), a CPU-governort `performance` üzemmódra állítja (ha elérhető), a hang osztályú (`bInterfaceClass = 01`) USB-eszközökön (`/sys/bus/usb/devices`) kikapcsolja az energiatakarékosságot (`power/control = on`), létrehozza a `/etc/security/limits.d/95-aqulify-audio.conf` fájlt (`@audio - rtprio 95`, `@audio - memlock unlimited`), és beállítja az `aqulify-tune.service` (`oneshot`) szolgáltatást az újraindítás utáni megőrzéshez. A jelenlegi állapot az `aqulify-tune status` paranccsal kérdezhető le. A hangolás bármikor maradéktalanul visszavonható a `sudo aqulify-tune disable` paranccsal (vagy a csomag eltávolításakor a `DEBIAN/prerm` automatikusan visszaállítja az eredeti állapotot).

**English:** The package installs `/usr/bin/aqulify-tune`, which remains **off by default**. Running `sudo aqulify-tune enable` saves the original system settings (`/var/lib/aqulify-tune/state`), sets the CPU scaling governor to `performance` (where supported), disables USB autosuspend (`power/control = on`) for USB Audio Class (`bInterfaceClass = 01`) devices under `/sys/bus/usb/devices`, creates `/etc/security/limits.d/95-aqulify-audio.conf` (`@audio - rtprio 95`, `@audio - memlock unlimited`), and installs `aqulify-tune.service` (`oneshot`) so the settings persist across reboots. Use `aqulify-tune status` to inspect current values. All changes are completely reversible at any time via `sudo aqulify-tune disable` (and are automatically reverted by `DEBIAN/prerm` if the package is removed).

---

## 6. Zene NAS-ról (hálózati megosztás csatolása)

A hálózati meghajtókon (NAS) tárolt zenetár eléréséhez a megosztást a gazdarendszer szintjén kell felcsatolni (az Aqulify biztonsági okokból nem kér be és nem tárol NAS-jelszavakat). Raspberry Pi OS alatt javasolt a megosztást a `/etc/fstab` fájlban beállítani az automatikus csatoláshoz (`systemd.automount`).

### CIFS / SMB megosztás felcsatolása

1. Telepítsd a CIFS klienscsomagot:
   ```bash
   sudo apt install --no-install-recommends cifs-utils
   ```
2. Hozz létre egy csatolási pontot (pl. `/mnt/zene`):
   ```bash
   sudo mkdir -p /mnt/zene
   ```
3. Készíts egy védett hitelesítő fájlt (pl. `/etc/aqulify-nas-credentials`), és állítsd be a szigorú hozzáférési jogosultságot (`0600`):
   ```bash
   sudo sh -c 'cat > /etc/aqulify-nas-credentials << EOF
   username=nasfelhasznalo
   password=nasjelszo
   EOF'
   sudo chmod 0600 /etc/aqulify-nas-credentials
   ```
4. Add hozzá az alábbi sort a `/etc/fstab` fájlhoz:
   ```text
   //192.0.2.10/zene /mnt/zene cifs credentials=/etc/aqulify-nas-credentials,iocharset=utf8,uid=1000,gid=1000,_netdev,nofail,x-systemd.automount 0 0
   ```
   Az opciók magyarázata:
   - `_netdev`: a rendszer megvárja a hálózat felállását a csatolás előtt.
   - `nofail`: a rendszer akkor is hiba nélkül indul, ha a NAS épp nem érhető el.
   - `x-systemd.automount`: a mappa csak akkor csatolódik ténylegesen, amikor egy folyamat (pl. az Aqulify) megnyitja.

### NFS megosztás felcsatolása

NFS v4 megosztás esetén:
1. Telepítsd az NFS segédprogramokat:
   ```bash
   sudo apt install --no-install-recommends nfs-common
   ```
2. Add hozzá a `/etc/fstab` fájlhoz:
   ```text
   192.0.2.10:/zene /mnt/zene nfs ro,_netdev,nofail,x-systemd.automount 0 0
   ```

A csatolás után a felcsatolt mappa (`/mnt/zene`) a grafikus felületen vagy a távirányító felületén a **Zenei mappák** menüpontban a „Tallózás” gombbal (vagy kézi útvonal-megadással) hozzáadható a gyökérkönyvtárakhoz.

Hivatalos rendszerdokumentáció:
- `mount.cifs(8)`: [https://manpages.debian.org/testing/cifs-utils/mount.cifs.8.en.html](https://manpages.debian.org/testing/cifs-utils/mount.cifs.8.en.html)
- `fstab(5)`: [https://manpages.debian.org/testing/mount/fstab.5.en.html](https://manpages.debian.org/testing/mount/fstab.5.en.html)
- `systemd.mount(5)`: [https://manpages.debian.org/testing/systemd/systemd.mount.5.en.html](https://manpages.debian.org/testing/systemd/systemd.mount.5.en.html)

---

## 6a. Hálózati vevő (lejátszás telefonról vagy számítógépről)

Az Aqulify hálózati vevőként is megjelenhet: a telefon vagy a számítógép hálózati hangkimenet-választójában az Aqulify neve látszik, és az odaküldött hang az Aqulify DSP-láncán (EQ, konvolver stb.) át szól. **Alapból ki van kapcsolva.**

- **Működés:** a vevő a rendszer `shairport-sync` programja (MIT licenc, külön csomag). A motor gyermekfolyamatként indítja, a hangját az „Aqulify” eszközre (`aqulify_sink`) irányítja, összeomláskor újraindítja, a motor leállásakor leállítja. A motor hangútja nem változik.
- **Telepítés:**
  ```bash
  sudo apt install shairport-sync pipewire-pulse
  sudo systemctl disable --now shairport-sync
  ```
  A második sor a csomag saját, rendszerszintű vevőjét kapcsolja ki (a Debian-csomag ezt magától elindítja; az a hangot az Aqulify megkerülésével, közvetlenül a hangkártyára küldené). Az Aqulify a saját példányát a felhasználó munkamenetében indítja. A `pipewire-pulse` a régebbi (4.3 előtti) `shairport-sync` miatt kell, amelynek még nincs közvetlen PipeWire-kimenete.
- **Bekapcsolás:** a felületen a Kimenet menü → *Hangkimenet (haladó)* → *Hálózati vevő* jelölőnégyzet; ugyanitt állítható a kijelzett név (üresen: „Aqulify”). Ha a program nincs telepítve, a felület ezt jelzi és megmutatja a telepítő parancsot.
- **Minőség (őszintén):** veszteségmentes, de kb. CD-minőségű (44,1 kHz / 16 bit). A küldő eszköz hangerője digitálisan hat, és a hangot a PipeWire a gráf mintavételére alakítja: ez az út **nem bitpontos és nem hi-res**. Bitpontos lejátszáshoz a beépített lejátszó való.
- **Hálózat:** TCP 7110, UDP 6110–6119 és az mDNS (UDP 5353, `avahi-daemon`). Tűzfal (pl. `ufw`) esetén ezeket engedélyezni kell. A saját portok miatt egy esetleg futó rendszerszintű vevővel sem ütközik.
- **Újabb protokoll-változat (több szobás szinkron):** ha a `shairport-sync` így készült, ehhez a külön `nqptp` rendszerszolgáltatás kell (UDP 319/320). Nélküle a program a klasszikus módban fut, ez egy eszközről szóló lejátszáshoz elég.
- **Napló:** `journalctl --user -u aqulify | grep "Hálózati vevő"`.
- **Más rendszerek:** lásd docs/halozati-vevo.md.

---

## 7. Eltávolítás

```bash
sudo apt remove aqulify
```

---

## 8. Nem ellenőrizve

Az alábbi pontok valódi Raspberry Pi hiányában (eddig emulált környezetben vagy virtuális eszközökkel vizsgálva) **még nincsenek teljes körűen ellenőrizve**:

- **Valódi Raspberry Pi 4 / 5 hardver:** a csomag települése és hosszú távú futása valódi Pi-n.
- **Bejelentkezés nélküli indulás (`loginctl enable-linger`) és Raspberry Pi OS Lite:** hogy a PipeWire és az `aqulify.service` felhasználói szolgáltatás bejelentkezés nélkül elindul-e és folyamatosan működik-e.
- **PipeWire és közvetlen ALSA hangút valódi Pi-n:** USB DAC-on keresztüli folyamatos nagy felbontású és DSD (DoP) lejátszás.
- **Valós idejű CPU-mérés (`test_dsp`) valódi hardveren.**
- **Grafikus felület (`aqulify-gui`) és 3D vízesés HDMI-n.**
- **Kódos párosítás SSH-n (`aqulify-engine --pair`):** valódi Pi-n SSH-n keresztül, fizikai telefonnal.
- **Bluetooth-tartalék Linuxon / Pi-n:** a protokoll és a D-Bus-réteg hamis szolgáltatással és valódi `dbus-daemon`-nal ellenőrzött, de **valódi BlueZ-zal, Bluetooth-rádióval és telefonnal nem**. Nem ismert még, hogy a BlueZ mely verzióin és a Pi OS alapértelmezett D-Bus-szabályai mellett (csoport / jogosultság) indul el hibátlanul, és hogy a telefon megtalálja-e a szolgáltatást.
- **Hálózati vevő (6a):** valódi telefonról valódi Pi-re még nem próbáltuk; az automatikus teszt hamis programmal ellenőrzi az indítást, az újraindítást és a leállítást. A generált beállítófájl kulcsait (general.port, udp_port_base, udp_port_range, dbus_service_bus, mpris_service_bus, pipewire.sink_target, pulseaudio.sink) a shairport-sync 2026-11-10-i forrásával egyeztettem: mind létezik, az értékek érvényesek. **Nem ellenőrzött:** a régebbi (3.x) kiadások pa szakasza, és hogy a disztribúciók csomagja mely kimeneti modulokkal készült.
