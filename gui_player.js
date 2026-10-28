/**
 * Aqulify 2.x – közös lejátszómodul (PC-s felület és telefon).
 *
 * AqNowPlaying: a „most szól” sáv. A motor protokolljára épül (docs/remote-api.md): status.media (a beépített
 * lejátszó vagy a gépen szóló más lejátszó), "media" parancsok, get_media_art, status.queue és a queue_* üzenetek.
 * Nem függ a befogadó felülettől: a küldést (send) és a gyökérelemet (root) a hívó adja, a szövegek és a stílus itt
 * vannak (a színek a felület --aq-* változóiból, tartalékkal).
 *
 *   const np = window.AqNowPlaying({ send, root, lang: "hu" });
 *   np.onState(stateMsg);   // a "state" üzenet (caps)
 *   np.onStatus(statusMsg); // a "status" üzenet (media, queue)
 *   np.onMessage(msg);      // minden más üzenet (media_art, queue)
 *   np.setLang("en");
 *
 * AqLibrary: a zenetár (albumok, mappák, listák, okos lista szerkesztő, sor; a telefonon a telefon saját zenéje is).
 * Elrendezés: layout "phone" (érintésre, alsó lapokkal) vagy "desktop" (rács, számtáblázat, dupla kattintás, jobb
 * gombos menü, Enter). A telefon (www/player.js) és a PC-s felület ugyanezt a kódot tölti be; a telefon példánya a
 * 2.x fából másolódik (aqulify-mobile: tools/sync-player.mjs), kézzel nem szerkesztendő.
 *
 *   const lib = window.AqLibrary({ send, root, lang: "hu", layout: "desktop", onOpenRoots, onPlay });
 *   lib.show(); lib.onMessage(m) (true: feldolgozta); lib.onStatus(statusMsg); lib.setLang("en"); lib.reset();
 */
