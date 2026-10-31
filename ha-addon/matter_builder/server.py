"""Matter Builder: the add-on's web page (Home Assistant ingress) and API.

Proxies are linked with a token of the add-on's own (POST /link on the
proxy: the page password plus a press of its BOOT button); the password is
never stored. Builds and updates run one at a time as a "job" with a log.
Only the standard library: the image is Espressif's ESP-IDF image.
"""

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
SUPERVISOR_TOKEN = os.environ.get("SUPERVISOR_TOKEN", "")
FIRMWARE = DATA / "firmware"
JOB_LOG = DATA / "job.log"

lock = threading.Lock()
job = {"kind": None, "running": False, "ok": None, "started": 0, "title": ""}
links = {}  # link id -> request on its way (the password only lives here)
found = {}  # device id -> a Matter Builder device seen on the network


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
    """A name from what the proxy knows: its beds ("Alex & Sam")."""
    status, text = request(resolve(host) or host, "/cmd", token, b"json")
    try:
        names = [b["name"] for b in json.loads(text)["beds"] if b.get("configured") and b.get("name")]
    except (ValueError, KeyError, TypeError):
        names = []
    return " & ".join(names) if status == 200 and names else host


def proxy_status(proxy):
    status, system = command(proxy, "system")
    if status == 401:
        return {"ok": False, "auth": True, "error": "key no longer accepted"}
    if status != 200:
        return {"ok": False, "error": system.strip() or f"HTTP {status}"}
    _, beds = command(proxy, "status")
    if not proxy.get("device_id"):
        remember_device_id(proxy)
    return {"ok": True, "system": system, "beds": beds}


def remember_device_id(proxy):
    """Proxies linked before /info existed learn their id once the firmware has it."""
    device = device_info(resolve(proxy["host"]) or proxy["host"])
    if not device:
        return
    proxies = load_proxies()
    for p in proxies:
        if p["id"] == proxy["id"]:
            p["device_id"] = device["id"]
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


def run_script(name):
    with JOB_LOG.open("a") as out:
        result = subprocess.run(["bash", str(HERE / name)], stdout=out, stderr=subprocess.STDOUT, env=os.environ.copy())
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
                 "token": reply["token"], "added": int(time.time()), "device_id": device.get("id")}
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
        if path == "/api/state":
            with lock:
                current = dict(job)
            proxies = [{k: p[k] for k in ("id", "name", "host", "added")} for p in load_proxies()]
            return self.reply({"proxies": proxies, "found": found_unlinked(), "toolchain": toolchain_ready(),
                               "builds": builds(), "job": current,
                               "releases": [], "settings": load_settings(), "system": system_info()})
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
            started = start_job("build", "Firmware bouwen", lambda: run_script("build.sh"))
            return self.reply({"started": started})
        parts = path.strip("/").split("/")
        if len(parts) == 4 and parts[:2] == ["api", "proxy"]:
            proxy = find_proxy(parts[2])
            if not proxy:
                return self.reply({"error": "unknown proxy"}, 404)
            if parts[3] == "update":
                image = FIRMWARE / Path(str(data.get("file", ""))).name
                if not image.is_file():
                    return self.reply({"error": "unknown firmware"}, 400)
                started = start_job("update", f"{proxy['name']} bijwerken", lambda: update_proxy(proxy, image))
                return self.reply({"started": started})
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
    print(f"Matter Builder on port {PORT}", flush=True)
    Server(("::", PORT), Handler).serve_forever()
