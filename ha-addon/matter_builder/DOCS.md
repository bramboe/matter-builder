# Matter Builder

Matter Builder sets up and maintains ESP32 devices that act as standard
**Matter devices**, over Wi-Fi or Thread. They work in Home Assistant, Apple
Home and Google Home at the same time, and keep working when Home Assistant
is off: every device has a settings page of its own.

The standard firmware is the **generic device** (ESP32-C6, Thread): relays,
LEDs, buttons, contacts, motion and SHT4x sensors on free pins, set up on the
device's page or in the add-on.

## What the add-on does

- **Proxies**: the devices you linked, with their firmware, network and
  connected devices, and a button to update them.
- **Linking**: give the address and password of a device's page, then press
  its BOOT button briefly (or switch its power off and on three times). The
  add-on gets a key of its own; it never stores the password. The device's
  page lists the add-on under *Programs with access*, where you can revoke it.
- **Connections** (generic devices): what is connected to a device, edited
  as a form or as YAML. Changes are saved as a draft on the device; *Apply*
  restarts it with the draft on trial, and *Keep* within 2 minutes makes it
  permanent. Without that, the device goes back by itself.
- **All proxies**: send the Wi-Fi network for configuration mode to every
  linked device at once, or update every device to the newest firmware of its
  own kind, one after the other.
- **Firmware**: ready-made firmware to install, and in **developer mode** your
  own builds.

## Ready-made firmware

Released firmware comes from the project's GitHub releases (option
`releases_repo`). Every release has a manifest with the SHA-256 of each image,
signed by the project; the add-on checks the signature against the key it
ships with and the hash of every image before installing anything. A release
that does not match is ignored.

Each firmware in a release also has a **factory image** for a new ESP32:
open it under *Firmware*, connect the ESP32 with USB to the computer you use
Home Assistant on, and choose *Install via USB*. That needs Chrome or Edge and
Home Assistant opened over HTTPS; a backup of the whole flash is saved first
unless you untick it. The ESP32 then opens its setup network, where you choose
its password and network; link it here afterwards.

## Own devices (developer mode)

Describe a device in YAML: its name, the application (generic) and its
connections as factory setting.
Wi-Fi, Thread and passwords are never in the spec: they are set on the
device's own page. *Build* generates the project (tools/mb_generate.py) and
produces an update file and an image for a new ESP32 over USB. The spec is
built into the firmware (GET /spec), so adopting the device recovers it.

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

A device on Thread is too slow for a 2 MB upload. The add-on therefore
switches it to *configuration mode* on Wi-Fi for a moment, uploads the
firmware in about 20 seconds, and the device returns to Thread. What is
connected to it is unavailable for a few minutes. If the new
firmware cannot get online, the device goes back to the previous one by
itself.

## Network

The add-on uses the host network: Thread addresses are routed through the
OpenThread Border Router, and a device in configuration mode is found with
mDNS. Its port is only answered for Home Assistant's ingress.