(function () {
  "use strict";

  const STR = {
    hu: {
      play: "Lejátszás", pause: "Szünet", prev: "Előző", next: "Következő",
      shuffle: "Keverés", repeatOff: "Ismétlés: ki", repeatAll: "Ismétlés: az egész sor", repeatOne: "Ismétlés: ez a szám",
      queue: "Lejátszási sor", queueEmpty: "A sor üres.", nothing: "Nincs lejátszás",
      resume: "Folytatás a sorból ({n} szám)", from: "{app}", seek: "Pozíció", close: "Bezárás",
      loading: "Betöltés…", nowItem: "Most szól"
    },
    en: {
      play: "Play", pause: "Pause", prev: "Previous", next: "Next",
      shuffle: "Shuffle", repeatOff: "Repeat: off", repeatAll: "Repeat: whole queue", repeatOne: "Repeat: this track",
      queue: "Play queue", queueEmpty: "The queue is empty.", nothing: "Nothing playing",
      resume: "Resume from the queue ({n} tracks)", from: "{app}", seek: "Position", close: "Close",
      loading: "Loading…", nowItem: "Now playing"
    }
  };

  const ICON = {
    play: '<path d="M8 5.5v13l11-6.5z"/>',
    pause: '<path d="M7 5h3.5v14H7zM13.5 5H17v14h-3.5z"/>',
    prev: '<path d="M6 5h2.2v14H6zM19 5.5v13L9.5 12z"/>',
    next: '<path d="M15.8 5H18v14h-2.2zM5 5.5v13L14.5 12z"/>',
    shuffle: '<path d="M4 7h3.5c1.6 0 2.6.7 3.5 2l3 4.5c.9 1.3 1.9 2 3.5 2H19M4 17h3.5c1.6 0 2.6-.7 3.5-2M14 9c.9-1.3 1.9-2 3.5-2H19M17 4.5 19.5 7 17 9.5M17 13l2.5 2.5L17 18" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/>',
    repeat: '<path d="M5 11V9.5A2.5 2.5 0 0 1 7.5 7H18M15.5 4.5 18 7l-2.5 2.5M19 13v1.5a2.5 2.5 0 0 1-2.5 2.5H6M8.5 19.5 6 17l2.5-2.5" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/>',
    queue: '<path d="M4 6.5h12M4 11h12M4 15.5h7M15 14v6l5-3z" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/>',
    note: '<path d="M10 17.5a2.5 2.5 0 1 1-2.5-2.5c.9 0 1.7.3 2.5.8V5l9-2v11.5a2.5 2.5 0 1 1-2.5-2.5c.9 0 1.7.3 2.5.8V7l-9 2" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linejoin="round"/>'
  };
  const svg = (name, size) =>
    `<svg viewBox="0 0 24 24" width="${size || 20}" height="${size || 20}" aria-hidden="true" fill="currentColor">${ICON[name]}</svg>`;

  const CSS = `
.aqnp { position: fixed; left: 0; right: 0; bottom: 0; z-index: 900; display: grid;
  grid-template-columns: minmax(180px, 1fr) minmax(280px, 2fr) minmax(120px, 1fr); align-items: center; gap: 16px;
  padding: 8px 16px; min-height: 64px; box-sizing: border-box;
  background: var(--aq-bg-2, #151b23); border-top: 1px solid var(--aq-line, #2a3340);
  color: var(--aq-text, #e8edf2); font-family: var(--aq-font, system-ui, sans-serif); font-size: var(--aq-fs-body, 12px); }
.aqnp[hidden], .aqnp [hidden], .aqnp-pop [hidden] { display: none !important; }
.aqnp-info { display: flex; align-items: center; gap: 10px; min-width: 0; }
.aqnp-art { width: 48px; height: 48px; flex: none; border-radius: 6px; object-fit: cover;
  background: var(--aq-input, #0f141a); border: 1px solid var(--aq-line, #2a3340); }
.aqnp-art-empty { display: flex; align-items: center; justify-content: center; color: var(--aq-text-2, #93a1b0); }
.aqnp-text { min-width: 0; }
.aqnp-title { font-size: var(--aq-fs-head, 13px); font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.aqnp-sub { color: var(--aq-text-2, #93a1b0); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; margin-top: 2px; }
.aqnp-app { color: var(--aq-text-2, #93a1b0); font-size: var(--aq-fs-sub, 11px); margin-top: 2px; }
.aqnp-center { display: flex; flex-direction: column; align-items: center; gap: 4px; min-width: 0; }
.aqnp-ctrls { display: flex; align-items: center; gap: 6px; }
.aqnp-btn { display: inline-flex; align-items: center; justify-content: center; width: 32px; height: 32px; padding: 0;
  border: none; border-radius: 50%; background: transparent; color: var(--aq-text, #e8edf2); cursor: pointer; }
.aqnp-btn:hover { background: var(--aq-input, rgba(255,255,255,0.06)); }
.aqnp-btn:focus-visible { outline: 2px solid var(--aq-accent, #4fd1c5); outline-offset: 2px; }
.aqnp-btn[disabled] { opacity: 0.35; cursor: default; background: transparent; }
.aqnp-btn.on { color: var(--aq-accent, #4fd1c5); }
.aqnp-play { width: 40px; height: 40px; background: var(--aq-text, #e8edf2); color: var(--aq-surface, #151b23); }
.aqnp-play:hover { background: var(--aq-text, #e8edf2); transform: scale(1.05); }
.aqnp-rep { position: relative; }
.aqnp-rep-one::after { content: "1"; position: absolute; right: 3px; bottom: 3px; font-size: 9px; font-weight: 700; }
.aqnp-seekrow { display: flex; align-items: center; gap: 8px; width: 100%; max-width: 640px; }
.aqnp-time { font-family: var(--aq-mono, ui-monospace, monospace); font-size: var(--aq-fs-sub, 11px);
  color: var(--aq-text-2, #93a1b0); min-width: 38px; text-align: center; font-variant-numeric: tabular-nums; }
.aqnp-seek { flex: 1; accent-color: var(--aq-accent, #4fd1c5); cursor: pointer; height: 16px; }
  .aqnp-seek::-webkit-slider-runnable-track { height: 4px; border-radius: 2px; background: linear-gradient(to right, var(--aq-accent, #4fd1c5) 0%, var(--aq-accent, #4fd1c5) var(--fill, 0%), var(--aq-line-2, #374251) var(--fill, 0%), var(--aq-line-2, #374251) 100%) !important; }
.aqnp-seek[disabled] { cursor: default; opacity: 0.4; }
.aqnp-right { display: flex; justify-content: flex-end; align-items: center; gap: 6px; }
.aqnp-empty { color: var(--aq-text-2, #93a1b0); }
.aqnp-resume { border: 1px solid var(--aq-line, #2a3340); background: var(--aq-input, #0f141a); color: var(--aq-text, #e8edf2);
  border-radius: 6px; padding: 6px 10px; cursor: pointer; font: inherit; }
.aqnp-resume:hover { border-color: var(--aq-accent, #4fd1c5); }
.aqnp-pop { position: fixed; right: 16px; bottom: 76px; z-index: 901; width: 380px; max-width: calc(100vw - 32px);
  max-height: min(60vh, 520px); display: flex; flex-direction: column; overflow: hidden;
  background: var(--aq-bg-2, #151b23); border: 1px solid var(--aq-line, #2a3340); border-radius: 10px;
  box-shadow: 0 12px 32px rgba(0,0,0,0.35); color: var(--aq-text, #e8edf2); font-family: var(--aq-font, system-ui, sans-serif);
  font-size: var(--aq-fs-body, 12px); }
.aqnp-pop[hidden] { display: none; }
.aqnp-pop-head { display: flex; justify-content: space-between; align-items: center; padding: 10px 12px;
  border-bottom: 1px solid var(--aq-line, #2a3340); font-weight: 600; font-size: var(--aq-fs-head, 13px); }
.aqnp-pop-list { overflow-y: auto; padding: 4px 0; }
.aqnp-qi { display: grid; grid-template-columns: 28px 1fr auto; gap: 8px; align-items: center; width: 100%; padding: 6px 12px;
  border: none; background: transparent; color: inherit; font: inherit; text-align: left; cursor: pointer; }
.aqnp-qi:hover { background: var(--aq-input, rgba(255,255,255,0.05)); }
.aqnp-qi.cur { color: var(--aq-accent, #4fd1c5); }
.aqnp-qi-n { color: var(--aq-text-2, #93a1b0); font-family: var(--aq-mono, ui-monospace, monospace); font-size: var(--aq-fs-sub, 11px); text-align: right; }
.aqnp-qi-t { min-width: 0; }
.aqnp-qi-t div { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.aqnp-qi-t div + div { color: var(--aq-text-2, #93a1b0); font-size: var(--aq-fs-sub, 11px); }
.aqnp-qi-d { color: var(--aq-text-2, #93a1b0); font-family: var(--aq-mono, ui-monospace, monospace); font-size: var(--aq-fs-sub, 11px); }
.aqnp-pop-msg { padding: 14px 12px; color: var(--aq-text-2, #93a1b0); }
body.aqnp-on { padding-bottom: 72px; }
@media (max-width: 760px) {
  .aqnp { grid-template-columns: 1fr; gap: 6px; }
  .aqnp-right { justify-content: center; }
  body.aqnp-on { padding-bottom: 150px; }
}`;

  function fmt(s) {
    s = Math.max(0, Math.floor(s || 0));
    const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), r = s % 60;
    return (h ? h + ":" + String(m).padStart(2, "0") : m) + ":" + String(r).padStart(2, "0");
  }

  window.AqNowPlaying = function (opts) {
    const send = opts.send;
    let lang = opts.lang === "hu" ? "hu" : "en";
    const t = (k, v) => (STR[lang][k] || k).replace(/\{(\w+)\}/g, (_, n) => (v && n in v ? v[n] : ""));

    if (!document.getElementById("aqnp-style")) {
      const st = document.createElement("style");
      st.id = "aqnp-style";
      st.textContent = CSS;
      document.head.appendChild(st);
    }

    // ---- váz
    const root = opts.root;
    root.className = "aqnp";
    root.hidden = true;
    root.setAttribute("role", "region");
    root.innerHTML = `
      <div class="aqnp-info">
        <img class="aqnp-art" alt="" hidden>
        <div class="aqnp-art aqnp-art-empty">${svg("note", 22)}</div>
        <div class="aqnp-text">
          <div class="aqnp-title"></div>
          <div class="aqnp-sub"></div>
          <div class="aqnp-app"></div>
        </div>
      </div>
      <div class="aqnp-center">
        <div class="aqnp-ctrls">
          <button type="button" class="aqnp-btn" data-k="shuffle">${svg("shuffle", 18)}</button>
          <button type="button" class="aqnp-btn" data-k="prev">${svg("prev")}</button>
          <button type="button" class="aqnp-btn aqnp-play" data-k="play">${svg("play", 22)}</button>
          <button type="button" class="aqnp-btn" data-k="next">${svg("next")}</button>
          <button type="button" class="aqnp-btn aqnp-rep" data-k="repeat">${svg("repeat", 18)}</button>
        </div>
        <div class="aqnp-seekrow">
          <span class="aqnp-time" data-k="pos">0:00</span>
          <input type="range" class="aqnp-seek" min="0" max="1000" value="0" step="1">
          <span class="aqnp-time" data-k="dur">0:00</span>
        </div>
        <div class="aqnp-empty" hidden></div>
      </div>
      <div class="aqnp-right">
        <button type="button" class="aqnp-btn" data-k="queue">${svg("queue", 18)}</button>
      </div>`;
    const $ = sel => root.querySelector(sel);
    const ui = {
      artImg: $("img.aqnp-art"), artEmpty: $(".aqnp-art-empty"),
      title: $(".aqnp-title"), sub: $(".aqnp-sub"), app: $(".aqnp-app"),
      shuffle: $('[data-k="shuffle"]'), prev: $('[data-k="prev"]'), play: $('[data-k="play"]'),
      next: $('[data-k="next"]'), repeat: $('[data-k="repeat"]'), queue: $('[data-k="queue"]'),
      ctrls: $(".aqnp-ctrls"), seekRow: $(".aqnp-seekrow"), seek: $(".aqnp-seek"),
      pos: $('[data-k="pos"]'), dur: $('[data-k="dur"]'), empty: $(".aqnp-empty")
    };

    const pop = document.createElement("div");
    pop.className = "aqnp-pop";
    pop.hidden = true;
    pop.setAttribute("role", "dialog");
    pop.innerHTML = `<div class="aqnp-pop-head"><span></span><button type="button" class="aqnp-btn" data-k="close">✕</button></div>
                     <div class="aqnp-pop-list"></div>`;
    document.body.appendChild(pop);
    const popTitle = pop.querySelector(".aqnp-pop-head span");
    const popList = pop.querySelector(".aqnp-pop-list");
    const popClose = pop.querySelector('[data-k="close"]');

    // ---- állapot
    let hasQueueCap = false;
    let media = null, mediaAt = 0, queue = null, queueItems = null, queueRev = -1;
    let artShown = null, dragging = false;

    const updateSeekFill = () => {
      const min = ui.seek.min === "" ? 0 : +ui.seek.min;
      const max = ui.seek.max === "" ? 1000 : +ui.seek.max;
      const val = +ui.seek.value;
      const pct = max > min ? Math.max(0, Math.min(100, ((val - min) / (max - min)) * 100)) : 0;
      ui.seek.style.setProperty("--fill", pct + "%");
    };
    updateSeekFill();

    const curPos = () => (media ? media.position + (media.playing ? (Date.now() - mediaAt) / 1000 : 0) : 0);
    const fromQueue = () => !!(media && media.available && media.app === "Aqulify" && queue && queue.index >= 0);

    // felirat és akadálymentes név együtt (az ariaLabel tulajdonságot nem minden WebView ismeri)
    const label = (node, text) => { node.title = text; node.setAttribute("aria-label", text); };
    function setTitles() {
      label(ui.shuffle, t("shuffle"));
      label(ui.prev, t("prev"));
      label(ui.next, t("next"));
      label(ui.queue, t("queue"));
      ui.seek.setAttribute("aria-label", t("seek"));
      label(popClose, t("close"));
      popTitle.textContent = t("queue");
      root.setAttribute("aria-label", t("nowItem"));
    }

    function render() {
      const m = media;
      const has = !!(m && m.available);
      const qCount = queue ? queue.count || 0 : 0;
      const show = has || hasQueueCap;
      root.hidden = !show;
      document.body.classList.toggle("aqnp-on", show);
      if (!show) { pop.hidden = true; return; }

      ui.ctrls.hidden = false;
      ui.seekRow.hidden = !has;
      ui.empty.hidden = has;
      if (!has) {
        ui.title.textContent = t("nothing");
        ui.sub.textContent = "";
        ui.app.textContent = "";
        ui.empty.replaceChildren();
        if (qCount > 0) {
          const b = document.createElement("button");
          b.type = "button";
          b.className = "aqnp-resume";
          b.textContent = t("resume", { n: qCount });
          b.onclick = resume;
          ui.empty.appendChild(b);
        }
        setArt("");
      } else {
        ui.title.textContent = m.title || m.app || "";
        ui.sub.textContent = [m.artist, m.album].filter(Boolean).join(" · ");
        ui.app.textContent = m.app && m.app !== "Aqulify" ? t("from", { app: m.app }) : "";
        if (m.art !== artShown) setArt(m.art || "");
      }

      const can = (has && m.can) || {};
      const playing = has && m.playing;
      ui.play.innerHTML = svg(playing ? "pause" : "play", 22);
      label(ui.play, t(playing ? "pause" : "play"));
      ui.play.disabled = has ? !(playing ? can.pause : can.play) : qCount === 0;
      ui.prev.disabled = !(has && can.previous);
      ui.next.disabled = !(has && can.next);
      ui.seek.disabled = !(has && can.seek && m.duration > 0);
      if (!has || !m.duration || m.duration <= 0) {
        if (!dragging) {
          ui.seek.value = "0";
          updateSeekFill();
        }
      }

      // keverés és ismétlés: a beépített lejátszó sorára vonatkozik
      const qOn = hasQueueCap && qCount > 0;
      ui.shuffle.hidden = ui.repeat.hidden = !hasQueueCap;
      ui.shuffle.disabled = ui.repeat.disabled = !qOn;
      ui.shuffle.classList.toggle("on", !!(queue && queue.shuffle));
      const rep = (queue && queue.repeat) || "off";
      ui.repeat.classList.toggle("on", rep !== "off");
      ui.repeat.classList.toggle("aqnp-rep-one", rep === "one");
      label(ui.repeat, t(rep === "one" ? "repeatOne" : rep === "all" ? "repeatAll" : "repeatOff"));
      ui.queue.hidden = !hasQueueCap;
      tick();
    }

    function setArt(id) {
      artShown = id;
      ui.artImg.hidden = true;
      ui.artImg.removeAttribute("src");
      ui.artEmpty.hidden = false;
      if (id) send({ type: "get_media_art", id });
    }
    ui.artImg.onerror = () => { ui.artImg.hidden = true; ui.artEmpty.hidden = false; };

    function tick() {
      if (!media || !media.available) {
        if (!dragging) {
          ui.seek.value = "0";
          updateSeekFill();
        }
        return;
      }
      const d = media.duration || 0;
      const p = d > 0 ? Math.min(curPos(), d) : curPos();
      if (!dragging) {
        ui.pos.textContent = fmt(p);
        if (d > 0) {
          ui.seek.value = String(Math.round((p / d) * 1000));
        } else {
          ui.seek.value = "0";
        }
        updateSeekFill();
      }
      ui.dur.textContent = d > 0 ? fmt(d) : "";
    }
    const timer = setInterval(tick, 250);

    // ---- vezérlés
    function togglePlay() {
      if (!media || !media.available) { resume(); return; }
      const can = media.can || {};
      const action = media.playing ? "pause" : "play";
      if (!can[action]) return;
      send({ type: "media", action });
      media.position = curPos();
      mediaAt = Date.now();
      media.playing = !media.playing;   // azonnali visszajelzés; a következő állapotüzenet megerősíti
      render();
    }
    function resume() {
      if (!queue || !(queue.count > 0)) return;
      send({ type: "queue_jump", index: queue.index >= 0 ? queue.index : 0 });
    }
    ui.play.onclick = togglePlay;
    ui.prev.onclick = () => send({ type: "media", action: "previous" });
    ui.next.onclick = () => send({ type: "media", action: "next" });
    ui.shuffle.onclick = () => send({ type: "queue_shuffle", on: !(queue && queue.shuffle) });
    ui.repeat.onclick = () => {
      const cur = (queue && queue.repeat) || "off";
      send({ type: "queue_repeat", mode: cur === "off" ? "all" : cur === "all" ? "one" : "off" });
    };
    ui.seek.addEventListener("input", () => {
      dragging = true;
      if (media && media.duration > 0) ui.pos.textContent = fmt((ui.seek.value / 1000) * media.duration);
      updateSeekFill();
    });
    ui.seek.addEventListener("change", () => {
      dragging = false;
      if (!media || !(media.duration > 0)) {
        updateSeekFill();
        return;
      }
      const pos = (ui.seek.value / 1000) * media.duration;
      send({ type: "media", action: "seek", position: pos });
      media.position = pos;
      mediaAt = Date.now();
      updateSeekFill();
      tick();
    });

    // ---- sor (felugró lista vagy asztali beágyazott nézet)
    let lastNotifySig = "";
    function getSnapshot() {
      return {
        media: media,
        artSrc: (!ui.artImg.hidden && ui.artImg.getAttribute("src")) || "",
        queue: queue,
        queueItems: queueItems
      };
    }
    function notifyUpdate(force) {
      if (typeof opts.onUpdate !== "function") return;
      const m = media || {};
      const q = queue || {};
      const sig = JSON.stringify([
        !!m.available, m.title || "", m.artist || "", m.album || "", m.app || "",
        (!ui.artImg.hidden && ui.artImg.getAttribute("src")) || "",
        q.rev, q.index, q.repeat, q.shuffle, q.count,
        Array.isArray(queueItems) ? queueItems.length : -1
      ]);
      if (!force && sig === lastNotifySig) return;
      lastNotifySig = sig;
      opts.onUpdate(getSnapshot());
    }

    if (typeof opts.onToggleCover === "function") {
      [ui.artImg, ui.artEmpty].forEach(el => {
        if (!el) return;
        el.onclick = e => { e.stopPropagation(); opts.onToggleCover(); };
      });
    }

    ui.queue.onclick = e => {
      if (typeof opts.onToggleQueue === "function") {
        e.stopPropagation();
        if (queueItems === null) send({ type: "queue_get" });
        opts.onToggleQueue();
        return;
      }
      pop.hidden = !pop.hidden;
      if (!pop.hidden) {
        if (queueItems === null) { popList.innerHTML = `<div class="aqnp-pop-msg">${t("loading")}</div>`; }
        send({ type: "queue_get" });
      }
    };
    popClose.onclick = () => { pop.hidden = true; ui.queue.focus(); };
    document.addEventListener("pointerdown", e => {
      if (!pop.hidden && !pop.contains(e.target) && !ui.queue.contains(e.target)) pop.hidden = true;
    }, true);

    function renderQueue() {
      popList.replaceChildren();
      if (!queueItems || queueItems.length === 0) {
        popList.innerHTML = `<div class="aqnp-pop-msg">${t("queueEmpty")}</div>`;
        return;
      }
      const isCurPlaying = !!(media && media.available && media.app === "Aqulify" && media.playing);
      const curIdx = isCurPlaying && queue ? queue.index : -1;
      queueItems.forEach((it, i) => {
        const b = document.createElement("button");
        b.type = "button";
        b.className = "aqnp-qi" + (i === curIdx ? " cur" : "");
        if (i === curIdx) b.setAttribute("aria-current", "true");
        const n = document.createElement("span"); n.className = "aqnp-qi-n"; n.textContent = String(i + 1);
        const tx = document.createElement("span"); tx.className = "aqnp-qi-t";
        const a = document.createElement("div"); a.textContent = it.title || "";
        const s = document.createElement("div"); s.textContent = [it.artist, it.album].filter(Boolean).join(" · ");
        tx.append(a, s);
        const d = document.createElement("span"); d.className = "aqnp-qi-d"; d.textContent = it.duration > 0 ? fmt(it.duration) : "";
        b.append(n, tx, d);
        b.onclick = () => send({ type: "queue_jump", index: i });
        popList.appendChild(b);
      });
      const cur = popList.querySelector(".cur");
      if (cur) cur.scrollIntoView({ block: "nearest" });
    }

    // ---- billentyű: szóköz = lejátszás / szünet (nem gépelés közben, nem nyitott ablak fölött)
    function onKey(e) {
      if (e.code !== "Space" || e.ctrlKey || e.altKey || e.metaKey || root.hidden) return;
      const a = document.activeElement;
      if (a && (a.isContentEditable || /^(INPUT|TEXTAREA|SELECT|BUTTON)$/.test(a.tagName))) return;
      if (opts.isBlocked && opts.isBlocked()) return;
      e.preventDefault();
      togglePlay();
    }
    document.addEventListener("keydown", onKey);

    setTitles();

    return {
      onState(m) {
        hasQueueCap = !!(m && Array.isArray(m.caps) && m.caps.includes("queue"));
        render();
      },
      onStatus(m) {
        media = (m && m.media) || { available: false };
        mediaAt = Date.now();
        queue = (m && m.queue) || null;
        if (queue && queue.rev !== queueRev) {
          queueRev = queue.rev;
          queueItems = null;
          if (!pop.hidden || (typeof opts.isQueueVisible === "function" && opts.isQueueVisible())) send({ type: "queue_get" });
        }
        render();
        notifyUpdate(false);
      },
      onMessage(m) {
        if (!m) return;
        if (m.type === "media_art" && m.id === artShown) {
          if (m.data) ui.artImg.src = `data:${m.mime || "image/jpeg"};base64,${m.data}`;
          else if (m.url) ui.artImg.src = m.url;
          const ok = !!(m.data || m.url);
          ui.artImg.hidden = !ok;
          ui.artEmpty.hidden = ok;
          notifyUpdate(true);
        } else if (m.type === "queue" && Array.isArray(m.items)) {
          queueItems = m.items;
          if (queue) { queue.index = m.index; queue.repeat = m.repeat; queue.shuffle = m.shuffle; queue.count = m.items.length; }
          if (typeof m.rev === "number") queueRev = m.rev;
          if (!pop.hidden) renderQueue();
          render();
          notifyUpdate(true);
        }
      },
      onDisconnect() {
        media = { available: false };
        queue = null;
        hasQueueCap = false;
        render();
        notifyUpdate(true);
      },
      setLang(l) {
        lang = l === "hu" ? "hu" : "en";
        setTitles();
        render();
        if (!pop.hidden) renderQueue();
        notifyUpdate(true);
      },
      getSnapshot,
      requestQueue() {
        send({ type: "queue_get" });
      },
      destroy() {
        clearInterval(timer);
        document.removeEventListener("keydown", onKey);
        pop.remove();
        root.replaceChildren();
        root.hidden = true;
        document.body.classList.remove("aqnp-on");
      }
    };
  };
})();

/* ===================================================================================================================
 * AqLibrary – a zenetár (a telefon Zenetár füléből, 3d / 4a–4d; 2.x 0027: közös modul, asztali elrendezés)
 * =================================================================================================================== */
