#!/bin/sh
# Golden parity harness. Two phases:
#   phase 1 (always): ref-vs-ref self-test — build the reference, run it twice over
#           the case matrix, assert byte-identical. Proves the harness + corpus are
#           deterministic before any ferret diff is trusted.
#   phase 2 (only when tests/golden/PARITY_ACTIVE exists): ferret-vs-ref parity.
# The only sanctioned normalization is the leading program-name token on stderr.
set -u
set -f # no pathname expansion: glob patterns in CASES (*.c) must pass literally

here=$(dirname "$0")
root=$(cd "$here/../.." && pwd)
cd "$root"

REFTAG=${REFTAG:-4.10.0}
ref="tests/.work/ref/find-$REFTAG"
FERRET="${FERRET:-./ferret}"

work=$(mktemp -d "${TMPDIR:-/tmp}/frtgold.XXXXXX")
trap 'chmod -R u+rwx "$work" 2>/dev/null; rm -rf "$work"' EXIT INT TERM

# Build the reference oracle.
if ! sh tests/golden/build-ref.sh "$REFTAG" >"$work/buildref.log" 2>&1; then
	echo "GOLDEN: could not build reference find $REFTAG:"; cat "$work/buildref.log"
	exit 1
fi

# Pin a private copy of ferret so a concurrent `gmake release` can't swap it mid-run.
UUT="$work/ferret.uut"
[ -x "$FERRET" ] && cp "$FERRET" "$UUT"

corpus="$work/corpus"
sh tests/golden/mkcorpus.sh "$corpus" >/dev/null

# Case matrix. %C expands to the corpus root. Cases grow per sprint (sprint 02+).
# Sprint 02 surface: -name/-iname/-type/-empty/-print/-print0, operators, implicit print.
CASES='
%C
%C -print
%C -print0
%C -name *.c
%C -iname *.C
%C -name *.txt
%C -name file.txt
%C -type f
%C -type d
%C -type l
%C -type f,l
%C -empty
%C -true
%C -false
%C -name *.c -o -name *.log
%C -type d -print
%C ! -name *.c
%C ( -name *.c -o -name *.dat )
%C -name *.c , -name *.log
%C -type f -a -name *.c
%C -maxdepth 1
%C -maxdepth 2
%C -maxdepth 0
%C -mindepth 1
%C -mindepth 2
%C -maxdepth 2 -mindepth 1
%C -maxdepth 1 -type d
%C -depth
%C -depth -name *.c
%C -d -type f
%C -name sub -prune
%C -name sub -prune -o -print
%C -depth -name sub -prune
%C -xdev
%C -noleaf
-L %C
-L %C -type l
-L %C -type d
-L %C -type f
-H %C
-L %C -maxdepth 2
-P %C -type l
'

# The reference binary is named find-<tag>, so it self-reports that as its program
# name in diagnostics; normalize it (and ferret / plain find) to PROG.
refbase=$(basename "$ref")
normprog() { sed "s/^$refbase: /PROG: /; s/^ferret: /PROG: /; s/^find: /PROG: /"; }

# run_case <binary> <case-string> -> writes o.out/o.err/o.rc in $work
run_case() {
	_bin=$1; _case=$2
	_expanded=$(printf '%s' "$_case" | sed "s#%C#$corpus#g")
	# shellcheck disable=SC2086
	set -- $_expanded
	"$_bin" "$@" >"$work/o.out" 2>"$work/o.err"
	echo $? >"$work/o.rc"
}

# compare two binaries over all CASES under one locale; returns fail count via file
phase() {
	_a=$1; _b=$2; _label=$3; _loc=$4
	_fails=0
	printf '%s\n' "$CASES" | while IFS= read -r c; do
		[ -n "$c" ] || continue
		LC_ALL="$_loc" run_case "$_a" "$c"
		cp "$work/o.out" "$work/a.out"; cp "$work/o.err" "$work/a.err"; cp "$work/o.rc" "$work/a.rc"
		LC_ALL="$_loc" run_case "$_b" "$c"
		ok=1
		cmp -s "$work/a.out" "$work/o.out" || ok=0
		normprog <"$work/a.err" >"$work/a.errn"; normprog <"$work/o.err" >"$work/o.errn"
		cmp -s "$work/a.errn" "$work/o.errn" || ok=0
		[ "$(cat "$work/a.rc")" = "$(cat "$work/o.rc")" ] || ok=0
		if [ "$ok" = 0 ]; then
			echo "  DIFF [$_label/$_loc]: $c (rc a=$(cat "$work/a.rc") o=$(cat "$work/o.rc"))"
			diff "$work/a.out" "$work/o.out" | head -8
			diff "$work/a.errn" "$work/o.errn" | head -8
			echo "$c" >>"$work/fails"
		fi
	done
}

# Available locales: C plus a UTF-8 one if the box has it.
utf8=""
for L in C.UTF-8 en_US.UTF-8 en_US.utf8; do
	[ "$(LC_ALL=$L locale charmap 2>/dev/null)" = "UTF-8" ] && { utf8=$L; break; }
done
locales="C"
[ -n "$utf8" ] && locales="$locales $utf8"

: >"$work/fails"

# Phase 1: ref vs ref (determinism self-test). Always runs.
for L in $locales; do
	phase "$ref" "$ref" "self" "$L"
done
if [ -s "$work/fails" ]; then
	echo "GOLDEN: self-test FAILED — harness or corpus is non-deterministic"
	exit 1
fi

# Phase 2: ferret vs ref. Gated on PARITY_ACTIVE.
if [ -f tests/golden/PARITY_ACTIVE ] && [ -x "$UUT" ]; then
	for L in $locales; do
		phase "$UUT" "$ref" "parity" "$L"
	done
	if [ -s "$work/fails" ]; then
		n=$(wc -l <"$work/fails" | tr -d ' ')
		echo "GOLDEN: parity FAILED ($n diffs vs find $REFTAG)"
		exit 1
	fi
	echo "GOLDEN: ok (parity vs find $REFTAG, locales: $locales)"
else
	echo "GOLDEN: ok (self-test only; parity gate inactive — no PARITY_ACTIVE)"
fi
exit 0
