# Testplan: firmware-update via de Matter-server van HA

Status: plan, nog niets uitgevoerd. Datum: 2026-10-31.
Hoort bij: `docs/matter-builder-ontwerp.md` §8.2.

## 1. Wat we willen bewijzen

Een zelfgebouwd `.ota`-bestand met test-VID `0xFFF1` kan via de Matter-server van HA (matterjs-server) naar de Octo-bridge. HA toont het als een gewone **update-entity**, de update loopt over **Thread**, en daarna werkt alles nog: de bedden, de HA-fabric en de Apple-fabrics.

Lukt dit, dan is OTA voor de Matter Builder opgelost zonder eigen updatecode in de firmware.

## 2. Uitgangssituatie (gecontroleerd 2026-10-31)

| Wat | Waarde |
|---|---|
| Bridge in HA | "Octo Bed Bridge", ESP32-C6, **Matter over Thread** (MTD), netwerk `ha-thread-1234`, `fd00::1234` |
| Firmware | 1.0.0, `CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER=100`, VID `0xFFF1`, PID `0x8000` |
| OTA-requestor | **uit** (`CONFIG_ENABLE_OTA_REQUESTOR=n` in `sdkconfig.defaults`), dus HA toont nu geen update-entity |
| Fabrics | 3: Apple Home, Apple Keychain, Home Assistant |
| App-slots C6 | 2 × 3 MB (`partitions_c6.csv`), image 1,85 MB, dus ruimte genoeg |
| Matter Server add-on | 9.3.0 (matterjs), `enable_test_net_dcl: true`, `beta: true` |
| Bestaande updateroute | HTTP `/ota` via `tools/octo.sh --ota` (werkt ook over Thread met `OCTO_HOST=[fd7e:…]`) |
| Vangnet | `boot_guard`: een nieuwe firmware moet zich binnen de deadline via het netwerk melden, anders start automatisch de vorige slot. Daarnaast is er het commando `revert`. |

Gevonden in de API-documentatie van matterjs-server (`docs/websockets_api.md`, schema 14):
- `initiate_ota_upload` levert een `upload_id` (60 s geldig). Daarna gaat `POST /ota-upload/<upload_id>` met de ruwe `.ota`-bytes naar dezelfde poort als `/ws` (5580), vanuit **dezelfde client**.
- Het image wordt opgeslagen op VID/PID/versie uit de header. `check_node_update` toont het voor elke node met die VID/PID.
- **Test-images (`0xFFF1`) worden alleen aangeboden met `--enable-test-net-dcl`.** Dat staat bij jou al aan.
- `update_node` met `node_id` en `software_version` start de update.

Poort 5580 is vanaf de Mac **niet** bereikbaar (`homeassistant.local:5580` gaf geen antwoord). De upload moet dus binnen HA gebeuren: via de "Advanced SSH & Web Terminal" add-on (host-netwerk) naar `localhost:5580`.

## 3. Twee firmwareversies bouwen

Beide worden gebouwd op een aparte testbranch (`ota-test`), in `build-c6`.

| Versie | `PROJECT_VER` | `SOFTWARE_VERSION_NUMBER` | Verschil met 1.0.0 |
|---|---|---|---|
| **1.0.1** | `1.0.1` | `101` | `CONFIG_ENABLE_OTA_REQUESTOR=y`, `CONFIG_CHIP_OTA_IMAGE_BUILD=y` |
| **1.0.2** | `1.0.2` | `102` | Alleen het versienummer. Zo is zeker dat een geslaagde update zichtbaar is aan de versie. |

Wijzigingen in de code:
- `sdkconfig.defaults`: de OTA-requestor aan. De opmerking "updates go through /ota" moet worden aangepast. `/ota` blijft bestaan als terugvaloptie.
- `CMakeLists.txt`: `PROJECT_VER`.
- `CONFIG_CHIP_OTA_IMAGE_BUILD=y` laat esp-matter zelf `build-c6/octo_bridge-ota.bin` maken. Dat is het `.ota`-bestand met Matter-header (VID/PID/versie). Als dat niet werkt, gebruik dan handmatig `connectedhomeip/src/app/ota_image_tool.py create -v 0xFFF1 -p 0x8000 -vn 102 -vs 1.0.2 -da sha256 octo_bridge.bin octo_bridge-1.0.2.ota`.

Controle na het bouwen (op de Mac, zonder het apparaat):
- de imagegrootte van 1.0.1 ten opzichte van 1.0.0, om te zien wat de requestor kost;
- `ota_image_tool.py show octo_bridge-1.0.2.ota` toont VID `0xFFF1`, PID `0x8000` en versie 102;
- de classic-ESP32-build doet niet mee aan deze test. De requestor maakt die waarschijnlijk te groot (96% vol). Dat noteren we als bevinding.

