# octo-matter

ESP32-firmware die Octo-bedden (Octo Actuators, BLE `FFE0/FFE1` met 4-cijferige
PIN) als standaard **Matter**-apparaten aanbiedt. Geen ESPHome, geen
Bluetooth-proxy en geen HACS-integratie nodig: Home Assistant koppelt de bedden
via de gewone Matter-integratie.

Opvolger van de Home Assistant-integratie
[octo-bed](https://github.com/bramboe/octo-bed); het BLE-protocol is daaruit
geport en wordt met golden vectors gelijk gehouden.

> **Status: 1.0.** Eén bridge bedient twee bedden. Per bed: hoofd en benen
> (Window Covering), licht, een stopknop en een presetkeuze (plat, stand 1-3,
> zelfde als het andere bed, huidige stand bewaren). Hoofd en benen bewegen
> samen als ze dezelfde kant op gaan, anders na elkaar. Op de ESP draait een
> instellingenpagina voor bedden, looptijden en kalibratie. Zie
> [docs/ontwerp.md](docs/ontwerp.md).

## Borden

| Bord | Matter via | Build |
|---|---|---|
| **ESP32-C6** (aanbevolen) | Thread, via een Thread border router | `build-c6/`, `sdkconfig.c6` |
| Klassieke ESP32 | Wi-Fi | `build/`, `sdkconfig` |

Bluetooth is op beide alleen voor de bedden: Matter wordt via het netwerk
gekoppeld (Thread-dataset of Wi-Fi uit `secrets.h`), niet via BLE.

In Home Assistant verschijnen de stopknop als drukknop en de presetkeuze als
keuzelijst; Apple Home toont alleen de covers en het licht. "Beide" (per bed
en voor beide bedden) zijn groepen in Home Assistant.

## Terug naar ESPHome

Altijd mogelijk, zie [docs/esphome-fallback.md](docs/esphome-fallback.md).
Maak **vóór de eerste flash** van een ESPHome-apparaat een backup:

```sh
tools/backup_flash.sh /dev/cu.usbserial-XXXX
```

## Opbouw

| Pad | Inhoud |
|---|---|
| `components/octo_protocol` | Octo BLE-pakketprotocol (C++ port van `protocol.py`) |
| `components/octo_core` | Bedverbindingen, boot guard, console-commando's, HTTP-API en instellingenpagina |
| `firmware/bridge` | De Matter-bridge (esp-matter release/v1.6) |
| `test/host` | Host-tests tegen golden vectors uit de Python-referentie |
| `tools/` | `octo.sh` (console via het netwerk), release, vectorgenerator, flash-backup en -restore |
| `docs/` | Ontwerp, installatie en terugvalprocedure |

## Bouwen

Vereist ESP-IDF v5.5.5 (`~/esp/esp-idf-v5.5.5`, met `install.sh esp32,esp32c6`)
en esp-matter `release/v1.6` (`~/esp/esp-matter`). Kopieer
`firmware/bridge/main/secrets.example.h` naar `secrets.h` (staat niet in git); voor
de C6 hoeft daar niets in: Wi-Fi en Thread stel je in op de pagina. Het wachtwoord van de pagina kies je
op de pagina zelf.

```sh
. ~/esp/esp-idf-v5.5.5/export.sh && . ~/esp/esp-matter/export.sh
cd firmware/bridge
idf.py -B build-c6 -D SDKCONFIG=sdkconfig.c6 set-target esp32c6   # eenmalig
idf.py -B build-c6 -D SDKCONFIG=sdkconfig.c6 build                # ESP32-C6
idf.py build                                                      # klassieke ESP32
```

`tools/release.sh` bouwt beide en zet een release met SHA256SUMS en
[installatie-instructies](docs/install.md) in `dist/`. De images bevatten je
`secrets.h`: deel ze niet.

## Bedienen

`tools/octo.sh <commando>` stuurt een consolecommando via het netwerk
(`OCTO_HOST=<ip>`, of `[ipv6]` of `<naam>.local` voor de C6). Updates:
`tools/octo.sh --ota firmware/bridge/build-c6/octo_bridge.bin`.

De instellingenpagina staat op `http://<adres>/`; `tools/octo.sh system` toont
het adres. De eerste keer kies je daar een wachtwoord; daarna vraagt ze er
één keer per browser om. Vergeten: `password reset` op de seriële console.
`tools/octo.sh` gebruikt `OCTO_TOKEN=<wachtwoord>`. Op de pagina kun je:

- een bed kiezen uit de Bluetooth-scan en met PIN en naam opslaan;
- per motor de looptijd invullen, of meten: helemaal omlaag, *Start meting*,
  en *Stop* zodra het onderdeel bovenaan is;
- bewegen, stoppen, het licht schakelen en standen kiezen of bewaren;
- de koppelcode zien.

In de console (zie `help`), de PIN wordt in NVS opgeslagen, niet in de firmware:

```
octo> scan
octo> bed set 0 AA:BB:CC:DD:EE:01 0000 Sam
octo> status
octo> move 0 head up 3
octo> preset 0 save 1
octo> preset 0 1
octo> stop 0
```

Koppelen met Matter: elke bridge maakt bij de eerste start een eigen
koppelcode (`matter` toont die), `matter open` opent een koppelvenster voor
een extra controller, `matter fabrics` en `matter remove <n>` tonen en
verwijderen gekoppelde controllers.

## Host-tests

```sh
test/host/run.sh
```

Na een protocolwijziging in octo-bed: `tools/gen_vectors.py ../octo-bed`.

## Licentie

MIT
