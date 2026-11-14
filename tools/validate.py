#!/usr/bin/env python3
"""Checks the YAML files people copy from and the catalog lists, before they
reach a release or the nightly catalog:

  recipes and example configs   examples/*.yaml, template/recipes/*.yaml
                                (with tools/mb_generate.py, as the add-on does)
  packages                      firmware/*/package.yaml, packages/*/package.yaml,
                                template/packages/*/package.yaml
  components                    examples/components/*/component.yaml
  catalog lists                 catalog/blocklist.txt, catalog/verified.txt

    python3 tools/validate.py          exit 1 with a line per problem
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from mb_generate import SpecError, check, parse_yaml  # noqa: E402
from release_manifest import CHIPS, PACKAGE_FIELDS, as_list  # noqa: E402

NETWORKS = {"thread", "wifi"}
REQUIRES = {"bluetooth", "thread", "wifi"}
KINDS = {"standard", "package"}
MATTER = {"on_off", "dimmable", "button", "contact", "occupancy", "climate", "color", "air_quality", "electrical"}
NAME = re.compile(r"^[a-z][a-z0-9_-]{0,39}$")
REPO = re.compile(r"^[A-Za-z0-9-]{1,39}/[A-Za-z0-9._-]{1,100}$")

problems = []


def problem(path, text):
    problems.append(f"{path.relative_to(ROOT)}: {text}")


def load(path):
    try:
        data = parse_yaml(path.read_text())
    except (OSError, ValueError) as err:
        problem(path, f"not readable YAML ({err})")
        return None
    if not isinstance(data, dict):
        problem(path, "not a mapping")
        return None
    return data


def check_recipe(path):
    data = load(path)
    if data is None:
        return
    try:
        check(data)
    except SpecError as err:
        problem(path, str(err))


def check_setting(path, key, field):
    if not isinstance(field, dict):
        return problem(path, f"settings.{key}: a mapping")
    kind = field.get("type")
    if kind not in ("int", "bool", "str"):
        return problem(path, f"settings.{key}: type int, bool or str")
    if not str(field.get("title") or "").strip():
        problem(path, f"settings.{key}: a title")
    default = field.get("default")
    if kind == "int":
        low, high = field.get("min"), field.get("max")
        if not isinstance(low, int) or not isinstance(high, int) or low > high:
            return problem(path, f"settings.{key}: whole numbers min <= max")
        if default is not None and (not isinstance(default, int) or not low <= default <= high):
            problem(path, f"settings.{key}: default within {low}-{high}")
    elif kind == "bool" and default is not None and not isinstance(default, bool):
        problem(path, f"settings.{key}: default true or false")
    elif kind == "str" and field.get("options") is not None:
        options = as_list(field["options"])
        if not options or (default is not None and str(default) not in options):
            problem(path, f"settings.{key}: default one of the options")


def check_package(path):
    data = load(path)
    if data is None:
        return
    for key in PACKAGE_FIELDS:
        if key not in data or str(data[key]).strip() == "":
            problem(path, f"missing {key}")
    if data.get("kind") not in KINDS:
        problem(path, f"kind one of {', '.join(sorted(KINDS))}")
    if data.get("format") != 1:
        problem(path, "format 1")
    if "name" in data and not NAME.match(str(data["name"])):
        problem(path, "name: lowercase letters, digits, - and _")
    for key, allowed in (("requires", REQUIRES), ("network", NETWORKS), ("chips", set(CHIPS))):
        unknown = set(as_list(data.get(key, []))) - allowed
        if unknown:
            problem(path, f"{key}: unknown {', '.join(sorted(unknown))}")
    settings = data.get("settings")
    if settings is not None:
        if not isinstance(settings, dict):
            problem(path, "settings: a mapping")
        else:
            for key, field in settings.items():
                if not re.match(r"^[a-z_][a-z0-9_]{0,31}$", key):
                    problem(path, f"settings.{key}: lowercase letters, digits and _")
                check_setting(path, key, field)


def check_component(path):
    data = load(path)
    if data is None:
        return
    if data.get("format") != 1:
        problem(path, "format 1")
    if not re.match(r"^[a-z][a-z0-9_]{0,23}$", str(data.get("type") or "")):
        problem(path, "type: lowercase letters, digits and _, at most 24")
    if data.get("matter") not in MATTER:
        problem(path, f"matter one of {', '.join(sorted(MATTER))}")
    if data.get("pins") not in (0, 1, 2):
        problem(path, "pins 0, 1 or 2")
    if not str(data.get("title") or "").strip():
        problem(path, "a title")
    unknown = set(as_list(data.get("requires", []))) - REQUIRES
    if unknown:
        problem(path, f"requires: unknown {', '.join(sorted(unknown))}")
    if not (path.parent / "CMakeLists.txt").exists():
        problem(path, "no CMakeLists.txt next to it")


def check_list(path):
    for number, line in enumerate(path.read_text().splitlines(), 1):
        entry = line.split("#", 1)[0].strip()
        if entry and not REPO.match(entry):
            problem(path, f"line {number}: owner/repository, got “{entry}”")


def main():
    for path in sorted([*ROOT.glob("examples/*.yaml"), *ROOT.glob("template/recipes/*.yaml")]):
        check_recipe(path)
    for path in sorted([*ROOT.glob("firmware/*/package.yaml"), *ROOT.glob("packages/*/package.yaml"),
                        *ROOT.glob("template/packages/*/package.yaml")]):
        check_package(path)
    for path in sorted(ROOT.glob("examples/components/*/component.yaml")):
        check_component(path)
    for path in (ROOT / "catalog/blocklist.txt", ROOT / "catalog/verified.txt"):
        if path.exists():
            check_list(path)
    for line in problems:
        print(line)
    print(f"{len(problems)} problem(s)" if problems else "all files are valid")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
