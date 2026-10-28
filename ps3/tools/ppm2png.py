#!/usr/bin/env python3
# Converts the host build's P6 PPM dumps to PNG, standard library only: ppm2png.py file.ppm...
import struct
import sys
import zlib


def chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def convert(path):
    with open(path, "rb") as f:
        data = f.read()
    fields = data.split(maxsplit=4)
    w, h = int(fields[1]), int(fields[2])
    pixels = fields[4][: w * h * 3]
    rows = b"".join(b"\0" + pixels[y * w * 3 : (y + 1) * w * 3] for y in range(h))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b"")
    with open(path[: -len(".ppm")] + ".png", "wb") as f:
        f.write(png)


for p in sys.argv[1:]:
    convert(p)
