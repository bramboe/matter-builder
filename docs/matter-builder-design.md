# Matter Builder: an ESPHome alternative based on Matter

Status: design, partly built. Written 2026-10-29; updated 2026-10-31 (layers, management, adoption and access) and 2026-11-01 (packages, catalog, pages and flashing, §16).
Reference: ESP-IDF v5.5.5, esp-matter release/v1.6 (fixed commit), ESP32-C6.

## 1. Goal

A reproducible way to make ESP32 devices that present themselves as **standard Matter devices**, over Wi-Fi or Thread depending on the chip. As easy as ESPHome: describe the device in YAML, an app in Home Assistant installs the firmware, and HA adds it through the regular Matter integration.

Principles:
- No integration or API of our own in HA. HA only sees the Matter device.
- You describe Matter **device types**, not HA entities.
- Every build is reproducible: fixed toolchain versions, a spec in git, and a manifest with hashes.
- **Users do not edit files.** Everything a user sets goes through Matter or the device's local settings page: Wi-Fi, the Thread network, passwords, and modes such as configuration mode. What HA already knows (the Thread dataset, and the Wi-Fi details when HA has them) the add-on takes over. Values built into the firmware are at most a fallback for developers.
- **A device works on its own.** After installation HA is not needed: settings, GPIOs of the configurable layer (§6), Wi-Fi, Thread and updates go through the local page. HA is an extra manager, not a requirement. The add-on is needed for a new device, a new built driver, another board or network type, and a full reinstall.
- **Ordinary users do not build.** They install the add-on, pick ready-made firmware (the configurable generic firmware or a package), install it over USB or the network, and set everything up on the page or in the add-on. No toolchain, no waiting. That firmware is built reproducibly per release (fixed versions, same toolchain as the add-on) and signed.
- **Developer mode: HA as builder.** Whoever wants a driver or application of their own turns on developer mode. The add-on then downloads the toolchain at fixed versions and builds itself, with build and crash logs in view; a new Matter or Thread version only arrives when the add-on is updated. No external web flasher: ready-made firmware is installed by the add-on itself (§16.5).
- A second, simple application proves that the design is generic.

Not goals for v1: Matter certification, a Vendor ID of our own, publication in the add-on store, and devices that do not fit Matter device types.

## 2. Why not just ESPHome

| | ESPHome | Matter Builder |
|---|---|---|
| Link with HA | Native API of its own plus the ESPHome integration | Standard Matter |
| Other controllers | HA only | Multi-admin: Apple Home, Google and HA at once |
| Thread | No | Yes, on C6, H2 and C5 |
| Free-form entities (text, select, named button) | Yes, anything | No, only what Matter clusters and the HA Matter integration support |
| Live logs, OTA | Built in | Our own (§8) |
| Firmware footprint | Small | Large (±1.5–1.8 MB, much RAM) |

The trade: more standard and Thread, for less freedom in what you can offer.

## 3. Architecture

```
 ┌──────────────── Home Assistant ──────────────────────────────────────┐
 │                                                                       │
 │  Matter Builder (add-on, ingress)             Matter integration      │
 │   ├ editor (YAML)                             └ Matter server         │
 │   ├ generator  ── spec → ESP-IDF project           ▲                  │
 │   ├ builder    ── ESP-IDF + esp-matter (fixed)     │ commission       │
 │   ├ provisioner── factory NVS, QR ─────────────────┘ over IP          │
 │   ├ flasher    ── Web Serial (browser) / OTA                          │
 │   └ backups    ── flash read/restore                                  │
 │                                                                       │
 │  Thread/OTBR integration ─ dataset ─▶ provisioner                     │
 └───────────────────────────────────────────────────────────────────────┘
                     │ USB (Web Serial) or network
                     ▼
     ┌──── ESP32 device ─────────────────────────────────┐
     │ generated: app_main, endpoints, sdkconfig          │
     │ runtime:   mb_core (capabilities, bindings, log)   │
     │ components: drivers (gpio, i2c, ble, …)            │
     │ factory partition: discriminator, passcode,        │
     │                    Wi-Fi details or Thread dataset │
     └────────────────────────────────────────────────────┘
```

Four layers:
1. **Spec** (YAML), §4
2. **Component library** (ESP-IDF components of a fixed shape), §5
3. **Generator** (spec → ESP-IDF project), §6
4. **Builder app** in HA (build, provision, flash, add), §7

## 4. The spec

