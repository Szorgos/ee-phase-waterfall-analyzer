/**
 * Fagyasztott egység- és integrációs tesztek a szobakorrekciós varázsló F1–F4 javításaihoz (T1–T8).
 * A tényleges gui_integrated_system.html kódot tölti be vm-ben, és az i18n/gui/*.json fájlokat ellenőrzi.
 * Futtatás: node tests/test_room_wizard.js
 */

const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

const ROOT = path.join(__dirname, '..');
const HTML_PATH = path.join(ROOT, 'gui_integrated_system.html');
const I18N_DIR = path.join(ROOT, 'i18n', 'gui');
const AqulifyImport = require(path.join(ROOT, 'import_parser.js'));

const LANGS = [
  'hu', 'en', 'es', 'fr', 'zh', 'de', 'it', 'nl', 'pl', 'ru', 'pt', 'ro',
  'el', 'tr', 'fi', 'da', 'nb', 'sv', 'he', 'ja', 'ko', 'cs', 'uk'
];

// --- Minimális DOM + localStorage + WebSocket + Blob környezet a valódi GUI-kód futtatásához ---

class FakeClassList {
  constructor() { this._set = new Set(); }
  toggle(name, force) {
    if (force === undefined) {
      if (this._set.has(name)) { this._set.delete(name); return false; }
      this._set.add(name); return true;
    }
    if (force) { this._set.add(name); return true; }
    this._set.delete(name); return false;
  }
  contains(name) { return this._set.has(name); }
}

class FakeElement {
  constructor(tag, id = '') {
    this.tagName = tag.toUpperCase();
    this.id = id;
    this.style = { cssText: '', display: '', color: '', width: '', visibility: '', borderBottom: '', fontWeight: '' };
    this.dataset = {};
    this.attributes = {};
    this.children = [];
    this.classList = new FakeClassList();
    this._value = '';
    this._textContent = '';
    this.files = [];
    this.onclick = null;
  }
  setAttribute(k, v) { this.attributes[k] = String(v); }
  getAttribute(k) { return this.attributes[k] ?? null; }
  appendChild(child) {
    this.children.push(child);
    if (this.tagName === 'SELECT' && this.children.length === 1 && !this._value) {
      this._value = child.value;
    }
    return child;
  }
  get options() {
    return this.children.filter(c => c.tagName === 'OPTION');
  }
  get selectedIndex() {
    return this.options.findIndex(o => o.value === this._value);
  }
  get value() {
    if (this.tagName === 'SELECT') {
      const opts = this.options;
      if (opts.length === 0) return '';
      if (opts.some(o => o.value === this._value)) return this._value;
      return opts[0].value;
    }
    return this._value;
  }
  set value(v) {
    const s = String(v);
    if (this.tagName === 'SELECT') {
      const opts = this.options;
      if (opts.some(o => o.value === s)) {
        this._value = s;
      } else if (opts.length > 0) {
        this._value = opts[0].value;
      } else {
        this._value = '';
      }
    } else {
      this._value = s;
    }
  }
  get innerHTML() { return this._textContent; }
  set innerHTML(v) {
    if (v === '') {
      this.children = [];
      this._textContent = '';
      if (this.tagName === 'SELECT') this._value = '';
    } else {
      this._textContent = String(v);
    }
  }
  get textContent() { return this._textContent; }
  set textContent(v) { this._textContent = String(v); }
  getContext() {
    return {
      clearRect() {}, beginPath() {}, moveTo() {}, lineTo() {}, stroke() {}, setLineDash() {},
      strokeStyle: '', lineWidth: 1
    };
  }
  click() {
    if (this._onClickCapture) this._onClickCapture(this);
  }
}

function extractFunctionBlock(src, fnSignature) {
  const idx = src.indexOf(fnSignature);
  if (idx < 0) throw new Error('Nem található a forrásban: ' + fnSignature);
  const braceStart = src.indexOf('{', idx);
  let depth = 0;
  for (let i = braceStart; i < src.length; i++) {
    const ch = src[i];
    if (ch === '{') depth++;
    else if (ch === '}') {
      depth--;
      if (depth === 0) return src.slice(idx, i + 1);
    }
  }
  throw new Error('Nem záródó blokk: ' + fnSignature);
}

