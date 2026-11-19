// A Worker végpontjainak tesztje a Workers-futtatókörnyezetben (Miniflare), valódi helyi D1-gyel.
// A kéréseket a tényleges fetch-kezelő kapja (exports.default.fetch), a választ és a D1 tartalmát ellenőrizzük.
import { env, exports } from "cloudflare:workers";
import { describe, expect, it } from "vitest";
import { checkRate } from "../src/ratelimit";

let ipCounter = 0;
/** Minden teszt saját IP-ről küld, hogy a gyakoriságkorlát ne keveredjen. */
const freshIp = () => `203.0.113.${++ipCounter}`;

function validReport(over: Record<string, unknown> = {}) {
  return {
    format: 1,
    kind: "manual",
    symptom: "noSound",
    created_unix: 1795100000,
    state: {
      format: 1,
      version: "2.6.1",
      platform: "linux",
      package: "deb",
      pid: 4242,
      clean_exit: false,
      reports_enabled: true,
      heartbeats: [{ thread: "bridge", label: "pullAudio", age_ms: 12 }],
      stalls: [],
      process: { cpu_pct: 1.5, rss_kb: 1000 },
      sections: { engine: { path: "natív DSD, DSD256", device: "FiiO BTR7" } },
    },
    crash: "",
    log: ["===== Aqulify motor indul: 2026-11-19 10:00:00 =====", "[Lejátszó] Szól: <path>"],
    ...over,
  };
}

function post(body: string | Uint8Array, opts: { ip?: string; headers?: Record<string, string> } = {}) {
  return exports.default.fetch("https://hibajelento.example/report", {
    method: "POST",
    headers: { "Content-Type": "application/json", "CF-Connecting-IP": opts.ip ?? freshIp(), ...opts.headers },
    body,
  });
}

async function rowCount(): Promise<number> {
  const r = await env.DB.prepare("SELECT COUNT(*) AS n FROM reports").first<{ n: number }>();
  return r?.n ?? -1;
}

describe("helyes jelentés", () => {
  it("201, id; a D1-ben a kétszer tisztított jelentés van, IP nélkül", async () => {
    const ip = freshIp();
    const before = await rowCount();
    const res = await post(JSON.stringify(validReport()), { ip });
    expect(res.status).toBe(201);
    const j = await res.json<{ ok: boolean; id: number }>();
    expect(j.ok).toBe(true);
    expect(typeof j.id).toBe("number");
    expect(res.headers.get("Access-Control-Allow-Origin")).toBeNull();
    expect(await rowCount()).toBe(before + 1);

    const row = await env.DB.prepare("SELECT * FROM reports WHERE id = ?1").bind(j.id).first<Record<string, unknown>>();
    expect(row).not.toBeNull();
    expect(row!.kind).toBe("manual");
    expect(row!.symptom).toBe("noSound");
    expect(row!.version).toBe("2.6.1");
    expect(row!.platform).toBe("linux");
    expect(row!.package).toBe("deb");
    expect(row!.server_scrub_changes).toBe(0); // a kliens jól tisztított
    expect(JSON.parse(row!.body as string)).toEqual(validReport());

    // Az IP sehol nincs a D1-ben (a jelentésekben és a korlát-táblában sem)
    const dump = JSON.stringify([
      (await env.DB.prepare("SELECT * FROM reports").all()).results,
      (await env.DB.prepare("SELECT * FROM rate_limit").all()).results,
    ]);
    expect(dump).not.toContain(ip);
    expect(dump).not.toContain("203.0.113.");
  });

  it("a mindhárom kind elfogadott, a state lehet null", async () => {
    for (const kind of ["manual", "previous_abnormal", "stall"]) {
      const res = await post(JSON.stringify(validReport({ kind, state: null, symptom: "" })));
      expect(res.status, kind).toBe(201);
    }
  });

  it("GET /health 200, ismeretlen út 404, GET /report 405", async () => {
    expect((await exports.default.fetch("https://h.example/health")).status).toBe(200);
    expect((await exports.default.fetch("https://h.example/egyeb")).status).toBe(404);
    const r = await exports.default.fetch("https://h.example/report");
    expect(r.status).toBe(405);
    expect(r.headers.get("Allow")).toBe("POST");
  });
});