### 4.1 Shape

The first idea, more general than what is built now (§16.2 has the current device YAML):

```yaml
device:
  name: Shed
  board: esp32-c6            # board profile, see §9
  transport: auto            # auto | wifi | thread
  bridge: true               # aggregator + bridged nodes
  vendor_id: 0xFFF1          # test VID, see §10
  product_id: 0x8001

network:
  thread: from_ha            # dataset from HA's Thread integration

components:
  - id: lamp
    driver: gpio_switch
    pin: 4
  - id: climate
    driver: i2c_sht4x
    pins: { sda: 6, scl: 7 }

endpoints:
  - name: Lamp
    bridged: true
    children:
      - { type: on_off_plug_in_unit, source: lamp.state }
  - name: Climate
    bridged: true
    children:
      - { type: temperature_sensor, source: climate.temperature }
      - { type: humidity_sensor,    source: climate.humidity }
```

### 4.2 Rules
- `source: <component>.<capability>` links an endpoint to a capability of a component. The generator checks that the capability type fits the device type: a `position` fits `window_covering`, a `bool` fits `on_off_*`, and so on.
- `bridged: true` means a Bridged Node under the Aggregator, with its own `NodeLabel`.
- Secrets never go into a spec: they are set on the device (§1).
- A spec without `bridge` gives a plain device with endpoints directly under the root.

## 5. Component library

### 5.1 Shape of a component
Every component is an ESP-IDF component with:

```
components/<name>/
  component.yaml      # schema: config fields, capabilities, needs (ble, i2c, psram)
  include/<name>.h
  <name>.cpp          # driver
```

A driver implements a small, fixed interface of `mb_core`:

```cpp
struct Capability {            // offered by the driver
  const char* id;              // "head", "light", "temperature"
  CapType type;                // Position, Bool, Level, Temperature, Event, Mode …
  // driver → Matter: the driver calls core.report(cap, value)
  // Matter → driver: the core calls cap.on_write(value)
};

class Driver {
 public:
  virtual esp_err_t setup(const Config&) = 0;
  virtual std::span<Capability> capabilities() = 0;
  virtual void loop() {}         // optional, through an mb_core task
};
```

### 5.2 Bindings (in `mb_core`, not per driver)
A binding links a capability type to a Matter cluster:

| Capability | Matter device type | Clusters / attributes |
|---|---|---|
| `Position` (0–100) | Window Covering | Lift, PositionAware; `TargetPositionLiftPercent100ths`, `CurrentPosition…`, `OperationalStatus` |
| `Bool` | On/Off Light / Plug-in Unit | OnOff |
| `Level` | Dimmable Light | OnOff + LevelControl |
| `Color` | Extended Color Light | OnOff + LevelControl + ColorControl (hue/saturation, xy, colour temperature); the LED strips of `generic` |
| `Temperature` | Temperature Sensor | TemperatureMeasurement |
| `Humidity` | Humidity Sensor | RelativeHumidityMeasurement |
| `Contact` | Contact Sensor | BooleanState |
| `Occupancy` | Occupancy Sensor | OccupancySensing |
| `Event` (button) | Generic Switch | Switch (momentary, events) |
| `Mode` (select) | Mode Select | ModeSelect |

The point: the Matter-specific code (attribute callbacks, updating `OperationalStatus`, reporting) exists once.

### 5.3 First components
- `gpio_switch`, `gpio_binary` (relay, contact, button)
- `i2c_sht4x` or `i2c_bme280` (temperature and humidity)
- `ble_central`: a shared NimBLE layer for drivers that control BLE devices, for packages

## 6. Generator

A CLI (`tools/mb_generate.py <spec>`), also called by the add-on. The output is a complete ESP-IDF project:

| File | Contents |
|---|---|
| `main/app_main.cpp` | init of NVS, network, `mb_core`, drivers and the Matter node |
| `main/endpoints.cpp` | endpoints through esp-matter's dynamic data model (`endpoint::…::create(…)`, `set_parent_endpoint`), so without ZAP |
| `main/CMakeLists.txt` | the components used |
| `sdkconfig.defaults` | board profile + transport + feature flags (NimBLE, CHIPoBLE on/off, OpenThread) |
| `partitions.csv` | per board profile, with factory NVS and two OTA slots |
| `build-manifest.json` | spec hash, component versions, toolchain versions |

