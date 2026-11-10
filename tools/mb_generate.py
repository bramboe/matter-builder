"""Matter Builder generator: a device spec (YAML) to an ESP-IDF project.

    python3 tools/mb_generate.py <spec.yaml> <output folder>   generate
    python3 tools/mb_generate.py --check <spec.yaml>            only validate

A spec says what is built, never personal settings (Wi-Fi, Thread network,
passwords, bed MAC/PIN: those are set on the device's own page):

    device:
      name: Shed                # name in controllers, and <slug>.local
      app: generic              # the application built: generic, or octo_bridge
                                # where firmware/bridge is present (not in the add-on)
      board: esp32-c6           # board profile (only esp32-c6 for now)
      version: 0.1.0
    connections:                # generic only: factory setting, editable on the page
      - { slot: 0, type: relay, pin: 4, name: Pump }
      - { slot: 1, type: sht4x, pin: 6, scl: 7, name: Climate }
      - { slot: 2, type: pulse_relay, pin: 5, number: 2, name: Gate }
    components:                 # connection types of their own, built in (developer mode)
      - source: github:owner/repo/path/to/component@v1.0.0
      - source: local:examples/components/pulse_relay

A component is a folder with a component.yaml (type, title, matter, pins,
number, requires) and an ESP-IDF component that calls io::register_driver()
(components/mb_driver). GitHub sources are pinned to a tag or commit and kept
in a cache (MB_COMPONENT_CACHE).

The project is the application's own folder (firmware/<app>) plus a
generated main/spec_defaults.h and main/spec.yaml (built into the firmware,
so adopting the device recovers its spec). The same spec and sources always
give the same project.
"""

import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# The bed bridge is only for a checkout or add-on that has its sources
# (tools/addon_sync.sh --with-octo); the official add-on ships generic only
APPS = {app: folder for app, folder in {"generic": "firmware/generic", "octo_bridge": "firmware/bridge"}.items()
        if (ROOT / folder).is_dir()}
# What an application needs beyond its connections: the beds take their
# commands over Bluetooth
APP_REQUIRES = {"octo_bridge": ["bluetooth"]}
BOARDS = {"esp32-c6", "esp32"}
# What a board offers; a config or package may only ask for what its board has.
# Only the C6 is built for now; the others are here for the add-on's checks.
BOARD_CAPS = {
    "esp32-c6": {"network": ["thread", "wifi"], "bluetooth": True},
    "esp32-h2": {"network": ["thread"], "bluetooth": True},
    "esp32-s3": {"network": ["wifi"], "bluetooth": True},
    "esp32": {"network": ["wifi"], "bluetooth": True},
    "esp32-s2": {"network": ["wifi"], "bluetooth": False},
}


def requirements(connections):
    """What a generic device's connections need from its board: Bluetooth for
    the Bluetooth connections, nothing else beyond the network."""
    return ["bluetooth"] if any(c["type"] in BLE_TYPES or c["type"] == "ble_command" for c in connections) else []
TYPES = {"relay", "led", "button", "contact", "motion", "sht4x", "ir_send", "ir_receive", "ble_sensor", "ble_presence"}
IR_TYPES = {"ir_send", "ir_receive"}
BLE_TYPES = {"ble_sensor", "ble_presence"}
# Types the generic firmware registers like a component (main/ble_device.cpp):
# a command to a Bluetooth device, its number is the command (1-8)
BUILTIN_DRIVERS = {"ble_command": {"type": "ble_command", "title": "Bluetooth command", "matter": "on_off",
                                   "pins": 0, "number": "Command (1-8)"}}
# Options that work on the device itself (io.cpp): what an output does at
# power-on and after how many seconds it switches off; the output an input
# switches (a button toggles it, a contact, motion or presence sets it)
POWER_MODES = ["off", "on", "last", "toggle"]
OUTPUT_TYPES = {"relay", "led"}
CONTROL_TYPES = {"button", "contact", "motion", "ble_presence"}
IR_CODE = re.compile(r"^nec:[0-9A-Fa-f]{1,4}:[0-9A-Fa-f]{1,4}$")
MAC = re.compile(r"^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$")
SAFE_PINS = {0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 18, 19, 20, 21, 22, 23}  # ESP32-C6, see firmware/generic/main/io.cpp
SAFE_PINS_ESP32 = {2, 4, 5, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33}  # classic ESP32


class SpecError(Exception):
    pass


# ---------------------------------------------------------------- YAML subset
# Enough YAML for a spec, without libraries: nested mappings by indentation,
# lists of mappings ("- key: value"), flow mappings and lists ({ a: 1 },
# [1, 2], nested), comments, quoted strings, numbers and booleans. Unlike
# full YAML, on/off/yes/no stay words (a rule's "on: 0" is a key).

def scalar(text):
    text = text.strip()
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "\"'":
        return text[1:-1]
    if text in ("true", "false"):
        return text == "true"
    if re.fullmatch(r"-?\d+", text):
        return int(text)
    if re.fullmatch(r"-?\d+\.\d+", text):
        return float(text)
    if re.fullmatch(r"0x[0-9a-fA-F]+", text):
        return int(text, 16)
    return text