function extractAudioDevicesBlock(src) {
  const marker = 'if (data && data.type === "audio_devices" && Array.isArray(data.inputs))';
  const idx = src.indexOf(marker);
  if (idx < 0) throw new Error('audio_devices kezelő nem található a gui_integrated_system.html-ben');
  const braceStart = src.indexOf('{', idx);
  let depth = 0;
  for (let i = braceStart; i < src.length; i++) {
    const ch = src[i];
    if (ch === '{') depth++;
    else if (ch === '}') {
      depth--;
      if (depth === 0) return src.slice(idx, i + 1);
    }
  }
  throw new Error('Nem záródó audio_devices blokk');
}

function createHarness() {
  const html = fs.readFileSync(HTML_PATH, 'utf8');
  const huI18n = JSON.parse(fs.readFileSync(path.join(I18N_DIR, 'hu.json'), 'utf8'));

  const evalBiquadSrc = extractFunctionBlock(html, 'function evalBiquad(');
  const curvePeakSrc = extractFunctionBlock(html, 'function curvePeakDbFor(');
  const audioDevicesSrc = extractAudioDevicesBlock(html);

  const rwStart = html.indexOf('var rwStep = 1;');
  const rwEnd = html.indexOf('// Sávcsökkentő (Band Reducer)', rwStart);
  assert(rwStart > 0 && rwEnd > rwStart, 'Szobakorrekciós varázsló JS szakasza nem található');
  const rwSectionSrc = html.slice(rwStart, rwEnd);

  const elements = new Map();
  const makeEl = (tag, id) => {
    const el = new FakeElement(tag, id);
    elements.set(id, el);
    return el;
  };

  // Szükséges DOM elemek létrehozása (a valódi HTML azonosítókkal)
  const micSel = makeEl('select', 'rwMicSelect');
  const defOpt = new FakeElement('option');
  defOpt.value = 'default';
  defOpt.textContent = huI18n['rw.micDefault'] || 'Alapértelmezett mikrofon';
  micSel.appendChild(defOpt);
  micSel.value = 'default';

  makeEl('span', 'rwMicAvailNote');
  makeEl('input', 'rwCalFileInput');
  makeEl('button', 'btnRwClearCal');
  makeEl('span', 'rwCalFileStatus');
  makeEl('div', 'rwStep3CalStatus');
  makeEl('div', 'rwResultCalInfo');

  const chSel = makeEl('select', 'rwChannelSelect');
  for (const v of ['both', 'left', 'right']) {
    const o = new FakeElement('option'); o.value = v; o.textContent = v; chSel.appendChild(o);
  }
  chSel.value = 'both';

  const lvlSlider = makeEl('input', 'rwLevelSlider'); lvlSlider.value = '-20';
  const targetSel = makeEl('select', 'rwTargetSelect');
  const tOpt = new FakeElement('option'); tOpt.value = 'room_balanced'; targetSel.appendChild(tOpt);
  targetSel.value = 'room_balanced';
  const bassSlider = makeEl('input', 'rwBassSlider'); bassSlider.value = '4';
  const trebleSlider = makeEl('input', 'rwTrebleSlider'); trebleSlider.value = '-3';
  const kneeSlider = makeEl('input', 'rwKneeSlider'); kneeSlider.value = '160';
  const smoothSel = makeEl('select', 'rwSmoothingSelect');
  const sOpt = new FakeElement('option'); sOpt.value = 'var'; smoothSel.appendChild(sOpt);
  smoothSel.value = 'var';
  const hrSlider = makeEl('input', 'rwHeadroomSlider'); hrSlider.value = '-4';

  makeEl('div', 'modalRoomWizard');
  for (let i = 1; i <= 5; i++) {
    makeEl('div', 'rwStep' + i);
    makeEl('div', 'rwStepPill' + i);
  }
  makeEl('button', 'btnRwBack');
  makeEl('button', 'btnRwNext');
  makeEl('button', 'btnRwStartSweep');
  makeEl('button', 'btnRwEmergencyStop');
  makeEl('div', 'rwSweepProgressBar');
  makeEl('span', 'rwSweepProgressPct');
  makeEl('span', 'rwSweepStatusText');
  makeEl('span', 'rwPointCountLabel');
  makeEl('span', 'rwPointCountStep4');
  makeEl('button', 'btnRwNextPoint');
  makeEl('div', 'rwPointList');
  makeEl('div', 'rwPointListStep4');
  makeEl('div', 'rwHumanSummaryText');
  makeEl('div', 'rwEqModeRow');
  makeEl('button', 'btnRwModeSplit');
  makeEl('button', 'btnRwModeCommon');
  makeEl('div', 'rwLrLevelDiffNotice');
  makeEl('div', 'rwLegendCommon');
  makeEl('div', 'rwLegendSplit');
  makeEl('div', 'rwFilterListText');
  const canvas = makeEl('canvas', 'rwCurveCanvas'); canvas.width = 660; canvas.height = 180;
  makeEl('input', 'inputPreamp');
  makeEl('button', 'btnRwAbCompare');
  makeEl('div', 'rwApplyResultNotice');
  const abNotice = makeEl('div', 'rwAbLevelNotice');
  abNotice.style.display = 'none';

  const storage = new Map();
  const localStorage = {
    getItem(k) { return storage.has(k) ? storage.get(k) : null; },
    setItem(k, v) { storage.set(k, String(v)); },
    removeItem(k) { storage.delete(k); },
    _map: storage
  };

  const sentMessages = [];
  const savedBlobs = [];

  class FakeBlob {
    constructor(parts, opts) {
      this.parts = parts;
      this.type = opts && opts.type;
      this.text = parts.join('');
      savedBlobs.push(this);
    }
  }

  class FakeFileReader {
    readAsText(file) {
      if (typeof this.onload === 'function') {
        this.onload({ target: { result: file._content } });
      }
    }
  }

  const document = {
    getElementById(id) { return elements.get(id) || null; },
    createElement(tag) {
      const el = new FakeElement(tag);
      return el;
    }
  };

  function t(key, vars) {
    let s = Object.prototype.hasOwnProperty.call(huI18n, key) ? huI18n[key] : key;
    if (vars) {
      for (const [k, v] of Object.entries(vars)) {
        s = s.replace(new RegExp('\\{' + k + '\\}', 'g'), String(v));
      }
    }
    return s;
  }

  function fmtNum(val, dec) {
    if (typeof dec === 'number') return Number(val).toFixed(dec).replace('.', ',');
    return String(val);
  }

  const sandbox = {
    console,
    Math,
    Number,
    String,
    Array,
    Object,
    JSON,
    Date,
    setTimeout,
    clearTimeout,
    document,
    localStorage,
    Blob: FakeBlob,
    URL: { createObjectURL: () => 'blob:fake' },
    FileReader: FakeFileReader,
    WebSocket: { OPEN: 1 },
    pipewireWs: {
      readyState: 1,
      send(msg) { sentMessages.push(JSON.parse(msg)); }
    },
    AqulifyImport,
    aqLang: 'hu',
    currentDecramp: 0,
    currentPreamp: 0.0,
    autoPreampMode: false,
    isEqEnabled: false,
    isLimiterEnabled: false,
    eqSplit: false,
    nodesL_store: [],
    nodesR_store: [],
    lastSentEqSig: '',
    lastSentPreamp: 0,
    t,
    fmtNum,
    alert() {},
    prompt() { return null; },
    syncActiveChannelStore() {},
    normBand(b) {
      return {
        freq: b.freq,
        gain: typeof b.gain === 'number' ? b.gain : (b.gainDb || 0),
        q: b.q || 1.0,
        type: b.type || 1,
        mode: 0,
        slope: 0
      };
    },
    setEqChannels(L, R, split) {
      sandbox.nodesL_store = L || [];
      sandbox.nodesR_store = R || [];
      sandbox.eqSplit = !!split;
    },
    switchEqEditChannel() {},
    setModuleActive(mod, on) {
      if (mod === 'eq') sandbox.isEqEnabled = !!on;
      if (mod === 'limiter') sandbox.isLimiterEnabled = !!on;
    },
    updateEqUI() {},
    drawEq() {},
    eqFullSig() { return 'sig'; },
    setPreampBadge() {},
    sendParamToPipewire() {}
  };

  vm.createContext(sandbox);
  vm.runInContext(
    `${evalBiquadSrc}\n${curvePeakSrc}\n${rwSectionSrc}\n` +
    `function __dispatchAudioDevices(data) {\n${audioDevicesSrc}\n}\n`,
    sandbox
  );

  return { sandbox, elements, storage, sentMessages, savedBlobs, html };
}

