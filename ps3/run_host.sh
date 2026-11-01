#!/bin/sh
# The host game build under qemu-ppc64 in ppc64-qemu-test:2, MELEE_HOST_* from the environment (tests/sys_host.c).
# MELEE_ISO_DIR: the folder whose games/ holds the disc image. A 4 GB guest space, as the PS3's user addresses.
ROOT="$(cd "$(dirname "$0")/.." && pwd -W 2>/dev/null || pwd)"
ISO_DIR="${MELEE_ISO_DIR:-E:/Users/Matteo/Desktop/Emulatori/PS3/dev_hdd0/game/MELE00001/USRDIR}"
MSYS_NO_PATHCONV=1 docker run --rm -v "$ROOT:/src" -v "$ISO_DIR:/scratch:ro" -w /src/ps3 \
    -e MELEE_HOST_SECONDS -e MELEE_HOST_DRAW -e MELEE_HOST_DUMP -e MELEE_HOST_DUMP_EVERY -e MELEE_HOST_DUMP_FROM -e MELEE_HOST_PAD \
    -e MELEE_HOST_MATCH -e MELEE_HOST_RB -e MELEE_HOST_RB_TRACE -e MELEE_HOST_SEED -e MELEE_HOST_RB_RENDERDIFF -e MELEE_HOST_RB_HASHAT -e MELEE_HOST_RB_FIRST ppc64-qemu-test:2 qemu-ppc64 -R 0x100000000 build/linux/game/melee_host "$@"
