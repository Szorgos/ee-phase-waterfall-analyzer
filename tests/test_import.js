/**
 * Automatikus teszt az import-mintafájlok és a VART-EREDMENYEK.md alapján.
 * Futtatás: node tests/test_import.js
 */

const fs = require('fs');
const path = require('path');
const {
  EqBand,
  ParsedConfig,
  parseApoText,
  parseCamillaDspYaml,
  parseCsvPoints,
  parseMicCalibration,
  reduceBands,
  exportApoConfig
} = require('../import_parser');

const DATA_DIR = path.join(__dirname, 'data', 'import');

let passed = 0;
let total = 0;

function assertEq(actual, expected, msg = "") {
  total++;
  if (actual !== expected) {
    throw new Error(`FAIL: ${msg} -> Elvárt: ${expected}, Kapott: ${actual}`);
  }
  passed++;
}

function assertApprox(actual, expected, tol = 0.15, msg = "") {
  total++;
  if (Math.abs(actual - expected) > tol) {
    throw new Error(`FAIL: ${msg} -> Elvárt: ~${expected} (tol ${tol}), Kapott: ${actual}`);
  }
  passed++;
}

console.log("=== Aqulify JavaScript Import & Band Reducer Tesztek ===");

// 1. 01-parametrikus.txt
console.log("Teszt 1: 01-parametrikus.txt");
const t1 = fs.readFileSync(path.join(DATA_DIR, "01-parametrikus.txt"), "utf8");
const p1 = parseApoText(t1);
assertApprox(p1.preamp, -6.2, 0.01, "01 Preamp");
assertEq(p1.bandsL.length, 4, "01 4 sáv L-en");
assertEq(p1.skipped.some(s => s[0].includes("OFF") || s[1].includes("OFF")), true, "01 OFF sor megnevezve kimaradt");

// 2. 02-szoba-bal-jobb.txt
console.log("Teszt 2: 02-szoba-bal-jobb.txt");
const t2 = fs.readFileSync(path.join(DATA_DIR, "02-szoba-bal-jobb.txt"), "utf8");
const p2 = parseApoText(t2);
assertApprox(p2.preamp, -5.0, 0.01, "02 Preamp");
assertEq(p2.eqSplit, true, "02 Külön bal/jobb EQ aktív");
assertApprox(p2.delayLMs, 0.35, 0.01, "02 L delay");
assertApprox(p2.delayRMs, 0.0, 0.01, "02 R delay");
assertEq(p2.bandsL.length, 4, "02 L sávok száma (3 PK + 1 HSC)");
assertEq(p2.bandsR.length, 3, "02 R sávok száma (2 PK + 1 HSC)");
assertApprox(p2.bandsL[0].freq, 42.0, 0.1, "02 L band 0 freq");
assertApprox(p2.bandsR[0].freq, 45.0, 0.1, "02 R band 0 freq");

// 3. 03-azonos-bal-jobb.txt
console.log("Teszt 3: 03-azonos-bal-jobb.txt");
const t3 = fs.readFileSync(path.join(DATA_DIR, "03-azonos-bal-jobb.txt"), "utf8");
const p3 = parseApoText(t3);
assertEq(p3.eqSplit, false, "03 Azonos bal/jobb sávoknál eqSplit hamis");
assertEq(p3.bandsL.length, 2, "03 L sávok száma");
assertEq(p3.bandsR.length, 2, "03 R sávok száma");
assertApprox(p3.bandsL[0].freq, 60.0, 0.1, "03 PK 60 Hz");
assertApprox(p3.bandsL[1].freq, 120.0, 0.1, "03 LS 120 Hz");
assertApprox(p3.bandsL[1].gain, 2.0, 0.1, "03 LS +2 dB");

// 4. 04-meredekseg.txt
console.log("Teszt 4: 04-meredekseg.txt");
const t4 = fs.readFileSync(path.join(DATA_DIR, "04-meredekseg.txt"), "utf8");
const p4 = parseApoText(t4);
assertEq(p4.bandsL.length, 5, "04 Mind az 5 sáv betöltve");
assertApprox(p4.bandsL[0].freq, 25.0, 0.1, "04 HP 25");
assertApprox(p4.bandsL[1].freq, 18000.0, 1.0, "04 LP 18k");
assertApprox(p4.bandsL[2].freq, 80.0, 0.1, "04 LS 80");
assertApprox(p4.bandsL[3].freq, 6000.0, 1.0, "04 HS 6k");
assertApprox(p4.bandsL[4].freq, 30.0, 0.1, "04 HP 30");

