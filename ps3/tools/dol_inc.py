#!/usr/bin/env python3
"""Writes the bytes at a DOL address as a C initializer list, as decomp-toolkit's raw headers (config.yml).
Usage: dol_inc.py main.dol 0x8040CD40 0x23E00 out.inc"""
import struct
import sys


def main():
    dol, addr, size, out = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0), sys.argv[4]
    with open(dol, "rb") as f:
        data = f.read()
    offsets = struct.unpack_from(">18I", data, 0)
    addrs = struct.unpack_from(">18I", data, 0x48)
    sizes = struct.unpack_from(">18I", data, 0x90)
    for off, base, length in zip(offsets, addrs, sizes):
        if length and base <= addr and addr + size <= base + length:
            chunk = data[off + addr - base : off + addr - base + size]
            break
    else:
        sys.exit(f"{addr:#x}+{size:#x} is in no DOL section")
    with open(out, "w") as g:
        for i in range(0, size, 16):
            g.write(" ".join(f"0x{b:02X}," for b in chunk[i : i + 16]) + "\n")


if __name__ == "__main__":
    main()
