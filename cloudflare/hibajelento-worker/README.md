# Hibajelentés-fogadó (Cloudflare Worker)

A program beépített hibajelentésének **fogadó oldala**: egy Cloudflare Worker, amely a tisztított jelentést
(`include/error_report.hpp`, `build()`, format 1) fogadja, ellenőrzi, még egyszer tisztítja, és egy **privát D1**
adatbázisba írja. Csak a tulajdonos látja. A programban nincs token vagy kulcs; a végpont nyilvános, a védelem a
szigorú ellenőrzés, a méret- és a gyakoriságkorlát.

**Állapot:** élesítve 2026-11-19-én (`https://aqulify-hibajelento.gergoeagle.workers.dev`), a napi megőrzési
törléssel együtt. A kliens (motor) oldali küldés a 2.7.0-ban jelenik meg.

## Mit csinál

- `POST /report`: egy jelentés fogadása.
  1. `Content-Type: application/json` kell, tömörítés (`Content-Encoding`) nem. A bevallott 64 KB fölötti méret
     azonnal `413`.
  2. **Gyakoriságkorlát** (a törzs feldolgozása előtt, tehát a hibás kérések is számítanak): IP-nként 10 kérés
     óránként, összesen 1000 óránként (a `wrangler.toml` `[vars]` részében állítható). Az IP **nem kerül tárolásra**:
     a korlát-tábla kulcsa `SHA-256(RATE_SALT | óra | IP)` első 32 jegye; a só Worker-titok, a kulcs óránként
     változik, a két óránál régebbi sorok törlődnek.
  3. A törzs beolvasása legfeljebb 64 KB-ig (a bevallott hossztól függetlenül), szigorú UTF-8, JSON.
  4. **Sémaellenőrzés** (`src/validate.ts`): pontosan a hét felső szintű mező, ismeretlen mező nem lehet;
     `format` = 1; `kind` ∈ `manual`, `previous_abnormal`, `stall`; `symptom` rövid kulcs (`[A-Za-z0-9_-]`, ≤ 64);
     `created_unix` egész; `state` objektum (mélység ≤ 12, ≤ 5000 elem) vagy `null`; `crash` ≤ 16 KB; `log` ≤ 400
     sor, soronként ≤ 1024 karakter. A hibaválasz csak a mező nevét mondja, a beküldött tartalmat nem ismétli.
  5. **Második tisztítás** (`src/scrub.ts`): minden szövegen (a kulcsokon is) ugyanazok a minták, mint a kliens
     `scrub()`-jában: fájlútvonal → `<path>`, e-mail → `<email>`, IPv4/IPv6 → `<ip>`, MAC → `<mac>`; ugyanazokkal a
     kivételekkel (helyi visszacsatolási cím, négyrészes kernelverzió). A felhasználónevet, gépnevet, eszközneveket a
     szerver nem ismeri; azokat a kliens tisztítja. A `server_scrub_changes` oszlop mutatja, hány szöveget kellett a
     szervernek módosítania (ha a kliens jól tisztít: 0).
  6. Tárolás a D1 `reports` táblájában (`migrations/0001_init.sql`), IP nélkül.
- `GET /health`: `{"ok":true,"format":[1]}` (D1-et nem érint).
- **Megőrzés** (`src/retention.ts`): naponta 03:17 UTC-kor (Cron Trigger, `wrangler.toml` `[triggers]`) törli a
  `RETENTION_DAYS` napnál (alap: 90) régebben fogadott jelentéseket, forgalomtól függetlenül. A felület és a kiadási
  jegyzet „90 napig tárolva” ígérete erre épül; ha a `RETENTION_DAYS` változik, a szövegeket is módosítani kell.
- CORS nincs (a kliens nem böngésző). A naplóba csak rögzített hibakód kerül (`hibajelento: d1_error` stb.), a
  kérés tartalma és az IP soha; a Cloudflare-naplózás (`[observability]`) ki van kapcsolva.

## A végpont szerződése (a 2./3. szakasz klienséhez)

**URL:** `https://aqulify-hibajelento.<fiók-aldomain>.workers.dev/report` (a pontos cím a `wrangler deploy` végén
látszik; saját domain is köthető). A kliensbe fordítási állandóként kerül; token, kulcs, azonosító nincs.

**Kérés:**

```
POST /report HTTP/1.1
Content-Type: application/json; charset=utf-8
(Content-Encoding nincs)

{"format":1,"kind":"manual","symptom":"noSound","created_unix":1795100000,
 "state":{…state.json…}|null,"crash":"…","log":["…","…"]}
```

- A törzs pontosan az `aq::report::build()` által adott `Report.json` (UTF-8, legfeljebb **65 536 bájt**).
- **Ha nagyobb**, a kliens a `log` legrégebbi sorait hagyja el, amíg belefér (a `build()` alapból 200 sort ad, soronként
  legfeljebb 400 karakter, és az állapotfájl is lehet nagy; 64 KB-ot ez elérheti).
- Új mező vagy új `kind` csak új `format` értékkel jöhet; ahhoz a Workert is bővíteni kell.

**Válaszok** (mindig JSON):

