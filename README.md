# octo-matter

ESP32-firmware die Octo-bedden (Octo Actuators, BLE `FFE0/FFE1` met 4-cijferige
PIN) als standaard **Matter**-apparaten aanbiedt. Geen ESPHome, geen
Bluetooth-proxy en geen HACS-integratie nodig: Home Assistant koppelt de bedden
via de gewone Matter-integratie.

Opvolger van de Home Assistant-integratie
[octo-bed](https://github.com/bramboe/octo-bed); het BLE-protocol is daaruit
geport en wordt met golden vectors gelijk gehouden.

> **Status: F0 (BLE-spike).** Nog geen Matter. Zie [docs/ontwerp.md](docs/ontwerp.md).

## Terug naar ESPHome

Altijd mogelijk, zie [docs/esphome-fallback.md](docs/esphome-fallback.md).
Maak **vóór de eerste flash** een backup:

```sh
tools/backup_flash.sh /dev/cu.usbserial-XXXX
```

## Opbouw

| Pad | Inhoud |
|---|---|
| `components/octo_protocol` | Octo BLE-pakketprotocol (C++ port van `protocol.py`) |
| `firmware/spike` | F0: NimBLE-central naar 1–2 bedden, PIN, keep-alive, seriële console |
| `test/host` | Host-tests tegen golden vectors uit de Python-referentie |
| `tools/` | Vectorgenerator, flash-backup en -restore |
| `docs/` | Ontwerp en terugvalprocedure |

## Host-tests

```sh
test/host/run.sh
```

Na een protocolwijziging in octo-bed: `tools/gen_vectors.py ../octo-bed`.

## F0 bouwen en testen

Vereist ESP-IDF v5.5.5. Kopieer eerst `firmware/spike/main/secrets.example.h`
naar `secrets.h` en vul wifi en token in (staat niet in git).

Installeren op een ESP32 die nog ESPHome draait, via het netwerk:

```sh
~/esp/esphome-venv/bin/python tools/esphome_ota_upload.py <ip> firmware/spike/build/octo_spike.bin
tools/octo.sh status          # daarna alles via het netwerk
tools/octo.sh --ota firmware/spike/build/octo_spike.bin   # volgende updates
```

Of via USB:

```sh
cd firmware/spike
idf.py set-target esp32
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

In de console, of via `tools/octo.sh <commando>` (de PIN wordt in NVS opgeslagen, niet in de firmware):

```
octo> scan
octo> bed set 0 AA:BB:CC:DD:EE:01 0000 Links
octo> bed set 1 XX:XX:XX:XX:XX:XX 1234 Rechts
octo> status
octo> move 0 head up 3
octo> stop 0
octo> light 0 on
```

F0 is geslaagd als beide bedden 24 uur verbonden blijven (de `STAT`-regels,
elke 5 minuten, tonen `drops=0` of alleen herstelde drops) en bewegen werkt.
Sluit tijdens het testen de `octo_bed`-integratie en de OCTO-app af: het bed
accepteert maar één verbinding.

## Licentie

MIT
