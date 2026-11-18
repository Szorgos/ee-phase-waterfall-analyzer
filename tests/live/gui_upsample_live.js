// Felmintavételezés élő felületi tesztje (Playwright, K13 és K14).
// Indítás: tests/live/multiout_live.sh <build> node tests/live/gui_upsample_live.js
const { chromium } = require("playwright");
const fs = require("fs");
const path = require("path");
const here = __dirname;

let fails = 0;
const check = (ok, msg) => {
  console.log((ok ? "  [OK]   " : "  [HIBA] ") + msg);
  if (!ok) fails++;
};

function makeSineWav(filePath, sr = 44100, freq = 1000, seconds = 6) {
  const n = sr * seconds;
  const dataBytes = n * 4;
  const buf = Buffer.alloc(44 + dataBytes);
  buf.write("RIFF", 0);
  buf.writeUInt32LE(36 + dataBytes, 4);
  buf.write("WAVEfmt ", 8);
  buf.writeUInt32LE(16, 16);
  buf.writeUInt16LE(1, 20);
  buf.writeUInt16LE(2, 22);
  buf.writeUInt32LE(sr, 24);
  buf.writeUInt32LE(sr * 4, 28);
  buf.writeUInt16LE(4, 32);
  buf.writeUInt16LE(16, 34);
  buf.write("data", 36);
  buf.writeUInt32LE(dataBytes, 40);
  for (let i = 0; i < n; i++) {
    const v = Math.round(0.5 * Math.sin(2 * Math.PI * freq * i / sr) * 32767);
    buf.writeInt16LE(v, 44 + i * 4);
    buf.writeInt16LE(v, 44 + i * 4 + 2);
  }
  fs.writeFileSync(filePath, buf);
}