const VALID_CAL_90 = [
  '"Sens Factor =-0.7423dB, SERNO: 9951784"',
  '20.0\t+0.80',
  '100.0\t+0.20',
  '1000.0\t0.00',
  '10000.0\t-0.60',
  '20000.0\t-1.20'
].join('\n');

const VALID_CAL_OTHER = [
  '"Sens Factor =-1.5dB"',
  '20.0\t-0.50',
  '1000.0\t0.00',
  '20000.0\t+0.90'
].join('\n');

function runTest(name, fn) {
  try {
    fn();
    console.log(`PASS: ${name}`);
    return true;
  } catch (err) {
    console.error(`FAIL: ${name}\n  ${err && err.message ? err.message : err}`);
    return false;
  }
}

let allOk = true;

// T1: kalibráció betöltése, majd mikrofonváltás olyan mikrofonra, amelyhez nincs mentett
//     -> a kalibráció aktív marad, és az új kulcs alatt is mentve; 3. lépés státusza is mutatja.
allOk = runTest('T1: Kalibráció megmarad mikrofonváltáskor (ha az újhoz nincs mentett)', () => {
  const { sandbox, elements, storage } = createHarness();
  sandbox.__dispatchAudioDevices({
    type: 'audio_devices',
    available: true,
    inputs: [
      { id: 'mic-a', name: 'Mikrofon A' },
      { id: 'mic-b', name: 'UMIK-1' }
    ]
  });

  // Kalibráció betöltése az alapértelmezett mikrofonra
  const inp = elements.get('rwCalFileInput');
  inp.value = 'C:\\fakepath\\99-51784_90deg.txt';
  inp.files = [{ name: '99-51784_90deg.txt', _content: VALID_CAL_90 }];
  sandbox.onRwCalFileLoaded(inp);
  assert(sandbox.rwActiveMicCal && sandbox.rwActiveMicCal.fileName === '99-51784_90deg.txt',
    'A betöltött kalibrációnak aktívnak kell lennie');

  // Váltás mic-b-re (ehhez még nincs mentett kalibráció)
  elements.get('rwMicSelect').value = 'mic-b';
  sandbox.onRwMicChanged();

  assert(sandbox.rwActiveMicCal !== null, 'Mikrofonváltás után rwActiveMicCal nem válhat null-lá');
  assert.strictEqual(sandbox.rwActiveMicCal.fileName, '99-51784_90deg.txt',
    'A kalibráció fájlneve maradjon aktív');
  const savedUnderNewMic = storage.get('aq_mic_cal_mic-b');
  assert(savedUnderNewMic, 'A kalibráció legyen elmentve az új mikrofon kulcsa (aq_mic_cal_mic-b) alá is');
  assert.strictEqual(JSON.parse(savedUnderNewMic).fileName, '99-51784_90deg.txt');

  // 3. lépés egysoros kalibráció állapota (F1/5)
  const step3Status = elements.get('rwStep3CalStatus').textContent;
  assert(step3Status.includes('99-51784_90deg.txt') && step3Status.includes('90°'),
    `3. lépés kalibráció státusza hiányos: ${JSON.stringify(step3Status)}`);
}) && allOk;

