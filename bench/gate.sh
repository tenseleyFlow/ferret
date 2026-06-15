#!/bin/sh
# Perf gate: parse a hyperfine CSV and fail unless ferret is faster than find.
# Rule: ferret * margin <= find  (margin default 1.00 = strictly faster).
# Usage: gate.sh results.csv [mean|min] [label]
#   mean (default): reliable above ~10ms.  min: noise-robust below ~10ms.
# Env: FRT_PERF_MARGIN overrides the margin.
set -u
csv=${1:?usage: gate.sh results.csv [mean|min] [label]}
metric=${2:-mean}
label=${3:-}
margin=${FRT_PERF_MARGIN:-1.00}

# hyperfine CSV header: command,mean,stddev,median,user,system,min,max
col=2
[ "$metric" = min ] && col=7

# Match the binary on the FIRST token of the command, so a corpus path containing
# "ferret" or "find" can't be mistaken for the tool's row.
a=$(awk -F, -v c="$col" 'NR>1 { split($1,w," "); n=split(w[1],q,"/"); b=q[n];
	if (b=="ferret" || b=="frt") { print $c; exit } }' "$csv")
t=$(awk -F, -v c="$col" 'NR>1 { split($1,w," "); n=split(w[1],q,"/"); b=q[n];
	if (b=="find" || b ~ /^find-/) { print $c; exit } }' "$csv")

if [ -z "$a" ] || [ -z "$t" ]; then
	echo "PERF GATE $label: could not find both rows in $csv (ferret=$a find=$t)"
	exit 1
fi

verdict=$(awk -v a="$a" -v t="$t" -v m="$margin" 'BEGIN { printf (a * m <= t) ? "PASS" : "FAIL" }')
speedup=$(awk -v a="$a" -v t="$t" 'BEGIN { if (a > 0) printf "%.2f", t / a; else printf "inf" }')
printf 'PERF GATE %s: ferret=%ss find=%ss (%sx, %s) -> %s\n' "$label" "$a" "$t" "$speedup" "$metric" "$verdict"
[ "$verdict" = PASS ]