def flow(text):
    """A flow value ({...}, [...], or a scalar) and nothing after it."""
    def parse(i):
        while i < len(text) and text[i] == " ":
            i += 1
        if i < len(text) and text[i] in "{[":
            closing, is_map = ("}", True) if text[i] == "{" else ("]", False)
            result = {} if is_map else []
            i += 1
            while True:
                while i < len(text) and text[i] in " ,":
                    i += 1
                if i >= len(text):
                    raise SpecError(f"cannot read “{text}”: missing {closing}")
                if text[i] == closing:
                    return result, i + 1
                if is_map:
                    m = re.match(r"\s*([A-Za-z0-9_\-]+)\s*:", text[i:])
                    if not m:
                        raise SpecError(f"cannot read “{text[i:].strip()}”")
                    i += m.end()
                    value, i = parse(i)
                    result[m.group(1)] = value
                else:
                    value, i = parse(i)
                    result.append(value)
        # a scalar: up to , } ] (outside quotes)
        start = i
        if i < len(text) and text[i] in "\"'":
            end = text.find(text[i], i + 1)
            if end < 0:
                raise SpecError(f"cannot read “{text}”: unclosed quote")
            return text[i + 1:end], end + 1
        while i < len(text) and text[i] not in ",}]":
            i += 1
        return scalar(text[start:i]), i

    value, end = parse(0)
    if text[end:].strip():
        raise SpecError(f"cannot read “{text}”")
    return value


def flow_mapping(text):
    return flow(text)


def value_of(text):
    text = text.strip()
    return flow(text) if text[:1] in "{[" else scalar(text)


def parse_yaml(text):
    lines = []
    for number, raw in enumerate(text.splitlines(), 1):
        line = re.sub(r"\s+#.*$|^\s*#.*$", "", raw).rstrip()
        if line.strip():
            lines.append((number, len(line) - len(line.lstrip()), line.strip()))

    def block(i, indent):
        if i < len(lines) and lines[i][2].startswith("- "):
            items = []
            while i < len(lines) and lines[i][1] == indent and lines[i][2].startswith("- "):
                number, _, content = lines[i]
                rest = content[2:].strip()
                if rest.startswith("{"):
                    items.append(value_of(rest))
                    i += 1
                    continue
                if ":" not in rest:
                    items.append(scalar(rest))
                    i += 1
                    continue
                # "- key: value" then more keys indented under it
                fake = (number, indent + 2, rest)
                lines[i] = fake
                item, i = mapping(i, indent + 2)
                items.append(item)
            return items, i
        return mapping(i, indent)

    def mapping(i, indent):
        result = {}
        while i < len(lines) and lines[i][1] == indent and not lines[i][2].startswith("- "):
            number, _, content = lines[i]
            if ":" not in content:
                raise SpecError(f"line {number}: expected “key: value”")
            key, rest = content.split(":", 1)
            key, rest = key.strip(), rest.strip()
            i += 1
            if rest:
                result[key] = value_of(rest)
            elif i < len(lines) and lines[i][1] > indent:
                result[key], i = block(i, lines[i][1])
            elif i < len(lines) and lines[i][1] == indent and lines[i][2].startswith("- "):
                result[key], i = block(i, indent)
            else:
                result[key] = None
        if i < len(lines) and lines[i][1] > indent:
            raise SpecError(f"line {lines[i][0]}: unexpected indentation")
        return result, i

    if not lines:
        raise SpecError("the spec is empty")
    data, i = block(0, lines[0][1])
    if i < len(lines):
        raise SpecError(f"line {lines[i][0]}: unexpected text")
    return data


# ---------------------------------------------------------------- validation

def slug_of(name):
    slug = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")
    return slug[:24] or "device"


def version_number(version):
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", str(version))
    if not match:
        raise SpecError("device.version must look like 1.2.3")
    major, minor, patch = (int(x) for x in match.groups())
    if minor > 99 or patch > 99:
        raise SpecError("device.version: minor and patch at most 99")
    return major * 10000 + minor * 100 + patch


# ---------------------------------------------------------------- rules

# The rules on the device (firmware/generic/main/rules.h): YAML for people,
# "rule set" options for the firmware, and one JSON shape for the add-on
RULES_MAX = 12
RULE_STEPS = 6
DAYS = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"]
SLOT_TRIGGERS = ["press", "long", "active", "inactive", "on", "off"]
VALUE_TRIGGERS = {"above": 100, "below": 100, "humid_above": 100, "humid_below": 100}  # °C or % to centi
SLOT_CONDS = ["active", "inactive", "on", "off"]
SLOT_STEPS = ["on", "off", "toggle"]
RULE_STEPS_KINDS = ["enable", "disable", "toggle_rule"]