describe("személyes adat a bemenetben (a kliens nem tisztított)", () => {
  it("a szerver kitörli: útvonal, e-mail, IP, MAC – a D1-ben nincs nyoma, a számláló jelzi", async () => {
    const leaky = validReport({
      kind: "stall",
      crash: "SIGSEGV a /home/anna/forras/aqulify/motor.cpp fájlban",
      log: [
        "[Hálózat] kliens 192.168.1.23 csatlakozott",
        "[Bluetooth] 00:1A:7D:DA:71:13 párosítva",
        "[Fiók] anna.kovacs@example.hu",
        "[Windows] C:\\Users\\anna\\Music\\a.flac",
        "[Rendszer] Linux 6.6.87.2-microsoft-standard-WSL2",
      ],
      state: {
        version: "2.6.1",
        sections: { engine: { lib: "/home/anna/Zene", ip: "2001:db8:85a3::8a2e:370:7334", device: "FiiO BTR7" } },
      },
    });
    const res = await post(JSON.stringify(leaky));
    expect(res.status).toBe(201);
    const { id } = await res.json<{ id: number }>();
    const row = await env.DB.prepare("SELECT body, server_scrub_changes FROM reports WHERE id = ?1")
      .bind(id)
      .first<{ body: string; server_scrub_changes: number }>();
    const body = row!.body;
    for (const bad of ["/home/anna", "anna.kovacs", "example.hu", "192.168.1.23", "00:1A:7D", "C:\\\\Users", "2001:db8"])
      expect(body, bad).not.toContain(bad);
    for (const keep of ["FiiO BTR7", "6.6.87.2-microsoft-standard-WSL2", "SIGSEGV"]) expect(body, keep).toContain(keep);
    expect(row!.server_scrub_changes).toBe(7); // crash, 4 naplósor, lib, ip (a kernelverziós sor nem változik)
    const parsed = JSON.parse(body);
    expect(parsed.log[0]).toBe("[Hálózat] kliens <ip> csatlakozott");
    expect(parsed.state.sections.engine.lib).toBe("<path>");
  });
});

describe("rossz méret", () => {
  it("64 KB fölött 413 (bevallott Content-Length nélkül is), és nem tárol", async () => {
    const before = await rowCount();
    const big = JSON.stringify(validReport({ log: Array.from({ length: 70 }, () => "x".repeat(1000)) }));
    expect(new TextEncoder().encode(big).byteLength).toBeGreaterThan(65536);
    // ReadableStream: a futtatókörnyezet nem tud Content-Length-et adni, a Worker a beolvasáskor vág
    const stream = new ReadableStream({
      start(c) {
        c.enqueue(new TextEncoder().encode(big));
        c.close();
      },
    });
    const r1 = await exports.default.fetch("https://h.example/report", {
      method: "POST",
      headers: { "Content-Type": "application/json", "CF-Connecting-IP": freshIp() },
      body: stream,
    });
    expect(r1.status).toBe(413);
    const r2 = await post(big);
    expect(r2.status).toBe(413);
    expect(await r2.json()).toEqual({ ok: false, error: "too_large", max_bytes: 65536 });
    expect(await rowCount()).toBe(before);
  });
  it("épp a korlát alatt elfogadja", async () => {
    let log: string[] = [];
    let body = "";
    // annyi sor, hogy 64 KB alatt maradjon
    for (let n = 1; ; ++n) {
      const next = Array.from({ length: n }, () => "y".repeat(1000));
      const b = JSON.stringify(validReport({ log: next }));
      if (new TextEncoder().encode(b).byteLength > 65536) break;
      log = next;
      body = b;
    }
    expect(log.length).toBeGreaterThan(50);
    expect((await post(body)).status).toBe(201);
  });
});

