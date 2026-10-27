#!/bin/sh
# PC check of ps3/gx's fragment and vertex program generators against cgcomp, in ps3dev-latest:latest.
ROOT="$(cd "$(dirname "$0")/../../.." && pwd -W 2>/dev/null || pwd)"
MSYS_NO_PATHCONV=1 docker run --rm -v "$ROOT:/src" -w /src/ps3/tools/rsxcheck ps3dev-latest:latest sh -c '
  GX=../../gx
  mkdir -p build && rm -f build/case*
  gcc -O1 -Wall -Wno-unused -I../../../libs/dolphin/include -o build/rsxcheck rsxcheck.c \
    $GX/fpgen.c $GX/tevgen.c $GX/vpgen.c $GX/xfgen.c -lm || exit 1
  build/rsxcheck gen build || exit 1
  for a in build/case*.fasm; do
    [ -e "$a" ] && { /usr/local/ps3dev/bin/cgcomp -a -f "$a" "${a%.fasm}.fpo" > /dev/null || echo "cgcomp failed on $a"; }
  done
  for a in build/case*.vasm; do
    [ -e "$a" ] && { /usr/local/ps3dev/bin/cgcomp -a -v "$a" "${a%.vasm}.vpo" > /dev/null || echo "cgcomp failed on $a"; }
  done
  build/rsxcheck cmp build
'
