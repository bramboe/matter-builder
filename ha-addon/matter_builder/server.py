"""Matter Builder: the add-on's web page (Home Assistant ingress) and API.

Proxies are linked with a token of the add-on's own (POST /link on the
proxy: the page password plus a press of its BOOT button); the password is
never stored. Builds and updates run one at a time as a "job" with a log.
Only the standard library: the image is Espressif's ESP-IDF image.
"""

import base64
import hashlib
import http.server
import ipaddress
import json
import os
import re
import shutil
import secrets
import socket
import struct
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

try:  # in the image; a local test without it simply finds nothing
    from zeroconf import IPVersion, ServiceBrowser, ServiceStateChange, Zeroconf
except ImportError:
    Zeroconf = None

DATA = Path(os.environ.get("MB_DATA", "/data"))
HERE = Path(__file__).resolve().parent
PORT = int(os.environ.get("MB_PORT", "8099"))
# Home Assistant ingress connects from this address; with host networking
# the port is also open on the LAN, so nothing else may use it
INGRESS = "172.30.32.2"
ALLOW_ANY = os.environ.get("MB_ALLOW_ANY") == "1"  # local testing only
COMMIT = os.environ.get("ESP_MATTER_COMMIT", "")
CONFIG_HOST = "octo-bridge.local"  # configuration mode of firmware that does not say where
PROGRAM_NAME = "Home Assistant"

PROXIES = DATA / "proxies.json"
SETTINGS = DATA / "settings.json"
NOTIFIED = DATA / "notified.json"
RELEASES = DATA / "releases"
SPECS = DATA / "specs"
SRC = Path(os.environ.get("MB_SRC", "/opt/octo-matter"))
RELEASE_KEY = HERE / "release-key.pem"
# Where ready-made firmware comes from: GitHub releases, signed (see
# .github/workflows/release.yml); the add-on option "releases_repo" overrides it
DEFAULT_RELEASES_REPO = "bramboe/matter-builder"
# Other package sources (GitHub repositories) keep their public key here, at
# the root of their default branch; the add-on pins it when a source is added
SOURCE_KEY_FILE = "matter-builder-key.pem"
REPO_NAME = re.compile(r"^[A-Za-z0-9-]{1,39}/[A-Za-z0-9._-]{1,100}$")
# The catalog of all package sources, built nightly by .github/workflows/catalog.yml
DEFAULT_CATALOG_URL = "https://bramboe.github.io/matter-builder/catalog.json"
SUPERVISOR_TOKEN = os.environ.get("SUPERVISOR_TOKEN", "")
FIRMWARE = DATA / "firmware"
FACTORY = FIRMWARE / "factory"  # images for a new ESP32 (USB)
JOB_LOG = DATA / "job.log"
# Flash backups before installing over USB on this machine: in Home
# Assistant's share folder, so they can be fetched over Samba or the file editor
BACKUPS = Path(os.environ.get("MB_BACKUPS", "/share/matter-builder/backups"))

lock = threading.Lock()
job = {"kind": None, "running": False, "ok": None, "started": 0, "title": ""}
links = {}  # link id -> request on its way (the password only lives here)
found = {}  # device id -> a Matter Builder device seen on the network
release_state = {"release": None, "images": [], "factory": [], "packages": [], "sources": [], "error": None, "checked": 0}
catalog_cache = {"data": None, "error": None, "fetched": 0}


# ---------------------------------------------------------------- storage

def load_proxies():
    try:
        return json.loads(PROXIES.read_text())
    except (OSError, ValueError):
        return []


def save_proxies(proxies):
    PROXIES.parent.mkdir(parents=True, exist_ok=True)
    tmp = PROXIES.with_suffix(".tmp")
    tmp.write_text(json.dumps(proxies, indent=2))
    os.chmod(tmp, 0o600)
    tmp.replace(PROXIES)


def load_settings():
    try:
        return {"dev_mode": False, **json.loads(SETTINGS.read_text())}
    except (OSError, ValueError):
        return {"dev_mode": False}


def save_settings(settings):
    SETTINGS.parent.mkdir(parents=True, exist_ok=True)
    SETTINGS.write_text(json.dumps(settings))


def system_info():
    """What decides how long a build takes: processor cores and memory."""
    try:
        cores = len(os.sched_getaffinity(0))
    except AttributeError:
        cores = os.cpu_count() or 0
    memory = {}
    try:
        for line in Path("/proc/meminfo").read_text().splitlines():
            key, value = line.split(":", 1)
            memory[key] = int(value.split()[0]) * 1024
    except (OSError, ValueError):
        pass
    disk = shutil.disk_usage(DATA if DATA.exists() else "/")
    return {"cores": cores, "memory": memory.get("MemTotal", 0), "memory_free": memory.get("MemAvailable", 0),
            "disk_free": disk.free}


def find_proxy(proxy_id):
    return next((p for p in load_proxies() if p["id"] == proxy_id), None)


# ---------------------------------------------------------------- proxies

def url_host(host):
    """An IPv6 address needs brackets in a URL."""
    host = host.strip()
    try:
        if ipaddress.ip_address(host).version == 6:
            return f"[{host}]"
    except ValueError:
        pass  # a name, or an address with a port
    return host


def request(host, path, token, body=None, headers=None, timeout=20):
    """Returns (status, text). Status 0: no answer."""
    req = urllib.request.Request(f"http://{url_host(host)}{path}", data=body, method="POST" if body is not None else "GET")
    req.add_header("X-Octo-Token", token)
    for key, value in (headers or {}).items():
        req.add_header(key, value)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as reply:
            return reply.status, reply.read().decode(errors="replace")
    except urllib.error.HTTPError as err:
        return err.code, err.read().decode(errors="replace")
    except (OSError, ValueError) as err:
        return 0, str(err)