## 4. Testverloop

Bij elke stap die het apparaat raakt (**⚠**) eerst je akkoord. Kies een moment waarop de bedden niet nodig zijn.

### Stap 0: nulmeting (alleen lezen)
- `tools/octo.sh status` en `system`: noteer de slot, versie, heap, Thread-rol en de bedverbindingen.
- In HA: sw_version 1.0.0, beschikbaar, 3 fabrics, de covers en lichten van beide bedden werken.
- In Apple Home: de bedden zijn zichtbaar.

### Stap 1 ⚠: 1.0.1 installeren via de bestaande HTTP-route
- `OCTO_HOST='[fd00::1234]' tools/octo.sh --ota firmware/bridge/build-c6/octo_bridge.bin`
- Verwacht: herstart, `boot_guard` bevestigt de firmware, HA toont **sw_version 1.0.1** en er verschijnt een **update-entity** bij "Octo Bed Bridge" (status: up-to-date).
- Controle: de bedden bewegen, de fabrics zijn nog alle 3 aanwezig, en Apple Home ziet de bedden nog.
- Mislukt het: `boot_guard` zet automatisch 1.0.0 terug. Gebeurt dat niet, dan via `tools/octo.sh revert`.

Deze stap is al waardevol op zich. Hij laat zien dat de requestor op een bridge met BLE en Thread qua geheugen past. Noteer de heap na het opstarten en na een bedcommando.

### Stap 2: 1.0.2 uploaden naar de Matter-server (raakt het apparaat niet)
1. Het `.ota`-bestand via de Samba-share naar `/share/octo-ota/octo_bridge-1.0.2.ota`.
2. In de SSH & Web Terminal een klein script (`tools/ota_upload.py`, nog te schrijven, alleen standaardbibliotheek + `websockets` of `aiohttp`) dat:
   - verbinding maakt met `ws://localhost:5580/ws`;
   - `initiate_ota_upload` stuurt en het `upload_id` krijgt;
   - binnen 60 s `POST http://localhost:5580/ota-upload/<id>` met de bytes stuurt;
   - het antwoord (`MatterSoftwareVersion`) toont.
3. Verwacht: `200` met versie 102, VID `0xFFF1`, PID `0x8000`.

Onzekerheid: of "dezelfde client" het IP-adres betekent of dezelfde websocketverbinding. Het script houdt de websocket open tijdens de POST en doet beide vanaf hetzelfde adres, dus het werkt in beide gevallen.

### Stap 3: HA ziet de update (alleen lezen)
- Update-entity → "Controleren op updates" (of `check_node_update` via het script).
- Verwacht: **1.0.2 beschikbaar**.
- Komt die niet: kijk in de logs van de Matter Server add-on (staat test-net DCL echt aan, matcht VID/PID?).

### Stap 4 ⚠: update via HA installeren
- In HA op de update-entity op **Installeren** klikken. Dat stuurt `update_node` met versie 102.
- Verwacht verloop: de server stuurt `AnnounceOTAProvider`, de bridge vraagt het image op, en BDX haalt ±1,85 MB op over Thread. Ik schat **10–30 minuten**; dat meten we. Daarna herstart de bridge en meldt de update als toegepast.
- Ondertussen: de voortgang in de update-entity volgen. Bedien de bedden niet tijdens de overdracht, maar kijk wel of de bridge bereikbaar blijft.

### Stap 5: controle na de update (alleen lezen)
- sw_version **1.0.2** in HA, en de update-entity staat op up-to-date.
- `tools/octo.sh status`: de slot is gewisseld, en `boot_guard` heeft de firmware bevestigd.
- Alle 3 fabrics zijn nog aanwezig, en Apple Home ziet de bedden.
- De bedden: hoofd, voeten en licht van beide bedden werken, en de PIN-koppeling herstelt zich.
- Noteer: de duur van de overdracht, de heap, en of er tijdens de overdracht iets haperde (BLE en Thread delen de 2,4 GHz-radio op de C6).

### Stap 6 ⚠: terug naar een nette versie
- Wil je de OTA-requestor houden: maak 1.0.1/1.0.2 netjes, commit het op main en neem het op in `release.sh`.
- Zo niet: `revert` naar de vorige slot, of 1.0.0 via `octo.sh --ota`. Let op: een Matter-update naar een **lagere** versie wordt niet geaccepteerd, dus terug naar 1.0.0 gaat via HTTP.

## 5. Stopcriteria

