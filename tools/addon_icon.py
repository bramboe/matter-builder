"""Draws the add-on's icon.png (128x128): a chip on the Home Assistant blue.
Only the standard library; edges are smoothed by drawing at 4x and averaging.

    python3 tools/addon_icon.py ha-addon/matter_builder/icon.png
"""

import struct
import sys
import zlib

SIZE, SCALE = 128, 4
BLUE, WHITE = (0, 154, 199), (255, 255, 255)


def rounded(x, y, x0, y0, x1, y1, r):
    """True when (x, y) lies in the rectangle with rounded corners."""
    if not (x0 <= x <= x1 and y0 <= y <= y1):
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def colour(x, y):
    """The colour at (x, y) in 0..128 units, None for transparent."""
    if not rounded(x, y, 0, 0, 128, 128, 24):
        return None
    # Chip body: a white outline with a blue inside and a white core
    if rounded(x, y, 38, 38, 90, 90, 8) and not rounded(x, y, 44, 44, 84, 84, 4):
        return WHITE
    if rounded(x, y, 54, 54, 74, 74, 3):
        return WHITE
    # Three pins on each side
    for p in (50, 64, 78):
        for a0, a1 in ((24, 38), (90, 104)):
            if p - 3 <= x <= p + 3 and a0 <= y <= a1:
                return WHITE
            if p - 3 <= y <= p + 3 and a0 <= x <= a1:
                return WHITE
    return BLUE


def png(width, height, rows):
    def chunk(kind, data):
        return struct.pack("!I", len(data)) + kind + data + struct.pack("!I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    raw = b"".join(b"\0" + bytes(row) for row in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack("!IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main(path):
    rows = []
    for y in range(SIZE):
        row = []
        for x in range(SIZE):
            samples = [colour((x + (i + 0.5) / SCALE), (y + (j + 0.5) / SCALE))
                       for i in range(SCALE) for j in range(SCALE)]
            solid = [s for s in samples if s is not None]
            alpha = round(255 * len(solid) / len(samples))
            rgb = [round(sum(s[c] for s in solid) / len(solid)) if solid else 0 for c in range(3)]
            row += rgb + [alpha]
        rows.append(row)
    with open(path, "wb") as f:
        f.write(png(SIZE, SIZE, rows))


if __name__ == "__main__":
    main(sys.argv[1])
