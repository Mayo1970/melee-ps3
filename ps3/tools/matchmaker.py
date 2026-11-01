#!/usr/bin/env python3
# The netplay matchmaker (ps3/net/netplay.c): consoles that send the same 8-digit code get each other's addresses.
# Run it where both reach UDP 51001 (a public IP, or a forwarded port); Python 3 only: python3 matchmaker.py
import argparse
import os
import socket
import struct
import time

MAGIC = b"MLNP"
VERSION = 1
REG, WAIT, PAIR = 1, 2, 3
# A searching console registers each second: one silent this long has left. A pair answers repeats this long.
WAIT_S = 5
PAIRED_S = 30


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def head(kind):
    return MAGIC + bytes([kind, VERSION, 0, 0])


def pair_packet(nonce, session, role, peer_addr, peer_local):
    ip = struct.unpack(">I", socket.inet_aton(peer_addr[0]))[0]
    return head(PAIR) + struct.pack(">IIB3xIH2xIH2x", nonce, session, role, ip, peer_addr[1], peer_local[0],
                                    peer_local[1])


def main():
    ap = argparse.ArgumentParser(description="Pairs PS3 Melee consoles that enter the same netplay code.")
    ap.add_argument("--port", type=int, default=51001, help="UDP port (default 51001)")
    args = ap.parse_args()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", args.port))
    # A blocked recvfrom never sees Ctrl+C or a signal on Windows: wake each second.
    sock.settimeout(1.0)
    log(f"matchmaker on UDP port {args.port}")
    waiting = {}  # code -> the first console: addr, nonce, local address, time
    paired = {}  # nonce -> (addr, PAIR packet, time)
    while True:
        try:
            data, addr = sock.recvfrom(1500)
        except socket.timeout:
            continue
        except ConnectionResetError:
            # Windows reports an earlier send's ICMP "port unreachable" on the next receive.
            continue
        now = time.monotonic()
        for code in [c for c, w in waiting.items() if now - w["t"] > WAIT_S]:
            del waiting[code]
        for nonce in [n for n, p in paired.items() if now - p[2] > PAIRED_S]:
            del paired[nonce]
        if len(data) < 32 or data[:4] != MAGIC or data[4] != REG or data[5] != VERSION:
            continue
        code = data[8:16]
        if not code.isdigit():
            continue
        nonce, local_ip, local_port, build = struct.unpack(">IIH2xI", data[16:32])
        if nonce in paired:
            # Our answer was lost: the console registers again.
            sock.sendto(paired[nonce][1], addr)
            continue
        first = waiting.get(code)
        # The same address is the same console, searching again.
        if first is None or first["nonce"] == nonce or first["addr"] == addr:
            if first is None:
                log(f"{addr[0]}:{addr[1]} waits on a code (build {build:08x})")
            waiting[code] = {"addr": addr, "nonce": nonce, "local": (local_ip, local_port), "t": now}
            sock.sendto(head(WAIT) + struct.pack(">I", nonce), addr)
            continue
        del waiting[code]
        session = struct.unpack(">I", os.urandom(4))[0]
        to_first = pair_packet(first["nonce"], session, 1, addr, (local_ip, local_port))
        to_second = pair_packet(nonce, session, 2, first["addr"], first["local"])
        sock.sendto(to_first, first["addr"])
        sock.sendto(to_second, addr)
        paired[first["nonce"]] = (first["addr"], to_first, now)
        paired[nonce] = (addr, to_second, now)
        log(f"paired {first['addr'][0]}:{first['addr'][1]} (player 1) with {addr[0]}:{addr[1]} (player 2)")


if __name__ == "__main__":
    main()
