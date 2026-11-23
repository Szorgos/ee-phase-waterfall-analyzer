/**
 * Szerveroldali, második tisztítás. Az include/error_report.hpp scrub() mintaalapú részének átirata
 * (e-mail, MAC, IPv4, IPv6, fájlútvonal, ismétlődő jelölők összevonása), ugyanazokkal a kivételekkel:
 * - a helyi visszacsatolási cím (127.x, 0.0.0.0, ::1, ws://127.0.0.1:47899, localhost) megmarad;
 * - a négyrészes kernelverzió (6.6.87.2-microsoft-standard-WSL2) nem IP-cím, de a ragos cím (192.168.1.23-hoz) igen.
 *
 * Eltérés a klienstől: a szerver nem ismeri a gép felhasználónevét, gépnevét, otthoni mappáját és a párosított
 * eszközök nevét, ezért ezeket csak akkor tünteti el, ha útvonalban, e-mailben vagy címben vannak. Azokat a kliens
 * tisztítja (ScrubContext).
 *
 * A tisztítás egy-egy (már dekódolt) szövegértéken fut, nem a JSON-szövegen: ez a C++ jsonMode=false útja.
 */

const isAsciiAlnum = (c: number): boolean =>
  (c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122);
const isHex = (c: number): boolean => (c >= 48 && c <= 57) || (c >= 65 && c <= 70) || (c >= 97 && c <= 102);
const isDigit = (c: number): boolean => c >= 48 && c <= 57;
const isAsciiAlpha = (c: number): boolean => (c >= 65 && c <= 90) || (c >= 97 && c <= 122);
/** Szóhatárhoz: ASCII betű/szám, aláhúzás és minden nem ASCII karakter (ékezetes betűk) a szó része. */
const isWordish = (c: number): boolean => isAsciiAlnum(c) || c === 95 || c >= 0x80;

const code = (s: string, i: number): number => s.charCodeAt(i); // NaN a végén: minden összehasonlítás hamis

/** Útvonal vége: sorvég, idézőjel, aposztróf, tabulátor. */
function pathEnd(s: string, i: number): number {
  while (i < s.length) {
    const c = s[i];
    if (c === "\n" || c === "\r" || c === '"' || c === "'" || c === "\t") break;
    ++i;
  }
  return i;
}

function pathStartOk(s: string, i: number): boolean {
  if (i === 0) return true;
  const p = s[i - 1];
  return !(isWordish(code(s, i - 1)) || p === "." || p === "-" || p === "/" || p === "\\" || p === "~");
}

export function scrubPaths(s: string): string {
  let out = "";
  let i = 0;
  while (i < s.length) {
    const c = s[i];
    // Egy már kicserélt <path> folytatása is az útvonal része: <path>/Zene/a.flac
    if (s.startsWith("<path>", i)) {
      out += "<path>";
      i += 6;
      if (i < s.length && (s[i] === "/" || s[i] === "\\")) i = pathEnd(s, i);
      continue;
    }
    // Helyi visszacsatolási URL: a gép és a port marad, az utána jövő útvonal <path>
    if (
      c === "/" &&
      i > 0 &&
      s[i - 1] === ":" &&
      s.startsWith("//", i) &&
      (s.startsWith("127.", i + 2) || s.startsWith("localhost", i + 2) || s.startsWith("[::1]", i + 2))
    ) {
      let e = i + 2;
      while (e < s.length && (isAsciiAlnum(code(s, e)) || ".:[]-".includes(s[e]))) ++e;
      out += s.slice(i, e);
      i = e;
      if (i < s.length && s[i] === "/") {
        const pe = pathEnd(s, i);
        if (pe - i > 1) {
          out += "<path>";
          i = pe;
        }
      }
      continue;
    }
    // Unix: /a/b… vagy ~/…
    if ((c === "/" || (c === "~" && s[i + 1] === "/")) && pathStartOk(s, i)) {
      const e = pathEnd(s, i);
      const cand = s.slice(i, e);
      if (c === "~" || cand.split("/").length - 1 >= 2) {
        out += "<path>";
        i = e;
        continue;
      }
    }
    // Windows: C:\… vagy C:/…
    if (i + 2 < s.length && isAsciiAlpha(code(s, i)) && s[i + 1] === ":" && (s[i + 2] === "\\" || s[i + 2] === "/") && pathStartOk(s, i)) {
      out += "<path>";
      i = pathEnd(s, i + 2);
      continue;
    }
    // UNC: \\gép\megosztás
    if (c === "\\" && pathStartOk(s, i) && i + 2 < s.length && s[i + 1] === "\\" && isWordish(code(s, i + 2))) {
      out += "<path>";
      i = pathEnd(s, i + 2);
      continue;
    }
    out += c;
    ++i;
  }
  return out;
}