Stoppen en terugdraaien (`revert`) als:
- de bridge na stap 1 of stap 4 langer dan 10 minuten onbereikbaar is in HA en `boot_guard` niet zelf terugschakelt;
- een fabric verdwenen is;
- de bedden niet meer reageren na een herstart.

## 6. Wat dit oplevert voor de Matter Builder

| Vraag | Antwoord na de test |
|---|---|
| Werkt OTA met test-VID via matterjs-server? | ja/nee |
| Is upload via `initiate_ota_upload` + `POST` bruikbaar vanuit een add-on? | ja/nee + bijzonderheden |
| Hoe lang duurt 1,85 MB over Thread? | minuten |
| Wat kost de OTA-requestor in flash en heap? | KB |
| Past de requestor op de classic ESP32? | waarschijnlijk niet, rekensom |

De uitkomsten gaan naar §8.2 van het ontwerpdocument.

## 7. Builds (2026-10-31)

Gebouwd in de worktree `~/Documents/GitHub/octo-matter-ota` (branch `ota-test`, vanaf `main` e241069). Inhoud: OTA-requestor aan, OTA-image-build aan, en de part-labels van de andere sessie (FixedLabel + eigen DeviceInfoProvider, `ha_entitylabel`). `CMakeLists.txt` controleert dat `PROJECT_VER_NUMBER` gelijk is aan `CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER`. Die controle heeft al een keer een fout in de versienummers tegengehouden.

| Bestand (`dist/ota-test/`) | Gebruik | Header |
|---|---|---|
| `octo_bridge-1.0.1.bin` | stap 1, via `tools/octo.sh --ota` | — |
| `octo_bridge-1.0.1.ota` | niet nodig, alleen ter controle | VID 0xFFF1, PID 0x8000, versie 101, "1.0.1" |
| `octo_bridge-1.0.2.ota` | stap 2, uploaden naar de Matter-server | VID 0xFFF1, PID 0x8000, versie 102, "1.0.2" |

Grootte (ESP32-C6):

| | 1.0.0 (live) | 1.0.1 | Verschil |
|---|---|---|---|
| Image | 1 851 008 B | 1 895 170 B | **+43 KB** (OTA-requestor + part-labels) |
| Bezetting app-slot (3 MB) | 59% | 60% | |
| Statisch D/IRAM | — | 190 KB (42%) | |

`tools/ota_upload.py` (in de worktree) is getest tegen een nep-server op de Mac, met een upload van 1,9 MB plus `check_node_update`. In de Matter-server is de bridge **node 81** (0x51).

Nog niet gedaan: stap 0 en verder op het echte apparaat. Niets is gecommit.

## 8. Resultaten (2026-10-31)

**HTTP-OTA over Thread is onbruikbaar.** In 30 minuten kwam 996 724 van de 1 895 248 bytes aan (52,6%, ±0,55 KB/s). Daarna liep `curl` tegen zijn tijdslimiet aan. Slot app1 was half beschreven, maar de opstartkeuze wees nog naar app0 (1.0.0), dus er was geen risico. Tijdens de upload gaf ping 470 ms met 50% pakketverlies.

**USB-route.** De backup staat in `~/Documents/octo-c6-backup/20261031-105706` (dubbel gelezen). 1.0.1 is met `otatool write_ota_partition --slot 1` in app1 gezet (4,5 s) en met `switch_ota_partition --slot 1` geactiveerd. Valkuil: een poort openen zet de C6 vaak in downloadmodus ("waiting for download"), ook via `esp_idf_monitor`. Kabels los en de voeding even eraf lost dat op. Daarna geen poort openen.

**1.0.1:** draait, is bevestigd, 3 fabrics, HA toont sw 1.0.1. Er verschijnt (nog) geen update-entity in HA; dat moet bij de Matter-OTA-stap opnieuw bekeken worden (re-interview?).

**1.0.2 = configuratiemodus** (Wi-Fi, één opstart, Thread, Matter en BLE uit; zie `main/config_mode.h`):

| Meting | Waarde |
|---|---|
| Image | 2 278 070 B (+383 KB voor Wi-Fi en mDNS), slot voor 72% vol |
| Statisch RAM | 235 KB (52%), vrij geheugen in de normale modus ±168–174 KB (was 215 KB) |
| Van commando tot `octo-bridge.local` op Wi-Fi | 5 s (RSSI −57) |
| Upload 2,28 MB via `/ota` op Wi-Fi | **19,6 s (116 KB/s)**, ±200× sneller dan Thread |
| Terug op Thread na de upload | 8 s; beide bedden binnen 30 s weer verbonden, 3 fabrics, HA-covers beschikbaar |

Nog open: Matter-OTA (BDX) met 1.0.3 via de Matter-server, nu de OTA-requestor erin zit; de update-entity in HA.
