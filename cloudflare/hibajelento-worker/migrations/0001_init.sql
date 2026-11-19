-- Aqulify hibajelentések (privát D1-adatbázis). IP-cím nincs benne.
CREATE TABLE IF NOT EXISTS reports (
  id                   INTEGER PRIMARY KEY AUTOINCREMENT,
  received_unix        INTEGER NOT NULL,           -- a fogadás ideje (szerveróra)
  format               INTEGER NOT NULL,           -- a jelentés formátuma (1)
  kind                 TEXT    NOT NULL,           -- manual | previous_abnormal | stall
  symptom              TEXT    NOT NULL,           -- a felhasználó választotta tünet kulcsa (lehet üres)
  version              TEXT,                       -- state.version (ha van)
  platform             TEXT,                       -- state.platform
  package              TEXT,                       -- state.package
  server_scrub_changes INTEGER NOT NULL,           -- hány szöveget módosított a szerveroldali tisztítás (0 = a kliens jól tisztított)
  body                 TEXT    NOT NULL            -- a teljes, kétszer tisztított jelentés (JSON)
);
CREATE INDEX IF NOT EXISTS reports_received ON reports (received_unix);

-- Gyakoriságkorlát: a kulcs sózott, óránként változó kivonat (az IP nem kerül tárolásra); 2 óra után törlődik.
CREATE TABLE IF NOT EXISTS rate_limit (
  k TEXT PRIMARY KEY,
  w INTEGER NOT NULL,   -- az óra sorszáma (unix idő / 3600)
  n INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS rate_limit_w ON rate_limit (w);