export function scrubEmails(s: string): string {
  let at = 0;
  while ((at = s.indexOf("@", at)) !== -1) {
    let l = at;
    while (l > 0 && (isAsciiAlnum(code(s, l - 1)) || "._%+-".includes(s[l - 1]))) --l;
    let r = at + 1;
    let dot = false;
    while (r < s.length && (isAsciiAlnum(code(s, r)) || s[r] === "." || s[r] === "-")) {
      dot ||= s[r] === ".";
      ++r;
    }
    while (r > at + 1 && s[r - 1] === ".") --r; // a mondatvégi pont nem a domain része
    if (l < at && dot && r > at + 3) {
      s = s.slice(0, l) + "<email>" + s.slice(r);
      at = l + 7;
    } else {
      ++at;
    }
  }
  return s;
}

export function scrubMacs(s: string): string {
  let out = "";
  let i = 0;
  while (i < s.length) {
    if (i + 17 <= s.length && (i === 0 || !(isAsciiAlnum(code(s, i - 1)) || s[i - 1] === ":" || s[i - 1] === "-"))) {
      const sep = s[i + 2];
      let ok = sep === ":" || sep === "-" || sep === "_";
      for (let g = 0; ok && g < 6; ++g) {
        const p = i + g * 3;
        ok = isHex(code(s, p)) && isHex(code(s, p + 1)) && (g === 5 || s[p + 2] === sep);
      }
      const e = i + 17;
      if (ok && (e >= s.length || !(isAsciiAlnum(code(s, e)) || s[e] === sep))) {
        out += "<mac>";
        i = e;
        continue;
      }
    }
    out += s[i++];
  }
  return out;
}

export function scrubIpv4(s: string): string {
  let out = "";
  let i = 0;
  while (i < s.length) {
    const c = code(s, i);
    if (isDigit(c) && (i === 0 || !(isWordish(code(s, i - 1)) || s[i - 1] === "."))) {
      let j = i;
      let parts = 0;
      let ok = true;
      const oct = [0, 0, 0, 0];
      while (parts < 4) {
        let k = j;
        let v = 0;
        while (k < s.length && isDigit(code(s, k)) && k - j < 4) v = v * 10 + (code(s, k++) - 48);
        if (k === j || k - j > 3 || v > 255) {
          ok = false;
          break;
        }
        oct[parts] = v;
        ++parts;
        j = k;
        if (parts < 4) {
          if (j < s.length && s[j] === ".") ++j;
          else {
            ok = false;
            break;
          }
        }
      }
      const endOk = j >= s.length || !(isWordish(code(s, j)) || (s[j] === "." && isDigit(code(s, j + 1))));
      // Négyrészes kernelverzió: a négyes után „-szó-” jön. A ragos cím (192.168.1.23-hoz) után nincs második kötőjel.
      let kernel = false;
      if (ok && parts === 4 && j + 1 < s.length && s[j] === "-" && isAsciiAlpha(code(s, j + 1))) {
        let k = j + 1;
        while (k < s.length && isAsciiAlnum(code(s, k))) ++k;
        kernel = k + 1 < s.length && s[k] === "-" && isAsciiAlnum(code(s, k + 1));
      }
      // Linux USB-útvonal (usb-0000:00:14.0-4.1.2.3): a kötőjel előtt „usb-<pci>-” áll (benne legalább egy ':')
      let usbPath = false;
      if (ok && parts === 4 && i >= 6 && s[i - 1] === "-") {
        let p = i - 1;
        let hasColon = false;
        while (p > 0) {
          const ch = code(s, p - 1);
          if (isHex(ch) || ch === 58 || ch === 46) {
            if (ch === 58) hasColon = true;
            --p;
          } else {
            break;
          }
        }
        if (hasColon && p >= 4 && s.slice(p - 4, p).toLowerCase() === "usb-") {
          usbPath = true;
        }
      }
      if (ok && parts === 4 && endOk && !kernel && !usbPath) {
        const loopback = oct[0] === 127 || (oct[0] === 0 && oct[1] === 0 && oct[2] === 0 && oct[3] === 0);
        out += loopback ? s.slice(i, j) : "<ip>";
        i = j;
        continue;
      }
      // nem cím: a számjegysor egyben megy tovább
      let k = i;
      while (k < s.length && (isDigit(code(s, k)) || s[k] === ".")) ++k;
      out += s.slice(i, k);
      i = k;
      continue;
    }
    out += s[i++];
  }
  return out;
}

export function scrubIpv6(s: string): string {
  let out = "";
  let i = 0;
  const inRun = (c: number): boolean => isHex(c) || c === 58; // ':'
  while (i < s.length) {
    const c = code(s, i);
    if (inRun(c) && (i === 0 || !(isWordish(code(s, i - 1)) || s[i - 1] === "." || s[i - 1] === ":"))) {
      let j = i;
      while (j < s.length && inRun(code(s, j))) ++j;
      const run = s.slice(i, j);
      const colons = run.split(":").length - 1;
      const dbl = run.includes("::");
      const letter = /[A-Za-z]/.test(run);
      const hexDigit = /[0-9A-Fa-f]/.test(run);
      let e = j;
      if (s[e] === "%") {
        // zónaazonosító (fe80::1%eth0)
        ++e;
        while (e < s.length && isWordish(code(s, e))) ++e;
      }
      const endOk = e >= s.length || !(isWordish(code(s, e)) || (s[e] === "." && isDigit(code(s, e + 1))));
      const looksV6 = colons >= 2 && hexDigit && (dbl || letter || colons >= 3) && endOk;
      if (looksV6 && run !== "::1" && run !== "::") {
        out += "<ip>";
        i = e;
        continue;
      }
      out += run;
      i = j;
      continue;
    }
    out += s[i++];
  }
  return out;
}

/** A [WS Bridge] naplósorokban szereplő távoli/párosított eszköznevek cseréje <name>-re. */
export function scrubBridgeDeviceNames(s: string): string {
  const rules: Array<{ prefix: string; untilParen: boolean }> = [
    { prefix: "[WS Bridge] Új eszköz párosítva: ", untilParen: false },
    { prefix: "[WS Bridge] Távoli eszköz csatlakozott Bluetoothon: ", untilParen: false },
    { prefix: "[WS Bridge] Távoli eszköz csatlakozott: ", untilParen: false },
    { prefix: "[WS Bridge] Távoli eszköz jóváhagyva és csatlakozott: ", untilParen: false },
    { prefix: " korábbi párosítás lecserélve: ", untilParen: false },
    { prefix: "[WS Bridge] Távoli eszköz lecsatlakozott: ", untilParen: true },
    { prefix: "[WS Bridge] Párosítási kérelem: ", untilParen: true },
  ];
  for (const r of rules) {
    let pos = 0;
    while ((pos = s.indexOf(r.prefix, pos)) !== -1) {
      const start = pos + r.prefix.length;
      let lineEnd = start;
      while (lineEnd < s.length && s[lineEnd] !== "\n" && s[lineEnd] !== "\r") ++lineEnd;
      let end = lineEnd;
      if (r.untilParen) {
        const p = s.indexOf(" (", start);
        if (p !== -1 && p < lineEnd) end = p;
      }
      if (end > start) {
        const cur = s.slice(start, end);
        if (cur !== "<name>") {
          s = s.slice(0, start) + "<name>" + s.slice(end);
          pos = start + 6;
          continue;
        }
      }
      pos = start;
    }
  }
  return s;
}

/**
 * Többtagú, nagybetűs DMI/sorozatszám-szerű gépazonosítók (pl. ALSA SOF kártyák longname mezője:
 * ACMECOMPUTERLIMITED-NOTEBOOK1234-99887766554-ABCD1E2) cseréje <dmi>-re.
 * A 3 tagú alak is <dmi> (SOF longname: GYARTO-TERMEKKOD-TermekNevVerzio, pl. ACME-12B34567XY-ExampleBookPro9Gen2i):
 * csupa nagybetűs első tag (>=3), a 2. tagban >=6 számjegy, a 3. tag >=8 hosszú és kis- és nagybetűt is tartalmaz,
 * összhossz >= 20. A mintának a kliens (include/error_report.hpp) megfelelőjével azonosnak kell maradnia.
 */
