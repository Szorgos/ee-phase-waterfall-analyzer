// A felület élő tesztje a több kimenethez: a valódi felület (Playwright) a tesztkörnyezet motorjához csatlakozik,
// a „Kimenetek” fülön kattintva állítja be a második hangkártyát, a szerkesztett kimenetre EQ-t tesz, és a két
// hangkártya felvételén ellenőrzi az eredményt. Indítás: tests/live/multiout_live.sh <build> node tests/live/gui_multiout_live.js
const { chromium } = require("playwright");
const { execFileSync } = require("child_process");
const path = require("path");
const here = __dirname;
let fails = 0;
const check = (ok, msg) => { console.log((ok ? "  [OK]   " : "  [HIBA] ") + msg); if (!ok) fails++; };
const levels = name => JSON.parse(execFileSync("python3", ["-I", path.join(here, "record_levels.py"), name], { encoding: "utf8" }).trim().split("\n").pop());
const fmt = l => `A 100 Hz ${l.a["100"].toFixed(1)} / 4 kHz ${l.a["4000"].toFixed(1)} | B 100 Hz ${l.b["100"].toFixed(1)} / 4 kHz ${l.b["4000"].toFixed(1)} dBFS`;

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 1600, height: 1000 } });
  const errors = [];
  page.on("pageerror", e => errors.push(e.message));
  await page.goto("file://" + path.resolve(here, "../../gui_integrated_system.html"));
  await page.waitForFunction(() => typeof moCaps !== "undefined" && moCaps === true, null, { timeout: 15000 });
  check(await page.isVisible("#tabBtn-outputs"), "a „Kimenetek” fül megjelenik (a motor jelzi a képességet)");
  check(!(await page.isVisible("#zoneBar")), "kikapcsolt több kimenetnél nincs zónaválasztó");
  await page.click("#tabBtn-outputs");
  await page.waitForTimeout(500);
  await page.click("#btnMoToggle");
  await page.waitForFunction(() => moStatus && moStatus.enabled === true, null, { timeout: 15000 });
  check(true, "bekapcsolás a fülön");
  // A zárt <select> opcióit a Playwright mindig rejtettnek látja: a jelenlétre várunk, nem a láthatóságra
  await page.waitForSelector('#moTable tr[data-k="1"] .mo-sink option[value="dac_b"]', { state: "attached", timeout: 15000 });

  // hangkártyák felirata: a motor nyelvfüggetlen fajtát (kind) és leírást (desc) küld, a felirat a választott nyelven
  // készül a Kimenet menüben és a „Kimenetek” fülön is („aq_test_speaker”: a tesztkörnyezet harmadik hangkártyája)
  const startLang = await page.evaluate(() => aqLang);
  const sinkTexts = () => page.evaluate(() => {
    const opt = sel => { const o = document.querySelector(sel); return o ? { text: o.textContent, title: o.title } : null; };
    return {
      menuSpk: opt('#selectPhysicalSink option[value="aq_test_speaker"]'),
      menuB: opt('#selectPhysicalSink option[value="dac_b"]'),
      tabSpk: opt('#moTable tr[data-k="1"] .mo-sink option[value="aq_test_speaker"]'),
      tabB: opt('#moTable tr[data-k="1"] .mo-sink option[value="dac_b"]'),
    };
  });
  await page.waitForFunction(() => !!document.querySelector('#selectPhysicalSink option[value="aq_test_speaker"]'), null, { timeout: 8000 });
  for (const [lang, word] of [["hu", "Hangszóró (Test Speaker)"], ["en", "Speaker (Test Speaker)"]]) {
    await page.selectOption("#selectLang", lang);
    await page.waitForTimeout(300);
    const s = await sinkTexts();
    check(!!s.menuSpk && s.menuSpk.text.includes(word) && !!s.tabSpk && s.tabSpk.text.includes(word),
          `hangkártya felirata (${lang}): menü „${s.menuSpk && s.menuSpk.text}”, fül „${s.tabSpk && s.tabSpk.text}”`);
    check(!!s.menuB && s.menuB.text.includes("DAC B") && s.menuB.title === "dac_b" && !!s.tabB && s.tabB.text.includes("DAC B") && s.tabB.title === "dac_b",
          `a rendszer szerinti eszköznév (node.description) a felirat, a nyers név a súgóban (${lang})`);
  }
  check(!(await page.evaluate(() => /Hangszóró|Analóg/.test(document.getElementById("selectPhysicalSink").textContent))),
        "angol felületen nincs magyar hangkártya-felirat");
  await page.selectOption("#selectLang", startLang);
  await page.waitForTimeout(300);
  await page.selectOption('#moTable tr[data-k="1"] .mo-sink', "dac_b");
  await page.waitForFunction(() => moStatus && moStatus.outputs && moStatus.outputs[1] && moStatus.outputs[1].sink === "dac_b", null, { timeout: 15000 });
  check(true, "a 2. kimenet hangkártyája a listából: DAC B");
  await page.screenshot({ path: path.join(process.env.AQ_SHOTS || process.env.AQ_WORK || here, "gui_outputs.png") });
  check(await page.isVisible("#zoneBar"), "a „Szerkesztett kimenet” választó megjelenik");

  // 2. kimenet szerkesztése: EQ +6 dB 4 kHz-en a felület saját sávszerkesztőjével
  await page.click('#zoneBtns button:nth-child(2)');
  await page.waitForTimeout(800);
  await page.screenshot({ path: path.join(process.env.AQ_SHOTS || process.env.AQ_WORK || here, "gui_zone2.png") });
  check(await page.evaluate(() => aqEditZone === 1 && document.body.classList.contains("zone-editing")), "szerkesztés: 2. kimenet (jelzés a felületen)");
  await page.evaluate(() => {
    nodes = [{ id: 1, freq: 4000, gain: 6, q: 1.0, type: 1, mode: 0, slope: 0, width: 4, color: "#6FB7FF" }];
    selectedNodeId = 1;
    setModuleActive("eq", true);
    updateEqUI();
    drawEq();
  });
  await page.waitForTimeout(1200);
  let l = levels("gui_zone2");
  // a felület a +6 dB-es sávhoz automatikus előerősítést tesz (túlvezérlés ellen): a 4 kHz 6 dB-lel a 100 Hz fölött
  const rel = l.b["4000"] - l.b["100"];
  check(Math.abs(l.a["4000"] + 12) < 0.3 && Math.abs(l.a["100"] + 12) < 0.3 && Math.abs(rel - 6) < 0.6,
        `a 2. kimenet EQ-ja csak a DAC B-n (4 kHz ${rel.toFixed(1)} dB-lel a 100 Hz fölött): ` + fmt(l));

  // vissza az 1. kimenetre: a felület az 1. kimenet (érintetlen) állapotát mutatja
  await page.click('#zoneBtns button:nth-child(1)');
  await page.waitForTimeout(1000);
  const main = await page.evaluate(() => ({ zone: aqEditZone, eq: isEqEnabled, bands: nodes.length }));
  check(main.zone === 0 && main.eq === false && main.bands === 0, `az 1. kimenet nézete: EQ ${main.eq ? "be" : "ki"}, ${main.bands} sáv`);
  await page.click('#zoneBtns button:nth-child(2)');
  await page.waitForTimeout(1000);
  const z2 = await page.evaluate(() => ({ zone: aqEditZone, eq: isEqEnabled, bands: nodes.length, f: nodes[0] && nodes[0].freq }));
  check(z2.zone === 1 && z2.eq === true && z2.bands === 1 && Math.round(z2.f) === 4000, "a 2. kimenetre visszaváltva az EQ-ja megvan (4 kHz, +6 dB)");
  await page.click('#zoneBtns button:nth-child(1)');
  await page.waitForTimeout(600);

  // keresztváltó a fülön
  await page.selectOption("#moMode", "crossover");
  await page.waitForFunction(() => moStatus && moStatus.mode === "crossover", null, { timeout: 15000 });
  await page.fill("#moXoFreqs input", "500");
  await page.dispatchEvent("#moXoFreqs input", "change");
  await page.waitForFunction(() => moStatus && moStatus.crossovers && Math.round(moStatus.crossovers[0]) === 500, null, { timeout: 15000 });
  check(!(await page.isVisible("#zoneBar")), "keresztváltóban a zónaválasztó rejtett (közös lánc)");
  await page.waitForTimeout(800);
  l = levels("gui_xo");
  check(Math.abs(l.a["100"] + 12) < 0.4 && l.a["4000"] < -55 && Math.abs(l.b["4000"] + 12) < 0.4 && l.b["100"] < -55, "keresztváltó 500 Hz a felületről: " + fmt(l));
  await page.screenshot({ path: path.join(process.env.AQ_SHOTS || process.env.AQ_WORK || here, "gui_xover.png") });

  // kikapcsolás
  await page.click("#btnMoToggle");
  await page.waitForFunction(() => moStatus && moStatus.enabled === false, null, { timeout: 15000 });
  await page.waitForTimeout(800);
  l = levels("gui_off");
  check(l.b["100"] < -80 && Math.abs(l.a["4000"] + 12) < 0.3, "kikapcsolva a DAC B néma: " + fmt(l));
  check(errors.length === 0, "nincs JavaScript-hiba" + (errors.length ? ": " + errors.join(" | ") : ""));
  await browser.close();
  console.log(fails ? `\nSIKERTELEN (${fails} hiba)` : "\nMINDEN FELÜLETI ÉLŐ TESZT SIKERES (0 hiba)");
  process.exit(fails ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });
