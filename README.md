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