Decisions:
- **Two layers.** A spec describes both; the page only changes the configurable layer.
  - **Built:** specific drivers (packages), the board, the network type and which components are in the firmware. Generated and built by the add-on or CI.
  - **Configurable:** standard components that are in the firmware (switch/relay, button, contact, LED/PWM, a few common sensors, simple BLE writes). Which pin, which type and which name is kept in NVS and set on the page. The endpoints are made at start.
  - There is **no YAML parser** in the firmware. The page sends structured settings, and the firmware keeps them compactly in NVS.
- **The spec travels along.** Every device keeps its spec and `build-manifest.json` (GET /spec). When someone changes the configurable layer on the page, the device updates that part. So an add-on can adopt a device including its YAML, even when it was made on another system (§7.4).
- **Fixed endpoint numbers.** Every component has a fixed id tied to an endpoint number, so entities in HA and Apple Home do not shift on a change.
- **Safe pins and applying with fallback.** The board profile knows which pins must not be used (strapping, flash, USB); the page only offers safe pins, with a test button per pin. A new setting is applied on trial: without confirmation within 2 minutes, or after a crash, the previous one comes back (`boot_guard`).
- The generator is deterministic: the same spec and versions give exactly the same project, which CI can check with a diff.
- A generated project also builds without the builder (`idf.py build`). You are not tied to the tool.

## 7. Builder app in HA (add-on)

### 7.1 Image
- **Two modes** (§1). By default the add-on is small: it takes signed, ready-made firmware from the project's releases and installs it. **Developer mode** downloads the toolchain to `/data` (not in backups) and builds itself.
- **Releases**: `.github/workflows/release.yml` builds for a tag `v*` with `ha-addon/matter_builder/{toolchain,build}.sh`, writes with `tools/release_manifest.py` a manifest (file, application, version, chip, size, SHA-256; factory images; package descriptions) and signs it with ECDSA P-256 (secret `MB_SIGNING_KEY`). The add-on checks the signature with `release-key.pem` and every image's hash, and ignores a release that does not match. The private key is kept outside the repository.
- Same source and versions give the same image; this can be checked by rebuilding it in developer mode.
- Measured (HA as an amd64 VM): toolchain installation ±15 min, first build for the ESP32-C6 ±35 min. Later builds reuse the build folder and ccache in `/data`. Build time mostly depends on the host's processor cores and memory (±1.5 GB per parallel compile step); the add-on shows both.
- Base: Debian, with **fixed versions** of ESP-IDF v5.5.5, esp-matter release/v1.6 (fixed commit) and the connectedhomeip submodule at a fixed commit.
- Architectures: amd64 and aarch64 (HA Green, Pi 5). The connectedhomeip host tools must build on aarch64; a risk to test early.

### 7.2 Ingress UI
The page layout was revised in §16.4; the functions below stay, in their new place.

1. **Management** (all devices): name, chip, network and signal, firmware version, online or "differs", and what is connected. **Send to all devices** for:
   - firmware updates, first on one device as a trial and then the rest one by one, only when the first comes back healthy;
   - Wi-Fi: the device tests the new network and keeps the old one when that fails;
   - time zone and NTP; log level and the target of live logs (§8.3);
   - configuration mode: timeout, and the setup network on or off with a password;
   - renewing tokens; Matter maintenance (open the pairing window, remove old controllers);
   - backup of settings, to be restored on a replacement device.

   Not to all devices: the Thread dataset (Thread spreads a new network itself with a pending dataset; the add-on only shows a difference) and settings that differ per device.
2. **Per device**: the same data in a **UI mode** and a **YAML mode**, like HA's automation editor. The YAML has two blocks: *settings* (configurable layer, active at once) and *firmware* (built layer, needs a build and OTA). Before applying, the add-on shows a diff marked "direct" or "build needed". The add-on keeps the wanted state, the device reports the actual one; when something was changed on the page, the add-on shows "changed on device" and you choose to take it over or set it back.
3. **Keys**: per device what the add-on received, behind an eye with a copy button: the add-on's token (relink, revoke) and the Matter pairing code with QR. Once, for all devices: Wi-Fi and the Thread dataset (read-only, from the Thread integration). The page password is not here: it belongs to the user and stays on the device.
4. **Build**: log stream, giving `firmware.bin`, `factory.bin` and the manifest.
5. **Flash**: Web Serial in the browser (esptool-js), like ESPHome, always with a full flash backup first (§11).
6. **Add**: QR code and the manual pairing code, plus "Add to HA" (§7.3).
7. **Maintenance**: logs, backup and restore, fabrics.