// T2: mikrofonváltás olyanra, amelyhez van mentett -> az töltődik be
allOk = runTest('T2: Mentett kalibrációval rendelkező mikrofonra váltáskor a mentett töltődik be', () => {
  const { sandbox, elements, storage } = createHarness();
  sandbox.__dispatchAudioDevices({
    type: 'audio_devices',
    available: true,
    inputs: [
      { id: 'mic-a', name: 'Mikrofon A' },
      { id: 'mic-b', name: 'Mikrofon B' }
    ]
  });

  // mic-b-hez már van mentett kalibráció
  const parsedB = AqulifyImport.parseMicCalibration(VALID_CAL_OTHER, 'micB.cal');
  storage.set('aq_mic_cal_mic-b', JSON.stringify({ fileName: 'micB.cal', parsed: parsedB }));

  // mic-a-n betöltünk egy másikat
  elements.get('rwMicSelect').value = 'mic-a';
  const inp = elements.get('rwCalFileInput');
  inp.files = [{ name: 'micA_90deg.txt', _content: VALID_CAL_90 }];
  sandbox.onRwCalFileLoaded(inp);
  assert.strictEqual(sandbox.rwActiveMicCal.fileName, 'micA_90deg.txt');

  // Váltás mic-b-re -> micB.cal kell betöltődjön
  elements.get('rwMicSelect').value = 'mic-b';
  sandbox.onRwMicChanged();
  assert(sandbox.rwActiveMicCal && sandbox.rwActiveMicCal.fileName === 'micB.cal',
    `Elvárt micB.cal, kapott: ${sandbox.rwActiveMicCal && sandbox.rwActiveMicCal.fileName}`);
}) && allOk;