export function scrubDmiStrings(s: string): string {
  let out = "";
  let i = 0;
  while (i < s.length) {
    const c = code(s, i);
    const startOk =
      c >= 65 &&
      c <= 90 &&
      (i === 0 || !(isWordish(code(s, i - 1)) || s[i - 1] === "-" || s[i - 1] === "."));
    if (startOk) {
      let j = i;
      let parts = 1;
      let curStart = i;
      let firstUpperOnly = true;
      let firstLen = 0;
      let hasLongSerial = false;
      let valid = true;
      let secondDigits = 0; // a 2. tag számjegyeinek száma (3 tagú alak)
      let lastLen = 0; // az utolsó tag hossza, és hogy van-e benne kis- / nagybetű
      let lastLower = false;
      let lastUpper = false;
      const finishPart = (a: number, b: number, partIdx: number) => {
        const len = b - a;
        if (len === 0) {
          valid = false;
          return;
        }
        let digits = 0;
        let upperOnly = true;
        let hasLower = false;
        let hasUpper = false;
        for (let k = a; k < b; ++k) {
          const ch = code(s, k);
          if (isDigit(ch)) {
            ++digits;
            upperOnly = false;
          } else if (ch >= 65 && ch <= 90) {
            hasUpper = true;
          } else {
            upperOnly = false;
            if (ch >= 97 && ch <= 122) hasLower = true;
          }
        }
        if (partIdx === 1) {
          firstUpperOnly = upperOnly;
          firstLen = len;
        }
        if (partIdx === 2) secondDigits = digits;
        lastLen = len;
        lastLower = hasLower;
        lastUpper = hasUpper;
        if (digits >= 6) hasLongSerial = true;
      };
      while (j < s.length) {
        const ch = code(s, j);
        if (isAsciiAlnum(ch) || ch === 95) {
          ++j;
        } else if (s[j] === "-" && j + 1 < s.length && (isAsciiAlnum(code(s, j + 1)) || s[j + 1] === "_")) {
          finishPart(curStart, j, parts);
          ++parts;
          ++j;
          curStart = j;
        } else {
          break;
        }
      }
      finishPart(curStart, j, parts);
      const endOk = j >= s.length || !(isWordish(code(s, j)) || s[j] === "." || s[j] === ":");
      const fourPlus = parts >= 4 && hasLongSerial;
      const three = parts === 3 && secondDigits >= 6 && lastLen >= 8 && lastLower && lastUpper;
      if (valid && endOk && (fourPlus || three) && j - i >= 20 && firstUpperOnly && firstLen >= 3) {
        out += "<dmi>";
        i = j;
        continue;
      }
      out += s.slice(i, j);
      i = j;
      continue;
    }
    out += s[i++];
  }
  return out;
}

export function collapseRepeats(s: string): string {
  for (const tok of ["<path>", "<ip>", "<mac>", "<name>", "<dmi>"]) {
    const twice = tok + tok;
    while (s.includes(twice)) s = s.split(twice).join(tok);
  }
  return s;
}

/** Egy szöveg tisztítása (a C++ scrub() 2–4. lépésének mintaalapú része, ugyanabban a sorrendben). */
export function scrubText(text: string): string {
  let s = text;
  s = scrubBridgeDeviceNames(s);
  s = scrubDmiStrings(s);
  s = scrubEmails(s);
  s = scrubMacs(s);
  s = scrubIpv4(s);
  s = scrubIpv6(s);
  s = scrubPaths(s);
  s = collapseRepeats(s);
  return s;
}

/**
 * Egy JSON-érték minden szövege (a kulcsok is) tisztítva. A számok, logikai értékek, null változatlanok.
 * changes: hány szöveg változott (ha a kliens jól tisztított, 0).
 */
export function scrubValue(v: unknown, stats: { changes: number } = { changes: 0 }): unknown {
  if (typeof v === "string") {
    const t = scrubText(v);
    if (t !== v) stats.changes++;
    return t;
  }
  if (Array.isArray(v)) return v.map((x) => scrubValue(x, stats));
  if (v !== null && typeof v === "object") {
    // Object.fromEntries: saját mezőt hoz létre („__proto__” kulcsnál sem állít prototípust)
    return Object.fromEntries(
      Object.entries(v as Record<string, unknown>).map(([k, x]) => {
        const nk = scrubText(k);
        if (nk !== k) stats.changes++;
        return [nk, scrubValue(x, stats)];
      }),
    );
  }
  return v;
}