def clock_minute(text, where):
    m = re.fullmatch(r"(\d{1,2}):(\d{2})", str(text).strip())
    if not m or int(m.group(1)) > 23 or int(m.group(2)) > 59:
        raise SpecError(f"{where}: a time like 22:30")
    return int(m.group(1)) * 60 + int(m.group(2))


def one_key(mapping, kinds, where):
    keys = [k for k in mapping if k in kinds]
    if len(keys) != 1:
        raise SpecError(f"{where}: one of {', '.join(kinds)}")
    return keys[0]


def check_rules(rules, slots):
    """The rules of a spec as the add-on's JSON shape, with the firmware's options."""
    if rules is None:
        return []
    if not isinstance(rules, list) or len(rules) > RULES_MAX:
        raise SpecError(f"rules: a list of at most {RULES_MAX}")
    out = []
    # A rule's id is its number on the device (and its switch in Matter):
    # its place in the list unless "id:" says otherwise, so ids stay put
    ids = [r.get("id", n) if isinstance(r, dict) else n for n, r in enumerate(rules)]
    if any(not isinstance(i, int) or not 0 <= i < RULES_MAX for i in ids) or len(set(ids)) != len(ids):
        raise SpecError(f"rules: each id 0-{RULES_MAX - 1}, once")
    for position, r in enumerate(rules):
        index = ids[position]
        where = f"rule {index}"
        if not isinstance(r, dict):
            raise SpecError(f"{where}: a mapping")
        unknown = set(r) - {"id", "name", "when", "if", "do", "enabled", "matter"}
        if unknown:
            raise SpecError(f"{where}: unknown “{sorted(unknown)[0]}”")
        name = str(r.get("name") or f"Rule {index + 1}").strip()
        if not 1 <= len(name) <= 23 or '"' in name or "\\" in name:
            raise SpecError(f"{where}: name 1-23 characters, no quotes")

        def slot(value, label):
            if not isinstance(value, int) or value not in slots:
                raise SpecError(f"{where}: {label} must be a connection of this config")
            return value

        when = r.get("when")
        if not isinstance(when, dict):
            raise SpecError(f"{where}: when is a mapping, e.g. {{active: 1}}")
        kind = one_key(when, SLOT_TRIGGERS + list(VALUE_TRIGGERS) + ["time", "boot"], f"{where}: when")
        trig = {"kind": kind, "slot": 0, "value": 0, "days": 0}
        if kind in SLOT_TRIGGERS:
            trig["slot"] = slot(when[kind], "when")
            if kind == "press":
                count = when.get("count", 1)
                if not isinstance(count, int) or not 1 <= count <= 5:
                    raise SpecError(f"{where}: count 1-5")
                trig["value"] = count
        elif kind in VALUE_TRIGGERS:
            trig["slot"] = slot(when[kind], "when")
            value = when.get("value")
            if not isinstance(value, (int, float)) or not -40 <= value <= 125:
                raise SpecError(f"{where}: value in °C or %")
            trig["value"] = round(value * 100)
        elif kind == "time":
            trig["value"] = clock_minute(when["time"], f"{where}: when")
            days = when.get("days") or []
            if isinstance(days, str):
                days = [d.strip() for d in days.split(",")]
            if any(str(d).lower()[:3] not in DAYS for d in days):
                raise SpecError(f"{where}: days like [mon, tue]")
            trig["days"] = sum(1 << DAYS.index(str(d).lower()[:3]) for d in days)
        cond = None
        if r.get("if") is not None:
            c = r["if"]
            if not isinstance(c, dict):
                raise SpecError(f"{where}: if is a mapping, e.g. {{on: 0}}")
            ck = one_key(c, SLOT_CONDS + list(VALUE_TRIGGERS) + ["time"], f"{where}: if")
            cond = {"kind": ck, "slot": 0, "value": 0, "value2": 0}
            if ck == "time":
                parts = str(c["time"]).split("-")
                if len(parts) != 2:
                    raise SpecError(f"{where}: if time like 22:00-07:00")
                cond["value"], cond["value2"] = (clock_minute(x, f"{where}: if") for x in parts)
            else:
                cond["slot"] = slot(c[ck], "if")
                if ck in VALUE_TRIGGERS:
                    value = c.get("value")
                    if not isinstance(value, (int, float)) or not -40 <= value <= 125:
                        raise SpecError(f"{where}: if value in °C or %")
                    cond["value"] = round(value * 100)
        steps = r.get("do")
        if not isinstance(steps, list) or not 1 <= len(steps) <= RULE_STEPS:
            raise SpecError(f"{where}: do is a list of 1 to {RULE_STEPS} steps")
        do = []
        for st in steps:
            if not isinstance(st, dict) or len(st) != 1:
                raise SpecError(f"{where}: each step is one of on, off, toggle, level, wait, enable, disable, toggle_rule")
            (sk, sv), = st.items()
            if sk in SLOT_STEPS:
                do.append({"kind": sk, "target": slot(sv, sk), "value": 0})
            elif sk == "level":
                if not (isinstance(sv, list) and len(sv) == 2 and isinstance(sv[1], int) and 1 <= sv[1] <= 254):
                    raise SpecError(f"{where}: level is [connection, 1-254]")
                do.append({"kind": "level", "target": slot(sv[0], "level"), "value": sv[1]})
            elif sk == "wait":
                if not isinstance(sv, int) or not 1 <= sv <= 65535:
                    raise SpecError(f"{where}: wait in seconds, 1-65535")
                do.append({"kind": "wait", "target": 0, "value": sv})
            elif sk in RULE_STEPS_KINDS:
                if not isinstance(sv, int) or sv not in ids:
                    raise SpecError(f"{where}: {sk} names a rule of this config (an id)")
                do.append({"kind": sk, "target": sv, "value": 0})
            else:
                raise SpecError(f"{where}: unknown step “{sk}”")
        out.append({"index": index, "name": name, "enabled": bool(r.get("enabled", True)),
                    "matter": bool(r.get("matter", False)), "when": trig, "if": cond, "do": do})
    return out


