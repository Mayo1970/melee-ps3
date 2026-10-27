#!/bin/sh
# TOC size of the probe links: .got of each ELF, r2off stubs, and the .toc input per object (upper bound).
B=$1
P=$2
shift 2
for e in multi single; do
    f="$B/toc_$e.elf"
    if [ -f "$f" ]; then
        # "[ 7]" splits into two fields and "[17]" into one, so find the name first.
        got=$("${P}readelf" -SW "$f" | awk '{ for (i = 1; i < NF; i++) if ($i == ".got") print $(i + 4) }')
        echo "$e: .got 0x$got, r2off stubs $("${P}nm" "$f" | grep -c r2off)"
    else
        echo "$e: no ELF; first errors:"
        grep -m 5 -E 'error|truncated' "$B/toc_$e.log"
    fi
done
"${P}size" -A "$@" 2> /dev/null | awk '$1 == ".toc" { t += $2 } END { printf ".toc input over all objects: %d bytes (0x%x)\n", t, t }'