// T3: ha nincs aktív kalibráció, a fájlmező üres
allOk = runTest('T3: Ha nincs aktív kalibráció, a fájlbeviteli mező üres', () => {
  const { sandbox, elements } = createHarness();
  const inp = elements.get('rwCalFileInput');
  inp.value = 'C:\\fakepath\\beragadt.txt';
  sandbox.rwActiveMicCal = null;
  sandbox.loadRwCalForCurrentMic();
  assert.strictEqual(sandbox.rwActiveMicCal, null);
  assert.strictEqual(inp.value, '', `A fájlmezőnek üresnek kell lennie, de: ${JSON.stringify(inp.value)}`);
  const step3Status = elements.get('rwStep3CalStatus').textContent;
  assert(step3Status.toLowerCase().includes('kalibráció nélkül'),
    `3. lépésnél kalibráció nélkül felirat kell, kapott: ${JSON.stringify(step3Status)}`);
}) && allOk;

// T4: eszközlista-frissítés nem ejti a választott eszközt "default"-ra
allOk = runTest('T4: Eszközlista-frissítés megtartja a választott eszközt "(nem elérhető)" jelzéssel és figyelmeztetéssel', () => {
  const { sandbox, elements } = createHarness();
  const sel = elements.get('rwMicSelect');
  const note = elements.get('rwMicAvailNote');

  // 1. Megérkezik a lista az UMIK-1-gyel, a felhasználó kiválasztja
  sandbox.__dispatchAudioDevices({
    type: 'audio_devices',
    available: true,
    inputs: [{ id: 'umik-1', name: 'UMIK-1' }]
  });
  sel.value = 'umik-1';
  assert.strictEqual(sel.value, 'umik-1');

  // 2. Új lista jön, amiben átmenetileg nincs benne az UMIK-1
  sandbox.__dispatchAudioDevices({
    type: 'audio_devices',
    available: true,
    inputs: [{ id: 'builtin', name: 'Beépített mikrofon' }]
  });
  assert.strictEqual(sel.value, 'umik-1',
    `A kiválasztott eszköz nem eshet vissza default-ra! Kapott: ${sel.value}`);
  const curOpt = sel.options.find(o => o.value === 'umik-1');
  assert(curOpt && curOpt.textContent.includes('nem elérhető'),
    `Az opció jelezze, hogy nem elérhető: ${curOpt && curOpt.textContent}`);
  assert(note.textContent.trim().length > 0, 'Figyelmeztetésnek látszania kell a hiányzó mikrofonról');

  // 3. Később újra megjelenik az UMIK-1 -> figyelmeztetés eltűnik, választás marad
  sandbox.__dispatchAudioDevices({
    type: 'audio_devices',
    available: true,
    inputs: [
      { id: 'builtin', name: 'Beépített mikrofon' },
      { id: 'umik-1', name: 'UMIK-1' }
    ]
  });
  assert.strictEqual(sel.value, 'umik-1');
  assert.strictEqual(note.textContent.trim(), '', 'Ha újra elérhető, a figyelmeztetés tűnjön el');
}) && allOk;

