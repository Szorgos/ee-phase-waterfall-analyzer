# Hálózati vevő (lejátszás telefonról vagy számítógépről)

Az Aqulify kétféleképpen fogadhat zenét a hálózatról. Mindkettő alapból ki van kapcsolva; a Kimenet menü → *Hangkimenet (haladó)* → *Hálózati vevő* blokkban kapcsolhatók be.

| | **UPnP / DLNA lejátszó** (saját) | **Hálózati vevő** (AirPlay-szerű, `shairport-sync`) |
|---|---|---|
| Minőség | A fájl eredeti formátuma: **hi-res, bitpontos** is lehet, DSD is (DoP / natív DSD) | Kb. CD-minőség (44,1 kHz / 16 bit), nem bitpontos |
| Hangút | A beépített lejátszó, a teljes láncon (EQ, felmintavételezés, kizárólagos kimenet) | A rendszer hangútja az „Aqulify” eszközön át (EQ) |
| Vezérlés | UPnP / DLNA vezérlőalkalmazás a telefonon vagy a számítógépen | A telefon vagy a gép hálózati hangkimenet-választója |
| Linux, Raspberry Pi | **Van** | Van (külön csomag) |
| Windows | **Van** | Nincs |
| macOS | **Van** | Nincs (lásd lent) |

## UPnP / DLNA lejátszó

Az Aqulify egy szabványos **UPnP MediaRenderer:1** eszközként jelenik meg a helyi hálózaton. Bármely UPnP / DLNA vezérlőalkalmazás (telefonon vagy számítógépen) kiválaszthatja lejátszónak, és a hálózati médiaszerver (NAS, otthoni zeneszerver) zenéjét rajta játssza le. A zene **nem a telefonon át** megy: az Aqulify maga tölti le a szervertől, és a beépített lejátszó a teljes DSP-láncon át játssza le, ugyanúgy, mint egy helyi fájlt.

**Bekapcsolás:** Kimenet menü → *Hangkimenet (haladó)* → *Hálózati vevő* → **UPnP / DLNA lejátszó**. Beállítható:

- **Név a vezérlőkben** – üresen „Aqulify (gépnév)”.
- **Fix hangerő (bitpontos)** – a vezérlő hangerő-csúszkája nem hat; a vezérlő mindig 100-at lát. Kikapcsolva a vezérlő a „most szól” sáv hangerejét állítja (kizárólagos módban a lánc végén, digitálisan, ditherrel; megosztott módban a rendszer hangerejét).
- **Csak ezekről a címekről** – vesszővel elválasztott IPv4-címek; üresen bárki vezérelheti a helyi hálózatról.

Az állapotsor mutatja, milyen címen és porton fut, és melyik vezérlő küldte az utolsó parancsot.

### Mit tud

- **Formátumok:** FLAC, WAV, AIFF, ALAC (M4A / CAF), MP3, Ogg Vorbis, Opus, WavPack, DSF, DFF; élő folyamként (internetrádió a vezérlőből) MP3, AAC, Ogg, FLAC. A `GetProtocolInfo` csak ezeket hirdeti.
- **Hi-res és bitpontos:** a szám eredeti mintavétellel és bitmélységgel szól; kizárólagos kimeneten, kikapcsolt EQ-val és fix hangerővel bitpontos. DSD-fájl DoP-pal / natív DSD-ként, ha a DSD natívan be van kapcsolva; különben PCM-re alakítva.
- **Szünetmentes váltás:** ha a vezérlő megadja a következő számot (`SetNextAVTransportURI`), az Aqulify előre letölti, és azonos mintavételnél mintapontosan, szünet nélkül folytatja.
- **Tekerés:** `REL_TIME` / `ABS_TIME` (és `TRACK_NR` = 1), mintapontosan, a hálózat nélkül (a szám a memóriában van). Élő folyamban nincs tekerés (`710`).
- **„Most szól”:** a cím, előadó, album a fájl címkéiből, ha azok hiányoznak, a vezérlő által küldött metaadatból (DIDL-Lite); a felület és a telefonos távirányító ugyanazt mutatja.
- **Állapotjelzés a vezérlőnek:** GENA-események (`LastChange`), legfeljebb 5 másodpercenként.

