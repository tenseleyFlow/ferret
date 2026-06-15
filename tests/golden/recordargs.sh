#!/bin/sh
# argv recorder for -exec/-execdir/-ok golden tests. Prints one line per
# invocation: "X" followed by each argument. Captures argument bytes, ordering,
# and '+' batch grouping identically for ferret and find (no cwd/path noise).
printf 'X'
for a in "$@"; do
	printf ' %s' "$a"
done
printf '\n'
