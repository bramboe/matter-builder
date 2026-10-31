#!/usr/bin/env python3
"""Hand a Matter OTA image to Home Assistant's Matter server (matterjs-server).

Run it where the server's port 5580 is reachable, e.g. in the "Advanced SSH &
Web Terminal" app (host network):

    python3 ota_upload.py /share/octo-ota/octo_bridge-ota.bin
    python3 ota_upload.py --check 81          # does the server offer an update to node 81?

The server only serves test images (vendor 0xFFF1) when the Matter Server app
runs with "Enable test-net DCL usage". Standard library only: the SSH app has
python3 but no extra packages.
"""

import argparse
import base64
import json
import os
import socket
import struct
import sys
import urllib.error
import urllib.parse
import urllib.request


class WebSocket:
    """Just enough of RFC 6455 for the Matter server's JSON messages."""

    def __init__(self, url, timeout=30):
        parts = urllib.parse.urlsplit(url)
        self.sock = socket.create_connection((parts.hostname, parts.port or 80), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        request = (
            f"GET {parts.path or '/'} HTTP/1.1\r\n"
            f"Host: {parts.hostname}:{parts.port or 80}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(request.encode())
        header = b""
        while b"\r\n\r\n" not in header:
            chunk = self.sock.recv(1024)
            if not chunk:
                raise ConnectionError("server closed the connection during the handshake")
            header += chunk
        status, _, self.buffer = header.partition(b"\r\n\r\n")
        status_line = status.split(b"\r\n")[0]
        if b" 101 " not in status_line:
            raise ConnectionError("no websocket: " + status_line.decode(errors="replace"))

    def _read(self, n):
        while len(self.buffer) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("server closed the connection")
            self.buffer += chunk
        data, self.buffer = self.buffer[:n], self.buffer[n:]
        return data

    def send(self, message):
        payload = json.dumps(message).encode()
        mask = os.urandom(4)
        if len(payload) < 126:
            header = struct.pack("!BB", 0x81, 0x80 | len(payload))
        elif len(payload) < 65536:
            header = struct.pack("!BBH", 0x81, 0x80 | 126, len(payload))
        else:
            header = struct.pack("!BBQ", 0x81, 0x80 | 127, len(payload))
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(header + mask + masked)

    def receive(self):
        message = b""
        while True:
            first, second = self._read(2)
            length = second & 0x7F
            if length == 126:
                (length,) = struct.unpack("!H", self._read(2))
            elif length == 127:
                (length,) = struct.unpack("!Q", self._read(8))
            payload = self._read(length)
            opcode = first & 0x0F
            if opcode == 0x8:
                raise ConnectionError("server closed the websocket")
            if opcode == 0x9:  # ping: answer and keep reading
                self.sock.sendall(struct.pack("!BB", 0x8A, 0x80) + os.urandom(4))
                continue
            message += payload
            if first & 0x80:
                return json.loads(message)

    def call(self, command, args, message_id):
        self.send({"message_id": message_id, "command": command, "args": args})
        while True:
            reply = self.receive()
            if reply.get("message_id") != message_id:
                continue  # server info or events
            if "error_code" in reply:
                raise RuntimeError(f"{command}: error {reply['error_code']}: {reply.get('details', reply)}")
            return reply.get("result")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("image", nargs="?", help="Matter OTA image (.ota / *-ota.bin)")
    parser.add_argument("--check", type=int, metavar="NODE_ID", help="run check_node_update for this node")
    parser.add_argument("--server", default="http://localhost:5580", help="Matter server (default %(default)s)")
    options = parser.parse_args()
    if not options.image and options.check is None:
        parser.error("give an image to upload and/or --check NODE_ID")

    ws_url = options.server.replace("http://", "ws://").rstrip("/") + "/ws"
    ws = WebSocket(ws_url)
    info = ws.receive()  # the server greets with its info
    print(f"Matter server {info.get('sdk_version', '?')}, schema {info.get('schema_version', '?')}")

    if options.image:
        with open(options.image, "rb") as f:
            image = f.read()
        reservation = ws.call("initiate_ota_upload", {}, "upload")
        if len(image) > reservation["max_size"]:
            sys.exit(f"image is {len(image)} bytes, the server takes at most {reservation['max_size']}")
        # The websocket stays open during the POST: the id is only accepted from the client that reserved it.
        request = urllib.request.Request(
            f"{options.server.rstrip('/')}/ota-upload/{reservation['upload_id']}",
            data=image,
            headers={"Content-Type": "application/octet-stream"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=120) as response:
                print("stored:", json.dumps(json.load(response), indent=2))
        except urllib.error.HTTPError as e:
            sys.exit(f"upload refused: HTTP {e.code}: {e.read().decode(errors='replace')}")

    if options.check is not None:
        update = ws.call("check_node_update", {"node_id": options.check}, "check")
        print("update for node", options.check, ":", json.dumps(update, indent=2) if update else "none")


if __name__ == "__main__":
    main()
