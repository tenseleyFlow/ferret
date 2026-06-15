#!/bin/sh
# Build the reference GNU find (parity target 4.10.0) into tests/.work/ref/.
# Uses the local tree in .docs/refs/findutils when present (dev box); otherwise
# fetches the 4.10.0 release tarball (CI / fresh checkouts, where .docs/ is
# gitignored & absent). Idempotent.
set -eu

TAG=${1:-4.10.0}
TARBALL_URL="https://ftp.gnu.org/gnu/findutils/findutils-$TAG.tar.xz"
OUT=tests/.work/ref
bin="$OUT/find-$TAG"

mkdir -p "$OUT"
[ -x "$bin" ] && { echo "ref find $TAG present"; exit 0; }

srcdir=""
if [ -f ".docs/refs/findutils/find/find.c" ] || [ -d ".docs/refs/findutils/find" ]; then
	srcdir=".docs/refs/findutils"
else
	srcdir="tests/.work/findutils-$TAG"
	if [ ! -d "$srcdir/find" ]; then
		tb="tests/.work/findutils-$TAG.tar.xz"
		if command -v fetch >/dev/null 2>&1; then
			fetch -o "$tb" "$TARBALL_URL"
		elif command -v curl >/dev/null 2>&1; then
			curl -sSL -o "$tb" "$TARBALL_URL"
		else
			wget -O "$tb" "$TARBALL_URL"
		fi
		( cd tests/.work && tar xf "findutils-$TAG.tar.xz" )
	fi
fi

# Build find/find. findutils ships a configure script; --without-selinux avoids a
# libselinux dependency we don't need for parity.
( cd "$srcdir" \
	&& { [ -x config.status ] || ./configure --without-selinux >/dev/null 2>&1; } \
	&& { gmake -s -j4 >/dev/null 2>&1 || make -s -j4 >/dev/null 2>&1; } )
cp "$srcdir/find/find" "$bin"

# Guard the build: a shared local clone can sit on the wrong version. Assert the
# binary reports the requested version before any test trusts it as the oracle.
got=$("$bin" --version 2>/dev/null | sed -n '1s/.*GNU findutils) \([0-9.]*\).*/\1/p')
if [ "$got" != "$TAG" ]; then
	echo "build-ref: $bin reports version '$got', expected '$TAG' — wrong build (refusing it)" >&2
	rm -f "$bin"
	exit 1
fi
echo "built ref find $TAG -> $bin"
