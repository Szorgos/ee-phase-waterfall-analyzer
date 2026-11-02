// A „most szól” sáv hangerő-csúszkájának élő tesztje: a valódi felület (Playwright) a tesztkörnyezet motorjához
// csatlakozik; a csúszka a kimeneti hangkártya (dac_a) hangerejét állítja, a némítás gomb némít, és a kívülről
// (pl. a rendszer hangerő-szabályzójával) változtatott érték megjelenik a felületen.
// Indítás: tests/live/multiout_live.sh <build> node tests/live/gui_volume_live.js
const { chromium } = require("playwright");
const { execFileSync } = require("child_process");
const path = require("path");
const here = __dirname;
let fails = 0;
const check = (ok, msg) => { console.log((ok ? "  [OK]   " : "  [HIBA] ") + msg); if (!ok) fails++; };
const pactl = (...a) => execFileSync("pactl", a, { encoding: "utf8", env: Object.assign({}, process.env, { LC_ALL: "C" }) });
const volPct = () => { const m = /(\d+)%/.exec(pactl("get-sink-volume", "dac_a")); return m ? +m[1] : -1; };
const muted = () => /yes/.test(pactl("get-sink-mute", "dac_a"));
const sleep = ms => new Promise(r => setTimeout(r, ms));

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 1600, height: 1000 } });
  const errors = [];
  page.on("pageerror", e => errors.push(e.message));
  await page.addInitScript(() => { try { localStorage.setItem("aqulify_lang", "hu"); } catch (e) {} });
  await page.goto("file://" + path.resolve(here, "../../gui_integrated_system.html"));
  console.log("Hangerő a „most szól” sávban");
  await page.waitForSelector(".aqnp-vol:not([hidden])", { timeout: 15000 });
  check(true, "a hangerő-csúszka látszik (a motor küldi a hangkártya hangerejét)");

  await page.evaluate(() => { const r = document.querySelector(".aqnp-volbar"); r.value = "40"; r.dispatchEvent(new Event("input")); r.dispatchEvent(new Event("change")); });
  await sleep(1000);
  check(volPct() === 40, `a csúszka 40 %-ra: a hangkártya hangereje ${volPct()} %`);
  check(/40%/.test(await page.innerText(".aqnp-volpct")), "a felület 40 %-ot mutat");

  await page.click('[data-k="mute"]');
  await sleep(800);
  check(muted(), "némítás gomb: a hangkártya némítva");
  await page.click('[data-k="mute"]');
  await sleep(800);
  check(!muted() && volPct() === 40, "újra a gomb: a némítás feloldva, a hangerő marad");

  pactl("set-sink-volume", "dac_a", "70%");
  await page.waitForFunction(() => /70%/.test(document.querySelector(".aqnp-volpct").innerText), null, { timeout: 6000 });
  check(true, "kívülről 70 %-ra állítva: a csúszka követi");
  await page.screenshot({ path: path.join(process.env.AQ_SHOTS || process.env.AQ_WORK || here, "gui_volume.png") });

  check(errors.length === 0, "nincs JavaScript-hiba" + (errors.length ? ": " + errors.join(" | ") : ""));
  await browser.close();
  console.log(fails ? `${fails} HIBA` : "SIKERES");
  process.exit(fails ? 1 : 0);
})().catch(e => { console.error(e); process.exit(1); });
