#!/bin/sh
# Host game matches, 4 at a time: each argument is a MELEE_HOST_MATCH value. A log per match in build/hostmatch,
# then a line per match: its exit status (0: ran its time) and the last panic or fault line.
cd "$(dirname "$0")/.." || exit 1
mkdir -p build/hostmatch
SECONDS_EACH="${MELEE_HOST_SECONDS:-40}"
run() {
    log="build/hostmatch/$(echo "$1" | tr '/,' '_-').txt"
    MELEE_HOST_SECONDS="$SECONDS_EACH" MELEE_HOST_PAD="${MELEE_HOST_PAD:-200:A,300:A,400:A}" MELEE_HOST_MATCH="$1" \
        timeout $((SECONDS_EACH + 120)) ./run_host.sh > "$log" 2>&1
    status=$?
    last=$(grep -E "assertion|OSPanic |melee: signal|qemu:" "$log" | head -2 | tr '\n' ' ')
    echo "$1: exit $status; $last"
}
for m in "$@"; do
    run "$m" &
    while [ "$(jobs -r | wc -l)" -ge 4 ]; do sleep 1; done
done
wait
