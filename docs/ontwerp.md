# Octo Bed → Matter bridge — ontwerp

Status: F0–F2 gebouwd (2026-10-29). Afwijkingen van dit ontwerp staan in §13.
Bron voor protocol en gedrag: `~/Documents/GitHub/octo-bed` v2.9.2 (`protocol.py`, `const.py`, `octo_bed_client.py`).

## 1. Doel

Eén ESP32 naast de bedden die de Octo-bedden via BLE aanstuurt en ze als **standaard Matter-apparaten** aanbiedt. Home Assistant (en eventueel Apple Home/Google) koppelt via de gewone Matter-integratie. Geen ESPHome, geen Bluetooth-proxy, geen HACS-integratie meer nodig.

Niet-doelen (voor v1): Matter-certificering, ondersteuning van andere bedmerken, Thread (zie §3).

## 2. Architectuur

```
 Home Assistant ──(Matter over Wi-Fi/IPv6)──┐
 Apple Home (optioneel, multi-admin) ───────┤
                                            ▼
                    ┌──── ESP32 (ex-proxy Bedroom) "Octo Matter Bridge" ────┐
                    │  Matter-laag (esp-matter / connectedhomeip)           │
                    │    Aggregator ─ bridged endpoints per bed + "Beide"   │
                    │  Bed-laag                                             │
                    │    BedLink ×2  (NimBLE central, PIN, keep-alive)      │
                    │    Motion  ×2  (tijdgestuurde positie, herhaalcmd's)  │
                    │  Config-laag                                          │
                    │    NVS (PIN, MAC, reistijden, posities, presets)      │
                    │    Lokale web-UI (scan, PIN, kalibratie, update)      │
                    └───────────────┬───────────────────────┬───────────────┘
                                    │ BLE                   │ BLE
                               Bed links (RC2)         Bed rechts (RC2)
```

Kernidee: alle bedlogica die nu in Python in HA draait (connection manager, PIN keep-alive, positie-schatting, kalibratie, presets, "beide bedden") verhuist naar de firmware. HA ziet alleen standaard clusters.

## 3. Hardware

**Keuze: de bestaande "Bluetooth Proxy Bedroom" wordt opgeofferd.** Volgens HA is dat een klassieke ESP32 (model `esp32`, MAC `24:6f:28:5f:03:7c`, ESPHome 2026.10.0), die al op de juiste plek bij de bedden staat.

Gevolgen van de klassieke ESP32 (520 KB SRAM, meestal 4 MB flash, geen PSRAM):

| Punt | Gevolg | Aanpak |
|---|---|---|
| Flash (waarschijnlijk 4 MB) | Een Matter-app is ±1,5–1,8 MB; twee OTA-slots passen net | Flashgrootte eerst uitlezen (`esptool flash_id`); eigen partitietabel; zonodig v1 zonder OTA-slot B |
| RAM | Matter + Wi-Fi + NimBLE dual-role + 2 links + webserver is krap | NimBLE (niet Bluedroid), max. 3 BLE-verbindingen, kleine buffers; web-UI minimaal; heap meten in F0/F1 |
| Alleen Wi-Fi (geen Thread) | Geen probleem; web-UI is daardoor wel bereikbaar | — |
| Wegvallen van de proxy | Andere BLE-apparaten die via deze proxy liepen, vallen terug op "Home Bluetooth Proxy" of de HA-host | Vooraf in HA nagaan welke BLE-apparaten via `bluetooth_proxy_bedroom` gaan |

**Aanbeveling bij aanschaf: ESP32-S3-DevKitC-1-N16R8** (16 MB flash, 8 MB PSRAM, ±€10–15), of de `-1U`-variant met externe antenne als het bereik tot de bedden krap is. Daarmee verdwijnen de geheugen- en flashrisico's, en de proxy blijft werken tijdens de ontwikkeling. Pas na F3 wordt de oude proxy uitgezet of hergebruikt. De firmware is gelijk, alleen een andere build-target.

Niet geschikt: ESP32-C6 en -C3 (geen PSRAM, zelfde RAM-probleem), ESP32-H2 (geen Wi-Fi).

Voor het flashen: een volledige backup van de huidige ESPHome-firmware maken (`esptool read_flash 0 ALL`), zodat de proxy met één commando terug te zetten is.

