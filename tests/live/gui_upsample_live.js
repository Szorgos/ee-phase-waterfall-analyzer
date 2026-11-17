// Felmintavételezés élő felületi és fordítási teszt (K13, K14) Playwrighttal.
// Indítás: node tests/live/gui_upsample_live.js
const { chromium } = require("playwright");
const path = require("path");
const here = __dirname;
let fails = 0;
const check = (ok, msg) => { console.log((ok ? "  [OK]   " : "  [HIBA] ") + msg); if (!ok) fails++; };

(async () => {
  let browser;
  try {
    browser = await chromium.launch();
    const page = await browser.newPage({ viewport: { width: 1600, height: 1000 } });
    const errors = [];
    page.on("pageerror", e => errors.push("pageerror: " + e.message));
    page.on("console", m => {
      if (m.type() === "error" && !m.text().includes("ERR_FILE_NOT_FOUND")) {
        errors.push("console.error: " + m.text());
      }
    });

    const guiPath = "file://" + path.resolve(here, "../../gui_integrated_system.html");
    await page.goto(guiPath);
    await page.waitForTimeout(1000);

    // Konzolhibák ellenőrzése
    check(errors.length === 0, "K13: nincs felületi JavaScript / konzolhiba");

    // initPipewireBridge és felület megléte
    const hasBridge = await page.evaluate(() => typeof initPipewireBridge === "function");
    check(hasBridge, "K13: initPipewireBridge definiált");

    // Felmintavételezés vezérlők jelenléte
    const upsampleBlock = await page.$("#upsampleBlock");
    check(upsampleBlock !== null, "K13: felmintavételezés blokk (#upsampleBlock) létezik a felületen");

    // K14: Nyelvváltás ellenőrzése (de, ja) nyers kulcsok nélkül
    for (const lang of ["de", "ja"]) {
      const untranslated = await page.evaluate((l) => {
        if (typeof setLanguage === "function") setLanguage(l);
        const block = document.querySelector("#upsampleBlock");
        if (!block) return ["missing_block"];
        const text = block.innerText;
        const matches = text.match(/ups\.[a-zA-Z0-9_]+/g);
        return matches || [];
      }, lang);
      check(untranslated.length === 0, `K14: ${lang} nyelven nincsenek nyers ups.* fordítási kulcsok`);
    }

  } catch (err) {
    check(false, "K13/K14 teszt kivétel: " + err.message);
  } finally {
    if (browser) await browser.close();
  }

  console.log(`\n${fails ? "SIKERTELEN" : "MINDEN TESZT SIKERES"} (${fails} hiba)`);
  process.exit(fails ? 1 : 0);
})();