### 7.3 Provisioning and commissioning
The add-on makes a **factory NVS partition** per device (esp-matter `mfg_tool`):
- a unique discriminator and a random passcode, instead of the test code 20202021;
- test DAC/PAI for the test VID;
- **network details**: for Thread the *preferred dataset* from HA's Thread integration (OTBR), through the HA API.

The device is then on the network right after flashing and commissionable through DNS-SD. The add-on asks the Matter server for **on-network commissioning** with the known passcode. No phone and no BLE commissioning needed.

Fallback: CHIPoBLE on, and pairing with the HA companion app and the QR code. An option per spec (`commissioning: ble`).

To check: whether the Matter server's websocket API (`/ws`, port 5580; see §8.1) supports commissioning over IP with only a setup code, and how the add-on reaches that API.

### 7.4 Adoption and access
- **The page password belongs to the user.** They choose it at the first visit of the page. It is the only secret a person needs.
- **Every program gets a token of its own**, issued by the device: the add-on, or later a script. The device keeps only a hash in NVS. The page lists them ("Home Assistant, since Oct 31") with **Revoke**.
- **Linking needs the password and physical presence.** The add-on asks for a link with the page password; the device opens a 2-minute window and only issues a token when the BOOT button is pressed within that time. When the button cannot be reached, switching the power off and on three times counts instead. "Already a Matter admin in HA" does **not** count: no adoption without a physical step.
- **Adopting** (a device made elsewhere): after linking the add-on reads the device's spec, manifest and settings (§6), and can rebuild it identically. With the adoption the right to send signed firmware moves to the new owner, under the same conditions.
- **Forgotten password**: with the device in hand. Holding the BOOT button for a few seconds opens recovery (setup network, choose a new password); connections, Matter pairings and network stay. Program tokens stay valid.

## 8. OTA, logs and diagnostics

Principle (researched 2026-10-31): **HA processes, the ESP stays simple.** Everything heavy happens in the add-on: making the image, keeping versions, decoding logs, analysing crashes. The ESP only sends raw bytes.

### 8.1 Context: HA's Matter server changed
Since Matter Server add-on 9.0 (June 2026) HA runs **matterjs-server** (TypeScript, matter.js). python-matter-server is archived. Everything here targets matterjs-server, including the commissioning API of §7.3.

### 8.2 OTA: through HA's regular Matter update (preferred)
matterjs-server has a built-in OTA provider that also accepts **local `.ota` files**:
- `--ota-provider-dir <path>` plus `--enable-test-net-dcl`; according to its README both are needed for files of your own. The server imports the `.ota` files at start and removes them afterwards.
- There is also an upload mechanism (`--ota-upload-max-size-mb`, default 64 MB, staging in `ota-uploads/`), which might let the add-on upload without a shared folder or restart. To research.
- HA then shows a regular **update entity** for the device. This works over **Wi-Fi and Thread** (BDX runs over the Matter session), and needs no extra firmware code: the OTA requestor is in esp-matter by default.

What the add-on does for it:
1. Raise `SoftwareVersion` with every build. An update is only offered and accepted when it is higher.
2. Make an `.ota` file with `ota_image_tool.py` (connectedhomeip) with the right VID/PID and version.
3. Put the file in the provider folder or upload it.

Still to check:
- ~~whether HA's Matter Server add-on offers these options~~ **Checked 2026-10-31**: Matter Server add-on 9.3.0 has `enable_test_net_dcl`, and `--ota-provider-dir` can be passed through `matter_server_args`. But that folder must be inside the Matter server's container, which another add-on cannot simply reach. The server's **upload API** is therefore the better route, still to research;
- whether test VID `0xFFF1` works with a local file. Google does not support it, but matter.js is another implementation.

**Without HA**: Apple Home does not update our devices. Apple looks for updates in the production DCL, which only lists certified products with a Vendor ID of their own; test VID `0xFFF1` is only in the test DCL. So without HA an update goes through the page: "Firmware update" with a file. On Thread the device switches itself to configuration mode on Wi-Fi for that (upload ±20 s instead of over an hour) and back. `boot_guard` stays the safety net.

Fallback: HTTP OTA from the add-on to `http_api` (`/ota`). Fast on Wi-Fi; possible but slow on Thread.

