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
sudo apt install ./aqulify_2.6.0-1~pi1_arm64.deb
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

## 4. Telefonos távirányító és párosítás

- **Asztali felületen (HDMI-monitorral):** az `aqulify-gui` ablakában kattints a **Távirányító** gombra, és kapcsold be a **„Hálózati elérés a telefonos távirányítóhoz”** lehetőséget.
- **Kijelző nélkül (SSH-n vagy terminálból):** a futó motor mellett kérj párosító kódot a következő paranccsal:
  ```bash
  aqulify-engine --pair
  ```
  A parancs a helyi vezérlőcsatornán csatlakozik a futó motorhoz, szükség esetén bekapcsolja a hálózati elérést, és kiírja a 6 jegyű párosító kódot, annak érvényességi idejét és a gép hálózati címeit.
- **Hálózati elérés:** Linuxon és Raspberry Pi OS-en a telefonos távirányító és a Pi között helyi Wi-Fi / Ethernet hálózat használható.

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