### Hogyan működik

1. A vezérlő `SetAVTransportURI`-val megadja a szám címét a szerveren. Az Aqulify ellenőrzi a címet (lásd lent), és **azonnal letölti a memóriába** (Linuxon a RAM-ban lévő futásidejű mappába, mint az előbetöltésnél; ha ott nincs elég hely, a rendszer ideiglenes mappájába). Ha a kapcsolat közben megszakad, HTTP `Range`-dzsel ott folytatja, ahol abbamaradt.
2. `Play`-re a kész fájl a beépített lejátszóba kerül. A letöltés alatt az állapot `TRANSITIONING`.
3. A következő szám ugyanígy előre letöltődik; a lejátszó a mostani szám végén szünetmentesen vált rá. Ha a következő szám letöltése a mostani végéig nem készül el, a váltás rövid szünettel, a letöltés után történik.
4. Ha közben a felületen vagy a telefonos távirányítón más szól (zenetár, rádió), a UPnP-lejátszás `STOPPED` állapotba lép.

Egy szám legfeljebb 2 GiB lehet. A tartalom nélkül (`Content-Length` nélkül) érkező, vagy a metaadatban `audioBroadcast` osztályú folyam élő folyamként, a rádió dekóderével szól.

### Hálózati biztonság

- **Alapból kikapcsolva.** Bekapcsolva csak a magánhálózati (RFC 1918: 10/8, 172.16/12, 192.168/16) interfészeken hirdet és fogad kapcsolatot, és csak abból az alhálózatból, amelyhez az interfész tartozik. Nincs UPnP-IGD és nincs porttovábbítás.
- **A UPnP-nek nincs jelszava:** a helyi hálózaton bárki vezérelheti (ezt a felület is kimondja). A *Csak ezekről a címekről* lista ezt szűkíti.
- **Médiacím-szabály:** a rendszer a vezérlő által megadott címről tölt le, ezért szigorú:
  - magánhálózati cím csak akkor, ha **ugyanabban az alhálózatban** van, mint az interfész, amelyen a vezérlő kérése jött;
  - nyilvános cím csak `http://` vagy `https://` sémával;
  - **mindig tiltott:** loopback (127/8, `::1`, `localhost`), link-local (169.254/16 – a felhők metaadat-címe is –, `fe80::/10`), multicast, fenntartott tartományok, és a felhasználónév / jelszó az URL-ben.
  - A név feloldása után **minden** feloldott címre ugyanez érvényes, és a letöltés a már ellenőrzött címre kapcsolódik (Linuxon / macOS-en a `curl --resolve`, Windowson a WinHTTP a feloldott címre), így a névfeloldás közben nem változhat. Átirányításnál minden lépés újra ellenőrződik (legfeljebb 5).
- **Bemeneti korlátok:** SOAP-törzs legfeljebb 64 KiB, fejléc 8 KiB, legfeljebb 16 egyidejű kapcsolat, legfeljebb 30 eseményfeliratkozás lejáró időzítővel (60–1800 s). Az eseményértesítés (GENA) visszahívási címe csak `http://` IPv4-cím lehet a kérés alhálózatából. Az XML-bontó nem fogad el `DOCTYPE`-ot (nincs entitás-kiterjesztés), a mélység és az elemek száma korlátos.

### Portok

| Port | Mire |
|---|---|
| UDP 1900 (239.255.255.250) | SSDP-felfedezés (szabványos) |
| TCP 49494 (foglaltnál a következő 20 közül az első szabad) | Eszközleírás, vezérlés (SOAP), események (GENA) |

Ha a gépen tűzfal fut, ezeket a helyi hálózat felé engedélyezni kell. Az **alkalmazásonkénti tűzfalak** (pl. OpenSnitch) a motor *kimenő* forgalmát is szűrik: ha az Aqulify-motor kimenő kapcsolata tiltva van, a vezérlő nem találja meg (az SSDP-válasz nem megy ki), és nem kap állapotértesítést; ilyenkor a motort engedélyezni kell. A letöltés Linuxon a rendszer `curl` programjával megy.

