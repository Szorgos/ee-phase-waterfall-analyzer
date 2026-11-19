/**
 * A jelentés (format 1) szigorú ellenőrzése. A formátum az include/error_report.hpp build() kimenete:
 *   {"format":1,"kind":"…","symptom":"…","created_unix":N,"state":{…}|null,"crash":"…","log":["…",…]}
 * Ismeretlen felső szintű mező, hiányzó mező, rossz típus, ismeretlen format vagy kind: elutasítva.
 * A hibaüzenet csak a mező nevét mondja, a beküldött tartalmat soha nem ismétli.
 */

export const KINDS = ["manual", "previous_abnormal", "stall"] as const;
export type Kind = (typeof KINDS)[number];

export interface ReportV1 {
  format: 1;
  kind: Kind;
  symptom: string;
  created_unix: number;
  state: Record<string, unknown> | null;
  crash: string;
  log: string[];
}

export const LIMITS = {
  symptomChars: 64,
  crashChars: 16 * 1024, // crash.txt: a kliens legfeljebb 16 KB-ot olvas
  logLines: 400, // a kliens alapból 200 sort küld
  logLineChars: 1024, // a kliens 400 karakterre vágja a sort
  stateDepth: 12,
  stateNodes: 5000,
  stateStringChars: 16 * 1024,
  stateKeyChars: 128,
} as const;

const TOP_KEYS = ["format", "kind", "symptom", "created_unix", "state", "crash", "log"];

export type Validation = { ok: true; report: ReportV1 } | { ok: false; field: string };

function isPlainObject(v: unknown): v is Record<string, unknown> {
  return v !== null && typeof v === "object" && !Array.isArray(v);
}

/** Az állapot (state.json) bejárása: mélység, elemszám, szöveghossz. Igaz, ha rendben van. */
function stateOk(v: unknown): boolean {
  let nodes = 0;
  const walk = (x: unknown, depth: number): boolean => {
    if (++nodes > LIMITS.stateNodes || depth > LIMITS.stateDepth) return false;
    if (typeof x === "string") return x.length <= LIMITS.stateStringChars;
    if (typeof x === "number") return Number.isFinite(x);
    if (typeof x === "boolean" || x === null) return true;
    if (Array.isArray(x)) return x.every((y) => walk(y, depth + 1));
    if (isPlainObject(x))
      return Object.entries(x).every(([k, y]) => k.length <= LIMITS.stateKeyChars && walk(y, depth + 1));
    return false;
  };
  return walk(v, 0);
}

export function validateReport(v: unknown): Validation {
  if (!isPlainObject(v)) return { ok: false, field: "(root)" };
  for (const k of Object.keys(v)) if (!TOP_KEYS.includes(k)) return { ok: false, field: "(unknown field)" };
  for (const k of TOP_KEYS) if (!Object.hasOwn(v, k)) return { ok: false, field: k };

  if (v.format !== 1) return { ok: false, field: "format" };
  if (typeof v.kind !== "string" || !(KINDS as readonly string[]).includes(v.kind)) return { ok: false, field: "kind" };
  if (typeof v.symptom !== "string" || v.symptom.length > LIMITS.symptomChars || !/^[A-Za-z0-9_-]*$/.test(v.symptom))
    return { ok: false, field: "symptom" };
  if (typeof v.created_unix !== "number" || !Number.isSafeInteger(v.created_unix) || v.created_unix <= 0 || v.created_unix > 0xffffffff)
    return { ok: false, field: "created_unix" };
  if (v.state !== null && !(isPlainObject(v.state) && stateOk(v.state))) return { ok: false, field: "state" };
  if (typeof v.crash !== "string" || v.crash.length > LIMITS.crashChars) return { ok: false, field: "crash" };
  if (
    !Array.isArray(v.log) ||
    v.log.length > LIMITS.logLines ||
    !v.log.every((l) => typeof l === "string" && l.length <= LIMITS.logLineChars)
  )
    return { ok: false, field: "log" };

  return { ok: true, report: v as unknown as ReportV1 };
}

/** Rövid, kereshető oszlop az állapotból (verzió, platform, csomag), ha szöveg és rövid; különben null. */
export function stateField(state: Record<string, unknown> | null, key: string): string | null {
  const x = state?.[key];
  return typeof x === "string" && x.length <= 64 ? x : null;
}
