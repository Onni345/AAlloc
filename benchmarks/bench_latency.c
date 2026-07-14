/*
 * bench_latency.c — per-operation malloc/free latency with p99.
 *
 * Scenarios:
 *   1. Uncontended  — single thread, small/medium/large sizes
 *   2. Contended    — 8 threads sharing 10 arenas, all allocating concurrently
 *   3. Cross-free   — thread A allocates a batch, thread B frees it;
 *                     measures the owner-arena-lock path
 */

#include "bench_harness.h"
#include <pthread.h>

#define N_SAMPLES   200000
#define CROSS_BATCH  1000   /* heap space is limited after prior tests */
#define N_THREADS   8

/* ── portable barrier (macOS lacks pthread_barrier) ──────────────────────── */

typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    int             count;
    int             total;
    int             generation;
} barrier_t;

static void barrier_init(barrier_t *b, int n) {
    pthread_mutex_init(&b->mtx, NULL);
    pthread_cond_init(&b->cv, NULL);
    b->count = 0; b->total = n; b->generation = 0;
}

static void barrier_wait(barrier_t *b) {
    pthread_mutex_lock(&b->mtx);
    int gen = b->generation;
    if (++b->count == b->total) {
        b->count = 0;
        b->generation++;
        pthread_cond_broadcast(&b->cv);
    } else {
        while (b->generation == gen)
            pthread_cond_wait(&b->cv, &b->mtx);
    }
    pthread_mutex_unlock(&b->mtx);
}

static void barrier_destroy(barrier_t *b) {
    pthread_mutex_destroy(&b->mtx);
    pthread_cond_destroy(&b->cv);
}

/* ── single-thread latency ───────────────────────────────────────────────── */

static void bench_uncontended(size_t sz, const char *label) {
    uint64_t *alloc_s = malloc(N_SAMPLES * sizeof(uint64_t));
    uint64_t *free_s  = malloc(N_SAMPLES * sizeof(uint64_t));
    if (!alloc_s || !free_s) { fprintf(stderr, "OOM\n"); exit(1); }

    for (int i = 0; i < 1024; i++) { void *p = BENCH_MALLOC(sz); BENCH_FREE(p); }

    for (size_t i = 0; i < N_SAMPLES; i++) {
        uint64_t t0 = now_ns();
        void *p = BENCH_MALLOC(sz);
        uint64_t t1 = now_ns();
        BENCH_FREE(p);
        uint64_t t2 = now_ns();
        alloc_s[i] = t1 - t0;
        free_s[i]  = t2 - t1;
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "malloc %-20s", label);
    print_stats(buf, compute_stats(alloc_s, N_SAMPLES));
    snprintf(buf, sizeof(buf), "free   %-20s", label);
    print_stats(buf, compute_stats(free_s,  N_SAMPLES));

    free(alloc_s); free(free_s);
}

/* ── contended latency ───────────────────────────────────────────────────── */

typedef struct {
    size_t    sz;
    uint64_t *alloc_s;
    uint64_t *free_s;
    size_t    n;
    barrier_t *barrier;
} contend_arg_t;

static void *contend_thread(void *arg) {
    contend_arg_t *a = arg;
    barrier_wait(a->barrier);

    for (size_t i = 0; i < a->n; i++) {
        uint64_t t0 = now_ns();
        void *p = BENCH_MALLOC(a->sz);
        uint64_t t1 = now_ns();
        BENCH_FREE(p);
        uint64_t t2 = now_ns();
        a->alloc_s[i] = t1 - t0;
        a->free_s[i]  = t2 - t1;
    }
    return NULL;
}

