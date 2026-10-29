#!/bin/sh
# Names the host game build's addresses (a fault or OSPanic stack line, 0x optional), innermost first.
ROOT="$(cd "$(dirname "$0")/../.." && pwd -W 2>/dev/null || pwd)"
ADDRS=$(echo "$@" | tr ' ' '\n' | grep -E '^(0x)?[0-9a-fA-F]{6,}$' | sed 's/^0x//; s/^/0x/' | tr '\n' ' ')
MSYS_NO_PATHCONV=1 docker run --rm -v "$ROOT:/src" -w /src/ps3 ppc64-qemu-test:2 \
    powerpc64-linux-gnu-addr2line -f -e ${MELEE_HOST_BIN:-build/linux/game/melee_host} $ADDRS | sed -n 'p;n' | tr '\n' ' '
echo