// T5: a mérés eredményében (4. lépés) és mentett JSON-ban szerepel a használt kalibráció neve (vagy "nincs")
allOk = runTest('T5: A 4. lépésben és a mentett JSON-ban szerepel a méréshez használt kalibráció neve / hiánya', () => {
  const { sandbox, elements, sentMessages, savedBlobs } = createHarness();
  const inp = elements.get('rwCalFileInput');
  inp.files = [{ name: '99-51784.txt', _content: VALID_CAL_90 }];
  sandbox.onRwCalFileLoaded(inp);

  // Mérés indítása kalibrációval
  sandbox.startRwSweepMeasurement();
  const startMsg = sentMessages.find(m => m.type === 'start_measurement');
  assert(startMsg && Array.isArray(startMsg.cal_points) && startMsg.cal_points.length === 5,
    'start_measurement küldje el a cal_points tömböt az aktív kalibrációból');

  // Mérés eredménye megérkezik
  sandbox.handleRwMeasurementResult({
    type: 'measurement_result',
    channel: 'both',
    freqs: [20, 100, 1000, 10000],
    measured_dbs: [0, 6, 0, -2],
    target_dbs: [4, 2, 0, -2],
    bands: [{ freq: 100, gainDb: -4.0, q: 2.0, type: 1 }],
    bands_left: [{ freq: 100, gainDb: -4.0, q: 2.0, type: 1 }],
    bands_right: [{ freq: 100, gainDb: -3.5, q: 2.0, type: 1 }],
    human_summary_hu: 'Rendben',
    human_summary_en: 'OK'
  });

  const calInfoWith = elements.get('rwResultCalInfo').textContent;
  assert(calInfoWith.includes('99-51784.txt'),
    `4. lépés mutassa a kalibráció nevét, kapott: ${JSON.stringify(calInfoWith)}`);

  sandbox.saveRwSessionToFile();
  const saved1 = JSON.parse(savedBlobs[savedBlobs.length - 1].text);
  assert.strictEqual(saved1.cal_file, '99-51784.txt',
    `A mentett JSON-ban legyen ott cal_file, kapott: ${JSON.stringify(saved1.cal_file)}`);

  // Újraszámítás (recomputed: true) ne veszítse el a cal_file mezőt
  sandbox.handleRwMeasurementResult({
    type: 'measurement_result',
    recomputed: true,
    channel: 'both',
    freqs: [20, 100, 1000, 10000],
    measured_dbs: [0, 6, 0, -2],
    target_dbs: [6, 2, 0, -2],
    bands: [{ freq: 100, gainDb: -4.0, q: 2.0, type: 1 }],
    human_summary_hu: 'Újraszámolva',
    human_summary_en: 'Recomputed'
  });
  assert(elements.get('rwResultCalInfo').textContent.includes('99-51784.txt'),
    'Recompute után is maradjon meg a kalibráció neve');

  // Mérés kalibráció nélkül
  sandbox.clearRwMicCal();
  sentMessages.length = 0;
  sandbox.startRwSweepMeasurement();
  const startMsgNoCal = sentMessages.find(m => m.type === 'start_measurement');
  assert(startMsgNoCal && !startMsgNoCal.cal_points, 'Kalibráció nélkül ne menjen cal_points');

  sandbox.handleRwMeasurementResult({
    type: 'measurement_result',
    channel: 'both',
    freqs: [20, 100, 1000, 10000],
    measured_dbs: [0, 6, 0, -2],
    target_dbs: [4, 2, 0, -2],
    bands: [{ freq: 100, gainDb: -4.0, q: 2.0, type: 1 }],
    human_summary_hu: 'Rendben',
    human_summary_en: 'OK'
  });

  const calInfoNone = elements.get('rwResultCalInfo').textContent;
  assert(calInfoNone.toLowerCase().includes('kalibráció nélkül'),
    `Kalibráció nélküli mérésnél jelezze a 4. lépésben, kapott: ${JSON.stringify(calInfoNone)}`);
  sandbox.saveRwSessionToFile();
  const saved2 = JSON.parse(savedBlobs[savedBlobs.length - 1].text);
  assert.strictEqual(saved2.cal_file, null,
    `Kalibráció nélküli mentett JSON-ban cal_file legyen null, kapott: ${JSON.stringify(saved2.cal_file)}`);
}) && allOk;

