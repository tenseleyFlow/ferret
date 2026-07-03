# ferret

A from-scratch C reimplementation of GNU `find(1)`. For a given expression and filesystem state the
output matches GNU findutils 4.10.0 byte for byte; it runs faster on the workloads measured. Two
binaries, `ferret` and `frt`.

## Status

v0.1.0-dev. The predicate and action surface is complete. A golden suite checks ferret's output byte
for byte against a locally built GNU find 4.10.0 over a recorded case matrix, in the C and C.UTF-8
locales, on Ubuntu, macOS, FreeBSD, and musl/Alpine in CI. The Linux job re-runs the suite under the
io_uring stat backend, held to the same parity.

Traversal is iterative with directory-fd recycling, so deep trees don't overflow the C stack or
exhaust the fd table, and ferret has been through three adversarial audit passes. `-newerXt` and
`-newermt` dates are parsed by [frtdate](https://github.com/tenseleyFlow/frtdate), an in-house
submodule that reimplements the common slice of gnulib's date grammar without gnulib: ISO, `@epoch`,
relative offsets, month-name and numeric dates, weekdays, times, AM/PM, and timezone offsets.

Known gaps: the `%Z` (SELinux context) `-printf` directive needs libselinux to match find, and the
date grammar's long tail (named timezones like `EST`, 2-digit years in month-name dates, ordinal
days of month) is unimplemented. Both error rather than diverge silently. The `-O3`/`-O4` dataflow
optimizations are not built, but output is identical at every `-O` level, so that affects speed only,
not parity.

## Build

```sh
git clone --recurse-submodules https://github.com/tenseleyFlow/ferret   # frtdate lives in deps/
./configure        # probes the toolchain, writes config.h / config.mk
make               # builds ./ferret and ./frt   (gmake on *BSD)
make release       # -O3 -flto portable build
make debug         # ASan/UBSan build
make install       # honors PREFIX / DESTDIR
```

Needs a C11 compiler and GNU make (`gmake` on FreeBSD). No third-party dependencies; the date parser
is the in-house [frtdate](https://github.com/tenseleyFlow/frtdate) submodule (`deps/frtdate`, libc
only — already cloned, run `git submodule update --init` if you cloned without `--recurse-submodules`),
and `liburing` is used if present.

## Test and benchmark

```sh
make test          # unit tests (ASan/UBSan) + golden parity vs a locally built find 4.10.0
make bench         # hyperfine ferret vs find; the gate fails if ferret isn't faster
sh ci/preflight.sh # build, test, bench on the Linux/macOS boxes over Tailscale
```

The golden suite builds the reference find 4.10.0 and compares stdout, stderr, and exit code. The
only normalized difference is the leading program-name token on stderr.

## Performance

The main saving is not calling `lstat` when `d_type` already answers the type, plus `getdents` with a
64 KB buffer, arena allocation with inline names, cost-based predicate reordering, and batched
`write(2)`. By default metadata stats run inline — one `fstatat` per file, as a predicate needs it.
A worker pool runs them in parallel for stat-heavy traversals — auto-engaged on a stat-bound physical
walk, or forced with `--ferret-threads N` — with identical output. `FRT_IO=uring` instead batches the
stats through Linux io_uring (`statx`), falling back to the pool where io_uring is unavailable; the
fill mirrors `fstatat` exactly, so output is unchanged. On the metadata workloads measured the pool is
the faster backend (it spreads `statx` across cores; io_uring reaps on one thread), so io_uring stays
an opt-in alternative rather than the default.

## Layout

```
src/        implementation (sys/ is the only platform-aware layer)
deps/       frtdate submodule (date parser, libc only)
tests/      unit/ harness + golden/ parity suite
bench/      corpus generator, hyperfine runner, perf gate
ci/         preflight script  (.github/workflows/ci.yml drives CI)
.docs/      design, audits  (local)
```

## License

MIT. See [LICENSE](LICENSE).
