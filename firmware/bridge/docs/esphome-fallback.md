# Terug naar ESPHome

De Matter-firmware komt op de ESP32 die nu "Bluetooth Proxy Bedroom" is
(klassieke ESP32, MAC `24:6f:28:5f:03:7c`). Er zijn meerdere onafhankelijke
manieren om terug te gaan.

## 0. Via het netwerk (installatie over ESPHome-OTA)

De bridge wordt via ESPHome's eigen OTA geïnstalleerd
(`tools/esphome_ota_upload.py`). ESPHome blijft dan in het andere
firmwareslot staan.

- **Automatisch:** een firmware die nog nooit online is geweest, gaat terug
  naar het andere slot (ESPHome) als hij 3× crasht in de eerste minuut, of na
  10 minuten nog geen werkend netwerk heeft.
- **Zelf, zolang ESPHome nog in het andere slot staat:** `tools/octo.sh revert`.
  `tools/octo.sh system` laat zien wat er in beide slots staat.
- **Let op:** de *tweede* netwerk-update van de bridge schrijft over het
  ESPHome-slot heen. Daarna gaat terug via een ESPHome-image: in de Device
  Builder *Install → Manual download → OTA format*, dan
  `tools/octo.sh --ota <bestand>.bin`.
- Lukt dit allemaal niet, dan via USB (1 of 2 hieronder).

## 1. Volledige flash-backup (exact terug zoals het was)

Vóór de eerste flash, met de ESP32 via USB aan de Mac:

```sh
tools/backup_flash.sh /dev/cu.usbserial-XXXX
```

- Leest de flash twee keer en vergelijkt; bij verschil wordt er niets opgeslagen.
- Schrijft naar `~/Documents/octo-esphome-backup/<datum>/` (buiten de repo,
  want de dump bevat het wifi-wachtwoord en de ESPHome API-sleutel).
- Legt ook chip-id en flashgrootte vast (nodig voor de partitietabel).

Terugzetten:

```sh
tools/restore_flash.sh /dev/cu.usbserial-XXXX ~/Documents/octo-esphome-backup/<datum>
```

Daarna draait exact dezelfde ESPHome-firmware weer, met dezelfde
instellingen. HA herkent het apparaat vanzelf.

## 2. ESPHome YAML bewaren

De ESPHome-configuratie staat in de **ESPHome Device Builder**-add-on in Home
Assistant. Zolang je dat apparaat daar **niet verwijdert**, kun je altijd
opnieuw installeren via *Install → Plug into this computer*.

Extra zekerheid: in de Device Builder bij het apparaat ⋮ → *Download YAML*,
en de YAML in dezelfde backupmap zetten (niet in deze repo: hij verwijst naar
secrets).

## 3. HA-kant

- Verwijder het ESPHome-apparaat "Bluetooth Proxy Bedroom" pas uit HA als de
  Matter-bridge (F3) stabiel draait. Een offline ESPHome-apparaat kan prima
  blijven staan; na terugzetten is het meteen weer online.
- De `octo_bed`-integratie (HACS) blijft geïnstalleerd tot en met F3. Valt de
  bridge terug, dan werkt die integratie weer zodra de proxy terug is.