// T6: csak vágó sávok -> preamp 0 dB; emelő sáv -> a mai képlet
allOk = runTest('T6: Csak vágó sávoknál 0 dB preamp és megfelelő üzenet; emelő sávnál tartalékos képlet', () => {
  const { sandbox, elements } = createHarness();
  elements.get('rwHeadroomSlider').value = '-4';

  const cutOnlyL = [
    { type: 1, freq: 135, gain: -13.0, q: 2.65, mode: 0, slope: 0 },
    { type: 1, freq: 280, gain: -10.0, q: 3.5, mode: 0, slope: 0 },
    { type: 1, freq: 1000, gain: -1.5, q: 1.0, mode: 0, slope: 0 }
  ];
  const cutOnlyR = [
    { type: 1, freq: 120, gain: -11.3, q: 4.32, mode: 0, slope: 0 },
    { type: 1, freq: 430, gain: -9.5, q: 3.2, mode: 0, slope: 0 }
  ];

  // Csak vágó (mindkét csatornán) -> 0 dB
  const preCut = sandbox.computeRwPreampDb(cutOnlyL, cutOnlyR);
  assert.strictEqual(preCut, 0, `Csak vágásnál preamp legyen 0 dB, kapott: ${preCut}`);

  // Emelő sáv -> -(csúcs + 4.0)
  const withBoost = [
    { type: 1, freq: 135, gain: -8.0, q: 2.5, mode: 0, slope: 0 },
    { type: 1, freq: 60, gain: 2.5, q: 1.5, mode: 0, slope: 0 }
  ];
  const preBoost = sandbox.computeRwPreampDb(cutOnlyL, withBoost);
  assert(preBoost <= -6.0 && preBoost >= -7.0,
    `+2.5 dB emelés + 4 dB tartaléknál kb. -6.5 dB várható, kapott: ${preBoost}`);

  // Alkalmazás zöld üzenete csak vágásnál
  sandbox.rwMeasuredData = {
    channel: 'both',
    bands: cutOnlyL.map(b => ({ freq: b.freq, gainDb: b.gain, q: b.q, type: 1 })),
    bands_left: cutOnlyL.map(b => ({ freq: b.freq, gainDb: b.gain, q: b.q, type: 1 })),
    bands_right: cutOnlyR.map(b => ({ freq: b.freq, gainDb: b.gain, q: b.q, type: 1 }))
  };
  sandbox.rwEqMode = 'split';
  sandbox.applyRwCorrection();
  assert.strictEqual(sandbox.currentPreamp, 0, 'Alkalmazás után currentPreamp legyen 0');
  const notice = elements.get('rwApplyResultNotice').textContent;
  assert(notice.includes('Csak vágás') && notice.includes('0 dB'),
    `Csak vágásnál az alkalmazási üzenet mondja ki a 0 dB-t, kapott: ${JSON.stringify(notice)}`);
}) && allOk;

// T7: A/B tájékoztató sor csak |X| >= 1 dB-nél jelenik meg, helyes előjellel
allOk = runTest('T7: A/B hangerő-tájékoztató csak |X| >= 1 dB esetén jelenik meg, helyes előjellel', () => {
  const { sandbox, elements } = createHarness();
  const abEl = elements.get('rwAbLevelNotice');

  // 1. Csak 60 Hz mélyvágás -> 200 Hz–5 kHz átlag |X| < 1 dB -> rejtve
  sandbox.rwMeasuredData = {
    channel: 'both',
    bands: [{ freq: 60, gainDb: -8.0, q: 4.0, type: 1 }]
  };
  sandbox.rwEqMode = 'common';
  sandbox.applyRwCorrection();
  assert.strictEqual(abEl.style.display, 'none',
    `Kis 200Hz-5kHz eltérésnél rejtve kell maradnia, de display=${abEl.style.display}, text=${abEl.textContent}`);
  assert.strictEqual(abEl.textContent, '');

  // 2. Erős középtartományi vágás (200 Hz–5 kHz átlag < -1 dB -> halkabb)
  sandbox.rwMeasuredData = {
    channel: 'both',
    bands: [
      { freq: 400, gainDb: -6.0, q: 0.7, type: 1 },
      { freq: 1500, gainDb: -4.0, q: 0.7, type: 1 }
    ]
  };
  sandbox.rwEqMode = 'common';
  sandbox.applyRwCorrection();
  assert(abEl.style.display !== 'none' && abEl.textContent.length > 0,
    'Jelentős középtartományi vágásnál az A/B tájékoztatónak látszania kell');
  assert(abEl.textContent.includes('halkabb') && !abEl.textContent.includes('hangosabb'),
    `Vágásnál "halkabb" szöveg kell, kapott: ${JSON.stringify(abEl.textContent)}`);

  // 3. Előjel-ellenőrzés (közvetlen segédfüggvénnyel is, hogy a negatív és pozitív irány egyértelmű legyen)
  assert.strictEqual(typeof sandbox.computeRwAbMidbandDeltaDb, 'function',
    'computeRwAbMidbandDeltaDb függvénynek léteznie kell');
  const dQuiet = sandbox.computeRwAbMidbandDeltaDb(
    [{ type: 1, freq: 1000, gain: -6.0, q: 0.5, mode: 0, slope: 0 }], null, 0
  );
  assert(dQuiet <= -1.5, `1 kHz -6 dB vágásnál negatív átlag várható, kapott: ${dQuiet}`);

  const dLoud = sandbox.computeRwAbMidbandDeltaDb(
    [{ type: 1, freq: 1000, gain: +6.0, q: 0.5, mode: 0, slope: 0 }], null, 0
  );
  assert(dLoud >= +1.5, `1 kHz +6 dB emelésnél pozitív átlag várható, kapott: ${dLoud}`);

  const msgQuiet = sandbox.formatRwAbLevelNotice(dQuiet);
  const msgLoud = sandbox.formatRwAbLevelNotice(dLoud);
  const msgSmall = sandbox.formatRwAbLevelNotice(-0.5);
  assert(msgQuiet.includes('halkabb') && !msgQuiet.includes('hangosabb'), `Negatív X -> halkabb: ${msgQuiet}`);
  assert(msgLoud.includes('hangosabb') && !msgLoud.includes('halkabb'), `Pozitív X -> hangosabb: ${msgLoud}`);
  assert.strictEqual(msgSmall, '', `|X| < 1 dB -> üres szöveg, kapott: ${msgSmall}`);
}) && allOk;

