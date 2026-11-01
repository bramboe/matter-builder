# Octo bed bridge

Firmware for an ESP32-C6 that offers Octo beds (Octo Actuators, Bluetooth
`FFE0/FFE1` with a 4-digit PIN) as standard **Matter** devices. No ESPHome,
Bluetooth proxy or custom integration: Home Assistant, Apple Home and Google
Home pair the beds with their own Matter support.

It follows the Home Assistant integration
[octo-bed](https://github.com/bramboe/octo-bed); the Bluetooth protocol is
ported from it and kept equal with golden vectors (`test/host`).

One bridge controls one bed, or two with the setting `beds: 2` (in the
Matter Builder add-on's YAML for the device, *Use 2 beds* on the page, or
`settings set beds 2`; the bridge restarts). Per bed: head and legs (Window
Covering), the light, a stop button and a preset choice (flat, positions 1-3,
with two beds the same as the other bed, save the current position). Head and legs move together when they
go the same way, otherwise one after the other. Home Assistant shows the stop
button and the presets; Apple Home shows the covers and the light.

## Install

From the Matter Builder add-on: **Catalog → Octo bed bridge → Use**. On a new
ESP32-C6 it installs the factory image over USB. The firmware is built and
signed by this repository's release workflow, and holds nothing personal.

## Set up

With no Thread network yet, the bridge opens the access point
`Octo-Bridge-XXXX`. Join it and open `http://192.168.4.1/`:

1. Choose the password of the settings page.
2. Enter your Wi-Fi (used only for configuration mode) and paste the Thread
   network: in Home Assistant, *Settings → Devices & services → Thread →
   Configure*, the (i) of the preferred network.
3. Add the beds from the Bluetooth scan, with their PIN and a name of your
   own. The PIN is stored on the bridge, never in the firmware.
4. Set the travel time per motor, or measure it: all the way down, *Start
   measuring*, and *Stop* when the part is at the top.
5. Pair with Matter using the code on the page.

Entities get the bed's name plus the part (Head, Legs, Light), so in Home
Assistant they become for example `cover.<bed>_head`.

Later the page is at `http://<address>/`. Forgot the password: hold BOOT for
5-10 seconds (password cleared, setup access point), or `password reset` on
the serial console.

## Console

On the serial console, or over the network with `tools/octo.sh <command>`
(`OCTO_HOST=<address>`, `OCTO_TOKEN=<password>`); see `help`:

```
octo> scan
octo> bed set 0 AA:BB:CC:DD:EE:01 0000 Sam
octo> status
octo> move 0 head up 3
octo> preset 0 save 1
octo> preset 0 1
octo> stop 0
```

`matter` shows the pairing code, `matter open` opens a window for one more
controller, `matter fabrics` and `matter remove <n>` list and remove paired
controllers.

## Building

As in the main README, with this folder:

```sh
. ~/esp/esp-idf-v5.5.5/export.sh && . ~/esp/esp-matter/export.sh
cd firmware/bridge
cp main/secrets.example.h main/secrets.h
idf.py -B build-c6 -D SDKCONFIG=build-c6/sdkconfig -D IDF_TARGET=esp32c6 build
```

`secrets.h` can stay as the example: Wi-Fi, Thread and the password are set
on the page. Host tests: `test/host/run.sh`; after a protocol change in
octo-bed, `tools/gen_vectors.py ../octo-bed`.

## Back to ESPHome

Always possible, see [docs/esphome-fallback.md](docs/esphome-fallback.md)
(in Dutch, like the other notes in `docs/`).

## License

MIT
