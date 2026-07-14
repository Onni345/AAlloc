/*
 * bench_throughput.c — allocation throughput across four workloads.
 *
 * Workloads:
 *   A  Tiny    8–64 B, alloc-N-then-free-all pattern
 *   B  Medium  128 B – 4 KiB, same
 *   C  Large   8 KiB – 256 KiB, same
 *   D  Mixed   8 B – 256 KiB, random interleaved alloc/free (50/50 live set)
 *
 * Reports millions of (alloc+free) operations per second for each workload.
 */

#include "bench_harness.h"
#include <pthread.h>

#define BENCH_SECS        3.0
#define BATCH_SIZE        512
#define BATCH_SIZE_LARGE   32   /* sbrk heap limits large live sets on macOS */
#define LIVE_SET          512   /* keep peak RSS under ~4 MB */

/* ── workloads ───────────────────────────────────────────────────────────── */

static double run_batch_workload(size_t sz_lo, size_t sz_hi, int batch, int label) {
    uint64_t rng = 0xdeadbeefcafe1234ULL + (uint64_t)label;
    void **ptrs  = malloc((size_t)batch * sizeof(void *));
    size_t *sizes = malloc((size_t)batch * sizeof(size_t));
    if (!ptrs || !sizes) { fprintf(stderr, "OOM\n"); exit(1); }

    uint64_t ops = 0;
    uint64_t t0 = now_ns();

    while (elapsed_sec(t0, now_ns()) < BENCH_SECS) {
        for (int i = 0; i < batch; i++) {
            sizes[i] = rand_size(&rng, sz_lo, sz_hi);
            ptrs[i] = BENCH_MALLOC(sizes[i]);
            if (!ptrs[i]) { fprintf(stderr, "malloc returned NULL (sz=%zu)\n", sizes[i]); exit(1); }
        }
        for (int i = 0; i < batch; i++)
            BENCH_FREE(ptrs[i]);
        ops += (uint64_t)batch * 2;
    }

    free(ptrs); free(sizes);
    uint64_t t1 = now_ns();
    return (double)ops / elapsed_sec(t0, t1) / 1e6;
}

/* Workload D: random interleaved alloc/free with a live set. */
static double run_mixed_workload(void) {
    uint64_t rng = 0xabcdef1234567890ULL;
    void *live[LIVE_SET];
    memset(live, 0, sizeof(live));
    uint64_t ops = 0;
    uint64_t t0 = now_ns();

    /* prime the live set */
    for (int i = 0; i < LIVE_SET; i++) {
        size_t sz = rand_size(&rng, 8, 8 * 1024);
        live[i] = BENCH_MALLOC(sz);
        if (!live[i]) { fprintf(stderr, "malloc returned NULL\n"); exit(1); }
    }

    while (elapsed_sec(t0, now_ns()) < BENCH_SECS) {
        size_t slot = (size_t)(xorshift64(&rng) % LIVE_SET);
        BENCH_FREE(live[slot]);
        live[slot] = BENCH_MALLOC(rand_size(&rng, 8, 8 * 1024));
        if (!live[slot]) { fprintf(stderr, "malloc returned NULL\n"); exit(1); }
        ops += 2;
    }

    for (int i = 0; i < LIVE_SET; i++) BENCH_FREE(live[i]);
    uint64_t t1 = now_ns();
    return (double)ops / elapsed_sec(t0, t1) / 1e6;
}

int main(void) {
    printf("=== Throughput Benchmark [%s] ===\n\n", ALLOC_NAME);
    printf("  Each workload runs for %.0f seconds.\n\n", BENCH_SECS);

    double a = run_batch_workload(8,     64,       BATCH_SIZE,       'A');
    printf("  A  Tiny   (8–64 B):          %7.3f M ops/sec\n", a);

    double b = run_batch_workload(128,   4096,     BATCH_SIZE,       'B');
    printf("  B  Medium (128 B–4 KiB):     %7.3f M ops/sec\n", b);

    double c = run_batch_workload(8192,  64*1024,  BATCH_SIZE_LARGE, 'C');
    printf("  C  Large  (8–64 KiB):        %7.3f M ops/sec\n", c);

    double d = run_mixed_workload();
    printf("  D  Mixed  (8 B–8 KiB):       %7.3f M ops/sec  [random interleaved]\n", d);

    printf("\n");
    return 0;
}