## 4. Bed-laag (port van de huidige Python-code)

### 4.1 Protocol (`bed/protocol.{h,cpp}`)
1-op-1 port van `protocol.py`:
- Pakket `[0x40, esc(cmd0, cmd1, len_hi, len_lo, checksum, data…), 0x40]`, checksum `((som) ^ 0xFF) + 1`, antwoorden met `0x80` als startbyte in de checksum.
- Byte stuffing `0x40/0x3C/0x4F/0x41 → 0x3C + 01/02/03/04`.
- Commando's: motor up/down/stop/mempos (`0x02 0x70..0x73`), preset opslaan (`0x10 0x70`), drive mode (`0x10 0x71/72`), PIN (`0x20 0x43`), capabilities (`0x20 0x71/72`).
- Motormaskers: hoofd `0x02`, voeten `0x04`, beide `0x06`.
- **Testen:** golden vectors genereren met de bestaande `protocol.py` (alle commando's, PIN, licht, RGBW, feature-responses) en die als host-unittests tegen de C++-port draaien. Zo is de port aantoonbaar gelijk.

### 4.2 BedLink (`bed/bed_link.{h,cpp}`) — één per bed
Zelfde model als de connection manager in v2.9:
- Eén taak per bed die als enige verbindt; commando's vragen een verbinding aan, verbinden nooit zelf.
- Alleen verbinden als het bed adverteert (passieve scan op het MAC uit NVS), backoff `2,5,10,20,30,60 s`, direct opnieuw bij nieuwe advertentie.
- Beide bedden **na elkaar** opzetten (één GATT-discovery tegelijk).
- Na connect: notify op `FFE1` aanzetten, PIN sturen, wachten op `NOTIFY_PIN_ACCEPTED` / `REJECTED`, dan feature discovery (`0x20 0x71`).
- Keep-alive: PIN elke 25 s, en direct bij `40214400001b40` / `40217f0000e040`.
- Liveness: geen ack na 3 keep-alives (≈80 s) → link als dood behandelen en opnieuw opbouwen.
- Status gaat naar Matter als `Reachable` in Bridged Device Basic Information (HA toont het bed dan als niet beschikbaar).

### 4.3 Motion (`bed/motion.{h,cpp}`)
- Positie is **tijdgeschat** (het bed rapporteert geen positie): `positie = start + (doel − start) · t / reistijd`.
- Tijdens bewegen het motorcommando elke **375 ms** herhalen (zoals de afstandsbediening), daarna `STOP`.
- Doelen 0 % en 100 % rijden bewust iets langer door (eindstop), zodat afwijking zich herstelt.
- Hoofd en voeten tegelijk naar dezelfde kant → `BOTH`-commando; verschillende richtingen → afwisselend.
- Stopt de BLE-link tijdens bewegen → beweging afbreken en positie bevriezen op de laatst bevestigde tijd.
- Positie naar NVS schrijven bij stilstand (niet tijdens bewegen, flash-slijtage).

## 5. Matter-datamodel

Apparaattype root: **Aggregator** (bridge). Per bed een set bridged endpoints; elk krijgt `Bridged Device Basic Information` met `NodeLabel` ("Bed links") en `Reachable`. In HA wordt elk bridged apparaat een eigen device.

| Endpoint | Device type | Clusters / gedrag |
|---|---|---|
| Bed X – Hoofd | Window Covering | Features Lift + PositionAwareLift. `GoToLiftPercentage`, `UpOrOpen`, `DownOrClose`, `StopMotion`. `OperationalStatus` tijdens bewegen |
| Bed X – Voeten | Window Covering | idem |
| Bed X – Licht | Extended Color Light (RGBW-bed) of On/Off Light | OnOff, LevelControl, ColorControl (HS/XY → RGBW in firmware, W = min(R,G,B)) |
| Bed X – Preset | Mode Select (te verifiëren in HA) **of** 3× On/Off "momentary" | Preset kiezen = bed rijdt erheen; momentary zet zichzelf terug op uit |
| Beide – Hoofd/Voeten | Window Covering | Stuurt beide bedden tegelijk, positie = gemiddelde |
| Beide – Licht | On/Off Light | Beide lampen |

**Richting:** Matter `CurrentPositionLiftPercent100ths` betekent "% dicht". Mapping: bed **omhoog = open**. Dus `Lift100ths = (100 − bedpositie) · 100`. In HA komt dat uit als cover-positie = bedpositie (0 % plat, 100 % omhoog), gelijk aan de huidige integratie.

**Dynamische endpoints:** het licht-type (aan/uit vs RGBW) en hardware-presets hangen af van de capabilities van het bed. Die worden bij de eerste verbinding ontdekt, in NVS bewaard, en de bridged endpoints worden daarna bij boot uit NVS opgebouwd (esp-matter ondersteunt runtime-endpoints voor bridges). Verandert het, dan endpoint verwijderen en opnieuw aanmaken.

**Bewust niet in Matter:** PIN, MAC-keuze, reistijden, kalibratie, synchro-modus. Vendor-specifieke clusters toont HA niet (bekend van de G2H), dus dit gaat via de web-UI.

**Vendor/product-ID:** test-VID `0xFFF1` + PID in de testrange met test-DAC, zoals bij het G2H-project. Voor eigen gebruik voldoende.

## 6. Configuratie en web-UI

Na Matter-commissioning (die ook de Wi-Fi-gegevens levert) is de ESP op het LAN bereikbaar op `http://octo-bridge.local`.

Pagina's:
1. **Bedden** — scan naar `RC2`-advertenties (MAC + RSSI), max. 2 kiezen, naam + PIN invullen, "Test PIN" (verbinden, PIN-ack afwachten).
2. **Kalibratie** — per bed/onderdeel: rijdt naar 0 %, start op "Start" omhoog met timer, "Klaar" bij het hoogste punt; reistijd 5–120 s geclamped, zoals nu. "Samen kalibreren" voor beide bedden.
3. **Presets** — huidige stand opslaan als preset 1–3 (software) of hardware-slot.
4. **Status** — verbinding per bed, laatste fout, RSSI, uptime, fabrics.
5. **Update** — firmware uploaden (OTA-partitie, rollback bij mislukte boot).

Beveiliging: de web-UI staat standaard **uit**. Een korte druk op de BOOT-knop zet hem 15 minuten aan. Wachtwoord wordt bij de eerste boot gegenereerd en via de seriële log getoond (en staat op de QR-sticker). De PIN verlaat de ESP nooit (niet in de web-UI teruglezen, niet loggen).

## 7. Updates

- v1: firmware-upload via web-UI + USB als fallback; A/B OTA-partities met automatische rollback.
- Later: Matter OTA (OTA Requestor). Voor een eigen test-VID is de route via de HA Matter-server nog uit te zoeken.

## 8. Repo-opzet (nieuwe repo, bv. `octo-matter`)

```
main/
  app_main.cpp
  bed/protocol.{h,cpp}     port van protocol.py
  bed/bed_link.{h,cpp}     NimBLE central, PIN, keep-alive, reconnect
  bed/motion.{h,cpp}       positie, herhaalcommando's, presets
  bed/group.{h,cpp}        "Beide bedden"
  matter/bridge.cpp        aggregator + dynamische endpoints
  matter/callbacks.cpp     cluster-commando's → bed-laag
  config/store.{h,cpp}     NVS
  web/                     http-server + statische pagina's
test/host/                 unittests met golden vectors uit protocol.py
tools/gen_vectors.py       vectors genereren uit de octo-bed repo
sdkconfig.defaults         NimBLE central+peripheral, BLE aan na commissioning
sdkconfig.defaults.esp32   klassieke ESP32: kleine buffers, max. 3 BLE-links
sdkconfig.defaults.esp32s3 plan B: PSRAM aan
partitions_4mb.csv         eigen partitietabel voor 4 MB flash
```

Toolchain: ESP-IDF 5.x + esp-matter (C++). CI: build + host-tests in GitHub Actions.

Let op in de configuratie: esp-matter zet BLE standaard uit na commissioning. Dat moet uit, en NimBLE moet in central- én peripheral-rol draaien.

## 9. Fasering

| Fase | Inhoud | Klaar als |
|---|---|---|
| **F0 – BLE-spike** | Backup van de proxy-flash; ESP32 + NimBLE: verbinden met beide bedden, PIN, keep-alive, hoofd omhoog/stop. Nog geen Matter | 24 uur stabiel met twee bedden, beweging werkt |
| **F1 – Eén bed in Matter** | Aggregator met hoofd, voeten en licht; commissionen in HA | Covers en licht werken in HA, positie klopt binnen ±5 %, **vrije heap na commissioning ≥ 40 KB** (anders: plan B, S3) |
| **F2 – Config** | Web-UI: scan, PIN, reistijden, kalibratie; NVS | Nieuw bed instellen zonder herflashen |
| **F3 – Twee bedden** | Tweede BedLink, "Beide"-endpoints, presets | Beide bedden + groep tegelijk stabiel |
| **F4 – Afronding** | Watchdog, rollback-OTA, release-pakket (binary, bron, SHA256, installatie) | Release 1.0 |

## 10. Migratie vanuit de huidige setup

1. Het bed accepteert maar **één BLE-verbinding** en adverteert niet zolang er een is. Schakel tijdens testen de `octo_bed`-integratie uit (en sluit de OCTO-app op de telefoon).
2. Omdat de proxy zelf wordt omgeflasht, valt de oude route weg zodra F0 begint. Bereikt "Home Bluetooth Proxy" of de HA-host de bedden, dan kan de oude integratie tijdelijk via die route blijven werken. Zo niet, dan is het bed tot F1 alleen met de afstandsbediening te bedienen. Terug naar de oude situatie kan via de flash-backup.
3. Na F3: de `octo_bed`-integratie en het ESPHome-device "Bluetooth Proxy Bedroom" uit HA verwijderen. Automations die `cover.*_head` gebruiken naar de nieuwe Matter-entity-id's omzetten.
4. De HACS-integratie kan blijven bestaan voor andere gebruikers; de golden vectors houden beide implementaties gelijk.

## 11. Risico's

| Risico | Kans | Aanpak |
|---|---|---|
| Geheugen op klassieke ESP32: Matter + 2 BLE-links + web | **Hoog** | NimBLE, kleine buffers, minimale web-UI; meetpunt in F1; plan B ESP32-S3 |
| Flash 4 MB te klein voor 2 OTA-slots | Middel | Eerst uitlezen; anders update alleen via USB |
| BLE-bereik ESP ↔ beide bedden | Laag | De proxy staat er nu al en bereikt de bedden |
| Mode Select niet bruikbaar in HA | Middel | Fallback: On/Off-endpoints per preset |
| Positie loopt weg (tijdschatting) | Bekend | Eindstop-doorrijden, kalibratie, hetzelfde gedrag als nu |
| Hardware-presets: bed rijdt zelf, positie onbekend | Bekend | Voorkeur voor softwarepresets; na hardware-preset positie als onzeker markeren |
| Matter OTA met test-VID | Middel | v1 via web-UI |

## 12. Open vragen

1. ~~Welk board?~~ Besloten: de klassieke ESP32 van "Bluetooth Proxy Bedroom", met de S3 als plan B. Daarmee is het ook Wi-Fi (geen Thread).
2. Welk exacte board is het (DevKit / WROOM-32 / anders) en hoeveel flash heeft het? Uit te lezen bij de backup.
3. Lopen er naast de bedden nog andere BLE-apparaten via deze proxy?
4. Moet het ook in Apple Home komen (multi-admin)?
5. Welke functies wil je echt houden: synchro-modus, hardware-presets, RGBW?

## 13. Afwijkingen in de bouw

- **Koppelen gaat via wifi, niet via BLE.** CHIPoBLE staat uit, zodat NimBLE
  alleen van de bedverbindingen is. De wifi-gegevens komen uit `secrets.h`.
- **Eén Matter-apparaat per bed** (bridged node met hoofd, benen en licht als
  onderdelen) in plaats van losse apparaten per onderdeel. Endpoints: 2–5
  bed 1, 6–9 bed 2; vast, zodat controllers ze herkennen.
- **De instellingenpagina staat altijd aan** en is beveiligd met het token
  uit `secrets.h`, in plaats van via de BOOT-knop.
- **Namen in Apple Home:** iOS haalt de naam van de bridge uit de DCL; met de
  test-ID's (0xFFF1/0x8000) wordt dat "Matter Accessory". De bedden zelf
  krijgen wel hun eigen naam.
- **Geheugen op de klassieke ESP32 is krap** (±30 KB vrij, dalend naar
  ±8–20 KB bij koppelen of veel verkeer). Een ESP32-C6 of -S3 is de uitweg.

