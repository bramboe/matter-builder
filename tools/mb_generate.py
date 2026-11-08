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
BOARDS = {"esp32-c6"}
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
    return ["bluetooth"] if any(c["type"] in BLE_TYPES for c in connections) else []
TYPES = {"relay", "led", "button", "contact", "motion", "sht4x", "ir_send", "ir_receive", "ble_sensor", "ble_presence"}
IR_TYPES = {"ir_send", "ir_receive"}
BLE_TYPES = {"ble_sensor", "ble_presence"}
# Options that work on the device itself (io.cpp): what an output does at
# power-on and after how many seconds it switches off; the output an input
# switches (a button toggles it, a contact, motion or presence sets it)
POWER_MODES = ["off", "on", "last", "toggle"]
OUTPUT_TYPES = {"relay", "led"}
CONTROL_TYPES = {"button", "contact", "motion", "ble_presence"}
IR_CODE = re.compile(r"^nec:[0-9A-Fa-f]{1,4}:[0-9A-Fa-f]{1,4}$")
MAC = re.compile(r"^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$")
SAFE_PINS = {0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 18, 19, 20, 21, 22, 23}  # see firmware/generic/main/io.cpp


class SpecError(Exception):
    pass


# ---------------------------------------------------------------- YAML subset
# Enough YAML for a spec, without libraries: nested mappings by indentation,
# lists of mappings ("- key: value"), flow mappings ({ a: 1, b: 2 }),
# comments, quoted strings, numbers and booleans.

def scalar(text):
    text = text.strip()
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "\"'":
        return text[1:-1]
    if text in ("true", "false"):
        return text == "true"
    if re.fullmatch(r"-?\d+", text):
        return int(text)
    if re.fullmatch(r"0x[0-9a-fA-F]+", text):
        return int(text, 16)
    return text


def flow_mapping(text):
    body = text.strip()[1:-1]
    result = {}
    for part in [p for p in body.split(",") if p.strip()]:
        if ":" not in part:
            raise SpecError(f"cannot read “{part.strip()}”")
        key, value = part.split(":", 1)
        result[key.strip()] = scalar(value)
    return result


def value_of(text):
    text = text.strip()
    return flow_mapping(text) if text.startswith("{") and text.endswith("}") else scalar(text)


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


def check(spec):
    """The validated spec, with defaults filled in. Raises SpecError."""
    if not isinstance(spec, dict) or not isinstance(spec.get("device"), dict):
        raise SpecError("the spec needs a “device:” section")
    unknown = set(spec) - {"device", "connections", "components"}
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
    drivers = {c["type"]: c for c in components}

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
                            "power", "off_after", "controls"}
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
            if pin not in SAFE_PINS or (pin in pins and not shared):
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
        slots.add(slot)
        second = (c.get("pin2", 0) if driver is not None else c.get("scl", 0)) or 0
        clean.append({"slot": slot, "type": kind, "pin": c.get("pin", 0) if used else 0,
                      "scl": second, "inverted": bool(c.get("inverted", False)), "name": label,
                      **({"code": code} if code else {}), **({"mac": mac} if mac else {}),
                      **({"number": number} if driver is not None and driver.get("number") else {}), **options})
    for c in clean:
        target = next((x for x in clean if x["slot"] == c.get("controls")), None) if "controls" in c else None
        if "controls" in c and (target is None or not (target["type"] in OUTPUT_TYPES | {"ir_send"} or
                                                    (target["type"] in drivers and drivers[target["type"]]["matter"] in ("on_off", "dimmable")))):
            raise SpecError(f"connection {c['slot']}: controls must name an output or IR code in this config")
    if '"' in name or "\\" in name:
        raise SpecError("device.name: no quotes")
    requires = sorted(set(requirements(clean)) | set(APP_REQUIRES.get(app, [])) |
                      {r for d in components for r in d["requires"]})
    if "bluetooth" in requires and not BOARD_CAPS[board]["bluetooth"]:
        what = "the Bluetooth connections need" if app == "generic" else f"{app} needs"
        raise SpecError(f"{what} a board with Bluetooth, and {board} has none")
    return {"name": name, "slug": slug_of(name), "app": app, "board": board, "version": version,
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
    for c in spec["connections"]:
        rows.append(f'    {{{c["slot"]}, "{c["type"]}", {c["pin"]}, {c["scl"]}, {"true" if c["inverted"] else "false"}, '
                    f'"{c["name"]}", "{c.get("code", "")}", "{c.get("mac", "")}", {c.get("number", 0)}, '
                    f'{POWER_MODES.index(c.get("power", "off"))}, {c.get("off_after", 0)}, {c.get("controls", -1)}}},')
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
