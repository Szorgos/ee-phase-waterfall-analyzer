# Hálózati vevő (lejátszás telefonról vagy számítógépről)

Az Aqulify hálózati vevőként is megjelenhet: a telefon vagy a számítógép a hálózati hangkimenetek listájában ezen a néven látja, és az odaküldött hang az Aqulify DSP-láncán (EQ stb.) át szól. Alapból ki van kapcsolva; a Kimenet menü → *Hangkimenet (haladó)* → *Hálózati vevő* jelölőnégyzettel kapcsolható be, ugyanitt állítható a kijelzett név.

**Minőség (őszintén):** veszteségmentes, de kb. CD-minőségű (44,1 kHz / 16 bit). A küldő eszköz hangereje digitálisan hat, ezért ez az út **nem bitpontos és nem hi-res**. Bitpontos lejátszáshoz a beépített lejátszó való.

## Platformok

| Platform | Állapot |
|---|---|
| Linux, Raspberry Pi | Van. A rendszer `shairport-sync` programját (MIT licenc, külön csomag) indítja a motor. Telepítés és részletek: `docs/pi.md`, 6a szakasz. |
| Windows | **Nincs.** A kapcsoló a felületen nem jelenik meg. |
| macOS | **Az Aqulify nem tartalmaz vevőt, a kapcsoló nem jelenik meg.** Lásd alább. |

### macOS

A macOS 12 vagy újabb rendszeren, újabb (kb. 2018 utáni) Maceken a rendszernek van saját hálózati vevője (a Rendszerbeállítások Általános részében). Az az Aqulify-tól független szolgáltatás: a hangja a Mac kimenetén szól.

**Nincs ellenőrizve**, hogy az Aqulify rendszerhang-rögzítő útja (Core Audio Tap, illetve BlackHole) ennek a vevőnek a hangját is elkapja-e és EQ-zza-e. Nem állítjuk, hogy igen; mi ezt nem tudtuk kipróbálni.