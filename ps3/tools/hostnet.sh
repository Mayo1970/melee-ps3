#!/bin/sh
# Two host instances play a netplay match in one container: p1 hosts (no UPnP there: its code is its LAN address),
# p2 joins; logs in build/hostnet (or MELEE_HOST_NET_OUT). Environment: MELEE_HOST_MATCH1/2, _SECONDS, _NET_LAG,
# _NET_DELAY, _PAD1/2.
cd "$(dirname "$0")/.." || exit 1
ROOT="$(cd .. && pwd -W 2>/dev/null || pwd)"
ISO_DIR="${MELEE_ISO_DIR:-E:/Users/Matteo/Desktop/Emulatori/PS3/dev_hdd0/game/MELE00001/USRDIR}"
OUT="${MELEE_HOST_NET_OUT:-build/hostnet}"
mkdir -p "$OUT"

# A pad script of n steps from retrace 700, a new input every few retraces; seed changes the pattern.
script() {
    awk -v seed="$1" 'BEGIN {
        split("SL SR SU SD A B X Y R Z SL+A SR+A SU+A SD+A SL+B SR+B SU+B CL CR CU CD L SL+X SR+Y", in_, " ");
        n = 25; at = 700; out = "";
        for (i = 0; i < 240; i++) {
            k = (i * 7 + seed * 11 + int(i / 5) * seed) % n + 1;
            len = 3 + (i * 5 + seed) % 9;
            out = out (i ? "," : "") at ":" in_[k] ":" len;
            at += len + 2 + (i * 3 + seed) % 7;
        }
        print out;
    }'
}

PAD1="${MELEE_HOST_PAD1:-$(script 3)}"
PAD2="${MELEE_HOST_PAD2:-$(script 8)}"
# Each instance's own fighter is its port 1; the match takes p1's stage and rules.
MSYS_NO_PATHCONV=1 docker run --rm -v "$ROOT:/src" -v "$ISO_DIR:/scratch:ro" -w /src/ps3 \
    -e MATCH1="${MELEE_HOST_MATCH1:-2,9/31/0}" -e MATCH2="${MELEE_HOST_MATCH2:-12,5/8/0}" \
    -e MELEE_HOST_SECONDS="${MELEE_HOST_SECONDS:-120}" \
    -e MELEE_HOST_NET_LAG -e MELEE_HOST_NET_DELAY -e PAD1="$PAD1" -e PAD2="$PAD2" -e OUT="$OUT" \
    ppc64-qemu-test:2 sh -c '
        CODE=$(hostname -i | awk "{ split(\$1, a, \".\"); printf \"%02X%02X%02X%02X\", a[1], a[2], a[3], a[4] }")
        MELEE_HOST_MATCH="$MATCH1" MELEE_HOST_NET_HOST=1 MELEE_HOST_NET_PORT=51000 MELEE_HOST_PAD="$PAD1" \
            qemu-ppc64 -R 0x100000000 build/linux/game/melee_host > "$OUT/p1.txt" 2>&1 &
        sleep 3
        MELEE_HOST_MATCH="$MATCH2" MELEE_HOST_NET_JOIN=$CODE MELEE_HOST_NET_PORT=51002 MELEE_HOST_PAD="$PAD2" \
            qemu-ppc64 -R 0x100000000 build/linux/game/melee_host > "$OUT/p2.txt" 2>&1 &
        wait'
grep -h "netplay: \|desync\|assertion\|OSPanic \|melee: signal" "$OUT/p1.txt" | grep -v heartbeat | head -20
