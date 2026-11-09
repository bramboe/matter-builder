# Matter Builder

Matter Builder sets up and maintains ESP32 devices that act as standard
**Matter devices**, over Wi-Fi or Thread. They work in Home Assistant, Apple
Home and Google Home at the same time, and keep working when Home Assistant
is off: every device has a settings page of its own.

The standard firmware is the **generic device**: relays, LEDs, buttons,
contacts, motion and SHT4x sensors on free pins, set up on the device's page
or in the add-on. It runs on the ESP32-C6 (Thread or Wi-Fi) and the classic
ESP32 (Wi-Fi); see *Chips and networks*.

## What the add-on does

- **Devices**: the devices you linked, with their state, firmware and what
  is connected, and *Update to …* when a newer firmware of their kind exists.
  Devices found on the network but not linked yet show up with *Adopt*.
- **New device**: a wizard that installs firmware on a new ESP32 and links it.
- **Linking**: give the address and password of a device's page, then press
  its BOOT button briefly (or switch its power off and on three times; six
  times is recovery, like holding BOOT, for a device in a wall box). The
  add-on gets a key of its own; it never stores the password. The device's
  page lists the add-on under *Programs with access*, where you can revoke it.
- **Open page**: the device's own settings page, opened through the add-on.
  Over Thread a page takes half a minute to load, while what it asks the
  device afterwards takes about a second; so the add-on keeps the page (it
  fetches it once per firmware version, in the background) and passes the
  page's requests to the device unchanged. You sign in with the device's own
  password, as on the device itself. *Open on the device* (the ⋮ menu) opens
  the page directly, at the device's fixed `.local` name.
- **Logs**: the device's log, live, like ESPHome's Logs but without a USB
  cable. The device keeps its last 16 KB in RAM (from its boot on), and the
  dialog adds new lines every 2 seconds; warnings are orange, errors red.
  *Download* saves what is shown. Chatter (Matter's message-by-message
  lines, the Bluetooth stack's writes) stays on the serial port only.
  Firmware from before bridge 1.0.16 and generic 0.1.6 has no log yet.
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
- **Settings**: the Wi-Fi network for configuration mode of every device
  (leave the password empty to keep each device's own),
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
Before installing, the wizard checks that the board is the firmware's chip
and has flash enough (8 MB for the C6, 4 MB for the classic ESP32), and says
so when it is not.

## Chips and networks

| Chip | Network | Firmware |
|---|---|---|
| ESP32-C6 | Thread or Wi-Fi, chosen in the wizard | `mb_generic` (Thread), `mb_generic_wifi` (Wi-Fi) |
| Classic ESP32 | Wi-Fi | `mb_generic_esp32` |

The flash must hold the firmware twice (an update goes into the second
copy): the **ESP32-C6 needs 8 MB**, the **classic ESP32 4 MB** (the common
boards; its firmware is made smaller to fit). The wizard checks it before
installing.

**Thread** needs a Thread border router (Home Assistant Connect ZBT-1/ZBT-2,
an Apple TV or HomePod, a Nest Hub). It uses little power, forms a mesh that
reaches far, and keeps no Wi-Fi password on the device for normal use.
Updates switch to Wi-Fi for about a minute when the device can reach your
Wi-Fi; over Thread alone an update takes about half an hour, which is fine
at night (*Updating a device*).

**Wi-Fi** needs only your Wi-Fi network. Updates take seconds. It uses more
power and needs good Wi-Fi where the device is. A Wi-Fi device gets its
network on its setup page, or from the Home Assistant app when you pair it
over Bluetooth (the app gives it the network with the pairing). When it
cannot connect within three minutes, it opens its setup network again.

To change a C6 from one network to the other, install the other firmware
over USB.

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
    power: last        # at power-on: off (default), on, last (as it was), toggle
    off_after: 600     # seconds until it switches itself off (0: never)
    name: Pump
  - { slot: 1, type: contact, pin: 5, controls: 0, name: Door }
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

- **On the device itself**, also when Home Assistant is off: `power` (what
  an output does after a power cut), `off_after` (it switches itself off),
  and `controls` on a button, contact, motion or presence connection: a
  button toggles that output, the others switch it with them. With
  `off_after` on the output, it goes off that long after the input went
  inactive (a light that stays on while there is motion). Matter hears of
  every change, so controllers show it. The catalog has two recipes with
  these: *Light switch* and *Motion light*.

Wi-Fi, Thread and passwords never go in: they are set on the device itself.
A device's page can look for Wi-Fi networks (*Look for networks*) instead of
typing the name; on Thread the radio pauses for those few seconds.

## Rules on the device

A generic device runs rules of its own, like HAA's actions or ESPHome's
automations, but set as data instead of compiled: **when** something happens,
**only if** something holds, **then** up to six steps. They keep working when
Home Assistant is off. Set them in the device's *Configuration* (Rules), in
YAML, or on the device's own page.

- **When**: a button pressed (1-5 times) or held, an input becoming active or
  inactive, an output turning on or off (also by Home Assistant), a
  temperature or humidity crossing a value (with a little hysteresis), a time
  of day on chosen days, or the device starting.
- **Only if**: an input active or not, an output on or off, a temperature or
  humidity above or below a value, or between two times (over midnight too).
- **Then**: turn an output on, off or the other way, set a brightness, wait,
  or turn another rule on or off. A rule triggered again while it waits
  starts over, so a motion light stays on while there is motion.
- **A switch in Matter** per rule (*matter: true*): Home Assistant, Apple
  Home or Google Home turn the rule on and off ("motion light off at night")
  while the rule itself runs on the device. It shows up after a restart and
  keeps its place (rule n is always the same switch).
- **The clock** for time rules: the add-on sets it with Home Assistant's time
  zone (summer time included) and keeps it right; SNTP is tried as well,
  which on Thread needs NAT64 at the border router. On the device's page,
  *Set from this device* sets it from your phone.
- **Trying out without wiring**: `io simulate <n> press|long|active|inactive`
  and `io simulate <n> climate <centi °C> <centi %>` on the device, and *Run
  now* in a rule's menu.

```yaml
rules:
  - name: Night light
    when: { active: 1 }            # press: 3 + count: 2, long: 3, on: 0, above: 2 + value: 25.5,
                                   # time: "22:30" + days: [sat, sun], boot: true
    if: { time: "22:00-07:00" }    # on: 0, inactive: 1, below: 2 + value: 18
    do: [ {on: 0}, {wait: 120}, {off: 0} ]   # toggle, level: [0, 128], enable/disable/toggle_rule: <id>
    matter: true
  - name: Hot
    id: 3                          # its number on the device; the list position when left out
    when: { above: 2, value: 26 }
    do: [ {on: 4} ]
```

At most 12 rules; a step that triggers another rule may do so three levels
deep, and a rule runs at most ten times a second (a loop stops there).

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

**A Wi-Fi device** (classic ESP32, or a C6 on Wi-Fi) is updated straight
over its own network: a few seconds of upload, then a restart.

**Over Wi-Fi, by the device itself** (generic 0.1.8 and bed bridge 1.0.17
on): the add-on asks the device over Thread to fetch the firmware. The
device restarts on its Wi-Fi network for that moment, downloads the image
from Home Assistant (port 8098 of its address, with a one-time link), or,
when that Wi-Fi cannot reach Home Assistant (an IoT network often cannot),
the same release straight from GitHub. It checks the image against the
SHA-256 the add-on sends it (from the signed release), installs it and goes
back to Thread with it: about a minute. What is connected to it is
unavailable meanwhile. If the new firmware cannot get online, the device
goes back to the previous one by itself.

Your own builds (developer mode) are not on GitHub: for those the device's
Wi-Fi must reach Home Assistant, or the update goes over Thread.

**At night, by themselves**: turn on *Update automatically at night* in a
device's Firmware tab, and choose the hour under Settings (Night updates,
03:00 by default, in Home Assistant's time zone). At that hour the add-on
updates every such device to the newest firmware of its own kind, one after
the other; one that does not come back does not stop the others.

**Over Thread** when Wi-Fi does not work (no Wi-Fi network set on the
device, or it cannot connect) or for older firmware: roughly 1 KB per
second, about half an hour for a 2 MB image, with the progress shown in the
task bar. The device keeps working meanwhile.

## Network

The add-on uses the host network: Thread addresses are routed through the
OpenThread Border Router, and a device in configuration mode is found with
mDNS. Its port is only answered for Home Assistant's ingress.
