# Internetrádió

A beépített lejátszó internetrádiót is játszik: az adás ugyanazon a láncon szól, mint egy zenefájl (EQ, A/B,
felmintavételezés, mérők, vízesés), minden platformon (Windows, macOS, Linux, Raspberry Pi).

- Állomáslista: a felhasználó saját listája (név + cím), előre betöltött állomás nincs. Mentés: `radio.json` a
  beállítások mellett (Linux / Pi / macOS: a konfigurációs mappa, Windows: `%APPDATA%\Aqulify`).
- Felület: Zenetár panel → **Rádió** fül (új állomás, lejátszás, átnevezés, cím módosítása, sorrend, törlés).
- Formátumok: MP3, Ogg Vorbis, Ogg Opus, FLAC (natív és Ogg); ICY metaadat („most szól”), PLS és M3U lista.
  Nem támogatott (külön hibakóddal): AAC (`aac`) és HLS (`hls`).
- Puffer: indulás előtt 2 s; ha kiürül, a lejátszó csendet ad és újrapufferel (nem áll le). Legfeljebb 10 s; túlcsorduláskor
  (szünet, órajel-eltérés) a legrégebbi rész kiesik, az adás élő marad.
- Újracsatlakozás: a kapcsolat megszakadásakor magától, 1, 2, 4 … legfeljebb 30 s várakozással; 8 egymás utáni sikertelen
  próba után hiba. Az első kapcsolódás hibája azonnal látszik (nincs újrapróbálás).

## Szállítás

- Linux / Pi és macOS: a rendszer `curl` programja (argumentumtömbbel, héj nélkül; `-q`: a felhasználó `.curlrc`-je nem
  számít; proxy nincs). Ha a `curl` hiányzik, a hibakód `nocurl`.
- Windows: WinHTTP (proxy nélkül). A régi „ICY 200 OK” válaszsort a WinHTTP nem fogadja el (ilyen szerver ma ritka).

## Biztonság (SSRF-védelem)

A rádiónál `http://` is kell, ezért ez nem a `media_url.hpp` szűk https-fehérlistája, hanem annak ellentéte: bármely
**nyilvános** cím mehet, a helyi és belső címek nem.

- Tiltott cél: loopback (127/8, ::1), magánhálózat (10/8, 172.16/12, 192.168/16, 100.64/10, fc00::/7), link-local
  (169.254/16 – a felhők metaadat-címe is –, fe80::/10), multicast, fenntartott és dokumentációs tartományok, 0/8, és az
  IPv6-ba ágyazott ilyen IPv4-cím (::ffff:, 64:ff9b::, 2002::). IPv6-ból csak a 2000::/3 globális tartomány.
- A motor maga oldja fel a nevet; ha **bármelyik** feloldott cím tiltott, nem kapcsolódik. A curl a már ellenőrzött címre
  kapcsolódik (`--resolve`), így a név a kettő között nem kötődhet át (DNS-újrakötés). Windowson a WinHTTP is az ellenőrzött
  címet kapja, ahol a rendszer ezt ismeri, és a válasz után a ténylegesen használt cím is ellenőrzés alá esik: tiltott címnél
  a törzs nem olvasódik.
- Átirányítást a szállítás nem követ: a motor minden lépésnél újra ellenőriz (legfeljebb 5 lépés; a lejátszási lista is egy
  lépés). Csak `http` és `https` séma.
- Időkorlátok: kapcsolódás 10 s, a fejléc 15 s, adat nélkül legfeljebb 20 s (utána újracsatlakozás).
- Méretkorlátok: fejléc 16 KB, lejátszási lista 64 KB, cím 2048 karakter, ICY-blokk 4080 bájt, állomásnév 120 bájt,
  legfeljebb 500 állomás.
- A hálózatról (telefon) tetszőleges cím nem játszható le, csak a listában lévő állomás; a listát a saját gép vagy egy
  adminisztrátor távirányító szerkesztheti.

## Távvezérlő protokoll (WebSocket-híd)

Képesség: az állapotüzenet `caps` tömbjében `"radio"`.

| Üzenet | Ki küldheti | Válasz |
|---|---|---|
| `{"type":"radio_list"}` | bárki | `radio` |
| `{"type":"radio_play","id":"…"}` | bárki | `radio_result` |
| `{"type":"radio_stop"}` | bárki | `radio_result` |
| `{"type":"radio_add","name":"…","url":"…"}` | helyi / admin | `radio` |
| `{"type":"radio_update","id":"…","name"?:"…","url"?:"…"}` | helyi / admin | `radio` |
| `{"type":"radio_remove","id":"…"}` | helyi / admin | `radio` |
| `{"type":"radio_move","from":0,"to":2}` | helyi / admin | `radio` |

```json
{"type":"radio","rev":7,"items":[{"id":"r3f9a0c12bd","name":"Kedvenc adó","url":"https://stream.example.org/live.mp3"}]}
{"type":"radio_result","ok":false,"error":"remote"}
```

Hibakódok a `radio_result`-ban: `remote` (nem admin távirányító szerkesztene), `unknown` (nincs ilyen állomás), `name`,
`url`, `full`, `index`, `empty`, `unsupported`.

A `radio_play` aszinkron: a válasz azonnal jön, a kapcsolódás állapota az állapotüzenet `radio` mezőjében:

```json
"radio":{"rev":7,"count":3,"state":"playing","station":"r3f9a0c12bd","codec":"mp3","bitrate":128}
```

- `rev`: ha változik, a lista megváltozott (`radio_list` újra).
- `state`: `idle` | `connecting` | `buffering` | `playing` | `reconnecting` | `error`.
- `error` (hiba és újracsatlakozás esetén): `dns`, `blocked`, `connect`, `timeout`, `tls`, `http`, `aac`, `hls`, `html`,
  `format`, `decode`, `playlist`, `redirect`, `nocurl`, `formatChange`, `eof`, `empty`, `output`, `url`, `aborted`.
- `codec`, `bitrate` (kbit/s, az `icy-br` fejlécből; 0: ismeretlen), `reconnects` (ha nem 0).

A „most szól” a meglévő `media` mezőben jön: `live: true`, `title` / `artist` az ICY `StreamTitle` („Előadó - Cím”)
vagy az Ogg-megjegyzés alapján, `album` az állomás neve; `duration` 0, `can.seek` false. A lejátszás vezérlése (szünet,
leállítás) a meglévő `media` paranccsal megy; a `stop` a rádiót is leállítja.

## Tesztek

`test_radio` (CMake cél): a tiszta segédek, és egy helyi próbaszerverrel (a teszt maga indítja a 127.0.0.1-en) a valódi
szállítás: ICY metaadat, átirányítás, PLS, megszakadás és újracsatlakozás, elakadás, tiltott címek (közvetlenül, átirányítás
és lejátszási lista után is), nem támogatott formátumok, megszakítás, a FilePlayer és a vezérlő / híd üzenetei.