def command(proxy, line, timeout=20):
    return request(resolve(proxy["host"]) or proxy["host"], "/cmd", proxy["token"], line.encode(), timeout=timeout)


def proxy_name(host, token):
    """A name from what the proxy knows: its own name, or its beds ("Alex & Sam")."""
    status, text = request(resolve(host) or host, "/cmd", token, b"json")
    try:
        state = json.loads(text)
        if "io" in state:
            return state["io"]["name"]
        names = [b["name"] for b in state["beds"] if b.get("configured") and b.get("name")]
    except (ValueError, KeyError, TypeError):
        names = []
    return " & ".join(names) if status == 200 and names else host


def proxy_status(proxy):
    status, system = command(proxy, "system")
    if status == 401:
        return {"ok": False, "auth": True, "error": "key no longer accepted"}
    if status != 200:
        return {"ok": False, "error": system.strip() or f"HTTP {status}"}
    _, state = command(proxy, "json")
    try:
        state = json.loads(state)
    except ValueError:
        state = {}
    _, beds = command(proxy, "status") if "beds" in state else (0, "")
    # "running: app1 @0x320000: octo_bridge 1.0.10 (...)": the firmware may
    # have changed kind since the last time (e.g. generic, then Octo again)
    running_app = system.split("\n")[0].split(": ")[-1].split(" ")[0]
    if not proxy.get("device_id") or proxy.get("app") != running_app:
        remember_device_id(proxy)
    return {"ok": True, "system": system, "beds": beds, "io": state.get("io")}


def remember_device_id(proxy):
    """Proxies linked before /info existed learn their id once the firmware has it."""
    device = device_info(resolve(proxy["host"]) or proxy["host"])
    if not device:
        return
    proxies = load_proxies()
    for p in proxies:
        if p["id"] == proxy["id"]:
            p["device_id"] = device["id"]
            p["app"] = device.get("app")
    save_proxies(proxies)


# ---------------------------------------------------------------- mDNS

def mdns_query(name, qtype):
    labels = b"".join(bytes([len(part)]) + part.encode() for part in name.rstrip(".").split("."))
    # One question, "unicast response" bit set so the answer comes back to us
    return struct.pack("!6H", 0, 0, 1, 0, 0, 0) + labels + b"\0" + struct.pack("!HH", qtype, 0x8001)


def skip_name(packet, pos):
    while pos < len(packet):
        length = packet[pos]
        if length == 0:
            return pos + 1
        if length & 0xC0 == 0xC0:
            return pos + 2
        pos += 1 + length
    return pos


def mdns_resolve(name, timeout=2.0):
    """The address of a .local name, IPv4 first. None when nobody answers."""
    family_types = [(socket.AF_INET, 1, "224.0.0.251"), (socket.AF_INET6, 28, "ff02::fb")]
    for family, qtype, group in family_types:
        try:
            sock = socket.socket(family, socket.SOCK_DGRAM)
        except OSError:
            continue
        with sock:
            sock.settimeout(timeout)
            try:
                sock.sendto(mdns_query(name, qtype), (group, 5353))
                end = time.time() + timeout
                while time.time() < end:
                    packet, _ = sock.recvfrom(4096)
                    answers = struct.unpack("!H", packet[6:8])[0]
                    pos = skip_name(packet, 12) + 4 if struct.unpack("!H", packet[4:6])[0] else 12
                    for _ in range(answers):
                        pos = skip_name(packet, pos)
                        rtype, _, _, length = struct.unpack("!HHIH", packet[pos:pos + 10])
                        data = packet[pos + 10:pos + 10 + length]
                        pos += 10 + length
                        if rtype == 1 and length == 4:
                            return socket.inet_ntop(socket.AF_INET, data)
                        if rtype == 28 and length == 16:
                            return socket.inet_ntop(socket.AF_INET6, data)
            except (OSError, struct.error):
                continue
    return None


def resolve(host):
    host = host.strip().strip("[]")
    if host.count(":") == 1:
        return host  # name or IPv4 with a port
    return mdns_resolve(host) if host.endswith(".local") else host


# ---------------------------------------------------------------- jobs

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
STEP = re.compile(r"^\[(\d+)/(\d+)\]")
PERCENT = re.compile(r"\((\d+) ?%\)")  # esptool: "Writing at 0x00010000... (12 %)"


def clean_log(text):
    """Without terminal colours; a line rewritten in place (\r) keeps its last state."""
    lines = [ANSI.sub("", line.split("\r")[-1]) for line in text.split("\n")]
    return "\n".join(lines)


def progress(text):
    """The last "[n/m]" step of the build (ninja), and the phase it is in."""
    phase, done, total = "", 0, 0
    for line in text.split("\n"):
        if line.startswith("== "):
            phase = line[3:].strip()
        match = STEP.match(line)
        if match:
            done, total = int(match.group(1)), int(match.group(2))
        match = PERCENT.search(line)
        if match:
            done, total = int(match.group(1)), 100
    return {"phase": phase, "done": done, "total": total}


def log(text):
    with JOB_LOG.open("a") as f:
        f.write(text.rstrip("\n") + "\n")


def start_job(kind, title, target):
    with lock:
        if job["running"]:
            return False
        job.update(kind=kind, running=True, ok=None, started=time.time(), title=title)
    JOB_LOG.parent.mkdir(parents=True, exist_ok=True)
    JOB_LOG.write_text("")

    def run():
        ok = False
        try:
            ok = target()
        except Exception as err:  # the log shows it; the server keeps running
            log(f"!! {err!r}")
        with lock:
            job.update(running=False, ok=ok)

    threading.Thread(target=run, daemon=True).start()
    return True


