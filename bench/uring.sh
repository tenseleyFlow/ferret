#!/bin/sh
# Compare the three stat backends on a stat-heavy, non-selective traversal:
#   serial    one fstatat per file, inline
#   pool      fstatat fanned across the worker pool
#   io_uring  statx batched through one io_uring ring
# Informational only — never gates. Backend choice is environment-dependent;
# this just produces the numbers. Linux + liburing build only; cold-cache needs
# root (drop_caches), so it is skipped without it.
set -u

here=$(dirname "$0")
root=$(cd "$here/.." && pwd)
cd "$root"

FERRET="${FERRET:-./ferret}"
command -v hyperfine >/dev/null 2>&1 || { echo "uring-bench: no hyperfine — skipping"; exit 0; }
grep -q 'FRT_HAS_LIBURING = 1' config.mk 2>/dev/null || { echo "uring-bench: no liburing — skipping"; exit 0; }
[ -x "$FERRET" ] || { echo "uring-bench: no $FERRET — skipping"; exit 0; }

work=bench/.work
mkdir -p "$work"
corpus="$work/corpus"
sh bench/mkclasses.sh "$corpus" >/dev/null

# -size stats every entry and carries no -name filter, so the whole tree goes
# through the stat backend (the case the pool/io_uring exist to accelerate).
Q="$corpus/flat -size +1c"

hy() {
	hyperfine -w 2 -r 15 "$@" \
		-n serial   "$FERRET --ferret-threads 1 $Q >/dev/null" \
		-n pool     "$FERRET --ferret-threads 16 $Q >/dev/null" \
		-n io_uring "env FRT_IO=uring $FERRET $Q >/dev/null" \
		|| echo "uring-bench: hyperfine run failed (non-fatal)"
}

echo "## io_uring vs pool vs serial — 20k-file flat dir, -size +1c"
echo "### warm cache"
hy

if [ "$(id -u)" = 0 ] || sudo -n true 2>/dev/null; then
	echo "### cold cache (drop_caches before each run)"
	hy --prepare 'sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null'
else
	echo "uring-bench: no root for drop_caches — warm-cache only"
fi