(async () => {
  const workDir = process.env.AQ_WORK || here;
  const wavPath = path.join(workDir, "gui_ups_44k.wav");
  makeSineWav(wavPath, 44100, 1000, 8);

  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 1600, height: 1000 } });
  const errors = [];
  page.on("pageerror", e => errors.push(e.message));
  await page.addInitScript(() => {
    try { localStorage.setItem("aqulify_lang", "hu"); } catch (e) {}
  });
  await page.goto("file://" + path.resolve(here, "../../gui_integrated_system.html"));
  await page.waitForFunction(() => typeof aoCaps !== "undefined" && aoCaps === true && aoOpts !== null, null, { timeout: 15000 });

  // Alap ellenőrzés: initPipewireBridge
  const hasInitBridge = await page.evaluate(() => typeof initPipewireBridge === "function");
  check(hasInitBridge, "K13: typeof initPipewireBridge === 'function'");

  // Kimenet menü és felmintavételezés blokk
  await page.click("#btnSinkMenu");
  check(await page.isVisible("#aoUpsampleWrap"), "K13: a Felmintavételezés blokk látható a Kimenet menüben");

  // 1. Szabály beállítása: ×2
  await page.selectOption("#upsRule", "1");
  await page.waitForFunction(() => aoOpts && aoOpts.upsampleRule === 1, null, { timeout: 5000 });
  check(true, "K13: ×2 szabály beállítva, az audio_opts visszaigazolta");

  // 2. Előbeállítás gombok
  await page.click("#upsPreset2");
  await page.waitForFunction(() => aoOpts && aoOpts.upsamplePhase === 0, null, { timeout: 5000 });
  check(true, "K13: Minimumfázis előbeállítás (fázis = 0) visszaigazolva");
  await page.click("#upsPreset1");
  await page.waitForFunction(() => aoOpts && aoOpts.upsamplePhase === 25, null, { timeout: 5000 });
  check(true, "K13: Köztes előbeállítás (fázis = 25) visszaigazolva");

  // 3. Haladó rész és passband >= stopband ellenőrzés
  await page.click("#upsAdvDetails summary");
  await page.fill("#upsPassband", "105");
  await page.dispatchEvent("#upsPassband", "change");
  check(await page.isVisible("#upsErrOrder"), "K13: passband >= stopband esetén érthető hibaüzenet jelenik meg");
  const pbAfterErr = await page.inputValue("#upsPassband");
  check(parseFloat(pbAfterErr) < 100, `K13: érvénytelen érték után a régi érték maradt (${pbAfterErr}%)`);

  // 4. „i” súgó és licencmegjelölés
  await page.click("#upsHelpBtn");
  check(await page.isVisible("#upsHelpBox"), "K13: az 'i' súgó megnyílik");
  const helpText = await page.innerText("#upsHelpBox");
  check(helpText.includes("libsoxr, LGPL-2.1+"), "K13: a súgó tartalmazza a libsoxr, LGPL-2.1+ megjelölést");

  // 5. K12(a) élő állapotsor a felületen: „forrást követi” -> 44,1 -> 88,2 kHz, DAC 88,2 kHz
  await page.selectOption("#aoRate", "follow");
  await page.waitForFunction(() => aoOpts && aoOpts.rateMode === "follow", null, { timeout: 5000 });
  await page.evaluate(p => moSend({ type: "player_open", path: p }), wavPath);
  await page.waitForFunction(() => {
    const u = engineStatus && engineStatus.upsample;
    return u && u.active === true && u.targetRate === 88200 && u.actualDacRate === 88200 && u.resampleWarning === false;
  }, null, { timeout: 10000 });
  const stFollow = await page.innerText("#upsStatus");
  check(/44[.,]1 kHz/.test(stFollow) && /88[.,]2 kHz/.test(stFollow) && !/Figyelem/.test(stFollow),
        "K13: forrást követő módban az állapotsor a valóságot mutatja: " + stFollow);
  await page.evaluate(() => moSend({ type: "player_stop" }));
  await page.waitForTimeout(800);

  // 6. K12(b) élő állapotsor a felületen: „rendszer” mód (48 kHz) -> visszaalakítás figyelmeztetés
  await page.selectOption("#aoRate", "system");
  await page.waitForFunction(() => aoOpts && aoOpts.rateMode === "system", null, { timeout: 5000 });
  await page.evaluate(p => moSend({ type: "player_open", path: p }), wavPath);
  await page.waitForFunction(() => {
    const u = engineStatus && engineStatus.upsample;
    return u && u.active === true && u.actualDacRate === 48000 && u.resampleWarning === true;
  }, null, { timeout: 10000 });
  const stWarn = await page.innerText("#upsStatus");
  check(/48 kHz/.test(stWarn) && /Figyelem/.test(stWarn),
        "K13: rendszer módban az állapotsor a visszaalakítás figyelmeztetését mutatja: " + stWarn);
  await page.evaluate(() => moSend({ type: "player_stop" }));
  await page.waitForTimeout(500);

  // 7. Újratöltés után megmarad
  await page.reload();
  await page.waitForFunction(() => typeof aoCaps !== "undefined" && aoCaps === true && aoOpts !== null, null, { timeout: 15000 });
  await page.click("#btnSinkMenu");
  const ruleAfterReload = await page.inputValue("#upsRule");
  check(ruleAfterReload === "1" && (await page.evaluate(() => aoOpts.upsampleRule === 1)),
        `K13: újratöltés után a beállítás megmaradt (upsRule=${ruleAfterReload})`);

  // 8. K14: Német és japán nyelvváltás ellenőrzése (nincs magyar szó és nincs nyers ups. kulcs)
  const huWords = ["Felmintavételezés", "Szabály", "Kikapcsolva", "Fejtér", "Pontosság", "Haladó", "Figyelem", "Túlvezérelt", "Záró sáv"];
  for (const lang of ["de", "ja"]) {
    await page.evaluate(l => {
      if (typeof applyLang === "function") applyLang(l);
      else if (typeof setLanguage === "function") setLanguage(l);
    }, lang);
    await page.waitForTimeout(300);
    // Súgó kinyitása, hogy a súgó szövege is ellenőrzésre kerüljön
    const helpHidden = await page.evaluate(() => document.getElementById("upsHelpBox").hidden);
    if (helpHidden) await page.click("#upsHelpBtn");
    const txt = await page.innerText("#aoUpsampleWrap");
    const hasRawKey = /\bups\.[a-zA-Z]+/.test(txt);
    const foundHu = huWords.filter(w => txt.includes(w));
    check(!hasRawKey && foundHu.length === 0,
          `K14 (${lang}): nincs nyers ups. kulcs és nincs magyar szó az új blokkban` +
          (hasRawKey ? " [nyers ups. kulcs!]" : "") + (foundHu.length ? ` [magyar: ${foundHu.join(",")}]` : ""));
  }

  // Visszaállítás alapállapotra
  await page.selectOption("#upsRule", "0");
  await page.selectOption("#aoRate", "system");
  await page.waitForFunction(() => aoOpts && aoOpts.upsampleRule === 0 && aoOpts.rateMode === "system", null, { timeout: 5000 });

  check(errors.length === 0, "K13: nincs JavaScript-hiba" + (errors.length ? ": " + errors.join(" | ") : ""));
  await browser.close();
  console.log(fails ? `${fails} HIBA` : "SIKERES");
  process.exit(fails ? 1 : 0);
})().catch(e => {
  console.error(e);
  process.exit(1);
});
