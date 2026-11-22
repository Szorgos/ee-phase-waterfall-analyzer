/**
 * Egységteszt a 3D vízesés lefelé folyás élének (wfRowEdgeProfile) viselkedésére.
 * A tényleges gui_integrated_system.html kódot tölti be vm-ben.
 * Futtatás: node tests/test_waterfall_edge.js
 */

const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

const htmlPath = path.join(__dirname, '..', 'gui_integrated_system.html');
const html = fs.readFileSync(htmlPath, 'utf8');

const mPlane = html.match(/const planeDepth = (\d+(?:\.\d+)?);/);
const mProfile = html.match(/const WF_EDGE_START = [\s\S]*?\nfunction wfRowEdgeProfile\([\s\S]*?\n\}/);
assert(mPlane, 'planeDepth nem található a gui_integrated_system.html-ben');
assert(mProfile, 'wfRowEdgeProfile nem található a gui_integrated_system.html-ben');

const ctx = {};
vm.runInNewContext(`const planeDepth = ${mPlane[1]};\n${mProfile[0]}\nthis.wfRowEdgeProfile = wfRowEdgeProfile;\nthis.planeDepth = planeDepth;\nthis.WF_EDGE_START = WF_EDGE_START;\nthis.WF_EDGE_DROP = WF_EDGE_DROP;`, ctx);

const { wfRowEdgeProfile, planeDepth, WF_EDGE_START, WF_EDGE_DROP } = ctx;

// 1. Felfelé folyás: minden sor vízszintes síkon marad, nincs lebukás vagy kifutás
for (let i = 0; i <= 20; i++) {
  const nz = i / 20;
  const p = wfRowEdgeProfile(nz, true);
  assert.strictEqual(p.yDrop, 0, `felfelé módnál yDrop=0 (nz=${nz})`);
  assert.strictEqual(p.ampScale, 1, `felfelé módnál ampScale=1 (nz=${nz})`);
  assert.strictEqual(p.bgBlend, 0, `felfelé módnál bgBlend=0 (nz=${nz})`);
  assert(Math.abs(p.z - (-planeDepth / 2 + planeDepth * nz)) < 1e-9, `felfelé módnál z lineáris (nz=${nz})`);
}

// 2. Lefelé folyás: perem előtti zóna változatlan
for (const nz of [0, 0.25, 0.5, WF_EDGE_START]) {
  const p = wfRowEdgeProfile(nz, false);
  assert.strictEqual(p.yDrop, 0, `perem előtt yDrop=0 (nz=${nz})`);
  assert.strictEqual(p.ampScale, 1, `perem előtt ampScale=1 (nz=${nz})`);
  assert.strictEqual(p.bgBlend, 0, `perem előtt bgBlend=0 (nz=${nz})`);
}

// 3. Lefelé folyás: perem zóna ívesen lebukik, amplitúdó és szín kifut, z monoton és végén +planeDepth/2
let prev = wfRowEdgeProfile(WF_EDGE_START, false);
for (let i = 1; i <= 10; i++) {
  const nz = WF_EDGE_START + (1 - WF_EDGE_START) * (i / 10);
  const cur = wfRowEdgeProfile(nz, false);
  assert(cur.yDrop < prev.yDrop, `yDrop csökken a peremen (nz=${nz})`);
  assert(cur.ampScale < prev.ampScale && cur.ampScale > 0, `ampScale csökken a peremen (nz=${nz})`);
  assert(cur.bgBlend > prev.bgBlend && cur.bgBlend <= 1, `bgBlend nő a peremen (nz=${nz})`);
  assert(cur.z >= prev.z, `z monoton nő (nz=${nz})`);
  prev = cur;
}

const end = wfRowEdgeProfile(1, false);
assert(Math.abs(end.z - planeDepth / 2) < 1e-9, 'az utolsó sor z koordinátája +planeDepth/2 marad');
assert(Math.abs(end.yDrop + WF_EDGE_DROP) < 1e-9, 'az utolsó sor yDrop-ja -WF_EDGE_DROP');

// 4. Határértékek (clamp)
const under = wfRowEdgeProfile(-0.5, false);
const over = wfRowEdgeProfile(1.5, false);
assert.strictEqual(under.z, -planeDepth / 2);
assert.strictEqual(over.z, planeDepth / 2);
assert(Number.isFinite(over.yDrop) && Number.isFinite(over.ampScale) && Number.isFinite(over.bgBlend));

console.log('OK: wfRowEdgeProfile minden tesztje sikeres.');
