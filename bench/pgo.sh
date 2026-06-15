#!/bin/sh
# Opt-in profile-guided build (clang/llvm). Two passes: instrument, profile over
# representative workloads, rebuild with the profile. NOT the default release —
# packaged builds stay plain -O3 -flto for reproducibility. ~a few % on large
# directories; within noise on small ones.
set -eu

here=$(dirname "$0")
root=$(cd "$here/.." && pwd)
cd "$root"

CC=${CC:-cc}
case "$($CC --version 2>/dev/null)" in
*clang*) ;;
*) echo "pgo: clang/llvm required (got $($CC --version | head -1)); skipping"; exit 0 ;;
esac
command -v llvm-profdata >/dev/null 2>&1 || {
	echo "pgo: llvm-profdata not found; skipping"; exit 0; }

work=bench/.work
mkdir -p "$work"
pgo="$work/pgo"; prof="$work/ferret.profdata"
rm -rf "$pgo"; mkdir -p "$pgo"

mk="gmake"; command -v gmake >/dev/null 2>&1 || mk="make"

echo "pgo: pass 1 (instrumented build)"
$mk -s clean
$mk -s all CFLAGS="-O3 -DNDEBUG -fprofile-generate=$pgo" LDFLAGS="-flto -fprofile-generate=$pgo"

echo "pgo: profiling representative workloads"
sh bench/mkclasses.sh "$work/corpus" >/dev/null 2>&1 || true
for c in flat wide deep; do
	d="$work/corpus/$c"
	[ -d "$d" ] || continue
	LLVM_PROFILE_FILE="$pgo/%p.profraw" ./ferret "$d" >/dev/null 2>&1 || true
	LLVM_PROFILE_FILE="$pgo/%p.profraw" ./ferret "$d" -type f -size +0c >/dev/null 2>&1 || true
	LLVM_PROFILE_FILE="$pgo/%p.profraw" ./ferret "$d" -name 'f*' >/dev/null 2>&1 || true
done
llvm-profdata merge -output="$prof" "$pgo"/*.profraw

echo "pgo: pass 2 (profile-use rebuild)"
$mk -s clean
$mk -s all CFLAGS="-O3 -DNDEBUG -fprofile-use=$prof -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled" \
	LDFLAGS="-flto -fprofile-use=$prof"
strip ferret frt 2>/dev/null || true
echo "pgo: done -> ./ferret (profile-optimized)"