describe("rossz séma, rossz formátum", () => {
  const bad: [string, unknown, string][] = [
    ["ismeretlen format", validReport({ format: 2 }), "format"],
    ["format szövegként", validReport({ format: "1" }), "format"],
    ["ismeretlen kind", validReport({ kind: "egyeb" }), "kind"],
    ["symptom nem kulcs", validReport({ symptom: "nem szól a hang" }), "symptom"],
    ["created_unix tört", validReport({ created_unix: 1.5 }), "created_unix"],
    ["state tömb", validReport({ state: [] }), "state"],
    ["crash szám", validReport({ crash: 5 }), "crash"],
    ["log nem szöveg", validReport({ log: [1, 2] }), "log"],
    ["log túl sok sor", validReport({ log: Array.from({ length: 401 }, () => "a") }), "log"],
    ["ismeretlen mező", validReport({ email: "x@y.hu" }), "(unknown field)"],
    ["hiányzó mező", (({ crash: _c, ...rest }) => rest)(validReport()), "crash"],
    ["nem objektum", [1, 2, 3], "(root)"],
    ["túl mély state", validReport({ state: JSON.parse('{"a":'.repeat(20) + "1" + "}".repeat(20)) }), "state"],
  ];
  for (const [name, body, field] of bad) {
    it(`${name} → 400 bad_schema (${field}), és nem tárol`, async () => {
      const before = await rowCount();
      const res = await post(JSON.stringify(body));
      expect(res.status).toBe(400);
      expect(await res.json()).toEqual({ ok: false, error: "bad_schema", field });
      expect(await rowCount()).toBe(before);
    });
  }
  it("a hibaválasz nem ismétli a beküldött tartalmat", async () => {
    const res = await post(JSON.stringify(validReport({ kind: "anna.kovacs@example.hu" })));
    expect(await res.text()).not.toContain("anna");
  });
  it("nem JSON → 400 bad_json; rossz UTF-8 → 400 bad_json", async () => {
    expect((await post("{nem json")).status).toBe(400);
    const r = await post(new Uint8Array([0x7b, 0x22, 0xff, 0xfe, 0x22, 0x7d]));
    expect(await r.json()).toEqual({ ok: false, error: "bad_json" });
  });
  it("rossz Content-Type → 415; tömörített törzs → 415", async () => {
    const body = JSON.stringify(validReport());
    expect((await post(body, { headers: { "Content-Type": "text/plain" } })).status).toBe(415);
    expect((await post(body, { headers: { "Content-Encoding": "gzip" } })).status).toBe(415);
    expect((await post(body, { headers: { "Content-Type": "application/json; charset=utf-8" } })).status).toBe(201);
  });
});

describe("tömeges kérés (gyakoriságkorlát)", () => {
  it("ugyanarról az IP-ről a 11. kérés 429 Retry-After-rel; másik IP-ről továbbra is megy", async () => {
    const ip = freshIp();
    const body = JSON.stringify(validReport());
    for (let i = 0; i < 10; ++i) expect((await post(body, { ip })).status, `kérés ${i + 1}`).toBe(201);
    const before = await rowCount();
    const res = await post(body, { ip });
    expect(res.status).toBe(429);
    const ra = Number(res.headers.get("Retry-After"));
    expect(ra).toBeGreaterThan(0);
    expect(ra).toBeLessThanOrEqual(3600);
    expect(await rowCount()).toBe(before);
    // a hibás kérés is számít (a korlát a feldolgozás előtt van)
    expect((await post("{nem json", { ip })).status).toBe(429);
    expect((await post(body)).status).toBe(201);
  });
  it("az összesített korlát IP-től függetlenül megfog; a régi ablak sorai törlődnek", async () => {
    const hour = 500_000;
    const t = hour * 3_600_000 + 1000;
    const cfg = { perIp: 100, global: 3 };
    const res = [];
    for (let i = 0; i < 4; ++i) res.push(await checkRate(env.DB, "so-0123456789abcdef", `198.51.100.${i}`, cfg, t));
    expect(res.map((r) => r.ok)).toEqual([true, true, true, false]);
    // két órával később a régi sorok eltűnnek
    await checkRate(env.DB, "so-0123456789abcdef", "198.51.100.9", cfg, t + 2 * 3_600_000);
    const old = await env.DB.prepare("SELECT COUNT(*) AS n FROM rate_limit WHERE w = ?1").bind(hour).first<{ n: number }>();
    expect(old!.n).toBe(0);
  });
  it("a korlát-tábla kulcsa sózott kivonat, nem az IP", async () => {
    const ip = "198.51.100.77";
    await checkRate(env.DB, "so-0123456789abcdef", ip, { perIp: 10, global: 1000 }, Date.now());
    const rows = (await env.DB.prepare("SELECT k FROM rate_limit").all<{ k: string }>()).results;
    expect(rows.some((r) => /^ip:[0-9a-f]{32}$/.test(r.k))).toBe(true);
    expect(JSON.stringify(rows)).not.toContain(ip);
  });
});

