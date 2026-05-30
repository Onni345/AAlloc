# atomic allocator (AAlloc)
malloc, realloc, calloc, free, etc.

## goals
### technical implementation
- segregated free lists
- constant time free/coalesce using boundary tags
- per-arena threads
- slab allocation (cache)
- robustness testing
- *posix standard (memalign, posix_memalign) [tentative]*
- *visualization [tentative]*

### tools
- basic unit tests, MT stress tests

### performance
- fragmentation benchmark
- throughput benchmark
- latency benchmark
- scalability benchmark (1–N threads)
- compare to glibc malloc, jemalloc, *[more tbd]*

### diagnostics
- heap consistency checker