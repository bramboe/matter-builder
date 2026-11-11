"""Builds catalog.json: every package source and recipe the add-on can offer.

    GITHUB_TOKEN=<token> python3 tools/build_catalog.py <output folder>

A package source is a public GitHub repository with the topic
"matter-builder-package" and its public key (ECDSA P-256) in
matter-builder-key.pem at the root of its default branch. Its latest release
must have a manifest.json signed with that key (tools/release_manifest.py);
its packages are listed from that manifest. Recipes are recipes/*.yaml in the
repository (examples/*.yaml for the official source): specs of the generic device (tools/mb_generate.py), so only
connections, no code.

Nobody approves a source before it is listed. Users rate a source with a
star on its repository: from LIKED_STARS stars it is marked as liked by
users. Users report one with an issue here titled "Catalog report:
<owner/name>" (the add-on's Report link): with open reports from
REPORTS_TO_HIDE different accounts it is left out until the reports are
closed.

This repository is always in the catalog, as the official source. A
repository in catalog/blocklist.txt is left out; one in catalog/verified.txt
is marked as looked at by the project. Nothing here is trusted by the add-on
on its own: the add-on checks every release against the key it pinned when
the user added the source.
"""

import base64
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from mb_generate import SpecError, check, parse_yaml  # noqa: E402

OFFICIAL = os.environ.get("MB_OFFICIAL_REPO", "bramboe/matter-builder")
TOPIC = "matter-builder-package"
KEY_FILE = "matter-builder-key.pem"
API = "https://api.github.com"


def get(url, accept="application/vnd.github+json"):
    headers = {"Accept": accept, "User-Agent": "matter-builder-catalog"}
    if os.environ.get("GITHUB_TOKEN") and url.startswith(API):
        headers["Authorization"] = f"Bearer {os.environ['GITHUB_TOKEN']}"
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=30) as reply:
        return reply.read()


def get_json(url):
    return json.loads(get(url))


def listed(name):
    path = ROOT / "catalog" / name
    if not path.exists():
        return set()
    return {line.split("#")[0].strip().lower() for line in path.read_text().splitlines()} - {""}


def openssl(*args, data=None):
    return subprocess.run(["openssl", *args], input=data, capture_output=True)


def fingerprint(pem):
    with tempfile.TemporaryDirectory() as tmp:
        key = Path(tmp) / "k"
        key.write_bytes(pem)
        text = openssl("pkey", "-pubin", "-in", str(key), "-noout", "-text")
        if text.returncode != 0 or b"prime256v1" not in text.stdout and b"P-256" not in text.stdout:
            return None
        der = openssl("pkey", "-pubin", "-in", str(key), "-outform", "DER")
    return hashlib.sha256(der.stdout).hexdigest() if der.returncode == 0 else None


def verified_manifest(manifest, signature, pem):
    with tempfile.TemporaryDirectory() as tmp:
        for name, data in (("m", manifest), ("s", signature), ("k", pem)):
            (Path(tmp) / name).write_bytes(data)
        result = openssl("dgst", "-sha256", "-verify", str(Path(tmp) / "k"), "-signature", str(Path(tmp) / "s"),
                         str(Path(tmp) / "m"))
    return result.returncode == 0


def recipes(repo, branch, folder):
    """<folder>/*.yaml of a repository, as checked generic specs."""
    try:
        listing = get_json(f"{API}/repos/{repo}/contents/{folder}?ref={urllib.parse.quote(branch)}")
    except OSError:
        return []
    out = []
    for entry in listing if isinstance(listing, list) else []:
        if not entry.get("name", "").endswith(".yaml") or entry.get("size", 0) > 20000:
            continue
        try:
            text = base64.b64decode(get_json(entry["url"])["content"]).decode()
            spec = check(parse_yaml(text))
        except (OSError, ValueError, KeyError, SpecError) as err:
            print(f"  {repo}: recipe {entry['name']} skipped: {err}")
            continue
        if spec["app"] != "generic":
            continue
        out.append({"id": f"{repo}/{entry['name'][:-5]}", "title": spec["name"], "text": text,
                    "connections": spec["connections"], "requires": spec.get("requires", [])})
    return out


LIKED_STARS = 5
REPORTS_TO_HIDE = 3
REPORT_TITLE = "catalog report:"


