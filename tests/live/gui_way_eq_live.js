// Útonkénti EQ élő felületi és fordítási teszt (K15, K16) Playwrighttal.
// Indítás: tests/live/multiout_live.sh <build> node tests/live/gui_way_eq_live.js
const { chromium } = require("playwright");
const { execFileSync } = require("child_process");
const path = require("path");
const here = __dirname;
let fails = 0;
const check = (ok, msg) => { console.log((ok ? "  [OK]   " : "  [HIBA] ") + msg); if (!ok) fails++; };
const levels = name => JSON.parse(execFileSync("python3", ["-I", path.join(here, "record_levels.py"), name], { encoding: "utf8" }).trim().split("\n").pop());

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 1600, height: 1000 } });
  const errors = [];
  page.on("pageerror", e => errors.push("pageerror: " + e.message));
  page.on("console", m => { if (m.type() === "error" && !m.text().includes("ERR_FILE_NOT_FOUND")) errors.push("console.error: " + m.text()); });

  await page.goto("file://" + path.resolve(here, "../../gui_integrated_system.html"));
  await page.waitForFunction(() => typeof moCaps !== "undefined" && moCaps === true && typeof initPipewireBridge === "function", null, { timeout: 15000 });
  check(await page.evaluate(() => typeof initPipewireBridge === "function"), "initPipewireBridge definiált");

  await page.click("#tabBtn-outputs");
  await page.waitForTimeout(400);
  if (!(await page.evaluate(() => moStatus && moStatus.enabled === true))) {
    await page.click("#btnMoToggle");
    await page.waitForFunction(() => moStatus && moStatus.enabled === true, null, { timeout: 15000 });
  }
  await page.waitForSelector('#moTable tr[data-k="1"] .mo-sink option[value="dac_b"]', { state: "attached", timeout: 15000 });
  await page.selectOption('#moTable tr[data-k="1"] .mo-sink', "dac_b");
  await page.waitForFunction(() => moStatus && moStatus.outputs && moStatus.outputs[1] && moStatus.outputs[1].sink === "dac_b", null, { timeout: 15000 });

  // Zóna módban az EQ gomb rejtett
  await page.selectOption("#moMode", "zones");
  await page.waitForFunction(() => moStatus && moStatus.mode === "zones", null, { timeout: 10000 });
  const eqCountZones = await page.locator('#moTable tr[data-k="1"] button.mo-way-eq:visible').count();
  check(eqCountZones === 0, "K15: zóna módban az útonkénti EQ gomb rejtett");

  // Keresztváltó 500 Hz
  await page.selectOption("#moMode", "crossover");
  await page.waitForFunction(() => moStatus && moStatus.mode === "crossover", null, { timeout: 15000 });
  await page.fill("#moXoFreqs input", "500");
  await page.dispatchEvent("#moXoFreqs input", "change");
  await page.waitForFunction(() => moStatus && moStatus.crossovers && Math.round(moStatus.crossovers[0]) === 500, null, { timeout: 15000 });

  // EQ gomb megnyitása a 2. kimeneten
  const eqBtnSel = '#moTable tr[data-k="1"] button.mo-way-eq';
  await page.waitForSelector(eqBtnSel, { state: "visible", timeout: 8000 });
  await page.click(eqBtnSel);
  await page.waitForSelector("#wayEqModal", { state: "visible", timeout: 8000 });

  // Sáv felvétele és bekapcsolása: Bell +6 dB @ 4000 Hz, Q=1
  const toggleChecked = await page.isChecked("#wayEqToggle");
  if (!toggleChecked) await page.click("#wayEqToggle");
  await page.click("#wayEqAddBandBtn");
  await page.waitForSelector("#wayEqBands .way-band-row", { state: "visible", timeout: 5000 });
  await page.selectOption("#wayEqBands .way-band-row:first-child select.way-b-type", "bell");
  await page.fill("#wayEqBands .way-band-row:first-child input.way-b-freq", "4000");
  await page.fill("#wayEqBands .way-band-row:first-child input.way-b-gain", "6");
  await page.fill("#wayEqBands .way-band-row:first-child input.way-b-q", "1");
  await page.dispatchEvent("#wayEqBands .way-band-row:first-child input.way-b-q", "change");

  await page.waitForFunction(() => moStatus && moStatus.outputs && moStatus.outputs[1] &&
    moStatus.outputs[1].way && moStatus.outputs[1].way.on === true &&
    moStatus.outputs[1].way.bands && moStatus.outputs[1].way.bands.length === 1 &&
    Math.round(moStatus.outputs[1].way.bands[0].freq) === 4000, null, { timeout: 15000 });

  await page.waitForTimeout(800);
  const l = levels("gui_way_eq");
  console.log(`K_METRIC: {"test":"K15_audio","a_100":${l.a["100"].toFixed(2)},"a_4k":${l.a["4000"].toFixed(2)},"b_100":${l.b["100"].toFixed(2)},"b_4k":${l.b["4000"].toFixed(2)}}`);
  check(Math.abs(l.b["4000"] + 6.0) < 0.4 && l.b["100"] < -55 && Math.abs(l.a["100"] + 12.0) < 0.4 && l.a["4000"] < -60,
        `K15 élő mérés: B 4 kHz=${l.b["4000"].toFixed(2)} dBFS, B 100 Hz=${l.b["100"].toFixed(2)}, A 100 Hz=${l.a["100"].toFixed(2)}`);

  // Grafikon pontjainak egyezése a motor response adatával
  const plotChk = await page.evaluate(() => {
    const p = window.wayEqLastPlot;
    const r = moStatus && moStatus.response;
    if (!p || !r || !r.freqs || !r.ways || !r.ways[1] || !r.sum) return { ok: false, reason: "hiányzó adat" };
    if (p.wayDb.length !== r.ways[1].length || p.wayDb.length < 50) return { ok: false, reason: "hossz eltérés" };
    let maxDiff = 0;
    for (let i = 0; i < p.wayDb.length; i++) {
      maxDiff = Math.max(maxDiff, Math.abs(p.wayDb[i] - r.ways[1][i]), Math.abs(p.sumDb[i] - r.sum[i]));
    }
    return { ok: maxDiff < 0.01, maxDiff, count: p.wayDb.length };
  });
  console.log(`K_METRIC: {"test":"K15_plot","count":${plotChk.count || 0},"max_diff":${(plotChk.maxDiff ?? 999).toFixed(5)}}`);
  check(plotChk.ok, `K15 grafikon egyezik a motor response adatával (${plotChk.count} pont, max eltérés ${plotChk.maxDiff})`);

  // K16: német és japán nyelven nincs nyers way. kulcs és nincs magyar szó
  for (const lang of ["de", "ja"]) {
    await page.selectOption("#selectLang", lang);
    await page.waitForTimeout(300);
    await page.click("#wayEqHelpBtn");
    await page.waitForTimeout(200);
    const txt = await page.evaluate(() => {
      const m = document.getElementById("wayEqModal");
      const b = document.querySelector('#moTable tr[data-k="1"] button.mo-way-eq');
      return (m ? m.innerText : "") + "\n" + (b ? b.innerText : "");
    });
    const noRawKey = !/\bway\.[a-z_]+\b/.test(txt);
    const noHungarian = !/(Útonkénti|Erősítés|Mélypolc|Magaspolc|Alaphelyzet|Nincs kapcsolat|[őűŐŰ])/.test(txt);
    const hasJa = lang !== "ja" || /[\u3040-\u30ff\u4e00-\u9faf]/.test(txt);
    check(noRawKey && noHungarian && hasJa, `K16 (${lang}): nincs nyers way. kulcs és magyar szó a szerkesztőben`);
  }
  await page.selectOption("#selectLang", "hu");
  await page.waitForTimeout(200);

  // Oldal újratöltése után ugyanazt mutatja
  await page.reload();
  await page.waitForFunction(() => typeof moStatus !== "undefined" && moStatus && moStatus.enabled === true, null, { timeout: 15000 });
  await page.click("#tabBtn-outputs");
  await page.waitForSelector(eqBtnSel, { state: "visible", timeout: 8000 });
  await page.click(eqBtnSel);
  await page.waitForSelector("#wayEqModal", { state: "visible", timeout: 8000 });
  const reloadedOk = await page.evaluate(() => {
    const tgl = document.getElementById("wayEqToggle");
    const freq = document.querySelector("#wayEqBands .way-band-row input.way-b-freq");
    const gain = document.querySelector("#wayEqBands .way-band-row input.way-b-gain");
    return !!(tgl && tgl.checked && freq && Math.round(Number(freq.value)) === 4000 && gain && Math.round(Number(gain.value)) === 6);
  });
  check(reloadedOk, "K15: oldal újratöltése után a szerkesztő ugyanazt mutatja (4000 Hz, +6 dB)");
  await page.click("#wayEqCloseBtn");

  // Zónára váltás és vissza -> megmarad
  await page.selectOption("#moMode", "zones");
  await page.waitForFunction(() => moStatus && moStatus.mode === "zones", null, { timeout: 10000 });
  check((await page.locator(eqBtnSel + ":visible").count()) === 0, "K15: zónára váltáskor az EQ gomb eltűnik");
  await page.selectOption("#moMode", "crossover");
  await page.waitForFunction(() => moStatus && moStatus.mode === "crossover", null, { timeout: 10000 });
  await page.click(eqBtnSel);
  await page.waitForSelector("#wayEqModal", { state: "visible", timeout: 5000 });
  const backOk = await page.evaluate(() => {
    const tgl = document.getElementById("wayEqToggle");
    const freq = document.querySelector("#wayEqBands .way-band-row input.way-b-freq");
    return !!(tgl && tgl.checked && freq && Math.round(Number(freq.value)) === 4000);
  });
  check(backOk, "K15: zónából keresztváltóra visszaváltva a beállítás megmarad");

  check(errors.length === 0, "K15: nincs JS/konzolhiba" + (errors.length ? ": " + errors.join(" | ") : ""));
  await browser.close();
  console.log(fails ? `\nSIKERTELEN (${fails} hiba)` : "\nMINDEN FELÜLETI WAY EQ TESZT SIKERES (0 hiba)");
  process.exit(fails ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });
