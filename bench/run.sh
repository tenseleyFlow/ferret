#!/bin/sh
# Benchmark driver: hyperfine ferret vs find over benchmark shapes; gate that
# ferret is faster. Gates only when tests/golden/PARITY_ACTIVE exists (the same
# point ferret produces real output) and only on configs golden proves identical.
set -u

here=$(dirname "$0")
root=$(cd "$here/.." && pwd)
cd "$root"

REFTAG=${REFTAG:-4.10.0}
ref="tests/.work/ref/find-$REFTAG"
FERRET="${FERRET:-./ferret}"

if ! command -v hyperfine >/dev/null 2>&1; then
	echo "bench: hyperfine not found — skipping (install it to run the perf gate)"
	exit 0
fi

[ -x "$ref" ] || sh tests/golden/build-ref.sh "$REFTAG" >/dev/null 2>&1 || true
[ -x "$ref" ] || { echo "bench: no reference find; skipping"; exit 0; }

if [ ! -f tests/golden/PARITY_ACTIVE ]; then
	echo "bench: parity gate inactive (no PARITY_ACTIVE) — harness present, nothing to gate yet"
	exit 0
fi

work=bench/.work
mkdir -p "$work"
corpus="$work/corpus"
sh bench/mkclasses.sh "$corpus" >/dev/null

rc=0
bench_one() {
	_lbl=$1; shift
	_csv="$work/m_$_lbl.csv"
	# No -N: the >/dev/null redirect needs a shell. Both tools pay the same sh -c
	# overhead, so the comparison stays fair (output is discarded to time compute).
	hyperfine -w 5 -r 30 --export-csv "$_csv" \
		"$FERRET $* >/dev/null" "$ref $* >/dev/null" >/dev/null 2>&1 || {
		echo "bench: hyperfine failed for $_lbl"; rc=1; return; }
	# below ~10ms use the min metric (noise-robust); else mean.
	tmean=$(awk -F, 'NR>1 { split($1,w," "); n=split(w[1],q,"/"); b=q[n];
		if (b=="find" || b ~ /^find-/) {print $2; exit} }' "$_csv")
	if awk -v t="$tmean" 'BEGIN { exit !(t + 0 < 0.010) }'; then
		sh bench/gate.sh "$_csv" min "$_lbl" || rc=1
	else
		sh bench/gate.sh "$_csv" mean "$_lbl" || rc=1
	fi
}

# Headline configs (audit 04): full traversal, name-only (no stat), type (d_type).
bench_one flat_default     "$corpus/flat"
bench_one flat_name        "$corpus/flat" -name 'f001*'
bench_one wide_default     "$corpus/wide"
bench_one wide_type        "$corpus/wide" -type f
bench_one deep_default     "$corpus/deep"

exit $rc
