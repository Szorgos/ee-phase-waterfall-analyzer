# Függő fordítások (nagy felbontás, A rész)

A következő kulcsok magyarul és angolul készek; a másik 21 nyelvben (es, fr, zh, de, it, nl, pl, ru, pt, ro, el, tr, fi, da, nb, sv, he, ja, ko, cs, uk) **ideiglenesen az angol szöveg** áll bent. A 23 nyelvű fordítás a nagy felbontás A–B–C részei után következik; addig ez a lista a teendő.

Fájl: `i18n/gui/<nyelv>.json`, összeállítás: `python tools/i18n_build.py`.

- Terhelés-tanácsadó: `la.title`, `la.whyCpu`, `la.whyXrun`, `la.step.two_threads`, `la.step.precision24`, `la.step.lower_target`, `la.step.upsample_off`, `la.step.none`, `la.optimize`, `la.keep`, `la.never`, `la.undo`, `la.doneTitle`, `la.doneText`, `la.setting`, `la.settingHint`
- DSP-korlát: `hires.dspOff`
- Felmintavételezés: `ups.maxRate`, `ups.maxRateNone`, `ups.threads`, `ups.threadsAuto`, `ups.threads1`, `ups.threads2`, `ups.threadsHint`, `ups.core`, `ups.coreOff`, `ups.coreN`, `ups.coreHint`, `ups.statusThreads`, `ups.statusCore`

Fordításnál ügyelj a változókra (`{rate}`, `{cpu}`, `{n}`), és hogy a szálak / dedikált mag szövege őszinte maradjon: nem ígér hangminőség-javulást.
