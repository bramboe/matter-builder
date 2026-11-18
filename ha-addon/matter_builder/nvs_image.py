"""An NVS partition for one new device, written next to its factory image.

The firmware keeps its settings in ESP-IDF's NVS. A factory image leaves the
"nvs" partition empty (0xFF), so the add-on can fill it for one board before
it is installed: its Thread network, its Wi-Fi, the add-on's own key and a
recipe to run at the first start. Release images stay the same for everyone.

This writes NVS format version 2 (ESP-IDF 4 and later), unencrypted, as
ESP-IDF's nvs_partition_gen.py does: 4 KB pages of a 32-byte header, a
32-byte entry bitmap and 126 entries of 32 bytes. Strings end in NUL; blobs
are BLOB_DATA chunks plus a BLOB_IDX entry. The last page stays empty, as
NVS needs one free page. The tests check the result with ESP-IDF's own
parser when it is installed (test/addon/test_provisioning.py).
"""

import struct
import zlib

PAGE = 4096
ENTRIES = 126
ENTRY = 32
FIRST_ENTRY = 64
ACTIVE, FULL = 0xFFFFFFFE, 0xFFFFFFFC
VERSION2 = 0xFE
U8, SZ, BLOB_DATA, BLOB_IDX = 0x01, 0x21, 0x42, 0x48
CHUNK_ANY = 0xFF
MAX_STRING = 124 * 32  # bytes, NUL included: a string and its header stay below a page's 126 entries
MAX_KEY = 15


def crc32(data):
    return zlib.crc32(bytes(data), 0xFFFFFFFF) & 0xFFFFFFFF


class Page:
    def __init__(self, number):
        self.buf = bytearray(b"\xff") * PAGE
        self.used = 0
        header = bytearray(b"\xff") * 32
        struct.pack_into("<II", header, 0, ACTIVE, number)
        header[8] = VERSION2
        struct.pack_into("<I", header, 28, crc32(header[4:28]))
        self.buf[0:32] = header

    def free(self):
        return ENTRIES - self.used

    def put(self, data, count):
        start = FIRST_ENTRY + ENTRY * self.used
        self.buf[start:start + len(data)] = data
        for _ in range(count):
            bit = self.used * 2  # two bits per entry: 10 = written
            self.buf[32 + bit // 8] &= ~(1 << (bit % 8)) & 0xFF
            self.used += 1

    def full(self):
        struct.pack_into("<I", self.buf, 0, FULL)


def header(ns, kind, span, chunk, key):
    entry = bytearray(b"\xff") * 32
    entry[0], entry[1], entry[2], entry[3] = ns, kind, span, chunk
    name = key.encode()
    if not name or len(name) > MAX_KEY:
        raise ValueError(f"NVS key {key!r}: 1 to {MAX_KEY} characters")
    entry[8:24] = name + b"\0" * (16 - len(name))
    return entry


def seal(entry):
    struct.pack_into("<I", entry, 4, crc32(entry[0:4] + entry[8:32]))
    return entry


class Image:
    """Namespaces with u8, string and blob values, as one NVS partition."""

    def __init__(self):
        self.items = []  # (namespace, key, kind, value)

    def u8(self, namespace, key, value):
        self.items.append((namespace, key, "u8", int(value) & 0xFF))

    def string(self, namespace, key, value):
        data = str(value).encode() + b"\0"
        if len(data) > MAX_STRING:
            raise ValueError(f"NVS string {key!r} is longer than {MAX_STRING - 1} bytes")
        self.items.append((namespace, key, "str", data))

    def blob(self, namespace, key, value):
        self.items.append((namespace, key, "blob", bytes(value)))

    def build(self, size):
        if size % PAGE or size < 2 * PAGE:
            raise ValueError("an NVS partition is two or more 4 KB pages")
        pages = [Page(0)]

        def room(entries):
            if pages[-1].free() < entries:
                pages[-1].full()
                pages.append(Page(len(pages)))
            return pages[-1]

        namespaces = {}
        for namespace, key, kind, value in self.items:
            if namespace not in namespaces:
                namespaces[namespace] = len(namespaces) + 1
                if len(namespaces) > 254:
                    raise ValueError("too many NVS namespaces")
                entry = header(0, U8, 1, CHUNK_ANY, namespace)
                entry[24] = namespaces[namespace]
                room(1).put(seal(entry), 1)
            ns = namespaces[namespace]
            if kind == "u8":
                entry = header(ns, U8, 1, CHUNK_ANY, key)
                entry[24] = value
                room(1).put(seal(entry), 1)
            elif kind == "str":
                count = (len(value) + 31) // 32
                entry = header(ns, SZ, count + 1, CHUNK_ANY, key)
                struct.pack_into("<H", entry, 24, len(value))
                struct.pack_into("<I", entry, 28, crc32(value))
                room(count + 2).put(seal(entry) + value, count + 1)  # as nvs_partition_gen: one entry spare
            else:
                chunks, offset = 0, 0
                while True:
                    if pages[-1].free() < 2:
                        room(ENTRIES)
                    page = pages[-1]
                    part = value[offset:offset + (page.free() - 1) * ENTRY]
                    count = (len(part) + 31) // 32
                    entry = header(ns, BLOB_DATA, count + 1, chunks, key)
                    struct.pack_into("<H", entry, 24, len(part))
                    struct.pack_into("<I", entry, 28, crc32(part))
                    page.put(seal(entry) + part, count + 1)
                    chunks += 1
                    offset += len(part)
                    if offset >= len(value):
                        break
                entry = header(ns, BLOB_IDX, 1, CHUNK_ANY, key)
                struct.pack_into("<I", entry, 24, len(value))
                entry[28], entry[29] = chunks, 0
                room(1).put(seal(entry), 1)
        if len(pages) > size // PAGE - 1:  # one page stays free for NVS itself
            raise ValueError("the settings do not fit the device's NVS partition")
        out = b"".join(bytes(p.buf) for p in pages)
        return out + b"\xff" * (size - len(out))


# ---------------------------------------------------------------- partition table

TABLE_OFFSET = 0x8000
TABLE_MAGIC = b"\xaa\x50"


def partitions(image):
    """The partition table of a factory image: [{name, type, subtype, offset, size}]."""
    table = []
    for i in range(TABLE_OFFSET, TABLE_OFFSET + 0xC00, 32):
        row = image[i:i + 32]
        if len(row) < 32 or row[0:2] != TABLE_MAGIC:
            break
        kind, subtype, offset, size = struct.unpack_from("<BBII", row, 2)
        table.append({"name": row[12:28].split(b"\0")[0].decode(errors="replace"), "type": kind,
                      "subtype": subtype, "offset": offset, "size": size})
    return table


def nvs_partition(image):
    """(offset, size) of the image's "nvs" partition, or None."""
    for p in partitions(image):
        if p["type"] == 1 and p["subtype"] == 2 and p["name"] == "nvs":
            return p["offset"], p["size"]
    return None


def with_nvs(image, nvs):
    """The factory image with this NVS partition in its place. Only into a
    part that is still empty (0xFF), so nothing of the image is overwritten."""
    where = nvs_partition(image)
    if where is None:
        raise ValueError("the firmware has no NVS partition in its partition table")
    offset, size = where
    if len(nvs) != size:
        raise ValueError("the NVS image does not match the partition's size")
    padded = bytearray(image) + b"\xff" * max(0, offset + size - len(image))
    if any(b != 0xFF for b in padded[offset:offset + size]):
        raise ValueError("the factory image already holds data where its NVS partition is")
    padded[offset:offset + size] = nvs
    return bytes(padded)
