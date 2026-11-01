"""Matter Builder generator: a device spec (YAML) to an ESP-IDF project.

    python3 tools/mb_generate.py <spec.yaml> <output folder>   generate
    python3 tools/mb_generate.py --check <spec.yaml>            only validate

A spec says what is built, never personal settings (Wi-Fi, Thread network,
passwords, bed MAC/PIN: those are set on the device's own page):

    device:
      name: Shed                # name in controllers, and <slug>.local
      app: generic              # the application built (now only generic)
      board: esp32-c6           # board profile (only esp32-c6 for now)
      version: 0.1.0
    connections:                # generic only: factory setting, editable on the page
      - { slot: 0, type: relay, pin: 4, name: Pump }
      - { slot: 1, type: sht4x, pin: 6, scl: 7, name: Climate }

The project is the application's own folder (firmware/<app>) plus a
generated main/spec_defaults.h and main/spec.yaml (built into the firmware,
so adopting the device recovers its spec). The same spec and sources always
give the same project.
"""

import json
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APPS = {"generic": "firmware/generic"}
BOARDS = {"esp32-c6"}
TYPES = {"relay", "led", "button", "contact", "motion", "sht4x"}
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
    unknown = set(spec) - {"device", "connections"}
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

    connections = spec.get("connections") or []
    if connections and app != "generic":
        raise SpecError("connections: only for app generic")
    if not isinstance(connections, list):
        raise SpecError("connections: a list")
    slots, pins, clean = set(), set(), []
    for c in connections:
        if not isinstance(c, dict):
            raise SpecError("connections: each one is a mapping")
        unknown = set(c) - {"slot", "type", "pin", "scl", "name", "inverted"}
        if unknown:
            raise SpecError(f"connections: unknown “{sorted(unknown)[0]}”")
        slot, kind = c.get("slot"), c.get("type")
        if not isinstance(slot, int) or not 0 <= slot <= 7 or slot in slots:
            raise SpecError(f"connections: slot 0-7, each once (got {slot})")
        if kind not in TYPES:
            raise SpecError(f"connection {slot}: type one of {', '.join(sorted(TYPES))}")
        used = [c.get("pin")] + ([c.get("scl")] if kind == "sht4x" else [])
        for pin in used:
            if pin not in SAFE_PINS or pin in pins:
                raise SpecError(f"connection {slot}: pin {pin} is not usable or already in use")
            pins.add(pin)
        if kind == "sht4x" and any(x.get("type") == "sht4x" for x in clean):
            raise SpecError("only one SHT4x")
        label = str(c.get("name") or kind).strip()[:23]
        if '"' in label or "\\" in label:
            raise SpecError(f"connection {slot}: no quotes in the name")
        slots.add(slot)
        clean.append({"slot": slot, "type": kind, "pin": c.get("pin"), "scl": c.get("scl", 0) or 0,
                      "inverted": bool(c.get("inverted", False)), "name": label})
    if '"' in name or "\\" in name:
        raise SpecError("device.name: no quotes")
    return {"name": name, "slug": slug_of(name), "app": app, "board": board, "version": version,
            "version_number": number, "connections": sorted(clean, key=lambda x: x["slot"])}


# ---------------------------------------------------------------- generation

def spec_header(spec):
    rows = []
    for c in spec["connections"]:
        rows.append(f'    {{{c["slot"]}, "{c["type"]}", {c["pin"]}, {c["scl"]}, {"true" if c["inverted"] else "false"}, '
                    f'"{c["name"]}"}},')
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
        "};",
        "",
        f"constexpr int SPEC_CONNECTION_COUNT = {len(spec['connections'])};",
        f"constexpr SpecConnection SPEC_CONNECTIONS[{max(len(spec['connections']), 1)}] = {{",
        *(rows or ['    {-1, "", 0, 0, false, ""},']),
        "};",
        "",
    ])


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
