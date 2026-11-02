// A felület kezelőszerveinek élő tesztje: a valódi felületen kattint, és egy külön kapcsolaton a motor állapotából
// ellenőrzi, hogy a beállítás odaért-e. Indítás: tests/live/multiout_live.sh <build> node tests/live/gui_controls_live.js
const { chromium } = require("playwright");
const { execFileSync } = require("child_process");
const path = require("path");
const here = __dirname;
let fails = 0;
const check = (ok, msg) => { console.log((ok ? "  [OK]   " : "  [HIBA] ") + msg); if (!ok) fails++; };
const engine = () => JSON.parse(execFileSync("python3", ["-I", path.join(here, "get_state.py")], { encoding: "utf8" }).trim());
const wait = ms => new Promise(r => setTimeout(r, ms));

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 1600, height: 1000 } });
  const errors = [];
  page.on("pageerror", e => errors.push(e.message));
  await page.goto("file://" + path.resolve(here, "../../gui_integrated_system.html"));
  await page.waitForFunction(() => typeof isPipewireConnected !== "undefined" && isPipewireConnected && guiStateSynced, null, { timeout: 15000 });
  await wait(800);

  // 1. Modulok be- és kikapcsolása a fülükön
  const mods = ["crossfeed", "convolver", "limiter", "bass", "mono", "autogain", "magic"];
  for (const m of mods) {
    await page.click(`#tabBtn-${m}`);
    await wait(250);
    await page.click(`#btnToggle-${m}`);
    await wait(500);
    let st = engine();
    const on = st.modules && st.modules[m] === 1;
    await page.click(`#btnToggle-${m}`);
    await wait(500);
    st = engine();
    const off = st.modules && st.modules[m] === 0;
    check(on && off, `${m}: a kapcsoló a motorban be (${on ? "1" : "?"}) és ki (${off ? "0" : "?"})`);
  }
  // EQ kapcsoló és a Konvolverrel való kölcsönös kizárás
  await page.click("#tabBtn-eq");
  await wait(250);
  await page.click("#btnEqBypassTab");
  await wait(500);
  let st = engine();
  check(st.modules.eq === 1, "EQ: a fül kapcsolója bekapcsolja a motorban");
  await page.click("#tabBtn-convolver"); await wait(250);
  await page.click("#btnToggle-convolver"); await wait(500);
  st = engine();
  check(st.modules.convolver === 1 && st.modules.eq === 0, "Konvolver bekapcsolásakor az EQ kikapcsol (kölcsönös kizárás)");
  await page.click("#btnToggle-convolver"); await wait(400);

  // 2. EQ mód és Decramp
  await page.click("#tabBtn-eq"); await wait(250);
  for (const [v, name] of [["1", "FIR"], ["2", "FFT"], ["0", "IIR"]]) {
    await page.selectOption("#selectEqMode", v);
    await wait(500);
    st = engine();
    check(String(st.eqMode) === v, `EQ mód: ${name} (motor: ${st.eqMode})`);
  }
  const decrampOpts = await page.$$eval("#selectDecramp option", os => os.map(o => o.value));
  const dVal = decrampOpts.find(v => v !== String((engine().decramp ?? "")) ) || decrampOpts[0];
  await page.selectOption("#selectDecramp", dVal); await wait(500);
  st = engine();
  check(String(st.decramp) === dVal, `Decramp: ${dVal} (motor: ${st.decramp})`);
  await page.selectOption("#selectDecramp", decrampOpts[0]); await wait(300);

  // 3. Előerősítés a mezőből
  await page.fill("#inputPreamp", "-4.5");
  await page.dispatchEvent("#inputPreamp", "input");
  await wait(500);
  st = engine();
  check(Math.abs(st.preamp + 4.5) < 0.05, `Előerősítés: −4,5 dB (motor: ${st.preamp})`);

  // 4. Crossfeed előbeállítás (nem kapcsolja be magától)
  await page.click("#tabBtn-crossfeed"); await wait(250);
  const before = engine().modules.crossfeed;
  await page.click("#cfPreset-meier"); await wait(500);
  st = engine();
  check(st.crossfeedPreset !== undefined && st.modules.crossfeed === before, `Crossfeed előbeállítás: Meier (motor: ${JSON.stringify(st.crossfeedPreset)}), a modul állapota nem változott`);

  // 5. Mono: mélyvágás a listából
  await page.click("#tabBtn-mono"); await wait(250);
  await page.selectOption("#monoControls select", String(5));   // 0 ki, 1 10, 2 15, 3 20, 4 25, 5 30, 6 35 Hz
  await wait(500);
  st = engine();
  check(st.monoBass && st.monoBass.subsonicHz === 30, `Mélyvágás: 30 Hz (motor: ${st.monoBass && st.monoBass.subsonicHz})`);

  check(errors.length === 0, "nincs JavaScript-hiba" + (errors.length ? ": " + errors.join(" | ") : ""));
  await browser.close();
  console.log(fails ? `\nSIKERTELEN (${fails} hiba)` : "\nMINDEN KEZELŐSZERV-TESZT SIKERES (0 hiba)");
  process.exit(fails ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });
