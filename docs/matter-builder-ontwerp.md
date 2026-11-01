# Matter Builder — een ESPHome-alternatief op basis van Matter

Status: ontwerp, nog geen code. Datum: 2026-10-29, bijgewerkt 2026-10-31 (lagen, beheer, adoptie en toegang) en 2026-11-01 (pakketten, catalogus, pagina's en flashen, §16).
Werktitel: **Matter Builder**. De naam ligt nog niet vast.
Referentietoepassing: deze repo (Octo-bridge F1, `firmware/bridge`, esp-matter release/v1.6, ESP-IDF v5.5.5).

## 1. Doel

Een reproduceerbare manier om ESP32-apparaten te maken die zich als **standaard Matter-apparaten** aanbieden, over Wi-Fi of Thread, afhankelijk van de chip. Het werkt net zo eenvoudig als ESPHome: je beschrijft het apparaat in YAML, een app in Home Assistant bouwt en flasht de firmware, en HA voegt het toe via de gewone Matter-integratie.

Uitgangspunten:
- Er is geen eigen HA-integratie of API-koppeling. HA ziet alleen het Matter-apparaat. Dat is dezelfde lijn als bij de G2H-camera.
- Je beschrijft Matter **device types**, geen HA-entities.
- Elke build is reproduceerbaar: vaste toolchainversies, een spec in git, en een manifest met hashes.
- **De gebruiker past geen bestanden aan.** Alles wat een gebruiker instelt, gaat via Matter of via de lokale instellingenpagina van het apparaat: Wi-Fi, het Thread-netwerk, wachtwoorden, en modi zoals de configuratiemodus. Gegevens die al in HA staan (de Thread-dataset, en de Wi-Fi-gegevens als HA die kent) neemt de add-on automatisch over. Waarden die in de firmware worden ingebakken, zijn hooguit een terugvaloptie voor ontwikkelaars.
- **Een proxy werkt autonoom.** Na de eerste installatie is HA niet nodig: instellingen, GPIO's in de instelbare laag (§6), Wi-Fi, Thread en updates gaan via de lokale pagina. HA is een extra beheerder, geen vereiste. De add-on is nodig voor een nieuwe proxy, een nieuwe gebouwde driver, een ander board of netwerktype, en een volledige herinstallatie.
- **Een gewone gebruiker bouwt niet.** Hij installeert de add-on, kiest kant-en-klare firmware (de algemene, instelbare firmware of een bekende toepassing zoals de Octo-bridge), installeert die via USB of over het netwerk, en stelt alles in op de pagina of in de add-on. Geen toolchain, geen wachten. Die firmware wordt per release reproduceerbaar gebouwd (vaste versies, zelfde toolchain als de add-on) en ondertekend.
- **Ontwikkelaarsmodus: HA als bouwer.** Wie een eigen driver of toepassing wil, zet de ontwikkelaarsmodus aan. De add-on downloadt dan de toolchain op vaste versies en bouwt zelf, met build- en crashlogs in beeld; een nieuwe Matter- of Thread-versie komt pas binnen als de add-on wordt bijgewerkt. Er komt geen externe webflasher: ook kant-en-klare firmware wordt door de add-on zelf geïnstalleerd.
- Octo is toepassing nr. 1. Een tweede, eenvoudige toepassing moet bewijzen dat de opzet generiek is.

Niet-doelen voor v1: Matter-certificering, een eigen Vendor ID, publicatie in de add-on store, en apparaten die niet in de Matter-device-types passen.

## 2. Waarom niet gewoon ESPHome

| | ESPHome | Matter Builder |
|---|---|---|
| Koppeling met HA | Eigen native API plus de ESPHome-integratie | Standaard Matter (python-matter-server) |
| Andere controllers | Alleen HA | Multi-admin: Apple Home, Google en HA tegelijk |
| Thread | Nee | Ja, op C6, H2 en C5 |
| Vrije entities (text, select, knop met naam) | Ja, alles kan | Nee, alleen wat Matter-clusters en de HA-Matter-integratie ondersteunen |
| Live logs, OTA | Ingebouwd | Zelf regelen (§8) |
| Firmwarefootprint | Klein | Groot (±1,5–1,8 MB, veel RAM) |

De ruil is dus: meer standaard en Thread, in ruil voor minder vrijheid in wat je kunt aanbieden.

## 3. Architectuur

```
 ┌──────────────── Home Assistant ──────────────────────────────────────┐
 │                                                                       │
 │  Matter Builder (app/add-on, Ingress)        Matter-integratie        │
 │   ├ editor (YAML) + secrets                   └ python-matter-server  │
 │   ├ generator  ── spec → ESP-IDF-project           ▲                  │
 │   ├ builder    ── ESP-IDF + esp-matter (vast)      │ commission       │
 │   ├ provisioner── factory-NVS, QR, creds ──────────┘ via IP           │
 │   ├ flasher    ── WebSerial (browser) / OTA                           │
 │   └ backups    ── flash read/restore                                  │
 │                                                                       │
 │  Thread/OTBR-integratie ─ dataset ─▶ provisioner                      │
 └───────────────────────────────────────────────────────────────────────┘
                     │ USB (WebSerial) of netwerk
                     ▼
     ┌──── ESP32-apparaat ───────────────────────────────┐
     │ gegenereerd: app_main, endpoints, sdkconfig        │
     │ runtime:     mb_core (capabilities, bindings, log) │
     │ componenten: drivers (octo_bed, gpio, i2c, …)      │
     │ factory-partitie: discriminator, passcode,         │
     │                   Wi-Fi-creds of Thread-dataset    │
     └────────────────────────────────────────────────────┘
```

Vier lagen:
1. **Spec** (YAML), §4
2. **Componentbibliotheek** (ESP-IDF-componenten met een vaste vorm), §5
3. **Generator** (spec → ESP-IDF-project), §6
4. **Builder-app** in HA (bouwen, provisionen, flashen, toevoegen), §7

## 4. De spec

### 4.1 Opbouw

```yaml
device:
  name: Bed
  board: esp32-c6            # board-profiel, zie §9
  transport: auto            # auto | wifi | thread
  bridge: true               # aggregator + bridged nodes
  vendor_id: 0xFFF1          # test-VID, zie §10
  product_id: 0x8001

network:
  wifi: !secret wifi         # alleen bij transport wifi
  thread: from_ha            # dataset uit de Thread-integratie van HA

components:
  - id: sam_bed
    driver: octo_bed
    ble: { mac: AA:BB:CC:DD:EE:01, pin: !secret octo_pin }
  - id: alex_bed
    driver: octo_bed
    ble: { mac: AA:BB:CC:DD:EE:02, pin: !secret octo_pin }

endpoints:
  - name: Sam
    bridged: true
    children:
      - { type: window_covering, source: sam_bed.head }
      - { type: window_covering, source: sam_bed.feet }
      - { type: on_off_light,    source: sam_bed.light }
  - name: Alex
    bridged: true
    children:
      - { type: window_covering, source: alex_bed.head }
      - { type: window_covering, source: alex_bed.feet }
      - { type: on_off_light,    source: alex_bed.light }

diagnostics:
  http_log: true             # alleen bij Wi-Fi zinvol
```

### 4.2 Regels
- `source: <component>.<capability>` koppelt een endpoint aan een capability van een component. De generator controleert of het capability-type past bij het device type: een `position` past bij `window_covering`, een `bool` bij `on_off_*`, enzovoort.
- `bridged: true` betekent een Bridged Node onder de Aggregator, met zijn eigen `NodeLabel`. Dat is wat de Octo-bridge nu per bed doet.
- `!secret` werkt zoals bij ESPHome: `secrets.yaml` staat naast de spec en komt nooit in git.
- Een spec zonder `bridge` geeft een gewoon apparaat met endpoints direct onder de root.

## 5. Componentbibliotheek

### 5.1 De vorm van een component
Elk component is een ESP-IDF-component met:

```
components/<naam>/
  component.yaml      # schema: config-velden, capabilities, eisen (ble, i2c, psram)
  include/<naam>.h
  <naam>.cpp          # driver
```

Een driver implementeert een klein, vast interface van `mb_core`:

```cpp
struct Capability {            // door de driver aangeboden
  const char* id;              // "head", "light", "temperature"
  CapType type;                // Position, Bool, Level, Temperature, Event, Mode …
  // driver → Matter: driver roept core.report(cap, value) aan
  // Matter → driver: core roept cap.on_write(value) aan
};

class Driver {
 public:
  virtual esp_err_t setup(const Config&) = 0;
  virtual std::span<Capability> capabilities() = 0;
  virtual void loop() {}         // optioneel, via mb_core-taak
};
```

### 5.2 Bindings (in `mb_core`, niet per driver)
Een binding koppelt een capability-type aan een Matter-cluster:

| Capability | Matter device type | Clusters / attributen |
|---|---|---|
| `Position` (0–100) | Window Covering | Lift, PositionAware; `TargetPositionLiftPercent100ths`, `CurrentPosition…`, `OperationalStatus` |
| `Bool` | On/Off Light / Plug-in Unit | OnOff |
| `Level` | Dimmable Light | OnOff + LevelControl |
| `Temperature` | Temperature Sensor | TemperatureMeasurement |
| `Humidity` | Humidity Sensor | RelativeHumidityMeasurement |
| `Contact` | Contact Sensor | BooleanState |
| `Occupancy` | Occupancy Sensor | OccupancySensing |
| `Event` (knop) | Generic Switch | Switch (momentary, events) |
| `Mode` (keuzelijst) | Mode Select | ModeSelect |

Het voordeel: de Matter-specifieke code (attribuutcallbacks, `OperationalStatus` bijwerken, reporting) staat maar één keer in de code. Nu zit dat bij Octo in `matter_bridge.cpp`.

### 5.3 Startset componenten
- `octo_bed`: driver uit `components/octo_protocol` + `components/octo_core/bed_link.cpp`, capabilities `head`, `feet`, `light`
- `gpio_switch`, `gpio_binary` (relais, contact, knop)
- `i2c_sht4x` of `i2c_bme280` (temperatuur en vochtigheid), als eenvoudige tweede toepassing
- `ble_central`: gedeelde NimBLE-laag voor drivers die BLE-apparaten aansturen. Die wordt uit `bed_link` gehaald.

## 6. Generator

Een CLI (`mb generate <spec>`), die later ook door de add-on wordt aangeroepen. De uitvoer is een volledig ESP-IDF-project:

| Bestand | Inhoud |
|---|---|
| `main/app_main.cpp` | init van NVS, netwerk, `mb_core`, drivers en Matter-node |
| `main/endpoints.cpp` | endpoint-opbouw via het dynamische datamodel van esp-matter (`endpoint::window_covering::create(…)`, `set_parent_endpoint`, zoals nu in `matter_bridge.cpp`), dus zonder ZAP |
| `main/CMakeLists.txt` | lijst met gebruikte componenten |
| `sdkconfig.defaults` | board-profiel + transport + featureflags (NimBLE, CHIPoBLE aan/uit, OpenThread) |
| `partitions.csv` | per board-profiel, met factory-NVS en eventueel twee OTA-slots |
| `build-manifest.json` | spec-hash, componentversies, toolchainversies |

Beslissingen:
- **Twee lagen.** Een spec beschrijft beide; de pagina past alleen de instelbare laag aan.
  - **Gebouwd:** specifieke drivers (zoals `octo_bed`), het board, het netwerktype en welke componenten er in de firmware zitten. Dit wordt gegenereerd en gebouwd door de add-on.
  - **Instelbaar:** standaardcomponenten die in de firmware zitten (schakelaar/relais, knop, contact, LED/PWM, een paar gangbare sensoren, eenvoudige BLE-schrijfacties). Welke pin, welk type en welke naam staat in NVS en wordt op de pagina ingesteld. De endpoints ontstaan bij het opstarten.
  - Er zit **geen YAML-parser** in de firmware. De pagina stuurt gestructureerde instellingen, en de firmware bewaart die compact in NVS.
- **De spec reist mee.** Elke proxy bewaart zijn spec en `build-manifest.json` in een eigen flash-partitie. Wijzigt iemand de instelbare laag op de pagina, dan werkt de proxy dat deel van zijn spec bij. Zo kan een add-on een proxy adopteren inclusief YAML, ook als hij op een ander systeem is gemaakt (§7.4).
- **Vaste endpointnummers.** Elk component heeft een vast id dat aan een endpointnummer gekoppeld blijft, zodat entiteiten in HA en Apple Home niet verschuiven bij een wijziging.
- **Veilige pinnen en toepassen met terugval.** Het board-profiel weet welke pinnen niet mogen (strapping, flash, USB); de pagina biedt alleen veilige pinnen aan, met een testknop per pin. Een nieuwe instelling wordt toegepast als proef: zonder bevestiging binnen 2 minuten, of na een crash, gaat de vorige terug (`boot_guard`).
- De generator is deterministisch: dezelfde spec en dezelfde versies geven exact hetzelfde project. In CI kun je dat met een diff controleren.
- Een gegenereerd project is ook zonder de builder te bouwen (`idf.py build`). Je zit dus niet vast aan de tool.

## 7. Builder-app in HA (add-on)

### 7.1 Image
- **Twee modi** (§1). Standaard is de add-on klein: hij haalt ondertekende, kant-en-klare firmware van de releases van het project en installeert die. De **ontwikkelaarsmodus** downloadt de toolchain naar `/data` (niet in de back-up) en bouwt zelf. In 0.1 zit ESP-IDF nog in het image zelf (`espressif/idf`, 3,6 GB); zodra er releases zijn, gaat ook ESP-IDF naar de download van de ontwikkelaarsmodus.
- **Releases** (uitgewerkt 2026-11-01): `.github/workflows/release.yml` bouwt bij een tag `v*` beide toepassingen met `ha-addon/matter_builder/{toolchain,build}.sh`, schrijft met `tools/release_manifest.py` een manifest (bestand, toepassing, versie, chip, grootte, SHA-256) en ondertekent dat met ECDSA P-256 (secret `MB_SIGNING_KEY`). De add-on controleert de handtekening met `release-key.pem` en per image de hash, en negeert een release die niet klopt. De privésleutel staat buiten de repo.
- **Releases** worden gebouwd in CI (GitHub Actions) met precies de toolchain van de add-on, per chip en per toepassing, met een manifest met hashes en een handtekening. Dezelfde bron en versies geven hetzelfde image; dat is te controleren door het in de ontwikkelaarsmodus na te bouwen.
- Gemeten (0.1, HA als amd64-VM): toolchain-installatie ±15 min, eerste build voor de ESP32-C6 ±35 min. Een volgende build hergebruikt build-map en ccache in `/data`. Bouwtijd hangt vooral af van het aantal processorkernen en het geheugen van de host (±1,5 GB per gelijktijdige compilatiestap); de add-on toont beide.
- Basis: Debian, met **vaste versies** van ESP-IDF v5.5.5, esp-matter release/v1.6 (vaste commit) en connectedhomeip-submodule op een vaste commit.
- Architecturen: amd64 en aarch64 (HA Green, Pi 5). De connectedhomeip-hosttools moeten op aarch64 bouwen. Dat is een risico om vroeg te testen.
- Grootte: verwacht 5–8 GB. Daarom de toolchain eventueel in een apart volume dat bij de eerste start wordt gedownload, en de add-on-image zelf klein houden.
- Caches: ccache en per board-profiel een voorgebouwd basisproject, zodat een build na een kleine wijziging minuten duurt in plaats van een half uur.

### 7.2 Ingress-UI
De indeling in pagina's is herzien in §16.4; de functies hieronder blijven, op hun nieuwe plek.

1. **Beheer** (overzicht van alle proxies): naam, chip, netwerk en signaal, firmwareversie, online of "afwijkend", en wat er is aangesloten. Een knop **"Wijziging naar alle proxies"** voor:
   - firmware-updates, eerst op één proxy als proef en daarna de rest één voor één, pas als de eerste gezond terugkomt;
   - Wi-Fi: de proxy test het nieuwe netwerk en houdt het oude als dat mislukt;
   - taal, tijdzone en NTP; logniveau en het doel voor live logs (§8.3);
   - configuratiemodus: time-out, en het setup-netwerk aan of uit met een wachtwoord;
   - tokens vernieuwen; Matter-onderhoud (koppelvenster openen, oude controllers verwijderen);
   - back-up van instellingen, terug te zetten op een vervangende proxy.

   Niet naar alle proxies: de Thread-dataset (Thread verspreidt een nieuw netwerk zelf met een pending dataset; de add-on toont alleen een afwijking) en instellingen die per apparaat verschillen (bed-MAC, PIN, looptijden).
2. **Per proxy**: dezelfde gegevens in een **UI-modus** en een **YAML-modus**, zoals de automatiseringseditor van HA. De YAML heeft twee blokken: *instellingen* (instelbare laag, direct actief) en *firmware* (gebouwde laag, vraagt een build en OTA). Voor het toepassen toont de add-on een diff met "direct" of "build nodig". De add-on bewaart de gewenste stand, de proxy meldt de werkelijke; is er op de pagina iets gewijzigd, dan toont de add-on "gewijzigd op apparaat" en kies je overnemen of terugzetten.
3. **Sleutels**: per proxy wat de add-on heeft gekregen, achter een oogje met een kopieerknop: het token van de add-on (opnieuw koppelen, intrekken) en de Matter-koppelcode met QR. Eén keer, voor alle proxies: Wi-Fi en de Thread-dataset (alleen-lezen, uit de Thread-integratie). Het paginawachtwoord staat hier niet: dat is van de gebruiker en blijft op de proxy. In de YAML staat overal `!secret`, nooit de waarde.
4. **Build**: logstream, met als resultaat `firmware.bin`, `factory.bin` en het manifest.
5. **Flash**: WebSerial in de browser (esptool-js), zoals ESPHome, en altijd eerst een volledige flash-backup (§11).
6. **Toevoegen**: QR-code en de handmatige koppelcode, plus een knop "Toevoegen aan HA" (§7.3).
7. **Onderhoud**: logs, backup en restore, fabrics bekijken.

### 7.3 Provisioning en commissioning
De add-on maakt per apparaat een **factory-NVS-partitie** (met esp-matter `mfg_tool`):
- een unieke discriminator en een willekeurige passcode, in plaats van de testcode 20202021 die de Octo-bridge nu gebruikt;
- test-DAC/PAI voor de test-VID;
- **netwerkgegevens**:
  - Wi-Fi: SSID en wachtwoord uit `secrets.yaml`;
  - Thread: de *preferred dataset* opgehaald uit de Thread-integratie van HA (OTBR), via de HA-API vanuit de add-on.

Het apparaat staat daardoor direct na het flashen op het netwerk en is commissionable via DNS-SD. De add-on vraagt daarna de Matter-server om **on-network commissioning** met de bekende passcode. Je hebt geen telefoon en geen BLE-commissioning nodig. Bij de Octo-bridge werkt dit nu al zo voor Wi-Fi (CHIPoBLE uit).

Terugvaloptie: CHIPoBLE aan, en koppelen via de HA-companion-app met de QR-code. Dat is een optie per spec (`commissioning: ble`).

Te controleren in F3: of de websocket-API van matterjs-server (`/ws`, poort 5580; zie §8.1) het commissionen via IP met alleen een setup-code ondersteunt, en hoe de add-on die API bereikt.

### 7.4 Adoptie en toegang
- **Het paginawachtwoord is van de gebruiker.** Hij kiest het bij het eerste bezoek op de pagina (Octo 1.0.4). Het is het enige geheim dat een mens nodig heeft.
- **Elk programma krijgt een eigen token**, uitgegeven door de proxy: de add-on, of later een eigen script. De proxy bewaart alleen een hash in NVS. Op de pagina staat de lijst ("Home Assistant, sinds 31 okt") met een knop **Intrekken**.
- **Koppelen vraagt wachtwoord én fysieke aanwezigheid.** De add-on vraagt een koppeling aan met het paginawachtwoord; de proxy opent dan 2 minuten een koppelvenster, en geeft pas een token als binnen die tijd op de BOOT-knop wordt gedrukt. Is de knop niet bereikbaar, dan telt drie keer kort de stroom aan en uit als alternatief. "Al Matter-beheerder in HA" telt **niet** als bewijs: zonder fysieke stap geen adoptie.
- **Adopteren** (een proxy die elders is gemaakt): na het koppelen leest de add-on spec, manifest en instellingen van de proxy (§6), en kan hem daarna identiek opnieuw bouwen. Met de adoptie gaat ook het recht om ondertekende firmware te sturen over naar de nieuwe eigenaar, onder dezelfde voorwaarden.
- **Wachtwoord vergeten**: met de proxy in de hand. De BOOT-knop een paar seconden vasthouden opent de herstelstand (setup-netwerk, nieuw wachtwoord kiezen); bedden, Matter-koppelingen en netwerk blijven staan. Tokens van programma's blijven geldig.

## 8. OTA, logs en diagnostiek

Uitgangspunt (onderzocht 2026-10-31): **HA is verwerker, de ESP blijft dom.** Alles wat zwaar is, gebeurt in de add-on: het image maken, versies bijhouden, logs decoderen, crashes analyseren. De ESP verstuurt alleen ruwe bytes.

### 8.1 Context: de Matter-server van HA is veranderd
Sinds Matter Server add-on 9.0 (juni 2026) draait HA op **matterjs-server** (TypeScript, matter.js). python-matter-server is gearchiveerd. Alles in dit ontwerp richt zich dus op matterjs-server. Dat geldt ook voor de commissioning-API uit §7.3.

### 8.2 OTA: via de gewone Matter-update in HA (voorkeur)
matterjs-server heeft een ingebouwde OTA-provider die ook **lokale `.ota`-bestanden** accepteert:
- `--ota-provider-dir <pad>` plus `--enable-test-net-dcl`. Volgens de README zijn beide nodig voor eigen bestanden. De server importeert de `.ota`-bestanden bij het starten en verwijdert ze daarna.
- Er is ook een upload-mechanisme (`--ota-upload-max-size-mb`, standaard 64 MB, staging in `ota-uploads/`). Daarmee kan de add-on mogelijk uploaden zonder gedeelde map en zonder herstart. Uitzoeken in F4.
- HA toont dan een gewone **update-entity** bij het apparaat. Dit werkt over **Wi-Fi én Thread**, want BDX loopt over de Matter-sessie, en er is geen extra code in de firmware nodig. De OTA-requestor zit standaard in esp-matter.

Wat de add-on daarvoor doet:
1. Bij elke build `SoftwareVersion` automatisch ophogen. Een update wordt alleen aangeboden en geaccepteerd als die hoger is.
2. Met `ota_image_tool.py` (connectedhomeip) een `.ota`-bestand maken met de juiste VID/PID en versie.
3. Het bestand in de provider-dir zetten of uploaden.

Nog te controleren:
- ~~of de Matter Server add-on van HA deze opties aanbiedt~~ **Gecontroleerd 2026-10-31**: in jouw HA draait Matter Server add-on 9.3.0 met `enable_test_net_dcl: true` (staat al aan). `--ota-provider-dir` kun je meegeven via de optie `matter_server_args`. Wel een probleem: die map moet in de container van de Matter-server staan, en daar kan een andere add-on niet zomaar bij. De **upload-API** van de server is daarom de betere route, en die moet nog worden uitgezocht;
- of test-VID `0xFFF1` met een lokaal bestand werkt. Google ondersteunt dat niet, maar matter.js is een andere implementatie.

**Zonder HA**: Apple Home werkt onze proxies niet bij. Apple zoekt updates in de productie-DCL, en daar staan alleen gecertificeerde producten met een eigen Vendor ID; test-VID `0xFFF1` staat alleen in de test-DCL. De grootte van de update maakt daarvoor niet uit. Zonder HA gaat een update daarom via de pagina: "Firmware bijwerken" met een bestand. Op Thread schakelt de proxy daarvoor zelf naar de configuratiemodus op Wi-Fi (upload ±20 s in plaats van ruim een uur) en daarna terug. `boot_guard` blijft het vangnet.

Terugvaloptie: HTTP-OTA vanuit de add-on naar `http_api` (de Octo-bridge heeft `/ota` al). Dat is snel op Wi-Fi, en op Thread mogelijk maar traag.

**Delta-OTA** (`esp_delta_ota` uit idf-extra-components): de add-on maakt een patch ten opzichte van de vorige build. Die heeft hij toch, door het manifest. Dat scheelt vooral op Thread veel zendtijd. Het bespaart **geen** flashruimte, want het oude image moet intact blijven tot de nieuwe klaar is. Dit is iets voor later. Eerst moet volle OTA werken.

### 8.3 Live logs
Er zijn drie niveaus, alle drie met de verwerking in de add-on:

| Niveau | Hoe | Wi-Fi | Thread |
|---|---|---|---|
| **Live** | De firmware stuurt logregels als UDP-pakketjes naar de add-on (syslog-achtig, IPv6). De add-on toont ze in de UI, zoals `esphome logs`. | ja | ja (HA-host is via OTBR bereikbaar, laag volume) |
| **Op aanvraag** | Matter **Diagnostic Logs cluster** (0x0032): `RetrieveLogsRequest`, met kleine logs inline en grote via BDX. Een ringbuffer in de firmware. | ja | ja |
| **Na een crash** | ESP-IDF core dump naar een flash-partitie, opgehaald na de herstart (HTTP of Diagnostic Logs met intent `CrashLogs`). De add-on decodeert met de ELF van precies die build (manifest). | ja | ja |

**Tokenized logging** past perfect bij "HA verwerkt": met `pw_tokenizer` (Pigweed, zit al in connectedhomeip) worden logteksten tijdens het bouwen vervangen door 4-byte tokens. De teksten staan in een tokendatabase in de add-on, en de ESP verstuurt alleen tokens en argumenten. Pigweed noemt ±90% minder flash per logbericht en ±70% minder bytes over de lucht. Dat is ideaal voor Thread, en het ruimt flash op (§8.5). Een kanttekening: dit werkt alleen voor onze eigen logs. Voor de interne logs van ESP-IDF en CHIP is het veel meer werk en dat valt buiten v1.

Of matterjs-server het Diagnostic Logs cluster als client ondersteunt, is niet gevonden. Daarom doet de add-on het zelf: die is via de Matter-server-API een controller op de fabric en kan het commando sturen. Ook dit is uitzoekwerk. Live via UDP heeft daarom voorrang.

### 8.4 Diagnostiek in HA
In HA verschijnt alleen wat Matter standaard biedt (Basic Information, General Diagnostics, Wi-Fi/Thread Network Diagnostics). Heap of uptime kunnen niet als entity in HA verschijnen. De add-on-UI toont die wel, op basis van de live logs of een kleine statusregel in de UDP-stroom.

### 8.5 Firmwaregrootte

**Meting** van de Octo F1-bridge (klassieke ESP32, 4 MB, slots van 1,84 MB): het image is **1,76 MB, dus 96% van een app-slot**. De statische DRAM is 108 KB (87%).

| Blok | Flash |
|---|---|
| Matter (`libCHIP` + `libesp_matter`) | 425 KB |
| Wi-Fi (`net80211`, `pp`, `phy`, `wpa_supplicant`) | 280 KB |
| BLE (`bt`, `btdm_app`) | 176 KB |
| `libstdc++` | 143 KB |
| ±150 KB rodata toegeschreven aan `esp_app_desc.c` | waarschijnlijk samengevoegde stringconstanten (logteksten e.d.) van de hele firmware |
| libc, lwip, mbedtls, FreeRTOS, … | ±300 KB |
| **Octo zelf** (`octo_core` + `main`) | **66 KB (±4%)** |

**Conclusie over het idee om logica naar HA te verplaatsen:** de toepassingslogica is maar ±4% van het image. Het image is groot omdat de Matter-stack en de radio's groot zijn. Logica naar HA verplaatsen maakt het dus nauwelijks kleiner. Bovendien heb je dan een ander protocol tussen ESP en HA nodig, en dan ben je weer bij ESPHome of een eigen integratie. Bij een volledig "dunne" ESP draait Matter op de HA-host (een bridge zoals Matterbridge of de HA Matter Hub), maar HA praat dan met zichzelf. Dat is alleen zinvol voor Apple of Google.

Waar HA-zijdige verwerking **wel** helpt:
1. **Codegeneratie (al in het ontwerp).** Alleen de clusters en device types uit de spec gaan mee. esp-matter heeft daar een gedocumenteerde optie voor ("Excluding unused Matter clusters").
2. **Tokenized logs (§8.3)**: logteksten verdwijnen uit de flash.
3. **Decoderen op HA**: geen symbolen of tekstuele foutmeldingen op het apparaat nodig.
4. **De generator kiest per board-profiel automatisch de bekende besparingen.** Cijfers van Espressif (ESP32-C3, light-example):

| Instelling | Effect |
|---|---|
| `CONFIG_ENABLE_CHIP_SHELL=n` | −66 KB flash, +10 KB heap |
| `CONFIG_NEWLIB_NANO_FORMAT=y` | −47 KB flash |
| Max. dynamische endpoints en device types op het werkelijke aantal | ±550 B RAM per stuk |
| NimBLE trimmen: alleen de rollen die nodig zijn, max. verbindingen | −22 KB flash, +19 KB heap bij boot |
| Kleinere event-logbuffers en wachtrijen | +6,5 KB heap (risico: gemiste events) |

Daarnaast: lagere `LOG_DEFAULT_LEVEL` in releasebuilds, assertions op niveau 1, en code van IRAM naar flash verplaatsen.

5. **Partities slimmer indelen.** Op 4 MB gaat nu bijna alles naar twee app-slots. Als NVS en coredump erbij moeten, moet er per profiel een rekensom gemaakt worden. De builder weigert een build die niet past, net als ESPHome.

Eindoordeel: de klassieke ESP32 met Wi-Fi + BLE-central + Matter blijft het randgeval. Met de bovenstaande maatregelen schat ik dat er 150–250 KB vrij te maken is. Het **advies blijft: C6 (8 MB) als standaard en S3 voor zware BLE-bridges.** Voor C6 heb je al een partitietabel met slots van 3 MB.

## 9. Board-profielen en transport

| Profiel | Transport | Flash/PSRAM | Geschikt voor |
|---|---|---|---|
| `esp32` (classic) | Wi-Fi | 4 MB / – | Alleen lichte toepassingen. De Octo-bridge haalt het maar net (heap ±33 KB, dip naar 14 KB bij commissioning). |
| `esp32-s3-n16r8` | Wi-Fi | 16 MB / 8 MB | Zware toepassingen: BLE-bridges, veel endpoints |
| `esp32-c6` | Wi-Fi **of** Thread | 4–8 MB / – | **Standaardboard**: sensoren, relais, kleine bridges |
| `esp32-h2` | Thread | 4 MB / – | Zuinige sensoren (eventueel Thread SED) |
| `esp32-c5` | Wi-Fi 2,4/5 GHz of Thread | 4–8 MB / – | Optioneel, later |

`transport: auto` kiest Thread als de chip dat kan en HA een Thread-dataset heeft, en anders Wi-Fi. Wi-Fi en Thread tegelijk (C6) wordt niet ondersteund. Een apparaat kiest er één.

Elk profiel legt vast: target, partitietabel, sdkconfig-fragment, BLE-limieten, en of OTA-slot B past.

## 10. Identiteit en certificaten

- Test-VID `0xFFF1`, PID per toepassing (vaste tabel in de repo), test-DAC per apparaat.
- Gevolg: HA en Apple tonen het apparaat als ongecertificeerd. Voor eigen gebruik is dat prima.
- Het aantal fabrics is dynamisch, dus nooit hardcoden (G2H-les).

## 11. Veiligheid en herstel

- Altijd eerst een volledige flash-backup (`esptool read_flash 0 ALL`) vóór de eerste Matter-flash op een bestaand apparaat. Dit generaliseert `tools/backup_flash.sh` en `restore_flash.sh`. Backups gaan naar `/share/matter-builder/backups/`, nooit in git.
- USB-snelheid per board instelbaar (de klassieke ESP32 van de proxy vraagt 115200 baud).
- Een `boot_guard` (al aanwezig in `octo_core`) wordt een standaardonderdeel van `mb_core`: na herhaalde crashes start het apparaat in een veilige modus met alleen netwerk en OTA.
- De BOOT-knop heeft drie duidelijk verschillende functies, zodat je niet per ongeluk alles wist:

  | Druk | Functie |
  |---|---|
  | kort | koppelvenster bevestigen (§7.4), alleen als er een aanvraag openstaat |
  | 5 s vasthouden | herstelstand: setup-netwerk, nieuw paginawachtwoord |
  | 10 s vasthouden | fabrieksreset volgens Matter |

  Drie keer kort de stroom aan en uit (binnen 10 s per keer) telt als korte druk, voor apparaten waarvan de knop niet bereikbaar is.
- Alleen ondertekende firmware wordt geaccepteerd zodra adoptie bestaat (§7.4).

## 12. Hoe de Octo-repo hierop aansluit

| Nu (octo-matter) | Straks (Matter Builder) |
|---|---|
| `components/octo_protocol` | `components/octo_bed` (driver), ongewijzigd protocol |
| `components/octo_core/bed_link.cpp` | `components/ble_central` (generiek) + het Octo-deel in `octo_bed` |
| `components/octo_core/boot_guard.cpp` | `mb_core` |
| `components/octo_core/http_api.cpp` | `mb_core` diagnostics/OTA (Wi-Fi) |
| `firmware/bridge/main/matter_bridge.cpp` | gegenereerde `endpoints.cpp` + generieke bindings in `mb_core` |
| `firmware/bridge/main/secrets.h` | `secrets.yaml` + factory-NVS |
| `firmware/bridge/sdkconfig.defaults` | board-profiel `esp32` + gegenereerd fragment |
| `tools/backup_flash.sh` / `restore_flash.sh` | functie in de add-on |
| handmatige commissioning, passcode 20202021 | unieke passcode + on-network commissioning |

Acceptatiecriterium voor F1: de Octo-bridge die uit `octo.yaml` wordt gegenereerd, gedraagt zich in HA identiek aan de huidige F1-firmware: dezelfde endpoints, covers, lichten en NodeLabels.

## 13. Fasering

| Fase | Inhoud | Klaar als |
|---|---|---|
| **F0** Herstructureren | `mb_core` en `octo_bed` uit de huidige code halen, geen nieuwe functies | Huidige Octo-firmware bouwt en werkt op de nieuwe structuur |
| **F1** Generator-CLI | `mb generate octo.yaml` → project | Gegenereerde Octo-firmware gedraagt zich identiek (§12) |
| **F2** Tweede toepassing | C6 met SHT4x + relais op Thread | Werkt in HA via Thread, zonder Octo-specifieke code in `mb_core` |
| **F3** HA add-on | build, WebSerial-flash, factory-NVS, QR, on-network commissioning (Wi-Fi + Thread-dataset uit HA) | Nieuw apparaat van spec tot HA-entity zonder terminal |
| **F4** OTA en logs | `.ota` via matterjs-server (update-entity in HA), UDP-logstream, coredump-decoder, daarna tokenized logs | Update van Octo-bridge zonder USB via HA, live logs in de add-on |
| **F5** Afronding | documentatie, componentschema's, CI met reproduceerbaarheidscheck | Een derde toepassing kan worden toegevoegd door alleen een component te schrijven |

## 14. Open beslissingen

1. **Repo**: een nieuwe, generieke repo (voorstel) met Octo als voorbeeldtoepassing, of verder in `octo-matter`?
2. **Thread meteen in v1**, met de C6 als standaard (voorstel), of eerst alleen Wi-Fi?
3. **Publicatie**: alleen voor eigen HA (voorstel voor v1), of later als publieke add-on? Dat bepaalt of een eigen Vendor ID de moeite waard is.
4. **Naam**.

Genomen (2026-10-31):
- **Autonoom, met HA als extra**: een proxy werkt na installatie zonder HA (§1).
- **Twee lagen** in plaats van alleen code genereren (§6).
- **Koppelen** met paginawachtwoord en een fysieke handeling; Matter-beheerder zijn is geen bewijs (§7.4).
- **Gewone gebruiker installeert kant-en-klare, ondertekende firmware; bouwen in HA is de ontwikkelaarsmodus**; geen externe webflasher (§1).

Genomen (2026-11-01):
- **Drie lagen**: standaardfirmware, pakketten, ontwikkelaarsmodus (§16.1). De Octo-bridge is een pakket, niet de kern.
- **Pakketbronnen met een eigen sleutel**: specifieke projecten onderhoudt de maker zelf, niet het project (§16.2).
- **Catalogus** zonder server, via een GitHub-onderwerp en GitHub Pages (§16.3).
- **Drie pagina's**: Home, Maken, Catalogus, plus Instellingen; de catalogus komt uit op Maken (§16.4).

## 15. Risico's

| Risico | Impact | Mitigatie |
|---|---|---|
| connectedhomeip-hosttools bouwen niet op aarch64 | Add-on werkt niet op HA Green/Pi | Vroeg testen in F3, eventueel builds op een andere machine (remote builder) |
| Image/toolchain te groot voor HA-host | Installatie faalt of de schijf loopt vol | Toolchain als los volume, optioneel externe builder |
| On-network commissioning niet via de Matter-server-API | Telefoon blijft nodig | Terugvaloptie BLE via de companion app |
| Lokale OTA met test-VID werkt niet in matterjs-server, of de HA-add-on biedt de opties niet | Updates alleen via HTTP of USB | HTTP-OTA als terugvaloptie (§8.2) |
| Image past niet in een slot (Octo nu 96%) | Geen OTA meer mogelijk | Besparingen uit §8.5, groottecheck in de builder, C6/S3 |
| Device-types dekken een toepassing niet | Functie niet zichtbaar in HA | Bij het ontwerp van de spec aangeven; geen vendor-clusters als oplossing (HA toont ze niet) |
| RAM op klassieke ESP32 | Instabiel bij commissioning | Board-profiel met limieten, aanbeveling C6/S3 |

## 16. Pakketten, catalogus en flashen (2026-11-01)

### 16.1 Drie lagen
1. **Standaardfirmware (`generic`)**: het eigenlijke product. Eén ondertekende firmware per chip, met algemene bouwstenen (relais, LED, knop, contact, beweging, SHT4x; later bijvoorbeeld PWM/dimmer, analoge ingang, algemene I2C-sensoren, UART). Ingesteld via YAML in de add-on of op de lokale pagina, zonder compileren. Dit dekt de meeste gebruikers.
2. **Pakketten**: firmware met eigen code voor wat `generic` niet kan, zoals een BLE-protocol (de Octo-bedden), IR of een eigen seriële koppeling. Elk pakket wordt in CI gebouwd en ondertekend en als kant-en-klare firmware aangeboden. Ook hier stelt de gebruiker alles in zonder te bouwen.
3. **Ontwikkelaarsmodus**: lokaal bouwen in HA, voor eigen code die nog geen pakket is.

Grens tussen 1 en 2: `generic` groeit alleen met breed bruikbare bouwstenen (en heeft ruimte nodig; nu ±64 % van het app-slot). Protocollen voor één soort apparaat worden een pakket.

### 16.2 Pakketbronnen
- **Officieel**: deze repo, ondertekend met de sleutel van het project. Inhoud: `generic` en een paar voorbeeldpakketten (Octo).
- **Community**: wie iets specifieks wil, maakt een eigen repo uit een **sjabloonrepo** (bouw-workflow, pakketstructuur, `release_manifest.py`), maakt een eigen sleutel en tagt een release. GitHub bouwt en ondertekent; het project reviewt, bouwt en ondertekent niets.
- In de add-on voeg je een bron toe zoals een custom repository in HACS. De add-on onthoudt bij het toevoegen de publieke sleutel van die bron en accepteert daarna alleen releases met die sleutel. De UI toont steeds **officieel** of **community** en wie de maker is.
- Vorm van een pakket: `packages/<naam>/package.yaml` met naam, beschrijving, maker, chip(s), de versie van het pakketformaat waarop het bouwt, en een **schema** van de instellingen. De add-on maakt uit dat schema het formulier en de YAML-controle; er is geen UI per pakket. Code in `components/<naam>`, bovenop `mb_core`.
- **Onderhoud voor het project**: `mb_core` en het pakketformaat. Dat formaat krijgt een versienummer en blijft stabiel, zodat community-pakketten blijven werken.

Apparaat-YAML (voorbeeld):
```yaml
device:
  name: Tuinhuis
package: generic          # of: octo-bed, of <bron>/<pakket>
connections:              # voor generic
  - type: relay
    pin: 4
    name: Lamp
settings: {}              # volgens het schema van het pakket
```

### 16.3 Catalogus (marketplace)
- Delen gebeurt op twee manieren:
  - **Recepten**: alleen een YAML voor `generic`. Daar zit geen code in, dus geen risico en geen bouwwerk. Dit wordt naar verwachting het meeste.
  - **Pakketten**: firmware uit een bron (§16.2).
- Een bron of recept doet mee met het GitHub-onderwerp `matter-builder-package`.
- Een nachtelijke GitHub Action in deze repo zoekt die repo's, controleert hun ondertekende manifest en schrijft één `catalog.json` naar GitHub Pages. De add-on haalt alleen dat bestand op, dus de API-grens van GitHub speelt niet.
- Per item: naam, beschrijving, maker, GitHub-sterren, downloads (uit GitHub), officieel/community, en een badge **geverifieerd** voor wat het project zelf heeft bekeken (optioneel).
- Moderatie: een **blokkeerlijst** in deze repo; bij de volgende run verdwijnt het item. Ondersteuning en meldingen lopen via de issues van de maker.
- Kosten: geen. Grenzen van GitHub (publieke repo, gratis account): onbeperkte Actions-minuten, 6 uur per job, 20 jobs tegelijk, 10 GB cache, 2 GB per bestand en 1000 bestanden per release, geen grens op downloads.
- Bescherming blijft: handtekening per bron, adoptie met wachtwoord en BOOT-knop (§7.4), proefperiode en terugdraaien bij `generic`.

### 16.4 Pagina's van de add-on
1. **Home**: eigen proxies (status, versie, update, "gevonden, nog niet gekoppeld") en een kaart **Nieuw apparaat** met *Zelf maken* en *Uit catalogus*. Een proxy openen geeft zijn **detailpagina** (aansluitingen in UI/YAML, update, token).
2. **Maken**: beginnen met leeg, een voorbeeld of een catalogus-item; instellen in UI of YAML; **Controleren**; **Installeren** (USB op een nieuwe ESP32, of toepassen op een bestaande proxy). In ontwikkelaarsmodus ook **Bouwen**.
3. **Catalogus**: zoeken en filteren (recepten, pakketten, officieel, community), detail per item. **Gebruiken** opent Maken met het item ingevuld: er is één installatieweg.
4. **Instellingen** (tandwiel): tokens achter een oogje, Wi-Fi en taal voor alle proxies, pakketbronnen, ontwikkelaarsmodus en toolchain.

### 16.5 Installeren zonder bouwen
- Bouwen kan alleen in de ontwikkelaarsmodus, maar **flashen kan iedereen**.
- De release bevat per pakket een updatebestand en een **fabrieksbestand** (bootloader, partitietabel en app in één), zodat een lege ESP32 rechtstreeks uit de release kan.
- Bij `generic` betekent "installeren": de standaardfirmware flashen, of laten staan als hij er al op staat. Daarna zet de add-on naam en aansluitingen uit de YAML erop, met de bestaande proefperiode.
- Flashen via USB kan op drie manieren, in deze volgorde van voorkeur:
  1. **In de browser** vanuit de add-on (esptool-js, meegeleverd). Dit vraagt HTTPS en Chrome of Edge.
  2. **Op de HA-host**: de ESP32 in de HA-machine prikken, en de add-on flasht zelf met esptool. Dit werkt ook als HA via gewoon http wordt geopend. De add-on moet daarvoor bij de seriële poorten kunnen.
  3. **Zonder HA**: een flashpagina van het project op GitHub Pages. Daarop staat dezelfde esptool-js, en de pagina controleert in de browser de handtekening en de hash (WebCrypto, P-256). Dat past bij "autonoom zonder HA" en is geen externe webflasher: de pagina komt uit deze repo. De firmware moet dan ook op Pages staan, want releasebestanden zijn waarschijnlijk niet vanuit een webpagina op te halen (CORS); dat is nog te controleren.
- Er komt geen afhankelijkheid van web.esphome.io of de flasher van Espressif. Iemand kan het fabrieksbestand wel altijd zelf flashen met esptool.

### 16.6 Volgorde van uitvoeren
1. Fabrieksbestanden en pakketinformatie in de release en het manifest.
2. Installeren zonder bouwen voor `generic` (§16.5), met de pagina's van §16.4.
3. Flashen op de HA-host (§16.5, manier 2).
4. Octo omzetten naar `packages/octo-bed` en het pakketformaat met schema.
5. Pakketbronnen in de add-on, plus de sjabloonrepo.
6. Catalogus: de Action, `catalog.json` op Pages, de blokkeerlijst en de pagina Catalogus.
7. Flashpagina op GitHub Pages (§16.5, manier 3).
8. Documentatie: "een pakket maken" en "een recept delen".