(function () {
  "use strict";

  const STR = {
    hu: {
      albums: "Albumok", folders: "Mappák", lists: "Listák", queue: "Sor", loading: "Betöltés...",
      empty: "A zenetár üres. A zenei mappák a PC-n állíthatók be (alapból a Zene mappa).",
      scanning: "A PC most indexeli a zenetárat ({n} fájl)...", noAlbums: "Nincs album.",
      various: "Különböző előadók", tracks: "{n} szám", play: "Lejátszás", shuffle: "Keverve",
      playNext: "Következőként", playLast: "A sor végére", addToList: "Listához adás...", cancel: "Mégse", ok: "Rendben",
      folderEmpty: "Ez a mappa üres.", roots: "Zenei mappák", back: "Vissza",
      queueEmpty: "A sor üres. Indíts el egy albumot, mappát vagy listát.", clear: "Ürítés", saveAsList: "Mentés listaként",
      repeatOff: "Ismétlés: ki", repeatAll: "Ismétlés: sor", repeatOne: "Ismétlés: szám", shuffleOn: "Keverés: be",
      shuffleOff: "Keverés: ki", moveUp: "Feljebb", moveDown: "Lejjebb", remove: "Eltávolítás", removeFromList: "Törlés a listából",
      newList: "Új lista", newSmart: "Új okos lista", listName: "A lista neve:", rename: "Átnevezés", delete: "Törlés",
      deleteConfirm: "Törlöd a(z) „{name}” listát?", editRules: "Szabályok", unavailable: "nem elérhető",
      smartNote: "Okos lista: a tartalmát a PC a szabályok szerint számolja.", noLists: "Még nincs lista.",
      chooseList: "Melyik listához?", added: "Hozzáadva: {name}", queued: "A sorba került", saved: "Mentve: {name}",
      error: "Hiba: {e}", smartEditor: "Okos lista", name: "Név", match: "Feltételek", matchAll: "mind teljesüljön",
      matchAny: "bármelyik teljesüljön", addCondition: "+ Feltétel", sort: "Rendezés", desc: "csökkenő",
      limit: "Legfeljebb (0: mind)", save: "Mentés", days: "nap", more: "Továbbiak",
      colTitle: "Cím", colArtist: "Előadó", colAlbum: "Album", colDuration: "Hossz",
      stats: "{tracks} szám · {albums} album", playAlbum: "Album lejátszása",
      f_artist: "Előadó", f_album: "Album", f_title: "Cím", f_genre: "Műfaj", f_year: "Év", f_duration: "Hossz (mp)",
      f_plays: "Lejátszásszám", f_added: "Felkerült", f_lastPlayed: "Utoljára játszott",
      o_contains: "tartalmazza", o_notContains: "nem tartalmazza", o_is: "pontosan", o_isNot: "nem", o_startsWith: "ezzel kezdődik",
      o_eq: "=", o_ne: "≠", o_gt: ">", o_ge: "≥", o_lt: "<", o_le: "≤", o_inLast: "az utóbbi … napban",
      o_notInLast: "nem az utóbbi … napban", o_never: "soha",
      s_added: "Felkerülés", s_lastPlayed: "Utolsó lejátszás", s_plays: "Lejátszásszám", s_year: "Év", s_artist: "Előadó",
      s_album: "Album", s_title: "Cím", s_duration: "Hossz", s_random: "Véletlen",
      e_empty: "nincs mit lejátszani", e_name: "adj meg nevet", e_unknown: "nem található", e_smart: "okos listához nem adható szám",
      e_full: "betelt", e_index: "érvénytelen hely", e_rules: "hibás szabály",
      srcPc: "💻 PC zenetára", srcPhone: "📱 Telefon zenéje",
      phoneEmpty: "Még nincs hozzáadva zenei mappa a telefonról.",
      addFolder: "Mappa hozzáadása", rescan: "Újrabejárás",
      scanningPhone: "A telefon zenetárának beolvasása...",
      phoneTracks: "{n} szám található a telefonon",
      onlyAndroid: "A telefon zenetára az Android alkalmazásban érhető el.",
      removeFolderConfirm: "Törlöd a(z) „{name}” mappát a telefonos zenetárból?",
      phoneTrackLocalMatch: "Helyi másolat a PC-n: „{title}”",
      sendingTrack: "Zene küldése a PC-re...",
      copyToPc: "Másolás a PC-re",
      copyConfirmPrompt: "A(z) „{title}” még nincs a PC zenetárában. Mit szeretnél tenni?",
      optPlayOnly: "▶ Csak most játssza le",
      optCopyOnce: "📥 Másolja a PC-re",
      optCopyAlways: "💾 Mindig másolja a PC-re",
      optNeverAsk: "🚫 Soha ne kérdezze (csak lejátszás)",
      copiedToPc: "A szám átmásolva a PC zenetárába!",
      copyPrefTitle: "Másolás a PC-re beállítása",
      prefAsk: "Kérdezzen minden új számnál",
      prefAlways: "Mindig másolja a PC-re",
      prefNever: "Soha ne másolja (csak lejátszás)",
      prefSaved: "Beállítás elmentve",
      onPhone: "A telefonon",
      matchedOnPc: "PC-s másolat",
      phoneDisconnected: "A telefon nem kapcsolódik",
    },
    en: {
      albums: "Albums", folders: "Folders", lists: "Playlists", queue: "Queue", loading: "Loading...",
      empty: "The library is empty. Music folders are set on the PC (by default the Music folder).",
      scanning: "The PC is indexing the library ({n} files)...", noAlbums: "No albums.",
      various: "Various artists", tracks: "{n} tracks", play: "Play", shuffle: "Shuffle",
      playNext: "Play next", playLast: "Add to queue", addToList: "Add to playlist...", cancel: "Cancel", ok: "OK",
      folderEmpty: "This folder is empty.", roots: "Music folders", back: "Back",
      queueEmpty: "The queue is empty. Start an album, folder or playlist.", clear: "Clear", saveAsList: "Save as playlist",
      repeatOff: "Repeat: off", repeatAll: "Repeat: queue", repeatOne: "Repeat: track", shuffleOn: "Shuffle: on",
      shuffleOff: "Shuffle: off", moveUp: "Move up", moveDown: "Move down", remove: "Remove", removeFromList: "Remove from playlist",
      newList: "New playlist", newSmart: "New smart playlist", listName: "Playlist name:", rename: "Rename", delete: "Delete",
      deleteConfirm: "Delete the playlist \"{name}\"?", editRules: "Rules", unavailable: "unavailable",
      smartNote: "Smart playlist: the PC computes its content from the rules.", noLists: "No playlists yet.",
      chooseList: "Which playlist?", added: "Added: {name}", queued: "Added to the queue", saved: "Saved: {name}",
      error: "Error: {e}", smartEditor: "Smart playlist", name: "Name", match: "Conditions", matchAll: "all must match",
      matchAny: "any may match", addCondition: "+ Condition", sort: "Sort by", desc: "descending",
      limit: "At most (0: all)", save: "Save", days: "days", more: "More",
      colTitle: "Title", colArtist: "Artist", colAlbum: "Album", colDuration: "Length",
      stats: "{tracks} tracks · {albums} albums", playAlbum: "Play album",
      f_artist: "Artist", f_album: "Album", f_title: "Title", f_genre: "Genre", f_year: "Year", f_duration: "Length (s)",
      f_plays: "Play count", f_added: "Added", f_lastPlayed: "Last played",
      o_contains: "contains", o_notContains: "does not contain", o_is: "is", o_isNot: "is not", o_startsWith: "starts with",
      o_eq: "=", o_ne: "≠", o_gt: ">", o_ge: "≥", o_lt: "<", o_le: "≤", o_inLast: "in the last … days",
      o_notInLast: "not in the last … days", o_never: "never",
      s_added: "Added", s_lastPlayed: "Last played", s_plays: "Play count", s_year: "Year", s_artist: "Artist",
      s_album: "Album", s_title: "Title", s_duration: "Length", s_random: "Random",
      e_empty: "nothing to play", e_name: "enter a name", e_unknown: "not found", e_smart: "cannot add tracks to a smart playlist",
      e_full: "full", e_index: "invalid position", e_rules: "invalid rule",
      srcPc: "💻 PC Library", srcPhone: "📱 Phone Music",
      phoneEmpty: "No music folder added from phone yet.",
      addFolder: "Add folder", rescan: "Rescan",
      scanningPhone: "Scanning phone music library...",
      phoneTracks: "{n} tracks found on phone",
      onlyAndroid: "Phone music library is available in the Android app.",
      removeFolderConfirm: "Remove folder \"{name}\" from phone library?",
      phoneTrackLocalMatch: "Local copy found on PC: \"{title}\"",
      sendingTrack: "Streaming track to PC...",
      copyToPc: "Copy to PC",
      copyConfirmPrompt: "\"{title}\" is not in the PC library yet. What would you like to do?",
      optPlayOnly: "▶ Play now only",
      optCopyOnce: "📥 Copy to PC",
      optCopyAlways: "💾 Always copy to PC",
      optNeverAsk: "🚫 Never ask (play only)",
      copiedToPc: "Track copied to PC library!",
      copyPrefTitle: "Copy to PC settings",
      prefAsk: "Ask for every new track",
      prefAlways: "Always copy to PC",
      prefNever: "Never copy (play only)",
      prefSaved: "Setting saved",
      onPhone: "On phone",
      matchedOnPc: "Local copy on PC",
      phoneDisconnected: "Phone not connected",
    },
  };

  // Az okos lista mezői és műveletei (a motor szabályai szerint)
  const FIELDS = {
    artist: "text", album: "text", title: "text", genre: "text",
    year: "number", duration: "number", plays: "number", added: "date", lastPlayed: "date",
  };
  const OPS = {
    text: ["contains", "notContains", "is", "isNot", "startsWith"],
    number: ["eq", "ne", "gt", "ge", "lt", "le"],
    date: ["inLast", "notInLast"],
  };
  const SORTS = ["added", "lastPlayed", "plays", "year", "artist", "album", "title", "duration", "random"];
  const PAGE = 60;            // ennyi albumot kér egyszerre
  const ART_PARALLEL = 4;     // egyszerre ennyi borítót kér

  // A színek a befogadó felület változóiból: a PC-s felület --aq-*, a telefon --text / --accent / … nevei, tartalékkal.
  const LIB_CSS = `
.aqlib { --l-bg: var(--aq-bg, var(--bg, #121316)); --l-panel: var(--aq-surface, var(--panel, #181a1f));
  --l-line: var(--aq-line, var(--line, #262932)); --l-text: var(--aq-text, var(--text, #ededee));
  --l-text2: var(--aq-text-2, var(--text-2, #8e95a5)); --l-accent: var(--aq-accent, var(--accent, #e5a93c));
  --l-ink: var(--aq-accent-ink, var(--accent-contrast, #121316)); --l-danger: var(--aq-danger, var(--danger, #d9534f));
  --l-input: var(--aq-input, var(--bg, #121316)); --l-hover: var(--aq-surface-2, rgba(255,255,255,0.05));
  --l-soft: var(--aq-accent-soft, var(--accent-muted, rgba(229,169,60,0.15)));
  --l-fs: 14px; --l-fs-sm: 13px; --l-fs-title: 18px; position: relative; color: var(--l-text); }
.aqlib [hidden] { display: none !important; }
.aqlib ul { list-style: none; margin: 0; padding: 0; }
.aqlib .hint { color: var(--l-text2); font-size: var(--l-fs); margin: 0 0 12px; }
.aqlib .detail { color: var(--l-text2); font-size: var(--l-fs-sm); }
.aqlib button { font-family: inherit; cursor: pointer; }
.aqlib button.icon { background: transparent; color: var(--l-text); border: 1px solid var(--l-line); border-radius: 50%;
  width: 44px; height: 44px; padding: 0; font-size: 20px; line-height: 1; flex: none; display: inline-flex; align-items: center; justify-content: center; }
.aqlib button.icon:hover { border-color: var(--l-accent); }
.aqlib button.wide { width: 100%; margin-top: 14px; border: none; border-radius: 10px; padding: 12px 16px; font-size: 15px; font-weight: 600;
  background: var(--l-accent); color: var(--l-ink); }
.lib-src-nav { display: flex; gap: 6px; margin-bottom: 12px; background: rgba(255,255,255,0.04); padding: 4px; border-radius: 10px; border: 1px solid var(--l-line); }
.lib-src-tab { flex: 1; padding: 7px 10px; border-radius: 7px; border: none; background: transparent; color: var(--l-text2); font-size: var(--l-fs-sm); font-weight: 500; text-align: center; }
.lib-src-tab.active { background: var(--l-line); color: var(--l-text); font-weight: 600; }
.lib-phone-card { text-align: center; padding: 24px 16px; border: 1px dashed var(--l-line); border-radius: 12px; margin: 12px 0; }
.lib-phone-card p { margin: 0 0 16px; color: var(--l-text2); font-size: var(--l-fs); }
.lib-topbar { display: flex; align-items: center; gap: 12px; margin-bottom: 12px; flex-wrap: wrap; }
.lib-nav { display: flex; gap: 4px; overflow-x: auto; flex: 1; }
.lib-tab { flex: 1; padding: 8px 10px; border-radius: 8px; border: 1px solid var(--l-line); background: transparent; color: var(--l-text2);
  font-size: var(--l-fs); font-weight: 500; white-space: nowrap; }
.lib-tab.active { background: var(--l-line); color: var(--l-text); }
.lib-stats { color: var(--l-text2); font-size: var(--l-fs-sm); white-space: nowrap; }
.lib-actions { display: flex; align-items: center; gap: 8px; margin: 4px 0 12px; }
.lib-actions.wrap { flex-wrap: wrap; }
.lib-btn { background: var(--l-line); color: var(--l-text); border: 1px solid transparent; border-radius: 8px; padding: 8px 12px;
  font-size: var(--l-fs); font-weight: 500; }
.lib-btn:hover { border-color: var(--l-accent); }
.lib-btn.accent { background: var(--l-accent); color: var(--l-ink); font-weight: 600; }
.lib-btn.on { border-color: var(--l-accent); color: var(--l-accent); }
.lib-btn.danger { color: var(--l-danger); }
.lib-head { display: flex; align-items: center; gap: 12px; margin: 4px 0 8px; }
.lib-head-text { min-width: 0; flex: 1; }
.lib-head-title { font-size: var(--l-fs-title); font-weight: 650; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.lib-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(140px, 1fr)); gap: 12px; }
.lib-card { background: transparent; color: var(--l-text); border: none; padding: 0; text-align: left; font-weight: 400; min-width: 0; display: block; }
.lib-card-art { position: relative; }
.lib-card-open { width: 100%; }
.lib-card-title { font-size: var(--l-fs); font-weight: 600; margin-top: 6px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.lib-card-sub { font-size: 12px; color: var(--l-text2); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.lib-card-play { position: absolute; right: 8px; bottom: 8px; width: 40px; height: 40px; border-radius: 50%; border: none;
  background: var(--l-accent); color: var(--l-ink); font-size: 16px; display: none; align-items: center; justify-content: center;
  box-shadow: 0 4px 12px rgba(0,0,0,0.35); }
.lib-art { width: 100%; aspect-ratio: 1; border-radius: 8px; object-fit: cover; display: block; background: linear-gradient(135deg, #0e1628, #3a225c); }
.lib-art.empty { opacity: 0.55; }
.lib-art.big { width: 96px; flex: none; }
.lib-art.small { width: 44px; flex: none; }
.lib-tracks li { display: flex; align-items: center; gap: 10px; padding: 10px 0; border-top: 1px solid var(--l-line); cursor: pointer; }
.lib-tracks li:first-child { border-top: none; }
.lib-tracks .meta { flex: 1; min-width: 0; text-align: left; }
.lib-tracks .title { font-weight: 600; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.lib-track.current .title { color: var(--l-accent); }
.lib-track.off { opacity: 0.45; }
.lib-no { width: 26px; flex: none; text-align: right; color: var(--l-text2); font-size: var(--l-fs-sm); font-variant-numeric: tabular-nums; }
.lib-dur { flex: none; color: var(--l-text2); font-size: var(--l-fs-sm); font-variant-numeric: tabular-nums; }
.lib-col { display: none; }
.aqlib button.icon.lib-more { width: 36px; height: 36px; font-size: 18px; border-color: transparent; }
.lib-sheet { position: fixed; inset: 0; background: rgba(0,0,0,0.55); display: flex; align-items: flex-end; justify-content: center; z-index: 1200; }
.lib-sheet-card { width: 100%; max-width: 640px; background: var(--l-panel); border-top: 1px solid var(--l-line); border-radius: 14px 14px 0 0;
  padding: 12px 16px max(16px, env(safe-area-inset-bottom)); display: flex; flex-direction: column; gap: 8px; max-height: 70vh; overflow-y: auto; }
.lib-sheet-title { color: var(--l-text2); font-size: var(--l-fs); margin-bottom: 4px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.lib-sheet-text { color: var(--l-text); font-size: var(--l-fs); margin-bottom: 4px; white-space: normal; }
.lib-sheet-btn { width: 100%; background: var(--l-line); color: var(--l-text); font-weight: 500; text-align: left; border: 1px solid transparent;
  border-radius: 10px; padding: 12px 16px; font-size: 15px; }
.lib-sheet-btn:hover, .lib-sheet-btn:focus-visible { border-color: var(--l-accent); outline: none; }
.lib-sheet-btn.secondary { background: transparent; border: 1px solid var(--l-line); text-align: center; }
.lib-sheet-btn.accent { background: var(--l-accent); color: var(--l-ink); text-align: center; font-weight: 600; }
.lib-sheet-btn.danger { color: var(--l-danger); }
.lib-sheet-btn.accent.danger { background: var(--l-danger); color: #fff; }
.lib-sheet-row { display: flex; gap: 8px; }
.lib-sheet-row .lib-sheet-btn { flex: 1; text-align: center; }
.lib-sheet input, .lib-form input, .lib-cond input { width: 100%; padding: 10px 12px; border-radius: 10px; border: 1px solid var(--l-line);
  background: var(--l-input); color: var(--l-text); font-size: 15px; box-sizing: border-box; font-family: inherit; }
.lib-sheet input:focus, .lib-form input:focus, .lib-cond input:focus { outline: 2px solid var(--l-accent); outline-offset: 1px; }
.lib-toast { position: fixed; left: 50%; bottom: max(24px, env(safe-area-inset-bottom)); transform: translateX(-50%); background: var(--l-line);
  color: var(--l-text); padding: 10px 16px; border-radius: 10px; font-size: var(--l-fs); z-index: 1300; max-width: 90vw; }
.lib-form label { display: block; margin: 10px 0; }
.lib-form label > span { display: block; font-size: var(--l-fs-sm); color: var(--l-text2); margin-bottom: 4px; }
.lib-form select, .lib-cond select { padding: 10px; border-radius: 10px; border: 1px solid var(--l-line); background: var(--l-input); color: var(--l-text); font-size: 15px; }
.lib-form > label > select { width: 100%; }
.lib-conds { display: flex; flex-direction: column; gap: 8px; margin: 4px 0 8px; }
.lib-cond { display: flex; flex-wrap: wrap; align-items: center; gap: 6px; }
.lib-cond select { flex: 1 1 40%; min-width: 0; }
.lib-cond input { flex: 1 1 40%; min-width: 0; width: auto; }
.lib-check { display: flex !important; align-items: center; gap: 6px; margin: 0 !important; }
.lib-check input { width: auto; }
.lib-check span { margin: 0 !important; }
.lib-thead { display: none; }

/* ---- asztali elrendezés (PC) ---- */
.aqlib-desktop { --l-fs: var(--aq-fs-body, 12px); --l-fs-sm: var(--aq-fs-sub, 11px); --l-fs-title: 20px; font-size: var(--l-fs); }
.aqlib-desktop .lib-nav { flex: 0 1 auto; }
.aqlib-desktop button.icon { width: 36px; height: 36px; font-size: 18px; }
.aqlib-desktop .lib-tab { flex: 0 0 auto; padding: 7px 14px; }
.aqlib-desktop .lib-btn { padding: 7px 12px; }
.aqlib-desktop .lib-grid { grid-template-columns: repeat(auto-fill, minmax(168px, 1fr)); gap: 20px 18px; }
.aqlib-desktop .lib-card:hover .lib-card-play, .aqlib-desktop .lib-card:focus-within .lib-card-play { display: inline-flex; }
.aqlib-desktop .lib-card:hover .lib-card-title { color: var(--l-accent); }
.aqlib-desktop .lib-art.big { width: 168px; }
.aqlib-desktop .lib-head { gap: 20px; margin: 8px 0 14px; }
.aqlib-desktop .lib-tracks li, .aqlib-desktop .lib-thead { display: grid; gap: 12px; align-items: center; padding: 6px 10px; border-top: none; border-radius: 6px; }
.aqlib-desktop .lib-tracks.cols4 li, .aqlib-desktop .lib-thead.cols4 { grid-template-columns: 32px minmax(0, 2fr) minmax(0, 1.2fr) 56px 32px; }
.aqlib-desktop .lib-tracks.cols5 li, .aqlib-desktop .lib-thead.cols5 { grid-template-columns: 32px minmax(0, 2fr) minmax(0, 1.2fr) minmax(0, 1.2fr) 56px 32px; }
.aqlib-desktop .lib-tracks li.lib-folder { grid-template-columns: 44px minmax(0, 1fr); }
.aqlib-desktop .lib-tracks li:hover { background: var(--l-hover); }
.aqlib-desktop .lib-tracks li:focus { outline: none; background: var(--l-soft); }
.aqlib-desktop .lib-tracks li:focus-visible { outline: 2px solid var(--l-accent); outline-offset: -2px; }
.aqlib-desktop .lib-tracks .detail.sub { display: none; }
.aqlib-desktop .lib-col { display: block; color: var(--l-text2); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.aqlib-desktop .lib-thead { color: var(--l-text2); font-size: var(--l-fs-sm); text-transform: uppercase; letter-spacing: 0.06em;
  border-bottom: 1px solid var(--l-line); border-radius: 0; margin-bottom: 4px; }
.aqlib-desktop .lib-thead span:nth-last-child(2) { text-align: right; }
.aqlib-desktop .lib-dur { text-align: right; }
.aqlib-desktop button.icon.lib-more { width: 28px; height: 28px; font-size: 16px; opacity: 0; }
.aqlib-desktop .lib-tracks li:hover button.icon.lib-more, .aqlib-desktop .lib-tracks li:focus-within button.icon.lib-more { opacity: 1; }
.aqlib-desktop .lib-sheet { align-items: center; }
.aqlib-desktop .lib-sheet-card { max-width: 420px; border: 1px solid var(--l-line); border-radius: 12px; padding: 16px; box-shadow: 0 16px 40px rgba(0,0,0,0.4); }
.aqlib-desktop .lib-sheet-btn { padding: 9px 12px; font-size: var(--l-fs); }
.aqlib-desktop .lib-form { max-width: 640px; }
.aqlib-desktop .lib-form input, .aqlib-desktop .lib-cond input, .aqlib-desktop .lib-form select, .aqlib-desktop .lib-cond select,
.aqlib-desktop .lib-sheet input { font-size: var(--l-fs); padding: 8px 10px; }
.lib-ctx { position: fixed; z-index: 1250; min-width: 200px; max-width: 320px; background: var(--l-panel); border: 1px solid var(--l-line);
  border-radius: 8px; padding: 4px; box-shadow: 0 12px 32px rgba(0,0,0,0.4); display: flex; flex-direction: column; }
.lib-ctx-title { color: var(--l-text2); font-size: var(--l-fs-sm); padding: 6px 10px 4px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.lib-ctx button { text-align: left; background: transparent; color: var(--l-text); border: none; border-radius: 5px; padding: 7px 10px; font-size: var(--l-fs); }
.lib-ctx button:hover, .lib-ctx button:focus-visible { background: var(--l-hover); outline: none; }
.lib-ctx button.danger { color: var(--l-danger); }`;

  window.AqLibrary = function (opts) {
    const send = opts.send;
    let S = STR[opts.lang === "hu" ? "hu" : "en"];
    const t = (k, v) => (S[k] || k).replace(/\{(\w+)\}/g, (_, n) => (v && n in v ? v[n] : ""));
    const root = opts.root;
    const desktop = opts.layout === "desktop";

    if (!document.getElementById("aqlib-style")) {
      const st = document.createElement("style");
      st.id = "aqlib-style";
      st.textContent = LIB_CSS;
      document.head.appendChild(st);
    }
    root.classList.add("aqlib", desktop ? "aqlib-desktop" : "aqlib-phone");

    const el = (tag, cls, text) => {
      const e = document.createElement(tag);
      if (cls) e.className = cls;
      if (text !== undefined && text !== null) e.textContent = text;
      return e;
    };
    const btn = (text, cls, onclick) => {
      const b = el("button", cls || "lib-btn", text);
      b.type = "button";
      b.onclick = onclick;
      return b;
    };
    const fmt = s => {
      s = Math.max(0, Math.round(s || 0));
      const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), r = s % 60;
      return (h ? h + ":" + String(m).padStart(2, "0") : m) + ":" + String(r).padStart(2, "0");
    };
    const errText = e => {
      const k = "e_" + String(e || "").split(":")[0];
      return S[k] || String(e || "");
    };

    // ---- Forrás (PC zenetára vagy a telefon helyi zenéje; az utóbbi csak a telefonos alkalmazásban)
    let source = "pc";
    let phoneIndex = null;
    let phoneScanning = false;
    const MobileLib = (window.Capacitor && window.Capacitor.Plugins && window.Capacitor.Plugins.AqMobileLibrary) || null;
    try {
      const saved = localStorage.getItem("aq_phone_library_cache");
      if (saved) phoneIndex = JSON.parse(saved);
    } catch (e) {}

    // ---- váz: forrásválasztó (telefon), alnézet-választó (+ statisztika a PC-n), tartalom, felugró lap, üzenetsáv
    root.replaceChildren();
    const srcNav = el("div", "lib-src-nav");
    const btnSrcPc = btn("", "lib-src-tab active", () => setSource("pc"));
    const btnSrcPhone = btn("", "lib-src-tab", () => setSource("phone"));
    srcNav.append(btnSrcPc, btnSrcPhone);
    srcNav.hidden = desktop;

    const topbar = el("div", "lib-topbar");
    const nav = el("div", "lib-nav");
    const stats = el("span", "lib-stats");
    const rootsBtn = btn("", "lib-btn", () => opts.onOpenRoots && opts.onOpenRoots());
    stats.hidden = rootsBtn.hidden = !desktop;
    if (!opts.onOpenRoots) rootsBtn.hidden = true;
    topbar.append(nav, stats, rootsBtn);
    const body = el("div", "lib-body");
    const sheet = el("div", "lib-sheet");
    sheet.hidden = true;
    const toastEl = el("div", "lib-toast");
    toastEl.hidden = true;
    root.append(srcNav, topbar, body, sheet, toastEl);
    const navBtns = {};
    ["albums", "folders", "lists", "queue"].forEach(k => {
      navBtns[k] = btn("", "lib-tab", () => open({ kind: k }, true));
      nav.appendChild(navBtns[k]);
    });
    function setStaticTexts() {
      btnSrcPc.textContent = t("srcPc");
      btnSrcPhone.textContent = t("srcPhone");
      Object.keys(navBtns).forEach(k => { navBtns[k].textContent = t(k); });
      rootsBtn.textContent = "📁 " + t("roots");
      renderStats();
    }

    function setSource(s) {
      if (source === s) return;
      source = s;
      btnSrcPc.classList.toggle("active", source === "pc");
      btnSrcPhone.classList.toggle("active", source === "phone");
      open({ kind: "albums" }, true);
      if (source === "phone" && !phoneIndex && MobileLib) {
        scanPhoneLibrary();
      }
    }

    let toastTimer = null;
    function toast(text) {
      toastEl.textContent = text;
      toastEl.hidden = false;
      clearTimeout(toastTimer);
      toastTimer = setTimeout(() => { toastEl.hidden = true; }, 2500);
    }

    // ---- felugró elemek: menü (telefonon alsó lap, PC-n a mutató mellett), párbeszédablak (szöveg, megerősítés)
    let ctx = null;
    function closeCtx() { if (ctx) { ctx.remove(); ctx = null; } }
    document.addEventListener("pointerdown", e => { if (ctx && !ctx.contains(e.target)) closeCtx(); }, true);
    window.addEventListener("blur", closeCtx);
    window.addEventListener("resize", closeCtx);

    /** Felugró menü: [{label, run, danger}]. Ha van esemény (jobb gomb / ⋯ a PC-n), a mutató mellett nyílik. */
    function menu(title, items, ev) {
      if (desktop && ev && typeof ev.clientX === "number") {
        closeCtx();
        ctx = el("div", "lib-ctx");
        ctx.setAttribute("role", "menu");
        if (title) ctx.appendChild(el("div", "lib-ctx-title", title));
        items.forEach(it => {
          const b = btn(it.label, it.danger ? "danger" : "", () => { closeCtx(); it.run(); });
          b.setAttribute("role", "menuitem");
          ctx.appendChild(b);
        });
        ctx.addEventListener("keydown", e => {
          const bs = [...ctx.querySelectorAll("button")];
          const i = bs.indexOf(document.activeElement);
          if (e.key === "Escape") { e.preventDefault(); closeCtx(); }
          else if (e.key === "ArrowDown") { e.preventDefault(); bs[(i + 1) % bs.length].focus(); }
          else if (e.key === "ArrowUp") { e.preventDefault(); bs[(i - 1 + bs.length) % bs.length].focus(); }
        });
        root.appendChild(ctx);
        const r = ctx.getBoundingClientRect();
        const x = Math.min(ev.clientX, window.innerWidth - r.width - 8);
        const y = Math.min(ev.clientY, window.innerHeight - r.height - 8);
        ctx.style.left = Math.max(8, x) + "px";
        ctx.style.top = Math.max(8, y) + "px";
        const first = ctx.querySelector("button");
        if (first) first.focus();
        return;
      }
      sheet.replaceChildren();
      const card = el("div", "lib-sheet-card");
      if (title) card.appendChild(el("div", "lib-sheet-title", title));
      items.forEach(it => {
        card.appendChild(btn(it.label, "lib-sheet-btn" + (it.danger ? " danger" : ""), () => { closeMenu(); it.run(); }));
      });
      card.appendChild(btn(t("cancel"), "lib-sheet-btn secondary", closeMenu));
      sheet.appendChild(card);
      sheet.hidden = false;
      sheet.onclick = e => { if (e.target === sheet) closeMenu(); };
    }
    function closeMenu() { sheet.hidden = true; sheet.replaceChildren(); }

    /**
     * Saját párbeszédablak a böngésző prompt() / confirm() helyett (a téma színeivel, minden WebView-ban egyformán).
     * input: szövegbevitel (Promise<string|null>), különben megerősítés (Promise<boolean>). Enter: rendben, Esc: mégse.
     */
    function ask(text, o) {
      o = o || {};
      return new Promise(resolve => {
        closeCtx();
        sheet.replaceChildren();
        const card = el("div", "lib-sheet-card");
        card.setAttribute("role", "dialog");
        card.setAttribute("aria-modal", "true");
        card.appendChild(el("div", "lib-sheet-text", text));
        let input = null;
        if (o.input) {
          input = el("input");
          input.type = "text";
          input.value = o.value || "";
          card.appendChild(input);
        }
        let done = false;
        const finish = val => {
          if (done) return;
          done = true;
          closeMenu();
          resolve(val);
        };
        const okVal = () => (o.input ? input.value : true);
        const cancelVal = () => (o.input ? null : false);
        const row = el("div", "lib-sheet-row");
        row.append(btn(t("cancel"), "lib-sheet-btn secondary", () => finish(cancelVal())),
                   btn(o.okLabel || t("ok"), "lib-sheet-btn accent" + (o.danger ? " danger" : ""), () => finish(okVal())));
        card.appendChild(row);
        card.addEventListener("keydown", e => {
          if (e.key === "Escape") { e.preventDefault(); finish(cancelVal()); }
          else if (e.key === "Enter" && (input ? document.activeElement === input : true)) { e.preventDefault(); finish(okVal()); }
        });
        sheet.appendChild(card);
        sheet.hidden = false;
        sheet.onclick = e => { if (e.target === sheet) finish(cancelVal()); };
        // törlésnél a „Mégse” kapja a fókuszt (egy véletlen Enter ne töröljön)
        setTimeout(() => {
          if (input) { input.focus(); input.select(); } else (o.danger ? row.firstChild : row.lastChild).focus();
        }, 0);
      });
    }

    // ---- borítók: gyorsítótár, legfeljebb néhány kérés egyszerre, csak a látható képekhez
    const art = new Map();        // id -> data URL ("" = nincs borító)
    const artWait = [];           // sorban álló elemek: { id, uri }
    const artBusy = new Set();
    const artObserver = "IntersectionObserver" in window ? new IntersectionObserver(entries => {
      entries.forEach(en => {
        if (!en.isIntersecting) return;
        artObserver.unobserve(en.target);
        wantArt(en.target.dataset.art, en.target.dataset.uri);
      });
    }, { rootMargin: "200px" }) : null;

    function wantArt(id, extraUri) {
      if (!id || art.has(id) || artBusy.has(id)) return;
      if (artWait.some(item => item.id === id)) return;
      artWait.push({ id, uri: extraUri || null });
      pumpArt();
    }

    function pumpArt() {
      while (artBusy.size < ART_PARALLEL && artWait.length) {
        const item = artWait.shift();
        const id = item.id;
        const uri = item.uri;
        artBusy.add(id);

        if (id.startsWith("a_m_") || id.startsWith("t_m_") || uri) {
          if (MobileLib && uri) {
            MobileLib.getTrackArt({ uri }).then(res => {
              gotArt({ id, data: res && res.data ? res.data : "", mime: res && res.mime ? res.mime : "image/jpeg" });
            }).catch(() => {
              gotArt({ id, data: "" });
            });
          } else {
            gotArt({ id, data: "" });
          }
        } else {
          send({ type: "library_art", id });
        }
      }
    }

    function artImg(id, hasArt, cls, extraUri) {
      const img = el("img", cls || "lib-art");
      img.alt = "";
      if (!hasArt) { img.classList.add("empty"); return img; }
      img.dataset.art = id;
      if (extraUri) img.dataset.uri = extraUri;
      if (art.has(id)) {
        if (art.get(id)) img.src = art.get(id); else img.classList.add("empty");
      } else if (artObserver) {
        artObserver.observe(img);
      } else {
        wantArt(id, extraUri);
      }
      return img;
    }

    function gotArt(m) {
      artBusy.delete(m.id);
      const url = m.data ? `data:${m.mime || "image/jpeg"};base64,${m.data}` : "";
      art.set(m.id, url);
      root.querySelectorAll(`img[data-art="${CSS.escape(m.id)}"]`).forEach(img => {
        if (url) img.src = url; else img.classList.add("empty");
      });
      pumpArt();
    }

    // ---- állapot és navigáció
    let view = { kind: "albums" };
    const history = [];
    let info = null, albums = [], albumsTotal = -1, albumsLoading = false;
    let queue = null, queueRev = -1, queueIndex = -2, lists = null, listsRev = -1, statsRev = -1;
    let pendingOpenList = false;   // létrehozás / mentés után a válaszként jövő listát nyitjuk meg
    let pickFor = null;            // listához adás: a forrás (üzenetrész), amíg a felhasználó listát választ
    let lastRendered = null;       // a legutóbb kirajzolt album / mappa / lista (nyelvváltáskor újrarajzoláshoz)

    function open(v, top) {
      closeCtx();
      if (top) history.length = 0;
      else history.push(view);
      view = v;
      render();
      if (source === "pc") fetchFor(v);
    }

    function back() {
      closeCtx();
      view = history.pop() || { kind: "albums" };
      render();
      if (source === "pc") fetchFor(view);
    }
    function fetchFor(v) {
      if (v.kind === "albums") {
        send({ type: "library_info" });
        if (albumsTotal < 0) moreAlbums();
      } else if (v.kind === "album") send({ type: "library_album", id: v.id });
      else if (v.kind === "folder") send({ type: "library_folder", id: v.id || "" });
      else if (v.kind === "lists") send({ type: "playlist_list" });
      else if (v.kind === "list") send({ type: "playlist_get", id: v.id });
      else if (v.kind === "queue") send({ type: "queue_get" });
    }
    function moreAlbums() {
      if (albumsLoading || (albumsTotal >= 0 && albums.length >= albumsTotal)) return;
      albumsLoading = true;
      send({ type: "library_albums", offset: albums.length, limit: PAGE });
    }
    function renderStats() {
      stats.textContent = info && !info.scanning && info.tracks !== undefined
        ? t("stats", { tracks: info.tracks, albums: info.albums }) : "";
    }

    function header(title, sub, withBack) {
      const h = el("div", "lib-head");
      if (withBack) {
        const b = btn("‹", "icon", back);
        b.title = t("back");
        b.setAttribute("aria-label", t("back"));
        h.appendChild(b);
      }
      const txt = el("div", "lib-head-text");
      txt.appendChild(el("div", "lib-head-title", title));
      if (sub) txt.appendChild(el("div", "detail", sub));
      h.appendChild(txt);
      return h;
    }

    /** Asztali számtáblázat fejléce (#, cím, előadó, [album], hossz). */
    function tracksList(showAlbum) {
      const ul = el("ul", "lib-tracks" + (desktop ? (showAlbum ? " cols5" : " cols4") : ""));
      if (!desktop) return { ul, nodes: [ul] };
      const head = el("div", "lib-thead " + (showAlbum ? "cols5" : "cols4"));
      head.append(el("span", null, "#"), el("span", null, t("colTitle")), el("span", null, t("colArtist")));
      if (showAlbum) head.appendChild(el("span", null, t("colAlbum")));
      head.append(el("span", null, t("colDuration")), el("span"));
      return { ul, nodes: [head, ul] };
    }

    /**
     * Egy szám sora. Telefonon: koppintásra lejátszás, a ⋯ gombra menü. PC-n: dupla kattintás vagy Enter lejátszik,
     * jobb gomb vagy a ⋯ menüt nyit, külön oszlopban az előadó (és az album).
     */
    function trackRow(tr, o) {
      const li = el("li", "lib-track" + (o.current ? " current" : "") + (tr.available === false ? " off" : ""));
      if (o.number !== undefined || desktop) li.appendChild(el("span", "lib-no", o.number !== undefined ? o.number : ""));
      const meta = el("div", "meta");
      meta.appendChild(el("div", "title", tr.title || "—"));
      let statusTag = null;
      if (tr.id && tr.id.startsWith("p_")) {
        if (tr.matched) statusTag = "💾 " + t("matchedOnPc");
        else if (tr.available !== false) statusTag = "📱 " + t("onPhone");
        else statusTag = "📱 " + t("phoneDisconnected");
      } else if (tr.available === false) {
        statusTag = t("unavailable");
      }
      const sub = [tr.artist, o.showAlbum ? tr.album : null, statusTag].filter(Boolean).join(" · ");
      if (sub) meta.appendChild(el("div", "detail sub", sub));
      if (desktop && statusTag) meta.appendChild(el("div", "detail", statusTag));
      li.appendChild(meta);
      li.appendChild(el("span", "lib-col", tr.artist || ""));
      if (o.showAlbum) li.appendChild(el("span", "lib-col", tr.album || ""));
      li.appendChild(el("span", "lib-dur", tr.duration > 0 ? fmt(tr.duration) : ""));
      const more = btn("⋯", "icon lib-more", e => { e.stopPropagation(); o.menu(e); });
      more.title = t("more");
      more.setAttribute("aria-label", t("more"));
      li.appendChild(more);
      if (desktop) {
        li.tabIndex = 0;
        li.ondblclick = () => o.play();
        li.onkeydown = e => {
          if (e.key === "Enter") { e.preventDefault(); o.play(); }
          else if (e.key === "ContextMenu" || (e.shiftKey && e.key === "F10")) {
            e.preventDefault();
            const r = li.getBoundingClientRect();
            o.menu({ clientX: r.left + 40, clientY: r.bottom });
          } else if (e.key === "ArrowDown" && li.nextElementSibling) { e.preventDefault(); li.nextElementSibling.focus(); }
          else if (e.key === "ArrowUp" && li.previousElementSibling) { e.preventDefault(); li.previousElementSibling.focus(); }
        };
        li.oncontextmenu = e => { e.preventDefault(); li.focus(); o.menu(e); };
      } else {
        li.onclick = () => o.play();
      }
      return li;
    }

    /** Mappa- vagy listasor: kattintásra (PC-n Enterre is) megnyílik. */
    function openRow(li, onOpen) {
      li.onclick = onOpen;
      if (desktop) {
        li.tabIndex = 0;
        li.onkeydown = e => {
          if (e.key === "Enter") { e.preventDefault(); onOpen(); }
          else if (e.key === "ArrowDown" && li.nextElementSibling) { e.preventDefault(); li.nextElementSibling.focus(); }
          else if (e.key === "ArrowUp" && li.previousElementSibling) { e.preventDefault(); li.previousElementSibling.focus(); }
        };
      }
      return li;
    }

    /** A szám (vagy forrás) általános menüje: következőként, a sor végére, listához. */
    function sourceMenu(title, src, extra, ev) {
      menu(title, [
        { label: t("playNext"), run: () => { send(Object.assign({ type: "queue_add", next: true }, src)); toast(t("queued")); } },
        { label: t("playLast"), run: () => { send(Object.assign({ type: "queue_add", next: false }, src)); toast(t("queued")); } },
        { label: t("addToList"), run: () => pickList(src) },
      ].concat(extra || []), ev);
    }
    function pickList(src) {
      pickFor = src;
      send({ type: "playlist_list" });
    }
    function showListPicker() {
      const own = (lists ? lists.items : []).filter(l => !l.smart);
      const src = pickFor;
      const items = own.map(l => ({ label: l.name, run: () => {
        const payload = Object.assign({}, src, { type: "playlist_add", id: l.id });
        if (src && src.source === "phone_track") payload.track_id = src.id;
        send(payload);
        toast(t("added", { name: l.name }));
      } }));
      items.push({ label: "＋ " + t("newList"), run: async () => {
        const n = await ask(t("listName"), { input: true });
        if (n && n.trim()) {
          const payload = Object.assign({}, src, { type: "playlist_create", name: n.trim() });
          if (src && src.source === "phone_track") payload.track_id = src.id;
          send(payload);
          toast(t("added", { name: n.trim() }));
        }
      } });
      pickFor = null;
      menu(t("chooseList"), items);
    }

    function playSource(src, start) {
      send({ type: "queue_shuffle", on: false });
      send(Object.assign({ type: "queue_play" }, src, start ? { start } : {}));
      opts.onPlay && opts.onPlay();
    }
    function playButtons(src, tracks) {
      const row = el("div", "lib-actions");
      row.appendChild(btn("▶ " + t("play"), "lib-btn accent", () => playSource(src)));
      row.appendChild(btn("🔀 " + t("shuffle"), "lib-btn", () => {
        // keverve: véletlen számtól indul, a többi a keverés szerint
        const start = tracks && tracks.length ? tracks[Math.floor(Math.random() * tracks.length)].id : undefined;
        send({ type: "queue_shuffle", on: true });
        send(Object.assign({ type: "queue_play", start }, src));
        opts.onPlay && opts.onPlay();
      }));
      return row;
    }
    function moreButton(onMenu) {
      const b = btn("⋯", "icon", e => onMenu(e));
      b.title = t("more");
      b.setAttribute("aria-label", t("more"));
      return b;
    }

    // ---- nézetek
    function render() {
      Object.entries(navBtns).forEach(([k, b]) => {
        const top = view.kind === k || (k === "albums" && view.kind === "album") ||
                    (k === "folders" && view.kind === "folder") || (k === "lists" && (view.kind === "list" || view.kind === "smartEdit"));
        b.classList.toggle("active", top);
        b.setAttribute("aria-pressed", top ? "true" : "false");
      });
      renderStats();
      body.replaceChildren();
      if (source === "phone") {
        if (view.kind === "albums") renderPhoneAlbums();
        else if (view.kind === "album") renderPhoneAlbum(view.album);
        else if (view.kind === "folders") renderPhoneFolders();
        else if (view.kind === "folder") renderPhoneFolder(view.folder);
        else if (view.kind === "lists") renderLists();
        else if (view.kind === "list") body.appendChild(el("div", "hint", t("loading")));
        else if (view.kind === "queue") renderQueue();
        else if (view.kind === "smartEdit") renderSmartEditor();
        return;
      }
      if (view.kind === "albums") renderAlbums();
      else if (view.kind === "album") { if (lastRendered && lastRendered.kind === "album" && lastRendered.data.id === view.id) renderAlbum(lastRendered.data); else body.appendChild(el("div", "hint", t("loading"))); }
      else if (view.kind === "folders") open({ kind: "folder", id: "" }, true);
      else if (view.kind === "folder") { if (lastRendered && lastRendered.kind === "folder" && (lastRendered.data.id || "") === (view.id || "")) renderFolder(lastRendered.data); else body.appendChild(el("div", "hint", t("loading"))); }
      else if (view.kind === "lists") renderLists();
      else if (view.kind === "list") { if (lastRendered && lastRendered.kind === "list" && lastRendered.data.id === view.id) renderList(lastRendered.data); else body.appendChild(el("div", "hint", t("loading"))); }
      else if (view.kind === "queue") renderQueue();
      else if (view.kind === "smartEdit") renderSmartEditor();
    }

    // ---- Telefonos zenetár (4a: SAF mappák, címkék és bejárás)
    async function scanPhoneLibrary() {
      if (!MobileLib) {
        toast(t("onlyAndroid"));
        return;
      }
      phoneScanning = true;
      render();
      try {
        const res = await MobileLib.scanLibrary();
        phoneIndex = res;
        try { localStorage.setItem("aq_phone_library_cache", JSON.stringify(res)); } catch (e) {}
        toast(t("phoneTracks", { n: (res && res.total_tracks) || 0 }));
      } catch (err) {
        toast(t("error", { e: (err && err.message) || err }));
      } finally {
        phoneScanning = false;
        render();
      }
    }

    async function addPhoneFolder() {
      if (!MobileLib) {
        toast(t("onlyAndroid"));
        return;
      }
      try {
        const res = await MobileLib.chooseFolder();
        if (res && res.uri) {
          toast(t("added", { name: res.name || "Mappa" }));
          await scanPhoneLibrary();
        }
      } catch (err) {
        if (err && err.message && !err.message.includes("nem választott")) {
          toast(t("error", { e: err.message }));
        }
      }
    }

    async function removePhoneFolder(uri, name) {
      if (!MobileLib) return;
      if (!(await ask(t("removeFolderConfirm", { name }), { okLabel: t("delete"), danger: true }))) return;
      try {
        await MobileLib.removeFolder({ uri });
        toast(t("saved", { name: name || "Mappa" }));
        await scanPhoneLibrary();
      } catch (err) {
        toast(t("error", { e: err.message }));
      }
    }

    // ------------------------------------------------------------------ Telefonos zeneátvitel (4b)
    let phoneStreaming = null;

    function getTrackExt(tr) {
      if (!tr) return "flac";
      const name = tr.file || tr.title || "";
      const dot = name.lastIndexOf(".");
      if (dot > 0) {
        const ext = name.substring(dot + 1).toLowerCase();
        if (["flac", "wav", "mp3", "ogg", "m4a", "aac", "opus"].includes(ext)) return ext;
      }
      return "flac";
    }

    function base64ToUint8Array(b64) {
      const binary = atob(b64);
      const len = binary.length;
      const bytes = new Uint8Array(len);
      for (let i = 0; i < len; i++) bytes[i] = binary.charCodeAt(i);
      return bytes;
    }

    function buildAepfFrame(flags, id, ext, offset, totalSize, chunkBytes) {
      const enc = new TextEncoder();
      const idBytes = enc.encode(id);
      const extBytes = enc.encode(ext);
      const idLen = Math.min(idBytes.length, 64);
      const extLen = Math.min(extBytes.length, 16);
      const dataLen = chunkBytes ? chunkBytes.length : 0;
      const headerLen = 28 + idLen + extLen;
      const totalLen = headerLen + dataLen;

      const buffer = new ArrayBuffer(totalLen);
      const dv = new DataView(buffer);
      const u8 = new Uint8Array(buffer);

      u8[0] = 0x41; u8[1] = 0x45; u8[2] = 0x50; u8[3] = 0x46; // "AEPF"
      u8[4] = 1; // version
      u8[5] = flags;
      u8[6] = idLen;
      u8[7] = extLen;

      dv.setBigUint64(8, BigInt(offset), true);
      dv.setBigUint64(16, BigInt(totalSize), true);
      dv.setUint32(24, dataLen, true);

      u8.set(idBytes.subarray(0, idLen), 28);
      u8.set(extBytes.subarray(0, extLen), 28 + idLen);
      if (dataLen > 0) {
        u8.set(chunkBytes, headerLen);
      }
      return buffer;
    }

    async function streamNextChunk() {
      if (!phoneStreaming) return;
      const { tr, ext, offset, totalSize, chunkSize } = phoneStreaming;
      if (!MobileLib) return;

      try {
        const res = await MobileLib.readTrackChunk({
          uri: tr.uri,
          offset: offset,
          length: chunkSize || 65536
        });

        const bytes = res.data ? base64ToUint8Array(res.data) : new Uint8Array(0);
        const bytesRead = res.bytesRead !== undefined ? res.bytesRead : bytes.length;
        const eof = !!res.eof || (bytesRead < (chunkSize || 65536)) || ((offset + bytesRead) >= totalSize);

        const flags = eof ? 0x01 : 0x00;
        const frame = buildAepfFrame(flags, tr.id, ext, offset, totalSize, bytes);
        send(frame);

        phoneStreaming.offset += bytesRead;
        if (!eof && bytesRead > 0) {
          setTimeout(streamNextChunk, 10);
        }
      } catch (err) {
        toast(t("error", { e: err.message || err }));
        phoneStreaming = null;
      }
    }

    function promptCopyChoice(tr, onChosen) {
      sheet.replaceChildren();
      const card = el("div", "lib-sheet-card");
      card.appendChild(el("div", "lib-sheet-title", t("copyConfirmPrompt", { title: tr.title || tr.file || "Szám" })));

      const choose = (save, pref) => {
        closeMenu();
        if (pref) localStorage.setItem("aq_phone_copy_pref", pref);
        onChosen(save);
      };

      card.appendChild(btn(t("optPlayOnly"), "lib-sheet-btn", () => choose(false, null)));
      card.appendChild(btn(t("optCopyOnce"), "lib-sheet-btn accent", () => choose(true, null)));
      card.appendChild(btn(t("optCopyAlways"), "lib-sheet-btn", () => choose(true, "always")));
      card.appendChild(btn(t("optNeverAsk"), "lib-sheet-btn secondary", () => choose(false, "never")));
      card.appendChild(btn(t("cancel"), "lib-sheet-btn secondary", () => {
        closeMenu();
        phoneStreaming = null;
      }));
      sheet.appendChild(card);
      sheet.hidden = false;
      sheet.onclick = e => {
        if (e.target === sheet) {
          closeMenu();
          phoneStreaming = null;
        }
      };
    }

    function pickCopyPrefMenu() {
      const cur = localStorage.getItem("aq_phone_copy_pref") || "ask";
      menu(t("copyPrefTitle"), [
        {
          label: (cur === "ask" ? "✓ " : "") + t("prefAsk"),
          run: () => { localStorage.setItem("aq_phone_copy_pref", "ask"); toast(t("prefSaved")); }
        },
        {
          label: (cur === "always" ? "✓ " : "") + t("prefAlways"),
          run: () => { localStorage.setItem("aq_phone_copy_pref", "always"); toast(t("prefSaved")); }
        },
        {
          label: (cur === "never" ? "✓ " : "") + t("prefNever"),
          run: () => { localStorage.setItem("aq_phone_copy_pref", "never"); toast(t("prefSaved")); }
        },
      ]);
    }

    function playPhoneTrack(tr, o) {
      if (!tr) return;
      const action = (o && o.action) || "play";
      const next = o && o.next;
      const forceCopy = !!(o && o.forceCopy);
      const ext = getTrackExt(tr);
      const pref = localStorage.getItem("aq_phone_copy_pref") || "ask";
      const saveToPc = forceCopy || (pref === "always");

      if (forceCopy) {
        toast(t("sendingTrack") + ": " + (tr.title || tr.file));
      } else {
        toast((action === "play" ? t("play") : t("queued")) + ": " + (tr.title || tr.file));
      }

      phoneStreaming = {
        tr,
        ext,
        action,
        next,
        forceCopy,
        saveToPc,
        offset: 0,
        totalSize: tr.size || 0,
        chunkSize: 65536
      };

      send({
        type: "phone_track_offer",
        id: tr.id,
        title: tr.title || "",
        artist: tr.artist || "",
        album: tr.album || "",
        duration: tr.duration || 0,
        size: tr.size || 0,
        ext
      });
    }

    function phoneTrackMenu(tr) {
      menu(tr.title || tr.file, [
        { label: "📥 " + t("copyToPc"), run: () => playPhoneTrack(tr, { action: "none", forceCopy: true }) },
        { label: t("playNext"), run: () => playPhoneTrack(tr, { action: "queue", next: true }) },
        { label: t("playLast"), run: () => playPhoneTrack(tr, { action: "queue", next: false }) },
        { label: "＋ " + t("addToList"), run: () => pickList({
          source: "phone_track",
          id: tr.id,
          title: tr.title || tr.file || "Phone Track",
          artist: tr.artist || "",
          album: tr.album || "",
          duration: tr.duration || 0
        }) },
      ]);
    }

    function renderPhoneAlbums() {
      const topBar = el("div", "lib-actions wrap");
      topBar.appendChild(btn("＋ " + t("addFolder"), "lib-btn accent", addPhoneFolder));
      topBar.appendChild(btn("🔄 " + t("rescan"), "lib-btn", scanPhoneLibrary));
      topBar.appendChild(btn("⚙️ " + t("copyPrefTitle"), "lib-btn", pickCopyPrefMenu));
      if (phoneIndex && phoneIndex.total_tracks !== undefined) {
        topBar.appendChild(el("span", "detail", t("phoneTracks", { n: phoneIndex.total_tracks })));
      }
      body.appendChild(topBar);

      if (phoneScanning) {
        body.appendChild(el("div", "hint", t("scanningPhone")));
        return;
      }

      if (!MobileLib) {
        const c = el("div", "lib-phone-card");
        c.appendChild(el("p", null, t("onlyAndroid")));
        body.appendChild(c);
        return;
      }

      const albumsList = (phoneIndex && phoneIndex.albums) || [];
      if (!albumsList.length) {
        const c = el("div", "lib-phone-card");
        c.appendChild(el("p", null, t("phoneEmpty")));
        c.appendChild(btn("＋ " + t("addFolder"), "lib-btn accent", addPhoneFolder));
        body.appendChild(c);
        return;
      }

      const grid = el("div", "lib-grid");
      albumsList.forEach(a => {
        const card = el("button", "lib-card");
        card.type = "button";
        card.appendChild(artImg(a.id, !!a.art_track_uri, null, a.art_track_uri));
        card.appendChild(el("div", "lib-card-title", a.name || "—"));
        card.appendChild(el("div", "lib-card-sub", a.artist || ""));
        card.onclick = () => open({ kind: "album", id: a.id, album: a });
        grid.appendChild(card);
      });
      body.appendChild(grid);
    }

    function renderPhoneAlbum(a) {
      if (!a) { back(); return; }
      body.replaceChildren();
      const tracks = (phoneIndex && phoneIndex.tracks) ? phoneIndex.tracks.filter(x => {
        const key = (x.album + "::" + (x.artist || "Ismeretlen")).toLowerCase();
        const aKey = (a.name + "::" + (a.artist || "Ismeretlen")).toLowerCase();
        return key === aKey || x.album.toLowerCase() === a.name.toLowerCase();
      }) : [];

      const sub = [a.artist, a.year || null, t("tracks", { n: tracks.length }), fmt(a.duration)]
        .filter(Boolean).join(" · ");
      const head = header(a.name || "—", sub, true);
      head.insertBefore(artImg(a.id, !!a.art_track_uri, "lib-art big", a.art_track_uri), head.children[1]);
      body.appendChild(head);

      const actions = el("div", "lib-actions");
      actions.appendChild(btn("▶ " + t("play"), "lib-btn accent", () => {
        if (tracks.length) playPhoneTrack(tracks[0]);
      }));
      actions.appendChild(btn("🔀 " + t("shuffle"), "lib-btn", () => {
        if (tracks.length) {
          const rand = tracks[Math.floor(Math.random() * tracks.length)];
          playPhoneTrack(rand);
        }
      }));
      if (tracks.length) {
        actions.appendChild(btn("＋ " + t("addToList"), "lib-btn", () => {
          pickList({
            source: "phone_tracks",
            items: tracks.map(tr => ({
              id: tr.id,
              title: tr.title || tr.file || "Phone Track",
              artist: tr.artist || "",
              album: tr.album || "",
              duration: tr.duration || 0
            }))
          });
        }));
      }
      body.appendChild(actions);

      const ul = el("ul", "lib-tracks");
      tracks.forEach((tr, idx) => {
        ul.appendChild(trackRow(tr, {
          number: idx + 1,
          showAlbum: false,
          play: () => playPhoneTrack(tr),
          menu: () => phoneTrackMenu(tr),
        }));
      });
      body.appendChild(ul);
    }

    function renderPhoneFolders() {
      const topBar = el("div", "lib-actions wrap");
      topBar.appendChild(btn("＋ " + t("addFolder"), "lib-btn accent", addPhoneFolder));
      topBar.appendChild(btn("🔄 " + t("rescan"), "lib-btn", scanPhoneLibrary));
      topBar.appendChild(btn("⚙️ " + t("copyPrefTitle"), "lib-btn", pickCopyPrefMenu));
      body.appendChild(topBar);

      if (phoneScanning) {
        body.appendChild(el("div", "hint", t("scanningPhone")));
        return;
      }

      const folders = (phoneIndex && phoneIndex.folders) || [];
      if (!folders.length) {
        const c = el("div", "lib-phone-card");
        c.appendChild(el("p", null, t("phoneEmpty")));
        c.appendChild(btn("＋ " + t("addFolder"), "lib-btn accent", addPhoneFolder));
        body.appendChild(c);
        return;
      }

      const ul = el("ul", "lib-tracks");
      folders.forEach(f => {
        const li = el("li", "lib-folder");
        const meta = el("div", "meta");
        meta.appendChild(el("div", "title", "📁 " + f.name));
        meta.appendChild(el("div", "detail", (f.path || f.name) + " · " + t("tracks", { n: f.track_count || 0 })));
        li.appendChild(meta);
        ul.appendChild(openRow(li, () => open({ kind: "folder", id: f.id, folder: f })));
      });
      body.appendChild(ul);
    }

    function renderPhoneFolder(f) {
      if (!f) { back(); return; }
      body.replaceChildren();
      const tracks = (phoneIndex && phoneIndex.tracks) ? phoneIndex.tracks.filter(x => x.folder_id === f.id) : [];

      const head = header(f.name, (f.path || "") + " · " + t("tracks", { n: tracks.length }), true);
      body.appendChild(head);

      const actions = el("div", "lib-actions");
      if (tracks.length) {
        actions.appendChild(btn("▶ " + t("play"), "lib-btn accent", () => playPhoneTrack(tracks[0])));
      }
      actions.appendChild(btn("🗑 " + t("delete"), "lib-btn danger", () => removePhoneFolder(f.uri, f.name)));
      body.appendChild(actions);

      const ul = el("ul", "lib-tracks");
      tracks.forEach((tr, idx) => {
        ul.appendChild(trackRow(tr, {
          number: idx + 1,
          showAlbum: true,
          play: () => playPhoneTrack(tr),
          menu: () => phoneTrackMenu(tr),
        }));
      });
      body.appendChild(ul);
    }

    function renderAlbums() {
      if (info && info.scanning) body.appendChild(el("div", "hint", t("scanning", { n: info.scanned || 0 })));
      if (albumsTotal === 0 && info && !info.scanning) { body.appendChild(el("div", "hint", t("empty"))); return; }
      if (albumsTotal < 0) { body.appendChild(el("div", "hint", t("loading"))); return; }
      const grid = el("div", "lib-grid");
      albums.forEach(a => {
        const card = el("div", "lib-card");
        const openBtn = el("button", "lib-card lib-card-open");
        openBtn.type = "button";
        const artWrap = el("div", "lib-card-art");
        artWrap.appendChild(artImg(a.id, a.art));
        openBtn.appendChild(artWrap);
        openBtn.appendChild(el("div", "lib-card-title", a.title || "—"));
        openBtn.appendChild(el("div", "lib-card-sub", a.various ? t("various") : (a.artist || "")));
        openBtn.onclick = () => open({ kind: "album", id: a.id });
        card.appendChild(openBtn);
        if (desktop) {
          // rámutatáskor a borítón: az album lejátszása megnyitás nélkül
          const play = btn("▶", "lib-card-play", e => { e.stopPropagation(); playSource({ source: "album", id: a.id }); });
          play.title = t("playAlbum");
          play.setAttribute("aria-label", t("playAlbum") + ": " + (a.title || ""));
          artWrap.appendChild(play);
          card.oncontextmenu = e => {
            e.preventDefault();
            sourceMenu(a.title, { source: "album", id: a.id }, [], e);
          };
        }
        grid.appendChild(card);
      });
      body.appendChild(grid);
      if (albums.length < albumsTotal) {
        // görgetéskor a következő adag
        const more = el("div", "hint", t("loading"));
        body.appendChild(more);
        if ("IntersectionObserver" in window) {
          const io = new IntersectionObserver(en => { if (en.some(x => x.isIntersecting)) { io.disconnect(); moreAlbums(); } });
          io.observe(more);
        } else moreAlbums();
      }
    }

    function renderAlbum(a) {
      lastRendered = { kind: "album", data: a };
      body.replaceChildren();
      const sub = [a.various ? t("various") : a.artist, a.year || null, t("tracks", { n: a.tracks.length }), fmt(a.duration)]
        .filter(Boolean).join(" · ");
      const head = header(a.title || "—", sub, true);
      head.insertBefore(artImg(a.id, a.art, "lib-art big"), head.children[1]);
      body.appendChild(head);
      const src = { source: "album", id: a.id };
      const actions = playButtons(src, a.tracks);
      actions.appendChild(moreButton(e => sourceMenu(a.title, src, [], e)));
      body.appendChild(actions);
      const list = tracksList(false);
      a.tracks.forEach((tr, i) => list.ul.appendChild(trackRow(tr, {
        number: tr.track || (desktop ? i + 1 : ""), showAlbum: false,   // címke nélkül a PC-n a sorszám
        play: () => playSource(src, tr.id),
        menu: ev => sourceMenu(tr.title, { source: "tracks", ids: [tr.id] }, [], ev),
      })));
      list.nodes.forEach(n => body.appendChild(n));
    }

    function renderFolder(f) {
      lastRendered = { kind: "folder", data: f };
      body.replaceChildren();
      const isRoot = !f.id;
      body.appendChild(header(isRoot ? t("roots") : f.name, isRoot ? null : t("tracks", { n: f.tracks.length }), !isRoot));
      if (isRoot && !f.folders.length) { body.appendChild(el("div", "hint", t("empty"))); return; }
      if (f.tracks.length) {
        const src = { source: "folder", id: f.id };
        const actions = playButtons(src, f.tracks);
        actions.appendChild(moreButton(e => sourceMenu(f.name, src, [], e)));
        body.appendChild(actions);
      }
      if (f.folders.length) {
        const ul = el("ul", "lib-tracks");
        f.folders.forEach(sf => {
          const li = el("li", "lib-folder");
          li.appendChild(artImg(sf.id, sf.art, "lib-art small"));
          const meta = el("div", "meta");
          meta.appendChild(el("div", "title", "📁 " + sf.name));
          meta.appendChild(el("div", "detail", t("tracks", { n: sf.tracks })));
          li.appendChild(meta);
          ul.appendChild(openRow(li, () => open({ kind: "folder", id: sf.id })));
        });
        body.appendChild(ul);
      }
      if (f.tracks.length) {
        const list = tracksList(true);
        f.tracks.forEach((tr, i) => list.ul.appendChild(trackRow(tr, {
          number: desktop ? i + 1 : undefined, showAlbum: true,
          play: () => playSource({ source: "folder", id: f.id }, tr.id),
          menu: ev => sourceMenu(tr.title, { source: "tracks", ids: [tr.id] }, [], ev),
        })));
        list.nodes.forEach(n => body.appendChild(n));
      }
      if (!isRoot && !f.folders.length && !f.tracks.length) body.appendChild(el("div", "hint", t("folderEmpty")));
    }

    function renderLists() {
      body.replaceChildren();
      const actions = el("div", "lib-actions");
      actions.appendChild(btn("＋ " + t("newList"), "lib-btn", async () => {
        const n = await ask(t("listName"), { input: true });
        if (n && n.trim()) { pendingOpenList = true; send({ type: "playlist_create", name: n.trim() }); }
      }));
      actions.appendChild(btn("✨ " + t("newSmart"), "lib-btn", () =>
        open({ kind: "smartEdit", id: null, name: "", rules: { match: "all", conditions: [], sort: "added", desc: true, limit: 0 } })));
      body.appendChild(actions);
      if (!lists) { body.appendChild(el("div", "hint", t("loading"))); return; }
      if (!lists.items.length) { body.appendChild(el("div", "hint", t("noLists"))); return; }
      const ul = el("ul", "lib-tracks");
      lists.items.forEach(l => {
        const li = el("li", "lib-folder");
        if (desktop) li.appendChild(el("span", "lib-no", l.smart ? "✨" : "🎵"));
        const meta = el("div", "meta");
        meta.appendChild(el("div", "title", (desktop ? "" : (l.smart ? "✨ " : "🎵 ")) + l.name));
        meta.appendChild(el("div", "detail", t("tracks", { n: l.count }) + " · " + fmt(l.duration)));
        li.appendChild(meta);
        ul.appendChild(openRow(li, () => open({ kind: "list", id: l.id })));
      });
      body.appendChild(ul);
    }

    function renderList(p) {
      lastRendered = { kind: "list", data: p };
      body.replaceChildren();
      const total = p.items.reduce((s, x) => s + (x.duration || 0), 0);
      body.appendChild(header((p.smart ? "✨ " : "") + p.name, t("tracks", { n: p.items.length }) + " · " + fmt(total), true));
      if (p.smart) body.appendChild(el("div", "hint", t("smartNote")));
      const src = { source: "playlist", id: p.id };
      const actions = playButtons(src, p.items.filter(x => x.available !== false));
      const listMenu = [
        { label: t("playNext"), run: () => { send({ type: "queue_add", next: true, source: "playlist", id: p.id }); toast(t("queued")); } },
        { label: t("playLast"), run: () => { send({ type: "queue_add", next: false, source: "playlist", id: p.id }); toast(t("queued")); } },
        { label: t("rename"), run: async () => {
          const n = await ask(t("listName"), { input: true, value: p.name });
          if (n && n.trim()) send({ type: "playlist_update", id: p.id, name: n.trim() });
        } },
      ];
      if (p.smart) listMenu.push({ label: t("editRules"), run: () => open({ kind: "smartEdit", id: p.id, name: p.name, rules: JSON.parse(JSON.stringify(p.rules || {})) }) });
      listMenu.push({ label: t("delete"), danger: true, run: async () => {
        if (await ask(t("deleteConfirm", { name: p.name }), { okLabel: t("delete"), danger: true })) {
          send({ type: "playlist_delete", id: p.id });
          back();
        }
      } });
      actions.appendChild(moreButton(e => menu(p.name, listMenu, e)));
      body.appendChild(actions);
      const list = tracksList(true);
      p.items.forEach((tr, i) => {
        const extra = p.smart ? [] : [
          { label: t("moveUp"), run: () => i > 0 && send({ type: "playlist_move", id: p.id, from: i, to: i - 1 }) },
          { label: t("moveDown"), run: () => i + 1 < p.items.length && send({ type: "playlist_move", id: p.id, from: i, to: i + 1 }) },
          { label: t("removeFromList"), danger: true, run: () => send({ type: "playlist_remove", id: p.id, indices: [i] }) },
        ];
        list.ul.appendChild(trackRow(tr, {
          number: desktop ? i + 1 : undefined, showAlbum: true,
          play: () => {
            if (tr.available === false) return;
            playSource(src, tr.id);
          },
          menu: ev => sourceMenu(tr.title, tr.id && tr.id.startsWith("p_")
            ? { source: "phone_track", id: tr.id, title: tr.title, artist: tr.artist, album: tr.album, duration: tr.duration }
            : { source: "tracks", ids: [tr.id] }, extra, ev),
        }));
      });
      list.nodes.forEach(n => body.appendChild(n));
    }

    function renderQueue() {
      body.replaceChildren();
      if (!queue) { body.appendChild(el("div", "hint", t("loading"))); return; }
      const actions = el("div", "lib-actions wrap");
      const rep = queue.repeat || "off";
      actions.appendChild(btn("🔁 " + t(rep === "all" ? "repeatAll" : rep === "one" ? "repeatOne" : "repeatOff"),
        "lib-btn" + (rep !== "off" ? " on" : ""),
        () => send({ type: "queue_repeat", mode: rep === "off" ? "all" : rep === "all" ? "one" : "off" })));
      actions.appendChild(btn("🔀 " + t(queue.shuffle ? "shuffleOn" : "shuffleOff"), "lib-btn" + (queue.shuffle ? " on" : ""),
        () => send({ type: "queue_shuffle", on: !queue.shuffle })));
      if (queue.items.length) {
        actions.appendChild(btn(t("saveAsList"), "lib-btn", async () => {
          const n = await ask(t("listName"), { input: true });
          if (n && n.trim()) { pendingOpenList = true; send({ type: "playlist_create", name: n.trim(), source: "queue" }); }
        }));
        actions.appendChild(btn(t("clear"), "lib-btn danger", () => send({ type: "queue_clear" })));
      }
      body.appendChild(actions);
      if (!queue.items.length) { body.appendChild(el("div", "hint", t("queueEmpty"))); return; }
      const list = tracksList(true);
      queue.items.forEach((tr, i) => {
        list.ul.appendChild(trackRow(tr, {
          number: i + 1, current: i === queue.index, showAlbum: true,
          play: () => { send({ type: "queue_jump", index: i }); opts.onPlay && opts.onPlay(); },
          menu: ev => menu(tr.title, [
            { label: t("moveUp"), run: () => i > 0 && send({ type: "queue_move", from: i, to: i - 1 }) },
            { label: t("moveDown"), run: () => i + 1 < queue.items.length && send({ type: "queue_move", from: i, to: i + 1 }) },
            { label: t("addToList"), run: () => pickList({ source: "tracks", ids: [tr.id] }) },
            { label: t("remove"), danger: true, run: () => send({ type: "queue_remove", index: i }) },
          ], ev),
        }));
      });
      list.nodes.forEach(n => body.appendChild(n));
      if (body.scrollTop === 0) {
        const cur = list.ul.querySelector(".current");
        if (cur && cur.scrollIntoView) cur.scrollIntoView({ block: "nearest" });
      }
    }

    function renderSmartEditor() {
      body.replaceChildren();
      const v = view;
      const r = v.rules;
      r.conditions = Array.isArray(r.conditions) ? r.conditions : [];
      body.appendChild(header("✨ " + t("smartEditor"), null, true));
      const form = el("div", "lib-form");
      const nameIn = el("input");
      nameIn.value = v.name || "";
      nameIn.placeholder = t("name");
      nameIn.oninput = () => { v.name = nameIn.value; };
      form.appendChild(labeled(t("name"), nameIn));
      const matchSel = select([["all", t("matchAll")], ["any", t("matchAny")]], r.match || "all", x => { r.match = x; });
      form.appendChild(labeled(t("match"), matchSel));
      const conds = el("div", "lib-conds");
      const drawConds = () => {
        conds.replaceChildren();
        r.conditions.forEach((c, i) => {
          const row = el("div", "lib-cond");
          const kind = FIELDS[c.field] || "text";
          const fieldSel = select(Object.keys(FIELDS).map(f => [f, t("f_" + f)]), c.field, x => {
            c.field = x;
            const k = FIELDS[x];
            const ops = OPS[k].concat(x === "lastPlayed" ? ["never"] : []);
            if (!ops.includes(c.op)) c.op = ops[0];
            if (k === "text") c.value = typeof c.value === "string" ? c.value : "";
            else c.value = typeof c.value === "number" ? c.value : (k === "date" ? 30 : 0);
            drawConds();
          });
          const ops = OPS[kind].concat(c.field === "lastPlayed" ? ["never"] : []);
          const opSel = select(ops.map(o => [o, t("o_" + o)]), c.op, x => { c.op = x; drawConds(); });
          row.append(fieldSel, opSel);
          if (c.op !== "never") {
            const val = el("input");
            val.type = kind === "text" ? "text" : "number";
            val.inputMode = kind === "text" ? "text" : "decimal";
            val.value = c.value === undefined ? "" : c.value;
            val.oninput = () => { c.value = kind === "text" ? val.value : Number(val.value); };
            row.appendChild(val);
            if (kind === "date") row.appendChild(el("span", "detail", t("days")));
          }
          row.appendChild(btn("×", "icon", () => { r.conditions.splice(i, 1); drawConds(); }));
          conds.appendChild(row);
        });
      };
      drawConds();
      form.appendChild(conds);
      form.appendChild(btn(t("addCondition"), "lib-btn", () => {
        r.conditions.push({ field: "artist", op: "contains", value: "" });
        drawConds();
      }));
      const sortSel = select(SORTS.map(s => [s, t("s_" + s)]), r.sort || "added", x => { r.sort = x; });
      const desc = el("input");
      desc.type = "checkbox";
      desc.checked = r.desc !== false;
      desc.onchange = () => { r.desc = desc.checked; };
      const sortRow = el("div", "lib-cond");
      const descLbl = el("label", "lib-check");
      descLbl.append(desc, el("span", null, t("desc")));
      sortRow.append(sortSel, descLbl);
      form.appendChild(labeled(t("sort"), sortRow));
      const lim = el("input");
      lim.type = "number";
      lim.min = "0";
      lim.value = r.limit || 0;
      lim.oninput = () => { r.limit = Math.max(0, Math.floor(Number(lim.value) || 0)); };
      form.appendChild(labeled(t("limit"), lim));
      form.appendChild(btn(t("save"), "wide", () => {
        const name = (v.name || "").trim();
        if (!name) { toast(t("error", { e: t("e_name") })); return; }
        const rules = { match: r.match || "all", sort: r.sort || "added", desc: r.desc !== false, limit: r.limit || 0,
                        conditions: r.conditions.map(c => (c.op === "never" ? { field: c.field, op: c.op } : { field: c.field, op: c.op, value: c.value })) };
        pendingOpenList = true;
        if (v.id) send({ type: "playlist_update", id: v.id, name, rules });
        else send({ type: "playlist_create", name, rules });
      }));
      body.appendChild(form);
    }
    function labeled(text, input) {
      const l = el("label");
      l.appendChild(el("span", null, text));
      l.appendChild(input);
      return l;
    }
    function select(options, value, onChange) {
      const s = el("select");
      options.forEach(([v, label]) => {
        const o = el("option", null, label);
        o.value = v;
        s.appendChild(o);
      });
      s.value = value;
      s.onchange = () => onChange(s.value);
      return s;
    }

    // ---- a motor üzenetei
    function onMessage(m) {
      switch (m.type) {
        case "phone_track_have":
          toast(t("phoneTrackLocalMatch", { title: m.title || "Szám" }));
          if (phoneStreaming && phoneStreaming.tr && phoneStreaming.tr.id === m.id) {
            const act = phoneStreaming.action;
            const nxt = phoneStreaming.next;
            phoneStreaming = null;
            if (act === "play") {
              send({ type: "library_play", id: m.local_id });
            } else if (act === "queue") {
              send({ type: "queue_add", source: "tracks", ids: [m.local_id], next: !!nxt });
            }
          }
          return true;
        case "phone_track_ready":
          if (phoneStreaming && phoneStreaming.tr && phoneStreaming.tr.id === m.id) {
            const tr = phoneStreaming.tr;
            const act = phoneStreaming.action;
            const nxt = phoneStreaming.next;
            const saveToPc = phoneStreaming.saveToPc;
            const ext = phoneStreaming.ext;
            phoneStreaming = null;

            if (saveToPc) {
              send({
                type: "phone_track_import",
                id: tr.id,
                ext,
                title: tr.title || tr.file || "Phone Track",
                artist: tr.artist || "",
                album: tr.album || ""
              });
            }

            if (act === "play") {
              send({
                type: "library_play",
                id: tr.id,
                title: tr.title || tr.file || "Phone Track",
                artist: tr.artist || "",
                album: tr.album || "",
                duration: tr.duration || 0
              });
            } else if (act === "queue") {
              send({
                type: "queue_add",
                source: "phone_track",
                id: tr.id,
                title: tr.title || tr.file || "Phone Track",
                artist: tr.artist || "",
                album: tr.album || "",
                duration: tr.duration || 0,
                next: !!nxt
              });
            }
          }
          return true;
        case "phone_track_send":
          if (phoneStreaming && phoneStreaming.tr && phoneStreaming.tr.id === m.id) {
            const startStream = () => {
              if (!phoneStreaming) return;
              phoneStreaming.offset = m.offset || 0;
              phoneStreaming.chunkSize = m.chunk_size || 65536;
              toast(t("sendingTrack"));
              streamNextChunk();
            };

            if (phoneStreaming.forceCopy) {
              phoneStreaming.saveToPc = true;
              startStream();
            } else {
              const pref = localStorage.getItem("aq_phone_copy_pref") || "ask";
              if (pref === "always") {
                phoneStreaming.saveToPc = true;
                startStream();
              } else if (pref === "never") {
                phoneStreaming.saveToPc = false;
                startStream();
              } else {
                promptCopyChoice(phoneStreaming.tr, (save) => {
                  if (!phoneStreaming) return;
                  phoneStreaming.saveToPc = save;
                  startStream();
                });
              }
            }
          }
          return true;
        case "phone_track_imported":
          toast(t("copiedToPc"));
          return true;
        case "phone_track_demand":
          if (phoneIndex && phoneIndex.tracks && !phoneStreaming) {
            const tr = phoneIndex.tracks.find(x => x.id === m.id);
            if (tr) {
              playPhoneTrack(tr, { action: "none", forceCopy: false });
            }
          }
          return true;
        case "phone_track_error":
          toast(t("error", { e: m.error || "Hiba" }));
          phoneStreaming = null;
          return true;
        case "library":
          info = m;
          renderStats();
          if (view.kind === "albums") {
            if (!m.scanning && albumsTotal >= 0 && albumsTotal !== m.albums) { albums = []; albumsTotal = -1; moreAlbums(); }
            if (!root.hidden) render();
          }
          return true;
        case "library_albums":
          albumsLoading = false;
          if (m.offset === albums.length) albums = albums.concat(m.items || []);
          albumsTotal = m.total;
          if (view.kind === "albums") render();
          return true;
        case "library_album":
          if (view.kind === "album" && view.id === m.id) renderAlbum(m);
          return true;
        case "library_folder":
          if (view.kind === "folder" && (view.id || "") === (m.id || "")) renderFolder(m);
          return true;
        case "library_error":
          if (["album", "folder"].includes(view.kind)) { body.replaceChildren(header("", null, true), el("div", "hint", t("error", { e: errText(m.error) }))); }
          return true;
        case "library_art":
          gotArt(m);
          return true;
        case "queue":
          queue = m;
          queueRev = m.rev;
          queueIndex = m.index;
          if (view.kind === "queue") renderQueue();
          return true;
        case "queue_result":
          if (m.ok === false) toast(t("error", { e: errText(m.error) }));
          return true;
        case "playlists":
          lists = m;
          listsRev = m.rev;
          if (pickFor) { showListPicker(); return true; }
          if (view.kind === "lists") renderLists();
          return true;
        case "playlist":
          if (pendingOpenList) {
            // létrehozás / mentés után a lista nyílik meg (a szerkesztő helyére), visszalépve a listák jönnek
            pendingOpenList = false;
            toast(t("saved", { name: m.name }));
            view = { kind: "list", id: m.id };
            history.length = 0;
            history.push({ kind: "lists" });
            render();
          }
          if (view.kind === "list" && view.id === m.id) renderList(m);
          return true;
        case "playlist_result":
          pendingOpenList = false;
          if (m.ok === false) toast(t("error", { e: errText(m.error) }));
          return true;
      }
      return false;
    }

    /** Az állapotüzenet: ha a sor vagy a listák változtak, a nyitott nézet frissül. */
    function onStatus(st) {
      const q = st.queue;
      if (q && (q.rev !== queueRev || q.index !== queueIndex)) {
        queueRev = q.rev;
        queueIndex = q.index;
        if (view.kind === "queue" && !root.hidden) send({ type: "queue_get" });
      } else if (!q && queue && queue.items.length) {
        queue = { rev: -1, index: -1, items: [], repeat: "off", shuffle: false };
        if (view.kind === "queue") renderQueue();
      }
      const p = st.playlists;
      if (p && (p.rev !== listsRev || p.stats !== statsRev)) {
        listsRev = p.rev;
        statsRev = p.stats;
        if (root.hidden) return;
        if (view.kind === "lists") send({ type: "playlist_list" });
        else if (view.kind === "list") send({ type: "playlist_get", id: view.id });
      }
    }

    setStaticTexts();

    return {
      show() {
        render();
        if (source === "pc") fetchFor(view);
      },
      onMessage,
      onStatus,
      setLang(l) {
        S = STR[l === "hu" ? "hu" : "en"];
        setStaticTexts();
        render();
      },
      reset() { art.clear(); artBusy.clear(); artWait.length = 0; albums = []; albumsTotal = -1; albumsLoading = false; info = null; lastRendered = null; },
    };
  };
})();
