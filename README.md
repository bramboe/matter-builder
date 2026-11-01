# Matter Builder

ESP32 devices as standard **Matter** devices, over Thread or Wi-Fi, set up
like ESPHome but without a Home Assistant integration of their own: they work
in Home Assistant, Apple Home and Google Home at the same time, and keep
working when Home Assistant is off.

- **Ready-made, signed firmware.** Ordinary users do not build. The standard
  firmware, the *generic device*, covers relays, LEDs, buttons, contacts,
  motion and SHT4x sensors on free pins of an ESP32-C6.
- **Set up on the device.** Every device has a settings page of its own:
  password, Wi-Fi, Thread network and what is connected. Nothing personal is
  built into the firmware.
- **Home Assistant add-on as extra.** It finds and links devices, edits their
  connections as a form or as YAML, updates them, and installs firmware on a
  new ESP32 over USB. In developer mode it builds firmware itself.
- **Safe adoption.** Linking a program needs the device's password and a
  press of its BOOT button. Firmware updates are only accepted when they are
  signed; new connections run on trial and are rolled back automatically.

## Layers

| Layer | What | Needs building |
|---|---|---|
| Standard firmware (`generic`) | General building blocks, configured on the device | No |
| Packages | Firmware with code of its own (e.g. a Bluetooth protocol), built and signed by its source | No, for the user |
| Developer mode | Your own code, built in Home Assistant | Yes |

See [docs/matter-builder-design.md](docs/matter-builder-design.md) for the
design, including package sources and the catalog.

## Repository

| Path | Contents |
|---|---|
| `components/mb_core` | Shared core: Matter and Thread setup, settings page API, adoption, OTA, configuration mode, boot guard |
| `firmware/generic` | The generic device (ESP32-C6, esp-matter release/v1.6) |
| `ha-addon/matter_builder` | The Home Assistant add-on |
| `tools/mb_generate.py` | Turns a device spec (YAML) into a project |
| `tools/release_manifest.py` | Writes and signs a release manifest |
| `examples/` | Example device specs |
| `.github/workflows/release.yml` | Builds and signs a release for a tag `v*` |

## Building

Needs ESP-IDF v5.5.5 (`~/esp/esp-idf-v5.5.5`, `install.sh esp32c6`) and
esp-matter `release/v1.6` at commit `fe1efc6c` (`~/esp/esp-matter`):

```sh
. ~/esp/esp-idf-v5.5.5/export.sh && . ~/esp/esp-matter/export.sh
cd firmware/generic
idf.py -B build -D SDKCONFIG=build/sdkconfig -D IDF_TARGET=esp32c6 build
```

The add-on's developer mode does the same with the same versions
(`ha-addon/matter_builder/toolchain.sh` and `build.sh`).

## Releases

A tag `v<version>` builds the firmware in GitHub Actions, writes
`manifest.json` with the SHA-256 of every image and signs it with ECDSA P-256
(secret `MB_SIGNING_KEY`). The add-on checks the signature against
`ha-addon/matter_builder/release-key.pem` and every image against its hash
before it installs anything.

## License

MIT