def rule_options(r):
    """The firmware's "rule set" options for one rule (the add-on writes the same)."""
    w = r["when"]
    when = {"time": f"time:{w['value']}:{w['days']}", "boot": "boot"}.get(w["kind"])
    if when is None:
        when = f"{w['kind']}:{w['slot']}" + (f":{w['value']}" if w["kind"] in VALUE_TRIGGERS or w["kind"] == "press" else "")
    opts = [f"enabled={int(r['enabled'])}", f"matter={int(r['matter'])}", f"when={when}"]
    c = r.get("if")
    if c:
        opts.append("if=" + (f"time:{c['value']}:{c['value2']}" if c["kind"] == "time" else
                             f"{c['kind']}:{c['slot']}" + (f":{c['value']}" if c["kind"] in VALUE_TRIGGERS else "")))
    for st in r["do"]:
        opts.append("do=" + (f"wait:{st['value']}" if st["kind"] == "wait" else
                             f"level:{st['target']}:{st['value']}" if st["kind"] == "level" else f"{st['kind']}:{st['target']}"))
    opts.append("name-hex=" + r["name"].encode().hex())
    return " ".join(opts)


BLE_UUID = re.compile(r"^([0-9A-Fa-f]{4}|[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12})$")
BLE_COMMANDS = 8


def check_bluetooth_device(device):
    """A Bluetooth LE device the ESP32 sends commands to (firmware: ble_device):
    where to write (service, characteristic) and up to 8 commands. The same
    for everyone with that device, so it fits a recipe; the device's address
    may be given, its first message after connecting (a PIN) never is: that
    is set on the device's own page."""
    if device is None:
        return None
    if not isinstance(device, dict):
        raise SpecError("bluetooth_device: a mapping")
    unknown = set(device) - {"address", "address_type", "service", "characteristic", "write", "idle", "commands"}
    if unknown:
        raise SpecError(f"bluetooth_device: unknown “{sorted(unknown)[0]}”")
    out = {}
    for key in ("service", "characteristic"):
        value = str(device.get(key) or "")
        if not BLE_UUID.match(value):
            raise SpecError(f"bluetooth_device: {key} a UUID like FFE0 or 0000ffe0-0000-1000-8000-00805f9b34fb")
        out[key] = value.upper()
    write = str(device.get("write", "response"))
    if write not in ("response", "no-response"):
        raise SpecError("bluetooth_device: write response or no-response")
    out["write"] = write
    idle = device.get("idle", 30)
    if not isinstance(idle, int) or isinstance(idle, bool) or not 0 <= idle <= 3600:
        raise SpecError("bluetooth_device: idle in seconds, 0-3600")
    out["idle"] = idle
    if device.get("address"):
        address = str(device["address"])
        if not MAC.match(address):
            raise SpecError("bluetooth_device: address like AA:BB:CC:DD:EE:FF")
        kind = str(device.get("address_type", "public"))
        if kind not in ("public", "random"):
            raise SpecError("bluetooth_device: address_type public or random")
        out["address"], out["address_type"] = address.upper(), kind
    commands = device.get("commands") or []
    if not isinstance(commands, list) or len(commands) > BLE_COMMANDS:
        raise SpecError(f"bluetooth_device: commands a list of at most {BLE_COMMANDS}")
    out["commands"] = []
    for n, c in enumerate(commands, 1):
        if not isinstance(c, dict):
            raise SpecError(f"bluetooth_device command {n}: a mapping")
        unknown = set(c) - {"label", "bytes", "times", "gap"}
        if unknown:
            raise SpecError(f"bluetooth_device command {n}: unknown “{sorted(unknown)[0]}”")
        label = str(c.get("label") or f"Command {n}").strip()
        if not 1 <= len(label) <= 32 or '"' in label or "\\" in label:
            raise SpecError(f"bluetooth_device command {n}: label 1-32 characters, no quotes")
        data = re.sub(r"[\s:-]", "", str(c.get("bytes") if c.get("bytes") is not None else "")).upper()
        if not re.match(r"^([0-9A-F]{2}){1,64}$", data):
            raise SpecError(f"bluetooth_device command {n}: bytes as hex in quotes, 1-64 bytes (e.g. \"4002730000\")")
        times, gap = c.get("times", 1), c.get("gap", 0)
        if not isinstance(times, int) or isinstance(times, bool) or not 1 <= times <= 50:
            raise SpecError(f"bluetooth_device command {n}: times 1-50")
        if not isinstance(gap, int) or isinstance(gap, bool) or not 0 <= gap <= 2000:
            raise SpecError(f"bluetooth_device command {n}: gap in milliseconds, 0-2000")
        out["commands"].append({"label": label, "bytes": data, "times": times, "gap": gap})
    return out


