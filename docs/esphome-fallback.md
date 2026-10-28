# Terug naar ESPHome

De Matter-firmware komt op de ESP32 die nu "Bluetooth Proxy Bedroom" is
(klassieke ESP32, MAC `24:6f:28:5f:03:7c`). Er zijn drie onafhankelijke
manieren om terug te gaan. Zorg dat 1 en 2 klaar zijn vóór de eerste flash.

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
