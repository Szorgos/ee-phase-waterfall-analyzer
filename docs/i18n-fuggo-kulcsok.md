# Függő fordítások (nagy felbontás, A rész)

A következő kulcsok magyarul és angolul készek; a másik 21 nyelvben (es, fr, zh, de, it, nl, pl, ru, pt, ro, el, tr, fi, da, nb, sv, he, ja, ko, cs, uk) **ideiglenesen az angol szöveg** áll bent. A 23 nyelvű fordítás a nagy felbontás A–B–C részei után következik; addig ez a lista a teendő.

Fájl: `i18n/gui/<nyelv>.json`, összeállítás: `python tools/i18n_build.py`.

- Terhelés-tanácsadó: `la.title`, `la.whyCpu`, `la.whyXrun`, `la.step.two_threads`, `la.step.precision24`, `la.step.lower_target`, `la.step.upsample_off`, `la.step.none`, `la.optimize`, `la.keep`, `la.never`, `la.undo`, `la.doneTitle`, `la.doneText`, `la.setting`, `la.settingHint`
- DSP-korlát: `hires.dspOff`
- Felmintavételezés: `ups.maxRate`, `ups.maxRateNone`, `ups.threads`, `ups.threadsAuto`, `ups.threads1`, `ups.threads2`, `ups.threadsHint`, `ups.core`, `ups.coreOff`, `ups.coreN`, `ups.coreHint`, `ups.statusThreads`, `ups.statusCore`

- ASIO-kimenet (C rész bekötése): `ao.asio`, `ao.asioDriver`, `ao.asioHint`, `ao.asioNone`, `ao.asioFirst`, `ao.asioMissing`, `ao.asioCredit`, `ao.asioOn`, `ao.asioDsdNative`, `ao.asioDsdDop`, `ao.asioFallback`, valamint az `ao.asioErr_*` hibaokok (`not_open`, `no_driver`, `driver_not_found`, `invalid_clsid`, `com_create_failed`, `init_failed`, `channels`, `rate`, `format`, `buffer`, `create_buffers`, `busy`, `start_failed`, `reset`, `device_lost`, `hw_malfunction`, `no_dsp`, `open_failed`, `other`)

- Natív DSD (B rész): `ao.dsdNativeOn`, `ao.exclRate` (új); **módosult a szövege** (a 21 nyelvben még a régi, csak DoP-ról szóló változat áll): `ao.dsdDop`, `ao.dsdDopHint`
- Referencia előbeállítás (SRC): `ups.presetReference`, `ups.presetReferenceHint` (új)

Az `ao.asioCredit` védjegy-feltüntetés: a cégnév és a „védjegye és szoftvere” szerkezet maradjon meg minden nyelven.

Fordításnál ügyelj a változókra (`{rate}`, `{cpu}`, `{n}`), és hogy a szálak / dedikált mag szövege őszinte maradjon: nem ígér hangminőség-javulást.