def check(spec):
    """The validated spec, with defaults filled in. Raises SpecError."""
    if not isinstance(spec, dict) or not isinstance(spec.get("device"), dict):
        raise SpecError("the spec needs a “device:” section")
    unknown = set(spec) - {"device", "connections", "components", "rules", "bluetooth_device"}
    if unknown:
        raise SpecError(f"unknown section “{sorted(unknown)[0]}”")
    device = spec["device"]
    unknown = set(device) - {"name", "app", "board", "version"}
    if unknown:
        raise SpecError(f"device: unknown “{sorted(unknown)[0]}”")
    name = str(device.get("name") or "").strip()
    if not name or len(name) > 32:
        raise SpecError("device.name: 1 to 32 characters")
    app = device.get("app", "generic")
    if app not in APPS:
        raise SpecError(f"device.app: one of {', '.join(APPS)}")
    board = device.get("board", "esp32-c6")
    if board not in BOARDS:
        raise SpecError(f"device.board: one of {', '.join(sorted(BOARDS))}")
    version = str(device.get("version", "0.1.0"))
    number = version_number(version)

    components = load_components(spec.get("components") or [])
    if components and app != "generic":
        raise SpecError("components: only for app generic")
    drivers = {**BUILTIN_DRIVERS, **{c["type"]: c for c in components}}

    connections = spec.get("connections") or []
    if connections and app != "generic":
        raise SpecError("connections: only for app generic")
    if not isinstance(connections, list):
        raise SpecError("connections: a list")
    slots, pins, clean = set(), set(), []
    for c in connections:
        if not isinstance(c, dict):
            raise SpecError("connections: each one is a mapping")
        unknown = set(c) - {"slot", "type", "pin", "scl", "pin2", "name", "inverted", "code", "mac", "number",
                            "power", "off_after", "controls", "group"}
        if unknown:
            raise SpecError(f"connections: unknown “{sorted(unknown)[0]}”")
        slot, kind = c.get("slot"), c.get("type")
        if not isinstance(slot, int) or not 0 <= slot <= 7 or slot in slots:
            raise SpecError(f"connections: slot 0-7, each once (got {slot})")
        driver = drivers.get(kind)
        if kind not in TYPES and driver is None:
            raise SpecError(f"connection {slot}: type one of {', '.join(sorted(TYPES | set(drivers)))}")
        number = 0
        if driver is not None:
            if driver.get("number"):
                number = c.get("number", 0)
                if not isinstance(number, int) or not 0 <= number <= 65535:
                    raise SpecError(f"connection {slot}: number 0-65535 ({driver['number']})")
            elif "number" in c:
                raise SpecError(f"connection {slot}: {kind} takes no number")
        code = mac = ""
        if kind in BLE_TYPES:
            mac = str(c.get("mac") or "")
            if not MAC.match(mac):
                raise SpecError(f"connection {slot}: mac like AA:BB:CC:DD:EE:FF")
            mac = mac.upper()
            used = []
        elif driver is not None:
            used = [c.get("pin"), c.get("pin2")][:driver["pins"]]
        else:
            used = [c.get("pin")] + ([c.get("scl")] if kind == "sht4x" else [])
        if kind in IR_TYPES:
            code = str(c.get("code") or "")
            if not IR_CODE.match(code):
                raise SpecError(f"connection {slot}: code like nec:00FF:A25D")
            _, address, command = code.split(":")
            code = f"nec:{int(address, 16):04X}:{int(command, 16):04X}"
        for pin in used:
            # IR codes of the same kind share their LED or receiver
            shared = kind in IR_TYPES and any(x["type"] == kind and x["pin"] == pin for x in clean)
            if pin not in (SAFE_PINS_ESP32 if board == "esp32" else SAFE_PINS) or (pin in pins and not shared):
                raise SpecError(f"connection {slot}: pin {pin} is not usable or already in use")
            pins.add(pin)
        if kind in IR_TYPES and len({x["pin"] for x in clean if x["type"] == kind} | {c.get("pin")}) > 2:
            raise SpecError(f"connection {slot}: at most 2 pins for {kind}")
        if kind == "sht4x" and any(x.get("type") == "sht4x" for x in clean):
            raise SpecError("only one SHT4x")
        is_output = kind in OUTPUT_TYPES or (driver is not None and driver["matter"] in ("on_off", "dimmable"))
        can_control = kind in CONTROL_TYPES or (driver is not None and driver["matter"] in ("button", "contact", "occupancy"))
        options = {}
        if "power" in c or "off_after" in c:
            if not is_output:
                raise SpecError(f"connection {slot}: power and off_after are for outputs")
            power = c.get("power", "off")
            power = {True: "on", False: "off"}.get(power, power) if isinstance(power, bool) else str(power).lower()  # YAML reads on/off as true/false
            if power not in POWER_MODES:
                raise SpecError(f"connection {slot}: power one of {', '.join(POWER_MODES)}")
            off_after = c.get("off_after", 0)
            if not isinstance(off_after, int) or not 0 <= off_after <= 65535:
                raise SpecError(f"connection {slot}: off_after in seconds, 0-65535")
            if power != "off":
                options["power"] = power
            if off_after:
                options["off_after"] = off_after
        if "controls" in c and c["controls"] is not None:
            target = c["controls"]
            if not can_control:
                raise SpecError(f"connection {slot}: controls is for buttons, contacts, motion and presence")
            if not isinstance(target, int) or not 0 <= target <= 7 or target == slot:
                raise SpecError(f"connection {slot}: controls another connection 0-7")
            options["controls"] = target
        label = str(c.get("name") or kind).strip()[:23]
        if '"' in label or "\\" in label:
            raise SpecError(f"connection {slot}: no quotes in the name")
        # Connections with the same group are one device in Matter (Apple Home: one accessory)
        group = str(c.get("group") or "").strip()
        if len(group) > 23 or '"' in group or "\\" in group:
            raise SpecError(f"connection {slot}: group a name of at most 23 characters, without quotes")
        if group:
            options["group"] = group
        slots.add(slot)
        second = (c.get("pin2", 0) if driver is not None else c.get("scl", 0)) or 0
        clean.append({"slot": slot, "type": kind, "pin": c.get("pin", 0) if used else 0,
                      "scl": second, "inverted": bool(c.get("inverted", False)), "name": label,
                      **({"code": code} if code else {}), **({"mac": mac} if mac else {}),
                      **({"number": number} if driver is not None and driver.get("number") else {}), **options})
    groups = []
    for c in sorted(clean, key=lambda x: x["slot"]):
        if c.get("group") and c["group"] not in groups:
            groups.append(c["group"])
    if len(groups) > 4:
        raise SpecError(f"at most 4 groups ({len(groups)}: {', '.join(groups)})")
    for c in clean:
        target = next((x for x in clean if x["slot"] == c.get("controls")), None) if "controls" in c else None
        if "controls" in c and (target is None or not (target["type"] in OUTPUT_TYPES | {"ir_send"} or
                                                    (target["type"] in drivers and drivers[target["type"]]["matter"] in ("on_off", "dimmable")))):
            raise SpecError(f"connection {c['slot']}: controls must name an output or IR code in this config")
    if '"' in name or "\\" in name:
        raise SpecError("device.name: no quotes")
    bluetooth_device = check_bluetooth_device(spec.get("bluetooth_device"))
    if bluetooth_device and app != "generic":
        raise SpecError("bluetooth_device: only for app generic")
    for c in clean:
        if c["type"] == "ble_command":
            count = len(bluetooth_device["commands"]) if bluetooth_device else BLE_COMMANDS
            if not 1 <= c.get("number", 0) <= count:
                raise SpecError(f"connection {c['slot']}: number is a command of the Bluetooth device (1-{count})")
    requires = sorted(set(requirements(clean)) | set(APP_REQUIRES.get(app, [])) |
                      {r for d in components for r in d["requires"]} |
                      ({"bluetooth"} if bluetooth_device or any(c["type"] == "ble_command" for c in clean) else set()))
    if "bluetooth" in requires and not BOARD_CAPS[board]["bluetooth"]:
        what = "the Bluetooth connections need" if app == "generic" else f"{app} needs"
        raise SpecError(f"{what} a board with Bluetooth, and {board} has none")
    rules = check_rules(spec.get("rules"), {c["slot"] for c in clean})
    if rules and app != "generic":
        raise SpecError("rules: only for app generic")
    return {"name": name, "slug": slug_of(name), "app": app, "board": board, "version": version, "rules": rules,
            **({"bluetooth_device": bluetooth_device} if bluetooth_device else {}),
            "version_number": number, "connections": sorted(clean, key=lambda x: x["slot"]),
            "requires": requires, "network": BOARD_CAPS[board]["network"][0],
            "components": [{"source": d["source"], "type": d["type"], "path": d["path"]} for d in components],
            "drivers": [{k: d[k] for k in ("type", "title", "description", "matter", "pins", "number")} for d in components]}


