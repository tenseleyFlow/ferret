#!/bin/sh
# Build the reference GNU find (parity target 4.10.0) into tests/.work/ref/.
# Uses the local tree in .docs/refs/findutils when present (dev box); otherwise
# fetches the 4.10.0 release tarball (CI / fresh checkouts, where .docs/ is
# gitignored & absent). Idempotent.
set -eu

TAG=${1:-4.10.0}
OUT=tests/.work/ref

# `build-ref.sh bfs [tag]` builds the bfs oracle instead (superset phase: the
# @bfs golden cases and matrix-check diff against it). Linux CI builds it here;
# macOS/FreeBSD install it via brew/pkg. The BSD-find oracle has no build target:
# it is the system /usr/bin/find on FreeBSD/macOS (porting BSD find to Linux is
# not a test dep).
if [ "$TAG" = "bfs" ]; then
	BTAG=${2:-4.1.1}
	bin="$OUT/bfs"
	mkdir -p "$OUT"
	[ -x "$bin" ] && { echo "ref bfs present"; exit 0; }
	srcdir="tests/.work/bfs-$BTAG"
	if [ ! -d "$srcdir" ]; then
		tb="tests/.work/bfs-$BTAG.tar.gz"
		url="https://github.com/tavianator/bfs/archive/refs/tags/$BTAG.tar.gz"
		if command -v fetch >/dev/null 2>&1; then fetch -o "$tb" "$url"
		elif command -v curl >/dev/null 2>&1; then curl -sSL -o "$tb" "$url"
		else wget -O "$tb" "$url"; fi
		( cd tests/.work && tar xf "bfs-$BTAG.tar.gz" )
	fi
	( cd "$srcdir" \
		&& { [ -f gen/config.mk ] || ./configure >/dev/null 2>&1; } \
		&& { gmake -s -j4 >/dev/null 2>&1 || make -s -j4 >/dev/null 2>&1; } )
	cp "$srcdir/bin/bfs" "$bin"
	# A tarball build has no git metadata, so bfs stamps major.minor only
	# (4.1.1 -> "bfs 4.1"). Accept the tag or a prefix of it.
	got=$("$bin" --version 2>/dev/null | sed -n '1s/^bfs \([0-9.]*\).*/\1/p')
	case "$BTAG" in
	"$got" | "$got".*) : ;;
	*)
		echo "build-ref: $bin reports bfs '$got', expected '$BTAG' (refusing it)" >&2
		rm -f "$bin"; exit 1 ;;
	esac
	echo "built ref bfs $BTAG -> $bin"
	exit 0
fi

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
		# ftp.gnu.org connect-fails from CI runners now and then; try the geo
		# mirror first and fall back. A partial download fails the tar extract.
		for url in "https://ftpmirror.gnu.org/findutils/findutils-$TAG.tar.xz" \
		           "https://ftp.gnu.org/gnu/findutils/findutils-$TAG.tar.xz"; do
			rm -f "$tb"
			if command -v fetch >/dev/null 2>&1; then
				fetch -o "$tb" "$url" && break || true
			elif command -v curl >/dev/null 2>&1; then
				curl -fsSL -o "$tb" "$url" && break || true
			else
				wget -O "$tb" "$url" && break || true
			fi
		done
		[ -s "$tb" ] || { echo "build-ref: could not fetch findutils $TAG" >&2; exit 1; }
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