static void bench_contended(size_t sz, const char *label) {
    barrier_t barrier;
    barrier_init(&barrier, N_THREADS);

    uint64_t *alloc_all = malloc((size_t)N_THREADS * N_SAMPLES * sizeof(uint64_t));
    uint64_t *free_all  = malloc((size_t)N_THREADS * N_SAMPLES * sizeof(uint64_t));
    if (!alloc_all || !free_all) { fprintf(stderr, "OOM\n"); exit(1); }

    contend_arg_t args[N_THREADS];
    pthread_t tids[N_THREADS];
    for (int i = 0; i < N_THREADS; i++) {
        args[i] = (contend_arg_t){
            .sz      = sz,
            .alloc_s = alloc_all + i * N_SAMPLES,
            .free_s  = free_all  + i * N_SAMPLES,
            .n       = N_SAMPLES,
            .barrier = &barrier,
        };
        pthread_create(&tids[i], NULL, contend_thread, &args[i]);
    }
    for (int i = 0; i < N_THREADS; i++) pthread_join(tids[i], NULL);
    barrier_destroy(&barrier);

    char buf[64];
    snprintf(buf, sizeof(buf), "malloc %-20s", label);
    print_stats(buf, compute_stats(alloc_all, (size_t)N_THREADS * N_SAMPLES));
    snprintf(buf, sizeof(buf), "free   %-20s", label);
    print_stats(buf, compute_stats(free_all,  (size_t)N_THREADS * N_SAMPLES));

    free(alloc_all); free(free_all);
}

/* ── cross-free latency ──────────────────────────────────────────────────── */

typedef struct {
    void    **ptrs;
    size_t    n;
    uint64_t *samples;
    barrier_t *barrier;
} cross_arg_t;

static void *cross_alloc_thread(void *arg) {
    cross_arg_t *a = arg;
    for (size_t i = 0; i < a->n; i++) {
        a->ptrs[i] = BENCH_MALLOC(64);
        if (!a->ptrs[i]) { fprintf(stderr, "OOM\n"); exit(1); }
    }
    barrier_wait(a->barrier);  /* signal: batch is ready */
    barrier_wait(a->barrier);  /* wait for freeing thread to finish */
    return NULL;
}

static void *cross_free_thread(void *arg) {
    cross_arg_t *a = arg;
    barrier_wait(a->barrier);  /* wait until batch is allocated */
    for (size_t i = 0; i < a->n; i++) {
        uint64_t t0 = now_ns();
        BENCH_FREE(a->ptrs[i]);
        a->samples[i] = now_ns() - t0;
    }
    barrier_wait(a->barrier);  /* signal done */
    return NULL;
}

static void bench_cross_free(void) {
    void    **ptrs  = malloc(CROSS_BATCH * sizeof(void *));
    uint64_t *samps = malloc(CROSS_BATCH * sizeof(uint64_t));
    if (!ptrs || !samps) { fprintf(stderr, "OOM\n"); exit(1); }

    barrier_t barrier;
    barrier_init(&barrier, 2);

    cross_arg_t arg = { .ptrs = ptrs, .n = CROSS_BATCH,
                        .samples = samps, .barrier = &barrier };

    pthread_t ta, tb;
    pthread_create(&ta, NULL, cross_alloc_thread, &arg);
    pthread_create(&tb, NULL, cross_free_thread,  &arg);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    barrier_destroy(&barrier);

    print_stats("free   cross-thread (64 B)     ", compute_stats(samps, CROSS_BATCH));
    free(ptrs); free(samps);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== Latency Benchmark [%s] ===\n\n", ALLOC_NAME);

    printf("-- Uncontended (1 thread, %d samples each) --\n", N_SAMPLES);
    bench_uncontended(64,        "tiny (64 B)");
    bench_uncontended(1024,      "medium (1 KiB)");
    bench_uncontended(64 * 1024, "large (64 KiB)");

    printf("\n-- Contended (%d threads, %d samples/thread) --\n", N_THREADS, N_SAMPLES);
    bench_contended(64,        "tiny (64 B)");
    bench_contended(1024,      "medium (1 KiB)");
    bench_contended(4096,      "large (4 KiB)");

    printf("\n-- Cross-thread free (%d ops) --\n", CROSS_BATCH);
    bench_cross_free();

    printf("\n");
    return 0;
}
