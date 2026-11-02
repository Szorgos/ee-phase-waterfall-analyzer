// A felület élő tesztje a haladó hangkimenethez és a Csatornák modulhoz: a valódi felület (Playwright) a
// tesztkörnyezet motorjához csatlakozik. Indítás: tests/live/multiout_live.sh <build> node tests/live/gui_audio_live.js
const { chromium } = require("playwright");
const { execFileSync } = require("child_process");
const path = require("path");
const here = __dirname;
let fails = 0;
const check = (ok, msg) => { console.log((ok ? "  [OK]   " : "  [HIBA] ") + msg); if (!ok) fails++; };
const engineState = () => JSON.parse(execFileSync("python3", ["-I", path.join(here, "get_state.py")], { encoding: "utf8" }).trim().split("\n").pop());

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 1600, height: 1000 } });
  const errors = [];
  page.on("pageerror", e => errors.push(e.message));
  await page.addInitScript(() => { try { localStorage.setItem("aqulify_lang", "hu"); } catch (e) {} });
  await page.goto("file://" + path.resolve(here, "../../gui_integrated_system.html"));
  await page.waitForFunction(() => typeof aoCaps !== "undefined" && aoCaps === true, null, { timeout: 15000 });

  console.log("Haladó hangkimenet a Kimenet menüben");
  await page.click("#btnSinkMenu");
  check(await page.isVisible("#audioOptsWrap"), "a „Hangkimenet (haladó)” rész látszik (a motor jelzi a képességet)");
  await page.waitForFunction(() => aoOpts !== null, null, { timeout: 5000 });
  await page.selectOption("#aoBuffer", "256");
  await page.waitForFunction(() => aoOpts && aoOpts.buffer === 256, null, { timeout: 5000 });
  check(true, "pufferméret 256 minta a listából, a motor visszaigazolta");
  await page.check("#aoKeepAwake");
  await page.waitForFunction(() => aoOpts && aoOpts.keepAwake === true, null, { timeout: 5000 });
  await page.selectOption("#aoRate", "follow");
  await page.waitForFunction(() => aoOpts && aoOpts.rateMode === "follow", null, { timeout: 5000 });
  check(true, "ébren tartás és forrást követő mintavétel bekapcsolva");
  await page.waitForFunction(() => /Bitpontos/.test(document.getElementById("aoPath").innerText) && /ébren/.test(document.getElementById("aoPath").innerText),
                             null, { timeout: 8000 });
  const pathText = await page.innerText("#aoPath");
  check(/Puffer: \d+ minta/.test(pathText), "jelút: " + pathText.replace(/\n/g, " | "));
  await page.screenshot({ path: path.join(process.env.AQ_SHOTS || process.env.AQ_WORK || here, "gui_audio_opts.png") });
  await page.selectOption("#aoRate", "system");
  await page.uncheck("#aoKeepAwake");
  await page.selectOption("#aoBuffer", "0");
  await page.waitForFunction(() => aoOpts && aoOpts.buffer === 0 && !aoOpts.keepAwake && aoOpts.rateMode === "system", null, { timeout: 5000 });
  check(true, "visszaállítva: automatikus, ébren tartás ki, rendszer mintavétel");
  await page.keyboard.press("Escape");
  await page.mouse.click(800, 600);

  console.log("Csatornák és dither");
  check(await page.isVisible("#tabBtn-outstage"), "a „Csatornák” fül látszik");
  await page.click("#tabBtn-outstage");
  await page.waitForTimeout(400);
  await page.click("#btnToggle-outstage");
  await page.fill("#in-osDelayL", "1.5");
  await page.fill("#in-osGainR", "-3");
  await page.click("#in-osInvertR");
  await page.selectOption("#in-osDither", "2");
  await page.selectOption("#in-osBits", "0");
  await page.waitForTimeout(1200);
  const st = engineState();
  const o = st.outstage || {};
  check(st.modules && st.modules.outstage === 1, "a modul bekapcsolva a motorban");
  check(Math.abs(o.delayL - 1.5) < 1e-3 && Math.abs(o.gainR + 3) < 1e-3 && o.invertR === 1 && o.dither === 2 && o.bits === 16,
        `a motor beállítása: ${JSON.stringify(o)}`);
  await page.screenshot({ path: path.join(process.env.AQ_SHOTS || process.env.AQ_WORK || here, "gui_outstage.png") });
  await page.waitForFunction(() => /módosítja/.test(document.getElementById("aoPath").innerText), null, { timeout: 8000 });
  check(true, "jelút: a bekapcsolt csatornakiegyenlítés után „A DSP módosítja a jelet”");
  await page.click("#btnToggle-outstage");
  await page.waitForTimeout(800);
  check(engineState().modules.outstage === 0, "kikapcsolás a felületről");

  check(errors.length === 0, "nincs JavaScript-hiba" + (errors.length ? ": " + errors.join(" | ") : ""));
  await browser.close();
  console.log(fails ? `${fails} HIBA` : "SIKERES");
  process.exit(fails ? 1 : 0);
})().catch(e => { console.error(e); process.exit(1); });
