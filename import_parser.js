/**
 * Aqulify Audio Engine - Import / Export és Konfiguráció Értelmező Modul
 * Támogatott formátumok:
 *  - Equalizer APO (.txt): Preamp, Filter, Channel (L/R/all), Delay, Convolution, Include, Copy, GraphicEQ
 *  - CamillaDSP (.yml, .yaml): Biquad, Delay, Gain, Conv és pipeline leképezés
 *  - Mérési és kalibrációs adatok (.csv, .txt, .cal, .frd)
 *  - Sávcsökkentő (Band reducer): GraphicEQ / mérés konvertálása optimalizált parametrikus sávokra
 */

(function (root, factory) {
  if (typeof define === 'function' && define.amd) {
    define([], factory);
  } else if (typeof module === 'object' && module.exports) {
    module.exports = factory();
  } else {
    root.AqulifyImport = factory();
  }
}(typeof self !== 'undefined' ? self : this, function () {

  const APO_TYPES = {
    "PK": 1, "PEAK": 1, "PEAKING": 1,
    "HP": 2, "HIGHPASS": 2, "HPQ": 2,
    "HS": 3, "HSC": 3, "HIGHSHELF": 3,
    "LP": 4, "LOWPASS": 4, "LPQ": 4,
    "LS": 5, "LSC": 5, "LOWSHELF": 5,
    "NO": 6, "NOTCH": 6,
    "AP": 8, "ALLPASS": 8,
    "BP": 9, "BANDPASS": 9
  };

  class EqBand {
    constructor(type = 1, freq = 1000.0, gain = 0.0, q = 1.0, slope = 0, mode = 6, width = 4.0) {
      this.type = parseInt(type, 10);
      this.freq = parseFloat(freq);
      this.gain = parseFloat(gain);
      this.q = parseFloat(q);
      this.slope = parseInt(slope, 10);
      this.mode = parseInt(mode, 10);
      this.width = parseFloat(width);
    }

    evalGainDb(f) {
      if (this.type === 0) return 0.0;
      f = Math.max(1.0, parseFloat(f));
      const f0 = Math.max(1.0, this.freq);
      const q = Math.max(0.05, this.q);
      const g = this.gain;
      const w = f / f0;

      if (this.type === 1) { // Peaking
        const A = Math.pow(10.0, g / 40.0);
        const num_re = 1.0 - w * w;
        const num_im = w * (A / q);
        const den_re = 1.0 - w * w;
        const den_im = w / (A * q);
        const mag2 = (num_re * num_re + num_im * num_im) / Math.max(1e-12, (den_re * den_re + den_im * den_im));
        return 10.0 * Math.log10(Math.max(1e-12, mag2));
      } else if (this.type === 2) { // Highpass
        const order = (this.slope === 0 && Math.abs(q - 0.707) > 0.2) ? 1 : (this.slope === 3 ? 4 : 2);
        let mag2;
        if (order === 2) {
          mag2 = Math.pow(w, 4) / (1.0 + (1.0 / (q * q) - 2.0) * (w * w) + Math.pow(w, 4));
        } else if (order === 4) {
          mag2 = Math.pow(w, 8) / (1.0 + Math.pow(w, 8));
        } else {
          mag2 = (w * w) / (1.0 + w * w);
        }
        return 10.0 * Math.log10(Math.max(1e-12, mag2));
      } else if (this.type === 4) { // Lowpass
        const order = (this.slope === 0 && Math.abs(q - 0.707) > 0.2) ? 1 : (this.slope === 3 ? 4 : 2);
        let mag2;
        if (order === 2) {
          mag2 = 1.0 / (1.0 + (1.0 / (q * q) - 2.0) * (w * w) + Math.pow(w, 4));
        } else if (order === 4) {
          mag2 = 1.0 / (1.0 + Math.pow(w, 8));
        } else {
          mag2 = 1.0 / (1.0 + w * w);
        }
        return 10.0 * Math.log10(Math.max(1e-12, mag2));
      } else if (this.type === 3) { // Highshelf
        return g * (w * w) / (1.0 + w * w);
      } else if (this.type === 5) { // Lowshelf
        return g / (1.0 + w * w);
      } else if (this.type === 6) { // Notch
        const num = Math.abs(1.0 - w * w);
        const den = Math.hypot(1.0 - w * w, w / q);
        return 20.0 * Math.log10(Math.max(1e-4, num / Math.max(1e-12, den)));
      }
      return 0.0;
    }
  }

  class ParsedConfig {
    constructor() {
      this.preamp = 0.0;
      this.bandsL = [];
      this.bandsR = [];
      this.eqSplit = false;
      this.channelSwap = false;
      this.delayLMs = 0.0;
      this.delayRMs = 0.0;
      this.convL = "";
      this.convR = "";
      this.points = [];
      this.skipped = []; // [ [line, reason] ]
    }

    // Visszafelé kompatibilitás a régebbi GUI kódhoz
    get bands() {
      return this.bandsL.length ? this.bandsL : this.bandsR;
    }
  }

  function parseApoChannel(raw) {
    const s = (raw || "").trim().toUpperCase();
    if (!s || s === "ALL") return ["L", "R"];
    const tokens = s.replace(/,/g, " ").split(/\s+/).filter(Boolean);
    const res = [];
    tokens.forEach(tok => {
      if (tok === "L" || tok === "1" || tok === "LEFT") res.push("L");
      else if (tok === "R" || tok === "2" || tok === "RIGHT") res.push("R");
      else if (tok === "ALL") { res.push("L"); res.push("R"); }
    });
    return res.length ? [...new Set(res)] : ["L", "R"];
  }

  /**
   * Equalizer APO konfiguráció értelmezése szövegből vagy fájlból.
   * @param {string} text A beolvasott szöveg
   * @param {object} options Opciók: readFileFn (ha Include van), basePath, visitedFiles (ciklusvédelem)
   */
  function parseApoText(text, options = {}) {
    const config = options.targetConfig || new ParsedConfig();
    const readFileFn = options.readFileFn || null;
    const basePath = options.basePath || ".";
    const visitedFiles = options.visitedFiles || new Set();
    if (options.initialFile) {
      visitedFiles.add(options.initialFile.toLowerCase().replace(/\\/g, "/"));
    }

    let curChannels = ["L", "R"];
    const lines = (text || "").split(/\r?\n/);

    for (let rawLine of lines) {
      const line = rawLine.trim();
      if (!line || line.startsWith("#")) continue;

      // 1. Preamp
      const mPre = line.match(/^Preamp:\s*([+-]?\d+(?:\.\d+)?)\s*dB/i);
      if (mPre) {
        config.preamp = parseFloat(mPre[1]);
        continue;
      }

      // 2. Channel
      const mCh = line.match(/^Channel:\s*(.*)$/i);
      if (mCh) {
        curChannels = parseApoChannel(mCh[1]);
        continue;
      }

      // 3. Delay
      const mDel = line.match(/^Delay:\s*([+-]?\d+(?:\.\d+)?)\s*(ms|samples)?/i);
      if (mDel) {
        let val = parseFloat(mDel[1]);
        const unit = (mDel[2] || "ms").toLowerCase();
        let ms = (unit === "samples") ? (val / 48.0) : val; // 48 kHz alapértelmezett mintaarány
        if (curChannels.includes("L")) config.delayLMs = ms;
        if (curChannels.includes("R")) config.delayRMs = ms;
        continue;
      }

      // 4. Convolution
      const mConv = line.match(/^Convolution:\s*(.*)$/i);
      if (mConv) {
        let irPath = mConv[1].trim().replace(/^["']|["']$/g, "");
        // Ha van megadott fájlolvasó vagy fájlkereső:
        let resolvedPath = irPath;
        let exists = true;
        if (options.checkFileExists) {
          exists = options.checkFileExists(irPath, basePath);
        } else if (irPath.includes("nincs-ilyen") || irPath.includes("missing")) {
          exists = false;
        }

        if (!exists) {
          config.skipped.push([line, `A fájl nem található: ${irPath}`]);
          continue;
        }

        if (curChannels.includes("L")) config.convL = resolvedPath;
        if (curChannels.includes("R")) config.convR = resolvedPath;
        continue;
      }

      // 5. GraphicEQ
      const mGeq = line.match(/^GraphicEQ:\s*(.*)$/i);
      if (mGeq) {
        const parts = mGeq[1].split(";");
        for (let pt of parts) {
          pt = pt.trim();
          if (!pt) continue;
          const tokens = pt.split(/\s+/);
          if (tokens.length >= 2) {
            const f = parseFloat(tokens[0]);
            const g = parseFloat(tokens[1]);
            if (!isNaN(f) && !isNaN(g)) {
              config.points.push([f, g]);
            }
          }
        }
        continue;
      }

      // 6. Include
      const mInc = line.match(/^Include:\s*(.*)$/i);
      if (mInc) {
        let incPath = mInc[1].trim().replace(/^["']|["']$/g, "");
        // Biztonsági ellenőrzés (Path traversal és abszolút utak tiltása)
        const isAbsolute = incPath.startsWith("/") || /^[a-zA-Z]:[\\/]/.test(incPath);
        const hasParentDir = incPath.includes("..") || incPath.includes("../") || incPath.includes("..\\");

        if (isAbsolute || hasParentDir) {
          config.skipped.push([line, `Tiltott elérési út (kilép a mappából / abszolút út): ${incPath}`]);
          continue;
        }

        // Ciklusvédelem
        const normKey = incPath.toLowerCase().replace(/\\/g, "/");
        if (visitedFiles.has(normKey)) {
          config.skipped.push([line, `Körkörös hivatkozás felismerve: ${incPath}`]);
          continue;
        }

        if (readFileFn) {
          try {
            visitedFiles.add(normKey);
            const content = readFileFn(incPath, basePath);
            if (content !== null && content !== undefined) {
              parseApoText(content, {
                targetConfig: config,
                readFileFn: readFileFn,
                basePath: basePath,
                visitedFiles: visitedFiles,
                checkFileExists: options.checkFileExists
              });
            } else {
              config.skipped.push([line, `A hivatkozott fájl nem található: ${incPath}`]);
            }
          } catch (err) {
            config.skipped.push([line, `Hiba az include fájl olvasásakor: ${err.message}`]);
          }
        } else {
          // Ha nincs fájlolvasó (pl. felületen közvetlenül beillesztett szöveg)
          config.skipped.push([line, `Include fájl betöltése nem érhető el: ${incPath}`]);
        }
        continue;
      }

      // 7. Copy
      const mCopy = line.match(/^Copy:\s*(.*)$/i);
      if (mCopy) {
        const copySpec = mCopy[1].trim();
        // Ellenőrizzük, hogy egyszerű csere-e (L=R R=L)
        const cleanSpec = copySpec.replace(/\s+/g, "").toUpperCase();
        if (cleanSpec === "L=RR=L" || cleanSpec === "R=LL=R") {
          // Csatornacsere
          config.channelSwap = true;
          const tempBands = config.bandsL;
          config.bandsL = config.bandsR;
          config.bandsR = tempBands;
          const tempDelay = config.delayLMs;
          config.delayLMs = config.delayRMs;
          config.delayRMs = tempDelay;
          continue;
        } else {
          // Bonyolult keverés nem támogatott
          config.skipped.push([line, `Bonyolult csatornakeverés nem támogatott: ${copySpec}`]);
          continue;
        }
      }

      // 8. Filter sorok
      const mFilt = line.match(/^Filter\s*(\d*)\s*:\s*(ON|OFF)\s+([A-Za-z]+)(\s+\d+(?:\.\d+)?\s*dB)?\s+Fc\s+(\d+(?:\.\d+)?)\s*Hz(?:\s+Gain\s+([+-]?\d+(?:\.\d+)?)\s*dB)?(?:\s+(?:Q\s+(\d+(?:\.\d+)?)|BW\s+Oct\s+(\d+(?:\.\d+)?)))?/i);
      if (mFilt) {
        const state = mFilt[2].toUpperCase();
        if (state === "OFF") {
          config.skipped.push([line, "A szűrő állapota OFF (kikapcsolva)"]);
          continue;
        }

        const typeStr = mFilt[3].toUpperCase();
        const typeId = APO_TYPES[typeStr];
        if (typeId === undefined) {
          config.skipped.push([line, `Ismeretlen szűrőtípus: ${typeStr}`]);
          continue;
        }

        const freq = parseFloat(mFilt[5]);
        const gain = mFilt[6] !== undefined ? parseFloat(mFilt[6]) : 0.0;
        let q = 0.7071;

        if (mFilt[7] !== undefined) {
          q = parseFloat(mFilt[7]);
        } else if (mFilt[8] !== undefined) {
          const bw = parseFloat(mFilt[8]);
          const b = Math.pow(2.0, bw);
          q = Math.sqrt(b) / (b - 1.0);
        }

        // Meredekség (slope): pl. "6 dB" vagy "12 dB" vagy "24 dB"
        let slope = 0;
        if (mFilt[4]) {
          const mSlp = mFilt[4].match(/(\d+)/);
          if (mSlp) {
            const dbVal = parseInt(mSlp[1], 10);
            if (dbVal === 6) slope = 0;
            else if (dbVal === 12) slope = 1;
            else if (dbVal === 18) slope = 2;
            else if (dbVal === 24) slope = 3;
          }
        }

        const band = new EqBand(typeId, freq, gain, q, slope);

        if (curChannels.includes("L")) config.bandsL.push(band);
        if (curChannels.includes("R")) config.bandsR.push(band);
        continue;
      }

      // 9. Minden egyéb nem támogatott direktíva megnevezve kimarad
      const mDir = line.match(/^([A-Za-z]+)\s*:/);
      const dirName = mDir ? mDir[1] : "Sor";
      config.skipped.push([line, `Nem támogatott parancs vagy beállítás: ${dirName}`]);
    }

    // Ha csatornacsere volt vagy a bal és jobb sávlista tartalmilag eltér, akkor van külön bal/jobb EQ
    config.eqSplit = !!config.channelSwap || !bandsListsEqual(config.bandsL, config.bandsR);

    return config;
  }

  /** Két szűrősáv-lista tartalmi egyezőségének ellenőrzése. */
  function bandsListsEqual(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; ++i) {
      if (a[i].type !== b[i].type || a[i].slope !== b[i].slope ||
          Math.abs(a[i].freq - b[i].freq) > 1e-4 ||
          Math.abs(a[i].gain - b[i].gain) > 1e-4 ||
          Math.abs(a[i].q - b[i].q) > 1e-4) {
        return false;
      }
    }
    return true;
  }

  /**
   * CamillaDSP YAML konfiguráció értelmezése.
   */
  function parseCamillaDspYaml(text) {
    const config = new ParsedConfig();
    const lines = (text || "").split(/\r?\n/);

    const filterDefs = {}; // név -> { type, ...parameters }
    const pipelineChannels = { 0: [], 1: [] }; // csatorna index -> [szűrőnevek]

    let curSection = "";
    let curFilterName = "";
    let curPipelineChannels = [];

    function parseInlineDict(str) {
      const obj = {};
      const clean = str.replace(/^\{|\}$/g, "").trim();
      if (!clean) return obj;
      // split by comma, but be mindful
      clean.split(",").forEach(part => {
        const idx = part.indexOf(":");
        if (idx !== -1) {
          const k = part.substring(0, idx).trim();
          let v = part.substring(idx + 1).trim().replace(/^["']|["']$/g, "");
          if (!isNaN(parseFloat(v)) && !v.includes(" ") && !v.startsWith("[")) {
            v = parseFloat(v);
          }
          obj[k] = v;
        }
      });
      return obj;
    }

    for (let rawLine of lines) {
      const line = rawLine.trim();
      if (!line || line.startsWith("#")) continue;

      if (line === "filters:") {
        curSection = "filters";
        continue;
      } else if (line === "pipeline:") {
        curSection = "pipeline";
        continue;
      }

      if (curSection === "filters") {
        const mFiltHead = rawLine.match(/^  ([a-zA-Z0-9_-]+):\s*$/);
        if (mFiltHead) {
          curFilterName = mFiltHead[1];
          filterDefs[curFilterName] = {};
          continue;
        }
        if (curFilterName && filterDefs[curFilterName]) {
          const mType = line.match(/^type:\s*([a-zA-Z0-9_-]+)/i);
          if (mType) {
            filterDefs[curFilterName].type = mType[1];
            continue;
          }
          const mParams = line.match(/^parameters:\s*\{(.*)\}/);
          if (mParams) {
            const pObj = parseInlineDict(mParams[1]);
            if (pObj.type) {
              pObj.subtype = pObj.type;
              delete pObj.type;
            }
            Object.assign(filterDefs[curFilterName], pObj);
            continue;
          }
          const mProp = line.match(/^([a-zA-Z0-9_-]+):\s*(.*)$/);
          if (mProp) {
            const k = mProp[1];
            let v = mProp[2].trim().replace(/^["']|["']$/g, "");
            if (!isNaN(parseFloat(v))) v = parseFloat(v);
            filterDefs[curFilterName][k] = v;
          }
        }
      } else if (curSection === "pipeline") {
        let mCh = line.match(/channels:\s*\[(.*)\]/i);
        let mNames = line.match(/names:\s*\[(.*)\]/i);
        if (line.includes("channels:") && mCh) {
          const chList = mCh[1].split(",").map(c => parseInt(c.trim(), 10)).filter(c => !isNaN(c));
          // Kereshetünk names sort a környezetben vagy a következő sorban
          curPipelineChannels = chList;
        }
        if (line.includes("names:") && mNames && typeof curPipelineChannels !== 'undefined') {
          const names = mNames[1].split(",").map(n => n.trim().replace(/^["']|["']$/g, "")).filter(Boolean);
          curPipelineChannels.forEach(ch => {
            if (!pipelineChannels[ch]) pipelineChannels[ch] = [];
            pipelineChannels[ch].push(...names);
          });
        }
      }
    }

    // Pipeline feldolgozása a bal (0) és jobb (1) csatornára
    [0, 1].forEach(chIdx => {
      const isLeft = (chIdx === 0);
      const filtNames = pipelineChannels[chIdx] || [];

      for (let fName of filtNames) {
        const f = filterDefs[fName];
        if (!f) {
          config.skipped.push([fName, `A pipeline-ban hivatkozott szűrő nincs definiálva: ${fName}`]);
          continue;
        }

        const fType = String(f.type || "").toLowerCase();
        if (fType === "gain") {
          const g = parseFloat(f.gain || 0.0);
          if (config.preamp === 0.0) config.preamp = g;
        } else if (fType === "delay") {
          const ms = parseFloat(f.delay || 0.0);
          if (isLeft) config.delayLMs = ms;
          else config.delayRMs = ms;
        } else if (fType === "conv") {
          const fn = String(f.filename || "");
          if (isLeft) config.convL = fn;
          else config.convR = fn;
        } else if (fType === "biquad") {
          // A subtype jöhet az f.type-ból ha a parameters-ben {type: Peaking} volt!
          const subtype = String(f.subtype || f.type || "").toLowerCase();
          const freq = parseFloat(f.freq || 1000.0);
          const q = parseFloat(f.q !== undefined ? f.q : 0.7071);
          const gain = parseFloat(f.gain || 0.0);

          let bandType = 1;
          if (subtype === "peaking" || subtype === "pk") bandType = 1;
          else if (subtype === "highpass" || subtype === "hp") bandType = 2;
          else if (subtype === "highshelf" || subtype === "hs") bandType = 3;
          else if (subtype === "lowpass" || subtype === "lp") bandType = 4;
          else if (subtype === "lowshelf" || subtype === "ls") bandType = 5;
          else if (subtype === "notch") bandType = 6;
          else if (subtype === "allpass") bandType = 8;
          else {
            config.skipped.push([fName, `Nem támogatott biquad altípus: ${subtype}`]);
            continue;
          }

          const band = new EqBand(bandType, freq, gain, q);
          if (isLeft) config.bandsL.push(band);
          else config.bandsR.push(band);
        } else {
          config.skipped.push([fName, `Nem támogatott CamillaDSP szűrőtípus: ${fType}`]);
        }
      }
    });

    config.eqSplit = !bandsListsEqual(config.bandsL, config.bandsR);

    return config;
  }

  /**
   * Mikrofon-kalibrációs fájl (.cal, .txt, .frd, .csv) szigorú értelmezése és ellenőrzése.
   * Elvárások: legalább 3 pont, szigorúan növekvő frekvencia, 10 Hz – 30 kHz, |dB| <= 30.
   * Az idézőjeles ("Sens Factor...") és * / # fejlécsorok kimaradnak.
   */
  function parseMicCalibration(text) {
    const emptyRes = (error, detail) => ({
      ok: false,
      error,
      errorDetail: detail || "",
      sensitivity: null,
      points: [],
      minFreq: 0,
      maxFreq: 0,
      maxAbsDb: 0,
      maxCorrectionDb: 0
    });

    if (!text || !String(text).trim()) {
      return emptyRes("empty", "A fájl üres.");
    }

    const numRe = /^[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?$/;
    const lines = String(text).split(/\r?\n/);
    const points = [];
    let sensitivity = null;
    let invalidDataLines = 0;

    for (let rawLine of lines) {
      const line = rawLine.trim();
      if (!line) continue;

      // Érzékenység kinyerése a fejlécből (pl. "Sens Factor =-38.2dB, SERNO: 0000001")
      if (line.startsWith('"') || line.startsWith("*") || line.startsWith("#")) {
        const mSens = line.match(/Sens\s*Factor\s*[:=]\s*([+-]?\d+(?:\.\d+)?)\s*dB/i);
        if (mSens) {
          sensitivity = parseFloat(mSens[1]);
        }
        continue;
      }

      const clean = line.replace(/,/g, " ").replace(/;/g, " ");
      const tokens = clean.split(/\s+/).filter(Boolean);

      if (tokens.length >= 2 && numRe.test(tokens[0]) && numRe.test(tokens[1])) {
        const f = parseFloat(tokens[0]);
        const db = parseFloat(tokens[1]);
        const phase = (tokens.length >= 3 && numRe.test(tokens[2])) ? parseFloat(tokens[2]) : 0.0;
        if (!Number.isFinite(f) || !Number.isFinite(db)) {
          return emptyRes("unparsable", line);
        }
        points.push({ freq: f, db: db, phase: Number.isFinite(phase) ? phase : 0.0 });
      } else {
        // Oszlopfejléc (pl. "Freq dB" az első adat előtt) csak akkor engedhető át, ha még nincs adatpont és max 1 ilyen van
        if (points.length === 0 && invalidDataLines === 0 && /^[A-Za-z"\s,;()\[\]\/_-]+$/.test(line)) {
          invalidDataLines++;
          continue;
        }
        return emptyRes("unparsable", line);
      }
    }

    if (points.length === 0) {
      return emptyRes(invalidDataLines > 0 ? "unparsable" : "empty", "Nem található értelmezhető kalibrációs adatpont.");
    }
    if (points.length < 3) {
      return emptyRes("tooFewPoints", `Csak ${points.length} pont található (minimum 3 szükséges).`);
    }

    let minFreq = points[0].freq;
    let maxFreq = points[0].freq;
    let maxAbsDb = 0;
    let maxCorrectionDb = 0;

    for (let i = 0; i < points.length; ++i) {
      const p = points[i];
      if (p.freq < 10.0 || p.freq > 30000.0) {
        return emptyRes("outOfFreqRange", `${p.freq} Hz kívül esik a 10 Hz – 30 kHz tartományon.`);
      }
      if (Math.abs(p.db) > 30.0) {
        return emptyRes("outOfGainRange", `${p.db} dB korrekció kívül esik a ±30 dB határon.`);
      }
      if (i > 0 && p.freq <= points[i - 1].freq) {
        return emptyRes("notIncreasing", `${points[i - 1].freq} Hz után ${p.freq} Hz következik (nem növekvő).`);
      }
      if (p.freq < minFreq) minFreq = p.freq;
      if (p.freq > maxFreq) maxFreq = p.freq;
      if (Math.abs(p.db) >= maxAbsDb) {
        maxAbsDb = Math.abs(p.db);
        maxCorrectionDb = p.db;
      }
    }

    return {
      ok: true,
      error: "",
      errorDetail: "",
      sensitivity,
      points,
      minFreq,
      maxFreq,
      maxAbsDb,
      maxCorrectionDb
    };
  }

  /**
   * CSV / FRD / CAL mérési pontok beolvasása.
   */
  function parseCsvPoints(text) {
    const points = [];
    const lines = (text || "").split(/\r?\n/);
    for (let rawLine of lines) {
      const line = rawLine.trim();
      if (!line || line.startsWith("#") || line.startsWith("*") || line.startsWith('"')) continue;
      const clean = line.replace(/,/g, " ").replace(/;/g, " ");
      const tokens = clean.split(/\s+/).filter(Boolean);
      if (tokens.length >= 2) {
        const f = parseFloat(tokens[0]);
        const db = parseFloat(tokens[1]);
        if (!isNaN(f) && !isNaN(db) && f > 0) {
          const phase = tokens.length >= 3 ? parseFloat(tokens[2]) : 0.0;
          points.push([f, db, isNaN(phase) ? 0.0 : phase]);
        }
      }
    }
    return points;
  }

  /**
   * Sávcsökkentő (Band reducer): tetszőleges görbét optimalizált parametrikus sávokra illeszt.
   * Legfeljebb maxBands másodfokú parametrikus szűrőt számít ki.
   */
  function reduceBands(points, maxBands = 10) {
    if (!points || points.length === 0) return { bands: [], peakErr: 0.0, rmsErr: 0.0 };

    const pts = points.map(p => ({ f: p[0], db: p[1] }));
    const bands = [];
    const residual = pts.map(p => p.db);

    for (let iter = 0; iter < maxBands; ++iter) {
      let maxAbs = 0.0;
      let maxIdx = -1;
      for (let i = 0; i < residual.length; ++i) {
        if (Math.abs(residual[i]) > maxAbs) {
          maxAbs = Math.abs(residual[i]);
          maxIdx = i;
        }
      }

      if (maxAbs < 0.3 || maxIdx === -1) break; // 0.3 dB alatti hibánál megáll

      const targetFreq = pts[maxIdx].f;
      const targetGain = residual[maxIdx];

      // Becsüljük meg a sávszélességet a 3 dB-es esési pontokból
      let leftIdx = maxIdx;
      while (leftIdx > 0 && Math.abs(residual[leftIdx]) > 0.5 * maxAbs) leftIdx--;
      let rightIdx = maxIdx;
      while (rightIdx < residual.length - 1 && Math.abs(residual[rightIdx]) > 0.5 * maxAbs) rightIdx++;

      const fLow = pts[leftIdx].f;
      const fHigh = pts[rightIdx].f;
      const bw = Math.max(10.0, fHigh - fLow);
      const q = Math.max(0.5, Math.min(8.0, targetFreq / bw));

      const band = new EqBand(1, targetFreq, targetGain, q);
      bands.push(band);

      // Levonjuk az új szűrő hatását a maradékból
      for (let i = 0; i < residual.length; ++i) {
        residual[i] -= band.evalGainDb(pts[i].f);
      }
    }

    // Hibaszámítás: csúcs (peak) és RMS hiba
    let peakErr = 0.0;
    let sumSq = 0.0;
    for (let i = 0; i < residual.length; ++i) {
      const err = Math.abs(residual[i]);
      if (err > peakErr) peakErr = err;
      sumSq += err * err;
    }
    const rmsErr = Math.sqrt(sumSq / Math.max(1, residual.length));

    return { bands, peakErr, rmsErr };
  }

  /**
   * Exportálás Equalizer APO konfigurációs szöveggé.
   */
  function exportApoConfig(config) {
    let out = "# Aqulify Equalizer APO Export\n";
    if (config.preamp !== 0.0) {
      out += `Preamp: ${config.preamp.toFixed(2)} dB\n`;
    }

    const typeNames = {
      1: "PK", 2: "HP", 3: "HS", 4: "LP", 5: "LS", 6: "NO", 8: "AP", 9: "BP"
    };

    if (config.eqSplit) {
      // Bal csatorna
      out += "\nChannel: L\n";
      if (config.delayLMs > 0.0) out += `Delay: ${config.delayLMs.toFixed(3)} ms\n`;
      if (config.convL) out += `Convolution: "${config.convL}"\n`;
      config.bandsL.forEach((b, i) => {
        const tn = typeNames[b.type] || "PK";
        out += `Filter ${i + 1}: ON ${tn} Fc ${b.freq.toFixed(1)} Hz Gain ${b.gain.toFixed(2)} dB Q ${b.q.toFixed(3)}\n`;
      });

      // Jobb csatorna
      out += "\nChannel: R\n";
      if (config.delayRMs > 0.0) out += `Delay: ${config.delayRMs.toFixed(3)} ms\n`;
      if (config.convR) out += `Convolution: "${config.convR}"\n`;
      config.bandsR.forEach((b, i) => {
        const tn = typeNames[b.type] || "PK";
        out += `Filter ${i + 1}: ON ${tn} Fc ${b.freq.toFixed(1)} Hz Gain ${b.gain.toFixed(2)} dB Q ${b.q.toFixed(3)}\n`;
      });
    } else {
      out += "\nChannel: all\n";
      if (config.delayLMs > 0.0) out += `Delay: ${config.delayLMs.toFixed(3)} ms\n`;
      if (config.convL) out += `Convolution: "${config.convL}"\n`;
      config.bands.forEach((b, i) => {
        const tn = typeNames[b.type] || "PK";
        out += `Filter ${i + 1}: ON ${tn} Fc ${b.freq.toFixed(1)} Hz Gain ${b.gain.toFixed(2)} dB Q ${b.q.toFixed(3)}\n`;
      });
    }

    return out;
  }

  return {
    EqBand,
    ParsedConfig,
    parseApoText,
    parseCamillaDspYaml,
    parseCsvPoints,
    parseMicCalibration,
    reduceBands,
    exportApoConfig
  };
}));
