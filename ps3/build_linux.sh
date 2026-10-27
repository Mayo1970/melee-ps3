#!/bin/sh
# The compile check with GCC 13 for ppc64 big-endian (see TOOLCHAIN in the Makefile), in ppc64-qemu-test:2.
ROOT="$(cd "$(dirname "$0")/.." && pwd -W 2>/dev/null || pwd)"
MSYS_NO_PATHCONV=1 docker run --rm -v "$ROOT:/src" -w /src/ps3 ppc64-qemu-test:2 make TOOLCHAIN=linux "$@"