**Delta OTA** (`esp_delta_ota` from idf-extra-components): the add-on makes a patch against the previous build, which it has through the manifest. Saves much airtime on Thread, but **no** flash space, since the old image must stay intact until the new one is done. Later; full OTA first.

### 8.3 Live logs
Three levels, all processed in the add-on:

| Level | How | Wi-Fi | Thread |
|---|---|---|---|
| **Live** | The firmware sends log lines as UDP packets to the add-on (syslog-like, IPv6). The add-on shows them, like `esphome logs`. | yes | yes (the HA host is reachable through OTBR, low volume) |
| **On request** | Matter **Diagnostic Logs cluster** (0x0032): `RetrieveLogsRequest`, small logs inline and large ones over BDX. A ring buffer in the firmware. | yes | yes |
| **After a crash** | ESP-IDF core dump to a flash partition, fetched after the restart (HTTP or Diagnostic Logs with intent `CrashLogs`). The add-on decodes with the ELF of exactly that build (manifest). | yes | yes |

**Tokenized logging** fits "HA processes": with `pw_tokenizer` (Pigweed, already in connectedhomeip) log texts are replaced by 4-byte tokens at build time. The texts are in a token database in the add-on, and the ESP only sends tokens and arguments. Pigweed quotes ±90% less flash per log message and ±70% fewer bytes over the air: ideal for Thread, and it frees flash (§8.5). It only works for our own logs; ESP-IDF's and CHIP's internal logs are much more work and outside v1.

Whether matterjs-server supports the Diagnostic Logs cluster as a client was not found. So the add-on does it itself: through the Matter server API it is a controller on the fabric and can send the command. To research; live over UDP comes first.

### 8.4 Diagnostics in HA
HA only shows what Matter offers as standard (Basic Information, General Diagnostics, Wi-Fi/Thread Network Diagnostics). Heap or uptime cannot be HA entities. The add-on UI shows them, from the live logs or a small status line in the UDP stream.

### 8.5 Firmware size

**Measured** on a first BLE bridge application (classic ESP32, 4 MB, 1.84 MB slots): the image is **1.76 MB, 96% of an app slot**. Static DRAM is 108 KB (87%).

| Block | Flash |
|---|---|
| Matter (`libCHIP` + `libesp_matter`) | 425 KB |
| Wi-Fi (`net80211`, `pp`, `phy`, `wpa_supplicant`) | 280 KB |
| BLE (`bt`, `btdm_app`) | 176 KB |
| `libstdc++` | 143 KB |
| ±150 KB rodata attributed to `esp_app_desc.c` | probably merged string constants (log texts and such) of the whole firmware |
| libc, lwip, mbedtls, FreeRTOS, … | ±300 KB |
| **The application itself** | **66 KB (±4%)** |

**Conclusion on moving logic to HA:** the application logic is only ±4% of the image. The image is large because the Matter stack and radios are large, so moving logic to HA hardly makes it smaller. It would also need another protocol between ESP and HA, which is ESPHome or an integration of our own again. With a fully "thin" ESP, Matter runs on the HA host (a bridge such as Matterbridge), but then HA talks to itself; only useful for Apple or Google.

Where processing in HA **does** help:
1. **Code generation (already in the design).** Only the clusters and device types of the spec are included. esp-matter documents this ("Excluding unused Matter clusters").
2. **Tokenized logs (§8.3)**: log texts leave the flash.
3. **Decoding in HA**: no symbols or textual error messages needed on the device.
4. **The generator picks the known savings per board profile.** Espressif's figures (ESP32-C3, light example):

| Setting | Effect |
|---|---|
| `CONFIG_ENABLE_CHIP_SHELL=n` | −66 KB flash, +10 KB heap |
| `CONFIG_NEWLIB_NANO_FORMAT=y` | −47 KB flash |
| Max. dynamic endpoints and device types at the real count | ±550 B RAM each |
| Trimming NimBLE: only the roles needed, max. connections | −22 KB flash, +19 KB heap at boot |
| Smaller event log buffers and queues | +6.5 KB heap (risk: missed events) |

Also: a lower `LOG_DEFAULT_LEVEL` in release builds, assertions at level 1, and moving code from IRAM to flash.

5. **Smarter partitions.** On 4 MB nearly everything goes to two app slots. With NVS and coredump added, every profile needs its own sum. The builder refuses a build that does not fit, like ESPHome.

Verdict: the classic ESP32 with Wi-Fi + BLE central + Matter stays the edge case; the measures above might free 150–250 KB. **Advice: C6 (8 MB) as standard and S3 for heavy BLE bridges.** The C6 already has a partition table with 3 MB slots.