// 5. 05-grafikus.txt + Sávcsökkentő
console.log("Teszt 5: 05-grafikus.txt + Sávcsökkentő");
const t5 = fs.readFileSync(path.join(DATA_DIR, "05-grafikus.txt"), "utf8");
const p5 = parseApoText(t5);
assertEq(p5.points.length >= 50, true, "05 GraphicEQ pontok kinyerve");
const red = reduceBands(p5.points, 10);
assertEq(red.bands.length <= 10, true, "05 Redukált sávok száma <= 10");
assertEq(red.peakErr < 2.5, true, `05 Csúcs eltérés kicsi (${red.peakErr.toFixed(2)} dB)`);
assertEq(red.rmsErr < 1.0, true, `05 RMS eltérés kicsi (${red.rmsErr.toFixed(2)} dB)`);
console.log(`   -> Sávcsökkentő eredmény: ${red.bands.length} sáv, csúcs hiba: ${red.peakErr.toFixed(2)} dB, RMS: ${red.rmsErr.toFixed(2)} dB`);

// Helper fájlolvasó az Include és létezés tesztekhez
function readFileHelper(subPath, baseDir) {
  const full = path.join(baseDir, subPath);
  if (fs.existsSync(full)) {
    return fs.readFileSync(full, "utf8");
  }
  return null;
}

function checkFileExistsHelper(subPath, baseDir) {
  const full = path.join(baseDir, subPath);
  return fs.existsSync(full);
}

// 6. 06-include/fo.txt
console.log("Teszt 6: 06-include/fo.txt");
const dir6 = path.join(DATA_DIR, "06-include");
const t6_fo = fs.readFileSync(path.join(dir6, "fo.txt"), "utf8");
const p6_fo = parseApoText(t6_fo, { readFileFn: readFileHelper, basePath: dir6 });
assertApprox(p6_fo.preamp, -3.0, 0.01, "06 Preamp -3 dB");
assertEq(p6_fo.bandsL.length, 2, "06 2 sáv a két alfájlból");

// 7. 06-include/kilepo.txt (Path traversal védelem)
console.log("Teszt 7: 06-include/kilepo.txt");
const t6_kil = fs.readFileSync(path.join(dir6, "kilepo.txt"), "utf8");
const p6_kil = parseApoText(t6_kil, { readFileFn: readFileHelper, basePath: dir6 });
assertEq(p6_kil.bandsL.length, 0, "06 Kilépő: nincs betöltött sáv");
const escapeSkips = p6_kil.skipped.filter(s => s[1].includes("kilép") || s[1].includes("Tiltott"));
assertEq(escapeSkips.length, 3, "06 Mindhárom kilepo Include elutasítva");

// 8. 06-include/kor-a.txt (Ciklusvédelem)
console.log("Teszt 8: 06-include/kor-a.txt");
const t6_kor = fs.readFileSync(path.join(dir6, "kor-a.txt"), "utf8");
const p6_kor = parseApoText(t6_kor, { readFileFn: readFileHelper, basePath: dir6, initialFile: "kor-a.txt" });
assertEq(p6_kor.bandsL.length, 1, "06 Ciklus: PK 100 betöltve");
const cycleSkips = p6_kor.skipped.filter(s => s[1].includes("Körkörös"));
assertEq(cycleSkips.length >= 1, true, "06 Körkörös hivatkozás felismerve");

// 9. 07-konvolucio/eq.txt
console.log("Teszt 9: 07-konvolucio/eq.txt");
const dir7 = path.join(DATA_DIR, "07-konvolucio");
const t7 = fs.readFileSync(path.join(dir7, "eq.txt"), "utf8");
const p7 = parseApoText(t7, { checkFileExists: checkFileExistsHelper, basePath: dir7 });
assertApprox(p7.preamp, -2.0, 0.01, "07 Preamp -2 dB");
assertEq(path.basename(p7.convL), "ir-bal.wav", "07 Bal IR fájl");
assertEq(path.basename(p7.convR), "ir-jobb.wav", "07 Jobb IR fájl");

// 10. 07-konvolucio/hianyzo.txt
console.log("Teszt 10: 07-konvolucio/hianyzo.txt");
const t7_hianyz = fs.readFileSync(path.join(dir7, "hianyzo.txt"), "utf8");
const p7_hianyz = parseApoText(t7_hianyz, { checkFileExists: checkFileExistsHelper, basePath: dir7 });
const missingSkips = p7_hianyz.skipped.filter(s => s[1].includes("nem található"));
assertEq(missingSkips.length >= 1, true, "07 Hiányzó fájl hibaüzenet");

// 11. 09-copy.txt
console.log("Teszt 11: 09-copy.txt");
const t9_copy = fs.readFileSync(path.join(DATA_DIR, "09-copy.txt"), "utf8");
const p9_copy = parseApoText(t9_copy);
assertEq(p9_copy.eqSplit, true, "09 Copy: szétválasztva");
assertEq(p9_copy.bandsL.length, 1, "09 Copy: PK 100 mindkét csatornán");
assertEq(p9_copy.bandsR.length, 1, "09 Copy: PK 100 mindkét csatornán");

