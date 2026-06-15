#!/bin/sh
# Build deterministic benchmark corpus shapes under <root>: flat, wide, deep.
# Idempotent per shape (skips if already present). Tunable via env.
set -eu
root=${1:?usage: mkclasses.sh <root>}
mkdir -p "$root"

FLAT=${FLAT:-20000}
WDIRS=${WDIRS:-200}
WFILES=${WFILES:-100}
DEPTH=${DEPTH:-400}

# 'seq' on Linux, 'jot' on BSD — wrap.
nums() { if command -v seq >/dev/null 2>&1; then seq 1 "$1"; else jot "$1"; fi; }

if [ ! -d "$root/flat" ]; then
	d="$root/flat"; mkdir -p "$d"
	nums "$FLAT" | awk -v p="$d" '{printf "%s/f%06d\n", p, $1}' | xargs -n 2000 touch
fi

if [ ! -d "$root/wide" ]; then
	d="$root/wide"; mkdir -p "$d"; j=0
	while [ "$j" -lt "$WDIRS" ]; do
		s="$d/d$(printf %04d "$j")"; mkdir -p "$s"
		nums "$WFILES" | awk -v p="$s" '{printf "%s/f%04d\n", p, $1}' | xargs -n 1000 touch
		j=$((j + 1))
	done
fi

if [ ! -d "$root/deep" ]; then
	d="$root/deep"; p="$d"; mkdir -p "$p"; i=0
	while [ "$i" -lt "$DEPTH" ]; do p="$p/d"; i=$((i + 1)); done
	mkdir -p "$p"; : > "$p/leaf"
fi

echo "$root"
