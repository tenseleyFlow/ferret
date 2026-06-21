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

# argv recorder for -exec/-execdir/-ok cases (%R expands to it).
recorder="$root/tests/golden/recordargs.sh"
chmod +x "$recorder" 2>/dev/null || true

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
%C -size +1k
%C -size -1k
%C -size 0
%C -size 100c
%C -size +99c
%C -size 3b
%C -perm 644
%C -perm 600
%C -perm -644
%C -perm /222
%C -perm -0
%C -perm -u+r
%C -links 2
%C -links +1
%C -links 1
%C -uid %U
%C -gid %G
%C -uid +0
%C -nouser
%C -nogroup
%C -samefile %C/small.bin
%C -readable
%C -writable
%C -type f -a -size +1k
%C -size +1k -o -type d
%C -mtime +30
%C -mtime +100
%C -atime +30
%C -mmin +1440
%C -newer %C/old2020.txt
%C -newer %C/old2020.txt -type f
%C -anewer %C/old2015.txt
%C -newermt 2018-06-15
%C -newermt @1000000000
%C -newermt yesterday
%C -newermt now
%C -newermt tomorrow
%C -newermt today
%C -newermt monday
%C -newermt friday
%C -newermt 3/15/2020
%C -newermt 2020/03/15
%C -newermt 2020-03-15T12:00:00
%C -newermt 20200315
%C -newermt 2020-03-15T06:00:00+05:00
%C -newermt 2020-03-15T12:00:00N
%C -newermm %C/old2020.txt
%C -used 1
%C -used +1
%C -used -1
%C -used +0
-files0-from %C/files0.list -type f
-files0-from %C/files0.list -print
-files0-from %C/files0.list -name *.c
%C -context foo
%C -context
%C -daystart -mtime +30
%C -type f -exec %R {} ;
%C -type f -exec %R {} +
%C -type f -execdir %R {} ;
%C -type f -execdir %R {} +
%C -name *.c -exec %R pre {} post ;
%C -type f -exec %R {} ; -print
%C -name *.c -exec false ; -o -print
%C -quit
%C -print -quit
%C -name b1.dat -quit
%C -printf %p\n
%C -printf %f|%h\n
%C -printf %P=%d:%y\n
%C -printf %H|%p\n
%C -type f -printf %s|%m|%M\n
%C -printf %i|%n\n
%C -type f -printf %u|%g|%U|%G\n
%C -printf %y|%Y\n
%C -type l -printf %p->%l\n
%C -printf [%-20p]\n
%C -printf pct%%done\t%p\n
%C/old2020.txt -printf %t\n
%C/old2020.txt -printf %a|%c\n
%C/old2020.txt -printf %T@\n
%C/old2020.txt -printf %TY-%Tm-%TdT%TH:%TM:%TS\n
%C/old2020.txt -printf %Tc|%T+\n
%C/old2015.txt -printf %AY-%Am-%Ad|%Cj\n
%C -type f -printf %S\n
%C -ls
%C -type f -ls
%C -type l -ls
%C -name *.c -ls
%C -path */sub/*
%C -ipath */SUB/*
%C -wholename *.c
%C -lname file.txt
%C -lname *
%C -ilname FILE.TXT
%C -xtype f
%C -xtype d
%C -xtype l
%C -regex .*\.c
%C -iregex .*\.C
%C -regex .*/sub
%C -regextype posix-egrep -regex .*/(alpha|beta)
%C -regextype posix-extended -regex .*\.(c|log)
%C -regextype posix-basic -regex .*\.c
%C -printf %F\n
%C -type l -printf %p=%F\n
'

# The reference binary is named find-<tag>, so it self-reports that as its program
# name in diagnostics; normalize it (and ferret / plain find) to PROG.
refbase=$(basename "$ref")
# find reports its program name as the full argv[0] it was invoked with (e.g.
# "tests/.work/ref/find-4.10.0:"), not just the basename — normalize both, plus
# ferret/find. '#' delimiter so the path's slashes need no escaping.
normprog() { sed "s#^$ref: #PROG: #; s/^$refbase: /PROG: /; s/^ferret: /PROG: /; s/^find: /PROG: /"; }

# %U/%G expand to the current uid/gid (deterministic per machine; both tools agree)
me_uid=$(id -u)
me_gid=$(id -g)

# run_case <binary> <case-string> -> writes o.out/o.err/o.rc in $work
run_case() {
	_bin=$1; _case=$2
	_expanded=$(printf '%s' "$_case" | sed "s#%C#$corpus#g; s#%U#$me_uid#g; s#%G#$me_gid#g; s#%R#$recorder#g")
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

# -ok reads y/n from stdin (can't go through the generic matrix). Pipe an
# alternating answer stream; compare stdout + normalized stderr + rc.
check_ok() {
	for _loc in $locales; do
		ans='y
n
y
n
y
n
y
n'
		printf '%s\n' "$ans" | LC_ALL=$_loc "$UUT" "$corpus" -type f -ok "$recorder" '{}' ';' \
			>"$work/ok.a.o" 2>"$work/ok.a.e"; ra=$?
		printf '%s\n' "$ans" | LC_ALL=$_loc "$ref" "$corpus" -type f -ok "$recorder" '{}' ';' \
			>"$work/ok.b.o" 2>"$work/ok.b.e"; rb=$?
		normprog <"$work/ok.a.e" >"$work/ok.a.en"; normprog <"$work/ok.b.e" >"$work/ok.b.en"
		if ! cmp -s "$work/ok.a.o" "$work/ok.b.o" || ! cmp -s "$work/ok.a.en" "$work/ok.b.en" ||
			[ "$ra" != "$rb" ]; then
			echo "  DIFF [ok/$_loc] rc a=$ra b=$rb"
			diff "$work/ok.a.o" "$work/ok.b.o" | head -6
			diff "$work/ok.a.en" "$work/ok.b.en" | head -4
			echo "ok" >>"$work/fails"
		fi
	done
}

# -delete is destructive: run each tool on an identical copy, then compare the
# resulting tree (listed with the reference), stderr, and exit code.
check_delete() {
	rm -rf "$work/delA" "$work/delB"
	cp -R "$corpus" "$work/delA"
	cp -R "$corpus" "$work/delB"
	"$ref" "$work/delB" -delete 2>"$work/del.b.e"; rb=$?
	"$UUT" "$work/delA" -delete 2>"$work/del.a.e"; ra=$?
	{ [ -d "$work/delB" ] && (cd "$work/delB" && "$ref" . | sort); } >"$work/del.b.tree" 2>/dev/null
	{ [ -d "$work/delA" ] && (cd "$work/delA" && "$ref" . | sort); } >"$work/del.a.tree" 2>/dev/null
	normprog <"$work/del.a.e" | sed "s#$work/delA#C#g" >"$work/del.a.en"
	normprog <"$work/del.b.e" | sed "s#$work/delB#C#g" >"$work/del.b.en"
	if ! cmp -s "$work/del.a.tree" "$work/del.b.tree" || ! cmp -s "$work/del.a.en" "$work/del.b.en" ||
		[ "$ra" != "$rb" ]; then
		echo "  DIFF [delete] rc a=$ra b=$rb"
		diff "$work/del.b.tree" "$work/del.a.tree" | head -8
		diff "$work/del.b.en" "$work/del.a.en" | head -4
		echo "delete" >>"$work/fails"
	fi
}

# -fprint/-fprint0/-fprintf/-fls write to a named file; compare those files.
check_files() {
	for spec in "fprint" "fprint0" "fls" "fprintf"; do
		fa="$work/fv.a"; fb="$work/fv.b"
		case "$spec" in
		fprint)   "$ref" "$corpus" -type f -fprint "$fb" >/dev/null 2>&1
			  "$UUT" "$corpus" -type f -fprint "$fa" >/dev/null 2>&1 ;;
		fprint0)  "$ref" "$corpus" -type f -fprint0 "$fb" >/dev/null 2>&1
			  "$UUT" "$corpus" -type f -fprint0 "$fa" >/dev/null 2>&1 ;;
		fls)      LC_ALL=C "$ref" "$corpus" -fls "$fb" >/dev/null 2>&1
			  LC_ALL=C "$UUT" "$corpus" -fls "$fa" >/dev/null 2>&1 ;;
		fprintf)  "$ref" "$corpus" -type f -fprintf "$fb" '%p|%s|%y\n' >/dev/null 2>&1
			  "$UUT" "$corpus" -type f -fprintf "$fa" '%p|%s|%y\n' >/dev/null 2>&1 ;;
		esac
		if ! cmp -s "$fa" "$fb"; then
			echo "  DIFF [$spec file]"
			diff "$fb" "$fa" | head -6
			echo "$spec" >>"$work/fails"
		fi
	done
}

