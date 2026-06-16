# ferret

A from-scratch C reimplementation of GNU `find(1)`. For a given expression and filesystem state the
output matches GNU findutils 4.10.0 byte for byte; it runs faster on the workloads measured. Two
binaries, `ferret` and `frt`.

## Status

Early. The M0 scaffold builds and the harness is green; the predicate surface lands sprint by sprint
(see `.docs/sprints/`). A golden suite checks output byte for byte against a locally built GNU find
4.10.0, backed by a differential fuzzer, in CI on Ubuntu, macOS, FreeBSD, and musl/Alpine; the Linux
job re-runs the suite under the io_uring stat backend so its output is held to the same parity.

## Build

```sh
./configure        # probes the toolchain, writes config.h / config.mk
make               # builds ./ferret and ./frt   (gmake on *BSD)
make release       # -O3 -flto portable build
make debug         # ASan/UBSan build
make install       # honors PREFIX / DESTDIR
```

Needs a C11 compiler and GNU make (`gmake` on FreeBSD). No third-party dependencies; `liburing` is
used if present.

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
an opt-in alternative rather than the default. Numbers land as the surface fills in (sprint 02+).

## Layout

```
src/        implementation (sys/ is the only platform-aware layer)
tests/      unit/ harness + golden/ parity suite
bench/      corpus generator, hyperfine runner, perf gate
ci/         preflight script  (.github/workflows/ci.yml drives CI)
.docs/      design, audits, sprints  (local)
```

## License

MIT. See [LICENSE](LICENSE).
