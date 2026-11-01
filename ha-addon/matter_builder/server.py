"""Matter Builder: the add-on's web page (Home Assistant ingress) and API.

Proxies are linked with a token of the add-on's own (POST /link on the
proxy: the page password plus a press of its BOOT button); the password is
never stored. Builds and updates run one at a time as a "job" with a log.
Only the standard library: the image is Espressif's ESP-IDF image.
"""

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
CONFIG_HOST = "octo-bridge.local"  # a proxy in configuration mode
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
SUPERVISOR_TOKEN = os.environ.get("SUPERVISOR_TOKEN", "")
FIRMWARE = DATA / "firmware"
FACTORY = FIRMWARE / "factory"  # images for a new ESP32 (USB)
JOB_LOG = DATA / "job.log"

lock = threading.Lock()
job = {"kind": None, "running": False, "ok": None, "started": 0, "title": ""}
links = {}  # link id -> request on its way (the password only lives here)
found = {}  # device id -> a Matter Builder device seen on the network
release_state = {"release": None, "images": [], "error": None, "checked": 0}


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

    log(f"== Looking for {CONFIG_HOST}")
    address = None
    end = time.time() + 150
    while time.time() < end and not address:
        time.sleep(3)
        address = mdns_resolve(CONFIG_HOST)
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

def releases_repo():
    try:
        return json.loads((DATA / "options.json").read_text()).get("releases_repo") or DEFAULT_RELEASES_REPO
    except (OSError, ValueError):
        return DEFAULT_RELEASES_REPO


def download(url, timeout=60):
    req = urllib.request.Request(url, headers={"Accept": "application/octet-stream", "User-Agent": "matter-builder"})
    with urllib.request.urlopen(req, timeout=timeout) as reply:
        return reply.read()


def signature_ok(manifest, signature):
    """ECDSA P-256 over SHA-256, checked with openssl against the add-on's key."""
    with tempfile.TemporaryDirectory() as tmp:
        (Path(tmp) / "m").write_bytes(manifest)
        (Path(tmp) / "s").write_bytes(signature)
        result = subprocess.run(["openssl", "dgst", "-sha256", "-verify", str(RELEASE_KEY), "-signature",
                                 str(Path(tmp) / "s"), str(Path(tmp) / "m")], capture_output=True)
    return result.returncode == 0


def check_releases():
    """The latest release: only kept when its manifest's signature is valid."""
    repo = releases_repo()
    try:
        info = json.loads(download(f"https://api.github.com/repos/{repo}/releases/latest", 20))
        assets = {a["name"]: a["browser_download_url"] for a in info.get("assets", [])}
        manifest = download(assets["manifest.json"])
        signature = download(assets["manifest.json.sig"])
    except (OSError, ValueError, KeyError) as err:
        release_state.update(images=[], error=f"{repo}: {err}", checked=int(time.time()))
        return
    if not signature_ok(manifest, signature):
        release_state.update(images=[], error="signature does not match: release ignored", checked=int(time.time()))
        return
    data = json.loads(manifest)
    images = [dict(img, url=assets.get(img["file"])) for img in data.get("images", []) if assets.get(img["file"])]
    release_state.update(release=data.get("release"), images=images, error=None, checked=int(time.time()))


def releases_loop():
    while True:
        check_releases()
        time.sleep(6 * 3600)


def release_image(name):
    """A release image on disk, downloaded when needed and checked against the signed hash."""
    image = next((img for img in release_state["images"] if img["file"] == name), None)
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
        if not linked(device["id"], host, address):
            notify(device)


discovery = Discovery()


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
                    "firmware": sorted((p.name for p in FIRMWARE.glob(f"{project}-*.bin")), key=version_key)
                    if project else [], "factory": sorted(factory, key=lambda n: version_key(n.removesuffix("-factory.bin")))})
    return out


def examples():
    folder = SRC / "examples"
    return {p.stem: p.read_text() for p in sorted(folder.glob("*.yaml"))} if folder.exists() else {}


# ---------------------------------------------------------------- management

# What the management pages may send to a proxy: its connections, name,
# language and Wi-Fi; nothing that changes access or the firmware
ALLOWED_COMMANDS = ("io ", "name ", "language ", "wifi ssid ", "wifi password ")


def allowed(line):
    return line in ("io json", "json", "language") or line.startswith(ALLOWED_COMMANDS)


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
        if path == "/api/state":
            with lock:
                current = dict(job)
            proxies = [{k: p.get(k) for k in ("id", "name", "host", "added", "app")} for p in load_proxies()]
            return self.reply({"proxies": proxies, "found": found_unlinked(), "toolchain": toolchain_ready(),
                               "builds": builds(), "job": current,
                               "releases": [{k: i[k] for k in ("file", "app", "version")} for i in release_state["images"]],
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
        if path == "/api/settings":
            settings = load_settings()
            if "dev_mode" in data:
                settings["dev_mode"] = bool(data["dev_mode"])
            save_settings(settings)
            return self.reply(settings)
        if path == "/api/toolchain":
            started = start_job("toolchain", "Toolchain installeren", lambda: run_script("toolchain.sh"))
            return self.reply({"started": started})
        if path == "/api/build":
            if not toolchain_ready():
                return self.reply({"error": "install the toolchain first"}, 400)
            app = "generic" if data.get("app") == "generic" else "bridge"
            started = start_job("build", app, lambda: run_script("build.sh", app))
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
                started = start_job("update", f"{proxy['name']} bijwerken", lambda: update_proxy(proxy, image))
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
