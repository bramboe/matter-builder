"""Matter Builder: the add-on's web page (Home Assistant ingress) and API.

Also runs on its own, without Home Assistant (MB_STANDALONE=1): then it has
a login of its own (a password chosen on the page, sessions in a cookie)
instead of Home Assistant's.

Proxies are linked with a token of the add-on's own (POST /link on the
proxy: the page password plus a press of its BOOT button); the password is
never stored. Builds and updates run one at a time as a "job" with a log.
Only the standard library: the image is Espressif's ESP-IDF image.
"""

import base64
import gzip
import hmac
import hashlib
import http.client
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
import urllib.parse
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
# On its own, without Home Assistant: anyone on the network may connect, so
# everything but the login needs a session (see "standalone login" below)
STANDALONE = os.environ.get("MB_STANDALONE") == "1"
COMMIT = os.environ.get("ESP_MATTER_COMMIT", "")
CONFIG_HOST = "octo-bridge.local"  # configuration mode of firmware that does not say where
PROGRAM_NAME = "Matter Builder" if STANDALONE else "Home Assistant"

PROXIES = DATA / "proxies.json"
PAGES = DATA / "pages"  # each device's own settings page, kept with its ETag
SETTINGS = DATA / "settings.json"
NOTIFIED = DATA / "notified.json"
RELEASES = DATA / "releases"
RELEASE_CACHE = RELEASES / "verified.json"  # the merged, signature-checked list
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
BACKUPS = Path(os.environ.get("MB_BACKUPS") or (DATA / "backups" if STANDALONE else "/share/matter-builder/backups"))
AUTH = DATA / "auth.json"

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
        return {"dev_mode": False, "night_hour": 3, **json.loads(SETTINGS.read_text())}
    except (OSError, ValueError):
        return {"dev_mode": False, "night_hour": 3}


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
    name = re.search(r"settings page: http://([A-Za-z0-9-]+\.local)/", system)
    if name and proxy.get("local") != name.group(1):
        remember(proxy, local=name.group(1))
    # The page is served from what is kept here (quick, also over Thread):
    # fetched anew for each firmware, so it always matches what runs
    running_version = (system.split("\n")[0].split(": ")[-1].split(" ") + [""])[1]
    if not (PAGES / f"{proxy['id']}.html").exists() or proxy.get("page_version") != running_version:
        remember(proxy, page_version=running_version)
        prefetch_page(proxy)
    return {"ok": True, "system": system, "beds": beds, "io": state.get("io")}


# One missed answer over Thread is common; "offline" only after two in a row,
# and then with since when (like ESPHome's "Offline 2d")
last_status = {}  # id -> (last good status, misses since)