def reports():
    """Sources reported by users: repository (lower case) -> the accounts with an open report."""
    out, page = {}, 1
    while page <= 5:
        try:
            issues = get_json(f"{API}/repos/{OFFICIAL}/issues?state=open&per_page=100&page={page}")
        except OSError as err:
            print(f"  reports not read: {err}")
            break
        for issue in issues if isinstance(issues, list) else []:
            title = str(issue.get("title") or "").strip().lower()
            if "pull_request" in issue or not title.startswith(REPORT_TITLE):
                continue
            repo = title[len(REPORT_TITLE):].strip().split(" ")[0]
            if repo.count("/") == 1:
                out.setdefault(repo, set()).add((issue.get("user") or {}).get("login"))
        if not isinstance(issues, list) or len(issues) < 100:
            break
        page += 1
    return out


def source(repo, official, verified):
    """The catalog items of one repository, or None when it does not qualify."""
    info = get_json(f"{API}/repos/{repo}")
    if info.get("private") or info.get("archived"):
        return None
    branch = info["default_branch"]
    # A source with only recipes needs no key: recipes are connections, not code
    try:
        pem = get(f"https://raw.githubusercontent.com/{repo}/{branch}/{KEY_FILE}", accept="text/plain")
    except OSError:
        pem = b""
    key = fingerprint(pem) if pem else None
    if pem and not key:
        print(f"  {repo}: {KEY_FILE} is not an ECDSA P-256 public key")
        return None
    base = {"source": repo, "official": official, "verified": official or repo.lower() in verified,
            "maker": info["owner"]["login"], "stars": info.get("stargazers_count", 0),
            "liked": not official and info.get("stargazers_count", 0) >= LIKED_STARS,
            "url": info["html_url"], "key_sha256": key}
    items = []
    try:
        # A source with only recipes has no key and no release
        release = get_json(f"{API}/repos/{repo}/releases/latest") if key else None
        assets = {a["name"]: a for a in (release or {}).get("assets", [])}
        manifest = get(assets["manifest.json"]["browser_download_url"], accept="application/octet-stream")
        signature = get(assets["manifest.json.sig"]["browser_download_url"], accept="application/octet-stream")
    except (OSError, KeyError):
        release = None
    if release and verified_manifest(manifest, signature, pem):
        data = json.loads(manifest)
        for pkg in data.get("packages", []):
            images = [i for i in data.get("images", []) if i["app"] == pkg["app"]]
            if not images:
                continue
            newest = images[-1]
            factory = next((f["file"] for f in data.get("factory", [])
                            if f["app"] == pkg["app"] and f["version"] == newest["version"]), None)
            items.append(dict(base, kind="firmware", id=f"{repo}/{pkg['name']}", title=pkg["title"],
                              description=pkg["description"], app=pkg["app"], version=newest["version"],
                              requires=pkg.get("requires", []), network=pkg.get("network", ["thread"]),
                              settings=pkg.get("settings", {}),
                              release=data.get("release"), updated=release.get("published_at"),
                              downloads=sum(a.get("download_count", 0) for a in assets.values()),
                              factory=factory))
    elif release:
        print(f"  {repo}: release signature does not match its key: firmware left out")
    # The project's own recipes are its examples
    for recipe in recipes(repo, branch, "examples" if official else "recipes"):
        items.append(dict(base, kind="recipe", **recipe))
    return items


def main(out):
    blocked, verified = listed("blocklist.txt"), listed("verified.txt")
    found = get_json(f"{API}/search/repositories?q=topic:{TOPIC}&per_page=100")
    repos = [OFFICIAL] + [r["full_name"] for r in found.get("items", []) if r["full_name"].lower() != OFFICIAL.lower()]
    reported = reports()
    items = []
    for repo in repos:
        if repo.lower() in blocked:
            print(f"  {repo}: blocked")
            continue
        reporters = reported.get(repo.lower(), set())
        if repo != OFFICIAL and len(reporters) >= REPORTS_TO_HIDE:
            print(f"  {repo}: left out, reported by {len(reporters)} users")
            continue
        try:
            got = source(repo, repo == OFFICIAL, verified)
        except Exception as err:  # one source that goes wrong must not stop the catalog
            print(f"  {repo}: skipped: {err}")
            continue
        items += got or []
        print(f"  {repo}: {len(got or [])} items")
    Path(out).mkdir(parents=True, exist_ok=True)
    (Path(out) / "catalog.json").write_text(json.dumps(
        {"format": 1, "generated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "items": items}, indent=1) + "\n")
    print(f"{len(items)} items")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "site")