describe("megőrzési idő (90 nap)", () => {
  async function insertAt(receivedUnix: number, symptom: string): Promise<number> {
    const r = await env.DB.prepare(
      "INSERT INTO reports (received_unix, format, kind, symptom, server_scrub_changes, body) VALUES (?1, 1, 'manual', ?2, 0, '{}') RETURNING id",
    )
      .bind(receivedUnix, symptom)
      .first<{ id: number }>();
    return r!.id;
  }
  const exists = async (id: number) =>
    (await env.DB.prepare("SELECT COUNT(*) AS n FROM reports WHERE id = ?1").bind(id).first<{ n: number }>())!.n === 1;

  it("a napi ütemezett futás pontosan a 90 napnál régebbieket törli", async () => {
    const nowMs = 1_800_000_000_000;
    const now = nowMs / 1000;
    const cutoff = now - 90 * 86_400;
    const tooOld = await insertAt(cutoff - 1, "retOld");
    const atLimit = await insertAt(cutoff, "retLimit");
    const recent = await insertAt(now - 86_400, "retRecent");

    const { default: worker } = await import("../src/index");
    const pending: Promise<unknown>[] = [];
    const ctx = { waitUntil: (p: Promise<unknown>) => pending.push(p), passThroughOnException: () => {} };
    await worker.scheduled(
      { scheduledTime: nowMs, cron: "17 3 * * *", noRetry: () => {} } as unknown as ScheduledController,
      { DB: env.DB, RATE_SALT: "tesztso-0123456789abcdef", RETENTION_DAYS: "90" },
      ctx as unknown as ExecutionContext,
    );
    await Promise.all(pending);

    expect(pending.length).toBe(1);
    expect(await exists(tooOld)).toBe(false);
    expect(await exists(atLimit)).toBe(true);
    expect(await exists(recent)).toBe(true);
  });

  it("a megőrzési idő a RETENTION_DAYS-ből jön; hibás érték esetén 90 nap", async () => {
    const { purgeOldReports, retentionCutoff } = await import("../src/retention");
    const nowMs = 1_900_000_000_000;
    const now = nowMs / 1000;
    expect(retentionCutoff(nowMs, 90)).toBe(now - 90 * 86_400);
    const a = await insertAt(now - 10 * 86_400, "ret10");
    const b = await insertAt(now - 3 * 86_400, "ret3");
    expect(await purgeOldReports(env.DB, nowMs, 7)).toBeGreaterThanOrEqual(1);
    expect(await exists(a)).toBe(false);
    expect(await exists(b)).toBe(true);

    const { default: worker } = await import("../src/index");
    const c = await insertAt(now - 89 * 86_400, "ret89");
    const pending: Promise<unknown>[] = [];
    await worker.scheduled(
      { scheduledTime: nowMs, cron: "17 3 * * *", noRetry: () => {} } as unknown as ScheduledController,
      { DB: env.DB, RETENTION_DAYS: "nem szám" },
      { waitUntil: (p: Promise<unknown>) => pending.push(p) } as unknown as ExecutionContext,
    );
    await Promise.all(pending);
    expect(await exists(c)).toBe(true); // hibás beállításnál is 90 nap, nem kevesebb
  });
});

describe("beállítás nélkül", () => {
  it("RATE_SALT nélkül 503 not_configured, és nem tárol", async () => {
    const { default: worker } = await import("../src/index");
    const before = await rowCount();
    const res = await worker.fetch(
      new Request("https://h.example/report", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(validReport()),
      }),
      { DB: env.DB },
    );
    expect(res.status).toBe(503);
    expect(await rowCount()).toBe(before);
  });
});