def run_script(name, *args):
    with JOB_LOG.open("a") as out:
        result = subprocess.run(["bash", str(HERE / name), *args], stdout=out, stderr=subprocess.STDOUT,
                                env=os.environ.copy())
    log(f"== exit {result.returncode}")
    return result.returncode == 0


def toolchain_ready():
    return bool(COMMIT) and (DATA / "toolchain" / f"ready-{COMMIT}").exists()


def builds():
    out = []
    for path in sorted(FIRMWARE.glob("*.bin"), key=lambda p: p.stat().st_mtime, reverse=True):
        stat = path.stat()
        out.append({"file": path.name, "size": stat.st_size, "date": int(stat.st_mtime)})
    return out


def update_proxy(proxy, image):
    """Configuration mode on Wi-Fi (fast upload), the image, then back."""
    log(f"== Update {proxy['name']} with {image.name}")
    status, text = command(proxy, "system")
    if status != 200:
        log(f"!! Proxy not reachable: {text.strip()}")
        return False
    log(text.strip().splitlines()[0])

    log("== Configuration mode on Wi-Fi")
    status, text = command(proxy, "wifi config 15")
    log(text.strip() or f"(no answer, HTTP {status}: the proxy may already be restarting)")
    if status == 200 and "restarting" not in text:
        log("!! The proxy did not go to configuration mode")
        return False

    # The proxy says where it will be ("... http://matter-builder.local/")
    found = re.search(r"http://([A-Za-z0-9-]+\.local)/", text)
    host = found.group(1) if found else CONFIG_HOST
    log(f"== Looking for {host}")
    address = None
    end = time.time() + 150
    while time.time() < end and not address:
        time.sleep(3)
        address = mdns_resolve(host)
    if not address:
        log("!! Not found on Wi-Fi: it returns to Thread by itself within 15 minutes")
        return False
    log(f"   found at {address}")

    log("== Uploading the firmware")
    time.sleep(2)
    status, text = request(address, "/ota", proxy["token"], image.read_bytes(), timeout=300)
    log(text.strip() or f"HTTP {status}")
    if status != 200:
        return False

    log("== Waiting for the proxy on its own address")
    end = time.time() + 240
    while time.time() < end:
        time.sleep(5)
        status, text = command(proxy, "system", timeout=10)
        if status == 200:
            log(text.strip().splitlines()[0])
            log("== Done")
            return True
    log("!! The proxy did not come back yet; boot_guard returns to the previous firmware if it cannot get online")
    return False


# ---------------------------------------------------------------- releases

def option(name, default):
    try:
        return json.loads((DATA / "options.json").read_text()).get(name) or default
    except (OSError, ValueError):
        return default


def releases_repo():
    return option("releases_repo", DEFAULT_RELEASES_REPO)


def download(url, timeout=60, accept="application/octet-stream"):
    req = urllib.request.Request(url, headers={"Accept": accept, "User-Agent": "matter-builder"})
    with urllib.request.urlopen(req, timeout=timeout) as reply:
        return reply.read()


def key_fingerprint(pem):
    """SHA-256 of an ECDSA P-256 public key (DER), or None when it is not one."""
    with tempfile.TemporaryDirectory() as tmp:
        (Path(tmp) / "k").write_bytes(pem)
        text = subprocess.run(["openssl", "pkey", "-pubin", "-in", str(Path(tmp) / "k"), "-noout", "-text"],
                              capture_output=True, text=True)
        if text.returncode != 0 or "prime256v1" not in text.stdout and "P-256" not in text.stdout:
            return None
        der = subprocess.run(["openssl", "pkey", "-pubin", "-in", str(Path(tmp) / "k"), "-outform", "DER"],
                             capture_output=True)
    return hashlib.sha256(der.stdout).hexdigest() if der.returncode == 0 else None


def signature_ok(manifest, signature, key_pem):
    """ECDSA P-256 over SHA-256, checked with openssl against a source's key."""
    with tempfile.TemporaryDirectory() as tmp:
        (Path(tmp) / "m").write_bytes(manifest)
        (Path(tmp) / "s").write_bytes(signature)
        (Path(tmp) / "k").write_bytes(key_pem)
        result = subprocess.run(["openssl", "dgst", "-sha256", "-verify", str(Path(tmp) / "k"), "-signature",
                                 str(Path(tmp) / "s"), str(Path(tmp) / "m")], capture_output=True)
    return result.returncode == 0


def sources():
    """The project's own releases first (its key ships with the add-on), then
    the package sources added by the user, each with the key pinned then."""
    own = releases_repo()
    out = [{"repo": own, "key": RELEASE_KEY.read_text(), "official": True}]
    for src in load_settings().get("sources", []):
        if src.get("repo") != own:
            out.append({"repo": src["repo"], "key": src["key"], "official": False})
    return out


def check_source(src):
    """A source's latest release, only when its manifest's signature is valid."""
    result = {"repo": src["repo"], "official": src["official"], "release": None, "error": None, "data": {}, "assets": {}}
    try:
        info = json.loads(download(f"https://api.github.com/repos/{src['repo']}/releases/latest", 20,
                                     accept="application/vnd.github+json"))
        assets = {a["name"]: a["browser_download_url"] for a in info.get("assets", [])}
        manifest = download(assets["manifest.json"])
        signature = download(assets["manifest.json.sig"])
    except (OSError, ValueError, KeyError) as err:
        result["error"] = f"{src['repo']}: {err}"
        return result
    if not signature_ok(manifest, signature, src["key"].encode()):
        result["error"] = f"{src['repo']}: signature does not match: release ignored"
        return result
    result.update(data=json.loads(manifest), assets=assets)
    result["release"] = result["data"].get("release")
    return result


