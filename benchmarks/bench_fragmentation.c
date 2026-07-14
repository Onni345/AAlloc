/*
 * bench_fragmentation.c — heap utilization / fragmentation measurement.
 *
 * aalloc uses sbrk(2) for heap growth. On macOS, sbrk is limited to ~4 MB,
 * so all patterns are sized to stay well within that budget.
 *
 * Metric: utilization = payload / (payload + header_overhead)
 * where header_overhead = N_blocks × HEADER_BYTES (24 B on 64-bit).
 *
 * This is the internal-fragmentation metric: fraction of touched memory that
 * went to user data vs allocator bookkeeping.
 *
 * Three patterns:
 *   1. Per-size-class table  — theoretical & measured utilization per size
 *   2. Checkerboard          — alloc 2N blocks, free even-indexed (holes)
 *   3. Steady-state          — random alloc/free, 50K ops, 256-block live set
 */

#include "bench_harness.h"

/* aalloc has 3 size_t fields before data: size_state, left_size, owner */
#define HEADER_BYTES  24

#define CHECKER_N     128   /* 256 total blocks × ≤512 B ≈ 128 KB live */
#define STEADY_LIVE   128
#define STEADY_OPS   50000

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void print_util(const char *label, size_t payload, size_t n_blocks) {
    size_t overhead = n_blocks * HEADER_BYTES;
    size_t total    = payload + overhead;
    double pct = total ? 100.0 * (double)payload / (double)total : 0.0;
    printf("  %-40s  payload=%5zu KiB  overhead=%4zu KiB  util=%.1f%%\n",
           label, payload / 1024, overhead / 1024, pct);
}

/* ── pattern 1: per-size-class utilization ───────────────────────────────── */

static void bench_size_classes(void) {
    /* Sizes up to 512 B to stay within sbrk budget (macOS caps at ~4 MB).
     * 100 blocks × 512 B = 50 KB; cumulative high-water ~200 KB. */
    static const size_t sizes[] = {8, 16, 32, 64, 128, 256, 512};
    static const int    n_each  = 100;
    int nsz = (int)(sizeof(sizes) / sizeof(sizes[0]));

    void **ptrs = malloc((size_t)n_each * sizeof(void *));
    if (!ptrs) { fprintf(stderr, "OOM\n"); exit(1); }

    for (int s = 0; s < nsz; s++) {
        for (int i = 0; i < n_each; i++) {
            ptrs[i] = BENCH_MALLOC(sizes[s]);
            if (!ptrs[i]) { fprintf(stderr, "malloc returned NULL (sz=%zu)\n", sizes[s]); exit(1); }
        }
        size_t payload  = sizes[s] * (size_t)n_each;
        size_t overhead = HEADER_BYTES * (size_t)n_each;
        double pct = 100.0 * (double)payload / (double)(payload + overhead);
        printf("  sz=%4zu B  util=%5.1f%%  (payload=%zu B, hdr=%zu B/block)\n",
               sizes[s], pct, payload, HEADER_BYTES);
        for (int i = 0; i < n_each; i++) BENCH_FREE(ptrs[i]);
    }
    free(ptrs);
}

/* ── pattern 2: checkerboard ─────────────────────────────────────────────── */

static void bench_checkerboard(void) {
    int total = CHECKER_N * 2;
    void  **ptrs  = malloc((size_t)total * sizeof(void *));
    size_t *sizes = malloc((size_t)total * sizeof(size_t));
    if (!ptrs || !sizes) { fprintf(stderr, "OOM\n"); exit(1); }

    uint64_t rng = 0x123456789abcdefULL;
    size_t payload_all = 0;

    for (int i = 0; i < total; i++) {
        sizes[i] = rand_size(&rng, 64, 512);
        ptrs[i]  = BENCH_MALLOC(sizes[i]);
        if (!ptrs[i]) { fprintf(stderr, "malloc returned NULL\n"); exit(1); }
        payload_all += sizes[i];
    }
    print_util("checkerboard (all live)", payload_all, (size_t)total);

    size_t payload_odd = 0;
    for (int i = 0; i < total; i++) {
        if (i % 2 == 0) {
            BENCH_FREE(ptrs[i]); ptrs[i] = NULL;
        } else {
            payload_odd += sizes[i];
        }
    }
    /* denominator stays 'total' — shows external-fragmentation picture:
     * half the blocks freed but the bookkeeping overhead "remains". */
    print_util("checkerboard (50% freed, holes)", payload_odd, (size_t)total);

    for (int i = 0; i < total; i++)
        if (ptrs[i]) BENCH_FREE(ptrs[i]);

    free(ptrs); free(sizes);
}

/* ── pattern 3: steady-state ─────────────────────────────────────────────── */

static void bench_steady(void) {
    void  **live  = malloc(STEADY_LIVE * sizeof(void *));
    size_t *sizes = malloc(STEADY_LIVE * sizeof(size_t));
    if (!live || !sizes) { fprintf(stderr, "OOM\n"); exit(1); }

    uint64_t rng = 0xfedcba9876543210ULL;
    size_t payload = 0;

    for (int i = 0; i < STEADY_LIVE; i++) {
        sizes[i] = rand_size(&rng, 8, 512);
        live[i]  = BENCH_MALLOC(sizes[i]);
        if (!live[i]) { fprintf(stderr, "malloc returned NULL\n"); exit(1); }
        payload += sizes[i];
    }

    for (int op = 0; op < STEADY_OPS; op++) {
        size_t slot = (size_t)(xorshift64(&rng) % STEADY_LIVE);
        payload -= sizes[slot];
        BENCH_FREE(live[slot]);
        sizes[slot] = rand_size(&rng, 8, 512);
        live[slot]  = BENCH_MALLOC(sizes[slot]);
        if (!live[slot]) { fprintf(stderr, "malloc returned NULL\n"); exit(1); }
        payload += sizes[slot];
    }

    print_util("steady-state (live set)", payload, STEADY_LIVE);

    for (int i = 0; i < STEADY_LIVE; i++) BENCH_FREE(live[i]);
    free(live); free(sizes);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== Fragmentation Benchmark [%s] ===\n\n", ALLOC_NAME);
    printf("  Header overhead: %d bytes per allocation (size_state + left_size + owner)\n\n",
           HEADER_BYTES);

    printf("-- Per-size-class utilization (%d blocks each) --\n", 100);
    bench_size_classes();

    printf("\n-- Checkerboard (2×%d blocks, 64–512 B) --\n", CHECKER_N);
    bench_checkerboard();

    printf("\n-- Steady-state (%d live blocks, %d ops, 8–512 B) --\n",
           STEADY_LIVE, STEADY_OPS);
    bench_steady();

    printf("\n");
    return 0;
}
