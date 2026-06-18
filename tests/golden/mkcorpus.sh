#!/bin/sh
# Build a deterministic base corpus for golden parity tests.
# Usage: mkcorpus.sh <dir>   (idempotent: removes <dir> first)
set -eu
dir=${1:?usage: mkcorpus.sh <dir>}

rm -rf "$dir"
mkdir -p "$dir"

mkdir -p "$dir/alpha/sub/sub2"
mkdir -p "$dir/beta"
mkdir -p "$dir/empty"
mkdir -p "$dir/a dir with spaces"

: > "$dir/file.txt"
: > "$dir/.hidden"
: > "$dir/alpha/a1.txt"
: > "$dir/alpha/a2.log"
: > "$dir/alpha/sub/deep.c"
: > "$dir/alpha/sub/sub2/deeper.c"
: > "$dir/alpha/2.c"
: > "$dir/alpha/1.c"
: > "$dir/beta/b1.dat"
: > "$dir/a dir with spaces/inside.txt"
: > "$dir/naïve.txt"

# sized files + pinned perms + a hardlink (for -size/-perm/-links parity)
head -c 1536 /dev/zero > "$dir/big.bin" 2>/dev/null || true
head -c 100 /dev/zero > "$dir/small.bin" 2>/dev/null || true
ln "$dir/small.bin" "$dir/small.hardlink" 2>/dev/null || true
chmod 0644 "$dir/big.bin" 2>/dev/null || true
chmod 0600 "$dir/small.bin" 2>/dev/null || true
chmod 0444 "$dir/beta/b1.dat" 2>/dev/null || true
chmod 0755 "$dir/alpha/sub/deep.c" 2>/dev/null || true

# pinned old mtimes (for -mtime/-newer parity — large margins, skew-robust)
touch -t 202001010000 "$dir/old2020.txt" 2>/dev/null || true
touch -t 201506150000 "$dir/old2015.txt" 2>/dev/null || true

# future atime, fresh ctime (for -used parity: atime-ctime > 0 -> a true outcome;
# old2020/old2015 set atime in the past, so -used is false there). Far enough out
# that atime stays after ctime for any plausible test date.
: > "$dir/used_future.txt"
touch -a -t 203501010000 "$dir/used_future.txt" 2>/dev/null || true

# symlinks: good (file), broken, dir, and a cycle to an ancestor (for -L loop tests)
ln -s file.txt "$dir/good.link" 2>/dev/null || true
ln -s nonexistent-target "$dir/broken.link" 2>/dev/null || true
ln -s alpha "$dir/dir.link" 2>/dev/null || true
ln -s .. "$dir/alpha/up.link" 2>/dev/null || true

echo "$dir"
