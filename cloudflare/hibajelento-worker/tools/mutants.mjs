#!/usr/bin/env node
// Mutánsok a szerveroldali tisztításhoz (szabalyok-mi.md 3. pont: személyes adat tisztítása).
// Minden mutáns egy szándékos, csendes hibát tesz a forrásba; a teszteknek meg kell ölniük (vitest ≠ 0).
// A forrás minden futás után visszaáll (akkor is, ha a futás megszakad). Futtatás: npm run mutants
import { execSync } from "node:child_process";
import { readFileSync, writeFileSync } from "node:fs";

const MUTANTS = [
  ["M01", "src/scrub.ts", "e-mail-tisztítás kimarad", "  s = scrubEmails(s);\n", ""],
  ["M02", "src/scrub.ts", "MAC: csak kettőspontos elválasztás", 'let ok = sep === ":" || sep === "-" || sep === "_";', 'let ok = sep === ":";'],
  ["M03", "src/scrub.ts", "IPv4: a 192.x is „helyi”", "oct[0] === 127 ||", "oct[0] === 127 || oct[0] === 192 ||"],
  ["M04", "src/scrub.ts", "kernelverzió-kivétel: minden „-betű” utáni cím marad (ragos cím szivárog)",
    'kernel = k + 1 < s.length && s[k] === "-" && isAsciiAlnum(code(s, k + 1));', "kernel = true;"],
  ["M05", "src/scrub.ts", "IPv6: a rövidített (::) alak nem cím", "(dbl || letter || colons >= 3)", "(colons >= 3 && !dbl)"],
  ["M06", "src/scrub.ts", "Windows-útvonal: csak C:\\ alak, a C:/ nem", '(s[i + 2] === "\\\\" || s[i + 2] === "/")', '(s[i + 2] === "\\\\")'],
  ["M07", "src/scrub.ts", "UNC-útvonal kimarad", 'if (c === "\\\\" && pathStartOk(s, i) &&', 'if (false && pathStartOk(s, i) &&'],
  ["M08", "src/scrub.ts", "~/ útvonal kimarad", '(c === "/" || (c === "~" && s[i + 1] === "/"))', '(c === "/")'],
  ["M09", "src/scrub.ts", "az útvonal a szóköznél véget ér (a fájlnév szivárog)", 'c === "\\t") break;', 'c === "\\t" || c === " ") break;'],
  ["M10", "src/scrub.ts", "a kulcsok nem tisztulnak", "const nk = scrubText(k);", "const nk = k;"],
  ["M11", "src/scrub.ts", "a tömbök nem tisztulnak", "if (Array.isArray(v)) return v.map((x) => scrubValue(x, stats));", "if (Array.isArray(v)) return v;"],
  ["M12", "src/scrub.ts", "a helyi URL utáni útvonal megmarad", "if (pe - i > 1) {", "if (false) {"],
  ["M13", "src/scrub.ts", "e-mail: a pont és a + nem a név része (a név eleje szivárog)", '"._%+-".includes(s[l - 1])', '"_%-".includes(s[l - 1])'],
  ["M14", "src/index.ts", "a tisztítatlan jelentés kerül a D1-be", "const body = JSON.stringify(clean);", "const body = JSON.stringify(v.report);"],
  ["M15", "src/scrub.ts", "[WS Bridge] eszköznevek tisztítása kimarad", "  s = scrubBridgeDeviceNames(s);\n", ""],
  ["M16", "src/scrub.ts", "DMI-lánc tisztítása kimarad", "  s = scrubDmiStrings(s);\n", ""],
  ["M17", "src/scrub.ts", "USB-útvonal kivétel kimarad az IPv4-ből", "if (ok && parts === 4 && endOk && !kernel && !usbPath)", "if (ok && parts === 4 && endOk && !kernel)"],
];

let failed = 0;
for (const [id, file, desc, find, repl] of MUTANTS) {
  const orig = readFileSync(file, "utf8");
  const n = orig.split(find).length - 1;
  if (n !== 1) {
    console.log(`${id} NEM ALKALMAZHATÓ (${n} találat): ${desc}`);
    failed++;
    continue;
  }
  writeFileSync(file, orig.replace(find, repl));
  let killed = false;
  try {
    execSync("npx vitest run", { stdio: "ignore" });
  } catch {
    killed = true;
  } finally {
    writeFileSync(file, orig);
  }
  console.log(`${id} ${killed ? "megölve" : "TÚLÉLTE"}: ${desc}`);
  if (!killed) failed++;
}
// Az eredeti forrással a tesztek zöldek (a mutánsokat nem egy eleve piros futás „ölte meg”)
execSync("npx vitest run", { stdio: "ignore" });
console.log(failed ? `HIBA: ${failed} mutáns nem halt meg vagy nem alkalmazható` : `Mind a ${MUTANTS.length} mutáns megölve; az eredeti forrással zöld.`);
process.exit(failed ? 1 : 0);