# ---------------------------------------------------------------- components

COMPONENT_TYPE = re.compile(r"^[a-z][a-z0-9_]{1,23}$")
MATTER_KINDS = {"on_off", "dimmable", "button", "contact", "occupancy", "climate"}
GITHUB_SOURCE = re.compile(r"^github:([A-Za-z0-9-]+)/([A-Za-z0-9._-]+)((?:/[A-Za-z0-9._-]+)*)@([A-Za-z0-9._/-]+)$")


def fetch(source):
    """The folder of a component source: local:<path in this repository>, or
    github:<owner>/<repo>[/<path>]@<tag or commit> (cloned once into the cache)."""
    if source.startswith("local:"):
        path = (ROOT / source[6:]).resolve()
        if ROOT not in path.parents or not path.is_dir():
            raise SpecError(f"components: {source} is not a folder of this repository")
        return path
    m = GITHUB_SOURCE.match(source)
    if not m:
        raise SpecError(f"components: source like github:owner/repo/path@v1.0 or local:path (got {source})")
    owner, repo, sub, ref = m.groups()
    cache = Path(os.environ.get("MB_COMPONENT_CACHE", Path.home() / ".cache" / "matter-builder" / "components"))
    target = cache / f"{owner}__{repo}__{ref.replace('/', '_')}"
    if not target.exists():
        url = f"https://github.com/{owner}/{repo}.git"
        tmp = target.with_suffix(".tmp")
        shutil.rmtree(tmp, ignore_errors=True)
        tmp.parent.mkdir(parents=True, exist_ok=True)
        ok = subprocess.run(["git", "clone", "--quiet", "--depth", "1", "--branch", ref, url, str(tmp)],
                            capture_output=True).returncode == 0
        if not ok:  # a commit, not a tag or branch
            shutil.rmtree(tmp, ignore_errors=True)
            ok = (subprocess.run(["git", "clone", "--quiet", url, str(tmp)], capture_output=True).returncode == 0 and
                  subprocess.run(["git", "-C", str(tmp), "checkout", "--quiet", ref], capture_output=True).returncode == 0)
        if not ok:
            shutil.rmtree(tmp, ignore_errors=True)
            raise SpecError(f"components: could not fetch {source}")
        tmp.rename(target)
    path = (target / sub.lstrip("/")).resolve()
    if target.resolve() not in [path, *path.parents] or not path.is_dir():
        raise SpecError(f"components: {source} has no folder {sub or '/'}")
    return path


