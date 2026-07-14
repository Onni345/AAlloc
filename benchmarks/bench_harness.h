/*
 * bench_harness.h — shared timing, stats, and allocator-switch helpers.
 *
 * Compile-time allocator selection:
 *   (default)      USE_AALLOC  — aa_malloc / aa_free
 *   -DUSE_SYSTEM               — system malloc / free
 */

#ifndef BENCH_HARNESS_H
#define BENCH_HARNESS_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── allocator selection ─────────────────────────────────────────────────── */

#ifdef USE_SYSTEM
#  define BENCH_MALLOC  malloc
#  define BENCH_FREE    free
#  define BENCH_REALLOC realloc
#  define BENCH_CALLOC  calloc
#  define ALLOC_NAME    "glibc"
#else
#  include "aalloc.h"
#  define BENCH_MALLOC  aa_malloc
#  define BENCH_FREE    aa_free
#  define BENCH_REALLOC aa_realloc
#  define BENCH_CALLOC  aa_calloc
#  define ALLOC_NAME    "aalloc"
#endif

/* ── timing ──────────────────────────────────────────────────────────────── */

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline double elapsed_sec(uint64_t start, uint64_t end) {
    return (double)(end - start) / 1e9;
}

/* ── stats helpers ───────────────────────────────────────────────────────── */

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

typedef struct {
    double   mean_ns;
    uint64_t p50_ns;
    uint64_t p99_ns;
    uint64_t p999_ns;
    uint64_t min_ns;
    uint64_t max_ns;
    size_t   n;
} stats_t;

/* Computes stats in-place (sorts the array). */
static stats_t compute_stats(uint64_t *samples, size_t n) {
    qsort(samples, n, sizeof(*samples), cmp_u64);
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += (double)samples[i];
    stats_t s = {
        .mean_ns = sum / (double)n,
        .p50_ns  = samples[n / 2],
        .p99_ns  = samples[(size_t)(n * 0.99)],
        .p999_ns = samples[(size_t)(n * 0.999)],
        .min_ns  = samples[0],
        .max_ns  = samples[n - 1],
        .n       = n,
    };
    return s;
}

static void print_stats(const char *label, stats_t s) {
    printf("  %-30s  mean=%6.1f ns  p50=%6lu ns  p99=%7lu ns  p999=%8lu ns  (n=%zu)\n",
           label, s.mean_ns,
           (unsigned long)s.p50_ns,
           (unsigned long)s.p99_ns,
           (unsigned long)s.p999_ns,
           s.n);
}

/* ── simple PRNG (xorshift64) ────────────────────────────────────────────── */

static inline uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return (*state = x);
}

/* Random size in [lo, hi] rounded to 8 bytes. */
static inline size_t rand_size(uint64_t *rng, size_t lo, size_t hi) {
    size_t range = hi - lo + 1;
    size_t s = lo + (size_t)(xorshift64(rng) % range);
    return (s + 7) & ~(size_t)7;
}

#endif /* BENCH_HARNESS_H */
