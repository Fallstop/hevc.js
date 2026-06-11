#!/bin/bash
# bench_ab.sh — Interleaved A/B benchmark of two native decoder binaries.
#
# Usage: ./tools/bench_ab.sh <decoderA> <decoderB> <bitstream.265> [pairs]
#
# Runs A,B,A,B,... so machine load and thermal drift hit both binaries
# equally; the per-pair time ratio stays meaningful even when absolute
# times swing (hyperfine runs each command's batch separately, which
# doesn't protect against drift between the batches).
#
# Measures USER CPU time, not wall time: CPU time is largely immune to
# time-slicing from other processes, so results stay usable on a loaded
# machine. (Core-type scheduling on Apple Silicon can still shift it —
# treat results under heavy load as indicative, and confirm when idle.)

set -e

A="$1"; B="$2"; STREAM="$3"; PAIRS="${4:-10}"
if [ -z "$STREAM" ]; then
    echo "Usage: $0 <decoderA> <decoderB> <bitstream.265> [pairs]"
    exit 1
fi

decode_ms() {
    # Prints user CPU time in ms for one decode
    { /usr/bin/time -p "$1" "$STREAM" -o /dev/null >/dev/null; } 2>&1 |
        awk '/^user/ {printf "%.0f", $2 * 1000}'
}

# Warmup both (page cache, CPU clocks)
decode_ms "$A" >/dev/null
decode_ms "$B" >/dev/null

ratios=()
a_times=()
b_times=()
for i in $(seq 1 "$PAIRS"); do
    ta=$(decode_ms "$A")
    tb=$(decode_ms "$B")
    r=$(echo "$ta $tb" | awk '{printf "%.4f", $1 / $2}')
    a_times+=("$ta"); b_times+=("$tb"); ratios+=("$r")
    printf "  Pair %2d: A %8sms  B %8sms  (A/B %s)\n" "$i" "$ta" "$tb" "$r"
done

median() {
    printf "%s\n" "$@" | sort -n | awk '{v[NR]=$1} END {print v[int((NR+1)/2)]}'
}

ma=$(median "${a_times[@]}")
mb=$(median "${b_times[@]}")
mr=$(median "${ratios[@]}")
echo ""
echo "  A: $A  median ${ma}ms"
echo "  B: $B  median ${mb}ms"
echo "$mr" | awk '{d=($1-1)*100; printf "  Median per-pair ratio A/B: %.3f (B is %.1f%% %s)\n", $1, (d<0?-d:d), (d>=0?"faster":"slower")}'