def check_releases():
    """All sources; an image name that two sources use is taken from the first."""
    results = [check_source(src) for src in sources()]
    merged = {"images": [], "factory": [], "packages": []}
    seen = set()
    for res in results:
        for kind in ("images", "factory"):
            for img in res["data"].get(kind, []):
                if img["file"] in res["assets"] and (kind, img["file"]) not in seen:
                    seen.add((kind, img["file"]))
                    merged[kind].append(dict(img, url=res["assets"][img["file"]], source=res["repo"], official=res["official"]))
        for pkg in res["data"].get("packages", []):
            merged["packages"].append(dict(pkg, source=res["repo"], official=res["official"]))
    own = results[0]
    release_state.update(release=own["release"], error=own["error"], checked=int(time.time()),
                         sources=[{k: r[k] for k in ("repo", "official", "release", "error")} for r in results], **merged)


def preview_source(repo):
    """What the user confirms before adding a source: its key's fingerprint."""
    if not REPO_NAME.match(repo):
        return {"error": "not a GitHub repository (owner/name)"}
    try:
        info = json.loads(download(f"https://api.github.com/repos/{repo}", 20, accept="application/vnd.github+json"))
    except urllib.error.HTTPError as err:
        return {"error": f"{repo} was not found on GitHub" if err.code == 404 else f"{repo}: {err}"}
    except (OSError, ValueError) as err:
        return {"error": f"{repo}: {err}"}
    try:
        pem = download(f"https://raw.githubusercontent.com/{repo}/HEAD/{SOURCE_KEY_FILE}", 20)
    except urllib.error.HTTPError as err:
        return {"error": f"{repo} is not a package source: it has no {SOURCE_KEY_FILE}" if err.code == 404 else f"{repo}: {err}"}
    except OSError as err:
        return {"error": f"{repo}: {err}"}
    fingerprint = key_fingerprint(pem)
    if not fingerprint:
        return {"error": f"{repo} has no valid {SOURCE_KEY_FILE}"}
    return {"repo": repo, "fingerprint": fingerprint, "key": pem.decode(), "description": info.get("description") or "",
            "stars": info.get("stargazers_count", 0), "owner": info.get("owner", {}).get("login", "")}


def catalog():
    """The catalog of package sources, fetched at most once an hour."""
    if catalog_cache["data"] is None or time.time() - catalog_cache["fetched"] > 3600:
        try:
            catalog_cache.update(data=json.loads(download(option("catalog_url", DEFAULT_CATALOG_URL), 20,
                                                          accept="application/json")), error=None)
        except (OSError, ValueError) as err:
            catalog_cache["error"] = str(err)
        catalog_cache["fetched"] = time.time()
    return {"catalog": catalog_cache["data"], "error": catalog_cache["error"]}


def releases_loop():
    while True:
        check_releases()
        time.sleep(6 * 3600)


def release_image(name, kind="images"):
    """A release image on disk, downloaded when needed and checked against the
    signed hash: an update image, or with kind "factory" one for a new ESP32."""
    image = next((img for img in release_state[kind] if img["file"] == name), None)
    if image is None:
        return None
    RELEASES.mkdir(parents=True, exist_ok=True)
    path = RELEASES / image["file"]
    if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != image["sha256"]:
        data = download(image["url"], 300)
        if hashlib.sha256(data).hexdigest() != image["sha256"]:
            raise ValueError("downloaded firmware does not match the signed hash")
        path.write_bytes(data)
    return path


# ---------------------------------------------------------------- Home Assistant's Matter server

# The Matter Server add-on listens on the host network; so does this add-on
MATTER_SERVER = ("127.0.0.1", 5580)


