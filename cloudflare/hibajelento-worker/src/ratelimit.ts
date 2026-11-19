/**
 * Gyakoriságkorlát D1-ben, óránkénti ablakkal.
 * - IP-nként: a kulcs SHA-256(RATE_SALT | óra | IP) első 32 hex jegye. Az IP maga sehol nem kerül tárolásra; a só
 *   titkos (Worker-titok), és a kulcs óránként változik, ezért két óra jelentései nem köthetők össze.
 * - Összesen (IP nélkül): a D1 ingyenes keretének védelmére.
 * A két óránál régebbi sorok minden hívásnál törlődnek.
 */

export interface RateConfig {
  perIp: number;
  global: number;
}

export type RateResult = { ok: true } | { ok: false; retryAfter: number };

async function sha256Hex(text: string): Promise<string> {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

export async function checkRate(db: D1Database, salt: string, ip: string, cfg: RateConfig, nowMs: number): Promise<RateResult> {
  const hour = Math.floor(nowMs / 3_600_000);
  const ipKey = "ip:" + (await sha256Hex(`${salt}|${hour}|${ip}`)).slice(0, 32);
  const globalKey = `global:${hour}`;
  const upsert = db.prepare(
    "INSERT INTO rate_limit (k, w, n) VALUES (?1, ?2, 1) ON CONFLICT(k) DO UPDATE SET n = n + 1 RETURNING n",
  );
  const [ipRes, globalRes] = await db.batch<{ n: number }>([
    upsert.bind(ipKey, hour),
    upsert.bind(globalKey, hour),
    db.prepare("DELETE FROM rate_limit WHERE w < ?1").bind(hour - 1),
  ]);
  const nIp = ipRes.results[0]?.n ?? Number.POSITIVE_INFINITY;
  const nGlobal = globalRes.results[0]?.n ?? Number.POSITIVE_INFINITY;
  if (nIp > cfg.perIp || nGlobal > cfg.global) {
    const retryAfter = Math.max(1, Math.ceil(((hour + 1) * 3_600_000 - nowMs) / 1000));
    return { ok: false, retryAfter };
  }
  return { ok: true };
}