| Kód | Törzs | A kliens teendője |
|---|---|---|
| `201` | `{"ok":true,"id":123}` | Elküldve: a helyi jelentés törölhető; a felület „Hibajelentés elküldve” sávot mutathat. |
| `400` | `{"ok":false,"error":"bad_json"}` vagy `{"ok":false,"error":"bad_schema","field":"kind"}` | Kliens-hiba: ne küldje újra ugyanazt. |
| `413` | `{"ok":false,"error":"too_large","max_bytes":65536}` | Rövidítse a naplót, és küldje újra; ha így sem fér bele, dobja el. |
| `415` | `{"ok":false,"error":"unsupported_media_type"}` / `"unsupported_encoding"` | Kliens-hiba: ne küldje újra. |
| `429` | `{"ok":false,"error":"rate_limited"}`, fejléc: `Retry-After: <mp>` | Legkorábban `Retry-After` másodperc múlva próbálja újra. |
| `503` | `{"ok":false,"error":"not_configured"}` | A Worker nincs beállítva (nincs `RATE_SALT`): később újra. |
| `500` | `{"ok":false,"error":"storage"}` | Később újra (pl. a következő indításkor). |
| `404` / `405` | `{"ok":false,"error":"not_found"}` / `"method_not_allowed"` | Rossz URL vagy metódus: kliens-hiba. |

Hálózati hiba vagy időtúllépés: mint az `500`. Javasolt: legfeljebb néhány próbálkozás indításonként, és a jelentés
helyben marad, amíg `201` nem jön, vagy a felhasználó ki nem kapcsolja a beállítást.

## Helyi fejlesztés és tesztek

Node.js 20+ kell (a tesztek WSL-ben, Ubuntu, Node 22 alatt futottak). Az Ubuntu csomagolt npm 9.2-je hibásan oldja
fel a peer-függőségeket; ezért van a mappában `.npmrc` (`legacy-peer-deps=true`).

```bash
cd cloudflare/hibajelento-worker
npm ci
npm test               # vitest a Workers-futtatókörnyezetben (Miniflare, helyi D1)
npm run typecheck
npm run mutants        # 14 mutáns a tisztításra; mind meg kell haljon
bash tools/live_probe.sh   # élő próba: wrangler dev --local + curl (Linux/WSL)
```

Semmi nem megy a Cloudflare felé: a tesztek és az élő próba helyben futnak (`--local`, `WRANGLER_SEND_METRICS=false`).

## Telepítés (a tulajdonos végzi)

Claude nem lép be a Cloudflare-fiókba, és nem élesít. A lépések egy Node.js-t futtató gépen (Windows PowerShellben is
ugyanígy), a tároló `cloudflare/hibajelento-worker` mappájában:

1. **Függőségek:** `npm ci`
2. **Belépés:** `npx wrangler login` (a böngészőben a Cloudflare-fiókkal; ugyanaz a fiók, amelyen az
   `aqulify.pages.dev` fut).
3. **D1-adatbázis létrehozása:** `npx wrangler d1 create aqulify-hibajelentes`
   A kimenetben lévő `database_id` értékét be kell írni a `wrangler.toml`-ba (a `00000000-…` helyére), és commitolni.
4. **Táblák létrehozása az éles adatbázisban:** `npx wrangler d1 migrations apply aqulify-hibajelentes --remote`
5. **A só (titok) beállítása** – egy legalább 16 karakteres véletlen szöveg, amit sehova máshova nem kell leírni:
   - Linux/macOS: `openssl rand -hex 32 | npx wrangler secret put RATE_SALT`
   - Windows PowerShell: `-join ((1..32) | % { '{0:x2}' -f (Get-Random -Max 256) }) | npx wrangler secret put RATE_SALT`
   Só nélkül a Worker minden jelentésre `503 not_configured`-ot ad (szándékosan: sózatlan kivonatot nem tárol).
6. **Élesítés:** `npx wrangler deploy` – a végén kiírja a címet (`https://aqulify-hibajelento.<aldomain>.workers.dev`).
7. **Ellenőrzés:**
   - `curl https://…workers.dev/health` → `{"ok":true,"format":[1]}`
   - egy próbajelentés:
     `curl -X POST https://…workers.dev/report -H "Content-Type: application/json" --data '{"format":1,"kind":"manual","symptom":"","created_unix":1795100000,"state":null,"crash":"","log":["proba"]}'`
     → `{"ok":true,"id":1}`
   - a próbasor törlése: `npx wrangler d1 execute aqulify-hibajelentes --remote --command "DELETE FROM reports WHERE id = 1"`
8. A címet meg kell adni a kliens (3. szakasz) fejlesztőjének.

### A jelentések olvasása

```bash
# az utolsó 20 jelentés összefoglalója
npx wrangler d1 execute aqulify-hibajelentes --remote --command \
  "SELECT id, datetime(received_unix,'unixepoch') AS fogadva, kind, symptom, version, platform, package, server_scrub_changes FROM reports ORDER BY id DESC LIMIT 20"
# egy jelentés teljes szövege
npx wrangler d1 execute aqulify-hibajelentes --remote --json --command "SELECT body FROM reports WHERE id = 42"
# a 90 napnál régebbiek törlése (naponta magától is lefut; kézzel csak ellenőrzéshez kell)
npx wrangler d1 execute aqulify-hibajelentes --remote --command "DELETE FROM reports WHERE received_unix < unixepoch() - 90*86400"
```

Ha a `server_scrub_changes` nem 0, a kliens tisztítása valamit kihagyott: a jelentés szövegében a `<path>`, `<ip>`,
`<email>`, `<mac>` jelölők mutatják, mit kellett a szervernek kitörölnie.

### Figyelem

- `npx wrangler tail` (élő napló) a kérések metaadatait, köztük az IP-címet is mutatja: csak hibakereséshez, ne
  rendszeresen.
- Ingyenes keret: a Workers napi 100 000 kérést enged; a D1 napi írási kerete is van. Egy jelentés 1 beszúrás + a
  korlát 2 sora; az óránkénti összkorlát (1000) ezt a keret alatt tartja.
- A korlát-táblában nincs IP, de a sózott kivonat két óráig megvan. Ha a só kiszivárogna, egy IPv4-cím
  végigpróbálással visszafejthető lenne; ezért a só csak Worker-titokként létezik.