// 12. 09-copy-bonyolult.txt
console.log("Teszt 12: 09-copy-bonyolult.txt");
const t9_bony = fs.readFileSync(path.join(DATA_DIR, "09-copy-bonyolult.txt"), "utf8");
const p9_bony = parseApoText(t9_bony);
assertEq(p9_bony.skipped.some(s => s[1].includes("nem támogatott")), true, "09 Bonyolult keverés megnevezve kimaradt");
assertEq(p9_bony.bandsL.length, 1, "09 PK 100 betöltődött");

// 13. 09-egyeb-sorok.txt
console.log("Teszt 13: 09-egyeb-sorok.txt");
const t9_egy = fs.readFileSync(path.join(DATA_DIR, "09-egyeb-sorok.txt"), "utf8");
const p9_egy = parseApoText(t9_egy);
assertEq(p9_egy.bandsL.length, 1, "09 Egy sáv betöltve (PK 1000)");
assertEq(p9_egy.bandsL[0].freq, 1000.0, "09 PK 1000");
assertEq(p9_egy.skipped.length >= 4, true, "09 Device, Stage, Eval, XYZ megnevezve kimaradt");

// 14. 10-meres.csv
console.log("Teszt 14: 10-meres.csv");
const t10_m = fs.readFileSync(path.join(DATA_DIR, "10-meres.csv"), "utf8");
const pts10_m = parseCsvPoints(t10_m);
assertEq(pts10_m.length > 100, true, "10 Mérés: > 100 pont beolvasva");
let peakPt = pts10_m[0];
pts10_m.forEach(p => { if (p[1] > peakPt[1]) peakPt = p; });
assertApprox(peakPt[0], 45.0, 5.0, "10 Mérés csúcs ~45 Hz");
assertApprox(peakPt[1] - 75.0, 6.0, 0.5, "10 Mérés csúcs ~+6 dB a 75 dB alaphoz képest");

// 15. 10-mikrofon-kalibracio.txt (parseCsvPoints + parseMicCalibration szigorú ellenőrzés)
console.log("Teszt 15: 10-mikrofon-kalibracio.txt");
const t10_k = fs.readFileSync(path.join(DATA_DIR, "10-mikrofon-kalibracio.txt"), "utf8");
const pts10_k = parseCsvPoints(t10_k);
assertEq(pts10_k.length, 30, "10 Kalibráció adatpontok száma (fejléc nélkül 30)");
assertApprox(pts10_k[0][1], 0.8, 0.05, "10 Kalibráció 20 Hz +0.8 dB");
assertApprox(pts10_k[pts10_k.length - 1][1], -1.2, 0.05, "10 Kalibráció 16 kHz -1.2 dB");

const calRes = parseMicCalibration(t10_k);
assertEq(calRes.ok, true, "10 Kalibráció validálása sikeres");
assertEq(calRes.points.length, 30, "10 Kalibráció 30 pont (31 sorból az első idézőjeles fejléc)");
assertApprox(calRes.sensitivity, -38.2, 0.01, "10 Kalibráció Sens Factor kinyerve");
assertApprox(calRes.points[0].db, 0.8, 0.01, "10 Kalibráció 20 Hz +0.8 dB");
assertApprox(calRes.points[1].db, 0.8, 0.01, "10 Kalibráció 25.2 Hz +0.8 dB");
assertApprox(calRes.points[calRes.points.length - 1].db, -1.2, 0.01, "10 Kalibráció 16255 Hz -1.2 dB");
assertApprox(calRes.maxAbsDb, 1.2, 0.01, "10 Kalibráció max abs korrekció 1.2 dB");

// 16. 10-celgorbe.txt
console.log("Teszt 16: 10-celgorbe.txt");
const t10_c = fs.readFileSync(path.join(DATA_DIR, "10-celgorbe.txt"), "utf8");
const pts10_c = parseCsvPoints(t10_c);
assertEq(pts10_c.length, 6, "10 Célgörbe 6 pont");
assertApprox(pts10_c[0][1], 4.0, 0.1, "10 Célgörbe 20 Hz +4 dB");
assertApprox(pts10_c[pts10_c.length - 1][1], -4.0, 0.1, "10 Célgörbe 20 kHz -4 dB");

// 17. 11-camilladsp.yml
console.log("Teszt 17: 11-camilladsp.yml");
const t11 = fs.readFileSync(path.join(DATA_DIR, "11-camilladsp.yml"), "utf8");
const p11 = parseCamillaDspYaml(t11);
assertApprox(p11.preamp, -5.0, 0.01, "11 CamillaDSP Preamp -5 dB");
assertEq(p11.eqSplit, true, "11 CamillaDSP Split L/R");
assertApprox(p11.delayLMs, 0.35, 0.01, "11 CamillaDSP Delay L");
assertEq(p11.bandsL.length, 3, "11 CamillaDSP L sávok (HP 25, PK 44, HS 8000)");
assertEq(p11.bandsR.length, 3, "11 CamillaDSP R sávok (HP 25, PK 47, HS 8000)");
assertEq(p11.convL, "", "11 room_l nincs a pipeline-ban -> nem került be");