// T8: rw.experimentalText egyik nyelven sem tartalmazza a "GitHub" szót, és tartalmazza az adott nyelv er.reportBtn feliratát
allOk = runTest('T8: rw.experimentalText GitHub-mentes és tartalmazza er.reportBtn-t mind a 23 nyelven (+ F4 súgó)', () => {
  for (const lang of LANGS) {
    const p = path.join(I18N_DIR, `${lang}.json`);
    const d = JSON.parse(fs.readFileSync(p, 'utf8'));
    const exp = d['rw.experimentalText'] || '';
    const btn = d['er.reportBtn'] || '';
    assert(btn.length > 0, `${lang}: er.reportBtn üres`);
    assert(!/github/i.test(exp), `${lang}: rw.experimentalText még tartalmazza a GitHub szót: ${exp}`);
    assert(exp.includes(btn), `${lang}: rw.experimentalText nem tartalmazza szó szerint er.reportBtn-t (${JSON.stringify(btn)}): ${exp}`);
  }
  // HTML fallback is egyezzen
  const html = fs.readFileSync(HTML_PATH, 'utf8');
  const mExp = html.match(/data-i18n="rw\.experimentalText">([^<]+)</);
  assert(mExp, 'data-i18n="rw.experimentalText" nem található a HTML-ben');
  assert(!/github/i.test(mExp[1]), `HTML fallback még tartalmazza a GitHub szót: ${mExp[1]}`);
  assert(mExp[1].includes('Hibát jelentek'), `HTML fallback nem tartalmazza a "Hibát jelentek" feliratot: ${mExp[1]}`);

  // F4 súgó kiegészítés ellenőrzése hu + en
  const hu = JSON.parse(fs.readFileSync(path.join(I18N_DIR, 'hu.json'), 'utf8'));
  const en = JSON.parse(fs.readFileSync(path.join(I18N_DIR, 'en.json'), 'utf8'));
  assert(hu['rw.helpPointsP'].includes('Zajos szobában') && hu['rw.helpPointsP'].includes('egyetlen mérőpontból'),
    'hu rw.helpPointsP nem tartalmazza a zajos szoba / egy pont mondatot');
  assert(/noisy room/i.test(en['rw.helpPointsP']) && /single/i.test(en['rw.helpPointsP']),
    'en rw.helpPointsP nem tartalmazza a noisy room / single point mondatot');
}) && allOk;

if (!allOk) {
  console.error('\nEREDMÉNY: VAN SIKERTELEN TESZT (PIROS).');
  process.exit(1);
}
console.log('\nEREDMÉNY: MINDEN TESZT (T1–T8) SIKERES (ZÖLD).');
