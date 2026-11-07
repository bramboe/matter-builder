# Matter Builder

Matter Builder sets up and maintains ESP32 devices that act as standard
**Matter devices**, over Wi-Fi or Thread. They work in Home Assistant, Apple
Home and Google Home at the same time, and keep working when Home Assistant
is off: every device has a settings page of its own.

The standard firmware is the **generic device** (ESP32-C6, Thread): relays,
LEDs, buttons, contacts, motion and SHT4x sensors on free pins, set up on the
device's page or in the add-on.

## What the add-on does

- **Devices**: the devices you linked, with their state, firmware and what
  is connected, and *Update to …* when a newer firmware of their kind exists.
  Devices found on the network but not linked yet show up with *Adopt*.
- **New device**: a wizard that installs firmware on a new ESP32 and links it.
- **Linking**: give the address and password of a device's page, then press
  its BOOT button briefly (or switch its power off and on three times). The
  add-on gets a key of its own; it never stores the password. The device's
  page lists the add-on under *Programs with access*, where you can revoke it.
- **Configuration**: every device's configuration as a form or as YAML.
  For a generic device you edit its name and connections; changes are saved
  as a draft on the device, *Apply* restarts it with the draft on trial, and
  *Keep* within 2 minutes makes it permanent (without that, it goes back by
  itself). Firmware with settings of its own (such as the number of beds of
  the bed bridge) shows them as a form or as a `settings:` block in YAML;
  saving checks them and restarts the device. Other firmware shows what it
  reports, read-only.
- **Configs**: your configurations (YAML) kept in the add-on: install one on a
  new ESP32, apply it to a linked device, or save a device's configuration as
  one. The New device wizard asks for a configuration too and applies it once
  the device is linked.
- **Catalog**: firmware and recipes from the official source and from
  community sources, with a page per item.
- **Settings**: the Wi-Fi network for configuration mode of every device,
  package sources, developer mode. *Update all* (top bar) updates every device
  to the newest firmware of its own kind, one after the other.

## Ready-made firmware

Released firmware comes from the project's GitHub releases (option
`releases_repo`). Every release has a manifest with the SHA-256 of each image,
signed by the project; the add-on checks the signature against the key it
ships with and the hash of every image before installing anything. A release
that does not match is ignored.

Each firmware in a release also has a **factory image** for a new ESP32.
*New device* installs it in one of three ways:

- **Plug into this computer**: over USB from the browser. Needs Chrome or
  Edge, and Home Assistant opened over HTTPS. The flash backup is downloaded.
- **Plug into the Home Assistant machine**: the add-on flashes it itself with
  esptool; works in any browser. The flash backup goes to
  `/share/matter-builder/backups`.
- **Download the file**: flash it yourself at address 0.

The ESP32 then opens its setup network, where you choose its password and
network; link it afterwards, and the wizard continues with its connections.

## Catalog and package sources

The catalog (built nightly on the project's GitHub Pages) lists firmware and
recipes from every package source: GitHub repositories with the topic
`matter-builder-package`.

- A **recipe** is a generic device's connections, nothing else. *Use* puts it
  on a new ESP32 or fills in a linked generic device's connections; nothing
  changes on the device until you save and apply.
- **Firmware** from a community source is only installed after you added the
  source: the add-on then pins the source's key (you see its fingerprint
  first), and from then on only accepts releases signed with that key.
- The first community install asks you to confirm that the project did not
  test or audit community items.

Sources are listed under Settings, where you can add one by its GitHub name
or remove it. To make a source of your own, start from the `template` folder
of the project's repository.

## Configuration YAML

One format for devices, configs, recipes and the wizard:

```yaml
device:
  name: Shed
  app: generic

connections:
  - slot: 0
    type: relay        # relay, led, button, contact, motion, sht4x
    pin: 4             # 0-7, 10, 11, 18-23
    name: Pump
  - { slot: 1, type: contact, pin: 5, name: Door }
  - slot: 2
    type: sht4x
    pin: 6             # SDA
    scl: 7
    name: Climate
  - { slot: 3, type: ir_send, pin: 18, code: "nec:00FF:A25D", name: TV power }
  - { slot: 4, type: ir_receive, pin: 19, code: "nec:00FF:629D", name: Remote up }
  - { slot: 5, type: ble_sensor, mac: "A4:C1:38:12:34:56", name: Kitchen }
  - { slot: 6, type: ble_presence, mac: "C8:4F:0E:00:11:22", name: Keys }
```