// 18. Export - Visszaimport teszt
console.log("Teszt 18: Export - Visszaimport");
const expText = exportApoConfig(p2);
const reimported = parseApoText(expText);
assertApprox(reimported.preamp, p2.preamp, 0.01, "Export-Import Preamp");
assertEq(reimported.bandsL.length, p2.bandsL.length, "Export-Import L sávok száma");
assertEq(reimported.bandsR.length, p2.bandsR.length, "Export-Import R sávok száma");
assertApprox(reimported.bandsL[0].freq, p2.bandsL[0].freq, 0.1, "Export-Import Band 0 Freq");

// 19. Mikrofon-kalibráció érvényesítési és formátumtesztek (hibás fájlok + elválasztók)
console.log("Teszt 19: Mikrofon-kalibráció hibás és speciális formák");
const calEmpty = parseMicCalibration("   \n# komment\n* csillag\n");
assertEq(calEmpty.ok, false, "Üres kalibrációs fájl elutasítva");
assertEq(calEmpty.points.length, 0, "Üresnél 0 pont töltődik");

const calTwoPts = parseMicCalibration("20 0.5\n1000 0.0\n");
assertEq(calTwoPts.ok, false, "2 pontos kalibrációs fájl elutasítva (< 3)");
assertEq(calTwoPts.error, "tooFewPoints", "2 pont hibakód");

const calDesc = parseMicCalibration("20 0.0\n2000 0.5\n500 -0.2\n");
assertEq(calDesc.ok, false, "Nem növekvő frekvencia elutasítva");
assertEq(calDesc.error, "notIncreasing", "Nem növekvő hibakód");

const calText = parseMicCalibration("Ez egy sima szöveges dokumentum\nami nem tartalmaz méréseket.\nSemmi szám.\n");
assertEq(calText.ok, false, "Szöveges fájl elutasítva");
assertEq(calText.points.length, 0, "Szöveges fájlnál 0 pont töltődik");

const calRangeDb = parseMicCalibration("20 0.0\n1000 35.0\n10000 0.0\n");
assertEq(calRangeDb.ok, false, "|dB| > 30 elutasítva");
assertEq(calRangeDb.error, "outOfGainRange", "|dB| > 30 hibakód");

const calRangeFreq = parseMicCalibration("5 0.0\n1000 0.0\n10000 0.0\n");
assertEq(calRangeFreq.ok, false, "freq < 10 Hz elutasítva");
assertEq(calRangeFreq.error, "outOfFreqRange", "freq < 10 Hz hibakód");

const calSemiPhase = parseMicCalibration("* UMIK-1 90deg\n20.0;+0.80;15.2\n1000,0.00,0.0\n20000\t-1.20\t-10.5\n");
assertEq(calSemiPhase.ok, true, "Pontosvessző/vessző/tab + fázis elfogadva");
assertEq(calSemiPhase.points.length, 3, "3 pont elválasztókkal");
assertApprox(calSemiPhase.points[2].phase, -10.5, 0.01, "Fázis oszlop megőrizve");

// Tizedesvessző (európai táblázatkezelők exportja): pontosvessző vagy tabulátor mellett a vessző tizedesjel
const calDecComma = parseMicCalibration("20;0,8\n1000;0\n20000;-1,2\n");
assertEq(calDecComma.ok, true, "Pontosvessző + tizedesvessző elfogadva");
assertApprox(calDecComma.points[0].db, 0.8, 0.001, "20;0,8 → +0,8 dB (nem 0)");
assertApprox(calDecComma.points[2].db, -1.2, 0.001, "20000;-1,2 → −1,2 dB");
const calTabComma = parseMicCalibration("20\t0,8\t5,5\n1000\t0\t0\n20000\t-1,2\t-3\n");
assertApprox(calTabComma.points[0].db, 0.8, 0.001, "tab + tizedesvessző: +0,8 dB");
assertApprox(calTabComma.points[0].phase, 5.5, 0.001, "tab + tizedesvessző: fázis 5,5");
const csvDecComma = parseCsvPoints("45;6,0;10\n100;0,5;0\n");
assertApprox(csvDecComma[0][1], 6.0, 0.001, "mérési CSV: 45;6,0 → +6 dB");

console.log(`\n========================================`);
console.log(`MINDEN TESZT SIKERES: ${passed} / ${total} ellenőrzés rendben!`);
console.log(`========================================\n`);
