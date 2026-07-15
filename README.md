# aalloc — Atomic Allocator

A Linux memory allocator written in C, designed as a drop-in replacement
for `malloc`/`free`/`realloc`/`calloc`.

## Features

- **Segregated free lists** — 20 size classes from 8 B to 16 KiB
- **Boundary-tag coalescing** — O(1) merge of adjacent free blocks
- **Per-thread arenas** — eliminates lock contention on the hot path
- **Slab allocator** — magazine-based cache for objects ≤ 256 bytes
- **Allocator statistics** — lock-free counters, fragmentation estimate
- **Benchmark suite** — throughput, fragmentation, multi-thread; comparison against glibc, jemalloc, tcmalloc

## Repository Layout

```
include/            Public header (aalloc.h) and internal headers
src/                Allocator implementation
tests/
  unit/             Per-module unit tests
  stress/           Randomised multi-thread stress test
  framework/        Lightweight test harness (header-only)
benchmarks/         Throughput, fragmentation, and MT benchmarks
scripts/            Benchmark runner and result plotter
docs/               Architecture, API reference, benchmark guide
```

## Quick Start

```sh
# Build libraries
make

# Run unit tests
make tests

# Run stress test
make stress

# Build benchmarks
make benchmarks

# Compare allocators (requires jemalloc / tcmalloc installed)
./scripts/compare_allocators.sh
```

## Goals

### Technical Implementation
- Segregated free lists
- Constant-time free/coalesce using boundary tags
- Per-arena threads
- Slab allocation (cache)
- Robustness testing
- *POSIX memalign / posix_memalign [tentative]*
- *Heap visualisation [tentative]*

### Performance
- Fragmentation benchmark
- Throughput benchmark
- Latency benchmark
- Scalability benchmark (1–N threads)
- Comparison against glibc malloc, jemalloc, tcmalloc

### Diagnostics
- Heap consistency checker
- Allocator statistics via `aa_print_stats()`

## Documentation

- [Architecture](docs/architecture.md)
- [API Reference](docs/api.md)
- [Benchmark Guide](docs/benchmarks.md)

## Building in Debug Mode

```sh
make DEBUG=1
```

Enables `AALLOC_DEBUG` assertions and disables optimisation.

## Implementation Status

| Milestone | Status |
|-----------|--------|
| Repository structure | ✓ |
| Build system | ✓ |
| Public APIs and headers | ✓ |
| Allocator metadata structures | ✓ |
| malloc / calloc | ✓ |
| free | ✓ |
| realloc | ✓ |
| Arena subsystem | |
| Slab subsystem |  |
| Statistics subsystem |  |
| Benchmarks | |
| Documentation | |

SAMPLE BUILD BENCHMARK:
=== AAlloc Benchmark Runner ===
Date: Tue Jul 14 18:48:23 CDT 2026
Host: Darwin 22.1.0 arm64

[1/2] Building libaalloc...
[2/2] Building benchmarks...

────────────────────────────────────────────────────────────
  bench_throughput
────────────────────────────────────────────────────────────

=== Throughput Benchmark [aalloc] ===

  Each workload runs for 3 seconds.

  A  Tiny   (8–64 B):           40.693 M ops/sec
  B  Medium (128 B–4 KiB):      81.473 M ops/sec
  C  Large  (8–64 KiB):         87.904 M ops/sec
  D  Mixed  (8 B–8 KiB):        19.866 M ops/sec  [random interleaved]


=== Throughput Benchmark [glibc] ===

  Each workload runs for 3 seconds.

  A  Tiny   (8–64 B):           36.405 M ops/sec
  B  Medium (128 B–4 KiB):      32.037 M ops/sec
  C  Large  (8–64 KiB):         26.973 M ops/sec
  D  Mixed  (8 B–8 KiB):        29.266 M ops/sec  [random interleaved]


────────────────────────────────────────────────────────────
  bench_latency
────────────────────────────────────────────────────────────

=== Latency Benchmark [aalloc] ===

-- Uncontended (1 thread, 200000 samples each) --
  malloc tiny (64 B)              mean=  53.7 ns  p50=    42 ns  p99=     84 ns  p999=     125 ns  (n=200000)
  free   tiny (64 B)              mean=  39.0 ns  p50=    42 ns  p99=     84 ns  p999=      84 ns  (n=200000)
  malloc medium (1 KiB)           mean=  35.4 ns  p50=    42 ns  p99=     42 ns  p999=      42 ns  (n=200000)
  free   medium (1 KiB)           mean=  31.7 ns  p50=    42 ns  p99=     42 ns  p999=      42 ns  (n=200000)
  malloc large (64 KiB)           mean=  30.2 ns  p50=    41 ns  p99=     42 ns  p999=      84 ns  (n=200000)
  free   large (64 KiB)           mean=  26.5 ns  p50=    41 ns  p99=     42 ns  p999=      83 ns  (n=200000)

-- Contended (8 threads, 200000 samples/thread) --
  malloc tiny (64 B)              mean=  47.7 ns  p50=    42 ns  p99=    125 ns  p999=     125 ns  (n=1600000)
  free   tiny (64 B)              mean=  27.1 ns  p50=    41 ns  p99=     83 ns  p999=      84 ns  (n=1600000)
  malloc medium (1 KiB)           mean=  42.2 ns  p50=    41 ns  p99=     84 ns  p999=     125 ns  (n=1600000)
  free   medium (1 KiB)           mean=  28.7 ns  p50=    41 ns  p99=     83 ns  p999=      84 ns  (n=1600000)
  malloc large (4 KiB)            mean=  31.6 ns  p50=    41 ns  p99=     84 ns  p999=      84 ns  (n=1600000)
  free   large (4 KiB)            mean=  28.1 ns  p50=    41 ns  p99=     83 ns  p999=      84 ns  (n=1600000)

