#!/bin/sh
# ferret test driver: unit tests (ASan/UBSan) -> Makefile SRC guard -> golden parity.
# Run from the repo root (gmake test). Exit 0 only if everything passes.
set -u

here=$(dirname "$0")
root=$(cd "$here/.." && pwd)
cd "$root"

CC=${CC:-cc}
fail=0
work=$(mktemp -d "${TMPDIR:-/tmp}/frttest.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

# Reuse the platform feature macros the real build computed.
CONF_CFLAGS=""
LDLIBS_OPT=""
[ -f config.mk ] && {
	CONF_CFLAGS=$(sed -n 's/^CONF_CFLAGS = //p' config.mk)
	LDLIBS_OPT=$(sed -n 's/^LDLIBS_OPT += //p' config.mk | tr '\n' ' ')
}

san="-fsanitize=address,undefined -fno-sanitize-recover=all"
[ "${FRT_TEST_SANITIZE:-1}" = 0 ] && san=""

CFLAGS_T="-std=c11 -g -O1 $san $CONF_CFLAGS -Isrc -I. -D_FILE_OFFSET_BITS=64"

# Library sources = all src/*.c and src/sys/*.c except main.c (tests provide main).
libsrc=$(ls src/*.c src/sys/*.c 2>/dev/null | grep -v '/main\.c$' | tr '\n' ' ')

echo "== unit tests =="
for t in tests/unit/*_test.c; do
	[ -e "$t" ] || continue
	name=$(basename "$t" .c)
	if ! $CC $CFLAGS_T -o "$work/$name" "$t" $libsrc $LDLIBS_OPT 2>"$work/$name.build"; then
		echo "BUILD FAIL $name"; cat "$work/$name.build"; fail=1; continue
	fi
	if ! "$work/$name" ; then
		fail=1
	fi
done

echo "== Makefile SRC vs filesystem =="
sed -n '/^SRC = /,/[^\\]$/p' Makefile \
	| sed 's/^SRC = //; s/\\//g' | tr -s ' \t' '\n' | grep '\.c$' | sort >"$work/mk_src"
ls src/*.c src/sys/*.c 2>/dev/null | sort >"$work/fs_src"
if ! diff -u "$work/mk_src" "$work/fs_src" >"$work/src_diff"; then
	echo "SRC GUARD FAIL: Makefile SRC list does not match src/*.c on disk:"
	cat "$work/src_diff"; fail=1
fi

echo "== golden parity =="
if ! sh tests/golden/run.sh; then
	fail=1
fi

if [ "$fail" = 0 ]; then
	echo "ALL TESTS PASSED"
else
	echo "TESTS FAILED"
fi
exit $fail
