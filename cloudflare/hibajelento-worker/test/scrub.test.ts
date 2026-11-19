// A szerveroldali tisztítás tesztjei. Az esetek a src/test_watchdog.cpp [3] és [6] csoportjának mintaalapú részei
// (a felhasználónév, gépnév, eszköznév a kliens dolga: a szerver nem ismeri), és néhány további, határesetet
// ellenőrző minta. A mutánsok (tools/mutants.mjs) ezt a fájlt futtatják.
import { describe, expect, it } from "vitest";
import { scrubText, scrubValue } from "../src/scrub";

/** Determinisztikus álvéletlen (mulberry32), hogy a hiba ismételhető legyen. */
function rng(seed: number) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

describe("[3] mintaalapú tisztítás (a C++ [3] átirata)", () => {
  for (const seed of [1, 2, 3, 20261119]) {
    it(`útvonal, IPv4, IPv6, MAC eltűnik; a DAC neve, idő, verzió, helyi cím megmarad (seed ${seed})`, () => {
      const r = rng(seed);
      const int = (lo: number, hi: number) => lo + Math.floor(r() * (hi - lo + 1));
      const word = (lo: number, hi: number) =>
        Array.from({ length: int(lo, hi) }, () => String.fromCharCode(97 + int(0, 25))).join("");
      const hex2 = () => int(0, 255).toString(16).padStart(2, "0");
      const user = "u" + word(6, 9);
      const ip4a = `192.168.${int(1, 254)}.${int(1, 254)}`;
      const ip4b = `10.${int(1, 254)}.${int(1, 254)}.${int(1, 254)}`;
      const mac = [hex2(), hex2(), hex2(), hex2(), hex2(), hex2()].join(":");
      const ip6a = `fe80::${(0x1000 + int(0, 255)).toString(16)}:${int(0, 255).toString(16)}ff:fe${hex2()}:${(0x100 + int(0, 255)).toString(16)}`;
      const ip6b = `2001:db8:85a3::8a2e:370:${1000 + int(0, 255)}`;
      const macDash = mac.replaceAll(":", "-").toUpperCase();
      const macBluez = macDash.replaceAll("-", "_");
      const email = `${user}.nev+zene@example-posta.hu`;

      const text =
        `[Lejátszó] Szól: /home/${user}/Zene/Album (2020)/01 dal.flac\n` +
        `[Zenetár] mappa: ~/Zene/klasszikus\n` +
        `[Windows] C:\\Users\\${user}\\Music\\a.flac megnyitva\n` +
        `[Windows] megosztás \\\\NAS-${user}\\zene\\b.flac\n` +
        `[Hálózat] kliens ${ip4a} és ${ip4b}:47820, helyi 127.0.0.1\n` +
        `[Hálózat] IPv6 ${ip6a} és [${ip6b}]:47820, zóna fe80::1%eth0\n` +
        `[Bluetooth] eszköz ${mac} / ${macDash}, nyelő bluez_output.${macBluez}.1\n` +
        `[Fiók] kapcsolat: ${email}.\n` +
        `12:34:56 DAC: FiiO BTR7, DSD256, natív DSD; verzió 2.6.1; bridge: pullAudio\n`;
      const out = scrubText(text);

      expect(out).not.toContain(user);
      for (const bad of ["/home/", "C:\\Users", "~/Zene", "01 dal.flac", "\\\\NAS"]) expect(out).not.toContain(bad);
      expect(out).toContain("<path>");
      for (const bad of [ip4a, ip4b, ip6a, ip6b, "fe80:", "2001:db8"]) expect(out).not.toContain(bad);
      for (const bad of [mac, macDash, macBluez, mac.toUpperCase()]) expect(out).not.toContain(bad);
      expect(out).not.toContain("example-posta");
      expect(out).toContain("<email>.");
      expect(out).toContain("<ip>:47820");
      expect(out).toContain("<mac>");
      for (const keep of ["FiiO BTR7", "DSD256", "bridge: pullAudio", "12:34:56", "2.6.1", "127.0.0.1"]) expect(out).toContain(keep);
    });
  }
});

