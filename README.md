# Matter Builder

[![Sponsor on GitHub](https://img.shields.io/badge/Sponsor-GitHub-EA4AAA?logo=githubsponsors&logoColor=white)](https://github.com/sponsors/bramboe?frequency=one-time) [![Sponsor bunq.me](https://img.shields.io/badge/Sponsor-bunq.me-2E9AD0)](https://bunq.me/bramboe)

ESP32 devices as standard **Matter** devices, over Thread or Wi-Fi, set up
like ESPHome but without a Home Assistant integration of their own: they work
in Home Assistant, Apple Home and Google Home at the same time, and keep
working when Home Assistant is off.

- **Ready-made, signed firmware.** Ordinary users do not build. The standard
  firmware, the *generic device*, covers relays, LEDs, buttons, contacts,
  motion and SHT4x sensors on free pins of an ESP32-C6, plus infrared
  (NEC send and receive) and Bluetooth LE listening (BTHome v2, pvvx and
  ATC1441 thermometers, presence of a tag).
- **Shows what a device needs.** Every firmware and package states whether
  it needs Thread, Wi-Fi or Bluetooth; the add-on checks it against the board
  before installing.
- **Matter pairing over Bluetooth, optional.** Off by default (it costs about
  30 KB of RAM); devices pair with a code over the network instead.
- **Components.** A connection type of your own, linked from a GitHub
  repository or a local folder, built in developer mode.
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
| Packages | Firmware with code of its own (e.g. a Bluetooth protocol), built and signed by its source; the official source has the Octo bed bridge | No, for the user |
| Developer mode | Your own code, built in Home Assistant | Yes |

See [docs/matter-builder-design.md](docs/matter-builder-design.md) for the
design, including package sources and the catalog.

## Repository

| Path | Contents |
|---|---|
| `components/mb_core` | Shared core: Matter and Thread setup, settings page API, adoption, OTA, configuration mode, boot guard |
| `firmware/generic` | The generic device (ESP32-C6, esp-matter release/v1.6) |
| `firmware/bridge`, `components/octo_*` | The Octo bed bridge package: two Octo beds over Bluetooth ([README](firmware/bridge/README.md)) |
| `ha-addon/matter_builder` | The Home Assistant add-on |
| `tools/mb_generate.py` | Turns a device spec (YAML) into a project |
| `tools/release_manifest.py` | Writes and signs a release manifest |
| `examples/` | Example device specs, and `examples/components/` (a component: a connection type of its own) |
| `components/mb_driver` | The interface for components |
| `.github/workflows/release.yml` | Builds and signs a release for a tag `v*` |
| `.github/workflows/catalog.yml`, `tools/build_catalog.py`, `catalog/` | The catalog of package sources on GitHub Pages, with the block and verified lists |
| `template/` | Start of a package source of your own |
| `matter-builder-key.pem` | The project's public key (the add-on ships the same one) |

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

A tag `v<version>` builds the generic device and the Octo bed bridge in GitHub Actions, writes
`manifest.json` with the SHA-256 of every image and signs it with ECDSA P-256
(secret `MB_SIGNING_KEY`). The add-on checks the signature against
`ha-addon/matter_builder/release-key.pem` and every image against its hash
before it installs anything.

## Status

| Part | State |
|---|---|
| Generic device 0.1.3 (released) | Tested on an ESP32-C6: setup access point, linking, connections with trial and rollback, IR send, Bluetooth presence next to Thread |
| Generic device 0.1.6, Octo bridge 1.0.16 (released) | Tested on an ESP32-C6: pairing over Bluetooth (stable with two controllers and Bluetooth presence next to it; 174 KB free with pairing on, 199 KB off), components build; the bed bridge added to Home Assistant with both beds, its number of beds changed from the add-on |
| Add-on 0.20.0 | Devices, Catalog and Configs tabs; visual and YAML editor; settings form per firmware; flashing on the Home Assistant host; adding a device to Home Assistant; updates over Thread when Wi-Fi is out of reach |
| Catalog | Built nightly on GitHub Pages from repositories with the topic `matter-builder-package` |

## License

MIT
