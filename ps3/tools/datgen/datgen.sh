#!/bin/sh
# The native .dat schema in ps3/build/datgen: the PPC DWARF build, melee-dat, types.bin, `native codegen`.
# From the repo root: docker run --rm -v "$PWD:/src" -w /src melee-datgen:1 sh ps3/tools/datgen/datgen.sh
set -e
OUT=ps3/build/datgen
# The build trees stay in the container: the bind mount is slow for many small files.
WORK=/tmp/datgen
mkdir -p "$OUT" "$WORK"
cmake -S . -B "$WORK/dwarf" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/ppc32-clang.cmake" \
    -DMELEE_DWARF=ON "-DCMAKE_C_FLAGS=-include $PWD/ps3/tools/datgen/gx_wg.h"
cmake --build "$WORK/dwarf"
cargo build --release -p melee-dat --target-dir "$WORK/cargo"
DAT="$WORK/cargo/release/melee-dat"
"$DAT" types export --dwarf "$WORK/dwarf/melee.elf" -o "$OUT/types.bin"
rm -rf "$OUT/schema"
"$DAT" native codegen config/GALE01/dat.yml -p . --types "$OUT/types.bin" --files orig/GALE01/files \
    --out-dir "$OUT/schema"
cp "$DAT" "$OUT/melee-dat"
ls -l "$OUT" "$OUT/schema" | head -40