# -fstype: query the reference's own %F for the corpus (portable across fs), then
# check -fstype with that value (matches) and a bogus value (matches nothing).
check_fstype() {
	ft=$("$ref" "$corpus" -maxdepth 0 -printf '%F\n' 2>/dev/null)
	[ -n "$ft" ] || return
	for v in "$ft" nosuchfs; do
		"$ref" "$corpus" -fstype "$v" 2>/dev/null | sort >"$work/fst.b"
		"$UUT" "$corpus" -fstype "$v" 2>/dev/null | sort >"$work/fst.a"
		if ! cmp -s "$work/fst.a" "$work/fst.b"; then
			echo "  DIFF [fstype $v]"
			diff "$work/fst.b" "$work/fst.a" | head -6
			echo "fstype" >>"$work/fails"
		fi
	done
}

# Optimizer invariant: ferret's output must be identical across -O levels (only
# pure predicates reorder; actions are pinned). Compare -O0 vs -O1..-O4.
check_olevels() {
	printf '%s\n' "$CASES" | while IFS= read -r c; do
		[ -n "$c" ] || continue
		case "$c" in *-fprint*|*-fls*|*-fprintf*) continue ;; esac # write to files
		exp=$(printf '%s' "$c" | sed "s#%C#$corpus#g; s#%U#$me_uid#g; s#%G#$me_gid#g; s#%R#$recorder#g")
		# shellcheck disable=SC2086
		set -- $exp
		LC_ALL=C "$UUT" -O0 "$@" >"$work/ol0.o" 2>"$work/ol0.e"; r0=$?
		for lv in 1 2 3 4; do
			LC_ALL=C "$UUT" -O$lv "$@" >"$work/oln.o" 2>"$work/oln.e"; rn=$?
			if ! cmp -s "$work/ol0.o" "$work/oln.o" || [ "$r0" != "$rn" ]; then
				echo "  DIFF [-O0 vs -O$lv]: $c"
				diff "$work/ol0.o" "$work/oln.o" | head -4
				echo "olevel" >>"$work/fails"
			fi
		done
	done
}

# Parallel-stat invariant: output must be identical for any --ferret-threads N
# (only stat work is parallel; order is unchanged). Compare 1 vs 4 over CASES.
check_threads() {
	printf '%s\n' "$CASES" | while IFS= read -r c; do
		[ -n "$c" ] || continue
		case "$c" in *-fprint*|*-fls*|*-fprintf*) continue ;; esac
		exp=$(printf '%s' "$c" | sed "s#%C#$corpus#g; s#%U#$me_uid#g; s#%G#$me_gid#g; s#%R#$recorder#g")
		# shellcheck disable=SC2086
		set -- $exp
		LC_ALL=C "$UUT" --ferret-threads 1 "$@" >"$work/th1.o" 2>"$work/th1.e"; r1=$?
		LC_ALL=C "$UUT" --ferret-threads 4 "$@" >"$work/th4.o" 2>"$work/th4.e"; r4=$?
		if ! cmp -s "$work/th1.o" "$work/th4.o" || [ "$r1" != "$r4" ]; then
			echo "  DIFF [threads 1 vs 4]: $c"
			diff "$work/th1.o" "$work/th4.o" | head -4
			echo "threads" >>"$work/fails"
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
	check_ok
	check_delete
	check_files
	check_fstype
	check_olevels
	check_threads
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
