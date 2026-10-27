#!/bin/sh
# PS3 build in ps3dev-latest:latest. The mount is the repo root, so src/ and libs/ are visible.
ROOT="$(cd "$(dirname "$0")/.." && pwd -W 2>/dev/null || pwd)"
MSYS_NO_PATHCONV=1 docker run --rm -v "$ROOT:/src" -w /src/ps3 ps3dev-latest:latest make "$@"
