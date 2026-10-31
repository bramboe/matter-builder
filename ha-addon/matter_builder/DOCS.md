# Matter Builder

Matter Builder sets up and maintains ESP32 devices that act as standard
**Matter devices**, over Wi-Fi or Thread. They work in Home Assistant, Apple
Home and Google Home at the same time, and keep working when Home Assistant
is off: every device has a settings page of its own.

The first application is the **Octo bed bridge** (ESP32-C6, Thread).

## What the add-on does

- **Proxies**: the devices you linked, with their firmware, network and
  connected devices, and a button to update them.
- **Linking**: give the address and password of a device's page, then press
  its BOOT button briefly (or switch its power off and on three times). The
  add-on gets a key of its own; it never stores the password. The device's
  page lists the add-on under *Programs with access*, where you can revoke it.
- **Firmware**: ready-made firmware to install, and in **developer mode** your
  own builds.

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
firmware in about 20 seconds, and the device returns to Thread. The beds (or
other connected devices) are unavailable for a few minutes. If the new
firmware cannot get online, the device goes back to the previous one by
itself.

## Network

The add-on uses the host network: Thread addresses are routed through the
OpenThread Border Router, and a device in configuration mode is found with
mDNS. Its port is only answered for Home Assistant's ingress.
