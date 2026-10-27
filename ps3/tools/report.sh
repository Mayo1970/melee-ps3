#!/bin/sh
# Compile check summary: failed sources, error kinds, and the warnings that matter for LP64, with top files.
B=${1:-build}
find "$B" -name '*.o.log' | sort > "$B/logs.txt"
echo "sources: $(wc -l < "$B/logs.txt"), failed: $(find "$B" -name '*.o.failed' | wc -l)"
xargs cat < "$B/logs.txt" | awk '
function kind(line) {
    if (line ~ /: error: /) return "error"
    if (line ~ /cast from pointer to integer of different size/) return "pointer-to-int cast"
    if (line ~ /cast to pointer from integer of different size/) return "int-to-pointer cast"
    if (line ~ /(integer from pointer|pointer from integer) without a cast/) return "int-conversion"
    if (line ~ /implicit declaration of function/) return "implicit declaration"
    if (line ~ /incompatible pointer type/) return "incompatible pointer type"
    if (line ~ /\[-Wformat/) return "format"
    if (line ~ /: warning: /) return "other warning"
    return ""
}
{
    k = kind($0)
    if (k == "") next
    n[k]++
    split($0, p, ":")
    files[k, p[1]]++
    if (k == "error" || k == "other warning") {
        m = $0
        sub(/^[^:]*:[0-9]+:[0-9]+: (error|warning): /, "", m)
        gsub(/\047[^\047]*\047/, "X", m)
        msg[k, m]++
    }
}
END {
    split("error|pointer-to-int cast|int-to-pointer cast|int-conversion|implicit declaration|incompatible pointer type|format|other warning", order, "|")
    for (i = 1; i in order; i++) {
        k = order[i]
        printf "\n%s: %d\n", k, n[k]
        cmd = "sort -rn | head -12"
        for (key in files) {
            split(key, q, SUBSEP)
            if (q[1] == k) printf "%6d  %s\n", files[key], q[2] | cmd
        }
        close(cmd)
        if (k == "error" || k == "other warning") {
            print "  kinds:"
            for (key in msg) {
                split(key, q, SUBSEP)
                if (q[1] == k) printf "%6d  %s\n", msg[key], q[2] | cmd
            }
            close(cmd)
        }
    }
}'