-- Cross-thread free (1000 ops) --
  free   cross-thread (64 B)       mean=  24.2 ns  p50=    41 ns  p99=     42 ns  p999=      42 ns  (n=1000)


=== Latency Benchmark [glibc] ===

-- Uncontended (1 thread, 200000 samples each) --
  malloc tiny (64 B)              mean=  15.5 ns  p50=     0 ns  p99=     42 ns  p999=      42 ns  (n=200000)
  free   tiny (64 B)              mean=  15.5 ns  p50=     0 ns  p99=     42 ns  p999=      42 ns  (n=200000)
  malloc medium (1 KiB)           mean=  16.7 ns  p50=     0 ns  p99=     42 ns  p999=      42 ns  (n=200000)
  free   medium (1 KiB)           mean=  16.9 ns  p50=     0 ns  p99=     42 ns  p999=      42 ns  (n=200000)
  malloc large (64 KiB)           mean=  21.2 ns  p50=    41 ns  p99=     42 ns  p999=      42 ns  (n=200000)
  free   large (64 KiB)           mean=  21.1 ns  p50=    41 ns  p99=     42 ns  p999=      42 ns  (n=200000)

-- Contended (8 threads, 200000 samples/thread) --
  malloc tiny (64 B)              mean=  21.2 ns  p50=     0 ns  p99=     42 ns  p999=      42 ns  (n=1600000)
  free   tiny (64 B)              mean=  19.9 ns  p50=     0 ns  p99=     42 ns  p999=      42 ns  (n=1600000)
  malloc medium (1 KiB)           mean=  18.1 ns  p50=     0 ns  p99=     42 ns  p999=      42 ns  (n=1600000)
  free   medium (1 KiB)           mean=  19.2 ns  p50=     0 ns  p99=     42 ns  p999=      42 ns  (n=1600000)
  malloc large (4 KiB)            mean=  17.8 ns  p50=     0 ns  p99=     42 ns  p999=      42 ns  (n=1600000)
  free   large (4 KiB)            mean=  17.8 ns  p50=     0 ns  p99=     42 ns  p999=      42 ns  (n=1600000)

-- Cross-thread free (1000 ops) --
  free   cross-thread (64 B)       mean=  36.0 ns  p50=    42 ns  p99=     42 ns  p999=    1417 ns  (n=1000)


────────────────────────────────────────────────────────────
  bench_fragmentation
────────────────────────────────────────────────────────────

=== Fragmentation Benchmark [aalloc] ===

  Header overhead: 24 bytes per allocation (size_state + left_size + owner)

-- Per-size-class utilization (100 blocks each) --
  sz=   8 B  util= 25.0%  (payload=800 B, hdr=24 B/block)
  sz=  16 B  util= 40.0%  (payload=1600 B, hdr=24 B/block)
  sz=  32 B  util= 57.1%  (payload=3200 B, hdr=24 B/block)
  sz=  64 B  util= 72.7%  (payload=6400 B, hdr=24 B/block)
  sz= 128 B  util= 84.2%  (payload=12800 B, hdr=24 B/block)
  sz= 256 B  util= 91.4%  (payload=25600 B, hdr=24 B/block)
  sz= 512 B  util= 95.5%  (payload=51200 B, hdr=24 B/block)

-- Checkerboard (2×128 blocks, 64–512 B) --
  checkerboard (all live)                   payload=   71 KiB  overhead=   6 KiB  util=92.3%
  checkerboard (50% freed, holes)           payload=   35 KiB  overhead=   6 KiB  util=85.7%

-- Steady-state (128 live blocks, 50000 ops, 8–512 B) --
  steady-state (live set)                   payload=   33 KiB  overhead=   3 KiB  util=91.8%


=== Fragmentation Benchmark [glibc] ===

  Header overhead: 24 bytes per allocation (size_state + left_size + owner)

-- Per-size-class utilization (100 blocks each) --
  sz=   8 B  util= 25.0%  (payload=800 B, hdr=24 B/block)
  sz=  16 B  util= 40.0%  (payload=1600 B, hdr=24 B/block)
  sz=  32 B  util= 57.1%  (payload=3200 B, hdr=24 B/block)
  sz=  64 B  util= 72.7%  (payload=6400 B, hdr=24 B/block)
  sz= 128 B  util= 84.2%  (payload=12800 B, hdr=24 B/block)
  sz= 256 B  util= 91.4%  (payload=25600 B, hdr=24 B/block)
  sz= 512 B  util= 95.5%  (payload=51200 B, hdr=24 B/block)

-- Checkerboard (2×128 blocks, 64–512 B) --
  checkerboard (all live)                   payload=   71 KiB  overhead=   6 KiB  util=92.3%
  checkerboard (50% freed, holes)           payload=   35 KiB  overhead=   6 KiB  util=85.7%

-- Steady-state (128 live blocks, 50000 ops, 8–512 B) --
  steady-state (live set)                   payload=   33 KiB  overhead=   3 KiB  util=91.8%


