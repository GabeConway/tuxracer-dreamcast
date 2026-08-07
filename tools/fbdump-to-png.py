#!/usr/bin/env python3
"""Turn a guest FBDUMP out of a harness console log into a PNG.

    bash tools/fbdump.sh                       # build, run, decode, in one step
    python3 tools/fbdump-to-png.py console.log out.png     # decode by hand

The guest side is dc/src/dc_fbdump.c. It prints:

    FBDUMP begin 320x240 rgb565
    FBROW <base64 of one row, RGB565 little-endian>
    ... one line per row ...
    FBDUMP end

A truncated dump still decodes — missing rows come out black — because the
common reason to truncate is a hang, and a partial frame is exactly what you
want to look at in that case.

No dependencies: writes an uncompressed-deflate PNG with the stdlib.
"""

import base64
import re
import struct
import sys
import zlib

BEGIN = re.compile(r"FBDUMP begin (\d+)x(\d+) rgb565")


def decode(log_text):
    m = BEGIN.search(log_text)
    if not m:
        raise SystemExit("no 'FBDUMP begin' line in the log — was the build "
                         "made with -DTR_FBDUMP_FRAME=<n>?")
    w, h = int(m.group(1)), int(m.group(2))

    rows = []
    for line in log_text[m.end():].splitlines():
        if line.startswith("FBDUMP end"):
            break
        if not line.startswith("FBROW "):
            continue          # interleaved game logging is expected, skip it
        payload = line[6:].strip()
        # A row can arrive short: the runner kills Flycast on its own schedule
        # and the emulated SCIF drops characters under load. Pad to a legal
        # base64 length and keep whatever decoded rather than losing the frame.
        payload += "=" * (-len(payload) % 4)
        try:
            raw = base64.b64decode(payload, validate=False)
        except Exception:
            raw = b""
        rows.append(raw[:w * 2].ljust(w * 2, b"\x00"))

    if not rows:
        raise SystemExit("found the header but no FBROW lines")
    return w, h, rows


def rgb565_to_rgb888(raw, w):
    out = bytearray(w * 3)
    for x in range(w):
        px = raw[x * 2] | (raw[x * 2 + 1] << 8)      # little-endian
        r = (px >> 11) & 0x1F
        g = (px >> 5) & 0x3F
        b = px & 0x1F
        # Replicate the high bits into the low ones so full-scale stays full
        # scale (31 -> 255, not 248).
        out[x * 3 + 0] = (r << 3) | (r >> 2)
        out[x * 3 + 1] = (g << 2) | (g >> 4)
        out[x * 3 + 2] = (b << 3) | (b >> 2)
    return bytes(out)


def write_png(path, w, h, rows):
    scanlines = bytearray()
    black = b"\x00" * (w * 3)
    for y in range(h):
        scanlines.append(0)                          # filter type: none
        scanlines += rgb565_to_rgb888(rows[y], w) if y < len(rows) else black

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(scanlines), 9))
    png += chunk(b"IEND", b"")

    with open(path, "wb") as f:
        f.write(png)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: fbdump-to-png.py <console.log> <out.png>")
    with open(sys.argv[1], errors="replace") as f:
        text = f.read()
    w, h, rows = decode(text)
    write_png(sys.argv[2], w, h, rows)
    print("%s: %dx%d, %d of %d rows present" %
          (sys.argv[2], w, h, len(rows), h))


if __name__ == "__main__":
    main()