### Szabványok (forrás)

A megvalósítás csak a nyilvános szabványokból készült (`include/upnp_*.hpp`): *UPnP Device Architecture 1.1* (SSDP, leírás, SOAP-vezérlés, GENA-események), *AVTransport:1*, *RenderingControl:1*, *ConnectionManager:1* (Service Template 1.01), a *ContentDirectory:1* DIDL-Lite metaadat-formátuma és a DLNA `protocolInfo`.

Megvalósított műveletek:

- **AVTransport:1** – SetAVTransportURI, SetNextAVTransportURI, Play, Pause, Stop, Seek, Next, Previous, GetTransportInfo, GetPositionInfo, GetMediaInfo, GetDeviceCapabilities, GetTransportSettings, SetPlayMode (csak `NORMAL`), GetCurrentTransportActions. Hibakódok: 701, 710, 711, 712, 714, 716, 717, 718.
- **RenderingControl:1** – ListPresets, SelectPreset (`FactoryDefaults`), GetVolume, SetVolume, GetMute, SetMute (csatorna: `Master`).
- **ConnectionManager:1** – GetProtocolInfo, GetCurrentConnectionIDs, GetCurrentConnectionInfo.

Nincs (később): OpenHome (Playlist, Time, Info, Volume) – ezzel a renderer saját lejátszási listát tartana, és a telefon kikapcsolása után is továbbjátszana. Most a lejátszási listát a vezérlő tartja: a következő számot csak akkor tudja megadni, ha fut.

### Kipróbálás

- Egységteszt: `build/test_upnp` (SOAP, DIDL-Lite, időformátum, hibakódok, `LastChange`, címszabály, SSDP-csomagok, állapotgép; ha a gépnek van magánhálózati címe, valódi letöltés és a HTTP-szolgáltatás is).
- Élő teszt a valódi motorral: `tests/live/multiout_live.sh build python3 -I tests/live/upnp_live.py` (felfedezés, SetURI / Play / Seek / SetNext / hangerő / Stop egy helyi próbaszerver fájljával). Ha a gépen már fut egy Aqulify, `AQ_ENGINE_PORT=47890` előtaggal.

## Hálózati vevő (`shairport-sync`)

A telefon vagy a számítógép a hálózati hangkimenetek listájában ezen a néven látja az Aqulify-t, és az odaküldött hang az Aqulify DSP-láncán (EQ stb.) át szól. Alapból ki van kapcsolva; a *Hálózati vevő* jelölőnégyzettel kapcsolható be, ugyanitt állítható a kijelzett név.

**Minőség (őszintén):** veszteségmentes, de kb. CD-minőségű (44,1 kHz / 16 bit). A küldő eszköz hangereje digitálisan hat, ezért ez az út **nem bitpontos és nem hi-res**. Bitpontos hálózati lejátszáshoz a UPnP / DLNA lejátszó való.

| Platform | Állapot |
|---|---|
| Linux, Raspberry Pi | Van. A rendszer `shairport-sync` programját (MIT licenc, külön csomag) indítja a motor. Telepítés és részletek: `docs/pi.md`, 6a szakasz. |
| Windows | **Nincs.** A kapcsoló a felületen nem jelenik meg (a UPnP / DLNA lejátszó van). |
| macOS | **Az Aqulify nem tartalmaz ilyen vevőt, a kapcsoló nem jelenik meg.** Lásd alább. |

### macOS

A macOS 12 vagy újabb rendszeren, újabb (kb. 2018 utáni) Maceken a rendszernek van saját hálózati vevője (a Rendszerbeállítások Általános részében). Az az Aqulify-tól független szolgáltatás: a hangja a Mac kimenetén szól.

**Nincs ellenőrizve**, hogy az Aqulify rendszerhang-rögzítő útja (Core Audio Tap, illetve BlackHole) ennek a vevőnek a hangját is elkapja-e és EQ-zza-e. Nem állítjuk, hogy igen; mi ezt nem tudtuk kipróbálni.