def matter_server(command, args, timeout=300):
    """One command to the Matter server's WebSocket API, and its answer.
    A small client of its own: the add-on has no WebSocket library."""
    sock = socket.create_connection(MATTER_SERVER, timeout=10)
    sock.settimeout(timeout)
    try:
        key = base64.b64encode(os.urandom(16)).decode()
        sock.sendall((f"GET /ws HTTP/1.1\r\nHost: {MATTER_SERVER[0]}:{MATTER_SERVER[1]}\r\nUpgrade: websocket\r\n"
                      f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = sock.recv(1024)
            if not chunk:
                raise OSError("Matter server closed the connection")
            head += chunk
        status_line = head.split(b"\r\n")[0].decode(errors="replace")
        if " 101 " not in status_line:
            raise OSError(f"Matter server refused: {status_line}")
        rest = head.split(b"\r\n\r\n", 1)[1]

        def read(n):
            nonlocal rest
            while len(rest) < n:
                chunk = sock.recv(65536)
                if not chunk:
                    raise OSError("Matter server closed the connection")
                rest += chunk
            data, rest = rest[:n], rest[n:]
            return data

        def send(opcode, payload):
            mask = os.urandom(4)
            size = len(payload)
            header = bytes([0x80 | opcode]) + (bytes([0x80 | size]) if size < 126 else
                                               bytes([0x80 | 126]) + struct.pack(">H", size) if size < 65536 else
                                               bytes([0x80 | 127]) + struct.pack(">Q", size))
            sock.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

        def receive():
            message = b""
            while True:
                first, second = read(2)
                size = second & 0x7F
                if size == 126:
                    size = struct.unpack(">H", read(2))[0]
                elif size == 127:
                    size = struct.unpack(">Q", read(8))[0]
                payload = read(size)
                opcode = first & 0x0F
                if opcode == 9:
                    send(10, payload)
                    continue
                if opcode == 8:
                    raise OSError("Matter server closed the connection")
                message += payload
                if first & 0x80:
                    return json.loads(message)

        receive()  # server info first
        message_id = secrets.token_hex(4)
        send(1, json.dumps({"message_id": message_id, "command": command, "args": args}).encode())
        while True:
            reply = receive()
            if reply.get("message_id") == message_id:
                if "error_code" in reply:
                    raise OSError(f"Matter server: {reply.get('details') or reply['error_code']}")
                return reply.get("result")
    finally:
        sock.close()


def add_to_ha(proxy):
    """A device into Home Assistant's Matter fabric: a pairing window on the
    device, then the Matter server commissions it over the network."""
    log(f"== Adding {proxy['name']} to Home Assistant")
    status, text = command(proxy, "matter open")
    log(text.strip())
    if status != 200:
        return False
    status, text = command(proxy, "matter")
    try:
        code = json.loads(text)["qr"]
    except (ValueError, KeyError):
        log("!! The device gave no pairing code")
        return False
    log("== Commissioning over the network (a minute or two on Thread)")
    try:
        node = matter_server("commission_with_code", {"code": code, "network_only": True})
    except (OSError, ValueError) as err:
        if "already commissioned into this fabric" in str(err):
            log("== Already in Home Assistant")
            return True
        log(f"!! {err}")
        return False
    log(f"== Added: Matter node {node.get('node_id') if isinstance(node, dict) else node}")
    return True


# ---------------------------------------------------------------- USB on this machine

def serial_ports():
    """USB serial ports on the Home Assistant machine (the add-on has "uart"),
    by their stable name when there is one."""
    ports, seen = [], set()
    by_id = Path("/dev/serial/by-id")
    for link in sorted(by_id.glob("*")) if by_id.exists() else []:
        target = str(link.resolve())
        seen.add(target)
        ports.append({"path": str(link), "name": link.name.removeprefix("usb-").replace("_", " ")})
    for dev in sorted(Path("/dev").glob("tty[UA][SC][BM]*")):
        if str(dev) not in seen:
            ports.append({"path": str(dev), "name": dev.name})
    return ports


def flash_on_host(port, image, backup):
    """A factory image at 0 with esptool, after a backup of the whole flash."""
    esptool = ["python3", "-m", "esptool", "--chip", "esp32c6", "-p", port, "-b", "460800"]
    if backup:
        BACKUPS.mkdir(parents=True, exist_ok=True)
        target = BACKUPS / f"backup-esp32c6-{time.strftime('%Y%m%d-%H%M%S')}.bin"
        log(f"== Backup of the whole flash to {target}")
        if not run_logged([*esptool, "read_flash", "0", "ALL", str(target)]):
            return False
    log(f"== Installing {image.name}")
    return run_logged([*esptool, "write_flash", "0x0", str(image)])


def run_logged(cmd):
    with JOB_LOG.open("a") as out:
        result = subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT)
    log(f"== exit {result.returncode}")
    return result.returncode == 0


# ---------------------------------------------------------------- discovery

def device_info(address):
    """The device's public /info, or None when it is not a Matter Builder device."""
    status, text = request(address, "/info", "", timeout=4)
    try:
        info = json.loads(text) if status == 200 else None
    except ValueError:
        return None
    return info if isinstance(info, dict) and info.get("kind") == "matter-builder" else None


def usable_address(addresses):
    """A routable address: IPv6 on the Thread or home network, else IPv4."""
    v6 = [a for a in addresses if ":" in a and not a.lower().startswith("fe80")]
    v4 = [a for a in addresses if "." in a and ":" not in a]
    return (v6 + v4 + [None])[0]


def linked(device_id, host, address):
    return any(p.get("device_id") == device_id or p["host"] in (host, address) for p in load_proxies())


def notify(device):
    """A Home Assistant notification for a device found for the first time."""
    try:
        notified = set(json.loads(NOTIFIED.read_text()))
    except (OSError, ValueError):
        notified = set()
    if not SUPERVISOR_TOKEN or device["id"] in notified:
        return
    name = device["app"].replace("_", " ").capitalize()
    message = (f"**{name}** ({device['host']}, firmware {device['version']}) can be linked to Matter Builder.\n\n"
               "[Open Matter Builder](/hassio/ingress/local_matter_builder)")
    body = json.dumps({"title": "Matter Builder: device found", "message": message,
                       "notification_id": f"matter_builder_{device['id']}"}).encode()
    req = urllib.request.Request("http://supervisor/core/api/services/persistent_notification/create", data=body,
                                 method="POST", headers={"Authorization": f"Bearer {SUPERVISOR_TOKEN}",
                                                         "Content-Type": "application/json"})
    try:
        urllib.request.urlopen(req, timeout=10).close()
    except OSError as err:
        print(f"notification failed: {err}", flush=True)
        return
    notified.add(device["id"])
    NOTIFIED.write_text(json.dumps(sorted(notified)))


def dismiss(device_id):
    if not SUPERVISOR_TOKEN or not device_id:
        return
    body = json.dumps({"notification_id": f"matter_builder_{device_id}"}).encode()
    req = urllib.request.Request("http://supervisor/core/api/services/persistent_notification/dismiss", data=body,
                                 method="POST", headers={"Authorization": f"Bearer {SUPERVISOR_TOKEN}",
                                                         "Content-Type": "application/json"})
    try:
        urllib.request.urlopen(req, timeout=10).close()
    except OSError:
        pass


class Discovery:
    """Every Matter device announces itself (_matter._tcp once paired,
    _matterc._udp while waiting for pairing); those answering /info as
    "matter-builder" are ours. Each host is asked at most every 10 minutes."""

    TYPES = ["_matter._tcp.local.", "_matterc._udp.local."]

    def __init__(self):
        self.zc = None
        self.asked = {}

    def start(self):
        if Zeroconf is None:
            print("zeroconf missing: no discovery", flush=True)
            return
        self.zc = Zeroconf(ip_version=IPVersion.All)
        ServiceBrowser(self.zc, self.TYPES, handlers=[self.on_change])

    def on_change(self, zeroconf, service_type, name, state_change):
        if state_change is not ServiceStateChange.Removed:
            threading.Thread(target=self.check, args=(service_type, name), daemon=True).start()

    def check(self, service_type, name):
        info = self.zc.get_service_info(service_type, name, timeout=3000)
        if not info:
            return
        host = (info.server or "").rstrip(".")
        with lock:
            if time.time() - self.asked.get(host, 0) < 600:
                return
            self.asked[host] = time.time()
        address = usable_address(info.parsed_addresses())
        device = device_info(address) if address else None
        if not device:
            return
        device.update(host=host, address=address, seen=int(time.time()))
        with lock:
            found[device["id"]] = device
        print(f"found {device['app']} {device['version']} ({device['id']}) at {host} {address}", flush=True)
        moved(device)
        if not linked(device["id"], host, address):
            notify(device)


discovery = Discovery()


def moved(device):
    """A linked device seen at another address (a new Thread address after a
    reset or reinstall): follow it, so its card shows it again; a key it no
    longer accepts then shows as "Link again"."""
    proxies = load_proxies()
    changed = False
    for p in proxies:
        if p.get("device_id") == device["id"] and p["host"] not in (device["host"], device["address"]):
            print(f"{p['name']} moved from {p['host']} to {device['address']}", flush=True)
            p["host"], p["app"] = device["address"], device.get("app")
            changed = True
    if changed:
        save_proxies(proxies)


def found_unlinked():
    with lock:
        devices = list(found.values())
    return [d for d in devices if not linked(d["id"], d["host"], d["address"])]


# ---------------------------------------------------------------- device specs

def check_spec(text):
    """(spec, None) when valid, else (None, error), using the generator itself."""
    with tempfile.NamedTemporaryFile("w", suffix=".yaml", delete=False) as f:
        f.write(text)
        path = f.name
    try:
        result = subprocess.run(["python3", str(SRC / "tools" / "mb_generate.py"), "--check", path],
                                capture_output=True, text=True)
    finally:
        os.unlink(path)
    if result.returncode != 0:
        return None, (result.stderr.strip() or result.stdout.strip()).removeprefix("spec error: ")
    return json.loads(result.stdout), None


def list_specs():
    out = []
    for path in sorted(SPECS.glob("*.yaml")):
        spec, error = check_spec(path.read_text())
        project = spec["slug"].replace("-", "_") if spec else None
        factory = [f.name for f in FACTORY.glob(f"{project}-*-factory.bin")] if project else []
        out.append({"slug": path.stem, "name": spec["name"] if spec else path.stem, "app": spec["app"] if spec else "",
                    "version": spec["version"] if spec else "", "error": error,
                    "requires": spec.get("requires", []) if spec else [], "network": spec.get("network") if spec else None,
                    "firmware": sorted((p.name for p in FIRMWARE.glob(f"{project}-*.bin")), key=version_key)
                    if project else [], "factory": sorted(factory, key=lambda n: version_key(n.removesuffix("-factory.bin")))})
    return out


def examples():
    folder = SRC / "examples"
    return {p.stem: p.read_text() for p in sorted(folder.glob("*.yaml"))} if folder.exists() else {}


# ---------------------------------------------------------------- management

# What the management pages may send to a proxy: its connections, name,
# and Wi-Fi, and its Matter pairing (the code, and a window for one more
# controller such as Home Assistant); nothing that changes access or the firmware
ALLOWED_COMMANDS = ("io ", "name ", "wifi ssid ", "wifi password ")


def allowed(line):
    return line in ("io json", "json", "wifi json", "matter", "matter open", "pairing", "pairing ble on",
                    "pairing ble off", "reboot") or line.startswith(ALLOWED_COMMANDS)


def version_key(name):
    """octo_bridge-1.0.10.bin -> (1, 0, 10)"""
    version = name.rsplit("-", 1)[-1].removesuffix(".bin")
    return tuple(int(part) if part.isdigit() else 0 for part in version.split("."))


def newest_firmware(app):
    """The newest image for an application: own builds and signed releases."""
    names = [p.name for p in FIRMWARE.glob(f"{app}-*.bin")] + \
            [i["file"] for i in release_state["images"] if i["app"] == app]
    return max(names, key=version_key, default=None)


def update_all():
    """Every proxy to the newest firmware of its own application, one after the
    other; stops at the first that does not come back."""
    for proxy in load_proxies():
        app = proxy.get("app")
        name = newest_firmware(app) if app else None
        if not name:
            log(f"== {proxy['name']}: no firmware for '{app}', skipped")
            continue
        status, system = command(proxy, "system")
        running = (system.split("\n")[0].split(": ")[-1].split(" (")[0] if status == 200 else "")
        if running.endswith(" " + name.rsplit("-", 1)[-1].removesuffix(".bin")):
            log(f"== {proxy['name']}: already on {running}")
            continue
        image = FIRMWARE / name if (FIRMWARE / name).is_file() else release_image(name)
        if not update_proxy(proxy, image):
            log("!! Stopped: the other proxies keep their firmware")
            return False
    log("== All proxies up to date")
    return True


# ---------------------------------------------------------------- links

def link_start(host, password):
    address = resolve(host) or host
    status, text = request(address, "/link", password, PROGRAM_NAME.encode())
    if status == 401:
        return {"error": "wrong password"}
    if status != 200:
        return {"error": text.strip() or "proxy not reachable"}
    reply = json.loads(text)
    if "id" not in reply:
        return {"error": reply.get("error", text)}
    link_id = secrets.token_hex(8)
    links[link_id] = {"host": host, "address": address, "password": password, "id": reply["id"]}
    return {"link": link_id, "seconds": reply.get("seconds", 120)}


def link_poll(link_id):
    pending = links.get(link_id)
    if not pending:
        return {"state": "unknown"}
    status, text = request(pending["address"], "/link", pending["password"], headers={"X-Octo-Link": pending["id"]})
    if status != 200:
        return {"state": "waiting", "note": text.strip()}
    reply = json.loads(text)
    if reply.get("state") == "linked":
        links.pop(link_id, None)
        device = device_info(pending["address"]) or {}
        proxy = {"id": secrets.token_hex(4), "name": proxy_name(pending["host"], reply["token"]), "host": pending["host"],
                 "token": reply["token"], "added": int(time.time()), "device_id": device.get("id"),
                 "app": device.get("app")}
        # Linking again (a new key) replaces the old entry
        proxies = [p for p in load_proxies()
                   if p["host"] != pending["host"] and not (device.get("id") and p.get("device_id") == device["id"])]
        save_proxies(proxies + [proxy])
        dismiss(device.get("id"))
        return {"state": "linked", "id": proxy["id"]}
    if reply.get("state") != "waiting":
        links.pop(link_id, None)
    return reply


# ---------------------------------------------------------------- HTTP

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def allowed(self):
        if ALLOW_ANY or self.client_address[0] in (INGRESS, "::ffff:" + INGRESS):
            return True
        self.send_error(403)
        return False

    def reply(self, data, status=200, ctype="application/json"):
        body = data if isinstance(data, bytes) else json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        length = int(self.headers.get("Content-Length", 0) or 0)
        try:
            return json.loads(self.rfile.read(length) or b"{}")
        except ValueError:
            return {}

    def do_GET(self):
        if self.allowed():
            self.guarded(self.get)

    def do_POST(self):
        if self.allowed():
            self.guarded(self.post)

    def guarded(self, handler):
        try:
            handler()
        except Exception as err:  # answer the page instead of dropping the connection
            self.reply({"error": f"{err.__class__.__name__}: {err}"}, 500)

    def get(self):
        path = self.path.split("?")[0]
        if path in ("/", "/index.html"):
            return self.reply((HERE / "www" / "index.html").read_bytes(), ctype="text/html; charset=utf-8")
        if path == "/esptool.js":
            # esptool-js 0.7.0 (Apache-2.0, www/esptool-js.LICENSE): USB flashing in the browser
            return self.reply((HERE / "www" / "esptool.js").read_bytes(), ctype="text/javascript; charset=utf-8")
        if path == "/api/catalog":
            return self.reply(catalog())
        if path == "/api/serial":
            return self.reply({"ports": serial_ports()})
        if path == "/api/state":
            with lock:
                current = dict(job)
            proxies = [{k: p.get(k) for k in ("id", "name", "host", "added", "app")} for p in load_proxies()]
            return self.reply({"proxies": proxies, "found": found_unlinked(), "toolchain": toolchain_ready(),
                               "builds": builds(), "job": current,
                               "releases": [{k: i[k] for k in ("file", "app", "version", "source", "official")}
                                            for i in release_state["images"]],
                               "release_factory": [{k: i[k] for k in ("file", "app", "version", "source", "official")}
                                                   for i in release_state["factory"]],
                               "sources": release_state["sources"],
                               "packages": release_state["packages"],
                               "release": release_state["release"], "release_error": release_state["error"],
                               "settings": load_settings(), "system": system_info()})
        if path == "/api/specs":
            return self.reply({"specs": list_specs(), "examples": examples()})
        if path.startswith("/api/specs/"):
            spec_file = SPECS / (Path(path).name + ".yaml")
            if not spec_file.is_file():
                return self.reply({"error": "unknown spec"}, 404)
            return self.reply({"text": spec_file.read_text()})
        if path.startswith("/api/download/"):
            name = Path(path).name
            file = FACTORY / name if name.endswith("-factory.bin") else FIRMWARE / name
            if not file.is_file() and name.endswith("-factory.bin"):
                try:
                    file = release_image(name, "factory") or file
                except (OSError, ValueError) as err:
                    return self.reply({"error": str(err)}, 502)
            if not file.is_file():
                return self.reply({"error": "unknown file"}, 404)
            body = file.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Disposition", f'attachment; filename="{name}"')
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path == "/api/job":
            with lock:
                current = dict(job)
            text = clean_log(JOB_LOG.read_text(errors="replace")) if JOB_LOG.exists() else ""
            current["progress"] = progress(text)
            current["log"] = text[-30000:]
            return self.reply(current)
        parts = path.strip("/").split("/")
        if len(parts) == 4 and parts[:2] == ["api", "proxy"]:
            proxy = find_proxy(parts[2])
            if not proxy:
                return self.reply({"error": "unknown proxy"}, 404)
            if parts[3] == "status":
                return self.reply(proxy_status(proxy))
            if parts[3] == "token":
                return self.reply({"token": proxy["token"]})
        if len(parts) == 3 and parts[:2] == ["api", "link"]:
            return self.reply(link_poll(parts[2]))
        self.reply({"error": "not found"}, 404)

    def post(self):
        path = self.path.split("?")[0]
        data = self.body()
        if path == "/api/link":
            host, password = str(data.get("host", "")).strip(), str(data.get("password", ""))
            if not host or not password:
                return self.reply({"error": "address and password are needed"}, 400)
            return self.reply(link_start(host, password))
        if path == "/api/specs":
            text = str(data.get("text", ""))
            spec, error = check_spec(text)
            if error:
                return self.reply({"error": error}, 400)
            if data.get("check_only"):
                return self.reply({"spec": spec})
            SPECS.mkdir(parents=True, exist_ok=True)
            (SPECS / f"{spec['slug']}.yaml").write_text(text)
            return self.reply({"spec": spec})
        if path.startswith("/api/specs/") and path.endswith("/build"):
            spec_file = SPECS / (Path(path).parent.name + ".yaml")
            if not spec_file.is_file():
                return self.reply({"error": "unknown spec"}, 404)
            if not toolchain_ready():
                return self.reply({"error": "install the toolchain first"}, 400)
            started = start_job("build", f"spec:{spec_file.stem}", lambda: run_script("build.sh", "spec", str(spec_file)))
            return self.reply({"started": started})
        if path.startswith("/api/specs/") and path.endswith("/delete"):
            spec_file = SPECS / (Path(path).parent.name + ".yaml")
            spec_file.unlink(missing_ok=True)
            return self.reply({"ok": True})
        if path == "/api/bulk":
            lines = [str(line) for line in data.get("lines", []) if allowed(str(line))]
            if not lines:
                return self.reply({"error": "nothing allowed to send"}, 400)
            results = []
            for proxy in load_proxies():
                answers = [command(proxy, line) for line in lines]
                ok = all(status == 200 for status, _ in answers)
                results.append({"id": proxy["id"], "name": proxy["name"], "ok": ok,
                                "text": " ".join(text.strip() for _, text in answers)})
            return self.reply({"results": results})
        if path == "/api/bulk/update":
            started = start_job("update", "all", update_all)
            return self.reply({"started": started})
        if path == "/api/releases/check":
            check_releases()
            return self.reply({"ok": release_state["error"] is None, "error": release_state["error"]})
        if path == "/api/flash":
            port = str(data.get("port", ""))
            if port not in [p["path"] for p in serial_ports()]:
                return self.reply({"error": "unknown USB port"}, 400)
            name = Path(str(data.get("file", ""))).name
            image = FACTORY / name
            if not image.is_file():
                try:
                    image = release_image(name, "factory")
                except (OSError, ValueError) as err:
                    return self.reply({"error": str(err)}, 502)
            if image is None or not image.is_file():
                return self.reply({"error": "unknown firmware"}, 400)
            backup = bool(data.get("backup", True))
            started = start_job("flash", name, lambda: flash_on_host(port, image, backup))
            return self.reply({"started": started})
        if path == "/api/sources/preview":
            return self.reply(preview_source(str(data.get("repo", "")).strip()))
        if path == "/api/sources/add":
            found_source = preview_source(str(data.get("repo", "")).strip())
            if "error" in found_source:
                return self.reply(found_source, 400)
            if found_source["fingerprint"] != data.get("fingerprint"):
                return self.reply({"error": "the key changed since it was shown: check the source again"}, 409)
            settings = load_settings()
            settings["sources"] = [x for x in settings.get("sources", []) if x["repo"] != found_source["repo"]] + [
                {"repo": found_source["repo"], "key": found_source["key"], "fingerprint": found_source["fingerprint"],
                 "added": int(time.time())}]
            save_settings(settings)
            check_releases()
            return self.reply({"ok": True})
        if path == "/api/sources/remove":
            settings = load_settings()
            settings["sources"] = [x for x in settings.get("sources", []) if x["repo"] != data.get("repo")]
            save_settings(settings)
            check_releases()
            return self.reply({"ok": True})
        if path == "/api/settings":
            settings = load_settings()
            if "community_ok" in data:
                settings["community_ok"] = bool(data["community_ok"])
            if "dev_mode" in data:
                settings["dev_mode"] = bool(data["dev_mode"])
            save_settings(settings)
            return self.reply(settings)
        if path == "/api/toolchain":
            started = start_job("toolchain", "Installing the toolchain", lambda: run_script("toolchain.sh"))
            return self.reply({"started": started})
        if path == "/api/build":
            if not toolchain_ready():
                return self.reply({"error": "install the toolchain first"}, 400)
            started = start_job("build", "generic", lambda: run_script("build.sh", "generic"))
            return self.reply({"started": started})
        parts = path.strip("/").split("/")
        if len(parts) == 4 and parts[:2] == ["api", "proxy"]:
            proxy = find_proxy(parts[2])
            if not proxy:
                return self.reply({"error": "unknown proxy"}, 404)
            if parts[3] == "update":
                name = Path(str(data.get("file", ""))).name
                image = FIRMWARE / name
                if not image.is_file():
                    image = release_image(name)
                if image is None or not image.is_file():
                    return self.reply({"error": "unknown firmware"}, 400)
                if proxy.get("app") and not image.name.startswith(proxy["app"] + "-"):
                    return self.reply({"error": "firmware for another application"}, 400)
                started = start_job("update", proxy["name"], lambda: update_proxy(proxy, image))
                return self.reply({"started": started})
            if parts[3] == "add_to_ha":
                started = start_job("matter", proxy["name"], lambda: add_to_ha(proxy))
                return self.reply({"started": started})
            if parts[3] == "command":
                line = str(data.get("line", "")).strip()
                if not allowed(line):
                    return self.reply({"error": "not allowed from here"}, 400)
                status, text = command(proxy, line)
                return self.reply({"status": status, "text": text})
            if parts[3] == "rename":
                name = str(data.get("name", "")).strip()[:40]
                proxies = load_proxies()
                for p in proxies:
                    if p["id"] == proxy["id"] and name:
                        p["name"] = name
                save_proxies(proxies)
                return self.reply({"ok": True})
            if parts[3] == "forget":
                save_proxies([p for p in load_proxies() if p["id"] != proxy["id"]])
                return self.reply({"ok": True})
        self.reply({"error": "not found"}, 404)


class Server(http.server.ThreadingHTTPServer):
    address_family = socket.AF_INET6
    daemon_threads = True

    def server_bind(self):
        self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
        super().server_bind()


if __name__ == "__main__":
    DATA.mkdir(parents=True, exist_ok=True)
    discovery.start()
    threading.Thread(target=releases_loop, daemon=True).start()
    print(f"Matter Builder on port {PORT}", flush=True)
    Server(("::", PORT), Handler).serve_forever()
