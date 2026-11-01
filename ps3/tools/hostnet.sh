#!/bin/sh
# Two host instances play a netplay match in one container, paired by tools/matchmaker.py on the PC; logs in
# build/hostnet. Environment: MELEE_HOST_MATCH, _SECONDS, _NET_LAG, _NET_DELAY, _NET_NOLAN, _PAD1, _PAD2 (GOAL.md).
cd "$(dirname "$0")/.." || exit 1
ROOT="$(cd .. && pwd -W 2>/dev/null || pwd)"
ISO_DIR="${MELEE_ISO_DIR:-E:/Users/Matteo/Desktop/Emulatori/PS3/dev_hdd0/game/MELE00001/USRDIR}"
mkdir -p build/hostnet

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
python tools/matchmaker.py --port 51001 > build/hostnet/matchmaker.txt 2>&1 &
MM=$!
MSYS_NO_PATHCONV=1 docker run --rm -v "$ROOT:/src" -v "$ISO_DIR:/scratch:ro" -w /src/ps3 \
    -e MELEE_HOST_MATCH="${MELEE_HOST_MATCH:-2h,12h/31/0}" -e MELEE_HOST_SECONDS="${MELEE_HOST_SECONDS:-120}" \
    -e MELEE_HOST_NET_CODE=12345678 -e MELEE_HOST_NET_LAG -e MELEE_HOST_NET_DELAY -e MELEE_HOST_NET_NOLAN \
    -e PAD1="$PAD1" -e PAD2="$PAD2" \
    ppc64-qemu-test:2 sh -c '
        export MELEE_HOST_NET_SERVER="$(getent hosts host.docker.internal | cut -d" " -f1):51001"
        MELEE_HOST_NET_PORT=51000 MELEE_HOST_PAD="$PAD1" qemu-ppc64 -R 0x100000000 build/linux/game/melee_host \
            > build/hostnet/p1.txt 2>&1 &
        # The first console on the code is player 1.
        sleep 3
        MELEE_HOST_NET_PORT=51002 MELEE_HOST_PAD="$PAD2" qemu-ppc64 -R 0x100000000 build/linux/game/melee_host \
            > build/hostnet/p2.txt 2>&1 &
        wait'
# devkitPro's MSYS2 Python does not get Git Bash's signals: end it by its Windows PID.
taskkill //PID "$(cat /proc/$MM/winpid)" //F > /dev/null 2>&1 || kill $MM 2> /dev/null
grep -h "netplay: \|desync\|assertion\|OSPanic \|melee: signal" build/hostnet/p1.txt | head -20
