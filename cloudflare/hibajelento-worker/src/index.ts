/**
 * Aqulify hibajelentés-fogadó (Cloudflare Worker).
 *
 * POST /report  – egy tisztított jelentés (include/error_report.hpp, format 1) fogadása, második tisztítás, tárolás D1-ben
 * GET  /health  – él-e (D1-et nem érint)
 * Naponta (Cron Trigger): a RETENTION_DAYS napnál (alap: 90) régebbi jelentések törlése (src/retention.ts)
 *
 * Nincs CORS (a kliens nem böngésző). A naplóba csak rögzített hibakód kerül, a kérés tartalma és az IP soha.
 * A végpont szerződése: README.md.
 */

import { checkRate } from "./ratelimit";
import { DEFAULT_RETENTION_DAYS, purgeOldReports } from "./retention";
import { scrubValue } from "./scrub";
import { stateField, validateReport } from "./validate";

export interface Env {
  DB: D1Database;
  /** Worker-titok (wrangler secret put RATE_SALT). Nélküle a végpont nem fogad jelentést. */
  RATE_SALT?: string;
  RATE_PER_IP_PER_HOUR?: string;
  RATE_GLOBAL_PER_HOUR?: string;
  MAX_BODY_BYTES?: string;
  /** A jelentések megőrzési ideje napokban (alap: 90). */
  RETENTION_DAYS?: string;
}

const DEFAULTS = { perIp: 10, global: 1000, maxBytes: 64 * 1024 };

function num(v: string | undefined, def: number): number {
  const n = Number(v);
  return Number.isSafeInteger(n) && n > 0 ? n : def;
}

function reply(status: number, body: Record<string, unknown>, extra: Record<string, string> = {}): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      "Content-Type": "application/json; charset=utf-8",
      "Cache-Control": "no-store",
      "X-Content-Type-Options": "nosniff",
      ...extra,
    },
  });
}

const fail = (status: number, error: string, extra: Record<string, unknown> = {}, headers: Record<string, string> = {}) =>
  reply(status, { ok: false, error, ...extra }, headers);

/** A törzs beolvasása legfeljebb max bájtig; null, ha hosszabb. */
async function readCapped(req: Request, max: number): Promise<Uint8Array | null> {
  if (!req.body) return new Uint8Array(0);
  const reader = req.body.getReader();
  const chunks: Uint8Array[] = [];
  let total = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    total += value.byteLength;
    if (total > max) {
      await reader.cancel();
      return null;
    }
    chunks.push(value);
  }
  const out = new Uint8Array(total);
  let off = 0;
  for (const c of chunks) {
    out.set(c, off);
    off += c.byteLength;
  }
  return out;
}

async function handleReport(req: Request, env: Env): Promise<Response> {
  if (!env.RATE_SALT || env.RATE_SALT.length < 16) {
    console.error("hibajelento: not_configured");
    return fail(503, "not_configured");
  }
  const maxBytes = num(env.MAX_BODY_BYTES, DEFAULTS.maxBytes);

  const ct = (req.headers.get("Content-Type") ?? "").toLowerCase();
  if (!/^application\/json\s*(;|$)/.test(ct)) return fail(415, "unsupported_media_type");
  const enc = (req.headers.get("Content-Encoding") ?? "identity").toLowerCase();
  if (enc !== "identity") return fail(415, "unsupported_encoding");
  const declared = req.headers.get("Content-Length");
  if (declared !== null && Number(declared) > maxBytes) return fail(413, "too_large", { max_bytes: maxBytes });

  // Gyakoriságkorlát a törzs feldolgozása előtt (a hibás kérések is számítanak)
  const ip = req.headers.get("CF-Connecting-IP") ?? "unknown";
  let rate;
  try {
    rate = await checkRate(
      env.DB,
      env.RATE_SALT,
      ip,
      { perIp: num(env.RATE_PER_IP_PER_HOUR, DEFAULTS.perIp), global: num(env.RATE_GLOBAL_PER_HOUR, DEFAULTS.global) },
      Date.now(),
    );
  } catch {
    console.error("hibajelento: rate_store_error");
    return fail(500, "storage");
  }
  if (!rate.ok) return fail(429, "rate_limited", {}, { "Retry-After": String(rate.retryAfter) });

  const bytes = await readCapped(req, maxBytes);
  if (bytes === null) return fail(413, "too_large", { max_bytes: maxBytes });
  let parsed: unknown;
  try {
    parsed = JSON.parse(new TextDecoder("utf-8", { fatal: true, ignoreBOM: false }).decode(bytes));
  } catch {
    return fail(400, "bad_json");
  }
  const v = validateReport(parsed);
  if (!v.ok) return fail(400, "bad_schema", { field: v.field });

  const stats = { changes: 0 };
  const clean = scrubValue(v.report, stats) as typeof v.report;
  const body = JSON.stringify(clean);
  try {
    const row = await env.DB.prepare(
      "INSERT INTO reports (received_unix, format, kind, symptom, version, platform, package, server_scrub_changes, body) " +
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9) RETURNING id",
    )
      .bind(
        Math.floor(Date.now() / 1000),
        clean.format,
        clean.kind,
        clean.symptom,
        stateField(clean.state, "version"),
        stateField(clean.state, "platform"),
        stateField(clean.state, "package"),
        stats.changes,
        body,
      )
      .first<{ id: number }>();
    return reply(201, { ok: true, id: row?.id ?? null });
  } catch {
    console.error("hibajelento: d1_error");
    return fail(500, "storage");
  }
}

export default {
  async fetch(req: Request, env: Env): Promise<Response> {
    const { pathname } = new URL(req.url);
    if (pathname === "/report") {
      if (req.method !== "POST") return fail(405, "method_not_allowed", {}, { Allow: "POST" });
      return handleReport(req, env);
    }
    if (pathname === "/health") {
      if (req.method !== "GET") return fail(405, "method_not_allowed", {}, { Allow: "GET" });
      return reply(200, { ok: true, format: [1] });
    }
    return fail(404, "not_found");
  },

  async scheduled(controller: ScheduledController, env: Env, ctx: ExecutionContext): Promise<void> {
    const days = num(env.RETENTION_DAYS, DEFAULT_RETENTION_DAYS);
    ctx.waitUntil(
      purgeOldReports(env.DB, controller.scheduledTime, days).catch(() => {
        console.error("hibajelento: retention_error");
      }),
    );
  },
} satisfies ExportedHandler<Env>;