def posix_zone():
    """Home Assistant's time zone (TZ, set by the Supervisor) as a POSIX TZ string,
    for a device's local time: the last line of its zoneinfo file."""
    name = os.environ.get("TZ", "")
    files = ([Path("/usr/share/zoneinfo") / name] if name and ".." not in name else []) + [Path("/etc/localtime")]
    for path in files:
        try:
            data = path.read_bytes()
            footer = data.rstrip(b"\n").rsplit(b"\n", 1)[-1].decode() if data.startswith(b"TZif") else ""
            if footer and len(footer) < 64:
                return footer
        except (OSError, UnicodeDecodeError):
            continue
    offset = int(-(time.altzone if time.localtime().tm_isdst > 0 else time.timezone))  # without DST rules
    hours, minutes = divmod(abs(offset) // 60, 60)
    sign = "-" if offset > 0 else "+"  # POSIX counts west as positive
    return f"LT{sign}{hours}" + (f":{minutes:02d}" if minutes else "")


clock_checked = {}  # id -> when the device's clock was last looked at


def keep_clock(proxy):
    """A device with rules keeps its clock from Home Assistant: SNTP over Thread
    needs NAT64 at the border router, which not every home has."""
    now = time.time()
    if now - clock_checked.get(proxy["id"], 0) < 3600:
        return
    clock_checked[proxy["id"]] = now
    status, text = command(proxy, "time", timeout=10)
    try:
        clock = json.loads(text) if status == 200 else None
    except ValueError:
        clock = None  # firmware without a clock
    if not isinstance(clock, dict):
        return
    if not clock.get("set") or abs(int(clock.get("unix", 0)) - now) > 30:
        command(proxy, f"time set {int(time.time())}", timeout=10)
    zone = posix_zone()
    if clock.get("zone") != zone and " " not in zone:
        command(proxy, f"time zone {zone}", timeout=10)


def checked_status(proxy):
    status = proxy_status(proxy)
    good, misses = last_status.get(proxy["id"], (None, 0))
    now = int(time.time())
    if status["ok"]:
        status = dict(status, last_seen=now)
        last_status[proxy["id"]] = (status, 0)
        if status.get("io") is not None:  # a generic device: rules may use the clock
            threading.Thread(target=keep_clock, args=(proxy,), daemon=True).start()
        if now - (proxy.get("last_seen") or 0) > 600:  # on disk now and then, not every poll
            remember(proxy, last_seen=now)
        return status
    last_status[proxy["id"]] = (good, misses + 1)
    if good is not None and misses == 0 and not status.get("auth"):
        return dict(good, stale=True)
    seen = proxy.get("last_seen")
    if good is not None:
        seen = max(seen or 0, good.get("last_seen", 0)) or seen
    return dict(status, last_seen=seen)


LOG_HEAD = re.compile(r"log (\d+) ([01]) (\d+)\n")


def device_log(proxy, start, boot):
    """New log lines of a device since byte `start` (its "log" command, 4 KB a
    time). A different boot id means it restarted: then from the beginning."""
    status, text = command(proxy, f"log {start}", timeout=20)
    head = LOG_HEAD.match(text) if status == 200 else None
    if status == 200 and head is None:
        return {"unsupported": True}  # firmware from before the log
    if head is None:
        return {"error": text.strip() or f"HTTP {status}"}
    restarted = bool(boot) and head.group(3) != boot
    if restarted and start:
        status, text = command(proxy, "log 0", timeout=20)
        head = LOG_HEAD.match(text) if status == 200 else None
        if head is None:
            return {"error": text.strip() or f"HTTP {status}"}
    return {"next": int(head.group(1)), "lost": head.group(2) == "1", "boot": head.group(3),
            "restarted": restarted, "text": text[head.end():]}


def remember(proxy, **fields):
    proxies = load_proxies()
    for p in proxies:
        if p["id"] == proxy["id"]:
            p.update(fields)
    save_proxies(proxies)
    proxy.update(fields)


# ---------------------------------------------------------------- the device's page, served here

# Over Thread the page itself (20-70 KB) takes half a minute; what it asks
# the device afterwards is small. So the page comes from here, fetched once
# per firmware (ETag), and its requests go through unchanged: the device
# password stays the user's, the add-on adds no key of its own.
RELAY_GET = {"auth", "info", "spec", "link"}
RELAY_POST = {"cmd", "auth", "ota", "link"}
page_fetching = set()


def device_request(host, method, path, body=None, headers=None, timeout=60):
    """(status, headers, body) from the device. The body goes in pieces: a
    timeout per piece, since a whole image over Thread takes half an hour."""
    conn = http.client.HTTPConnection(url_host(host), timeout=timeout)
    try:
        conn.putrequest(method, path, skip_accept_encoding=True)
        for key, value in (headers or {}).items():
            conn.putheader(key, value)
        if body is not None:
            conn.putheader("Content-Length", str(len(body)))
        conn.endheaders()
        for start in range(0, len(body or b""), 4096):
            conn.send(body[start:start + 4096])
        reply = conn.getresponse()
        return reply.status, dict(reply.getheaders()), reply.read()
    finally:
        conn.close()


def device_page(proxy):
    """The device's page as it serves it, or None. Asked with the kept ETag:
    while the firmware is the same the device answers 304 in about a second."""
    html, tag = PAGES / f"{proxy['id']}.html", PAGES / f"{proxy['id']}.etag"
    headers = {"Accept-Encoding": "gzip"}
    if html.exists() and tag.exists():
        headers["If-None-Match"] = tag.read_text()
    try:
        status, reply_headers, body = device_request(resolve(proxy["host"]) or proxy["host"], "GET", "/", None, headers)
    except (OSError, http.client.HTTPException):
        return html.read_bytes() if html.exists() else None  # its requests will say it is away
    if status == 304 and html.exists():
        return html.read_bytes()
    if status != 200:
        return None
    if {k.lower(): v for k, v in reply_headers.items()}.get("content-encoding") == "gzip":
        body = gzip.decompress(body)
    PAGES.mkdir(parents=True, exist_ok=True)
    fresh = html.with_suffix(".new")
    fresh.write_bytes(body)
    fresh.replace(html)  # whole: it may be served while it is written
    etag = {k.lower(): v for k, v in reply_headers.items()}.get("etag")
    if etag:
        tag.write_text(etag)
    elif tag.exists():
        tag.unlink()
    return body


def prefetch_page(proxy):
    if proxy["id"] in page_fetching:
        return
    page_fetching.add(proxy["id"])

    def fetch():
        try:
            device_page(proxy)
        finally:
            page_fetching.discard(proxy["id"])
    threading.Thread(target=fetch, daemon=True).start()


def portal_page(html, proxy):
    """The page with relative request paths (older firmware asks '/cmd'), its
    password kept per device, the device's own address for Wi-Fi, and a way back."""
    text = html.decode(errors="replace")
    for old, new in (("fetch('/cmd'", "fetch('cmd'"), ("fetch('/auth'", "fetch('auth'"),
                     ("'POST', '/ota'", "'POST', 'ota'")):
        text = text.replace(old, new)
    home = json.dumps(proxy.get("local") or proxy["host"])
    shim = ("<script>window.mbHome=" + home + ";(function(){var id=" + json.dumps(proxy["id"]) +
            ",P=Storage.prototype,g=P.getItem,s=P.setItem,r=P.removeItem,"
            "k=function(n){return /Token$/.test(n)?n+':'+id:n};"
            "P.getItem=function(n){return g.call(this,k(n))};"
            "P.setItem=function(n,v){return s.call(this,k(n),v)};"
            "P.removeItem=function(n){return r.call(this,k(n))}})();</script>")
    # The way back: newer pages put an arrow in their own header (as Home
    # Assistant's pages do) when told where to; older ones get a link above
    if 'id="backBtn"' in text:
        shim = shim.replace("<script>", '<script>window.mbBack="../../";', 1)
    else:
        back = ('<a href="../../" style="display:block;padding:10px 16px;font:500 14px system-ui,sans-serif;'
                'color:#009ac7;text-decoration:none">&larr; Matter Builder</a>')
        text = re.sub(r"<body[^>]*>", lambda m: m.group(0) + back, text, count=1)
    text = re.sub(r"<head[^>]*>", lambda m: m.group(0) + shim, text, count=1)
    return text.encode()


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
    """The last "[n/m]" step of the build (ninja), and the phase it is in. A
    new phase starts without progress: "waiting" after an upload is not 100 %."""
    phase, done, total = "", 0, 0
    for line in text.split("\n"):
        if line.startswith("== "):
            phase, done, total = line[3:].strip(), 0, 0
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


def run_script(name, *args, env=None):
    with JOB_LOG.open("a") as out:
        result = subprocess.run(["bash", str(HERE / name), *args], stdout=out, stderr=subprocess.STDOUT,
                                env={**os.environ, **(env or {})})
    log(f"== exit {result.returncode}")
    return result.returncode == 0


def toolchain_ready():
    """esp-matter at this version, and ESP-IDF where idf_env.sh looks for it:
    in Espressif's image, or downloaded into /data (an older add-on's
    toolchain had ESP-IDF in its image and must be installed once more)."""
    idf = Path("/opt/esp/idf/export.sh") if Path("/opt/esp/idf/export.sh").exists() else DATA / "toolchain" / "esp-idf" / "export.sh"
    return bool(COMMIT) and (DATA / "toolchain" / f"ready-{COMMIT}").exists() and idf.exists()


def builds():
    out = []
    for path in sorted(FIRMWARE.glob("*.bin"), key=lambda p: p.stat().st_mtime, reverse=True):
        stat = path.stat()
        out.append({"file": path.name, "size": stat.st_size, "date": int(stat.st_mtime)})
    return out


# ---------------------------------------------------------------- updates fetched over Wi-Fi
# The device downloads the image itself, on Wi-Fi, from Home Assistant's own
# address: that works across subnets and VLANs, where finding the device in
# configuration mode needs mDNS (one subnet). A token per update, valid a
# quarter of an hour, is the only way to the file; the device checks the
# SHA-256 the add-on sends it over Thread.
FETCH_PORT = int(os.environ.get("MB_FETCH_PORT", "8098"))
fetch_tokens = {}  # token -> (path, valid until)


def lan_ip():
    """This machine's address on the home network (the add-on uses the host network)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect(("192.0.2.1", 80))  # no packet is sent: only the route is looked up
        return s.getsockname()[0]


class FetchHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        token = self.path.removeprefix("/fw/")
        path, until = fetch_tokens.get(token, (None, 0))
        if path is None or time.time() > until or not Path(path).is_file():
            self.send_error(404)
            return
        body = Path(path).read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def running_line(text):
    return (text.strip().splitlines() or [""])[0]


def restarted_since(proxy, moment):
    """Whether the device started after `moment`, by its uptime ("system
    json"); firmware without it counts as restarted after a minute."""
    status, text = command(proxy, "system json", timeout=10)
    try:
        uptime = json.loads(text).get("uptime") if status == 200 else None
    except ValueError:
        uptime = None
    if not isinstance(uptime, (int, float)):
        return status == 200 and time.time() - moment > 60
    return uptime < time.time() - moment - 2


def fetch_over_wifi(proxy, image):
    """True when the device fetched and booted the image; False to go over Thread."""
    data = image.read_bytes()
    sha = hashlib.sha256(data).hexdigest()
    token = secrets.token_urlsafe(18)
    fetch_tokens[token] = (str(image), time.time() + 900)
    try:
        url = f"http://{lan_ip()}:{FETCH_PORT}/fw/{token}"
    except OSError as err:
        log(f"   no address for Home Assistant on the home network ({err})")
        return False
    # A release image also has its download on GitHub: the device tries that
    # when its Wi-Fi cannot reach Home Assistant (an IoT network often cannot)
    release = next((i for i in release_state["images"] if i["file"] == image.name and i.get("sha256") == sha), None)
    second = f" {release['url']}" if release and str(release.get("url", "")).startswith("https://") else ""
    _, before = command(proxy, "system", timeout=10)
    log("== Fetching over Wi-Fi: the device downloads the firmware itself (a minute or so)")
    sent = time.time()
    status, text = command(proxy, f"wifi fetch {url} {sha}{second}")
    if status == 200 and not text.startswith("restarting"):
        log(f"   {text.strip()}")
        return False
    log("   " + (text.strip() or "(it restarts on Wi-Fi)"))
    time.sleep(20)
    back, now = False, ""
    end = time.time() + 300  # Wi-Fi join, download, restart, back on Thread
    while time.time() < end:
        status, now = command(proxy, "system", timeout=10)
        # Back means: it answers and it has restarted since the command. An
        # answer from before its restart (it can take a while to get to it)
        # says nothing yet
        if status == 200 and restarted_since(proxy, sent):
            back = True
            break
        time.sleep(5)
    fetch_tokens.pop(token, None)
    if not back:
        log("!! The device did not come back on Thread yet; boot_guard returns to the previous firmware if it cannot get online")
        return False
    _, wifi = command(proxy, "wifi json", timeout=10)
    try:
        result = json.loads(wifi).get("last_result", "")
    except ValueError:
        result = ""
    switched = running_line(now).split(":")[1:2] != running_line(before).split(":")[1:2]  # "running: app1 ..." slot
    # The image's version running from the other slot is proof enough, also
    # when the device's own note of the result got lost
    version = image.stem.rsplit("-", 1)[-1]
    runs_it = f" {version} " in running_line(now) + " " and f" {version} " not in running_line(before) + " "
    if switched and (result.startswith("fetch ok") or runs_it):
        if "internet" in result:
            log("   fetched from GitHub: this Wi-Fi does not reach Home Assistant")
        log(running_line(now))
        log("== Done")
        return True
    why = {"password": "Wi-Fi refused the password", "notfound": "Wi-Fi network not found",
           "noconnect": "no Wi-Fi connection"}.get(result.split(" ")[0], result or "no result")
    log(f"   over Wi-Fi it did not work: {why}")
    return False


def update_proxy(proxy, image):
    """The image over Wi-Fi: the device fetching it (fast), else the device in
    configuration mode reached by the add-on, else over Thread (slow)."""
    log(f"== Update {proxy['name']} with {image.name}")
    status, text = command(proxy, "system")
    if status != 200:
        log(f"!! Proxy not reachable: {text.strip()}")
        return False
    log(text.strip().splitlines()[0])

    # A Wi-Fi device (the classic ESP32, or a C6 set up on Wi-Fi) is on Wi-Fi
    # already: straight to /ota, no detour
    info = device_info(resolve(proxy["host"]) or proxy["host"]) or {}
    # Firmware 0.1.10 / 1.0.20 on report their update slot: a larger image is
    # refused here instead of after the whole upload
    size = image.stat().st_size
    if info.get("slot") and size > info["slot"]:
        log(f"!! {image.name} ({size // 1024} KB) is larger than this device's firmware slot "
            f"({info['slot'] // 1024} KB): this image is not for this board")
        return False
    if info.get("network") == "wifi":
        status, text = upload_over_thread(proxy, image, wifi=True)
        log(text.strip() or f"HTTP {status}")
        return status == 200 and wait_until_back(proxy, image)

    status, wifi = command(proxy, "wifi json")
    try:
        can_fetch = status == 200 and json.loads(wifi).get("fetch") is True
    except ValueError:
        can_fetch = False
    if can_fetch:
        if fetch_over_wifi(proxy, image):
            return True
        log("== Over Thread instead (slow, about half an hour)")
        status, text = upload_over_thread(proxy, image)
        log(text.strip() or f"HTTP {status}")
        return status == 200 and wait_until_back(proxy, image)

    if proxy.get("ota_over_thread"):
        # Learned on an earlier update: Wi-Fi does not reach this device
        status, text = upload_over_thread(proxy, image)
        log(text.strip() or f"HTTP {status}")
        return status == 200 and wait_until_back(proxy, image)

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
    if address:
        log(f"   found at {address}")
        log("== Uploading the firmware")
        time.sleep(2)
        status, text = request(address, "/ota", proxy["token"], image.read_bytes(), timeout=300)
    else:
        # Home Assistant cannot see the device on Wi-Fi (another network or
        # VLAN, or mDNS blocked): it returns to Thread after its idle time,
        # and the image goes over Thread instead, slower but on the same route
        log("   not found on Wi-Fi from here (another network?)")
        remember_ota_over_thread(proxy)  # next time straight over Thread
        log("== Waiting for it to return to Thread (up to 16 minutes)")
        end = time.time() + 16 * 60
        back = False
        while time.time() < end and not back:
            time.sleep(10)
            back = command(proxy, "system", timeout=8)[0] == 200
        if not back:
            log("!! It did not come back on Thread; try again later")
            return False
        status, text = upload_over_thread(proxy, image)
    log(text.strip() or f"HTTP {status}")
    return status == 200 and wait_until_back(proxy, image)


THREAD_RATE = 1200  # bytes per second, measured: Thread is slow for a whole image


def upload_over_thread(proxy, image, wifi=False):
    """The image in pieces, with progress; a timeout per piece, not for the whole.
    The same upload serves a Wi-Fi device, where it takes seconds."""
    data = image.read_bytes()
    if wifi:
        log(f"== Uploading the firmware over Wi-Fi ({len(data) // 1024} KB)")
    else:
        minutes = max(1, round(len(data) / THREAD_RATE / 60))
        log(f"== Uploading the firmware over Thread (about {minutes} minutes for {len(data) // 1024} KB)")
    host = resolve(proxy["host"]) or proxy["host"]
    conn = http.client.HTTPConnection(url_host(host), timeout=120)  # brackets: http.client strips them
    try:
        conn.putrequest("POST", "/ota")
        conn.putheader("X-Octo-Token", proxy["token"])
        conn.putheader("Content-Length", str(len(data)))
        conn.endheaders()
        step, shown = 4096, -1
        for start in range(0, len(data), step):
            conn.send(data[start:start + step])
            percent = 100 * (start + step) // len(data) if start + step < len(data) else 100
            if percent // 5 != shown // 5:
                shown = percent
                log(f"   {min(start + step, len(data)) // 1024} of {len(data) // 1024} KB ({percent} %)")
        reply = conn.getresponse()
        return reply.status, reply.read().decode(errors="replace")
    except (OSError, http.client.HTTPException) as err:
        return 0, f"upload stopped: {err}"
    finally:
        conn.close()


def remember_ota_over_thread(proxy):
    proxies = load_proxies()
    for p in proxies:
        if p["id"] == proxy["id"]:
            p["ota_over_thread"] = True
    save_proxies(proxies)


def wait_until_back(proxy, image=None):
    """The device on its own address again, running the new firmware: one that
    comes back on the old version did not start the new one (its boot guard
    went back), which is a failed update, not a done one."""
    want = image.name.removesuffix(".bin").rpartition("-")[2] if image is not None else None
    log("== Waiting for the proxy on its own address")
    time.sleep(15)  # it answers with the old firmware until it restarts
    end = time.time() + 240
    running = None
    while time.time() < end:
        time.sleep(5)
        status, text = command(proxy, "system", timeout=10)
        if status != 200:
            continue
        running = text.strip().splitlines()[0]
        if want is None or f" {want} " in running:
            log(running)
            log("== Done")
            return True
    if running:
        log(running)
        log(f"!! The device went back to its previous firmware: {image.name} did not start on it")
        return False
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


def download_asset(url, api_url=None, timeout=60):
    """A release file; when GitHub's download link fails (it gives a 5xx now and
    then), the same file through the API. Integrity comes from the signature."""
    try:
        return download(url, timeout)
    except (OSError, ValueError):
        if not api_url:
            raise
        return download(api_url, timeout)


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
        api_assets = {a["name"]: a["url"] for a in info.get("assets", []) if a.get("url")}
        manifest = download_asset(assets["manifest.json"], api_assets.get("manifest.json"))
        signature = download_asset(assets["manifest.json.sig"], api_assets.get("manifest.json.sig"))
    except (OSError, ValueError, KeyError) as err:
        result["error"] = f"{src['repo']}: {err}"
        return result
    if not signature_ok(manifest, signature, src["key"].encode()):
        result["error"] = f"{src['repo']}: signature does not match: release ignored"
        return result
    result.update(data=json.loads(manifest), assets=assets, api_assets=api_assets)
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
                    merged[kind].append(dict(img, url=res["assets"][img["file"]], api_url=res.get("api_assets", {}).get(img["file"]),
                                             source=res["repo"], official=res["official"]))
        for pkg in res["data"].get("packages", []):
            merged["packages"].append(dict(pkg, source=res["repo"], official=res["official"]))
    own = results[0]
    sources_state = [{k: r[k] for k in ("repo", "official", "release", "error")} for r in results]
    if not merged["images"] and not merged["factory"] and release_state["images"] + release_state["factory"]:
        # GitHub out of reach: keep what was verified before, so images on disk stay usable
        release_state.update(error=own["error"], sources=sources_state)
        return
    release_state.update(release=own["release"], error=own["error"], checked=int(time.time()),
                         sources=sources_state, **merged)
    if merged["images"] or merged["factory"]:
        try:
            RELEASES.mkdir(parents=True, exist_ok=True)
            RELEASE_CACHE.write_text(json.dumps(release_state))
        except OSError:
            pass


def load_release_cache():
    """The last verified release list, for a start while GitHub is out of reach."""
    try:
        release_state.update(json.loads(RELEASE_CACHE.read_text()))
    except (OSError, ValueError):
        pass


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
    load_release_cache()
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
        data = download_asset(image["url"], image.get("api_url"), 300)
        if hashlib.sha256(data).hexdigest() != image["sha256"]:
            raise ValueError("downloaded firmware does not match the signed hash")
        path.write_bytes(data)
    return path


# ---------------------------------------------------------------- Home Assistant's Matter server

# The Matter Server add-on listens on the host network; so does this add-on
MATTER_SERVER = ("127.0.0.1", 5580)


def ws_connect(address, path, timeout, who, host=None):
    """A WebSocket connection: (send, receive, socket). send(opcode, bytes),
    receive() gives the next JSON message. A small client of its own: the
    add-on has no WebSocket library."""
    sock = socket.create_connection(address, timeout=10)
    sock.settimeout(timeout)
    try:
        key = base64.b64encode(os.urandom(16)).decode()
        sock.sendall((f"GET {path} HTTP/1.1\r\nHost: {host or f'{address[0]}:{address[1]}'}\r\nUpgrade: websocket\r\n"
                      f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = sock.recv(1024)
            if not chunk:
                raise OSError(f"{who} closed the connection")
            head += chunk
        status_line = head.split(b"\r\n")[0].decode(errors="replace")
        if " 101 " not in status_line:
            raise OSError(f"{who} refused: {status_line}")
    except Exception:
        sock.close()
        raise
    rest = head.split(b"\r\n\r\n", 1)[1]

    def read(n):
        nonlocal rest
        while len(rest) < n:
            chunk = sock.recv(65536)
            if not chunk:
                raise OSError(f"{who} closed the connection")
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
                raise OSError(f"{who} closed the connection")
            message += payload
            if first & 0x80:
                return json.loads(message)

    return send, receive, sock


def matter_server(command, args, timeout=300):
    """One command to the Matter server's WebSocket API, and its answer."""
    send, receive, sock = ws_connect(MATTER_SERVER, "/ws", timeout, "Matter server")
    try:
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


# ---------------------------------------------------------------- names in Home Assistant
# Parts that are grouped into one device (a bed's head, legs and light; the
# connections of a group) carry their own name as the fixed label
# "ha_entitylabel". Home Assistant only appends that to secondary entities
# ("Identify (Head)"): for a bridge, the main entity of each part (the cover,
# the light) is named after the device alone, so two covers are both "Sam"
# (cover.sam, cover.sam_2). After adding a device, the add-on gives those
# main entities their part's name ("Sam Head", cover.sam_head), in Home
# Assistant's entity registry, as a user would by hand. Entities that already
# have a name of their own are left alone.

def ha_core_session():
    """Home Assistant's WebSocket API through the Supervisor: (call, socket)."""
    send, receive, sock = ws_connect(("supervisor", 80), "/core/websocket", 30, "Home Assistant", host="supervisor")
    try:
        receive()  # auth_required
        send(1, json.dumps({"type": "auth", "access_token": SUPERVISOR_TOKEN}).encode())
        if receive().get("type") != "auth_ok":
            raise OSError("Home Assistant refused the add-on's token")
    except Exception:
        sock.close()
        raise
    counter = 0

    def call(kind, **fields):
        nonlocal counter
        counter += 1
        send(1, json.dumps({"id": counter, "type": kind, **fields}).encode())
        while True:
            reply = receive()
            if reply.get("id") == counter and reply.get("type") == "result":
                if not reply.get("success"):
                    raise OSError(str((reply.get("error") or {}).get("message") or "Home Assistant refused"))
                return reply.get("result")

    return call, sock


def slug(text):
    return re.sub(r"[^a-z0-9]+", "_", str(text).lower()).strip("_")


def part_labels(node):
    """endpoint -> its "ha_entitylabel", from a node of the Matter server
    (FixedLabel's LabelList: cluster 64, attribute 0)."""
    labels = {}
    for key, value in (node.get("attributes") or {}).items():
        parts = key.split("/")
        if len(parts) != 3 or parts[1:] != ["64", "0"] or not isinstance(value, list):
            continue
        for item in value:
            if not isinstance(item, dict):
                continue
            name, text = item.get("0", item.get("label")), item.get("1", item.get("value"))
            if str(name).lower() == "ha_entitylabel" and text:
                labels[int(parts[0])] = str(text)
    return labels


def name_parts_in_ha(node_id, wait=90):
    """Names the main entity of every labelled part of a Matter node; how many."""
    if not SUPERVISOR_TOKEN:
        return 0
    labels = part_labels(matter_server("get_node", {"node_id": node_id}, timeout=30))
    if not labels:
        return 0
    marker = re.compile(rf"-{node_id:016X}-\d+-(\d+)-")
    call, sock = ha_core_session()
    try:
        # Home Assistant makes the entities a little after the device was added
        deadline, mine, before = time.time() + wait, [], -1
        while time.time() < deadline:
            entities = call("config/entity_registry/list")
            mine = [e for e in entities if e.get("platform") == "matter" and marker.search(e.get("unique_id") or "")]
            if mine and len(mine) == before:
                break  # all there: the same number twice
            before = len(mine)
            time.sleep(5)
        taken = {e["entity_id"] for e in entities}
        devices = {d["id"]: d for d in call("config/device_registry/list")}
        named = 0
        for entry in mine:
            endpoint = int(marker.search(entry["unique_id"]).group(1))
            if endpoint not in labels:
                continue
            if "original_name" not in entry:
                entry = call("config/entity_registry/get", entity_id=entry["entity_id"])
            # the main entity of the part: named after the device (no name of its own), not renamed by the user
            if entry.get("name") or entry.get("original_name"):
                continue
            label = labels[endpoint]
            device = devices.get(entry.get("device_id")) or {}
            base, part = slug(device.get("name_by_user") or device.get("name") or ""), slug(label)
            domain = entry["entity_id"].split(".")[0]
            wanted = f"{domain}.{base}_{part}" if base and part else ""
            change = {"entity_id": entry["entity_id"], "name": label}
            if wanted and wanted != entry["entity_id"] and wanted not in taken:
                change["new_entity_id"] = wanted
                taken.add(wanted)
            call("config/entity_registry/update", **change)
            log(f"   named in Home Assistant: {change.get('new_entity_id', entry['entity_id'])} ({label})")
            named += 1
        return named
    finally:
        sock.close()


HA_VENDOR = 0x134B  # Nabu Casa: the vendor of Home Assistant's Matter fabric


def ha_knows_device(proxy):
    """Whether Home Assistant's Matter integration has this device: True,
    False, or None when that cannot be told. A paired Matter device announces
    itself on the network as "<fabric>-<node>" for every controller it
    belongs to (discovery keeps those per host); Home Assistant's devices
    carry the same pair in their identifiers."""
    if not SUPERVISOR_TOKEN:
        return None
    with lock:
        hosts = {d.get("host") for d in found.values() if d.get("id") == proxy.get("device_id")} | {proxy.get("local")}
        announced = set().union(*[discovery.nodes.get(h, set()) for h in hosts if h]) if hosts else set()
    if not announced:
        return None  # not seen announcing itself (yet): not sure
    try:
        call, sock = ha_core_session()
    except OSError:
        return None
    try:
        known = set()
        for device in call("config/device_registry/list"):
            for identifier in device.get("identifiers", []):
                match = re.match(r"^deviceid_([0-9A-F]{16}-[0-9A-F]{16})", str(identifier[1]) if len(identifier) > 1 and identifier[0] == "matter" else "")
                if match:
                    known.add(match.group(1))
        if not known:
            return None  # no Matter devices at all: its fabric is not known here
        fabrics = {pair.split("-")[0] for pair in known}
        mine = {pair for pair in announced if pair.split("-")[0] in fabrics}
        # Announcing itself on Home Assistant's fabric under a node Home Assistant
        # does not have: Home Assistant forgot it
        return bool(mine & known) if mine else None
    except (OSError, ValueError, KeyError, IndexError):
        return None
    finally:
        sock.close()


def drop_stale_ha_fabric(proxy):
    """Removes Home Assistant's fabric from the device ("matter fabrics" /
    "matter remove"), when Home Assistant no longer has the device."""
    status, text = command(proxy, "matter fabrics")
    stale = [int(m.group(1)) for m in re.finditer(r"fabric (\d+): vendor 0x([0-9A-F]+)", text or "")
             if int(m.group(2), 16) == HA_VENDOR] if status == 200 else []
    if not stale:
        return False
    for index in stale:
        _, reply = command(proxy, f"matter remove {index}", timeout=30)
        log(f"== Home Assistant had forgotten the device: its old key removed ({reply.strip()})")
    return True


def add_to_ha(proxy):
    """A device into Home Assistant's Matter fabric: a pairing window on the
    device, then the Matter server commissions it over the network."""
    log(f"== Adding {proxy['name']} to Home Assistant")
    # A paired device on newer firmware gives a one-time code ("matter share"):
    # its own pairing code stays on the device. Otherwise its own code, in a
    # pairing window ("matter open")
    status, text = command(proxy, "matter share")
    try:
        code = json.loads(text)["qr"] if status == 200 else None
    except (ValueError, KeyError, TypeError):
        code = None
    if code:
        log("   a one-time code, valid for 10 minutes")
    else:
        status, text = command(proxy, "matter open")
        log(text.strip())
        if status != 200:
            return False
        status, text = command(proxy, "matter")
        # JSON from the generic device, text ("QR code: MT:...") from the bed bridge
        try:
            code = json.loads(text)["qr"]
        except (ValueError, KeyError, TypeError):
            match = re.search(r"MT:[0-9A-Z.\-]+", text or "")
            code = match.group(0) if match else None
    if not code:
        log("!! The device gave no pairing code")
        return False
    log("== Commissioning over the network (a minute or two on Thread)")
    try:
        node = matter_server("commission_with_code", {"code": code, "network_only": True})
    except (OSError, ValueError) as err:
        already = "already commissioned into this fabric" in str(err)
        known = ha_knows_device(proxy) if already else None
        for _ in range(6):  # it announces itself anew after opening the window: give that a moment
            if not already or known is not None:
                break
            time.sleep(4)
            known = ha_knows_device(proxy)
        if already and known is not False:
            log("== Already in Home Assistant")
            return True
        # Home Assistant forgot the device (removed while it was offline), but
        # the device still holds Home Assistant's old key: that key goes, then
        # once more. Only when Home Assistant has no device at its address, so
        # a device Home Assistant knows stays as it is.
        if already:
            log("== The device still has Home Assistant's key, but Home Assistant has no such device")
        if not (already or re.search(r"conflict|fabric.*exist|already.*fabric", str(err), re.I)) or not drop_stale_ha_fabric(proxy):
            log(f"!! {err}")
            return False
        command(proxy, "matter open")
        status, text = command(proxy, "matter")
        try:
            code = json.loads(text)["qr"]
        except (ValueError, KeyError, TypeError):
            match = re.search(r"MT:[0-9A-Z.\-]+", text or "")
            code = match.group(0) if match else code
        try:
            node = matter_server("commission_with_code", {"code": code, "network_only": True})
        except (OSError, ValueError) as again:
            log(f"!! {again}")
            return False
    node_id = node.get("node_id") if isinstance(node, dict) else node
    log(f"== Added: Matter node {node_id}")
    if isinstance(node_id, int):
        remember(proxy, matter_node=node_id)
        name_parts(proxy, node_id)
    return True


def name_parts(proxy, node_id=None):
    """The parts of a grouped device get their names in Home Assistant; a
    problem here is told, and does not undo that the device was added."""
    node_id = node_id if isinstance(node_id, int) else proxy.get("matter_node")
    if not isinstance(node_id, int):
        log("!! Not known which Matter node this is in Home Assistant: add it from here once more")
        return False
    try:
        count = name_parts_in_ha(node_id)
        if count:
            log(f"== {count} part{'s' if count != 1 else ''} named in Home Assistant")
        return True
    except (OSError, ValueError, KeyError) as err:
        log(f"!! The parts were not named in Home Assistant ({err}): rename them there if two have the same name")
        return False


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


CHIP_NAMES = {"esp32c6": "ESP32-C6", "esp32h2": "ESP32-H2", "esp32s3": "ESP32-S3", "esp32c3": "ESP32-C3",
              "esp32c5": "ESP32-C5", "esp32": "ESP32"}
# Chips there is firmware for (firmware/generic/CMakeLists.txt)
BUILD_CHIPS = ("esp32c6", "esp32", "esp32s3", "esp32c3", "esp32c5")
# Flash for two app slots: 3 MB each (partitions_8mb.csv), 1984 KB each on
# the classic ESP32 and the ESP32-C3 (partitions_4mb.csv)
MIN_FLASH_MB = {"esp32": 4, "esp32c3": 4}
DEFAULT_MIN_FLASH_MB = 8
# What esptool calls a chip, most specific first; anything else that starts
# with ESP32 is the classic one, which reports its die (ESP32-D0WD-V3,
# ESP32-PICO-…, ESP32-U4WDH)
CHIP_PREFIXES = (("ESP32-C61", "esp32c61"), ("ESP32-C6", "esp32c6"), ("ESP32-C5", "esp32c5"),
                 ("ESP32-C3", "esp32c3"), ("ESP8685", "esp32c3"), ("ESP32-C2", "esp32c2"),
                 ("ESP8684", "esp32c2"), ("ESP32-H2", "esp32h2"), ("ESP32-H4", "esp32h4"),
                 ("ESP32-S3", "esp32s3"), ("ESP32-S2", "esp32s2"), ("ESP32-P4", "esp32p4"),
                 ("ESP32", "esp32"))


def chip_key(name):
    """esptool's name for a chip as the firmware's (esp32c6, esp32, …), or ""."""
    upper = name.upper()
    return next((key for prefix, key in CHIP_PREFIXES if upper.startswith(prefix)), "")


def read_board(port):
    """What is on a USB port, asked with esptool: its name as esptool gives
    it, the chip, and the megabytes of flash (0 when unknown). None when no
    ESP32 answered. The second value is esptool's output."""
    result = subprocess.run(["python3", "-m", "esptool", "-p", port, "flash_id"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
    text = result.stdout
    if result.returncode != 0:
        return None, text
    found = re.search(r"Chip (?:is|type:)\s*(ESP[-\w]*)", text)
    name = found.group(1).upper() if found else ""
    size = re.search(r"Detected flash size:\s*(\d+)MB", text)
    return {"name": name, "chip": chip_key(name), "flash_mb": int(size.group(1)) if size else 0}, text


def chip_and_network(app):
    """The chip and network of a firmware by its name, as the release writes
    them (tools/release_manifest.py), for images and builds without them."""
    if app.endswith("_wifi"):
        return "esp32c6", "wifi"
    for chip in CHIP_NAMES:
        if app.endswith("_" + chip):
            return chip, "thread" if chip == "esp32h2" else "wifi"
    return "esp32c6", "thread"


def image_summary(image):
    chip, network = chip_and_network(image["app"])
    return {**{k: image[k] for k in ("file", "app", "version", "source", "official")},
            "chip": image.get("chip", chip), "network": image.get("network", network)}


def check_board(port, chip):
    """The board on the port is the image's chip, with flash enough for two
    images; the reason when it is not, else None."""
    board, text = read_board(port)
    with JOB_LOG.open("a") as out:
        out.write(text)
    if board is None:
        return "no ESP32 answered on that port (hold BOOT while plugging it in, and try again)"
    wanted = CHIP_NAMES.get(chip, chip.upper())
    if board["name"] and board["chip"] != chip:
        return (f"this board is an {board['name']}, the firmware is for the {wanted}: "
                f"choose the firmware for the {board['name']}")
    need = MIN_FLASH_MB.get(chip, DEFAULT_MIN_FLASH_MB)
    if board["flash_mb"] and board["flash_mb"] < need:
        return (f"this board has {board['flash_mb']} MB of flash; the firmware needs {need} MB "
                f"(room for an update next to it): look for a module with {need} MB or more")
    return None


def flash_on_host(port, image, backup):
    """A factory image at 0 with esptool, after a backup of the whole flash."""
    chip, _ = chip_and_network(image.name.removesuffix("-factory.bin").rpartition("-")[0])
    log(f"== Checking the board ({CHIP_NAMES.get(chip, chip)} with "
        f"{MIN_FLASH_MB.get(chip, DEFAULT_MIN_FLASH_MB)} MB of flash needed)")
    problem = check_board(port, chip)
    if problem:
        log(f"!! {problem}")
        return False
    esptool = ["python3", "-m", "esptool", "--chip", chip, "-p", port, "-b", "460800"]
    if backup:
        BACKUPS.mkdir(parents=True, exist_ok=True)
        target = BACKUPS / f"backup-{chip}-{time.strftime('%Y%m%d-%H%M%S')}.bin"
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
        self.nodes = {}  # host -> the "<fabric>-<node>" pairs it announces (_matter._tcp)

    def start(self):
        if Zeroconf is None:
            print("zeroconf missing: no discovery", flush=True)
            return
        # IPv4 and IPv6, else IPv4 alone (a host or emulator without IPv6
        # multicast), else no discovery: linking by address still works
        for version in (IPVersion.All, IPVersion.V4Only):
            try:
                self.zc = Zeroconf(ip_version=version)
                break
            except OSError as err:
                print(f"discovery over {version.name}: {err}", flush=True)
        if self.zc is None:
            print("no discovery: link devices by their address", flush=True)
            return
        ServiceBrowser(self.zc, self.TYPES, handlers=[self.on_change])

    def on_change(self, zeroconf, service_type, name, state_change):
        if state_change is ServiceStateChange.Removed:
            pair = re.match(r"^([0-9A-F]{16}-[0-9A-F]{16})\._matter\._tcp", name)
            with lock:
                for pairs in self.nodes.values():
                    pairs.discard(pair.group(1) if pair else None)
        if state_change is not ServiceStateChange.Removed:
            threading.Thread(target=self.check, args=(service_type, name), daemon=True).start()

    def check(self, service_type, name):
        info = self.zc.get_service_info(service_type, name, timeout=3000)
        if not info:
            return
        host = (info.server or "").rstrip(".")
        pair = re.match(r"^([0-9A-F]{16}-[0-9A-F]{16})\._matter\._tcp", name)
        with lock:
            if pair and host:
                self.nodes.setdefault(host, set()).add(pair.group(1))
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
        if not linked(device["id"], host, address) and device["id"] not in ignored_devices():
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


def ignored_devices():
    """Found devices the user does not want offered for linking (like "Ignore"
    on a discovered device in Home Assistant): id -> what it was, for the
    list in the settings where it can be shown again."""
    value = load_settings().get("ignored")
    return value if isinstance(value, dict) else {}


def set_ignored(device_id, on):
    settings = load_settings()
    ignored = settings.get("ignored") if isinstance(settings.get("ignored"), dict) else {}
    if on:
        with lock:
            device = dict(found.get(device_id) or {})
        ignored[device_id] = {"app": device.get("app", ""), "host": device.get("host", ""), "since": int(time.time())}
        dismiss(device_id)
    else:
        ignored.pop(device_id, None)
    settings["ignored"] = ignored
    save_settings(settings)


def found_unlinked():
    with lock:
        devices = list(found.values())
    ignored = ignored_devices()
    return [d for d in devices if d["id"] not in ignored and not linked(d["id"], d["host"], d["address"])]


# A found device that is gone (unplugged, or it left the network) must not
# stay on the page: one that is not linked is asked again every few minutes,
# and dropped when it did not answer twice in a row. It comes back by itself
# when it announces itself again.
FOUND_RECHECK = 180
found_misses = {}


def found_sweep():
    while True:
        time.sleep(60)
        with lock:
            devices = [dict(d) for d in found.values()]
        for device in devices:
            if time.time() - device.get("seen", 0) < FOUND_RECHECK or linked(device["id"], device["host"], device["address"]):
                continue
            try:
                answer = device_info(device["address"])
            except Exception:
                answer = None
            with lock:
                if answer and answer.get("id") == device["id"]:
                    found_misses.pop(device["id"], None)
                    if device["id"] in found:
                        found[device["id"]].update(seen=int(time.time()), version=answer.get("version", device.get("version")))
                    continue
                found_misses[device["id"]] = found_misses.get(device["id"], 0) + 1
                gone = found_misses[device["id"]] >= 2
                if gone:
                    found.pop(device["id"], None)
                    found_misses.pop(device["id"], None)
                    discovery.asked.pop(device["host"], None)  # asked at once when it is back
                else:
                    # not again for a while: the next round is the second try
                    if device["id"] in found:
                        found[device["id"]]["seen"] = int(time.time()) - FOUND_RECHECK + 60
            if gone:
                print(f"{device['app']} at {device['host']} no longer answers: not offered any more", flush=True)
                dismiss(device["id"])


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


# ---------------------------------------------------------------- share to the catalog
# A recipe gets into the catalog from its maker's own public GitHub
# repository with the topic "matter-builder-package". With GitHub's device
# flow the add-on does that for the user: it shows a short code, the user
# enters it on github.com and allows it, and the add-on makes the repository
# (when it is not there), adds the recipe and sets the topic. The access
# GitHub hands out is used for that once and then forgotten: it is never
# written to disk. Without a GitHub app configured (GITHUB_CLIENT_ID), the
# page shows the same steps to do by hand.

# The public id of the "Matter Builder" OAuth app on GitHub (device flow on);
# not a secret. The add-on option "github_client_id" or MB_GITHUB_CLIENT_ID
# replaces it, for a fork with an app of its own.
DEFAULT_GITHUB_CLIENT_ID = "Ov23liUmcv8YXuijzubu"
SHARE_TOPIC = "matter-builder-package"
SHARE_REPO = re.compile(r"^[A-Za-z0-9._-]{1,100}$")
shares = {}  # id -> a share on its way (device code, what to publish, state)


def github_client_id():
    return os.environ.get("MB_GITHUB_CLIENT_ID") or option("github_client_id", DEFAULT_GITHUB_CLIENT_ID)


def github_form(url, fields):
    """A form post to github.com that answers in JSON (the device flow)."""
    req = urllib.request.Request(url, data=urllib.parse.urlencode(fields).encode(),
                                 headers={"Accept": "application/json", "User-Agent": "matter-builder"})
    try:
        with urllib.request.urlopen(req, timeout=20) as reply:
            return json.loads(reply.read())
    except urllib.error.HTTPError as err:  # GitHub says what is wrong in the body
        try:
            return json.loads(err.read())
        except ValueError:
            raise err from None


def github_api(method, path, token, body=None):
    """GitHub's REST API as the user: (status, JSON)."""
    req = urllib.request.Request("https://api.github.com" + path, method=method,
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Accept": "application/vnd.github+json", "User-Agent": "matter-builder",
                                          "Authorization": f"Bearer {token}", "X-GitHub-Api-Version": "2022-11-28"})
    try:
        with urllib.request.urlopen(req, timeout=30) as reply:
            return reply.status, json.loads(reply.read() or b"{}")
    except urllib.error.HTTPError as err:
        try:
            return err.code, json.loads(err.read() or b"{}")
        except ValueError:
            return err.code, {}


def shareable_text(text):
    """A configuration as a recipe for others: without what is the user's
    own (a Bluetooth device's address)."""
    return "".join(line for line in text.splitlines(True) if not re.match(r"\s*address(_type)?\s*:", line))


def share_start(slug, repo):
    client = github_client_id()
    if not client:
        return {"error": "no GitHub app is set up for this"}
    spec_file = SPECS / f"{Path(slug).name}.yaml"
    if not spec_file.is_file():
        return {"error": "unknown configuration"}
    if not SHARE_REPO.match(repo or ""):
        return {"error": "the repository name: letters, digits, . - _"}
    text = shareable_text(spec_file.read_text())
    _, problem = check_spec(text)  # what goes out is a valid recipe
    if problem:
        return {"error": f"this configuration cannot be shared: {problem}"}
    try:
        answer = github_form("https://github.com/login/device/code", {"client_id": client, "scope": "public_repo"})
    except (OSError, ValueError) as err:
        return {"error": f"GitHub is not reachable: {err}"}
    if "device_code" not in answer:
        if answer.get("error") == "device_flow_disabled":
            return {"error": "The GitHub app has the device flow turned off (its settings: Enable Device Flow)."}
        return {"error": str(answer.get("error_description") or answer.get("error") or "GitHub refused")}
    share_id = secrets.token_urlsafe(9)
    now = time.time()
    for key in [k for k, v in shares.items() if v["until"] < now]:
        shares.pop(key, None)
    shares[share_id] = {"device_code": answer["device_code"], "interval": int(answer.get("interval", 5)),
                        "until": now + int(answer.get("expires_in", 900)), "next": 0, "slug": Path(slug).name,
                        "repo": repo, "text": text, "state": "waiting", "result": None}
    return {"id": share_id, "code": answer["user_code"], "url": answer.get("verification_uri", "https://github.com/login/device"),
            "interval": shares[share_id]["interval"]}


def share_publish(token, share):
    """The repository (made when it is not there), the recipe in it, the topic on it."""
    status, user = github_api("GET", "/user", token)
    if status != 200:
        raise OSError(f"GitHub did not say who you are (HTTP {status})")
    full = f"{user['login']}/{share['repo']}"
    status, repo = github_api("GET", f"/repos/{full}", token)
    if status == 404:
        status, repo = github_api("POST", "/user/repos", token, {
            "name": share["repo"], "description": "Recipes for Matter Builder", "private": False, "auto_init": True,
            "homepage": "https://github.com/bramboe/matter-builder"})
        if status not in (200, 201):
            raise OSError(f"the repository was not made: {repo.get('message') or status}")
    elif status != 200:
        raise OSError(f"{full}: {repo.get('message') or status}")
    if repo.get("private"):
        raise OSError(f"{full} is private: the catalog only lists public repositories")
    branch = repo.get("default_branch") or "main"
    path = f"/repos/{full}/contents/recipes/{share['slug']}.yaml"
    body = {"message": f"Recipe: {share['slug']}", "branch": branch,
            "content": base64.b64encode(share["text"].encode()).decode()}
    for attempt in range(5):  # a repository made a second ago may not take files yet
        status, existing = github_api("GET", f"{path}?ref={urllib.parse.quote(branch)}", token)
        if status == 200 and existing.get("sha"):
            body["sha"] = existing["sha"]
        status, put = github_api("PUT", path, token, body)
        if status in (200, 201):
            break
        time.sleep(2)
    else:
        raise OSError(f"the recipe was not added: {put.get('message') or status}")
    status, topics = github_api("GET", f"/repos/{full}/topics", token)
    names = topics.get("names", []) if status == 200 else []
    if SHARE_TOPIC not in names:
        status, answer = github_api("PUT", f"/repos/{full}/topics", token, {"names": names + [SHARE_TOPIC]})
        if status != 200:
            raise OSError(f"the topic was not set: {answer.get('message') or status}")
    return {"repo": full, "url": repo.get("html_url") or f"https://github.com/{full}",
            "file": f"https://github.com/{full}/blob/{branch}/recipes/{share['slug']}.yaml"}


def share_poll(share_id):
    share = shares.get(share_id)
    if share is None:
        return {"state": "expired"}
    if share["state"] != "waiting":
        return {"state": share["state"], **(share["result"] or {})}
    now = time.time()
    if now > share["until"]:
        shares.pop(share_id, None)
        return {"state": "expired"}
    if now < share["next"]:
        return {"state": "waiting"}
    share["next"] = now + share["interval"]
    try:
        answer = github_form("https://github.com/login/oauth/access_token", {
            "client_id": github_client_id(), "device_code": share["device_code"],
            "grant_type": "urn:ietf:params:oauth:grant-type:device_code"})
    except (OSError, ValueError):
        return {"state": "waiting"}
    token = answer.get("access_token")
    if not token:
        error = answer.get("error")
        if error == "slow_down":
            share["interval"] += 5
        if error in ("authorization_pending", "slow_down"):
            return {"state": "waiting"}
        share.update(state="failed", result={"error": "You did not allow it on GitHub." if error == "access_denied"
                                             else "The code expired: start again." if error == "expired_token"
                                             else str(answer.get("error_description") or error or "GitHub refused")})
        return {"state": "failed", **share["result"]}
    share["device_code"] = ""
    try:
        share.update(state="done", result=share_publish(token, share))
    except (OSError, ValueError, KeyError) as err:
        share.update(state="failed", result={"error": str(err)})
    del token  # used once: not kept, not stored
    return {"state": share["state"], **share["result"]}


# ---------------------------------------------------------------- management

# What the management pages may send to a proxy: its connections, name,
# and Wi-Fi, its Matter pairing (the code, and a window for one more
# controller such as Home Assistant) and the settings its firmware declares;
# nothing that changes access or the firmware
ALLOWED_COMMANDS = ("io ", "name ", "wifi ssid ", "wifi password ", "wifi ssid-hex ", "wifi password-hex ", "rule ",
                    "bledev ")
HEX_WIFI = re.compile(r"wifi (ssid|password)-hex ([0-9a-f]+)")


def send_line(proxy, line):
    """A command; firmware from before the hex Wi-Fi commands gets the plain one."""
    status, text = command(proxy, line)
    match = HEX_WIFI.fullmatch(line)
    if match and status == 200 and not text.startswith(("ok", "error")):
        plain = bytes.fromhex(match.group(2)).decode(errors="replace")
        status, text = command(proxy, f"wifi {match.group(1)} {plain}")
    return status, text
SETTING_SET = re.compile(r"settings set [a-z_][a-z0-9_]{0,31} [A-Za-z0-9_.:-]{1,64}")


def allowed(line):
    return line in ("io json", "json", "wifi json", "matter", "matter json", "matter open", "matter share", "matter close", "pairing", "pairing ble on",
                    "pairing ble off", "reboot", "settings json", "system json", "bledev json", "events") or line.startswith(ALLOWED_COMMANDS) \
        or SETTING_SET.fullmatch(line) is not None


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


def running_version(proxy):
    """The running firmware's version ("1.0.17"), or None when the device does not answer."""
    status, system = command(proxy, "system")
    if status != 200:
        return None
    return system.split("\n")[0].split(" (")[0].rsplit(" ", 1)[-1]


def night_updates():
    """Devices with automatic updates on, each to the newest firmware of its
    own application, one after the other."""
    done = 0
    for proxy in [p for p in load_proxies() if p.get("auto_update")]:
        app = proxy.get("app")
        name = newest_firmware(app) if app else None
        version = running_version(proxy)
        if not name or version is None:
            log(f"== {proxy['name']}: " + ("not reachable, skipped" if version is None else "no firmware, skipped"))
            continue
        if version_key(f"x-{version}.bin") >= version_key(name):
            log(f"== {proxy['name']}: already up to date ({version})")
            continue
        image = FIRMWARE / name if (FIRMWARE / name).is_file() else release_image(name)
        if update_proxy(proxy, image):
            done += 1
        else:
            log(f"!! {proxy['name']} did not update; the others go on")
    log(f"== Night updates done: {done} updated")
    return True


def night_loop():
    """At the hour set in the settings (Home Assistant's time zone), once a day."""
    while True:
        time.sleep(300)
        settings = load_settings()
        now = time.localtime()
        today = time.strftime("%Y-%m-%d", now)
        if now.tm_hour != settings.get("night_hour", 3) or settings.get("night_done") == today:
            continue
        if not any(p.get("auto_update") for p in load_proxies()):
            continue
        check_releases()  # the newest release, not the one from hours ago
        if start_job("update", "night", night_updates):
            settings["night_done"] = today
            save_settings(settings)
        # a job already running: tried again in five minutes, still within the hour


# ---------------------------------------------------------------- network health
# Every 5 minutes each linked device is asked how its network is doing
# ("system json": its "net" object). A day of these, per device, says where a
# problem is: the device and its mesh, the way out to the home network (the
# border router), or the way from there to here. A Thread device that
# drops out is often the border router's fault, not the device's.

HEALTH = DATA / "health"
HEALTH_EVERY = 300
HEALTH_KEEP = 86400
health_samples = {}  # device id -> samples of the last day, oldest first
health_lock = threading.Lock()


def health_file(proxy_id):
    return HEALTH / f"{re.sub(r'[^A-Za-z0-9_-]', '', proxy_id)}.json"


def health_get(proxy_id):
    with health_lock:
        if proxy_id not in health_samples:
            try:
                health_samples[proxy_id] = json.loads(health_file(proxy_id).read_text())
            except (OSError, ValueError):
                health_samples[proxy_id] = []
        return list(health_samples[proxy_id])


def health_sample(proxy):
    """One measurement: does it answer, how fast, and what it says about its network."""
    health_get(proxy["id"])  # loaded from disk first
    started = time.time()
    status, text = command(proxy, "system json", timeout=20)
    sample = {"t": int(started), "ok": False}
    if status == 200:
        try:
            info = json.loads(text)
            sample.update(ok=True, rtt=int((time.time() - started) * 1000), uptime=info.get("uptime"),
                          net=info.get("net") if isinstance(info.get("net"), dict) else None)
            # Memory: free now, the lowest since it started, the largest free
            # block, what each task has left of its stack, why it last restarted
            for key in ("heap", "heap_min", "heap_block"):
                if isinstance(info.get(key), int):
                    sample[key] = info[key]
            if isinstance(info.get("stacks"), dict):
                sample["stacks"] = {str(k): v for k, v in info["stacks"].items() if isinstance(v, int)}
            if isinstance(info.get("reset"), str):
                sample["reset"] = info["reset"]
            # Matter: paired with how many controllers, how many are connected now
            if isinstance(info.get("matter"), dict):
                sample["matter"] = {k: info["matter"].get(k) for k in ("state", "fabrics", "connected")}
        except ValueError:
            sample["error"] = "no answer it understands"
    else:
        sample["error"] = text.strip()[:120] or f"HTTP {status}"
    with health_lock:
        samples = [x for x in health_samples.get(proxy["id"], []) if x["t"] > started - HEALTH_KEEP] + [sample]
        health_samples[proxy["id"]] = samples
        HEALTH.mkdir(parents=True, exist_ok=True)
        health_file(proxy["id"]).write_text(json.dumps(samples))
    return sample


def health_loop():
    while True:
        time.sleep(HEALTH_EVERY)
        with lock:
            busy = job["running"]
        if busy:  # an update or install talks to devices: not in between
            continue
        for proxy in load_proxies():
            try:
                health_sample(proxy)
            except Exception as err:  # one device must not stop the others
                print(f"health {proxy.get('name')}: {err}", flush=True)


def grown(samples, key, parent=None):
    """How much a counter of the device grew over the samples (restarts reset it)."""
    total, previous, previous_up = 0, None, None
    for sample in samples:
        net = sample.get("net") or {}
        value = (net.get(parent) or {}).get(key) if parent else net.get(key)
        up = sample.get("uptime")
        if not isinstance(value, (int, float)):
            continue
        if previous is not None and up is not None and previous_up is not None and up >= previous_up:
            total += max(0, value - previous)
        previous, previous_up = value, up
    return total


def restarts(samples):
    ups = [s["uptime"] for s in samples if s.get("ok") and isinstance(s.get("uptime"), (int, float))]
    return sum(1 for a, b in zip(ups, ups[1:]) if b < a)


def crashes(samples):
    """Restarts in the samples that were a crash or a watchdog, not an update or a power cut."""
    count, previous = 0, None
    for sample in samples:
        up = sample.get("uptime") if sample.get("ok") else None
        if not isinstance(up, (int, float)):
            continue
        if previous is not None and up < previous and sample.get("reset") in ("crash", "watchdog"):
            count += 1
        previous = up
    return count


def kb(size):
    return f"{size / 1024:.0f} KB" if size >= 10240 else f"{size / 1024:.1f} KB"


def memory_link(ok):
    """The device's memory over the day: how low it got, fragmentation, the
    tightest task stack and crashes. None when the firmware does not say."""
    with_memory = [s for s in ok if isinstance(s.get("heap_min"), int)]
    if not with_memory:
        return None
    # Since its last restart only: the lowest point is counted from each
    # start, and a restart (an update, say) begins with a clean slate
    start = 0
    for i in range(1, len(with_memory)):
        before, now = with_memory[i - 1].get("uptime"), with_memory[i].get("uptime")
        if isinstance(before, (int, float)) and isinstance(now, (int, float)) and now < before:
            start = i
    with_memory = with_memory[start:]
    last = with_memory[-1]
    lowest = min(s["heap_min"] for s in with_memory)
    block, free = last.get("heap_block"), last.get("heap")
    stacks = last.get("stacks") or {}
    task, left = min(stacks.items(), key=lambda item: item[1]) if stacks else (None, None)
    crashed = crashes(ok)
    name = "Device memory"
    if left is not None and left < 256:
        return {"name": name, "state": "bad",
                "text": f"The task “{task}” has only {left} bytes of stack left: a crash is close. Please report this."}
    if lowest < 8192:
        return {"name": name, "state": "bad",
                "text": f"Almost out of memory: {kb(lowest)} free at its lowest. It may restart by itself; fewer connections help."}
    if crashed >= 2:
        return {"name": name, "state": "bad", "text": f"Restarted after a crash {crashed}× in the last day."}
    if left is not None and left < 768:
        return {"name": name, "state": "warn", "text": f"The task “{task}” is low on stack ({left} bytes left)."}
    if lowest < 20480:
        return {"name": name, "state": "warn", "text": f"Memory ran low: {kb(lowest)} free at its lowest."}
    if isinstance(block, int) and isinstance(free, int) and block < 12288 and block < free // 3:
        return {"name": name, "state": "warn",
                "text": f"Memory is fragmented: {kb(free)} free, but the largest piece is {kb(block)}. A restart tidies it."}
    if crashed:
        return {"name": name, "state": "warn", "text": "Restarted after a crash once in the last day."}
    detail = f"; tightest stack: {task}, {kb(left)} left" if task else ""
    return {"name": name, "state": "ok", "text": f"{kb(free) if isinstance(free, int) else kb(lowest)} free, {kb(lowest)} at its lowest{detail}."}


def controllers_link(ok):
    """Paired is not the same as connected: a controller that is paired but
    holds no connection is what Apple Home shows as "No Response". None when
    the firmware does not say."""
    with_matter = [s for s in ok if isinstance(s.get("matter"), dict) and isinstance(s["matter"].get("fabrics"), int)]
    if not with_matter:
        return None
    last = with_matter[-1]["matter"]
    fabrics, connected = last["fabrics"], last.get("connected") or 0
    name = "Matter controllers"
    if fabrics == 0:
        return {"name": name, "state": "unknown", "text": "Not paired with a controller yet."}
    # How long none has been connected: the checks in a row that say so
    silent = 0
    for sample in reversed(with_matter):
        if (sample["matter"].get("connected") or 0) > 0 or not sample["matter"].get("fabrics"):
            break
        silent += 1
    plural = "s" if fabrics != 1 else ""
    if connected == 0 and silent >= 2:
        since = clock_text(with_matter[-silent]["t"])
        return {"name": name, "state": "warn",
                "text": f"Paired with {fabrics} controller{plural}, but none has been connected since {since} "
                        "(Apple Home shows that as “No Response”). Check the controller, or its way to this device."}
    if connected == 0:
        return {"name": name, "state": "ok", "text": f"Paired with {fabrics} controller{plural}; none connected at this moment."}
    return {"name": name, "state": "ok", "text": f"{connected} of {fabrics} controller{plural} connected."}


def clock_text(t):
    return time.strftime("%H:%M", time.localtime(t))


def health_verdict(samples, others_down=False):
    """Per link: "ok", "warn" or "bad" with a sentence; and one summary up top."""
    if not samples:
        return {"summary": "No measurements yet: the first one comes within five minutes.", "state": "unknown",
                "links": []}
    ok = [s for s in samples if s.get("ok")]
    latest, last_ok = samples[-1], (ok[-1] if ok else None)
    net = (last_ok or {}).get("net") or {}
    thread = net.get("kind", "thread") == "thread"
    links = []

    # 1. The device and its own network
    detaches, changes = grown(ok, "detaches"), grown(ok, "parent_changes")
    rebooted = restarts(ok)
    if not net:
        links.append({"name": "Device", "state": "unknown",
                      "text": "This firmware does not report its network yet (update it for network health)."})
    elif thread:
        parent = net.get("parent") or {}
        rssi = parent.get("rssi")
        tx, retry = grown(ok, "tx"), grown(ok, "tx_retry")
        name = "Device ↔ Thread mesh"
        if not net.get("attached"):
            links.append({"name": name, "state": "bad", "text": "Not connected to the Thread network."})
        elif (isinstance(rssi, int) and rssi < -85) or parent.get("lqi_in", 3) <= 1:
            links.append({"name": name, "state": "warn",
                          "text": f"Weak signal to its parent ({rssi} dBm). A mains-powered Thread device closer by helps."})
        elif detaches >= 3 or changes >= 6:
            links.append({"name": name, "state": "warn",
                          "text": "Unstable: " + " and ".join(x for x in (f"lost the network {detaches}×" if detaches else "",
                                                                     f"changed parent {changes}×" if changes else "") if x)
                                  + " in the last day."})
        elif tx > 200 and retry / tx > 0.3:
            links.append({"name": name, "state": "warn",
                          "text": f"Busy radio: {round(100 * retry / tx)} % of messages needed a retry."})
        else:
            role = net.get("role", "")
            detail = (f"signal to its parent {rssi} dBm" if role == "child" and isinstance(rssi, int)
                      else f"{net.get('neighbors', 0)} neighbours" if role in ("router", "leader") else "connected")
            links.append({"name": name, "state": "ok", "text": f"{role.capitalize() or 'Connected'}, {detail}."})
        # 2. The way out of the mesh: the border router
        name = "Thread mesh ↔ border router"
        routers = net.get("border_routers", 0)
        if net.get("attached") is False:
            links.append({"name": name, "state": "unknown", "text": "Not known while the device is off the network."})
        elif not net.get("route") or not net.get("omr"):
            links.append({"name": name, "state": "bad",
                          "text": "No border router offers a way to your home network. Check your Apple TV, HomePod, Nest Hub or Home Assistant's border router."})
        elif net.get("srp") not in (None, "Registered"):
            links.append({"name": name, "state": "warn",
                          "text": f"Not registered with the border router (SRP: {net.get('srp')}): Matter controllers may not find it."})
        else:
            links.append({"name": name, "state": "ok",
                          "text": f"Way out through {routers} border router{'s' if routers != 1 else ''}"
                                  + (" (no backup if it goes down)." if routers == 1 else ".")})
    else:
        rssi = net.get("rssi")
        name = "Device ↔ Wi-Fi"
        if not net.get("attached"):
            links.append({"name": name, "state": "bad", "text": "Not connected to Wi-Fi."})
        elif isinstance(rssi, int) and rssi < -80:
            links.append({"name": name, "state": "bad" if rssi < -87 else "warn",
                          "text": f"Weak Wi-Fi signal ({rssi} dBm): move it or the access point closer."})
        elif detaches >= 3 or grown(ok, "disconnects") >= 3:
            links.append({"name": name, "state": "warn",
                          "text": f"Lost Wi-Fi {grown(ok, 'disconnects')}× in the last day."})
        else:
            links.append({"name": name, "state": "ok",
                          "text": f"Signal {rssi} dBm." if isinstance(rssi, int) else "Connected."})
    if rebooted:
        links[0] = dict(links[0], note=f"Restarted {rebooted}× in the last day.")
    controllers = controllers_link(ok)
    if controllers:
        links.append(controllers)
    memory = memory_link(ok)
    if memory:
        links.append(memory)

    # 3. From there to Matter Builder
    rtts = sorted(s["rtt"] for s in ok if "rtt" in s)
    median = rtts[len(rtts) // 2] if rtts else None
    ratio = len(ok) / len(samples)
    name = ("Border router" if thread else "Wi-Fi") + " ↔ Matter Builder"
    text = f"Answered {len(ok)} of {len(samples)} checks in the last day" + (f", typically in {median} ms." if median else ".")
    # The longest stretch without answers (not the one going on now: the summary says that)
    gap, run = None, []
    for sample in samples:
        if not sample.get("ok"):
            run.append(sample["t"])
            continue
        if len(run) >= 2 and (gap is None or sample["t"] - run[0] > gap[1]):
            gap = (run[0], sample["t"] - run[0])
        run = []
    if gap:
        text += f" Unreachable for {round(gap[1] / 60)} min from {clock_text(gap[0])}."
    links.append({"name": name, "state": "bad" if ratio < 0.8 else "warn" if ratio < 0.95 or (median or 0) > 3000 else "ok",
                  "text": text})

    # The summary: where to look first
    if not latest.get("ok"):
        since = latest["t"]
        for sample in reversed(samples):
            if sample.get("ok"):
                break
            since = sample["t"]
        summary = f"Not heard from since {clock_text(since)}."
        if others_down and thread:
            summary += " Your other Thread devices do not answer either: most likely the border router or the Thread network, not this device."
        elif memory and memory["state"] == "bad":
            summary += " The last time it answered it was almost out of memory: it may have stopped by itself. Power it off and on."
        elif last_ok and all(link["state"] == "ok" for link in links[:-1]):
            summary += (" The last time it answered, its network was fine: most likely it has no power"
                        + (", or the border router is down." if thread else ", or Wi-Fi is down."))
        state = "bad"
    else:
        worst = next((link for level in ("bad", "warn") for link in links if link["state"] == level), None)
        summary = worst["text"] if worst else (
            f"Fine now. It was unreachable for {round(gap[1] / 60)} min from {clock_text(gap[0])}." if gap else "All fine.")
        state = worst["state"] if worst else "ok"
    return {"summary": summary, "state": state, "links": links, "kind": "thread" if thread else "wifi"}


def health_reply(proxy):
    samples = health_get(proxy["id"])
    others = [p for p in load_proxies() if p["id"] != proxy["id"]]
    latest = [health_get(p["id"])[-1:] for p in others]
    thread_others = [x[0] for x in latest if x and ((x[0].get("net") or {}).get("kind") == "thread" or not x[0].get("ok"))]
    others_down = bool(thread_others) and all(not x.get("ok") for x in thread_others)
    return {"samples": [{"t": x["t"], "ok": x.get("ok"), "rtt": x.get("rtt")} for x in samples],
            "verdict": health_verdict(samples, others_down), "every": HEALTH_EVERY}


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


# ---------------------------------------------------------------- standalone login
# Without Home Assistant there is nobody in front of the page, so it has a
# password of its own. The first password is chosen on the page; from another
# machine that also needs the setup code printed in the log, so nobody else on
# the network can claim a fresh installation first. Sessions are random
# tokens in an HttpOnly, SameSite=Strict cookie; only their hashes are stored.

SESSION_DAYS = 30
PBKDF2_ROUNDS = 310000
auth_lock = threading.Lock()
login_failures = {"count": 0, "until": 0.0}
SETUP_CODE = "-".join(secrets.token_hex(2).upper() for _ in range(3))


def auth_load():
    try:
        return json.loads(AUTH.read_text())
    except (OSError, ValueError):
        return {}


def auth_save(data):
    AUTH.parent.mkdir(parents=True, exist_ok=True)
    tmp = AUTH.with_suffix(".tmp")
    tmp.write_text(json.dumps(data))
    os.chmod(tmp, 0o600)
    tmp.replace(AUTH)


def password_hash(password, salt, rounds=PBKDF2_ROUNDS):
    return hashlib.pbkdf2_hmac("sha256", password.encode(), bytes.fromhex(salt), rounds).hex()


def password_set():
    return bool(auth_load().get("hash"))


def password_ok(password):
    data = auth_load()
    if not data.get("hash"):
        return False
    return hmac.compare_digest(password_hash(password, data["salt"], data.get("rounds", PBKDF2_ROUNDS)), data["hash"])


def set_password(password):
    """A new password ends every session."""
    salt = secrets.token_hex(16)
    auth_save({"salt": salt, "rounds": PBKDF2_ROUNDS, "hash": password_hash(password, salt), "sessions": {}})


def session_key(token):
    return hashlib.sha256(token.encode()).hexdigest()


def new_session():
    token = secrets.token_urlsafe(32)
    with auth_lock:
        data = auth_load()
        now = time.time()
        sessions = {k: v for k, v in data.get("sessions", {}).items() if v > now}
        sessions[session_key(token)] = now + SESSION_DAYS * 86400
        data["sessions"] = sessions
        auth_save(data)
    return token


def session_ok(token):
    if not token:
        return False
    return auth_load().get("sessions", {}).get(session_key(token), 0) > time.time()


def end_session(token):
    with auth_lock:
        data = auth_load()
        data.get("sessions", {}).pop(session_key(token or ""), None)
        auth_save(data)


def password_problem(password):
    if len(password) < 8:
        return "at least 8 characters"
    if len(password) > 128:
        return "at most 128 characters"
    return None


# ---------------------------------------------------------------- HTTP

# Without a session: the page itself (it shows the login) and the login API
PUBLIC_PATHS = {"/", "/index.html", "/esptool.js", "/api/auth", "/api/auth/login", "/api/auth/setup"}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def allowed(self):
        if STANDALONE:
            return self.standalone_allowed()
        if ALLOW_ANY or self.client_address[0] in (INGRESS, "::ffff:" + INGRESS):
            return True
        self.send_error(403)
        return False

    def session_token(self):
        for part in self.headers.get("Cookie", "").split(";"):
            key, _, value = part.strip().partition("=")
            if key == "mb_session":
                return value
        return ""

    def local_client(self):
        return self.client_address[0] in ("127.0.0.1", "::1", "::ffff:127.0.0.1")

    def standalone_allowed(self):
        # Another site may not post here in the name of a logged-in browser
        # (the cookie is SameSite=Strict as well)
        origin = self.headers.get("Origin")
        if self.command == "POST" and origin and urllib.parse.urlsplit(origin).netloc != self.headers.get("Host", ""):
            self.reply({"error": "cross-site request"}, 403)
            return False
        if self.path.split("?")[0] in PUBLIC_PATHS or session_ok(self.session_token()):
            return True
        self.reply({"error": "login"}, 401)
        return False

    def reply(self, data, status=200, ctype="application/json", headers=None):
        body = data if isinstance(data, bytes) else json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)

    def session_cookie(self, token, max_age):
        return {"Set-Cookie": f"mb_session={token}; HttpOnly; SameSite=Strict; Path=/; Max-Age={max_age}"}

    def auth_post(self, path, data):
        """The standalone login: first password, log in, log out, change it."""
        if not STANDALONE:
            return self.reply({"error": "not found"}, 404)
        password = str(data.get("password", ""))
        if path == "/api/auth/setup":
            if password_set():
                return self.reply({"error": "a password is already set"}, 409)
            code = str(data.get("code", "")).strip().upper()
            if not self.local_client() and not hmac.compare_digest(code, SETUP_CODE):
                return self.reply({"error": "wrong setup code: it is in the log of Matter Builder"}, 403)
            problem = password_problem(password)
            if problem:
                return self.reply({"error": f"the password needs {problem}"}, 400)
            set_password(password)
            print("password set", flush=True)
            return self.reply({"ok": True}, headers=self.session_cookie(new_session(), SESSION_DAYS * 86400))
        if path == "/api/auth/login":
            now = time.time()
            if now < login_failures["until"]:
                return self.reply({"error": f"too many attempts: try again in {int(login_failures['until'] - now) + 1} s"}, 429)
            if not password_ok(password):
                login_failures["count"] += 1
                if login_failures["count"] >= 5:
                    login_failures.update(count=0, until=now + 30)
                time.sleep(1)
                return self.reply({"error": "wrong password"}, 403)
            login_failures.update(count=0, until=0.0)
            return self.reply({"ok": True}, headers=self.session_cookie(new_session(), SESSION_DAYS * 86400))
        if path == "/api/auth/logout":
            end_session(self.session_token())
            return self.reply({"ok": True}, headers=self.session_cookie("", 0))
        if path == "/api/auth/password":
            if not password_ok(str(data.get("old", ""))):
                time.sleep(1)
                return self.reply({"error": "the current password is wrong"}, 403)
            problem = password_problem(password)
            if problem:
                return self.reply({"error": f"the password needs {problem}"}, 400)
            set_password(password)  # ends every other session
            return self.reply({"ok": True}, headers=self.session_cookie(new_session(), SESSION_DAYS * 86400))
        return self.reply({"error": "not found"}, 404)

    def body(self):
        length = int(self.headers.get("Content-Length", 0) or 0)
        try:
            return json.loads(self.rfile.read(length) or b"{}")
        except ValueError:
            return {}

    def do_GET(self):
        if self.allowed():
            self.guarded(lambda: self.portal("GET") if self.path.startswith("/portal/") else self.get())

    def do_POST(self):
        if self.allowed():
            self.guarded(lambda: self.portal("POST") if self.path.startswith("/portal/") else self.post())

    def portal(self, method):
        """/portal/<id>/: the device's page from here; its requests to the device."""
        parts = self.path.split("?")[0].split("/")  # "", "portal", id, [name]
        proxy = find_proxy(parts[2]) if len(parts) > 2 else None
        if not proxy:
            return self.reply(b"unknown device\n", 404, "text/plain")
        if len(parts) == 3:  # without the slash, relative paths would miss the id
            self.send_response(301)
            self.send_header("Location", parts[2] + "/")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        name = "/".join(parts[3:])
        if name == "" and method == "GET":
            # The kept copy at once, and asked anew meanwhile for the next
            # opening: no wait on a device that answers slowly (Thread)
            kept = PAGES / f"{proxy['id']}.html"
            if kept.exists():
                html = kept.read_bytes()
                prefetch_page(proxy)
            else:
                html = device_page(proxy)
            if html is None:
                return self.reply(b"The device is not reachable on the network right now.\n", 502, "text/plain")
            return self.reply(portal_page(html, proxy), ctype="text/html; charset=utf-8")
        if name not in (RELAY_GET if method == "GET" else RELAY_POST):
            return self.reply(b"not found\n", 404, "text/plain")
        length = int(self.headers.get("Content-Length", 0) or 0)
        body = self.rfile.read(length) if method == "POST" else None
        headers = {key: self.headers[key] for key in ("X-Octo-Token", "X-Octo-Link", "Content-Type")
                   if self.headers.get(key)}
        try:
            status, reply_headers, data = device_request(resolve(proxy["host"]) or proxy["host"], method,
                                                         "/" + name, body, headers,
                                                         timeout=120 if name == "ota" else 60)
        except (OSError, http.client.HTTPException) as err:
            return self.reply(f"device not reachable: {err}\n".encode(), 502, "text/plain")
        ctype = {k.lower(): v for k, v in reply_headers.items()}.get("content-type", "text/plain")
        return self.reply(data, status, ctype)

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
        if path == "/api/auth":
            return self.reply({"standalone": STANDALONE, "password_set": STANDALONE and password_set(),
                               "logged_in": not STANDALONE or session_ok(self.session_token()),
                               "needs_code": STANDALONE and not self.local_client()})
        if path == "/api/catalog":
            return self.reply(catalog())
        if path == "/api/serial":
            return self.reply({"ports": serial_ports()})
        if path == "/api/state":
            with lock:
                current = dict(job)
            proxies = [{k: p.get(k) for k in ("id", "name", "host", "added", "app", "local", "auto_update")} for p in load_proxies()]
            return self.reply({"proxies": proxies, "found": found_unlinked(), "toolchain": toolchain_ready(),
                               "builds": builds(), "job": current,
                               "releases": [image_summary(i) for i in release_state["images"]],
                               "release_factory": [image_summary(i) for i in release_state["factory"]],
                               "sources": release_state["sources"],
                               "packages": release_state["packages"],
                               "release": release_state["release"], "release_error": release_state["error"],
                               "ignored": [{"id": k, **v} for k, v in ignored_devices().items()],
                               "share_github": bool(github_client_id()),
                               "settings": load_settings(), "system": system_info(), "standalone": STANDALONE})
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
                return self.reply(checked_status(proxy))
            if parts[3] == "logs":
                query = urllib.parse.parse_qs(self.path.partition("?")[2])
                return self.reply(device_log(proxy, int((query.get("from") or ["0"])[0] or 0),
                                             (query.get("boot") or [""])[0]))
            if parts[3] == "token":
                return self.reply({"token": proxy["token"]})
            if parts[3] == "health":
                return self.reply(health_reply(proxy))
            if parts[3] == "in_ha":  # whether Home Assistant has a Matter device at its address
                return self.reply({"known": ha_knows_device(proxy)})
        if len(parts) == 3 and parts[:2] == ["api", "link"]:
            return self.reply(link_poll(parts[2]))
        self.reply({"error": "not found"}, 404)

    def post(self):
        path = self.path.split("?")[0]
        data = self.body()
        if path.startswith("/api/auth/"):
            return self.auth_post(path, data)
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
                answers = [send_line(proxy, line) for line in lines]
                ok = all(status == 200 and not text.startswith("error") for status, text in answers)
                results.append({"id": proxy["id"], "name": proxy["name"], "ok": ok,
                                "text": " ".join(text.strip() for _, text in answers)})
            return self.reply({"results": results})
        if path == "/api/bulk/update":
            started = start_job("update", "all", update_all)
            return self.reply({"started": started})
        if path == "/api/releases/check":
            check_releases()
            return self.reply({"ok": release_state["error"] is None, "error": release_state["error"]})
        if path == "/api/board":
            # Which ESP32 is on a USB port of this machine, for the wizard
            port = str(data.get("port", ""))
            if port not in [p["path"] for p in serial_ports()]:
                return self.reply({"error": "unknown USB port"}, 400)
            with lock:
                busy = job["running"]
            if busy:
                return self.reply({"error": "busy with another job"}, 409)
            try:
                board, _ = read_board(port)
            except (OSError, subprocess.SubprocessError) as err:
                return self.reply({"error": str(err)}, 502)
            return self.reply({"board": board})
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
            if "night_hour" in data and isinstance(data["night_hour"], int) and 0 <= data["night_hour"] <= 23:
                settings["night_hour"] = data["night_hour"]
            save_settings(settings)
            return self.reply(settings)
        if path == "/api/share/start":
            return self.reply(share_start(str(data.get("slug", "")), str(data.get("repo", ""))))
        if path == "/api/share/poll":
            return self.reply(share_poll(str(data.get("id", ""))))
        if path == "/api/found/ignore":
            device_id = str(data.get("id", ""))
            if not re.match(r"^[0-9A-Fa-f]{6,32}$", device_id):
                return self.reply({"error": "no such device"}, 400)
            set_ignored(device_id, bool(data.get("on", True)))
            return self.reply({"ok": True})
        if path == "/api/toolchain":
            started = start_job("toolchain", "Installing the toolchain", lambda: run_script("toolchain.sh"))
            return self.reply({"started": started})
        if path == "/api/build":
            if not toolchain_ready():
                return self.reply({"error": "install the toolchain first"}, 400)
            # The generic device for another chip or network than the default
            # (ESP32-C6 on Thread): {"chip": "esp32"}, {"network": "wifi"}
            chip, network = str(data.get("chip", "")), str(data.get("network", ""))
            if chip not in ("", *BUILD_CHIPS) or network not in ("", "thread", "wifi"):
                return self.reply({"error": f"chip one of {', '.join(BUILD_CHIPS)}, network thread or wifi"}, 400)
            env = {k: v for k, v in (("MB_CHIP", chip), ("MB_NETWORK", network)) if v}
            started = start_job("build", "generic", lambda: run_script("build.sh", "generic", env=env))
            return self.reply({"started": started})
        parts = path.strip("/").split("/")
        if len(parts) == 4 and parts[:2] == ["api", "proxy"]:
            proxy = find_proxy(parts[2])
            if not proxy:
                return self.reply({"error": "unknown proxy"}, 404)
            if parts[3] == "health":  # measure now
                health_sample(proxy)
                return self.reply(health_reply(proxy))
            if parts[3] == "auto":
                remember(proxy, auto_update=bool(data.get("on")))
                return self.reply({"ok": True, "auto_update": proxy.get("auto_update")})
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
                if STANDALONE:
                    return self.reply({"error": "only in the Home Assistant add-on"}, 400)
                started = start_job("matter", proxy["name"], lambda: add_to_ha(proxy))
                return self.reply({"started": started})
            if parts[3] == "name_parts":
                if STANDALONE:
                    return self.reply({"error": "only in the Home Assistant add-on"}, 400)
                node_id = data.get("node_id")
                started = start_job("matter", proxy["name"], lambda: name_parts(proxy, node_id))
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
    threading.Thread(target=night_loop, daemon=True).start()
    threading.Thread(target=health_loop, daemon=True).start()
    threading.Thread(target=found_sweep, daemon=True).start()
    fetch_server = Server(("::", FETCH_PORT), FetchHandler)
    threading.Thread(target=fetch_server.serve_forever, daemon=True).start()
    print(f"Matter Builder on port {PORT}, updates fetched from port {FETCH_PORT}", flush=True)
    if STANDALONE and not password_set():
        print(f"No password yet: choose one on the page. From another machine it asks for this setup code: {SETUP_CODE}", flush=True)
    Server(("::", PORT), Handler).serve_forever()
