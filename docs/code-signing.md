# Kódaláírás a SignPath Foundationnel

A Windows kiadásokat a [SignPath Foundation](https://signpath.org/) ingyenes, nyílt forráskódú projekteknek szóló
tanúsítványával írjuk alá. A privát kulcs a SignPath hardveres kulcstárában (HSM) marad; a tárolóba titok csak
API-token formájában kerül. Kiadóként a Windows a „SignPath Foundation” nevet mutatja.

## 1. Jelentkezés (egyszer)

1. Jelentkezés: <https://signpath.org/apply> (projekt: `Szorgos/ee-phase-waterfall-analyzer`, licenc: GPL-3.0).
   A feltételek: OSI-licenc, nincs zárt forrású komponens, aktívan karbantartott projekt, már van kiadása, és a
   honlapon (README) szerepel a kódaláírási szabályzat (megvan: README → „Kódaláírási szabályzat”).
2. Elfogadás után a SignPath felületén:
   - **Projekt** slug: `aqulify` (ha más, add meg a `SIGNPATH_PROJECT_SLUG` változóban).
   - **Trusted build system:** GitHub.com, a tárolóhoz rendelve.
   - **Aláírási szabály (signing policy):** `release-signing` (alapértelmezett név), kézi jóváhagyással.
   - **Két artifact configuration** (lent): `binaries` és `installer`.
   - **API token:** egy CI felhasználóhoz, „Submitter” joggal a `release-signing` szabályra.

## 2. GitHub beállítások

Settings → Secrets and variables → Actions:

| Típus | Név | Érték |
|---|---|---|
| Secret | `SIGNPATH_API_TOKEN` | a SignPath CI felhasználó API tokenje |
| Variable | `SIGNPATH_ORGANIZATION_ID` | a SignPath szervezet azonosítója |
| Variable (opcionális) | `SIGNPATH_PROJECT_SLUG` | ha nem `aqulify` |
| Variable (opcionális) | `SIGNPATH_SIGNING_POLICY_SLUG` | ha nem `release-signing` |

Amíg a `SIGNPATH_API_TOKEN` nincs beállítva, a Release workflow aláíratlan telepítőt készít, mint eddig.

## 3. Artifact configuration-ök

`binaries` – a motor, a felület és az Equalizer APO plugin (a GitHub artifact egy zip):

```xml
<?xml version="1.0" encoding="utf-8"?>
<artifact-configuration xmlns="http://signpath.io/artifact-configuration/v1">
  <zip-file>
    <pe-file path="Aqulify.exe"><authenticode-sign/></pe-file>
    <pe-file path="AqulifyGui.exe"><authenticode-sign/></pe-file>
    <pe-file path="Aqulify.dll"><authenticode-sign/></pe-file>
  </zip-file>
</artifact-configuration>
```

`installer` – a telepítő (a fenti, már aláírt fájlokat tartalmazza):

```xml
<?xml version="1.0" encoding="utf-8"?>
<artifact-configuration xmlns="http://signpath.io/artifact-configuration/v1">
  <zip-file>
    <pe-file path="Aqulify-Setup.exe"><authenticode-sign/></pe-file>
  </zip-file>
</artifact-configuration>
```

## 4. Kiadás

Ugyanúgy, mint eddig: verzió a `CMakeLists.txt`-ben, `RELEASE_NOTES.md`, majd Actions → Release → Run workflow.
A workflow két aláírási kérést küld (programfájlok, majd telepítő); mindkettőt a SignPath felületén kell
jóváhagyni, a workflow addig vár (legfeljebb 10 percig kérésenként).
