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
      play: "Lejátszás", pause: "Szünet", stop: "Leállítás", prev: "Előző", next: "Következő",
      shuffle: "Keverés", repeatOff: "Ismétlés: ki", repeatAll: "Ismétlés: az egész sor", repeatOne: "Ismétlés: ez a szám",
      queue: "Lejátszási sor", queueEmpty: "A sor üres.", nothing: "Nincs lejátszás",
      resume: "Folytatás a sorból ({n} szám)", from: "{app}", seek: "Pozíció", close: "Bezárás",
      loading: "Betöltés…", nowItem: "Most szól", volume: "Hangerő", mute: "Némítás", unmute: "Némítás ki"
    },
    en: {
      play: "Play", pause: "Pause", stop: "Stop", prev: "Previous", next: "Next",
      shuffle: "Shuffle", repeatOff: "Repeat: off", repeatAll: "Repeat: whole queue", repeatOne: "Repeat: this track",
      queue: "Play queue", queueEmpty: "The queue is empty.", nothing: "Nothing playing",
      resume: "Resume from the queue ({n} tracks)", from: "{app}", seek: "Position", close: "Close",
      loading: "Loading…", nowItem: "Now playing", volume: "Volume", mute: "Mute", unmute: "Unmute"
    },
    es: {
      play: "Reproducir",
      pause: "Pausar",
      stop: "Detener", prev: "Anterior",
      next: "Siguiente",
      shuffle: "Aleatorio",
      repeatOff: "Repetir: no",
      repeatAll: "Repetir: toda la cola",
      repeatOne: "Repetir: esta pista",
      queue: "Cola de reproducción",
      queueEmpty: "La cola está vacía.",
      nothing: "Nada en reproducción",
      resume: "Reanudar desde la cola ({n} pistas)",
      from: "{app}",
      seek: "Posición",
      close: "Cerrar",
      loading: "Cargando…",
      nowItem: "En reproducción",
      volume: "Volumen",
      mute: "Silenciar",
      unmute: "Reactivar sonido"
    },
    fr: {
      play: "Lecture",
      pause: "Pause",
      stop: "Arrêter", prev: "Précédent",
      next: "Suivant",
      shuffle: "Aléatoire",
      repeatOff: "Répéter : off",
      repeatAll: "Répéter : toute la file",
      repeatOne: "Répéter : cette piste",
      queue: "File d'attente",
      queueEmpty: "La file d'attente est vide.",
      nothing: "Rien en lecture",
      resume: "Reprendre la file ({n} pistes)",
      from: "{app}",
      seek: "Position",
      close: "Fermer",
      loading: "Chargement…",
      nowItem: "En lecture",
      volume: "Volume",
      mute: "Muet",
      unmute: "Rétablir le son"
    },
    zh: {
      play: "播放",
      pause: "暂停",
      stop: "停止", prev: "上一首",
      next: "下一首",
      shuffle: "随机播放",
      repeatOff: "循环：关",
      repeatAll: "循环：全部队列",
      repeatOne: "单曲循环：当前曲目",
      queue: "播放队列",
      queueEmpty: "播放队列为空。",
      nothing: "暂无播放内容",
      resume: "从播放队列继续播放（{n} 首曲目）",
      from: "{app}",
      seek: "播放进度",
      close: "关闭",
      loading: "正在加载…",
      nowItem: "正在播放",
      volume: "音量",
      mute: "静音",
      unmute: "取消静音"
    },
    de: {
      play: "Wiedergabe",
      pause: "Pause",
      stop: "Stopp", prev: "Vorheriger",
      next: "Nächster",
      shuffle: "Zufallswiedergabe",
      repeatOff: "Wiederholung: aus",
      repeatAll: "Wiederholung: ganze Warteschlange",
      repeatOne: "Wiederholung: aktueller Titel",
      queue: "Wiedergabeschlange",
      queueEmpty: "Die Warteschlange ist leer.",
      nothing: "Keine Wiedergabe",
      resume: "Aus Warteschlange fortsetzen ({n} Titel)",
      from: "{app}",
      seek: "Position",
      close: "Schließen",
      loading: "Wird geladen…",
      nowItem: "Aktueller Titel",
      volume: "Lautstärke",
      mute: "Stummschalten",
      unmute: "Stummschaltung aufheben"
    },
    it: {
      play: "Riproduci",
      pause: "Pausa",
      stop: "Stop", prev: "Precedente",
      next: "Successivo",
      shuffle: "Casuale",
      repeatOff: "Ripeti: disattivato",
      repeatAll: "Ripeti: intera coda",
      repeatOne: "Ripeti: questo brano",
      queue: "Coda di riproduzione",
      queueEmpty: "La coda di riproduzione è vuota.",
      nothing: "Nessun brano in riproduzione",
      resume: "Riprendi dalla coda ({n} brani)",
      from: "{app}",
      seek: "Posizione",
      close: "Chiudi",
      loading: "Caricamento in corso…",
      nowItem: "In riproduzione",
      volume: "Volume",
      mute: "Disattiva audio",
      unmute: "Riattiva audio"
    },
    nl: {
      play: "Afspelen",
      pause: "Pauze",
      stop: "Stop", prev: "Vorige",
      next: "Volgende",
      shuffle: "Willekeurig",
      repeatOff: "Herhalen: uit",
      repeatAll: "Herhalen: hele wachtrij",
      repeatOne: "Herhalen: dit nummer",
      queue: "Afspeelwachtrij",
      queueEmpty: "De wachtrij is leeg.",
      nothing: "Geen muziek die afspeelt",
      resume: "Hervatten uit de wachtrij ({n} nummers)",
      from: "{app}",
      seek: "Positie",
      close: "Sluiten",
      loading: "Bezig met laden…",
      nowItem: "Nu aan het spelen",
      volume: "Volume",
      mute: "Dempen",
      unmute: "Dempen opheffen"
    },
    pl: {
      play: "Odtwarzaj",
      pause: "Pauza",
      stop: "Zatrzymaj", prev: "Poprzedni",
      next: "Następny",
      shuffle: "Losowo",
      repeatOff: "Powtarzanie: wył.",
      repeatAll: "Powtarzanie: cała kolejka",
      repeatOne: "Powtarzanie: ten utwór",
      queue: "Kolejka odtwarzania",
      queueEmpty: "Kolejka jest pusta.",
      nothing: "Nic nie jest odtwarzane",
      resume: "Wznów z kolejki ({n} utworów)",
      from: "{app}",
      seek: "Pozycja",
      close: "Zamknij",
      loading: "Ładowanie…",
      nowItem: "Teraz odtwarzane",
      volume: "Głośność",
      mute: "Wycisz",
      unmute: "Wyłącz wyciszenie"
    },
    ru: {
      play: "Воспроизведение",
      pause: "Пауза",
      stop: "Стоп", prev: "Предыдущий",
      next: "Следующий",
      shuffle: "В случайном порядке",
      repeatOff: "Повтор: выкл",
      repeatAll: "Повтор: вся очередь",
      repeatOne: "Повтор: один трек",
      queue: "Очередь воспроизведения",
      queueEmpty: "Очередь пуста.",
      nothing: "Ничего не воспроизводится",
      resume: "Продолжить из очереди ({n} треков)",
      from: "{app}",
      seek: "Позиция",
      close: "Закрыть",
      loading: "Загрузка…",
      nowItem: "Сейчас играет",
      volume: "Громкость",
      mute: "Заглушить",
      unmute: "Включить звук"
    },
    pt: {
      play: "Reproduzir",
      pause: "Pausar",
      stop: "Parar", prev: "Anterior",
      next: "Próximo",
      shuffle: "Aleatório",
      repeatOff: "Repetir: desligado",
      repeatAll: "Repetir: toda a fila",
      repeatOne: "Repetir: esta faixa",
      queue: "Fila de reprodução",
      queueEmpty: "A fila está vazia.",
      nothing: "Nada tocando",
      resume: "Retomar da fila ({n} faixas)",
      from: "{app}",
      seek: "Posição",
      close: "Fechar",
      loading: "Carregando…",
      nowItem: "Tocando agora",
      volume: "Volume",
      mute: "Silenciar",
      unmute: "Reativar som"
    },
    ro: {
      play: "Redare",
      pause: "Pauză",
      stop: "Oprește", prev: "Anterior",
      next: "Următor",
      shuffle: "Aleatoriu",
      repeatOff: "Repetare: oprită",
      repeatAll: "Repetare: toată coada",
      repeatOne: "Repetare: această piesă",
      queue: "Coadă de redare",
      queueEmpty: "Coada este goală.",
      nothing: "Nimic în redare",
      resume: "Reluare din coadă ({n} piese)",
      from: "{app}",
      seek: "Poziție",
      close: "Închide",
      loading: "Se încarcă…",
      nowItem: "În redare acum",
      volume: "Volum",
      mute: "Dezactivează sunetul",
      unmute: "Reactivează sunetul"
    },
    el: {
      play: "Αναπαραγωγή",
      pause: "Παύση",
      stop: "Διακοπή", prev: "Προηγούμενο",
      next: "Επόμενο",
      shuffle: "Τυχαία σειρά",
      repeatOff: "Επανάληψη: ανενεργή",
      repeatAll: "Επανάληψη: όλη η ουρά",
      repeatOne: "Επανάληψη: αυτό το κομμάτι",
      queue: "Ουρά αναπαραγωγής",
      queueEmpty: "Η ουρά είναι κενή.",
      nothing: "Δεν αναπαράγεται τίποτα",
      resume: "Συνέχιση από την ουρά ({n} κομμάτια)",
      from: "{app}",
      seek: "Θέση",
      close: "Κλείσιμο",
      loading: "Φόρτωση…",
      nowItem: "Τώρα αναπαράγεται",
      volume: "Ένταση",
      mute: "Σίγαση",
      unmute: "Κατάργηση σίγασης"
    },
    tr: {
      play: "Oynat",
      pause: "Duraklat",
      stop: "Durdur", prev: "Önceki",
      next: "Sonraki",
      shuffle: "Karışık",
      repeatOff: "Tekrar: kapalı",
      repeatAll: "Tekrar: tüm sıra",
      repeatOne: "Tekrar: bu parça",
      queue: "Oynatma sırası",
      queueEmpty: "Sıra boş.",
      nothing: "Hiçbir şey çalmıyor",
      resume: "Sıradan devam et ({n} parça)",
      from: "{app}",
      seek: "Konum",
      close: "Kapat",
      loading: "Yükleniyor…",
      nowItem: "Şimdi çalıyor",
      volume: "Ses düzeyi",
      mute: "Sessize al",
      unmute: "Sesi aç"
    },
    fi: {
      play: "Toista",
      pause: "Tauko",
      stop: "Pysäytä", prev: "Edellinen",
      next: "Seuraava",
      shuffle: "Sekoita",
      repeatOff: "Toisto: pois",
      repeatAll: "Toisto: koko jono",
      repeatOne: "Toisto: tämä kappale",
      queue: "Toistojono",
      queueEmpty: "Jono on tyhjä.",
      nothing: "Mitään ei toisteta",
      resume: "Jatka jonosta ({n} kappaletta)",
      from: "{app}",
      seek: "Kohta",
      close: "Sulje",
      loading: "Ladataan…",
      nowItem: "Nyt toistetaan",
      volume: "Äänenvoimakkuus",
      mute: "Mykistä",
      unmute: "Poista mykistys"
    },
    da: {
      play: "Afspil",
      pause: "Pause",
      stop: "Stop", prev: "Forrige",
      next: "Næste",
      shuffle: "Bland",
      repeatOff: "Gentag: fra",
      repeatAll: "Gentag: hele køen",
      repeatOne: "Gentag: dette spor",
      queue: "Afspilningskø",
      queueEmpty: "Køen er tom.",
      nothing: "Intet afspilles",
      resume: "Genoptag fra kø ({n} spor)",
      from: "{app}",
      seek: "Position",
      close: "Luk",
      loading: "Indlæser…",
      nowItem: "Afspilles nu",
      volume: "Lydstyrke",
      mute: "Slå lyd fra",
      unmute: "Slå lyd til"
    },
    nb: {
      play: "Spill av",
      pause: "Pause",
      stop: "Stopp", prev: "Forrige",
      next: "Neste",
      shuffle: "Bland",
      repeatOff: "Gjenta: av",
      repeatAll: "Gjenta: hele køen",
      repeatOne: "Gjenta: dette sporet",
      queue: "Avspillingskø",
      queueEmpty: "Køen er tom.",
      nothing: "Ingenting spilles",
      resume: "Gjenoppta fra kø ({n} spor)",
      from: "{app}",
      seek: "Posisjon",
      close: "Lukk",
      loading: "Laster inn…",
      nowItem: "Spilles nå",
      volume: "Volum",
      mute: "Demp",
      unmute: "Opphev demping"
    },
    sv: {
      play: "Spela",
      pause: "Pausa",
      stop: "Stoppa", prev: "Föregående",
      next: "Nästa",
      shuffle: "Blanda",
      repeatOff: "Repetera: av",
      repeatAll: "Repetera: hela kön",
      repeatOne: "Repetera: detta spår",
      queue: "Uppspelningskö",
      queueEmpty: "Kön är tom.",
      nothing: "Inget spelas",
      resume: "Återuppta från kö ({n} spår)",
      from: "{app}",
      seek: "Position",
      close: "Stäng",
      loading: "Läser in…",
      nowItem: "Spelas nu",
      volume: "Volym",
      mute: "Ljud av",
      unmute: "Ljud på"
    },
    he: {
      play: "נגן",
      pause: "השהה",
      stop: "עצור", prev: "הקודם",
      next: "הבא",
      shuffle: "ערבב",
      repeatOff: "חזרה: כבויה",
      repeatAll: "חזרה: כל התור",
      repeatOne: "חזרה: רצועה זו",
      queue: "תור השמעה",
      queueEmpty: "התור ריק.",
      nothing: "שום דבר אינו מושמע",
      resume: "המשך מהתור ({n} רצועות)",
      from: "{app}",
      seek: "מיקום",
      close: "סגור",
      loading: "טוען…",
      nowItem: "מתנגן כעת",
      volume: "עוצמת שמע",
      mute: "השתק",
      unmute: "בטל השתקה"
    },
    ja: {
      play: "再生",
      pause: "一時停止",
      stop: "停止", prev: "前の曲",
      next: "次の曲",
      shuffle: "シャッフル",
      repeatOff: "リピート: オフ",
      repeatAll: "リピート: すべて",
      repeatOne: "リピート: この曲",
      queue: "再生キュー",
      queueEmpty: "キューは空です。",
      nothing: "再生停止中",
      resume: "キューから再開 ({n} 曲)",
      from: "{app}",
      seek: "シーク",
      close: "閉じる",
      loading: "読み込み中…",
      nowItem: "再生中",
      volume: "音量",
      mute: "ミュート",
      unmute: "ミュート解除"
    },
    ko: {
      play: "재생",
      pause: "일시 정지",
      stop: "정지", prev: "이전 곡",
      next: "다음 곡",
      shuffle: "셔플",
      repeatOff: "반복: 끔",
      repeatAll: "반복: 전체 대기열",
      repeatOne: "반복: 현재 곡",
      queue: "재생 대기열",
      queueEmpty: "대기열이 비어 있습니다.",
      nothing: "재생 중인 곡 없음",
      resume: "대기열에서 이어 재생 ({n}곡)",
      from: "{app}",
      seek: "탐색",
      close: "닫기",
      loading: "로드 중…",
      nowItem: "현재 재생 중",
      volume: "볼륨",
      mute: "음소거",
      unmute: "음소거 해제"
    },
    cs: {
      play: "Přehrát",
      pause: "Pozastavit",
      stop: "Zastavit", prev: "Předchozí",
      next: "Další",
      shuffle: "Náhodně",
      repeatOff: "Opakování: vypnuto",
      repeatAll: "Opakování: celá fronta",
      repeatOne: "Opakování: tato skladba",
      queue: "Fronta přehrávání",
      queueEmpty: "Fronta je prázdná.",
      nothing: "Nic se nepřehrává",
      resume: "Obnovit z fronty ({n} skladeb)",
      from: "{app}",
      seek: "Pozice",
      close: "Zavřít",
      loading: "Načítání…",
      nowItem: "Právě hraje",
      volume: "Hlasitost",
      mute: "Ztlumit",
      unmute: "Zrušit ztlumení"
    },
    uk: {
      play: "Відтворити",
      pause: "Пауза",
      stop: "Стоп", prev: "Попередня",
      next: "Наступна",
      shuffle: "Випадково",
      repeatOff: "Повтор: вимкнено",
      repeatAll: "Повтор: вся черга",
      repeatOne: "Повтор: цей трек",
      queue: "Черга відтворення",
      queueEmpty: "Черга порожня.",
      nothing: "Нічого не грає",
      resume: "Відновити з черги ({n} треків)",
      from: "{app}",
      seek: "Позиція",
      close: "Закрити",
      loading: "Завантаження…",
      nowItem: "Зараз грає",
      volume: "Гучність",
      mute: "Вимкнути звук",
      unmute: "Увімкнути звук"
    },
  };

  const ICON = {
    play: '<path d="M8 5.5v13l11-6.5z"/>',
    pause: '<path d="M7 5h3.5v14H7zM13.5 5H17v14h-3.5z"/>',
    prev: '<path d="M6 5h2.2v14H6zM19 5.5v13L9.5 12z"/>',
    next: '<path d="M15.8 5H18v14h-2.2zM5 5.5v13L14.5 12z"/>',
    shuffle: '<path d="M4 7h3.5c1.6 0 2.6.7 3.5 2l3 4.5c.9 1.3 1.9 2 3.5 2H19M4 17h3.5c1.6 0 2.6-.7 3.5-2M14 9c.9-1.3 1.9-2 3.5-2H19M17 4.5 19.5 7 17 9.5M17 13l2.5 2.5L17 18" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/>',
    repeat: '<path d="M5 11V9.5A2.5 2.5 0 0 1 7.5 7H18M15.5 4.5 18 7l-2.5 2.5M19 13v1.5a2.5 2.5 0 0 1-2.5 2.5H6M8.5 19.5 6 17l2.5-2.5" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/>',
    queue: '<path d="M4 6.5h12M4 11h12M4 15.5h7M15 14v6l5-3z" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/>',
    vol: '<path d="M4 9.5h3.5L12 5.5v13l-4.5-4H4z" fill="currentColor"/><path d="M15.5 9a4.5 4.5 0 0 1 0 6M18 6.5a8 8 0 0 1 0 11" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"/>',
    volLow: '<path d="M4 9.5h3.5L12 5.5v13l-4.5-4H4z" fill="currentColor"/><path d="M15.5 9a4.5 4.5 0 0 1 0 6" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"/>',
    volMute: '<path d="M4 9.5h3.5L12 5.5v13l-4.5-4H4z" fill="currentColor"/><path d="M15.5 9.5l5 5M20.5 9.5l-5 5" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"/>',
    stop: '<rect x="6.5" y="6.5" width="11" height="11" rx="1.5" fill="currentColor"/>',
    note: '<path d="M10 17.5a2.5 2.5 0 1 1-2.5-2.5c.9 0 1.7.3 2.5.8V5l9-2v11.5a2.5 2.5 0 1 1-2.5-2.5c.9 0 1.7.3 2.5.8V7l-9 2" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linejoin="round"/>'
  };
  const svg = (name, size) =>
    `<svg viewBox="0 0 24 24" width="${size || 20}" height="${size || 20}" aria-hidden="true" fill="currentColor">${ICON[name]}</svg>`;

  // A forrás formátuma egy sorban: PCM-nél „24 bit / 96 kHz” (ismeretlen bitmélységnél csak a kHz), DSD-nél „DSD256”
  function formatLabel(f) {
    if (!f) return "";
    if (f.dsd > 0) {
      const base = f.dsd % 44100 === 0 ? 44100 : 48000;
      return "DSD" + Math.round(f.dsd / base);
    }
    if (!(f.rate > 0)) return "";
    const khz = new Intl.NumberFormat(document.documentElement.lang || undefined, { maximumFractionDigits: 1 }).format(f.rate / 1000);
    return (f.bits > 0 ? f.bits + " bit / " : "") + khz + " kHz";
  }

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
.aqnp-app { color: var(--aq-text-2, #93a1b0); font-size: var(--aq-fs-sub, 11px); margin-top: 2px; min-height: 1.3em; }
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
.aqnp-vol { display: flex; align-items: center; gap: 4px; }
.aqnp-volbar { width: 104px; accent-color: var(--aq-accent, #4fd1c5); cursor: pointer; height: 16px; }
  .aqnp-volbar::-webkit-slider-runnable-track { height: 4px; border-radius: 2px; background: linear-gradient(to right, var(--aq-accent, #4fd1c5) 0%, var(--aq-accent, #4fd1c5) var(--fill, 0%), var(--aq-line-2, #374251) var(--fill, 0%), var(--aq-line-2, #374251) 100%) !important; }
.aqnp-volpct { font-family: var(--aq-mono, ui-monospace, monospace); font-size: var(--aq-fs-sub, 11px); color: var(--aq-text-2, #93a1b0);
  min-width: 30px; text-align: right; font-variant-numeric: tabular-nums; }
.aqnp-vol.muted .aqnp-volbar { opacity: 0.45; }
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
    let lang = STR[opts.lang] ? opts.lang : "en";
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
          <button type="button" class="aqnp-btn" data-k="stop">${svg("stop", 18)}</button>
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
        <div class="aqnp-vol" hidden>
          <button type="button" class="aqnp-btn" data-k="mute">${svg("vol", 18)}</button>
          <input type="range" class="aqnp-volbar" min="0" max="100" step="1" value="0">
          <span class="aqnp-volpct"></span>
        </div>
        <button type="button" class="aqnp-btn" data-k="queue">${svg("queue", 18)}</button>
      </div>`;
    const $ = sel => root.querySelector(sel);
    const ui = {
      artImg: $("img.aqnp-art"), artEmpty: $(".aqnp-art-empty"),
      title: $(".aqnp-title"), sub: $(".aqnp-sub"), app: $(".aqnp-app"),
      shuffle: $('[data-k="shuffle"]'), prev: $('[data-k="prev"]'), play: $('[data-k="play"]'),
      stop: $('[data-k="stop"]'), next: $('[data-k="next"]'), repeat: $('[data-k="repeat"]'), queue: $('[data-k="queue"]'),
      ctrls: $(".aqnp-ctrls"), seekRow: $(".aqnp-seekrow"), seek: $(".aqnp-seek"),
      pos: $('[data-k="pos"]'), dur: $('[data-k="dur"]'), empty: $(".aqnp-empty"),
      vol: $(".aqnp-vol"), volBar: $(".aqnp-volbar"), volPct: $(".aqnp-volpct"), mute: $('[data-k="mute"]')
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
      label(ui.stop, t("stop"));
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
      ui.seekRow.hidden = !has || !!m.live;   // élő folyam (rádió): nincs hossz, nincs tekerés
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
        // Egy sor: más lejátszónál a forrás neve, a beépített lejátszónál a forrás formátuma (pl. 24 bit / 96 kHz, DSD256)
        ui.app.textContent = m.app && m.app !== "Aqulify" ? t("from", { app: m.app }) : formatLabel(m.fmt);
        if (m.art !== artShown) setArt(m.art || "");
      }

      const can = (has && m.can) || {};
      const playing = has && m.playing;
      const external = has && m.app !== "Aqulify";
      ui.play.innerHTML = svg(playing ? "pause" : "play", 22);
      label(ui.play, t(playing ? "pause" : "play"));
      ui.play.disabled = has ? (external ? !(can.play || can.pause) : !(playing ? can.pause : can.play)) : qCount === 0;
      ui.prev.disabled = !(has && can.previous);
      ui.next.disabled = !(has && can.next);
      ui.stop.disabled = !has;   // a beépített lejátszót leállítja (elengedi a kimenetet), más lejátszót szüneteltet
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

    // ---- hangerő: a kimeneti hangkártyáé (a motor a status.volume mezőben küldi; a csúszka a rendszer százaléka)
    let volume = null, volDragging = false, volSendTimer = 0, volLastSent = -1, volHoldUntil = 0;
    let playHoldUntil = 0, playHoldVal = false;
    function renderVolume() {
      const v = volume;
      ui.vol.hidden = !v;
      if (!v) return;
      const muted = !!v.muted;
      if (!volDragging && Date.now() > volHoldUntil) ui.volBar.value = String(Math.round((v.level || 0) * 100));
      const pct = +ui.volBar.value;
      ui.volBar.style.setProperty("--fill", pct + "%");
      ui.volPct.textContent = pct + "%";
      ui.vol.classList.toggle("muted", muted);
      ui.mute.innerHTML = svg(muted || pct === 0 ? "volMute" : pct < 50 ? "volLow" : "vol", 18);
      label(ui.mute, t(muted ? "unmute" : "mute"));
      ui.volBar.setAttribute("aria-label", t("volume"));
    }
    function sendVolume(force) {
      const lv = +ui.volBar.value / 100;
      if (!force && lv === volLastSent) return;
      volLastSent = lv;
      volHoldUntil = Date.now() + 1500;   // a következő állapotüzenetek még a régi értéket hozhatják
      const msg = { type: "set_volume", level: lv };
      if (volume && volume.muted && lv > 0) msg.muted = false;   // a csúszka mozgatása feloldja a némítást
      send(msg);
      if (volume) { volume.level = lv; if (msg.muted === false) volume.muted = false; }
    }
    ui.volBar.addEventListener("input", () => {
      volDragging = true;
      renderVolume();
      if (!volSendTimer) volSendTimer = setTimeout(() => { volSendTimer = 0; sendVolume(false); }, 60);
    });
    ui.volBar.addEventListener("change", () => { volDragging = false; sendVolume(true); renderVolume(); });
    ui.mute.onclick = () => {
      if (!volume) return;
      volume.muted = !volume.muted;
      volHoldUntil = Date.now() + 1500;
      send({ type: "set_volume", muted: volume.muted });
      renderVolume();
    };
    ui.vol.addEventListener("wheel", e => {
      if (!volume) return;
      e.preventDefault();
      ui.volBar.value = String(Math.max(0, Math.min(100, +ui.volBar.value + (e.deltaY < 0 ? 2 : -2))));
      renderVolume();
      sendVolume(false);
    }, { passive: false });

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
      const external = media.app !== "Aqulify";
      let action;
      if (external) {
        if (!(can.play || can.pause)) return;
        action = "toggle";   // külső lejátszónál (pl. MPRIS Tidal/Electron) a PlayPause nem függ a késő PlaybackStatus-tól
      } else {
        action = media.playing ? "pause" : "play";
        if (!can[action]) return;
      }
      send({ type: "media", action });
      media.position = curPos();
      mediaAt = Date.now();
      media.playing = !media.playing;   // azonnali visszajelzés; a 700 ms tartási ablak védi a régi status ellen
      playHoldVal = media.playing;
      playHoldUntil = Date.now() + 700;
      render();
    }
    function resume() {
      if (!queue || !(queue.count > 0)) return;
      send({ type: "queue_jump", index: queue.index >= 0 ? queue.index : 0 });
    }
    ui.play.onclick = togglePlay;
    ui.prev.onclick = () => send({ type: "media", action: "previous" });
    ui.next.onclick = () => send({ type: "media", action: "next" });
    ui.stop.onclick = () => send({ type: "media", action: "stop" });
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
        if (m && m.volume && typeof m.volume.level === "number") {
          if (Date.now() > volHoldUntil) volume = { level: m.volume.level, muted: !!m.volume.muted };
        } else {
          volume = null;
        }
        renderVolume();
        media = (m && m.media) || { available: false };
        if (media && media.available && Date.now() < playHoldUntil) {
          if (!!media.playing === playHoldVal) playHoldUntil = 0;
          else media.playing = playHoldVal;
        }
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
        lang = STR[l] ? l : "en";
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
      radio: "Rádió", radioAdd: "Új állomás", radioName: "Az állomás neve", radioUrl: "Az állomás címe (http:// vagy https://)",
      radioEmpty: "Még nincs állomás. Vedd fel a kedvenc rádióid címét (a rádió honlapján általában „Hallgatás” vagy „Stream” néven található; .pls és .m3u is jó).",
      radioNote: "A rádió a beépített lejátszón szól, az EQ-n és a teljes hangláncon át. Támogatott: MP3, AAC (HE-AAC is), Ogg Vorbis, Opus, FLAC; a szegmentált (HLS) adás még nem.",
      radioPlay: "Lejátszás", radioStop: "Leállítás", radioEditUrl: "Cím módosítása",
      rs_connecting: "Kapcsolódás…", rs_buffering: "Pufferelés…", rs_playing: "Szól", rs_reconnecting: "Újracsatlakozás…", rs_error: "Hiba: {e}",
      r_url: "Érvénytelen cím (http:// vagy https:// kell).", r_name: "Adj nevet az állomásnak.", r_full: "Az állomáslista megtelt.",
      r_blocked: "Ez a cím helyi vagy magánhálózati címre mutat, biztonsági okból nem nyitható meg.", r_dns: "A szerver neve nem található.",
      r_connect: "A szerver nem érhető el.", r_timeout: "A szerver nem válaszolt időben.", r_tls: "A biztonságos kapcsolat nem jött létre.",
      r_http: "A szerver hibát adott.",
      r_hls: "Ez szegmentált (HLS) adás, ezt még nem támogatjuk.", r_html: "Ez a cím egy weboldal, nem hangfolyam.", r_format: "Ismeretlen formátum.",
      r_decode: "Az adás nem dekódolható.", r_playlist: "A lejátszási listában nincs használható cím.", r_redirect: "Túl sok átirányítás.",
      r_nocurl: "A rádióhoz a curl program kell (telepítsd a csomagkezelővel).", r_formatChange: "Az adás formátuma megváltozott.",
      r_eof: "Az adás megszakadt.", r_empty: "Az adás üres.", r_unknown: "Ismeretlen állomás.", r_output: "A hangkimenet nem nyitható meg.",
      r_remote: "Az állomáslista csak a gépről (vagy adminisztrátor telefonról) szerkeszthető.",
      radioSearch: "Állomás keresése", radioSearchQ: "Az állomás neve (vagy egy része)", radioCatalogOn: "Állomáskereső bekapcsolása",
      radioCatalogOff: "Kereső kikapcsolása", radioCatalogOk: "Bekapcsolom",
      radioCatalogText: "Az állomáskereső a radio-browser.info közösségi állomáskatalógust használja. Kereséskor ez a gép közvetlenül a katalógus szervereihez fordul: ezek látják a gép IP-címét és a keresett szöveget. A szervereket a katalógus közössége üzemelteti; adatvédelmi szabályzatot és az adatbázis licencét nem találtuk. Az Aqulify csak a keresést küldi el (kattintásszámlálást, a lejátszott állomásokat és más adatot nem). A kereső bármikor kikapcsolható.",
      radioResults: "Találatok: „{q}”", radioSearching: "Keresés…", radioNoResults: "Nincs találat.", radioAddThis: "Felvétel a listába",
      radioAdded: "Felvéve: {name}", r_catalogOff: "Az állomáskereső ki van kapcsolva.", r_query: "Adj meg keresőszöveget.",
      r_json: "A katalógus válasza nem értelmezhető.", r_size: "A katalógus válasza túl nagy.",
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
      notSupportedHere: "Ez a funkció ezen az eszközön nem érhető el.",
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
      radio: "Radio", radioAdd: "New station", radioName: "Station name", radioUrl: "Station address (http:// or https://)",
      radioEmpty: "No stations yet. Add the address of your favourite stations (on the station's website it is usually labelled “Listen” or “Stream”; .pls and .m3u work too).",
      radioNote: "Radio plays through the built-in player, the EQ and the whole audio chain. Supported: MP3, AAC (incl. HE-AAC), Ogg Vorbis, Opus, FLAC; segmented (HLS) streams not yet.",
      radioPlay: "Play", radioStop: "Stop", radioEditUrl: "Change address",
      rs_connecting: "Connecting…", rs_buffering: "Buffering…", rs_playing: "Playing", rs_reconnecting: "Reconnecting…", rs_error: "Error: {e}",
      r_url: "Invalid address (http:// or https:// required).", r_name: "Give the station a name.", r_full: "The station list is full.",
      r_blocked: "This address points to a local or private network address and cannot be opened for security reasons.", r_dns: "Server name not found.",
      r_connect: "The server cannot be reached.", r_timeout: "The server did not answer in time.", r_tls: "The secure connection could not be established.",
      r_http: "The server returned an error.",
      r_hls: "This is a segmented (HLS) stream, which is not supported yet.", r_html: "This address is a web page, not an audio stream.", r_format: "Unknown format.",
      r_decode: "The stream cannot be decoded.", r_playlist: "The playlist has no usable address.", r_redirect: "Too many redirects.",
      r_nocurl: "Radio needs the curl program (install it with your package manager).", r_formatChange: "The stream format changed.",
      r_eof: "The stream was interrupted.", r_empty: "The stream is empty.", r_unknown: "Unknown station.", r_output: "The audio output cannot be opened.",
      r_remote: "The station list can only be edited on the computer (or from an admin phone).",
      radioSearch: "Find a station", radioSearchQ: "Station name (or part of it)", radioCatalogOn: "Turn on station search",
      radioCatalogOff: "Turn off search", radioCatalogOk: "Turn on",
      radioCatalogText: "Station search uses the radio-browser.info community station catalogue. When you search, this computer contacts the catalogue's servers directly: they see this computer's IP address and the search text. The servers are run by the catalogue's community; we could not find a privacy policy or a licence for the database. Aqulify only sends the search (no click counting, no played stations, no other data). You can turn search off at any time.",
      radioResults: "Results for “{q}”", radioSearching: "Searching…", radioNoResults: "No results.", radioAddThis: "Add to list",
      radioAdded: "Added: {name}", r_catalogOff: "Station search is turned off.", r_query: "Enter a search text.",
      r_json: "The catalogue's answer could not be read.", r_size: "The catalogue's answer is too large.",
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
      notSupportedHere: "This feature is not available on this device.",
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
    es: {
      albums: "Álbumes",
      folders: "Carpetas",
      lists: "Listas de reproducción",
      queue: "Cola",
      loading: "Cargando...",
      empty: "La biblioteca está vacía. Las carpetas de música se configuran en el PC (por defecto la carpeta Música).",
      scanning: "El PC está indexando la biblioteca ({n} archivos)...",
      noAlbums: "Sin álbumes.",
      various: "Varios artistas",
      tracks: "{n} pistas",
      play: "Reproducir",
      shuffle: "Aleatorio",
      playNext: "Reproducir siguiente",
      playLast: "Añadir a la cola",
      addToList: "Añadir a lista de reproducción...",
      cancel: "Cancelar",
      ok: "OK",
      folderEmpty: "Esta carpeta está vacía.",
      roots: "Carpetas de música",
      back: "Atrás",
      queueEmpty: "La cola está vacía. Inicia un álbum, carpeta o lista de reproducción.",
      clear: "Vaciar",
      saveAsList: "Guardar como lista de reproducción",
      repeatOff: "Repetir: no",
      repeatAll: "Repetir: cola",
      repeatOne: "Repetir: pista",
      shuffleOn: "Aleatorio: sí",
      shuffleOff: "Aleatorio: no",
      moveUp: "Subir",
      moveDown: "Bajar",
      remove: "Eliminar",
      removeFromList: "Eliminar de la lista de reproducción",
      newList: "Nueva lista de reproducción",
      newSmart: "Nueva lista de reproducción inteligente",
      listName: "Nombre de la lista de reproducción:",
      rename: "Renombrar",
      delete: "Eliminar",
      deleteConfirm: "¿Eliminar la lista de reproducción «{name}»?",
      editRules: "Reglas",
      unavailable: "no disponible",
      smartNote: "Lista de reproducción inteligente: el PC calcula su contenido a partir de las reglas.",
      noLists: "Aún no hay listas de reproducción.",
      chooseList: "¿Qué lista de reproducción?",
      added: "Añadido: {name}",
      queued: "Añadido a la cola",
      saved: "Guardado: {name}",
      error: "Error: {e}",
      smartEditor: "Lista de reproducción inteligente",
      name: "Nombre",
      match: "Condiciones",
      matchAll: "deben coincidir todas",
      matchAny: "puede coincidir cualquiera",
      addCondition: "+ Condición",
      sort: "Ordenar por",
      desc: "descendente",
      limit: "Como máximo (0: todas)",
      save: "Guardar",
      days: "días",
      more: "Más",
      colTitle: "Título",
      colArtist: "Artista",
      colAlbum: "Álbum",
      colDuration: "Duración",
      stats: "{tracks} pistas · {albums} álbumes",
      playAlbum: "Reproducir álbum",
      f_artist: "Artista",
      f_album: "Álbum",
      f_title: "Título",
      f_genre: "Género",
      f_year: "Año",
      f_duration: "Duración (s)",
      f_plays: "Reproducciones",
      f_added: "Añadido",
      f_lastPlayed: "Última reproducción",
      o_contains: "contiene",
      o_notContains: "no contiene",
      o_is: "es",
      o_isNot: "no es",
      o_startsWith: "empieza por",
      o_eq: "=",
      o_ne: "≠",
      o_gt: ">",
      o_ge: "≥",
      o_lt: "<",
      o_le: "≤",
      o_inLast: "en los últimos … días",
      o_notInLast: "no en los últimos … días",
      o_never: "nunca",
      s_added: "Añadido",
      s_lastPlayed: "Última reproducción",
      s_plays: "Reproducciones",
      s_year: "Año",
      s_artist: "Artista",
      s_album: "Álbum",
      s_title: "Título",
      s_duration: "Duración",
      s_random: "Aleatorio",
      e_empty: "nada que reproducir",
      e_name: "introduce un nombre",
      e_unknown: "no encontrado",
      e_smart: "no se pueden añadir pistas a una lista de reproducción inteligente",
      e_full: "llena",
      e_index: "posición no válida",
      e_rules: "regla no válida",
      srcPc: "💻 Biblioteca del PC",
      srcPhone: "📱 Música del teléfono",
      phoneEmpty: "Aún no se ha añadido ninguna carpeta de música del teléfono.",
      addFolder: "Añadir carpeta",
      rescan: "Reexplorar",
      scanningPhone: "Explorando biblioteca de música del teléfono...",
      phoneTracks: "{n} pistas encontradas en el teléfono",
      notSupportedHere: "Esta función no está disponible en este dispositivo.",
      removeFolderConfirm: "¿Eliminar la carpeta «{name}» de la biblioteca del teléfono?",
      phoneTrackLocalMatch: "Copia local encontrada en el PC: «{title}»",
      sendingTrack: "Transmitiendo pista al PC...",
      copyToPc: "Copiar al PC",
      copyConfirmPrompt: "«{title}» aún no está en la biblioteca del PC. ¿Qué deseas hacer?",
      optPlayOnly: "▶ Solo reproducir ahora",
      optCopyOnce: "📥 Copiar al PC",
      optCopyAlways: "💾 Copiar siempre al PC",
      optNeverAsk: "🚫 No preguntar nunca (solo reproducir)",
      copiedToPc: "¡Pista copiada a la biblioteca del PC!",
      copyPrefTitle: "Ajustes de copia al PC",
      prefAsk: "Preguntar para cada pista nueva",
      prefAlways: "Copiar siempre al PC",
      prefNever: "No copiar nunca (solo reproducir)",
      prefSaved: "Ajuste guardado",
      onPhone: "En el teléfono",
      matchedOnPc: "Copia local en el PC",
      phoneDisconnected: "Teléfono no conectado",
      radio: "Radio",
      radioAdd: "Nueva emisora",
      radioName: "Nombre de la emisora",
      radioUrl: "Dirección de la emisora (http:// o https://)",
      radioEmpty: "Aún no hay emisoras. Añade la dirección de tus emisoras favoritas (en el sitio web de la emisora suele llamarse «Escuchar» o «Stream»; también sirven .pls y .m3u).",
      radioNote: "La radio suena a través del reproductor integrado, el ecualizador y toda la cadena de audio. Compatible: MP3, AAC (incl. HE-AAC), Ogg Vorbis, Opus, FLAC; los flujos segmentados (HLS) todavía no.",
      radioPlay: "Reproducir",
      radioStop: "Detener",
      radioEditUrl: "Cambiar dirección",
      rs_connecting: "Conectando…",
      rs_buffering: "Almacenando en búfer…",
      rs_playing: "Reproduciendo",
      rs_reconnecting: "Reconectando…",
      rs_error: "Error: {e}",
      r_url: "Dirección no válida (se requiere http:// o https://).",
      r_name: "Ponle un nombre a la emisora.",
      r_full: "La lista de emisoras está llena.",
      r_blocked: "Esta dirección apunta a una red local o privada y no se puede abrir por motivos de seguridad.",
      r_dns: "No se encontró el nombre del servidor.",
      r_connect: "No se puede acceder al servidor.",
      r_timeout: "El servidor no respondió a tiempo.",
      r_tls: "No se pudo establecer la conexión segura.",
      r_http: "El servidor devolvió un error.",
      r_hls: "Es un flujo segmentado (HLS), que todavía no es compatible.",
      r_html: "Esta dirección es una página web, no un flujo de audio.",
      r_format: "Formato desconocido.",
      r_decode: "No se puede decodificar el flujo.",
      r_playlist: "La lista de reproducción no tiene ninguna dirección utilizable.",
      r_redirect: "Demasiadas redirecciones.",
      r_nocurl: "La radio necesita el programa curl (instálalo con tu gestor de paquetes).",
      r_formatChange: "El formato del flujo ha cambiado.",
      r_eof: "El flujo se interrumpió.",
      r_empty: "El flujo está vacío.",
      r_unknown: "Emisora desconocida.",
      r_output: "No se puede abrir la salida de audio.",
      r_remote: "La lista de emisoras solo se puede editar en el ordenador (o desde un teléfono administrador).",
      radioSearch: "Buscar una emisora",
      radioSearchQ: "Nombre de la emisora (o parte de él)",
      radioCatalogOn: "Activar la búsqueda de emisoras",
      radioCatalogOff: "Desactivar la búsqueda",
      radioCatalogOk: "Activar",
      radioCatalogText: "La búsqueda de emisoras usa el catálogo comunitario de emisoras radio-browser.info. Al buscar, este equipo contacta directamente con los servidores del catálogo: ven la dirección IP de este equipo y el texto buscado. Los servidores los gestiona la comunidad del catálogo; no hemos encontrado una política de privacidad ni una licencia de la base de datos. Aqulify solo envía la búsqueda (sin contar clics, sin emisoras reproducidas ni otros datos). Puedes desactivar la búsqueda en cualquier momento.",
      radioResults: "Resultados para «{q}»",
      radioSearching: "Buscando…",
      radioNoResults: "Sin resultados.",
      radioAddThis: "Añadir a la lista",
      radioAdded: "Añadida: {name}",
      r_catalogOff: "La búsqueda de emisoras está desactivada.",
      r_query: "Introduce un texto de búsqueda.",
      r_json: "No se pudo leer la respuesta del catálogo.",
      r_size: "La respuesta del catálogo es demasiado grande."
    },
    fr: {
      albums: "Albums",
      folders: "Dossiers",
      lists: "Playlists",
      queue: "File d'attente",
      loading: "Chargement...",
      empty: "La bibliothèque est vide. Les dossiers de musique se définissent sur le PC (par défaut le dossier Musique).",
      scanning: "Le PC indexe la bibliothèque ({n} fichiers)...",
      noAlbums: "Aucun album.",
      various: "Artistes divers",
      tracks: "{n} pistes",
      play: "Lecture",
      shuffle: "Aléatoire",
      playNext: "Lire ensuite",
      playLast: "Ajouter à la file d'attente",
      addToList: "Ajouter à la playlist...",
      cancel: "Annuler",
      ok: "OK",
      folderEmpty: "Ce dossier est vide.",
      roots: "Dossiers de musique",
      back: "Retour",
      queueEmpty: "La file d'attente est vide. Lancez un album, un dossier ou une playlist.",
      clear: "Effacer",
      saveAsList: "Enregistrer comme playlist",
      repeatOff: "Répéter : off",
      repeatAll: "Répéter : file",
      repeatOne: "Répéter : piste",
      shuffleOn: "Aléatoire : on",
      shuffleOff: "Aléatoire : off",
      moveUp: "Monter",
      moveDown: "Descendre",
      remove: "Supprimer",
      removeFromList: "Retirer de la playlist",
      newList: "Nouvelle playlist",
      newSmart: "Nouvelle playlist intelligente",
      listName: "Nom de la playlist :",
      rename: "Renommer",
      delete: "Supprimer",
      deleteConfirm: "Supprimer la playlist « {name} » ?",
      editRules: "Règles",
      unavailable: "non disponible",
      smartNote: "Playlist intelligente : le PC calcule son contenu à partir des règles.",
      noLists: "Aucune playlist pour l'instant.",
      chooseList: "Quelle playlist ?",
      added: "Ajouté : {name}",
      queued: "Ajouté à la file d'attente",
      saved: "Enregistré : {name}",
      error: "Erreur : {e}",
      smartEditor: "Playlist intelligente",
      name: "Nom",
      match: "Conditions",
      matchAll: "toutes doivent correspondre",
      matchAny: "au moins une doit correspondre",
      addCondition: "+ Condition",
      sort: "Trier par",
      desc: "décroissant",
      limit: "Au maximum (0 : tout)",
      save: "Enregistrer",
      days: "jours",
      more: "Plus",
      colTitle: "Titre",
      colArtist: "Artiste",
      colAlbum: "Album",
      colDuration: "Durée",
      stats: "{tracks} pistes · {albums} albums",
      playAlbum: "Lire l'album",
      f_artist: "Artiste",
      f_album: "Album",
      f_title: "Titre",
      f_genre: "Genre",
      f_year: "Année",
      f_duration: "Durée (s)",
      f_plays: "Lectures",
      f_added: "Ajouté",
      f_lastPlayed: "Dernière lecture",
      o_contains: "contient",
      o_notContains: "ne contient pas",
      o_is: "est",
      o_isNot: "n'est pas",
      o_startsWith: "commence par",
      o_eq: "=",
      o_ne: "≠",
      o_gt: ">",
      o_ge: "≥",
      o_lt: "<",
      o_le: "≤",
      o_inLast: "au cours des … derniers jours",
      o_notInLast: "pas au cours des … derniers jours",
      o_never: "jamais",
      s_added: "Ajouté",
      s_lastPlayed: "Dernière lecture",
      s_plays: "Lectures",
      s_year: "Année",
      s_artist: "Artiste",
      s_album: "Album",
      s_title: "Titre",
      s_duration: "Durée",
      s_random: "Aléatoire",
      e_empty: "rien à lire",
      e_name: "entrez un nom",
      e_unknown: "introuvable",
      e_smart: "impossible d'ajouter des pistes à une playlist intelligente",
      e_full: "pleine",
      e_index: "position non valide",
      e_rules: "règle non valide",
      srcPc: "💻 Bibliothèque du PC",
      srcPhone: "📱 Musique du téléphone",
      phoneEmpty: "Aucun dossier musical ajouté depuis le téléphone.",
      addFolder: "Ajouter un dossier",
      rescan: "Réanalyser",
      scanningPhone: "Analyse de la bibliothèque musicale du téléphone...",
      phoneTracks: "{n} pistes trouvées sur le téléphone",
      notSupportedHere: "Cette fonction n'est pas disponible sur cet appareil.",
      removeFolderConfirm: "Retirer le dossier « {name} » de la bibliothèque du téléphone ?",
      phoneTrackLocalMatch: "Copie locale trouvée sur le PC : « {title} »",
      sendingTrack: "Diffusion de la piste vers le PC...",
      copyToPc: "Copier sur le PC",
      copyConfirmPrompt: "« {title} » n'est pas encore dans la bibliothèque du PC. Que souhaitez-vous faire ?",
      optPlayOnly: "▶ Lire maintenant uniquement",
      optCopyOnce: "📥 Copier sur le PC",
      optCopyAlways: "💾 Toujours copier sur le PC",
      optNeverAsk: "🚫 Ne plus demander (lecture seule)",
      copiedToPc: "Piste copiée dans la bibliothèque du PC !",
      copyPrefTitle: "Paramètres de copie sur le PC",
      prefAsk: "Demander pour chaque nouvelle piste",
      prefAlways: "Toujours copier sur le PC",
      prefNever: "Ne jamais copier (lecture seule)",
      prefSaved: "Paramètre enregistré",
      onPhone: "Sur le téléphone",
      matchedOnPc: "Copie locale sur le PC",
      phoneDisconnected: "Téléphone non connecté",
      radio: "Radio",
      radioAdd: "Nouvelle station",
      radioName: "Nom de la station",
      radioUrl: "Adresse de la station (http:// ou https://)",
      radioEmpty: "Aucune station pour l'instant. Ajoutez l'adresse de vos stations préférées (sur le site de la station, elle s'appelle généralement « Écouter » ou « Stream » ; .pls et .m3u fonctionnent aussi).",
      radioNote: "La radio passe par le lecteur intégré, l'égaliseur et toute la chaîne audio. Pris en charge : MP3, AAC (y compris HE-AAC), Ogg Vorbis, Opus, FLAC ; flux segmentés (HLS) pas encore.",
      radioPlay: "Lire",
      radioStop: "Arrêter",
      radioEditUrl: "Modifier l'adresse",
      rs_connecting: "Connexion…",
      rs_buffering: "Mise en mémoire tampon…",
      rs_playing: "Lecture en cours",
      rs_reconnecting: "Reconnexion…",
      rs_error: "Erreur : {e}",
      r_url: "Adresse non valide (http:// ou https:// requis).",
      r_name: "Donnez un nom à la station.",
      r_full: "La liste des stations est pleine.",
      r_blocked: "Cette adresse pointe vers un réseau local ou privé et ne peut pas être ouverte pour des raisons de sécurité.",
      r_dns: "Nom du serveur introuvable.",
      r_connect: "Le serveur est injoignable.",
      r_timeout: "Le serveur n'a pas répondu à temps.",
      r_tls: "La connexion sécurisée n'a pas pu être établie.",
      r_http: "Le serveur a renvoyé une erreur.",
      r_hls: "Il s'agit d'un flux segmenté (HLS), pas encore pris en charge.",
      r_html: "Cette adresse est une page web, pas un flux audio.",
      r_format: "Format inconnu.",
      r_decode: "Le flux ne peut pas être décodé.",
      r_playlist: "La liste de lecture ne contient aucune adresse exploitable.",
      r_redirect: "Trop de redirections.",
      r_nocurl: "La radio a besoin du programme curl (installez-le avec votre gestionnaire de paquets).",
      r_formatChange: "Le format du flux a changé.",
      r_eof: "Le flux a été interrompu.",
      r_empty: "Le flux est vide.",
      r_unknown: "Station inconnue.",
      r_output: "La sortie audio ne peut pas être ouverte.",
      r_remote: "La liste des stations ne peut être modifiée que sur l'ordinateur (ou depuis un téléphone administrateur).",
      radioSearch: "Trouver une station",
      radioSearchQ: "Nom de la station (ou une partie)",
      radioCatalogOn: "Activer la recherche de stations",
      radioCatalogOff: "Désactiver la recherche",
      radioCatalogOk: "Activer",
      radioCatalogText: "La recherche de stations utilise le catalogue communautaire de stations radio-browser.info. Lors d'une recherche, cet ordinateur contacte directement les serveurs du catalogue : ils voient l'adresse IP de cet ordinateur et le texte recherché. Les serveurs sont gérés par la communauté du catalogue ; nous n'avons trouvé ni politique de confidentialité ni licence pour la base de données. Aqulify n'envoie que la recherche (pas de comptage de clics, pas de stations écoutées, aucune autre donnée). Vous pouvez désactiver la recherche à tout moment.",
      radioResults: "Résultats pour « {q} »",
      radioSearching: "Recherche…",
      radioNoResults: "Aucun résultat.",
      radioAddThis: "Ajouter à la liste",
      radioAdded: "Ajoutée : {name}",
      r_catalogOff: "La recherche de stations est désactivée.",
      r_query: "Saisissez un texte à rechercher.",
      r_json: "La réponse du catalogue n'a pas pu être lue.",
      r_size: "La réponse du catalogue est trop volumineuse."
    },
    zh: {
      albums: "专辑",
      folders: "文件夹",
      lists: "播放列表",
      queue: "播放队列",
      loading: "正在加载...",
      empty: "音乐库为空。音乐文件夹在电脑上设置（默认为“音乐”文件夹）。",
      scanning: "电脑正在为音乐库建立索引（{n} 个文件）...",
      noAlbums: "暂无专辑。",
      various: "群星",
      tracks: "{n} 首曲目",
      play: "播放",
      shuffle: "随机播放",
      playNext: "下一首播放",
      playLast: "添加到播放队列",
      addToList: "添加到播放列表...",
      cancel: "取消",
      ok: "OK",
      folderEmpty: "此文件夹为空。",
      roots: "音乐文件夹",
      back: "返回",
      queueEmpty: "播放队列为空。请开始播放专辑、文件夹或播放列表。",
      clear: "清空",
      saveAsList: "另存为播放列表",
      repeatOff: "循环：关",
      repeatAll: "循环：队列",
      repeatOne: "循环：单曲",
      shuffleOn: "随机：开",
      shuffleOff: "随机：关",
      moveUp: "上移",
      moveDown: "下移",
      remove: "移除",
      removeFromList: "从播放列表中移除",
      newList: "新建播放列表",
      newSmart: "新建智能播放列表",
      listName: "播放列表名称：",
      rename: "重命名",
      delete: "删除",
      deleteConfirm: "确定删除播放列表“{name}”吗？",
      editRules: "规则",
      unavailable: "不可用",
      smartNote: "智能播放列表：电脑将根据规则计算其内容。",
      noLists: "暂无播放列表。",
      chooseList: "选择哪个播放列表？",
      added: "已添加：{name}",
      queued: "已添加到播放队列",
      saved: "已保存：{name}",
      error: "错误：{e}",
      smartEditor: "智能播放列表",
      name: "名称",
      match: "条件",
      matchAll: "须满足全部条件",
      matchAny: "满足任一条件即可",
      addCondition: "+ 添加条件",
      sort: "排序依据",
      desc: "降序",
      limit: "数量上限（0：全部）",
      save: "保存",
      days: "天",
      more: "更多",
      colTitle: "标题",
      colArtist: "艺术家",
      colAlbum: "专辑",
      colDuration: "时长",
      stats: "{tracks} 首曲目 · {albums} 张专辑",
      playAlbum: "播放专辑",
      f_artist: "艺术家",
      f_album: "专辑",
      f_title: "标题",
      f_genre: "流派",
      f_year: "年份",
      f_duration: "时长（秒）",
      f_plays: "播放次数",
      f_added: "添加时间",
      f_lastPlayed: "上次播放",
      o_contains: "包含",
      o_notContains: "不包含",
      o_is: "是",
      o_isNot: "不是",
      o_startsWith: "开头是",
      o_eq: "=",
      o_ne: "≠",
      o_gt: ">",
      o_ge: "≥",
      o_lt: "<",
      o_le: "≤",
      o_inLast: "在过去 … 天内",
      o_notInLast: "不在过去 … 天内",
      o_never: "从未",
      s_added: "添加时间",
      s_lastPlayed: "上次播放",
      s_plays: "播放次数",
      s_year: "年份",
      s_artist: "艺术家",
      s_album: "专辑",
      s_title: "标题",
      s_duration: "时长",
      s_random: "随机",
      e_empty: "没有可播放的内容",
      e_name: "请输入名称",
      e_unknown: "未找到",
      e_smart: "无法向智能播放列表手动添加曲目",
      e_full: "已满",
      e_index: "无效位置",
      e_rules: "无效规则",
      srcPc: "💻 电脑音乐库",
      srcPhone: "📱 手机音乐",
      phoneEmpty: "尚未添加手机音乐文件夹。",
      addFolder: "添加文件夹",
      rescan: "重新扫描",
      scanningPhone: "正在扫描手机音乐库...",
      phoneTracks: "在手机上找到 {n} 首曲目",
      notSupportedHere: "此功能在此设备上不可用。",
      removeFolderConfirm: "确定从手机音乐库中移除文件夹“{name}”吗？",
      phoneTrackLocalMatch: "在电脑上找到本地副本：“{title}”",
      sendingTrack: "正在向电脑传输曲目...",
      copyToPc: "复制到电脑",
      copyConfirmPrompt: "“{title}”尚未加入电脑音乐库。你想执行什么操作？",
      optPlayOnly: "▶ 仅立即播放",
      optCopyOnce: "📥 复制到电脑",
      optCopyAlways: "💾 始终复制到电脑",
      optNeverAsk: "🚫 不再询问（仅播放）",
      copiedToPc: "曲目已复制到电脑音乐库！",
      copyPrefTitle: "复制到电脑设置",
      prefAsk: "每首新曲目均询问",
      prefAlways: "始终复制到电脑",
      prefNever: "从不复制（仅播放）",
      prefSaved: "设置已保存",
      onPhone: "在手机上",
      matchedOnPc: "电脑上的本地副本",
      phoneDisconnected: "手机未连接",
      radio: "电台",
      radioAdd: "新电台",
      radioName: "电台名称",
      radioUrl: "电台地址（http:// 或 https://）",
      radioEmpty: "还没有电台。请添加您喜爱电台的地址（在电台网站上通常标为“收听”或“Stream”；.pls 和 .m3u 也可以）。",
      radioNote: "电台通过内置播放器、均衡器和整个音频链路播放。支持：MP3、AAC（含 HE-AAC）、Ogg Vorbis、Opus、FLAC；暂不支持分段（HLS）流。",
      radioPlay: "播放",
      radioStop: "停止",
      radioEditUrl: "更改地址",
      rs_connecting: "正在连接…",
      rs_buffering: "正在缓冲…",
      rs_playing: "正在播放",
      rs_reconnecting: "正在重新连接…",
      rs_error: "错误：{e}",
      r_url: "地址无效（必须是 http:// 或 https://）。",
      r_name: "请给电台起个名字。",
      r_full: "电台列表已满。",
      r_blocked: "此地址指向本地或私有网络地址，出于安全原因无法打开。",
      r_dns: "找不到服务器名称。",
      r_connect: "无法连接到服务器。",
      r_timeout: "服务器未及时响应。",
      r_tls: "无法建立安全连接。",
      r_http: "服务器返回了错误。",
      r_hls: "这是分段（HLS）流，暂不支持。",
      r_html: "此地址是网页，而不是音频流。",
      r_format: "未知格式。",
      r_decode: "无法解码此流。",
      r_playlist: "播放列表中没有可用的地址。",
      r_redirect: "重定向次数过多。",
      r_nocurl: "电台需要 curl 程序（请用包管理器安装）。",
      r_formatChange: "流的格式发生了变化。",
      r_eof: "流被中断。",
      r_empty: "流为空。",
      r_unknown: "未知电台。",
      r_output: "无法打开音频输出。",
      r_remote: "电台列表只能在电脑上编辑（或通过管理员手机编辑）。",
      radioSearch: "查找电台",
      radioSearchQ: "电台名称（或其中一部分）",
      radioCatalogOn: "开启电台搜索",
      radioCatalogOff: "关闭搜索",
      radioCatalogOk: "开启",
      radioCatalogText: "电台搜索使用 radio-browser.info 社区电台目录。搜索时，本机会直接连接目录的服务器：它们能看到本机的 IP 地址和搜索文字。这些服务器由目录社区运营；我们没有找到其隐私政策或数据库许可协议。Aqulify 只发送搜索内容（不统计点击，不发送播放过的电台，也不发送其他数据）。您可以随时关闭搜索。",
      radioResults: "“{q}”的搜索结果",
      radioSearching: "正在搜索…",
      radioNoResults: "没有结果。",
      radioAddThis: "添加到列表",
      radioAdded: "已添加：{name}",
      r_catalogOff: "电台搜索已关闭。",
      r_query: "请输入搜索文字。",
      r_json: "无法读取目录的响应。",
      r_size: "目录的响应过大。"
    },
    de: {
      albums: "Alben", folders: "Ordner", lists: "Playlists", queue: "Warteschlange", loading: "Wird geladen...",
      empty: "Die Musikbibliothek ist leer. Musikordner werden auf dem PC eingerichtet (standardmäßig der Ordner Musik).",
      scanning: "Der PC indiziert die Bibliothek ({n} Dateien)...", noAlbums: "Keine Alben vorhanden.",
      various: "Verschiedene Interpreten", tracks: "{n} Titel", play: "Wiedergabe", shuffle: "Zufällig",
      playNext: "Als nächstes", playLast: "Ans Ende der Warteschlange", addToList: "Zu Playlist hinzufügen...", cancel: "Abbrechen", ok: "OK",
      folderEmpty: "Dieser Ordner ist leer.", roots: "Musikordner", back: "Zurück",
      queueEmpty: "Die Warteschlange ist leer. Starten Sie ein Album, einen Ordner oder eine Playlist.", clear: "Leeren", saveAsList: "Als Playlist speichern",
      repeatOff: "Wiederholung: aus", repeatAll: "Wiederholung: Warteschlange", repeatOne: "Wiederholung: Titel", shuffleOn: "Zufall: ein",
      shuffleOff: "Zufall: aus", moveUp: "Nach oben", moveDown: "Nach unten", remove: "Entfernen", removeFromList: "Aus Playlist entfernen",
      newList: "Neue Playlist", newSmart: "Neue intelligente Playlist", listName: "Playlist-Name:", rename: "Umbenennen", delete: "Löschen",
      deleteConfirm: "Playlist „{name}“ löschen?", editRules: "Regeln", unavailable: "nicht verfügbar",
      smartNote: "Intelligente Playlist: Der PC berechnet den Inhalt anhand von Regeln.", noLists: "Noch keine Playlists vorhanden.",
      chooseList: "Zu welcher Playlist?", added: "Hinzugefügt: {name}", queued: "Zur Warteschlange hinzugefügt", saved: "Gespeichert: {name}",
      error: "Fehler: {e}", smartEditor: "Intelligente Playlist", name: "Name", match: "Bedingungen", matchAll: "alle müssen zutreffen",
      matchAny: "mindestens eine muss zutreffen", addCondition: "+ Bedingung", sort: "Sortieren nach", desc: "absteigend",
      limit: "Maximal (0: alle)", save: "Speichern", days: "Tage", more: "Mehr",
      colTitle: "Titel", colArtist: "Interpret", colAlbum: "Album", colDuration: "Dauer",
      stats: "{tracks} Titel · {albums} Alben", playAlbum: "Album abspielen",
      f_artist: "Interpret", f_album: "Album", f_title: "Titel", f_genre: "Genre", f_year: "Jahr", f_duration: "Dauer (s)",
      f_plays: "Wiedergaben", f_added: "Hinzugefügt", f_lastPlayed: "Zuletzt gespielt",
      o_contains: "enthält", o_notContains: "enthält nicht", o_is: "ist genau", o_isNot: "ist nicht", o_startsWith: "beginnt mit",
      o_eq: "=", o_ne: "≠", o_gt: ">", o_ge: "≥", o_lt: "<", o_le: "≤", o_inLast: "in den letzten … Tagen",
      o_notInLast: "nicht in den letzten … Tagen", o_never: "nie",
      s_added: "Hinzugefügt", s_lastPlayed: "Zuletzt gespielt", s_plays: "Wiedergaben", s_year: "Jahr", s_artist: "Interpret",
      s_album: "Album", s_title: "Titel", s_duration: "Dauer", s_random: "Zufällig",
      e_empty: "nichts abzuspielen", e_name: "Name eingeben", e_unknown: "nicht gefunden", e_smart: "Titel können keiner intelligenten Playlist manuell hinzugefügt werden",
      e_full: "voll", e_index: "ungültige Position", e_rules: "ungültige Regel",
      srcPc: "💻 PC-Bibliothek", srcPhone: "📱 Telefon-Musik",
      phoneEmpty: "Noch kein Musikordner vom Telefon hinzugefügt.",
      addFolder: "Ordner hinzufügen", rescan: "Neu einlesen",
      scanningPhone: "Telefon-Musikbibliothek wird durchsucht...",
      phoneTracks: "{n} Titel auf dem Telefon gefunden",
      notSupportedHere: "Diese Funktion ist auf diesem Gerät nicht verfügbar.",
      removeFolderConfirm: "Ordner „{name}“ aus der Telefonbibliothek entfernen?",
      phoneTrackLocalMatch: "Lokale Kopie auf dem PC gefunden: „{title}“",
      sendingTrack: "Titel wird zum PC gestreamt...",
      copyToPc: "Auf PC kopieren",
      copyConfirmPrompt: "„{title}“ befindet sich noch nicht in der PC-Bibliothek. Was möchten Sie tun?",
      optPlayOnly: "▶ Nur jetzt abspielen",
      optCopyOnce: "📥 Auf PC kopieren",
      optCopyAlways: "💾 Immer auf PC kopieren",
      optNeverAsk: "🚫 Nie fragen (nur abspielen)",
      copiedToPc: "Titel in die PC-Bibliothek kopiert!",
      copyPrefTitle: "Einstellungen für das Kopieren auf den PC",
      prefAsk: "Bei jedem neuen Titel nachfragen",
      prefAlways: "Immer auf den PC kopieren",
      prefNever: "Nie kopieren (nur abspielen)",
      prefSaved: "Einstellung gespeichert",
      onPhone: "Auf dem Telefon",
      matchedOnPc: "Lokale Kopie auf dem PC",
      phoneDisconnected: "Telefon nicht verbunden",
      radio: "Radio",
      radioAdd: "Neuer Sender",
      radioName: "Sendername",
      radioUrl: "Senderadresse (http:// oder https://)",
      radioEmpty: "Noch keine Sender. Füge die Adresse deiner Lieblingssender hinzu (auf der Website des Senders heißt sie meist „Zuhören“ oder „Stream“; .pls und .m3u funktionieren auch).",
      radioNote: "Das Radio läuft über den integrierten Player, den Equalizer und die gesamte Audiokette. Unterstützt: MP3, AAC (inkl. HE-AAC), Ogg Vorbis, Opus, FLAC; segmentierte (HLS) Streams noch nicht.",
      radioPlay: "Wiedergabe",
      radioStop: "Stopp",
      radioEditUrl: "Adresse ändern",
      rs_connecting: "Verbindung wird hergestellt…",
      rs_buffering: "Pufferung…",
      rs_playing: "Wiedergabe läuft",
      rs_reconnecting: "Verbindung wird neu hergestellt…",
      rs_error: "Fehler: {e}",
      r_url: "Ungültige Adresse (http:// oder https:// erforderlich).",
      r_name: "Gib dem Sender einen Namen.",
      r_full: "Die Senderliste ist voll.",
      r_blocked: "Diese Adresse verweist auf eine lokale oder private Netzwerkadresse und kann aus Sicherheitsgründen nicht geöffnet werden.",
      r_dns: "Servername nicht gefunden.",
      r_connect: "Der Server ist nicht erreichbar.",
      r_timeout: "Der Server hat nicht rechtzeitig geantwortet.",
      r_tls: "Die sichere Verbindung konnte nicht hergestellt werden.",
      r_http: "Der Server hat einen Fehler zurückgegeben.",
      r_hls: "Das ist ein segmentierter (HLS) Stream, der noch nicht unterstützt wird.",
      r_html: "Diese Adresse ist eine Webseite, kein Audiostream.",
      r_format: "Unbekanntes Format.",
      r_decode: "Der Stream kann nicht dekodiert werden.",
      r_playlist: "Die Wiedergabeliste enthält keine verwendbare Adresse.",
      r_redirect: "Zu viele Weiterleitungen.",
      r_nocurl: "Das Radio benötigt das Programm curl (installiere es mit deiner Paketverwaltung).",
      r_formatChange: "Das Streamformat hat sich geändert.",
      r_eof: "Der Stream wurde unterbrochen.",
      r_empty: "Der Stream ist leer.",
      r_unknown: "Unbekannter Sender.",
      r_output: "Der Audioausgang kann nicht geöffnet werden.",
      r_remote: "Die Senderliste kann nur am Computer (oder von einem Administrator-Smartphone) bearbeitet werden.",
      radioSearch: "Sender suchen",
      radioSearchQ: "Sendername (oder ein Teil davon)",
      radioCatalogOn: "Sendersuche einschalten",
      radioCatalogOff: "Suche ausschalten",
      radioCatalogOk: "Einschalten",
      radioCatalogText: "Die Sendersuche nutzt den Community-Sender-Katalog radio-browser.info. Bei einer Suche kontaktiert dieser Computer direkt die Server des Katalogs: Sie sehen die IP-Adresse dieses Computers und den Suchtext. Die Server werden von der Community des Katalogs betrieben; wir konnten weder eine Datenschutzerklärung noch eine Lizenz für die Datenbank finden. Aqulify sendet nur die Suche (kein Klickzählen, keine gespielten Sender, keine anderen Daten). Du kannst die Suche jederzeit ausschalten.",
      radioResults: "Ergebnisse für „{q}“",
      radioSearching: "Suche läuft…",
      radioNoResults: "Keine Ergebnisse.",
      radioAddThis: "Zur Liste hinzufügen",
      radioAdded: "Hinzugefügt: {name}",
      r_catalogOff: "Die Sendersuche ist ausgeschaltet.",
      r_query: "Gib einen Suchtext ein.",
      r_json: "Die Antwort des Katalogs konnte nicht gelesen werden.",
      r_size: "Die Antwort des Katalogs ist zu groß."
    },
    it: {
      albums: "Album", folders: "Cartelle", lists: "Playlist", queue: "Coda", loading: "Caricamento in corso...",
      empty: "La libreria è vuota. Le cartelle musicali si impostano sul PC (predefinita la cartella Musica).",
      scanning: "Il PC sta indicizzando la libreria ({n} file)...", noAlbums: "Nessun album.",
      various: "Artisti vari", tracks: "{n} brani", play: "Riproduci", shuffle: "Casuale",
      playNext: "Riproduci come successivo", playLast: "Aggiungi a fine coda", addToList: "Aggiungi alla playlist...", cancel: "Annulla", ok: "OK",
      folderEmpty: "Questa cartella è vuota.", roots: "Cartelle musicali", back: "Indietro",
      queueEmpty: "La coda è vuota. Avvia un album, una cartella o una playlist.", clear: "Svuota", saveAsList: "Salva come playlist",
      repeatOff: "Ripeti: disattivato", repeatAll: "Ripeti: coda", repeatOne: "Ripeti: brano", shuffleOn: "Casuale: attivo",
      shuffleOff: "Casuale: disattivato", moveUp: "Sposta su", moveDown: "Sposta giù", remove: "Rimuovi", removeFromList: "Rimuovi dalla playlist",
      newList: "Nuova playlist", newSmart: "Nuova smart playlist", listName: "Nome playlist:", rename: "Rinomina", delete: "Elimina",
      deleteConfirm: "Eliminare la playlist \"{name}\"?", editRules: "Regole", unavailable: "non disponibile",
      smartNote: "Smart playlist: il PC ne calcola il contenuto in base alle regole.", noLists: "Nessuna playlist al momento.",
      chooseList: "Quale playlist?", added: "Aggiunto: {name}", queued: "Aggiunto alla coda", saved: "Salvato: {name}",
      error: "Errore: {e}", smartEditor: "Smart playlist", name: "Nome", match: "Condizioni", matchAll: "devono corrispondere tutte",
      matchAny: "può corrisponderne una qualsiasi", addCondition: "+ Condizione", sort: "Ordina per", desc: "decrescente",
      limit: "Al massimo (0: tutti)", save: "Salva", days: "giorni", more: "Altro",
      colTitle: "Titolo", colArtist: "Artista", colAlbum: "Album", colDuration: "Durata",
      stats: "{tracks} brani · {albums} album", playAlbum: "Riproduci album",
      f_artist: "Artista", f_album: "Album", f_title: "Titolo", f_genre: "Genere", f_year: "Anno", f_duration: "Durata (s)",
      f_plays: "Riproduzioni", f_added: "Aggiunto", f_lastPlayed: "Ultimo ascolto",
      o_contains: "contiene", o_notContains: "non contiene", o_is: "è esattamente", o_isNot: "non è", o_startsWith: "inizia con",
      o_eq: "=", o_ne: "≠", o_gt: ">", o_ge: "≥", o_lt: "<", o_le: "≤", o_inLast: "negli ultimi … giorni",
      o_notInLast: "non negli ultimi … giorni", o_never: "mai",
      s_added: "Aggiunto", s_lastPlayed: "Ultimo ascolto", s_plays: "Riproduzioni", s_year: "Anno", s_artist: "Artista",
      s_album: "Album", s_title: "Titolo", s_duration: "Durata", s_random: "Casuale",
      e_empty: "nulla da riprodurre", e_name: "inserisci un nome", e_unknown: "non trovato", e_smart: "impossibile aggiungere brani a una smart playlist",
      e_full: "pieno", e_index: "posizione non valida", e_rules: "regola non valida",
      srcPc: "💻 Libreria PC", srcPhone: "📱 Musica telefono",
      phoneEmpty: "Nessuna cartella musicale aggiunta dal telefono finora.",
      addFolder: "Aggiungi cartella", rescan: "Riscansiona",
      scanningPhone: "Scansione libreria musicale del telefono...",
      phoneTracks: "{n} brani trovati sul telefono",
      notSupportedHere: "Funzionalità non disponibile su questo dispositivo.",
      removeFolderConfirm: "Rimuovere la cartella \"{name}\" dalla libreria del telefono?",
      phoneTrackLocalMatch: "Copia locale trovata sul PC: \"{title}\"",
      sendingTrack: "Streaming brano verso il PC...",
      copyToPc: "Copia sul PC",
      copyConfirmPrompt: "\"{title}\" non è ancora nella libreria del PC. Cosa desideri fare?",
      optPlayOnly: "▶ Riproduci ora soltanto",
      optCopyOnce: "📥 Copia sul PC",
      optCopyAlways: "💾 Copia sempre sul PC",
      optNeverAsk: "🚫 Non chiedere più (solo riproduzione)",
      copiedToPc: "Brano copiato nella libreria del PC!",
      copyPrefTitle: "Impostazioni di copia su PC",
      prefAsk: "Chiedi per ogni nuovo brano",
      prefAlways: "Copia sempre sul PC",
      prefNever: "Non copiare mai (solo riproduzione)",
      prefSaved: "Impostazione salvata",
      onPhone: "Sul telefono",
      matchedOnPc: "Copia locale sul PC",
      phoneDisconnected: "Telefono non connesso",
      radio: "Radio",
      radioAdd: "Nuova stazione",
      radioName: "Nome della stazione",
      radioUrl: "Indirizzo della stazione (http:// o https://)",
      radioEmpty: "Ancora nessuna stazione. Aggiungi l'indirizzo delle tue stazioni preferite (sul sito della stazione di solito si chiama «Ascolta» o «Stream»; funzionano anche .pls e .m3u).",
      radioNote: "La radio passa dal lettore integrato, dall'equalizzatore e da tutta la catena audio. Supportati: MP3, AAC (incl. HE-AAC), Ogg Vorbis, Opus, FLAC; flussi segmentati (HLS) non ancora.",
      radioPlay: "Riproduci",
      radioStop: "Arresta",
      radioEditUrl: "Cambia indirizzo",
      rs_connecting: "Connessione in corso…",
      rs_buffering: "Buffering…",
      rs_playing: "In riproduzione",
      rs_reconnecting: "Riconnessione in corso…",
      rs_error: "Errore: {e}",
      r_url: "Indirizzo non valido (richiesto http:// o https://).",
      r_name: "Dai un nome alla stazione.",
      r_full: "L'elenco delle stazioni è pieno.",
      r_blocked: "Questo indirizzo punta a una rete locale o privata e non può essere aperto per motivi di sicurezza.",
      r_dns: "Nome del server non trovato.",
      r_connect: "Il server non è raggiungibile.",
      r_timeout: "Il server non ha risposto in tempo.",
      r_tls: "Impossibile stabilire la connessione sicura.",
      r_http: "Il server ha restituito un errore.",
      r_hls: "Questo è un flusso segmentato (HLS), non ancora supportato.",
      r_html: "Questo indirizzo è una pagina web, non un flusso audio.",
      r_format: "Formato sconosciuto.",
      r_decode: "Impossibile decodificare il flusso.",
      r_playlist: "La playlist non contiene alcun indirizzo utilizzabile.",
      r_redirect: "Troppi reindirizzamenti.",
      r_nocurl: "La radio richiede il programma curl (installalo con il tuo gestore di pacchetti).",
      r_formatChange: "Il formato del flusso è cambiato.",
      r_eof: "Il flusso è stato interrotto.",
      r_empty: "Il flusso è vuoto.",
      r_unknown: "Stazione sconosciuta.",
      r_output: "Impossibile aprire l'uscita audio.",
      r_remote: "L'elenco delle stazioni può essere modificato solo sul computer (o da un telefono amministratore).",
      radioSearch: "Trova una stazione",
      radioSearchQ: "Nome della stazione (o parte di esso)",
      radioCatalogOn: "Attiva la ricerca di stazioni",
      radioCatalogOff: "Disattiva la ricerca",
      radioCatalogOk: "Attiva",
      radioCatalogText: "La ricerca di stazioni usa il catalogo comunitario di stazioni radio-browser.info. Quando cerchi, questo computer contatta direttamente i server del catalogo: vedono l'indirizzo IP di questo computer e il testo cercato. I server sono gestiti dalla comunità del catalogo; non abbiamo trovato né un'informativa sulla privacy né una licenza del database. Aqulify invia solo la ricerca (nessun conteggio dei clic, nessuna stazione ascoltata, nessun altro dato). Puoi disattivare la ricerca in qualsiasi momento.",
      radioResults: "Risultati per «{q}»",
      radioSearching: "Ricerca in corso…",
      radioNoResults: "Nessun risultato.",
      radioAddThis: "Aggiungi all'elenco",
      radioAdded: "Aggiunta: {name}",
      r_catalogOff: "La ricerca di stazioni è disattivata.",
      r_query: "Inserisci un testo da cercare.",
      r_json: "Impossibile leggere la risposta del catalogo.",
      r_size: "La risposta del catalogo è troppo grande."
    },
    nl: {
      albums: "Albums", folders: "Mappen", lists: "Afspeellijsten", queue: "Wachtrij", loading: "Bezig met laden...",
      empty: "De muziekbibliotheek is leeg. Muziekmappen worden op de PC ingesteld (standaard de map Muziek).",
      scanning: "De PC indexeert de muziekbibliotheek ({n} bestanden)...", noAlbums: "Geen albums.",
      various: "Diverse artiesten", tracks: "{n} nummers", play: "Afspelen", shuffle: "Willekeurig",
      playNext: "Als volgende afspelen", playLast: "Aan het einde van de wachtrij", addToList: "Aan afspeellijst toevoegen...", cancel: "Annuleren", ok: "OK",
      folderEmpty: "Deze map is leeg.", roots: "Muziekmappen", back: "Terug",
      queueEmpty: "De wachtrij is leeg. Start een album, map of afspeellijst.", clear: "Wissen", saveAsList: "Opslaan als afspeellijst",
      repeatOff: "Herhalen: uit", repeatAll: "Herhalen: wachtrij", repeatOne: "Herhalen: nummer", shuffleOn: "Willekeurig: aan",
      shuffleOff: "Willekeurig: uit", moveUp: "Omhoog", moveDown: "Omlaag", remove: "Verwijderen", removeFromList: "Verwijderen uit afspeellijst",
      newList: "Nieuwe afspeellijst", newSmart: "Nieuwe slimme afspeellijst", listName: "Naam afspeellijst:", rename: "Hernoemen", delete: "Verwijderen",
      deleteConfirm: "Afspeellijst \"{name}\" verwijderen?", editRules: "Regels", unavailable: "niet beschikbaar",
      smartNote: "Slimme afspeellijst: de PC berekent de inhoud op basis van regels.", noLists: "Nog geen afspeellijsten.",
      chooseList: "Welke afspeellijst?", added: "Toegevoegd: {name}", queued: "Toegevoegd aan de wachtrij", saved: "Opgeslagen: {name}",
      error: "Fout: {e}", smartEditor: "Slimme afspeellijst", name: "Naam", match: "Voorwaarden", matchAll: "alle moeten overeenkomen",
      matchAny: "minstens één moet overeenkomen", addCondition: "+ Voorwaarde", sort: "Sorteren op", desc: "aflopend",
      limit: "Maximaal (0: alles)", save: "Opslaan", days: "dagen", more: "Meer",
      colTitle: "Titel", colArtist: "Artiest", colAlbum: "Album", colDuration: "Duur",
      stats: "{tracks} nummers · {albums} albums", playAlbum: "Album afspelen",
      f_artist: "Artiest", f_album: "Album", f_title: "Titel", f_genre: "Genre", f_year: "Jaar", f_duration: "Duur (s)",
      f_plays: "Aantal keren gespeeld", f_added: "Toegevoegd", f_lastPlayed: "Laatst gespeeld",
      o_contains: "bevat", o_notContains: "bevat niet", o_is: "is exact", o_isNot: "is niet", o_startsWith: "begint met",
      o_eq: "=", o_ne: "≠", o_gt: ">", o_ge: "≥", o_lt: "<", o_le: "≤", o_inLast: "in de afgelopen … dagen",
      o_notInLast: "niet in de afgelopen … dagen", o_never: "nooit",
      s_added: "Toegevoegd", s_lastPlayed: "Laatst gespeeld", s_plays: "Aantal keren gespeeld", s_year: "Jaar", s_artist: "Artiest",
      s_album: "Album", s_title: "Titel", s_duration: "Duur", s_random: "Willekeurig",
      e_empty: "niets om af te spelen", e_name: "voer een naam in", e_unknown: "niet gevonden", e_smart: "kan geen nummers handmatig toevoegen aan een slimme afspeellijst",
      e_full: "vol", e_index: "ongeldige positie", e_rules: "ongeldige regel",
      srcPc: "💻 PC-bibliotheek", srcPhone: "📱 Telefoonmuziek",
      phoneEmpty: "Nog geen muziekmap toegevoegd vanaf telefoon.",
      addFolder: "Map toevoegen", rescan: "Opnieuw scannen",
      scanningPhone: "Telefoonmuziek scannen...",
      phoneTracks: "{n} nummers gevonden op telefoon",
      notSupportedHere: "Functie niet beschikbaar op dit apparaat.",
      removeFolderConfirm: "Map \"{name}\" verwijderen uit telefoonbibliotheek?",
      phoneTrackLocalMatch: "Lokale kopie gevonden op PC: \"{title}\"",
      sendingTrack: "Nummer streamen naar PC...",
      copyToPc: "Kopiëren naar PC",
      copyConfirmPrompt: "\"{title}\" staat nog niet in de PC-bibliotheek. Wat wilt u doen?",
      optPlayOnly: "▶ Alleen nu afspelen",
      optCopyOnce: "📥 Kopiëren naar PC",
      optCopyAlways: "💾 Altijd kopiëren naar PC",
      optNeverAsk: "🚫 Nooit meer vragen (alleen afspelen)",
      copiedToPc: "Nummer gekopieerd naar PC-bibliotheek!",
      copyPrefTitle: "Instellingen voor kopiëren naar PC",
      prefAsk: "Vragen bij elk nieuw nummer",
      prefAlways: "Altijd kopiëren naar PC",
      prefNever: "Nooit kopiëren (alleen afspelen)",
      prefSaved: "Instelling opgeslagen",
      onPhone: "Op telefoon",
      matchedOnPc: "Lokale kopie op PC",
      phoneDisconnected: "Telefoon niet verbonden",
      radio: "Radio",
      radioAdd: "Nieuwe zender",
      radioName: "Zendernaam",
      radioUrl: "Zenderadres (http:// of https://)",
      radioEmpty: "Nog geen zenders. Voeg het adres van je favoriete zenders toe (op de website van de zender heet dat meestal “Luisteren” of “Stream”; .pls en .m3u werken ook).",
      radioNote: "Radio speelt via de ingebouwde speler, de equalizer en de hele audioketen. Ondersteund: MP3, AAC (incl. HE-AAC), Ogg Vorbis, Opus, FLAC; gesegmenteerde (HLS) streams nog niet.",
      radioPlay: "Afspelen",
      radioStop: "Stoppen",
      radioEditUrl: "Adres wijzigen",
      rs_connecting: "Verbinden…",
      rs_buffering: "Bufferen…",
      rs_playing: "Speelt af",
      rs_reconnecting: "Opnieuw verbinden…",
      rs_error: "Fout: {e}",
      r_url: "Ongeldig adres (http:// of https:// vereist).",
      r_name: "Geef de zender een naam.",
      r_full: "De zenderlijst is vol.",
      r_blocked: "Dit adres verwijst naar een lokaal of privénetwerkadres en kan om veiligheidsredenen niet worden geopend.",
      r_dns: "Servernaam niet gevonden.",
      r_connect: "De server is niet bereikbaar.",
      r_timeout: "De server antwoordde niet op tijd.",
      r_tls: "De beveiligde verbinding kon niet worden opgebouwd.",
      r_http: "De server gaf een fout terug.",
      r_hls: "Dit is een gesegmenteerde (HLS) stream, die nog niet wordt ondersteund.",
      r_html: "Dit adres is een webpagina, geen audiostream.",
      r_format: "Onbekend formaat.",
      r_decode: "De stream kan niet worden gedecodeerd.",
      r_playlist: "De afspeellijst bevat geen bruikbaar adres.",
      r_redirect: "Te veel omleidingen.",
      r_nocurl: "Radio heeft het programma curl nodig (installeer het met je pakketbeheerder).",
      r_formatChange: "Het streamformaat is gewijzigd.",
      r_eof: "De stream is onderbroken.",
      r_empty: "De stream is leeg.",
      r_unknown: "Onbekende zender.",
      r_output: "De audio-uitgang kan niet worden geopend.",
      r_remote: "De zenderlijst kan alleen op de computer worden bewerkt (of vanaf een beheerderstelefoon).",
      radioSearch: "Zender zoeken",
      radioSearchQ: "Zendernaam (of een deel ervan)",
      radioCatalogOn: "Zenderzoeker inschakelen",
      radioCatalogOff: "Zoeken uitschakelen",
      radioCatalogOk: "Inschakelen",
      radioCatalogText: "De zenderzoeker gebruikt de community-zenderscatalogus radio-browser.info. Bij het zoeken neemt deze computer rechtstreeks contact op met de servers van de catalogus: die zien het IP-adres van deze computer en de zoektekst. De servers worden beheerd door de community van de catalogus; we hebben geen privacybeleid of licentie voor de database gevonden. Aqulify verstuurt alleen de zoekopdracht (geen kliktelling, geen afgespeelde zenders, geen andere gegevens). Je kunt zoeken op elk moment uitschakelen.",
      radioResults: "Resultaten voor “{q}”",
      radioSearching: "Zoeken…",
      radioNoResults: "Geen resultaten.",
      radioAddThis: "Aan lijst toevoegen",
      radioAdded: "Toegevoegd: {name}",
      r_catalogOff: "Zenderzoeker is uitgeschakeld.",
      r_query: "Voer een zoektekst in.",
      r_json: "Het antwoord van de catalogus kon niet worden gelezen.",
      r_size: "Het antwoord van de catalogus is te groot."
    },
    pl: {
      albums: "Albumy", folders: "Foldery", lists: "Listy odtwarzania", queue: "Kolejka", loading: "Ładowanie...",
      empty: "Biblioteka jest pusta. Foldery muzyczne konfiguruje się na komputerze (domyślnie folder Muzyka).",
      scanning: "Komputer indeksuje bibliotekę ({n} plików)...", noAlbums: "Brak albumów.",
      various: "Różni wykonawcy", tracks: "{n} utworów", play: "Odtwarzaj", shuffle: "Losowo",
      playNext: "Odtwórz jako następny", playLast: "Na koniec kolejki", addToList: "Dodaj do listy...", cancel: "Anuluj", ok: "OK",
      folderEmpty: "Ten folder jest pusty.", roots: "Foldery muzyczne", back: "Wstecz",
      queueEmpty: "Kolejka jest pusta. Uruchom album, folder lub listę.", clear: "Wyczyść", saveAsList: "Zapisz jako listę",
      repeatOff: "Powtarzanie: wył.", repeatAll: "Powtarzanie: kolejka", repeatOne: "Powtarzanie: utwór", shuffleOn: "Losowo: włączone",
      shuffleOff: "Losowo: wyłączone", moveUp: "W górę", moveDown: "W dół", remove: "Usuń", removeFromList: "Usuń z listy",
      newList: "Nowa lista", newSmart: "Nowa inteligentna lista", listName: "Nazwa listy:", rename: "Zmień nazwę", delete: "Usuń",
      deleteConfirm: "Usunąć listę „{name}”?", editRules: "Reguły", unavailable: "niedostępne",
      smartNote: "Inteligentna lista: komputer oblicza jej zawartość na podstawie reguł.", noLists: "Brak list odtwarzania.",
      chooseList: "Do której listy?", added: "Dodano: {name}", queued: "Dodano do kolejki", saved: "Zapisano: {name}",
      error: "Błąd: {e}", smartEditor: "Inteligentna lista", name: "Nazwa", match: "Warunki", matchAll: "wszystkie muszą być spełnione",
      matchAny: "dowolny może być spełniony", addCondition: "+ Warunek", sort: "Sortuj według", desc: "malejąco",
      limit: "Maksymalnie (0: wszystkie)", save: "Zapisz", days: "dni", more: "Więcej",
      colTitle: "Tytuł", colArtist: "Wykonawca", colAlbum: "Album", colDuration: "Czas",
      stats: "{tracks} utworów · {albums} albumów", playAlbum: "Odtwórz album",
      f_artist: "Wykonawca", f_album: "Album", f_title: "Tytuł", f_genre: "Gatunek", f_year: "Rok", f_duration: "Czas (s)",
      f_plays: "Odtworzenia", f_added: "Dodano", f_lastPlayed: "Ostatnio odtwarzane",
      o_contains: "zawiera", o_notContains: "nie zawiera", o_is: "jest dokładnie", o_isNot: "nie jest", o_startsWith: "zaczyna się od",
      o_eq: "=", o_ne: "≠", o_gt: ">", o_ge: "≥", o_lt: "<", o_le: "≤", o_inLast: "w ciągu ostatnich … dni",
      o_notInLast: "nie w ciągu ostatnich … dni", o_never: "nigdy",
      s_added: "Dodano", s_lastPlayed: "Ostatnio odtwarzane", s_plays: "Odtworzenia", s_year: "Rok", s_artist: "Wykonawca",
      s_album: "Album", s_title: "Tytuł", s_duration: "Czas", s_random: "Losowo",
      e_empty: "brak utworów do odtworzenia", e_name: "podaj nazwę", e_unknown: "nie znaleziono", e_smart: "nie można ręcznie dodawać utworów do inteligentnej listy",
      e_full: "pełna", e_index: "nieprawidłowa pozycja", e_rules: "nieprawidłowa reguła",
      srcPc: "💻 Biblioteka PC", srcPhone: "📱 Muzyka z telefonu",
      phoneEmpty: "Nie dodano jeszcze folderu muzycznego z telefonu.",
      addFolder: "Dodaj folder", rescan: "Przeskanuj ponownie",
      scanningPhone: "Skanowanie biblioteki telefonu...",
      phoneTracks: "Znaleziono {n} utworów na telefonie",
      notSupportedHere: "Funkcja niedostępna na tym urządzeniu.",
      removeFolderConfirm: "Usunąć folder „{name}” z biblioteki telefonu?",
      phoneTrackLocalMatch: "Znaleziono lokalną kopię na PC: „{title}”",
      sendingTrack: "Przesyłanie utworu do PC...",
      copyToPc: "Kopiuj na PC",
      copyConfirmPrompt: "Utwór „{title}” nie znajduje się jeszcze w bibliotece PC. Co chcesz zrobić?",
      optPlayOnly: "▶ Tylko odtwórz teraz",
      optCopyOnce: "📥 Kopiuj na PC",
      optCopyAlways: "💾 Zawsze kopiuj na PC",
      optNeverAsk: "🚫 Nigdy nie pytaj (tylko odtwarzaj)",
      copiedToPc: "Utwór skopiowany do biblioteki PC!",
      copyPrefTitle: "Ustawienia kopiowania na PC",
      prefAsk: "Pytaj przy każdym nowym utworze",
      prefAlways: "Zawsze kopiuj na PC",
      prefNever: "Nigdy nie kopiuj (tylko odtwarzaj)",
      prefSaved: "Ustawienie zapisane",
      onPhone: "W telefonie",
      matchedOnPc: "Lokalna kopia na PC",
      phoneDisconnected: "Telefon nie jest połączony",
      radio: "Radio",
      radioAdd: "Nowa stacja",
      radioName: "Nazwa stacji",
      radioUrl: "Adres stacji (http:// lub https://)",
      radioEmpty: "Brak stacji. Dodaj adresy ulubionych stacji (na stronie stacji zwykle opisany jest jako „Słuchaj” lub „Stream”; działają też .pls i .m3u).",
      radioNote: "Radio gra przez wbudowany odtwarzacz, korektor i cały tor audio. Obsługiwane: MP3, AAC (w tym HE-AAC), Ogg Vorbis, Opus, FLAC; strumieni segmentowanych (HLS) jeszcze nie.",
      radioPlay: "Odtwarzaj",
      radioStop: "Zatrzymaj",
      radioEditUrl: "Zmień adres",
      rs_connecting: "Łączenie…",
      rs_buffering: "Buforowanie…",
      rs_playing: "Gra",
      rs_reconnecting: "Ponowne łączenie…",
      rs_error: "Błąd: {e}",
      r_url: "Nieprawidłowy adres (wymagane http:// lub https://).",
      r_name: "Nadaj stacji nazwę.",
      r_full: "Lista stacji jest pełna.",
      r_blocked: "Ten adres wskazuje na adres sieci lokalnej lub prywatnej i ze względów bezpieczeństwa nie można go otworzyć.",
      r_dns: "Nie znaleziono nazwy serwera.",
      r_connect: "Serwer jest nieosiągalny.",
      r_timeout: "Serwer nie odpowiedział na czas.",
      r_tls: "Nie udało się nawiązać bezpiecznego połączenia.",
      r_http: "Serwer zwrócił błąd.",
      r_hls: "To strumień segmentowy (HLS), który nie jest jeszcze obsługiwany.",
      r_html: "Ten adres to strona internetowa, a nie strumień audio.",
      r_format: "Nieznany format.",
      r_decode: "Nie można zdekodować strumienia.",
      r_playlist: "Lista odtwarzania nie zawiera użytecznego adresu.",
      r_redirect: "Zbyt wiele przekierowań.",
      r_nocurl: "Radio wymaga programu curl (zainstaluj go menedżerem pakietów).",
      r_formatChange: "Format strumienia się zmienił.",
      r_eof: "Strumień został przerwany.",
      r_empty: "Strumień jest pusty.",
      r_unknown: "Nieznana stacja.",
      r_output: "Nie można otworzyć wyjścia audio.",
      r_remote: "Listę stacji można edytować tylko na komputerze (lub z telefonu administratora).",
      radioSearch: "Znajdź stację",
      radioSearchQ: "Nazwa stacji (lub jej fragment)",
      radioCatalogOn: "Włącz wyszukiwanie stacji",
      radioCatalogOff: "Wyłącz wyszukiwanie",
      radioCatalogOk: "Włącz",
      radioCatalogText: "Wyszukiwanie stacji korzysta ze społecznościowego katalogu stacji radio-browser.info. Podczas wyszukiwania ten komputer łączy się bezpośrednio z serwerami katalogu: widzą one adres IP tego komputera i wpisany tekst. Serwery są prowadzone przez społeczność katalogu; nie znaleźliśmy polityki prywatności ani licencji bazy danych. Aqulify wysyła tylko zapytanie (bez liczenia kliknięć, bez odtwarzanych stacji, bez innych danych). Wyszukiwanie można wyłączyć w dowolnej chwili.",
      radioResults: "Wyniki dla „{q}”",
      radioSearching: "Wyszukiwanie…",
      radioNoResults: "Brak wyników.",
      radioAddThis: "Dodaj do listy",
      radioAdded: "Dodano: {name}",
      r_catalogOff: "Wyszukiwanie stacji jest wyłączone.",
      r_query: "Wpisz tekst do wyszukania.",
      r_json: "Nie udało się odczytać odpowiedzi katalogu.",
      r_size: "Odpowiedź katalogu jest zbyt duża."
    },
    ru: {
      albums: "Альбомы", folders: "Папки", lists: "Плейлисты", queue: "Очередь", loading: "Загрузка...",
      empty: "Медиатека пуста. Музыкальные папки настраиваются на ПК (по умолчанию папка Музыка).",
      scanning: "ПК выполняет индексацию медиатеки ({n} файлов)...", noAlbums: "Нет альбомов.",
      various: "Разные исполнители", tracks: "{n} треков", play: "Воспроизведение", shuffle: "Случайно",
      playNext: "Играть следующим", playLast: "В конец очереди", addToList: "Добавить в плейлист...", cancel: "Отмена", ok: "OK",
      folderEmpty: "Эта папка пуста.", roots: "Музыкальные папки", back: "Назад",
      queueEmpty: "Очередь пуста. Запустите альбом, папку или плейлист.", clear: "Очистить", saveAsList: "Сохранить как плейлист",
      repeatOff: "Повтор: выкл", repeatAll: "Повтор: очередь", repeatOne: "Повтор: трек", shuffleOn: "Случайно: вкл",
      shuffleOff: "Случайно: выкл", moveUp: "Выше", moveDown: "Ниже", remove: "Удалить", removeFromList: "Удалить из плейлиста",
      newList: "Новый плейлист", newSmart: "Новый смарт-плейлист", listName: "Название плейлиста:", rename: "Переименовать", delete: "Удалить",
      deleteConfirm: "Удалить плейлист «{name}»?", editRules: "Правила", unavailable: "недоступно",
      smartNote: "Смарт-плейлист: ПК формирует его содержимое по правилам.", noLists: "Пока нет плейлистов.",
      chooseList: "В какой плейлист?", added: "Добавлено: {name}", queued: "Добавлено в очередь", saved: "Сохранено: {name}",
      error: "Ошибка: {e}", smartEditor: "Смарт-плейлист", name: "Название", match: "Условия", matchAll: "все должны совпадать",
      matchAny: "любое может совпадать", addCondition: "+ Условие", sort: "Сортировка по", desc: "по убыванию",
      limit: "Не более (0: все)", save: "Сохранить", days: "дней", more: "Еще",
      colTitle: "Название", colArtist: "Исполнитель", colAlbum: "Альбом", colDuration: "Длительность",
      stats: "{tracks} треков · {albums} альбомов", playAlbum: "Слушать альбом",
      f_artist: "Исполнитель", f_album: "Альбом", f_title: "Название", f_genre: "Жанр", f_year: "Год", f_duration: "Длительность (с)",
      f_plays: "Воспроизведения", f_added: "Добавлено", f_lastPlayed: "Последнее воспроизведение",
      o_contains: "содержит", o_notContains: "не содержит", o_is: "в точности", o_isNot: "не является", o_startsWith: "начинается с",
      o_eq: "=", o_ne: "≠", o_gt: ">", o_ge: "≥", o_lt: "<", o_le: "≤", o_inLast: "за последние … дней",
      o_notInLast: "не за последние … дней", o_never: "никогда",
      s_added: "Добавлено", s_lastPlayed: "Последнее воспроизведение", s_plays: "Воспроизведения", s_year: "Год", s_artist: "Исполнитель",
      s_album: "Альбом", s_title: "Название", s_duration: "Длительность", s_random: "Случайно",
      e_empty: "нечего воспроизводить", e_name: "введите название", e_unknown: "не найдено", e_smart: "в смарт-плейлист нельзя добавлять треки вручную",
      e_full: "заполнено", e_index: "неверная позиция", e_rules: "недопустимое правило",
      srcPc: "💻 Медиатека ПК", srcPhone: "📱 Музыка телефона",
      phoneEmpty: "Папки с музыкой с телефона пока не добавлены.",
      addFolder: "Добавить папку", rescan: "Пересканировать",
      scanningPhone: "Сканирование музыки телефона...",
      phoneTracks: "На телефоне найдено {n} треков",
      notSupportedHere: "Функция недоступна на этом устройстве.",
      removeFolderConfirm: "Удалить папку «{name}» из медиатеки телефона?",
      phoneTrackLocalMatch: "Найдена локальная копия на ПК: «{title}»",
      sendingTrack: "Потоковая передача трека на ПК...",
      copyToPc: "Копировать на ПК",
      copyConfirmPrompt: "Трека «{title}» еще нет в медиатеке ПК. Что вы хотите сделать?",
      optPlayOnly: "▶ Воспроизвести только сейчас",
      optCopyOnce: "📥 Скопировать на ПК",
      optCopyAlways: "💾 Всегда копировать на ПК",
      optNeverAsk: "🚫 Больше не спрашивать (только воспроизведение)",
      copiedToPc: "Трек скопирован в медиатеку ПК!",
      copyPrefTitle: "Параметры копирования на ПК",
      prefAsk: "Спрашивать для каждого нового трека",
      prefAlways: "Всегда копировать на ПК",
      prefNever: "Никогда не копировать (только воспроизведение)",
      prefSaved: "Параметр сохранен",
      onPhone: "На телефоне",
      matchedOnPc: "Локальная копия на ПК",
      phoneDisconnected: "Телефон не подключен",
      radio: "Радио",
      radioAdd: "Новая станция",
      radioName: "Название станции",
      radioUrl: "Адрес станции (http:// или https://)",
      radioEmpty: "Станций пока нет. Добавьте адрес любимых станций (на сайте станции он обычно называется «Слушать» или «Stream»; .pls и .m3u тоже подходят).",
      radioNote: "Радио играет через встроенный плеер, эквалайзер и всю аудиоцепочку. Поддерживаются: MP3, AAC (включая HE-AAC), Ogg Vorbis, Opus, FLAC; сегментированные потоки (HLS) пока нет.",
      radioPlay: "Воспроизвести",
      radioStop: "Стоп",
      radioEditUrl: "Изменить адрес",
      rs_connecting: "Подключение…",
      rs_buffering: "Буферизация…",
      rs_playing: "Играет",
      rs_reconnecting: "Повторное подключение…",
      rs_error: "Ошибка: {e}",
      r_url: "Недопустимый адрес (нужен http:// или https://).",
      r_name: "Дайте станции название.",
      r_full: "Список станций заполнен.",
      r_blocked: "Этот адрес указывает на локальную или частную сеть и не может быть открыт по соображениям безопасности.",
      r_dns: "Имя сервера не найдено.",
      r_connect: "Сервер недоступен.",
      r_timeout: "Сервер не ответил вовремя.",
      r_tls: "Не удалось установить защищённое соединение.",
      r_http: "Сервер вернул ошибку.",
      r_hls: "Это сегментированный поток (HLS), он пока не поддерживается.",
      r_html: "Этот адрес — веб-страница, а не аудиопоток.",
      r_format: "Неизвестный формат.",
      r_decode: "Поток не удаётся декодировать.",
      r_playlist: "В плейлисте нет пригодного адреса.",
      r_redirect: "Слишком много перенаправлений.",
      r_nocurl: "Для радио нужна программа curl (установите её менеджером пакетов).",
      r_formatChange: "Формат потока изменился.",
      r_eof: "Поток был прерван.",
      r_empty: "Поток пуст.",
      r_unknown: "Неизвестная станция.",
      r_output: "Не удаётся открыть аудиовыход.",
      r_remote: "Список станций можно редактировать только на компьютере (или с телефона администратора).",
      radioSearch: "Найти станцию",
      radioSearchQ: "Название станции (или его часть)",
      radioCatalogOn: "Включить поиск станций",
      radioCatalogOff: "Выключить поиск",
      radioCatalogOk: "Включить",
      radioCatalogText: "Поиск станций использует общественный каталог станций radio-browser.info. При поиске этот компьютер обращается напрямую к серверам каталога: они видят IP-адрес этого компьютера и текст запроса. Серверы поддерживает сообщество каталога; политики конфиденциальности и лицензии на базу данных мы не нашли. Aqulify отправляет только сам запрос (без подсчёта кликов, без проигранных станций и других данных). Поиск можно отключить в любой момент.",
      radioResults: "Результаты по запросу «{q}»",
      radioSearching: "Поиск…",
      radioNoResults: "Ничего не найдено.",
      radioAddThis: "Добавить в список",
      radioAdded: "Добавлено: {name}",
      r_catalogOff: "Поиск станций выключен.",
      r_query: "Введите текст для поиска.",
      r_json: "Не удалось прочитать ответ каталога.",
      r_size: "Ответ каталога слишком большой."
    },
    pt: {
      albums: "Álbuns", folders: "Pastas", lists: "Playlists", queue: "Fila", loading: "Carregando...",
      empty: "A biblioteca está vazia. As pastas de música são definidas no PC (por padrão, a pasta Músicas).",
      scanning: "O PC está indexando a biblioteca ({n} arquivos)...", noAlbums: "Nenhum álbum.",
      various: "Vários artistas", tracks: "{n} faixas", play: "Reproduzir", shuffle: "Aleatório",
      playNext: "Tocar a seguir", playLast: "Adicionar à fila", addToList: "Adicionar à playlist...", cancel: "Cancelar", ok: "OK",
      folderEmpty: "Esta pasta está vazia.", roots: "Pastas de música", back: "Voltar",
      queueEmpty: "A fila está vazia. Inicie um álbum, pasta ou playlist.", clear: "Limpar", saveAsList: "Salvar como playlist",
      repeatOff: "Repetir: desligado", repeatAll: "Repetir: fila", repeatOne: "Repetir: faixa", shuffleOn: "Aleatório: ligado",
      shuffleOff: "Aleatório: desligado", moveUp: "Mover para cima", moveDown: "Mover para baixo", remove: "Remover", removeFromList: "Remover da playlist",
      newList: "Nova playlist", newSmart: "Nova playlist inteligente", listName: "Nome da playlist:", rename: "Renomear", delete: "Excluir",
      deleteConfirm: "Excluir a playlist \"{name}\"?", editRules: "Regras", unavailable: "indisponível",
      smartNote: "Playlist inteligente: o PC calcula o conteúdo a partir das regras.", noLists: "Nenhuma playlist ainda.",
      chooseList: "Qual playlist?", added: "Adicionado: {name}", queued: "Adicionado à fila", saved: "Salvo: {name}",
      error: "Erro: {e}", smartEditor: "Playlist inteligente", name: "Nome", match: "Condições", matchAll: "todas devem coincidir",
      matchAny: "qualquer uma pode coincidir", addCondition: "+ Condição", sort: "Ordenar por", desc: "decrescente",
      limit: "No máximo (0: todas)", save: "Salvar", days: "dias", more: "Mais",
      colTitle: "Título", colArtist: "Artista", colAlbum: "Álbum", colDuration: "Duração",
      stats: "{tracks} faixas · {albums} álbuns", playAlbum: "Tocar álbum",
      f_artist: "Artista", f_album: "Álbum", f_title: "Título", f_genre: "Gênero", f_year: "Ano", f_duration: "Duração (s)",
      f_plays: "Reproduções", f_added: "Adicionado", f_lastPlayed: "Última reprodução",
      o_contains: "contém", o_notContains: "não contém", o_is: "é", o_isNot: "não é", o_startsWith: "começa com",
      o_eq: "=", o_ne: "≠", o_gt: ">", o_ge: "≥", o_lt: "<", o_le: "≤", o_inLast: "nos últimos … dias",
      o_notInLast: "não nos últimos … dias", o_never: "nunca",
      s_added: "Adicionado", s_lastPlayed: "Última reprodução", s_plays: "Reproduções", s_year: "Ano", s_artist: "Artista",
      s_album: "Álbum", s_title: "Título", s_duration: "Duração", s_random: "Aleatório",
      e_empty: "nada para reproduzir", e_name: "digite um nome", e_unknown: "não encontrado", e_smart: "não é possível adicionar faixas a uma playlist inteligente",
      e_full: "cheio", e_index: "posição inválida", e_rules: "regra inválida",
      srcPc: "💻 Biblioteca do PC", srcPhone: "📱 Músicas do Celular",
      phoneEmpty: "Nenhuma pasta de música adicionada do celular ainda.",
      addFolder: "Adicionar pasta", rescan: "Examinar novamente",
      scanningPhone: "Examinando biblioteca de músicas do celular...",
      phoneTracks: "{n} faixas encontradas no celular",
      notSupportedHere: "Este recurso não está disponível neste dispositivo.",
      removeFolderConfirm: "Remover a pasta \"{name}\" da biblioteca do celular?",
      phoneTrackLocalMatch: "Cópia local encontrada no PC: \"{title}\"",
      sendingTrack: "Transmitindo faixa para o PC...",
      copyToPc: "Copiar para o PC",
      copyConfirmPrompt: "\"{title}\" ainda não está na biblioteca do PC. O que você deseja fazer?",
      optPlayOnly: "▶ Reproduzir apenas agora",
      optCopyOnce: "📥 Copiar para o PC",
      optCopyAlways: "💾 Sempre copiar para o PC",
      optNeverAsk: "🚫 Não perguntar novamente (apenas reproduzir)",
      copiedToPc: "Faixa copiada para a biblioteca do PC!",
      copyPrefTitle: "Configurações de cópia para o PC",
      prefAsk: "Perguntar para cada nova faixa",
      prefAlways: "Sempre copiar para o PC",
      prefNever: "Nunca copiar (apenas reproduzir)",
      prefSaved: "Configuração salva",
      onPhone: "No celular",
      matchedOnPc: "Cópia local no PC",
      phoneDisconnected: "Celular não conectado",
      radio: "Rádio",
      radioAdd: "Nova estação",
      radioName: "Nome da estação",
      radioUrl: "Endereço da estação (http:// ou https://)",
      radioEmpty: "Ainda não há estações. Adicione o endereço das suas estações favoritas (no site da estação costuma chamar-se “Ouvir” ou “Stream”; .pls e .m3u também funcionam).",
      radioNote: "A rádio toca através do reprodutor integrado, do equalizador e de toda a cadeia de áudio. Suportado: MP3, AAC (incl. HE-AAC), Ogg Vorbis, Opus, FLAC; fluxos segmentados (HLS) ainda não.",
      radioPlay: "Reproduzir",
      radioStop: "Parar",
      radioEditUrl: "Alterar endereço",
      rs_connecting: "A ligar…",
      rs_buffering: "A armazenar em buffer…",
      rs_playing: "A reproduzir",
      rs_reconnecting: "A voltar a ligar…",
      rs_error: "Erro: {e}",
      r_url: "Endereço inválido (é necessário http:// ou https://).",
      r_name: "Dê um nome à estação.",
      r_full: "A lista de estações está cheia.",
      r_blocked: "Este endereço aponta para uma rede local ou privada e não pode ser aberto por razões de segurança.",
      r_dns: "Nome do servidor não encontrado.",
      r_connect: "Não é possível contactar o servidor.",
      r_timeout: "O servidor não respondeu a tempo.",
      r_tls: "Não foi possível estabelecer a ligação segura.",
      r_http: "O servidor devolveu um erro.",
      r_hls: "Este é um fluxo segmentado (HLS), que ainda não é suportado.",
      r_html: "Este endereço é uma página web, não um fluxo de áudio.",
      r_format: "Formato desconhecido.",
      r_decode: "Não é possível descodificar o fluxo.",
      r_playlist: "A lista de reprodução não tem nenhum endereço utilizável.",
      r_redirect: "Demasiados redirecionamentos.",
      r_nocurl: "A rádio precisa do programa curl (instale-o com o seu gestor de pacotes).",
      r_formatChange: "O formato do fluxo mudou.",
      r_eof: "O fluxo foi interrompido.",
      r_empty: "O fluxo está vazio.",
      r_unknown: "Estação desconhecida.",
      r_output: "Não é possível abrir a saída de áudio.",
      r_remote: "A lista de estações só pode ser editada no computador (ou a partir de um telemóvel administrador).",
      radioSearch: "Procurar uma estação",
      radioSearchQ: "Nome da estação (ou parte dele)",
      radioCatalogOn: "Ativar a pesquisa de estações",
      radioCatalogOff: "Desativar a pesquisa",
      radioCatalogOk: "Ativar",
      radioCatalogText: "A pesquisa de estações usa o catálogo comunitário de estações radio-browser.info. Ao pesquisar, este computador contacta diretamente os servidores do catálogo: estes veem o endereço IP deste computador e o texto pesquisado. Os servidores são geridos pela comunidade do catálogo; não encontrámos uma política de privacidade nem uma licença da base de dados. O Aqulify envia apenas a pesquisa (sem contagem de cliques, sem estações reproduzidas, sem outros dados). Pode desativar a pesquisa a qualquer momento.",
      radioResults: "Resultados para “{q}”",
      radioSearching: "A pesquisar…",
      radioNoResults: "Sem resultados.",
      radioAddThis: "Adicionar à lista",
      radioAdded: "Adicionada: {name}",
      r_catalogOff: "A pesquisa de estações está desativada.",
      r_query: "Introduza um texto de pesquisa.",
      r_json: "Não foi possível ler a resposta do catálogo.",
      r_size: "A resposta do catálogo é demasiado grande."
    },
    ro: {
      albums: "Albume", folders: "Foldere", lists: "Playlisturi", queue: "Coadă", loading: "Se încarcă...",
      empty: "Biblioteca este goală. Folderele de muzică sunt configurate pe PC (implicit folderul Muzică).",
      scanning: "PC-ul indexează biblioteca ({n} fișiere)...", noAlbums: "Niciun album.",
      various: "Artiști diverși", tracks: "{n} piese", play: "Redare", shuffle: "Aleatoriu",
      playNext: "Redă următoarea", playLast: "Adaugă la coadă", addToList: "Adaugă la playlist...", cancel: "Anulare", ok: "OK",
      folderEmpty: "Acest folder este gol.", roots: "Foldere de muzică", back: "Înapoi",
      queueEmpty: "Coada este goală. Porniți un album, folder sau playlist.", clear: "Golește", saveAsList: "Salvează ca playlist",
      repeatOff: "Repetare: oprită", repeatAll: "Repetare: coadă", repeatOne: "Repetare: piesă", shuffleOn: "Aleatoriu: pornit",
      shuffleOff: "Aleatoriu: oprit", moveUp: "Mută mai sus", moveDown: "Mută mai jos", remove: "Elimină", removeFromList: "Elimină din playlist",
      newList: "Playlist nou", newSmart: "Playlist inteligent nou", listName: "Nume playlist:", rename: "Redenumește", delete: "Șterge",
      deleteConfirm: "Ștergeți playlistul \"{name}\"?", editRules: "Reguli", unavailable: "indisponibil",
      smartNote: "Playlist inteligent: PC-ul calculează conținutul pe baza regulilor.", noLists: "Niciun playlist încă.",
      chooseList: "În ce playlist?", added: "Adăugat: {name}", queued: "Adăugat la coadă", saved: "Salvat: {name}",
      error: "Eroare: {e}", smartEditor: "Playlist inteligent", name: "Nume", match: "Condiții", matchAll: "toate trebuie să se potrivească",
      matchAny: "oricare se poate potrivi", addCondition: "+ Condiție", sort: "Sortează după", desc: "descrescător",
      limit: "Cel mult (0: toate)", save: "Salvează", days: "zile", more: "Mai mult",
      colTitle: "Titlu", colArtist: "Artist", colAlbum: "Album", colDuration: "Durată",
      stats: "{tracks} piese · {albums} albume", playAlbum: "Redă albumul",
      f_artist: "Artist", f_album: "Album", f_title: "Titlu", f_genre: "Gen", f_year: "An", f_duration: "Durată (s)",
      f_plays: "Redări", f_added: "Adăugat", f_lastPlayed: "Ultima redare",
      o_contains: "conține", o_notContains: "nu conține", o_is: "este", o_isNot: "nu este", o_startsWith: "începe cu",
      o_eq: "=", o_ne: "≠", o_gt: ">", o_ge: "≥", o_lt: "<", o_le: "≤", o_inLast: "în ultimele … zile",
      o_notInLast: "nu în ultimele … zile", o_never: "niciodată",
      s_added: "Adăugat", s_lastPlayed: "Ultima redare", s_plays: "Redări", s_year: "An", s_artist: "Artist",
      s_album: "Album", s_title: "Titlu", s_duration: "Durată", s_random: "Aleatoriu",
      e_empty: "nimic de redat", e_name: "introduceți un nume", e_unknown: "negăsit", e_smart: "nu se pot adăuga piese manual la un playlist inteligent",
      e_full: "plin", e_index: "poziție nevalidă", e_rules: "regulă nevalidă",
      srcPc: "💻 Biblioteca PC", srcPhone: "📱 Muzică telefon",
      phoneEmpty: "Niciun folder de muzică adăugat de pe telefon încă.",
      addFolder: "Adaugă folder", rescan: "Rescanează",
      scanningPhone: "Scanare bibliotecă muzică telefon...",
      phoneTracks: "{n} piese găsite pe telefon",
      notSupportedHere: "Această funcție nu este disponibilă pe acest dispozitiv.",
      removeFolderConfirm: "Eliminați folderul \"{name}\" din biblioteca telefonului?",
      phoneTrackLocalMatch: "Copie locală găsită pe PC: \"{title}\"",
      sendingTrack: "Streaming piesă către PC...",
      copyToPc: "Copiază pe PC",
      copyConfirmPrompt: "\"{title}\" nu se află încă în biblioteca PC-ului. Ce doriți să faceți?",
      optPlayOnly: "▶ Redă doar acum",
      optCopyOnce: "📥 Copiază pe PC",
      optCopyAlways: "💾 Copiază întotdeauna pe PC",
      optNeverAsk: "🚫 Nu mai întreba (doar redare)",
      copiedToPc: "Piesă copiată în biblioteca PC-ului!",
      copyPrefTitle: "Setări copiere pe PC",
      prefAsk: "Întreabă pentru fiecare piesă nouă",
      prefAlways: "Copiază întotdeauna pe PC",
      prefNever: "Nu copia niciodată (doar redare)",
      prefSaved: "Setare salvată",
      onPhone: "Pe telefon",
      matchedOnPc: "Copie locală pe PC",
      phoneDisconnected: "Telefon neconectat",
      radio: "Radio",
      radioAdd: "Stație nouă",
      radioName: "Numele stației",
      radioUrl: "Adresa stației (http:// sau https://)",
      radioEmpty: "Încă nu sunt stații. Adaugă adresa stațiilor tale preferate (pe site-ul stației se numește de obicei „Ascultă” sau „Stream”; funcționează și .pls și .m3u).",
      radioNote: "Radioul redă prin playerul integrat, egalizator și întregul lanț audio. Acceptate: MP3, AAC (inclusiv HE-AAC), Ogg Vorbis, Opus, FLAC; fluxurile segmentate (HLS) încă nu.",
      radioPlay: "Redă",
      radioStop: "Oprește",
      radioEditUrl: "Schimbă adresa",
      rs_connecting: "Se conectează…",
      rs_buffering: "Se încarcă în buffer…",
      rs_playing: "Se redă",
      rs_reconnecting: "Se reconectează…",
      rs_error: "Eroare: {e}",
      r_url: "Adresă nevalidă (este necesar http:// sau https://).",
      r_name: "Dă un nume stației.",
      r_full: "Lista de stații este plină.",
      r_blocked: "Această adresă indică o adresă de rețea locală sau privată și nu poate fi deschisă din motive de securitate.",
      r_dns: "Numele serverului nu a fost găsit.",
      r_connect: "Serverul nu poate fi contactat.",
      r_timeout: "Serverul nu a răspuns la timp.",
      r_tls: "Conexiunea securizată nu a putut fi stabilită.",
      r_http: "Serverul a returnat o eroare.",
      r_hls: "Acesta este un flux segmentat (HLS), care nu este încă acceptat.",
      r_html: "Această adresă este o pagină web, nu un flux audio.",
      r_format: "Format necunoscut.",
      r_decode: "Fluxul nu poate fi decodat.",
      r_playlist: "Lista de redare nu are nicio adresă utilizabilă.",
      r_redirect: "Prea multe redirecționări.",
      r_nocurl: "Radioul are nevoie de programul curl (instalează-l cu managerul de pachete).",
      r_formatChange: "Formatul fluxului s-a schimbat.",
      r_eof: "Fluxul a fost întrerupt.",
      r_empty: "Fluxul este gol.",
      r_unknown: "Stație necunoscută.",
      r_output: "Ieșirea audio nu poate fi deschisă.",
      r_remote: "Lista de stații poate fi editată doar pe calculator (sau de pe un telefon de administrator).",
      radioSearch: "Găsește o stație",
      radioSearchQ: "Numele stației (sau o parte din el)",
      radioCatalogOn: "Pornește căutarea de stații",
      radioCatalogOff: "Oprește căutarea",
      radioCatalogOk: "Pornește",
      radioCatalogText: "Căutarea de stații folosește catalogul comunitar de stații radio-browser.info. Când cauți, acest calculator contactează direct serverele catalogului: acestea văd adresa IP a acestui calculator și textul căutat. Serverele sunt administrate de comunitatea catalogului; nu am găsit o politică de confidențialitate și nici o licență pentru baza de date. Aqulify trimite doar căutarea (fără numărarea clicurilor, fără stațiile redate, fără alte date). Poți opri căutarea oricând.",
      radioResults: "Rezultate pentru „{q}”",
      radioSearching: "Se caută…",
      radioNoResults: "Niciun rezultat.",
      radioAddThis: "Adaugă în listă",
      radioAdded: "Adăugată: {name}",
      r_catalogOff: "Căutarea de stații este oprită.",
      r_query: "Introdu un text de căutat.",
      r_json: "Răspunsul catalogului nu a putut fi citit.",
      r_size: "Răspunsul catalogului este prea mare."
    },
    el: {
      albums: "Άλμπουμ", folders: "Φάκελοι", lists: "Λίστες αναπαραγωγής", queue: "Ουρά", loading: "Φόρτωση...",
      empty: "Η βιβλιοθήκη είναι κενή. Οι φάκελοι μουσικής ορίζονται στον υπολογιστή (από προεπιλογή ο φάκελος Μουσική).",
      scanning: "Ο υπολογιστής ευρετηριάζει τη βιβλιοθήκη ({n} αρχεία)...", noAlbums: "Δεν υπάρχουν άλμπουμ.",
      various: "Διάφοροι καλλιτέχνες", tracks: "{n} κομμάτια", play: "Αναπαραγωγή", shuffle: "Τυχαία σειρά",
      playNext: "Αναπαραγωγή επόμενου", playLast: "Προσθήκη στην ουρά", addToList: "Προσθήκη σε λίστα...", cancel: "Άκυρο", ok: "OK",
      folderEmpty: "Αυτός ο φάκελος είναι κενός.", roots: "Φάκελοι μουσικής", back: "Πίσω",
      queueEmpty: "Η ουρά είναι κενή. Ξεκινήστε ένα άλμπουμ, φάκελο ή λίστα.", clear: "Εκκαθάριση", saveAsList: "Αποθήκευση ως λίστα",
      repeatOff: "Επανάληψη: ανενεργή", repeatAll: "Επανάληψη: ουρά", repeatOne: "Επανάληψη: κομμάτι", shuffleOn: "Τυχαία σειρά: ενεργή",
      shuffleOff: "Τυχαία σειρά: ανενεργή", moveUp: "Μετακίνηση πάνω", moveDown: "Μετακίνηση κάτω", remove: "Αφαίρεση", removeFromList: "Αφαίρεση από τη λίστα",
      newList: "Νέα λίστα", newSmart: "Νέα έξυπνη λίστα", listName: "Όνομα λίστας:", rename: "Μετονομασία", delete: "Διαγραφή",
      deleteConfirm: "Διαγραφή της λίστας \"{name}\"?", editRules: "Κανόνες", unavailable: "μη διαθέσιμο",
      smartNote: "Έξυπνη λίστα: ο υπολογιστής υπολογίζει το περιεχόμενό της βάσει των κανόνων.", noLists: "Δεν υπάρχουν λίστες ακόμα.",
      chooseList: "Σε ποια λίστα;", added: "Προστέθηκε: {name}", queued: "Προστέθηκε στην ουρά", saved: "Αποθηκεύτηκε: {name}",
      error: "Σφάλμα: {e}", smartEditor: "Έξυπνη λίστα", name: "Όνομα", match: "Συνθήκες", matchAll: "πρέπει να ταιριάζουν όλες",
      matchAny: "μπορεί να ταιριάζει οποιαδήποτε", addCondition: "+ Συνθήκη", sort: "Ταξινόμηση κατά", desc: "φθίνουσα",
      limit: "Το πολύ (0: όλα)", save: "Αποθήκευση", days: "ημέρες", more: "Περισσότερα",
      colTitle: "Τίτλος", colArtist: "Καλλιτέχνης", colAlbum: "Άλμπουμ", colDuration: "Διάρκεια",
      stats: "{tracks} κομμάτια · {albums} άλμπουμ", playAlbum: "Αναπαραγωγή άλμπουμ",
      f_artist: "Καλλιτέχνης", f_album: "Άλμπουμ", f_title: "Τίτλος", f_genre: "Είδος", f_year: "Έτος", f_duration: "Διάρκεια (δ)",
      f_plays: "Αναπαραγωγές", f_added: "Προστέθηκε", f_lastPlayed: "Τελευταία αναπαραγωγή",
      o_contains: "περιέχει", o_notContains: "δεν περιέχει", o_is: "είναι", o_isNot: "δεν είναι", o_startsWith: "ξεκινά με",
      o_eq: "=", o_ne: "≠", o_gt: ">", o_ge: "≥", o_lt: "<", o_le: "≤", o_inLast: "τις τελευταίες … ημέρες",
      o_notInLast: "όχι τις τελευταίες … ημέρες", o_never: "ποτέ",
      s_added: "Προστέθηκε", s_lastPlayed: "Τελευταία αναπαραγωγή", s_plays: "Αναπαραγωγές", s_year: "Έτος", s_artist: "Καλλιτέχνης",
      s_album: "Άλμπουμ", s_title: "Τίτλος", s_duration: "Διάρκεια", s_random: "Τυχαία",
      e_empty: "τίποτα για αναπαραγωγή", e_name: "εισαγάγετε όνομα", e_unknown: "δεν βρέθηκε", e_smart: "δεν είναι δυνατή η προσθήκη κομματιών σε έξυπνη λίστα",
      e_full: "πλήρες", e_index: "μη έγκυρη θέση", e_rules: "μη έγκυρος κανόνας",
      srcPc: "💻 Βιβλιοθήκη PC", srcPhone: "📱 Μουσική τηλεφώνου",
      phoneEmpty: "Δεν έχει προστεθεί ακόμη φάκελος μουσικής από το τηλέφωνο.",
      addFolder: "Προσθήκη φακέλου", rescan: "Επανασάρωση",
      scanningPhone: "Σάρωση μουσικής τηλεφώνου...",
      phoneTracks: "Βρέθηκαν {n} κομμάτια στο τηλέφωνο",
      notSupportedHere: "Αυτή η λειτουργία δεν είναι διαθέσιμη σε αυτή τη συσκευή.",
      removeFolderConfirm: "Αφαίρεση του φακέλου \"{name}\" από τη βιβλιοθήκη τηλεφώνου;",
      phoneTrackLocalMatch: "Βρέθηκε τοπικό αντίγραφο στο PC: \"{title}\"",
      sendingTrack: "Μετάδοση κομματιού στο PC...",
      copyToPc: "Αντιγραφή στο PC",
      copyConfirmPrompt: "Το \"{title}\" δεν υπάρχει ακόμη στη βιβλιοθήκη του PC. Τι θέλετε να κάνετε;",
      optPlayOnly: "▶ Αναπαραγωγή μόνο τώρα",
      optCopyOnce: "📥 Αντιγραφή στο PC",
      optCopyAlways: "💾 Πάντα αντιγραφή στο PC",
      optNeverAsk: "🚫 Να μην ερωτηθώ ξανά (μόνο αναπαραγωγή)",
      copiedToPc: "Το κομμάτι αντιγράφηκε στη βιβλιοθήκη του PC!",
      copyPrefTitle: "Ρυθμίσεις αντιγραφής στο PC",
      prefAsk: "Ερώτηση για κάθε νέο κομμάτι",
      prefAlways: "Πάντα αντιγραφή στο PC",
      prefNever: "Ποτέ αντιγραφή (μόνο αναπαραγωγή)",
      prefSaved: "Η ρύθμιση αποθηκεύτηκε",
      onPhone: "Στο τηλέφωνο",
      matchedOnPc: "Τοπικό αντίγραφο στο PC",
      phoneDisconnected: "Το τηλέφωνο δεν είναι συνδεδεμένο",
      radio: "Ραδιόφωνο",
      radioAdd: "Νέος σταθμός",
      radioName: "Όνομα σταθμού",
      radioUrl: "Διεύθυνση σταθμού (http:// ή https://)",
      radioEmpty: "Δεν υπάρχουν ακόμη σταθμοί. Προσθέστε τη διεύθυνση των αγαπημένων σας σταθμών (στον ιστότοπο του σταθμού συνήθως ονομάζεται «Ακούστε» ή «Stream»· λειτουργούν και τα .pls και .m3u).",
      radioNote: "Το ραδιόφωνο παίζει μέσω του ενσωματωμένου αναπαραγωγέα, του ισοσταθμιστή και ολόκληρης της αλυσίδας ήχου. Υποστηρίζονται: MP3, AAC (συμπ. HE-AAC), Ogg Vorbis, Opus, FLAC· τμηματικές ροές (HLS) όχι ακόμη.",
      radioPlay: "Αναπαραγωγή",
      radioStop: "Διακοπή",
      radioEditUrl: "Αλλαγή διεύθυνσης",
      rs_connecting: "Σύνδεση…",
      rs_buffering: "Προσωρινή αποθήκευση…",
      rs_playing: "Αναπαράγεται",
      rs_reconnecting: "Επανασύνδεση…",
      rs_error: "Σφάλμα: {e}",
      r_url: "Μη έγκυρη διεύθυνση (απαιτείται http:// ή https://).",
      r_name: "Δώστε ένα όνομα στον σταθμό.",
      r_full: "Η λίστα σταθμών είναι γεμάτη.",
      r_blocked: "Αυτή η διεύθυνση δείχνει σε τοπικό ή ιδιωτικό δίκτυο και δεν μπορεί να ανοιχθεί για λόγους ασφαλείας.",
      r_dns: "Το όνομα του διακομιστή δεν βρέθηκε.",
      r_connect: "Ο διακομιστής δεν είναι προσβάσιμος.",
      r_timeout: "Ο διακομιστής δεν απάντησε εγκαίρως.",
      r_tls: "Δεν ήταν δυνατή η δημιουργία της ασφαλούς σύνδεσης.",
      r_http: "Ο διακομιστής επέστρεψε σφάλμα.",
      r_hls: "Αυτή είναι τμηματική ροή (HLS), η οποία δεν υποστηρίζεται ακόμη.",
      r_html: "Αυτή η διεύθυνση είναι ιστοσελίδα, όχι ροή ήχου.",
      r_format: "Άγνωστη μορφή.",
      r_decode: "Η ροή δεν μπορεί να αποκωδικοποιηθεί.",
      r_playlist: "Η λίστα αναπαραγωγής δεν έχει χρησιμοποιήσιμη διεύθυνση.",
      r_redirect: "Πάρα πολλές ανακατευθύνσεις.",
      r_nocurl: "Το ραδιόφωνο χρειάζεται το πρόγραμμα curl (εγκαταστήστε το με τον διαχειριστή πακέτων).",
      r_formatChange: "Η μορφή της ροής άλλαξε.",
      r_eof: "Η ροή διακόπηκε.",
      r_empty: "Η ροή είναι κενή.",
      r_unknown: "Άγνωστος σταθμός.",
      r_output: "Η έξοδος ήχου δεν μπορεί να ανοιχθεί.",
      r_remote: "Η λίστα σταθμών μπορεί να επεξεργαστεί μόνο στον υπολογιστή (ή από τηλέφωνο διαχειριστή).",
      radioSearch: "Εύρεση σταθμού",
      radioSearchQ: "Όνομα σταθμού (ή μέρος του)",
      radioCatalogOn: "Ενεργοποίηση αναζήτησης σταθμών",
      radioCatalogOff: "Απενεργοποίηση αναζήτησης",
      radioCatalogOk: "Ενεργοποίηση",
      radioCatalogText: "Η αναζήτηση σταθμών χρησιμοποιεί τον κοινοτικό κατάλογο σταθμών radio-browser.info. Όταν κάνετε αναζήτηση, αυτός ο υπολογιστής επικοινωνεί απευθείας με τους διακομιστές του καταλόγου: βλέπουν τη διεύθυνση IP αυτού του υπολογιστή και το κείμενο αναζήτησης. Οι διακομιστές λειτουργούν από την κοινότητα του καταλόγου· δεν βρήκαμε πολιτική απορρήτου ούτε άδεια χρήσης της βάσης δεδομένων. Το Aqulify στέλνει μόνο την αναζήτηση (χωρίς καταμέτρηση κλικ, χωρίς σταθμούς που παίχτηκαν, χωρίς άλλα δεδομένα). Μπορείτε να απενεργοποιήσετε την αναζήτηση ανά πάσα στιγμή.",
      radioResults: "Αποτελέσματα για «{q}»",
      radioSearching: "Αναζήτηση…",
      radioNoResults: "Κανένα αποτέλεσμα.",
      radioAddThis: "Προσθήκη στη λίστα",
      radioAdded: "Προστέθηκε: {name}",
      r_catalogOff: "Η αναζήτηση σταθμών είναι απενεργοποιημένη.",
      r_query: "Εισαγάγετε κείμενο αναζήτησης.",
      r_json: "Η απάντηση του καταλόγου δεν μπόρεσε να διαβαστεί.",
      r_size: "Η απάντηση του καταλόγου είναι πολύ μεγάλη."
    },
    tr: {
      albums: "Albümler", folders: "Klasörler", lists: "Çalma Listeleri", queue: "Sıra", loading: "Yükleniyor...",
      empty: "Kütüphane boş. Müzik klasörleri bilgisayardan ayarlanır (varsayılan olarak Müzik klasörü).",
      scanning: "Bilgisayar kütüphaneyi dizine ekliyor ({n} dosya)...", noAlbums: "Albüm yok.",
      various: "Çeşitli Sanatçılar", tracks: "{n} parça", play: "Oynat", shuffle: "Karışık",
      playNext: "Sonrakini çal", playLast: "Sıraya ekle", addToList: "Çalma listesine ekle...", cancel: "İptal", ok: "Tamam",
      folderEmpty: "Bu klasör boş.", roots: "Müzik klasörleri", back: "Geri",
      queueEmpty: "Sıra boş. Bir albüm, klasör veya çalma listesi başlatın.", clear: "Temizle", saveAsList: "Çalma listesi olarak kaydet",
      repeatOff: "Tekrar: kapalı", repeatAll: "Tekrar: sıra", repeatOne: "Tekrar: parça", shuffleOn: "Karışık: açık",
      shuffleOff: "Karışık: kapalı", moveUp: "Yukarı taşı", moveDown: "Aşağı taşı", remove: "Kaldır", removeFromList: "Listeden kaldır",
      newList: "Yeni çalma listesi", newSmart: "Yeni akıllı liste", listName: "Liste adı:", rename: "Yeniden adlandır", delete: "Sil",
      deleteConfirm: "\"{name}\" çalma listesi silinsin mi?", editRules: "Kurallar", unavailable: "kullanılamıyor",
      smartNote: "Akıllı liste: Bilgisayar içeriği kurallara göre otomatik hesaplar.", noLists: "Henüz çalma listesi yok.",
      chooseList: "Hangi listeye?", added: "Eklendi: {name}", queued: "Sıraya eklendi", saved: "Kaydedildi: {name}",
      error: "Hata: {e}", smartEditor: "Akıllı liste", name: "Ad", match: "Koşullar", matchAll: "tümü eşleşmeli",
      matchAny: "herhangi biri eşleşebilir", addCondition: "+ Koşul", sort: "Sırala", desc: "azalan",
      limit: "En fazla (0: tümü)", save: "Kaydet", days: "gün", more: "Daha fazla",
      colTitle: "Başlık", colArtist: "Sanatçı", colAlbum: "Albüm", colDuration: "Süre",
      stats: "{tracks} parça · {albums} albüm", playAlbum: "Albümü çal",
      f_artist: "Sanatçı", f_album: "Albüm", f_title: "Başlık", f_genre: "Tür", f_year: "Yıl", f_duration: "Süre (sn)",
      f_plays: "Çalma sayısı", f_added: "Eklendi", f_lastPlayed: "Son çalınma",
      o_contains: "içerir", o_notContains: "içermez", o_is: "şudur", o_isNot: "şu değildir", o_startsWith: "ile başlar",
      o_eq: "=", o_ne: "≠", o_gt: ">", o_ge: "≥", o_lt: "<", o_le: "≤", o_inLast: "son … gün içinde",
      o_notInLast: "son … gün içinde değil", o_never: "asla",
      s_added: "Eklendi", s_lastPlayed: "Son çalınma", s_plays: "Çalma sayısı", s_year: "Yıl", s_artist: "Sanatçı",
      s_album: "Albüm", s_title: "Başlık", s_duration: "Süre", s_random: "Rastgele",
      e_empty: "çalınacak bir şey yok", e_name: "bir ad girin", e_unknown: "bulunamadı", e_smart: "akıllı listeye manuel parça eklenemez",
      e_full: "dolu", e_index: "geçersiz konum", e_rules: "geçersiz kural",
      srcPc: "💻 PC Kütüphanesi", srcPhone: "📱 Telefon Müziği",
      phoneEmpty: "Telefondan henüz müzik klasörü eklenmedi.",
      addFolder: "Klasör ekle", rescan: "Yeniden tara",
      scanningPhone: "Telefon müzik kütüphanesi taranıyor...",
      phoneTracks: "Telefonda {n} parça bulundu",
      notSupportedHere: "Bu özellik bu cihazda desteklenmiyor.",
      removeFolderConfirm: "\"{name}\" klasörü telefon kütüphanesinden kaldırılsın mı?",
      phoneTrackLocalMatch: "PC'de yerel kopya bulundu: \"{title}\"",
      sendingTrack: "Parça bilgisayara aktarılıyor...",
      copyToPc: "PC'ye kopyala",
      copyConfirmPrompt: "\"{title}\" henüz PC kütüphanesinde yok. Ne yapmak istersiniz?",
      optPlayOnly: "▶ Yalnızca şimdi çal",
      optCopyOnce: "📥 PC'ye kopyala",
      optCopyAlways: "💾 Her zaman PC'ye kopyala",
      optNeverAsk: "🚫 Bir daha sorma (yalnızca çal)",
      copiedToPc: "Parça PC kütüphanesine kopyalandı!",
      copyPrefTitle: "PC'ye kopyalama ayarları",
      prefAsk: "Her yeni parça için sor",
      prefAlways: "Her zaman PC'ye kopyala",
      prefNever: "Asla kopyalama (yalnızca çal)",
      prefSaved: "Ayar kaydedildi",
      onPhone: "Telefonda",
      matchedOnPc: "PC'de yerel kopya",
      phoneDisconnected: "Telefon bağlı değil",
      radio: "Radyo",
      radioAdd: "Yeni istasyon",
      radioName: "İstasyon adı",
      radioUrl: "İstasyon adresi (http:// veya https://)",
      radioEmpty: "Henüz istasyon yok. Sevdiğiniz istasyonların adresini ekleyin (istasyonun web sitesinde genellikle “Dinle” veya “Stream” olarak geçer; .pls ve .m3u de çalışır).",
      radioNote: "Radyo; yerleşik oynatıcı, ekolayzır ve tüm ses zinciri üzerinden çalar. Desteklenenler: MP3, AAC (HE-AAC dahil), Ogg Vorbis, Opus, FLAC; parçalı (HLS) akışlar henüz değil.",
      radioPlay: "Oynat",
      radioStop: "Durdur",
      radioEditUrl: "Adresi değiştir",
      rs_connecting: "Bağlanıyor…",
      rs_buffering: "Arabelleğe alınıyor…",
      rs_playing: "Çalıyor",
      rs_reconnecting: "Yeniden bağlanıyor…",
      rs_error: "Hata: {e}",
      r_url: "Geçersiz adres (http:// veya https:// gerekli).",
      r_name: "İstasyona bir ad verin.",
      r_full: "İstasyon listesi dolu.",
      r_blocked: "Bu adres yerel veya özel bir ağ adresine işaret ediyor ve güvenlik nedeniyle açılamaz.",
      r_dns: "Sunucu adı bulunamadı.",
      r_connect: "Sunucuya ulaşılamıyor.",
      r_timeout: "Sunucu zamanında yanıt vermedi.",
      r_tls: "Güvenli bağlantı kurulamadı.",
      r_http: "Sunucu bir hata döndürdü.",
      r_hls: "Bu parçalı (HLS) bir akış ve henüz desteklenmiyor.",
      r_html: "Bu adres bir web sayfası, ses akışı değil.",
      r_format: "Bilinmeyen biçim.",
      r_decode: "Akışın kodu çözülemiyor.",
      r_playlist: "Oynatma listesinde kullanılabilir adres yok.",
      r_redirect: "Çok fazla yönlendirme.",
      r_nocurl: "Radyo, curl programına ihtiyaç duyar (paket yöneticinizle kurun).",
      r_formatChange: "Akış biçimi değişti.",
      r_eof: "Akış kesintiye uğradı.",
      r_empty: "Akış boş.",
      r_unknown: "Bilinmeyen istasyon.",
      r_output: "Ses çıkışı açılamıyor.",
      r_remote: "İstasyon listesi yalnızca bilgisayarda (veya bir yönetici telefondan) düzenlenebilir.",
      radioSearch: "İstasyon bul",
      radioSearchQ: "İstasyon adı (veya bir kısmı)",
      radioCatalogOn: "İstasyon aramayı aç",
      radioCatalogOff: "Aramayı kapat",
      radioCatalogOk: "Aç",
      radioCatalogText: "İstasyon arama, radio-browser.info topluluk istasyon kataloğunu kullanır. Arama yaptığınızda bu bilgisayar doğrudan kataloğun sunucularına bağlanır: sunucular bu bilgisayarın IP adresini ve arama metnini görür. Sunucuları kataloğun topluluğu işletir; veri tabanı için bir gizlilik politikası veya lisans bulamadık. Aqulify yalnızca aramayı gönderir (tık sayımı, çalınan istasyonlar veya başka veri yok). Aramayı istediğiniz zaman kapatabilirsiniz.",
      radioResults: "“{q}” için sonuçlar",
      radioSearching: "Aranıyor…",
      radioNoResults: "Sonuç yok.",
      radioAddThis: "Listeye ekle",
      radioAdded: "Eklendi: {name}",
      r_catalogOff: "İstasyon arama kapalı.",
      r_query: "Bir arama metni girin.",
      r_json: "Kataloğun yanıtı okunamadı.",
      r_size: "Kataloğun yanıtı çok büyük."
    },
    fi: {
      albums: "Albumit", folders: "Kansiot", lists: "Soittolistat", queue: "Jono", loading: "Ladataan...",
      empty: "Kirjasto on tyhjä. Musiikkikansiot määritetään tietokoneella (oletuksena Musiikki-kansio).",
      scanning: "Tietokone indeksoi kirjastoa ({n} tiedostoa)...", noAlbums: "Ei albumeita.",
      various: "Eri esittäjiä", tracks: "{n} kappaletta", play: "Toista", shuffle: "Sekoita",
      playNext: "Toista seuraavana", playLast: "Lisää jonoon", addToList: "Lisää soittolistaan...", cancel: "Peruuta", ok: "OK",
      folderEmpty: "Tämä kansio on tyhjä.", roots: "Musiikkikansiot", back: "Takaisin",
      queueEmpty: "Jono on tyhjä. Käynnistä albumi, kansio tai soittolista.", clear: "Tyhjennä", saveAsList: "Tallenna soittolistana",
      repeatOff: "Toisto: pois", repeatAll: "Toisto: jono", repeatOne: "Toisto: kappale", shuffleOn: "Sekoitus: päällä",
      shuffleOff: "Sekoitus: pois", moveUp: "Siirrä ylös", moveDown: "Siirrä alas", remove: "Poista", removeFromList: "Poista soittolistalta",
      newList: "Uusi soittolista", newSmart: "Uusi älykäs soittolista", listName: "Soittolistan nimi:", rename: "Nimeä uudelleen", delete: "Poista",
      deleteConfirm: "Poistetaanko soittolista \"{name}\"?", editRules: "Säännöt", unavailable: "ei saatavilla",
      smartNote: "Älykäs soittolista: tietokone laskee sisällön sääntöjen perusteella.", noLists: "Ei vielä soittolistoja.",
      chooseList: "Mihin soittolistaan?", added: "Lisätty: {name}", queued: "Lisätty jonoon", saved: "Tallennettu: {name}",
      error: "Virhe: {e}", smartEditor: "Älykäs soittolista", name: "Nimi", match: "Ehdot", matchAll: "kaikkien on täsmättävä",
      matchAny: "minkä tahansa voi täsmätä", addCondition: "+ Ehto", sort: "Järjestä", desc: "laskeva",
      limit: "Enintään (0: kaikki)", save: "Tallenna", days: "päivää", more: "Lisää",
      colTitle: "Nimi", colArtist: "Esittäjä", colAlbum: "Albumi", colDuration: "Kesto",
      stats: "{tracks} kappaletta · {albums} albumia", playAlbum: "Toista albumi",
      f_artist: "Esittäjä", f_album: "Albumi", f_title: "Nimi", f_genre: "Genre", f_year: "Vuosi", f_duration: "Kesto (s)",
      f_plays: "Toistokerrat", f_added: "Lisätty", f_lastPlayed: "Viimeksi toistettu",
      o_contains: "sisältää", o_notContains: "ei sisällä", o_is: "on", o_isNot: "ei ole", o_startsWith: "alkaa",
      o_eq: "=", o_ne: "≠", o_gt: ">", o_ge: "≥", o_lt: "<", o_le: "≤", o_inLast: "viimeisten … päivän aikana",
      o_notInLast: "ei viimeisten … päivän aikana", o_never: "ei koskaan",
      s_added: "Lisätty", s_lastPlayed: "Viimeksi toistettu", s_plays: "Toistokerrat", s_year: "Vuosi", s_artist: "Esittäjä",
      s_album: "Albumi", s_title: "Nimi", s_duration: "Kesto", s_random: "Satunnainen",
      e_empty: "ei toistettavaa", e_name: "anna nimi", e_unknown: "ei löydy", e_smart: "älykkääseen soittolistaan ei voi lisätä kappaleita manuaalisesti",
      e_full: "täysi", e_index: "virheellinen sijainti", e_rules: "virheellinen sääntö",
      srcPc: "💻 PC-kirjasto", srcPhone: "📱 Puhelimen musiikki",
      phoneEmpty: "Puhelimesta ei ole vielä lisätty musiikkikansioita.",
      addFolder: "Lisää kansio", rescan: "Skannaa uudelleen",
      scanningPhone: "Skannataan puhelimen musiikkikirjastoa...",
      phoneTracks: "Puhelimesta löytyi {n} kappaletta",
      notSupportedHere: "Tämä toiminto ei ole käytettävissä tällä laitteella.",
      removeFolderConfirm: "Poistetaanko kansio \"{name}\" puhelinkirjastosta?",
      phoneTrackLocalMatch: "Paikallinen kopio löytyi tietokoneelta: \"{title}\"",
      sendingTrack: "Suoratoistetaan kappaletta tietokoneelle...",
      copyToPc: "Kopioi tietokoneelle",
      copyConfirmPrompt: "Kappaletta \"{title}\" ei ole vielä tietokoneen kirjastossa. Mitä haluat tehdä?",
      optPlayOnly: "▶ Toista vain nyt",
      optCopyOnce: "📥 Kopioi tietokoneelle",
      optCopyAlways: "💾 Kopioi aina tietokoneelle",
      optNeverAsk: "🚫 Älä kysy enää (vain toisto)",
      copiedToPc: "Kappale kopioitu tietokoneen kirjastoon!",
      copyPrefTitle: "Tietokoneelle kopioinnin asetukset",
      prefAsk: "Kysy jokaisesta uudesta kappaleesta",
      prefAlways: "Kopioi aina tietokoneelle",
      prefNever: "Älä koskaan kopioi (vain toisto)",
      prefSaved: "Asetus tallennettu",
      onPhone: "Puhelimessa",
      matchedOnPc: "Paikallinen kopio tietokoneella",
      phoneDisconnected: "Puhelinta ei ole yhdistetty",
      radio: "Radio",
      radioAdd: "Uusi asema",
      radioName: "Aseman nimi",
      radioUrl: "Aseman osoite (http:// tai https://)",
      radioEmpty: "Ei vielä asemia. Lisää suosikkiasemiesi osoite (aseman verkkosivulla se on yleensä nimeltään ”Kuuntele” tai ”Stream”; myös .pls ja .m3u toimivat).",
      radioNote: "Radio toistetaan sisäänrakennetun soittimen, taajuuskorjaimen ja koko äänireitin kautta. Tuettu: MP3, AAC (myös HE-AAC), Ogg Vorbis, Opus, FLAC; segmentoituja (HLS) virtoja ei vielä.",
      radioPlay: "Toista",
      radioStop: "Pysäytä",
      radioEditUrl: "Muuta osoitetta",
      rs_connecting: "Yhdistetään…",
      rs_buffering: "Puskuroidaan…",
      rs_playing: "Toistaa",
      rs_reconnecting: "Yhdistetään uudelleen…",
      rs_error: "Virhe: {e}",
      r_url: "Virheellinen osoite (http:// tai https:// vaaditaan).",
      r_name: "Anna asemalle nimi.",
      r_full: "Asemaluettelo on täynnä.",
      r_blocked: "Tämä osoite osoittaa paikalliseen tai yksityiseen verkkoon, eikä sitä voi avata turvallisuussyistä.",
      r_dns: "Palvelimen nimeä ei löytynyt.",
      r_connect: "Palvelimeen ei saada yhteyttä.",
      r_timeout: "Palvelin ei vastannut ajoissa.",
      r_tls: "Suojattua yhteyttä ei voitu muodostaa.",
      r_http: "Palvelin palautti virheen.",
      r_hls: "Tämä on segmentoitu (HLS) virta, jota ei vielä tueta.",
      r_html: "Tämä osoite on verkkosivu, ei äänivirta.",
      r_format: "Tuntematon muoto.",
      r_decode: "Virtaa ei voi dekoodata.",
      r_playlist: "Soittolistassa ei ole käyttökelpoista osoitetta.",
      r_redirect: "Liikaa uudelleenohjauksia.",
      r_nocurl: "Radio tarvitsee curl-ohjelman (asenna se paketinhallinnalla).",
      r_formatChange: "Virran muoto muuttui.",
      r_eof: "Virta katkesi.",
      r_empty: "Virta on tyhjä.",
      r_unknown: "Tuntematon asema.",
      r_output: "Äänilähtöä ei voi avata.",
      r_remote: "Asemaluetteloa voi muokata vain tietokoneella (tai ylläpitäjän puhelimella).",
      radioSearch: "Etsi asema",
      radioSearchQ: "Aseman nimi (tai osa siitä)",
      radioCatalogOn: "Kytke aseman haku päälle",
      radioCatalogOff: "Kytke haku pois",
      radioCatalogOk: "Kytke päälle",
      radioCatalogText: "Aseman haku käyttää yhteisön ylläpitämää radio-browser.info-asemaluetteloa. Haettaessa tämä tietokone ottaa yhteyden suoraan luettelon palvelimiin: ne näkevät tämän tietokoneen IP-osoitteen ja hakutekstin. Palvelimia ylläpitää luettelon yhteisö; emme löytäneet tietosuojakäytäntöä emmekä tietokannan lisenssiä. Aqulify lähettää vain haun (ei napsautusten laskentaa, ei toistettuja asemia, ei muita tietoja). Voit kytkeä haun pois milloin tahansa.",
      radioResults: "Tulokset haulle ”{q}”",
      radioSearching: "Haetaan…",
      radioNoResults: "Ei tuloksia.",
      radioAddThis: "Lisää luetteloon",
      radioAdded: "Lisätty: {name}",
      r_catalogOff: "Aseman haku on pois päältä.",
      r_query: "Kirjoita hakuteksti.",
      r_json: "Luettelon vastausta ei voitu lukea.",
      r_size: "Luettelon vastaus on liian suuri."
    },
    da: {
      albums: "Albummer", folders: "Mapper", lists: "Playlister", queue: "Kø", loading: "Indlæser...",
      empty: "Biblioteket er tomt. Musikmapper konfigureres på pc'en (som standard mappen Musik).",
      scanning: "Pc'en indekserer biblioteket ({n} filer)...", noAlbums: "Ingen albummer.",
      various: "Forskellige kunstnere", tracks: "{n} spor", play: "Afspil", shuffle: "Bland",
      playNext: "Afspil som næste", playLast: "Føj til kø", addToList: "Føj til playliste...", cancel: "Annuller", ok: "OK",
      folderEmpty: "Denne mappe er tom.", roots: "Musikmapper", back: "Tilbage",
      queueEmpty: "Køen er tom. Start et album, en mappe eller en playliste.", clear: "Ryd", saveAsList: "Gem som playliste",
      repeatOff: "Gentag: fra", repeatAll: "Gentag: kø", repeatOne: "Gentag: spor", shuffleOn: "Bland: til",
      shuffleOff: "Bland: fra", moveUp: "Flyt op", moveDown: "Flyt ned", remove: "Fjern", removeFromList: "Fjern fra playliste",
      newList: "Ny playliste", newSmart: "Ny smart playliste", listName: "Navn på playliste:", rename: "Omdøb", delete: "Slet",
      deleteConfirm: "Slet playlisten \"{name}\"?", editRules: "Regler", unavailable: "utilgængelig",
      smartNote: "Smart playliste: Pc'en beregner indholdet baseret på regler.", noLists: "Ingen playlister endnu.",
      chooseList: "Hvilken playliste?", added: "Tilføjet: {name}", queued: "Føjet til køen", saved: "Gemt: {name}",
      error: "Fejl: {e}", smartEditor: "Smart playliste", name: "Navn", match: "Betingelser", matchAll: "alle skal matche",
      matchAny: "mindst én skal matche", addCondition: "+ Betingelse", sort: "Sorter efter", desc: "faldende",
      limit: "Højst (0: alle)", save: "Gem", days: "dage", more: "Mere",
      colTitle: "Titel", colArtist: "Kunstner", colAlbum: "Album", colDuration: "Varighed",
      stats: "{tracks} spor · {albums} albummer", playAlbum: "Afspil album",
      f_artist: "Kunstner", f_album: "Album", f_title: "Titel", f_genre: "Genre", f_year: "År", f_duration: "Varighed (s)",
      f_plays: "Afspilninger", f_added: "Tilføjet", f_lastPlayed: "Sidst afspillet",
      o_contains: "indeholder", o_notContains: "indeholder ikke", o_is: "er", o_isNot: "er ikke", o_startsWith: "begynder med",
      o_eq: "=", o_ne: "≠", o_gt: ">", o_ge: "≥", o_lt: "<", o_le: "≤", o_inLast: "i de seneste … dage",
      o_notInLast: "ikke i de seneste … dage", o_never: "aldrig",
      s_added: "Tilføjet", s_lastPlayed: "Sidst afspillet", s_plays: "Afspilninger", s_year: "År", s_artist: "Kunstner",
      s_album: "Album", s_title: "Titel", s_duration: "Varighed", s_random: "Tilfældig",
      e_empty: "intet at afspille", e_name: "indtast et navn", e_unknown: "ikke fundet", e_smart: "kan ikke tilføje spor manuelt til en smart playliste",
      e_full: "fuld", e_index: "ugyldig position", e_rules: "ugyldig regel",
      srcPc: "💻 Pc-bibliotek", srcPhone: "📱 Telefonmusik",
      phoneEmpty: "Ingen musikmappe tilføjet fra telefonen endnu.",
      addFolder: "Tilføj mappe", rescan: "Genscan",
      scanningPhone: "Scanner telefonens musikbibliotek...",
      phoneTracks: "{n} spor fundet på telefonen",
      notSupportedHere: "Denne funktion understøttes ikke på denne enhed.",
      removeFolderConfirm: "Fjern mappen \"{name}\" fra telefonens bibliotek?",
      phoneTrackLocalMatch: "Lokal kopi fundet på pc: \"{title}\"",
      sendingTrack: "Overfører spor til pc...",
      copyToPc: "Kopier til pc",
      copyConfirmPrompt: "\"{title}\" er ikke i pc-biblioteket endnu. Hvad vil du gøre?",
      optPlayOnly: "▶ Afspil kun nu",
      optCopyOnce: "📥 Kopier til pc",
      optCopyAlways: "💾 Kopier altid til pc",
      optNeverAsk: "🚫 Spørg ikke igen (kun afspil)",
      copiedToPc: "Sporet blev kopieret til pc-biblioteket!",
      copyPrefTitle: "Indstillinger for kopiering til pc",
      prefAsk: "Spørg for hvert nyt spor",
      prefAlways: "Kopier altid til pc",
      prefNever: "Kopier aldrig (kun afspil)",
      prefSaved: "Indstilling gemt",
      onPhone: "På telefon",
      matchedOnPc: "Lokal kopi på pc",
      phoneDisconnected: "Telefon ikke tilsluttet",
      radio: "Radio",
      radioAdd: "Ny station",
      radioName: "Stationsnavn",
      radioUrl: "Stationsadresse (http:// eller https://)",
      radioEmpty: "Ingen stationer endnu. Tilføj adressen på dine yndlingsstationer (på stationens websted hedder den som regel “Lyt” eller “Stream”; .pls og .m3u virker også).",
      radioNote: "Radioen afspilles gennem den indbyggede afspiller, equalizeren og hele lydkæden. Understøttet: MP3, AAC (inkl. HE-AAC), Ogg Vorbis, Opus, FLAC; segmenterede (HLS) streams endnu ikke.",
      radioPlay: "Afspil",
      radioStop: "Stop",
      radioEditUrl: "Skift adresse",
      rs_connecting: "Opretter forbindelse…",
      rs_buffering: "Bufferer…",
      rs_playing: "Afspiller",
      rs_reconnecting: "Genopretter forbindelse…",
      rs_error: "Fejl: {e}",
      r_url: "Ugyldig adresse (http:// eller https:// kræves).",
      r_name: "Giv stationen et navn.",
      r_full: "Stationslisten er fuld.",
      r_blocked: "Denne adresse peger på en lokal eller privat netværksadresse og kan ikke åbnes af sikkerhedshensyn.",
      r_dns: "Servernavnet blev ikke fundet.",
      r_connect: "Serveren kan ikke nås.",
      r_timeout: "Serveren svarede ikke i tide.",
      r_tls: "Den sikre forbindelse kunne ikke oprettes.",
      r_http: "Serveren returnerede en fejl.",
      r_hls: "Dette er en segmenteret (HLS) stream, som endnu ikke understøttes.",
      r_html: "Denne adresse er en webside, ikke en lydstream.",
      r_format: "Ukendt format.",
      r_decode: "Streamen kan ikke dekodes.",
      r_playlist: "Afspilningslisten har ingen brugbar adresse.",
      r_redirect: "For mange omdirigeringer.",
      r_nocurl: "Radioen kræver programmet curl (installer det med din pakkehåndtering).",
      r_formatChange: "Streamens format blev ændret.",
      r_eof: "Streamen blev afbrudt.",
      r_empty: "Streamen er tom.",
      r_unknown: "Ukendt station.",
      r_output: "Lydudgangen kan ikke åbnes.",
      r_remote: "Stationslisten kan kun redigeres på computeren (eller fra en administratortelefon).",
      radioSearch: "Find en station",
      radioSearchQ: "Stationsnavn (eller en del af det)",
      radioCatalogOn: "Slå stationssøgning til",
      radioCatalogOff: "Slå søgning fra",
      radioCatalogOk: "Slå til",
      radioCatalogText: "Stationssøgningen bruger fællesskabets stationskatalog radio-browser.info. Når du søger, kontakter denne computer katalogets servere direkte: de kan se denne computers IP-adresse og søgeteksten. Serverne drives af katalogets fællesskab; vi kunne ikke finde en privatlivspolitik eller en licens til databasen. Aqulify sender kun søgningen (ingen klikoptælling, ingen afspillede stationer, ingen andre data). Du kan slå søgningen fra når som helst.",
      radioResults: "Resultater for “{q}”",
      radioSearching: "Søger…",
      radioNoResults: "Ingen resultater.",
      radioAddThis: "Føj til listen",
      radioAdded: "Tilføjet: {name}",
      r_catalogOff: "Stationssøgning er slået fra.",
      r_query: "Indtast en søgetekst.",
      r_json: "Katalogets svar kunne ikke læses.",
      r_size: "Katalogets svar er for stort."
    },
    nb: {
      albums: "Albumer", folders: "Mapper", lists: "Spillelister", queue: "Kø", loading: "Laster inn...",
      empty: "Biblioteket er tomt. Musikkmapper konfigureres på PC-en (som standard Musikk-mappen).",
      scanning: "PC-en indekserer biblioteket ({n} filer)...", noAlbums: "Ingen albumer.",
      various: "Forskjellige artister", tracks: "{n} spor", play: "Spill av", shuffle: "Bland",
      playNext: "Spill som neste", playLast: "Legg til i kø", addToList: "Legg til i spilleliste...", cancel: "Avbryt", ok: "OK",
      folderEmpty: "Denne mappen er tom.", roots: "Musikkmapper", back: "Tilbake",
      queueEmpty: "Køen er tom. Start et album, en mappe eller en spilleliste.", clear: "Tøm", saveAsList: "Lagre som spilleliste",
      repeatOff: "Gjenta: av", repeatAll: "Gjenta: kø", repeatOne: "Gjenta: spor", shuffleOn: "Bland: på",
      shuffleOff: "Bland: av", moveUp: "Flytt opp", moveDown: "Flytt ned", remove: "Fjern", removeFromList: "Fjern fra spilleliste",
      newList: "Ny spilleliste", newSmart: "Ny smart spilleliste", listName: "Spillelistenavn:", rename: "Gi nytt navn", delete: "Slett",
      deleteConfirm: "Slett spillelisten \"{name}\"?", editRules: "Regler", unavailable: "utilgjengelig",
      smartNote: "Smart spilleliste: PC-en beregner innholdet basert på regler.", noLists: "Ingen spillelister ennå.",
      chooseList: "Hvilken spilleliste?", added: "Lagt til: {name}", queued: "Lagt til i køen", saved: "Lagret: {name}",
      error: "Feil: {e}", smartEditor: "Smart spilleliste", name: "Navn", match: "Betingelser", matchAll: "alle må samsvare",
      matchAny: "minst én kan samsvare", addCondition: "+ Betingelse", sort: "Sorter etter", desc: "synkende",
      limit: "Maks (0: alle)", save: "Lagre", days: "dager", more: "Mer",
      colTitle: "Tittel", colArtist: "Artist", colAlbum: "Album", colDuration: "Varighet",
      stats: "{tracks} spor · {albums} albumer", playAlbum: "Spill album",
      f_artist: "Artist", f_album: "Album", f_title: "Titel", f_genre: "Sjanger", f_year: "År", f_duration: "Varighet (s)",
      f_plays: "Avspillinger", f_added: "Lagt til", f_lastPlayed: "Sist spilt",
      o_contains: "inneholder", o_notContains: "inneholder ikke", o_is: "er", o_isNot: "er ikke", o_startsWith: "starter med",
      o_eq: "=", o_ne: "≠", o_gt: ">", o_ge: "≥", o_lt: "<", o_le: "≤", o_inLast: "i løpet av de siste … dagene",
      o_notInLast: "ikke i løpet av de siste … dagene", o_never: "aldri",
      s_added: "Lagt til", s_lastPlayed: "Sist spilt", s_plays: "Avspillinger", s_year: "År", s_artist: "Artist",
      s_album: "Album", s_title: "Tittel", s_duration: "Varighet", s_random: "Tilfeldig",
      e_empty: "ingenting å spille", e_name: "skriv inn et navn", e_unknown: "ikke funnet", e_smart: "kan ikke legge til spor manuelt i en smart spilleliste",
      e_full: "full", e_index: "ugyldig posisjon", e_rules: "ugyldig regel",
      srcPc: "💻 PC-bibliotek", srcPhone: "📱 Telefonmusikk",
      phoneEmpty: "Ingen musikkmappe lagt til fra telefonen ennå.",
      addFolder: "Legg til mappe", rescan: "Skann på nytt",
      scanningPhone: "Skanner musikkbibliotek på telefon...",
      phoneTracks: "{n} spor funnet på telefonen",
      notSupportedHere: "Denne funksjonen støttes ikke på denne enheten.",
      removeFolderConfirm: "Fjern mappen \"{name}\" fra telefonens bibliotek?",
      phoneTrackLocalMatch: "Lokal kopi funnet på PC: \"{title}\"",
      sendingTrack: "Overfører spor til PC...",
      copyToPc: "Kopier til PC",
      copyConfirmPrompt: "\"{title}\" er ikke i PC-biblioteket ennå. Hva vil du gjøre?",
      optPlayOnly: "▶ Bare spill nå",
      optCopyOnce: "📥 Kopier til PC",
      optCopyAlways: "💾 Alltid kopier til PC",
      optNeverAsk: "🚫 Ikke spør igjen (bare spill)",
      copiedToPc: "Sporet ble kopiert til PC-biblioteket!",
      copyPrefTitle: "Innstillinger for kopiering til PC",
      prefAsk: "Spør for hvert nye spor",
      prefAlways: "Alltid kopier til PC",
      prefNever: "Aldri kopier (bare spill)",
      prefSaved: "Innstilling lagret",
      onPhone: "På telefonen",
      matchedOnPc: "Lokal kopi på PC",
      phoneDisconnected: "Telefon ikke tilkoblet",
      radio: "Radio",
      radioAdd: "Ny stasjon",
      radioName: "Stasjonsnavn",
      radioUrl: "Stasjonsadresse (http:// eller https://)",
      radioEmpty: "Ingen stasjoner ennå. Legg til adressen til favorittstasjonene dine (på stasjonens nettsted heter den vanligvis «Lytt» eller «Stream»; .pls og .m3u fungerer også).",
      radioNote: "Radioen spilles gjennom den innebygde spilleren, equalizeren og hele lydkjeden. Støttet: MP3, AAC (inkl. HE-AAC), Ogg Vorbis, Opus, FLAC; segmenterte (HLS) strømmer ennå ikke.",
      radioPlay: "Spill av",
      radioStop: "Stopp",
      radioEditUrl: "Endre adresse",
      rs_connecting: "Kobler til…",
      rs_buffering: "Bufrer…",
      rs_playing: "Spiller",
      rs_reconnecting: "Kobler til på nytt…",
      rs_error: "Feil: {e}",
      r_url: "Ugyldig adresse (http:// eller https:// kreves).",
      r_name: "Gi stasjonen et navn.",
      r_full: "Stasjonslisten er full.",
      r_blocked: "Denne adressen peker til en lokal eller privat nettverksadresse og kan ikke åpnes av sikkerhetshensyn.",
      r_dns: "Servernavnet ble ikke funnet.",
      r_connect: "Serveren kan ikke nås.",
      r_timeout: "Serveren svarte ikke i tide.",
      r_tls: "Den sikre tilkoblingen kunne ikke opprettes.",
      r_http: "Serveren returnerte en feil.",
      r_hls: "Dette er en segmentert (HLS) strøm, som ennå ikke støttes.",
      r_html: "Denne adressen er en nettside, ikke en lydstrøm.",
      r_format: "Ukjent format.",
      r_decode: "Strømmen kan ikke dekodes.",
      r_playlist: "Spillelisten har ingen brukbar adresse.",
      r_redirect: "For mange omdirigeringer.",
      r_nocurl: "Radioen trenger programmet curl (installer det med pakkebehandleren din).",
      r_formatChange: "Strømformatet endret seg.",
      r_eof: "Strømmen ble avbrutt.",
      r_empty: "Strømmen er tom.",
      r_unknown: "Ukjent stasjon.",
      r_output: "Lydutgangen kan ikke åpnes.",
      r_remote: "Stasjonslisten kan bare redigeres på datamaskinen (eller fra en administratortelefon).",
      radioSearch: "Finn en stasjon",
      radioSearchQ: "Stasjonsnavn (eller en del av det)",
      radioCatalogOn: "Slå på stasjonssøk",
      radioCatalogOff: "Slå av søk",
      radioCatalogOk: "Slå på",
      radioCatalogText: "Stasjonssøket bruker fellesskapets stasjonskatalog radio-browser.info. Når du søker, kontakter denne datamaskinen katalogens servere direkte: de ser IP-adressen til denne datamaskinen og søketeksten. Serverne drives av katalogens fellesskap; vi fant verken personvernerklæring eller lisens for databasen. Aqulify sender bare søket (ingen klikktelling, ingen avspilte stasjoner, ingen andre data). Du kan slå av søket når som helst.",
      radioResults: "Resultater for «{q}»",
      radioSearching: "Søker…",
      radioNoResults: "Ingen resultater.",
      radioAddThis: "Legg til i listen",
      radioAdded: "Lagt til: {name}",
      r_catalogOff: "Stasjonssøket er slått av.",
      r_query: "Skriv inn en søketekst.",
      r_json: "Katalogens svar kunne ikke leses.",
      r_size: "Katalogens svar er for stort."
    },
    sv: {
      albums: "Album", folders: "Mappar", lists: "Spellistor", queue: "Kö", loading: "Läser in...",
      empty: "Biblioteket är tomt. Musikmappar konfigureras på datorn (som standard mappen Musik).",
      scanning: "Datorn indexerar biblioteket ({n} filer)...", noAlbums: "Inga album.",
      various: "Blandade artister", tracks: "{n} spår", play: "Spela", shuffle: "Blanda",
      playNext: "Spela som nästa", playLast: "Lägg till i kö", addToList: "Lägg till i spellista...", cancel: "Avbryt", ok: "OK",
      folderEmpty: "Denna mapp är tom.", roots: "Musikmappar", back: "Tillbaka",
      queueEmpty: "Kön är tom. Starta ett album, en mapp eller en spellista.", clear: "Rensa", saveAsList: "Spara som spellista",
      repeatOff: "Repetera: av", repeatAll: "Repetera: kö", repeatOne: "Repetera: spår", shuffleOn: "Blanda: på",
      shuffleOff: "Blanda: av", moveUp: "Flytta upp", moveDown: "Flytta ned", remove: "Ta bort", removeFromList: "Ta bort från spellista",
      newList: "Ny spellista", newSmart: "Ny smart spellista", listName: "Spellistans namn:", rename: "Byt namn", delete: "Ta bort",
      deleteConfirm: "Ta bort spellistan \"{name}\"?", editRules: "Regler", unavailable: "otillgänglig",
      smartNote: "Smart spellista: Datorn beräknar innehållet utifrån regler.", noLists: "Inga spellistor ännu.",
      chooseList: "Vilken spellista?", added: "Tillagd: {name}", queued: "Tillagd i kön", saved: "Sparad: {name}",
      error: "Fel: {e}", smartEditor: "Smart spellista", name: "Namn", match: "Villkor", matchAll: "alla måste matcha",
      matchAny: "något kan matcha", addCondition: "+ Villkor", sort: "Sortera efter", desc: "fallande",
      limit: "Högst (0: alla)", save: "Spara", days: "dagar", more: "Mer",
      colTitle: "Titel", colArtist: "Artist", colAlbum: "Album", colDuration: "Längd",
      stats: "{tracks} spår · {albums} album", playAlbum: "Spela album",
      f_artist: "Artist", f_album: "Album", f_title: "Titel", f_genre: "Genre", f_year: "År", f_duration: "Längd (s)",
      f_plays: "Spelningar", f_added: "Tillagd", f_lastPlayed: "Senast spelad",
      o_contains: "innehåller", o_notContains: "innehåller inte", o_is: "är", o_isNot: "är inte", o_startsWith: "börjar med",
      o_eq: "=", o_ne: "≠", o_gt: ">", o_ge: "≥", o_lt: "<", o_le: "≤", o_inLast: "under de senaste … dagarna",
      o_notInLast: "inte under de senaste … dagarna", o_never: "aldrig",
      s_added: "Tillagd", s_lastPlayed: "Senast spelad", s_plays: "Spelningar", s_year: "År", s_artist: "Artist",
      s_album: "Album", s_title: "Titel", s_duration: "Längd", s_random: "Slumpmässig",
      e_empty: "inget att spela", e_name: "ange ett namn", e_unknown: "hittades inte", e_smart: "kan inte lägga till spår manuellt i en smart spellista",
      e_full: "full", e_index: "ogiltig position", e_rules: "ogiltig regel",
      srcPc: "💻 Datorbibliotek", srcPhone: "📱 Telefonmusik",
      phoneEmpty: "Ingen musikmapp har lagts till från telefonen ännu.",
      addFolder: "Lägg till mapp", rescan: "Sök igen",
      scanningPhone: "Genomsöker musikbibliotek på telefon...",
      phoneTracks: "{n} spår hittades på telefonen",
      notSupportedHere: "Denna funktion stöds inte på denna enhet.",
      removeFolderConfirm: "Ta bort mappen \"{name}\" från telefonens bibliotek?",
      phoneTrackLocalMatch: "Lokal kopia hittades på datorn: \"{title}\"",
      sendingTrack: "Överför spår till datorn...",
      copyToPc: "Kopiera till datorn",
      copyConfirmPrompt: "\"{title}\" finns inte i datorns bibliotek ännu. Vad vill du göra?",
      optPlayOnly: "▶ Spela bara nu",
      optCopyOnce: "📥 Kopiera till datorn",
      optCopyAlways: "💾 Kopiera alltid till datorn",
      optNeverAsk: "🚫 Fråga inte igen (spela bara)",
      copiedToPc: "Spåret kopierades till datorns bibliotek!",
      copyPrefTitle: "Inställningar för kopiering till datorn",
      prefAsk: "Fråga för varje nytt spår",
      prefAlways: "Kopiera alltid till datorn",
      prefNever: "Kopiera aldrig (spela bara)",
      prefSaved: "Inställning sparad",
      onPhone: "På telefonen",
      matchedOnPc: "Lokal kopia på datorn",
      phoneDisconnected: "Telefon ej ansluten",
      radio: "Radio",
      radioAdd: "Ny station",
      radioName: "Stationens namn",
      radioUrl: "Stationens adress (http:// eller https://)",
      radioEmpty: "Inga stationer än. Lägg till adressen till dina favoritstationer (på stationens webbplats heter den oftast ”Lyssna” eller ”Stream”; .pls och .m3u fungerar också).",
      radioNote: "Radion spelas via den inbyggda spelaren, equalizern och hela ljudkedjan. Stöds: MP3, AAC (inkl. HE-AAC), Ogg Vorbis, Opus, FLAC; segmenterade (HLS) strömmar ännu inte.",
      radioPlay: "Spela",
      radioStop: "Stoppa",
      radioEditUrl: "Ändra adress",
      rs_connecting: "Ansluter…",
      rs_buffering: "Buffrar…",
      rs_playing: "Spelar",
      rs_reconnecting: "Ansluter igen…",
      rs_error: "Fel: {e}",
      r_url: "Ogiltig adress (http:// eller https:// krävs).",
      r_name: "Ge stationen ett namn.",
      r_full: "Stationslistan är full.",
      r_blocked: "Den här adressen pekar på en lokal eller privat nätverksadress och kan inte öppnas av säkerhetsskäl.",
      r_dns: "Servernamnet hittades inte.",
      r_connect: "Servern kan inte nås.",
      r_timeout: "Servern svarade inte i tid.",
      r_tls: "Den säkra anslutningen kunde inte upprättas.",
      r_http: "Servern returnerade ett fel.",
      r_hls: "Det här är en segmenterad (HLS) ström, som ännu inte stöds.",
      r_html: "Den här adressen är en webbsida, inte en ljudström.",
      r_format: "Okänt format.",
      r_decode: "Strömmen kan inte avkodas.",
      r_playlist: "Spellistan har ingen användbar adress.",
      r_redirect: "För många omdirigeringar.",
      r_nocurl: "Radion behöver programmet curl (installera det med din pakethanterare).",
      r_formatChange: "Strömmens format ändrades.",
      r_eof: "Strömmen avbröts.",
      r_empty: "Strömmen är tom.",
      r_unknown: "Okänd station.",
      r_output: "Ljudutgången kan inte öppnas.",
      r_remote: "Stationslistan kan bara redigeras på datorn (eller från en administratörstelefon).",
      radioSearch: "Hitta en station",
      radioSearchQ: "Stationens namn (eller en del av det)",
      radioCatalogOn: "Slå på stationssökning",
      radioCatalogOff: "Stäng av sökning",
      radioCatalogOk: "Slå på",
      radioCatalogText: "Stationssökningen använder gemenskapens stationskatalog radio-browser.info. När du söker kontaktar den här datorn katalogens servrar direkt: de ser den här datorns IP-adress och söktexten. Servrarna drivs av katalogens gemenskap; vi hittade varken någon integritetspolicy eller någon licens för databasen. Aqulify skickar bara sökningen (ingen klickräkning, inga spelade stationer, inga andra data). Du kan stänga av sökningen när som helst.",
      radioResults: "Resultat för ”{q}”",
      radioSearching: "Söker…",
      radioNoResults: "Inga resultat.",
      radioAddThis: "Lägg till i listan",
      radioAdded: "Tillagd: {name}",
      r_catalogOff: "Stationssökningen är avstängd.",
      r_query: "Ange en söktext.",
      r_json: "Katalogens svar kunde inte läsas.",
      r_size: "Katalogens svar är för stort."
    },
    he: {
      albums: "אלבומים", folders: "תיקיות", lists: "פלייליסטים", queue: "תור", loading: "טוען...",
      empty: "הספרייה ריקה. תיקיות מוזיקה מוגדרות במחשב (כברירת מחדל תיקיית מוזיקה).",
      scanning: "המחשב יוצר אינדקס לספרייה ({n} קבצים)...", noAlbums: "אין אלבומים.",
      various: "אמנים שונים", tracks: "{n} רצועות", play: "נגן", shuffle: "ערבב",
      playNext: "נגן הבא", playLast: "הוסף לתור", addToList: "הוסף לפלייליסט...", cancel: "ביטול", ok: "אישור",
      folderEmpty: "תיקייה זו ריקה.", roots: "תיקיות מוזיקה", back: "חזרה",
      queueEmpty: "התור ריק. התחל אלבום, תיקייה או פלייליסט.", clear: "נקה", saveAsList: "שמור כפלייליסט",
      repeatOff: "חזרה: כבויה", repeatAll: "חזרה: תור", repeatOne: "חזרה: רצועה", shuffleOn: "ערבוב: פועל",
      shuffleOff: "ערבוב: כבוי", moveUp: "הזז למעלה", moveDown: "הזז למטה", remove: "הסר", removeFromList: "הסר מהפלייליסט",
      newList: "פלייליסט חדש", newSmart: "פלייליסט חכם חדש", listName: "שם הפלייליסט:", rename: "שנה שם", delete: "מחק",
      deleteConfirm: "למחוק את הפלייליסט \"{name}\"?", editRules: "כללים", unavailable: "לא זמין",
      smartNote: "פלייליסט חכם: המחשב מחשב את התוכן על בסיס הכללים.", noLists: "אין עדיין פלייליסטים.",
      chooseList: "לאיזה פלייליסט?", added: "נוסף: {name}", queued: "נוסף לתור", saved: "נשמר: {name}",
      error: "שגיאה: {e}", smartEditor: "פלייליסט חכם", name: "שם", match: "תנאים", matchAll: "כולם חייבים להתאים",
      matchAny: "לפחות אחד יכול להתאים", addCondition: "+ תנאי", sort: "מיין לפי", desc: "סדר יורד",
      limit: "לכל היותר (0: הכל)", save: "שמור", days: "ימים", more: "עוד",
      colTitle: "כותרת", colArtist: "אמן", colAlbum: "אלבום", colDuration: "משך",
      stats: "{tracks} רצועות · {albums} אלבומים", playAlbum: "נגן אלבום",
      f_artist: "אמן", f_album: "אלבום", f_title: "כותרת", f_genre: "ז'אנר", f_year: "שנה", f_duration: "משך (שניות)",
      f_plays: "השמעות", f_added: "נוסף", f_lastPlayed: "הושמע לאחרונה",
      o_contains: "מכיל", o_notContains: "אינו מכיל", o_is: "הוא", o_isNot: "אינו", o_startsWith: "מתחיל ב",
      o_eq: "=", o_ne: "≠", o_gt: ">", o_ge: "≥", o_lt: "<", o_le: "≤", o_inLast: "במהלך … הימים האחרונים",
      o_notInLast: "לא במהלך … הימים האחרונים", o_never: "מעולם לא",
      s_added: "נוסף", s_lastPlayed: "הושמע לאחרונה", s_plays: "השמעות", s_year: "שנה", s_artist: "אמן",
      s_album: "אלבום", s_title: "כותרת", s_duration: "משך", s_random: "אקראי",
      e_empty: "אין מה לנגן", e_name: "הזן שם", e_unknown: "לא נמצא", e_smart: "לא ניתן להוסיף רצועות ידנית לפלייליסט חכם",
      e_full: "מלא", e_index: "מיקום לא חוקי", e_rules: "כלל לא חוקי",
      srcPc: "💻 ספריית מחשב", srcPhone: "📱 מוזיקת טלפון",
      phoneEmpty: "לא נוספה עדיין תיקיית מוזיקה מהטלפון.",
      addFolder: "הוסף תיקייה", rescan: "סרוק מחדש",
      scanningPhone: "סורק ספריית מוזיקה בטלפון...",
      phoneTracks: "{n} רצועות נמצאו בטלפון",
      notSupportedHere: "תכונה זו אינה נתמכת במכשיר זה.",
      removeFolderConfirm: "להסיר את התיקייה \"{name}\" מספריית הטלפון?",
      phoneTrackLocalMatch: "עותק מקומי נמצא במחשב: \"{title}\"",
      sendingTrack: "מעביר רצועה למחשב...",
      copyToPc: "העתק למחשב",
      copyConfirmPrompt: "\"{title}\" עדיין אינו בספריית המחשב. מה ברצונך לעשות?",
      optPlayOnly: "▶ נגן עכשיו בלבד",
      optCopyOnce: "📥 העתק למחשב",
      optCopyAlways: "💾 העתק תמיד למחשב",
      optNeverAsk: "🚫 אל תשאל שוב (נגן בלבד)",
      copiedToPc: "הרצועה הועתקה לספריית המחשב!",
      copyPrefTitle: "הגדרות העתקה למחשב",
      prefAsk: "שאל עבור כל רצועה חדשה",
      prefAlways: "העתק תמיד למחשב",
      prefNever: "לעולם אל תעתיק (נגן בלבד)",
      prefSaved: "ההגדרה נשמרה",
      onPhone: "בטלפון",
      matchedOnPc: "עותק מקומי במחשב",
      phoneDisconnected: "הטלפון אינו מחובר",
      radio: "רדיו",
      radioAdd: "תחנה חדשה",
      radioName: "שם התחנה",
      radioUrl: "כתובת התחנה (http:// או https://)",
      radioEmpty: "עדיין אין תחנות. הוסף את כתובת התחנות האהובות עליך (באתר התחנה היא נקראת בדרך כלל \"האזנה\" או \"Stream\"; גם .pls ו-.m3u עובדים).",
      radioNote: "הרדיו מושמע דרך הנגן המובנה, האקולייזר וכל שרשרת השמע. נתמכים: MP3, ‏AAC (כולל HE-AAC), ‏Ogg Vorbis, ‏Opus, ‏FLAC; זרמים מקוטעים (HLS) עדיין לא.",
      radioPlay: "נגן",
      radioStop: "עצור",
      radioEditUrl: "שנה כתובת",
      rs_connecting: "מתחבר…",
      rs_buffering: "מאחסן בחוצץ…",
      rs_playing: "מנגן",
      rs_reconnecting: "מתחבר מחדש…",
      rs_error: "שגיאה: {e}",
      r_url: "כתובת לא חוקית (נדרש http:// או https://).",
      r_name: "תן שם לתחנה.",
      r_full: "רשימת התחנות מלאה.",
      r_blocked: "כתובת זו מצביעה על כתובת ברשת מקומית או פרטית ולא ניתן לפתוח אותה מטעמי אבטחה.",
      r_dns: "שם השרת לא נמצא.",
      r_connect: "לא ניתן להגיע לשרת.",
      r_timeout: "השרת לא ענה בזמן.",
      r_tls: "לא ניתן היה ליצור את החיבור המאובטח.",
      r_http: "השרת החזיר שגיאה.",
      r_hls: "זהו זרם מקוטע (HLS), שעדיין אינו נתמך.",
      r_html: "כתובת זו היא דף אינטרנט, לא זרם שמע.",
      r_format: "פורמט לא מוכר.",
      r_decode: "לא ניתן לפענח את הזרם.",
      r_playlist: "ברשימת ההשמעה אין כתובת שימושית.",
      r_redirect: "יותר מדי הפניות.",
      r_nocurl: "הרדיו דורש את התוכנה curl (התקן אותה באמצעות מנהל החבילות).",
      r_formatChange: "פורמט הזרם השתנה.",
      r_eof: "הזרם נקטע.",
      r_empty: "הזרם ריק.",
      r_unknown: "תחנה לא מוכרת.",
      r_output: "לא ניתן לפתוח את יציאת השמע.",
      r_remote: "את רשימת התחנות אפשר לערוך רק במחשב (או מטלפון מנהל).",
      radioSearch: "חיפוש תחנה",
      radioSearchQ: "שם התחנה (או חלק ממנו)",
      radioCatalogOn: "הפעל חיפוש תחנות",
      radioCatalogOff: "כבה חיפוש",
      radioCatalogOk: "הפעל",
      radioCatalogText: "חיפוש התחנות משתמש בקטלוג התחנות הקהילתי radio-browser.info. בעת חיפוש, המחשב הזה פונה ישירות לשרתי הקטלוג: הם רואים את כתובת ה-IP של המחשב ואת טקסט החיפוש. השרתים מופעלים על ידי קהילת הקטלוג; לא מצאנו מדיניות פרטיות או רישיון למסד הנתונים. Aqulify שולח רק את החיפוש (ללא ספירת לחיצות, ללא תחנות שהושמעו, ללא נתונים אחרים). אפשר לכבות את החיפוש בכל עת.",
      radioResults: "תוצאות עבור \"{q}\"",
      radioSearching: "מחפש…",
      radioNoResults: "אין תוצאות.",
      radioAddThis: "הוסף לרשימה",
      radioAdded: "נוספה: {name}",
      r_catalogOff: "חיפוש התחנות כבוי.",
      r_query: "הזן טקסט לחיפוש.",
      r_json: "לא ניתן היה לקרוא את תשובת הקטלוג.",
      r_size: "תשובת הקטלוג גדולה מדי."
    },
    ja: {
      albums: "アルバム", folders: "フォルダ", lists: "プレイリスト", queue: "キュー", loading: "読み込み中...",
      empty: "ライブラリが空です。音楽フォルダはPC側で設定されます（デフォルトは「ミュージック」フォルダ）。",
      scanning: "PCがライブラリをスキャン中 ({n} ファイル)...", noAlbums: "アルバムがありません。",
      various: "オムニバス", tracks: "{n} 曲", play: "再生", shuffle: "シャッフル",
      playNext: "次に再生", playLast: "キューの最後に追加", addToList: "プレイリストに追加...", cancel: "キャンセル", ok: "OK",
      folderEmpty: "このフォルダは空です。", roots: "音楽フォルダ", back: "戻る",
      queueEmpty: "キューは空です。アルバム、フォルダ、またはプレイリストを開始してください。", clear: "クリア", saveAsList: "プレイリストとして保存",
      repeatOff: "リピート: オフ", repeatAll: "リピート: キュー全体", repeatOne: "リピート: 1曲", shuffleOn: "シャッフル: オン",
      shuffleOff: "シャッフル: オフ", moveUp: "上へ移動", moveDown: "下へ移動", remove: "削除", removeFromList: "プレイリストから削除",
      newList: "新規プレイリスト", newSmart: "新規スマートプレイリスト", listName: "プレイリスト名:", rename: "名前を変更", delete: "削除",
      deleteConfirm: "プレイリスト \"{name}\" を削除しますか？", editRules: "ルール", unavailable: "利用不可",
      smartNote: "スマートプレイリスト: ルールに基づいてPCが自動計算します。", noLists: "プレイリストはまだありません。",
      chooseList: "どのプレイリストに追加しますか？", added: "追加完了: {name}", queued: "キューに追加しました", saved: "保存完了: {name}",
      error: "エラー: {e}", smartEditor: "スマートプレイリスト", name: "名前", match: "条件", matchAll: "すべての条件に一致",
      matchAny: "いずれかの条件に一致", addCondition: "+ 条件追加", sort: "並べ替え", desc: "降順",
      limit: "上限 (0: 無制限)", save: "保存", days: "日", more: "その他",
      colTitle: "タイトル", colArtist: "アーティスト", colAlbum: "アルバム", colDuration: "時間",
      stats: "{tracks} 曲 · {albums} アルバム", playAlbum: "アルバムを再生",
      f_artist: "アーティスト", f_album: "アルバム", f_title: "タイトル", f_genre: "ジャンル", f_year: "年", f_duration: "時間 (秒)",
      f_plays: "再生回数", f_added: "追加日", f_lastPlayed: "前回の再生",
      o_contains: "含む", o_notContains: "含まない", o_is: "一致する", o_isNot: "一致しない", o_startsWith: "で始まる",
      o_eq: "=", o_ne: "≠", o_gt: ">", o_ge: "≥", o_lt: "<", o_le: "≤", o_inLast: "最近 … 日以内",
      o_notInLast: "最近 … 日以内を除く", o_never: "なし",
      s_added: "追加日", s_lastPlayed: "前回の再生", s_plays: "再生回数", s_year: "年", s_artist: "アーティスト",
      s_album: "アルバム", s_title: "タイトル", s_duration: "時間", s_random: "ランダム",
      e_empty: "再生対象がありません", e_name: "名前を入力してください", e_unknown: "見つかりません", e_smart: "スマートプレイリストに手動で曲を追加することはできません",
      e_full: "上限に達しました", e_index: "無効な位置です", e_rules: "無効なルールです",
      srcPc: "💻 PCライブラリ", srcPhone: "📱 スマートフォンの音楽",
      phoneEmpty: "スマートフォンから追加された音楽フォルダはまだありません。",
      addFolder: "フォルダ追加", rescan: "再スキャン",
      scanningPhone: "スマートフォンの音楽ライブラリをスキャン中...",
      phoneTracks: "スマートフォン上に {n} 曲見つかりました",
      notSupportedHere: "この機能はこのデバイスではサポートされていません。",
      removeFolderConfirm: "スマートフォンのライブラリからフォルダ \"{name}\" を削除しますか？",
      phoneTrackLocalMatch: "PC上にローカルコピーが見つかりました: \"{title}\"",
      sendingTrack: "PCへトラックをストリーミング中...",
      copyToPc: "PCへコピー",
      copyConfirmPrompt: "\"{title}\" はまだPCライブラリにありません。どうしますか？",
      optPlayOnly: "▶ 今すぐ再生のみ",
      optCopyOnce: "📥 PCへコピー",
      optCopyAlways: "💾 常にPCへコピー",
      optNeverAsk: "🚫 今後確認しない（再生のみ）",
      copiedToPc: "トラックがPCライブラリにコピーされました！",
      copyPrefTitle: "PCコピー設定",
      prefAsk: "新しいトラックごとに確認する",
      prefAlways: "常にPCへコピーする",
      prefNever: "コピーしない（再生のみ）",
      prefSaved: "設定を保存しました",
      onPhone: "スマートフォン内",
      matchedOnPc: "PCローカルコピーあり",
      phoneDisconnected: "スマートフォンが接続されていません",
      radio: "ラジオ",
      radioAdd: "新しい局",
      radioName: "局名",
      radioUrl: "局のアドレス (http:// または https://)",
      radioEmpty: "局がまだありません。お気に入りの局のアドレスを追加してください (局のウェブサイトでは通常「Listen」や「Stream」と表記されています。.pls や .m3u も使えます)。",
      radioNote: "ラジオは内蔵プレーヤー、イコライザー、オーディオチェーン全体を通して再生されます。対応：MP3、AAC (HE-AAC を含む)、Ogg Vorbis、Opus、FLAC。セグメント化された (HLS) ストリームには未対応です。",
      radioPlay: "再生",
      radioStop: "停止",
      radioEditUrl: "アドレスを変更",
      rs_connecting: "接続中…",
      rs_buffering: "バッファリング中…",
      rs_playing: "再生中",
      rs_reconnecting: "再接続中…",
      rs_error: "エラー：{e}",
      r_url: "アドレスが無効です (http:// または https:// が必要です)。",
      r_name: "局に名前を付けてください。",
      r_full: "局のリストがいっぱいです。",
      r_blocked: "このアドレスはローカルまたはプライベートネットワークのアドレスを指しており、セキュリティ上の理由で開けません。",
      r_dns: "サーバー名が見つかりません。",
      r_connect: "サーバーに接続できません。",
      r_timeout: "サーバーが時間内に応答しませんでした。",
      r_tls: "安全な接続を確立できませんでした。",
      r_http: "サーバーがエラーを返しました。",
      r_hls: "これはセグメント化された (HLS) ストリームで、まだ対応していません。",
      r_html: "このアドレスはウェブページであり、オーディオストリームではありません。",
      r_format: "不明な形式です。",
      r_decode: "ストリームをデコードできません。",
      r_playlist: "プレイリストに使用できるアドレスがありません。",
      r_redirect: "リダイレクトが多すぎます。",
      r_nocurl: "ラジオには curl プログラムが必要です (パッケージマネージャーでインストールしてください)。",
      r_formatChange: "ストリームの形式が変わりました。",
      r_eof: "ストリームが中断されました。",
      r_empty: "ストリームが空です。",
      r_unknown: "不明な局です。",
      r_output: "オーディオ出力を開けません。",
      r_remote: "局のリストは、コンピューター上 (または管理者のスマートフォン) でのみ編集できます。",
      radioSearch: "局を探す",
      radioSearchQ: "局名 (または一部)",
      radioCatalogOn: "局の検索をオンにする",
      radioCatalogOff: "検索をオフにする",
      radioCatalogOk: "オンにする",
      radioCatalogText: "局の検索には、コミュニティが運営する局カタログ radio-browser.info を使用します。検索するとき、このコンピューターはカタログのサーバーに直接接続し、サーバーはこのコンピューターの IP アドレスと検索文字列を知ることになります。サーバーはカタログのコミュニティが運営しており、プライバシーポリシーもデータベースのライセンスも確認できませんでした。Aqulify が送信するのは検索のみです (クリック数のカウント、再生した局、その他のデータは送信しません)。検索はいつでもオフにできます。",
      radioResults: "「{q}」の検索結果",
      radioSearching: "検索中…",
      radioNoResults: "結果がありません。",
      radioAddThis: "リストに追加",
      radioAdded: "追加しました：{name}",
      r_catalogOff: "局の検索はオフになっています。",
      r_query: "検索する文字列を入力してください。",
      r_json: "カタログの応答を読み取れませんでした。",
      r_size: "カタログの応答が大きすぎます。"
    },
    ko: {
      albums: "앨범", folders: "폴더", lists: "재생목록", queue: "대기열", loading: "로드 중...",
      empty: "라이브러리가 비어 있습니다. 음악 폴더는 PC에서 설정됩니다 (기본값은 음악 폴더).",
      scanning: "PC에서 라이브러리를 색인하는 중 ({n}개 파일)...", noAlbums: "앨범이 없습니다.",
      various: "다양한 아티스트", tracks: "{n}곡", play: "재생", shuffle: "셔플",
      playNext: "다음 곡으로 재생", playLast: "대기열 끝에 추가", addToList: "재생목록에 추가...", cancel: "취소", ok: "확인",
      folderEmpty: "이 폴더는 비어 있습니다.", roots: "음악 폴더", back: "뒤로",
      queueEmpty: "대기열이 비어 있습니다. 앨범, 폴더 또는 재생목록을 시작하세요.", clear: "지우기", saveAsList: "재생목록으로 저장",
      repeatOff: "반복: 끔", repeatAll: "반복: 대기열", repeatOne: "반복: 1곡", shuffleOn: "셔플: 켬",
      shuffleOff: "셔플: 끔", moveUp: "위로 이동", moveDown: "아래로 이동", remove: "제거", removeFromList: "재생목록에서 제거",
      newList: "새 재생목록", newSmart: "새 스마트 재생목록", listName: "재생목록 이름:", rename: "이름 바꾸기", delete: "삭제",
      deleteConfirm: "\"{name}\" 재생목록을 삭제하시겠습니까?", editRules: "규칙", unavailable: "사용 불가",
      smartNote: "스마트 재생목록: PC가 규칙을 기반으로 콘텐츠를 자동 계산합니다.", noLists: "재생목록이 없습니다.",
      chooseList: "어느 재생목록에 추가할까요?", added: "추가됨: {name}", queued: "대기열에 추가됨", saved: "저장됨: {name}",
      error: "오류: {e}", smartEditor: "스마트 재생목록", name: "이름", match: "조건", matchAll: "모든 조건 일치",
      matchAny: "하나 이상의 조건 일치", addCondition: "+ 조건 추가", sort: "정렬 기준", desc: "내림차순",
      limit: "최대 개수 (0: 전체)", save: "저장", days: "일", more: "더보기",
      colTitle: "제목", colArtist: "아티스트", colAlbum: "앨범", colDuration: "길이",
      stats: "{tracks}곡 · {albums}개 앨범", playAlbum: "앨범 재생",
      f_artist: "아티스트", f_album: "앨범", f_title: "제목", f_genre: "장르", f_year: "연도", f_duration: "길이 (초)",
      f_plays: "재생 횟수", f_added: "추가된 날짜", f_lastPlayed: "최근 재생",
      o_contains: "포함", o_notContains: "포함하지 않음", o_is: "일치", o_isNot: "일치하지 않음", o_startsWith: "시작 문자",
      o_eq: "=", o_ne: "≠", o_gt: ">", o_ge: "≥", o_lt: "<", o_le: "≤", o_inLast: "최근 … 일 이내",
      o_notInLast: "최근 … 일 이내 제외", o_never: "없음",
      s_added: "추가된 날짜", s_lastPlayed: "최근 재생", s_plays: "재생 횟수", s_year: "연도", s_artist: "아티스트",
      s_album: "앨범", s_title: "제목", s_duration: "길이", s_random: "무작위",
      e_empty: "재생할 항목이 없습니다", e_name: "이름을 입력하세요", e_unknown: "찾을 수 없음", e_smart: "스마트 재생목록에는 수동으로 곡을 추가할 수 없습니다",
      e_full: "가득 참", e_index: "잘못된 위치", e_rules: "잘못된 규칙",
      srcPc: "💻 PC 라이브러리", srcPhone: "📱 스마트폰 음악",
      phoneEmpty: "스마트폰에서 추가된 음악 폴더가 아직 없습니다.",
      addFolder: "폴더 추가", rescan: "다시 검색",
      scanningPhone: "스마트폰 음악 라이브러리 검색 중...",
      phoneTracks: "스마트폰에서 {n}곡 발견",
      notSupportedHere: "이 기능은 이 장치에서 지원되지 않습니다.",
      removeFolderConfirm: "스마트폰 라이브러리에서 \"{name}\" 폴더를 제거하시겠습니까?",
      phoneTrackLocalMatch: "PC에서 로컬 사본 발견: \"{title}\"",
      sendingTrack: "PC로 곡 스트리밍 중...",
      copyToPc: "PC로 복사",
      copyConfirmPrompt: "\"{title}\" 곡이 아직 PC 라이브러리에 없습니다. 어떻게 하시겠습니까?",
      optPlayOnly: "▶ 지금만 재생",
      optCopyOnce: "📥 PC로 복사",
      optCopyAlways: "💾 항상 PC로 복사",
      optNeverAsk: "🚫 다시 묻지 않음 (재생만)",
      copiedToPc: "곡이 PC 라이브러리에 복사되었습니다!",
      copyPrefTitle: "PC 복사 설정",
      prefAsk: "새 트랙마다 묻기",
      prefAlways: "항상 PC로 복사",
      prefNever: "복사하지 않음 (재생만)",
      prefSaved: "설정 저장됨",
      onPhone: "스마트폰에 있음",
      matchedOnPc: "PC에 로컬 사본 있음",
      phoneDisconnected: "스마트폰 연결되지 않음",
      radio: "라디오",
      radioAdd: "새 방송국",
      radioName: "방송국 이름",
      radioUrl: "방송국 주소(http:// 또는 https://)",
      radioEmpty: "아직 방송국이 없습니다. 즐겨 찾는 방송국의 주소를 추가하세요(방송국 웹사이트에서는 보통 “Listen” 또는 “Stream”으로 표시됩니다. .pls와 .m3u도 사용할 수 있습니다).",
      radioNote: "라디오는 내장 플레이어, 이퀄라이저, 전체 오디오 체인을 거쳐 재생됩니다. 지원: MP3, AAC(HE-AAC 포함), Ogg Vorbis, Opus, FLAC. 세그먼트(HLS) 스트림은 아직 지원하지 않습니다.",
      radioPlay: "재생",
      radioStop: "중지",
      radioEditUrl: "주소 변경",
      rs_connecting: "연결하는 중…",
      rs_buffering: "버퍼링 중…",
      rs_playing: "재생 중",
      rs_reconnecting: "다시 연결하는 중…",
      rs_error: "오류: {e}",
      r_url: "잘못된 주소입니다(http:// 또는 https://가 필요합니다).",
      r_name: "방송국 이름을 입력하세요.",
      r_full: "방송국 목록이 가득 찼습니다.",
      r_blocked: "이 주소는 로컬 또는 사설 네트워크 주소를 가리키므로 보안상 열 수 없습니다.",
      r_dns: "서버 이름을 찾을 수 없습니다.",
      r_connect: "서버에 연결할 수 없습니다.",
      r_timeout: "서버가 제때 응답하지 않았습니다.",
      r_tls: "보안 연결을 설정할 수 없습니다.",
      r_http: "서버가 오류를 반환했습니다.",
      r_hls: "세그먼트(HLS) 스트림이며 아직 지원되지 않습니다.",
      r_html: "이 주소는 웹 페이지이며 오디오 스트림이 아닙니다.",
      r_format: "알 수 없는 형식입니다.",
      r_decode: "스트림을 디코딩할 수 없습니다.",
      r_playlist: "재생 목록에 사용할 수 있는 주소가 없습니다.",
      r_redirect: "리디렉션이 너무 많습니다.",
      r_nocurl: "라디오에는 curl 프로그램이 필요합니다(패키지 관리자로 설치하세요).",
      r_formatChange: "스트림 형식이 바뀌었습니다.",
      r_eof: "스트림이 중단되었습니다.",
      r_empty: "스트림이 비어 있습니다.",
      r_unknown: "알 수 없는 방송국입니다.",
      r_output: "오디오 출력을 열 수 없습니다.",
      r_remote: "방송국 목록은 컴퓨터에서만(또는 관리자 휴대폰에서) 편집할 수 있습니다.",
      radioSearch: "방송국 찾기",
      radioSearchQ: "방송국 이름(또는 일부)",
      radioCatalogOn: "방송국 검색 켜기",
      radioCatalogOff: "검색 끄기",
      radioCatalogOk: "켜기",
      radioCatalogText: "방송국 검색은 커뮤니티가 운영하는 방송국 카탈로그 radio-browser.info를 사용합니다. 검색할 때 이 컴퓨터가 카탈로그 서버에 직접 접속하며, 서버는 이 컴퓨터의 IP 주소와 검색어를 볼 수 있습니다. 서버는 카탈로그 커뮤니티가 운영하며, 개인정보 처리방침이나 데이터베이스 라이선스는 찾지 못했습니다. Aqulify는 검색 요청만 보냅니다(클릭 수 집계, 재생한 방송국, 기타 데이터는 보내지 않습니다). 검색은 언제든지 끌 수 있습니다.",
      radioResults: "“{q}” 검색 결과",
      radioSearching: "검색하는 중…",
      radioNoResults: "결과가 없습니다.",
      radioAddThis: "목록에 추가",
      radioAdded: "추가됨: {name}",
      r_catalogOff: "방송국 검색이 꺼져 있습니다.",
      r_query: "검색어를 입력하세요.",
      r_json: "카탈로그의 응답을 읽을 수 없습니다.",
      r_size: "카탈로그의 응답이 너무 큽니다."
    },
    cs: {
      o_never: "nikdy",
      s_added: "Přidáno",
      s_lastPlayed: "Naposledy přehráno",
      s_plays: "Počet přehrání",
      s_year: "Rok",
      s_artist: "Interpret",
      s_album: "Album",
      s_title: "Název",
      s_duration: "Délka",
      s_random: "Náhodně",
      e_empty: "Není co přehrát",
      e_name: "Zadejte název",
      e_unknown: "Nenalezeno",
      e_smart: "Do inteligentního seznamu nelze přidávat skladby",
      e_full: "plné",
      e_index: "Neplatná pozice",
      e_rules: "Neplatné pravidlo",
      srcPc: "💻 Knihovna PC",
      srcPhone: "📱 Hudba v telefonu",
      phoneEmpty: "Ze telefonu zatím nebyla přidána žádná složka s hudbou.",
      addFolder: "Přidat složku",
      rescan: "Znovu prohledat",
      scanningPhone: "Prohledávání hudební knihovny telefonu…",
      phoneTracks: "V telefonu nalezeno skladeb: {n}",
      notSupportedHere: "Tato funkce na tomto zařízení není dostupná.",
      removeFolderConfirm: "Odebrat složku \"{name}\" z knihovny telefonu?",
      phoneTrackLocalMatch: "Místní kopie nalezena v PC: \"{title}\"",
      sendingTrack: "Streamování skladby do PC…",
      copyToPc: "Kopírovat do PC",
      copyConfirmPrompt: "\"{title}\" zatím není v knihovně PC. Co chcete udělat?",
      optPlayOnly: "▶ Přehrát jen teď",
      optCopyOnce: "📥 Kopírovat do PC",
      optCopyAlways: "💾 Vždy kopírovat do PC",
      optNeverAsk: "🚫 Nikdy se neptat (jen přehrát)",
      albums: "Alba", folders: "Složky", lists: "Seznamy skladeb", queue: "Fronta", loading: "Načítání...",
      empty: "Knihovna je prázdná. Hudební složky se nastavují v počítači (výchozí je složka Hudba).",
      scanning: "Počítač indexuje knihovnu ({n} souborů)...", noAlbums: "Žádná alba.",
      various: "Různí umělci", tracks: "{n} skladeb", play: "Přehrát", shuffle: "Náhodně",
      playNext: "Přehrát jako další", playLast: "Přidat do fronty", addToList: "Přidat do seznamu skladeb...", cancel: "Storno", ok: "OK",
      folderEmpty: "Tato složka je prázdná.", roots: "Hudební složky", back: "Zpět",
      queueEmpty: "Fronta je prázdná. Spusťte album, složku nebo seznam skladeb.", clear: "Vymazat", saveAsList: "Uložit jako seznam skladeb",
      repeatOff: "Opakování: vyp", repeatAll: "Opakování: fronta", repeatOne: "Opakování: skladba", shuffleOn: "Náhodně: zap",
      shuffleOff: "Náhodně: vyp", moveUp: "Posunout nahoru", moveDown: "Posunout dolů", remove: "Odstranit", removeFromList: "Odebrat ze seznamu",
      newList: "Nový seznam skladeb", newSmart: "Nový inteligentní seznam", listName: "Název seznamu skladeb:", rename: "Přejmenovat", delete: "Smazat",
      deleteConfirm: "Smazat seznam skladeb \"{name}\"?", editRules: "Pravidla", unavailable: "nedostupné",
      smartNote: "Inteligentní seznam skladeb: Počítač vypočítává obsah na základě pravidel.", noLists: "Zatím žádné seznamy skladeb.",
      chooseList: "Který seznam skladeb?", added: "Přidáno: {name}", queued: "Přidáno do fronty", saved: "Uloženo: {name}",
      error: "Chyba: {e}", smartEditor: "Inteligentní seznam skladeb", name: "Název", match: "Podmínky", matchAll: "vše musí odpovídat",
      matchAny: "alespoň jedno může odpovídat", addCondition: "+ Podmínka", sort: "Seřadit podle", desc: "sestupně",
      limit: "Maximálně (0: vše)", save: "Uložit", days: "dní", more: "Více",
      colTitle: "Název", colArtist: "Umělec", colAlbum: "Album", colDuration: "Délka",
      stats: "{tracks} skladeb · {albums} alb", playAlbum: "Přehrát album",
      f_artist: "Umělec", f_album: "Album", f_title: "Název", f_genre: "Žánr", f_year: "Rok", f_duration: "Délka (s)",
      f_plays: "Přehrání", f_added: "Přidáno", f_lastPlayed: "Naposledy hráno",
      o_contains: "obsahuje", o_notContains: "neobsahuje", o_is: "je", o_isNot: "není", o_startsWith: "začíná na",
      o_eq: "=", o_ne: "≠", o_gt: ">", o_ge: "≥", o_lt: "<", o_le: "≤", o_inLast: "za posledních … dní",
      o_notInLast: "ne za posledních … dní", o_before: "před", o_after: "po",
      copiedToPc: "Skladba byla zkopírována do knihovny v PC!",
      copyPrefTitle: "Nastavení kopírování do PC",
      prefAsk: "Zeptat se u každé nové skladby",
      prefAlways: "Vždy zkopírovat do PC",
      prefNever: "Nikdy nekopírovat (pouze přehrát)",
      prefSaved: "Nastavení bylo uloženo",
      onPhone: "V telefonu",
      matchedOnPc: "Místní kopie v PC",
      phoneDisconnected: "Telefon není připojen",
      radio: "Rádio",
      radioAdd: "Nová stanice",
      radioName: "Název stanice",
      radioUrl: "Adresa stanice (http:// nebo https://)",
      radioEmpty: "Zatím žádné stanice. Přidejte adresu svých oblíbených stanic (na webu stanice bývá označena „Poslouchat“ nebo „Stream“; fungují i .pls a .m3u).",
      radioNote: "Rádio hraje přes vestavěný přehrávač, ekvalizér a celý zvukový řetězec. Podporováno: MP3, AAC (vč. HE-AAC), Ogg Vorbis, Opus, FLAC; segmentované (HLS) proudy zatím ne.",
      radioPlay: "Přehrát",
      radioStop: "Zastavit",
      radioEditUrl: "Změnit adresu",
      rs_connecting: "Připojování…",
      rs_buffering: "Načítání do vyrovnávací paměti…",
      rs_playing: "Hraje",
      rs_reconnecting: "Opětovné připojování…",
      rs_error: "Chyba: {e}",
      r_url: "Neplatná adresa (vyžaduje se http:// nebo https://).",
      r_name: "Dejte stanici název.",
      r_full: "Seznam stanic je plný.",
      r_blocked: "Tato adresa směřuje na adresu místní nebo soukromé sítě a z bezpečnostních důvodů ji nelze otevřít.",
      r_dns: "Název serveru nebyl nalezen.",
      r_connect: "Server není dostupný.",
      r_timeout: "Server neodpověděl včas.",
      r_tls: "Zabezpečené spojení se nepodařilo navázat.",
      r_http: "Server vrátil chybu.",
      r_hls: "Toto je segmentovaný (HLS) proud, který zatím není podporován.",
      r_html: "Tato adresa je webová stránka, ne zvukový proud.",
      r_format: "Neznámý formát.",
      r_decode: "Proud nelze dekódovat.",
      r_playlist: "Seznam skladeb neobsahuje použitelnou adresu.",
      r_redirect: "Příliš mnoho přesměrování.",
      r_nocurl: "Rádio potřebuje program curl (nainstalujte jej správcem balíčků).",
      r_formatChange: "Formát proudu se změnil.",
      r_eof: "Proud byl přerušen.",
      r_empty: "Proud je prázdný.",
      r_unknown: "Neznámá stanice.",
      r_output: "Zvukový výstup nelze otevřít.",
      r_remote: "Seznam stanic lze upravovat jen na počítači (nebo z telefonu správce).",
      radioSearch: "Najít stanici",
      radioSearchQ: "Název stanice (nebo jeho část)",
      radioCatalogOn: "Zapnout vyhledávání stanic",
      radioCatalogOff: "Vypnout vyhledávání",
      radioCatalogOk: "Zapnout",
      radioCatalogText: "Vyhledávání stanic používá komunitní katalog stanic radio-browser.info. Při hledání se tento počítač připojuje přímo k serverům katalogu: ty vidí IP adresu tohoto počítače a hledaný text. Servery provozuje komunita katalogu; nenašli jsme zásady ochrany soukromí ani licenci databáze. Aqulify odesílá jen samotné hledání (žádné počítání kliknutí, žádné přehrávané stanice, žádná jiná data). Vyhledávání můžete kdykoli vypnout.",
      radioResults: "Výsledky pro „{q}“",
      radioSearching: "Hledá se…",
      radioNoResults: "Žádné výsledky.",
      radioAddThis: "Přidat do seznamu",
      radioAdded: "Přidáno: {name}",
      r_catalogOff: "Vyhledávání stanic je vypnuto.",
      r_query: "Zadejte hledaný text.",
      r_json: "Odpověď katalogu se nepodařilo přečíst.",
      r_size: "Odpověď katalogu je příliš velká."
    },
    uk: {
      o_never: "ніколи",
      s_added: "Додано",
      s_lastPlayed: "Востаннє відтворювалось",
      s_plays: "Кількість відтворень",
      s_year: "Рік",
      s_artist: "Виконавець",
      s_album: "Альбом",
      s_title: "Назва",
      s_duration: "Тривалість",
      s_random: "Випадково",
      e_empty: "Немає що відтворювати",
      e_name: "Введіть назву",
      e_unknown: "Не знайдено",
      e_smart: "Не можна додавати треки до розумного списку",
      e_full: "переповнено",
      e_index: "Неправильна позиція",
      e_rules: "Неправильне правило",
      srcPc: "💻 Бібліотека ПК",
      srcPhone: "📱 Музика телефону",
      phoneEmpty: "З телефону ще не додано жодної теки з музикою.",
      addFolder: "Додати теку",
      rescan: "Повторне сканування",
      scanningPhone: "Сканування музичної бібліотеки телефону…",
      phoneTracks: "Знайдено треків на телефоні: {n}",
      notSupportedHere: "Ця функція недоступна на цьому пристрої.",
      removeFolderConfirm: "Видалити теку \"{name}\" з бібліотеки телефону?",
      phoneTrackLocalMatch: "Локальну копію знайдено на ПК: \"{title}\"",
      sendingTrack: "Передача треку на ПК…",
      copyToPc: "Копіювати на ПК",
      copyConfirmPrompt: "\"{title}\" ще немає в бібліотеці ПК. Що бажаєте зробити?",
      optPlayOnly: "▶ Лише відтворити зараз",
      optCopyOnce: "📥 Копіювати на ПК",
      optCopyAlways: "💾 Завжди копіювати на ПК",
      optNeverAsk: "🚫 Ніколи не питати (лише відтворити)",
      albums: "Альбоми", folders: "Папки", lists: "Списки відтворення", queue: "Черга", loading: "Завантаження...",
      empty: "Медіатека порожня. Музичні папки налаштовуються на комп'ютері (типово папка Музика).",
      scanning: "Комп'ютер індексує медіатеку ({n} файлів)...", noAlbums: "Немає альбомів.",
      various: "Різні виконавці", tracks: "{n} треків", play: "Відтворити", shuffle: "Випадково",
      playNext: "Відтворити наступним", playLast: "Додати в кінець черги", addToList: "Додати до списку відтворення...", cancel: "Скасувати", ok: "OK",
      folderEmpty: "Ця папка порожня.", roots: "Музичні папки", back: "Назад",
      queueEmpty: "Черга порожня. Запустіть альбом, папку або список відтворення.", clear: "Очистити", saveAsList: "Зберегти як список",
      repeatOff: "Повтор: вимк", repeatAll: "Повтор: черга", repeatOne: "Повтор: трек", shuffleOn: "Випадково: увімк",
      shuffleOff: "Випадково: вимк", moveUp: "Вгору", moveDown: "Вниз", remove: "Видалити", removeFromList: "Вилучити зі списку",
      newList: "Новий список відтворення", newSmart: "Новий смарт-список", listName: "Назва списку:", rename: "Перейменувати", delete: "Видалити",
      deleteConfirm: "Видалити список відтворення \"{name}\"?", editRules: "Правила", unavailable: "недоступно",
      smartNote: "Смарт-список: комп'ютер формує вміст на основі правил.", noLists: "Ще немає списків відтворення.",
      chooseList: "Який список відтворення?", added: "Додано: {name}", queued: "Додано в чергу", saved: "Збережено: {name}",
      error: "Помилка: {e}", smartEditor: "Смарт-список", name: "Назва", match: "Умова", matchAll: "відповідає всім",
      matchAny: "відповідає будь-якій", addCondition: "+ Умова", sort: "Сортувати за", desc: "за спаданням",
      limit: "Максимум (0: всі)", save: "Зберегти", days: "днів", more: "Більше",
      colTitle: "Назва", colArtist: "Виконавець", colAlbum: "Альбом", colDuration: "Тривалість",
      stats: "{tracks} треків · {albums} альбомів", playAlbum: "Відтворити альбом",
      f_artist: "Виконавець", f_album: "Альбом", f_title: "Назва", f_genre: "Жанр", f_year: "Рік", f_duration: "Тривалість (с)",
      f_plays: "Прослуховувань", f_added: "Додано", f_lastPlayed: "Останнє відтворення",
      o_contains: "містить", o_notContains: "не містить", o_is: "є", o_isNot: "не є", o_startsWith: "починається з",
      o_eq: "=", o_ne: "≠", o_gt: ">", o_ge: "≥", o_lt: "<", o_le: "≤", o_inLast: "за останні … днів",
      o_notInLast: "не за останні … днів", o_before: "до", o_after: "після",
      copiedToPc: "Трек скопійовано до бібліотеки на ПК!",
      copyPrefTitle: "Налаштування копіювання на ПК",
      prefAsk: "Запитувати для кожного нового треку",
      prefAlways: "Завжди копіювати на ПК",
      prefNever: "Ніколи не копіювати (лише відтворювати)",
      prefSaved: "Налаштування збережено",
      onPhone: "На телефоні",
      matchedOnPc: "Локальна копія на ПК",
      phoneDisconnected: "Телефон не підключено",
      radio: "Радіо",
      radioAdd: "Нова станція",
      radioName: "Назва станції",
      radioUrl: "Адреса станції (http:// або https://)",
      radioEmpty: "Станцій поки немає. Додайте адресу улюблених станцій (на сайті станції вона зазвичай називається «Слухати» або «Stream»; .pls і .m3u теж підходять).",
      radioNote: "Радіо відтворюється через вбудований плеєр, еквалайзер і весь аудіоланцюжок. Підтримуються: MP3, AAC (зокрема HE-AAC), Ogg Vorbis, Opus, FLAC; сегментовані потоки (HLS) поки що ні.",
      radioPlay: "Відтворити",
      radioStop: "Зупинити",
      radioEditUrl: "Змінити адресу",
      rs_connecting: "Підключення…",
      rs_buffering: "Буферизація…",
      rs_playing: "Відтворюється",
      rs_reconnecting: "Повторне підключення…",
      rs_error: "Помилка: {e}",
      r_url: "Недійсна адреса (потрібен http:// або https://).",
      r_name: "Дайте станції назву.",
      r_full: "Список станцій заповнений.",
      r_blocked: "Ця адреса вказує на локальну або приватну мережу й з міркувань безпеки не може бути відкрита.",
      r_dns: "Ім'я сервера не знайдено.",
      r_connect: "Сервер недоступний.",
      r_timeout: "Сервер не відповів вчасно.",
      r_tls: "Не вдалося встановити захищене з'єднання.",
      r_http: "Сервер повернув помилку.",
      r_hls: "Це сегментований потік (HLS), який поки що не підтримується.",
      r_html: "Ця адреса — вебсторінка, а не аудіопотік.",
      r_format: "Невідомий формат.",
      r_decode: "Потік неможливо декодувати.",
      r_playlist: "У списку відтворення немає придатної адреси.",
      r_redirect: "Забагато перенаправлень.",
      r_nocurl: "Для радіо потрібна програма curl (встановіть її менеджером пакетів).",
      r_formatChange: "Формат потоку змінився.",
      r_eof: "Потік було перервано.",
      r_empty: "Потік порожній.",
      r_unknown: "Невідома станція.",
      r_output: "Неможливо відкрити аудіовихід.",
      r_remote: "Список станцій можна редагувати лише на комп'ютері (або з телефона адміністратора).",
      radioSearch: "Знайти станцію",
      radioSearchQ: "Назва станції (або її частина)",
      radioCatalogOn: "Увімкнути пошук станцій",
      radioCatalogOff: "Вимкнути пошук",
      radioCatalogOk: "Увімкнути",
      radioCatalogText: "Пошук станцій використовує спільнотний каталог станцій radio-browser.info. Під час пошуку цей комп'ютер звертається безпосередньо до серверів каталогу: вони бачать IP-адресу цього комп'ютера й текст запиту. Сервери підтримує спільнота каталогу; політики конфіденційності та ліцензії на базу даних ми не знайшли. Aqulify надсилає лише сам пошуковий запит (без підрахунку кліків, без відтворених станцій і інших даних). Пошук можна вимкнути будь-коли.",
      radioResults: "Результати за запитом «{q}»",
      radioSearching: "Пошук…",
      radioNoResults: "Нічого не знайдено.",
      radioAddThis: "Додати до списку",
      radioAdded: "Додано: {name}",
      r_catalogOff: "Пошук станцій вимкнено.",
      r_query: "Введіть текст для пошуку.",
      r_json: "Не вдалося прочитати відповідь каталогу.",
      r_size: "Відповідь каталогу завелика."
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
    let S = STR[opts.lang] || STR.en;
    // hiányzó fordításnál az angol szöveg (az új kulcsok előbb csak magyarul és angolul készülnek)
    const t = (k, v) => (S[k] || STR.en[k] || k).replace(/\{(\w+)\}/g, (_, n) => (v && n in v ? v[n] : ""));
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
    ["albums", "folders", "lists", "queue", "radio"].forEach(k => {
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
    let radio = null, radioRev = -1, radioSt = null, radioSearch = null, radioReqN = 0;
    const radioReqBase = Math.random().toString(36).slice(2, 8) + "-";   // a keresés jele (a válasz mindenkinek megy)
    let pendingOpenList = false;   // létrehozás / mentés után a válaszként jövő listát nyitjuk meg
    let pickFor = null;            // listához adás: a forrás (üzenetrész), amíg a felhasználó listát választ
    let lastRendered = null;       // a legutóbb kirajzolt album / mappa / lista (nyelvváltáskor újrarajzoláshoz)

    function open(v, top) {
      closeCtx();
      if (top) history.length = 0;
      else history.push(view);
      view = v;
      render();
      if (source === "pc" || v.kind === "radio") fetchFor(v);
    }

    function back() {
      closeCtx();
      view = history.pop() || { kind: "albums" };
      render();
      if (source === "pc" || view.kind === "radio") fetchFor(view);
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
      else if (v.kind === "radio") send({ type: "radio_list" });
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
        else if (view.kind === "radio") renderRadio();
        return;
      }
      if (view.kind === "radio") { renderRadio(); return; }
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
        toast(t("notSupportedHere"));
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
        toast(t("notSupportedHere"));
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
        c.appendChild(el("p", null, t("notSupportedHere")));
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

    // ---- Internetrádió: a felhasználó saját állomáslistája (radio_* üzenetek, status.radio)
    const radioErr = e => {
      const k = "r_" + String(e || "");
      return S[k] || STR.en[k] || errText(e);
    };
    function radioStateText() {
      if (!radioSt || radioSt.state === "idle") return "";
      if (radioSt.state === "error") return t("rs_error", { e: radioErr(radioSt.error) });
      const name = ((radio && radio.items) || []).find(s => s.id === radioSt.station);
      const what = t("rs_" + radioSt.state);
      const fmtTxt = radioSt.codec ? " · " + radioSt.codec.toUpperCase() + (radioSt.bitrate ? " " + radioSt.bitrate + " kbit/s" : "") : "";
      return (name ? name.name + ": " : "") + what + fmtTxt;
    }
    function radioHost(u) {
      try { return new URL(u).host; } catch (e) { return u; }
    }
    function renderRadio() {
      body.replaceChildren();
      const actions = el("div", "lib-actions");
      actions.appendChild(btn("＋ " + t("radioAdd"), "lib-btn", async () => {
        const n = await ask(t("radioName"), { input: true });
        if (!n || !n.trim()) return;
        const u = await ask(t("radioUrl"), { input: true, value: "https://" });
        if (u && u.trim()) send({ type: "radio_add", name: n.trim(), url: u.trim() });
      }));
      const catalogOn = !!((radio && radio.catalog) || (!radio && radioSt && radioSt.catalog));
      if (catalogOn) {
        actions.appendChild(btn("🔎 " + t("radioSearch"), "lib-btn", async () => {
          const q = await ask(t("radioSearchQ"), { input: true, value: radioSearch ? radioSearch.q : "" });
          if (!q || !q.trim()) return;
          const req = radioReqBase + (++radioReqN);
          radioSearch = { q: q.trim(), req, state: "searching", items: [], error: "" };
          send({ type: "radio_search", q: q.trim(), req });
          renderRadio();
        }));
      } else {
        // a külső katalógus csak a tájékoztató elfogadása után (alapból ki)
        actions.appendChild(btn("🔎 " + t("radioCatalogOn"), "lib-btn", async () => {
          if (await ask(t("radioCatalogText"), { okLabel: t("radioCatalogOk") })) send({ type: "radio_catalog", on: true });
        }));
      }
      const active = radioSt && radioSt.state !== "idle" && radioSt.state !== "error";
      if (active) actions.appendChild(btn("■ " + t("radioStop"), "lib-btn", () => send({ type: "radio_stop" })));
      body.appendChild(actions);
      const st = radioStateText();
      if (st) body.appendChild(el("div", "hint", st));
      if (!radio) { body.appendChild(el("div", "hint", t("loading"))); return; }
      if (!radio.items.length) body.appendChild(el("div", "hint", t("radioEmpty")));
      const ul = el("ul", "lib-tracks");
      radio.items.forEach((s, i) => {
        const cur = active && radioSt.station === s.id;
        const li = el("li", "lib-folder" + (cur ? " playing" : ""));
        if (desktop) li.appendChild(el("span", "lib-no", cur ? "▶" : "📻"));
        const meta = el("div", "meta");
        meta.appendChild(el("div", "title", (desktop ? "" : (cur ? "▶ " : "📻 ")) + s.name));
        meta.appendChild(el("div", "detail", radioHost(s.url)));
        li.appendChild(meta);
        li.appendChild(moreButton(e => {
          e.stopPropagation();
          menu(s.name, [
            { label: t("radioPlay"), run: () => { send({ type: "radio_play", id: s.id }); opts.onPlay && opts.onPlay(); } },
            { label: t("rename"), run: async () => {
              const n = await ask(t("radioName"), { input: true, value: s.name });
              if (n && n.trim()) send({ type: "radio_update", id: s.id, name: n.trim() });
            } },
            { label: t("radioEditUrl"), run: async () => {
              const u = await ask(t("radioUrl"), { input: true, value: s.url });
              if (u && u.trim()) send({ type: "radio_update", id: s.id, url: u.trim() });
            } },
            { label: t("moveUp"), run: () => i > 0 && send({ type: "radio_move", from: i, to: i - 1 }) },
            { label: t("moveDown"), run: () => i + 1 < radio.items.length && send({ type: "radio_move", from: i, to: i + 1 }) },
            { label: t("delete"), danger: true, run: async () => {
              if (await ask(t("deleteConfirm", { name: s.name }), { okLabel: t("delete"), danger: true })) send({ type: "radio_remove", id: s.id });
            } },
          ], e);
        }));
        ul.appendChild(openRow(li, () => { send({ type: "radio_play", id: s.id }); opts.onPlay && opts.onPlay(); }));
      });
      body.appendChild(ul);
      body.appendChild(el("div", "hint", t("radioNote")));
      if (catalogOn && radioSearch) {
        body.appendChild(header(t("radioResults", { q: radioSearch.q }), null, false));
        if (radioSearch.state === "searching") body.appendChild(el("div", "hint", t("radioSearching")));
        else if (radioSearch.error) body.appendChild(el("div", "hint", radioErr(radioSearch.error)));
        else if (!radioSearch.items.length) body.appendChild(el("div", "hint", t("radioNoResults")));
        else {
          const rl = el("ul", "lib-tracks");
          radioSearch.items.forEach(it => {
            const li = el("li", "lib-folder");
            if (desktop) li.appendChild(el("span", "lib-no", "＋"));
            const meta = el("div", "meta");
            meta.appendChild(el("div", "title", (desktop ? "" : "＋ ") + it.name));
            const det = [it.codec, it.bitrate ? it.bitrate + " kbit/s" : "", it.country, radioHost(it.url)].filter(Boolean).join(" · ");
            meta.appendChild(el("div", "detail", det));
            li.appendChild(meta);
            li.title = t("radioAddThis");
            // a sor kattintásra felveszi az állomást a saját listába
            rl.appendChild(openRow(li, () => {
              send({ type: "radio_add", name: it.name, url: it.url });
              toast(t("radioAdded", { name: it.name }));
            }));
          });
          body.appendChild(rl);
        }
      }
      if (catalogOn) {
        const off = el("div", "lib-actions");
        off.appendChild(btn(t("radioCatalogOff"), "lib-btn", () => { radioSearch = null; send({ type: "radio_catalog", on: false }); }));
        body.appendChild(off);
      }
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
        case "radio":
          radio = m;
          radioRev = m.rev;
          if (view.kind === "radio") renderRadio();
          return true;
        case "radio_result":
          if (m.ok === false) toast(radioErr(m.error));
          return true;
        case "radio_search":
          // a találatok minden kliensnek mennek: csak a saját (utolsó) keresésünkét vesszük át
          if (!radioSearch || m.req !== radioSearch.req || m.state === "searching") return true;
          radioSearch.state = "done";
          radioSearch.items = Array.isArray(m.items) ? m.items : [];
          radioSearch.error = m.error || "";
          if (view.kind === "radio") renderRadio();
          return true;
      }
      return false;
    }

    /** Az állapotüzenet: ha a sor vagy a listák változtak, a nyitott nézet frissül. */
    function onStatus(st) {
      const r = st.radio;
      if (r) {
        const changed = !radioSt || r.state !== radioSt.state || r.station !== radioSt.station || r.error !== radioSt.error ||
                        r.codec !== radioSt.codec || r.catalog !== radioSt.catalog;
        radioSt = r;
        if (r.rev !== radioRev && view.kind === "radio" && !root.hidden) { radioRev = r.rev; send({ type: "radio_list" }); }
        else if (changed && view.kind === "radio" && !root.hidden) renderRadio();
      }
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
        if (source === "pc" || view.kind === "radio") fetchFor(view);
      },
      onMessage,
      onStatus,
      setLang(l) {
        S = STR[l] || STR.en;
        setStaticTexts();
        render();
      },
      reset() { art.clear(); artBusy.clear(); artWait.length = 0; albums = []; albumsTotal = -1; albumsLoading = false; info = null; lastRendered = null; },
    };
  };
})();
