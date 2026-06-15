#!/bin/sh
# Build a deterministic base corpus for golden parity tests.
# Usage: mkcorpus.sh <dir>   (idempotent: removes <dir> first)
set -eu
dir=${1:?usage: mkcorpus.sh <dir>}

rm -rf "$dir"
mkdir -p "$dir"

mkdir -p "$dir/alpha/sub"
mkdir -p "$dir/beta"
mkdir -p "$dir/empty"
mkdir -p "$dir/a dir with spaces"

: > "$dir/file.txt"
: > "$dir/.hidden"
: > "$dir/alpha/a1.txt"
: > "$dir/alpha/a2.log"
: > "$dir/alpha/sub/deep.c"
: > "$dir/alpha/2.c"
: > "$dir/alpha/1.c"
: > "$dir/beta/b1.dat"
: > "$dir/a dir with spaces/inside.txt"
: > "$dir/naïve.txt"

# symlinks: one good, one broken
ln -s file.txt "$dir/good.link" 2>/dev/null || true
ln -s nonexistent-target "$dir/broken.link" 2>/dev/null || true

echo "$dir"