- **IR** (NEC codes, written like ESPHome: `nec:<address>:<command>`): an IR
  LED through a transistor sends a code (a switch that turns itself off), an
  IR receiver turns a code into a button press. Several codes may share one
  LED or receiver; at most two pins of each. To learn a code, point a remote
  at a receiver: the editor offers the last code it saw.
- **Bluetooth** listens only: thermometers that broadcast their readings
  (BTHome, or Xiaomi thermometers with the pvvx/ATC firmware) and tags with a
  fixed address for presence (seen in the last minute). It starts only when a
  connection needs it.

Wi-Fi, Thread and passwords never go in: they are set on the device itself.

## Pairing over Bluetooth

A device can be paired directly by a phone over Bluetooth (Matter's own
pairing, as most Matter devices do), also the first time: the phone gives it
the Thread network, so the setup network is not needed. It is a switch on
the device's page and in the device's Info here, off by default, and takes
effect after a restart:

- **Off**: pairing goes with the code (QR) over the network. Matter lets go
  of Bluetooth right after starting; Bluetooth only runs for Bluetooth
  connections. More memory free, and the whole radio for Thread.
- **On**: Matter keeps Bluetooth for pairing (also for adding a controller
  while the pairing window is open); Bluetooth connections listen along.
  It costs memory (the Bluetooth layer stays loaded) and radio time.

A device without a Thread network waits to be paired over Bluetooth while
the switch is on, instead of opening its setup network after 3 minutes.

## What a device needs

Devices, configs and catalog items show what they need: their network
(Thread or Wi-Fi) and Bluetooth when it is used for what they do, such as a
Bluetooth thermometer or a package that controls a device over Bluetooth.
The add-on refuses a config whose connections need more than the board has,
and IR or Bluetooth connections on generic firmware older than 0.1.3.

## Components

A component adds a connection type of its own to the generic firmware: a
folder with a `component.yaml` (type, title, which Matter device it is, how
many pins, an optional number setting) and an ESP-IDF component that
registers it (`components/mb_driver`). A config lists its components:

```yaml
components:
  - source: github:owner/repo/path/to/component@v1.0.0   # a tag or commit
  - source: local:examples/components/pulse_relay        # in this repository
connections:
  - { slot: 0, type: pulse_relay, pin: 4, number: 2, name: Gate }
```

The editors then offer the type with its fields. A component is code, so a
config with components is installed after a build (developer mode), or as a
package from the catalog; a device only accepts types its firmware has.
`examples/components/pulse_relay` is an example: a relay that switches on
for a number of seconds.

## Own builds (developer mode)

In developer mode a config can also be built into firmware of its own
(*Build* in its menu): its connections become the factory setting.
*Build* generates the project (tools/mb_generate.py) and produces an update
file and an image for a new ESP32 over USB. The config is built into the
firmware (GET /spec), so adopting the device recovers it.

## Developer mode

Developer mode builds firmware inside Home Assistant, with fixed versions of
ESP-IDF (v5.5.5) and esp-matter (release/v1.6):

1. *Install toolchain* downloads esp-matter and the parts of connectedhomeip
   the ESP32 build needs: about 6 GB, 15 minutes or more. It is not included
   in backups.
2. *Build* compiles the firmware. The first build for a chip takes half an
   hour or more, depending on the processor cores and memory of your host;
   later builds reuse the cache and take minutes.

Nothing personal is built into the firmware: Wi-Fi, Thread network and the
password are set on the device's own page.

## Updating a device

Thread is slow for a 2 MB upload. The add-on therefore switches the device
to *configuration mode* on Wi-Fi for a moment, uploads the firmware in about
20 seconds, and the device returns to Thread. What is connected to it is
unavailable for a few minutes. If the new firmware cannot get online, the
device goes back to the previous one by itself.

When Home Assistant cannot see the device on Wi-Fi (it is on another network
or VLAN, or mDNS does not get across), the add-on waits until the device is
back on Thread (at most 15 minutes) and sends the firmware over Thread
instead, which takes several minutes. It remembers this, and later updates
of that device go over Thread straight away.

## Network

The add-on uses the host network: Thread addresses are routed through the
OpenThread Border Router, and a device in configuration mode is found with
mDNS. Its port is only answered for Home Assistant's ingress.
