/**
 * Megőrzési idő: a jelentések legfeljebb RETENTION_DAYS napig (alap: 90) maradnak a D1-ben.
 * Naponta egyszer fut (Cron Trigger, wrangler.toml [triggers]), forgalomtól függetlenül; a felület és a kiadási jegyzet
 * ezt ígéri a felhasználónak ("90 napig tárolva").
 */

export const DEFAULT_RETENTION_DAYS = 90;

/** A határ: ennél korábban fogadott jelentés törlendő (unix másodperc). */
export function retentionCutoff(nowMs: number, days: number): number {
  return Math.floor(nowMs / 1000) - days * 86_400;
}

/** Törli a megőrzési időnél régebbi jelentéseket; a törölt sorok számát adja. */
export async function purgeOldReports(db: D1Database, nowMs: number, days: number): Promise<number> {
  const res = await db.prepare("DELETE FROM reports WHERE received_unix < ?1").bind(retentionCutoff(nowMs, days)).run();
  return res.meta.changes ?? 0;
}