def load_components(sources):
    if not isinstance(sources, list):
        raise SpecError("components: a list")
    out = []
    for entry in sources:
        source = entry.get("source") if isinstance(entry, dict) else entry
        if not isinstance(source, str):
            raise SpecError("components: each one has a source")
        path = fetch(source)
        yaml_file = path / "component.yaml"
        if not yaml_file.is_file() or not (path / "CMakeLists.txt").is_file():
            raise SpecError(f"components: {source} has no component.yaml and CMakeLists.txt")
        meta = parse_yaml(yaml_file.read_text())
        kind = str(meta.get("type", ""))
        if not COMPONENT_TYPE.match(kind) or kind in TYPES:
            raise SpecError(f"components: {source}: type a new lowercase name (got {kind})")
        if meta.get("matter") not in MATTER_KINDS:
            raise SpecError(f"components: {kind}: matter one of {', '.join(sorted(MATTER_KINDS))}")
        pins = meta.get("pins", 1)
        if pins not in (0, 1, 2):
            raise SpecError(f"components: {kind}: pins 0, 1 or 2")
        requires = meta.get("requires", [])
        if not isinstance(requires, list):
            requires = [r.strip() for r in str(requires).strip("[]").split(",") if r.strip()]
        if any(d["type"] == kind for d in out):
            raise SpecError(f"components: {kind} twice")
        out.append({"source": source, "type": kind, "path": str(path), "title": str(meta.get("title") or kind),
                    "description": str(meta.get("description") or ""), "matter": meta["matter"], "pins": pins,
                    "number": str(meta["number"]) if meta.get("number") else None, "requires": requires})
    return out


# ---------------------------------------------------------------- generation

