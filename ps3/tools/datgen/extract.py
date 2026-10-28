#!/usr/bin/env python3
"""Extracts a GameCube disc image to orig/GALE01: files/ for melee-dat, sys/main.dol for the PS3 build's fonts.
Usage: extract.py Melee.iso orig/GALE01"""
import os
import struct
import sys


def extract_dol(f, out):
    f.seek(0x420)
    dol_off = struct.unpack(">I", f.read(4))[0]
    f.seek(dol_off)
    header = f.read(0x100)
    offsets = struct.unpack_from(">18I", header, 0)
    sizes = struct.unpack_from(">18I", header, 0x90)
    end = max([0x100] + [o + s for o, s in zip(offsets, sizes) if s])
    f.seek(dol_off)
    os.makedirs(os.path.join(out, "sys"), exist_ok=True)
    with open(os.path.join(out, "sys", "main.dol"), "wb") as g:
        g.write(f.read(end))


def extract_files(f, out):
    f.seek(0x424)
    fst_off, fst_size = struct.unpack(">II", f.read(8))
    f.seek(fst_off)
    fst = f.read(fst_size)
    count = struct.unpack_from(">I", fst, 8)[0]
    names = fst[count * 12 :]
    # Directory entries hold the index after their last child; parents end where their children do.
    ends = [(count, os.path.join(out, "files"))]
    os.makedirs(ends[0][1], exist_ok=True)
    written = 0
    for i in range(1, count):
        while ends[-1][0] <= i:
            ends.pop()
        word, offset, size = struct.unpack_from(">III", fst, i * 12)
        name_off = word & 0xFFFFFF
        name = names[name_off : names.index(b"\0", name_off)].decode("shift_jis")
        path = os.path.join(ends[-1][1], name)
        if word >> 24:
            os.makedirs(path, exist_ok=True)
            ends.append((size, path))
        else:
            f.seek(offset)
            with open(path, "wb") as g:
                g.write(f.read(size))
            written += 1
    return written


def main():
    image, out = sys.argv[1], sys.argv[2]
    with open(image, "rb") as f:
        extract_dol(f, out)
        print(f"sys/main.dol, {extract_files(f, out)} files")


if __name__ == "__main__":
    main()