## 9. Board profiles and transport

| Profile | Transport | Flash/PSRAM | Suited for |
|---|---|---|---|
| `esp32` (classic) | Wi-Fi | 4 MB / – | Light applications only (heap ±33 KB, dipping to 14 KB at commissioning for a BLE bridge). Not built for now. |
| `esp32-s3-n16r8` | Wi-Fi | 16 MB / 8 MB | Heavy applications: BLE bridges, many endpoints |
| `esp32-c6` | Wi-Fi **or** Thread | 4–8 MB / – | **Standard board**: sensors, relays, small bridges |
| `esp32-h2` | Thread | 4 MB / – | Low-power sensors (possibly Thread SED) |
| `esp32-c5` | Wi-Fi 2.4/5 GHz or Thread | 4–8 MB / – | Optional, later |

`transport: auto` picks Thread when the chip can and HA has a Thread dataset, else Wi-Fi. Wi-Fi and Thread at once (C6) is not supported; a device picks one.

Every profile fixes: target, partition table, sdkconfig fragment, BLE limits, and whether OTA slot B fits.

## 10. Identity and certificates

- Test VID `0xFFF1`, PID per application (a fixed table in the repository), test DAC per device.
- So HA and Apple show the device as uncertified. Fine for own use.
- The number of fabrics is dynamic: never hard-code it.

## 11. Safety and recovery

- Always a full flash backup (`esptool read_flash 0 ALL`) before the first Matter flash on an existing device. Backups never go into git.
- USB speed set per board.
- `boot_guard` is part of `mb_core`: after repeated crashes the device starts in a safe mode with only network and OTA.
- The BOOT button has three clearly different presses, so nothing is wiped by accident:

  | Press | Function |
  |---|---|
  | short | confirm a link (§7.4), only while one is pending |
  | hold 3 s | a menu, shown on the status LED as blinks: 1 leave, 2 restart, 3 setup network (recovery, new page password), 4 unpair, 5 forget the network. A short press goes to the next, holding 3 s does it |
  | hold 10 s | factory reset |

  Switching the power off and on three times (within 10 s each) counts as a short press, for devices whose button cannot be reached.
- Only signed firmware is accepted once adoption exists (§7.4).

## 12. Origin

`mb_core` was extracted from a first application, a Bluetooth bridge for adjustable beds that now is a package of its own (§16.2): its boot guard, console commands, HTTP API, settings page, configuration mode and Thread dataset handling became the shared core. Acceptance criterion: an application on `mb_core` behaves in HA exactly as before (same endpoints, entities and NodeLabels).

## 13. Phases

| Phase | Contents | Done when |
|---|---|---|
| **F0** Restructure | Extract `mb_core` from the first application, no new functions | The application builds and works on the new structure |
| **F1** Generator CLI | `mb_generate.py <spec>` → project | A generated firmware behaves identically (§12) |
| **F2** Second application | C6 with SHT4x + relay on Thread | Works in HA over Thread, without application code in `mb_core` |
| **F3** HA add-on | build, Web Serial flash, factory NVS, QR, on-network commissioning (Wi-Fi + Thread dataset from HA) | A new device from spec to HA entity without a terminal |
| **F4** OTA and logs | `.ota` through matterjs-server (update entity in HA), UDP log stream, coredump decoder, then tokenized logs | An update without USB through HA, live logs in the add-on |
| **F5** Finish | documentation, component schemas, CI with a reproducibility check | A third application can be added by only writing a component |

## 14. Decisions

Open:
1. **Thread first in v1** with the C6 as standard (proposal), or Wi-Fi first?
2. **Publication**: own HA only for v1 (proposal), or later a public add-on? That decides whether a Vendor ID of our own is worth it.
3. **Name**.

Taken (2026-10-31):
- **Autonomous, with HA as extra**: a device works without HA after installation (§1).
- **Two layers** instead of only generating code (§6).
- **Linking** with the page password and a physical act; being a Matter admin is no proof (§7.4).
- **Ordinary users install ready-made, signed firmware; building in HA is developer mode**; no external web flasher (§1).