def spec_header(spec):
    rows = []
    groups = []  # numbered in the order they first appear, as the add-on does
    for c in sorted(spec["connections"], key=lambda x: x["slot"]):
        if c.get("group") and c["group"] not in groups:
            groups.append(c["group"])
    for c in spec["connections"]:
        rows.append(f'    {{{c["slot"]}, "{c["type"]}", {c["pin"]}, {c["scl"]}, {"true" if c["inverted"] else "false"}, '
                    f'"{c["name"]}", "{c.get("code", "")}", "{c.get("mac", "")}", {c.get("number", 0)}, '
                    f'{POWER_MODES.index(c.get("power", "off"))}, {c.get("off_after", 0)}, {c.get("controls", -1)}, '
                    f'{groups.index(c["group"]) + 1 if c.get("group") else 0}}},')
    return "\n".join([
        "// Generated by tools/mb_generate.py from main/spec.yaml: do not edit.",
        "#pragma once",
        "",
        f'#define SPEC_NAME "{spec["name"]}"',
        f'#define SPEC_SLUG "{spec["slug"]}"',
        f'#define SPEC_VERSION "{spec["version"]}"',
        "",
        "struct SpecConnection {",
        "    int slot;",
        "    const char* type;",
        "    int pin;",
        "    int scl;",
        "    bool inverted;",
        "    const char* name;",
        "    const char* code;  // IR: nec:<address>:<command>",
        "    const char* mac;   // Bluetooth",
        "    int number;        // the setting of a component type",
        "    int power;         // at power-on: 0 off, 1 on, 2 as it was, 3 the other way round",
        "    int off_after;     // seconds until an output switches itself off (0: never)",
        "    int controls;      // the output this input switches (-1: none)",
        "    int group;         // 1-4: one device with the others of its group (0: none)",
        "};",
        "",
        "// The rules, as \"<id> <rule set options>\" for the firmware",
        f"constexpr int SPEC_RULE_COUNT = {len(spec.get('rules') or [])};",
        f"constexpr const char* SPEC_RULES[{max(len(spec.get('rules') or []), 1)}] = {{",
        *([f'    "{r["index"]} {rule_options(r)}",' for r in spec.get("rules") or []] or ['    "",']),
        "};",
        "",
        f"constexpr int SPEC_CONNECTION_COUNT = {len(spec['connections'])};",
        f"constexpr SpecConnection SPEC_CONNECTIONS[{max(len(spec['connections']), 1)}] = {{",
        *(rows or ['    {-1, "", 0, 0, false, "", "", "", 0, 0, 0, -1},']),
        "};",
        "",
    ])


def add_components(out, components):
    """The components into the project (ESP-IDF finds <project>/components),
    the registry that registers them, and main's dependency on them."""
    for c in components:
        shutil.copytree(c["path"], out / "components" / c["type"],
                        ignore=shutil.ignore_patterns("build*", ".git", "managed_components"))
    names = [c["type"] for c in components]
    (out / "main" / "components_registry.cpp").write_text("\n".join([
        "// Generated by tools/mb_generate.py: the components of this config.",
        "",
        '#include "io.h"',
        "",
        *[f"void mb_register_{n}();" for n in names],
        "",
        "namespace io {",
        "",
        "void register_components() {",
        *[f"    mb_register_{n}();" for n in names],
        "}",
        "",
        "}  // namespace io",
        "",
    ]))
    cmake = (out / "main" / "CMakeLists.txt").read_text()
    cmake = cmake.replace("REQUIRES mb_core mb_driver", "REQUIRES mb_core mb_driver " + " ".join(names), 1)
    (out / "main" / "CMakeLists.txt").write_text(cmake)


def generate(spec_path, out):
    text = Path(spec_path).read_text()
    spec = check(parse_yaml(text))
    app_dir = ROOT / APPS[spec["app"]]
    out = Path(out)
    if out.exists():
        shutil.rmtree(out)
    ignore = shutil.ignore_patterns("build*", "sdkconfig", "sdkconfig.old", "*.log", "secrets.h",
                                    "managed_components", "dependencies.lock")
    shutil.copytree(app_dir, out, ignore=ignore)

    # Project name and version
    cmake = (out / "CMakeLists.txt").read_text()
    cmake = re.sub(r'set\(PROJECT_VER "[^"]*"\)', f'set(PROJECT_VER "{spec["version"]}")', cmake)
    cmake = re.sub(r"set\(PROJECT_VER_NUMBER \d+\)", f'set(PROJECT_VER_NUMBER {spec["version_number"]})', cmake)
    cmake = re.sub(r"project\([^)]*\)", f'project({spec["slug"].replace("-", "_")})', cmake)
    cmake = cmake.replace('"${CMAKE_CURRENT_LIST_DIR}/../../components"', f'"{ROOT / "components"}"')
    (out / "CMakeLists.txt").write_text(cmake)
    defaults = (out / "sdkconfig.defaults").read_text()
    defaults = re.sub(r"CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER=\d+",
                      f'CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER={spec["version_number"]}', defaults)
    (out / "sdkconfig.defaults").write_text(defaults)

    secrets = out / "main" / "secrets.example.h"
    if secrets.exists():
        shutil.copy(secrets, out / "main" / "secrets.h")

    (out / "main" / "spec_defaults.h").write_text(spec_header(spec))
    if spec["components"]:
        add_components(out, spec["components"])
    (out / "main" / "spec.yaml").write_text(text)
    (out / "build-manifest.json").write_text(json.dumps(
        {"spec": spec, "app_source": APPS[spec["app"]]}, indent=2) + "\n")
    return spec


def main(argv):
    try:
        if len(argv) == 3 and argv[1] == "--check":
            spec = check(parse_yaml(Path(argv[2]).read_text()))
            print(json.dumps(spec, indent=2))
            return 0
        if len(argv) == 3:
            spec = generate(argv[1], argv[2])
            print(f"generated {spec['slug']} {spec['version']} ({spec['app']}) in {argv[2]}")
            return 0
    except SpecError as err:
        print(f"spec error: {err}", file=sys.stderr)
        return 1
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