describe("[6] kernelverzió, ragos cím, helyi URL (a C++ [6] átirata)", () => {
  it("a kernelverzió marad, a valódi címek eltűnnek", () => {
    const text =
      "[Rendszer] Linux 6.6.87.2-microsoft-standard-WSL2 x86_64\n" +
      "[Hálózat] kliens 192.168.1.23 csatlakozott\n" +
      "[Hálózat] távoli cél 10.0.0.5:47820\n" +
      "[Hálózat] utolsó kliens: 192.168.1.23.\n" +
      "[Hálózat] a 192.168.1.23-hoz csatlakozik\n" +
      "[Híd] felület: ws://127.0.0.1:47899\n";
    const out = scrubText(text);
    expect(out).toContain("6.6.87.2-microsoft-standard-WSL2");
    expect(out).not.toContain("192.168.1.23");
    expect(out).not.toContain("10.0.0.5");
    expect(out).toContain("<ip>:47820");
    expect(out).toContain("<ip>.");
    expect(out).toContain("<ip>-hoz");
    expect(out).toContain("ws://127.0.0.1:47899");
  });
  it("nem helyi ws:// cím eltűnik, a helyi cím utáni útvonal <path>", () => {
    expect(scrubText("[Híd] távoli: ws://192.168.1.23:47820/ws\n")).not.toContain("192.168.1.23");
    expect(scrubText("felület: ws://127.0.0.1:47899/zene/sajat.flac")).toBe("felület: ws://127.0.0.1:47899<path>");
    expect(scrubText("http://localhost:8080/a/b")).toBe("http://localhost:8080<path>");
  });
});

describe("pontos kimenet (a C++ scrub() viselkedése szerint)", () => {
  const cases: [string, string][] = [
    ["Szól: /home/anna/Zene/a.flac", "Szól: <path>"],
    ["C:/Users/anna/Music/x.dsf vége", "<path>"],
    ["mappa ~/Zene", "mappa <path>"],
    ["arány 1/2 és /dev", "arány 1/2 és /dev"], // egy perjel: nem útvonal
    ["címek: 8.8.8.8, 0.0.0.0, 127.0.0.53", "címek: <ip>, 0.0.0.0, 127.0.0.53"],
    ["verzió 1.2.3.4.5 és 300.1.1.1", "verzió 1.2.3.4.5 és 300.1.1.1"], // öt rész, 300 > 255: nem cím
    ["::1 és :: marad, 2001:db8::1 nem", "::1 és :: marad, <ip> nem"],
    ["idő 12:34:56", "idő 12:34:56"], // nincs betű, nincs ::, csak két kettőspont
    ["AA:BB:CC:DD:EE:FF", "<mac>"],
    ["aa-bb-cc-dd-ee-ff-00", "aa-bb-cc-dd-ee-ff-00"], // hét csoport: nem MAC
    ["írj: a@b.cd", "írj: <email>"],
    ["@mention és x@y nem e-mail", "@mention és x@y nem e-mail"],
    ["192.168.0.1192.168.0.2", "192.168.0.1192.168.0.2"], // nem szóhatáron
    ["/a/b/c /d/e/f", "<path>"], // a szóköz nem zárja az útvonalat: egy <path>
    ["<ip><ip> és <path><path>", "<ip> és <path>"],
  ];
  for (const [inp, want] of cases) it(JSON.stringify(inp), () => expect(scrubText(inp)).toBe(want));
});

describe("szerkezet tisztítása", () => {
  it("kulcsok és mélyen beágyazott szövegek is tisztulnak, a számok és logikai értékek maradnak", () => {
    const stats = { changes: 0 };
    const v = scrubValue(
      {
        sections: { engine: { lib: "/home/anna/Zene", "10.1.2.3": "kulcsban cím", device: "FiiO BTR7" } },
        log: ["kliens 10.9.8.7", "rendben"],
        n: 3,
        b: true,
        z: null,
      },
      stats,
    );
    expect(v).toEqual({
      sections: { engine: { lib: "<path>", "<ip>": "kulcsban cím", device: "FiiO BTR7" } },
      log: ["kliens <ip>", "rendben"],
      n: 3,
      b: true,
      z: null,
    });
    expect(stats.changes).toBe(3);
  });
  it("„__proto__” kulcs saját mező marad, nem állít prototípust", () => {
    const parsed = JSON.parse('{"__proto__":{"x":"10.0.0.1"}}');
    const v = scrubValue(parsed) as Record<string, unknown>;
    expect(Object.getPrototypeOf(v)).toBe(Object.prototype);
    expect(JSON.stringify(v)).toBe('{"__proto__":{"x":"<ip>"}}');
  });
  it("kétszeri tisztítás ugyanazt adja (a kliens által már tisztított szöveget nem rontja)", () => {
    const samples = [
      "Szól: <path> kliens <ip>:47820 eszköz <mac> <email>",
      "Linux 6.6.87.2-microsoft-standard-WSL2, ws://127.0.0.1:47899<path>",
      "C:\\Users\\x\\a.flac és 10.0.0.1 és fe80::1%eth0",
    ];
    for (const s of samples) expect(scrubText(scrubText(s))).toBe(scrubText(s));
  });
});