Taken (2026-11-01):
- **A generic repository**, `bramboe/matter-builder`, public, in English.
- **Three layers**: standard firmware, packages, developer mode (§16.1). Specific applications are packages, not the core.
- **Package sources with a key of their own**: specific projects are kept by their makers, not by the project (§16.2).
- **Catalog** without a server, through a GitHub topic and GitHub Pages (§16.3).
- **Three pages**: Home, Create, Catalog, plus Settings; the catalog leads to Create (§16.4).

## 15. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| connectedhomeip host tools do not build on aarch64 | Developer mode does not work on HA Green/Pi | Test early; ready-made releases do not need it |
| Image/toolchain too large for the HA host | Installation fails or the disk fills up | Toolchain only in developer mode, in `/data`, outside backups |
| On-network commissioning not available through the Matter server API | A phone stays needed | Fallback BLE through the companion app |
| Local OTA with test VID does not work in matterjs-server, or HA's add-on does not offer the options | Updates only over HTTP or USB | HTTP OTA as fallback (§8.2) |
| Image does not fit a slot | No OTA anymore | Savings of §8.5, size check in the builder, C6/S3 |
| Device types do not cover an application | Function not visible in HA | Say so when designing the spec; no vendor clusters as a way out (HA does not show them) |
| RAM on the classic ESP32 | Unstable at commissioning | Board profile with limits, recommend C6/S3 |

## 16. Packages, catalog and flashing (2026-11-01)

### 16.1 Three layers
1. **Standard firmware (`generic`)**: the product itself. One signed firmware per chip, with general building blocks (relay, LED, button, contact, motion, SHT4x; later for example PWM/dimmer, analog input, general I2C sensors, UART). Set up with YAML in the add-on or on the local page, without compiling. Covers most users.
2. **Packages**: firmware with code of its own for what `generic` cannot do, such as a Bluetooth protocol, IR or a serial link of its own. Every package is built and signed in CI and offered as ready-made firmware. The user still sets everything up without building.
3. **Developer mode**: building locally in HA, for code of your own that is not a package yet.

Line between 1 and 2: `generic` only grows with widely useful building blocks (and needs room; now ±64% of the app slot). Protocols for one kind of device become a package.

### 16.2 Package sources
- **Official**: this repository, signed with the project's key. Contents: `generic`, and maybe a few example packages.
- **Community**: whoever wants something specific makes a repository of their own from a **template repository** (build workflow, package layout, `release_manifest.py`), makes a key of their own and tags a release. GitHub builds and signs; the project reviews, builds and signs nothing.
- In the add-on you add a source like a custom repository in HACS. The add-on remembers the source's public key when it is added and from then on only accepts releases with that key. The UI always shows **official** or **community** and who the maker is.
- Shape of a package: `packages/<name>/package.yaml` with name, description, maker, chip(s), the version of the package format it builds on, and a **schema** of its settings. The add-on makes the form and the YAML check from that schema; there is no UI per package. Code in `components/<name>`, on top of `mb_core`. (Now: `firmware/<app>/package.yaml` with name, app, kind, format, title and description.)
- **Upkeep for the project**: `mb_core` and the package format. The format gets a version number and stays stable, so community packages keep working.

Device YAML (example):
```yaml
device:
  name: Shed
package: generic          # or <source>/<package>
connections:              # for generic
  - type: relay
    pin: 4
    name: Lamp
settings: {}              # following the package's schema
```

### 16.3 Catalog (marketplace)
- Sharing comes in two kinds:
  - **Recipes**: only a YAML for `generic`. No code, so no risk and no building. Expected to be most of it.
  - **Packages**: firmware from a source (§16.2).
- A source or recipe joins with the GitHub topic `matter-builder-package`.
- A nightly GitHub Action in this repository finds those repositories, checks their signed manifests and writes one `catalog.json` to GitHub Pages. The add-on only fetches that file, so GitHub's API limit does not matter.
- Per item: name, description, maker, GitHub stars, downloads (from GitHub), official/community, and a **verified** badge for what the project looked at itself (optional).
- Moderation: a **block list** in this repository; the item disappears at the next run. Support and reports go through the maker's issues.
- Cost: none. GitHub's limits (public repository, free account): unlimited Actions minutes, 6 hours per job, 20 jobs at once, 10 GB cache, 2 GB per file and 1000 files per release, no limit on downloads.
- Protection stays: a signature per source, adoption with password and BOOT button (§7.4), trial and rollback for `generic`.

