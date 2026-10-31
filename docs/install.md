# octo-matter installeren

Een release (`tools/release.sh`) bevat de firmware voor twee borden. De
images zijn gebouwd met jouw `secrets.h` (Wi-Fi-wachtwoord, Thread-sleutel,
wachtwoord van de instellingenpagina): deel ze niet.

| Map | Bord | Matter via |
|---|---|---|
| `esp32c6/` | ESP32-C6 (8 MB flash) | Thread, aanbevolen |
| `esp32/` | Klassieke ESP32 (4 MB flash) | Wi-Fi |

Controleer eerst de bestanden: `shasum -a 256 -c SHA256SUMS`.

## Nieuw bord via USB

ESP32-C6:

```sh
cd esp32c6
python -m esptool --chip esp32c6 -p /dev/cu.usbmodemXXXX erase_flash
python -m esptool --chip esp32c6 -p /dev/cu.usbmodemXXXX write_flash \
    0x0 bootloader.bin 0x8000 partition-table.bin \
    0x19000 ota_data_initial.bin 0x20000 octo_bridge.bin
```

Klassieke ESP32 (USB-lezen en -schrijven lukt alleen op 115200 baud):

```sh
cd esp32
python -m esptool --chip esp32 -p /dev/cu.usbserial-XXXX -b 115200 write_flash \
    0x1000 bootloader.bin 0x8000 partition-table.bin \
    0xe000 ota_data_initial.bin 0x10000 octo_bridge.bin
```

## Update van een draaiende bridge

Via het netwerk, het andere app-slot blijft als terugval staan:

```sh
OCTO_HOST=<ip of [ipv6] of naam> tools/octo.sh --ota esp32c6/octo_bridge.bin
```

Over Thread duurt dat enkele minuten. Een nieuwe image die niet binnen
10 minuten online komt, of drie keer snel crasht, gaat vanzelf terug naar de
vorige.

## Daarna

1. **Netwerk.** De C6 gaat op het Thread-netwerk dat op de pagina is ingesteld
   (of uit `OCTO_THREAD_DATASET`). Heeft hij nog geen netwerk, dan opent hij
   het setup-netwerk "Octo-Bridge-XXXX": verbind je telefoon, de pagina opent
   vanzelf (anders http://192.168.4.1/). De klassieke ESP32 gaat op het
   Wi-Fi-netwerk uit `OCTO_WIFI_SSID`.
2. **Instellingenpagina.** `tools/octo.sh system` (of `system` op de seriële
   console) toont het adres, op Thread als `http://<naam>.local/`. Bij het
   eerste bezoek kies je daar een wachtwoord (vergeten: `password reset` op
   de seriële console).
3. **Bedden.** Op de pagina onder *Bed instellen*: bed uit de scan kiezen,
   PIN en naam invullen. Een bed accepteert maar één Bluetooth-verbinding:
   zet de OCTO-app en andere bridges uit.
4. **Koppelen.** Elke bridge maakt bij de eerste start een eigen koppelcode.
   Die staat op de instellingenpagina (*Systeem*) en onder `matter`. In Home
   Assistant: *Matter-apparaat toevoegen*. Een tweede controller (Apple Home)
   koppelt na `matter open` met dezelfde code.
5. **Kalibreren.** Per motor op de pagina: helemaal omlaag, *Start meting*,
   *Stop* zodra het onderdeel bovenaan is.