### 16.4 Pages of the add-on
1. **Home**: your devices (state, version, update, "found, not linked yet") and a card **New device** with *Create* and *From the catalog*. Opening a device gives its **detail page** (connections in UI/YAML, update, token).
2. **Create**: start from empty, an example or a catalog item; set up in UI or YAML; **Check**; **Install** (USB on a new ESP32, or apply to an existing device). In developer mode also **Build**.
3. **Catalog**: search and filter (recipes, packages, official, community), details per item. **Use** opens Create with the item filled in: there is one way to install.
4. **Settings** (gear): tokens behind an eye, Wi-Fi for all devices, package sources, developer mode and toolchain.

### 16.5 Installing without building
- Building is only possible in developer mode; **anyone can flash**.
- A release has an update image and a **factory image** (bootloader, partition table and app in one) per firmware, so a blank ESP32 can be installed straight from the release.
- For `generic`, "install" means: flash the standard firmware, or keep it when it is already there. Then the add-on sets the name and connections from the YAML, with the existing trial.
- Flashing over USB, in this order of preference:
  1. **In the browser** from the add-on (esptool-js, shipped with it). Needs HTTPS and Chrome or Edge.
  2. **On the HA host**: plug the ESP32 into the HA machine and the add-on flashes it itself with esptool. Also works when HA is opened over plain http. The add-on needs access to the serial ports for it.
  3. **Without HA**: a flash page of the project on GitHub Pages, with the same esptool-js; the page checks the signature and hash in the browser (WebCrypto, P-256). Fits "works without HA" and is not an external web flasher: the page comes from this repository. The firmware then has to be on Pages too, since release files can probably not be fetched from a web page (CORS); to check.
- No dependency on web.esphome.io or Espressif's flasher. Anyone can always flash the factory image with esptool themselves.

### 16.6 Requirements, Bluetooth pairing and components (2026-11-01)
- **Requirements.** A board says what it has (`BOARD_CAPS` in `tools/mb_generate.py`: network and Bluetooth); a config needs Bluetooth for Bluetooth connections, a package says `requires` and `network` in its `package.yaml`. The add-on shows them as labels and refuses what a board or firmware cannot do.
- **Pairing over Bluetooth** (CHIPoBLE) is built into the generic firmware and switched on the device (`pairing ble on|off`, off by default). Off, Matter lets go of Bluetooth after starting and the scanner starts its own stack when a connection listens; on, Matter keeps it and the scanner joins. `USE_BLE_ONLY_FOR_COMMISSIONING` is off, since Matter would otherwise release the Bluetooth memory for good after pairing. Costs ±100 KB flash always; RAM only while on.
- **Components** add connection types to the generic firmware: `component.yaml` plus an ESP-IDF component using `components/mb_driver` (`io::register_driver`, `report_*`). Configs list them under `components:` (`github:owner/repo/path@ref`, pinned, cached; or `local:path`); the generator copies them into the project and writes `main/components_registry.cpp`. A type's id comes from its name, so stored connections keep their type. Building is developer mode or a package source's CI; ordinary users get them as packages.

### 16.7 Order of work
1. Factory images and package descriptions in the release and the manifest. (Done 2026-11-01.)
2. Installing without building for `generic` (§16.5), with the pages of §16.4. (Done 2026-11-01, add-on 0.8.)
3. Flashing on the HA host (§16.5, way 2). (Done 2026-11-01, add-on 0.9.)
4. The package format with schema, and the first package in a source of its own. (Format 1 without a settings schema; first source still to do.)
5. Package sources in the add-on, plus the template repository. (Add-on 0.9; template in `template/`.)
6. Catalog: the Action, `catalog.json` on Pages, the block list and the Catalog page. (`tools/build_catalog.py`, `.github/workflows/catalog.yml`, `catalog/`.)
7. Flash page on GitHub Pages (§16.5, way 3).
8. Documentation: "making a package" and "sharing a recipe".

## Rule: into Matter only on the Matter thread

Matter's own work runs on its thread, with a stack sized for it. Code on
another task (the HTTP API, the console, a timer, the Bluetooth host) that
reads or changes Matter state goes through `matter_thread::run()`
(components/mb_core): the work runs on the Matter thread and the caller
waits for it. Taking the stack lock from the other task instead
(`LockChipStack`, `ScopedChipStackLock`) runs Matter's code on that task's
small stack: opening the pairing window that way crashed the bed bridge, and
so did removing a fabric. There is no exception for work that looks small.

Events towards Matter that nobody waits for (an input changed, a sensor
value) keep using `PlatformMgr().ScheduleWork()`.
